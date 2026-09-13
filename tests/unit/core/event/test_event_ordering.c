/*
 * Unit tests for deterministic event ordering.
 *
 * Verifies:
 *   SW-FR-EVENT-006  The software shall process events according to the
 *                    normative event-ordering specification.
 *   SYS-NF-001       Same input sequence produces same output sequence.
 *   SW-FR-FSM-046    Deterministic behavior for the same event sequence.
 *
 * Normative source: docs/system/event-ordering.md sections 4 and 8:
 *   1. timestamp_us ascending
 *   2. priority_class ascending
 *   3. sequence ascending
 */

#include "cancestry/event/queue.h"

#include "cancestry_test.h"

#include <string.h>

#define TEST_CAPACITY 16u
#define SCRIPT_LENGTH 64u
#define RECORD_CAPACITY 256u

typedef struct event_record {
    cancestry_event_id_t event_id;
    cancestry_event_type_t type;
    cancestry_priority_class_t priority_class;
    cancestry_time_us_t timestamp_us;
    cancestry_sequence_t sequence;
} event_record_t;

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

static bool records_equal(const event_record_t *lhs, const event_record_t *rhs)
{
    return (lhs->event_id == rhs->event_id) && (lhs->type == rhs->type) &&
           (lhs->priority_class == rhs->priority_class) &&
           (lhs->timestamp_us == rhs->timestamp_us) && (lhs->sequence == rhs->sequence);
}

static void record_of(const cancestry_event_t *event, event_record_t *record)
{
    memset(record, 0, sizeof(*record));
    record->event_id = event->event_id;
    record->type = event->type;
    record->priority_class = event->priority_class;
    record->timestamp_us = event->timestamp_us;
    record->sequence = event->sequence;
}

static void test_timestamp_ordering(void)
{
    cancestry_event_queue_t queue;
    cancestry_event_t storage[TEST_CAPACITY];
    cancestry_event_t event;
    cancestry_event_t out;
    const cancestry_time_us_t timestamps[] = {300u, 100u, 200u};
    size_t index;

    CANCESSTRY_TEST_CHECK(cancestry_event_queue_init(&queue, storage, TEST_CAPACITY));
    for (index = 0u; index < sizeof(timestamps) / sizeof(timestamps[0]); ++index) {
        event = make_event(CANCESTRY_EVENT_TYPE_CAN_RX, CANCESTRY_PRIORITY_CLASS_CAN_RX,
                           timestamps[index]);
        CANCESSTRY_TEST_CHECK(cancestry_event_queue_push(&queue, &event) ==
                              CANCESTRY_EVENT_QUEUE_OK);
    }

    CANCESSTRY_TEST_CHECK(cancestry_event_queue_pop(&queue, &out) == CANCESTRY_EVENT_QUEUE_OK);
    CANCESSTRY_TEST_CHECK_U64(out.timestamp_us, 100u);
    CANCESSTRY_TEST_CHECK(cancestry_event_queue_pop(&queue, &out) == CANCESTRY_EVENT_QUEUE_OK);
    CANCESSTRY_TEST_CHECK_U64(out.timestamp_us, 200u);
    CANCESSTRY_TEST_CHECK(cancestry_event_queue_pop(&queue, &out) == CANCESTRY_EVENT_QUEUE_OK);
    CANCESSTRY_TEST_CHECK_U64(out.timestamp_us, 300u);
}

