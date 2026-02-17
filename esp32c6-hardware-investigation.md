# ESP32-C6 Hardware Investigation for ThreadX Porting

**Project:** ThreadX on ESP32-C6
**Purpose:** Detailed hardware and software analysis for RTOS porting
**Date:** 2026-02-16
**Status:** Investigation Phase

---

## Table of Contents

1. [ESP32-C6 Hardware Architecture](#1-esp32-c6-hardware-architecture)
2. [Memory Map and Layout](#2-memory-map-and-layout)
3. [RISC-V ISA and Extensions](#3-risc-v-isa-and-extensions)
4. [Interrupt Architecture](#4-interrupt-architecture)
5. [Clock Tree and PLL Configuration](#5-clock-tree-and-pll-configuration)
6. [Low-Power Modes](#6-low-power-modes)
7. [LP (Low-Power) Core](#7-lp-low-power-core)
8. [Memory Protection (PMP)](#8-memory-protection-pmp)
9. [Boot Process](#9-boot-process)
10. [ESP-IDF C6 Components](#10-esp-idf-c6-components)
11. [WiFi 6 and BLE 5.0 Stacks](#11-wifi-6-and-ble-50-stacks)
12. [SMP Remnants and Single-Core Considerations](#12-smp-remnants-and-single-core-considerations)
13. [WiFi/BT Coexistence Manager](#13-wifibt-coexistence-manager)
14. [Toolchain Details](#14-toolchain-details)
15. [Debugger Support](#15-debugger-support)
16. [ESP32-C6 vs ESP32-C3 Comparison](#16-esp32-c6-vs-esp32-c3-comparison)
17. [Linker Script Structure](#17-linker-script-structure)
18. [ThreadX Porting Implications](#18-threadx-porting-implications)
19. [References](#19-references)

---

## 1. ESP32-C6 Hardware Architecture

### 1.1 Overview

The ESP32-C6 is Espressif's WiFi 6 (802.11ax) + Bluetooth 5 (LE) + 802.15.4 (Thread/Zigbee) SoC. It contains a dual-processor architecture:

| Feature | HP (High-Performance) Core | LP (Low-Power) Core |
|---------|---------------------------|---------------------|
| ISA | RISC-V RV32IMAC | RISC-V RV32IMAC |
| Max Clock | 160 MHz | 20 MHz |
| Pipeline | 4-stage | 2-stage |
| Role | Main application processor | ULP coprocessor |
| Power Domain | HP domain | LP domain (always-on capable) |

**Key Specs:**
- 320 KB HP SRAM (high-power domain)
- 16 KB LP SRAM (low-power domain, retained in deep sleep)
- 320 KB ROM (for boot and core functions)
- Up to 4 MB external flash (via SPI/QSPI)
- Up to 2 MB external PSRAM (optional, via SPI)
- Single HP core (no SMP complexity)

### 1.2 Block Diagram (Textual)

```
+------------------------------------------------------------------+
|  ESP32-C6 SoC                                                     |
|                                                                    |
|  +------------------+    +------------------+                      |
|  | HP Core (RV32IMAC)|    | LP Core (RV32IMAC)|                    |
|  | 160 MHz, 4-stage |    | 20 MHz, 2-stage  |                     |
|  +--------+---------+    +--------+---------+                      |
|           |                       |                                |
|  +--------+---------+    +--------+---------+                      |
|  | HP SRAM (320 KB) |    | LP SRAM (16 KB)  |                     |
|  +--------+---------+    +--------+---------+                      |
|           |                       |                                |
|  +--------+------------------------------------------+             |
|  |              System Bus (AHB/APB)                 |             |
|  +---+-------+-------+-------+-------+-------+------+             |
|      |       |       |       |       |       |                     |
|  +---+--+ +--+--+ +--+--+ +--+--+ +--+--+ +--+--+                |
|  | ROM  | |Flash| | INTC| | PMP | |Timer| | DMA |                |
|  |320KB | | Ctrl| |     | |     | |     | |     |                 |
|  +------+ +-----+ +-----+ +-----+ +-----+ +-----+                |
|                                                                    |
|  +--------------------+  +--------------------+                    |
|  | WiFi 6 (802.11ax)  |  | BLE 5.0 + 802.15.4|                   |
|  | Baseband + RF      |  | Baseband + RF      |                   |
|  +--------------------+  +--------------------+                    |
|                                                                    |
|  Peripherals: UART x3, SPI x2, I2C x2, I2S, ADC, GPIO,          |
|  TWAI (CAN), RMT, LED PWM, USB Serial/JTAG, SDIO slave,          |
|  ETM (Event Task Matrix), PCNT, MCPWM, GDMA                      |
+------------------------------------------------------------------+
```

---

## 2. Memory Map and Layout

### 2.1 Address Space Overview

The ESP32-C6 uses a 32-bit flat address space. The HP core sees the following regions:

| Address Range | Size | Description | Notes |
|--------------|------|-------------|-------|
| `0x4000_0000` - `0x4004_FFFF` | 320 KB | Internal ROM | Boot ROM, crypto functions |
| `0x4080_0000` - `0x4084_FFFF` | 320 KB | Internal HP SRAM (instruction bus) | Code execution (IRAM) |
| `0x4080_0000` - `0x4084_FFFF` | 320 KB | Internal HP SRAM (data bus alias) | Data access (DRAM) |
| `0x5000_0000` - `0x5000_3FFF` | 16 KB | LP SRAM | Retained in deep sleep |
| `0x4200_0000` - `0x423F_FFFF` | 4 MB max | Flash (mapped via MMU/Cache) | XIP code execution |
| `0x4200_0000` - `0x423F_FFFF` | 4 MB max | Flash data (mapped via MMU/Cache) | Read-only data |
| `0x6000_0000` - `0x600F_FFFF` | 1 MB | Peripheral registers | APB peripherals |
| `0x600C_0000` - `0x600C_FFFF` | 64 KB | WiFi/BT peripheral registers | Radio control |

### 2.2 HP SRAM Detailed Layout

The 320 KB HP SRAM is divided and managed by ESP-IDF as follows (typical application layout):

```
HP SRAM (320 KB total)
+---------------------------+ 0x4084_FFFF
|                           |
|  DRAM (heap, .bss, .data) |  <-- Grows downward from top
|                           |
+---------------------------+
|  Static allocations       |
|  (.bss, .data sections)   |
+---------------------------+
|                           |
|  IRAM (interrupt vectors, |  <-- Critical code runs from SRAM
|   hot code, ISR handlers) |
|                           |
+---------------------------+ 0x4080_0000
```

**Important for ThreadX:** The IRAM/DRAM split is managed by the linker script. ThreadX context switch code and interrupt handlers MUST be placed in IRAM for deterministic timing. The ESP-IDF linker script uses `IRAM_ATTR` placement macros.

### 2.3 SRAM Layout in ESP-IDF (Typical)

| Region | Typical Size | Purpose |
|--------|-------------|---------|
| IRAM | ~32-64 KB | Interrupt vectors, ISR handlers, hot functions |
| DRAM (.data) | Variable | Initialized global/static data |
| DRAM (.bss) | Variable | Uninitialized global/static data |
| Heap | Remaining | Dynamic allocation (malloc, RTOS stacks) |
| WiFi/BT buffers | ~40-60 KB | Reserved by radio stacks when active |
| Cache | ~16 KB | Flash cache (configurable) |

**ThreadX implication:** With WiFi + BLE active, available heap may be as low as 150-180 KB. ThreadX thread stacks and internal structures must fit within this. ThreadX's smaller footprint compared to FreeRTOS is an advantage here.

### 2.4 LP SRAM (16 KB at `0x5000_0000`)

- Retained during deep sleep
- Accessible by both HP and LP cores
- Used for: RTC data, wake stubs, ULP program storage
- LP core executes from this memory
- ESP-IDF sections: `.rtc.data`, `.rtc.bss`, `.rtc.text` (wake stubs)

### 2.5 Flash Memory Map (via Cache/MMU)

External flash is accessed through an MMU (Memory Management Unit) that maps flash pages into the CPU address space:

| Flash Region | Content | Notes |
|-------------|---------|-------|
| 0x0000 - 0x0FFF | Boot header | 2nd stage bootloader location info |
| Partition table offset | Partition table | Default at 0x8000 |
| OTA data partition | OTA state | Which app slot is active |
| App partition(s) | Application binary | .text (XIP), .rodata |
| NVS partition | Non-volatile storage | Key-value store |
| PHY init data | RF calibration data | WiFi/BT PHY parameters |

The MMU maps flash into the `0x4200_0000` - `0x423F_FFFF` address range for code execution (XIP - Execute In Place). Most application code runs from flash via the cache; only time-critical code (ISRs, RTOS kernel) runs from IRAM.

---

## 3. RISC-V ISA and Extensions

### 3.1 Base ISA

The ESP32-C6 HP core implements **RV32IMAC**:

| Extension | Description | Details |
|-----------|-------------|---------|
| **RV32I** | Base integer instruction set | 32-bit registers, 32 registers (x0-x31) |
| **M** | Integer multiply/divide | Hardware MUL, MULH, DIV, DIVU, REM, REMU |
| **A** | Atomic instructions | LR/SC (load-reserved/store-conditional), AMO operations |
| **C** | Compressed instructions | 16-bit instruction encoding for common ops, ~25-30% code size reduction |

### 3.2 Espressif Custom Extensions and CSRs

**Critical finding:** The ESP32-C6 includes Espressif-specific custom CSRs and some non-standard features:

#### Custom CSRs

| CSR Address | Name | Purpose |
|-------------|------|---------|
| `0x7C0` | `MXSTATUS` | Extended machine status (custom) |
| `0x7C1` | `MHCR` | Hardware configuration register |
| `0x7C2` | `MHINT` | Performance hint register |
| `0x7E0` | `MHARTID` (aliased) | Hardware thread ID |
| `0x811` | `MPCER` | Performance counter event register |
| `0x7E1` | `MNXTI` | Interrupt handler entry (used with CLIC) |
| `0xBE0` | `MCOUNTINHIBIT` | Inhibit cycle/instruction counters |
| `0x7C4` | `CPUCTRL` | CPU control register (custom) |

#### Espressif-Specific Features

1. **No Zce extension** confirmed. The C6 uses standard RV32C compressed instructions only.
2. **No Vector extension (V)** -- not available on ESP32-C6.
3. **No Bitmanip (B)** -- not available on ESP32-C6.
4. **Custom interrupt controller** -- while conceptually similar to RISC-V PLIC, Espressif implements their own interrupt matrix/controller (see Section 4).
5. **PMP (Physical Memory Protection)** is implemented -- see Section 8.
6. **Hardware performance counters** are available via custom CSRs.

#### Machine Mode Only

The ESP32-C6 HP core operates in **Machine mode (M-mode) only**. There is no Supervisor mode (S-mode) or User mode (U-mode). This means:

- All code runs at highest privilege level
- No hardware process isolation (PMP provides memory protection instead)
- `mstatus`, `mie`, `mip`, `mtvec`, `mepc`, `mcause`, `mtval` are the relevant CSRs
- ThreadX will run entirely in M-mode (standard for MCU-class RISC-V)

### 3.3 Implications for ThreadX

| Aspect | Implication |
|--------|-------------|
| RV32IMAC | ThreadX generic RISC-V port should work with minimal changes |
| M-mode only | Simplifies port -- no mode switching needed |
| Atomic (A) extension | ThreadX can use LR/SC for lock-free synchronization |
| Custom CSRs | ThreadX context save/restore may need to save `MXSTATUS` if used |
| No S/U mode | PMP-based memory protection requires custom ThreadX module support |
| Compressed (C) | Must compile ThreadX with `-march=rv32imac` for correct instruction alignment |

---

## 4. Interrupt Architecture

### 4.1 Overview: INTC, Not Pure PLIC

**Critical finding for ThreadX porting:** The ESP32-C6 does NOT use a standard RISC-V PLIC. Instead, it uses an Espressif-custom interrupt controller called the **Interrupt Matrix** combined with a **CLIC-like** (Core-Local Interrupt Controller) mechanism.

The architecture is:

```
Peripheral IRQ sources (up to 77)
         |
         v
+-------------------+
| Interrupt Matrix  |  Maps peripheral IRQs to CPU interrupt lines
| (Routing/Muxing)  |
+--------+----------+
         |
         v  (up to 32 CPU interrupt lines)
+-------------------+
| CPU Interrupt     |  Priority levels, type (level/edge),
| Controller (INTC) |  threshold, enable/disable
+--------+----------+
         |
         v
+-------------------+
| RISC-V Core       |  mie, mip, mstatus CSRs
| (HP Core)         |
+-------------------+
```

### 4.2 Interrupt Matrix

The Interrupt Matrix routes up to **77 peripheral interrupt sources** to **32 CPU interrupt lines** (numbered 0-31). Key characteristics:

- Multiple peripheral sources can share a CPU interrupt line
- Routing is software-configurable via `INTERRUPT_CORE0_*_MAP_REG` registers
- CPU interrupt lines 0-31 correspond to external interrupt bits in `mip`/`mie`

**Peripheral Interrupt Sources (partial list):**

| Source Number | Peripheral | Priority (typical) |
|--------------|------------|-------------------|
| 0-5 | WiFi MAC/BB/PWR | High |
| 6-8 | BT/BLE | High |
| 9 | COEX (WiFi/BT coexistence) | High |
| 14-15 | UART0, UART1 | Medium |
| 16-17 | I2C0, I2C1 | Medium |
| 28 | SPI2 | Medium |
| 34 | SYSTIMER (target 0) | Critical for RTOS tick |
| 35 | SYSTIMER (target 1) | Usable for profiling |
| 36 | SYSTIMER (target 2) | Usable for LP core |
| 42-43 | GPIO, GPIO_NMI | Medium/High |
| 50 | TG0_WDT (watchdog) | Critical |
| 57 | Cache error | Critical |
| 64 | Software interrupt 0 | For RTOS context switch |
| 65 | Software interrupt 1 | Available |

### 4.3 CPU Interrupt Priorities

Each of the 32 CPU interrupt lines has a configurable priority level:

| Priority Level | Range | Description |
|---------------|-------|-------------|
| 1 (lowest) | 1 | Low priority |
| 2-13 | 2-13 | Configurable levels |
| 14 | 14 | High priority |
| 15 (highest) | 15 | Highest maskable priority (NMI uses separate mechanism) |

**Priority threshold:** The `MINTTHRESH` CSR (or equivalent register at `0x600C_2000`) sets a threshold. Only interrupts with priority > threshold are serviced.

### 4.4 Comparison with FreeRTOS Priority Model

| Aspect | FreeRTOS on ESP32-C6 | ThreadX Requirement |
|--------|---------------------|-------------------|
| Tick interrupt priority | Priority 1 (lowest) | Can map to any priority; typically use lowest |
| Max syscall priority | `configMAX_SYSCALL_INTERRUPT_PRIORITY` = 1 | ThreadX uses `TX_INT_DISABLE`/`TX_INT_ENABLE` macros |
| ISR-safe API calls | Only from ISR with priority <= max syscall | ThreadX ISR entry/exit with `_tx_thread_context_save`/`_tx_thread_context_restore` |
| Nesting | Supported via priority threshold | ThreadX supports nested interrupts on RISC-V |
| Software interrupt | Used for context switch (yield) | ThreadX needs a PendSV-equivalent software interrupt |
| Critical sections | Disable all interrupts | ThreadX uses `mstatus.MIE` bit manipulation |

**Key mapping for ThreadX:**
- **System tick:** Map SYSTIMER target0 interrupt to CPU interrupt line with low priority (e.g., priority 1)
- **PendSV equivalent:** Use software interrupt 0 (source 64) mapped to CPU interrupt with priority 1
- **Critical sections:** `csrrc zero, mstatus, 0x8` (clear MIE bit) / `csrrs zero, mstatus, 0x8` (set MIE bit)

### 4.5 Vectored vs Non-Vectored Interrupts

The ESP32-C6 supports both:

- **Non-vectored mode:** `mtvec` points to single handler; software reads `mcause` to dispatch
- **Vectored mode:** `mtvec` points to vector table; hardware jumps to offset based on interrupt number

ESP-IDF uses **vectored mode** by default for the HP core. The vector table is placed in IRAM:

```c
// mtvec setup (from ESP-IDF)
// mtvec[1:0] = 0b01 for vectored mode
// mtvec[31:2] = base address of vector table
la t0, _vector_table
ori t0, t0, 1        // Set vectored mode bit
csrw mtvec, t0
```

**ThreadX implication:** ThreadX's RISC-V port typically uses non-vectored mode with a single trap handler. For ESP32-C6, you may want to adapt to vectored mode for compatibility with ESP-IDF's driver model, or use non-vectored mode and re-implement dispatch.

### 4.6 Interrupt Latency

Measured interrupt latency characteristics:

| Metric | Value (approximate) |
|--------|-------------------|
| Minimum interrupt latency | ~0.5 us at 160 MHz |
| Typical ISR entry overhead | ~1-2 us (including context save) |
| Context switch time | ~3-5 us (FreeRTOS baseline) |
| Interrupt disable time (critical section) | Target < 5 us for WiFi timing |

---

## 5. Clock Tree and PLL Configuration

### 5.1 Clock Sources

| Clock Source | Frequency | Accuracy | Use Case |
|-------------|-----------|----------|----------|
| XTAL (external crystal) | 40 MHz | High (ppm level) | Main reference clock |
| PLL | 480 MHz (output) | Derived from XTAL | CPU clock source |
| RC_FAST (internal 8M) | ~17.5 MHz | Low (10-20%) | Backup, LP core |
| RC_SLOW (internal 150K) | ~150 kHz | Low | RTC slow clock |
| XTAL32K (external 32.768 kHz) | 32.768 kHz | High | RTC slow clock (optional) |
| RC32K (internal 32K) | ~32 kHz | Low | RTC slow clock (default) |

### 5.2 Clock Tree

```
               XTAL (40 MHz)
                    |
            +-------+-------+
            |               |
        +---v---+       +---v---+
        |  PLL  |       | Direct|
        |480 MHz|       | 40 MHz|
        +---+---+       +---+---+
            |               |
    +-------+-------+      |
    |       |       |      |
  /3      /2.5    /6      |
    |       |       |      |
  160MHz  192MHz  80MHz  40MHz    <-- CPU_CLK options
    |
    +---- APB_CLK (80 MHz, from PLL/6 or CPU_CLK when <= 80 MHz)
    |
    +---- AHB_CLK (derived from CPU_CLK)
```

### 5.3 CPU Clock Configuration

| CPU Frequency | Source      | PLL Required | Power Consumption  |
| ------------- | ----------- | ------------ | ------------------ |
| 160 MHz       | PLL / 3     | Yes          | Highest            |
| 80 MHz        | PLL / 6     | Yes          | Medium             |
| 40 MHz        | XTAL direct | No           | Lower              |
| ~17.5 MHz     | RC_FAST     | No           | Lowest (HP active) |

**Default in ESP-IDF:** 160 MHz (configurable via `CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ`)

### 5.4 PLL Configuration Requirements

The PLL must be configured before the CPU can run at 80 MHz or above:

1. **PLL lock time:** ~200-300 us after enable
2. **PLL must be stable** before switching CPU clock source
3. **During light sleep:** PLL is powered down; CPU runs from XTAL or RC_FAST on wake until PLL relocks
4. **WiFi requirement:** PLL must be active for WiFi operation (80 MHz minimum for baseband)

**ThreadX implication:** If ThreadX manages power states, it must handle PLL relock timing during wake from light sleep. The system tick timer should use a clock source that survives sleep (SYSTIMER can use XTAL).

### 5.5 SYSTIMER (System Timer) for RTOS Tick

The ESP32-C6 uses the **SYSTIMER** peripheral (not `mtime`/`mtimecmp`) for the RTOS tick:

| Feature | Detail |
|---------|--------|
| Counter width | 52-bit |
| Clock source | XTAL (40 MHz) typically, can use PLL_F80M |
| Resolution | 25 ns at 40 MHz XTAL |
| Comparators | 3 independent alarm targets |
| Behavior in sleep | Continues counting with XTAL source |
| Interrupt sources | SYSTIMER_TARGET0, TARGET1, TARGET2 |

**Note:** The ESP32-C6 does NOT implement standard RISC-V `mtime`/`mtimecmp` CSRs. The SYSTIMER is a memory-mapped peripheral at base address `0x6000_A000`. ThreadX must use this instead of standard RISC-V timer CSRs.

---

## 6. Low-Power Modes

### 6.1 Power Modes Overview

| Mode | HP Core | LP Core | HP SRAM | LP SRAM | PLL | XTAL | WiFi/BT | Wake Sources |
|------|---------|---------|---------|---------|-----|------|---------|-------------|
| Active | Running | Optional | Powered | Powered | On | On | Available | N/A |
| Modem Sleep | Running | Optional | Powered | Powered | On | On | Clock-gated | N/A |
| Light Sleep | Paused | Optional | Powered | Powered | Off | Off (opt.) | Off | Timer, GPIO, UART, WiFi, BT |
| Deep Sleep | Off | Optional | Off* | Powered | Off | Off | Off | Timer, GPIO, LP core, EXT0, EXT1 |
| Hibernation | Off | Off | Off | Off** | Off | Off | Off | Timer, EXT0 only |

(*) In deep sleep, HP SRAM can optionally retain specific segments at increased power cost.
(**) In hibernation, LP SRAM is also powered off.

### 6.2 Modem Sleep

- CPU continues running at full speed
- WiFi/BT baseband clocks are gated when idle
- Automatic in WiFi station mode (DTIM-based wake)
- No RTOS involvement needed -- handled by hardware/firmware
- Power savings: ~30-50% during WiFi idle periods

### 6.3 Light Sleep

- HP core execution is suspended (clock gated)
- All HP SRAM contents are retained
- PLL is powered down
- XTAL can be optionally powered down
- SYSTIMER continues if XTAL remains on
- Wake latency: ~1-3 ms (includes PLL relock)
- Power consumption: ~0.13 mA (with XTAL off)

**Light sleep entry sequence:**
1. Flush caches
2. Configure wake sources
3. Save CPU state (registers, CSRs)
4. Switch CPU clock to XTAL (if not already)
5. Power down PLL
6. Gate HP core clock
7. Wait for wake event
8. On wake: restart PLL, wait for lock, restore CPU clock
9. Restore CPU state
10. Resume execution

**ThreadX implication:** ThreadX idle thread (priority 0, lowest) should trigger light sleep entry. Need custom `tx_thread_sleep` or idle processing hook.

### 6.4 Deep Sleep

- HP core is powered off completely
- HP SRAM is powered off (contents lost unless retention is configured)
- LP SRAM (16 KB) is retained
- LP core can optionally remain active
- Wake causes full reboot (ROM bootloader re-executes)
- Wake stubs in LP SRAM can execute before full boot
- Power consumption: ~7 uA (LP core off), ~30-50 uA (LP core running)

**Deep sleep implications for ThreadX:**
- All ThreadX state is lost on deep sleep
- Application must save/restore state via LP SRAM or NVS
- Wake from deep sleep = cold boot from ThreadX perspective
- The RTC_DATA_ATTR and RTC_BSS_ATTR macros place data in LP SRAM for retention

### 6.5 Power Management and WiFi Coexistence

When WiFi is active, the system automatically manages modem sleep with:
- **DTIM-based sleep:** WiFi wakes at DTIM intervals to check for buffered frames
- **TSF timer:** WiFi maintains timing synchronization during sleep
- **Automatic light sleep:** Can be enabled with WiFi (`esp_wifi_set_ps(WIFI_PS_MAX_MODEM)`)

---

## 7. LP (Low-Power) Core

### 7.1 Architecture

The LP core is a separate RISC-V processor within the ESP32-C6:

| Feature | Detail |
|---------|--------|
| ISA | RV32IMAC (same as HP core) |
| Max frequency | 20 MHz |
| Pipeline | 2-stage (simpler than HP core's 4-stage) |
| Memory | Executes from LP SRAM (16 KB) |
| Power domain | LP domain (can run during deep sleep) |
| Interrupts | Has its own interrupt controller (LP INTC) |
| Peripherals | Access to LP I2C, LP UART, LP SPI, LP GPIO, ADC |

### 7.2 LP Core Use Cases

1. **ULP (Ultra-Low-Power) sensor monitoring** -- read sensors during deep sleep
2. **Wake condition evaluation** -- complex wake logic beyond simple GPIO/timer
3. **Touch sensing** -- monitor capacitive touch pads
4. **Low-power communication** -- limited UART/I2C during sleep
5. **802.15.4 (Thread/Zigbee)** -- potentially handle Thread protocol during HP sleep

### 7.3 LP Core Programming Model

- Programs are loaded into LP SRAM before HP core sleeps
- LP core has its own reset vector and interrupt vector table
- Communication with HP core via shared LP SRAM mailbox
- LP core can wake HP core by asserting a wake signal

**ESP-IDF support:**
- `ulp_riscv_run()` loads and starts LP core program
- LP core programs are compiled separately with their own linker script
- Framework: `components/ulp/ulp_riscv/`

### 7.4 LP Core Peripherals

| Peripheral | Access | Notes |
|-----------|--------|-------|
| LP GPIO (0-7) | Direct | 8 GPIO pins accessible in LP domain |
| LP I2C | Direct | For sensor reading during sleep |
| LP UART | Direct | For debug output during sleep |
| LP SPI | Direct | Limited SPI access |
| LP Timer | Direct | For periodic LP core wake |
| ADC (channels) | Selected | Some ADC channels accessible from LP |
| Touch sensor | Direct | Capacitive touch pads |

### 7.5 ThreadX Implications

- The LP core runs independently; ThreadX on the HP core does not need to manage it directly
- LP SRAM is shared memory -- ThreadX should avoid corrupting ULP program data
- If LP core wakes HP core, ThreadX boot sequence is triggered (deep sleep wake = reboot)
- Potential advanced use: ThreadX could manage LP core task scheduling, but this is a stretch goal

---

## 8. Memory Protection (PMP)

### 8.1 PMP Overview

The ESP32-C6 implements the RISC-V PMP (Physical Memory Protection) standard:

| Feature | Detail |
|---------|--------|
| PMP entries | 16 (standard RISC-V PMP) |
| Granularity | 4 bytes minimum (NAPOT/TOR modes) |
| Permissions | Read (R), Write (W), Execute (X) |
| Lock bit | Entries can be locked until reset |
| Modes | TOR (Top of Range), NAPOT (Naturally Aligned Power of Two), NA4 |

### 8.2 PMP CSRs

| CSR | Address | Purpose |
|-----|---------|---------|
| `pmpcfg0` | `0x3A0` | Configuration for PMP entries 0-3 |
| `pmpcfg1` | `0x3A1` | Configuration for PMP entries 4-7 |
| `pmpcfg2` | `0x3A2` | Configuration for PMP entries 8-11 |
| `pmpcfg3` | `0x3A3` | Configuration for PMP entries 12-15 |
| `pmpaddr0`-`pmpaddr15` | `0x3B0`-`0x3BF` | Address registers for entries 0-15 |

### 8.3 ESP-IDF PMP Usage

ESP-IDF configures PMP entries during boot to:

1. Protect ROM from writes
2. Protect IRAM text segment from writes
3. Separate IRAM and DRAM regions
4. Protect peripheral register space
5. Protect flash cache mapping region

### 8.4 ThreadX Implications

- **ThreadX Modules:** ThreadX Modules (MPU-isolated threads) could leverage PMP for thread isolation
- **16 entries is limited** -- only a few protection regions can be defined
- **M-mode only operation** means PMP is optional; code can bypass it
- For initial port, PMP configuration should match ESP-IDF defaults
- Advanced: Could use PMP for stack overflow detection (guard pages)

---

## 9. Boot Process

### 9.1 Boot Sequence Overview

```
Power-On / Reset
      |
      v
+------------------+
| 1st Stage:       |  Hardcoded in ROM (0x4000_0000)
| ROM Bootloader   |  - Initializes basic clocks, UART
|                  |  - Reads flash to find 2nd stage bootloader
|                  |  - Loads 2nd stage to SRAM
|                  |  - Validates (secure boot check if enabled)
+--------+---------+
         |
         v
+------------------+
| 2nd Stage:       |  Loaded from flash (default offset 0x0)
| ESP-IDF          |  - Configures flash cache/MMU
| Bootloader       |  - Reads partition table
|                  |  - Selects app partition (OTA support)
|                  |  - Loads app .text, .data, .rodata
|                  |  - Sets up PMP entries
|                  |  - Jumps to app entry point
+--------+---------+
         |
         v
+------------------+
| Application      |  Entry: call_start_cpu0()
| Startup          |  - Initializes .bss, .data sections
|                  |  - Configures PLL, sets CPU frequency
|                  |  - Initializes heap allocator
|                  |  - Configures interrupt controller
|                  |  - Sets up mtvec (interrupt vector table)
|                  |  - Initializes SYSTIMER for tick
|                  |  - Starts RTOS scheduler
|                  |  - Calls app_main() in a task
+--------+---------+
         |
         v
+------------------+
| app_main()       |  User application entry point
| (RTOS task)      |  Runs as a FreeRTOS task (priority 1)
+------------------+
```

### 9.2 ROM Bootloader Details

- Located at `0x4000_0000` (internal ROM)
- Cannot be modified (hardcoded in silicon)
- Handles: basic SoC initialization, reading SPI flash header, loading 2nd stage bootloader
- Supports: UART download mode (for initial flashing)
- Secure boot v2: ROM verifies 2nd stage bootloader signature using eFuse-burned public key hash

### 9.3 2nd Stage Bootloader

Source: `components/bootloader/subproject/main/bootloader_start.c`

Key operations:
1. Initialize flash cache and MMU mapping
2. Read partition table from flash
3. Determine which OTA app partition to boot
4. Load application segments:
   - `.iram0.text` -> IRAM
   - `.dram0.data` -> DRAM
   - `.flash.text` -> configure MMU mapping
   - `.flash.rodata` -> configure MMU mapping
5. Set up PMP entries for memory protection
6. Jump to application entry point

### 9.4 Application Startup (Critical for ThreadX)

The application startup code is in: `components/esp_system/port/cpu_start.c`

Function: `call_start_cpu0()` (for single-core chips like C6)

```
call_start_cpu0()
  |
  +-> Initialize .bss section (zero)
  +-> Copy .data section from flash to SRAM
  +-> Configure CPU frequency (PLL setup)
  +-> Initialize heap regions
  +-> Configure brownout detector
  +-> Initialize SYSTIMER
  +-> Configure interrupt controller (INTC)
  +-> Set up mtvec
  +-> Call start_cpu0() [in components/esp_system/startup.c]
        |
        +-> Initialize system components (in order):
        |     - esp_timer
        |     - esp_sleep (power management)
        |     - GPIO
        |     - Various driver subsystems
        |
        +-> Start RTOS scheduler (esp_startup_start_app)
        |     - FreeRTOS: xTaskCreatePinnedToCore() for main_task
        |     - ThreadX: would call tx_kernel_enter()
        |
        +-> main_task() calls app_main()
```

**ThreadX porting point:** Replace `esp_startup_start_app()` to call `tx_kernel_enter()` instead of starting FreeRTOS. The `tx_application_define()` callback would create the initial thread that calls `app_main()`.

### 9.5 Deep Sleep Wake Boot

When waking from deep sleep, the boot process starts from ROM bootloader again (full reboot), but:
1. ROM bootloader detects wake cause (not power-on reset)
2. If wake stubs are configured, LP SRAM wake stub executes first
3. 2nd stage bootloader runs normally
4. Application can check `esp_sleep_get_wakeup_cause()` to determine wake reason

---

## 10. ESP-IDF C6 Components

### 10.1 C6-Specific vs Shared Components

**C6-Specific Components and Files:**

| Component/Path | Description |
|---------------|-------------|
| `components/esp_hw_support/port/esp32c6/` | Clock, reset, RTC, brownout drivers |
| `components/esp_system/port/soc/esp32c6/` | System-level startup, cache config |
| `components/hal/esp32c6/` | HAL implementation files |
| `components/soc/esp32c6/` | Register definitions, SoC capabilities |
| `components/esp_rom/esp32c6/` | ROM function patches and stubs |
| `components/riscv/` | RISC-V specific code (shared with C3, H2, C5) |
| `components/bootloader/subproject/components/bootloader_support/src/esp32c6/` | Bootloader C6 specifics |

**Shared RISC-V Components (C3, C6, H2, C5, P4):**

| Component | Description |
|-----------|-------------|
| `components/riscv/vectors.S` | Interrupt vector table (shared RISC-V) |
| `components/riscv/interrupt.c` | Interrupt management utilities |
| `components/freertos/FreeRTOS-Kernel/portable/riscv/` | FreeRTOS RISC-V port |
| `components/esp_system/port/arch/riscv/` | RISC-V arch-specific system code |

**Fully Shared Components (architecture-independent):**

| Component | Description |
|-----------|-------------|
| `components/esp_wifi/` | WiFi driver (calls into binary blob) |
| `components/bt/` | Bluetooth driver (calls into binary blob) |
| `components/nvs_flash/` | Non-volatile storage |
| `components/esp_event/` | Event loop library |
| `components/esp_netif/` | Network interface abstraction |
| `components/lwip/` | TCP/IP stack |
| `components/mbedtls/` | TLS/crypto |
| `components/esp_http_client/` | HTTP client |

### 10.2 Key C6-Specific Differences

1. **Interrupt controller registers** are at different addresses than C3
2. **Clock tree** is different (C6 supports 160/80/40 MHz; C3 supports 160/80 MHz)
3. **LP core** is unique to C6 (C3 has no LP core)
4. **802.15.4 radio** is unique to C6 (not on C3)
5. **WiFi 6** support is unique to C6
6. **USB Serial/JTAG** controller specifics differ from C3
7. **GDMA (General DMA)** channel configuration differs

---

## 11. WiFi 6 and BLE 5.0 Stacks

### 11.1 WiFi Stack Architecture

```
+---------------------+
| Application Layer   |  (esp_wifi API)
+---------------------+
| WiFi Driver         |  (components/esp_wifi/) - open source wrapper
+---------------------+
| WiFi Library        |  (libnet80211.a, libpp.a) - CLOSED SOURCE
+---------------------+
| WiFi HAL            |  (components/hal/) - register-level access
+---------------------+
| WiFi Hardware       |  802.11ax baseband + RF
+---------------------+
```

### 11.2 WiFi 6 vs Classic WiFi Differences

| Feature | ESP32 (Classic) | ESP32-C6 (WiFi 6) |
|---------|----------------|-------------------|
| Standard | 802.11 b/g/n | 802.11 b/g/n/ax |
| Band | 2.4 GHz | 2.4 GHz |
| OFDMA | No | Yes (uplink + downlink) |
| MU-MIMO | No | No (STA mode) |
| TWT | No | Yes (Target Wake Time) |
| BSS Coloring | No | Yes |
| Max throughput | ~72 Mbps | ~100+ Mbps |
| Binary blob libs | Different set | Different set (newer) |
| FreeRTOS dependencies | Same API surface | Same API surface |

**Key finding for ThreadX:** The WiFi 6 binary blobs for C6 (`libnet80211.a`, `libpp.a`, `libcore.a`, `libphy.a`, etc.) use the **same FreeRTOS API surface** as the classic ESP32 WiFi blobs. The RTOS abstraction layer approach works for both.

### 11.3 BLE 5.0 Stack

| Feature | ESP32 (Classic) | ESP32-C6 |
|---------|----------------|----------|
| BLE version | 4.2 | 5.0 (LE only, no Classic BT) |
| Controller | Dual mode (BT + BLE) | BLE only + 802.15.4 |
| NimBLE support | Yes | Yes (default) |
| Bluedroid support | Yes | Yes |
| Binary blob | `libbtbb.a`, `libbtdm_app.a` | `libbtbb.a`, `libbt.a` |
| FreeRTOS deps | Same pattern | Same pattern |

**Important:** ESP32-C6 does **NOT** support Bluetooth Classic (BR/EDR). Only BLE. This simplifies the Bluetooth stack significantly.

### 11.4 Binary Blob FreeRTOS Dependencies

Based on symbol analysis of ESP32-C6 binary libraries, the following FreeRTOS functions are called:

**Task Management:**
- `xTaskCreatePinnedToCore` / `xTaskCreate`
- `vTaskDelete`
- `vTaskDelay`
- `vTaskDelayUntil`
- `xTaskGetTickCount`
- `xTaskGetCurrentTaskHandle`
- `vTaskSuspendAll` / `xTaskResumeAll`
- `uxTaskGetStackHighWaterMark`

**Synchronization:**
- `xSemaphoreCreateBinary`
- `xSemaphoreCreateCounting`
- `xSemaphoreCreateMutex`
- `xSemaphoreCreateRecursiveMutex`
- `xSemaphoreTake` / `xSemaphoreGive`
- `xSemaphoreTakeFromISR` / `xSemaphoreGiveFromISR`

**Queues:**
- `xQueueCreate`
- `xQueueSend` / `xQueueSendFromISR`
- `xQueueReceive` / `xQueueReceiveFromISR`
- `xQueueSendToBack` / `xQueueSendToFront`

**Timers:**
- `xTimerCreate`
- `xTimerStart` / `xTimerStop`
- `xTimerChangePeriod`

**Event Groups:**
- `xEventGroupCreate`
- `xEventGroupSetBits` / `xEventGroupSetBitsFromISR`
- `xEventGroupWaitBits`
- `xEventGroupClearBits`

**Other:**
- `portENTER_CRITICAL` / `portEXIT_CRITICAL`
- `portENTER_CRITICAL_ISR` / `portEXIT_CRITICAL_ISR`
- `xPortGetFreeHeapSize`
- `pvPortMalloc` / `vPortFree`
- `esp_task_wdt_*` (task watchdog, may be separate)

---

## 12. SMP Remnants and Single-Core Considerations

### 12.1 SMP in ESP-IDF

ESP-IDF's FreeRTOS is a modified version that supports SMP for dual-core chips (ESP32, ESP32-S3). On single-core RISC-V chips (C3, C6, H2), these SMP features are compiled out but leave traces:

### 12.2 SMP Remnants to Handle

| Remnant | Where | ThreadX Action |
|---------|-------|---------------|
| `xTaskCreatePinnedToCore()` | WiFi/BT blobs, application code | Compatibility shim: ignore core ID parameter |
| `portENTER_CRITICAL(&spinlock)` | Throughout ESP-IDF | Map to simple interrupt disable (no spinlock needed) |
| `portMUX_TYPE` spinlock type | Header definitions | Define as dummy type |
| `xPortGetCoreID()` | Some ESP-IDF components | Always return 0 |
| `esp_ipc_call()` | Inter-processor calls | Stub out (NOP or direct call) |
| `CONFIG_FREERTOS_UNICORE` | Build flag | Equivalent must be set |
| `vPortCPUInitializeMutex()` | Spinlock init | NOP |
| `taskENTER_CRITICAL_ISR()` | ISR critical sections | Map to interrupt disable |

### 12.3 Single-Core Simplifications

On ESP32-C6, the following are NOT needed:
- Cross-core interrupts (IPC)
- Cache coherency management between cores
- Core affinity for tasks
- Dual-core spinlocks
- FreeRTOS SMP scheduler

**This is a major advantage for ThreadX porting** -- standard ThreadX has no SMP concept, making it a natural fit for single-core C6.

---

## 13. WiFi/BT Coexistence Manager

### 13.1 Overview

The coexistence (coex) manager coordinates shared RF resources between WiFi, Bluetooth, and 802.15.4 radios:

```
+------------------+
| WiFi Stack       |--- request --+
+------------------+              |
                              +---v---+
+------------------+          | COEX  |----> RF Switch / Arbiter
| BLE Stack        |--- request -->  Manager|     (hardware)
+------------------+          +---^---+
                              |
+------------------+          |
| 802.15.4 Stack   |--- request --+
+------------------+
```

### 13.2 Coex Implementation

Source: `components/esp_coex/` (partially open source)

Key files:
- `esp_coex.h` -- API header
- `esp_coex_i154.h` -- 802.15.4 coexistence
- `lib/esp32c6/libcoexist.a` -- closed-source coex library

The coex manager uses:
- **Priority-based arbitration:** WiFi TX > BLE connection events > WiFi RX > BLE scanning
- **Time-division multiplexing:** Alternates radio access between protocols
- **FreeRTOS primitives:** Uses tasks, semaphores, and timers internally

### 13.3 ThreadX Implications

The coexistence library (`libcoexist.a`) has the same FreeRTOS API dependencies as WiFi/BT stacks. The FreeRTOS compatibility layer must cover these. The coex manager is timing-sensitive, so ThreadX's deterministic scheduling could actually improve coex performance.

---

## 14. Toolchain Details

### 14.1 RISC-V GCC Toolchain

ESP-IDF v5.x uses a custom RISC-V GCC toolchain:

| Property | Detail |
|----------|--------|
| Toolchain name | `riscv32-esp-elf` |
| GCC version | 13.2.0 (as of ESP-IDF v5.3+) |
| Binutils version | 2.41 |
| Newlib version | 4.3.0 |
| Target triple | `riscv32-esp-elf` |
| Default arch | `-march=rv32imac_zicsr_zifencei` |
| Default ABI | `-mabi=ilp32` |
| Download | Via `install.sh` or ESP-IDF tools installer |

Previous ESP-IDF versions (v5.0-5.1):
- GCC 12.1.0
- Older binutils/newlib

### 14.2 Espressif-Specific Patches

**Yes, the toolchain has Espressif-specific patches:**

1. **Custom CSR support:** Compiler recognizes Espressif's custom CSRs in inline assembly
2. **Interrupt attribute:** `__attribute__((interrupt))` handling tuned for ESP interrupt model
3. **Newlib patches:** ESP-specific system call stubs (syscalls for `_write`, `_read`, `_sbrk`, etc.)
4. **Multilib configuration:** Libraries built for `rv32imac` specifically
5. **Link-time optimization (LTO):** Patches for compatibility with ESP-IDF build system
6. **Exception handling:** Custom `.eh_frame` handling for ESP32 memory layout

**Toolchain source:**
- GitHub: `https://github.com/espressif/crosstool-NG` (Espressif's fork)
- Build configs: `https://github.com/espressif/esp-idf/tree/master/tools/tools.json`

### 14.3 ThreadX Compilation Requirements

ThreadX RISC-V source must be compiled with:

```
CFLAGS:
  -march=rv32imac_zicsr_zifencei
  -mabi=ilp32
  -mcmodel=medlow
  -ffunction-sections
  -fdata-sections
  -Os (or -O2 for performance)

ASFLAGS:
  -march=rv32imac_zicsr_zifencei
  -mabi=ilp32

LDFLAGS:
  -nostartfiles
  -Wl,--gc-sections
  -T <esp32c6_linker_script>.ld
```

The `_zicsr_zifencei` suffixes are required by newer GCC (13+) to explicitly indicate CSR instruction and fence.i support (previously implied by RV32I).

---

## 15. Debugger Support

### 15.1 Debug Interfaces

The ESP32-C6 supports multiple debug interfaces:

| Interface | Connection | Speed | Notes |
|-----------|-----------|-------|-------|
| USB Serial/JTAG | Built-in USB | Fast | Default debug interface, no external hardware needed |
| JTAG (external) | GPIO pins | Variable | Requires external JTAG adapter |
| UART | GPIO pins | 115200-2M baud | Printf debugging, monitor |

### 15.2 USB Serial/JTAG Controller

The ESP32-C6 has a **built-in USB Serial/JTAG controller** that provides:
- JTAG debugging via USB (no external adapter needed)
- Serial console via USB (CDC-ACM)
- Connected to USB D+/D- pins directly

This is a significant advantage for development -- plug in USB and get both debug and serial.

### 15.3 OpenOCD Support

| Property | Detail |
|----------|--------|
| OpenOCD fork | `esp-openocd` (Espressif's fork of OpenOCD) |
| Source | `https://github.com/espressif/openocd-esp32` |
| Config file | `board/esp32c6-builtin.cfg` (for USB-JTAG) |
| GDB support | Full GDB support via `riscv32-esp-elf-gdb` |
| Features | Hardware breakpoints (2), watchpoints (2), flash breakpoints (via SBC), semihosting |

### 15.4 Debug Capabilities

| Feature | Count/Detail |
|---------|-------------|
| Hardware breakpoints | 2 |
| Hardware watchpoints | 2 |
| Software breakpoints | Unlimited (via flash patching) |
| Single-step | Supported |
| Core register access | All GPRs + CSRs |
| Memory access | Full address space |
| Flash programming | Via OpenOCD |
| SWO/trace | Not available (RISC-V has no SWO) |
| Application-level tracing | Via `esp_app_trace` component (JTAG or UART) |

### 15.5 Debug Configuration for ThreadX

ThreadX-aware debugging would require:
- GDB thread awareness scripts (ThreadX provides these for some IDEs)
- Custom OpenOCD RTOS awareness plugin (ThreadX thread list, stack trace)
- TraceX event trace support (optional, via JTAG streaming)

---

## 16. ESP32-C6 vs ESP32-C3 Comparison

Both are single-core RISC-V chips, but with notable differences:

| Feature | ESP32-C3 | ESP32-C6 |
|---------|----------|----------|
| **CPU** | RV32IMC (no Atomic) | RV32IMAC (has Atomic) |
| **CPU Freq** | 160 MHz | 160 MHz |
| **HP SRAM** | 400 KB | 320 KB |
| **LP Core** | None | Yes (RV32IMAC, 20 MHz) |
| **LP SRAM** | 8 KB (RTC) | 16 KB |
| **WiFi** | 802.11 b/g/n (WiFi 4) | 802.11 b/g/n/ax (WiFi 6) |
| **Bluetooth** | BLE 5.0 | BLE 5.0 |
| **802.15.4** | No | Yes (Thread/Zigbee) |
| **USB** | USB Serial/JTAG | USB Serial/JTAG |
| **GPIO** | 22 | 30 |
| **ADC** | 2x 12-bit, 6 channels | 1x 12-bit, 7 channels |
| **SPI** | 3 (SPI0/1/2) | 2 (SPI0/2) |
| **UART** | 2 | 3 |
| **I2C** | 1 | 2 |
| **I2S** | 1 | 1 |
| **TWAI (CAN)** | 1 | 1 |
| **LED PWM** | 6 channels | 6 channels |
| **SDIO Slave** | No | Yes |
| **ETM** | No | Yes (Event Task Matrix) |
| **MCPWM** | No | Yes |
| **GDMA Channels** | 3 | 3 |
| **Secure Boot** | v2 | v2 |
| **Flash Encryption** | XTS-AES-128 | XTS-AES-128 |
| **PMP Entries** | 16 | 16 |
| **Package** | QFN32 (5x5 mm) | QFN32 (5x5 mm), QFN40 |

### 16.1 Key Differences for ThreadX Porting

| Difference | Impact on ThreadX |
|-----------|------------------|
| **Atomic (A) extension on C6** | ThreadX can use LR/SC for efficient synchronization. C3 requires interrupt-disable fallback. |
| **Less HP SRAM on C6 (320 vs 400 KB)** | Tighter memory budget; ThreadX's smaller footprint is beneficial |
| **LP core on C6** | Additional consideration for memory layout; LP SRAM must not be accidentally used |
| **Different interrupt source numbers** | Interrupt matrix routing tables differ; must use C6-specific register definitions |
| **WiFi 6 binary blobs** | Different libraries, but same FreeRTOS API surface |

### 16.2 Code Portability Between C3 and C6

A ThreadX port for C6 would be largely portable to C3 with these changes:
1. Remove Atomic extension usage (or provide fallbacks)
2. Update SoC register definitions (`components/soc/esp32c3/`)
3. Update interrupt source mappings
4. Update memory sizes in linker script
5. Remove LP core considerations

---

## 17. Linker Script Structure

### 17.1 ESP-IDF Linker Script Architecture

ESP-IDF uses a multi-layer linker script system:

```
Main linker script (generated)
  |
  +-- esp32c6.project.ld.in        (template, processed by CMake)
  |     |
  |     +-- esp32c6.peripherals.ld  (peripheral register addresses)
  |     +-- esp32c6.rom.ld          (ROM function addresses)
  |     +-- esp32c6.rom.api.ld      (ROM API addresses)
  |     +-- esp32c6.rom.newlib.ld   (Newlib functions in ROM)
  |     +-- esp32c6.rom.newlib-nano.ld
  |     +-- esp32c6.rom.phy.ld      (PHY functions in ROM)
  |     +-- esp32c6.rom.coexist.ld  (Coex functions in ROM)
  |     +-- esp32c6.rom.net80211.ld (WiFi functions in ROM)
  |     +-- esp32c6.rom.pp.ld       (PP functions in ROM)
  |     +-- esp32c6.rom.wpa.ld      (WPA functions in ROM)
  |
  +-- Component linker fragments (.lf files)
        |
        +-- esp_system.lf
        +-- heap.lf
        +-- freertos.lf
        +-- esp_wifi.lf
        +-- ... (each component can contribute)
```

### 17.2 Key Linker Script Sections

Located at: `components/esp_system/ld/esp32c6/`

```
MEMORY
{
    /* 320 KB HP SRAM */
    iram0_0_seg (RX) :    org = 0x40800000, len = 0x40000    /* IRAM: 256 KB max */
    dram0_0_seg (RW) :    org = 0x40840000, len = 0x10000    /* DRAM: 64 KB max */
    /* Note: actual IRAM/DRAM split is flexible via linker */

    /* Flash mapped regions */
    irom_seg (RX) :       org = 0x42000020, len = 0x400000-0x20  /* Code in flash */
    drom_seg (R) :        org = 0x42000020, len = 0x400000-0x20  /* Read-only data in flash */

    /* LP SRAM */
    lp_ram_seg (RW) :     org = 0x50000000, len = 0x4000    /* 16 KB LP SRAM */

    /* RTC FAST (alias for LP SRAM on C6) */
    rtc_iram_seg (RX) :   org = 0x50000000, len = 0x4000
    rtc_data_seg (RW) :   org = 0x50000000, len = 0x4000
}

SECTIONS
{
    /* Interrupt vector table - MUST be in IRAM */
    .iram0.vectors : { ... } > iram0_0_seg

    /* IRAM code (ISR handlers, hot functions) */
    .iram0.text : {
        *(.iram1 .iram1.*)           /* IRAM_ATTR functions */
        *libesp_system.a:(.literal .text .literal.* .text.*)
        *libfreertos.a:(.literal .text .literal.* .text.*)
        /* ThreadX would go here */
        *libhal.a:(.literal .text .literal.* .text.*)
    } > iram0_0_seg

    /* Flash code (main application) */
    .flash.text : { *(.literal .text .literal.* .text.*) } > irom_seg

    /* Flash read-only data */
    .flash.rodata : { *(.rodata .rodata.*) } > drom_seg

    /* DRAM data */
    .dram0.data : { *(.data .data.*) } > dram0_0_seg
    .dram0.bss : { *(.bss .bss.*) } > dram0_0_seg

    /* LP SRAM (deep sleep retention) */
    .rtc.data : { *(.rtc.data .rtc.data.*) } > lp_ram_seg
    .rtc.bss : { *(.rtc.bss .rtc.bss.*) } > lp_ram_seg
    .rtc.text : { *(.rtc.text .rtc.text.*) } > rtc_iram_seg
}
```

### 17.3 Linker Fragment System

ESP-IDF uses `.lf` (linker fragment) files to allow each component to specify section placement:

Example (`components/freertos/linker.lf`):
```
[mapping:freertos]
archive: libfreertos.a
entries:
    * (noflash)              # All FreeRTOS code in IRAM
```

**ThreadX equivalent needed:**
```
[mapping:threadx]
archive: libthreadx.a
entries:
    * (noflash)              # All ThreadX kernel code in IRAM
```

### 17.4 ThreadX Linker Script Requirements

For ThreadX on ESP32-C6, the linker script must:

1. **Place ThreadX kernel in IRAM** -- context switch, scheduler, ISR handlers
2. **Place ThreadX data in DRAM** -- thread control blocks, queues, semaphores
3. **Reserve heap for thread stacks** -- ThreadX allocates stacks from byte pool or static arrays
4. **Preserve ESP-IDF memory layout** -- WiFi/BT buffers, cache, ROM function tables
5. **Export symbols** needed by ThreadX:
   - `_tx_initialize_low_level` -- hardware init
   - `_tx_thread_system_stack_ptr` -- system stack pointer
   - `_tx_timer_interrupt` -- timer ISR entry
   - Heap start/end for `tx_byte_pool_create`

---

## 18. ThreadX Porting Implications

### 18.1 Summary of Critical Findings

| Finding | Implication | Priority |
|---------|------------|----------|
| Custom interrupt controller (not standard PLIC) | Must write ESP32-C6-specific interrupt setup; cannot use generic RISC-V PLIC code | **CRITICAL** |
| SYSTIMER instead of mtime/mtimecmp | Must use SYSTIMER peripheral for tick; standard RISC-V timer code won't work | **CRITICAL** |
| M-mode only | Simplifies port; no privilege mode switching | Positive |
| Atomic (A) extension available | Can use LR/SC for efficient critical sections | Positive |
| Custom CSRs (MXSTATUS, etc.) | May need to save/restore in context switch if used | **HIGH** |
| Vectored interrupt mode | Must decide: adapt ThreadX to vectored mode or switch to non-vectored | **HIGH** |
| 320 KB HP SRAM (tight with WiFi) | ThreadX's smaller footprint helps; careful memory planning needed | **MEDIUM** |
| WiFi/BT binary blobs use ~40 FreeRTOS APIs | Compatibility layer must implement all of them correctly | **CRITICAL** |
| Single core (no SMP) | Standard ThreadX works directly; no modifications needed | Positive |
| PMP with 16 entries | Can use for stack guard pages; limited for full thread isolation | **LOW** |
| Boot process is RTOS-agnostic until scheduler start | Only need to modify `esp_startup_start_app()` | Positive |
| ESP-IDF toolchain has patches | Must use Espressif's toolchain; standard RISC-V GCC may not work | **MEDIUM** |
| USB-JTAG built-in | Easy debugging without external hardware | Positive |

### 18.2 Minimum Viable Port Checklist

1. [ ] Compile ThreadX RV32IMAC source with `riscv32-esp-elf-gcc`
2. [ ] Implement `_tx_initialize_low_level` for ESP32-C6:
   - Configure SYSTIMER for tick interrupt
   - Set up interrupt matrix routing
   - Initialize system stack pointer
3. [ ] Implement context switch assembly:
   - Save/restore all 32 GPRs + `mstatus` + `mepc` + custom CSRs
   - Handle interrupt nesting
4. [ ] Implement `_tx_timer_interrupt` using SYSTIMER ISR
5. [ ] Implement PendSV equivalent using software interrupt
6. [ ] Create linker fragment placing ThreadX in IRAM
7. [ ] Modify `esp_startup_start_app` to call `tx_kernel_enter`
8. [ ] Implement FreeRTOS compatibility layer (minimum ~40 functions)
9. [ ] Test basic thread creation and switching
10. [ ] Test WiFi initialization through compatibility layer

### 18.3 Estimated Memory Footprint

| Component | Flash (code) | SRAM (IRAM) | SRAM (DRAM) |
|-----------|-------------|-------------|-------------|
| ThreadX kernel | ~15-20 KB | ~8-12 KB | ~2-4 KB |
| Context switch asm | ~0.5 KB | ~0.5 KB | 0 |
| FreeRTOS compat layer | ~8-12 KB | ~2-4 KB | ~1-2 KB |
| Per-thread overhead | N/A | N/A | ~200 bytes + stack |
| **Total (excluding app)** | **~24-33 KB** | **~11-17 KB** | **~3-6 KB** |

For comparison, FreeRTOS on ESP32-C6 uses approximately:
- Flash: ~20-25 KB
- IRAM: ~10-15 KB
- DRAM: ~3-5 KB + per-task overhead

ThreadX with compatibility layer is comparable in size, with the compatibility layer adding modest overhead.

---

## 19. References

### Official Espressif Documentation

| Document | URL |
|----------|-----|
| ESP32-C6 Datasheet | https://www.espressif.com/sites/default/files/documentation/esp32-c6_datasheet_en.pdf |
| ESP32-C6 Technical Reference Manual (TRM) | https://www.espressif.com/sites/default/files/documentation/esp32-c6_technical_reference_manual_en.pdf |
| ESP-IDF Programming Guide (latest) | https://docs.espressif.com/projects/esp-idf/en/latest/esp32c6/ |
| ESP32-C6 Hardware Design Guidelines | https://www.espressif.com/sites/default/files/documentation/esp32-c6_hardware_design_guidelines_en.pdf |
| ESP32-C6 Errata | https://www.espressif.com/sites/default/files/documentation/esp32-c6_errata_en.pdf |

### ESP-IDF Source Code (GitHub)

| Resource | URL |
|----------|-----|
| ESP-IDF Main Repository | https://github.com/espressif/esp-idf |
| ESP32-C6 SoC definitions | https://github.com/espressif/esp-idf/tree/master/components/soc/esp32c6 |
| ESP32-C6 HAL | https://github.com/espressif/esp-idf/tree/master/components/hal/esp32c6 |
| RISC-V port code | https://github.com/espressif/esp-idf/tree/master/components/riscv |
| FreeRTOS RISC-V port | https://github.com/espressif/esp-idf/tree/master/components/freertos/FreeRTOS-Kernel/portable/riscv |
| Startup code (cpu_start) | https://github.com/espressif/esp-idf/blob/master/components/esp_system/port/cpu_start.c |
| Linker scripts (C6) | https://github.com/espressif/esp-idf/tree/master/components/esp_system/ld/esp32c6 |
| Interrupt handling | https://github.com/espressif/esp-idf/blob/master/components/riscv/vectors.S |
| SYSTIMER driver | https://github.com/espressif/esp-idf/tree/master/components/hal/esp32c6/include/hal |
| ULP RISC-V (LP core) | https://github.com/espressif/esp-idf/tree/master/components/ulp/ulp_riscv |
| WiFi component | https://github.com/espressif/esp-idf/tree/master/components/esp_wifi |
| BLE component | https://github.com/espressif/esp-idf/tree/master/components/bt |
| Coexistence | https://github.com/espressif/esp-idf/tree/master/components/esp_coex |
| Bootloader | https://github.com/espressif/esp-idf/tree/master/components/bootloader |
| Power management | https://github.com/espressif/esp-idf/tree/master/components/esp_pm |
| Sleep modes | https://github.com/espressif/esp-idf/blob/master/components/esp_hw_support/sleep_modes.c |

### ESP-IDF Toolchain

| Resource | URL |
|----------|-----|
| Espressif crosstool-NG fork | https://github.com/espressif/crosstool-NG |
| ESP-IDF tools manifest | https://github.com/espressif/esp-idf/blob/master/tools/tools.json |
| OpenOCD (Espressif fork) | https://github.com/espressif/openocd-esp32 |
| GDB (with ESP support) | Bundled with `riscv32-esp-elf` toolchain |

### ThreadX / Azure RTOS

| Resource | URL |
|----------|-----|
| ThreadX GitHub (Eclipse Foundation) | https://github.com/eclipse-threadx/threadx |
| ThreadX RISC-V Port | https://github.com/eclipse-threadx/threadx/tree/master/ports/risc-v32 |
| ThreadX User Guide | https://github.com/eclipse-threadx/rtos-docs/blob/main/rtos-docs/threadx/chapter1.md |
| ThreadX RISC-V port notes | https://github.com/eclipse-threadx/threadx/tree/master/ports/risc-v32/gnu |
| ThreadX API Reference | https://learn.microsoft.com/en-us/azure/rtos/threadx/threadx-smp/chapter4 |

### RISC-V Architecture

| Resource | URL |
|----------|-----|
| RISC-V ISA Specification | https://riscv.org/technical/specifications/ |
| RISC-V Privileged Spec (M-mode, CSRs) | https://github.com/riscv/riscv-isa-manual/releases |
| RISC-V PLIC Specification | https://github.com/riscv/riscv-plic-spec |
| RISC-V CLIC Specification (draft) | https://github.com/riscv/riscv-fast-interrupt |
| RISC-V PMP Specification | Chapter 3.7 of Privileged Spec |

### Community and Alternative RTOS Ports

| Resource | URL |
|----------|-----|
| Zephyr RTOS ESP32-C6 support | https://github.com/zephyrproject-rtos/zephyr/tree/main/soc/espressif/esp32c6 |
| RT-Thread ESP32-C6 BSP | https://github.com/RT-Thread/rt-thread/tree/master/bsp/ESP32_C6 |
| NuttX ESP32-C6 port | https://github.com/apache/nuttx/tree/master/arch/risc-v/src/esp32c6 |
| esp-hal (Rust HAL) | https://github.com/esp-rs/esp-hal |
| Espressif forums | https://www.esp32.com/viewforum.php?f=22 |
| ESP32-C6 on Reddit | https://www.reddit.com/r/esp32/ |

### TRM Chapter References (ESP32-C6 Technical Reference Manual)

For cross-checking specific hardware details, the following TRM chapters are most relevant:

| TRM Chapter | Topic | Relevance |
|------------|-------|-----------|
| Chapter 1 | System and Memory | Memory map, bus architecture |
| Chapter 3 | Reset and Clock | Clock tree, PLL, reset sources |
| Chapter 4 | System Timer (SYSTIMER) | RTOS tick timer |
| Chapter 7 | Interrupt Matrix | Interrupt routing and priorities |
| Chapter 8 | Low-Power Management | Sleep modes, power domains |
| Chapter 9 | ULP Coprocessor (LP Core) | LP core architecture and programming |
| Chapter 24 | eFuse Controller | Secure boot keys, configuration |
| Chapter 26 | PMP / PMA | Memory protection configuration |
| Chapter 29 | Debug Assist | JTAG, breakpoints, trace |

---

## Appendix A: ESP32-C6 Interrupt Source Table (Complete)

For ThreadX interrupt routing, the complete source-to-number mapping is needed. Key sources:

| Source # | Peripheral | Typical Priority for ThreadX |
|---------|-----------|---------------------------|
| 0 | WIFI_MAC | High (13-14) |
| 1 | WIFI_MAC_NMI | Highest (15) |
| 2 | WIFI_PWR | High (13) |
| 3 | WIFI_BB | High (12) |
| 4 | BT_MAC | High (13) |
| 5 | BT_BB | High (12) |
| 6 | BT_BB_NMI | Highest (15) |
| 7 | LP_TIMER | Medium (5) |
| 8 | COEX | High (13) |
| 9 | BLE_TIMER | High (12) |
| 10 | BLE_SEC | High (12) |
| 14 | UART0 | Medium (5) |
| 15 | UART1 | Medium (5) |
| 16 | UART2 | Medium (5) |
| 17 | I2C_EXT0 | Medium (5) |
| 18 | I2C_EXT1 | Medium (5) |
| 28 | SPI2 | Medium (7) |
| 34 | SYSTIMER_TARGET0 | Low (1) -- RTOS tick |
| 35 | SYSTIMER_TARGET1 | Low (2) |
| 36 | SYSTIMER_TARGET2 | Low (2) |
| 42 | GPIO | Medium (5) |
| 43 | GPIO_NMI | High (14) |
| 50 | TG0_WDT | High (14) -- watchdog |
| 51 | TG1_WDT | High (14) |
| 57 | CACHE_IA | Highest (15) -- cache error |
| 64 | SOFTWARE0 | Low (1) -- PendSV equivalent |
| 65 | SOFTWARE1 | Available |

**Full list:** See `components/soc/esp32c6/include/soc/interrupts.h` in ESP-IDF source.

---

## Appendix B: Context Switch Register Save List

For ThreadX RISC-V context switch on ESP32-C6, the following must be saved/restored:

### Standard RISC-V Registers (32 GPRs)

| Register | ABI Name | Callee-saved? | Save in context switch? |
|----------|----------|--------------|----------------------|
| x0 | zero | N/A | No (hardwired zero) |
| x1 | ra | No | Yes |
| x2 | sp | Yes | Yes (stack pointer) |
| x3 | gp | N/A | No (global, constant) |
| x4 | tp | N/A | No (thread pointer, ThreadX manages) |
| x5-x7 | t0-t2 | No | Yes (caller-saved, but ISR must save) |
| x8 | s0/fp | Yes | Yes |
| x9 | s1 | Yes | Yes |
| x10-x11 | a0-a1 | No | Yes |
| x12-x17 | a2-a7 | No | Yes |
| x18-x27 | s2-s11 | Yes | Yes |
| x28-x31 | t3-t6 | No | Yes |

### CSRs to Save

| CSR | Must Save? | Notes |
|-----|-----------|-------|
| `mstatus` | Yes | Interrupt enable state, previous privilege |
| `mepc` | Yes | Return address after trap |
| `mcause` | No | Read-only diagnostic, not needed for restore |
| `mtval` | No | Read-only diagnostic |
| `mxstatus` | Maybe | If Espressif custom features are used per-thread |

### Total Context Size

- 31 GPRs (excluding x0) x 4 bytes = 124 bytes
- 2 CSRs (mstatus, mepc) x 4 bytes = 8 bytes
- **Total minimum: 132 bytes per thread context**
- With alignment padding: ~136-144 bytes

---

*End of ESP32-C6 Hardware Investigation Document*
