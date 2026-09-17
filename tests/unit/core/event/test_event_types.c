/*
 * Unit tests for the common event structure.
 *
 * Verifies:
 *   SW-FR-EVENT-001  The software shall define a common event structure.
 *   SW-FR-EVENT-002  The software shall support event timestamps.
 *   SW-FR-EVENT-003  can_rx, signal_changed, timer_expired, state_entered,
 *                    state_exited, fault_raised, power_mode_changed.
 *   SYS-NF-002       Bounded resource usage (fixed-size event value type).
 *
 * Normative source: docs/system/event-ordering.md sections 1, 3, 4.
 *
 * Test ids (docs/trace/traceability.csv):
 *   EVENT-TYPES-001  SW-FR-EVENT-001
 *   EVENT-TYPES-002  SW-FR-EVENT-003
 */

#include "cancestry/event/types.h"

#include "cancestry_test.h"

#include <string.h>

/* The enumeration values are normative; they are asserted at compile time. */
CANCESSTRY_TEST_STATIC_ASSERT(CANCESTRY_PRIORITY_CLASS_FAULT == 0);
CANCESSTRY_TEST_STATIC_ASSERT(CANCESTRY_PRIORITY_CLASS_MODE == 1);
CANCESSTRY_TEST_STATIC_ASSERT(CANCESTRY_PRIORITY_CLASS_TIMER == 2);
CANCESSTRY_TEST_STATIC_ASSERT(CANCESTRY_PRIORITY_CLASS_CAN_RX == 3);
CANCESSTRY_TEST_STATIC_ASSERT(CANCESTRY_PRIORITY_CLASS_HOST == 4);
CANCESSTRY_TEST_STATIC_ASSERT(CANCESTRY_PRIORITY_CLASS_GENERATED == 5);
CANCESSTRY_TEST_STATIC_ASSERT(CANCESTRY_PRIORITY_CLASS_TRACE == 6);

CANCESSTRY_TEST_STATIC_ASSERT(CANCESTRY_EVENT_TYPE_CAN_RX == 1);
CANCESSTRY_TEST_STATIC_ASSERT(CANCESTRY_EVENT_TYPE_POWER_MODE_CHANGED == 7);

CANCESSTRY_TEST_STATIC_ASSERT(sizeof(cancestry_event_t) >= sizeof(cancestry_event_payload_t));

static cancestry_event_t make_event(cancestry_event_type_t type,
                                    cancestry_priority_class_t priority_class,
                                    cancestry_time_us_t timestamp_us,
                                    cancestry_sequence_t sequence)
{
    cancestry_event_t event;
    cancestry_event_init(&event);
    event.type = type;
    event.priority_class = priority_class;
    event.timestamp_us = timestamp_us;
    event.sequence = sequence;
    return event;
}

static void test_priority_class_values(void)
{
    CANCESSTRY_TEST_CHECK((int)CANCESTRY_PRIORITY_CLASS_FAULT == 0);
    CANCESSTRY_TEST_CHECK((int)CANCESTRY_PRIORITY_CLASS_MODE == 1);
    CANCESSTRY_TEST_CHECK((int)CANCESTRY_PRIORITY_CLASS_TIMER == 2);
    CANCESSTRY_TEST_CHECK((int)CANCESTRY_PRIORITY_CLASS_CAN_RX == 3);
    CANCESSTRY_TEST_CHECK((int)CANCESTRY_PRIORITY_CLASS_HOST == 4);
    CANCESSTRY_TEST_CHECK((int)CANCESTRY_PRIORITY_CLASS_GENERATED == 5);
    CANCESSTRY_TEST_CHECK((int)CANCESTRY_PRIORITY_CLASS_TRACE == 6);

    /* Lower number means higher priority. */
    CANCESSTRY_TEST_CHECK(CANCESTRY_PRIORITY_CLASS_FAULT < CANCESTRY_PRIORITY_CLASS_TRACE);
    CANCESSTRY_TEST_CHECK(cancestry_priority_class_is_valid(CANCESTRY_PRIORITY_CLASS_FAULT));
    CANCESSTRY_TEST_CHECK(cancestry_priority_class_is_valid(CANCESTRY_PRIORITY_CLASS_TRACE));
    CANCESSTRY_TEST_CHECK(!cancestry_priority_class_is_valid(CANCESTRY_PRIORITY_CLASS_COUNT));
    CANCESSTRY_TEST_CHECK(!cancestry_priority_class_is_valid(
        (cancestry_priority_class_t)(CANCESTRY_PRIORITY_CLASS_COUNT + 1)));
}

