/* BEACON - agent mission control for the ZECTRIX NOTE4.
 *
 * The shell: owns the board, runs the input/render loop, and decides when the
 * panel is allowed to change. Everything about *what* is drawn lives in
 * beacon_ui; everything about *how* it reaches the panel lives in
 * beacon_display; everything about where the data comes from lives in
 * beacon_net. This file is the policy that connects them.
 */

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#include "beacon_canvas.h"
#include "beacon_config.h"
#include "beacon_display.h"
#include "beacon_gray.h"
#include "beacon_mem.h"
#include "beacon_net.h"
#include "beacon_ui.h"
#include "esp_chip_info.h"
#include "esp_console.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "audio_codec.h"
#include "zectrix_board.h"
#include "zectrix_board_config.h"

namespace {

constexpr const char* kTag = "beacon";
constexpr const char* kFirmwareVersion = "v0.1.0";

/* How long DOWN must be held to power the device down. Matches the hardware
 * convention the vendor firmware established, so muscle memory carries over. */
constexpr int64_t kShutdownHoldUs = 3'000'000;

/* The clock in the header only advances a minute at a time, so there is no
 * point waking more often than this when nothing else is happening. */
constexpr TickType_t kIdleTick = pdMS_TO_TICKS(5000);

beacon::Canvas g_canvas;   /* 15 KB, internal RAM: SPI DMA source, hot path */
uint8_t* g_gray = nullptr; /* 60 KB packed 4bpp frame, allocated from PSRAM */

beacon::Config g_config;
beacon::Net g_net;
beacon::Display g_display;
beacon::Ui g_ui;
ZectrixBoard g_board;

/* Fleet is ~8.5 KB (16 agents x 528 B). Every one of these must live in
 * static storage: an 8 KB task stack cannot hold one, and the first attempt to
 * copy a Fleet into a local blew the main task's stack and corrupted the heap,
 * which surfaced as a LoadProhibited panic deep inside the SPI driver. */
static_assert(sizeof(beacon::Fleet) > 4096,
              "Fleet is large by design; keep it off the stack");

beacon::Fleet g_fleet;
beacon::Fleet g_incoming;   /* staging for a snapshot from the network task */
beacon::Fleet g_saved;      /* preview stashes the live fleet here          */
beacon::Device g_device;

/* Set by the `beacon-preview` console command and consumed by the main loop.
 * Drawing happens on one task only: Display owns a single panel and a single
 * previous-frame buffer, and letting the console task paint would race it. */
volatile bool g_preview_request = false;
volatile bool g_chirp_request = false;

int64_t g_boot_us = 0;
int64_t g_last_input_us = 0;
uint8_t g_led_pulses = 0;

void RefreshDeviceState() {
    g_device.link = g_net.link();
    g_device.rssi = g_net.rssi();
    g_device.last_sync_age_s = g_net.last_sync_age_s();
    g_device.uptime_s =
        static_cast<uint32_t>((esp_timer_get_time() - g_boot_us) / 1000000);
    g_device.full_refreshes = g_display.full_count();
    g_device.partial_refreshes = g_display.partial_count();

    const ZectrixPowerSnapshot power = g_board.ReadPowerSnapshot();
    g_device.battery_valid = power.battery_valid;
    g_device.battery_mv = power.battery_mv;
    g_device.battery_percent = power.battery_percent;
    g_device.charging = power.charge.charging;

    beacon::CopyField(g_device.ip, sizeof(g_device.ip), g_net.ip());
    beacon::CopyField(g_device.ssid, sizeof(g_device.ssid), g_config.ssid);
    beacon::CopyField(g_device.hub, sizeof(g_device.hub), g_config.hub);
    beacon::CopyField(g_device.firmware, sizeof(g_device.firmware), kFirmwareVersion);
}

/* Draw whatever the UI currently wants, choosing the refresh mode it asks
 * for. Returns true if the panel was actually touched. */
bool Paint(bool force_full) {
    RefreshDeviceState();
    const beacon::Paint mode = g_ui.Render(g_canvas, g_fleet, g_device);
    if (mode == beacon::Paint::kFull4bpp) {
        if (g_gray == nullptr) {
            g_gray = static_cast<uint8_t*>(
                beacon::BigAlloc(beacon::kScreenW * beacon::kScreenH / 2));
        }
        if (g_gray == nullptr) return false;
        g_ui.RenderQuiet4bpp(g_gray, g_fleet, g_device);
        return g_display.PresentGray(g_gray) == ESP_OK;
    }
    return g_display.Present(g_canvas, force_full ||
                                       mode == beacon::Paint::kFull1bpp) == ESP_OK;
}

/* The audible half of the alert.
 *
 * Two short rising tones, quiet and quickly over: this fires when an agent
 * starts waiting on you, and a desk object that startles you is one you
 * eventually unplug. Each tone gets a raised-cosine envelope, without which
 * the abrupt start and stop produce an audible click through the speaker that
 * is more noticeable than the tone itself.
 *
 * Everything here is best-effort. If the codec is absent or fails to come up,
 * the alert is simply visual.
 */
void Chirp() {
    if (!g_config.chirp) return;

    constexpr int kRate = ZECTRIX_AUDIO_SAMPLE_RATE;   /* 16 kHz mono */
    constexpr float kTones[] = {1046.5f, 1568.0f};     /* C6, G6      */
    constexpr int kToneMs = 110;
    constexpr int kGapMs = 45;
    constexpr int kEdgeMs = 9;
    constexpr float kAmplitude = 0.22f;                /* headroom, on purpose */

    static std::vector<int16_t> samples;
    if (samples.empty()) {
        const int tone = (kRate * kToneMs) / 1000;
        const int gap = (kRate * kGapMs) / 1000;
        const int edge = (kRate * kEdgeMs) / 1000;
        samples.reserve(static_cast<size_t>(tone) * 2 + gap);
        for (int t = 0; t < 2; ++t) {
            for (int i = 0; i < tone; ++i) {
                float envelope = 1.0f;
                if (i < edge) {
                    envelope = 0.5f * (1.0f - cosf(3.14159265f * i / edge));
                } else if (i > tone - edge) {
                    envelope =
                        0.5f * (1.0f - cosf(3.14159265f * (tone - i) / edge));
                }
                const float value =
                    sinf(2.0f * 3.14159265f * kTones[t] * i / kRate);
                samples.push_back(static_cast<int16_t>(
                    value * envelope * kAmplitude * 32767.0f));
            }
            if (t == 0) samples.insert(samples.end(), gap, 0);
        }
    }

    g_board.SetAudioPower(true);
    AudioCodec* codec = g_board.PrepareAudio();
    if (codec == nullptr || !codec->valid()) {
        ESP_LOGW(kTag, "no codec; alert is visual only");
        g_board.SetAudioPower(false);
        return;
    }
    codec->EnableOutput(true);
    codec->OutputData(samples);
    // A short tail of silence so the amplifier does not cut mid-decay.
    static std::vector<int16_t> tail(kRate / 20, 0);
    codec->OutputData(tail);
    codec->EnableOutput(false);
    g_board.SetAudioPower(false);
}

/* A short LED flutter when something starts waiting on you. The panel is
 * silent and slow; this is what catches the eye from across the room while the
 * e-ink is still settling. */
void PulseLed(int times) {
    for (int i = 0; i < times; ++i) {
        g_board.SetPowerLed(true);
        vTaskDelay(pdMS_TO_TICKS(70));
        g_board.SetPowerLed(false);
        vTaskDelay(pdMS_TO_TICKS(90));
    }
}

/* A self-contained fleet used by `beacon-preview`.
 *
 * This is a hardware acceptance test as much as a demo: it exercises every
 * screen and, crucially, every refresh mode - full 1bpp, partial 1bpp and
 * 16-grey - and logs how long each one actually took on this panel. That is
 * the only way to measure partial-refresh timing without a hub, and it lets
 * anyone building this check the display end to end before wiring up the
 * network. */
void FillPreviewFleet(beacon::Fleet* f, bool blocked) {
    *f = beacon::Fleet{};
    f->hour = 21;
    f->minute = 47;
    beacon::CopyField(f->date_label, sizeof(f->date_label), "MON 07 SEP");

    struct Row {
        const char* id;
        const char* title;
        const char* project;
        const char* machine;
        const char* activity;
        beacon::Status status;
        uint32_t age;
    };
    static const Row kRows[] = {
        {"raven:w2p1", "NOTE4 e-paper firmware bring-up", "zectrix-note4",
         "raven", "Bash(idf.py -p /dev/ttyACM0 flash)", beacon::Status::kBlocked, 252},
        {"raven:w4p1", "Hermes retry backoff + jitter", "hermes", "raven",
         "Edit(internal/queue/retry.go)", beacon::Status::kWorking, 194},
        {"atlas:w1p1", "CamoStack shader permutation cache", "CamoStack",
         "atlas", "Bash(cargo test --release)", beacon::Status::kWorking, 1268},
        {"atlas:w1p3", "Incus profile migration to v6", "incus-setup", "atlas",
         "Wrote 4 files, 2 tests passing", beacon::Status::kDone, 733},
        {"raven:w3p1", "local-llms bench harness", "local-llms", "raven", "",
         beacon::Status::kIdle, 5320},
    };

    for (const Row& row : kRows) {
        if (!blocked && row.status == beacon::Status::kBlocked) continue;
        beacon::Agent& a = f->agents[f->count];
        a = beacon::Agent{};
        beacon::CopyField(a.id, sizeof(a.id), row.id);
        beacon::CopyField(a.title, sizeof(a.title), row.title);
        beacon::CopyField(a.project, sizeof(a.project), row.project);
        beacon::CopyField(a.machine, sizeof(a.machine), row.machine);
        beacon::CopyField(a.activity, sizeof(a.activity), row.activity);
        beacon::CopyField(a.branch, sizeof(a.branch), "main");
        a.status = row.status;
        a.status_age_s = row.age;
        a.actionable = true;
        if (row.status == beacon::Status::kBlocked) {
            beacon::CopyField(a.question, sizeof(a.question),
                              "Run idf.py flash on /dev/ttyACM0? "
                              "This overwrites device firmware.");
            a.tokens = 84300;
            a.cost_milli = 2410;
            a.lines_added = 612;
            a.lines_removed = 88;
            const char* labels[] = {"Approve once", "Approve + don't ask",
                                    "Reject", "Focus this pane"};
            const char* ids[] = {"1", "2", "3", "focus"};
            for (int i = 0; i < 4; ++i) {
                beacon::CopyField(a.actions[i].label,
                                  sizeof(a.actions[i].label), labels[i]);
                beacon::CopyField(a.actions[i].id, sizeof(a.actions[i].id),
                                  ids[i]);
            }
            a.action_count = 4;
        }
        ++f->count;
        switch (row.status) {
            case beacon::Status::kBlocked: ++f->n_blocked; break;
            case beacon::Status::kWorking: ++f->n_working; break;
            case beacon::Status::kDone: ++f->n_done; break;
            case beacon::Status::kIdle: ++f->n_idle; break;
            default: break;
        }
    }
    f->n_machines = 2;
}

void RunPreview() {
    g_saved = g_fleet;
    const beacon::Screen saved_screen = g_ui.screen();

    struct Step {
        const char* name;
        beacon::Screen screen;
        bool blocked;
    };
    static const Step kSteps[] = {
        {"fleet (attention)", beacon::Screen::kFleet, true},
        {"session detail", beacon::Screen::kAgent, true},
        {"fleet (calm)", beacon::Screen::kFleet, false},
        {"system", beacon::Screen::kSystem, false},
        {"quiet / 16-grey", beacon::Screen::kQuiet, false},
    };

    for (const Step& step : kSteps) {
        FillPreviewFleet(&g_fleet, step.blocked);
        g_ui.OnFleetUpdated(g_fleet);
        g_ui.TakeAttentionEdge();  // the preview must not trigger the pager
        g_ui.GoTo(step.screen);
        Paint(true);
        ESP_LOGI(kTag, "preview: %-18s %lld ms", step.name,
                 g_display.last_refresh_us() / 1000);
        vTaskDelay(pdMS_TO_TICKS(2500));
    }

    // A partial refresh, measured on its own: the same screen with one timer
    // moved on, which is the shape of almost every real update.
    FillPreviewFleet(&g_fleet, false);
    g_ui.OnFleetUpdated(g_fleet);
    g_ui.GoTo(beacon::Screen::kFleet);
    Paint(true);
    for (int i = 0; i < 3; ++i) {
        for (uint8_t k = 0; k < g_fleet.count; ++k) {
            g_fleet.agents[k].status_age_s += 61;
        }
        Paint(false);
        ESP_LOGI(kTag, "preview: partial (timers)  %lld ms",
                 g_display.last_refresh_us() / 1000);
        vTaskDelay(pdMS_TO_TICKS(1200));
    }

    g_fleet = g_saved;
    g_ui.OnFleetUpdated(g_fleet);
    g_ui.TakeAttentionEdge();
    g_ui.GoTo(saved_screen);
    Paint(true);
    ESP_LOGI(kTag, "preview: done");
}

int CmdAlert(int, char**) {
    g_chirp_request = true;
    g_led_pulses = 3;
    printf("firing the attention alert\n");
    return 0;
}

int CmdPreview(int, char**) {
    g_preview_request = true;
    printf("running screen preview on the panel\n");
    return 0;
}

void Shutdown() {
    ESP_LOGI(kTag, "shutting down");
    g_display.Clear();
    g_display.Shutdown();
    g_board.SetPowerLed(false);
    g_board.SetAudioPower(false);
    vTaskDelay(pdMS_TO_TICKS(120));
    // On battery this cuts the rail; on USB the latch has no effect and the
    // device simply sits with a cleared panel.
    g_board.CutBatteryPower();
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
}

void LogBoot() {
    esp_chip_info_t chip;
    esp_chip_info(&chip);
    ESP_LOGI(kTag, "BEACON %s on ESP32-S3 rev %d, %u cores", kFirmwareVersion,
             chip.revision, chip.cores);
    ESP_LOGI(kTag, "heap %u internal / %u psram",
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
}

}  // namespace

extern "C" void app_main(void) {
    g_boot_us = esp_timer_get_time();
    g_last_input_us = g_boot_us;

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
        err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    LogBoot();
    ESP_ERROR_CHECK(beacon::ConfigLoad(&g_config));
    ESP_ERROR_CHECK(g_board.Init());
    g_board.SetPowerLed(false);

    if (g_display.Init() != ESP_OK) {
        ESP_LOGE(kTag, "display init failed; nothing else is worth doing");
        vTaskDelay(pdMS_TO_TICKS(2000));
        esp_restart();
    }

    // Splash immediately: the panel takes a moment and a blank screen during
    // boot reads as a dead device.
    g_ui.Reset();
    g_ui.GoTo(beacon::Screen::kSplash);
    g_device.link = beacon::Link::kBooting;
    beacon::CopyField(g_device.ssid, sizeof(g_device.ssid), g_config.ssid);
    Paint(true);

    beacon::ConfigRegisterConsole(&g_config);
    {
        const esp_console_cmd_t preview = {
            "beacon-preview", "Draw every screen on the panel and time each "
            "refresh mode", nullptr, CmdPreview, nullptr, nullptr, nullptr};
        esp_console_cmd_register(&preview);
        const esp_console_cmd_t alert = {
            "beacon-alert", "Fire the attention alert (LED + chirp)", nullptr,
            CmdAlert, nullptr, nullptr, nullptr};
        esp_console_cmd_register(&alert);
    }

    if (!beacon::ConfigComplete(g_config)) {
        ESP_LOGW(kTag, "no network configured; use beacon-set over serial");
        g_device.link = beacon::Link::kOffline;
        Paint(true);
    } else if (g_net.Start(g_config) != ESP_OK) {
        ESP_LOGE(kTag, "network start failed");
        g_device.link = beacon::Link::kOffline;
        Paint(true);
    }

    bool showed_fleet = false;
    int64_t down_pressed_us = 0;

    for (;;) {
        ZectrixButtonEvent event;
        const bool had_input = g_board.WaitButton(&event, kIdleTick);
        const int64_t now = esp_timer_get_time();
        bool force_full = false;
        bool repaint = false;

        if (had_input) {
            g_last_input_us = now;

            // DOWN doubles as the power button; a long hold shuts down rather
            // than moving the selection.
            if (event.button == ZectrixButton::kDown &&
                event.action == ZectrixButtonAction::kLongPress) {
                Shutdown();
            }
            (void)down_pressed_us;

            const beacon::Button button =
                event.button == ZectrixButton::kUp    ? beacon::Button::kUp
                : event.button == ZectrixButton::kDown ? beacon::Button::kDown
                                                       : beacon::Button::kOk;
            const beacon::Press press =
                event.action == ZectrixButtonAction::kLongPress
                    ? beacon::Press::kHold
                    : beacon::Press::kClick;

            const beacon::Intent intent = g_ui.OnInput(button, press, g_fleet);
            if (intent == beacon::Intent::kFireAction) {
                const bool queued = g_net.PostAction(g_ui.pending_agent_id(),
                                                     g_ui.pending_action_id());
                g_ui.set_toast(queued ? "SENT" : "BUSY", 3);
                g_ui.ClearPending();
                PulseLed(1);
            }
            /* No forced flash on input: moving the cursor changes one or two
             * rows, which the diff turns into a flash-free partial refresh.
             * Screen changes set the flag themselves through Ui::GoTo. */
            repaint = intent != beacon::Intent::kNone;
        }

        // New state from the hub.
        if (g_net.TakeFleetChanged()) {
            if (g_net.CopyFleet(&g_incoming)) {
                g_fleet = g_incoming;
                g_ui.OnFleetUpdated(g_fleet);
                repaint = true;
                if (!showed_fleet) {
                    showed_fleet = true;
                    g_ui.GoTo(beacon::Screen::kFleet);
                    force_full = true;
                }
            }
        }

        /* The one interruption the device is allowed to make. A session going
         * blocked pulls the screen out of quiet mode and flutters the LED;
         * nothing else may take the display away from you. */
        if (g_ui.TakeAttentionEdge()) {
            if (g_ui.screen() == beacon::Screen::kQuiet) {
                g_ui.GoTo(beacon::Screen::kFleet);
            }
            g_led_pulses = 3;
            g_chirp_request = true;
            repaint = true;
            force_full = true;
        }

        // Ambient mode: only from the fleet screen, only when nothing waits.
        if (!had_input && showed_fleet &&
            g_ui.screen() == beacon::Screen::kFleet &&
            g_fleet.n_blocked == 0 &&
            (now - g_last_input_us) > (int64_t)g_config.quiet_after_s * 1000000) {
            g_ui.GoTo(beacon::Screen::kQuiet);
            repaint = true;
        }

        // The clock and the elapsed timers move on their own; a periodic
        // repaint keeps them honest without a dedicated timer task. The diff
        // in Display::Present means an unchanged frame still costs nothing.
        if (!repaint && !had_input) repaint = true;

        if (g_preview_request) {
            g_preview_request = false;
            RunPreview();
            g_last_input_us = esp_timer_get_time();
            continue;
        }

        g_ui.tick_toast();
        if (repaint) Paint(force_full);

        if (g_led_pulses > 0) {
            PulseLed(g_led_pulses);
            g_led_pulses = 0;
        }
        if (g_chirp_request) {
            g_chirp_request = false;
            Chirp();
        }
    }
}
