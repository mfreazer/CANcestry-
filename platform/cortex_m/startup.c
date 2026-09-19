/*
 * CANcestry - Cortex-M boot recovery boundary.
 *
 * The startup integration is deliberately small: hardware safe-state setup
 * remains in watchdog initialization, then this function replays and clears
 * the one retained terminal fault after the persistent log is ready.
 */

#include "startup.h"

bool cancestry_cortex_m_startup_restore_retained_fault(
    cancestry_watchdog_t *wdg,
    cancestry_event_queue_t *persistent_fault_log)
{
    return cancestry_watchdog_restore_retained_fault(wdg, persistent_fault_log);
}
