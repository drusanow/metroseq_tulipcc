idf_component_register(
    SRCS
        "main.c"
        "engine/seq_engine.c"
        "engine/seq_model.c"
        "engine/midi_clock.c"
        "platform/cv_dac.c"
        "platform/midi_in.c"
        "platform/preview_audio.c"
        "platform/seq_task.c"
        "platform/storage.c"
        "ui/ui.c"
        "ui/ui_common.c"
        "ui/ui_mod.c"
        "ui/ui_seq.c"
        "ui/ui_set.c"
        "ui/ui_trk.c"
    INCLUDE_DIRS "." "platform"
    REQUIRES
        esp_driver_gpio esp_driver_gptimer esp_driver_i2c esp_driver_i2s
        esp_driver_ledc esp_driver_uart esp_lcd esp_timer nvs_flash lvgl tulip_hal
)
