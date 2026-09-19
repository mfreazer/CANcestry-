/*
 * Unit tests for bounded-queue overflow behavior.
 *
 * Verifies:
 *   SW-FR-EVENT-005  The software shall expose event drop counters.
 *   SYS-NF-002       Bounded resource usage (enforced queue limits).
 *   QA-v0.2-R04      Per-FSM incoming queue overflow policy.
 *   SW-FR-FSM-020    The Path A per-FSM reserve policy is covered by the
 *                    reserved-slot suite; this file retains the zero-reserve
 *                    generic overflow reference model.
 *
 * Normative source: docs/system/event-ordering.md section 9.
 * The reference-model cases explicitly configure zero reserved slots so they
 * continue to exercise the historical generic overflow primitive; Path A is
 * covered by test_reserved_fault_slots.c and is the default initializer policy.
 *
 * Policy under test, for a queue that is full:
 *   - non-fault event: the new event is dropped (drop-newest);
 *   - fault event: the fault is admitted and the newest non-fault event
 *     (highest timestamp, then priority, then sequence) is evicted;
 *   - fault event with only faults queued: the new fault is dropped, because
 *     there is no non-fault victim.
 *
 * Test ids (docs/trace/traceability.csv):
 *   EVENT-OVERFLOW-001  SW-FR-EVENT-005
 *   EVENT-OVERFLOW-002  QA-v0.2-R04
 */

#include "cancestry/event/queue.h"

#include "cancestry_test.h"

#include <string.h>

#define TEST_CAPACITY 4u

static void no_op_retention(uint32_t code, void *context)
{
    (void)code;
    (void)context;
}

static void no_op_action(void *context)
{
    (void)context;
}

static bool install_no_op_hooks(cancestry_event_queue_t *queue)
{
    cancestry_event_hard_fault_hooks_t hooks;

    hooks.write_retention_register = no_op_retention;
    hooks.hal_fail_safe = no_op_action;
    hooks.iwdg_escalate = no_op_action;
    hooks.context = NULL;
    return cancestry_event_queue_set_hard_fault_hooks(queue, &hooks);
}

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

static cancestry_event_t make_rx_event(cancestry_time_us_t timestamp_us, uint32_t can_id)
{
    cancestry_event_t event =
        make_event(CANCESTRY_EVENT_TYPE_CAN_RX, CANCESTRY_PRIORITY_CLASS_CAN_RX, timestamp_us);
    event.payload.can_rx.can_id = can_id;
    return event;
}

static cancestry_event_t make_fault_event(cancestry_time_us_t timestamp_us,
                                          cancestry_fault_code_t code)
{
    cancestry_event_t event =
        make_event(CANCESTRY_EVENT_TYPE_FAULT_RAISED, CANCESTRY_PRIORITY_CLASS_FAULT, timestamp_us);
    event.payload.fault.fault_code = code;
    event.payload.fault.severity = CANCESTRY_FAULT_SEVERITY_ERROR;
    return event;
}

