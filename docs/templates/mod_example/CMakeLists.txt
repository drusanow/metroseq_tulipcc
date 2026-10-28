idf_component_register(
    SRCS "example.c"
    INCLUDE_DIRS "."
    LDFRAGMENTS "linker.lf"
    REQUIRES tulip_studio lvgl json
    PRIV_REQUIRES freertos
)
