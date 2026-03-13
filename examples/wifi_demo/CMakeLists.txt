# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.16)

set(EXTRA_COMPONENT_DIRS
    "${CMAKE_CURRENT_SOURCE_DIR}/../../components/threadx"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../components/freertos"   # Override ESP-IDF's freertos
)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(wifi_demo)
