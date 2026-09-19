/*
 * QA-EV-01 / Issue #21 correction: Path A reserved fault slots.
 *
 * Verifies:
 *   SW-FR-EVENT-004  Bounded event queues preserve a configured fault reserve.
 *   SW-FR-EVENT-005  Reserved-slot and hard-fault saturation counters are
 *                    observable.
 *   SW-FR-EVENT-006  Fault admission remains deterministic at both boundaries.
 *   SW-FR-FSM-020    Per-FSM queues use the same reserved-slot policy.
 *   QA-EV-01         Ordinary traffic cannot consume the fault reserve and an
 *                    all-fault queue escalates instead of evicting a fault.
 *
 * Test ids (docs/trace/traceability.csv):
 *   EVENT-RESERVED-SLOTS-001
 *   EVENT-RESERVED-SLOTS-002
 *   EVENT-RESERVED-SLOTS-003
 *   EVENT-RESERVED-SLOTS-004
 *   EVENT-RESERVED-SLOTS-005
 */

#include "cancestry/event/queue.h"

#include "cancestry_test.h"

#include <stdint.h>

#define TEST_CAPACITY 4u

typedef struct hook_probe {
    uint32_t retention_calls;
    uint32_t safe_calls;
    uint32_t watchdog_calls;
    uint32_t last_retention_code;
    uint8_t order[3u];
    uint8_t order_count;
} hook_probe_t;

static cancestry_event_t make_event(cancestry_event_type_t type,
                                    cancestry_priority_class_t priority,
                                    cancestry_time_us_t timestamp,
                                    uint32_t code)
{
    cancestry_event_t event;

    cancestry_event_init(&event);
    event.type = type;
    event.priority_class = priority;
    event.timestamp_us = timestamp;
    if (priority == CANCESTRY_PRIORITY_CLASS_FAULT) {
        event.payload.fault.fault_code = code;
        event.payload.fault.severity = CANCESTRY_FAULT_SEVERITY_CRITICAL;
    } else {
        event.payload.can_rx.can_id = code;
    }
    return event;
}

static void hook_retention(uint32_t code, void *context)
{
    hook_probe_t *probe = (hook_probe_t *)context;
    probe->retention_calls++;
    probe->last_retention_code = code;
    if (probe->order_count < 3u) {
        probe->order[probe->order_count++] = 1u;
    }
}

static void hook_safe(void *context)
{
    hook_probe_t *probe = (hook_probe_t *)context;
    probe->safe_calls++;
    if (probe->order_count < 3u) {
        probe->order[probe->order_count++] = 2u;
    }
}

static void hook_watchdog(void *context)
{
    hook_probe_t *probe = (hook_probe_t *)context;
    probe->watchdog_calls++;
    if (probe->order_count < 3u) {
        probe->order[probe->order_count++] = 3u;
    }
}

static cancestry_event_hard_fault_hooks_t make_hooks(hook_probe_t *probe)
{
    cancestry_event_hard_fault_hooks_t hooks;

    hooks.write_retention_register = hook_retention;
    hooks.hal_fail_safe = hook_safe;
    hooks.iwdg_escalate = hook_watchdog;
    hooks.context = probe;
    return hooks;
}

static void test_default_reserve_blocks_ordinary_admission(void)
{
    cancestry_event_queue_t queue;
    cancestry_event_t storage[TEST_CAPACITY];
    cancestry_event_t event;
    const cancestry_event_queue_counters_t *counters;

    CANCESSTRY_TEST_CHECK(cancestry_event_queue_init(&queue, storage, TEST_CAPACITY));
    CANCESSTRY_TEST_CHECK_U64(cancestry_event_queue_reserved_fault_slots(&queue), 2u);

    event = make_event(CANCESTRY_EVENT_TYPE_CAN_RX, CANCESTRY_PRIORITY_CLASS_CAN_RX, 1u, 1u);
    CANCESSTRY_TEST_CHECK(cancestry_event_queue_push(&queue, &event) ==
                          CANCESTRY_EVENT_QUEUE_OK);
    event.timestamp_us = 2u;
    event.payload.can_rx.can_id = 2u;
    CANCESSTRY_TEST_CHECK(cancestry_event_queue_push(&queue, &event) ==
                          CANCESTRY_EVENT_QUEUE_OK);

    /* Two physical slots remain, but neither may be consumed by ordinary
     * traffic. This is the key Path A invariant. */
    event.timestamp_us = 3u;
    event.payload.can_rx.can_id = 3u;
    CANCESSTRY_TEST_CHECK(cancestry_event_queue_push(&queue, &event) ==
                          CANCESTRY_EVENT_QUEUE_ERR_RESERVED_FAULT_SLOTS);
    CANCESSTRY_TEST_CHECK_U64(cancestry_event_queue_size(&queue), 2u);
    counters = cancestry_event_queue_counters(&queue);
    CANCESSTRY_TEST_CHECK_U64(counters->dropped, 1u);
    CANCESSTRY_TEST_CHECK_U64(counters->overflow_events, 1u);
}

