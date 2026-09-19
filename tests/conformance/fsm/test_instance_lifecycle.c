/*
 * CANcestry FSM conformance: instance lifecycle.
 *
 * Contract under test (docs/packages/fsm-spec.md section 3, SwAD.md section 5,
 * docs/system/mode-fault-state-machine.md section 5):
 *
 *   DISABLED -> READY -> RUNNING -> SUSPENDED -> FAULT
 *
 * - an instance declared enabled loads into READY, one declared disabled stays
 *   DISABLED (SW-FR-FSM-004);
 * - no state is entered and no entry action runs before start (SW-FR-FSM-005,
 *   SW-FR-FSM-007), and variables are initialised from the declaration before it
 *   (SW-FR-FSM-033);
 * - only RUNNING instances process events and run timers;
 * - suspend is a pause (state and variables retained, no exit actions), resume
 *   continues, fault is containment recovered only by reset;
 * - an illegal lifecycle request is refused with CANCESTRY_FSM_ERR_STATE and
 *   changes nothing (fail closed);
 * - faulting one instance cannot disturb another (SW-FR-FSM-047).
 *
 * Test ids: FSM-LIFECYCLE-001 .. FSM-LIFECYCLE-008.
 */

#include "cancestry_fsm_conformance.h"

/* The machine and instances every case in this file shares. */
static const char *const lifecycle_yaml = "schema_version: \"0.2.0\"\n"
                                          "state_machines:\n"
                                          "  - name: cluster\n"
                                          "    description: Lifecycle contract subject\n"
                                          "    initial: IDLE\n"
                                          "    variables:\n"
                                          "      - name: cycles\n"
                                          "        type: integer\n"
                                          "        default: 1\n"
                                          "      - name: awake\n"
                                          "        type: boolean\n"
                                          "        default: false\n"
                                          "      - name: ratio\n"
                                          "        type: float\n"
                                          "        default: 0.5\n"
                                          "    timers:\n"
                                          "      - name: heartbeat\n"
                                          "        duration_ms: 4\n"
                                          "        repeat: true\n"
                                          "        auto_start: false\n"
                                          "    states:\n"
                                          "      - name: IDLE\n"
                                          "        entry:\n"
                                          "          - set_variable:\n"
                                          "              variable: cycles\n"
                                          "              value: 10\n"
                                          "        transitions:\n"
                                          "          - event: can_rx\n"
                                          "            message: StatusMsg\n"
                                          "            target: ACTIVE\n"
                                          "      - name: ACTIVE\n"
                                          "        entry:\n"
                                          "          - start_timer:\n"
                                          "              timer: heartbeat\n"
                                          "          - set_variable:\n"
                                          "              variable: awake\n"
                                          "              value: true\n"
                                          "        exit:\n"
                                          "          - stop_timer:\n"
                                          "              timer: heartbeat\n"
                                          "          - set_variable:\n"
                                          "              variable: awake\n"
                                          "              value: false\n"
                                          "        transitions:\n"
                                          "          - event: timer_expired\n"
                                          "            timer: heartbeat\n"
                                          "            target: PULSE\n"
                                          "      - name: PULSE\n"
                                          "        entry:\n"
                                          "          - set_variable:\n"
                                          "              variable: cycles\n"
                                          "              value: 1\n"
                                          "  - name: watchdog\n"
                                          "    initial: ARMED\n"
                                          "    states:\n"
                                          "      - name: ARMED\n"
                                          "        transitions:\n"
                                          "          - event: can_rx\n"
                                          "            target: TRIPPED\n"
                                          "      - name: TRIPPED\n"
                                          "        entry:\n"
                                          "          - raise_fault:\n"
                                          "              code: WATCHDOG_TIMEOUT\n"
                                          "              severity: critical\n"
                                          "instances:\n"
                                          "  - id: cluster.main\n"
                                          "    machine: cluster\n"
                                          "    enabled: true\n"
                                          "  - id: cluster.spare\n"
                                          "    machine: cluster\n"
                                          "    enabled: false\n"
                                          "  - id: watchdog.one\n"
                                          "    machine: watchdog\n"
                                          "    enabled: true\n"
                                          "  - id: watchdog.two\n"
                                          "    machine: watchdog\n"
                                          "    enabled: true\n";

