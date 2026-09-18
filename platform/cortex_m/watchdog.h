/*
 * CANcestry - Bare-metal Independent Watchdog (IWDG) and Fail-Safe Controller.
 *
 * Normative references:
 *   docs/software/SwRS.md          SW-FR-BM-005 (watchdog integration),
 *                                  SW-FR-BM-006 (fail-safe pin state on reset)
 *   docs/system/SyRS.md            SYS-SF-002 (fail-closed operation)
 *
 * Design notes:
 *   - Configures the ARM Cortex-M hardware Independent Watchdog (IWDG) driven
 *     by the internal dedicated LSI clock.
 *   - The main run loop is contractually required to refresh (feed) the IWDG
 *     at each fsm_tick. If execution hangs or a deadline is missed, the IWDG
 *     resets the processor.
 *   - Upon reset, hardware pins and transceivers are placed into a safe
 *     "0 Torque / Contactor Open" state immediately at startup. No CAN traffic
 *     is transmitted until the FSM explicitly authorizes it.
 *
 * Implements: SW-FR-BM-005, SW-FR-BM-006.
 */

#ifndef CANCESTRY_PLATFORM_CORTEX_M_WATCHDOG_H
#define CANCESTRY_PLATFORM_CORTEX_M_WATCHDOG_H

#include "cancestry/event/clock.h"
#include "cancestry/hal/types.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Safe state CAN message ID (Battery & Inverter fail-safe broadcast). */
#define CANCESTRY_SAFE_STATE_CAN_ID ((uint32_t)0x100u)

/** Reset reason detected at system startup. */
typedef enum cancestry_reset_cause {
    CANCESTRY_RESET_CAUSE_UNKNOWN = 0,
    CANCESTRY_RESET_CAUSE_POWER_ON = 1,
    CANCESTRY_RESET_CAUSE_WATCHDOG = 2,
    CANCESTRY_RESET_CAUSE_SOFTWARE = 3,
    CANCESTRY_RESET_CAUSE_PIN_RESET = 4
} cancestry_reset_cause_t;

/** Watchdog configuration parameters. */
typedef struct cancestry_watchdog_config {
    /** Watchdog timeout in milliseconds (e.g. 50 ms). */
    uint32_t timeout_ms;
    /** Automatically configure safe GPIO states on boot/reset. */
    bool enforce_safe_state_on_boot;
} cancestry_watchdog_config_t;

/** Watchdog instance state. */
typedef struct cancestry_watchdog {
    uint32_t timeout_ms;
    cancestry_reset_cause_t reset_cause;
    bool is_running;
    bool safe_state_active;
    bool tx_authorized;
    uint32_t feed_count;
    cancestry_time_us_t last_feed_us;
    /* Simulated countdown for HIL/host test harnesses */
    int32_t remaining_time_ms;
    bool hang_induced;
} cancestry_watchdog_t;

/**
 * Initialize the hardware watchdog and enforce initial safe pin states.
 *
 * Reads RCC reset flags to determine if the previous reset was caused by
 * an IWDG timeout. If so, records CANCESTRY_RESET_CAUSE_WATCHDOG and ensures
 * hardware outputs remain in safe state.
 *
 * @param wdg     Watchdog instance.
 * @param config  Configuration parameters.
 * @return true on successful initialization.
 */
bool cancestry_watchdog_init(cancestry_watchdog_t *wdg,
                             const cancestry_watchdog_config_t *config);

/**
 * Refresh (feed) the watchdog counter.
 *
 * Writes the reload key (0xAAAA) to the hardware IWDG register. Must be
 * called regularly by the main execution loop.
 */
void cancestry_watchdog_feed(cancestry_watchdog_t *wdg);

/**
 * @return true if the last reset was triggered by an IWDG timeout.
 */
bool cancestry_watchdog_did_reset(const cancestry_watchdog_t *wdg);

/**
 * Advance virtual time for host/HIL simulation of watchdog timing.
 *
 * @param wdg       Watchdog instance.
 * @param delta_ms  Elapsed time in milliseconds.
 * @return true if watchdog timeout triggered an MCU reset.
 */
bool cancestry_watchdog_sim_tick(cancestry_watchdog_t *wdg, uint32_t delta_ms);

/**
 * Induce an intentional hang to verify fail-safe reset recovery.
 */
void cancestry_watchdog_induce_hang(cancestry_watchdog_t *wdg);

/* ------------------------------------------------------------------------- */
/* Hardware Safe-State Pin Control (SW-FR-BM-006)                           */
/* ------------------------------------------------------------------------- */

/**
 * Hardware-configure GPIO and CAN transceiver pins to the safe state:
 * - CAN TX lines held recessive / high-impedance.
 * - Inverter torque demand forced to 0.0 Nm.
 * - Battery contactors forced OPEN (de-energized).
 * - Transmission authorization revoked.
 */
void cancestry_hardware_set_safe_state(cancestry_watchdog_t *wdg);

/**
 * @return true if hardware outputs are currently locked in the safe state.
 */
bool cancestry_hardware_is_safe_state(const cancestry_watchdog_t *wdg);

/**
 * Explicitly authorize CAN transmission once FSM confirms safe operation.
 *
 * @param wdg        Watchdog instance.
 * @param authorize  true to enable transmission, false to return to safe state.
 */
void cancestry_hardware_authorize_tx(cancestry_watchdog_t *wdg, bool authorize);

/**
 * @return true if CAN transmission is authorized by the FSM.
 */
bool cancestry_hardware_tx_is_authorized(const cancestry_watchdog_t *wdg);

/**
 * Construct the canonical safe-state CAN broadcast frame:
 * ID = 0x100, Torque = 0, Contactors = Open (0).
 */
cancestry_hal_frame_t cancestry_hardware_get_safe_state_frame(void);

#ifdef __cplusplus
}
#endif

#endif /* CANCESTRY_PLATFORM_CORTEX_M_WATCHDOG_H */
