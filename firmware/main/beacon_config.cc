#include "beacon_config.h"

#include "beacon_model.h"

#include <cstdio>
#include <cstring>

#include "argtable3/argtable3.h"
#include "esp_console.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

namespace beacon {
namespace {

constexpr const char* kTag = "beacon.cfg";
constexpr const char* kNamespace = "beacon";

Config* g_config = nullptr;

void GetStr(nvs_handle_t handle, const char* key, char* out, size_t cap) {
    size_t length = cap;
    if (nvs_get_str(handle, key, out, &length) != ESP_OK) {
        out[0] = '\0';
    }
}

}  // namespace

esp_err_t ConfigLoad(Config* out) {
    if (out == nullptr) return ESP_ERR_INVALID_ARG;

    // Compile-time defaults first, so a fresh device is usable without a
    // console session if the builder set them.
    CopyField(out->ssid, sizeof(out->ssid), CONFIG_BEACON_WIFI_SSID);
    CopyField(out->password, sizeof(out->password), CONFIG_BEACON_WIFI_PASSWORD);
    CopyField(out->hub, sizeof(out->hub), CONFIG_BEACON_HUB_URL);
    CopyField(out->token, sizeof(out->token), CONFIG_BEACON_HUB_TOKEN);
    out->quiet_after_s = CONFIG_BEACON_QUIET_AFTER_S;

    nvs_handle_t handle;
    if (nvs_open(kNamespace, NVS_READONLY, &handle) != ESP_OK) {
        return ESP_OK;  // Nothing stored yet; defaults stand.
    }
    char scratch[96];
    GetStr(handle, "ssid", scratch, sizeof(scratch));
    if (scratch[0]) CopyField(out->ssid, sizeof(out->ssid), scratch);
    GetStr(handle, "pass", scratch, sizeof(scratch));
    if (scratch[0]) CopyField(out->password, sizeof(out->password), scratch);
    GetStr(handle, "hub", scratch, sizeof(scratch));
    if (scratch[0]) CopyField(out->hub, sizeof(out->hub), scratch);
    GetStr(handle, "token", scratch, sizeof(scratch));
    if (scratch[0]) CopyField(out->token, sizeof(out->token), scratch);

    int32_t value = 0;
    if (nvs_get_i32(handle, "quiet", &value) == ESP_OK) {
        out->quiet_after_s = value;
    }
    uint8_t flag = 1;
    if (nvs_get_u8(handle, "chirp", &flag) == ESP_OK) {
        out->chirp = flag != 0;
    }
    if (nvs_get_u8(handle, "wifi_ps", &flag) == ESP_OK) {
        out->wifi_max_power_save = flag != 0;
    }
    nvs_close(handle);
    return ESP_OK;
}

esp_err_t ConfigSave(const Config& config) {
    nvs_handle_t handle;
    esp_err_t err = nvs_open(kNamespace, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    nvs_set_str(handle, "ssid", config.ssid);
    nvs_set_str(handle, "pass", config.password);
    nvs_set_str(handle, "hub", config.hub);
    nvs_set_str(handle, "token", config.token);
    nvs_set_i32(handle, "quiet", config.quiet_after_s);
    nvs_set_u8(handle, "chirp", config.chirp ? 1 : 0);
    nvs_set_u8(handle, "wifi_ps", config.wifi_max_power_save ? 1 : 0);
    err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}

bool ConfigComplete(const Config& config) {
    return config.ssid[0] != '\0' && config.hub[0] != '\0';
}

namespace {

struct {
    struct arg_str* key;
    struct arg_str* value;
    struct arg_end* end;
} g_set_args;

int CmdShow(int, char**) {
    const Config& c = *g_config;
    printf("ssid   %s\n", c.ssid);
    printf("pass   %s\n", c.password[0] ? "(set)" : "(empty)");
    printf("hub    %s\n", c.hub);
    printf("token  %s\n", c.token[0] ? "(set)" : "(empty)");
    printf("quiet  %d s\n", c.quiet_after_s);
    printf("chirp  %s\n", c.chirp ? "on" : "off");
    printf("wifi-ps %s\n", c.wifi_max_power_save ? "maximum" : "responsive");
    return 0;
}

int CmdSet(int argc, char** argv) {
    const int errors = arg_parse(argc, argv, (void**)&g_set_args);
    if (errors != 0) {
        arg_print_errors(stderr, g_set_args.end, argv[0]);
        return 1;
    }
    const char* key = g_set_args.key->sval[0];
    const char* value = g_set_args.value->count ? g_set_args.value->sval[0] : "";
    Config& c = *g_config;
    if (strcmp(key, "ssid") == 0) {
        CopyField(c.ssid, sizeof(c.ssid), value);
    } else if (strcmp(key, "pass") == 0) {
        CopyField(c.password, sizeof(c.password), value);
    } else if (strcmp(key, "hub") == 0) {
        CopyField(c.hub, sizeof(c.hub), value);
    } else if (strcmp(key, "token") == 0) {
        CopyField(c.token, sizeof(c.token), value);
    } else if (strcmp(key, "quiet") == 0) {
        c.quiet_after_s = atoi(value);
    } else if (strcmp(key, "chirp") == 0) {
        c.chirp = (strcmp(value, "on") == 0 || strcmp(value, "1") == 0);
    } else if (strcmp(key, "wifi-ps") == 0) {
        c.wifi_max_power_save =
            strcmp(value, "maximum") == 0 || strcmp(value, "max") == 0 ||
            strcmp(value, "on") == 0 || strcmp(value, "1") == 0;
    } else {
        printf("unknown key '%s' (ssid pass hub token quiet chirp wifi-ps)\n", key);
        return 1;
    }
    printf("set %s; run 'beacon-save' then 'beacon-reboot'\n", key);
    return 0;
}

int CmdSave(int, char**) {
    const esp_err_t err = ConfigSave(*g_config);
    printf("%s\n", err == ESP_OK ? "saved" : esp_err_to_name(err));
    return err == ESP_OK ? 0 : 1;
}

int CmdReboot(int, char**) {
    printf("rebooting\n");
    fflush(stdout);
    esp_restart();
    return 0;
}

}  // namespace

void ConfigRegisterConsole(Config* config) {
    g_config = config;

    esp_console_repl_t* repl = nullptr;
    esp_console_repl_config_t repl_config = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_config.prompt = "beacon>";
    repl_config.max_cmdline_length = 160;

    /* The backend-agnostic constructor: this board speaks USB-Serial/JTAG,
     * but esp_console_new_repl_usb_serial_jtag is compiled out unless
     * CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG is set, and binding the console to
     * whatever stdio is configured is the more honest thing to do anyway. */
    if (esp_console_new_repl_stdio(&repl_config, &repl) != ESP_OK) {
        ESP_LOGW(kTag, "console unavailable; using stored config only");
        return;
    }

    g_set_args.key = arg_str1(nullptr, nullptr, "<key>",
                              "ssid|pass|hub|token|quiet|chirp|wifi-ps");
    g_set_args.value = arg_str0(nullptr, nullptr, "<value>", "new value");
    g_set_args.end = arg_end(2);

    const esp_console_cmd_t commands[] = {
        {"beacon-show", "Print the current configuration", nullptr, CmdShow,
         nullptr, nullptr, nullptr},
        {"beacon-set", "Set a configuration key", nullptr, CmdSet,
         &g_set_args, nullptr, nullptr},
        {"beacon-save", "Persist the configuration to NVS", nullptr, CmdSave,
         nullptr, nullptr, nullptr},
        {"beacon-reboot", "Restart the device", nullptr, CmdReboot, nullptr,
         nullptr, nullptr},
    };
    for (const auto& command : commands) {
        ESP_ERROR_CHECK(esp_console_cmd_register(&command));
    }
    ESP_ERROR_CHECK(esp_console_start_repl(repl));
    ESP_LOGI(kTag, "console ready: beacon-show / beacon-set / beacon-save");
}

}  // namespace beacon
