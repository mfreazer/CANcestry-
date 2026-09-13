/*
 * Unit tests for the bounded event queue.
 *
 * Verifies:
 *   SW-FR-EVENT-002  The software shall support event timestamps.
 *   SW-FR-EVENT-004  The software shall maintain bounded event queues.
 *   SW-FR-EVENT-005  The software shall expose event drop counters.
 *   SYS-NF-002       Bounded resource usage.
 *   SYS-IR-005       Monotonic time source for event timestamps.
 *
 * Overflow-specific behavior is covered by test_event_overflow.c.
 */

#include "cancestry/event/queue.h"

#include "cancestry_test.h"

#include <string.h>

#define TEST_CAPACITY 8u

static cancestry_event_t make_event(cancestry_event_type_t type,
                                    cancestry_priority_class_t priority_class,
                                    cancestry_time_us_t timestamp_us)
{
    cancestry_event_t event;
    cancestry_event_init(&event);
    event.type = type;
    event.priority_class = priority_class;
    event.timestamp_us = timestamp_us;
    return event;
}

static void test_init_rejects_invalid_arguments(void)
{
    cancestry_event_queue_t queue;
    cancestry_event_t storage[4];
    cancestry_event_t event;

    CANCESSTRY_TEST_CHECK(!cancestry_event_queue_init(NULL, storage, 4u));
    CANCESSTRY_TEST_CHECK(!cancestry_event_queue_init(&queue, NULL, 4u));
    CANCESSTRY_TEST_CHECK(!cancestry_event_queue_init(&queue, storage, 0u));

    /* A queue left unusable by a failed init is safe to poke at. */
    CANCESSTRY_TEST_CHECK(!cancestry_event_queue_is_valid(&queue));
    CANCESSTRY_TEST_CHECK_U64(cancestry_event_queue_capacity(&queue), 0u);
    CANCESSTRY_TEST_CHECK_U64(cancestry_event_queue_size(&queue), 0u);
    CANCESSTRY_TEST_CHECK_U64(cancestry_event_queue_fault_depth(&queue), 0u);
    CANCESSTRY_TEST_CHECK(!cancestry_event_queue_is_empty(&queue));
    CANCESSTRY_TEST_CHECK(!cancestry_event_queue_is_full(&queue));
    CANCESSTRY_TEST_CHECK(cancestry_event_queue_peek(&queue) == NULL);
    CANCESSTRY_TEST_CHECK(cancestry_event_queue_counters(&queue) == NULL);
    CANCESSTRY_TEST_CHECK(!cancestry_event_queue_overflow_is_persistent(&queue));
    CANCESSTRY_TEST_CHECK(cancestry_event_queue_pop(&queue, &event) ==
                          CANCESTRY_EVENT_QUEUE_ERR_NULL);

    event = make_event(CANCESTRY_EVENT_TYPE_CAN_RX, CANCESTRY_PRIORITY_CLASS_CAN_RX, 0u);
    CANCESSTRY_TEST_CHECK(cancestry_event_queue_push(&queue, &event) ==
                          CANCESTRY_EVENT_QUEUE_ERR_NULL);
    CANCESSTRY_TEST_CHECK(cancestry_event_queue_push(NULL, &event) ==
                          CANCESTRY_EVENT_QUEUE_ERR_NULL);
    /* A NULL event is a caller bug, never a crash. */
    CANCESSTRY_TEST_CHECK(cancestry_event_queue_push(&queue, NULL) ==
                          CANCESTRY_EVENT_QUEUE_ERR_NULL);
}

static void test_init_accepts_valid_storage(void)
{
    cancestry_event_queue_t queue;
    cancestry_event_t storage[TEST_CAPACITY];

    CANCESSTRY_TEST_CHECK(cancestry_event_queue_init(&queue, storage, TEST_CAPACITY));
    CANCESSTRY_TEST_CHECK(cancestry_event_queue_is_valid(&queue));
    CANCESSTRY_TEST_CHECK_U64(cancestry_event_queue_capacity(&queue), TEST_CAPACITY);
    CANCESSTRY_TEST_CHECK_U64(cancestry_event_queue_size(&queue), 0u);
    CANCESSTRY_TEST_CHECK(cancestry_event_queue_is_empty(&queue));
    CANCESSTRY_TEST_CHECK(!cancestry_event_queue_is_full(&queue));
    CANCESSTRY_TEST_CHECK(cancestry_event_queue_peek(&queue) == NULL);

    /* Re-initializing the same object resets it. */
    CANCESSTRY_TEST_CHECK(cancestry_event_queue_init(&queue, storage, 2u));
    CANCESSTRY_TEST_CHECK_U64(cancestry_event_queue_capacity(&queue), 2u);
}

