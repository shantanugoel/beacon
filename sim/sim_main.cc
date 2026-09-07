/* Host-side renderer. Builds the same UI translation units as the firmware
 * and writes PNGs, so layout work is a sub-second loop instead of a flash
 * cycle. Fixtures below mirror the shapes the hub actually emits. */

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "beacon_gray.h"
#include "beacon_ui.h"

namespace {

// ---- minimal PNG writer (zlib stored blocks; no external deps) ----------
uint32_t Crc32(const uint8_t* p, size_t n, uint32_t crc = 0xFFFFFFFFu) {
    static uint32_t table[256];
    static bool init = false;
    if (!init) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
        init = true;
    }
    for (size_t i = 0; i < n; ++i) crc = table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    return crc;
}

void PutU32(std::vector<uint8_t>& v, uint32_t x) {
    v.push_back(x >> 24); v.push_back(x >> 16); v.push_back(x >> 8); v.push_back(x);
}

void Chunk(FILE* f, const char* type, const std::vector<uint8_t>& data) {
    std::vector<uint8_t> len;
    PutU32(len, static_cast<uint32_t>(data.size()));
    fwrite(len.data(), 1, 4, f);
    std::vector<uint8_t> body(type, type + 4);
    body.insert(body.end(), data.begin(), data.end());
    fwrite(body.data(), 1, body.size(), f);
    std::vector<uint8_t> crc;
    PutU32(crc, Crc32(body.data(), body.size()) ^ 0xFFFFFFFFu);
    fwrite(crc.data(), 1, 4, f);
}

/* scale: nearest-neighbour upscale so 1:1 pixel work is reviewable. */
void WritePng(const char* path, const uint8_t* rgb_rows, int w, int h) {
    FILE* f = fopen(path, "wb");
    if (!f) { std::fprintf(stderr, "cannot open %s\n", path); return; }
    const uint8_t sig[8] = {137, 'P', 'N', 'G', 13, 10, 26, 10};
    fwrite(sig, 1, 8, f);

    std::vector<uint8_t> ihdr;
    PutU32(ihdr, w); PutU32(ihdr, h);
    ihdr.push_back(8); ihdr.push_back(0); ihdr.push_back(0);
    ihdr.push_back(0); ihdr.push_back(0);
    Chunk(f, "IHDR", ihdr);

    std::vector<uint8_t> raw;
    raw.reserve(static_cast<size_t>(h) * (w + 1));
    for (int y = 0; y < h; ++y) {
        raw.push_back(0);
        raw.insert(raw.end(), rgb_rows + static_cast<size_t>(y) * w,
                   rgb_rows + static_cast<size_t>(y) * w + w);
    }
    // zlib stream with stored deflate blocks.
    std::vector<uint8_t> z{0x78, 0x01};
    size_t off = 0;
    while (off < raw.size()) {
        const size_t n = std::min<size_t>(65535, raw.size() - off);
        z.push_back(off + n >= raw.size() ? 1 : 0);
        z.push_back(n & 0xFF); z.push_back(n >> 8);
        z.push_back(~n & 0xFF); z.push_back((~n >> 8) & 0xFF);
        z.insert(z.end(), raw.begin() + off, raw.begin() + off + n);
        off += n;
    }
    uint32_t a = 1, b = 0;
    for (uint8_t c : raw) { a = (a + c) % 65521; b = (b + a) % 65521; }
    PutU32(z, (b << 16) | a);
    Chunk(f, "IDAT", z);
    Chunk(f, "IEND", {});
    fclose(f);
}

void Save1bpp(const char* path, const beacon::Canvas& c, int scale) {
    const int w = beacon::kScreenW * scale, h = beacon::kScreenH * scale;
    std::vector<uint8_t> img(static_cast<size_t>(w) * h);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const int sx = x / scale, sy = y / scale;
            const uint8_t byte =
                c.data()[static_cast<size_t>(sy) * beacon::kStride + (sx >> 3)];
            const bool white = byte & (0x80u >> (sx & 7));
            // Panel white is not paper white; bias so proofs read like e-ink.
            img[static_cast<size_t>(y) * w + x] = white ? 0xF2 : 0x14;
        }
    }
    WritePng(path, img.data(), w, h);
}

