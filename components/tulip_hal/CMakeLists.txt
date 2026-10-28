idf_component_register(
    SRCS
        "src/display.c"
        "src/fs.c"
        "src/gt911.c"
        "src/input.c"
        "src/lv_mem_psram.c"
        "src/settings.c"
        "src/sys.c"
        "src/touch_aim.c"
        "src/touch_cal.c"
        "src/touchcal_ui.c"
    INCLUDE_DIRS "include"
    REQUIRES lvgl esp_driver_i2c esp_driver_gpio esp_lcd nvs_flash app_update esp_partition fatfs wear_levelling
    PRIV_REQUIRES esp_driver_ledc esp_timer
)

# LVGL calls our allocator (src/lv_mem_psram.c) but links after us: pull it in.
target_link_libraries(${COMPONENT_LIB} INTERFACE "-u lv_malloc_core")
