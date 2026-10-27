idf_component_register(
    SRCS
        "engine/seq_clock.c"
        "engine/seq_engine.c"
        "engine/seq_model.c"
        "platform/cv_dac.c"
        "platform/metro_mod.c"
        "platform/preview_audio.c"
        "platform/storage.c"
        "ui/ui.c"
        "ui/ui_common.c"
        "ui/ui_mod.c"
        "ui/ui_seq.c"
        "ui/ui_set.c"
        "ui/ui_trk.c"
    INCLUDE_DIRS "."
    LDFRAGMENTS "linker.lf"
    PRIV_INCLUDE_DIRS "platform"
    REQUIRES tulip_studio tulip_hal lvgl
    PRIV_REQUIRES esp_driver_gpio esp_driver_i2c nvs_flash esp_timer
)