void Save4bpp(const char* path, const uint8_t* packed, int scale) {
    const int w = beacon::kScreenW * scale, h = beacon::kScreenH * scale;
    std::vector<uint8_t> img(static_cast<size_t>(w) * h);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const int sx = x / scale, sy = y / scale;
            const size_t i = static_cast<size_t>(sy) * beacon::kScreenW + sx;
            const uint8_t byte = packed[i / 2];
            const uint8_t lvl = (i & 1) ? (byte & 0x0F) : (byte >> 4);
            img[static_cast<size_t>(y) * w + x] =
                static_cast<uint8_t>(0x14 + (0xF2 - 0x14) * lvl / 15);
        }
    }
    WritePng(path, img.data(), w, h);
}

// ---- fixtures ----------------------------------------------------------
void Set(beacon::Agent& a, const char* id, const char* title, const char* proj,
         const char* branch, const char* machine, beacon::Status st,
         uint32_t age, const char* activity, const char* question = "") {
    std::snprintf(a.id, sizeof(a.id), "%s", id);
    std::snprintf(a.title, sizeof(a.title), "%s", title);
    std::snprintf(a.project, sizeof(a.project), "%s", proj);
    std::snprintf(a.branch, sizeof(a.branch), "%s", branch);
    std::snprintf(a.machine, sizeof(a.machine), "%s", machine);
    std::snprintf(a.activity, sizeof(a.activity), "%s", activity);
    std::snprintf(a.question, sizeof(a.question), "%s", question);
    a.status = st;
    a.status_age_s = age;
}

void Recount(beacon::Fleet& f) {
    f.n_working = f.n_blocked = f.n_done = f.n_idle = 0;
    for (uint8_t i = 0; i < f.count; ++i) {
        switch (f.agents[i].status) {
            case beacon::Status::kWorking: ++f.n_working; break;
            case beacon::Status::kBlocked: ++f.n_blocked; break;
            case beacon::Status::kDone: ++f.n_done; break;
            case beacon::Status::kIdle: ++f.n_idle; break;
            default: break;
        }
    }
}

beacon::Fleet MakeFleet(bool with_block) {
    beacon::Fleet f;
    f.hour = 21; f.minute = 47;
    std::snprintf(f.date_label, sizeof(f.date_label), "MON 07 SEP");
    int i = 0;
    if (with_block) {
        Set(f.agents[i], "raven:w2p1",
            "NOTE4 e-paper firmware bring-up", "zectrix-note4", "main",
            "raven", beacon::Status::kBlocked, 252,
            "Bash(idf.py -p /dev/ttyACM0 flash)",
            "Run idf.py flash on /dev/ttyACM0? This overwrites device firmware.");
        f.agents[i].actionable = true;
        f.agents[i].tokens = 84300; f.agents[i].cost_milli = 2410;
        f.agents[i].lines_added = 612; f.agents[i].lines_removed = 88;
        f.agents[i].action_count = 4;
        std::snprintf(f.agents[i].actions[0].label, 20, "Approve once");
        std::snprintf(f.agents[i].actions[0].id, 16, "approve");
        std::snprintf(f.agents[i].actions[1].label, 20, "Approve + don't ask");
        std::snprintf(f.agents[i].actions[1].id, 16, "approve_all");
        std::snprintf(f.agents[i].actions[2].label, 20, "Reject");
        std::snprintf(f.agents[i].actions[2].id, 16, "reject");
        std::snprintf(f.agents[i].actions[3].label, 20, "Focus this pane");
        std::snprintf(f.agents[i].actions[3].id, 16, "focus");
        ++i;
    }
    Set(f.agents[i], "raven:w4p1", "Hermes retry backoff + jitter", "hermes",
        "fix/backoff", "raven", beacon::Status::kWorking, 194,
        "Edit(internal/queue/retry.go)");
    f.agents[i].focused = true; f.agents[i].actionable = true;
    f.agents[i].tokens = 41200; f.agents[i].cost_milli = 980;
    f.agents[i].action_count = 2;
    std::snprintf(f.agents[i].actions[0].label, 20, "Interrupt");
    std::snprintf(f.agents[i].actions[0].id, 16, "interrupt");
    std::snprintf(f.agents[i].actions[1].label, 20, "Focus this pane");
    std::snprintf(f.agents[i].actions[1].id, 16, "focus");
    ++i;
    Set(f.agents[i], "atlas:w1p1", "CamoStack shader permutation cache",
        "CamoStack", "main", "atlas", beacon::Status::kWorking, 1268,
        "Bash(cargo test --release)");
    ++i;
    Set(f.agents[i], "atlas:w1p3", "Incus profile migration to v6",
        "incus-setup", "main", "atlas", beacon::Status::kDone, 733,
        "Wrote 4 files, 2 tests passing");
    ++i;
    Set(f.agents[i], "raven:w3p1", "local-llms bench harness", "local-llms",
        "bench", "raven", beacon::Status::kIdle, 5320, "");
    ++i;
    Set(f.agents[i], "pi:w1p1", "chess-masters opening book import",
        "chess-masters", "main", "pi", beacon::Status::kStale, 21600, "");
    ++i;
    f.count = static_cast<uint8_t>(i);
    Recount(f);
    return f;
}

