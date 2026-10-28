cmake_minimum_required(VERSION 3.16)

# Shared Tulip components and the app modules, on the shared base config.
set(TULIP_ROOT ${CMAKE_CURRENT_LIST_DIR}/../..)
set(EXTRA_COMPONENT_DIRS ${TULIP_ROOT}/components ${TULIP_ROOT}/modules)
set(SDKCONFIG_DEFAULTS "${TULIP_ROOT}/components/tulip_hal/sdkconfig.tulip;sdkconfig.defaults")
set(PROJECT_VER "1.0.0")   # shown by the launcher

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(drumster9)
