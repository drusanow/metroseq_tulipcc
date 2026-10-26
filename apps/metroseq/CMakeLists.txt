cmake_minimum_required(VERSION 3.16)

# Shared Tulip components (tulip_hal, lvgl) and the shared base config.
set(TULIP_ROOT ${CMAKE_CURRENT_LIST_DIR}/../..)
set(EXTRA_COMPONENT_DIRS ${TULIP_ROOT}/components)
set(SDKCONFIG_DEFAULTS "${TULIP_ROOT}/components/tulip_hal/sdkconfig.tulip;sdkconfig.defaults")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(metroseq)
