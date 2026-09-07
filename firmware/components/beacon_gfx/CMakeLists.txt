idf_component_register(
    SRCS "beacon_canvas.cc"
         "beacon_gray.cc"
         "beacon_mem.cc"
         "generated/beacon_fonts.c"
    INCLUDE_DIRS "include" "generated"
    REQUIRES esp_common heap
)
