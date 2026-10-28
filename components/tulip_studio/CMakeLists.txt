idf_component_register(
    SRCS
        "src/audio.c"
        "src/io_panel.c"
        "src/midi_io.c"
        "src/midi_parse.c"
        "src/patch.c"
        "src/rt.c"
        "src/shell.c"
        "src/studio.c"
        "src/tclock.c"
        "src/widgets.c"
    INCLUDE_DIRS "include"
    LDFRAGMENTS "linker.lf"
    REQUIRES lvgl tulip_hal esp_driver_i2c json
    PRIV_REQUIRES esp_driver_gptimer esp_driver_i2s esp_driver_uart esp_driver_gpio nvs_flash
                  esp_app_format freertos
)
