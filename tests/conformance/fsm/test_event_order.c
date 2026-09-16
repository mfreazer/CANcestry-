/*
 * CANcestry FSM conformance: event ordering and determinism.
 *
 * Contract under test (docs/system/event-ordering.md sections 3, 4, 7, 8, 10;
 * docs/software/SwRS.md SW-FR-FSM-043, SW-FR-FSM-046; SYS-NF-001):
 *
 *   - an instance's queue is served in the normative selection order:
 *     timestamp_us ascending, then priority_class ascending, then sequence
 *     ascending - not first-in-first-out;
 *   - the sequence tie-break is deterministic push order, which for a state
 *     machine means declaration order, not name order;
 *   - faults outrank the generated events that share their timestamp;
 *   - instances are dispatched in declaration order, one at a time;
 *   - generated events carry the GENERATED class, the causing sequence, and the
 *     inherited timestamp, and are never applied recursively;
 *   - the same event sequence and time base reproduce the same trace.
 *
 * Test ids: FSM-EVENT-ORDER-001 .. FSM-EVENT-ORDER-006.
 */

#include "cancestry_fsm_conformance.h"

/*
 * Two one-shot timers with the same deadline, declared in the order zeta, alpha.
 * Alphabetical or hash-ordered dispatch would fire alpha first; the contract is
 * declaration order.
 */
static const char *const tiebreak_yaml = "schema_version: \"0.2.0\"\n"
                                         "state_machines:\n"
                                         "  - name: tiebreak\n"
                                         "    initial: START\n"
                                         "    variables:\n"
                                         "      - name: winner\n"
                                         "        type: integer\n"
                                         "        default: 0\n"
                                         "    timers:\n"
                                         "      - name: zeta\n"
                                         "        duration_ms: 3\n"
                                         "        repeat: false\n"
                                         "        auto_start: false\n"
                                         "      - name: alpha\n"
                                         "        duration_ms: 3\n"
                                         "        repeat: false\n"
                                         "        auto_start: false\n"
                                         "    states:\n"
                                         "      - name: START\n"
                                         "        entry:\n"
                                         "          - start_timer:\n"
                                         "              timer: zeta\n"
                                         "          - start_timer:\n"
                                         "              timer: alpha\n"
                                         "        transitions:\n"
                                         "          - event: timer_expired\n"
                                         "            timer: zeta\n"
                                         "            target: BY_ZETA\n"
                                         "            actions:\n"
                                         "              - set_variable:\n"
                                         "                  variable: winner\n"
                                         "                  value: 1\n"
                                         "          - event: timer_expired\n"
                                         "            timer: alpha\n"
                                         "            target: BY_ALPHA\n"
                                         "            actions:\n"
                                         "              - set_variable:\n"
                                         "                  variable: winner\n"
                                         "                  value: 2\n"
                                         "      - name: BY_ZETA\n"
                                         "        transitions:\n"
                                         "          - event: timer_expired\n"
                                         "            timer: alpha\n"
                                         "            target: BOTH\n"
                                         "            actions:\n"
                                         "              - set_variable:\n"
                                         "                  variable: winner\n"
                                         "                  value: 3\n"
                                         "      - name: BY_ALPHA\n"
                                         "        transitions:\n"
                                         "          - event: timer_expired\n"
                                         "            timer: zeta\n"
                                         "            target: BOTH\n"
                                         "            actions:\n"
                                         "              - set_variable:\n"
                                         "                  variable: winner\n"
                                         "                  value: 4\n"
                                         "      - name: BOTH\n"
                                         "instances:\n"
                                         "  - id: tb.one\n"
                                         "    machine: tiebreak\n"
                                         "    enabled: true\n";

/*
 * A fault raised by an entry action is queued after that transition's state
 * events, but its priority class is FAULT, so it must be selected first.
 */