static void test_non_fault_events_drop_newest(void)
{
    cancestry_event_queue_t queue;
    cancestry_event_t storage[TEST_CAPACITY];
    cancestry_event_t event;
    cancestry_event_t out;
    const cancestry_event_queue_counters_t *counters;
    uint32_t index;

    CANCESSTRY_TEST_CHECK(cancestry_event_queue_init_with_reserved_fault_slots(&queue, storage, TEST_CAPACITY, 0u));
    CANCESSTRY_TEST_CHECK(install_no_op_hooks(&queue));

    for (index = 0u; index < TEST_CAPACITY; ++index) {
        event = make_rx_event((cancestry_time_us_t)(index + 1u), 0x100u + index);
        CANCESSTRY_TEST_CHECK(cancestry_event_queue_push(&queue, &event) ==
                              CANCESTRY_EVENT_QUEUE_OK);
    }
    CANCESSTRY_TEST_CHECK(cancestry_event_queue_is_full(&queue));

    /* The new event is the one that is dropped, not an older one. */
    event = make_rx_event(999u, 0xDEADu);
    CANCESSTRY_TEST_CHECK(cancestry_event_queue_push(&queue, &event) ==
                          CANCESTRY_EVENT_QUEUE_ERR_FULL);

    counters = cancestry_event_queue_counters(&queue);
    CANCESSTRY_TEST_CHECK_U64(counters->dropped, 1u);
    CANCESSTRY_TEST_CHECK_U64(counters->overflow_events, 1u);
    CANCESSTRY_TEST_CHECK_U64(counters->pushed, TEST_CAPACITY);
    CANCESSTRY_TEST_CHECK_U64(cancestry_event_queue_size(&queue), TEST_CAPACITY);

    for (index = 0u; index < TEST_CAPACITY; ++index) {
        CANCESSTRY_TEST_CHECK(cancestry_event_queue_pop(&queue, &out) == CANCESTRY_EVENT_QUEUE_OK);
        CANCESSTRY_TEST_CHECK_U64(out.payload.can_rx.can_id, 0x100u + index);
    }
    CANCESSTRY_TEST_CHECK(cancestry_event_queue_is_empty(&queue));
}

static void test_fault_is_admitted_and_evicts_the_newest_non_fault(void)
{
    cancestry_event_queue_t queue;
    cancestry_event_t storage[TEST_CAPACITY];
    cancestry_event_t event;
    cancestry_event_t out;
    const cancestry_event_queue_counters_t *counters;
    uint32_t index;

    CANCESSTRY_TEST_CHECK(cancestry_event_queue_init_with_reserved_fault_slots(&queue, storage, TEST_CAPACITY, 0u));
    CANCESSTRY_TEST_CHECK(install_no_op_hooks(&queue));

    /* Fill with non-fault events at t=10, 20, 30, 40. */
    for (index = 0u; index < TEST_CAPACITY; ++index) {
        event = make_rx_event((cancestry_time_us_t)((index + 1u) * 10u), 0x200u + index);
        CANCESSTRY_TEST_CHECK(cancestry_event_queue_push(&queue, &event) ==
                              CANCESTRY_EVENT_QUEUE_OK);
    }

    event = make_fault_event(5u, 0x11u);
    CANCESSTRY_TEST_CHECK(cancestry_event_queue_push(&queue, &event) ==
                          CANCESTRY_EVENT_QUEUE_OK_EVICTED_VICTIM);
    CANCESSTRY_TEST_CHECK(cancestry_event_queue_status_is_ok(
        CANCESTRY_EVENT_QUEUE_OK_EVICTED_VICTIM));

    counters = cancestry_event_queue_counters(&queue);
    CANCESSTRY_TEST_CHECK_U64(counters->dropped, 1u);
    CANCESSTRY_TEST_CHECK_U64(counters->overflow_events, 1u);
    CANCESSTRY_TEST_CHECK_U64(counters->pushed, TEST_CAPACITY + 1u);
    CANCESSTRY_TEST_CHECK_U64(cancestry_event_queue_size(&queue), TEST_CAPACITY);
    CANCESSTRY_TEST_CHECK_U64(cancestry_event_queue_fault_depth(&queue), 1u);

    /* The fault survives; the newest non-fault event (t=40) was evicted. */
    CANCESSTRY_TEST_CHECK(cancestry_event_queue_pop(&queue, &out) == CANCESTRY_EVENT_QUEUE_OK);
    CANCESSTRY_TEST_CHECK(out.type == CANCESTRY_EVENT_TYPE_FAULT_RAISED);
    CANCESSTRY_TEST_CHECK_U64(out.timestamp_us, 5u);

    for (index = 0u; index < TEST_CAPACITY - 1u; ++index) {
        CANCESSTRY_TEST_CHECK(cancestry_event_queue_pop(&queue, &out) == CANCESTRY_EVENT_QUEUE_OK);
        CANCESSTRY_TEST_CHECK_U64(out.timestamp_us, (uint64_t)((index + 1u) * 10u));
        CANCESSTRY_TEST_CHECK_U64(out.payload.can_rx.can_id, 0x200u + index);
    }
    CANCESSTRY_TEST_CHECK_U64(cancestry_event_queue_fault_depth(&queue), 0u);
}

