#include "beacon_net.h"

#include "beacon_mem.h"

#include <cstdio>
#include <cstring>

#include "cJSON.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

namespace beacon {
namespace {

constexpr const char* kTag = "beacon.net";
constexpr int kBodyCap = 12 * 1024;   /* 16 agents of prose, with headroom */
constexpr int kLongPollSeconds = 25;

EventGroupHandle_t g_wifi_events = nullptr;
constexpr int kWifiConnected = BIT0;
constexpr int kWifiFailed = BIT1;

void WifiEvent(void*, esp_event_base_t base, int32_t id, void* data) {
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(g_wifi_events, kWifiConnected);
        xEventGroupSetBits(g_wifi_events, kWifiFailed);
        // The supplicant gives up after one attempt; keep asking. A pager
        // that stays dark after a router reboot is a broken pager.
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        xEventGroupClearBits(g_wifi_events, kWifiFailed);
        xEventGroupSetBits(g_wifi_events, kWifiConnected);
        (void)data;
    }
}

Status ParseStatus(const char* s) {
    if (s == nullptr) return Status::kUnknown;
    switch (s[0]) {
        case 'i': return Status::kIdle;
        case 'w': return Status::kWorking;
        case 'b': return Status::kBlocked;
        case 'd': return Status::kDone;
        case 's': return Status::kStale;
        default: return Status::kUnknown;
    }
}

void CopyString(char* dst, size_t cap, const cJSON* node) {
    CopyField(dst, cap,
              (cJSON_IsString(node) && node->valuestring != nullptr)
                  ? node->valuestring
                  : nullptr);
}

int NumberOr(const cJSON* node, int fallback) {
    return cJSON_IsNumber(node) ? node->valueint : fallback;
}

/* Body accumulator for esp_http_client's event callback. */
struct Sink {
    char* buffer;
    int length;
    int cap;
    bool overflow;
};

esp_err_t HttpEvent(esp_http_client_event_t* event) {
    if (event->event_id != HTTP_EVENT_ON_DATA) return ESP_OK;
    Sink* sink = static_cast<Sink*>(event->user_data);
    if (sink == nullptr) return ESP_OK;
    if (sink->length + event->data_len >= sink->cap) {
        sink->overflow = true;
        return ESP_OK;
    }
    memcpy(sink->buffer + sink->length, event->data, event->data_len);
    sink->length += event->data_len;
    sink->buffer[sink->length] = '\0';
    return ESP_OK;
}

}  // namespace

uint32_t Net::last_sync_age_s() const {
    if (last_sync_us_ == 0) return 0;
    return static_cast<uint32_t>((esp_timer_get_time() - last_sync_us_) / 1000000);
}

bool Net::TakeFleetChanged() {
    const bool changed = fleet_changed_;
    fleet_changed_ = false;
    return changed;
}

bool Net::CopyFleet(Fleet* out) {
    if (out == nullptr || lock_ == nullptr || !have_fleet_) return false;
    if (xSemaphoreTake(lock_, pdMS_TO_TICKS(200)) != pdTRUE) return false;
    *out = fleet_;
    xSemaphoreGive(lock_);
    return true;
}

bool Net::PostAction(const char* agent_id, const char* action_id) {
    if (pending_) return false;  // One in flight is enough at three buttons.
    CopyField(pending_agent_, sizeof(pending_agent_), agent_id);
    CopyField(pending_action_, sizeof(pending_action_), action_id);
    pending_ = true;
    return true;
}

esp_err_t Net::StartWifi() {
    g_wifi_events = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &WifiEvent, nullptr, nullptr));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &WifiEvent, nullptr, nullptr));

    wifi_config_t wifi = {};
    CopyField(reinterpret_cast<char*>(wifi.sta.ssid), sizeof(wifi.sta.ssid),
              config_.ssid);
    CopyField(reinterpret_cast<char*>(wifi.sta.password),
              sizeof(wifi.sta.password), config_.password);
    wifi.sta.threshold.authmode =
        config_.password[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    // MAX_MODEM wakes on this listen interval rather than every AP DTIM.
    // Three beacons is a useful desk-device compromise: meaningful radio
    // sleep without making inbound status feel sluggish.
    wifi.sta.listen_interval = 3;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi));
    /* Modem sleep: the device spends nearly all its time parked in a long
     * poll, so letting the radio doze between beacons is the single biggest
     * battery win available. This was briefly suspected of causing a crash in
     * the Wi-Fi driver's own power-management timer path; the real cause was
     * a stack overflow in this task corrupting memory around it. */
    ESP_ERROR_CHECK(esp_wifi_set_ps(config_.wifi_max_power_save
                                        ? WIFI_PS_MAX_MODEM
                                        : WIFI_PS_MIN_MODEM));
    ESP_ERROR_CHECK(esp_wifi_start());
    return ESP_OK;
}

esp_err_t Net::SetMaxPowerSave(bool enabled) {
    config_.wifi_max_power_save = enabled;
    return esp_wifi_set_ps(enabled ? WIFI_PS_MAX_MODEM : WIFI_PS_MIN_MODEM);
}

