# SPDX-License-Identifier: Apache-2.0
if(CONFIG_RTOS_SELECTION_THREADX)
    # ThreadX mode: FreeRTOS API calls go through the freertos_compat layer.
    idf_component_register(
        SRCS "main.c"
        INCLUDE_DIRS "."
        REQUIRES freertos_compat
    )
else()
    # FreeRTOS mode: standard ESP-IDF FreeRTOS, no extra components needed.
    idf_component_register(
        SRCS "main.c"
        INCLUDE_DIRS "."
    )
endif()
