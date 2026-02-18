# ThreadX ESP-IDF Integration: menuconfig & Build System Spec

**Goal:** Seamlessly integrate ThreadX as a selectable RTOS in ESP-IDF with zero friction for developers

**Target:** ESP32-C6 (expandable to other RISC-V ESP chips later)

---

## Overview: Developer Experience

### What We Want to Enable

```bash
# Creating a new project with ThreadX
idf.py create-project my-threadx-app
cd my-threadx-app
idf.py set-target esp32c6
idf.py menuconfig
# → Select "ThreadX" from RTOS options
# → Configure ThreadX parameters
idf.py build
idf.py flash
# → Works exactly like FreeRTOS project
```

**Key Principle:** Developer shouldn't need to know ThreadX is different. It should "just work."

---

## Phase 1: Kconfig Integration

### 1.1 RTOS Selection Menu

**Location:** `components/esp_system/Kconfig`

Create a new top-level RTOS configuration:

```kconfig
menu "Operating System"

choice ESP_RTOS_SELECTION
    prompt "Select RTOS"
    default ESP_RTOS_FREERTOS
    depends on IDF_TARGET_ESP32C6 || IDF_TARGET_ESP32C5 || IDF_TARGET_ESP32H2
    help
        Select which RTOS to use for your ESP32-C6 project.
        
        FreeRTOS: Default RTOS with extensive ESP-IDF integration
        ThreadX: Azure RTOS ThreadX with compatibility layer
        
config ESP_RTOS_FREERTOS
    bool "FreeRTOS"
    help
        Use FreeRTOS as the operating system.
        
config ESP_RTOS_THREADX
    bool "ThreadX (Azure RTOS)"
    depends on IDF_TARGET_ESP32C6
    help
        Use ThreadX (Azure RTOS) as the operating system.
        Requires compatibility layer for WiFi/Bluetooth stacks.
        
endchoice

config ESP_RTOS_NAME
    string
    default "FreeRTOS" if ESP_RTOS_FREERTOS
    default "ThreadX" if ESP_RTOS_THREADX

endmenu
```

### 1.2 ThreadX-Specific Configuration Menu

**Location:** `components/threadx/Kconfig` (new component)

```kconfig
menu "ThreadX Configuration"
    depends on ESP_RTOS_THREADX
    visible if ESP_RTOS_THREADX

config THREADX_MAX_PRIORITIES
    int "Maximum number of thread priorities"
    range 32 1024
    default 32
    help
        Maximum number of priority levels available in ThreadX.
        Lower values use less memory.

config THREADX_MINIMAL_STACK_SIZE
    int "Minimum thread stack size (bytes)"
    range 512 8192
    default 1024
    help
        Minimum stack size for ThreadX threads.

config THREADX_TIMER_TICKS_PER_SECOND
    int "Timer ticks per second"
    range 10 1000
    default 100
    help
        System timer frequency. 100 = 10ms tick, 1000 = 1ms tick.

config THREADX_PREEMPTION_THRESHOLD
    bool "Enable preemption threshold"
    default y
    help
        Enable preemption-threshold scheduling in ThreadX.

config THREADX_ENABLE_STACK_CHECKING
    bool "Enable stack overflow checking"
    default y
    help
        Enable runtime stack overflow detection (performance impact).

config THREADX_ENABLE_EVENT_TRACE
    bool "Enable TraceX event logging"
    default n
    help
        Enable ThreadX event tracing for TraceX debugging tool.

menu "ThreadX Memory Configuration"

config THREADX_BYTE_POOL_SIZE
    int "Default byte pool size (bytes)"
    range 4096 262144
    default 32768
    help
        Size of ThreadX default byte memory pool.

config THREADX_BLOCK_POOL_SIZE
    int "Default block pool size (bytes)"
    range 4096 262144
    default 16384
    help
        Size of ThreadX default block memory pool.

endmenu

menu "FreeRTOS Compatibility Layer"

config THREADX_FREERTOS_COMPAT_FULL
    bool "Enable full FreeRTOS API compatibility"
    default y
    help
        Provide complete FreeRTOS API emulation layer.
        Required for WiFi/Bluetooth stacks.

config THREADX_FREERTOS_COMPAT_TASK_NOTIFICATIONS
    bool "Enable task notification emulation"
    depends on THREADX_FREERTOS_COMPAT_FULL
    default y
    help
        Emulate FreeRTOS task notifications using ThreadX semaphores.

config THREADX_FREERTOS_COMPAT_STREAM_BUFFERS
    bool "Enable stream buffer emulation"
    depends on THREADX_FREERTOS_COMPAT_FULL
    default y
    help
        Emulate FreeRTOS stream buffers.

config THREADX_FREERTOS_COMPAT_MESSAGE_BUFFERS
    bool "Enable message buffer emulation"
    depends on THREADX_FREERTOS_COMPAT_FULL
    default y
    help
        Emulate FreeRTOS message buffers.

endmenu

menu "Advanced ThreadX Settings"

config THREADX_INLINE_THREAD_RESUME_SUSPEND
    bool "Inline thread resume/suspend"
    default y
    help
        Use inline assembly for thread suspend/resume (faster).

config THREADX_REACTIVATE_INLINE
    bool "Inline timer reactivation"
    default n
    help
        Use inline code for timer reactivation (larger code size).

config THREADX_DISABLE_NOTIFY_CALLBACKS
    bool "Disable notify callbacks"
    default n
    help
        Disable all notify callbacks to reduce code size.

endmenu

endmenu
```