/* FSM-LIFECYCLE-001: what loading leaves an instance in. */
static void case_loaded_states(void)
{
    static fsm_test_fixture_t fx;
    int64_t cycles = -1;

    CANCESSTRY_TEST_CASE("FSM-LIFECYCLE-001 load leaves READY and DISABLED");
    CANCESSTRY_TEST_CHECK(fsm_test_init(&fx, lifecycle_yaml));
    CANCESSTRY_TEST_CHECK_U64(cancestry_fsm_engine_instance_count(&fx.engine), 4u);

    /* enabled: true loads into READY, enabled: false into DISABLED. */
    CANCESSTRY_TEST_CHECK_U64(fsm_test_lifecycle(&fx, "cluster.main"),
                              CANCESTRY_FSM_INSTANCE_LIFECYCLE_READY);
    CANCESSTRY_TEST_CHECK_U64(fsm_test_lifecycle(&fx, "cluster.spare"),
                              CANCESTRY_FSM_INSTANCE_LIFECYCLE_DISABLED);
    /* READY means "no state entered yet": the initial state is entered on start. */
    CANCESSTRY_TEST_CHECK(fsm_test_state(&fx, "cluster.main") == NULL);
    CANCESSTRY_TEST_CHECK(fsm_test_state(&fx, "cluster.spare") == NULL);

    /* Variables are initialised from the declaration, before any entry action. */
    CANCESSTRY_TEST_CHECK_I64(fsm_test_variable_int(&fx, "cluster.main", "cycles", &cycles),
                              CANCESTRY_FSM_OK);
    CANCESSTRY_TEST_CHECK_I64(cycles, 1);

    /* A READY instance is inert: events reach nobody. */
    {
        cancestry_event_t event;

        fsm_test_event_can_rx(&event, 1000u, FSM_TEST_IFACE_CAN0, FSM_TEST_MSG_STATUS, NULL, 0u);
        CANCESSTRY_TEST_CHECK_U64(fsm_test_process(&fx, &event), CANCESTRY_FSM_OK);
        CANCESSTRY_TEST_CHECK_U64(fx.engine.counters.events_ignored, 1u);
        CANCESSTRY_TEST_CHECK(fsm_test_state(&fx, "cluster.main") == NULL);
        CANCESSTRY_TEST_CHECK_U64(fsm_test_line_count(&fx), 0u);
    }
    /* The declaration order is the dispatch order, and ids resolve instances. */
    CANCESSTRY_TEST_CHECK_U64(cancestry_fsm_instance_at(&fx.engine, 0u)->id, 1u);
    CANCESSTRY_TEST_CHECK_STRING(cancestry_fsm_instance_at(&fx.engine, 0u)->def->id,
                                 "cluster.main");
    CANCESSTRY_TEST_CHECK(cancestry_fsm_instance_by_id(&fx.engine, "nope") == NULL);
    fsm_test_destroy(&fx);
}

/* FSM-LIFECYCLE-002: DISABLED -> READY -> RUNNING. */
static void case_enable_and_start(void)
{
    static fsm_test_fixture_t fx;
    cancestry_fsm_instance_t *spare;
    int64_t cycles = -1;

    CANCESSTRY_TEST_CASE("FSM-LIFECYCLE-002 DISABLED to READY to RUNNING");
    CANCESSTRY_TEST_CHECK(fsm_test_init(&fx, lifecycle_yaml));

    spare = fsm_test_instance(&fx, "cluster.spare");
    CANCESSTRY_TEST_CHECK_U64(cancestry_fsm_instance_enable(&fx.engine, spare),
                              CANCESTRY_FSM_OK);
    CANCESSTRY_TEST_CHECK_U64(spare->lifecycle, CANCESTRY_FSM_INSTANCE_LIFECYCLE_READY);
    /* Enabling twice is not a legal transition and changes nothing. */
    CANCESSTRY_TEST_CHECK_U64(cancestry_fsm_instance_enable(&fx.engine, spare),
                              CANCESTRY_FSM_ERR_STATE);

    CANCESSTRY_TEST_CHECK_U64(fsm_test_start(&fx, "cluster.spare"), CANCESTRY_FSM_OK);
    CANCESSTRY_TEST_CHECK_U64(spare->lifecycle, CANCESTRY_FSM_INSTANCE_LIFECYCLE_RUNNING);
    CANCESSTRY_TEST_CHECK_STRING(fsm_test_state(&fx, "cluster.spare"), "IDLE");
    /* start() entered the initial state, so its entry action has run. */
    CANCESSTRY_TEST_CHECK_I64(
        fsm_test_variable_int(&fx, "cluster.spare", "cycles", &cycles), CANCESTRY_FSM_OK);
    CANCESSTRY_TEST_CHECK_I64(cycles, 10);
    /* ...and the main instance is untouched: entry actions are per instance. */
    CANCESSTRY_TEST_CHECK_I64(fsm_test_variable_int(&fx, "cluster.main", "cycles", &cycles),
                              CANCESTRY_FSM_OK);
    CANCESSTRY_TEST_CHECK_I64(cycles, 1);
    /* Starting a RUNNING instance is refused, not re-entry. */
    CANCESSTRY_TEST_CHECK_U64(fsm_test_start(&fx, "cluster.spare"), CANCESTRY_FSM_ERR_STATE);
    CANCESSTRY_TEST_CHECK_U64(fsm_test_counters(&fx, "cluster.spare")->transitions, 1u);
    CANCESSTRY_TEST_CHECK(fsm_test_has_line(&fx, "state 2 (initial)->IDLE"));
    fsm_test_destroy(&fx);
}

