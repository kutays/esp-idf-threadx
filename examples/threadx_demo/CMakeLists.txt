cmake_minimum_required(VERSION 3.16)

# Pull in the ThreadX component from the parent repository.
# The threadx_demo uses native ThreadX API only — no FreeRTOS compat override.
set(EXTRA_COMPONENT_DIRS
    "${CMAKE_CURRENT_SOURCE_DIR}/../../components/threadx"
)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(threadx_demo)
