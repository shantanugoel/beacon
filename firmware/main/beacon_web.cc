#include "beacon_web.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "cJSON.h"
#include "esp_log.h"

extern const char configurator_html_start[]
    asm("_binary_configurator_html_start");
extern const char configurator_html_end[]
    asm("_binary_configurator_html_end");

namespace beacon {
namespace {

constexpr char kTag[] = "beacon.web";

Web* Self(httpd_req_t* request) {
    return static_cast<Web*>(request->user_ctx);
}

const char* LinkName(Link link) {
    switch (link) {
        case Link::kOnline: return "online";
        case Link::kWifiConnecting: return "connecting";
        case Link::kHubConnecting: return "connecting";
        case Link::kDegraded: return "degraded";
        case Link::kOffline: return "offline";
        default: return "booting";
    }
}

bool ReadBody(httpd_req_t* request, char* out, size_t capacity) {
    if (request->content_len <= 0 ||
        static_cast<size_t>(request->content_len) >= capacity) {
        return false;
    }
    int received = 0;
    while (received < request->content_len) {
        const int n = httpd_req_recv(request, out + received,
                                     request->content_len - received);
        if (n <= 0) return false;
        received += n;
    }
    out[received] = '\0';
    return true;
}

}  // namespace

esp_err_t Web::SendJson(httpd_req_t* request, const char* json) {
    httpd_resp_set_type(request, "application/json");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    return httpd_resp_sendstr(request, json);
}

esp_err_t Web::Root(httpd_req_t* request) {
    httpd_resp_set_type(request, "text/html; charset=utf-8");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    return httpd_resp_send(request, configurator_html_start,
                           configurator_html_end - configurator_html_start);
}

esp_err_t Web::Status(httpd_req_t* request) {
    Web* self = Self(request);
    WebStatus status;
    if (self == nullptr || self->provider_ == nullptr) {
        return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "status unavailable");
    }
    self->provider_(&status);

    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "device", "ZECTRIX NOTE4");
    cJSON_AddStringToObject(root, "firmware", status.device.firmware);
    cJSON_AddStringToObject(root, "ip", status.device.ip);
    cJSON_AddStringToObject(root, "ssid", status.device.ssid);
    cJSON_AddStringToObject(root, "hub", status.device.hub);
    cJSON_AddStringToObject(root, "link", LinkName(status.device.link));
    cJSON_AddNumberToObject(root, "rssi", status.device.rssi);
    cJSON_AddNumberToObject(root, "battery_percent",
                            status.device.battery_percent);
    cJSON_AddNumberToObject(root, "battery_mv", status.device.battery_mv);
    cJSON_AddBoolToObject(root, "battery_valid", status.device.battery_valid);
    cJSON_AddBoolToObject(root, "charging", status.device.charging);
    cJSON_AddNumberToObject(root, "uptime_s", status.device.uptime_s);
    cJSON_AddNumberToObject(root, "last_sync_age_s",
                            status.device.last_sync_age_s);
    cJSON_AddNumberToObject(root, "full_refreshes",
                            status.device.full_refreshes);
    cJSON_AddNumberToObject(root, "partial_refreshes",
                            status.device.partial_refreshes);
    cJSON_AddBoolToObject(root, "chirp", status.config.chirp);
    cJSON_AddNumberToObject(root, "quiet_after_s",
                            status.config.quiet_after_s);
    cJSON_AddBoolToObject(root, "wifi_max_power_save",
                          status.config.wifi_max_power_save);
    char* json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (json == nullptr) return ESP_ERR_NO_MEM;
    const esp_err_t result = SendJson(request, json);
    cJSON_free(json);
    return result;
}

bool Web::Enqueue(const WebCommand& command) {
    return commands_ != nullptr && xQueueSend(commands_, &command, 0) == pdTRUE;
}

esp_err_t Web::SaveConfig(httpd_req_t* request) {
    char body[256];
    if (!ReadBody(request, body, sizeof(body))) {
        return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST,
                                   "invalid body");
    }
    cJSON* root = cJSON_Parse(body);
    if (root == nullptr) {
        return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST,
                                   "invalid JSON");
    }
    const cJSON* chirp = cJSON_GetObjectItemCaseSensitive(root, "chirp");
    const cJSON* quiet = cJSON_GetObjectItemCaseSensitive(root, "quiet_after_s");
    const cJSON* wifi =
        cJSON_GetObjectItemCaseSensitive(root, "wifi_max_power_save");
    if (!cJSON_IsBool(chirp) || !cJSON_IsNumber(quiet) || !cJSON_IsBool(wifi)) {
        cJSON_Delete(root);
        return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST,
                                   "missing config value");
    }
    WebCommand command;
    command.type = WebCommandType::kSaveConfig;
    command.chirp = cJSON_IsTrue(chirp);
    command.quiet_after_s = std::clamp(quiet->valueint, 30, 3600);
    command.wifi_max_power_save = cJSON_IsTrue(wifi);
    cJSON_Delete(root);
    if (!Self(request)->Enqueue(command)) {
        return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "command queue busy");
    }
    return SendJson(request, "{\"ok\":true,\"message\":\"Settings queued\"}");
}

esp_err_t Web::Command(httpd_req_t* request) {
    WebCommand command;
    if (strcmp(request->uri, "/api/test/chirp") == 0) {
        command.type = WebCommandType::kTestChirp;
    } else if (strcmp(request->uri, "/api/test/led") == 0) {
        command.type = WebCommandType::kTestLed;
    } else if (strcmp(request->uri, "/api/test/display") == 0) {
        command.type = WebCommandType::kRefreshDisplay;
    } else if (strcmp(request->uri, "/api/show/system") == 0) {
        command.type = WebCommandType::kShowSystem;
    } else {
        return httpd_resp_send_err(request, HTTPD_404_NOT_FOUND, "not found");
    }
    if (!Self(request)->Enqueue(command)) {
        return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "command queue busy");
    }
    return SendJson(request, "{\"ok\":true,\"message\":\"Test queued\"}");
}

esp_err_t Web::Start(WebStatusProvider provider) {
    provider_ = provider;
    commands_ = xQueueCreate(8, sizeof(WebCommand));
    if (commands_ == nullptr) return ESP_ERR_NO_MEM;

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 8;
    config.lru_purge_enable = true;
    esp_err_t err = httpd_start(&server_, &config);
    if (err != ESP_OK) return err;

    const httpd_uri_t routes[] = {
        {"/", HTTP_GET, Root, this},
        {"/api/status", HTTP_GET, Status, this},
        {"/api/config", HTTP_POST, SaveConfig, this},
        {"/api/test/chirp", HTTP_POST, Command, this},
        {"/api/test/led", HTTP_POST, Command, this},
        {"/api/test/display", HTTP_POST, Command, this},
        {"/api/show/system", HTTP_POST, Command, this},
    };
    for (const auto& route : routes) {
        err = httpd_register_uri_handler(server_, &route);
        if (err != ESP_OK) return err;
    }
    ESP_LOGI(kTag, "configurator listening on port %u", config.server_port);
    return ESP_OK;
}

bool Web::TakeCommand(WebCommand* out) {
    return out != nullptr && commands_ != nullptr &&
           xQueueReceive(commands_, out, 0) == pdTRUE;
}

}  // namespace beacon
