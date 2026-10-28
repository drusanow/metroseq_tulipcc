idf_component_register(
    SRCS
        "core/d9_dsp.c"
        "core/d9_json.c"
        "core/d9_model.c"
        "core/d9_samples.c"
        "core/d9_seq.c"
        "d9_mod.c"
        "ui/d9_fxp.c"
        "ui/d9_kitp.c"
        "ui/d9_mixp.c"
        "ui/d9_seqp.c"
        "ui/d9_setp.c"
        "ui/d9_ui.c"
    INCLUDE_DIRS "."
    LDFRAGMENTS "linker.lf"
    PRIV_INCLUDE_DIRS "core" "ui"
    REQUIRES tulip_studio lvgl json
    PRIV_REQUIRES esp_partition esp_driver_i2c
)
