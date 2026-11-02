idf_component_register(
    SRCS
        "core/gx_dsp.c"
        "core/gx_engine.c"
        "core/gx_factory.c"
        "core/gx_json.c"
        "core/gx_model.c"
        "core/gx_seq.c"
        "gx_mod.c"
        "ui/gx_header.c"
        "ui/gx_main.c"
        "ui/gx_midip.c"
        "ui/gx_seqp.c"
        "ui/gx_tools.c"
        "ui/gx_ui.c"
        "ui/gx_widgets.c"
    INCLUDE_DIRS "."
    LDFRAGMENTS "linker.lf"
    PRIV_INCLUDE_DIRS "core" "ui"
    REQUIRES tulip_studio lvgl json
    PRIV_REQUIRES esp_timer
)