static void test_victim_is_newest_by_ordering_key(void)
{
    cancestry_event_queue_t queue;
    cancestry_event_t storage[TEST_CAPACITY];
    cancestry_event_t event;
    cancestry_event_t out;

    CANCESSTRY_TEST_CHECK(cancestry_event_queue_init_with_reserved_fault_slots(&queue, storage, TEST_CAPACITY, 0u));
    CANCESSTRY_TEST_CHECK(install_no_op_hooks(&queue));

    /* Insertion order deliberately differs from ordering order: the victim is
     * the event with the highest key (t=30, HOST), not the last pushed. */
    event = make_event(CANCESTRY_EVENT_TYPE_CAN_RX, CANCESTRY_PRIORITY_CLASS_HOST, 30u);
    CANCESSTRY_TEST_CHECK(cancestry_event_queue_push(&queue, &event) == CANCESTRY_EVENT_QUEUE_OK);
    event = make_event(CANCESTRY_EVENT_TYPE_TIMER_EXPIRED, CANCESTRY_PRIORITY_CLASS_TIMER, 10u);
    CANCESSTRY_TEST_CHECK(cancestry_event_queue_push(&queue, &event) == CANCESTRY_EVENT_QUEUE_OK);
    event = make_event(CANCESTRY_EVENT_TYPE_CAN_RX, CANCESTRY_PRIORITY_CLASS_CAN_RX, 20u);
    CANCESSTRY_TEST_CHECK(cancestry_event_queue_push(&queue, &event) == CANCESTRY_EVENT_QUEUE_OK);
    event = make_event(CANCESTRY_EVENT_TYPE_SIGNAL_CHANGED, CANCESTRY_PRIORITY_CLASS_TRACE, 25u);
    CANCESSTRY_TEST_CHECK(cancestry_event_queue_push(&queue, &event) == CANCESTRY_EVENT_QUEUE_OK);

    event = make_fault_event(1u, 0x22u);
    CANCESSTRY_TEST_CHECK(cancestry_event_queue_push(&queue, &event) ==
                          CANCESTRY_EVENT_QUEUE_OK_EVICTED_VICTIM);

    CANCESSTRY_TEST_CHECK(cancestry_event_queue_pop(&queue, &out) == CANCESTRY_EVENT_QUEUE_OK);
    CANCESSTRY_TEST_CHECK_U64(out.timestamp_us, 1u);
    CANCESSTRY_TEST_CHECK(cancestry_event_queue_pop(&queue, &out) == CANCESTRY_EVENT_QUEUE_OK);
    CANCESSTRY_TEST_CHECK_U64(out.timestamp_us, 10u);
    CANCESSTRY_TEST_CHECK(cancestry_event_queue_pop(&queue, &out) == CANCESTRY_EVENT_QUEUE_OK);
    CANCESSTRY_TEST_CHECK_U64(out.timestamp_us, 20u);
    CANCESSTRY_TEST_CHECK(cancestry_event_queue_pop(&queue, &out) == CANCESTRY_EVENT_QUEUE_OK);
    CANCESSTRY_TEST_CHECK_U64(out.timestamp_us, 25u);
    CANCESSTRY_TEST_CHECK(cancestry_event_queue_is_empty(&queue));
}