static void test_event_type_coverage(void)
{
    /* SW-FR-EVENT-003: all seven normative types exist and are distinguishable. */
    const cancestry_event_type_t expected[] = {
        CANCESTRY_EVENT_TYPE_CAN_RX,             CANCESTRY_EVENT_TYPE_SIGNAL_CHANGED,
        CANCESTRY_EVENT_TYPE_TIMER_EXPIRED,      CANCESTRY_EVENT_TYPE_STATE_ENTERED,
        CANCESTRY_EVENT_TYPE_STATE_EXITED,       CANCESTRY_EVENT_TYPE_FAULT_RAISED,
        CANCESTRY_EVENT_TYPE_POWER_MODE_CHANGED
    };
    size_t i;

    for (i = 0u; i < sizeof(expected) / sizeof(expected[0]); ++i) {
        CANCESSTRY_TEST_CHECK(cancestry_event_type_is_valid(expected[i]));
    }
    CANCESSTRY_TEST_CHECK(!cancestry_event_type_is_valid(CANCESTRY_EVENT_TYPE_INVALID));
    CANCESSTRY_TEST_CHECK(!cancestry_event_type_is_valid(CANCESTRY_EVENT_TYPE_COUNT));
    CANCESSTRY_TEST_CHECK(!cancestry_event_type_is_valid(
        (cancestry_event_type_t)(CANCESTRY_EVENT_TYPE_COUNT + 1)));

    CANCESSTRY_TEST_CHECK_STRING(cancestry_event_type_name(CANCESTRY_EVENT_TYPE_CAN_RX), "can_rx");
    CANCESSTRY_TEST_CHECK_STRING(cancestry_event_type_name(CANCESTRY_EVENT_TYPE_SIGNAL_CHANGED),
                                 "signal_changed");
    CANCESSTRY_TEST_CHECK_STRING(cancestry_event_type_name(CANCESTRY_EVENT_TYPE_TIMER_EXPIRED),
                                 "timer_expired");
    CANCESSTRY_TEST_CHECK_STRING(cancestry_event_type_name(CANCESTRY_EVENT_TYPE_STATE_ENTERED),
                                 "state_entered");
    CANCESSTRY_TEST_CHECK_STRING(cancestry_event_type_name(CANCESTRY_EVENT_TYPE_STATE_EXITED),
                                 "state_exited");
    CANCESSTRY_TEST_CHECK_STRING(cancestry_event_type_name(CANCESTRY_EVENT_TYPE_FAULT_RAISED),
                                 "fault_raised");
    CANCESSTRY_TEST_CHECK_STRING(
        cancestry_event_type_name(CANCESTRY_EVENT_TYPE_POWER_MODE_CHANGED), "power_mode_changed");
    CANCESSTRY_TEST_CHECK_STRING(cancestry_event_type_name(CANCESTRY_EVENT_TYPE_INVALID), "invalid");

    CANCESSTRY_TEST_CHECK_STRING(cancestry_priority_class_name(CANCESTRY_PRIORITY_CLASS_GENERATED),
                                 "GENERATED");
    CANCESSTRY_TEST_CHECK_STRING(cancestry_fault_severity_name(CANCESTRY_FAULT_SEVERITY_CRITICAL),
                                 "CRITICAL");
    CANCESSTRY_TEST_CHECK_STRING(cancestry_mode_name(CANCESTRY_MODE_LISTEN_ONLY), "LISTEN_ONLY");
}

static void test_event_init_and_validity(void)
{
    cancestry_event_t event;
    const cancestry_event_t zero_reference = {0};

    cancestry_event_init(NULL); /* shall not fault */

    cancestry_event_init(&event);
    CANCESSTRY_TEST_CHECK(event.type == CANCESTRY_EVENT_TYPE_INVALID);
    CANCESSTRY_TEST_CHECK_U64(event.timestamp_us, 0u);
    CANCESSTRY_TEST_CHECK_U64(event.sequence, CANCESTRY_SEQUENCE_NONE);
    CANCESSTRY_TEST_CHECK_U64(event.cause_sequence, CANCESTRY_SEQUENCE_NONE);
    CANCESSTRY_TEST_CHECK_U64(event.event_id, CANCESTRY_EVENT_ID_NONE);
    CANCESSTRY_TEST_CHECK_U64(memcmp(&event, &zero_reference, sizeof(event)), 0u);

    CANCESSTRY_TEST_CHECK(!cancestry_event_is_valid(NULL));
    CANCESSTRY_TEST_CHECK(!cancestry_event_is_valid(&event));

    event.type = CANCESTRY_EVENT_TYPE_CAN_RX;
    event.priority_class = CANCESTRY_PRIORITY_CLASS_CAN_RX;
    CANCESSTRY_TEST_CHECK(cancestry_event_is_valid(&event));

    event.priority_class = CANCESTRY_PRIORITY_CLASS_COUNT;
    CANCESSTRY_TEST_CHECK(!cancestry_event_is_valid(&event));
}