static void test_push_pop_preserves_the_event(void)
{
    cancestry_event_queue_t queue;
    cancestry_event_t storage[TEST_CAPACITY];
    cancestry_event_t event;
    cancestry_event_t out;

    CANCESSTRY_TEST_CHECK(cancestry_event_queue_init(&queue, storage, TEST_CAPACITY));

    cancestry_event_init(&event);
    event.type = CANCESTRY_EVENT_TYPE_CAN_RX;
    event.priority_class = CANCESTRY_PRIORITY_CLASS_CAN_RX;
    event.timestamp_us = 4242u;
    event.cause_sequence = 41u;
    event.payload.can_rx.interface_id = 2u;
    event.payload.can_rx.can_id = 0x2F1u;
    event.payload.can_rx.length = 3u;
    event.payload.can_rx.data[0] = 0x01u;
    event.payload.can_rx.data[1] = 0x02u;
    event.payload.can_rx.data[2] = 0x03u;

    CANCESSTRY_TEST_CHECK(cancestry_event_queue_push(&queue, &event) == CANCESTRY_EVENT_QUEUE_OK);
    CANCESSTRY_TEST_CHECK_U64(cancestry_event_queue_size(&queue), 1u);

    CANCESSTRY_TEST_CHECK(cancestry_event_queue_peek(&queue) != NULL);
    CANCESSTRY_TEST_CHECK_U64(cancestry_event_queue_peek(&queue)->timestamp_us, 4242u);

    memset(&out, 0, sizeof(out));
    CANCESSTRY_TEST_CHECK(cancestry_event_queue_pop(&queue, &out) == CANCESTRY_EVENT_QUEUE_OK);
    CANCESSTRY_TEST_CHECK(out.type == CANCESTRY_EVENT_TYPE_CAN_RX);
    CANCESSTRY_TEST_CHECK(out.priority_class == CANCESTRY_PRIORITY_CLASS_CAN_RX);
    CANCESSTRY_TEST_CHECK_U64(out.timestamp_us, 4242u);
    CANCESSTRY_TEST_CHECK_U64(out.cause_sequence, 41u);
    CANCESSTRY_TEST_CHECK_U64(out.payload.can_rx.can_id, 0x2F1u);
    CANCESSTRY_TEST_CHECK_U64(out.payload.can_rx.interface_id, 2u);
    CANCESSTRY_TEST_CHECK_U64(out.payload.can_rx.length, 3u);
    CANCESSTRY_TEST_CHECK_U64(out.payload.can_rx.data[2], 0x03u);

    /* The caller's event is never modified by the queue. */
    CANCESSTRY_TEST_CHECK_U64(event.sequence, CANCESTRY_SEQUENCE_NONE);
    CANCESSTRY_TEST_CHECK_U64(event.event_id, CANCESTRY_EVENT_ID_NONE);

    /* Popping with a NULL destination discards the event. */
    CANCESSTRY_TEST_CHECK(cancestry_event_queue_push(&queue, &event) == CANCESTRY_EVENT_QUEUE_OK);
    CANCESSTRY_TEST_CHECK(cancestry_event_queue_pop(&queue, NULL) == CANCESTRY_EVENT_QUEUE_OK);
    CANCESSTRY_TEST_CHECK(cancestry_event_queue_is_empty(&queue));
}

