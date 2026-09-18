# CANcestry Bare-Metal Platform & Hard Real-Time HAL (`platform/cortex_m/`)

Phase 11 (issue #28) provides the bare-metal ARM Cortex-M target port for CANcestry, replacing OS-dependent SocketCAN with direct register-level hardware peripheral drivers (FDCAN/bxCAN) and hardware-enforced safety mechanisms.

## Architecture

```
                  +-----------------------------------+
                  |          CAN Hardware             |
                  |     (FDCAN / bxCAN FIFO)          |
                  +-----------------+-----------------+
                                    | (Hardware Interrupt)
                                    v
                  +-----------------------------------+
                  |      hal_stm32_can_rx_isr()       |
                  | - Capture HW timestamp (DWT CYCCNT)|
                  | - Drain HW FIFO (bounded O(1))    |
                  | - Push to lock-free ISR queue     |
                  +-----------------+-----------------+
                                    |
                                    v (Lock-free SPSC hand-off)
+-------------------------------------------------------------+
| Main Loop (main() -> while(1))                              |
|                                                             |
|  1. Drain ISR queue into core event queue                  |
|  2. Process events through FSM runtime                      |
|  3. Tick FSM & feed Independent Watchdog (IWDG)             |
|                                                             |
| If loop hangs -> IWDG resets MCU -> Hardware Pins Safe State|
+-------------------------------------------------------------+
```

## Deliverables

### 1. Bare-Metal HAL (`hal_stm32.c`, `hal_stm32.h`)
- Register-level driver for STM32 FDCAN and bxCAN peripherals using hardware FIFOs.
- **Strictly non-blocking, interrupt-driven RX**: the ISR captures hardware timestamps via DWT `CYCCNT` and pushes frames into the lock-free ISR event queue (`core/event/include/cancestry/event/isr_queue.h`).
- **Thin ISR contract**: absolutely no decoding, UDS parsing, or FSM evaluation inside interrupt context.
- **Timestamp Monotonicity**: 64-bit cycle tracking with modulo 32-bit delta arithmetic guarantees monotonic timestamps across hardware counter rollovers for over 3,000 years.

### 2. Independent Watchdog (IWDG) & Fail-Safe Pin Controller (`watchdog.c`, `watchdog.h`)
- Configures and services the hardware IWDG clocked by the internal LSI oscillator.
- Main loop feeds the watchdog at each `fsm_tick`.
- If execution hangs or a deadline is missed, the IWDG resets the processor.
- **Hardware Safe State on Reset**: upon power-on or watchdog reset, GPIO pins for CAN TX and contactors are immediately placed into a safe "0 Torque / Contactor Open" state. Normal CAN transmission remains unauthorized until the FSM explicitly verifies health and grants authorization.
- Emits a canonical fail-safe CAN broadcast frame (`ID = 0x100`, Torque = 0, Contactors = Open) upon reset recovery.

### 3. Linker-Level Zero-Allocation Enforcement (`cancestry_baremetal.ld`, `alloc_stubs.c`)
- Custom GNU ld linker script placing core data structures in dedicated SRAM sections:
  - `.cancestry_core`: FSM instances, codec tables, event queues
  - `.cancestry_rings`: lock-free HAL RX and TX rings
  - `.cancestry_ram`: platform variables
- Strips `malloc`, `free`, `realloc`, `calloc`, `_sbrk`, and `sbrk` in the `/DISCARD/` section and asserts `!DEFINED(malloc)`.
- Any attempt to reference dynamic memory allocation produces an immediate undefined reference error at link time.
- Custom tripwires (`cancestry_zero_alloc_tripwire()`) trigger a HardFault or breakpoint if an allocation is ever attempted at runtime.

### 4. Lock-Free ISR-to-Main-Loop Queue (`core/event/include/cancestry/event/isr_queue.h`)
- Wait-free, lock-free Single-Producer Single-Consumer (SPSC) ring buffer.
- Producer (ISR) push completes in O(1) bounded time (< 1 µs), with drop-newest overflow policy.
- Consumer (main thread) drains pending events directly into the prioritized min-heap core event queue.

### 5. Conformance Suite (`tests/conformance/baremetal/`)
- `test_baremetal_latency.c`: proves cycle-accurate sub-50µs latency from CAN RX interrupt to FSM event processing (`BM-LAT-001`, `BM-LAT-002`), timestamp monotonicity across rollovers (`BM-LAT-003`), and bounded ISR execution (`BM-LAT-004`).
- `test_watchdog_fail_safe.c`: proves regular feeding keeps the system running (`BM-SAFE-001`), induced hangs trigger IWDG reset and immediate safe state (`BM-SAFE-002`), safe-state CAN broadcast is emitted (`BM-SAFE-003`), and FSM authorization gate prevents premature transmission (`BM-SAFE-004`).
- `test_baremetal_zero_alloc.c`: proves linker script rejects allocation symbols (`BM-ALLOC-001`).

## Compilation

Compiles cleanly with `arm-none-eabi-gcc`:
```sh
arm-none-eabi-gcc -O2 -Wall -Wextra -Werror -Wpedantic -mcpu=cortex-m4 -mthumb \
    -Iplatform/cortex_m -Icore/hal/include -Icore/event/include -c platform/cortex_m/hal_stm32.c
```