/* FSM-LIFECYCLE-003: RUNNING -> SUSPENDED -> RUNNING. */
static void case_suspend_and_resume(void)
{
    static fsm_test_fixture_t fx;
    cancestry_event_t event;
    int64_t cycles = -1;
    uint32_t processed_before = 0u;

    CANCESSTRY_TEST_CASE("FSM-LIFECYCLE-003 suspend is inert, resume continues");
    CANCESSTRY_TEST_CHECK(fsm_test_init(&fx, lifecycle_yaml));
    /* The declared "enabled: false" instance needs enable then start first, which
     * also proves a disabled instance is not runnable by any shortcut. */
    CANCESSTRY_TEST_CHECK_U64(fsm_test_start(&fx, "cluster.spare"), CANCESTRY_FSM_ERR_STATE);
    CANCESSTRY_TEST_CHECK_U64(
        cancestry_fsm_instance_enable(&fx.engine, fsm_test_instance(&fx, "cluster.spare")),
        CANCESTRY_FSM_OK);
    /* Suspend is only legal from RUNNING, and READY is not RUNNING. */
    CANCESSTRY_TEST_CHECK_U64(
        cancestry_fsm_instance_suspend(&fx.engine, fsm_test_instance(&fx, "cluster.spare")),
        CANCESTRY_FSM_ERR_STATE);
    CANCESSTRY_TEST_CHECK_U64(fsm_test_start(&fx, "cluster.spare"), CANCESTRY_FSM_OK);
    CANCESSTRY_TEST_CHECK_U64(fsm_test_start(&fx, "cluster.main"), CANCESTRY_FSM_OK);
    CANCESSTRY_TEST_CHECK_U64(fsm_test_start(&fx, "watchdog.one"), CANCESTRY_FSM_OK);
    CANCESSTRY_TEST_CHECK_U64(fsm_test_start(&fx, "watchdog.two"), CANCESTRY_FSM_OK);
    CANCESSTRY_TEST_CHECK_STRING(fsm_test_state(&fx, "cluster.main"), "IDLE");

    /* From RUNNING, suspend is the documented edge. */
    CANCESSTRY_TEST_CHECK_U64(
        cancestry_fsm_instance_suspend(&fx.engine, fsm_test_instance(&fx, "cluster.main")),
        CANCESTRY_FSM_OK);
    CANCESSTRY_TEST_CHECK_U64(fsm_test_lifecycle(&fx, "cluster.main"),
                              CANCESTRY_FSM_INSTANCE_LIFECYCLE_SUSPENDED);
    /* Suspension is a pause: no exit actions ran, the state is retained. */
    CANCESSTRY_TEST_CHECK_STRING(fsm_test_state(&fx, "cluster.main"), "IDLE");
    CANCESSTRY_TEST_CHECK_I64(fsm_test_variable_int(&fx, "cluster.main", "cycles", &cycles),
                              CANCESTRY_FSM_OK);
    CANCESSTRY_TEST_CHECK_I64(cycles, 10);

    /* A suspended instance receives nothing; the others still do. */
    fsm_test_reset(&fx);
    processed_before = fsm_test_counters(&fx, "cluster.main")->events_processed;
    fsm_test_event_can_rx(&event, 2000u, FSM_TEST_IFACE_CAN0, FSM_TEST_MSG_STATUS, NULL, 0u);
    CANCESSTRY_TEST_CHECK_U64(fsm_test_process(&fx, &event), CANCESTRY_FSM_OK);
    CANCESSTRY_TEST_CHECK_STRING(fsm_test_state(&fx, "cluster.main"), "IDLE");
    CANCESSTRY_TEST_CHECK_STRING(fsm_test_state(&fx, "cluster.spare"), "ACTIVE");
    CANCESSTRY_TEST_CHECK_U64(processed_before,
                             fsm_test_counters(&fx, "cluster.main")->events_processed);

    /* Resume does not replay the skipped event: it is not queued anywhere. */
    CANCESSTRY_TEST_CHECK_U64(
        cancestry_fsm_instance_resume(&fx.engine, fsm_test_instance(&fx, "cluster.main")),
        CANCESTRY_FSM_OK);
    CANCESSTRY_TEST_CHECK_STRING(fsm_test_state(&fx, "cluster.main"), "IDLE");
    CANCESSTRY_TEST_CHECK_U64(
        cancestry_fsm_instance_resume(&fx.engine, fsm_test_instance(&fx, "cluster.main")),
        CANCESTRY_FSM_ERR_STATE);

    /* Now the same event moves it. */
    fsm_test_event_can_rx(&event, 3000u, FSM_TEST_IFACE_CAN0, FSM_TEST_MSG_STATUS, NULL, 0u);
    CANCESSTRY_TEST_CHECK_U64(fsm_test_process(&fx, &event), CANCESTRY_FSM_OK);
    CANCESSTRY_TEST_CHECK_STRING(fsm_test_state(&fx, "cluster.main"), "ACTIVE");
    CANCESSTRY_TEST_CHECK(fsm_test_has_line(&fx, "timer 1 start_timer heartbeat duration=4 repeat=1"));
    CANCESSTRY_TEST_CHECK_I64(fsm_test_variable_int(&fx, "cluster.main", "awake", &cycles),
                              CANCESTRY_FSM_OK);
    CANCESSTRY_TEST_CHECK_I64(cycles, 1);
    fsm_test_destroy(&fx);
}

