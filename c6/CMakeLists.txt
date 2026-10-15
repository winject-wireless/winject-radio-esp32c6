cmake_minimum_required(VERSION 3.16)
# IDF's project.cmake calls git_describe; set a version before include.
# vX.Y.Z: X.Y is the winject manager<->radio protocol (same m-plane and
# d-plane as winject-radio-esp32), Z this repo's patch level.
set(PROJECT_VER "1.0.0")
add_compile_definitions(WINJECT_VERSION_STRING="v${PROJECT_VER}")
set(EXTRA_COMPONENT_DIRS
    ${CMAKE_CURRENT_LIST_DIR}/../components/sdio_msg
    ${CMAKE_CURRENT_LIST_DIR}/../components/sdio_link_slave)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(winject-esp32c6)
