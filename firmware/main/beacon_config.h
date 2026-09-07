#ifndef BEACON_CONFIG_H_
#define BEACON_CONFIG_H_

#include "esp_err.h"

namespace beacon {

/* Runtime configuration, held in NVS so credentials survive a reflash and can
 * be changed over the serial console without rebuilding. Kconfig supplies the
 * defaults for a first boot. */
struct Config {
    char ssid[33] = {};
    char password[65] = {};
    char hub[96] = {};       /* http://host:8787 */
    char token[65] = {};
    int quiet_after_s = 180; /* inactivity before the ambient screen */
    bool chirp = true;       /* audible alert when something needs you */
};

esp_err_t ConfigLoad(Config* out);
esp_err_t ConfigSave(const Config& config);
bool ConfigComplete(const Config& config);

/* Registers `beacon` console commands (set/show/save/reboot) on UART0. */
void ConfigRegisterConsole(Config* config);

}  // namespace beacon

#endif  // BEACON_CONFIG_H_