static void test_selection_order(void)
{
    cancestry_event_t early;
    cancestry_event_t late;
    cancestry_event_t high_priority;
    cancestry_event_t low_priority;
    cancestry_event_t first_sequence;
    cancestry_event_t second_sequence;

    /* 1. timestamp_us ascending. */
    early = make_event(CANCESTRY_EVENT_TYPE_CAN_RX, CANCESTRY_PRIORITY_CLASS_CAN_RX, 100u, 9u);
    late = make_event(CANCESTRY_EVENT_TYPE_CAN_RX, CANCESTRY_PRIORITY_CLASS_CAN_RX, 101u, 1u);
    CANCESSTRY_TEST_CHECK(cancestry_event_compare_order(&early, &late) < 0);
    CANCESSTRY_TEST_CHECK(cancestry_event_compare_order(&late, &early) > 0);

    /* 2. priority_class ascending when timestamps tie. */
    high_priority = make_event(CANCESTRY_EVENT_TYPE_FAULT_RAISED, CANCESTRY_PRIORITY_CLASS_FAULT,
                               100u, 9u);
    low_priority =
        make_event(CANCESTRY_EVENT_TYPE_CAN_RX, CANCESTRY_PRIORITY_CLASS_TRACE, 100u, 1u);
    CANCESSTRY_TEST_CHECK(cancestry_event_compare_order(&high_priority, &low_priority) < 0);
    CANCESSTRY_TEST_CHECK(cancestry_event_compare_order(&low_priority, &high_priority) > 0);

    /* 3. sequence ascending when timestamp and priority tie. */
    first_sequence = make_event(CANCESTRY_EVENT_TYPE_CAN_RX, CANCESTRY_PRIORITY_CLASS_CAN_RX, 100u,
                                1u);
    second_sequence = make_event(CANCESTRY_EVENT_TYPE_CAN_RX, CANCESTRY_PRIORITY_CLASS_CAN_RX, 100u,
                                 2u);
    CANCESSTRY_TEST_CHECK(cancestry_event_compare_order(&first_sequence, &second_sequence) < 0);
    CANCESSTRY_TEST_CHECK(cancestry_event_compare_order(&second_sequence, &first_sequence) > 0);

    /* Identical keys compare equal, and every event compares equal to itself. */
    CANCESSTRY_TEST_CHECK(cancestry_event_compare_order(&first_sequence, &first_sequence) == 0);
    CANCESSTRY_TEST_CHECK(cancestry_event_compare_order(NULL, NULL) == 0);
}

