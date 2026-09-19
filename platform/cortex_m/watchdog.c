/*
 * CANcestry - Bare-metal Independent Watchdog (IWDG) and Fail-Safe Controller.
 *
 * Implements: SW-FR-BM-005 (watchdog integration),
 *             SW-FR-BM-006 (fail-safe pin state on reset).
 */

#include "watchdog.h"

#include <string.h>

#if defined(__arm__) || defined(__thumb__)
/* STM32 Register Definitions */
#define IWDG_BASE        (0x40003000UL)
#define IWDG_KR          (*(volatile uint32_t *)(IWDG_BASE + 0x00u))
#define IWDG_PR          (*(volatile uint32_t *)(IWDG_BASE + 0x04u))
#define IWDG_RLR         (*(volatile uint32_t *)(IWDG_BASE + 0x08u))
#define IWDG_SR          (*(volatile uint32_t *)(IWDG_BASE + 0x0Cu))

#define RCC_BASE         (0x40021000UL)
#define RCC_CSR          (*(volatile uint32_t *)(RCC_BASE + 0x94u))
#define RCC_CSR_IWDGRSTF (1UL << 29u)
#define RCC_CSR_RMVF     (1UL << 23u)

#define GPIOA_BASE       (0x48000000UL)
#define GPIOA_MODER      (*(volatile uint32_t *)(GPIOA_BASE + 0x00u))
#define GPIOA_ODR        (*(volatile uint32_t *)(GPIOA_BASE + 0x14u))
#define GPIOA_BSRR       (*(volatile uint32_t *)(GPIOA_BASE + 0x18u))

/* RTC backup register: retention RAM/register storage survives IWDG reset. */
#define RTC_BASE         (0x40002800UL)
#define RTC_BKP0R        (*(volatile uint32_t *)(RTC_BASE + 0x50u))
#endif

/* Simulated hardware registers for testing & host verification. */
#if !defined(__arm__) && !defined(__thumb__)
static uint32_t s_sim_rcc_csr = 0u;
static volatile uint32_t s_sim_retention_register = 0u;
#endif
static bool s_sim_gpio_safe = true;

bool cancestry_watchdog_init(cancestry_watchdog_t *wdg,
                             const cancestry_watchdog_config_t *config)
{
    if (wdg == NULL) {
        return false;
    }

    memset(wdg, 0, sizeof(*wdg));
    wdg->timeout_ms = (config != NULL && config->timeout_ms > 0u) ? config->timeout_ms : 50u;
    wdg->remaining_time_ms = (int32_t)wdg->timeout_ms;
    wdg->safe_state_active = true;
    wdg->tx_authorized = false;
    wdg->hang_induced = false;

    /* Detect reset cause */
#if defined(__arm__) || defined(__thumb__)
    if ((RCC_CSR & RCC_CSR_IWDGRSTF) != 0u) {
        wdg->reset_cause = CANCESTRY_RESET_CAUSE_WATCHDOG;
        /* Clear reset flags */
        RCC_CSR |= RCC_CSR_RMVF;
    } else {
        wdg->reset_cause = CANCESTRY_RESET_CAUSE_POWER_ON;
    }
#else
    if ((s_sim_rcc_csr & (1u << 29)) != 0u) {
        wdg->reset_cause = CANCESTRY_RESET_CAUSE_WATCHDOG;
        s_sim_rcc_csr &= ~(1u << 29);
    } else {
        wdg->reset_cause = CANCESTRY_RESET_CAUSE_POWER_ON;
    }
#endif

    /* Enforce safe state upon reset */
    cancestry_hardware_set_safe_state(wdg);

    /* Initialize hardware IWDG */
#if defined(__arm__) || defined(__thumb__)
    /* Enable write access to PR and RLR */
    IWDG_KR = 0x5555u;
    /* Prescaler /32: 32kHz / 32 = 1kHz (1 ms per tick) */
    IWDG_PR = 0x03u;
    /* Set reload value */
    IWDG_RLR = (wdg->timeout_ms & 0x0FFFu);
    /* Reload counter */
    IWDG_KR = 0xAAAAu;
    /* Start watchdog */
    IWDG_KR = 0xCCCCu;
#endif

    wdg->is_running = true;
    return true;
}