static void test_priority_ordering(void)
{
    cancestry_event_queue_t queue;
    cancestry_event_t storage[TEST_CAPACITY];
    cancestry_event_t event;
    cancestry_event_t out;
    const cancestry_priority_class_t classes[] = {
        CANCESTRY_PRIORITY_CLASS_TRACE, CANCESTRY_PRIORITY_CLASS_FAULT,
        CANCESTRY_PRIORITY_CLASS_HOST, CANCESTRY_PRIORITY_CLASS_TIMER,
        CANCESTRY_PRIORITY_CLASS_GENERATED, CANCESTRY_PRIORITY_CLASS_MODE,
        CANCESTRY_PRIORITY_CLASS_CAN_RX};
    const cancestry_priority_class_t expected[] = {
        CANCESTRY_PRIORITY_CLASS_FAULT, CANCESTRY_PRIORITY_CLASS_MODE,
        CANCESTRY_PRIORITY_CLASS_TIMER, CANCESTRY_PRIORITY_CLASS_CAN_RX,
        CANCESTRY_PRIORITY_CLASS_HOST, CANCESTRY_PRIORITY_CLASS_GENERATED,
        CANCESTRY_PRIORITY_CLASS_TRACE};
    size_t index;

    CANCESSTRY_TEST_CHECK(cancestry_event_queue_init(&queue, storage, TEST_CAPACITY));
    /* Every event shares one timestamp, so priority decides. */
    for (index = 0u; index < sizeof(classes) / sizeof(classes[0]); ++index) {
        event = make_event(CANCESTRY_EVENT_TYPE_CAN_RX, classes[index], 500u);
        CANCESSTRY_TEST_CHECK(cancestry_event_queue_push(&queue, &event) ==
                              CANCESTRY_EVENT_QUEUE_OK);
    }
    for (index = 0u; index < sizeof(expected) / sizeof(expected[0]); ++index) {
        CANCESSTRY_TEST_CHECK(cancestry_event_queue_pop(&queue, &out) == CANCESTRY_EVENT_QUEUE_OK);
        CANCESSTRY_TEST_CHECK((int)out.priority_class == (int)expected[index]);
    }
}

static void test_sequence_ordering(void)
{
    cancestry_event_queue_t queue;
    cancestry_event_t storage[TEST_CAPACITY];
    cancestry_event_t event;
    cancestry_event_t out;
    uint32_t index;
    cancestry_sequence_t previous = 0u;

    CANCESSTRY_TEST_CHECK(cancestry_event_queue_init(&queue, storage, TEST_CAPACITY));
    /* Identical timestamps and priorities: insertion order decides. */
    for (index = 0u; index < 5u; ++index) {
        event = make_event(CANCESTRY_EVENT_TYPE_CAN_RX, CANCESTRY_PRIORITY_CLASS_CAN_RX, 77u);
        event.payload.can_rx.can_id = index;
        CANCESSTRY_TEST_CHECK(cancestry_event_queue_push(&queue, &event) ==
                              CANCESTRY_EVENT_QUEUE_OK);
    }
    for (index = 0u; index < 5u; ++index) {
        CANCESSTRY_TEST_CHECK(cancestry_event_queue_pop(&queue, &out) == CANCESTRY_EVENT_QUEUE_OK);
        CANCESSTRY_TEST_CHECK(out.sequence > previous);
        CANCESSTRY_TEST_CHECK_U64(out.payload.can_rx.can_id, (uint64_t)index);
        previous = out.sequence;
    }
}

static void test_generated_event_ordering(void)
{
    cancestry_event_queue_t queue;
    cancestry_event_t storage[TEST_CAPACITY];
    cancestry_event_t cause;
    cancestry_event_t out;

    CANCESSTRY_TEST_CHECK(cancestry_event_queue_init(&queue, storage, TEST_CAPACITY));

    /* Cause at t=100, an unrelated CAN_RX at t=120, and a generated event with
     * a 50 us delay at t=150. */
    cause = make_event(CANCESTRY_EVENT_TYPE_CAN_RX, CANCESTRY_PRIORITY_CLASS_CAN_RX, 100u);
    CANCESSTRY_TEST_CHECK(cancestry_event_queue_push(&queue, &cause) == CANCESTRY_EVENT_QUEUE_OK);
    CANCESSTRY_TEST_CHECK(cancestry_event_queue_pop(&queue, &cause) == CANCESTRY_EVENT_QUEUE_OK);

    out = make_event(CANCESTRY_EVENT_TYPE_CAN_RX, CANCESTRY_PRIORITY_CLASS_CAN_RX, 120u);
    CANCESSTRY_TEST_CHECK(cancestry_event_queue_push(&queue, &out) == CANCESTRY_EVENT_QUEUE_OK);
    CANCESSTRY_TEST_CHECK(cancestry_event_queue_emit_generated(&queue, &cause,
                                                               CANCESTRY_EVENT_TYPE_SIGNAL_CHANGED,
                                                               NULL, 50u) ==
                          CANCESTRY_EVENT_QUEUE_OK);

    CANCESSTRY_TEST_CHECK(cancestry_event_queue_pop(&queue, &out) == CANCESTRY_EVENT_QUEUE_OK);
    CANCESSTRY_TEST_CHECK_U64(out.timestamp_us, 120u);
    CANCESSTRY_TEST_CHECK(out.priority_class == CANCESTRY_PRIORITY_CLASS_CAN_RX);

    CANCESSTRY_TEST_CHECK(cancestry_event_queue_pop(&queue, &out) == CANCESTRY_EVENT_QUEUE_OK);
    CANCESSTRY_TEST_CHECK_U64(out.timestamp_us, 150u);
    CANCESSTRY_TEST_CHECK(out.priority_class == CANCESTRY_PRIORITY_CLASS_GENERATED);

    /* An undelayed generated event shares the cause timestamp, and the cause
     * wins because CAN_RX has a lower class number than GENERATED. */
    CANCESSTRY_TEST_CHECK(cancestry_event_queue_push(&queue, &cause) == CANCESTRY_EVENT_QUEUE_OK);
    CANCESSTRY_TEST_CHECK(cancestry_event_queue_emit_generated(&queue, &cause,
                                                               CANCESTRY_EVENT_TYPE_SIGNAL_CHANGED,
                                                               NULL, 0u) ==
                          CANCESTRY_EVENT_QUEUE_OK);
    CANCESSTRY_TEST_CHECK(cancestry_event_queue_pop(&queue, &out) == CANCESTRY_EVENT_QUEUE_OK);
    CANCESSTRY_TEST_CHECK(out.priority_class == CANCESTRY_PRIORITY_CLASS_CAN_RX);
    CANCESSTRY_TEST_CHECK(cancestry_event_queue_pop(&queue, &out) == CANCESTRY_EVENT_QUEUE_OK);
    CANCESSTRY_TEST_CHECK(out.priority_class == CANCESTRY_PRIORITY_CLASS_GENERATED);
    CANCESSTRY_TEST_CHECK_U64(out.timestamp_us, 100u);
}

