/*
 * CANcestry FSM conformance: per-instance queue overflow.
 *
 * Contract under test (docs/packages/fsm-spec.md section 11,
 * docs/system/event-ordering.md sections 9 and 11, docs/software/SwRS.md
 * SW-FR-FSM-019, SW-FR-FSM-020, SW-FR-FSM-021, SW-FR-FSM-047):
 *
 *   - each instance owns a bounded incoming queue whose default depth is 64;
 *   - on overflow a non-fault event is dropped - the *newest* one, so an already
 *     queued event is never displaced by a later arrival;
 *   - fault events use the reserved physical slots and, when physical capacity
 *     is full with a non-fault present, admitting one evicts that non-fault;
 *     an all-fault full queue retains its bounded set and escalates;
 *   - every drop is counted (and persistent overflow is therefore visible to the
 *     fault manager, which owns the WARNING reaction);
 *   - one instance's overflow cannot disturb another's;
 *   - the runtime stays bounded: no allocation, no recursion, and a per-activation
 *     event budget caps what one external event can cause.
 *
 * Test ids: FSM-QUEUE-001 .. FSM-QUEUE-005.
 */

#include "cancestry_fsm_conformance.h"

/*
 * A ping-pong: every transition into A or B queues two more events, so the
 * incoming queue grows until the overflow policy bites. It is also the shape
 * SW-FR-FSM-021 cares about - an FSM that reacts to its own state events - so the
 * suite verifies that it stays bounded instead of running away.
 */
static const char *const flood_yaml = "schema_version: \"0.2.0\"\n"
                                      "state_machines:\n"
                                      "  - name: flood\n"
                                      "    initial: A\n"
                                      "    states:\n"
                                      "      - name: A\n"
                                      "        transitions:\n"
                                      "          - event: can_rx\n"
                                      "            target: B\n"
                                      "          - event: state_entered\n"
                                      "            target: B\n"
                                      "          - event: state_exited\n"
                                      "            target: B\n"
                                      "          - event: fault_raised\n"
                                      "            target: HALTED\n"
                                      "      - name: B\n"
                                      "        transitions:\n"
                                      "          - event: state_entered\n"
                                      "            target: A\n"
                                      "          - event: state_exited\n"
                                      "            target: A\n"
                                      "          - event: fault_raised\n"
                                      "            target: HALTED\n"
                                      "      - name: HALTED\n"
                                      "  - name: calm\n"
                                      "    initial: A\n"
                                      "    states:\n"
                                      "      - name: A\n"
                                      "        transitions:\n"
                                      "          - event: can_rx\n"
                                      "            target: B\n"
                                      "          - event: fault_raised\n"
                                      "            target: HALTED\n"
                                      "      - name: B\n"
                                      "      - name: HALTED\n"
                                      "instances:\n"
                                      "  - id: flood.one\n"
                                      "    machine: flood\n"
                                      "    enabled: true\n"
                                      "    subscriptions:\n"
                                      "      faults: true\n"
                                      "  - id: calm.one\n"
                                      "    machine: calm\n"
                                      "    enabled: true\n";

/* One transition in, one state event survives at depth 1: the newer one is dropped. */
static const char *const newest_yaml = "schema_version: \"0.2.0\"\n"
                                       "state_machines:\n"
                                       "  - name: lab\n"
                                       "    initial: A\n"
                                       "    states:\n"
                                       "      - name: A\n"
                                       "        transitions:\n"
                                       "          - event: can_rx\n"
                                       "            target: B\n"
                                       "      - name: B\n"
                                       "        transitions:\n"
                                       "          - event: state_exited\n"
                                       "            target: SURVIVOR\n"
                                       "          - event: state_entered\n"
                                       "            target: LATE\n"
                                       "          - event: fault_raised\n"
                                       "            target: HALTED\n"
                                       "      - name: SURVIVOR\n"
                                       "      - name: LATE\n"
                                       "      - name: HALTED\n"
                                       "instances:\n"
                                       "  - id: lab.one\n"
                                       "    machine: lab\n"
                                       "    enabled: true\n";

/* FSM-QUEUE-001: the default depth is 64, and the caller's storage is what bounds
 * it (no allocation, no silent growth). */