void cancestry_watchdog_feed(cancestry_watchdog_t *wdg)
{
    if (wdg == NULL || !wdg->is_running || wdg->hang_induced) {
        return;
    }

#if defined(__arm__) || defined(__thumb__)
    /* Reload hardware counter */
    IWDG_KR = 0xAAAAu;
#endif

    wdg->remaining_time_ms = (int32_t)wdg->timeout_ms;
    wdg->feed_count++;
}

bool cancestry_watchdog_did_reset(const cancestry_watchdog_t *wdg)
{
    if (wdg == NULL) {
        return false;
    }
    return wdg->reset_cause == CANCESTRY_RESET_CAUSE_WATCHDOG;
}

void cancestry_watchdog_induce_hang(cancestry_watchdog_t *wdg)
{
    if (wdg != NULL) {
        wdg->hang_induced = true;
    }
}

void cancestry_watchdog_escalate(cancestry_watchdog_t *wdg)
{
    if (wdg == NULL) {
        return;
    }

    /* The outputs are safe before the watchdog reset is allowed to take its
     * bounded course. This is deliberately duplicated in the hook adapter so
     * a direct caller also gets the fail-safe invariant. */
    cancestry_hardware_set_safe_state(wdg);
    wdg->hang_induced = true;
    wdg->remaining_time_ms = 0;

#if defined(__arm__) || defined(__thumb__)
    /* Leave the IWDG running but reduce the remaining period to the smallest
     * useful reload. No feed can occur after escalation. */
    IWDG_KR = 0x5555u;
    IWDG_RLR = 1u;
    IWDG_KR = 0xAAAAu;
#endif
}

static void watchdog_hard_fault_fail_safe(void *context)
{
    cancestry_watchdog_t *wdg = (cancestry_watchdog_t *)context;
    cancestry_hardware_set_safe_state(wdg);
}

static void watchdog_hard_fault_iwdg(void *context)
{
    cancestry_watchdog_t *wdg = (cancestry_watchdog_t *)context;
    cancestry_watchdog_escalate(wdg);
}

void cancestry_watchdog_get_hard_fault_hooks(
    cancestry_event_hard_fault_hooks_t *hooks,
    cancestry_watchdog_t *wdg)
{
    if (hooks == NULL) {
        return;
    }
    hooks->write_retention_register = cancestry_watchdog_write_retention_register;
    hooks->hal_fail_safe = watchdog_hard_fault_fail_safe;
    hooks->iwdg_escalate = watchdog_hard_fault_iwdg;
    hooks->context = wdg;
}

bool cancestry_watchdog_bind_event_queue(cancestry_watchdog_t *wdg,
                                         cancestry_event_queue_t *queue)
{
    cancestry_event_hard_fault_hooks_t hooks;

    if (wdg == NULL || queue == NULL) {
        return false;
    }
    cancestry_watchdog_get_hard_fault_hooks(&hooks, wdg);
    return cancestry_event_queue_set_hard_fault_hooks(queue, &hooks);
}

void cancestry_watchdog_write_retention_register(uint32_t code, void *context)
{
    (void)context;
#if defined(__arm__) || defined(__thumb__)
    /* RTC backup registers are register-level retention storage. */
    RTC_BKP0R = code;
    __asm volatile("dmb" ::: "memory");
#else
    s_sim_retention_register = code;
#endif
}

uint32_t cancestry_watchdog_read_retention_register(
    const cancestry_watchdog_t *wdg)
{
    (void)wdg;
#if defined(__arm__) || defined(__thumb__)
    return RTC_BKP0R;
#else
    return s_sim_retention_register;
#endif
}

void cancestry_watchdog_clear_retention_register(cancestry_watchdog_t *wdg)
{
    cancestry_watchdog_write_retention_register(0u, wdg);
}