/* FSM-LIFECYCLE-004: RUNNING -> FAULT -> READY by reset only. */
static void case_fault_and_reset(void)
{
    static fsm_test_fixture_t fx;
    cancestry_event_t event;
    uint32_t processed_before = 0u;
    uint32_t queued_before = 0u;

    CANCESSTRY_TEST_CASE("FSM-LIFECYCLE-004 FAULT needs reset, nothing else");
    CANCESSTRY_TEST_CHECK(fsm_test_init(&fx, lifecycle_yaml));
    CANCESSTRY_TEST_CHECK_U64(fsm_test_start(&fx, "cluster.main"), CANCESTRY_FSM_OK);
    CANCESSTRY_TEST_CHECK_U64(
        cancestry_fsm_instance_fault(&fx.engine, fsm_test_instance(&fx, "cluster.main")),
        CANCESTRY_FSM_OK);
    CANCESSTRY_TEST_CHECK_U64(fsm_test_lifecycle(&fx, "cluster.main"),
                              CANCESTRY_FSM_INSTANCE_LIFECYCLE_FAULT);

    /* From FAULT neither resume nor start is legal. */
    CANCESSTRY_TEST_CHECK_U64(
        cancestry_fsm_instance_resume(&fx.engine, fsm_test_instance(&fx, "cluster.main")),
        CANCESTRY_FSM_ERR_STATE);
    CANCESSTRY_TEST_CHECK_U64(fsm_test_start(&fx, "cluster.main"), CANCESTRY_FSM_ERR_STATE);
    CANCESSTRY_TEST_CHECK_U64(
        cancestry_fsm_instance_suspend(&fx.engine, fsm_test_instance(&fx, "cluster.main")),
        CANCESTRY_FSM_ERR_STATE);

    /* FAULT is inert: the event reaches nobody, so no queueing and no processing. */
    processed_before = fsm_test_counters(&fx, "cluster.main")->events_processed;
    queued_before = fsm_test_counters(&fx, "cluster.main")->events_queued;
    fsm_test_event_can_rx(&event, 4000u, FSM_TEST_IFACE_CAN0, FSM_TEST_MSG_STATUS, NULL, 0u);
    CANCESSTRY_TEST_CHECK_U64(fsm_test_process(&fx, &event), CANCESTRY_FSM_OK);
    CANCESSTRY_TEST_CHECK_U64(fsm_test_counters(&fx, "cluster.main")->events_processed,
                              processed_before);
    CANCESSTRY_TEST_CHECK_U64(fsm_test_counters(&fx, "cluster.main")->events_queued,
                              queued_before);

    /* Reset recovers to READY, and the timer is stopped. */
    CANCESSTRY_TEST_CHECK_U64(
        cancestry_fsm_instance_reset(&fx.engine, fsm_test_instance(&fx, "cluster.main")),
        CANCESTRY_FSM_OK);
    CANCESSTRY_TEST_CHECK_U64(fsm_test_lifecycle(&fx, "cluster.main"),
                              CANCESTRY_FSM_INSTANCE_LIFECYCLE_READY);
    CANCESSTRY_TEST_CHECK(fsm_test_state(&fx, "cluster.main") == NULL);
    /* Reset re-initialises the variables from the declaration, not from the last
     * value the entry action wrote. */
    {
        int64_t cycles = -1;

        CANCESSTRY_TEST_CHECK_I64(fsm_test_variable_int(&fx, "cluster.main", "cycles", &cycles),
                                  CANCESTRY_FSM_OK);
        CANCESSTRY_TEST_CHECK_I64(cycles, 1);
    }
    CANCESSTRY_TEST_CHECK_U64(fsm_test_start(&fx, "cluster.main"), CANCESTRY_FSM_OK);
    CANCESSTRY_TEST_CHECK_STRING(fsm_test_state(&fx, "cluster.main"), "IDLE");
    /* Reset from READY or RUNNING is not a legal edge. */
    CANCESSTRY_TEST_CHECK_U64(
        cancestry_fsm_instance_reset(&fx.engine, fsm_test_instance(&fx, "cluster.main")),
        CANCESTRY_FSM_ERR_STATE);
    fsm_test_destroy(&fx);
}