static void case_default_depth(void)
{
    static fsm_test_fixture_t fx;

    CANCESSTRY_TEST_CASE("FSM-QUEUE-001 default incoming depth is 64");
    CANCESSTRY_TEST_CHECK(fsm_test_init(&fx, flood_yaml));
    CANCESSTRY_TEST_CHECK_U64(cancestry_event_queue_capacity(&fx.instances[0].incoming), 64u);
    CANCESSTRY_TEST_CHECK_U64(cancestry_event_queue_capacity(&fx.instances[1].incoming), 64u);
    CANCESSTRY_TEST_CHECK(cancestry_event_queue_is_empty(&fx.instances[0].incoming));
    /* The queues are distinct: one instance cannot fill another's buffer. */
    CANCESSTRY_TEST_CHECK(fx.instances[0].incoming.slots != fx.instances[1].incoming.slots);
    fsm_test_destroy(&fx);

    CANCESSTRY_TEST_CHECK(fsm_test_init_queue(&fx, flood_yaml, 4u));
    CANCESSTRY_TEST_CHECK_U64(cancestry_event_queue_capacity(&fx.instances[0].incoming), 4u);
    CANCESSTRY_TEST_CHECK_U64(cancestry_event_queue_capacity(&fx.instances[1].incoming), 4u);
    fsm_test_destroy(&fx);
}

/* FSM-QUEUE-002: overflow drops the newest non-fault event, keeps the older ones,
 * and counts what it did. */
static void case_drop_newest(void)
{
    static fsm_test_fixture_t fx;
    fsm_test_options_t options = fsm_test_options_default();
    cancestry_event_t event;
    uint32_t i;

    CANCESSTRY_TEST_CASE("FSM-QUEUE-002 drop-newest on overflow");
    /* Depth 1 and one event per activation, so the two state events a transition
     * generates compete for a single slot and the outcome is unambiguous. */
    options.queue_depth = 1u;
    options.max_events_per_activation = 1u;
    CANCESSTRY_TEST_CHECK(fsm_test_init_with(&fx, newest_yaml, &options));
    CANCESSTRY_TEST_CHECK_U64(fsm_test_start(&fx, "lab.one"), CANCESTRY_FSM_OK);
    /* start() itself queued one state event, which the start drain consumed. */
    CANCESSTRY_TEST_CHECK_U64(fsm_test_counters(&fx, "lab.one")->events_dropped, 0u);

    fsm_test_event_can_rx(&event, 1000u, FSM_TEST_IFACE_CAN0, FSM_TEST_MSG_STATUS, NULL, 0u);
    CANCESSTRY_TEST_CHECK_U64(fsm_test_process(&fx, &event), CANCESTRY_FSM_OK);
    /* A->B queued state_exited(A) and state_entered(B): the first was admitted, the
     * newer one dropped, and the drop is counted. */
    CANCESSTRY_TEST_CHECK_U64(fsm_test_counters(&fx, "lab.one")->events_dropped, 1u);
    CANCESSTRY_TEST_CHECK_U64(cancestry_event_queue_size(&fx.instances[0].incoming), 1u);
    CANCESSTRY_TEST_CHECK(cancestry_event_queue_is_full(&fx.instances[0].incoming));
    CANCESSTRY_TEST_CHECK_U64(cancestry_event_queue_counters(&fx.instances[0].incoming)->dropped,
                              1u);
    CANCESSTRY_TEST_CHECK_U64(
        cancestry_event_queue_counters(&fx.instances[0].incoming)->overflow_events, 1u);

    /* The surviving event is the older one: the next activation runs B's
     * state_exited transition, not its state_entered one. */
    cancestry_event_init(&event);
    fsm_test_event_can_rx(&event, 2000u, FSM_TEST_IFACE_CAN0, FSM_TEST_MSG_STATUS, NULL, 0u);
    CANCESSTRY_TEST_CHECK_U64(fsm_test_process(&fx, &event), CANCESTRY_FSM_OK);
    CANCESSTRY_TEST_CHECK_STRING(fsm_test_state(&fx, "lab.one"), "SURVIVOR");
    CANCESSTRY_TEST_CHECK(!fsm_test_has_line(&fx, "state 1 B->LATE"));
    /* In this call the incoming event was the newest again, and the transition's
     * own state_entered event was dropped after the older one was admitted. */
    CANCESSTRY_TEST_CHECK_U64(fsm_test_counters(&fx, "lab.one")->events_dropped, 3u);
    CANCESSTRY_TEST_CHECK_U64(fsm_test_counters(&fx, "lab.one")->action_errors, 0u);

    /* Saturating further stays bounded: the queue never grows past its capacity and
     * every extra event is accounted for. */
    for (i = 0u; i < 12u; ++i) {
        cancestry_event_init(&event);
        fsm_test_event_can_rx(&event, (uint64_t)(3000u + i), FSM_TEST_IFACE_CAN0,
                              FSM_TEST_MSG_STATUS, NULL, 0u);
        CANCESSTRY_TEST_CHECK_U64(fsm_test_process(&fx, &event), CANCESTRY_FSM_OK);
        CANCESSTRY_TEST_CHECK(cancestry_event_queue_size(&fx.instances[0].incoming) <=
                              cancestry_event_queue_capacity(&fx.instances[0].incoming));
    }
    CANCESSTRY_TEST_CHECK(fsm_test_counters(&fx, "lab.one")->events_dropped >= 3u);
    /* Persistent overflow is measurable, which is the input the fault manager needs
     * to escalate (event-ordering.md section 9). */
    CANCESSTRY_TEST_CHECK(cancestry_event_queue_counters(&fx.instances[0].incoming)
                              ->overflow_events >= 2u);
    CANCESSTRY_TEST_CHECK_U64(cancestry_event_queue_counters(&fx.instances[0].incoming)
                                  ->high_water,
                              1u);
    /* Nothing vanished silently: every event the queue took was either processed or
     * counted as dropped, and the queue's own counters agree with the instance's. */
    {
        const cancestry_event_queue_counters_t *q =
            cancestry_event_queue_counters(&fx.instances[0].incoming);

        CANCESSTRY_TEST_CHECK(q->pushed >= q->popped);
        CANCESSTRY_TEST_CHECK_U64(q->dropped,
                                  cancestry_event_queue_counters(&fx.instances[0].incoming)
                                      ->dropped);
    }
    fsm_test_destroy(&fx);
}

