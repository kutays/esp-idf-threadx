# Phase 0.3: WiFi/Bluetooth Stack Analysis

**Status:** Complete
**Date:** 2026-02-17
**Scope:** WiFi/BT binary blob FreeRTOS dependencies, OS adapter analysis, shim requirements

---

## 1. ESP32-C6 WiFi/BT Stack Overview

### 1.1 What's Different on ESP32-C6

| Feature | ESP32 (Classic) | ESP32-C6 |
|---------|----------------|----------|
| Architecture | Xtensa LX6 (dual-core) | RISC-V (single-core) |
| WiFi | 802.11 b/g/n (WiFi 4) | 802.11ax (WiFi 6) |
| Bluetooth | Classic BT + BLE 4.2 | **BLE 5.0 only** (no Classic) |
| Coexistence | Complex dual-core | Simpler single-core |
| Thread Pinning | Tasks pinned to cores | N/A (single core) |
| Binary Blobs | Xtensa compiled | RISC-V compiled |

**Key advantage:** No `xTaskCreatePinnedToCore` complexity. No dual-core synchronization. BLE-only simplifies BT integration.

---

## 2. WiFi Stack Architecture

### 2.1 Component Stack

```
+------------------------------------------+
|        Application Layer                  |
|  (HTTP, MQTT, mDNS, etc.)               |
+------------------------------------------+
|        esp_netif                          |  Network interface abstraction
|        (replaces tcpip_adapter)           |
+------------------------------------------+
|        lwIP TCP/IP Stack                  |  Open source (modified by Espressif)
+------------------------------------------+
|        esp_wifi API                       |  esp_wifi_init(), esp_wifi_start(), etc.
+------------------------------------------+
|        WiFi Supplicant (WPA)             |  Open source (wpa_supplicant)
+------------------------------------------+
|        WiFi Library (BINARY BLOBS)        |
|  libnet80211.a  - 802.11 MAC layer       |
|  libcore.a      - Core WiFi functions    |
|  libpp.a        - Packet processing      |
|  libcoexist.a   - WiFi/BT coexistence    |
+------------------------------------------+
|        PHY Layer (BINARY BLOB)            |
|  libphy.a       - RF calibration         |
|  libphy_init_data.a                      |
+------------------------------------------+
|        Hardware (WiFi MAC/BB/RF)          |
+------------------------------------------+
```