static void test_faults_use_reserved_slots_and_saturate_deterministically(void)
{
    cancestry_event_queue_t queue;
    cancestry_event_t storage[TEST_CAPACITY];
    cancestry_event_t event;
    hook_probe_t probe = {0u, 0u, 0u, 0u, {0u, 0u, 0u}, 0u};
    cancestry_event_hard_fault_hooks_t hooks = make_hooks(&probe);
    const cancestry_event_queue_counters_t *counters;

    CANCESSTRY_TEST_CHECK(cancestry_event_queue_init(&queue, storage, TEST_CAPACITY));
    CANCESSTRY_TEST_CHECK(cancestry_event_queue_set_hard_fault_hooks(&queue, &hooks));

    event = make_event(CANCESTRY_EVENT_TYPE_FAULT_RAISED,
                       CANCESTRY_PRIORITY_CLASS_FAULT, 10u, 10u);
    CANCESSTRY_TEST_CHECK(cancestry_event_queue_push(&queue, &event) ==
                          CANCESTRY_EVENT_QUEUE_OK);
    event.timestamp_us = 11u;
    event.payload.fault.fault_code = 11u;
    CANCESSTRY_TEST_CHECK(cancestry_event_queue_push(&queue, &event) ==
                          CANCESTRY_EVENT_QUEUE_OK);
    CANCESSTRY_TEST_CHECK_U64(cancestry_event_queue_fault_depth(&queue), 2u);

    /* The reserve admits two more faults even though ordinary traffic was
     * blocked at depth two. Once every physical slot is a fault, retain the
     * existing bounded diagnostic set and escalate in a stable order. */
    event.timestamp_us = 12u;
    event.payload.fault.fault_code = 12u;
    CANCESSTRY_TEST_CHECK(cancestry_event_queue_push(&queue, &event) ==
                          CANCESTRY_EVENT_QUEUE_OK);
    event.timestamp_us = 13u;
    event.payload.fault.fault_code = 13u;
    CANCESSTRY_TEST_CHECK(cancestry_event_queue_push(&queue, &event) ==
                          CANCESTRY_EVENT_QUEUE_OK);
    event.timestamp_us = 14u;
    event.payload.fault.fault_code = 14u;
    CANCESSTRY_TEST_CHECK(cancestry_event_queue_push(&queue, &event) ==
                          CANCESTRY_EVENT_QUEUE_ERR_FULL_FAULT);

    CANCESSTRY_TEST_CHECK_U64(cancestry_event_queue_size(&queue), TEST_CAPACITY);
    CANCESSTRY_TEST_CHECK_U64(cancestry_event_queue_fault_depth(&queue), TEST_CAPACITY);
    CANCESSTRY_TEST_CHECK_U64(probe.retention_calls, 1u);
    CANCESSTRY_TEST_CHECK_U64(probe.last_retention_code,
                              CANCESTRY_FAULT_CODE_QUEUE_SATURATION);
    CANCESSTRY_TEST_CHECK_U64(probe.safe_calls, 1u);
    CANCESSTRY_TEST_CHECK_U64(probe.watchdog_calls, 1u);
    CANCESSTRY_TEST_CHECK_U64(probe.order_count, 3u);
    CANCESSTRY_TEST_CHECK_U64(probe.order[0], 1u);
    CANCESSTRY_TEST_CHECK_U64(probe.order[1], 2u);
    CANCESSTRY_TEST_CHECK_U64(probe.order[2], 3u);
    counters = cancestry_event_queue_counters(&queue);
    CANCESSTRY_TEST_CHECK_U64(counters->hard_fault_escalations, 1u);
    CANCESSTRY_TEST_CHECK_U64(counters->dropped, 1u);
}

