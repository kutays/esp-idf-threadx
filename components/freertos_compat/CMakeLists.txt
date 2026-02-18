# FreeRTOS compatibility layer using ThreadX's official compat layer
#
# Uses tx_freertos.c and headers from the ThreadX submodule's
# utility/rtos_compatibility_layers/FreeRTOS/ directory.
# Supplements with ESP-IDF-specific bits (portmacro.h, newlib locks,
# xTaskCreatePinnedToCore).

if(NOT CONFIG_RTOS_SELECTION_THREADX)
    idf_component_register()
    return()
endif()

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

# Workaround: explicitly re-export threadx's public include dirs AND compile
# definitions.  Normally REQUIRES threadx propagates both, but
# component_requirements.py has a pre-existing bug when the component name
# matches its own submodule directory (components/threadx/threadx/), breaking
# ALL transitive propagation — include paths AND compile definitions.
#
# Without TX_INCLUDE_USER_DEFINE_FILE: tx_api.h never #includes tx_user.h,
# TX_THREAD_USER_EXTENSION is never defined, txfr_thread_ptr is absent from
# TX_THREAD, and tx_freertos.c fails with "has no member named txfr_thread_ptr".
#
# Without the include paths: tx_api.h itself is not found at all (Bug 30).
idf_component_get_property(threadx_dir threadx COMPONENT_DIR)
target_include_directories(${COMPONENT_LIB} PUBLIC
    "${threadx_dir}/threadx/common/inc"
    "${threadx_dir}/threadx/ports/risc-v64/gnu/inc"
    "${threadx_dir}/include"
)
target_compile_definitions(${COMPONENT_LIB} PUBLIC TX_INCLUDE_USER_DEFINE_FILE)
