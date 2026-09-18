/*
 * CANcestry Bare-Metal Conformance: Watchdog Fail-Safe and Recovery.
 *
 * Contract under test (docs/software/SwRS.md section 15):
 *   - SW-FR-BM-005: hardware watchdog integration (IWDG). The system shall
 *     configure and service an Independent Watchdog timer. If the main
 *     fsm_tick loop misses its deadline, the IWDG resets the processor.
 *   - SW-FR-BM-006: fail-safe state on reset / boot. Upon hardware reset or
 *     watchdog reset, hardware pins and CAN transceivers must be configured
 *     into a safe "0 Torque / Contactor Open" state until the FSM explicitly
 *     authorizes otherwise.
 *
 * Test ids: BM-SAFE-001 .. BM-SAFE-004.
 */

#include "cancestry/event/clock.h"
#include "cancestry/hal/types.h"
#include "watchdog.h"

#include "cancestry_test.h"

#include <string.h>

/* ------------------------------------------------------------------------- */
/* BM-SAFE-001: Normal Execution Periodic Feed Keeps Watchdog Alive         */
/* ------------------------------------------------------------------------- */
static void test_watchdog_normal_periodic_feed(void)
{
    cancestry_watchdog_t wdg;
    cancestry_watchdog_config_t cfg;
    uint32_t step;

    CANCESSTRY_TEST_CASE("BM-SAFE-001: regular watchdog feed prevents reset");

    memset(&cfg, 0, sizeof(cfg));
    cfg.timeout_ms = 50u;
    cfg.enforce_safe_state_on_boot = true;

    CANCESSTRY_TEST_CHECK(cancestry_watchdog_init(&wdg, &cfg));
    CANCESSTRY_TEST_CHECK(wdg.is_running);
    CANCESSTRY_TEST_CHECK_U64(wdg.timeout_ms, 50u);

    /* Initially starts in safe state */
    CANCESSTRY_TEST_CHECK(cancestry_hardware_is_safe_state(&wdg));
    CANCESSTRY_TEST_CHECK(!cancestry_hardware_tx_is_authorized(&wdg));

    /* FSM transitions to healthy operational state, authorizing TX */
    cancestry_hardware_authorize_tx(&wdg, true);
    CANCESSTRY_TEST_CHECK(cancestry_hardware_tx_is_authorized(&wdg));
    CANCESSTRY_TEST_CHECK(!cancestry_hardware_is_safe_state(&wdg));

    /* Simulate 10 loop iterations (10ms each, feeding every tick) */
    for (step = 0u; step < 10u; ++step) {
        /* Advance time 10ms */
        bool did_reset = cancestry_watchdog_sim_tick(&wdg, 10u);
        CANCESSTRY_TEST_CHECK(!did_reset);
        /* Feed watchdog at each fsm_tick */
        cancestry_watchdog_feed(&wdg);
    }

    CANCESSTRY_TEST_CHECK(!cancestry_watchdog_did_reset(&wdg));
    CANCESSTRY_TEST_CHECK(wdg.is_running);
    CANCESSTRY_TEST_CHECK_U64(wdg.feed_count, 10u);
    CANCESSTRY_TEST_CHECK(cancestry_hardware_tx_is_authorized(&wdg));
}

/* ------------------------------------------------------------------------- */
/* BM-SAFE-002: Induced Hang Triggers IWDG Reset & Safe State               */
/* ------------------------------------------------------------------------- */
static void test_watchdog_induced_hang_triggers_reset(void)
{
    cancestry_watchdog_t wdg;
    cancestry_watchdog_config_t cfg;
    bool did_reset;

    CANCESSTRY_TEST_CASE("BM-SAFE-002: induced hang triggers watchdog reset and safe state");

    memset(&cfg, 0, sizeof(cfg));
    cfg.timeout_ms = 50u;
    CANCESSTRY_TEST_CHECK(cancestry_watchdog_init(&wdg, &cfg));

    /* Authorize drive/transmission */
    cancestry_hardware_authorize_tx(&wdg, true);
    CANCESSTRY_TEST_CHECK(cancestry_hardware_tx_is_authorized(&wdg));

    /* Artificially induce a hang (simulating infinite loop or deadlock in main) */
    cancestry_watchdog_induce_hang(&wdg);

    /* Attempting to feed while hung does nothing */
    cancestry_watchdog_feed(&wdg);

    /* Time advances past deadline (55 ms > 50 ms timeout) */
    did_reset = cancestry_watchdog_sim_tick(&wdg, 55u);

    /* Watchdog must fire and reset the processor */
    CANCESSTRY_TEST_CHECK(did_reset);
    CANCESSTRY_TEST_CHECK(cancestry_watchdog_did_reset(&wdg));
    CANCESSTRY_TEST_CHECK_U64(wdg.reset_cause, CANCESTRY_RESET_CAUSE_WATCHDOG);

    /* Hardware outputs must immediately latch into safe state */
    CANCESSTRY_TEST_CHECK(cancestry_hardware_is_safe_state(&wdg));
    CANCESSTRY_TEST_CHECK(!cancestry_hardware_tx_is_authorized(&wdg));
}

