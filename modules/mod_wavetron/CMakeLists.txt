idf_component_register(
    SRCS "core/wt_params.c" "core/wt_tables.c" "core/wt_dsp.c" "core/wt_fx.c" "core/wt_patch.c"
         "wt_mod.c" "ui/wt_ui.c" "ui/wt_draw.c" "ui/wt_pages.c" "ui/wt_pages2.c"
    INCLUDE_DIRS "."
    PRIV_INCLUDE_DIRS "core" "ui"
    REQUIRES tulip_studio lvgl json
    LDFRAGMENTS "linker.lf"
)
# the audio path: optimise for speed
set_source_files_properties(core/wt_dsp.c core/wt_fx.c core/wt_tables.c
                            PROPERTIES COMPILE_OPTIONS "-O2")
