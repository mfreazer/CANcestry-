/*
 * CANcestry - Bare-metal STM32 / Cortex-M Hardware Abstraction Layer.
 *
 * Normative references:
 *   docs/software/SwRS.md          SW-FR-HAL-003 (non-blocking),
 *                                  SW-FR-BM-002 (bare-metal HAL),
 *                                  SW-FR-BM-003 (interrupt safety & bounded ISR),
 *                                  SW-FR-BM-004 (timestamp monotonicity),
 *                                  SW-FR-BM-007 (sub-50µs latency).
 *   docs/system/SyRS.md            SYS-NF-001 (determinism),
 *                                  SYS-NF-002 (zero heap allocation).
 *
 * Design notes:
 *   - Direct register-level driver for STM32 FDCAN and bxCAN peripherals.
 *   - Strictly non-blocking, interrupt-driven reception. The RX ISR extracts
 *     frames from the hardware FIFO, tags them with a hardware timestamp,
 *     and pushes them directly into the lock-free ISR event queue.
 *   - Main loop does all decoding, UDS parsing, and FSM evaluation.
 *   - Hardware timestamp monotonicity is guaranteed by 64-bit cycle tracking
 *     over the DWT cycle counter.
 *
 * Implements: SW-FR-BM-002, SW-FR-BM-003, SW-FR-BM-004, SW-FR-BM-007.
 */

#ifndef CANCESTRY_PLATFORM_CORTEX_M_HAL_STM32_H
#define CANCESTRY_PLATFORM_CORTEX_M_HAL_STM32_H

#include "cancestry/event/isr_queue.h"
#include "cancestry/hal/hal.h"
#include "cancestry/hal/types.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Maximum physical CAN controllers on the MCU. */
#define CANCESTRY_STM32_MAX_CONTROLLERS ((uint8_t)2u)

/** Hardware FIFO depth for STM32 FDCAN / bxCAN. */
#define CANCESTRY_STM32_FIFO_DEPTH ((uint8_t)3u)

/**
 * Monotonic 64-bit clock state backed by Cortex-M DWT CYCCNT.
 */
typedef struct hal_stm32_clock {
    volatile uint32_t last_raw_cycles;
    volatile uint64_t accumulated_cycles;
    uint32_t cpu_freq_hz;
    uint32_t cycles_per_us;
} hal_stm32_clock_t;

/**
 * Controller-specific context.
 */
typedef struct hal_stm32_controller {
    uint8_t iface_index;
    bool opened;
    bool fd_capable;
    volatile uint32_t *base_addr;
    cancestry_event_isr_queue_t *isr_queue;
    cancestry_hal_rx_ring_t *rx_ring;
    cancestry_hal_tx_ring_t *tx_ring;
    uint32_t rx_interrupt_count;
    uint32_t tx_frame_count;
    uint32_t bus_off_count;
} hal_stm32_controller_t;

/**
 * Bare-metal STM32 HAL context.
 */
typedef struct hal_stm32_context {
    hal_stm32_controller_t controllers[CANCESTRY_STM32_MAX_CONTROLLERS];
    hal_stm32_clock_t clock;
    cancestry_event_isr_queue_t *primary_isr_queue;
} hal_stm32_context_t;

/**
 * Initialize the hardware monotonic clock source (DWT cycle counter).
 *
 * @param clk          Clock structure to initialize.
 * @param cpu_freq_hz  Core clock frequency in Hz (e.g. 160000000 for 160 MHz).
 */
void hal_stm32_clock_init(hal_stm32_clock_t *clk, uint32_t cpu_freq_hz);

/**
 * Read the current 64-bit monotonic timestamp in microseconds.
 * Guaranteed strictly monotonic and free of rollover glitches.
 */
cancestry_time_us_t hal_stm32_get_timestamp_us(hal_stm32_clock_t *clk);

/**
 * CAN RX Interrupt Service Routine (ISR).
 *
 * Executed in NVIC interrupt context when new frames arrive in the hardware
 * FIFO. Bounded execution time O(1): reads frame, captures hardware timestamp,
 * and pushes to the lock-free ISR queue. Absolutely no decoding or UDS logic.
 *
 * @param ctx          STM32 HAL context.
 * @param iface_index  Index of the interrupting controller.
 */
void hal_stm32_can_rx_isr(hal_stm32_context_t *ctx, uint8_t iface_index);

/**
 * Retrieve the singleton Cortex-M bare-metal HAL backend vtable.
 */
const cancestry_hal_backend_t *cancestry_cortex_m_hal_backend(void);

/**
 * Associate a lock-free ISR queue with the Cortex-M HAL context.
 */
void hal_stm32_set_isr_queue(hal_stm32_context_t *ctx,
                             cancestry_event_isr_queue_t *isr_queue);

/* ------------------------------------------------------------------------- */
/* Hardware Injection & Test Interface (for HIL / QEMU conformance tests)     */
/* ------------------------------------------------------------------------- */

/**
 * Inject a frame into the simulated/hardware FIFO to test ISR latency.
 */
void hal_stm32_sim_inject_hardware_frame(hal_stm32_context_t *ctx,
                                         uint8_t iface_index,
                                         const cancestry_hal_frame_t *frame);

#ifdef __cplusplus
}
#endif

#endif /* CANCESTRY_PLATFORM_CORTEX_M_HAL_STM32_H */
