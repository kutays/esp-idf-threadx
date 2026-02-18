# ThreadX kernel component for ESP-IDF
#
# Builds the ThreadX kernel from the git submodule (threadx/) plus
# our ESP32-C6 port files (port/).

if(NOT CONFIG_RTOS_SELECTION_THREADX)
    idf_component_register()
    return()
endif()

set(THREADX_DIR ${CMAKE_CURRENT_SOURCE_DIR}/threadx)
set(PORT_DIR    ${CMAKE_CURRENT_SOURCE_DIR}/port)

# Collect all ThreadX common kernel sources
file(GLOB THREADX_COMMON_SRCS "${THREADX_DIR}/common/src/*.c")

# RISC-V port files — the risc-v64/gnu port is parameterized via
# STORE/LOAD/REGBYTES macros in tx_port.h and auto-adapts to RV32 when
# compiled with __riscv_xlen == 32.
set(THREADX_PORT_ASM
    ${THREADX_DIR}/ports/risc-v64/gnu/src/tx_thread_context_save.S
    ${THREADX_DIR}/ports/risc-v64/gnu/src/tx_thread_context_restore.S
    ${THREADX_DIR}/ports/risc-v64/gnu/src/tx_thread_schedule.S
    ${THREADX_DIR}/ports/risc-v64/gnu/src/tx_thread_stack_build.S
    ${THREADX_DIR}/ports/risc-v64/gnu/src/tx_thread_system_return.S
    ${THREADX_DIR}/ports/risc-v64/gnu/src/tx_thread_interrupt_control.S
)

# Port C file — defines _tx_timer_interrupt (increments system clock,
# checks time-slice and timer expiration). This is separate from the
# port assembly and was missing from the build.
set(THREADX_PORT_C
    ${THREADX_DIR}/ports/risc-v64/gnu/src/tx_timer_interrupt.c
)

# Our ESP32-C6 specific port files (override the generic low-level init)
set(ESP32C6_PORT_SRCS
    ${PORT_DIR}/tx_initialize_low_level.S
    ${PORT_DIR}/tx_esp32c6_timer.c
    ${PORT_DIR}/tx_port_startup.c
)

idf_component_register(
    SRCS
        ${THREADX_COMMON_SRCS}
        ${THREADX_PORT_ASM}
        ${THREADX_PORT_C}
        ${ESP32C6_PORT_SRCS}
    INCLUDE_DIRS
        "${THREADX_DIR}/common/inc"
        "${THREADX_DIR}/ports/risc-v64/gnu/inc"
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
    REQUIRES
        esp_hw_support
        esp_system
        esp_timer
        esp_rom
        soc
        hal
        log
        newlib
)

# esp_private/systimer.h and esp_private/periph_ctrl.h live in esp_hw_support
# private includes — expose them so our port code can use the same HAL
# paths that FreeRTOS's port_systick.c uses.
idf_component_get_property(esp_hw_support_dir esp_hw_support COMPONENT_DIR)
target_include_directories(${COMPONENT_LIB} PRIVATE
    "${esp_hw_support_dir}/include"
    "${esp_hw_support_dir}/include/esp_private"
)

# Define TX_INCLUDE_USER_DEFINE_FILE so ThreadX picks up our tx_user.h
target_compile_definitions(${COMPONENT_LIB} PUBLIC TX_INCLUDE_USER_DEFINE_FILE)

# Force the linker to include tx_port_startup.o from our archive.
#
# port_start_app_hook() is declared __attribute__((weak)) in FreeRTOS's
# app_startup.c. Weak symbols never trigger archive searches — the linker
# accepts "null" for them without looking in .a files. So our strong
# port_start_app_hook() would be silently skipped.
#
# -Wl,-u,port_start_app_hook tells the linker: treat this symbol as
# referenced (undefined), so SEARCH for it in all archives. This pulls in
# tx_port_startup.o and our strong definition wins over the weak one.
target_link_options(${COMPONENT_LIB} INTERFACE "-Wl,-u,port_start_app_hook")

# Suppress warnings in upstream ThreadX code
target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wno-unused-parameter
    -Wno-sign-compare
    -Wno-old-style-declaration
)