static void test_sequence_and_event_id_assignment(void)
{
    cancestry_event_queue_t queue;
    cancestry_event_t storage[TEST_CAPACITY];
    cancestry_event_t event;
    cancestry_event_t out;
    uint32_t index;

    CANCESSTRY_TEST_CHECK(cancestry_event_queue_init(&queue, storage, TEST_CAPACITY));

    /* Equal timestamps and priority: sequence decides, so push order wins. */
    for (index = 0u; index < 3u; ++index) {
        event = make_event(CANCESTRY_EVENT_TYPE_CAN_RX, CANCESTRY_PRIORITY_CLASS_CAN_RX, 100u);
        event.payload.can_rx.can_id = index + 1u;
        CANCESSTRY_TEST_CHECK(cancestry_event_queue_push(&queue, &event) ==
                              CANCESTRY_EVENT_QUEUE_OK);
    }

    for (index = 0u; index < 3u; ++index) {
        CANCESSTRY_TEST_CHECK(cancestry_event_queue_pop(&queue, &out) == CANCESTRY_EVENT_QUEUE_OK);
        CANCESSTRY_TEST_CHECK_U64(out.sequence, (uint64_t)index + 1u);
        CANCESSTRY_TEST_CHECK_U64(out.event_id, (uint64_t)index + 1u);
        CANCESSTRY_TEST_CHECK_U64(out.payload.can_rx.can_id, (uint64_t)index + 1u);
    }

    /* A caller-supplied event id is preserved, for replay correlation. */
    event = make_event(CANCESTRY_EVENT_TYPE_CAN_RX, CANCESTRY_PRIORITY_CLASS_CAN_RX, 100u);
    event.event_id = 987654u;
    CANCESSTRY_TEST_CHECK(cancestry_event_queue_push(&queue, &event) == CANCESTRY_EVENT_QUEUE_OK);
    CANCESSTRY_TEST_CHECK(cancestry_event_queue_pop(&queue, &out) == CANCESTRY_EVENT_QUEUE_OK);
    CANCESSTRY_TEST_CHECK_U64(out.event_id, 987654u);
    CANCESSTRY_TEST_CHECK_U64(out.sequence, 4u);
}

static void test_malformed_events_are_rejected(void)
{
    cancestry_event_queue_t queue;
    cancestry_event_t storage[TEST_CAPACITY];
    cancestry_event_t event;
    const cancestry_event_queue_counters_t *counters;

    CANCESSTRY_TEST_CHECK(cancestry_event_queue_init(&queue, storage, TEST_CAPACITY));

    event = make_event(CANCESTRY_EVENT_TYPE_INVALID, CANCESTRY_PRIORITY_CLASS_CAN_RX, 1u);
    CANCESSTRY_TEST_CHECK(cancestry_event_queue_push(&queue, &event) ==
                          CANCESTRY_EVENT_QUEUE_ERR_EVENT);

    event = make_event(CANCESTRY_EVENT_TYPE_CAN_RX, CANCESTRY_PRIORITY_CLASS_COUNT, 1u);
    CANCESSTRY_TEST_CHECK(cancestry_event_queue_push(&queue, &event) ==
                          CANCESTRY_EVENT_QUEUE_ERR_EVENT);

    event = make_event(CANCESTRY_EVENT_TYPE_COUNT, CANCESTRY_PRIORITY_CLASS_CAN_RX, 1u);
    CANCESSTRY_TEST_CHECK(cancestry_event_queue_push(&queue, &event) ==
                          CANCESTRY_EVENT_QUEUE_ERR_EVENT);

    counters = cancestry_event_queue_counters(&queue);
    CANCESSTRY_TEST_CHECK(counters != NULL);
    CANCESSTRY_TEST_CHECK_U64(counters->rejected, 3u);
    CANCESSTRY_TEST_CHECK_U64(counters->pushed, 0u);
    CANCESSTRY_TEST_CHECK_U64(counters->dropped, 0u);
    CANCESSTRY_TEST_CHECK_U64(cancestry_event_queue_size(&queue), 0u);
}