esp_err_t Net::Start(const Config& config) {
    config_ = config;
    lock_ = xSemaphoreCreateMutex();
    if (lock_ == nullptr) return ESP_ERR_NO_MEM;
    link_ = Link::kWifiConnecting;
    ESP_ERROR_CHECK(StartWifi());
    /* 8 KB was not enough: esp_http_client plus cJSON plus the TCP stack's
     * callbacks overflowed it, and the resulting memory corruption first
     * surfaced as a crash inside the Wi-Fi driver rather than here. */
    if (xTaskCreate(&Net::TaskEntry, "beacon_net", 12288, this, 5, nullptr)
        != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void Net::TaskEntry(void* arg) { static_cast<Net*>(arg)->Task(); }

bool Net::ParseState(const char* body, int length) {
    cJSON* root = cJSON_ParseWithLength(body, length);
    if (root == nullptr) {
        ESP_LOGW(kTag, "state parse failed");
        return false;
    }

    Fleet& next = staging_;
    next = Fleet{};
    next.rev = static_cast<uint32_t>(
        NumberOr(cJSON_GetObjectItemCaseSensitive(root, "rev"), 0));
    next.hour = static_cast<uint8_t>(
        NumberOr(cJSON_GetObjectItemCaseSensitive(root, "hh"), 0));
    next.minute = static_cast<uint8_t>(
        NumberOr(cJSON_GetObjectItemCaseSensitive(root, "mm"), 0));
    CopyString(next.date_label, sizeof(next.date_label),
               cJSON_GetObjectItemCaseSensitive(root, "d"));

    const cJSON* counts = cJSON_GetObjectItemCaseSensitive(root, "n");
    if (counts != nullptr) {
        next.n_working = NumberOr(cJSON_GetObjectItemCaseSensitive(counts, "w"), 0);
        next.n_blocked = NumberOr(cJSON_GetObjectItemCaseSensitive(counts, "b"), 0);
        next.n_done = NumberOr(cJSON_GetObjectItemCaseSensitive(counts, "d"), 0);
        next.n_idle = NumberOr(cJSON_GetObjectItemCaseSensitive(counts, "i"), 0);
        next.n_machines = NumberOr(cJSON_GetObjectItemCaseSensitive(counts, "m"), 0);
    }

    const cJSON* agents = cJSON_GetObjectItemCaseSensitive(root, "agents");
    const cJSON* item = nullptr;
    cJSON_ArrayForEach(item, agents) {
        if (next.count >= kMaxAgents) break;
        Agent& a = next.agents[next.count];
        a = Agent{};
        CopyString(a.id, sizeof(a.id), cJSON_GetObjectItemCaseSensitive(item, "id"));
        if (a.id[0] == '\0') continue;
        CopyString(a.machine, sizeof(a.machine),
                   cJSON_GetObjectItemCaseSensitive(item, "m"));
        CopyString(a.title, sizeof(a.title),
                   cJSON_GetObjectItemCaseSensitive(item, "t"));
        CopyString(a.project, sizeof(a.project),
                   cJSON_GetObjectItemCaseSensitive(item, "p"));
        CopyString(a.branch, sizeof(a.branch),
                   cJSON_GetObjectItemCaseSensitive(item, "b"));
        CopyString(a.activity, sizeof(a.activity),
                   cJSON_GetObjectItemCaseSensitive(item, "a"));
        CopyString(a.question, sizeof(a.question),
                   cJSON_GetObjectItemCaseSensitive(item, "q"));

        const cJSON* status = cJSON_GetObjectItemCaseSensitive(item, "s");
        a.status = ParseStatus(cJSON_IsString(status) ? status->valuestring
                                                      : nullptr);
        a.status_age_s = static_cast<uint32_t>(
            NumberOr(cJSON_GetObjectItemCaseSensitive(item, "age"), 0));
        a.tokens = static_cast<uint32_t>(
            NumberOr(cJSON_GetObjectItemCaseSensitive(item, "tk"), 0));
        a.cost_milli = static_cast<uint32_t>(
            NumberOr(cJSON_GetObjectItemCaseSensitive(item, "c"), 0));
        a.focused = cJSON_GetObjectItemCaseSensitive(item, "f") != nullptr;
        a.actionable = cJSON_GetObjectItemCaseSensitive(item, "w") != nullptr;

        const cJSON* diff = cJSON_GetObjectItemCaseSensitive(item, "dl");
        if (cJSON_IsArray(diff) && cJSON_GetArraySize(diff) == 2) {
            a.lines_added = cJSON_GetArrayItem(diff, 0)->valueint;
            a.lines_removed = cJSON_GetArrayItem(diff, 1)->valueint;
        }

        const cJSON* actions = cJSON_GetObjectItemCaseSensitive(item, "ax");
        const cJSON* action = nullptr;
        cJSON_ArrayForEach(action, actions) {
            if (a.action_count >= kMaxActions) break;
            if (!cJSON_IsArray(action) || cJSON_GetArraySize(action) < 2) continue;
            CopyString(a.actions[a.action_count].id,
                       sizeof(a.actions[a.action_count].id),
                       cJSON_GetArrayItem(action, 0));
            CopyString(a.actions[a.action_count].label,
                       sizeof(a.actions[a.action_count].label),
                       cJSON_GetArrayItem(action, 1));
            ++a.action_count;
        }
        ++next.count;
    }
    cJSON_Delete(root);

    if (xSemaphoreTake(lock_, pdMS_TO_TICKS(500)) != pdTRUE) return false;
    fleet_ = next;
    xSemaphoreGive(lock_);
    rev_ = next.rev;
    have_fleet_ = true;
    fleet_changed_ = true;
    last_sync_us_ = esp_timer_get_time();
    ESP_LOGI(kTag, "rev %u: %u agents (%u working, %u blocked)",
             static_cast<unsigned>(next.rev), next.count, next.n_working,
             next.n_blocked);
    return true;
}

void Net::SendPending() {
    if (!pending_) return;
    char url[160];
    snprintf(url, sizeof(url), "%s/v1/action", config_.hub);

    char payload[80];
    snprintf(payload, sizeof(payload), "{\"agent\":\"%s\",\"action\":\"%s\"}",
             pending_agent_, pending_action_);

    esp_http_client_config_t config = {};
    config.url = url;
    config.method = HTTP_METHOD_POST;
    config.timeout_ms = 8000;
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == nullptr) {
        pending_ = false;
        return;
    }
    esp_http_client_set_header(client, "Content-Type", "application/json");
    if (config_.token[0] != '\0') {
        char auth[80];
        snprintf(auth, sizeof(auth), "Bearer %.64s", config_.token);
        esp_http_client_set_header(client, "Authorization", auth);
    }
    esp_http_client_set_post_field(client, payload, strlen(payload));
    const esp_err_t err = esp_http_client_perform(client);
    ESP_LOGI(kTag, "action %s/%s -> %s (%d)", pending_agent_, pending_action_,
             esp_err_to_name(err), esp_http_client_get_status_code(client));
    esp_http_client_cleanup(client);
    pending_ = false;
}

bool Net::PollOnce() {
    static char* body = nullptr;
    if (body == nullptr) {
        // 12 KB of response buffer is a waste of internal RAM.
        body = static_cast<char*>(BigAlloc(kBodyCap));
        if (body == nullptr) return false;
    }

    char url[192];
    snprintf(url, sizeof(url), "%s/v1/state?rev=%u&wait=%d", config_.hub,
             static_cast<unsigned>(rev_), kLongPollSeconds);

    Sink sink{body, 0, kBodyCap, false};
    esp_http_client_config_t config = {};
    config.url = url;
    config.method = HTTP_METHOD_GET;
    config.event_handler = HttpEvent;
    config.user_data = &sink;
    // Comfortably longer than the server's hold, so a healthy long-poll is
    // never mistaken for a dead hub.
    config.timeout_ms = (kLongPollSeconds + 15) * 1000;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == nullptr) return false;
    if (config_.token[0] != '\0') {
        char auth[80];
        snprintf(auth, sizeof(auth), "Bearer %.64s", config_.token);
        esp_http_client_set_header(client, "Authorization", auth);
    }

    const esp_err_t err = esp_http_client_perform(client);
    const int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK) {
        ESP_LOGW(kTag, "poll failed: %s", esp_err_to_name(err));
        return false;
    }
    if (status == 304) {
        last_sync_us_ = esp_timer_get_time();
        return true;   // Healthy: nothing changed.
    }
    if (status != 200) {
        ESP_LOGW(kTag, "hub returned %d", status);
        return false;
    }
    if (sink.overflow) {
        ESP_LOGW(kTag, "state payload exceeded %d bytes", kBodyCap);
        return false;
    }
    return ParseState(body, sink.length);
}

