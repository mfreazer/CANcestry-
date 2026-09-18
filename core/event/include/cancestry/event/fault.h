/*
 * CANcestry - hard-fault escalation boundary.
 *
 * Normative references:
 *   docs/system/event-ordering.md  section 11 (reserved fault slots)
 *   docs/software/SwRS.md          SW-FR-EVENT-004 .. SW-FR-EVENT-008
 *   docs/software/SwRS.md          SW-FR-BM-005 .. SW-FR-BM-006
 *
 * The portable event core does not include a hardware driver. Instead, the
 * platform supplies two caller-owned hooks: one that drives the HAL outputs to
 * the fail-safe state and one that arms/escalates the independent watchdog.
 * The core invokes them in that order and never allocates or blocks.
 */

#ifndef CANCESTRY_EVENT_FAULT_H
#define CANCESTRY_EVENT_FAULT_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** One side effect in the hard-fault escalation sequence. */
typedef void (*cancestry_event_hard_fault_action_t)(void *context);

/**
 * Platform actions used when the event queue cannot admit another fault.
 *
 * @c context is borrowed for the duration of the call. Both actions are
 * required for a configured escalation path; the portable implementation
 * nevertheless invokes whichever non-NULL action is present so a partially
 * configured path fails closed rather than silently doing nothing.
 */
typedef struct cancestry_event_hard_fault_hooks {
    /** Immediately force torque to zero, open contactors and revoke TX. */
    cancestry_event_hard_fault_action_t hal_fail_safe;
    /** Escalate to the running IWDG; the next missed feed resets the MCU. */
    cancestry_event_hard_fault_action_t iwdg_escalate;
    /** Caller-owned context passed to both actions. */
    void *context;
} cancestry_event_hard_fault_hooks_t;

/**
 * Invoke the fail-safe and watchdog actions for an unrecoverable event fault.
 *
 * The HAL action is always called before the IWDG action. This makes the
 * software safe immediately even on a target where the watchdog reset takes a
 * bounded, non-zero amount of time. The function is deterministic and has no
 * heap or blocking path.
 *
 * @return true when both required hooks are configured and invoked; false when
 *         @p hooks is NULL or either required hook is missing. A non-NULL hook
 *         is still invoked on a partially configured input.
 */
bool cancestry_event_hard_fault_escalate(
    const cancestry_event_hard_fault_hooks_t *hooks);

#ifdef __cplusplus
}
#endif

#endif /* CANCESTRY_EVENT_FAULT_H */