static void test_empty_queue_behavior(void)
{
    cancestry_event_queue_t queue;
    cancestry_event_t storage[TEST_CAPACITY];
    cancestry_event_t event;

    CANCESSTRY_TEST_CHECK(cancestry_event_queue_init(&queue, storage, TEST_CAPACITY));

    CANCESSTRY_TEST_CHECK(cancestry_event_queue_peek(&queue) == NULL);
    CANCESSTRY_TEST_CHECK(cancestry_event_queue_pop(&queue, &event) ==
                          CANCESTRY_EVENT_QUEUE_ERR_EMPTY);
    CANCESSTRY_TEST_CHECK(cancestry_event_queue_pop(&queue, NULL) ==
                          CANCESTRY_EVENT_QUEUE_ERR_EMPTY);
    CANCESSTRY_TEST_CHECK_U64(cancestry_event_queue_counters(&queue)->popped, 0u);

    event = make_event(CANCESTRY_EVENT_TYPE_CAN_RX, CANCESTRY_PRIORITY_CLASS_CAN_RX, 1u);
    CANCESSTRY_TEST_CHECK(cancestry_event_queue_push(&queue, &event) == CANCESTRY_EVENT_QUEUE_OK);

    /* peek() observes the next event without removing it. */
    CANCESSTRY_TEST_CHECK(cancestry_event_queue_peek(&queue) != NULL);
    CANCESSTRY_TEST_CHECK(cancestry_event_queue_peek(&queue) != NULL);
    CANCESSTRY_TEST_CHECK_U64(cancestry_event_queue_size(&queue), 1u);

    CANCESSTRY_TEST_CHECK(cancestry_event_queue_pop(&queue, NULL) == CANCESTRY_EVENT_QUEUE_OK);
    CANCESSTRY_TEST_CHECK_U64(cancestry_event_queue_size(&queue), 0u);
    CANCESSTRY_TEST_CHECK_U64(cancestry_event_queue_counters(&queue)->popped, 1u);
}

static void test_counters_and_clear(void)
{
    cancestry_event_queue_t queue;
    cancestry_event_t storage[TEST_CAPACITY];
    cancestry_event_t event;
    const cancestry_event_queue_counters_t *counters;
    uint32_t index;

    CANCESSTRY_TEST_CHECK(cancestry_event_queue_init(&queue, storage, TEST_CAPACITY));

    for (index = 0u; index < 5u; ++index) {
        event = make_event(CANCESTRY_EVENT_TYPE_CAN_RX, CANCESTRY_PRIORITY_CLASS_CAN_RX,
                           1000u - (cancestry_time_us_t)index);
        CANCESSTRY_TEST_CHECK(cancestry_event_queue_push(&queue, &event) ==
                              CANCESTRY_EVENT_QUEUE_OK);
    }

    counters = cancestry_event_queue_counters(&queue);
    CANCESSTRY_TEST_CHECK_U64(counters->pushed, 5u);
    CANCESSTRY_TEST_CHECK_U64(counters->high_water, 5u);
    CANCESSTRY_TEST_CHECK_U64(counters->dropped, 0u);

    CANCESSTRY_TEST_CHECK(cancestry_event_queue_pop(&queue, NULL) == CANCESTRY_EVENT_QUEUE_OK);
    CANCESSTRY_TEST_CHECK_U64(cancestry_event_queue_counters(&queue)->popped, 1u);

    cancestry_event_queue_clear(&queue);
    CANCESSTRY_TEST_CHECK_U64(cancestry_event_queue_size(&queue), 0u);
    CANCESSTRY_TEST_CHECK(cancestry_event_queue_is_empty(&queue));

    /* Counters survive a clear: overflow history must not be lost. */
    counters = cancestry_event_queue_counters(&queue);
    CANCESSTRY_TEST_CHECK_U64(counters->pushed, 5u);
    CANCESSTRY_TEST_CHECK_U64(counters->popped, 1u);
    CANCESSTRY_TEST_CHECK_U64(counters->high_water, 5u);

    cancestry_event_queue_clear(NULL); /* shall not fault */
}

