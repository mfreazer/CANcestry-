/*
 * CANcestry - portable hard-fault escalation sequence.
 *
 * Implements SW-FR-EVENT-004 .. SW-FR-EVENT-008 and provides the hardware
 * binding point for SW-FR-BM-005 .. SW-FR-BM-006 and QA-EV-01.
 */

#include "cancestry/event/fault.h"

#include <stddef.h>

bool cancestry_event_hard_fault_escalate(
    const cancestry_event_hard_fault_hooks_t *hooks)
{
    bool complete;

    if (hooks == NULL) {
        return false;
    }

    /* Safe outputs first; the watchdog reset is the second, independent line
     * of defense and must not delay the immediate safe-state transition. */
    if (hooks->hal_fail_safe != NULL) {
        hooks->hal_fail_safe(hooks->context);
    }
    if (hooks->iwdg_escalate != NULL) {
        hooks->iwdg_escalate(hooks->context);
    }

    complete = (hooks->hal_fail_safe != NULL) && (hooks->iwdg_escalate != NULL);
    return complete;
}
