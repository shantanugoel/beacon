#ifndef BEACON_WEB_H_
#define BEACON_WEB_H_

#include <cstdint>

#include "beacon_config.h"
#include "beacon_model.h"
#include "esp_err.h"
#include "esp_http_server.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

namespace beacon {

enum class WebCommandType : uint8_t {
    kSaveConfig,
    kTestChirp,
    kTestLed,
    kRefreshDisplay,
    kShowSystem,
};

struct WebCommand {
    WebCommandType type = WebCommandType::kRefreshDisplay;
    int quiet_after_s = 180;
    bool chirp = true;
    bool wifi_max_power_save = true;
};

struct WebStatus {
    Device device{};
    Config config{};
};

using WebStatusProvider = void (*)(WebStatus* out);

/* Small, LAN-only control surface. It never touches the panel or codec from
 * the HTTP task; commands are queued back to the single-owner UI loop. */
class Web {
public:
    esp_err_t Start(WebStatusProvider provider);
    bool TakeCommand(WebCommand* out);

private:
    static esp_err_t Root(httpd_req_t* request);
    static esp_err_t Status(httpd_req_t* request);
    static esp_err_t SaveConfig(httpd_req_t* request);
    static esp_err_t Command(httpd_req_t* request);
    static esp_err_t SendJson(httpd_req_t* request, const char* json);
    bool Enqueue(const WebCommand& command);

    httpd_handle_t server_ = nullptr;
    QueueHandle_t commands_ = nullptr;
    WebStatusProvider provider_ = nullptr;
};

}  // namespace beacon

#endif  // BEACON_WEB_H_