---

## Phase 2: Component Structure

### 2.1 New Components to Create

```
components/
├── threadx/                          # ThreadX core RTOS
│   ├── CMakeLists.txt
│   ├── Kconfig
│   ├── idf_component.yml
│   ├── port/                         # ESP32-C6 RISC-V port
│   │   ├── risc-v/
│   │   │   ├── tx_port.c
│   │   │   ├── tx_port.h
│   │   │   ├── tx_thread_stack_build.c
│   │   │   ├── tx_timer_interrupt.c
│   │   │   └── tx_initialize_low_level.S
│   │   └── esp32c6/
│   │       ├── tx_esp32c6_startup.c
│   │       └── tx_esp32c6.ld          # Linker script
│   ├── src/                           # ThreadX source (from Azure RTOS)
│   │   └── [ThreadX core files]
│   └── include/
│       ├── tx_api.h
│       ├── tx_port.h
│       └── tx_user.h                  # Config header
│
├── threadx_compat/                    # FreeRTOS compatibility layer
│   ├── CMakeLists.txt
│   ├── Kconfig
│   ├── include/
│   │   ├── freertos/
│   │   │   ├── FreeRTOS.h            # FreeRTOS API shim
│   │   │   ├── task.h
│   │   │   ├── queue.h
│   │   │   ├── semphr.h
│   │   │   ├── timers.h
│   │   │   ├── event_groups.h
│   │   │   └── stream_buffer.h
│   │   └── freertos/
│   │       └── FreeRTOSConfig.h       # Generated from Kconfig
│   ├── src/
│   │   ├── freertos_tasks.c           # Task API mapping
│   │   ├── freertos_queue.c           # Queue API mapping
│   │   ├── freertos_semaphore.c       # Semaphore API mapping
│   │   ├── freertos_timers.c          # Timer API mapping
│   │   ├── freertos_event_groups.c    # Event groups API mapping
│   │   └── freertos_stream_buffer.c   # Stream buffer API mapping
│   └── test/
│       └── test_compat_layer.c        # Unit tests
│
└── esp_system/
    └── port/
        └── threadx/                   # ThreadX-specific system hooks
            ├── esp_system_threadx.c
            └── panic_handler_threadx.c
```

### 2.2 Modified Existing Components