static const char *const priority_yaml = "schema_version: \"0.2.0\"\n"
                                         "state_machines:\n"
                                         "  - name: prio\n"
                                         "    initial: A\n"
                                         "    states:\n"
                                         "      - name: A\n"
                                         "        transitions:\n"
                                         "          - event: can_rx\n"
                                         "            target: B\n"
                                         "      - name: B\n"
                                         "        entry:\n"
                                         "          - raise_fault:\n"
                                         "              code: LATE_FAULT\n"
                                         "              severity: warning\n"
                                         "        transitions:\n"
                                         "          - event: state_entered\n"
                                         "            target: C\n"
                                         "          - event: fault_raised\n"
                                         "            target: E\n"
                                         "      - name: C\n"
                                         "        entry:\n"
                                         "          - log:\n"
                                         "              level: info\n"
                                         "              message: reached C\n"
                                         "      - name: E\n"
                                         "        entry:\n"
                                         "          - log:\n"
                                         "              level: info\n"
                                         "              message: reached E\n"
                                         "instances:\n"
                                         "  - id: prio.one\n"
                                         "    machine: prio\n"
                                         "    enabled: true\n"
                                         "    subscriptions:\n"
                                         "      faults: true\n";

/* Two instances of one machine, to pin the dispatch order. */
static const char *const dispatch_yaml = "schema_version: \"0.2.0\"\n"
                                         "state_machines:\n"
                                         "  - name: dispatch\n"
                                         "    initial: IDLE\n"
                                         "    states:\n"
                                         "      - name: IDLE\n"
                                         "        transitions:\n"
                                         "          - event: can_rx\n"
                                         "            message: StatusMsg\n"
                                         "            target: ACTIVE\n"
                                         "      - name: ACTIVE\n"
                                         "instances:\n"
                                         "  - id: d.first\n"
                                         "    machine: dispatch\n"
                                         "    enabled: true\n"
                                         "  - id: d.second\n"
                                         "    machine: dispatch\n"
                                         "    enabled: true\n"
                                         "  - id: d.third\n"
                                         "    machine: dispatch\n"
                                         "    enabled: true\n";

/*
 * An event that arrives late with an earlier timestamp must still be selected
 * first, so the machine must not be first-in-first-out. max_events_per_activation
 * leaves the previous activation's state events in the queue, which is what makes
 * the interleaving observable in a single, deterministic step.
 */
static const char *const timestamp_yaml = "schema_version: \"0.2.0\"\n"
                                          "state_machines:\n"
                                          "  - name: tsorder\n"
                                          "    initial: A\n"
                                          "    states:\n"
                                          "      - name: A\n"
                                          "        transitions:\n"
                                          "          - event: can_rx\n"
                                          "            message: StatusMsg\n"
                                          "            target: B\n"
                                          "      - name: B\n"
                                          "        transitions:\n"
                                          "          - event: state_entered\n"
                                          "            target: C\n"
                                          "          - event: can_rx\n"
                                          "            message: StatusMsg\n"
                                          "            target: D\n"
                                          "      - name: C\n"
                                          "        transitions:\n"
                                          "          - event: can_rx\n"
                                          "            message: StatusMsg\n"
                                          "            target: FROM_C\n"
                                          "      - name: D\n"
                                          "      - name: FROM_C\n"
                                          "instances:\n"
                                          "  - id: ts.one\n"
                                          "    machine: tsorder\n"
                                          "    enabled: true\n";

/* FSM-EVENT-ORDER-001: equal timestamps and classes break on sequence, i.e. push
 * order, which for one activation's timer batch is declaration order. */