**Reference:**
- [ESP-IDF esp_wifi component](https://github.com/espressif/esp-idf/tree/master/components/esp_wifi)
- [ESP-IDF WiFi API Guide](https://docs.espressif.com/projects/esp-idf/en/latest/esp32c6/api-guides/wifi.html)

### 2.2 Binary Blob Libraries for ESP32-C6

Located in ESP-IDF at: `components/esp_wifi/lib/esp32c6/`

```
libcoexist.a    - WiFi/BT coexistence manager
libcore.a       - Core WiFi functionality
libespnow.a     - ESP-NOW protocol
libmesh.a       - ESP-MESH networking
libnet80211.a   - 802.11 MAC layer processing
libphy.a        - PHY/RF layer
libpp.a         - Packet processing pipeline
libsmartconfig.a - SmartConfig provisioning
libwapi.a       - WAPI security (Chinese standard)
```

**Reference:** [ESP-IDF WiFi libs](https://github.com/espressif/esp-idf/tree/master/components/esp_wifi/lib)

---

## 3. WiFi OS Adapter (Critical Interface)

### 3.1 The wifi_osi_funcs_t Structure

The WiFi binary blobs call OS functions through a **function pointer table**, NOT through direct symbol linking (for most functions). This is defined in:

`components/esp_wifi/include/esp_private/wifi_os_adapter.h`

### 3.2 Required FreeRTOS Functions (via OS Adapter)

| Category | Function Pointers | ThreadX Mapping |
|----------|-------------------|-----------------|
| **Semaphores** | `semphr_create`, `semphr_delete`, `semphr_take`, `semphr_give` | `tx_semaphore_*` |
| **Mutexes** | `mutex_create`, `mutex_delete`, `mutex_lock`, `mutex_unlock` | `tx_mutex_*` |
| **Queues** | `queue_create`, `queue_delete`, `queue_send`, `queue_recv` | `tx_queue_*` (with size adaptation) |
| **Tasks** | `task_create`, `task_delete`, `task_delay`, `task_ms_to_tick` | `tx_thread_*` |
| **Timers** | `timer_arm`, `timer_disarm`, `timer_done`, `timer_setfn` | `tx_timer_*` |
| **Events** | `event_group_create`, `event_group_delete`, `event_group_set_bits`, `event_group_wait_bits` | `tx_event_flags_*` |
| **Memory** | `malloc`, `free`, `calloc`, `zalloc` | `tx_byte_allocate` / `tx_byte_release` |
| **Misc** | `get_time`, `random`, `log_write`, `log_timestamp` | Custom implementations |
| **ISR** | `set_isr`, `ints_on`, `ints_off` | ESP-IDF interrupt APIs |

### 3.3 WiFi OS Adapter Implementation Pattern

```c
// Current FreeRTOS implementation (simplified from esp_wifi/src/wifi_init.c)
static wifi_osi_funcs_t g_wifi_osi_funcs = {
    ._version = ESP_WIFI_OS_ADAPTER_VERSION,
    ._semphr_create = wifi_semphr_create_wrapper,
    ._semphr_delete = wifi_semphr_delete_wrapper,
    ._semphr_take = wifi_semphr_take_wrapper,
    ._semphr_give = wifi_semphr_give_wrapper,
    ._mutex_create = wifi_mutex_create_wrapper,
    // ... etc
};

// For ThreadX, we replace these wrapper functions:
static void* IRAM_ATTR wifi_semphr_create_wrapper(uint32_t max, uint32_t init)
{
    // Instead of xSemaphoreCreateCounting(max, init):
    TX_SEMAPHORE *sem = tx_byte_allocate(...);
    tx_semaphore_create(sem, "wifi_sem", init);
    return (void*)sem;
}
```

**Reference:**
- [wifi_os_adapter.h](https://github.com/espressif/esp-idf/blob/master/components/esp_wifi/include/esp_private/wifi_os_adapter.h)
- [WiFi init source](https://github.com/espressif/esp-idf/blob/master/components/esp_wifi/src/wifi_init.c)

---

## 4. Directly Linked FreeRTOS Symbols

### 4.1 Not Everything Goes Through the OS Adapter

Some FreeRTOS functions are called **directly** by the binary blobs (linked at compile time, not through function pointers). These must be provided as actual function symbols.

### 4.2 How to Identify Them

Use `nm` or `readelf` on the WiFi blob libraries:

```bash
# List undefined symbols (FreeRTOS functions the blob needs)
riscv32-esp-elf-nm -u components/esp_wifi/lib/esp32c6/libnet80211.a | grep -i freertos
riscv32-esp-elf-nm -u components/esp_wifi/lib/esp32c6/libnet80211.a | grep -E "^\\s+U\\s+"

# Check all WiFi libraries
for lib in components/esp_wifi/lib/esp32c6/*.a; do
    echo "=== $lib ==="
    riscv32-esp-elf-nm -u "$lib" 2>/dev/null | grep -iE "task|queue|semphr|mutex|timer|event|port|critical"
done
```

### 4.3 Commonly Directly-Linked FreeRTOS Symbols

Based on analysis of similar ESP32 chips, expect these direct dependencies:

| Symbol | Category | Notes |
|--------|----------|-------|
| `xTaskCreate` | Task | May bypass OS adapter |
| `vTaskDelete` | Task | May bypass OS adapter |
| `vTaskDelay` | Task | Common in timing loops |
| `xTaskGetTickCount` | Timing | For timestamping |
| `portENTER_CRITICAL` | Critical section | Interrupt disable macro |
| `portEXIT_CRITICAL` | Critical section | Interrupt restore macro |
| `pvPortMalloc` | Memory | Heap allocation |
| `vPortFree` | Memory | Heap free |
| `xPortGetFreeHeapSize` | Memory | Heap stats |
| `vPortEnterCritical` | Critical section | Nesting-aware version |
| `vPortExitCritical` | Critical section | Nesting-aware version |

**Action item:** Run the `nm` commands on actual ESP32-C6 libraries to get the definitive list.

---

## 5. BLE Stack Architecture

### 5.1 ESP32-C6 BLE Stack

```
+------------------------------------------+
|     BLE Application                       |
|  (GATT Server/Client, GAP)               |
+------------------------------------------+
|     NimBLE Host Stack                     |  <-- OPEN SOURCE (Apache)
|  - L2CAP, ATT, GATT, SMP, GAP           |
|  - HCI host-side processing             |
+------------------------------------------+
|     Virtual HCI (VHCI) Interface         |  <-- API boundary
+------------------------------------------+
|     BLE Controller (BINARY BLOB)          |
|  - Link Layer                            |
|  - HCI controller-side                   |
|  - Baseband processing                   |
+------------------------------------------+
|     PHY / RF (shared with WiFi)          |
+------------------------------------------+
```

### 5.2 BLE OS Dependencies

**NimBLE Host (Open Source):**
- Uses `os_task`, `os_sem`, `os_mutex`, `os_mempool` abstractions
- ESP-IDF maps these to FreeRTOS via `nimble/porting/nimble/include/os/os.h`
- **Can be recompiled against ThreadX** directly
- Source: `components/bt/host/nimble/`

**BLE Controller (Binary Blob):**
- Uses OS function pointer table similar to WiFi (`bt_osi_funcs_t`)
- Located in: `components/bt/controller/esp32c6/`
- Dependencies: tasks, semaphores, queues, timers

### 5.3 BLE Controller OS Functions

```c
// BT controller OS function table (simplified)
typedef struct {
    osi_sem_new_fn      sem_new;
    osi_sem_free_fn     sem_free;
    osi_sem_take_fn     sem_take;
    osi_sem_give_fn     sem_give;
    osi_mutex_new_fn    mutex_new;
    osi_mutex_free_fn   mutex_free;
    osi_mutex_lock_fn   mutex_lock;
    osi_mutex_unlock_fn mutex_unlock;
    osi_task_new_fn     task_new;
    osi_task_delete_fn  task_delete;
    // ... queue, timer, memory functions
} bt_osi_funcs_t;
```

**Reference:**
- [ESP-IDF BT controller component](https://github.com/espressif/esp-idf/tree/master/components/bt/controller)
- [NimBLE port for ESP-IDF](https://github.com/espressif/esp-idf/tree/master/components/bt/host/nimble)
- [Apache NimBLE upstream](https://github.com/apache/mynewt-nimble)

---

## 6. lwIP TCP/IP Stack Dependencies

### 6.1 lwIP FreeRTOS Coupling

ESP-IDF's lwIP port uses FreeRTOS for:

| Feature | FreeRTOS Usage | ThreadX Equivalent |
|---------|---------------|-------------------|
| TCP/IP thread | `sys_thread_new()` -> `xTaskCreate` | `tx_thread_create` |
| Mailboxes | `sys_mbox_new()` -> `xQueueCreate` | `tx_queue_create` |
| Semaphores | `sys_sem_new()` -> `xSemaphoreCreate` | `tx_semaphore_create` |
| Mutexes | `sys_mutex_new()` -> `xSemaphoreCreateMutex` | `tx_mutex_create` |
| Thread-safe API | `LOCK_TCPIP_CORE()` | Critical section |

### 6.2 lwIP Port Location

The FreeRTOS-specific port is at:
- `components/lwip/port/freertos/`
- `components/lwip/port/freertos/sys_arch.c` -- **This file needs a ThreadX version**

The `sys_arch.c` file implements lwIP's `sys_arch` API using FreeRTOS. We need a `sys_arch_threadx.c` replacement.

**Reference:**
- [ESP-IDF lwIP component](https://github.com/espressif/esp-idf/tree/master/components/lwip)
- [lwIP sys_arch documentation](https://www.nongnu.org/lwip/2_1_x/group__sys__os.html)

---

## 7. esp_event System Dependencies

### 7.1 Event Loop Architecture

ESP-IDF's event system (`esp_event`) is used by WiFi/BT for event callbacks:

```c
// WiFi events flow:
esp_wifi_start()
    -> WiFi blob generates event
    -> Posts to esp_event loop (runs in FreeRTOS task)
    -> User handler called from event task
```

### 7.2 FreeRTOS Dependencies in esp_event

- Event loop runs in a **FreeRTOS task** (`esp_event_loop_run_task`)
- Uses **queue** for event posting
- Uses **mutex** for handler registration

### 7.3 ThreadX Adaptation

The `esp_event` component is **open source** and can be adapted to use ThreadX:
- Replace task creation with `tx_thread_create`
- Replace queue with `tx_queue_create`
- Replace mutex with `tx_mutex_create`

**Reference:** [esp_event component](https://github.com/espressif/esp-idf/tree/master/components/esp_event)

---

## 8. WiFi/BT Initialization Sequence

### 8.1 WiFi Initialization Order

```
1. nvs_flash_init()                    -- NVS for WiFi calibration data
2. esp_netif_init()                    -- Network interface
3. esp_event_loop_create_default()     -- Event loop (needs task)
4. esp_netif_create_default_wifi_sta() -- Create station interface
5. esp_wifi_init(&cfg)                 -- Initialize WiFi driver
   |-- Registers OS adapter functions
   |-- Creates WiFi task(s)
   |-- Allocates queues/semaphores
6. esp_wifi_set_mode(WIFI_MODE_STA)    -- Set station mode
7. esp_wifi_set_config(...)            -- Set SSID/password
8. esp_wifi_start()                    -- Start WiFi
   |-- RF calibration
   |-- MAC initialization
   |-- Scan for AP
9. esp_wifi_connect()                  -- Connect to AP
   |-- Association
   |-- DHCP (via lwIP)
```

### 8.2 BLE Initialization Order

```
1. nvs_flash_init()                    -- NVS for BT bonding data
2. esp_bt_controller_init(&cfg)        -- Init BT controller
   |-- Registers BT OS functions
   |-- Creates controller task
3. esp_bt_controller_enable(ESP_BT_MODE_BLE)
4. nimble_port_init()                  -- Init NimBLE host
   |-- Creates NimBLE host task
5. ble_hs_cfg = { ... }               -- Configure host stack
6. nimble_port_freertos_init(host_task) -- Start host task
```

### 8.3 WiFi + BLE Coexistence

When both WiFi and BLE are active:
- `libcoexist.a` manages RF time-sharing
- Requires precise timing synchronization
- Uses semaphores/events for coordination between WiFi and BT tasks
- **Potential risk:** Timing-sensitive coexistence may break with different RTOS scheduling

---

## 9. Deep Sleep / Wake Considerations

### 9.1 WiFi State During Sleep

- **Modem sleep:** WiFi maintains connection, modem powers down between beacons
- **Light sleep:** CPU sleeps, WiFi can wake on beacon (if configured)
- **Deep sleep:** WiFi state lost, full re-initialization on wake

### 9.2 FreeRTOS Dependencies in Sleep

- `esp_pm` (power management) uses FreeRTOS tick for timing
- WiFi power save callbacks run from FreeRTOS task context
- Light sleep uses `vTaskDelay` internally for idle detection

---

## 10. Dependency Matrix

### 10.1 WiFi -> RTOS Function Dependencies

| RTOS Function | Used By | Priority | Difficulty |
|--------------|---------|----------|------------|
| `xTaskCreate` | WiFi task, event task | Critical | Low |
| `vTaskDelete` | WiFi cleanup | Medium | Low |
| `vTaskDelay` | WiFi timing | Critical | Low |
| `xSemaphoreCreateBinary` | WiFi sync | Critical | Low |
| `xSemaphoreCreateCounting` | WiFi flow control | Critical | Low |
| `xSemaphoreTake/Give` | All WiFi sync | Critical | Low |
| `xSemaphoreCreateMutex` | WiFi thread safety | Critical | Low |
| `xQueueCreate` | WiFi events, packets | Critical | Medium (size mapping) |
| `xQueueSend/Receive` | WiFi data flow | Critical | Medium |
| `xEventGroupCreate` | WiFi state events | High | Low |
| `xEventGroupSetBits/WaitBits` | WiFi event signaling | High | Low |
| `xTimerCreate` | WiFi timeouts | High | Low |
| `pvPortMalloc/vPortFree` | WiFi memory | Critical | Low |
| `portENTER/EXIT_CRITICAL` | WiFi ISR safety | Critical | Low |
| `xTaskGetTickCount` | WiFi timestamping | High | Low |
| `vTaskNotifyGiveFromISR` | WiFi ISR -> task | Medium | Medium |

### 10.2 BLE -> RTOS Function Dependencies

Similar to WiFi, plus:
- NimBLE OS abstraction layer (open source, can be rewritten)
- BLE controller OS functions (function pointer table)
- HCI transport semaphores

---

## 11. Feasibility Assessment

### 11.1 WiFi Integration

| Aspect | Assessment |
|--------|-----------|
| OS adapter pattern | **Favorable** -- function pointers, not direct linking (mostly) |
| Directly linked symbols | **Manageable** -- provide global shim functions |
| lwIP adaptation | **Medium effort** -- need ThreadX sys_arch.c |
| esp_event adaptation | **Low effort** -- open source, straightforward |
| Coexistence timing | **Risk** -- needs testing |

### 11.2 BLE Integration

| Aspect | Assessment |
|--------|-----------|
| NimBLE host (open source) | **Favorable** -- can recompile against ThreadX |
| BLE controller (blob) | **Favorable** -- uses OS function table |
| VHCI interface | **Low risk** -- clean API boundary |

### 11.3 Overall: **Feasible with known risks**

The WiFi/BT integration is feasible. The OS adapter pattern is the key enabler. Main risks are:
1. Directly-linked FreeRTOS symbols in blobs (need `nm` analysis)
2. Timing-sensitive coexistence behavior
3. lwIP port requires a new sys_arch implementation

---

## 12. Recommended Next Steps

1. **Run `nm -u` on all ESP32-C6 WiFi/BT libraries** to get definitive FreeRTOS symbol list
2. **Study Zephyr's WiFi shim for ESP32-C6** -- closest reference
3. **Study esp-wifi Rust crate's FreeRTOS shim** -- simpler reference
4. **Start with WiFi-only** (no BLE) for initial proof of concept
5. **Implement OS adapter functions first** (function pointer table)
6. **Then handle directly-linked symbols** (global shim functions)

---

## 13. References

### ESP-IDF WiFi/BT Components

1. [esp_wifi component](https://github.com/espressif/esp-idf/tree/master/components/esp_wifi)
2. [wifi_os_adapter.h](https://github.com/espressif/esp-idf/blob/master/components/esp_wifi/include/esp_private/wifi_os_adapter.h)
3. [ESP-IDF WiFi API Guide](https://docs.espressif.com/projects/esp-idf/en/latest/esp32c6/api-guides/wifi.html)
4. [ESP-IDF WiFi API Reference](https://docs.espressif.com/projects/esp-idf/en/latest/esp32c6/api-reference/network/esp_wifi.html)
5. [BT controller component](https://github.com/espressif/esp-idf/tree/master/components/bt/controller)
6. [NimBLE ESP-IDF port](https://github.com/espressif/esp-idf/tree/master/components/bt/host/nimble)
7. [esp_event component](https://github.com/espressif/esp-idf/tree/master/components/esp_event)
8. [lwIP component](https://github.com/espressif/esp-idf/tree/master/components/lwip)
9. [ESP-IDF WiFi binary libs](https://github.com/espressif/esp-idf/tree/master/components/esp_wifi/lib)

### Alternative RTOS WiFi Integration

10. [Zephyr hal_espressif WiFi shim](https://github.com/zephyrproject-rtos/hal_espressif)
11. [esp-wifi Rust crate (FreeRTOS shim)](https://github.com/esp-rs/esp-hal/tree/main/esp-wifi)
12. [NuttX ESP32-C6 WiFi](https://github.com/apache/nuttx/tree/master/arch/risc-v/src/esp32c6)

### lwIP

13. [lwIP sys_arch documentation](https://www.nongnu.org/lwip/2_1_x/group__sys__os.html)
14. [lwIP FreeRTOS port](https://github.com/espressif/esp-idf/tree/master/components/lwip/port/freertos)

### NimBLE

15. [Apache NimBLE upstream](https://github.com/apache/mynewt-nimble)
16. [NimBLE porting guide](https://mynewt.apache.org/latest/network/ble_setup/ble_setup.html)

### ESP32-C6 Specifics

17. [ESP32-C6 Datasheet](https://www.espressif.com/sites/default/files/documentation/esp32-c6_datasheet_en.pdf)
18. [ESP32-C6 Technical Reference Manual](https://www.espressif.com/sites/default/files/documentation/esp32-c6_technical_reference_manual_en.pdf)

---

*Action required: Run `nm -u` on actual ESP32-C6 binary blobs to finalize the FreeRTOS symbol dependency list. The analysis above is based on pattern analysis and prior ESP32 knowledge.*