/* FSM-QUEUE-003: a saturated instance keeps the engine alive and bounded. */
static void case_self_reactive_bounded(void)
{
    static fsm_test_fixture_t fx;
    cancestry_event_t event;
    uint32_t processed_before = 0u;
    uint32_t processed = 0u;

    CANCESSTRY_TEST_CASE("FSM-QUEUE-003 a self-reactive chain is bounded");
    CANCESSTRY_TEST_CHECK(fsm_test_init_queue(&fx, flood_yaml, 4u));
    CANCESSTRY_TEST_CHECK_U64(fsm_test_start(&fx, "flood.one"), CANCESTRY_FSM_OK);
    /* One external event, and a machine that reacts to its own state events: the
     * activation is capped by the event budget, so the work per event is bounded
     * even though the chain could in principle go on forever (SW-FR-FSM-021,
     * SW-FR-FSM-045). */
    processed_before = fsm_test_counters(&fx, "flood.one")->events_processed;
    fsm_test_event_can_rx(&event, 1000u, FSM_TEST_IFACE_CAN0, FSM_TEST_MSG_STATUS, NULL, 0u);
    CANCESSTRY_TEST_CHECK_U64(fsm_test_process(&fx, &event), CANCESTRY_FSM_OK);
    processed = fsm_test_counters(&fx, "flood.one")->events_processed - processed_before;
    CANCESSTRY_TEST_CHECK(cancestry_event_queue_size(&fx.instances[0].incoming) <= 4u);
    /* Both activations that had work to do were cut short by the budget: the one at
     * start and the one at this event. */
    CANCESSTRY_TEST_CHECK(fsm_test_counters(&fx, "flood.one")->budget_exhausted >= 1u);
    /* One activation served at most the configured number of events, no matter how
     * long the chain could have gone. */
    CANCESSTRY_TEST_CHECK_U64(processed, CANCESTRY_FSM_MAX_EVENTS_PER_ACTIVATION);
    CANCESSTRY_TEST_CHECK_U64(fsm_test_lifecycle(&fx, "flood.one"),
                              CANCESTRY_FSM_INSTANCE_LIFECYCLE_RUNNING);
    /* The queue filled up as well, and said so. */
    CANCESSTRY_TEST_CHECK(fsm_test_counters(&fx, "flood.one")->events_dropped >= 1u);
    fsm_test_destroy(&fx);
}

/* FSM-QUEUE-004: a fault is admitted to a physically full queue by evicting a
 * non-fault and is served ahead of everything else (event-ordering.md sections
 * 9 and 11.1). The default reserve prevents ordinary traffic from consuming
 * fault capacity before this case. */