static void test_faults_fill_the_queue_then_drop_newest_fault(void)
{
    cancestry_event_queue_t queue;
    cancestry_event_t storage[TEST_CAPACITY];
    cancestry_event_t event;
    cancestry_event_t out;
    const cancestry_event_queue_counters_t *counters;
    uint32_t index;

    CANCESSTRY_TEST_CHECK(cancestry_event_queue_init_with_reserved_fault_slots(&queue, storage, TEST_CAPACITY, 0u));
    CANCESSTRY_TEST_CHECK(install_no_op_hooks(&queue));

    for (index = 0u; index < TEST_CAPACITY; ++index) {
        event = make_fault_event((cancestry_time_us_t)(index + 1u), 0x300u + index);
        CANCESSTRY_TEST_CHECK(cancestry_event_queue_push(&queue, &event) ==
                              CANCESTRY_EVENT_QUEUE_OK);
    }
    CANCESSTRY_TEST_CHECK_U64(cancestry_event_queue_fault_depth(&queue), TEST_CAPACITY);

    /* Nothing but faults is queued, so there is no victim to evict. */
    event = make_fault_event(500u, 0xBADu);
    CANCESSTRY_TEST_CHECK(cancestry_event_queue_push(&queue, &event) ==
                          CANCESTRY_EVENT_QUEUE_ERR_FULL_FAULT);
    CANCESSTRY_TEST_CHECK(!cancestry_event_queue_status_is_ok(
        CANCESTRY_EVENT_QUEUE_ERR_FULL_FAULT));

    counters = cancestry_event_queue_counters(&queue);
    CANCESSTRY_TEST_CHECK_U64(counters->dropped, 1u);
    CANCESSTRY_TEST_CHECK_U64(counters->overflow_events, 1u);
    CANCESSTRY_TEST_CHECK_U64(cancestry_event_queue_size(&queue), TEST_CAPACITY);
    CANCESSTRY_TEST_CHECK_U64(cancestry_event_queue_fault_depth(&queue), TEST_CAPACITY);

    for (index = 0u; index < TEST_CAPACITY; ++index) {
        CANCESSTRY_TEST_CHECK(cancestry_event_queue_pop(&queue, &out) == CANCESTRY_EVENT_QUEUE_OK);
        CANCESSTRY_TEST_CHECK_U64(out.payload.fault.fault_code, 0x300u + index);
    }
    CANCESSTRY_TEST_CHECK_U64(cancestry_event_queue_fault_depth(&queue), 0u);
}

static void test_repeated_faults_evict_one_victim_each(void)
{
    cancestry_event_queue_t queue;
    cancestry_event_t storage[TEST_CAPACITY];
    cancestry_event_t event;
    const cancestry_event_queue_counters_t *counters;
    uint32_t index;

    CANCESSTRY_TEST_CHECK(cancestry_event_queue_init_with_reserved_fault_slots(&queue, storage, TEST_CAPACITY, 0u));
    CANCESSTRY_TEST_CHECK(install_no_op_hooks(&queue));

    for (index = 0u; index < TEST_CAPACITY; ++index) {
        event = make_rx_event((cancestry_time_us_t)(index + 1u), 0x400u + index);
        CANCESSTRY_TEST_CHECK(cancestry_event_queue_push(&queue, &event) ==
                              CANCESTRY_EVENT_QUEUE_OK);
    }

    for (index = 0u; index < 2u; ++index) {
        event = make_fault_event((cancestry_time_us_t)(100u + index), 0x500u + index);
        CANCESSTRY_TEST_CHECK(cancestry_event_queue_push(&queue, &event) ==
                              CANCESTRY_EVENT_QUEUE_OK_EVICTED_VICTIM);
    }

    counters = cancestry_event_queue_counters(&queue);
    CANCESSTRY_TEST_CHECK_U64(counters->dropped, 2u);
    CANCESSTRY_TEST_CHECK_U64(counters->overflow_events, 2u);
    CANCESSTRY_TEST_CHECK_U64(cancestry_event_queue_size(&queue), TEST_CAPACITY);
    CANCESSTRY_TEST_CHECK_U64(cancestry_event_queue_fault_depth(&queue), 2u);

    cancestry_event_queue_clear(&queue);
    CANCESSTRY_TEST_CHECK_U64(cancestry_event_queue_fault_depth(&queue), 0u);
    CANCESSTRY_TEST_CHECK_U64(cancestry_event_queue_size(&queue), 0u);
}

