/*
 * CANcestry - hard-fault escalation boundary.
 *
 * Normative references:
 *   docs/system/event-ordering.md  section 11 (reserved fault slots)
 *   docs/software/SwRS.md          SW-FR-EVENT-004 .. SW-FR-EVENT-008
 *   docs/software/SwRS.md          SW-FR-BM-005 .. SW-FR-BM-006
 *
 * The portable event core does not include a hardware driver. Instead, the
 * platform supplies three caller-owned hooks: one that writes register-level
 * retention, one that drives the HAL outputs to the fail-safe state, and one
 * that arms/escalates the independent watchdog. The core invokes them in that
 * order and never allocates or blocks.
 */

#ifndef CANCESTRY_EVENT_FAULT_H
#define CANCESTRY_EVENT_FAULT_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Fault retained when the bounded event queue reaches terminal saturation. */
#define CANCESTRY_FAULT_CODE_QUEUE_SATURATION ((uint32_t)0x45565101u)

/** Source id recorded when a retained queue-saturation fault is replayed. */
#define CANCESTRY_FAULT_SOURCE_HARD_FAULT_ESCALATION ((uint32_t)0x45565102u)

/** One side effect in the hard-fault escalation sequence. */
typedef void (*cancestry_event_hard_fault_action_t)(void *context);

/** Register-level retention write; it must not use Flash/EEPROM or block. */
typedef void (*cancestry_event_retention_write_action_t)(uint32_t code,
                                                         void *context);

/**
 * Platform actions used when the event queue cannot admit another fault.
 *
 * @c context is borrowed for the duration of the call. All three actions are
 * required. An incomplete configuration enters the non-returning safe-spin
 * path instead of returning a false status or continuing unsafely.
 */
typedef struct cancestry_event_hard_fault_hooks {
    /** Write the terminal fault code to a boot-surviving retention register. */
    cancestry_event_retention_write_action_t write_retention_register;
    /** Immediately force torque to zero, open contactors and revoke TX. */
    cancestry_event_hard_fault_action_t hal_fail_safe;
    /** Escalate to the running IWDG; the next missed feed resets the MCU. */
    cancestry_event_hard_fault_action_t iwdg_escalate;
    /** Caller-owned context passed to all actions. */
    void *context;
} cancestry_event_hard_fault_hooks_t;

/**
 * Invoke the retention, fail-safe and watchdog actions for an unrecoverable
 * event fault.
 *
 * Retention is written before the HAL/IWDG sequence. The HAL action is always
 * called before the IWDG action, and a complete invocation never returns false.
 * If @p hooks is NULL or any required hook is missing, this function enters a
 * non-returning safe-spin loop. It never silently continues with incomplete
 * safety handling.
 *
 * @return true only when all required actions were invoked. The return is
 *         retained for source compatibility; an incomplete configuration does
 *         not return.
 */
bool cancestry_event_hard_fault_escalate(
    const cancestry_event_hard_fault_hooks_t *hooks);

#ifdef __cplusplus
}
#endif

#endif /* CANCESTRY_EVENT_FAULT_H */