static void case_sequence_tiebreak(void)
{
    static fsm_test_fixture_t fx;
    int64_t winner = -1;

    CANCESSTRY_TEST_CASE("FSM-EVENT-ORDER-001 sequence breaks equal timestamps");
    CANCESSTRY_TEST_CHECK(fsm_test_init(&fx, tiebreak_yaml));
    CANCESSTRY_TEST_CHECK_U64(fsm_test_start(&fx, "tb.one"), CANCESTRY_FSM_OK);
    CANCESSTRY_TEST_CHECK_U64(fsm_test_ticks(&fx, 3u), CANCESTRY_FSM_OK);

    /* zeta is declared first, so it carries the lower sequence and wins. */
    CANCESSTRY_TEST_CHECK(fsm_test_has_line(&fx, "state 1 START->BY_ZETA"));
    CANCESSTRY_TEST_CHECK(!fsm_test_has_line(&fx, "state 1 START->BY_ALPHA"));
    CANCESSTRY_TEST_CHECK_STRING(fsm_test_state(&fx, "tb.one"), "BOTH");
    CANCESSTRY_TEST_CHECK_I64(fsm_test_variable_int(&fx, "tb.one", "winner", &winner),
                              CANCESTRY_FSM_OK);
    CANCESSTRY_TEST_CHECK_I64(winner, 3);
    CANCESSTRY_TEST_CHECK_U64(fsm_test_counters(&fx, "tb.one")->timer_expiries, 2u);
    /* One event per start-up entry, two expiries, and the four state events the two
     * transitions generated: the whole batch is served inside the one tick. */
    CANCESSTRY_TEST_CHECK_U64(fsm_test_counters(&fx, "tb.one")->events_processed, 7u);
    fsm_test_destroy(&fx);
}

/* FSM-EVENT-ORDER-002: priority beats sequence. */
static void case_priority_beats_sequence(void)
{
    static fsm_test_fixture_t fx;

    CANCESSTRY_TEST_CASE("FSM-EVENT-ORDER-002 fault class outranks generated events");
    CANCESSTRY_TEST_CHECK(fsm_test_init(&fx, priority_yaml));
    {
        cancestry_event_t event;

        CANCESSTRY_TEST_CHECK_U64(fsm_test_start(&fx, "prio.one"), CANCESTRY_FSM_OK);
        fsm_test_event_can_rx(&event, 1000u, FSM_TEST_IFACE_CAN0, FSM_TEST_MSG_STATUS, NULL, 0u);
        CANCESSTRY_TEST_CHECK_U64(fsm_test_process(&fx, &event), CANCESTRY_FSM_OK);
    }
    /*
     * B's entry raised the fault, so the fault was queued after B's state_entered
     * event, yet it is selected first: the machine lands in E, and B's
     * state_entered transition (to C) never fires.
     */
    CANCESSTRY_TEST_CHECK_STRING(fsm_test_state(&fx, "prio.one"), "E");
    CANCESSTRY_TEST_CHECK(fsm_test_has_line(&fx, "log 1 info reached E"));
    CANCESSTRY_TEST_CHECK(!fsm_test_has_line(&fx, "reached C"));
    CANCESSTRY_TEST_CHECK_U64(fsm_test_counters(&fx, "prio.one")->faults_raised, 1u);
    fsm_test_destroy(&fx);
}

/* FSM-EVENT-ORDER-003: timestamp ordering wins over arrival order. */
static void case_timestamp_ordering(void)
{
    static fsm_test_fixture_t fx;
    fsm_test_options_t options = fsm_test_options_default();

    CANCESSTRY_TEST_CASE("FSM-EVENT-ORDER-003 earlier timestamp is selected first");
    options.max_events_per_activation = 1u;
    CANCESSTRY_TEST_CHECK(fsm_test_init_with(&fx, timestamp_yaml, &options));
    {
        cancestry_event_t first;
        cancestry_event_t late;

        CANCESSTRY_TEST_CHECK_U64(fsm_test_start(&fx, "ts.one"), CANCESTRY_FSM_OK);
        /* The budget of one event leaves A->B's state events in the queue. */
        fsm_test_event_can_rx(&first, 1000u, FSM_TEST_IFACE_CAN0, FSM_TEST_MSG_STATUS, NULL, 0u);
        CANCESSTRY_TEST_CHECK_U64(fsm_test_process(&fx, &first), CANCESTRY_FSM_OK);
        CANCESSTRY_TEST_CHECK_U64(cancestry_event_queue_size(&fx.instances[0].incoming), 2u);

        /* This event carries an earlier timestamp than the queued state events. */
        fsm_test_event_can_rx(&late, 500u, FSM_TEST_IFACE_CAN0, FSM_TEST_MSG_STATUS, NULL, 0u);
        CANCESSTRY_TEST_CHECK_U64(fsm_test_process(&fx, &late), CANCESTRY_FSM_OK);
    }
    /* It is selected first, so B's can_rx transition runs, not its state_entered. */
    CANCESSTRY_TEST_CHECK_STRING(fsm_test_state(&fx, "ts.one"), "D");
    CANCESSTRY_TEST_CHECK(!fsm_test_has_line(&fx, "state 1 B->C"));
    CANCESSTRY_TEST_CHECK(fsm_test_has_line(&fx, "state 1 B->D"));
    fsm_test_destroy(&fx);
}

