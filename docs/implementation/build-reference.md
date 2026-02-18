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

---

## Project Structure and Examples

### Directory Layout

```
threadx-esp32c6-project/
├── CMakeLists.txt              ← root ESP-IDF project (stub placeholder)
├── sdkconfig.defaults
├── main/
│   └── main.c                 ← stub redirecting to examples/
├── components/
│   ├── threadx/               ← shared ThreadX component
│   └── freertos_compat/       ← shared FreeRTOS compat component
└── examples/
    ├── threadx_demo/          ← native ThreadX API demo
    │   ├── CMakeLists.txt
    │   ├── sdkconfig.defaults  (CONFIG_RTOS_SELECTION_THREADX=y)
    │   └── main/
    │       ├── CMakeLists.txt
    │       └── main.c
    └── freertos_demo/         ← FreeRTOS API demo (works with both RTOSes)
        ├── CMakeLists.txt
        ├── sdkconfig.defaults  (CONFIG_RTOS_SELECTION_FREERTOS=y)
        └── main/
            ├── CMakeLists.txt
            └── main.c
```

Each example under `examples/` is a **completely independent, self-contained
ESP-IDF project**. It has its own `CMakeLists.txt`, its own `sdkconfig.defaults`,
its own build directory, and its own `idf.py menuconfig` session. The shared
components in `components/` are referenced from each example via CMake's
`EXTRA_COMPONENT_DIRS` mechanism (explained in detail below).

### Building the Examples

```bash
# Source ESP-IDF first (if not already done)
. $IDF_PATH/export.sh

# --- ThreadX native API demo ---
cd examples/threadx_demo
idf.py build flash monitor

# --- FreeRTOS demo (default: real FreeRTOS) ---
cd examples/freertos_demo
idf.py build flash monitor

# --- FreeRTOS demo running on top of ThreadX compat layer ---
cd examples/freertos_demo
idf.py menuconfig          # RTOS Selection → ThreadX → save
idf.py build flash monitor # same main.c, different RTOS underneath
```

Each example has its own `build/` subdirectory created on first build. You can
have both examples built simultaneously with independent `build/` directories.
Switching RTOS selection in one example does not affect the other.

### Why Two Examples?

**`threadx_demo`** uses the native ThreadX API: `tx_thread_create`,
`tx_thread_sleep`, `tx_time_get`, `TX_THREAD`, `ULONG`. This code can only
compile and run when `RTOS_SELECTION_THREADX` is selected.

**`freertos_demo`** uses the standard FreeRTOS API: `xTaskCreate`, `vTaskDelay`,
`xTaskGetTickCount`. This code works in two modes:

- **FreeRTOS selected**: calls go directly to ESP-IDF's native FreeRTOS
  implementation — no extra components involved, zero overhead.
- **ThreadX selected**: the same calls are intercepted by `freertos_compat`
  (`tx_freertos.c`), which translates them into ThreadX kernel calls. The
  `main.c` source is identical — this demonstrates the compat layer's purpose.

---

## CMake Design: EXTRA_COMPONENT_DIRS Explained

This section explains the CMake mechanism that makes the examples work, the
design decisions behind it, and how it will evolve.

### What is a CMake Variable?

In CMake, a variable is set with `set(NAME value)` and read with `${NAME}`.
Variables are scoped to the current `CMakeLists.txt` file (and any files it
`include()`s). When you write:

```cmake
set(EXTRA_COMPONENT_DIRS
    "${CMAKE_CURRENT_SOURCE_DIR}/../../components/threadx"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../components/freertos_compat"
)
```

you are creating a CMake list variable (CMake lists are semicolon-separated
strings) that contains two filesystem paths. `CMAKE_CURRENT_SOURCE_DIR` is a
built-in CMake variable that always holds the absolute path to the directory
containing the currently-executing `CMakeLists.txt`.

### What Does EXTRA_COMPONENT_DIRS Do?

`EXTRA_COMPONENT_DIRS` is NOT a standard CMake variable. It is defined and
consumed by **ESP-IDF's CMake build system**, specifically by
`$IDF_PATH/tools/cmake/project.cmake` which is included by the line:

```cmake
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
```

When `project.cmake` runs, it calls `idf_build_process()` which:

1. Reads the `EXTRA_COMPONENT_DIRS` variable that you set before the `include()`
2. Searches each listed directory for components
3. Adds discovered components to the build alongside the built-in ESP-IDF
   components (freertos, esp_log, hal, soc, etc.)

**Order matters**: `set(EXTRA_COMPONENT_DIRS ...)` MUST appear BEFORE
`include($ENV{IDF_PATH}/tools/cmake/project.cmake)`. If you put it after,
`project.cmake` has already run and your setting is ignored.

### How Does ESP-IDF Identify a Component?

A directory is recognized as an ESP-IDF component when it contains a
`CMakeLists.txt` file that calls `idf_component_register()`. ESP-IDF walks
the directories listed in `EXTRA_COMPONENT_DIRS` and checks:

```
For each DIR in EXTRA_COMPONENT_DIRS:
  If DIR/CMakeLists.txt calls idf_component_register() → DIR is a component
  Else → search subdirectories of DIR for components
```

Our root project's `CMakeLists.txt` uses the first pattern (direct component
paths):

```cmake
# Root project: points directly to component directories
set(EXTRA_COMPONENT_DIRS
    ${CMAKE_CURRENT_SOURCE_DIR}/components/threadx        # ← IS a component
    ${CMAKE_CURRENT_SOURCE_DIR}/components/freertos_compat # ← IS a component
)
```

We could alternatively point to the parent `components/` directory (ESP-IDF
would then search subdirectories):

```cmake
# Alternative: point to parent, ESP-IDF finds both components inside
set(EXTRA_COMPONENT_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/components)
```

Both approaches work. We use the explicit per-component paths to make the
dependency explicit and avoid accidentally picking up any other directories
that might be placed in `components/` in the future.

### Why the Examples Need EXTRA_COMPONENT_DIRS

Each example under `examples/` is a standalone ESP-IDF project. When you run
`idf.py build` from `examples/threadx_demo/`, ESP-IDF's CMake system starts
fresh. It automatically discovers:

- All components in `$IDF_PATH/components/` (the built-in ESP-IDF components)
- All components in the project's own `components/` subdirectory (if it exists)
- All components listed in `EXTRA_COMPONENT_DIRS`

It does NOT automatically walk up the directory tree to find components in
parent directories. From ESP-IDF's perspective, `examples/threadx_demo/` is a
complete project — it has no way to know that `threadx` and `freertos_compat`
live two directories up.

Without `EXTRA_COMPONENT_DIRS`, building `examples/threadx_demo/` would produce:

```
error: Could not find component 'threadx' required by 'main'
```

With `EXTRA_COMPONENT_DIRS` pointing to the parent's components:

```cmake
# examples/threadx_demo/CMakeLists.txt
set(EXTRA_COMPONENT_DIRS
    "${CMAKE_CURRENT_SOURCE_DIR}/../../components/threadx"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../components/freertos_compat"
)
```

ESP-IDF adds those two directories to its component search and finds `threadx`
and `freertos_compat`. The example can then declare `REQUIRES threadx` in its
`main/CMakeLists.txt` and everything works.

### Path Construction: CMAKE_CURRENT_SOURCE_DIR

`CMAKE_CURRENT_SOURCE_DIR` is set by CMake to the absolute path of the directory
being processed. For `examples/threadx_demo/CMakeLists.txt`, it is:

```
/home/kty/work/threadx-esp32c6-project/examples/threadx_demo
```

So `${CMAKE_CURRENT_SOURCE_DIR}/../../components/threadx` expands to:

```
/home/kty/work/threadx-esp32c6-project/examples/threadx_demo/../../components/threadx
→ /home/kty/work/threadx-esp32c6-project/components/threadx   (after resolution)
```

Using `CMAKE_CURRENT_SOURCE_DIR` instead of a hardcoded absolute path means the
project can be cloned to any location on any machine and still work. This is
portable CMake practice.

### Component Discovery and the Component Graph