static void test_typed_payloads(void)
{
    cancestry_event_t event;
    const uint8_t frame[] = {0x11u, 0x22u, 0x33u, 0x44u};

    cancestry_event_init(&event);
    event.type = CANCESTRY_EVENT_TYPE_CAN_RX;
    event.priority_class = CANCESTRY_PRIORITY_CLASS_CAN_RX;
    event.payload.can_rx.interface_id = 1u;
    event.payload.can_rx.can_id = 0x1A0u;
    event.payload.can_rx.is_extended = 0u;
    event.payload.can_rx.length = (uint8_t)sizeof(frame);
    memcpy(event.payload.can_rx.data, frame, sizeof(frame));
    CANCESSTRY_TEST_CHECK_U64(event.payload.can_rx.can_id, 0x1A0u);
    CANCESSTRY_TEST_CHECK_U64(event.payload.can_rx.length, 4u);
    CANCESSTRY_TEST_CHECK_U64(event.payload.can_rx.data[3], 0x44u);

    /* signal_changed carries old and new tagged values. */
    cancestry_event_init(&event);
    event.type = CANCESTRY_EVENT_TYPE_SIGNAL_CHANGED;
    event.priority_class = CANCESTRY_PRIORITY_CLASS_GENERATED;
    event.payload.signal_changed.signal_id = 7u;
    event.payload.signal_changed.signal_name = "VehicleSpeed";
    event.payload.signal_changed.old_value.kind = CANCESTRY_VALUE_KIND_REAL;
    event.payload.signal_changed.old_value.value.real = 41.5;
    event.payload.signal_changed.new_value.kind = CANCESTRY_VALUE_KIND_REAL;
    event.payload.signal_changed.new_value.value.real = 42.5;
    CANCESSTRY_TEST_CHECK_STRING(event.payload.signal_changed.signal_name, "VehicleSpeed");
    CANCESSTRY_TEST_CHECK_DOUBLE(event.payload.signal_changed.new_value.value.real, 42.5, 0.000001);
    CANCESSTRY_TEST_CHECK(event.payload.signal_changed.old_value.kind == CANCESTRY_VALUE_KIND_REAL);

    /* timer_expired, state and fault payloads are reachable through the union. */
    cancestry_event_init(&event);
    event.type = CANCESTRY_EVENT_TYPE_TIMER_EXPIRED;
    event.payload.timer_expired.timer_id = 3u;
    event.payload.timer_expired.instance_id = 12u;
    event.payload.timer_expired.missed_count = 2u;
    event.payload.timer_expired.is_periodic = 1u;
    CANCESSTRY_TEST_CHECK_U64(event.payload.timer_expired.missed_count, 2u);

    cancestry_event_init(&event);
    event.type = CANCESTRY_EVENT_TYPE_STATE_ENTERED;
    event.payload.state.instance_id = 4u;
    event.payload.state.state_id = 5u;
    event.payload.state.state_name = "ACTIVE";
    CANCESSTRY_TEST_CHECK_STRING(event.payload.state.state_name, "ACTIVE");

    cancestry_event_init(&event);
    event.type = CANCESTRY_EVENT_TYPE_FAULT_RAISED;
    event.priority_class = CANCESTRY_PRIORITY_CLASS_FAULT;
    event.payload.fault.fault_code = 0x1234u;
    event.payload.fault.severity = CANCESTRY_FAULT_SEVERITY_CRITICAL;
    event.payload.fault.source_id = 9u;
    CANCESSTRY_TEST_CHECK(event.payload.fault.severity == CANCESTRY_FAULT_SEVERITY_CRITICAL);

    cancestry_event_init(&event);
    event.type = CANCESTRY_EVENT_TYPE_POWER_MODE_CHANGED;
    event.priority_class = CANCESTRY_PRIORITY_CLASS_MODE;
    event.payload.mode.from_mode = CANCESTRY_MODE_ACTIVE;
    event.payload.mode.to_mode = CANCESTRY_MODE_SAFE;
    CANCESSTRY_TEST_CHECK(event.payload.mode.from_mode == CANCESTRY_MODE_ACTIVE);
    CANCESSTRY_TEST_CHECK(event.payload.mode.to_mode == CANCESTRY_MODE_SAFE);
}

static void test_event_is_a_value_type(void)
{
    cancestry_event_t source;
    cancestry_event_t copy;

    cancestry_event_init(&source);
    source.type = CANCESTRY_EVENT_TYPE_CAN_RX;
    source.priority_class = CANCESTRY_PRIORITY_CLASS_CAN_RX;
    source.timestamp_us = 1234u;
    source.sequence = 7u;
    source.cause_sequence = 6u;
    source.payload.can_rx.can_id = 0x123u;
    source.payload.can_rx.length = 2u;
    source.payload.can_rx.data[0] = 0xABu;
    source.payload.can_rx.data[1] = 0xCDu;

    copy = source; /* plain assignment: no ownership, no allocation */
    CANCESSTRY_TEST_CHECK_U64(copy.sequence, 7u);
    CANCESSTRY_TEST_CHECK_U64(copy.cause_sequence, 6u);
    CANCESSTRY_TEST_CHECK_U64(copy.payload.can_rx.can_id, 0x123u);
    CANCESSTRY_TEST_CHECK_U64(copy.payload.can_rx.data[1], 0xCDu);
    CANCESSTRY_TEST_CHECK(cancestry_event_compare_order(&copy, &source) == 0);

    source.payload.can_rx.can_id = 0u;
    CANCESSTRY_TEST_CHECK_U64(copy.payload.can_rx.can_id, 0x123u);
}

int main(void)
{
    CANCESSTRY_TEST_SUITE_BEGIN("event types");

    CANCESSTRY_TEST_CASE("priority class values match the normative table");
    test_priority_class_values();

    CANCESSTRY_TEST_CASE("event types cover the normative set");
    test_event_type_coverage();

    CANCESSTRY_TEST_CASE("event_init produces a known invalid state");
    test_event_init_and_validity();

    CANCESSTRY_TEST_CASE("selection order is timestamp, priority, sequence");
    test_selection_order();

    CANCESSTRY_TEST_CASE("payload union carries typed payloads");
    test_typed_payloads();

    CANCESSTRY_TEST_CASE("events are copyable value types");
    test_event_is_a_value_type();

    return CANCESSTRY_TEST_SUITE_END();
}
