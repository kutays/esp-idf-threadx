# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.16)

# Pull in the shared components from the parent repository.
# threadx and freertos_compat both support FreeRTOS mode (they register as
# empty components when RTOS_SELECTION_FREERTOS is set), so including them
# here makes the RTOS selection menuconfig option available in this project too.
# When running with RTOS_SELECTION_THREADX, the FreeRTOS API calls in main.c
# are transparently handled by the freertos_compat layer — no code changes needed.
set(EXTRA_COMPONENT_DIRS
    "${CMAKE_CURRENT_SOURCE_DIR}/../../components/threadx"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../components/freertos_compat"
)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(freertos_demo)