```
components/
├── esp_wifi/
│   └── CMakeLists.txt                 # Add ThreadX compat dependency
├── bt/
│   └── CMakeLists.txt                 # Add ThreadX compat dependency
├── esp_timer/
│   └── CMakeLists.txt                 # Add ThreadX timer backend
└── nvs_flash/
    └── CMakeLists.txt                 # Ensure RTOS-agnostic
```

---

## Phase 3: CMake Build System Integration

### 3.1 ThreadX Component CMakeLists.txt

**File:** `components/threadx/CMakeLists.txt`

```cmake
idf_component_register(
    SRCS
        # ThreadX core sources
        "src/tx_initialize_kernel_enter.c"
        "src/tx_thread_create.c"
        "src/tx_thread_delete.c"
        "src/tx_thread_resume.c"
        "src/tx_thread_suspend.c"
        "src/tx_mutex_create.c"
        "src/tx_semaphore_create.c"
        "src/tx_queue_create.c"
        "src/tx_timer_create.c"
        # ... (all ThreadX core .c files)
        
        # ESP32-C6 RISC-V port
        "port/risc-v/tx_port.c"
        "port/risc-v/tx_thread_stack_build.c"
        "port/risc-v/tx_timer_interrupt.c"
        "port/esp32c6/tx_esp32c6_startup.c"
        
    INCLUDE_DIRS
        "include"
        "port/risc-v"
        "port/esp32c6"
        
    REQUIRES
        esp_system
        esp_hw_support
        esp_timer
        
    PRIV_REQUIRES
        heap
)

# Add port-specific assembly files
target_sources(${COMPONENT_LIB} PRIVATE
    "port/risc-v/tx_initialize_low_level.S"
)

# Compile definitions from Kconfig
target_compile_definitions(${COMPONENT_LIB} PUBLIC
    TX_MAX_PRIORITIES=${CONFIG_THREADX_MAX_PRIORITIES}
    TX_MINIMUM_STACK=${CONFIG_THREADX_MINIMAL_STACK_SIZE}
    TX_TIMER_TICKS_PER_SECOND=${CONFIG_THREADX_TIMER_TICKS_PER_SECOND}
)

if(CONFIG_THREADX_PREEMPTION_THRESHOLD)
    target_compile_definitions(${COMPONENT_LIB} PUBLIC
        TX_ENABLE_PREEMPTION_THRESHOLD
    )
endif()

if(CONFIG_THREADX_ENABLE_STACK_CHECKING)
    target_compile_definitions(${COMPONENT_LIB} PUBLIC
        TX_ENABLE_STACK_CHECKING
    )
endif()

if(CONFIG_THREADX_ENABLE_EVENT_TRACE)
    target_compile_definitions(${COMPONENT_LIB} PUBLIC
        TX_ENABLE_EVENT_TRACE
    )
endif()

# Link ThreadX-specific linker script
target_linker_script(${COMPONENT_LIB} INTERFACE
    "port/esp32c6/tx_esp32c6.ld"
)
```

### 3.2 FreeRTOS Compatibility Layer CMakeLists.txt

**File:** `components/threadx_compat/CMakeLists.txt`

```cmake
if(CONFIG_ESP_RTOS_THREADX)
    idf_component_register(
        SRCS
            "src/freertos_tasks.c"
            "src/freertos_queue.c"
            "src/freertos_semaphore.c"
            "src/freertos_timers.c"
            "src/freertos_event_groups.c"
            "src/freertos_stream_buffer.c"
            
        INCLUDE_DIRS
            "include"
            
        REQUIRES
            threadx
            esp_system
            esp_timer
    )
    
    # Generate FreeRTOSConfig.h from Kconfig
    set(FREERTOS_CONFIG_HEADER "${CMAKE_CURRENT_BINARY_DIR}/include/FreeRTOSConfig.h")
    
    configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/FreeRTOSConfig.h.in"
        "${FREERTOS_CONFIG_HEADER}"
        @ONLY
    )
    
    target_include_directories(${COMPONENT_LIB} PUBLIC
        "${CMAKE_CURRENT_BINARY_DIR}/include"
    )
    
    # This component "replaces" freertos for dependent components
    idf_build_set_property(COMPILE_OPTIONS "-DCONFIG_FREERTOS_USE_THREADX=1" APPEND)
    
endif()
```