/* Deterministic pseudo-random generator: identical on every platform. */
static uint32_t next_random(uint32_t *state)
{
    *state = ((*state) * 1103515245u) + 12345u;
    return ((*state) >> 8) & 0x7FFFFFu;
}

/** Run one scripted interleaving of pushes and pops; record everything popped. */
static void run_script(uint32_t seed, event_record_t *records, size_t *record_count)
{
    cancestry_event_queue_t queue;
    cancestry_event_t storage[TEST_CAPACITY];
    cancestry_event_t event;
    cancestry_event_t out;
    uint32_t step;
    uint32_t push_count = 0u;

    CANCESSTRY_TEST_CHECK(cancestry_event_queue_init(&queue, storage, TEST_CAPACITY));
    *record_count = 0u;

    for (step = 0u; step < SCRIPT_LENGTH; ++step) {
        const uint32_t roll = next_random(&seed);

        if ((roll & 1u) != 0u) {
            if (cancestry_event_queue_size(&queue) >= TEST_CAPACITY) {
                continue;
            }
            event = make_event(CANCESTRY_EVENT_TYPE_CAN_RX,
                               (cancestry_priority_class_t)((roll >> 4) %
                                                            (uint32_t)CANCESTRY_PRIORITY_CLASS_COUNT),
                               (cancestry_time_us_t)((roll >> 8) % 50u));
            event.payload.can_rx.can_id = step;
            /* The queue assigns this same sequence; the record mirrors it. */
            event.sequence = ++push_count;
            CANCESSTRY_TEST_CHECK(cancestry_event_queue_push(&queue, &event) ==
                                  CANCESTRY_EVENT_QUEUE_OK);
            continue;
        }

        if (cancestry_event_queue_is_empty(&queue)) {
            continue;
        }
        CANCESSTRY_TEST_CHECK(cancestry_event_queue_pop(&queue, &out) == CANCESTRY_EVENT_QUEUE_OK);
        if (*record_count < RECORD_CAPACITY) {
            record_of(&out, &records[*record_count]);
            (*record_count)++;
        }
    }

    while (!cancestry_event_queue_is_empty(&queue)) {
        CANCESSTRY_TEST_CHECK(cancestry_event_queue_pop(&queue, &out) == CANCESTRY_EVENT_QUEUE_OK);
        if (*record_count < RECORD_CAPACITY) {
            record_of(&out, &records[*record_count]);
            (*record_count)++;
        }
    }
}