void Net::Task() {
    ESP_LOGI(kTag, "waiting for wi-fi");
    for (;;) {
        const EventBits_t bits = xEventGroupWaitBits(
            g_wifi_events, kWifiConnected, pdFALSE, pdFALSE,
            pdMS_TO_TICKS(1000));
        if (bits & kWifiConnected) break;
        link_ = Link::kWifiConnecting;
    }

    esp_netif_t* netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t info;
    if (netif != nullptr && esp_netif_get_ip_info(netif, &info) == ESP_OK) {
        snprintf(ip_, sizeof(ip_), IPSTR, IP2STR(&info.ip));
    }
    link_ = Link::kHubConnecting;
    ESP_LOGI(kTag, "ip %s, hub %s", ip_, config_.hub);

    int failures = 0;
    for (;;) {
        wifi_ap_record_t ap;
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) rssi_ = ap.rssi;

        SendPending();
        if (PollOnce()) {
            failures = 0;
            link_ = Link::kOnline;
        } else {
            ++failures;
            // One miss is a hiccup; keep showing the last good fleet and say
            // so. Several in a row means the hub is genuinely gone.
            link_ = have_fleet_ ? Link::kDegraded : Link::kOffline;
            vTaskDelay(pdMS_TO_TICKS(failures > 5 ? 10000 : 2000));
        }
    }
}

}  // namespace beacon