/* FSM-LIFECYCLE-005: a critical raise_fault action lands the instance in FAULT. */
static void case_critical_fault_action(void)
{
    static fsm_test_fixture_t fx;
    cancestry_event_t event;

    CANCESSTRY_TEST_CASE("FSM-LIFECYCLE-005 critical fault contains the instance");
    CANCESSTRY_TEST_CHECK(fsm_test_init(&fx, lifecycle_yaml));
    CANCESSTRY_TEST_CHECK_U64(fsm_test_start_all(&fx), 3u);
    CANCESSTRY_TEST_CHECK_STRING(fsm_test_state(&fx, "watchdog.one"), "ARMED");

    fsm_test_event_can_rx(&event, 5000u, FSM_TEST_IFACE_CAN0, FSM_TEST_MSG_STATUS, NULL, 0u);
    CANCESSTRY_TEST_CHECK_U64(fsm_test_process(&fx, &event), CANCESTRY_FSM_OK);
    CANCESSTRY_TEST_CHECK_U64(fsm_test_lifecycle(&fx, "watchdog.one"),
                              CANCESTRY_FSM_INSTANCE_LIFECYCLE_FAULT);
    CANCESSTRY_TEST_CHECK(fsm_test_has_line(&fx, "fault 3 WATCHDOG_TIMEOUT critical"));
    CANCESSTRY_TEST_CHECK_U64(fsm_test_counters(&fx, "watchdog.one")->faults_raised, 1u);
    /*
     * Everything published on the shared bus is accounted for: one state_entered per
     * started instance plus an exited/entered pair per transition, and exactly one
     * fault per watchdog; the queue's reserved fault capacity retains them in
     * this bounded fixture (all-fault saturation is a separate QA-EV-01 case).
     */
    {
        size_t faults = 0u;
        size_t state_events = 0u;
        cancestry_event_t published;

        while (cancestry_event_queue_pop(&fx.global_queue, &published) ==
               CANCESTRY_EVENT_QUEUE_OK) {
            if (published.type == CANCESTRY_EVENT_TYPE_FAULT_RAISED) {
                faults++;
            } else if (published.type == CANCESTRY_EVENT_TYPE_STATE_ENTERED ||
                       published.type == CANCESTRY_EVENT_TYPE_STATE_EXITED) {
                state_events++;
            }
        }
        CANCESSTRY_TEST_CHECK_U64(faults, 2u);
        CANCESSTRY_TEST_CHECK_U64(state_events, 9u);
    }
    CANCESSTRY_TEST_CHECK_U64(cancestry_event_queue_counters(&fx.global_queue)->dropped, 0u);
    /*
     * The sibling instance reacted too - the same event reaches every subscriber -
     * but each one's fault count, state and lifecycle are its own, and neither
     * faulted instance disturbed the cluster instances (SW-FR-FSM-047).
     */
    CANCESSTRY_TEST_CHECK_STRING(fsm_test_state(&fx, "watchdog.two"), "TRIPPED");
    CANCESSTRY_TEST_CHECK_U64(fsm_test_lifecycle(&fx, "watchdog.two"),
                              CANCESTRY_FSM_INSTANCE_LIFECYCLE_FAULT);
    CANCESSTRY_TEST_CHECK_U64(fsm_test_counters(&fx, "watchdog.two")->faults_raised, 1u);
    CANCESSTRY_TEST_CHECK_U64(fsm_test_counters(&fx, "watchdog.one")->faults_raised, 1u);
    CANCESSTRY_TEST_CHECK_U64(fsm_test_counters(&fx, "watchdog.one")->transitions, 2u);
    CANCESSTRY_TEST_CHECK_U64(fsm_test_lifecycle(&fx, "cluster.main"),
                              CANCESTRY_FSM_INSTANCE_LIFECYCLE_RUNNING);
    fsm_test_destroy(&fx);
}

