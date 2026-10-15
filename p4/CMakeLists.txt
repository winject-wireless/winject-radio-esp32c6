cmake_minimum_required(VERSION 3.16)
if(NOT DEFINED PROJECT_VER)
    set(PROJECT_VER "1.0.0")
endif()
set(EXTRA_COMPONENT_DIRS
    ${CMAKE_CURRENT_LIST_DIR}/../components/sdio_msg
    ${CMAKE_CURRENT_LIST_DIR}/../components/sdio_link_host
    ${CMAKE_CURRENT_LIST_DIR}/../components/winject_proxy)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(winject-p4-proxy)
