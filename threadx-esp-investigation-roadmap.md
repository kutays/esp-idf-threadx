# ThreadX on ESP32-C6: Investigation & Roadmap

**Project Goal:** Replace FreeRTOS in ESP-IDF with ThreadX while maintaining WiFi/Bluetooth functionality via ESP-HAL

**Target Hardware:** ESP32-C6 (RISC-V, Single Core)  
**Solo Developer:** [Your Name]  
**Status:** Investigation Phase  
**Last Updated:** 2026-02-16

## 🎯 Why ESP32-C6 is Perfect for This

**Strategic Advantages:**
- ✅ **RISC-V architecture** - Standard, well-documented ISA (vs proprietary Xtensa)
- ✅ **Single core** - Avoids Espressif's custom dual-core FreeRTOS implementation
- ✅ **Cleaner ThreadX port** - RISC-V is better supported in Azure RTOS ecosystem
- ✅ **Simpler interrupt model** - Standard RISC-V PLIC (Platform-Level Interrupt Controller)
- ✅ **Modern chip** - Better ESP-HAL support likely
- ✅ **WiFi 6 + BLE 5** - Still need to handle closed-source stacks, but simpler context

---

## Phase 0: Investigation & Discovery (CURRENT)

### 0.1 ESP-HAL Deep Dive

**Questions to Answer:**
- [ ] What is ESP-HAL's architecture? (Hardware Abstraction Layer details)
- [ ] Which ESP-IDF components does ESP-HAL abstract?
- [ ] Does ESP-HAL support WiFi stack integration without FreeRTOS?
- [ ] Does ESP-HAL support Bluetooth stack integration without FreeRTOS?
- [ ] What are ESP-HAL's dependencies? (Does it assume FreeRTOS?)
- [ ] Is there existing documentation for RTOS-agnostic usage?
- [ ] Are there any examples of alternative RTOS usage with ESP-HAL?

**Resources to Check:**
- [ ] ESP-HAL official documentation
- [ ] ESP-HAL GitHub repository and issue tracker
- [ ] ESP-IDF architecture documentation (component dependencies)
- [ ] Espressif forums/Discord for RTOS discussions
- [ ] Search for "ESP-HAL ThreadX", "ESP-HAL Zephyr", or similar migrations

**Expected Output:** Technical brief (2-3 pages) on ESP-HAL feasibility

---

### 0.2 ThreadX Integration Requirements

**Questions to Answer:**
- [ ] What is ThreadX's initialization flow?
- [ ] Does ThreadX have an existing RISC-V 32-bit port? (Check Azure RTOS repo)
- [ ] How does ThreadX handle RISC-V interrupts and exceptions?
- [ ] What ThreadX APIs map to FreeRTOS APIs? (task creation, mutexes, queues, etc.)
- [ ] What is ThreadX's memory model? (heap, stack management)
- [ ] Are there existing RISC-V MCU examples? (SiFive, GigaDevice, Nuclei)
- [ ] Does ThreadX support RISC-V vector extensions (if C6 has them)?

**RISC-V Specifics to Investigate:**
- [ ] Machine mode vs Supervisor mode - which does ThreadX use?
- [ ] CSR (Control and Status Register) usage
- [ ] PLIC (Platform-Level Interrupt Controller) integration
- [ ] Timer interrupt setup (mtime/mtimecmp)
- [ ] Context switch implementation for RISC-V

**Resources to Check:**
- [ ] ThreadX official documentation (Azure RTOS)
- [ ] Azure RTOS ThreadX GitHub - check ports/risc-v directory
- [ ] ESP32-C6 Technical Reference Manual (interrupt controller, memory layout)
- [ ] FreeRTOS → ThreadX migration guides
- [ ] Existing RISC-V RTOS ports for comparison (Zephyr, RT-Thread)

**Expected Output:** ThreadX feasibility assessment + API mapping table

---

### 0.3 WiFi/Bluetooth Stack Analysis

**Questions to Answer:**
- [ ] What APIs do ESP32 WiFi/BT stacks expose?
- [ ] Which APIs have FreeRTOS hard dependencies? (semaphores, tasks, queues)
- [ ] Can these dependencies be shimmed/abstracted?
- [ ] What is the threading model of WiFi/BT stacks? (event callbacks, task priorities)
- [ ] Are there initialization order dependencies?
- [ ] What happens during deep sleep/wake with WiFi/BT active?

