# Apps loaded from /files/studio/apps (an experiment): the functions they may
# call, generated from the public headers + abi/extra.txt (tools/tulip_abi.py)
set(abi_c "${CMAKE_CURRENT_BINARY_DIR}/abi_exports.c")
set(abi_tool "${CMAKE_CURRENT_LIST_DIR}/../../tools/tulip_abi.py")
if(NOT CMAKE_BUILD_EARLY_EXPANSION)
    set(abi_src "${abi_c}")
endif()

idf_component_register(
    SRCS
        "src/audio.c"
        "src/io_panel.c"
        "src/loader.c"
        "src/midi_io.c"
        "src/midi_parse.c"
        "src/patch.c"
        "src/rt.c"
        "src/shell.c"
        "src/studio.c"
        "src/tclock.c"
        "src/widgets.c"
        ${abi_src}
    INCLUDE_DIRS "include"
    LDFRAGMENTS "linker.lf"
    REQUIRES lvgl tulip_hal esp_driver_i2c json
    PRIV_REQUIRES esp_driver_gptimer esp_driver_i2s esp_driver_uart esp_driver_gpio nvs_flash
                  esp_app_format freertos elf_loader
)

if(NOT CMAKE_BUILD_EARLY_EXPANSION)
    file(GLOB abi_headers "${CMAKE_CURRENT_LIST_DIR}/include/studio/*.h")
    add_custom_command(
        OUTPUT "${abi_c}"
        COMMAND python3 "${abi_tool}" gen "${abi_c}"
        DEPENDS "${abi_tool}" "${CMAKE_CURRENT_LIST_DIR}/abi/extra.txt" ${abi_headers}
        COMMENT "Tulip Studio app ABI: export table"
        VERBATIM)
endif()