static void test_persistent_overflow_is_reported(void)
{
    cancestry_event_queue_t queue;
    cancestry_event_t storage[TEST_CAPACITY];
    cancestry_event_t event;
    const cancestry_event_queue_counters_t *counters;
    uint32_t index;

    CANCESSTRY_TEST_CHECK(cancestry_event_queue_init_with_reserved_fault_slots(&queue, storage, TEST_CAPACITY, 0u));
    CANCESSTRY_TEST_CHECK(install_no_op_hooks(&queue));

    for (index = 0u; index < TEST_CAPACITY; ++index) {
        event = make_rx_event((cancestry_time_us_t)(index + 1u), 0x600u + index);
        CANCESSTRY_TEST_CHECK(cancestry_event_queue_push(&queue, &event) ==
                              CANCESTRY_EVENT_QUEUE_OK);
    }

    /* Below the threshold the condition is not yet persistent. */
    for (index = 0u; index < CANCESTRY_EVENT_QUEUE_PERSISTENT_OVERFLOW_THRESHOLD - 1u; ++index) {
        event = make_rx_event(900u, 0x700u + index);
        CANCESSTRY_TEST_CHECK(cancestry_event_queue_push(&queue, &event) ==
                              CANCESTRY_EVENT_QUEUE_ERR_FULL);
        CANCESSTRY_TEST_CHECK(!cancestry_event_queue_overflow_is_persistent(&queue));
    }

    event = make_rx_event(900u, 0x800u);
    CANCESSTRY_TEST_CHECK(cancestry_event_queue_push(&queue, &event) ==
                          CANCESTRY_EVENT_QUEUE_ERR_FULL);
    CANCESSTRY_TEST_CHECK(cancestry_event_queue_overflow_is_persistent(&queue));

    counters = cancestry_event_queue_counters(&queue);
    CANCESSTRY_TEST_CHECK_U64(counters->consecutive_overflows,
                              CANCESTRY_EVENT_QUEUE_PERSISTENT_OVERFLOW_THRESHOLD);
    CANCESSTRY_TEST_CHECK_U64(counters->dropped,
                              CANCESTRY_EVENT_QUEUE_PERSISTENT_OVERFLOW_THRESHOLD);

    /* Making room and pushing successfully clears the streak. */
    CANCESSTRY_TEST_CHECK(cancestry_event_queue_pop(&queue, NULL) == CANCESTRY_EVENT_QUEUE_OK);
    event = make_rx_event(1000u, 0x900u);
    CANCESSTRY_TEST_CHECK(cancestry_event_queue_push(&queue, &event) == CANCESTRY_EVENT_QUEUE_OK);
    CANCESSTRY_TEST_CHECK_U64(cancestry_event_queue_counters(&queue)->consecutive_overflows, 0u);
    CANCESSTRY_TEST_CHECK(!cancestry_event_queue_overflow_is_persistent(&queue));
}

/*
 * Reference model of the documented policy, written independently of the
 * queue: a flat array, a linear scan for the minimum, and a linear scan for
 * the eviction victim.
 */
#define MODEL_CAPACITY 6u
#define MODEL_STEPS 20000u

typedef struct overflow_model {
    cancestry_event_t events[MODEL_CAPACITY];
    size_t size;
    uint32_t next_sequence;
    uint32_t next_event_id;
    uint32_t random_state;
    bool terminal;
} overflow_model_t;

static uint32_t model_random(overflow_model_t *model)
{
    model->random_state = (model->random_state * 1103515245u) + 12345u;
    return (model->random_state >> 8) & 0xFFFFu;
}

static int model_compare(const cancestry_event_t *lhs, const cancestry_event_t *rhs)
{
    if (lhs->timestamp_us != rhs->timestamp_us) {
        return (lhs->timestamp_us < rhs->timestamp_us) ? -1 : 1;
    }
    if (lhs->priority_class != rhs->priority_class) {
        return ((int)lhs->priority_class < (int)rhs->priority_class) ? -1 : 1;
    }
    if (lhs->sequence != rhs->sequence) {
        return (lhs->sequence < rhs->sequence) ? -1 : 1;
    }
    return 0;
}

