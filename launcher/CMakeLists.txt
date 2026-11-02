cmake_minimum_required(VERSION 3.16)

# The launcher is the factory app of the Tulip partition table.
set(TULIP_ROOT ${CMAKE_CURRENT_LIST_DIR}/..)
set(EXTRA_COMPONENT_DIRS ${TULIP_ROOT}/components)
set(SDKCONFIG_DEFAULTS "${TULIP_ROOT}/components/tulip_hal/sdkconfig.tulip;sdkconfig.defaults")
set(PROJECT_VER "1.0.0")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(tulip_launcher)