/* FSM-LIFECYCLE-006: disable stops everything; the queue is not replayed. */
static void case_disable_clears_pending(void)
{
    static fsm_test_fixture_t fx;
    cancestry_event_t event;
    uint32_t queued_before = 0u;

    CANCESSTRY_TEST_CASE("FSM-LIFECYCLE-006 disable stops timers and drops the queue");
    CANCESSTRY_TEST_CHECK(fsm_test_init(&fx, lifecycle_yaml));
    CANCESSTRY_TEST_CHECK_U64(fsm_test_start(&fx, "cluster.main"), CANCESTRY_FSM_OK);
    /* Park an event in the incoming queue by disabling... */
    CANCESSTRY_TEST_CHECK_U64(
        cancestry_fsm_instance_disable(&fx.engine, fsm_test_instance(&fx, "cluster.main")),
        CANCESTRY_FSM_OK);
    CANCESSTRY_TEST_CHECK_U64(fsm_test_lifecycle(&fx, "cluster.main"),
                              CANCESTRY_FSM_INSTANCE_LIFECYCLE_DISABLED);
    /* A disabled instance receives nothing at all. */
    queued_before = fsm_test_counters(&fx, "cluster.main")->events_queued;
    fsm_test_event_can_rx(&event, 6000u, FSM_TEST_IFACE_CAN0, FSM_TEST_MSG_STATUS, NULL, 0u);
    CANCESSTRY_TEST_CHECK_U64(fsm_test_process(&fx, &event), CANCESTRY_FSM_OK);
    CANCESSTRY_TEST_CHECK_U64(fsm_test_counters(&fx, "cluster.main")->events_queued,
                              queued_before);
    /* The timer the ACTIVE entry started is no longer running, so ticks are quiet. */
    CANCESSTRY_TEST_CHECK_U64(fsm_test_ticks(&fx, 10u), CANCESTRY_FSM_OK);
    CANCESSTRY_TEST_CHECK_U64(fsm_test_counters(&fx, "cluster.main")->timer_expiries, 0u);
    /* Disable is idempotent; re-enabling returns to READY and forgets the state. */
    CANCESSTRY_TEST_CHECK_U64(
        cancestry_fsm_instance_disable(&fx.engine, fsm_test_instance(&fx, "cluster.main")),
        CANCESTRY_FSM_OK);
    CANCESSTRY_TEST_CHECK_U64(
        cancestry_fsm_instance_enable(&fx.engine, fsm_test_instance(&fx, "cluster.main")),
        CANCESTRY_FSM_OK);
    CANCESSTRY_TEST_CHECK(fsm_test_state(&fx, "cluster.main") == NULL);
    fsm_test_destroy(&fx);
}