static void test_emit_stamps_events_from_the_injected_clock(void)
{
    cancestry_event_queue_t queue;
    cancestry_event_t storage[TEST_CAPACITY];
    cancestry_virtual_clock_t vclock;
    cancestry_clock_t clock;
    cancestry_event_t out;
    cancestry_event_payload_t payload;

    CANCESSTRY_TEST_CHECK(cancestry_event_queue_init(&queue, storage, TEST_CAPACITY));
    cancestry_virtual_clock_init(&vclock, 10000u);
    clock = cancestry_clock_from_virtual(&vclock);

    memset(&payload, 0, sizeof(payload));
    payload.can_rx.can_id = 0x100u;

    CANCESSTRY_TEST_CHECK(cancestry_event_queue_emit(&queue, &clock,
                                                     CANCESTRY_EVENT_TYPE_CAN_RX,
                                                     CANCESTRY_PRIORITY_CLASS_CAN_RX,
                                                     CANCESTRY_SEQUENCE_NONE,
                                                     &payload) == CANCESTRY_EVENT_QUEUE_OK);

    cancestry_virtual_clock_advance(&vclock, 555u);
    CANCESSTRY_TEST_CHECK(cancestry_event_queue_emit(&queue, &clock,
                                                     CANCESTRY_EVENT_TYPE_CAN_RX,
                                                     CANCESTRY_PRIORITY_CLASS_CAN_RX,
                                                     CANCESTRY_SEQUENCE_NONE,
                                                     NULL) == CANCESTRY_EVENT_QUEUE_OK);

    CANCESSTRY_TEST_CHECK(cancestry_event_queue_pop(&queue, &out) == CANCESTRY_EVENT_QUEUE_OK);
    CANCESSTRY_TEST_CHECK_U64(out.timestamp_us, 10000u);
    CANCESSTRY_TEST_CHECK_U64(out.payload.can_rx.can_id, 0x100u);

    CANCESSTRY_TEST_CHECK(cancestry_event_queue_pop(&queue, &out) == CANCESTRY_EVENT_QUEUE_OK);
    CANCESSTRY_TEST_CHECK_U64(out.timestamp_us, 10555u);

    /* The runtime never invents time: a missing or invalid clock is an error. */
    CANCESSTRY_TEST_CHECK(cancestry_event_queue_emit(&queue, NULL, CANCESTRY_EVENT_TYPE_CAN_RX,
                                                     CANCESTRY_PRIORITY_CLASS_CAN_RX,
                                                     CANCESTRY_SEQUENCE_NONE,
                                                     NULL) == CANCESTRY_EVENT_QUEUE_ERR_NULL);
    {
        cancestry_clock_t invalid_clock;
        invalid_clock.now_us = NULL;
        invalid_clock.context = NULL;
        CANCESSTRY_TEST_CHECK(!cancestry_clock_is_valid(&invalid_clock));
        CANCESSTRY_TEST_CHECK(cancestry_event_queue_emit(&queue, &invalid_clock,
                                                         CANCESTRY_EVENT_TYPE_CAN_RX,
                                                         CANCESTRY_PRIORITY_CLASS_CAN_RX,
                                                         CANCESTRY_SEQUENCE_NONE,
                                                         NULL) == CANCESTRY_EVENT_QUEUE_ERR_NULL);
    }
}

static void test_emit_generated_follows_the_generated_event_rules(void)
{
    cancestry_event_queue_t queue;
    cancestry_event_t storage[TEST_CAPACITY];
    cancestry_event_t cause;
    cancestry_event_t out;

    CANCESSTRY_TEST_CHECK(cancestry_event_queue_init(&queue, storage, TEST_CAPACITY));

    cause = make_event(CANCESTRY_EVENT_TYPE_CAN_RX, CANCESTRY_PRIORITY_CLASS_CAN_RX, 2000u);
    CANCESSTRY_TEST_CHECK(cancestry_event_queue_push(&queue, &cause) == CANCESTRY_EVENT_QUEUE_OK);
    CANCESSTRY_TEST_CHECK(cancestry_event_queue_pop(&queue, &cause) == CANCESTRY_EVENT_QUEUE_OK);
    CANCESSTRY_TEST_CHECK_U64(cause.sequence, 1u);

    /* No delay: the generated event inherits the causing timestamp. */
    CANCESSTRY_TEST_CHECK(cancestry_event_queue_emit_generated(&queue, &cause,
                                                               CANCESTRY_EVENT_TYPE_SIGNAL_CHANGED,
                                                               NULL, 0u) ==
                          CANCESTRY_EVENT_QUEUE_OK);
    CANCESSTRY_TEST_CHECK(cancestry_event_queue_pop(&queue, &out) == CANCESTRY_EVENT_QUEUE_OK);
    CANCESSTRY_TEST_CHECK(out.priority_class == CANCESTRY_PRIORITY_CLASS_GENERATED);
    CANCESSTRY_TEST_CHECK_U64(out.timestamp_us, 2000u);
    CANCESSTRY_TEST_CHECK_U64(out.cause_sequence, cause.sequence);
    CANCESSTRY_TEST_CHECK(out.type == CANCESTRY_EVENT_TYPE_SIGNAL_CHANGED);

    /* Explicit delay: the timestamp is the cause timestamp plus the delay. */
    CANCESSTRY_TEST_CHECK(cancestry_event_queue_emit_generated(&queue, &cause,
                                                               CANCESTRY_EVENT_TYPE_SIGNAL_CHANGED,
                                                               NULL, 250u) ==
                          CANCESTRY_EVENT_QUEUE_OK);
    CANCESSTRY_TEST_CHECK(cancestry_event_queue_pop(&queue, &out) == CANCESTRY_EVENT_QUEUE_OK);
    CANCESSTRY_TEST_CHECK_U64(out.timestamp_us, 2250u);

    CANCESSTRY_TEST_CHECK(cancestry_event_queue_emit_generated(&queue, NULL,
                                                               CANCESTRY_EVENT_TYPE_SIGNAL_CHANGED,
                                                               NULL, 0u) ==
                          CANCESTRY_EVENT_QUEUE_ERR_NULL);
    CANCESSTRY_TEST_CHECK(cancestry_event_queue_emit_generated(NULL, &cause,
                                                               CANCESTRY_EVENT_TYPE_SIGNAL_CHANGED,
                                                               NULL, 0u) ==
                          CANCESTRY_EVENT_QUEUE_ERR_NULL);
}

