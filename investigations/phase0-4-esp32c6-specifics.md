# Phase 0.4: ESP32-C6 Specific Considerations

**Status:** Complete
**Date:** 2026-02-17
**Scope:** ESP32-C6 hardware architecture, memory map, interrupt system, toolchain, boot process

---

## 1. ESP32-C6 Hardware Overview

### 1.1 Core Specifications

| Feature | Details |
|---------|---------|
| **CPU** | 32-bit RISC-V single-core, up to 160 MHz |
| **ISA** | RV32IMAC (Integer, Multiply, Atomic, Compressed) |
| **WiFi** | 802.11ax (WiFi 6), 2.4 GHz |
| **Bluetooth** | BLE 5.0 (LE only, no Classic BT) |
| **802.15.4** | Thread / Zigbee support |
| **SRAM** | 512 KB (HP SRAM) |
| **ROM** | 320 KB (bootloader + libs) |
| **Flash** | External SPI flash (up to 16 MB) |
| **LP Core** | Ultra-low-power RISC-V coprocessor (RV32I) |
| **USB** | USB Serial/JTAG controller |

**Reference:**
- [ESP32-C6 Datasheet](https://www.espressif.com/sites/default/files/documentation/esp32-c6_datasheet_en.pdf)
- [ESP32-C6 Technical Reference Manual](https://www.espressif.com/sites/default/files/documentation/esp32-c6_technical_reference_manual_en.pdf)

### 1.2 ESP32-C6 vs ESP32-C3 (Both RISC-V)

| Feature | ESP32-C3 | ESP32-C6 |
|---------|----------|----------|
| CPU | RV32IMC @ 160MHz | RV32IMAC @ 160MHz |
| WiFi | WiFi 4 (802.11n) | **WiFi 6 (802.11ax)** |
| Bluetooth | BLE 5.0 | BLE 5.0 |
| 802.15.4 | No | **Yes (Thread/Zigbee)** |
| SRAM | 400 KB | **512 KB** |
| LP Core | No | **Yes (ULP RISC-V)** |
| Atomic (A ext) | No | **Yes** |
| GPIO | 22 | 30 |

**Key difference for ThreadX:** ESP32-C6 has the **Atomic (A) extension**, enabling hardware atomic operations (`lr.w`/`sc.w`). This is useful for lock-free algorithms and more efficient mutex implementation.

---

## 2. Memory Map

### 2.1 ESP32-C6 Memory Layout

```
Address Range          Size     Description
-----------------------------------------------------------------
0x4000_0000-0x4004_FFFF  320KB   Internal ROM (bootloader, libs)
0x4080_0000-0x4087_FFFF  512KB   HP SRAM (instruction bus)
0x4080_0000-0x4087_FFFF  512KB   HP SRAM (data bus, aliased)
0x5000_0000-0x5000_3FFF   16KB   LP SRAM (for ULP/LP core)
0x4200_0000-0x42FF_FFFF   16MB   External Flash (via cache, mapped)
0x6000_0000-0x600F_FFFF          Peripheral registers
```

### 2.2 SRAM Regions

| Region | Size | Usage |
|--------|------|-------|
| HP SRAM | 512 KB | Main application RAM |
| LP SRAM | 16 KB | ULP coprocessor, RTC memory |
| ROM | 320 KB | Bootloader, crypto libs, WiFi/BT ROM functions |

### 2.3 Memory Layout for ThreadX

ThreadX needs these memory regions configured in the linker script:

```
+------------------------------------------+  0x4087_FFFF
|  Stack (grows down)                       |
|  - Main stack (MSP)                       |
|  - ThreadX thread stacks                  |
+------------------------------------------+
|  Heap                                     |
|  - ThreadX byte pools                     |
|  - ThreadX block pools                    |
+------------------------------------------+
|  .bss (uninitialized data)               |
+------------------------------------------+
|  .data (initialized data)                |
+------------------------------------------+
|  ThreadX kernel data                      |
|  - Thread control blocks                  |
|  - Semaphore/mutex/queue objects          |
+------------------------------------------+
|  .text (code) - runs from flash cache    |  0x4200_0000+
+------------------------------------------+
```

**Reference:** [ESP-IDF linker scripts](https://github.com/espressif/esp-idf/tree/master/components/esp_system/ld/esp32c6)

---

## 3. Interrupt System

### 3.1 ESP32-C6 Interrupt Architecture

ESP32-C6 uses a **custom interrupt controller** (not standard RISC-V PLIC):

```
+------------------------------------------+
|     Peripheral Interrupt Sources          |
|     (WiFi, BT, GPIO, UART, SPI, etc.)    |
+------------------------------------------+
           |
           v
+------------------------------------------+
|     Interrupt Matrix (INTMTX)            |  Maps peripheral IRQs
|     - Routes any source to any CPU line  |  to CPU interrupt lines
+------------------------------------------+
           |
           v
+------------------------------------------+
|     CPU Interrupt Controller             |  28 external interrupt
|     (INTC, ESP-specific)                 |  lines, configurable
|     - Priority levels (1-15)             |  priority and type
|     - Edge or Level triggered            |
+------------------------------------------+
           |
           v
+------------------------------------------+
|     RISC-V CPU                           |
|     - mstatus.MIE (global enable)        |
|     - mcause (interrupt number)          |
+------------------------------------------+
```

### 3.2 Interrupt Controller Details

| Feature | Details |
|---------|---------|
| External interrupt lines | 28 (from interrupt matrix) |
| Priority levels | 1-15 (0 = disabled) |
| Trigger types | Edge or Level (per line) |
| Nesting | Supported via priority threshold |
| Vectored mode | Supported |

### 3.3 Key Difference from Standard RISC-V PLIC

ESP32-C6 does **NOT** use the standard RISC-V PLIC (Platform-Level Interrupt Controller). Instead, it uses Espressif's custom **INTMTX + CPU INTC** combination.

**Impact on ThreadX:** The generic RISC-V ThreadX port assumes standard PLIC. We need to adapt:
- Interrupt enable/disable to use ESP32-C6's INTC registers
- Priority configuration to use ESP32-C6's priority scheme
- Use `esp_intr_alloc()` from ESP-IDF for interrupt management

### 3.4 Interrupt Priority vs ThreadX/FreeRTOS Priority

| Interrupt Priority (HW) | RTOS Thread Priority | Notes |
|-------------------------|---------------------|-------|
| 1 (lowest HW) | N/A | Below RTOS tick |
| 2-5 | RTOS-managed | Standard interrupt range |
| 6+ | Above RTOS | Cannot use RTOS APIs from ISR |
| NMI | Highest | Non-maskable, for watchdog/panic |

**FreeRTOS approach:** `configMAX_SYSCALL_INTERRUPT_PRIORITY` defines the threshold. Interrupts above this cannot call FreeRTOS APIs.

**ThreadX approach:** Similar concept -- ThreadX ISR functions should only be called from interrupts at or below a configured priority threshold.

**Reference:**
- [ESP32-C6 TRM: Interrupt Matrix](https://www.espressif.com/sites/default/files/documentation/esp32-c6_technical_reference_manual_en.pdf) (Chapter: Interrupt Matrix)
- [ESP-IDF Interrupt Allocation](https://docs.espressif.com/projects/esp-idf/en/latest/esp32c6/api-reference/system/intr_alloc.html)

---

## 4. Timer System

### 4.1 Available Timers on ESP32-C6

| Timer | Type | Usage |
|-------|------|-------|
| **SYSTIMER** | 52-bit counter, 16MHz | System tick, `esp_timer` |
| **TG0 Timer** | 54-bit general purpose | Application timers |
| **TG1 Timer** | 54-bit general purpose | Watchdog (typically) |
| **RTC Timer** | 32 KHz slow clock | Deep sleep wakeup |

### 4.2 SYSTIMER for ThreadX Tick

The SYSTIMER is the best candidate for the ThreadX system tick:

- 52-bit counter running at 16 MHz
- 3 comparators (alarm channels)
- Auto-reload capability
- Used by ESP-IDF for `esp_timer` and FreeRTOS tick

**Adaptation needed:** Replace the standard RISC-V `mtime`/`mtimecmp` timer handling in ThreadX port with ESP32-C6 SYSTIMER.

```c
// SYSTIMER configuration for ThreadX tick (pseudocode)
void tx_timer_setup(void)
{
    // Configure SYSTIMER alarm 0 for periodic tick
    systimer_hal_set_alarm_period(&hal, 0,
        SYSTIMER_FREQ / TX_TIMER_TICKS_PER_SECOND);
    systimer_hal_enable_alarm_int(&hal, 0);

    // Route SYSTIMER interrupt to CPU
    esp_intr_alloc(ETS_SYSTIMER_TARGET0_EDGE_INTR_SOURCE,
                   ESP_INTR_FLAG_IRAM,
                   tx_timer_interrupt_handler, NULL, NULL);
}
```

**Reference:**
- [ESP32-C6 TRM: System Timer](https://www.espressif.com/sites/default/files/documentation/esp32-c6_technical_reference_manual_en.pdf) (Chapter: System Timer)
- [ESP-IDF SYSTIMER HAL](https://github.com/espressif/esp-idf/tree/master/components/hal/esp32c6/include/hal)

---

## 5. RISC-V Extensions and Custom Features

### 5.1 ISA Extensions

| Extension | Present | Details |
|-----------|---------|---------|
| I (Integer) | Yes | Base integer instructions |
| M (Multiply) | Yes | Hardware multiply/divide |
| A (Atomic) | Yes | Atomic load-reserve/store-conditional (`lr.w`/`sc.w`) |
| C (Compressed) | Yes | 16-bit compressed instructions |
| F (Float) | **No** | No hardware FPU -- software float only |
| V (Vector) | **No** | No vector extension |
| Zicsr | Yes | CSR instructions |
| Zifencei | Yes | Instruction fence |

### 5.2 Espressif Custom CSRs

ESP32-C6 adds some **Espressif-specific CSRs** beyond the standard RISC-V set:

| CSR | Address | Purpose |
|-----|---------|---------|
| Custom PMP extensions | Various | Enhanced memory protection |
| Performance counters | Standard + custom | Cycle/instruction counting |

### 5.3 Physical Memory Protection (PMP)

ESP32-C6 supports RISC-V PMP (Physical Memory Protection):
- Up to 16 PMP regions
- Access control (R/W/X) per region
- Can be used for stack overflow protection
- Can isolate WiFi/BT blob memory from application

**ThreadX relevance:** ThreadX modules can use PMP for memory isolation (TX_THREAD_EXTENSION).

---

## 6. Clock Tree

### 6.1 Clock Sources

| Clock | Frequency | Usage |
|-------|-----------|-------|
| XTAL | 40 MHz | Main crystal oscillator |
| PLL | 160 MHz (from XTAL) | CPU clock |
| RC_FAST | 17.5 MHz (approx) | Fast RC oscillator |
| RC_SLOW | 136 kHz (approx) | RTC domain |
| XTAL32K | 32.768 kHz (optional) | Precise RTC timing |

### 6.2 CPU Clock Configuration

CPU typically runs at 160 MHz (PLL-derived). Can be scaled down for power saving:
- 160 MHz -- full performance
- 80 MHz -- reduced power
- 40 MHz -- low power (direct from XTAL)

**ThreadX impact:** Timer tick rate must account for possible CPU frequency changes (Dynamic Frequency Scaling, DFS).

**Reference:** [ESP-IDF Clock Tree](https://docs.espressif.com/projects/esp-idf/en/latest/esp32c6/api-reference/system/clk_tree_defs.html)

---

## 7. Low-Power Modes

### 7.1 Power Modes

| Mode | CPU State | SRAM | WiFi/BT | Wake Sources |
|------|-----------|------|---------|-------------|
| Active | Running | Retained | Active | N/A |
| Modem Sleep | Running | Retained | Powered down between operations | N/A |
| Light Sleep | Paused | Retained | Can maintain connection | Timer, GPIO, WiFi, BT, UART |
| Deep Sleep | Off | **Lost** (except LP SRAM) | Off | Timer, GPIO, LP core |

### 7.2 LP (Low-Power) Core

ESP32-C6 has an **ultra-low-power RISC-V coprocessor** (RV32I only):
- Runs at 20 MHz from RC_FAST clock
- Has access to 16 KB LP SRAM
- Can monitor GPIO, run simple programs during deep sleep
- Independent from main CPU

**ThreadX relevance:** LP core operates independently; ThreadX runs on the main HP core only. LP core programs are loaded separately.

### 7.3 ThreadX Light Sleep Considerations

For light sleep with ThreadX:
1. ThreadX idle thread should trigger `esp_light_sleep_start()`
2. SYSTIMER must be configured as wake source
3. On wake, ThreadX tick count must be compensated for sleep duration
4. WiFi power save callbacks need ThreadX task context

---

## 8. Boot Process

### 8.1 Full Boot Sequence

```
1. ROM Bootloader (in chip ROM)
   |-- Hardware initialization
   |-- Load 2nd stage bootloader from flash
   |-- Verify bootloader (if secure boot enabled)
   |
   v
2. 2nd Stage Bootloader (ESP-IDF, in flash)
   |-- Initialize flash cache
   |-- Read partition table
   |-- Select app partition (OTA support)
   |-- Verify app image (if secure boot enabled)
   |-- Decrypt app (if flash encryption enabled)
   |-- Load app to RAM
   |-- Jump to app entry point
   |
   v
3. Application Startup (ESP-IDF)
   |-- call_start_cpu0() [components/esp_system/port/cpu_start.c]
   |-- Initialize CPU (cache, MMU, clocks)
   |-- Initialize heap
   |-- start_cpu0_default() [components/esp_system/startup.c]
   |-- Initialize system components
   |-- Create main task
   |-- Start FreeRTOS scheduler <-- THIS IS WHERE WE DIVERGE
   |
   v
4. app_main() [User code]
   |-- For ThreadX: call tx_kernel_enter()
```

### 8.2 Where ThreadX Hooks In

**Option A (Recommended): Replace scheduler start**
- Keep ESP-IDF startup code through step 3
- Replace `vTaskStartScheduler()` with ThreadX initialization
- `app_main()` calls `tx_kernel_enter()`

**Option B: Replace entire startup**
- More control but more work
- Risk breaking secure boot, OTA, flash encryption

**Reference:**
- [ESP-IDF startup code](https://github.com/espressif/esp-idf/blob/master/components/esp_system/startup.c)
- [ESP-IDF CPU start (C6)](https://github.com/espressif/esp-idf/blob/master/components/esp_system/port/cpu_start.c)

---

## 9. Toolchain

### 9.1 ESP-IDF RISC-V Toolchain

| Component | Details |
|-----------|---------|
| Compiler | `riscv32-esp-elf-gcc` (GCC-based) |
| GCC Version | 13.2.0 (as of ESP-IDF v5.2+) |
| Binutils | `riscv32-esp-elf-binutils` |
| Newlib | `riscv32-esp-elf-newlib` |
| GDB | `riscv32-esp-elf-gdb` |

### 9.2 Espressif Toolchain Patches

Espressif applies **minimal patches** to the RISC-V GCC toolchain:
- Custom linker relaxation for ESP memory layout
- Newlib patches for ESP-IDF integration
- libgcc patches for atomic operations

These patches should NOT affect ThreadX compilation. The ThreadX RISC-V port uses standard GCC RISC-V assembly syntax.

### 9.3 Toolchain Compatibility with ThreadX

| Aspect | Compatible? | Notes |
|--------|------------|-------|
| Assembly syntax | Yes | GNU assembler, standard RISC-V |
| ABI | Yes | ILP32 (standard RV32) |
| ISA flags | Yes | `-march=rv32imac -mabi=ilp32` |
| Linker | Yes | GNU LD with custom scripts |
| C standard | Yes | C99/C11 supported |

### 9.4 Debugging

| Tool | Support |
|------|---------|
| **USB-JTAG** | Built-in on ESP32-C6 (USB Serial/JTAG peripheral) |
| **OpenOCD** | Supported via `openocd-esp32` |
| **GDB** | Full support via `riscv32-esp-elf-gdb` |
| **ESP-IDF Monitor** | UART-based console output |
| **Core Dump** | Supported (may need ThreadX adaptation) |
| **SystemView** | FreeRTOS-specific (need ThreadX TraceX instead) |

**Reference:**
- [ESP-IDF JTAG Debugging](https://docs.espressif.com/projects/esp-idf/en/latest/esp32c6/api-guides/jtag-debugging/index.html)
- [ESP toolchain releases](https://github.com/espressif/crosstool-NG/releases)

---

## 10. ESP-IDF Components: C6-Specific vs Shared

### 10.1 C6-Specific Components

| Component Path | Purpose |
|---------------|---------|
| `components/soc/esp32c6/` | C6 register definitions |
| `components/hal/esp32c6/` | C6-specific HAL implementations |
| `components/esp_hw_support/port/esp32c6/` | C6 HW support (clocks, sleep) |
| `components/esp_system/port/soc/esp32c6/` | C6 system port (startup, cache) |
| `components/esp_rom/esp32c6/` | C6 ROM function tables |
| `components/bootloader_support/src/esp32c6/` | C6 bootloader |
| `components/esp_wifi/lib/esp32c6/` | C6 WiFi binary blobs |
| `components/bt/controller/esp32c6/` | C6 BT controller blob |

### 10.2 SMP Remnants

ESP32-C6 is single-core, but some ESP-IDF code still has multi-core guards:

```c
// These patterns exist but are effectively no-ops on C6:
#if CONFIG_FREERTOS_UNICORE
    // Single-core path (this is what C6 uses)
#else
    // Multi-core path (not compiled for C6)
#endif
```

**Impact:** `CONFIG_FREERTOS_UNICORE=y` is always set for ESP32-C6. This simplifies:
- No `xTaskCreatePinnedToCore` -- just `xTaskCreate`
- No inter-processor interrupts (IPI)
- No cross-core spinlocks
- Simpler critical section implementation

---

## 11. Linker Script Structure

### 11.1 ESP-IDF Linker Scripts for C6

Located at `components/esp_system/ld/esp32c6/`:

```
sections.ld.in          -- Main linker script (generated from template)
memory.ld.in            -- Memory regions definition
```

Additional fragments from components:
```
components/esp_system/ld/esp32c6/
    memory.ld.in
    sections.ld.in

components/soc/esp32c6/ld/
    esp32c6.peripherals.ld    -- Peripheral register addresses
```

### 11.2 Key Memory Sections for ThreadX

The linker script must define:
- `.iram0.text` -- IRAM code (interrupt handlers, critical code)
- `.flash.text` -- Flash code (main application, cached)
- `.dram0.data` -- DRAM data (initialized)
- `.dram0.bss` -- DRAM BSS (uninitialized)
- `.noinit` -- No-init section (survives soft reset)

ThreadX-specific needs:
- Stack areas for ThreadX threads (in DRAM)
- Byte pool / block pool areas (in DRAM)
- ThreadX kernel data (preferably in DRAM for speed)
- ISR stack (in DRAM, accessible from interrupt context)

**Reference:** [ESP-IDF Linker Script Generation](https://docs.espressif.com/projects/esp-idf/en/latest/esp32c6/api-guides/linker-script-generation.html)

---

## 12. Summary: Key Gotchas for ThreadX Port

### 12.1 Must Address

| Issue | Impact | Solution |
|-------|--------|----------|
| **Non-standard interrupt controller** | ThreadX port assumes PLIC | Adapt to ESP32-C6 INTMTX + INTC |
| **SYSTIMER instead of mtime** | ThreadX uses mtime/mtimecmp | Rewrite timer interrupt handler |
| **ESP-IDF boot sequence** | Must preserve secure boot/OTA | Hook into `app_main()`, don't replace bootloader |
| **Newlib locks** | FreeRTOS mutexes for thread safety | Provide ThreadX-based lock implementation |
| **No FPU** | Software floating point | Ensure ThreadX port doesn't save FPU context |
| **Custom CSRs** | May conflict with ThreadX CSR usage | Verify no CSR conflicts |

### 12.2 Advantages

| Feature | Benefit |
|---------|---------|
| **Single core** | No SMP complexity |
| **Atomic extension** | Better lock-free primitives |
| **Standard RISC-V ABI** | ThreadX port compatible |
| **512 KB SRAM** | Plenty of room for ThreadX + WiFi + BT |
| **USB-JTAG** | Easy debugging without external hardware |
| **Built-in boot security** | Works unchanged with any RTOS |

---

## 13. References

### Espressif Documentation

1. [ESP32-C6 Datasheet](https://www.espressif.com/sites/default/files/documentation/esp32-c6_datasheet_en.pdf)
2. [ESP32-C6 Technical Reference Manual](https://www.espressif.com/sites/default/files/documentation/esp32-c6_technical_reference_manual_en.pdf)
3. [ESP-IDF Programming Guide (C6)](https://docs.espressif.com/projects/esp-idf/en/latest/esp32c6/index.html)
4. [ESP-IDF Interrupt Allocation](https://docs.espressif.com/projects/esp-idf/en/latest/esp32c6/api-reference/system/intr_alloc.html)
5. [ESP-IDF JTAG Debugging Guide](https://docs.espressif.com/projects/esp-idf/en/latest/esp32c6/api-guides/jtag-debugging/index.html)
6. [ESP-IDF Linker Script Generation](https://docs.espressif.com/projects/esp-idf/en/latest/esp32c6/api-guides/linker-script-generation.html)
7. [ESP-IDF Clock Tree](https://docs.espressif.com/projects/esp-idf/en/latest/esp32c6/api-reference/system/clk_tree_defs.html)
8. [ESP-IDF Sleep Modes](https://docs.espressif.com/projects/esp-idf/en/latest/esp32c6/api-reference/system/sleep_modes.html)

### ESP-IDF Source Code

9. [ESP-IDF startup.c](https://github.com/espressif/esp-idf/blob/master/components/esp_system/startup.c)
10. [ESP-IDF cpu_start.c](https://github.com/espressif/esp-idf/blob/master/components/esp_system/port/cpu_start.c)
11. [ESP-IDF linker scripts (C6)](https://github.com/espressif/esp-idf/tree/master/components/esp_system/ld/esp32c6)
12. [ESP-IDF SOC component (C6)](https://github.com/espressif/esp-idf/tree/master/components/soc/esp32c6)
13. [ESP-IDF HAL component (C6)](https://github.com/espressif/esp-idf/tree/master/components/hal/esp32c6)
14. [ESP-IDF esp_hw_support port (C6)](https://github.com/espressif/esp-idf/tree/master/components/esp_hw_support/port/esp32c6)

### RISC-V Architecture

15. [RISC-V Privileged Specification](https://riscv.org/specifications/privileged-isa/)
16. [RISC-V ISA Specification](https://riscv.org/specifications/)

### Toolchain

17. [Espressif crosstool-NG](https://github.com/espressif/crosstool-NG)
18. [ESP-IDF Tools](https://github.com/espressif/esp-idf/tree/master/tools)

### Related Chips

19. [ESP32-C3 Datasheet](https://www.espressif.com/sites/default/files/documentation/esp32-c3_datasheet_en.pdf) (for comparison)

---

*All URLs should be verified by the reader. Analysis based on ESP-IDF v5.x and ESP32-C6 rev 0.1+ silicon. Some details may change with newer ESP-IDF versions.*
