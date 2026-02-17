# FreeRTOS compatibility layer using ThreadX's official compat layer
#
# Uses tx_freertos.c and headers from the ThreadX submodule's
# utility/rtos_compatibility_layers/FreeRTOS/ directory.
# Supplements with ESP-IDF-specific bits (portmacro.h, newlib locks,
# xTaskCreatePinnedToCore).

set(TXFR_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../threadx/threadx/utility/rtos_compatibility_layers/FreeRTOS)

idf_component_register(
    SRCS
        "${TXFR_DIR}/tx_freertos.c"
        "src/port.c"
    INCLUDE_DIRS
        # Our include dir first — provides FreeRTOSConfig.h and portmacro.h
        "include"
        # Then the upstream compat headers (FreeRTOS.h, task.h, etc.)
        "${TXFR_DIR}"
    REQUIRES
        threadx
        log
        esp_common
        newlib
)

# Suppress warnings in upstream compat code
target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wno-unused-parameter
    -Wno-sign-compare
    -Wno-error=return-type
    -Wno-unused-variable
)

# Ensure our FreeRTOSConfig.h is found BEFORE ESP-IDF's real FreeRTOS headers.
# Use BEFORE to prepend to the include search path.
target_include_directories(${COMPONENT_LIB} BEFORE PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${TXFR_DIR}"
)
