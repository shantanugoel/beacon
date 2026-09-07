idf_component_register(
    SRCS "app_main.cc"
         "beacon_config.cc"
         "beacon_net.cc"
         "beacon_display.cc"
    INCLUDE_DIRS "."
    REQUIRES beacon_gfx beacon_ui zectrix_epd zectrix_board
             nvs_flash esp_wifi esp_http_client esp_netif console
             esp_timer driver
)