static void test_fault_occupancy_preserves_non_fault_share(void)
{
    cancestry_event_queue_t queue;
    cancestry_event_t storage[TEST_CAPACITY];
    cancestry_event_t event;

    CANCESSTRY_TEST_CHECK(cancestry_event_queue_init(&queue, storage, TEST_CAPACITY));

    event = make_event(CANCESTRY_EVENT_TYPE_FAULT_RAISED,
                       CANCESTRY_PRIORITY_CLASS_FAULT, 1u, 1u);
    CANCESSTRY_TEST_CHECK(cancestry_event_queue_push(&queue, &event) ==
                          CANCESTRY_EVENT_QUEUE_OK);
    event.timestamp_us = 2u;
    event.payload.fault.fault_code = 2u;
    CANCESSTRY_TEST_CHECK(cancestry_event_queue_push(&queue, &event) ==
                          CANCESTRY_EVENT_QUEUE_OK);

    /* The two ordinary slots remain usable even though two fault slots are
     * already occupied. The reserve is a non-fault-depth bound, not an
     * artificial total-depth bound. */
    event = make_event(CANCESTRY_EVENT_TYPE_CAN_RX,
                       CANCESTRY_PRIORITY_CLASS_CAN_RX, 3u, 3u);
    CANCESSTRY_TEST_CHECK(cancestry_event_queue_push(&queue, &event) ==
                          CANCESTRY_EVENT_QUEUE_OK);
    event.timestamp_us = 4u;
    event.payload.can_rx.can_id = 4u;
    CANCESSTRY_TEST_CHECK(cancestry_event_queue_push(&queue, &event) ==
                          CANCESTRY_EVENT_QUEUE_OK);
    event.timestamp_us = 5u;
    event.payload.can_rx.can_id = 5u;
    CANCESSTRY_TEST_CHECK(cancestry_event_queue_push(&queue, &event) ==
                          CANCESTRY_EVENT_QUEUE_ERR_RESERVED_FAULT_SLOTS);

    event = make_event(CANCESTRY_EVENT_TYPE_FAULT_RAISED,
                       CANCESTRY_PRIORITY_CLASS_FAULT, 6u, 6u);
    CANCESSTRY_TEST_CHECK(cancestry_event_queue_push(&queue, &event) ==
                          CANCESTRY_EVENT_QUEUE_OK_EVICTED_VICTIM);
    CANCESSTRY_TEST_CHECK_U64(cancestry_event_queue_fault_depth(&queue), 3u);
    CANCESSTRY_TEST_CHECK_U64(cancestry_event_queue_size(&queue), TEST_CAPACITY);
}

static void test_explicit_reserve_boundary_values(void)
{
    cancestry_event_queue_t queue;
    cancestry_event_t storage[TEST_CAPACITY];
    cancestry_event_t one_slot[1u];
    cancestry_event_t event;
    hook_probe_t probe = {0u, 0u, 0u, 0u, {0u, 0u, 0u}, 0u};
    cancestry_event_hard_fault_hooks_t hooks = make_hooks(&probe);

    CANCESSTRY_TEST_CHECK(!cancestry_event_queue_init_with_reserved_fault_slots(
        &queue, storage, TEST_CAPACITY, (uint16_t)(TEST_CAPACITY + 1u)));
    CANCESSTRY_TEST_CHECK(!cancestry_event_queue_is_valid(&queue));

    /* A one-slot fault-only queue is a valid explicit safety configuration:
     * ordinary traffic is refused, while one fault is still retained. */
    CANCESSTRY_TEST_CHECK(cancestry_event_queue_init_with_reserved_fault_slots(
        &queue, one_slot, 1u, 1u));
    CANCESSTRY_TEST_CHECK(cancestry_event_queue_set_hard_fault_hooks(&queue, &hooks));
    event = make_event(CANCESTRY_EVENT_TYPE_CAN_RX, CANCESTRY_PRIORITY_CLASS_CAN_RX, 1u, 1u);
    CANCESSTRY_TEST_CHECK(cancestry_event_queue_push(&queue, &event) ==
                          CANCESTRY_EVENT_QUEUE_ERR_RESERVED_FAULT_SLOTS);
    event = make_event(CANCESTRY_EVENT_TYPE_FAULT_RAISED,
                       CANCESTRY_PRIORITY_CLASS_FAULT, 2u, 2u);
    CANCESSTRY_TEST_CHECK(cancestry_event_queue_push(&queue, &event) ==
                          CANCESTRY_EVENT_QUEUE_OK);
    event.timestamp_us = 3u;
    CANCESSTRY_TEST_CHECK(cancestry_event_queue_push(&queue, &event) ==
                          CANCESTRY_EVENT_QUEUE_ERR_FULL_FAULT);
    CANCESSTRY_TEST_CHECK_U64(
        cancestry_event_queue_counters(&queue)->hard_fault_escalations, 1u);
}

