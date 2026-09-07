idf_component_register(
    SRCS "beacon_ui.cc"
         "beacon_chrome.cc"
         "beacon_screens.cc"
         "beacon_quiet.cc"
    INCLUDE_DIRS "include"
    REQUIRES beacon_gfx
)
