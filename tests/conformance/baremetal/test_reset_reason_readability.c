/*
 * QA-P31-04: boot replay of a retained hard-fault reset reason.
 *
 * Verifies SW-FR-LOG-004, SYS-FR-016 and SYS-SF-004: a retained queue
 * saturation code is injected into the persistent fault log and cleared only
 * after successful admission.
 *
 * Test id: RESET-REASON-001.
 */

#include "cancestry/event/queue.h"
#include "startup.h"

#include "cancestry_test.h"

int main(void)
{
    cancestry_watchdog_t watchdog;
    cancestry_watchdog_config_t config;
    cancestry_event_queue_t fault_log;
    cancestry_event_t storage[4u];
    cancestry_event_t event;

    CANCESSTRY_TEST_SUITE_BEGIN("retained reset reason");
    CANCESSTRY_TEST_CASE("RESET-REASON-001: boot replays and clears retention");

    config.timeout_ms = 5u;
    config.enforce_safe_state_on_boot = true;
    CANCESSTRY_TEST_CHECK(cancestry_watchdog_init(&watchdog, &config));
    CANCESSTRY_TEST_CHECK(cancestry_event_queue_init_with_reserved_fault_slots(
        &fault_log, storage, 4u, 2u));

    cancestry_watchdog_write_retention_register(
        CANCESTRY_FAULT_CODE_QUEUE_SATURATION, &watchdog);
    CANCESSTRY_TEST_CHECK_U64(
        cancestry_watchdog_read_retention_register(&watchdog),
        CANCESTRY_FAULT_CODE_QUEUE_SATURATION);

    CANCESSTRY_TEST_CHECK(cancestry_cortex_m_startup_restore_retained_fault(
        &watchdog, &fault_log));
    CANCESSTRY_TEST_CHECK_U64(cancestry_watchdog_read_retention_register(&watchdog), 0u);
    CANCESSTRY_TEST_CHECK(cancestry_event_queue_pop(&fault_log, &event) ==
                          CANCESTRY_EVENT_QUEUE_OK);
    CANCESSTRY_TEST_CHECK(event.type == CANCESTRY_EVENT_TYPE_FAULT_RAISED);
    CANCESSTRY_TEST_CHECK(event.priority_class == CANCESTRY_PRIORITY_CLASS_FAULT);
    CANCESSTRY_TEST_CHECK_U64(event.payload.fault.fault_code,
                              CANCESTRY_FAULT_CODE_QUEUE_SATURATION);
    CANCESSTRY_TEST_CHECK_U64(event.payload.fault.source_id,
                              CANCESTRY_FAULT_SOURCE_HARD_FAULT_ESCALATION);
    CANCESSTRY_TEST_CHECK(event.payload.fault.severity ==
                          CANCESTRY_FAULT_SEVERITY_CRITICAL);
    CANCESSTRY_TEST_CHECK(!cancestry_cortex_m_startup_restore_retained_fault(
        &watchdog, &fault_log));

    return CANCESSTRY_TEST_SUITE_END();
}
