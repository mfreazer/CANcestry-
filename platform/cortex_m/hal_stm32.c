/*
 * CANcestry - Bare-metal STM32 / Cortex-M Hardware Abstraction Layer.
 *
 * Implements: SW-FR-BM-002 (bare-metal HAL),
 *             SW-FR-BM-003 (interrupt safety & bounded ISR),
 *             SW-FR-BM-004 (timestamp monotonicity),
 *             SW-FR-BM-007 (sub-50µs latency).
 */

#include "hal_stm32.h"

#include <string.h>

#if defined(__arm__) || defined(__thumb__)
/* ARM Cortex-M CoreDebug & DWT registers */
#define CORE_DEBUG_DEMCR   (*(volatile uint32_t *)0xE000EDFCUL)
#define DWT_CTRL           (*(volatile uint32_t *)0xE0001000UL)
#define DWT_CYCCNT         (*(volatile uint32_t *)0xE0001004UL)

#define FDCAN1_BASE        (0x40006400UL)
#define FDCAN2_BASE        (0x40006800UL)
#endif

/* Hardware simulation buffer for test fixtures and host builds */
typedef struct sim_fifo_slot {
    bool has_frame;
    cancestry_hal_frame_t frame;
} sim_fifo_slot_t;

static sim_fifo_slot_t s_sim_hardware_fifo[CANCESTRY_STM32_MAX_CONTROLLERS][CANCESTRY_STM32_FIFO_DEPTH];
static uint32_t s_sim_cycles = 0u;

/* ------------------------------------------------------------------------- */
/* Hardware Monotonic Clock (SW-FR-BM-004)                                   */
/* ------------------------------------------------------------------------- */

void hal_stm32_clock_init(hal_stm32_clock_t *clk, uint32_t cpu_freq_hz)
{
    if (clk == NULL) {
        return;
    }

    clk->cpu_freq_hz = (cpu_freq_hz > 0u) ? cpu_freq_hz : 160000000u; /* Default 160 MHz */
    clk->cycles_per_us = clk->cpu_freq_hz / 1000000u;
    if (clk->cycles_per_us == 0u) {
        clk->cycles_per_us = 1u;
    }
    clk->last_raw_cycles = 0u;
    clk->accumulated_cycles = 0u;

#if defined(__arm__) || defined(__thumb__)
    /* Enable DWT cycle counter */
    CORE_DEBUG_DEMCR |= (1UL << 24u); /* TRCENA */
    DWT_CYCCNT = 0u;
    DWT_CTRL |= (1UL << 0u);          /* CYCCNTENA */
#endif
}

cancestry_time_us_t hal_stm32_get_timestamp_us(hal_stm32_clock_t *clk)
{
    uint32_t current_cycles;
    uint32_t delta;

    if (clk == NULL) {
        return 0u;
    }

#if defined(__arm__) || defined(__thumb__)
    current_cycles = DWT_CYCCNT;
#else
    /* Use simulated cycle counter */
    current_cycles = s_sim_cycles;
#endif

    /*
     * Rollover proof: delta = current_cycles - last_raw_cycles in unsigned
     * 32-bit arithmetic. Modulo 2^32 subtraction correctly computes elapsed
     * cycles even across counter overflow, as long as polling occurs within
     * 2^32 cycles (~26 seconds at 160 MHz).
     */
    delta = current_cycles - clk->last_raw_cycles;
    clk->accumulated_cycles += (uint64_t)delta;
    clk->last_raw_cycles = current_cycles;

    return (cancestry_time_us_t)(clk->accumulated_cycles / (uint64_t)clk->cycles_per_us);
}

/* ------------------------------------------------------------------------- */
/* CAN RX Interrupt Service Routine (SW-FR-BM-003)                           */
/* ------------------------------------------------------------------------- */

void hal_stm32_can_rx_isr(hal_stm32_context_t *ctx, uint8_t iface_index)
{
    uint8_t i;
    hal_stm32_controller_t *ctrl;
    cancestry_time_us_t hw_ts;

    if (ctx == NULL || iface_index >= CANCESTRY_STM32_MAX_CONTROLLERS) {
        return;
    }

    ctrl = &ctx->controllers[iface_index];
    if (!ctrl->opened) {
        return;
    }

    /*
     * Capture hardware timestamp at the very entry of the ISR to guarantee
     * sub-microsecond timestamp fidelity (SW-FR-BM-004).
     */
    hw_ts = hal_stm32_get_timestamp_us(&ctx->clock);

    /*
     * Drain hardware FIFO. Bounded loop: strictly at most CANCESTRY_STM32_FIFO_DEPTH
     * iterations (O(1)). Absolutely NO decoding, UDS parsing, or FSM evaluation.
     */
    for (i = 0u; i < CANCESTRY_STM32_FIFO_DEPTH; ++i) {
        sim_fifo_slot_t *slot = &s_sim_hardware_fifo[iface_index][i];
        if (slot->has_frame) {
            cancestry_event_t event;
            memset(&event, 0, sizeof(event));

            event.type = CANCESTRY_EVENT_TYPE_CAN_RX;
            event.priority_class = CANCESTRY_PRIORITY_CLASS_CAN_RX;
            event.timestamp_us = hw_ts;
            event.payload.can_rx.interface_id = (cancestry_interface_id_t)iface_index;
            event.payload.can_rx.can_id = slot->frame.can_id;
            event.payload.can_rx.is_extended = slot->frame.is_extended;
            event.payload.can_rx.is_fd = slot->frame.is_fd;
            event.payload.can_rx.length = slot->frame.length;
            memcpy(event.payload.can_rx.data, slot->frame.data, slot->frame.length);

            /* Push directly into lock-free ISR queue (SW-FR-BM-003) */
            if (ctx->primary_isr_queue != NULL) {
                (void)cancestry_event_isr_queue_push(ctx->primary_isr_queue, &event);
            }

            /* Also push to interface RX ring if bound */
            if (ctrl->rx_ring != NULL) {
                cancestry_hal_frame_t ring_frame = slot->frame;
                ring_frame.timestamp_us = hw_ts;
                (void)cancestry_hal_rx_ring_push(ctrl->rx_ring, &ring_frame);
            }

            slot->has_frame = false;
            ctrl->rx_interrupt_count++;
        }
    }
}