/* FSM-EVENT-ORDER-004: instances are dispatched in declaration order, whole
 * activation at a time. */
static void case_instance_dispatch_order(void)
{
    static fsm_test_fixture_t fx;
    cancestry_event_t event;

    CANCESSTRY_TEST_CASE("FSM-EVENT-ORDER-004 instance order is declaration order");
    CANCESSTRY_TEST_CHECK(fsm_test_init(&fx, dispatch_yaml));
    CANCESSTRY_TEST_CHECK_U64(fsm_test_start_all(&fx), 3u);
    fsm_test_reset(&fx);
    fsm_test_event_can_rx(&event, 1000u, FSM_TEST_IFACE_CAN0, FSM_TEST_MSG_STATUS, NULL, 0u);
    CANCESSTRY_TEST_CHECK_U64(fsm_test_process(&fx, &event), CANCESTRY_FSM_OK);
    /* One line per transition, in instance order: instance 1, then 2, then 3. No
     * other effects are due, so the recording is exactly the dispatch sequence. */
    CANCESSTRY_TEST_CHECK_U64(fsm_test_line_count(&fx), 3u);
    CANCESSTRY_TEST_CHECK_STRING(fsm_test_line_at(&fx, 0u), "state 1 IDLE->ACTIVE");
    CANCESSTRY_TEST_CHECK_STRING(fsm_test_line_at(&fx, 1u), "state 2 IDLE->ACTIVE");
    CANCESSTRY_TEST_CHECK_STRING(fsm_test_line_at(&fx, 2u), "state 3 IDLE->ACTIVE");
    CANCESSTRY_TEST_CHECK_U64(fx.engine.counters.events_delivered, 1u);
    fsm_test_destroy(&fx);
}

/* FSM-EVENT-ORDER-005: generated events follow event-ordering section 8. */
static void case_generated_event_fields(void)
{
    static fsm_test_fixture_t fx;
    cancestry_event_t event;
    cancestry_event_t published;
    size_t state_events = 0u;

    CANCESSTRY_TEST_CASE("FSM-EVENT-ORDER-005 generated events inherit class, time and cause");
    CANCESSTRY_TEST_CHECK(fsm_test_init(&fx, dispatch_yaml));
    CANCESSTRY_TEST_CHECK_U64(fsm_test_start(&fx, "d.first"), CANCESTRY_FSM_OK);
    /* Start emitted the initial state_entered on the global queue; take its
     * sequence so the cause link of the transition's events can be checked. */
    CANCESSTRY_TEST_CHECK_U64(cancestry_event_queue_pop(&fx.global_queue, &published),
                              CANCESTRY_EVENT_QUEUE_OK);
    CANCESSTRY_TEST_CHECK_U64(published.priority_class, CANCESTRY_PRIORITY_CLASS_GENERATED);
    CANCESSTRY_TEST_CHECK_U64(published.type, CANCESTRY_EVENT_TYPE_STATE_ENTERED);
    CANCESSTRY_TEST_CHECK_U64(published.cause_sequence, CANCESTRY_SEQUENCE_NONE);
    CANCESSTRY_TEST_CHECK_U64(published.timestamp_us, 0u);

    fsm_test_event_can_rx(&event, 7000u, FSM_TEST_IFACE_CAN0, FSM_TEST_MSG_STATUS, NULL, 0u);
    CANCESSTRY_TEST_CHECK_U64(fsm_test_process(&fx, &event), CANCESTRY_FSM_OK);
    while (cancestry_event_queue_pop(&fx.global_queue, &published) == CANCESTRY_EVENT_QUEUE_OK) {
        if (published.type != CANCESTRY_EVENT_TYPE_STATE_ENTERED &&
            published.type != CANCESTRY_EVENT_TYPE_STATE_EXITED) {
            continue;
        }
        state_events++;
        /* GENERATED class, timestamp inherited from the causing event. */
        CANCESSTRY_TEST_CHECK_U64(published.priority_class, CANCESTRY_PRIORITY_CLASS_GENERATED);
        CANCESSTRY_TEST_CHECK_U64(published.timestamp_us, 7000u);
        /* The causing event's sequence is carried, so a trace consumer can
         * attribute the event without searching (event-ordering.md section 8). */
        CANCESSTRY_TEST_CHECK(published.cause_sequence > CANCESTRY_SEQUENCE_NONE);
    }
    CANCESSTRY_TEST_CHECK_U64(state_events, 2u);
    fsm_test_destroy(&fx);
}