bool cancestry_watchdog_restore_retained_fault(
    cancestry_watchdog_t *wdg,
    cancestry_event_queue_t *persistent_fault_log)
{
    cancestry_event_t event;
    cancestry_event_queue_status_t status;
    uint32_t retained_code;

    if (wdg == NULL || persistent_fault_log == NULL ||
        !cancestry_event_queue_is_valid(persistent_fault_log)) {
        return false;
    }

    retained_code = cancestry_watchdog_read_retention_register(wdg);
    if (retained_code == 0u) {
        return false;
    }

    cancestry_event_init(&event);
    event.type = CANCESTRY_EVENT_TYPE_FAULT_RAISED;
    event.priority_class = CANCESTRY_PRIORITY_CLASS_FAULT;
    event.timestamp_us = 0u;
    event.payload.fault.fault_code = retained_code;
    event.payload.fault.severity = CANCESTRY_FAULT_SEVERITY_CRITICAL;
    event.payload.fault.source_id =
        CANCESTRY_FAULT_SOURCE_HARD_FAULT_ESCALATION;

    status = cancestry_event_queue_push(persistent_fault_log, &event);
    if (!cancestry_event_queue_status_is_ok(status)) {
        return false;
    }
    cancestry_watchdog_clear_retention_register(wdg);
    return true;
}

bool cancestry_watchdog_sim_tick(cancestry_watchdog_t *wdg, uint32_t delta_ms)
{
    if (wdg == NULL || !wdg->is_running) {
        return false;
    }

    wdg->remaining_time_ms -= (int32_t)delta_ms;

    if (wdg->remaining_time_ms <= 0) {
        /* Watchdog expired: triggers MCU reset! */
        wdg->is_running = false;
        wdg->reset_cause = CANCESTRY_RESET_CAUSE_WATCHDOG;
        s_sim_rcc_csr |= (1u << 29);

        /* Reset triggers immediate hardware safe state */
        cancestry_hardware_set_safe_state(wdg);
        return true;
    }

    return false;
}

void cancestry_hardware_set_safe_state(cancestry_watchdog_t *wdg)
{
    if (wdg != NULL) {
        wdg->safe_state_active = true;
        wdg->tx_authorized = false;
    }

#if defined(__arm__) || defined(__thumb__)
    /*
     * Configure CAN TX and contactor control pins into safe states:
     * - Pin PA12 (CAN_TX): set to input/floating (recessive)
     * - Pin PA8 (Inverter Enable / Torque): drive LOW (0V / disabled)
     * - Pin PA9 (Main Contactor): drive LOW (Open / de-energized)
     */
    GPIOA_BSRR = (1UL << (8u + 16u)) | (1UL << (9u + 16u)); /* Reset bits 8, 9 */
#endif

    s_sim_gpio_safe = true;
}

bool cancestry_hardware_is_safe_state(const cancestry_watchdog_t *wdg)
{
    if (wdg == NULL) {
        return false;
    }
    return wdg->safe_state_active && !wdg->tx_authorized && s_sim_gpio_safe;
}

void cancestry_hardware_authorize_tx(cancestry_watchdog_t *wdg, bool authorize)
{
    if (wdg == NULL) {
        return;
    }

    if (authorize) {
        wdg->tx_authorized = true;
        wdg->safe_state_active = false;
        s_sim_gpio_safe = false;
    } else {
        cancestry_hardware_set_safe_state(wdg);
    }
}

bool cancestry_hardware_tx_is_authorized(const cancestry_watchdog_t *wdg)
{
    if (wdg == NULL) {
        return false;
    }
    return wdg->tx_authorized && !wdg->safe_state_active;
}

cancestry_hal_frame_t cancestry_hardware_get_safe_state_frame(void)
{
    cancestry_hal_frame_t frame;
    memset(&frame, 0, sizeof(frame));

    frame.can_id = CANCESTRY_SAFE_STATE_CAN_ID;
    frame.interface_id = 0u;
    frame.is_extended = 0u;
    frame.is_fd = 0u;
    frame.length = 8u;

    /*
     * Payload encoding:
     * Byte 0..1: Torque limit (0 Nm, little-endian: 0x0000)
     * Byte 2: Contactor status (0 = OPEN, safe)
     * Byte 3: Safe-state active flag (1 = SAFE_STATE)
     * Byte 4..7: Reserved (0x00)
     */
    frame.data[0] = 0x00u;
    frame.data[1] = 0x00u;
    frame.data[2] = 0x00u;
    frame.data[3] = 0x01u;
    frame.data[4] = 0x00u;
    frame.data[5] = 0x00u;
    frame.data[6] = 0x00u;
    frame.data[7] = 0x00u;
    frame.timestamp_us = 0u;

    return frame;
}
