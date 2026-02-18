# Debugging ThreadX on ESP32-C6 with GDB and OpenOCD

This guide walks through everything you need to debug the ThreadX port using
JTAG. It assumes very little prior knowledge of RISC-V assembly or ESP32
internals and explains concepts from first principles before applying them.

---

## Table of Contents

1. [Why JTAG Debugging?](#1-why-jtag-debugging)
2. [What You Need](#2-what-you-need)
3. [ESP32-C6 Built-in USB JTAG](#3-esp32-c6-built-in-usb-jtag)
4. [Starting OpenOCD](#4-starting-openocd)
5. [Connecting GDB](#5-connecting-gdb)
6. [RISC-V Primer for Beginners](#6-risc-v-primer-for-beginners)
7. [Essential GDB Commands](#7-essential-gdb-commands)
8. [Reading RISC-V Control Registers (CSRs)](#8-reading-risc-v-control-registers-csrs)
9. [Inspecting ThreadX State](#9-inspecting-threadx-state)
10. [Inspecting Hardware Registers](#10-inspecting-hardware-registers)
11. [Debugging Recipes](#11-debugging-recipes)
12. [GDB Init File](#12-gdb-init-file)
13. [Tips and Common Pitfalls](#13-tips-and-common-pitfalls)

---

## 1. Why JTAG Debugging?

UART logging (`ESP_LOGI`) works well when code is running normally, but it fails
exactly when you need it most:

- **Interrupt handlers**: logging from an ISR can deadlock or corrupt state
- **Hangs**: if the CPU is spinning in a tight loop, no logging code runs
- **Crashes before logging initializes**: the first printable message never appears
- **Stack overflows**: the crash destroys the very stack that logging needs

JTAG debugging lets you **halt the CPU and inspect everything** — registers,
memory, the stack, peripheral registers — without the CPU needing to cooperate.
It is the difference between guessing and knowing.

The three bugs in our project that were hardest to diagnose (wrong mtvec mode,
EIP_STATUS always-zero, silent exception spin) could each have been confirmed
in seconds with a JTAG debugger.

---

## 2. What You Need

### Software (all included in ESP-IDF)

After running `. $IDF_PATH/export.sh`, these are on your PATH:

| Tool | Purpose |
|------|---------|
| `openocd` | Talks to the JTAG hardware, exposes a GDB server on TCP port 3333 |
| `riscv32-esp-elf-gdb` | GDB client that connects to OpenOCD |

Verify they are available:
```bash
which openocd
which riscv32-esp-elf-gdb
openocd --version
```

### Hardware

**ESP32-C6 development boards have a built-in USB JTAG adapter** — no external
hardware required. The same USB-C cable you use to flash the board also carries
JTAG signals. This works on boards with a USB-C connector directly to the
ESP32-C6 chip (not a separate USB-to-UART bridge chip).

If you have an older board with only a CP2102/CH340 USB bridge, you will need
an external JTAG adapter (e.g. ESP-Prog). That case is covered in
[Section 3](#3-esp32-c6-built-in-usb-jtag).

---

## 3. ESP32-C6 Built-in USB JTAG

The ESP32-C6 has a USB Serial/JTAG peripheral built into the chip itself (at
GPIO12/GPIO13). When you plug the board into your computer with a USB-C cable,
the operating system sees two USB devices:

- A **CDC serial port** (for `idf.py flash monitor` and `ESP_LOG` output)
- A **JTAG interface** (for OpenOCD)

Both appear on the same cable. You do not need a separate connection for
debugging.

### Identifying your device on Linux

```bash
ls /dev/ttyACM* /dev/ttyUSB*
# Built-in JTAG boards typically show /dev/ttyACM0 (CDC) and appear as
# "ESP JTAG" in lsusb output (vendor 303a, product 1001)
lsusb | grep -i esp
```

Expected output:
```
Bus 001 Device 005: ID 303a:1001 Espressif USB JTAG/serial debug unit
```

If you see `303a:1001`, you have the built-in JTAG and no external hardware is
needed.

### External JTAG (ESP-Prog or FT2232H)

If your board does not have built-in JTAG, you need an external adapter wired
to the ESP32-C6 JTAG pins:

| ESP32-C6 Pin | JTAG Signal |
|---|---|
| GPIO4 | TCK |
| GPIO5 | TDI |
| GPIO6 | TDO |
| GPIO7 | TMS |
| GND | GND |

Use `board/esp32c6-ftdi.cfg` instead of `board/esp32c6-builtin.cfg` in the
OpenOCD command (see next section).

---

## 4. Starting OpenOCD

OpenOCD is the bridge between your computer and the JTAG hardware. It runs as
a background server. GDB connects to it.

### Source ESP-IDF environment first

```bash
. /home/kty/work/esp32/esp-idf/export.sh
```

### Start OpenOCD (built-in JTAG)

```bash
openocd -f board/esp32c6-builtin.cfg
```

### Start OpenOCD (external JTAG adapter)

```bash
openocd -f board/esp32c6-ftdi.cfg
```

### What success looks like

```
Open On-Chip Debugger  v0.12.0-esp-20240318 (2024-03-18-09:53)
...
Info : Listening on port 3333 for gdb connections
Info : Listening on port 6666 for tcl connections
Info : Listening on port 4444 for telnet connections
Info : esp32c6: target state: halted
Info : esp32c6: Hart 0: PC=0x40818000
```

OpenOCD is now running and listening on port 3333. **Leave this terminal open.**

### What failure looks like

```
Error: libusb_open() failed with LIBUSB_ERROR_ACCESS
```

Fix with udev rules:
```bash
# Add udev rule for Espressif USB devices
echo 'SUBSYSTEM=="usb", ATTRS{idVendor}=="303a", MODE="0666"' | \
  sudo tee /etc/udev/rules.d/99-espressif.rules
sudo udevadm control --reload-rules
sudo udevadm trigger
# Then unplug and replug the board
```

### Alternative: use idf.py

```bash
idf.py openocd    # starts OpenOCD in background
idf.py gdb        # starts GDB and connects automatically
```

`idf.py gdb` is convenient but gives less control. The manual method below is
better for learning what is happening.

---

## 5. Connecting GDB

Open a **second terminal** (keep OpenOCD running in the first):

```bash
. /home/kty/work/esp32/esp-idf/export.sh
cd /home/kty/work/threadx-esp32c6-project

riscv32-esp-elf-gdb build/threadx_esp32c6.elf
```

You will see GDB's startup banner and then its prompt `(gdb)`.

Inside GDB, connect to OpenOCD:

```
(gdb) target remote :3333
```

Expected output:

```
Remote debugging using :3333
0x40818000 in ?? ()
(gdb)
```

The CPU is now **halted**. You have full control.

### Load symbols (if not already loaded by ELF)

```
(gdb) symbol-file build/threadx_esp32c6.elf
```

### Reset and halt (useful to start fresh)

```
(gdb) monitor reset halt
```

### Run to a point before stopping

```
(gdb) monitor reset halt
(gdb) break tx_application_define
(gdb) continue
```

---

## 6. RISC-V Primer for Beginners

You don't need to understand every instruction. This section covers what you
will encounter when debugging this project.

### 6.1 Integer Registers

RISC-V has 32 integer registers, x0 through x31. Each is 32 bits wide on the
ESP32-C6 (RV32). The registers have ABI names that indicate their conventional
use:

| Register | ABI Name | Role |
|---|---|---|
| x0 | zero | Always reads as 0. Writes are ignored. |
| x1 | ra | Return address — where a function returns to |
| x2 | sp | Stack pointer — points to the top of the current stack |
| x3 | gp | Global pointer — base for global variable access |
| x4 | tp | Thread pointer |
| x5–x7 | t0–t2 | Temporary registers (not preserved across calls) |
| x8 | s0/fp | Saved register / frame pointer |
| x9 | s1 | Saved register |
| x10–x11 | a0–a1 | Function arguments and return values |
| x12–x17 | a2–a7 | Function arguments |
| x18–x27 | s2–s11 | Saved registers (preserved across calls) |
| x28–x31 | t3–t6 | Temporary registers (not preserved across calls) |

**What matters for debugging:**
- `sp` (x2) — If this points to an invalid address, you have a stack overflow
- `ra` (x1) — Tells you where the current function will return to
- `a0` — First argument to a function, and its return value
- `pc` — The Program Counter: the address of the instruction currently executing
  (not x0–x31, it is a separate register)

### 6.2 Control and Status Registers (CSRs)

These are special registers that control CPU behaviour. You cannot use ordinary
`add`/`load` instructions on them — there are dedicated CSR instructions
(`csrr`, `csrw`, `csrs`, `csrc`).

The ones you will use most:

| CSR Name | What It Controls |
|---|---|
| `mstatus` | Global machine-mode status including MIE (interrupt enable bit 3) |
| `mie` | Individual interrupt enables (bit 11 = MEIE = machine external interrupt) |
| `mtvec` | Trap vector: where the CPU jumps when an interrupt or exception fires |
| `mcause` | After a trap: what caused it (bit 31=1 → interrupt, lower bits = which one) |
| `mepc` | After a trap: the PC value that was interrupted (where to return to) |
| `mscratch` | Scratch register for machine-mode use |
| `mip` | Interrupt pending bits (read-only status) |

### 6.3 mstatus Bit Fields

`mstatus` is 32 bits. The important ones:

```
bit  3 (MIE):  Machine Interrupt Enable — 1 = interrupts allowed, 0 = blocked
bit  7 (MPIE): Machine Previous Interrupt Enable — saved copy of MIE before trap
bit 11 (MPP0): Machine Previous Privilege Mode — low bit
bit 12 (MPP1): Machine Previous Privilege Mode — high bit
               0b11 = was in Machine mode before the trap
```

When the CPU takes an interrupt:
1. It copies MIE → MPIE (saves the old interrupt-enable state)
2. Sets MIE = 0 (disables further interrupts)
3. Jumps to the trap handler

When the handler executes `mret`:
1. It copies MPIE → MIE (restores interrupt-enable state)
2. Jumps back to mepc

So `mstatus = 0x1888` means: MIE=1 (bit 3), MPIE=1 (bit 7), MPP=11 (bits 11-12).

### 6.4 Reading mcause After a Trap

```
mcause bit 31 = 1:  This is an interrupt (not an exception)
mcause bit 31 = 0:  This is an exception

For interrupts, bits 30:0 = interrupt number:
  11 = Machine External Interrupt (from PLIC)
   3 = Machine Software Interrupt
   7 = Machine Timer Interrupt (mtimecmp — not used on ESP32-C6)

For exceptions, bits 30:0 = exception code:
   0 = Instruction address misaligned
   1 = Instruction access fault
   2 = Illegal instruction
   4 = Load address misaligned
   5 = Load access fault
   6 = Store/AMO address misaligned
   7 = Store/AMO access fault
  11 = Environment call from M-mode
```

When mcause = `0x8000000b` (binary: 1000...0001011), it means:
- Bit 31 = 1 → interrupt
- Bits 30:0 = 11 → Machine External Interrupt

This is the normal value you will see for SYSTIMER interrupts on ESP32-C6.

### 6.5 RISC-V Assembly You Will See in Backtraces

| Instruction | Meaning |
|---|---|
| `addi sp, sp, -128` | Allocate 128 bytes on the stack (move stack pointer down) |
| `sw ra, 124(sp)` | Store register `ra` at address `sp + 124` |
| `lw t0, 0(t1)` | Load 4 bytes from address `t1 + 0` into `t0` |
| `call foo` | Call function `foo` (saves return address in ra, jumps) |
| `ret` | Return from function (jumps to address in ra) |
| `mret` | Machine-mode return from trap (restores PC from mepc, re-enables interrupts) |
| `csrr t0, mcause` | Read CSR `mcause` into register `t0` |
| `csrw mtvec, t0` | Write register `t0` into CSR `mtvec` |
| `csrsi mstatus, 8` | Set bit 3 (MIE) in mstatus — enables interrupts |
| `csrci mstatus, 8` | Clear bit 3 (MIE) in mstatus — disables interrupts |
| `beqz t1, label` | Branch to `label` if `t1 == 0` |
| `bltz t0, label` | Branch to `label` if `t0 < 0` (bit 31 set) |
| `j label` | Unconditional jump to `label` |
| `la t0, symbol` | Load the ADDRESS of `symbol` into `t0` |
| `jr t0` | Jump to the address in register `t0` |

---

## 7. Essential GDB Commands

### 7.1 Execution Control

```
(gdb) continue          # (or c) resume execution
(gdb) step              # (or s) run one source line, step into function calls
(gdb) next              # (or n) run one source line, step over function calls
(gdb) stepi             # (or si) run exactly one machine instruction
(gdb) nexti             # (or ni) run one instruction, step over calls
(gdb) finish            # run until current function returns
(gdb) until 42          # run until line 42 of current file
```

### 7.2 Breakpoints

```
(gdb) break tx_application_define        # break at function entry
(gdb) break tx_esp32c6_timer.c:60       # break at file:line
(gdb) break *0x40800100                 # break at exact address
(gdb) info breakpoints                  # list all breakpoints
(gdb) delete 2                          # delete breakpoint #2
(gdb) disable 2                         # disable without deleting
(gdb) enable 2                          # re-enable
```

### 7.3 Watchpoints (break when a value changes)

```
(gdb) watch _tx_timer_system_clock      # halt when tick counter changes
(gdb) watch *0x3FC00000                 # halt when memory address changes
(gdb) rwatch _tx_timer_system_clock     # halt when read
(gdb) awatch _tx_timer_system_clock     # halt when read or written
(gdb) info watchpoints
```

Watchpoints are extremely useful for confirming "was this variable ever written?"

### 7.4 Examining Registers

```
(gdb) info registers          # all integer registers (x0-x31 + pc)
(gdb) info registers pc sp ra # specific registers
(gdb) p/x $sp                 # print sp in hex
(gdb) p/d $a0                 # print a0 as decimal
(gdb) p/t $mstatus            # print mstatus in binary
```

CSR registers (mstatus, mie, mtvec, mcause, mepc):
```
(gdb) p/x $mtvec
(gdb) p/x $mstatus
(gdb) p/x $mie
(gdb) p/x $mcause
(gdb) p/x $mepc
```

### 7.5 Examining Memory

The `x` command examines memory:  `x/NFS address`
- N = number of units
- F = format: `x` (hex), `d` (decimal), `t` (binary), `i` (instructions), `s` (string)
- S = size: `b` (byte), `h` (halfword=2), `w` (word=4), `g` (8 bytes)

```
(gdb) x/1wx 0x20001000          # read one word at PLIC enable register
(gdb) x/8wx 0x20001000          # read 8 words starting at PLIC base
(gdb) x/16wx $sp                # examine 16 words on the stack
(gdb) x/10i $pc                 # disassemble 10 instructions at current PC
(gdb) x/10i _tx_esp32c6_trap_handler  # disassemble the trap handler
```

### 7.6 Printing Variables and Expressions

```
(gdb) p _tx_timer_system_clock        # print global variable
(gdb) p _tx_thread_execute_ptr        # print pointer to ready thread
(gdb) p *_tx_thread_current_ptr       # dereference: print thread control block
(gdb) p/x &_tx_esp32c6_vector_table  # print address of our vector table
```

### 7.7 Backtrace (Call Stack)

```
(gdb) backtrace           # (or bt) show the current call chain
(gdb) bt 5                # show only the top 5 frames
(gdb) frame 2             # switch to frame #2 to inspect its locals
(gdb) info locals         # local variables in the current frame
(gdb) info args           # function arguments in current frame
```

### 7.8 Disassembly

```
(gdb) disassemble _tx_esp32c6_trap_handler    # disassemble a function
(gdb) disassemble $pc, $pc+40                 # disassemble around PC
(gdb) x/20i $pc-8                             # show instructions around PC
```

### 7.9 OpenOCD Monitor Commands (sent through GDB)

```
(gdb) monitor reset halt          # reset CPU and halt immediately
(gdb) monitor halt                # halt a running CPU
(gdb) monitor resume              # resume (same as continue but via OpenOCD)
(gdb) monitor reg mtvec           # read mtvec via OpenOCD
(gdb) monitor esp appimage_offset 0x10000   # set flash app offset (if needed)
```

### 7.10 Convenience Shortcuts

```
(gdb) set pagination off          # don't pause output with "---Type <return>---"
(gdb) set print array on          # print arrays in a more readable format
(gdb) set print pretty on         # pretty-print structures
(gdb) layout asm                  # show assembly window (TUI mode)
(gdb) layout reg                  # show register window
(gdb) layout split                # show source + assembly windows
(gdb) tui disable                 # exit TUI mode
```

---

## 8. Reading RISC-V Control Registers (CSRs)

CSRs are accessible in GDB by their name with a `$` prefix:

```
(gdb) p/x $mtvec
(gdb) p/x $mstatus
(gdb) p/x $mie
(gdb) p/x $mcause
(gdb) p/x $mepc
```

### What to look for in each

**`mtvec`** — Must point to our vector table:
```
(gdb) p/x $mtvec
$1 = 0x40800000    ← should be in IRAM, 256-byte aligned (last 8 bits = 0)
(gdb) p/x &_tx_esp32c6_vector_table
$2 = 0x40800000    ← should match mtvec exactly
```

If `mtvec` does not equal the address of `_tx_esp32c6_vector_table`, our
`_tx_initialize_low_level` either did not run, or something overwrote mtvec
after we set it.

**`mstatus`** — Global interrupt enable:
```
(gdb) p/x $mstatus
$3 = 0x1888

# Decode:
# bit  3 (value 8):    MIE = 1 → interrupts enabled NOW
# bit  7 (value 0x80): MPIE = 1 → interrupts were enabled before last trap
# bits 11-12 (0x1800): MPP = 0b11 → previous mode was Machine mode
```

If MIE (bit 3) is 0 when the CPU is in the scheduler idle loop, interrupts are
blocked and nothing will ever fire.

**`mie`** — Individual interrupt masks:
```
(gdb) p/x $mie
$4 = 0x800    ← bit 11 set = MEIE = Machine External Interrupt Enable

# bit 11 (0x800): MEIE = 1 → external interrupts (PLIC) can reach the CPU
# bit  7 (0x080): MTIE = 1 → timer interrupt (not used on ESP32-C6)
# bit  3 (0x008): MSIE = 1 → software interrupt
```

If `mie.MEIE` (bit 11) is 0, the PLIC can never deliver interrupts to the CPU
no matter how the PLIC is configured. We set this in `_tx_initialize_low_level`.

**`mcause`** — What caused the last trap (only meaningful after halting inside a trap):
```
(gdb) p/x $mcause
$5 = 0x8000000b   ← Machine External Interrupt (interrupt from PLIC)
# bit 31 = 1 → interrupt (not exception)
# bits 30:0 = 11 = 0xb → machine external interrupt

$5 = 0x00000007   ← Store/AMO access fault = likely stack overflow
# bit 31 = 0 → exception
# bits 30:0 = 7 → store fault
```

**`mepc`** — Where the trap returned from:
```
(gdb) p/x $mepc
$6 = 0x42001234    ← PC that was interrupted (in flash = thread code)
(gdb) info symbol 0x42001234
_tx_thread_schedule_loop + 4 in section .text  ← CPU was in idle loop when interrupted
```

---

## 9. Inspecting ThreadX State

These are the most important ThreadX global variables. All are in `.bss` (zero
at startup) and are updated as ThreadX runs.

### 9.1 Is ThreadX Initialized?

```
(gdb) p _tx_initialize_remaining_to_complete
```

- `2` = not yet reached `tx_application_define` (or still in it)
- `0` = fully initialized, scheduler running

If this is still `2` after the system appears to be running, `tx_kernel_enter()`
was never called — check `port_start_app_hook`.

### 9.2 Is the Tick Counter Advancing?

```
(gdb) p _tx_timer_system_clock
```

This is the ThreadX tick counter. It increments on every SYSTIMER interrupt.

Run it twice, a second apart:
```
(gdb) p _tx_timer_system_clock
$1 = 0
(gdb) continue
^C
(gdb) p _tx_timer_system_clock
$2 = 0     ← still 0: interrupt never fired (our bug!)
$2 = 147   ← advancing at ~100/sec: timer working correctly
```

### 9.3 Which Thread Is Running / Ready?

```
(gdb) p _tx_thread_current_ptr     # thread that last ran (NULL if idle)
(gdb) p _tx_thread_execute_ptr     # next thread to run (NULL if all sleeping)
```

If both are NULL, all threads are sleeping and the scheduler is in its idle loop.

To see the thread's name:
```
(gdb) p _tx_thread_current_ptr->tx_thread_name
$3 = 0x42001234 "main"
(gdb) p *_tx_thread_current_ptr
```

### 9.4 Thread State Fields

Each thread has a `TX_THREAD` control block. Important fields:

```
(gdb) p *_tx_thread_execute_ptr
```

Key fields in the output:
- `tx_thread_name` — the string name ("main", "blink", etc.)
- `tx_thread_state` — current state:
  - `0` = TX_READY
  - `1` = TX_COMPLETED
  - `2` = TX_TERMINATED
  - `4` = TX_SUSPENDED
  - `6` = TX_SLEEP — thread is sleeping (waiting for tick to advance)
  - `13` = TX_MUTEX_SUSP
- `tx_thread_stack_ptr` — current stack pointer (saved when thread suspended)
- `tx_thread_stack_start` — bottom of stack region
- `tx_thread_stack_end` — top of stack region
- `tx_thread_stack_size` — size in bytes
- `tx_thread_run_count` — how many times this thread has been scheduled

### 9.5 Check for Stack Overflow

```
(gdb) p _tx_thread_current_ptr->tx_thread_stack_start
$4 = (void *) 0x3fc8a000

(gdb) p _tx_thread_current_ptr->tx_thread_stack_ptr
$5 = (void *) 0x3fc89ff0    ← 16 bytes above bottom: nearly overflowed!

(gdb) p _tx_thread_current_ptr->tx_thread_stack_end
$6 = (void *) 0x3fc8b000

(gdb) p _tx_thread_current_ptr->tx_thread_stack_size
$7 = 4096
```

Stack grows downward. `tx_thread_stack_ptr` should be well above
`tx_thread_stack_start`. If they are equal or `stack_ptr < stack_start`,
overflow has occurred.

### 9.6 System State (Interrupt Nesting Count)

```
(gdb) p _tx_thread_system_state
```

- `0` = not in an ISR, threads are running normally
- `1` = currently inside one interrupt handler
- `2` = inside a nested interrupt (rare)

If this is stuck at 1 and the system is halted, you are paused inside an ISR.

---

## 10. Inspecting Hardware Registers

These are memory-mapped peripheral registers. Use `x/1wx address` to read them.

### 10.1 Quick Reference Table

| Address | Register | What It Shows |
|---|---|---|
| `0x60010000 + 57*4` = `0x600100E4` | INTMTX SYSTIMER_TARGET0 map | Which CPU line gets SYSTIMER_TARGET0 IRQ |
| `0x20001000` | PLIC MX ENABLE | Bitmask of enabled CPU interrupt lines |
| `0x20001004` | PLIC MX TYPE | 0=level, 1=edge per line (bit per CPU line) |
| `0x20001008` | PLIC MX CLEAR | Write 1 to bit N to clear edge latch for line N |
| `0x2000100C` | PLIC MX EIP_STATUS | Bitmask of pending CPU interrupt lines |
| `0x20001010 + N*4` | PLIC MX priority[N] | Priority of CPU interrupt line N |
| `0x20001090` | PLIC MX THRESH | Interrupt threshold — priority must exceed this |

### 10.2 Verify INTMTX Routes SYSTIMER to CPU Line 7

```
(gdb) x/1wx 0x600100E4
0x600100e4:    0x00000007    ← SYSTIMER_TARGET0 routes to CPU line 7 ✓
0x600100e4:    0x00000000    ← NOT routed anywhere → interrupt will never reach CPU
```

If the value is 0, `esp_rom_route_intr_matrix()` was not called, or
`_tx_port_setup_timer_interrupt` was not run.

### 10.3 Verify PLIC MX Is Configured

```
# PLIC enable register — should have bit 7 set (0x80) for CPU line 7
(gdb) x/1wx 0x20001000
0x20001000:    0x00000080    ← CPU line 7 enabled ✓
0x20001000:    0x00000000    ← nothing enabled → interrupt never fires

# PLIC type register — bit 7 should be 1 (edge-triggered)
(gdb) x/1wx 0x20001004
0x20001004:    0x00000080    ← CPU line 7 is edge-triggered ✓

# PLIC priority for CPU line 7 (offset 7*4 from priority base)
(gdb) x/1wx 0x2000102C
0x2000102c:    0x00000001    ← priority 1 ✓ (must be > threshold to fire)

# PLIC threshold — must be LESS than the interrupt priority
(gdb) x/1wx 0x20001090
0x20001090:    0x00000000    ← threshold 0 ✓ (priority 1 > 0, so interrupt fires)

# Print all 8 PLIC priority registers at once
(gdb) x/8wx 0x20001010
0x20001010: 0x00000000  0x00000000  0x00000000  0x00000000
0x20001020: 0x00000000  0x00000000  0x00000000  0x00000001
#  cpu_int:  0           1           2           3
#            4           5           6           7 ← our timer priority = 1
```

### 10.4 Verify SYSTIMER Is Running

The SYSTIMER counter 1 (OS tick counter) can be read to confirm it is running.

```
# Read SYSTIMER counter 1 value (low 32 bits) — address from TRM
(gdb) x/1wx 0x60023080    # SYSTIMER_UNIT1_VALUE_LO_REG (approximate — verify with TRM)
```

A simpler approach: halt the CPU twice a second apart and compare:
```
(gdb) p _tx_timer_system_clock   # should increase if SYSTIMER + ISR both work
```

### 10.5 Check Vector Table Contents

Our vector table is at `_tx_esp32c6_vector_table`. Each entry is a 4-byte `j`
instruction. Entry 7 (for CPU line 7, our SYSTIMER) is at `base + 7*4 = base + 28`.

```
(gdb) p/x &_tx_esp32c6_vector_table
$1 = 0x40800000

# Disassemble the vector table entries
(gdb) x/10i &_tx_esp32c6_vector_table
   0x40800000:  j    0x40800100   ← vector[0]: exception entry
   0x40800004:  j    0x40800120   ← vector[1]: unused int
   0x40800008:  j    0x40800120
   0x4080000c:  j    0x40800120
   0x40800010:  j    0x40800120
   0x40800014:  j    0x40800120
   0x40800018:  j    0x40800120
   0x4080001c:  j    0x40800140   ← vector[7]: trap handler ← THIS IS THE ONE
   ...

# Verify the address vector[7] jumps to IS our trap handler
(gdb) p/x &_tx_esp32c6_trap_handler
$2 = 0x40800140    ← should match the destination of vector[7] above
```

If vector[7] jumps to an unexpected address, the vector table was not installed
or our handler was placed at a different address.

---

## 11. Debugging Recipes

These are step-by-step procedures for specific debugging scenarios.

---

### Recipe 1: Where Is the CPU Stuck?

**Symptom**: System appears hung, no output, no reset.

**Steps**:
```
# In GDB (CPU running, not halted):
(gdb) monitor halt
Halt issued...  0x40818054 in _tx_thread_schedule_loop ()

(gdb) p/x $pc
$1 = 0x40818054

(gdb) info symbol 0x40818054
_tx_thread_schedule_loop + 4 in section .text

(gdb) backtrace
#0  0x40818054 in _tx_thread_schedule_loop ()
```

**Interpretation**:
- In `_tx_thread_schedule_loop` = CPU is in the ThreadX idle loop, spinning
  waiting for an interrupt. All threads are sleeping.
- If it is ALWAYS here when you halt: the interrupt is not firing.
- In `_tx_esp32c6_unused_int` = an unexpected interrupt fired.
- In `abort` or `__assert_func` = a crash or assertion fired.

---

### Recipe 2: Is the SYSTIMER Interrupt Firing?

**Symptom**: `tx_time_get()` always returns 0. System hangs after threads sleep.

**Method A: Watch the tick counter**
```
(gdb) watch _tx_timer_system_clock
Hardware watchpoint 1: _tx_timer_system_clock

(gdb) continue

# Wait 5 seconds...
# If the watchpoint triggers: interrupt is firing, timer is working
# If you hit Ctrl+C and counter is still 0: interrupt is NOT firing
```

**Method B: Break in the timer ISR**
```
(gdb) break _tx_esp32c6_timer_isr
(gdb) continue

# If this breakpoint hits: ISR is being called
# If it never hits after several seconds: ISR is not being called
```

**Method C: Check hardware state while CPU is halted in the idle loop**
```
(gdb) monitor halt

# 1. Is mtvec pointing to our table?
(gdb) p/x $mtvec
(gdb) p/x &_tx_esp32c6_vector_table
# These must be equal

# 2. Is mie.MEIE set?
(gdb) p/x $mie
# Bit 11 (0x800) must be set

# 3. Is mstatus.MIE set?
(gdb) p/x $mstatus
# Bit 3 (0x8) must be set

# 4. Is INTMTX routing SYSTIMER to CPU line 7?
(gdb) x/1wx 0x600100E4
# Must be 0x7

# 5. Is PLIC enabling CPU line 7?
(gdb) x/1wx 0x20001000
# Must have bit 7 set (0x80)

# 6. Is priority > threshold?
(gdb) x/1wx 0x2000102C    # priority for line 7
(gdb) x/1wx 0x20001090    # threshold
# priority must be > threshold
```

If ALL six checks pass and the interrupt still does not fire, suspect the
SYSTIMER alarm itself was not configured (SYSTIMER HAL init failed silently).

---

### Recipe 3: Did _tx_initialize_low_level Run?

**Symptom**: mtvec is wrong, mie.MEIE not set.

```
(gdb) monitor reset halt
(gdb) break _tx_initialize_low_level
(gdb) continue
# Does this breakpoint hit?

# If YES: step through and watch mtvec being set
(gdb) stepi
(gdb) stepi
# ... until you see the csrw mtvec instruction
(gdb) p/x $mtvec   # read back the value

# If NO: port_start_app_hook is not running, or tx_kernel_enter is not calling it
(gdb) break port_start_app_hook
(gdb) continue
# Does THIS breakpoint hit?
```

---

### Recipe 4: Catch a Crash (Exception)

**Symptom**: System resets without a panic dump, or silent hang.

Our exception handler calls `abort()`. Set a breakpoint on `abort` so you
catch it before it resets:

```
(gdb) monitor reset halt
(gdb) break abort
(gdb) continue

# When it halts in abort():
(gdb) backtrace
#0  abort () at ...
#1  _tx_esp32c6_exception_entry () at tx_initialize_low_level.S:132

(gdb) p/x $mepc    # PC at the time of the exception
$1 = 0x3fc89fe4   ← address that was executing when exception fired

(gdb) info symbol 0x3fc89fe4
blink_thread_entry + 12 in section .text

(gdb) p/x $mcause
$2 = 0x00000007   ← exception code 7 = Store/AMO access fault = bad write address

(gdb) p/x $sp
$3 = 0x3fc8a000   ← stack pointer

(gdb) p _tx_thread_current_ptr->tx_thread_stack_start
$4 = 0x3fc8a000   ← stack bottom = stack pointer: STACK OVERFLOW
```

**Reading mcause after a crash**:
- `0x7` (Store fault): likely a stack overflow — sp reached the guard region
- `0x5` (Load fault): reading from unmapped memory — dangling pointer
- `0x2` (Illegal instruction): jumped to wrong address (corrupted PC or ra)
- `0x1` (Instruction fault): tried to fetch from non-executable memory

---

### Recipe 5: Inspect the Interrupt Stack Frame

When the trap handler runs, it allocates 32*4=128 bytes on the stack and saves
all registers. You can read the saved state to understand what the CPU was doing
when the interrupt fired.

Set a breakpoint at the point just after context_save returns:
```
(gdb) break _tx_esp32c6_timer_isr
(gdb) commands
  > # When ISR is entered, sp points to the saved context frame
  > x/32wx $sp        # dump the entire 128-byte frame
  > p/x $mepc         # where was the CPU when the interrupt fired?
  > p _tx_timer_system_clock
  > continue
  > end
(gdb) continue
```

The frame layout (offset from sp at ISR entry, after context_save):
```
sp + 0*4:  frame type (1 = interrupt frame)
sp + 1*4:  s11 (x27)
sp + 2*4:  s10 (x26)
...
sp + 28*4: ra (x1) — saved by our trap handler, NOT context_save
sp + 30*4: mepc — PC at interrupt (saved by context_save)
```

---

### Recipe 6: Trace a Thread Context Switch

To see how ThreadX switches between threads:
```
(gdb) monitor reset halt
(gdb) break _tx_thread_schedule
(gdb) continue

# Hit in scheduler — a new thread is about to run
(gdb) p _tx_thread_execute_ptr->tx_thread_name
$1 = "main"

(gdb) break _tx_esp32c6_timer_isr
(gdb) continue

# The timer ISR fires. After ISR:
(gdb) p _tx_thread_execute_ptr->tx_thread_name
$2 = "blink"    ← timer woke blink thread (higher priority or timer expired)

(gdb) break _tx_thread_schedule
(gdb) continue
# Context restore will call _tx_thread_schedule to run "blink"
```

---

### Recipe 7: Find Where a Thread Is Waiting

```
(gdb) monitor halt
(gdb) p *_tx_thread_execute_ptr
# Look at tx_thread_state:
#   6 = TX_SLEEP — waiting for tx_thread_sleep to expire
#   4 = TX_SUSPENDED — waiting for event/semaphore/mutex
#   13 = TX_MUTEX_SUSP — blocked on mutex

# For TX_SLEEP: what tick will it wake at?
(gdb) p _tx_thread_execute_ptr->tx_thread_sleep_count
$1 = 93    ← will wake when _tx_timer_system_clock reaches 93

(gdb) p _tx_timer_system_clock
$2 = 0     ← timer not advancing → will never wake
```

---

## 12. GDB Init File

Create a file `gdbinit` in the project root. Load it automatically on startup:

```bash
riscv32-esp-elf-gdb -x gdbinit build/threadx_esp32c6.elf
```

**`gdbinit`** — save this in the project root:

```gdb
# Connect to OpenOCD
target remote :3333

# Quality of life
set pagination off
set print pretty on
set print array on

# Reset and halt on connect
monitor reset halt

# Convenience: show ThreadX tick and thread state
define txstate
  printf "tick:          %d\n", _tx_timer_system_clock
  printf "system_state:  %d (0=normal, 1=in ISR)\n", _tx_thread_system_state
  printf "current_ptr:   0x%08x\n", _tx_thread_current_ptr
  printf "execute_ptr:   0x%08x\n", _tx_thread_execute_ptr
  printf "initialized:   %d (0=ready)\n", _tx_initialize_remaining_to_complete
end
document txstate
  Print ThreadX scheduler state summary.
end

# Convenience: show critical CSRs
define csrstate
  printf "mtvec:    0x%08x\n", $mtvec
  printf "mstatus:  0x%08x  (MIE=%d, MPIE=%d)\n", $mstatus, ($mstatus>>3)&1, ($mstatus>>7)&1
  printf "mie:      0x%08x  (MEIE=%d)\n", $mie, ($mie>>11)&1
  printf "mcause:   0x%08x\n", $mcause
  printf "mepc:     0x%08x\n", $mepc
end
document csrstate
  Print RISC-V machine-mode CSR values.
end

# Convenience: show PLIC and INTMTX configuration
define hwstate
  printf "INTMTX SYSTIMER_TARGET0 -> CPU line: %d\n",  *(unsigned int*)0x600100E4
  printf "PLIC ENABLE:   0x%08x  (line7=%d)\n", *(unsigned int*)0x20001000, (*(unsigned int*)0x20001000>>7)&1
  printf "PLIC TYPE:     0x%08x  (line7 edge=%d)\n", *(unsigned int*)0x20001004, (*(unsigned int*)0x20001004>>7)&1
  printf "PLIC EIP:      0x%08x  (line7=%d)\n", *(unsigned int*)0x2000100C, (*(unsigned int*)0x2000100C>>7)&1
  printf "PLIC PRI[7]:   %d\n", *(unsigned int*)0x2000102C
  printf "PLIC THRESH:   %d\n", *(unsigned int*)0x20001090
end
document hwstate
  Print INTMTX and PLIC configuration for the SYSTIMER interrupt.
end

# Convenience: check for vector table correctness
define vtcheck
  printf "mtvec:        0x%08x\n", $mtvec
  printf "vector_table: 0x%08x\n", &_tx_esp32c6_vector_table
  if $mtvec == &_tx_esp32c6_vector_table
    printf "  mtvec OK: matches vector table address\n"
  else
    printf "  ERROR: mtvec does NOT match vector table address!\n"
  end
  printf "\nVector table disassembly (entries 0 and 7):\n"
  x/1i &_tx_esp32c6_vector_table
  x/1i &_tx_esp32c6_vector_table + 28
end
document vtcheck
  Verify mtvec points to our vector table and show entries 0 (exception) and 7 (timer).
end

# Breakpoint on abort to catch silent crashes
break abort
commands
  printf "*** ABORT CALLED — likely exception or assert\n"
  printf "mepc (faulting PC):  0x%08x\n", $mepc
  printf "mcause:              0x%08x\n", $mcause
  backtrace 10
end

printf "ThreadX GDB session ready.\n"
printf "Commands: txstate, csrstate, hwstate, vtcheck\n"
printf "Type 'continue' to run, Ctrl+C to halt.\n"
```

### Usage

```
$ riscv32-esp-elf-gdb -x gdbinit build/threadx_esp32c6.elf
ThreadX GDB session ready.
Commands: txstate, csrstate, hwstate, vtcheck
Type 'continue' to run, Ctrl+C to halt.

(gdb) continue
^C
(gdb) txstate
tick:          0
system_state:  0 (0=normal, 1=in ISR)
current_ptr:   0x00000000
execute_ptr:   0x00000000
initialized:   0 (0=ready)

(gdb) csrstate
mtvec:    0x40800000
mstatus:  0x00001888  (MIE=1, MPIE=1)
mie:      0x00000800  (MEIE=1)
mcause:   0x80000007
mepc:     0x40818054

(gdb) hwstate
INTMTX SYSTIMER_TARGET0 -> CPU line: 7
PLIC ENABLE:   0x00000080  (line7=1)
PLIC TYPE:     0x00000080  (line7 edge=1)
PLIC EIP:      0x00000000  (line7=0)
PLIC PRI[7]:   1
PLIC THRESH:   0

(gdb) vtcheck
mtvec:        0x40800000
vector_table: 0x40800000
  mtvec OK: matches vector table address
Vector table disassembly:
   0x40800000:  j  0x40800100
   0x4080001c:  j  0x40800140
```

---

## 13. Tips and Common Pitfalls

### Breakpoints in Interrupt Handlers

Hardware breakpoints are required for code in IRAM (our vector table and trap
handler live there). Software breakpoints work by writing a breakpoint
instruction into flash but cannot modify IRAM. OpenOCD on ESP32-C6 provides
2 hardware breakpoints.

```
(gdb) hbreak _tx_esp32c6_trap_handler    # hardware breakpoint (hbreak not break)
(gdb) hbreak _tx_esp32c6_timer_isr
```

If you get "cannot insert breakpoint" with a regular `break`, switch to `hbreak`.

### The CPU Halts Mid-Interrupt

If you halt the CPU while it is inside an interrupt handler:
- `_tx_thread_system_state` will be 1
- `$sp` will point into the ISR stack frame (not a thread's stack)
- `$mepc` shows where the interrupted code was running
- `backtrace` may be confusing because the frame chain is unusual

Use `p/x $mepc` to find where code was interrupted, not `backtrace`.

### "No symbol table" or Wrong Symbols

If GDB reports "no debugging symbols" or shows wrong function names, the ELF
file may be stale:

```
(gdb) symbol-file build/threadx_esp32c6.elf    # reload symbols
(gdb) monitor reset halt
(gdb) load                                      # reflash the ELF (slow)
```

### Watchdog Resets During Debugging

The ESP32-C6 has two watchdogs:
- Task Watchdog (TWDT): kicks if the idle task does not run
- Interrupt Watchdog (IWDT): kicks if interrupts are blocked too long

When halted at a breakpoint, watchdogs may fire and reset the CPU. Disable them
for debugging in `sdkconfig`:

```
CONFIG_ESP_TASK_WDT=n
CONFIG_ESP_INT_WDT=n
```

Or disable them at runtime from GDB:
```
# Write 0x50D83AA1 to the WDT write-protect register, then disable
(gdb) monitor halt
# Use idf.py menuconfig to permanently disable during development
```

### Flash and Debug Simultaneously

You can flash and debug in one command with idf.py:
```bash
idf.py flash -p /dev/ttyACM0 && \
  riscv32-esp-elf-gdb -x gdbinit build/threadx_esp32c6.elf
```

Or use `idf.py flash gdb` if your idf.py version supports it.

### Single-Stepping Through Interrupt Return (mret)

`stepi` will step through `mret` and land at the restored PC (`mepc`). This
lets you trace exactly where execution resumes after an interrupt:

```
(gdb) hbreak _tx_esp32c6_trap_handler
(gdb) continue
# Hit in trap handler
(gdb) stepi  # ... step through context_save, ISR, context_restore ...
# mret executes, PC jumps to mepc
(gdb) p/x $pc   # where did we land?
```

### OpenOCD Loses Connection

If OpenOCD drops the connection (common during flash writes or hard resets):
1. Close GDB with `quit`
2. Stop OpenOCD (Ctrl+C)
3. Restart OpenOCD
4. Restart GDB and reconnect

A Makefile target or shell script makes this less painful:
```bash
# debug.sh
pkill -f openocd 2>/dev/null; sleep 0.5
openocd -f board/esp32c6-builtin.cfg &
sleep 1
riscv32-esp-elf-gdb -x gdbinit build/threadx_esp32c6.elf
```