/* FSM-EVENT-ORDER-006: golden trace - two identical runs reproduce identical
 * output, byte for byte. */
static void case_golden_trace(void)
{
    static fsm_test_fixture_t first;
    static fsm_test_fixture_t second;
    char first_trace[8192];
    char second_trace[8192];
    size_t first_length;
    size_t second_length;

    CANCESSTRY_TEST_CASE("FSM-EVENT-ORDER-006 golden trace is reproducible");
    CANCESSTRY_TEST_CHECK(fsm_test_init(&first, tiebreak_yaml));
    CANCESSTRY_TEST_CHECK(fsm_test_init(&second, tiebreak_yaml));
    CANCESSTRY_TEST_CHECK_U64(fsm_test_start(&first, "tb.one"), CANCESTRY_FSM_OK);
    CANCESSTRY_TEST_CHECK_U64(fsm_test_start(&second, "tb.one"), CANCESTRY_FSM_OK);
    CANCESSTRY_TEST_CHECK_U64(fsm_test_ticks(&first, 5u), CANCESTRY_FSM_OK);
    CANCESSTRY_TEST_CHECK_U64(fsm_test_ticks(&second, 5u), CANCESTRY_FSM_OK);

    first_length = fsm_test_render_trace(&first, first_trace, sizeof(first_trace));
    second_length = fsm_test_render_trace(&second, second_trace, sizeof(second_trace));
    CANCESSTRY_TEST_CHECK(first_length > 0u);
    CANCESSTRY_TEST_CHECK_U64(first_length, second_length);
    CANCESSTRY_TEST_CHECK(memcmp(first_trace, second_trace, first_length) == 0);
    /* The golden content itself: the ordering contract is visible in the trace. */
    CANCESSTRY_TEST_CHECK(strstr(first_trace, "transition 1 (initial)->START") != NULL);
    CANCESSTRY_TEST_CHECK(strstr(first_trace, "zeta expired") != NULL);
    CANCESSTRY_TEST_CHECK(strstr(first_trace, "3000 1 timer 0 zeta expired") != NULL);
    CANCESSTRY_TEST_CHECK(strstr(first_trace, "3000 1 timer 0 alpha expired") != NULL);
    /* And the two runs are stable when repeated once more, in the same order. */
    CANCESSTRY_TEST_CHECK_U64(first.engine.counters.ticks, 5u);
    CANCESSTRY_TEST_CHECK_U64(second.engine.counters.ticks, 5u);
    fsm_test_destroy(&first);
    fsm_test_destroy(&second);
}

int main(void)
{
    CANCESSTRY_TEST_SUITE_BEGIN("fsm/event_order");
    case_sequence_tiebreak();
    case_priority_beats_sequence();
    case_instance_dispatch_order();
    case_timestamp_ordering();
    case_generated_event_fields();
    case_golden_trace();
    return CANCESSTRY_TEST_SUITE_END();
}