static void test_fault_preempts_newest_non_fault_after_reserve_is_used(void)
{
    cancestry_event_queue_t queue;
    cancestry_event_t storage[TEST_CAPACITY];
    cancestry_event_t event;
    cancestry_event_t out;

    CANCESSTRY_TEST_CHECK(cancestry_event_queue_init(&queue, storage, TEST_CAPACITY));

    event = make_event(CANCESTRY_EVENT_TYPE_CAN_RX, CANCESTRY_PRIORITY_CLASS_CAN_RX, 1u, 1u);
    CANCESSTRY_TEST_CHECK(cancestry_event_queue_push(&queue, &event) ==
                          CANCESTRY_EVENT_QUEUE_OK);
    event.timestamp_us = 2u;
    event.payload.can_rx.can_id = 2u;
    CANCESSTRY_TEST_CHECK(cancestry_event_queue_push(&queue, &event) ==
                          CANCESTRY_EVENT_QUEUE_OK);

    event = make_event(CANCESTRY_EVENT_TYPE_FAULT_RAISED,
                       CANCESTRY_PRIORITY_CLASS_FAULT, 3u, 3u);
    CANCESSTRY_TEST_CHECK(cancestry_event_queue_push(&queue, &event) ==
                          CANCESTRY_EVENT_QUEUE_OK);
    event.timestamp_us = 4u;
    event.payload.fault.fault_code = 4u;
    CANCESSTRY_TEST_CHECK(cancestry_event_queue_push(&queue, &event) ==
                          CANCESTRY_EVENT_QUEUE_OK);

    /* A new fault cannot use a fifth slot, so the newest ordinary event (id 2)
     * is deterministically evicted. */
    event.timestamp_us = 5u;
    event.payload.fault.fault_code = 5u;
    CANCESSTRY_TEST_CHECK(cancestry_event_queue_push(&queue, &event) ==
                          CANCESTRY_EVENT_QUEUE_OK_EVICTED_VICTIM);
    CANCESSTRY_TEST_CHECK_U64(cancestry_event_queue_fault_depth(&queue), 3u);

    while (cancestry_event_queue_pop(&queue, &out) == CANCESTRY_EVENT_QUEUE_OK) {
        CANCESSTRY_TEST_CHECK(out.payload.can_rx.can_id != 2u ||
                              out.priority_class == CANCESTRY_PRIORITY_CLASS_FAULT);
    }
}

int main(void)
{
    CANCESSTRY_TEST_SUITE_BEGIN("reserved fault slots");

    CANCESSTRY_TEST_CASE("default reserved slots block ordinary admission");
    test_default_reserve_blocks_ordinary_admission();

    CANCESSTRY_TEST_CASE("fault reserve and all-fault saturation are deterministic");
    test_faults_use_reserved_slots_and_saturate_deterministically();

    CANCESSTRY_TEST_CASE("fault occupancy preserves the ordinary share");
    test_fault_occupancy_preserves_non_fault_share();

    CANCESSTRY_TEST_CASE("explicit reserve boundaries remain safe");
    test_explicit_reserve_boundary_values();

    CANCESSTRY_TEST_CASE("fault admission evicts the newest ordinary event");
    test_fault_preempts_newest_non_fault_after_reserve_is_used();

    return CANCESSTRY_TEST_SUITE_END();
}
