/*
 * QA-EV-01 / Issue #21 correction: hardware-bound hard-fault escalation.
 *
 * Verifies:
 *   SW-FR-EVENT-004  Reserved fault capacity is bounded and deterministic.
 *   SW-FR-EVENT-006  All-fault saturation invokes the escalation sequence.
 *   SW-FR-BM-005    The IWDG path is armed when software cannot admit a fault.
 *   SW-FR-BM-006    HAL outputs are forced safe before the simulated reset.
 *   QA-EV-01        HAL fail-safe and IWDG hooks are both exercised.
 *
 * Test ids (docs/trace/traceability.csv):
 *   HARD-FAULT-ESCALATION-001
 *   HARD-FAULT-ESCALATION-002
 */

#include "cancestry/event/queue.h"

#include "cancestry_test.h"
#include "watchdog.h"

static cancestry_event_t make_fault(uint32_t code, cancestry_time_us_t timestamp)
{
    cancestry_event_t event;

    cancestry_event_init(&event);
    event.type = CANCESTRY_EVENT_TYPE_FAULT_RAISED;
    event.priority_class = CANCESTRY_PRIORITY_CLASS_FAULT;
    event.timestamp_us = timestamp;
    event.payload.fault.fault_code = code;
    event.payload.fault.severity = CANCESTRY_FAULT_SEVERITY_CRITICAL;
    return event;
}

static void test_queue_saturation_forces_safe_state_and_iwdg(void)
{
    cancestry_event_queue_t queue;
    cancestry_event_t storage[2u];
    cancestry_watchdog_t watchdog;
    cancestry_watchdog_config_t config;
    cancestry_event_t fault;

    config.timeout_ms = 5u;
    config.enforce_safe_state_on_boot = true;
    CANCESSTRY_TEST_CHECK(cancestry_watchdog_init(&watchdog, &config));
    cancestry_hardware_authorize_tx(&watchdog, true);
    CANCESSTRY_TEST_CHECK(cancestry_hardware_tx_is_authorized(&watchdog));

    CANCESSTRY_TEST_CHECK(cancestry_event_queue_init_with_reserved_fault_slots(
        &queue, storage, 2u, 2u));
    CANCESSTRY_TEST_CHECK(cancestry_watchdog_bind_event_queue(&watchdog, &queue));

    fault = make_fault(0x100u, 1u);
    CANCESSTRY_TEST_CHECK(cancestry_event_queue_push(&queue, &fault) ==
                          CANCESTRY_EVENT_QUEUE_OK);
    fault.payload.fault.fault_code = 0x101u;
    fault.timestamp_us = 2u;
    CANCESSTRY_TEST_CHECK(cancestry_event_queue_push(&queue, &fault) ==
                          CANCESTRY_EVENT_QUEUE_OK);

    fault.payload.fault.fault_code = 0x102u;
    fault.timestamp_us = 3u;
    CANCESSTRY_TEST_CHECK(cancestry_event_queue_push(&queue, &fault) ==
                          CANCESTRY_EVENT_QUEUE_ERR_FULL_FAULT);

    /* Safe-state hook runs before the watchdog's simulated reset path. */
    CANCESSTRY_TEST_CHECK(cancestry_hardware_is_safe_state(&watchdog));
    CANCESSTRY_TEST_CHECK(!cancestry_hardware_tx_is_authorized(&watchdog));
    CANCESSTRY_TEST_CHECK(watchdog.hang_induced);
    CANCESSTRY_TEST_CHECK_U64(
        cancestry_event_queue_counters(&queue)->hard_fault_escalations, 1u);

    CANCESSTRY_TEST_CHECK(cancestry_watchdog_sim_tick(&watchdog, 5u));
    CANCESSTRY_TEST_CHECK(cancestry_watchdog_did_reset(&watchdog));
    CANCESSTRY_TEST_CHECK(cancestry_hardware_is_safe_state(&watchdog));
}

static void test_escalation_is_safe_with_a_missing_hook(void)
{
    cancestry_event_hard_fault_hooks_t hooks;
    uint32_t safe_calls = 0u;

    hooks.hal_fail_safe = NULL;
    hooks.iwdg_escalate = NULL;
    hooks.context = &safe_calls;
    CANCESSTRY_TEST_CHECK(!cancestry_event_hard_fault_escalate(&hooks));
    CANCESSTRY_TEST_CHECK_U64(safe_calls, 0u);
}

int main(void)
{
    CANCESSTRY_TEST_SUITE_BEGIN("hard fault escalation");

    CANCESSTRY_TEST_CASE("queue saturation invokes HAL safe state and IWDG");
    test_queue_saturation_forces_safe_state_and_iwdg();

    CANCESSTRY_TEST_CASE("a missing hook fails closed without a side effect");
    test_escalation_is_safe_with_a_missing_hook();

    return CANCESSTRY_TEST_SUITE_END();
}
