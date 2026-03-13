# ThreadX on ESP32-C6

> **⚠️ Work in Progress — Not Production Ready**
>
> This is an experimental port of Eclipse ThreadX to the ESP32-C6 via ESP-IDF.
> It is under active development and is not suitable for production use.

## About

This project ports [Eclipse ThreadX](https://github.com/eclipse-threadx/threadx) (MIT)
as the RTOS for the ESP32-C6, running alongside the ESP-IDF component framework and its
WiFi/networking stack. The FreeRTOS API surface expected by ESP-IDF is provided through
ThreadX's upstream FreeRTOS compatibility layer, extended with ESP-IDF-specific shims.

Please see the project notes.

## How It Works

- **Port selection**: Uses the `risc-v64/gnu` ThreadX port on a 32-bit core the only
  available GNU toolchain port; the RV32 port is IAR-only.
- **Kernel entry**: A weak `port_start_app_hook()` override calls `tx_kernel_enter()`
  just before ESP-IDF would call `vTaskStartScheduler()`, handing control to ThreadX.
- **FreeRTOS compatibility**: ThreadX's upstream compat layer (`tx_freertos.c`) translates
  `xTaskCreate`, `xQueueSend`, semaphores, event groups, etc. into ThreadX primitives,
  allowing ESP-IDF components (WiFi, lwIP, NVS, esp_timer) to run unmodified.
- **Interrupt architecture**: Dual-path vectored dispatch — vector[17] routes to the
  ThreadX trap handler (SYSTIMER 100 Hz tick); all other vectors route to ESP-IDF's
  `_interrupt_handler` (WiFi, esp_timer, BLE). `_tx_thread_system_state` is kept
  consistent across both paths via custom `rtos_int_enter`/`rtos_int_exit` hooks.
- **Timer**: Direct PLIC/INTMTX register writes for the tick interrupt (level-triggered,
  SYSTIMER counter 1 / alarm 0, CPU line 17). `esp_intr_alloc()` is deliberately avoided.
- **Pre-kernel task fix**: ESP-IDF secondary init creates FreeRTOS tasks before
  `tx_kernel_enter()`. ThreadX's `_tx_thread_initialize()` clears them from the scheduler.
  The compat layer tracks and re-registers these tasks after kernel init so they run
  correctly (fixes esp_timer callbacks and WiFi scan completion).
- **PLIC threshold**: Set to 0 in the wifi_demo via a weak/strong `_tx_port_esp_idf_isr_init()`
  pattern so all ESP-IDF interrupt priorities are unmasked; the bare threadx_demo leaves
  the threshold at 1 (only the ThreadX tick fires).

## Examples

| Example | Description |
|---------|-------------|
| `examples/threadx_demo/` | Bare ThreadX multiple threads, tick counter, no WiFi |
| `examples/wifi_demo/` | ThreadX + ESP WiFi STA scan all channels, esp_timer, multi-thread |
| `examples/freertos_demo/` | Pure FreeRTOS reference (unmodified ESP-IDF) |

## Building

Requires ESP-IDF v5.4 and the ESP32-C6 target.

```bash
. $IDF_PATH/export.sh
cd examples/wifi_demo        # or threadx_demo / freertos_demo
idf.py build
idf.py flash monitor
```

After a fresh clone, apply the ThreadX submodule patches:

```bash
bash patches/threadx/apply.sh
```

## Documentation

Detailed implementation notes are in [`docs/implementation/`](docs/implementation/):

| Document | Contents |
|----------|----------|
| [`architecture.md`](docs/implementation/architecture.md) | Overall design and component layout |
| [`hardware-interrupts.md`](docs/implementation/hardware-interrupts.md) | ESP32-C6 PLIC, INTMTX, MTVEC, vectored dispatch |
| [`threadx-port.md`](docs/implementation/threadx-port.md) | ThreadX port details and calling conventions |
| [`freertos-compat.md`](docs/implementation/freertos-compat.md) | FreeRTOS compatibility layer internals |
| [`wifi-port.md`](docs/implementation/wifi-port.md) | WiFi integration — 30+ iterations documented |
| [`assembly-deep-dive.md`](docs/implementation/assembly-deep-dive.md) | Trap handler and context save/restore |
| [`bug-history.md`](docs/implementation/bug-history.md) | Every bug encountered and how it was fixed (Bugs 1–41) |
| [`build-reference.md`](docs/implementation/build-reference.md) | Build system, Kconfig, component structure |
| [`debugging-gdb-openocd.md`](docs/implementation/debugging-gdb-openocd.md) | GDB/OpenOCD setup for JTAG debugging |

## Project Note

This project has been started almost 2.5 years ago. But due to the complexities (after start
porting ESP32 Xtensa and SMP patch requirement for ThreadX) switched to a single core RISCV chip. This was a perfect
opportunity to test the potential and limitations of vibe coding on a complex maintenance / integration heavy project.

Project planned as multiple phases. The first phase was to intercept FreeRTOS and boot with ThreadX. Run test threads to test. The second phase was port the WIFI stack. The this first MVP is a functional ThreadX port that boots, schedules multiple threads,
fires a 100 Hz tick, and runs the ESP32-C6 WiFi stack reached after 41 documented
bugs across dozens of iterations.

## Dependencies

| Dependency | License | Notes |
|------------|---------|-------|
| [Eclipse ThreadX](https://github.com/eclipse-threadx/threadx) | MIT | Git submodule at `components/threadx/threadx/` |
| [ESP-IDF](https://github.com/espressif/esp-idf) | Apache 2.0 | External; set `$IDF_PATH` |

## License

Copyright 2024–2026 Kutay Sanal

Licensed under the Apache License, Version 2.0. See [`LICENSE`](LICENSE) for the
full license text.

ThreadX is © Microsoft Corporation / Eclipse Foundation, licensed under the MIT License.
ESP-IDF is © Espressif Systems, licensed under the Apache License 2.0.
