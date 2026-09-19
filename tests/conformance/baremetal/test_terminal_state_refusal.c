/*
 * QA-P31-02: terminal queue refusal after hard-fault escalation.
 *
 * Verifies SW-FR-EVENT-008, SYS-SF-004 and SYS-FR-016: once the bounded
 * fault queue has entered terminal hard-fault handling, no producer can add a
 * later ordinary or fault event and the escalation actions are not repeated.
 *
 * Test id: TERMINAL-STATE-001.
 */

#include "cancestry/event/queue.h"

#include "cancestry_test.h"

#include <stdint.h>

typedef struct probe {
    uint32_t retention_code;
    uint32_t safe_calls;
    uint32_t watchdog_calls;
} probe_t;

static void retain(uint32_t code, void *context)
{
    probe_t *probe = (probe_t *)context;
    probe->retention_code = code;
}

static void safe(void *context)
{
    probe_t *probe = (probe_t *)context;
    probe->safe_calls++;
}

static void watchdog(void *context)
{
    probe_t *probe = (probe_t *)context;
    probe->watchdog_calls++;
}

static cancestry_event_t make_fault(uint32_t code)
{
    cancestry_event_t event;

    cancestry_event_init(&event);
    event.type = CANCESTRY_EVENT_TYPE_FAULT_RAISED;
    event.priority_class = CANCESTRY_PRIORITY_CLASS_FAULT;
    event.timestamp_us = code;
    event.payload.fault.fault_code = code;
    event.payload.fault.severity = CANCESTRY_FAULT_SEVERITY_CRITICAL;
    return event;
}

static cancestry_event_t make_ordinary(void)
{
    cancestry_event_t event;

    cancestry_event_init(&event);
    event.type = CANCESTRY_EVENT_TYPE_CAN_RX;
    event.priority_class = CANCESTRY_PRIORITY_CLASS_CAN_RX;
    event.timestamp_us = 20u;
    event.payload.can_rx.can_id = 0x123u;
    return event;
}

int main(void)
{
    cancestry_event_queue_t queue;
    cancestry_event_t storage[1u];
    cancestry_event_hard_fault_hooks_t hooks;
    cancestry_event_t event;
    probe_t probe = {0u, 0u, 0u};

    CANCESSTRY_TEST_SUITE_BEGIN("terminal hard-fault state");
    CANCESSTRY_TEST_CASE("TERMINAL-STATE-001: escalation refuses all later pushes");

    hooks.write_retention_register = retain;
    hooks.hal_fail_safe = safe;
    hooks.iwdg_escalate = watchdog;
    hooks.context = &probe;

    CANCESSTRY_TEST_CHECK(cancestry_event_queue_init_with_reserved_fault_slots(
        &queue, storage, 1u, 1u));
    CANCESSTRY_TEST_CHECK(cancestry_event_queue_set_hard_fault_hooks(&queue, &hooks));

    event = make_fault(1u);
    CANCESSTRY_TEST_CHECK(cancestry_event_queue_push(&queue, &event) ==
                          CANCESTRY_EVENT_QUEUE_OK);
    event = make_fault(2u);
    CANCESSTRY_TEST_CHECK(cancestry_event_queue_push(&queue, &event) ==
                          CANCESTRY_EVENT_QUEUE_ERR_FULL_FAULT);
    CANCESSTRY_TEST_CHECK(cancestry_event_queue_is_terminal(&queue));
    CANCESSTRY_TEST_CHECK_U64(probe.retention_code,
                              CANCESTRY_FAULT_CODE_QUEUE_SATURATION);

    event = make_ordinary();
    CANCESSTRY_TEST_CHECK(cancestry_event_queue_push(&queue, &event) ==
                          CANCESTRY_EVENT_QUEUE_ERR_TERMINAL);
    event = make_fault(3u);
    CANCESSTRY_TEST_CHECK(cancestry_event_queue_push(&queue, &event) ==
                          CANCESTRY_EVENT_QUEUE_ERR_TERMINAL);
    CANCESSTRY_TEST_CHECK_U64(
        cancestry_event_queue_counters(&queue)->hard_fault_escalations, 1u);
    CANCESSTRY_TEST_CHECK_U64(probe.safe_calls, 1u);
    CANCESSTRY_TEST_CHECK_U64(probe.watchdog_calls, 1u);

    return CANCESSTRY_TEST_SUITE_END();
}