### 3.3 Top-Level Project CMakeLists.txt Template

**File:** `templates/threadx_project/CMakeLists.txt`

```cmake
# The following lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

# Set default RTOS to ThreadX if not specified
if(NOT DEFINED ENV{IDF_RTOS})
    set(ENV{IDF_RTOS} "threadx")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(threadx_example)
```

---

## Phase 4: idf.py Extensions

### 4.1 Custom idf.py Commands

**File:** `tools/idf_py_actions/threadx_actions.py`

```python
import os
import click
from idf_py_actions.tools import ensure_build_directory

def action_extensions(base_actions, project_path):
    def threadx_info(args):
        """Display ThreadX configuration information"""
        print("ThreadX Configuration:")
        print(f"  RTOS: ThreadX (Azure RTOS)")
        print(f"  Target: {os.environ.get('IDF_TARGET', 'esp32c6')}")
        print(f"  Compatibility Layer: FreeRTOS API emulation enabled")
        
    def create_threadx_project(args):
        """Create a new ThreadX project from template"""
        project_name = args.name
        template_path = os.path.join(
            os.environ['IDF_PATH'], 
            'examples/threadx/get-started/hello_world'
        )
        # ... project creation logic ...
        print(f"Created ThreadX project: {project_name}")
    
    return {
        'actions': {
            'threadx-info': {
                'callback': threadx_info,
                'help': 'Display ThreadX RTOS configuration.',
            },
            'create-threadx': {
                'callback': create_threadx_project,
                'help': 'Create new ThreadX project from template.',
                'options': [
                    click.option('--name', required=True, help='Project name'),
                ],
            },
        }
    }
```

### 4.2 Modified idf.py Workflow

```bash
# New workflow with ThreadX
idf.py set-target esp32c6
idf.py menuconfig
# → Component config → Operating System → Select "ThreadX"
idf.py threadx-info        # Show ThreadX config
idf.py build               # Build with ThreadX
idf.py flash               # Flash normally
idf.py monitor             # Monitor output
```

---

## Phase 5: Secure Boot Integration

### 5.1 Bootloader Compatibility