static void test_determinism_across_identical_runs(void)
{
    static event_record_t first[RECORD_CAPACITY];
    static event_record_t second[RECORD_CAPACITY];
    size_t first_count = 0u;
    size_t second_count = 0u;
    size_t index;

    run_script(0xC0FFEEu, first, &first_count);
    run_script(0xC0FFEEu, second, &second_count);

    CANCESSTRY_TEST_CHECK(first_count > 0u);
    CANCESSTRY_TEST_CHECK_U64(first_count, second_count);
    for (index = 0u; index < first_count && index < second_count; ++index) {
        CANCESSTRY_TEST_CHECK(records_equal(&first[index], &second[index]));
    }
}

/** Reference model: a linear scan for the minimum, using the same comparator. */
static size_t reference_min_index(const cancestry_event_t *events, size_t count)
{
    size_t best = 0u;
    size_t index;
    for (index = 1u; index < count; ++index) {
        if (cancestry_event_compare_order(&events[index], &events[best]) < 0) {
            best = index;
        }
    }
    return best;
}

static void test_heap_matches_the_reference_order(void)
{
    cancestry_event_queue_t queue;
    cancestry_event_t storage[TEST_CAPACITY];
    cancestry_event_t reference[TEST_CAPACITY];
    size_t reference_size = 0u;
    cancestry_event_t event;
    cancestry_event_t out;
    uint32_t seed = 0x1234u;
    uint32_t push_count = 0u;
    uint32_t step;

    CANCESSTRY_TEST_CHECK(cancestry_event_queue_init(&queue, storage, TEST_CAPACITY));

    for (step = 0u; step < 500u; ++step) {
        const uint32_t roll = next_random(&seed);
        size_t index;

        if ((roll & 1u) != 0u) {
            if (reference_size >= TEST_CAPACITY) {
                continue;
            }
            event = make_event(CANCESTRY_EVENT_TYPE_CAN_RX,
                               (cancestry_priority_class_t)((roll >> 4) %
                                                            (uint32_t)CANCESTRY_PRIORITY_CLASS_COUNT),
                               (cancestry_time_us_t)((roll >> 8) % 25u));
            event.sequence = ++push_count;
            CANCESSTRY_TEST_CHECK(cancestry_event_queue_push(&queue, &event) ==
                                  CANCESTRY_EVENT_QUEUE_OK);
            reference[reference_size++] = event;
            continue;
        }

        if (reference_size == 0u) {
            continue;
        }

        /* peek() must already agree with the reference minimum. */
        index = reference_min_index(reference, reference_size);
        CANCESSTRY_TEST_CHECK(cancestry_event_queue_peek(&queue) != NULL);
        CANCESSTRY_TEST_CHECK(cancestry_event_compare_order(cancestry_event_queue_peek(&queue),
                                                            &reference[index]) == 0);

        CANCESSTRY_TEST_CHECK(cancestry_event_queue_pop(&queue, &out) == CANCESTRY_EVENT_QUEUE_OK);
        CANCESSTRY_TEST_CHECK(cancestry_event_compare_order(&out, &reference[index]) == 0);
        CANCESSTRY_TEST_CHECK_U64(out.sequence, reference[index].sequence);

        reference[index] = reference[reference_size - 1u];
        reference_size--;
    }

    while (reference_size > 0u) {
        const size_t index = reference_min_index(reference, reference_size);
        CANCESSTRY_TEST_CHECK(cancestry_event_queue_pop(&queue, &out) == CANCESTRY_EVENT_QUEUE_OK);
        CANCESSTRY_TEST_CHECK(cancestry_event_compare_order(&out, &reference[index]) == 0);
        reference[index] = reference[reference_size - 1u];
        reference_size--;
    }
    CANCESSTRY_TEST_CHECK(cancestry_event_queue_is_empty(&queue));
}

int main(void)
{
    CANCESSTRY_TEST_SUITE_BEGIN("event ordering");

    CANCESSTRY_TEST_CASE("timestamp ascending dominates");
    test_timestamp_ordering();

    CANCESSTRY_TEST_CASE("priority class breaks timestamp ties");
    test_priority_ordering();

    CANCESSTRY_TEST_CASE("sequence breaks remaining ties");
    test_sequence_ordering();

    CANCESSTRY_TEST_CASE("generated events order by inherited timestamp");
    test_generated_event_ordering();

    CANCESSTRY_TEST_CASE("identical input sequences produce identical output sequences");
    test_determinism_across_identical_runs();

    CANCESSTRY_TEST_CASE("heap order matches the reference model under interleaving");
    test_heap_matches_the_reference_order();

    return CANCESSTRY_TEST_SUITE_END();
}