beacon::Device MakeDevice() {
    beacon::Device d;
    d.link = beacon::Link::kOnline;
    d.rssi = -58;
    d.battery_percent = 76; d.battery_mv = 3912; d.battery_valid = true;
    d.uptime_s = 9412; d.last_sync_age_s = 3;
    d.full_refreshes = 41; d.partial_refreshes = 388;
    std::snprintf(d.ssid, sizeof(d.ssid), "orbital-5");
    std::snprintf(d.ip, sizeof(d.ip), "10.0.4.61");
    std::snprintf(d.hub, sizeof(d.hub), "http://raven.local:8787");
    std::snprintf(d.firmware, sizeof(d.firmware), "v0.1.0");
    return d;
}

}  // namespace

int main(int argc, char** argv) {
    const std::string out = argc > 1 ? argv[1] : "sim/out";
    const int scale = argc > 2 ? atoi(argv[2]) : 2;

    static beacon::Canvas canvas;
    static uint8_t gray[beacon::kScreenW * beacon::kScreenH / 2];

    auto shot = [&](const char* name, beacon::Ui& ui,
                    const beacon::Fleet& f, const beacon::Device& d) {
        const std::string path = out + "/" + name + ".png";
        if (ui.Render(canvas, f, d) == beacon::Paint::kFull4bpp) {
            ui.RenderQuiet4bpp(gray, f, d);
            Save4bpp(path.c_str(), gray, scale);
        } else {
            Save1bpp(path.c_str(), canvas, scale);
        }
        std::printf("  %s\n", path.c_str());
    };

    const beacon::Device dev = MakeDevice();

    {   // Fleet, with someone waiting.
        beacon::Fleet f = MakeFleet(true);
        beacon::Ui ui; ui.Reset(); ui.OnFleetUpdated(f);
        ui.GoTo(beacon::Screen::kFleet);
        shot("01-fleet-attention", ui, f, dev);

        ui.OnInput(beacon::Button::kOk, beacon::Press::kClick, f);
        shot("02-agent-blocked", ui, f, dev);

        ui.OnInput(beacon::Button::kDown, beacon::Press::kClick, f);
        ui.OnInput(beacon::Button::kDown, beacon::Press::kClick, f);
        shot("03-agent-actions", ui, f, dev);

        ui.GoTo(beacon::Screen::kQuiet);
        shot("06-quiet-busy", ui, f, dev);
    }
    {   // Calm fleet.
        beacon::Fleet f = MakeFleet(false);
        beacon::Ui ui; ui.Reset(); ui.OnFleetUpdated(f);
        ui.GoTo(beacon::Screen::kFleet);
        shot("04-fleet-calm", ui, f, dev);

        // Exercise the real Fleet shortcut instead of bypassing navigation.
        ui.OnInput(beacon::Button::kUp, beacon::Press::kHold, f);
        shot("05-system", ui, f, dev);

        for (uint8_t i = 0; i < f.count; ++i) f.agents[i].status = beacon::Status::kIdle;
        Recount(f);
        ui.GoTo(beacon::Screen::kQuiet);
        shot("07-quiet-calm", ui, f, dev);
    }
    {   // Empty + boot states.
        beacon::Fleet f; f.hour = 9; f.minute = 5;
        std::snprintf(f.date_label, sizeof(f.date_label), "MON 07 SEP");
        beacon::Device d = MakeDevice();
        beacon::Ui ui; ui.Reset();
        d.link = beacon::Link::kWifiConnecting;
        ui.GoTo(beacon::Screen::kSplash);
        shot("08-splash", ui, f, d);

        d.link = beacon::Link::kDegraded;
        ui.GoTo(beacon::Screen::kFleet);
        shot("09-fleet-empty", ui, f, d);
    }
    return 0;
}
