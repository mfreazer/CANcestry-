/*
 * CANcestry - terminal hard-fault persistence and escalation sequence.
 *
 * Implements QA-P31-01 and QA-P31-03, SW-FR-LOG-004 and the hard-fault
 * boundary for SW-FR-EVENT-008, SYS-SF-004 and SYS-FR-016.
 */

#include "cancestry/event/fault.h"

#include <stddef.h>

#if defined(__arm__) || defined(__thumb__)
#define CANCESTRY_EVENT_NOP() __asm volatile("nop")
#else
/* Keep host tests and the portable core deterministic without depending on a
 * libc sleep primitive. The memory clobber prevents the loop from being
 * treated as a removable empty loop by optimizing compilers. */
#define CANCESTRY_EVENT_NOP() __asm volatile("" ::: "memory")
#endif

static void cancestry_event_safe_spin(void)
{
    for (;;) {
        CANCESTRY_EVENT_NOP();
    }
}

bool cancestry_event_hard_fault_escalate(
    const cancestry_event_hard_fault_hooks_t *hooks)
{
    if (hooks == NULL || hooks->write_retention_register == NULL ||
        hooks->hal_fail_safe == NULL || hooks->iwdg_escalate == NULL) {
        cancestry_event_safe_spin();
    }

    /* Retention must be committed before the reset path can run. */
    hooks->write_retention_register(CANCESTRY_FAULT_CODE_QUEUE_SATURATION,
                                    hooks->context);

    /* Safe outputs first; the watchdog reset is the second independent line
     * of defense and must not delay the immediate safe-state transition. */
    hooks->hal_fail_safe(hooks->context);
    hooks->iwdg_escalate(hooks->context);
    return true;
}