**Closed-Source Challenge:**
Since WiFi/BT are binary blobs, you'll need to:
- [ ] Reverse-engineer required RTOS APIs through symbols/headers
- [ ] Identify which FreeRTOS functions are actually called
- [ ] Document calling conventions and timing requirements

**Tools:**
- `nm` or `objdump` on WiFi/BT libraries to see symbol dependencies
- ESP-IDF component headers (esp_wifi.h, esp_bt.h)
- Runtime tracing if possible

**Expected Output:** Dependency matrix of WiFi/BT → RTOS functions

---

### 0.4 ESP32-C6 Specific Considerations

**Hardware Architecture:**
- [ ] Review ESP32-C6 memory map (SRAM, ROM, Flash layout)
- [ ] Understand RISC-V PLIC interrupt priorities vs FreeRTOS priority model
- [ ] Check if C6 has any custom RISC-V extensions beyond RV32IMAC
- [ ] Verify clock tree and PLL configuration requirements
- [ ] Document low-power modes (light sleep, deep sleep) on RISC-V

**ESP-IDF C6 Components:**
- [ ] Which ESP-IDF components are C6-specific vs reused from ESP32?
- [ ] Is there existing ESP-HAL support for C6? (Check esp-hal-rs or similar)
- [ ] Are WiFi 6 and BLE 5.0 stacks different from ESP32 classic?
- [ ] Any SMP (symmetric multiprocessing) remnants that need removal?

**Toolchain:**
- [ ] Confirm RISC-V GCC toolchain compatibility with ThreadX
- [ ] Check if ESP-IDF's RISC-V toolchain has any Espressif patches
- [ ] Verify debugger support (OpenOCD, JTAG) for C6

**Expected Output:** ESP32-C6 technical brief highlighting any gotchas

---

## Phase 1: Proof of Concept (After Investigation)

### 1.1 Minimal ThreadX Boot on ESP32-C6
- [ ] Get ThreadX RISC-V port compiled for RV32IMAC architecture
- [ ] Set up basic linker script for ESP32-C6 memory layout
- [ ] Initialize RISC-V machine mode CSRs (mstatus, mie, mtvec)
- [ ] Configure PLIC for basic interrupt routing
- [ ] Set up mtime/mtimecmp for system tick
- [ ] Create simple ThreadX task that blinks LED (GPIO)
- [ ] Verify interrupt handling with timer ISR
- [ ] Confirm UART output works (printf debugging)

**Success Criteria:** ThreadX scheduler running, basic I/O functional, stable system tick

---

### 1.2 RTOS Abstraction Layer
- [ ] Design thin abstraction layer between ThreadX and ESP-HAL
- [ ] Implement critical FreeRTOS API shims:
  - Task creation/deletion
  - Mutexes/semaphores
  - Queues
  - Timers
  - Event groups
- [ ] Map FreeRTOS priorities to ThreadX priorities

**Success Criteria:** Abstraction layer compiles and passes unit tests

---

### 1.3 ESP-HAL Integration
- [ ] Integrate ESP-HAL with ThreadX environment
- [ ] Initialize GPIO, UART, SPI via ESP-HAL
- [ ] Verify timer and interrupt handling through HAL
- [ ] Test peripheral drivers (I2C, ADC, etc.)

**Success Criteria:** ESP-HAL peripherals working with ThreadX

---

## Phase 2: WiFi Stack Integration

### 2.1 WiFi Initialization
- [ ] Port WiFi initialization code to use RTOS abstraction layer
- [ ] Verify nvs_flash, esp_netif, and event loops work
- [ ] Initialize WiFi driver without crashing

### 2.2 WiFi Connection
- [ ] Implement station mode connection
- [ ] Test DHCP acquisition
- [ ] Verify TCP/IP stack compatibility
- [ ] Test basic HTTP request

**Success Criteria:** ESP32 connects to WiFi and fetches webpage

---

## Phase 3: Bluetooth Stack Integration

### 3.1 BT Classic / BLE Init
- [ ] Port Bluetooth controller initialization
- [ ] Verify HCI layer compatibility
- [ ] Test basic BLE advertising

### 3.2 BT Functionality Test
- [ ] Test GATT server/client
- [ ] Verify BLE scanning
- [ ] Test concurrent WiFi + BT operation

**Success Criteria:** BLE device discoverable and functional

---

## Phase 4: System Hardening

### 4.1 Power Management
- [ ] Implement light sleep with ThreadX
- [ ] Test deep sleep and wake stubs
- [ ] Verify WiFi/BT state preservation across sleep