/* FSM-LIFECYCLE-007: SUSPENDED -> FAULT and SUSPENDED -> READY. */
static void case_suspended_edges(void)
{
    static fsm_test_fixture_t fx;

    CANCESSTRY_TEST_CASE("FSM-LIFECYCLE-007 suspended may fault or reset");
    CANCESSTRY_TEST_CHECK(fsm_test_init(&fx, lifecycle_yaml));
    CANCESSTRY_TEST_CHECK_U64(fsm_test_start(&fx, "cluster.main"), CANCESTRY_FSM_OK);
    CANCESSTRY_TEST_CHECK_U64(
        cancestry_fsm_instance_suspend(&fx.engine, fsm_test_instance(&fx, "cluster.main")),
        CANCESTRY_FSM_OK);
    CANCESSTRY_TEST_CHECK_U64(
        cancestry_fsm_instance_fault(&fx.engine, fsm_test_instance(&fx, "cluster.main")),
        CANCESTRY_FSM_OK);
    CANCESSTRY_TEST_CHECK_U64(fsm_test_lifecycle(&fx, "cluster.main"),
                              CANCESTRY_FSM_INSTANCE_LIFECYCLE_FAULT);
    CANCESSTRY_TEST_CHECK_U64(
        cancestry_fsm_instance_reset(&fx.engine, fsm_test_instance(&fx, "cluster.main")),
        CANCESTRY_FSM_OK);
    CANCESSTRY_TEST_CHECK_U64(fsm_test_lifecycle(&fx, "cluster.main"),
                              CANCESTRY_FSM_INSTANCE_LIFECYCLE_READY);
    /* DISABLED -> READY -> RUNNING is reachable again after full recovery. */
    CANCESSTRY_TEST_CHECK_U64(fsm_test_start(&fx, "cluster.main"), CANCESTRY_FSM_OK);
    CANCESSTRY_TEST_CHECK_STRING(fsm_test_state(&fx, "cluster.main"), "IDLE");
    fsm_test_destroy(&fx);
}

/* FSM-LIFECYCLE-008: the engine is usable with nothing bound and with a bad
 * configuration, without crashing or silently accepting nonsense. */
static void case_engine_argument_handling(void)
{
    static fsm_test_fixture_t fx;
    cancestry_event_t event;

    CANCESSTRY_TEST_CASE("FSM-LIFECYCLE-008 null and malformed arguments fail closed");
    CANCESSTRY_TEST_CHECK(fsm_test_init(&fx, lifecycle_yaml));
    CANCESSTRY_TEST_CHECK(cancestry_fsm_engine_init(&fx.engine, NULL) == false);
    CANCESSTRY_TEST_CHECK(!cancestry_fsm_engine_is_valid(&fx.engine));
    CANCESSTRY_TEST_CHECK_U64(cancestry_fsm_engine_process_event(&fx.engine, &event),
                              CANCESTRY_FSM_ERR_NULL);

    /* Rebuild the fixture, then hand it a malformed event. */
    fsm_test_destroy(&fx);
    CANCESSTRY_TEST_CHECK(fsm_test_init(&fx, lifecycle_yaml));
    cancestry_event_init(&event);
    CANCESSTRY_TEST_CHECK_U64(fsm_test_process(&fx, &event), CANCESTRY_FSM_ERR_ARGUMENT);
    CANCESSTRY_TEST_CHECK_U64(fsm_test_process(&fx, NULL), CANCESTRY_FSM_ERR_NULL);
    CANCESSTRY_TEST_CHECK(cancestry_fsm_instance_counters(NULL) == NULL);
    CANCESSTRY_TEST_CHECK_U64(cancestry_fsm_instance_lifecycle(NULL),
                              CANCESTRY_FSM_INSTANCE_LIFECYCLE_DISABLED);
    CANCESSTRY_TEST_CHECK_U64(cancestry_fsm_instance_enable(&fx.engine, NULL),
                              CANCESTRY_FSM_ERR_NULL);
    CANCESSTRY_TEST_CHECK_U64(cancestry_fsm_instance_get_variable(&fx.engine, NULL, "x", NULL),
                              CANCESTRY_FSM_ERR_NULL);
    fsm_test_destroy(&fx);
}

int main(void)
{
    CANCESSTRY_TEST_SUITE_BEGIN("fsm/instance_lifecycle");
    case_loaded_states();
    case_enable_and_start();
    case_suspend_and_resume();
    case_fault_and_reset();
    case_critical_fault_action();
    case_disable_clears_pending();
    case_suspended_edges();
    case_engine_argument_handling();
    return CANCESSTRY_TEST_SUITE_END();
}
