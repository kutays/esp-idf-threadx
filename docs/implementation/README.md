# ThreadX on ESP32-C6 — Implementation Documentation

## Document Index

| File | Contents |
|------|----------|
| [architecture.md](architecture.md) | System overview, boot flow, component relationships |
| [hardware-interrupts.md](hardware-interrupts.md) | ESP32-C6 interrupt hardware: INTMTX, PLIC, SYSTIMER |
| [assembly-deep-dive.md](assembly-deep-dive.md) | Line-by-line explanation of all assembly code |
| [threadx-port.md](threadx-port.md) | ThreadX kernel configuration and port decisions |
| [freertos-compat.md](freertos-compat.md) | FreeRTOS compatibility layer |
| [build-reference.md](build-reference.md) | Build commands, flash procedure, debug tips |
| [bug-history.md](bug-history.md) | Every bug found and fixed during development |

## Quick Overview

This project runs **ThreadX** (Microsoft Azure RTOS, now Eclipse ThreadX) on the
**ESP32-C6** microcontroller using **ESP-IDF** as the board support layer.

The key challenges this project solves:

1. **No standard RISC-V timer**: ESP32-C6 does not have mtime/mtimecmp. Uses the
   proprietary SYSTIMER peripheral instead.

2. **Non-standard interrupt controller**: ESP32-C6 does not have a standard PLIC in
   the usual sense. It has a two-level system: the Interrupt Matrix (INTMTX) routes
   peripheral sources to CPU lines, and the PLIC MX registers (at 0x20001000) control
   those CPU lines.

3. **No risc-v32 GNU port in ThreadX**: The official ThreadX repo has only an IAR port
   for 32-bit RISC-V. The risc-v64/gnu port is used because it is parameterized via
   STORE/LOAD/REGBYTES macros and compiles correctly for RV32.

4. **ESP-IDF FreeRTOS header conflicts**: ESP-IDF's components depend on FreeRTOS headers.
   We must intercept the include path so our thin compatibility shim is found first,
   without breaking ESP-IDF internals.

## Repository Layout

```
threadx-esp32c6-project/
├── CMakeLists.txt                  # Top-level ESP-IDF project
├── sdkconfig.defaults              # Build defaults (target, clock, watchdogs)
├── main/
│   ├── CMakeLists.txt
│   └── main.c                     # Demo: two threads using tx_thread_sleep
├── components/
│   ├── threadx/                   # ThreadX kernel ESP-IDF component
│   │   ├── CMakeLists.txt         # Collects kernel + port sources
│   │   ├── Kconfig                # menuconfig options
│   │   ├── linker.lf              # Place critical code in IRAM
│   │   ├── threadx/               # Git submodule: eclipse-threadx/threadx
│   │   ├── include/
│   │   │   └── tx_user.h          # ThreadX compile-time config
│   │   └── port/
│   │       ├── tx_initialize_low_level.S   # Boot init + trap handler (assembly)
│   │       ├── tx_timer_interrupt.c        # SYSTIMER + PLIC configuration
│   │       └── tx_port_startup.c           # ESP-IDF → ThreadX bridge
│   └── freertos_compat/           # FreeRTOS API shim over ThreadX
│       ├── CMakeLists.txt
│       ├── include/
│       │   ├── FreeRTOSConfig.h   # Config consumed by upstream compat layer
│       │   └── freertos/
│       │       └── portmacro.h    # Minimal port types
│       └── src/
│           └── port.c             # yield, critical sections, newlib locks
└── docs/                          # This documentation
```
