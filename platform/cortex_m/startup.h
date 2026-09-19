/*
 * CANcestry - Cortex-M boot recovery boundary.
 *
 * Implements QA-P31-01, SW-FR-LOG-004, SYS-FR-016 and SYS-SF-004.
 */

#ifndef CANCESTRY_PLATFORM_CORTEX_M_STARTUP_H
#define CANCESTRY_PLATFORM_CORTEX_M_STARTUP_H

#include "watchdog.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Replay a retained terminal queue-saturation fault during boot.
 *
 * A real Reset_Handler calls this after the watchdog/GPIO safe state is
 * established and the caller-owned persistent fault log is initialized.
 */
bool cancestry_cortex_m_startup_restore_retained_fault(
    cancestry_watchdog_t *wdg,
    cancestry_event_queue_t *persistent_fault_log);

#ifdef __cplusplus
}
#endif

#endif /* CANCESTRY_PLATFORM_CORTEX_M_STARTUP_H */