static cancestry_event_queue_status_t model_push(overflow_model_t *model,
                                                 const cancestry_event_t *event)
{
    cancestry_event_t stored = *event;

    if (model->terminal) {
        return CANCESTRY_EVENT_QUEUE_ERR_TERMINAL;
    }
    if (model->size < MODEL_CAPACITY) {
        stored.sequence = ++model->next_sequence;
        stored.event_id = ++model->next_event_id;
        model->events[model->size++] = stored;
        return CANCESTRY_EVENT_QUEUE_OK;
    }

    if (stored.priority_class != CANCESTRY_PRIORITY_CLASS_FAULT) {
        /* Drop-newest: the incoming event is the one that is lost. */
        return CANCESTRY_EVENT_QUEUE_ERR_FULL;
    }

    {
        size_t victim = MODEL_CAPACITY;
        size_t index;
        for (index = 0u; index < model->size; ++index) {
            if (model->events[index].priority_class == CANCESTRY_PRIORITY_CLASS_FAULT) {
                continue;
            }
            if (victim == MODEL_CAPACITY ||
                model_compare(&model->events[index], &model->events[victim]) > 0) {
                victim = index;
            }
        }
        if (victim == MODEL_CAPACITY) {
            model->terminal = true;
            return CANCESTRY_EVENT_QUEUE_ERR_FULL_FAULT;
        }
        model->events[victim] = model->events[model->size - 1u];
        model->size--;
        stored.sequence = ++model->next_sequence;
        stored.event_id = ++model->next_event_id;
        model->events[model->size++] = stored;
        return CANCESTRY_EVENT_QUEUE_OK_EVICTED_VICTIM;
    }
}

static cancestry_event_queue_status_t model_pop(overflow_model_t *model, cancestry_event_t *out)
{
    size_t best = 0u;
    size_t index;

    if (model->size == 0u) {
        return CANCESTRY_EVENT_QUEUE_ERR_EMPTY;
    }
    for (index = 1u; index < model->size; ++index) {
        if (model_compare(&model->events[index], &model->events[best]) < 0) {
            best = index;
        }
    }
    if (out != NULL) {
        *out = model->events[best];
    }
    model->events[best] = model->events[model->size - 1u];
    model->size--;
    return CANCESTRY_EVENT_QUEUE_OK;
}

static size_t model_fault_depth(const overflow_model_t *model)
{
    size_t count = 0u;
    size_t index;
    for (index = 0u; index < model->size; ++index) {
        if (model->events[index].priority_class == CANCESTRY_PRIORITY_CLASS_FAULT) {
            count++;
        }
    }
    return count;
}

static void test_overflow_policy_matches_reference_model(void)
{
    cancestry_event_t storage[MODEL_CAPACITY];
    cancestry_event_queue_t queue;
    overflow_model_t model;
    uint32_t step;

    memset(&model, 0, sizeof(model));
    model.random_state = 0x5EEDu;

    CANCESSTRY_TEST_CHECK(cancestry_event_queue_init_with_reserved_fault_slots(&queue, storage, MODEL_CAPACITY, 0u));
    CANCESSTRY_TEST_CHECK(install_no_op_hooks(&queue));

    /* Few distinct timestamps and all seven priority classes: lots of ties,
     * lots of overflow, and a fault about one time in seven. */
    for (step = 0u; step < MODEL_STEPS; ++step) {
        const uint32_t roll = model_random(&model);
        cancestry_event_t event;
        cancestry_event_t expected;
        cancestry_event_queue_status_t expected_status;
        cancestry_event_queue_status_t actual_status;

        cancestry_event_init(&event);
        event.type = CANCESTRY_EVENT_TYPE_CAN_RX;
        event.priority_class = (cancestry_priority_class_t)((roll >> 4) %
                                                            (uint32_t)CANCESTRY_PRIORITY_CLASS_COUNT);
        event.timestamp_us = (cancestry_time_us_t)((roll >> 8) % 4u);
        event.payload.can_rx.can_id = step;

        actual_status = cancestry_event_queue_push(&queue, &event);
        expected_status = model_push(&model, &event);
        CANCESSTRY_TEST_CHECK(actual_status == expected_status);

        CANCESSTRY_TEST_CHECK_U64(cancestry_event_queue_size(&queue), model.size);
        CANCESSTRY_TEST_CHECK_U64(cancestry_event_queue_fault_depth(&queue), model_fault_depth(&model));

        if ((roll & 3u) != 0u) {
            continue;
        }

        actual_status = cancestry_event_queue_pop(&queue, &event);
        expected_status = model_pop(&model, &expected);
        CANCESSTRY_TEST_CHECK(actual_status == expected_status);
        if (expected_status != CANCESTRY_EVENT_QUEUE_OK) {
            continue;
        }
        CANCESSTRY_TEST_CHECK(model_compare(&event, &expected) == 0);
        CANCESSTRY_TEST_CHECK_U64(event.sequence, expected.sequence);
        CANCESSTRY_TEST_CHECK_U64(event.event_id, expected.event_id);
    }
}