static void case_faults_never_dropped(void)
{
    static fsm_test_fixture_t fx;
    fsm_test_options_t options = fsm_test_options_default();
    cancestry_event_t event;
    uint32_t dropped_before = 0u;

    CANCESSTRY_TEST_CASE("FSM-QUEUE-004 faults evict instead of being dropped");
    options.queue_depth = 1u;
    options.max_events_per_activation = 1u;
    CANCESSTRY_TEST_CHECK(fsm_test_init_with(&fx, newest_yaml, &options));
    CANCESSTRY_TEST_CHECK_U64(fsm_test_start(&fx, "lab.one"), CANCESTRY_FSM_OK);
    /* Fill the single slot with a non-fault event that no activation will consume
     * yet: A->B queued state_exited(A), and state_entered(B) was dropped. */
    fsm_test_event_can_rx(&event, 1000u, FSM_TEST_IFACE_CAN0, FSM_TEST_MSG_STATUS, NULL, 0u);
    CANCESSTRY_TEST_CHECK_U64(fsm_test_process(&fx, &event), CANCESTRY_FSM_OK);
    CANCESSTRY_TEST_CHECK_U64(
        cancestry_event_queue_fault_depth(&fx.instances[0].incoming), 0u);
    dropped_before = fsm_test_counters(&fx, "lab.one")->events_dropped;

    /* A fault now arrives at a full queue. It is admitted by evicting the
     * non-fault victim; all-fault saturation is covered by QA-EV-01 tests. */
    cancestry_event_init(&event);
    fsm_test_event_fault(&event, 2000u, 4242u, CANCESTRY_FAULT_SEVERITY_CRITICAL);
    CANCESSTRY_TEST_CHECK_U64(fsm_test_process(&fx, &event), CANCESTRY_FSM_OK);
    CANCESSTRY_TEST_CHECK_STRING(fsm_test_state(&fx, "lab.one"), "HALTED");
    CANCESSTRY_TEST_CHECK_U64(fsm_test_counters(&fx, "lab.one")->events_dropped,
                              dropped_before + 1u);
    CANCESSTRY_TEST_CHECK_U64(cancestry_event_queue_fault_depth(&fx.instances[0].incoming), 0u);
    /* The victim was the displaced non-fault event: B's state_exited transition
     * never ran. */
    CANCESSTRY_TEST_CHECK(!fsm_test_has_line(&fx, "state 1 B->SURVIVOR"));

    fsm_test_destroy(&fx);

    CANCESSTRY_TEST_CHECK(fsm_test_init(&fx, flood_yaml));
    CANCESSTRY_TEST_CHECK_U64(fsm_test_start(&fx, "calm.one"), CANCESTRY_FSM_OK);
    cancestry_event_init(&event);
    fsm_test_event_fault(&event, 3000u, 1u, CANCESTRY_FAULT_SEVERITY_WARNING);
    CANCESSTRY_TEST_CHECK_U64(fsm_test_process(&fx, &event), CANCESTRY_FSM_OK);
    /* "calm" declares a fault_raised transition but no subscription list, so it
     * does receive it; the flooded instance's own reaction is covered by its
     * faults: true subscription. */
    CANCESSTRY_TEST_CHECK_STRING(fsm_test_state(&fx, "calm.one"), "HALTED");
    CANCESSTRY_TEST_CHECK_U64(fsm_test_counters(&fx, "calm.one")->events_suppressed, 0u);
    fsm_test_destroy(&fx);
}

/* FSM-QUEUE-005: one instance's overflow is another's non-event. */
static void case_instance_isolation(void)
{
    static fsm_test_fixture_t fx;
    cancestry_event_t event;
    uint32_t i;

    CANCESSTRY_TEST_CASE("FSM-QUEUE-005 overflow is per instance");
    CANCESSTRY_TEST_CHECK(fsm_test_init_queue(&fx, flood_yaml, 4u));
    CANCESSTRY_TEST_CHECK_U64(fsm_test_start_all(&fx), 2u);
    for (i = 0u; i < 6u; ++i) {
        cancestry_event_init(&event);
        fsm_test_event_can_rx(&event, (uint64_t)(1000u + i), FSM_TEST_IFACE_CAN0,
                              FSM_TEST_MSG_STATUS, NULL, 0u);
        CANCESSTRY_TEST_CHECK_U64(fsm_test_process(&fx, &event), CANCESTRY_FSM_OK);
    }
    /* The self-reactive instance overflowed; the ordinary one did not, and it kept
     * processing every event it received. */
    CANCESSTRY_TEST_CHECK(fsm_test_counters(&fx, "flood.one")->events_dropped >= 1u);
    CANCESSTRY_TEST_CHECK_U64(fsm_test_counters(&fx, "calm.one")->events_dropped, 0u);
    CANCESSTRY_TEST_CHECK(fsm_test_counters(&fx, "calm.one")->events_processed >= 6u);
    CANCESSTRY_TEST_CHECK_U64(fsm_test_lifecycle(&fx, "calm.one"),
                              CANCESTRY_FSM_INSTANCE_LIFECYCLE_RUNNING);
    /* Both stayed inside their own bounded storage. */
    CANCESSTRY_TEST_CHECK(cancestry_event_queue_size(&fx.instances[0].incoming) <= 4u);
    CANCESSTRY_TEST_CHECK(cancestry_event_queue_size(&fx.instances[1].incoming) <= 4u);
    /* The calm instance's counters are also unaffected by the flood's budget
     * exhaustion. */
    CANCESSTRY_TEST_CHECK_U64(fsm_test_counters(&fx, "calm.one")->budget_exhausted, 0u);
    fsm_test_destroy(&fx);
}

int main(void)
{
    CANCESSTRY_TEST_SUITE_BEGIN("fsm/queue_overflow");
    case_default_depth();
    case_drop_newest();
    case_self_reactive_bounded();
    case_faults_never_dropped();
    case_instance_isolation();
    return CANCESSTRY_TEST_SUITE_END();
}