static void test_fill_to_capacity_within_caller_storage(void)
{
    /* The storage is exactly TEST_CAPACITY events: any write past the end is
     * caught by AddressSanitizer. */
    cancestry_event_queue_t queue;
    cancestry_event_t storage[TEST_CAPACITY];
    cancestry_event_t event;
    uint32_t index;

    CANCESSTRY_TEST_CHECK(cancestry_event_queue_init(&queue, storage, TEST_CAPACITY));

    for (index = 0u; index < TEST_CAPACITY; ++index) {
        event = make_event(CANCESTRY_EVENT_TYPE_CAN_RX, CANCESTRY_PRIORITY_CLASS_CAN_RX,
                           (cancestry_time_us_t)(TEST_CAPACITY - index));
        event.payload.can_rx.can_id = index;
        CANCESSTRY_TEST_CHECK(cancestry_event_queue_push(&queue, &event) ==
                              CANCESTRY_EVENT_QUEUE_OK);
    }
    CANCESSTRY_TEST_CHECK(cancestry_event_queue_is_full(&queue));
    CANCESSTRY_TEST_CHECK_U64(cancestry_event_queue_counters(&queue)->high_water, TEST_CAPACITY);

    for (index = 0u; index < TEST_CAPACITY; ++index) {
        CANCESSTRY_TEST_CHECK(cancestry_event_queue_pop(&queue, &event) ==
                              CANCESTRY_EVENT_QUEUE_OK);
        CANCESSTRY_TEST_CHECK_U64(event.timestamp_us, (uint64_t)index + 1u);
    }
    CANCESSTRY_TEST_CHECK(cancestry_event_queue_is_empty(&queue));
}

int main(void)
{
    CANCESSTRY_TEST_SUITE_BEGIN("event queue");

    CANCESSTRY_TEST_CASE("init rejects invalid arguments");
    test_init_rejects_invalid_arguments();

    CANCESSTRY_TEST_CASE("init accepts caller-owned storage");
    test_init_accepts_valid_storage();

    CANCESSTRY_TEST_CASE("push and pop preserve the event");
    test_push_pop_preserves_the_event();

    CANCESSTRY_TEST_CASE("sequence and event id assignment");
    test_sequence_and_event_id_assignment();

    CANCESSTRY_TEST_CASE("malformed events are rejected and counted");
    test_malformed_events_are_rejected();

    CANCESSTRY_TEST_CASE("empty queue behavior");
    test_empty_queue_behavior();

    CANCESSTRY_TEST_CASE("counters and clear");
    test_counters_and_clear();

    CANCESSTRY_TEST_CASE("emit stamps events from the injected clock");
    test_emit_stamps_events_from_the_injected_clock();

    CANCESSTRY_TEST_CASE("generated events follow section 8 of the ordering spec");
    test_emit_generated_follows_the_generated_event_rules();

    CANCESSTRY_TEST_CASE("filling to capacity stays inside caller storage");
    test_fill_to_capacity_within_caller_storage();

    return CANCESSTRY_TEST_SUITE_END();
}
