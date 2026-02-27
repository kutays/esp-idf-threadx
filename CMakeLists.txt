# ThreadX on ESP32-C6 — Top-level ESP-IDF project
cmake_minimum_required(VERSION 3.16)

set(EXTRA_COMPONENT_DIRS
    ${CMAKE_CURRENT_SOURCE_DIR}/components/threadx
)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(threadx_esp32c6)