**Requirements:**
- ThreadX must work with existing ESP32-C6 secure boot v2
- No changes needed to bootloader itself (it's RTOS-agnostic)
- App signing and verification work normally

**File:** `components/bootloader_support/src/esp32c6/bootloader_esp32c6.c`

No changes needed - bootloader doesn't care about app RTOS.

### 5.2 Secure Boot Configuration

**File:** `components/threadx/port/esp32c6/tx_esp32c6_startup.c`

```c
#include "esp_secure_boot.h"
#include "tx_api.h"

void tx_application_define(void *first_unused_memory)
{
    #ifdef CONFIG_SECURE_BOOT_V2_ENABLED
    // Secure boot already verified by bootloader
    // No special handling needed in ThreadX
    #endif
    
    // Standard ThreadX app initialization
    // ...
}
```

### 5.3 Flash Encryption Compatibility

ThreadX applications work with flash encryption exactly like FreeRTOS:

```bash
# Enable secure boot + flash encryption in menuconfig
idf.py menuconfig
# → Security features → Enable secure boot v2
# → Security features → Enable flash encryption

# Build and flash with secure boot
idf.py build
idf.py flash

# Subsequent OTA updates work normally
```

**Key Point:** Security features are bootloader/hardware level, RTOS-independent.

---

## Phase 6: Example Projects

### 6.1 Hello World ThreadX

**File:** `examples/threadx/get-started/hello_world/main/hello_world_main.c`

```c
#include <stdio.h>
#include "tx_api.h"
#include "esp_log.h"

#define DEMO_STACK_SIZE 1024

static TX_THREAD demo_thread;
static ULONG demo_thread_stack[DEMO_STACK_SIZE / sizeof(ULONG)];

static const char *TAG = "hello_threadx";

void demo_thread_entry(ULONG thread_input)
{
    int count = 0;
    while(1) {
        ESP_LOGI(TAG, "Hello from ThreadX! Count: %d", count++);
        tx_thread_sleep(100); // Sleep 100 ticks (1 second if tick = 10ms)
    }
}

void tx_application_define(void *first_unused_memory)
{
    UINT status;
    
    ESP_LOGI(TAG, "ThreadX application starting...");
    
    status = tx_thread_create(
        &demo_thread,
        "demo_thread",
        demo_thread_entry,
        0,
        demo_thread_stack,
        DEMO_STACK_SIZE,
        1,  // Priority
        1,  // Preemption threshold
        TX_NO_TIME_SLICE,
        TX_AUTO_START
    );
    
    if (status != TX_SUCCESS) {
        ESP_LOGE(TAG, "Failed to create thread: %d", status);
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "Starting ThreadX kernel...");
    
    // Enter ThreadX kernel
    tx_kernel_enter();
    
    // Should never reach here
}
```

### 6.2 WiFi Station Example

**File:** `examples/threadx/wifi/getting_started/station/main/station_example_main.c`

```c
// This should look IDENTICAL to FreeRTOS version
// Thanks to compatibility layer!

#include "freertos/FreeRTOS.h"  // Actually maps to ThreadX
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"

// ... standard ESP-IDF WiFi station code ...
// No ThreadX-specific code visible!
```

---

## Phase 7: Testing & Validation

### 7.1 Automated Tests

**File:** `components/threadx_compat/test/test_compat_layer.c`

```c
#include "unity.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

TEST_CASE("FreeRTOS task creation compatibility", "[threadx_compat]")
{
    TaskHandle_t handle;
    BaseType_t ret = xTaskCreate(
        test_task,
        "test",
        2048,
        NULL,
        5,
        &handle
    );
    
    TEST_ASSERT_EQUAL(pdPASS, ret);
    TEST_ASSERT_NOT_NULL(handle);
    
    vTaskDelete(handle);
}

TEST_CASE("FreeRTOS queue operations", "[threadx_compat]")
{
    QueueHandle_t queue = xQueueCreate(10, sizeof(uint32_t));
    TEST_ASSERT_NOT_NULL(queue);
    
    uint32_t send_val = 42;
    BaseType_t ret = xQueueSend(queue, &send_val, 0);
    TEST_ASSERT_EQUAL(pdTRUE, ret);
    
    uint32_t recv_val;
    ret = xQueueReceive(queue, &recv_val, 0);
    TEST_ASSERT_EQUAL(pdTRUE, ret);
    TEST_ASSERT_EQUAL(send_val, recv_val);
    
    vQueueDelete(queue);
}

// ... more tests for all FreeRTOS APIs ...
```

### 7.2 CI/CD Integration

**File:** `.gitlab-ci.yml` (or GitHub Actions)

```yaml
test_threadx_esp32c6:
  stage: test
  tags:
    - esp32c6
  script:
    - idf.py set-target esp32c6
    - echo "CONFIG_ESP_RTOS_THREADX=y" >> sdkconfig.defaults
    - idf.py build
    - idf.py flash
    - idf.py monitor | tee output.log
    - grep "ThreadX kernel started" output.log
    
test_threadx_wifi:
  stage: test
  tags:
    - esp32c6
    - wifi
  script:
    - cd examples/threadx/wifi/getting_started/station
    - idf.py build flash monitor
    - # ... validate WiFi connection ...
```

---

## Phase 8: Documentation

### 8.1 Migration Guide

**File:** `docs/en/migration-guides/release-6.x/threadx.rst`

```rst
ThreadX RTOS Option
===================

Starting from ESP-IDF v6.0, ESP32-C6 supports ThreadX (Azure RTOS) as an alternative to FreeRTOS.

Why ThreadX?
------------

- Deterministic real-time performance
- Pre-certified for safety standards (IEC 61508, IEC 62304)
- Royalty-free under MIT license
- Smaller memory footprint

Enabling ThreadX
----------------

In menuconfig:

1. ``Component config`` → ``Operating System`` → Select ``ThreadX``
2. Configure ThreadX parameters under ``ThreadX Configuration``
3. Build and flash normally

API Compatibility
-----------------

ThreadX provides a FreeRTOS compatibility layer. Your existing code should work without changes:

.. code-block:: c

    #include "freertos/FreeRTOS.h"  // Automatically maps to ThreadX
    #include "freertos/task.h"
    
    void app_main(void) {
        xTaskCreate(my_task, "task", 2048, NULL, 5, NULL);
    }

Native ThreadX API
------------------

You can also use ThreadX APIs directly:

.. code-block:: c

    #include "tx_api.h"
    
    void tx_application_define(void *first_unused_memory) {
        tx_thread_create(&my_thread, ...);
    }

Limitations
-----------

- ThreadX currently only supported on ESP32-C6 (RISC-V targets)
- Some advanced FreeRTOS features not yet supported (co-routines, MPU)
```

### 8.2 API Reference

**File:** `docs/en/api-reference/system/threadx.rst`

Complete API documentation with examples.

---

## Phase 9: Rollout Plan

### 9.1 Release Strategy

**Alpha (Internal Testing)**
- Component structure in place
- Basic ThreadX boot working
- Compatibility layer for core APIs
- Hello World example functional

**Beta (Community Preview)**
- WiFi stack integration complete
- Bluetooth stack integration complete
- Example projects for common use cases
- Documentation published

**Stable Release (ESP-IDF v6.x)**
- Full test coverage
- CI/CD integration
- Production-ready compatibility layer
- Official support in ESP-IDF

### 9.2 Versioning

```
components/threadx/idf_component.yml:

version: "1.0.0"
description: "ThreadX RTOS port for ESP32-C6"
dependencies:
  idf: ">=6.0"
  
targets:
  - esp32c6
  
repository: https://github.com/espressif/esp-idf
license: MIT
```

---

## Implementation Checklist

### Core Integration
- [ ] Create `components/threadx/` with Kconfig
- [ ] Create `components/threadx_compat/` with FreeRTOS shim
- [ ] Implement RTOS selection in menuconfig
- [ ] Add ThreadX to ESP-IDF build system
- [ ] Port ThreadX RISC-V to ESP32-C6 specifics

### Compatibility Layer
- [ ] Task management APIs (xTaskCreate, vTaskDelete, etc.)
- [ ] Queue APIs (xQueueCreate, xQueueSend, xQueueReceive)
- [ ] Semaphore APIs (xSemaphoreCreateBinary, xSemaphoreTake, etc.)
- [ ] Mutex APIs (xSemaphoreCreateMutex, etc.)
- [ ] Timer APIs (xTimerCreate, xTimerStart, etc.)
- [ ] Event group APIs (xEventGroupCreate, xEventGroupSetBits, etc.)
- [ ] Stream buffer APIs (xStreamBufferCreate, etc.)
- [ ] Task notifications (xTaskNotifyGive, ulTaskNotifyTake, etc.)

### System Integration
- [ ] WiFi stack compatibility
- [ ] Bluetooth stack compatibility
- [ ] ESP-HAL integration
- [ ] NVS flash compatibility
- [ ] ESP timer backend for ThreadX
- [ ] Panic handler for ThreadX
- [ ] Core dump support

### Build System
- [ ] CMake component registration
- [ ] Linker script for ThreadX
- [ ] Kconfig auto-generation of tx_user.h
- [ ] idf.py extensions for ThreadX commands
- [ ] Project templates

### Security Features
- [ ] Verify secure boot v2 compatibility
- [ ] Verify flash encryption compatibility
- [ ] Test OTA updates with ThreadX

### Testing
- [ ] Unit tests for compatibility layer
- [ ] Integration tests for WiFi
- [ ] Integration tests for Bluetooth
- [ ] Performance benchmarks vs FreeRTOS
- [ ] Long-running stability tests

### Documentation
- [ ] ThreadX configuration guide
- [ ] Migration guide from FreeRTOS
- [ ] API reference documentation
- [ ] Example projects (hello world, WiFi, BLE, etc.)
- [ ] Troubleshooting guide

### CI/CD
- [ ] Add ThreadX builds to CI pipeline
- [ ] Automated hardware testing
- [ ] Performance regression tests

---

## Success Criteria

### Must Have (MVP)
✅ ThreadX selectable in menuconfig for ESP32-C6  
✅ Basic ThreadX kernel boots and runs  
✅ FreeRTOS compatibility layer functional  
✅ WiFi connects and stays connected  
✅ Example project compiles and runs  
✅ Flash/monitor workflow identical to FreeRTOS  

### Should Have (Beta)
✅ Bluetooth LE functional  
✅ Secure boot works unchanged  
✅ OTA updates work  
✅ All standard ESP-IDF examples work (with compat layer)  
✅ Performance within 10% of FreeRTOS  

### Nice to Have (v1.0)
✅ Native ThreadX examples (without compat layer)  
✅ TraceX debugging support  
✅ ThreadX-specific optimizations  
✅ Multi-target support (ESP32-C5, ESP32-H2)  

---

## Timeline Estimate

**Phase 1-2: Core Structure (2 weeks)**
- Kconfig setup
- Component structure
- Basic build system

**Phase 3-4: ThreadX Port (3 weeks)**
- RISC-V port adaptation
- ESP32-C6 specifics
- Boot sequence

**Phase 5: Compatibility Layer (4 weeks)**
- FreeRTOS API shims
- Testing each API category
- Bug fixes

**Phase 6: WiFi/BT Integration (3 weeks)**
- Analyze dependencies
- Implement required shims
- Stability testing

**Phase 7: Security & Build System (2 weeks)**
- Secure boot verification
- idf.py extensions
- Example projects

**Phase 8: Documentation & Testing (2 weeks)**
- Write docs
- Create examples
- CI/CD setup

**Total: ~16 weeks (4 months) for MVP**

---

## Risk Mitigation

### Risk: WiFi/BT stacks have hidden FreeRTOS dependencies
**Mitigation:** 
- Phase 0 investigation reveals these early
- Build comprehensive symbol analysis
- Have escape hatch to patch binary blobs if needed

### Risk: ThreadX RISC-V port doesn't exist or is incomplete
**Mitigation:**
- Verify port existence in Week 1
- If missing, allocate 4-6 weeks for custom port
- Leverage existing RISC-V RTOS ports as reference

### Risk: Performance regression vs FreeRTOS
**Mitigation:**
- Benchmark early and often
- Profile hotspots
- Optimize compatibility layer for common paths

### Risk: Community resistance to change
**Mitigation:**
- Make it opt-in (FreeRTOS remains default)
- Provide clear migration path
- Document benefits (certification, determinism)
- Gather early adopter feedback

---

## Notes

**Why This Design?**
- **Menuconfig integration:** Familiar to ESP-IDF developers
- **Compatibility layer:** Minimize migration effort
- **Component-based:** Clean separation, easy to maintain
- **Opt-in:** Doesn't break existing projects

**Future Expansion:**
- Support other RISC-V ESP32 chips (C5, H2, P4)
- Native ThreadX middleware (FileX, NetX Duo)
- ThreadX-specific power management optimizations
- Safety certification artifacts (for IEC 61508, etc.)

