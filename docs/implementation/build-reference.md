# Build, Flash, and Debug Reference

## Prerequisites

- ESP-IDF installed and sourced (`$IDF_PATH` set)
- ESP32-C6 development board connected via USB
- ThreadX submodule initialized

## Build Commands (in order)

```bash
# 1. Source the ESP-IDF environment
. $IDF_PATH/export.sh

# 2. Set target (only needed once, stored in sdkconfig)
idf.py set-target esp32c6

# 3. Configure (optional — defaults from sdkconfig.defaults are used)
idf.py menuconfig

# 4. Build
idf.py build

# 5. Flash (replace /dev/ttyUSB0 with your port)
idf.py -p /dev/ttyUSB0 flash

# 6. Monitor serial output
idf.py -p /dev/ttyUSB0 monitor

# Combined flash + monitor
idf.py -p /dev/ttyUSB0 flash monitor
```

## sdkconfig.defaults

Applied automatically on first build:

```ini
CONFIG_IDF_TARGET="esp32c6"
CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_160=y

# Disable watchdogs — required during initial bring-up
# (ThreadX doesn't feed the ESP-IDF watchdog)
CONFIG_ESP_TASK_WDT_EN=n
CONFIG_ESP_INT_WDT=n

CONFIG_ESP_CONSOLE_UART_DEFAULT=y
CONFIG_ESP_CONSOLE_UART_BAUDRATE=115200
CONFIG_LOG_DEFAULT_LEVEL_INFO=y
CONFIG_FREERTOS_HZ=100
```

## Expected Console Output (when working)

```
I (XXX) threadx_startup: === ThreadX taking over from ESP-IDF startup ===
I (XXX) threadx_startup: Entering ThreadX kernel...
I (XXX) threadx_startup: tx_application_define: setting up system resources
I (XXX) threadx_startup: ThreadX application defined — main thread created
I (XXX) threadx_startup: Main thread started, calling app_main()
I (XXX) main: ============================================
I (XXX) main:   ThreadX on ESP32-C6 — Demo Application
I (XXX) main: ============================================
I (XXX) main: ThreadX version: 6
I (XXX) main: Tick rate: 100 Hz
I (XXX) main: [main]  tick=0   count=0
I (XXX) main: [blink] tick=0   count=0
I (2XXX) main: [blink] tick=100 count=1   ← tick increments by 100 per second
I (4XXX) main: [main]  tick=200 count=1   ← main wakes every 200 ticks
```

If `tick` stays at 0 and output floods at full speed, `tx_thread_sleep()` is
not working — the SYSTIMER interrupt is not firing.

## Submodule Setup

If the `threadx/` submodule is empty:

```bash
cd /home/kty/work/threadx-esp32c6-project
git submodule update --init components/threadx/threadx
```

The submodule points to `eclipse-threadx/threadx` on GitHub.

## Checking Register State (with JTAG)

If you have JTAG attached (OpenOCD):

```bash
# Start OpenOCD for ESP32-C6
openocd -f board/esp32c6-builtin.cfg

# In another terminal, connect GDB
riscv32-esp-elf-gdb build/threadx_esp32c6.elf
(gdb) target remote :3333
(gdb) monitor reset halt

# Read CSRs
(gdb) info reg mstatus
(gdb) info reg mie
(gdb) info reg mcause
(gdb) info reg mepc

# Read PLIC MX registers
(gdb) x/1wx 0x20001000   # PLIC_MXINT_ENABLE_REG
(gdb) x/1wx 0x20001004   # PLIC_MXINT_TYPE_REG
(gdb) x/1wx 0x2000100C   # PLIC_EMIP_STATUS_REG

# Read SYSTIMER
(gdb) x/1wx 0x60050000   # SYSTIMER_CONF_REG (DR_REG_SYSTIMER_BASE + 0)
(gdb) x/1wx 0x60050034   # SYSTIMER_TARGET0_CONF_REG
(gdb) x/1wx 0x60050064   # SYSTIMER_INT_ENA_REG
(gdb) x/1wx 0x60050070   # SYSTIMER_INT_ST_REG (masked status)

# Read INTMTX map for source 57
(gdb) x/1wx 0x600100E4   # INTMTX map reg for SYSTIMER_TARGET0
```

## Key Peripheral Addresses for Debugging

| Peripheral           | Register                  | Address      |
|---------------------|---------------------------|--------------|
| PLIC MX             | Enable                    | 0x20001000   |
| PLIC MX             | Type (edge/level)         | 0x20001004   |
| PLIC MX             | Clear (edge latch)        | 0x20001008   |
| PLIC MX             | EIP Status (pending)      | 0x2000100C   |
| PLIC MX             | Priority line 7           | 0x2000102C   |
| PLIC MX             | Threshold                 | 0x20001090   |
| INTMTX              | Map for SYSTIMER_TARGET0  | 0x600100E4   |
| SYSTIMER            | CONF_REG                  | 0x60050000   |
| SYSTIMER            | TARGET0_CONF_REG          | 0x60050034   |
| SYSTIMER            | COMP0_LOAD_REG            | 0x60050050   |
| SYSTIMER            | INT_ENA_REG               | 0x60050064   |
| SYSTIMER            | INT_CLR_REG               | 0x6005006C   |
| SYSTIMER            | INT_ST_REG (read status)  | 0x60050070   |

Note: DR_REG_SYSTIMER_BASE must be verified against your ESP-IDF version's
`soc/esp32c6/register/soc/systimer_reg.h`.
