# SPDX-License-Identifier: Apache-2.0
# components/freertos/CMakeLists.txt
#
# Overrides ESP-IDF's built-in freertos component.
# Two modes controlled by CONFIG_RTOS_SELECTION_THREADX (from threadx Kconfig):
#   ThreadX mode: compile our ThreadX-backed FreeRTOS compat layer
#   FreeRTOS mode: compile the REAL FreeRTOS sources from $IDF_PATH

idf_build_get_property(non_os_build NON_OS_BUILD)
if(non_os_build)
    idf_component_register()
    return()
endif()

if(CONFIG_RTOS_SELECTION_THREADX)
    #
    # ── ThreadX Mode ─────────────────────────────────────────────
    #
    set(TXFR_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../threadx/threadx/utility/rtos_compatibility_layers/FreeRTOS)

    idf_component_register(
        SRCS
            "${TXFR_DIR}/tx_freertos.c"
            "threadx/src/app_startup.c"
            "threadx/src/port.c"
            "threadx/src/freertos_hooks.c"
            "threadx/src/rtos_int_hooks.S"
        INCLUDE_DIRS
            "threadx/include"      # Our freertos/ headers + FreeRTOSConfig.h (FIRST)
            "${TXFR_DIR}"          # Upstream compat (base type declarations)
        REQUIRES
            threadx esp_system log esp_common newlib heap esp_hw_support
    )

    # Suppress warnings in upstream compat code
    target_compile_options(${COMPONENT_LIB} PRIVATE
        -Wno-unused-parameter -Wno-sign-compare
        -Wno-error=return-type -Wno-unused-variable
    )

    # Bug 30 workaround: explicitly re-export threadx include dirs and compile defs.
    # CMake component propagation breaks when component name matches submodule dir.
    idf_component_get_property(threadx_dir threadx COMPONENT_DIR)
    target_include_directories(${COMPONENT_LIB} PUBLIC
        "${threadx_dir}/threadx/common/inc"
        "${threadx_dir}/threadx/ports/risc-v64/gnu/inc"
        "${threadx_dir}/include"
    )
    target_compile_definitions(${COMPONENT_LIB} PUBLIC TX_INCLUDE_USER_DEFINE_FILE)

    # Force linker to keep app_main symbol
    target_link_libraries(${COMPONENT_LIB} INTERFACE "-u app_main")

    # ── Suppress pointer-type warnings in ESP-IDF components ────────
    #
    # ESP-IDF code (esp_adapter.c, esp_coex_adapter.c) assumes FreeRTOS
    # handles are void* (which they are in real FreeRTOS). Our ThreadX
    # compat layer uses concrete struct pointers (txfr_sem_t *, etc.)
    # that have identical ABI (same pointer size) but different C types.
    #
    # Example: esp_adapter.c line 154 does:
    #   SemaphoreHandle_t *sem = (SemaphoreHandle_t *)(data);
    #   vSemaphoreDelete(sem);  // passes txfr_sem_t** where txfr_sem_t* expected
    #
    # This is safe at runtime (all pointers are 4 bytes on RISC-V) but
    # triggers -Wincompatible-pointer-types → error. We suppress it for
    # components we cannot modify.
    #
    # We use idf_build_set_property(COMPILE_OPTIONS ...) to add this globally
    # because target_compile_options on __idf_esp_wifi fails — that target
    # hasn't been created yet when our CMakeLists.txt runs (component
    # registration order is not guaranteed in ESP-IDF).
    idf_build_set_property(COMPILE_OPTIONS "$<$<COMPILE_LANGUAGE:C>:-Wno-incompatible-pointer-types>" APPEND)

else()
    #
    # ── FreeRTOS Mode ────────────────────────────────────────────
    # Compile the REAL ESP-IDF FreeRTOS from $IDF_PATH by absolute path.
    # ESP32-C6 only (RISC-V, single-core).
    #
    idf_build_get_property(idf_path IDF_PATH)
    set(FR "${idf_path}/components/freertos")
    set(K "FreeRTOS-Kernel")

    idf_component_register(
        SRCS
            "${FR}/heap_idf.c"
            "${FR}/app_startup.c"
            "${FR}/port_common.c"
            "${FR}/port_systick.c"
            "${FR}/${K}/list.c"
            "${FR}/${K}/queue.c"
            "${FR}/${K}/tasks.c"
            "${FR}/${K}/timers.c"
            "${FR}/${K}/event_groups.c"
            "${FR}/${K}/stream_buffer.c"
            "${FR}/${K}/portable/riscv/port.c"
            "${FR}/${K}/portable/riscv/portasm.S"
            "${FR}/esp_additions/freertos_compatibility.c"
            "${FR}/esp_additions/idf_additions.c"
        INCLUDE_DIRS
            "${FR}/config/include"
            "${FR}/config/include/freertos"
            "${FR}/config/riscv/include"
            "${FR}/${K}/include"
            "${FR}/${K}/portable/riscv/include"
            "${FR}/${K}/portable/riscv/include/freertos"
            "${FR}/esp_additions/include"
        PRIV_INCLUDE_DIRS
            "${FR}"
            "${FR}/${K}/include/freertos"
            "${FR}/esp_additions"
        LDFRAGMENTS
            "${FR}/linker_common.lf"
            "${FR}/linker.lf"
    )

    target_link_libraries(${COMPONENT_LIB} INTERFACE "-u app_main")

    if(CONFIG_FREERTOS_DEBUG_OCDAWARE)
        target_link_libraries(${COMPONENT_LIB} INTERFACE
            "-Wl,--undefined=FreeRTOS_openocd_params")
    endif()
endif()