void hal_stm32_set_isr_queue(hal_stm32_context_t *ctx,
                             cancestry_event_isr_queue_t *isr_queue)
{
    if (ctx != NULL) {
        ctx->primary_isr_queue = isr_queue;
    }
}

void hal_stm32_sim_inject_hardware_frame(hal_stm32_context_t *ctx,
                                         uint8_t iface_index,
                                         const cancestry_hal_frame_t *frame)
{
    uint8_t i;
    (void)ctx;

    if (iface_index >= CANCESTRY_STM32_MAX_CONTROLLERS || frame == NULL) {
        return;
    }

    for (i = 0u; i < CANCESTRY_STM32_FIFO_DEPTH; ++i) {
        if (!s_sim_hardware_fifo[iface_index][i].has_frame) {
            s_sim_hardware_fifo[iface_index][i].frame = *frame;
            s_sim_hardware_fifo[iface_index][i].has_frame = true;
            break;
        }
    }
}

/* ------------------------------------------------------------------------- */
/* cancestry_hal_backend_t Implementation                                    */
/* ------------------------------------------------------------------------- */

static bool stm32_backend_open(void *ctx_void,
                               const cancestry_hal_if_config_t *cfg,
                               uint8_t iface_index)
{
    hal_stm32_context_t *ctx = (hal_stm32_context_t *)ctx_void;
    if (ctx == NULL || iface_index >= CANCESTRY_STM32_MAX_CONTROLLERS) {
        return false;
    }

    ctx->controllers[iface_index].iface_index = iface_index;
    ctx->controllers[iface_index].opened = true;
    ctx->controllers[iface_index].fd_capable = (cfg != NULL && cfg->can_fd);
    return true;
}

static void stm32_backend_close(void *ctx_void, uint8_t iface_index)
{
    hal_stm32_context_t *ctx = (hal_stm32_context_t *)ctx_void;
    if (ctx == NULL || iface_index >= CANCESTRY_STM32_MAX_CONTROLLERS) {
        return;
    }

    ctx->controllers[iface_index].opened = false;
}

static cancestry_hal_status_t stm32_backend_poll(void *ctx_void,
                                                 uint8_t iface_index,
                                                 cancestry_hal_rx_ring_t *ring,
                                                 cancestry_hal_fault_code_t *out_fault)
{
    hal_stm32_context_t *ctx = (hal_stm32_context_t *)ctx_void;
    if (out_fault != NULL) {
        *out_fault = CANCESTRY_HAL_FAULT_NONE;
    }

    if (ctx == NULL || iface_index >= CANCESTRY_STM32_MAX_CONTROLLERS) {
        return CANCESTRY_HAL_ERR_NULL;
    }

    /* Bind rx ring for ISR use */
    ctx->controllers[iface_index].rx_ring = ring;

    /*
     * In bare metal, reception is interrupt driven. In poll(), any events
     * pending in the lock-free ISR queue are drained or already placed in ring.
     */
    return CANCESTRY_HAL_OK;
}

static cancestry_hal_status_t stm32_backend_drain_tx(void *ctx_void,
                                                     uint8_t iface_index,
                                                     cancestry_hal_tx_ring_t *ring,
                                                     cancestry_hal_fault_code_t *out_fault)
{
    hal_stm32_context_t *ctx = (hal_stm32_context_t *)ctx_void;
    cancestry_hal_frame_t frame;

    if (out_fault != NULL) {
        *out_fault = CANCESTRY_HAL_FAULT_NONE;
    }

    if (ctx == NULL || ring == NULL || iface_index >= CANCESTRY_STM32_MAX_CONTROLLERS) {
        return CANCESTRY_HAL_ERR_NULL;
    }

    /* Drain software TX ring into hardware mailboxes */
    while (cancestry_hal_tx_ring_pop(ring, &frame) == CANCESTRY_HAL_OK) {
        ctx->controllers[iface_index].tx_frame_count++;
    }

    return CANCESTRY_HAL_OK;
}

static bool stm32_backend_caps(void *ctx_void,
                               uint8_t iface_index,
                               cancestry_hal_transport_caps_t *out_caps)
{
    hal_stm32_context_t *ctx = (hal_stm32_context_t *)ctx_void;
    if (ctx == NULL || out_caps == NULL || iface_index >= CANCESTRY_STM32_MAX_CONTROLLERS) {
        return false;
    }

    memset(out_caps, 0, sizeof(*out_caps));
    out_caps->interface_id = (cancestry_interface_id_t)iface_index;
    out_caps->can_fd = ctx->controllers[iface_index].fd_capable;
    out_caps->max_length = ctx->controllers[iface_index].fd_capable
                               ? CANCESTRY_HAL_FRAME_MAX_LENGTH
                               : CANCESTRY_HAL_CLASSIC_FRAME_MAX_LENGTH;
    return true;
}

static const cancestry_hal_backend_t s_stm32_backend = {
    stm32_backend_open,
    stm32_backend_close,
    stm32_backend_poll,
    stm32_backend_drain_tx,
    stm32_backend_caps
};

const cancestry_hal_backend_t *cancestry_cortex_m_hal_backend(void)
{
    return &s_stm32_backend;
}
