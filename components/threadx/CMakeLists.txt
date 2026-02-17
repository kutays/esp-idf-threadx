# ThreadX kernel component for ESP-IDF
#
# Builds the ThreadX kernel from the git submodule (threadx/) plus
# our ESP32-C6 port files (port/).

set(THREADX_DIR ${CMAKE_CURRENT_SOURCE_DIR}/threadx)
set(PORT_DIR    ${CMAKE_CURRENT_SOURCE_DIR}/port)

# Collect all ThreadX common kernel sources
file(GLOB THREADX_COMMON_SRCS "${THREADX_DIR}/common/src/*.c")

# RISC-V port assembly — the risc-v64/gnu port is parameterized via
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

# Our ESP32-C6 specific port files (override the generic low-level init)
set(ESP32C6_PORT_SRCS
    ${PORT_DIR}/tx_initialize_low_level.S
    ${PORT_DIR}/tx_timer_interrupt.c
    ${PORT_DIR}/tx_port_startup.c
)

idf_component_register(
    SRCS
        ${THREADX_COMMON_SRCS}
        ${THREADX_PORT_ASM}
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

# Define TX_INCLUDE_USER_DEFINE_FILE so ThreadX picks up our tx_user.h
target_compile_definitions(${COMPONENT_LIB} PUBLIC TX_INCLUDE_USER_DEFINE_FILE)

# Suppress warnings in upstream ThreadX code
target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wno-unused-parameter
    -Wno-sign-compare
    -Wno-old-style-declaration
)