### 4.2 Stress Testing
- [ ] Long-running stability tests (24hr+)
- [ ] Memory leak detection
- [ ] Interrupt latency benchmarks
- [ ] Task switching performance vs FreeRTOS

### 4.3 Build System
- [ ] Create CMake integration for ThreadX
- [ ] Package as ESP-IDF component
- [ ] Document build process

---

## Risk Assessment

### HIGH RISK ⚠️
1. **WiFi/BT closed-source dependencies:** May have undocumented FreeRTOS assumptions
   - *Mitigation: ESP-C6 stacks might be cleaner than dual-core ESP32 versions*
2. **ESP-HAL maturity on C6:** May not be as mature as for older ESP32 chips
   - *Mitigation: C6 is newer, might have better HAL design from the start*

### MEDIUM RISK ⚡
1. **ThreadX RISC-V port gaps:** Port may exist but need ESP32-C6 specific adaptations
2. **Timing-sensitive operations:** WiFi calibration, BT timing might break
3. **Debugging difficulty:** Less tooling support than FreeRTOS
4. **Memory layout conflicts:** ThreadX vs FreeRTOS memory models differ

### LOW RISK ✅ (Thanks to ESP32-C6 choice!)
1. ~~**Dual-core synchronization**~~ - Not applicable, single core!
2. ~~**Xtensa proprietary architecture**~~ - RISC-V is standard and well-supported
3. ~~**Complex interrupt controller**~~ - RISC-V PLIC is straightforward

### MITIGATION STRATEGIES
- **Build escape hatches:** Keep FreeRTOS as fallback during development
- **Incremental testing:** Validate each layer before moving to next
- **Document everything:** You'll forget details in 2 weeks
- **Automated testing:** Catch regressions early
- **Leverage RISC-V ecosystem:** Look at other RISC-V MCU ThreadX ports for guidance

---

## Success Criteria (Overall Project)

- [ ] ThreadX scheduler running stably on ESP32
- [ ] WiFi connects and maintains connection
- [ ] Bluetooth LE advertising and connections work
- [ ] All functionality works with ESP-HAL (no direct register access)
- [ ] Performance within 20% of FreeRTOS baseline
- [ ] Power consumption within 10% of FreeRTOS baseline
- [ ] Build system integrated with ESP-IDF
- [ ] Documentation for others to replicate

---

## Investigation Phase Deliverables (Next Steps)

**This Week:**
1. ESP-HAL technical assessment document
2. ThreadX feasibility report
3. WiFi/BT dependency analysis

**Decision Point:** After investigation, GO/NO-GO decision on full implementation

---

## Questions for Claude Code / Research

When you're ready to dig in, here are good prompts for Claude Code:

```bash
# ESP-HAL investigation
claude
> Analyze ESP-HAL support for ESP32-C6. What components does it abstract?
> Compare ESP-HAL for C6 vs original ESP32. What's different?

# ThreadX RISC-V porting
> Does Azure RTOS ThreadX have a RISC-V port? Analyze its implementation.
> What would adapting ThreadX RV32 port for ESP32-C6 require?
> Create a FreeRTOS to ThreadX API mapping table for common operations

# ESP32-C6 architecture
> Analyze ESP32-C6 interrupt controller (PLIC) configuration
> Compare ESP32-C6 memory layout to ThreadX typical RISC-V requirements
> What are the differences between ESP32 (Xtensa) and C6 (RISC-V) WiFi stacks?

# Build system
> How can I integrate ThreadX as an ESP-IDF component for C6?
> Create a CMakeLists.txt for ThreadX RISC-V + ESP-HAL on C6
> What linker script modifications are needed for ThreadX on C6?
```

---

## Notes & Findings

[Use this section to capture investigation discoveries as you go]

**2026-02-16:**
- Started investigation phase
- **Key decision: Targeting ESP32-C6 (RISC-V, single-core)**
  - Avoids Espressif's custom dual-core FreeRTOS implementation
  - RISC-V is much better supported in ThreadX ecosystem than Xtensa
  - Standard PLIC interrupt controller vs proprietary Xtensa interrupts
  - Cleaner architecture overall
- Key concern: WiFi/BT binary blob dependencies on FreeRTOS
- ESP-HAL seems promising but need to verify C6 support level
- ThreadX likely has existing RISC-V port - need to verify RV32IMAC compatibility

**Next Actions:**
1. Check Azure RTOS GitHub for RISC-V port status
2. Find ESP32-C6 Technical Reference Manual
3. Search for ESP-HAL C6 examples or documentation
4. Look for any existing alternative RTOS ports on C6 (Zephyr, RT-Thread)