static void test_rejections_are_not_counted_as_drops(void)
{
    cancestry_event_queue_t queue;
    cancestry_event_t storage[TEST_CAPACITY];
    cancestry_event_t event;
    const cancestry_event_queue_counters_t *counters;

    CANCESSTRY_TEST_CHECK(cancestry_event_queue_init_with_reserved_fault_slots(&queue, storage, TEST_CAPACITY, 0u));
    CANCESSTRY_TEST_CHECK(install_no_op_hooks(&queue));

    event = make_event(CANCESTRY_EVENT_TYPE_INVALID, CANCESTRY_PRIORITY_CLASS_FAULT, 1u);
    CANCESSTRY_TEST_CHECK(cancestry_event_queue_push(&queue, &event) ==
                          CANCESTRY_EVENT_QUEUE_ERR_EVENT);

    counters = cancestry_event_queue_counters(&queue);
    CANCESSTRY_TEST_CHECK_U64(counters->rejected, 1u);
    CANCESSTRY_TEST_CHECK_U64(counters->dropped, 0u);
    CANCESSTRY_TEST_CHECK_U64(counters->overflow_events, 0u);
    CANCESSTRY_TEST_CHECK_U64(counters->pushed, 0u);

    CANCESSTRY_TEST_CHECK_STRING(cancestry_event_queue_status_name(CANCESTRY_EVENT_QUEUE_OK), "OK");
    CANCESSTRY_TEST_CHECK_STRING(
        cancestry_event_queue_status_name(CANCESTRY_EVENT_QUEUE_ERR_FULL_FAULT), "ERR_FULL_FAULT");
}

int main(void)
{
    CANCESSTRY_TEST_SUITE_BEGIN("event queue overflow");

    CANCESSTRY_TEST_CASE("non-fault events drop-newest when the queue is full");
    test_non_fault_events_drop_newest();

    CANCESSTRY_TEST_CASE("fault events are admitted by evicting the newest non-fault event");
    test_fault_is_admitted_and_evicts_the_newest_non_fault();

    CANCESSTRY_TEST_CASE("the eviction victim is the newest by ordering key");
    test_victim_is_newest_by_ordering_key();

    CANCESSTRY_TEST_CASE("faults fill the queue, then the newest fault is dropped");
    test_faults_fill_the_queue_then_drop_newest_fault();

    CANCESSTRY_TEST_CASE("each admitted fault evicts exactly one victim");
    test_repeated_faults_evict_one_victim_each();

    CANCESSTRY_TEST_CASE("persistent overflow is reported once the threshold is reached");
    test_persistent_overflow_is_reported();

    CANCESSTRY_TEST_CASE("randomized overflow behavior matches an independent reference model");
    test_overflow_policy_matches_reference_model();

    CANCESSTRY_TEST_CASE("rejected pushes are not counted as drops");
    test_rejections_are_not_counted_as_drops();

    return CANCESSTRY_TEST_SUITE_END();
}