Once ESP-IDF discovers all components (built-in + extra), it builds a **component
dependency graph** by reading each component's `REQUIRES` and `PRIV_REQUIRES`
declarations in their `idf_component_register()` calls.

```
main  →  REQUIRES threadx, freertos_compat
          threadx  →  REQUIRES esp_hw_support, esp_system, esp_timer, soc, hal, log, newlib
          freertos_compat  →  REQUIRES threadx, log, esp_common, newlib
```

ESP-IDF uses this graph to:
1. Determine which components must be compiled
2. Propagate include paths: if A REQUIRES B, A's source files can include
   headers from B's `INCLUDE_DIRS` without specifying the path
3. Propagate link libraries: if A REQUIRES B, B's compiled objects are linked
   into the final binary when A is included
4. Propagate compile definitions: if B declares a `PUBLIC` compile definition,
   A inherits it

### REQUIRES vs PRIV_REQUIRES

`idf_component_register` supports two dependency lists:

```cmake
idf_component_register(
    SRCS "..."
    REQUIRES      esp_hw_support log    # PUBLIC dependencies
    PRIV_REQUIRES newlib                # PRIVATE dependencies
)
```

**REQUIRES (public)**: The dependency is transitively exported. If component A
has `REQUIRES B`, and component C has `REQUIRES A`, then C also gets B's include
paths and link libraries automatically. Use this when your public header files
`#include` headers from the required component (downstream users of your headers
need those paths too).

**PRIV_REQUIRES (private)**: The dependency is only used internally. If A has
`PRIV_REQUIRES B`, and C has `REQUIRES A`, C does NOT get B's include paths.
Use this when the required component is only used in `.c` files, not exposed
via public headers.

In our `freertos_compat` component, `threadx` is listed under `REQUIRES`
(public) because `freertos_compat`'s public header `FreeRTOS.h` transitively
includes `tx_api.h` from threadx. Any component that uses `freertos_compat`
needs `tx_api.h` to be findable, so the propagation must be public.

### Conditional REQUIRES in freertos_demo

The `freertos_demo/main/CMakeLists.txt` uses a conditional:

```cmake
if(CONFIG_RTOS_SELECTION_THREADX)
    idf_component_register(
        SRCS "main.c"
        INCLUDE_DIRS "."
        REQUIRES freertos_compat   # pulls in threadx transitively via REQUIRES chain
    )
else()
    idf_component_register(
        SRCS "main.c"
        INCLUDE_DIRS "."
        # No REQUIRES — standard FreeRTOS is always available
    )
endif()
```

`CONFIG_RTOS_SELECTION_THREADX` is a CMake variable populated from `sdkconfig.cmake`,
which is generated by ESP-IDF from the Kconfig/sdkconfig system. When you run
`idf.py menuconfig`, Kconfig writes values to `sdkconfig`. ESP-IDF's build system
converts `sdkconfig` to `sdkconfig.cmake`, which sets CMake variables like:

```cmake
set(CONFIG_RTOS_SELECTION_THREADX "y")   # ThreadX selected
# or:
# CONFIG_RTOS_SELECTION_THREADX is absent  # FreeRTOS selected
```

CMake's `if(CONFIG_RTOS_SELECTION_THREADX)` evaluates to TRUE when the variable
is set to a non-empty, non-zero, non-FALSE string — `"y"` qualifies. When the
variable is absent (unset), `if()` evaluates to FALSE.

**Why only `freertos_compat` and not `threadx` directly?** In ThreadX mode,
`freertos_compat` already has `REQUIRES threadx` in its own `idf_component_register`.
Because REQUIRES is transitive, declaring `REQUIRES freertos_compat` in the main
component automatically brings in threadx's include paths and link libraries too.
There is no need to list `threadx` explicitly in main — that would be redundant.

### The Early Return Pattern in Component CMakeLists.txt

Both `threadx` and `freertos_compat` use an early return guard:

```cmake
if(NOT CONFIG_RTOS_SELECTION_THREADX)
    idf_component_register()   # register as an empty component
    return()                   # stop processing this file
endif()

# ... full component registration with SRCS, INCLUDE_DIRS, etc. ...
idf_component_register(SRCS ... INCLUDE_DIRS ... REQUIRES ...)
```

`return()` in CMake exits the current file's execution scope. When reached from
a `CMakeLists.txt`, it stops processing that file immediately — everything below
is not executed.

**Why register as empty before returning?** ESP-IDF requires that every component
directory call `idf_component_register()` exactly once. If a component's
`CMakeLists.txt` never calls it, ESP-IDF's CMake infrastructure throws an error.
Calling it with no arguments registers the component as an empty interface target:
no sources, no include dirs, no link libraries. The component "exists" in the
build graph (so `REQUIRES threadx` in other components doesn't fail) but
contributes nothing to the binary.

This pattern is the CMake equivalent of a compile-time ifdef: when FreeRTOS mode
is selected, the threadx component directory is present (ESP-IDF finds it via
`EXTRA_COMPONENT_DIRS`) but effectively does nothing.

### EXTRA_COMPONENT_DIRS vs idf_component.yml (Future)

The `EXTRA_COMPONENT_DIRS` approach used here is suitable for a monorepo where
the components live alongside the projects that use them. It has one limitation:
every project that wants to use these components must manually add the paths to
`EXTRA_COMPONENT_DIRS`. If ten projects want to use `threadx`, all ten need the
same `EXTRA_COMPONENT_DIRS` entry.

The modern ESP-IDF alternative is the **managed component system**
(IDF Component Manager). Instead of `EXTRA_COMPONENT_DIRS`, you create an
`idf_component.yml` manifest alongside your `CMakeLists.txt`:

```yaml
## idf_component.yml (future — not yet implemented)
dependencies:
  threadx:
    git: https://github.com/your-org/threadx-esp32c6-component.git
    version: ">=1.0.0"
  freertos_compat:
    git: https://github.com/your-org/freertos-compat-esp32c6.git
    version: ">=1.0.0"
```

ESP-IDF would then automatically download and register the components for any
project that has this manifest, anywhere on any machine. The `EXTRA_COMPONENT_DIRS`
lines in each example's `CMakeLists.txt` would be deleted entirely.

The current `EXTRA_COMPONENT_DIRS` approach is a correct and standard interim
solution. When the components are ready to be published as independent managed
components, the migration is a one-line change per example.

### Summary of CMake Variables and Functions Used

| Name | Type | Defined by | Purpose |
|---|---|---|---|
| `CMAKE_CURRENT_SOURCE_DIR` | Built-in variable | CMake | Absolute path of current CMakeLists.txt's directory |
| `EXTRA_COMPONENT_DIRS` | Convention variable | ESP-IDF | Extra directories to search for components |
| `CONFIG_RTOS_SELECTION_THREADX` | Generated variable | ESP-IDF (from sdkconfig) | True when ThreadX is selected in menuconfig |
| `idf_component_register()` | CMake function | ESP-IDF | Declares a component's sources, include dirs, and dependencies |
| `idf_component_get_property()` | CMake function | ESP-IDF | Reads a property (e.g., COMPONENT_DIR) from another component |
| `target_include_directories()` | CMake command | CMake built-in | Adds include search paths to a compile target |
| `target_compile_definitions()` | CMake command | CMake built-in | Adds preprocessor defines to a compile target |
| `target_compile_options()` | CMake command | CMake built-in | Adds compiler flags to a compile target |
| `target_link_options()` | CMake command | CMake built-in | Adds linker flags when linking against a target |
| `return()` | CMake command | CMake built-in | Stops execution of the current file |
| `include()` | CMake command | CMake built-in | Executes another CMake file in the current scope |
| `set()` | CMake command | CMake built-in | Creates or updates a CMake variable |
| `if() / elseif() / else() / endif()` | CMake commands | CMake built-in | Conditional execution |
| `project()` | CMake command | CMake built-in | Names the project; required in top-level CMakeLists.txt |
| `cmake_minimum_required()` | CMake command | CMake built-in | Declares minimum CMake version; required first line |

---
