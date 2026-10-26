idf_component_register(
    SRCS "sloe_dsp.c" "sloe_mod.c"
    INCLUDE_DIRS "."
    REQUIRES tulip_studio lvgl json
)