/* ------------------------------------------------------------------------- */
/* BM-SAFE-003: Safe-State CAN Frame Transmission Upon Recovery             */
/* ------------------------------------------------------------------------- */
static void test_watchdog_safe_state_can_transmission(void)
{
    cancestry_watchdog_t wdg;
    cancestry_watchdog_config_t cfg;
    cancestry_hal_frame_t safe_frame;

    CANCESSTRY_TEST_CASE("BM-SAFE-003: safe-state CAN broadcast (0 Torque / Contactor Open)");

    memset(&cfg, 0, sizeof(cfg));
    cfg.timeout_ms = 50u;
    cancestry_watchdog_init(&wdg, &cfg);

    /* System boots / recovers into safe state */
    CANCESSTRY_TEST_CHECK(cancestry_hardware_is_safe_state(&wdg));

    /* Construct safe state CAN frame */
    safe_frame = cancestry_hardware_get_safe_state_frame();

    /* Verify safe-state message parameters */
    CANCESSTRY_TEST_CHECK_U64(safe_frame.can_id, CANCESTRY_SAFE_STATE_CAN_ID);
    CANCESSTRY_TEST_CHECK_U64(safe_frame.length, 8u);

    /* Byte 0..1: Torque limit = 0 Nm */
    CANCESSTRY_TEST_CHECK_U64(safe_frame.data[0], 0x00u);
    CANCESSTRY_TEST_CHECK_U64(safe_frame.data[1], 0x00u);

    /* Byte 2: Contactors = OPEN (0x00) */
    CANCESSTRY_TEST_CHECK_U64(safe_frame.data[2], 0x00u);

    /* Byte 3: Safe-state active flag = 1 (0x01) */
    CANCESSTRY_TEST_CHECK_U64(safe_frame.data[3], 0x01u);

    /* Confirm CAN TX is blocked for all normal application messages */
    CANCESSTRY_TEST_CHECK(!cancestry_hardware_tx_is_authorized(&wdg));
}

/* ------------------------------------------------------------------------- */
/* BM-SAFE-004: FSM Authorization Gate Post-Reset Recovery                  */
/* ------------------------------------------------------------------------- */
static void test_watchdog_fsm_authorization_gate(void)
{
    cancestry_watchdog_t wdg;
    cancestry_watchdog_config_t cfg;

    CANCESSTRY_TEST_CASE("BM-SAFE-004: FSM authorization required to exit safe state");

    memset(&cfg, 0, sizeof(cfg));
    cfg.timeout_ms = 50u;

    /* Initialize after watchdog reset */
    cancestry_watchdog_init(&wdg, &cfg);
    CANCESSTRY_TEST_CHECK(!cancestry_hardware_tx_is_authorized(&wdg));

    /* Attempting to set false keeps it safe */
    cancestry_hardware_authorize_tx(&wdg, false);
    CANCESSTRY_TEST_CHECK(!cancestry_hardware_tx_is_authorized(&wdg));
    CANCESSTRY_TEST_CHECK(cancestry_hardware_is_safe_state(&wdg));

    /* FSM completes boot checks and grants authorization */
    cancestry_hardware_authorize_tx(&wdg, true);
    CANCESSTRY_TEST_CHECK(cancestry_hardware_tx_is_authorized(&wdg));
    CANCESSTRY_TEST_CHECK(!cancestry_hardware_is_safe_state(&wdg));

    /* If a safety condition fails, FSM revokes authorization */
    cancestry_hardware_authorize_tx(&wdg, false);
    CANCESSTRY_TEST_CHECK(!cancestry_hardware_tx_is_authorized(&wdg));
    CANCESSTRY_TEST_CHECK(cancestry_hardware_is_safe_state(&wdg));
}

int main(void)
{
    CANCESSTRY_TEST_SUITE_BEGIN("Watchdog Fail-Safe & Safe State Conformance");

    test_watchdog_normal_periodic_feed();
    test_watchdog_induced_hang_triggers_reset();
    test_watchdog_safe_state_can_transmission();
    test_watchdog_fsm_authorization_gate();

    return CANCESSTRY_TEST_SUITE_END();
}
