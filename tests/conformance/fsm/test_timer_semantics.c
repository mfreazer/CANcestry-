/*
 * CANcestry FSM conformance: timer semantics.
 *
 * Contract under test (docs/packages/fsm-spec.md section 9,
 * docs/software/SwAD.md section 8, docs/software/SwRS.md SW-FR-FSM-026..030 and
 * SW-FR-FSM-044, docs/system/mode-fault-state-machine.md section 5 "Timer
 * overrun"):
 *
 *   - one-shot timers fire exactly once;
 *   - periodic timers keep firing on their scheduled deadlines;
 *   - a skipped periodic period emits ONE event carrying missed_count, and the
 *     deadline advances by whole periods (no drift, no catch-up burst);
 *   - start, stop and reset behave as declared, auto_start starts a timer with
 *     the instance, and the tick is 1 ms;
 *   - expiry order is deadline, then instance, then declaration.
 *
 * Test ids: FSM-TIMER-001 .. FSM-TIMER-006.
 */

#include "cancestry_fsm_conformance.h"

static const char *const oneshot_yaml = "schema_version: \"0.2.0\"\n"
                                        "state_machines:\n"
                                        "  - name: lab\n"
                                        "    initial: IDLE\n"
                                        "    timers:\n"
                                        "      - name: ping\n"
                                        "        duration_ms: 3\n"
                                        "        repeat: false\n"
                                        "        auto_start: true\n"
                                        "    states:\n"
                                        "      - name: IDLE\n"
                                        "        transitions:\n"
                                        "          - event: timer_expired\n"
                                        "            timer: ping\n"
                                        "            target: FIRED\n"
                                        "      - name: FIRED\n"
                                        "instances:\n"
                                        "  - id: lab.one\n"
                                        "    machine: lab\n"
                                        "    enabled: true\n";

static const char *const periodic_yaml = "schema_version: \"0.2.0\"\n"
                                         "state_machines:\n"
                                         "  - name: lab\n"
                                         "    initial: IDLE\n"
                                         "    variables:\n"
                                         "      - name: beats\n"
                                         "        type: integer\n"
                                         "        default: 0\n"
                                         "    timers:\n"
                                         "      - name: beat\n"
                                         "        duration_ms: 2\n"
                                         "        repeat: true\n"
                                         "        auto_start: true\n"
                                         "    states:\n"
                                         "      - name: IDLE\n"
                                         "        transitions:\n"
                                         "          - event: timer_expired\n"
                                         "            timer: beat\n"
                                         "            target: IDLE\n"
                                         "            actions:\n"
                                         "              - set_variable:\n"
                                         "                  variable: beats\n"
                                         "                  value: \"var.beats + 1\"\n"
                                         "instances:\n"
                                         "  - id: lab.one\n"
                                         "    machine: lab\n"
                                         "    enabled: true\n";

static const char *const missed_yaml = "schema_version: \"0.2.0\"\n"
                                       "state_machines:\n"
                                       "  - name: lab\n"
                                       "    initial: IDLE\n"
                                       "    variables:\n"
                                       "      - name: beats\n"
                                       "        type: integer\n"
                                       "        default: 0\n"
                                       "      - name: missed\n"
                                       "        type: integer\n"
                                       "        default: -1\n"
                                       "    timers:\n"
                                       "      - name: beat\n"
                                       "        duration_ms: 2\n"
                                       "        repeat: true\n"
                                       "        auto_start: true\n"
                                       "    states:\n"
                                       "      - name: IDLE\n"
                                       "        transitions:\n"
                                       "          - event: timer_expired\n"
                                       "            timer: beat\n"
                                       "            target: IDLE\n"
                                       "            actions:\n"
                                       "              - set_variable:\n"
                                       "                  variable: beats\n"
                                       "                  value: \"var.beats + 1\"\n"
                                       "              - set_variable:\n"
                                       "                  variable: missed\n"
                                       "                  value: \"evt.missed_count\"\n"
                                       "instances:\n"
                                       "  - id: lab.one\n"
                                       "    machine: lab\n"
                                       "    enabled: true\n";

static const char *const control_yaml = "schema_version: \"0.2.0\"\n"
                                        "state_machines:\n"
                                        "  - name: lab\n"
                                        "    initial: IDLE\n"
                                        "    timers:\n"
                                        "      - name: manual\n"
                                        "        duration_ms: 5\n"
                                        "        repeat: false\n"
                                        "        auto_start: false\n"
                                        "      - name: auto\n"
                                        "        duration_ms: 4\n"
                                        "        repeat: true\n"
                                        "        auto_start: true\n"
                                        "    states:\n"
                                        "      - name: IDLE\n"
                                        "        transitions:\n"
                                        "          - event: can_rx\n"
                                        "            message: StatusMsg\n"
                                        "            target: IDLE\n"
                                        "            actions:\n"
                                        "              - start_timer:\n"
                                        "                  timer: manual\n"
                                        "          - event: signal_changed\n"
                                        "            signal: CtlStop\n"
                                        "            target: IDLE\n"
                                        "            actions:\n"
                                        "              - stop_timer:\n"
                                        "                  timer: manual\n"
                                        "          - event: signal_changed\n"
                                        "            signal: CtlReset\n"
                                        "            target: IDLE\n"
                                        "            actions:\n"
                                        "              - reset_timer:\n"
                                        "                  timer: manual\n"
                                        "          - event: signal_changed\n"
                                        "            signal: CtlOverride\n"
                                        "            target: IDLE\n"
                                        "            actions:\n"
                                        "              - start_timer:\n"
                                        "                  timer: manual\n"
                                        "                  duration_ms: 2\n"
                                        "                  repeat: true\n"
                                        "instances:\n"
                                        "  - id: lab.one\n"
                                        "    machine: lab\n"
                                        "    enabled: true\n";

static const char *const ordering_yaml = "schema_version: \"0.2.0\"\n"
                                         "state_machines:\n"
                                         "  - name: lab\n"
                                         "    initial: IDLE\n"
                                         "    timers:\n"
                                         "      - name: first\n"
                                         "        duration_ms: 2\n"
                                         "        repeat: false\n"
                                         "        auto_start: true\n"
                                         "      - name: second\n"
                                         "        duration_ms: 2\n"
                                         "        repeat: false\n"
                                         "        auto_start: true\n"
                                         "    states:\n"
                                         "      - name: IDLE\n"
                                         "        transitions:\n"
                                         "          - event: timer_expired\n"
                                         "            timer: first\n"
                                         "            target: ONE\n"
                                         "          - event: timer_expired\n"
                                         "            timer: second\n"
                                         "            target: TWO\n"
                                         "      - name: ONE\n"
                                         "        transitions:\n"
                                         "          - event: timer_expired\n"
                                         "            timer: second\n"
                                         "            target: TWO\n"
                                         "      - name: TWO\n"
                                         "instances:\n"
                                         "  - id: lab.a\n"
                                         "    machine: lab\n"
                                         "    enabled: true\n"
                                         "  - id: lab.b\n"
                                         "    machine: lab\n"
                                         "    enabled: true\n";

/* FSM-TIMER-001: a one-shot timer expires exactly once, on its deadline. */
static void case_one_shot(void)
{
    static fsm_test_fixture_t fx;
    cancestry_fsm_timer_state_t timer;

    CANCESSTRY_TEST_CASE("FSM-TIMER-001 one-shot fires once on the tick it is due");
    CANCESSTRY_TEST_CHECK(fsm_test_init(&fx, oneshot_yaml));
    CANCESSTRY_TEST_CHECK_U64(fsm_test_start(&fx, "lab.one"), CANCESTRY_FSM_OK);
    CANCESSTRY_TEST_CHECK_U64(cancestry_fsm_instance_get_timer(&fx.engine,
                                                              fx.instances, "ping", &timer),
                              CANCESTRY_FSM_OK);
    /* armed at t=0 for 3 ms, so the deadline is 3000 us and the tick is 1 ms */
    CANCESSTRY_TEST_CHECK(timer.running);
    CANCESSTRY_TEST_CHECK_U64(timer.duration_ms, 3u);
    CANCESSTRY_TEST_CHECK(timer.repeat == false);
    CANCESSTRY_TEST_CHECK_U64(timer.id, 1u);

    CANCESSTRY_TEST_CHECK_U64(fsm_test_ticks(&fx, 2u), CANCESTRY_FSM_OK);
    CANCESSTRY_TEST_CHECK_STRING(fsm_test_state(&fx, "lab.one"), "IDLE");
    CANCESSTRY_TEST_CHECK_U64(fsm_test_counters(&fx, "lab.one")->timer_expiries, 0u);

    /* The 3rd tick reaches 3000 us: not earlier, not later. */
    CANCESSTRY_TEST_CHECK_U64(fsm_test_tick(&fx), CANCESTRY_FSM_OK);
    CANCESSTRY_TEST_CHECK_STRING(fsm_test_state(&fx, "lab.one"), "FIRED");
    CANCESSTRY_TEST_CHECK_U64(fsm_test_counters(&fx, "lab.one")->timer_expiries, 1u);
    CANCESSTRY_TEST_CHECK_U64(cancestry_fsm_instance_get_timer(&fx.engine, fx.instances, "ping",
                                                                &timer),
                              CANCESTRY_FSM_OK);
    CANCESSTRY_TEST_CHECK(timer.running == false);
    CANCESSTRY_TEST_CHECK_U64(timer.expires, 1u);
    CANCESSTRY_TEST_CHECK_U64(timer.missed_ticks, 0u);
    /* And it never fires again. */
    CANCESSTRY_TEST_CHECK_U64(fsm_test_ticks(&fx, 20u), CANCESTRY_FSM_OK);
    CANCESSTRY_TEST_CHECK_U64(fsm_test_counters(&fx, "lab.one")->timer_expiries, 1u);
    fsm_test_destroy(&fx);
}

/* FSM-TIMER-002: a periodic timer keeps its schedule and never drifts. */
static void case_periodic(void)
{
    static fsm_test_fixture_t fx;
    cancestry_fsm_timer_state_t timer;
    int64_t beats = -1;

    CANCESSTRY_TEST_CASE("FSM-TIMER-002 periodic expiries follow the deadline");
    CANCESSTRY_TEST_CHECK(fsm_test_init(&fx, periodic_yaml));
    CANCESSTRY_TEST_CHECK_U64(fsm_test_start(&fx, "lab.one"), CANCESTRY_FSM_OK);
    CANCESSTRY_TEST_CHECK_U64(fsm_test_ticks(&fx, 9u), CANCESTRY_FSM_OK);
    /* Due at 2, 4, 6 and 8 ms: four expiries in nine 1 ms ticks. */
    CANCESSTRY_TEST_CHECK_U64(fsm_test_counters(&fx, "lab.one")->timer_expiries, 4u);
    CANCESSTRY_TEST_CHECK_I64(fsm_test_variable_int(&fx, "lab.one", "beats", &beats),
                              CANCESTRY_FSM_OK);
    CANCESSTRY_TEST_CHECK_I64(beats, 4);
    CANCESSTRY_TEST_CHECK_U64(
        cancestry_fsm_instance_get_timer(&fx.engine, fx.instances, "beat", &timer),
        CANCESTRY_FSM_OK);
    CANCESSTRY_TEST_CHECK(timer.running);
    CANCESSTRY_TEST_CHECK(timer.repeat);
    CANCESSTRY_TEST_CHECK_U64(timer.expires, 4u);
    /* Deadline is the next scheduled period (10 ms), not "last expiry + period"
     * and not "now + period": no drift accumulates. */
    CANCESSTRY_TEST_CHECK_U64(timer.deadline_us, 10000u);
    CANCESSTRY_TEST_CHECK_U64(fsm_test_counters(&fx, "lab.one")->timer_missed_ticks, 0u);
    fsm_test_destroy(&fx);
}

/* FSM-TIMER-003: skipped periods collapse into one event with missed_count. */
static void case_missed_ticks(void)
{
    static fsm_test_fixture_t fx;
    cancestry_fsm_timer_state_t timer;
    int64_t beats = -1;
    int64_t missed = -1;

    CANCESSTRY_TEST_CASE("FSM-TIMER-003 missed ticks are reported, not replayed");
    CANCESSTRY_TEST_CHECK(fsm_test_init_clocked(&fx, missed_yaml, 0u));
    CANCESSTRY_TEST_CHECK_U64(fsm_test_start(&fx, "lab.one"), CANCESTRY_FSM_OK);

    /* The clock jumps 5 ms and the runtime ticks once: periods 2 ms and 4 ms were
     * due, so exactly one event carries missed_count = 1. */
    fsm_test_advance_clock(&fx, 5000u);
    CANCESSTRY_TEST_CHECK_U64(fsm_test_tick(&fx), CANCESTRY_FSM_OK);
    CANCESSTRY_TEST_CHECK_U64(fsm_test_counters(&fx, "lab.one")->timer_expiries, 1u);
    CANCESSTRY_TEST_CHECK_U64(fsm_test_counters(&fx, "lab.one")->timer_missed_ticks, 1u);
    CANCESSTRY_TEST_CHECK_I64(fsm_test_variable_int(&fx, "lab.one", "beats", &beats),
                              CANCESTRY_FSM_OK);
    CANCESSTRY_TEST_CHECK_I64(beats, 1);
    CANCESSTRY_TEST_CHECK_I64(fsm_test_variable_int(&fx, "lab.one", "missed", &missed),
                              CANCESTRY_FSM_OK);
    CANCESSTRY_TEST_CHECK_I64(missed, 1);
    CANCESSTRY_TEST_CHECK_U64(
        cancestry_fsm_instance_get_timer(&fx.engine, fx.instances, "beat", &timer),
        CANCESTRY_FSM_OK);
    CANCESSTRY_TEST_CHECK_U64(timer.deadline_us, 6000u);
    CANCESSTRY_TEST_CHECK_U64(timer.missed_ticks, 1u);

    /* The next period is still on the original schedule: 6 ms. */
    fsm_test_advance_clock(&fx, 1000u);
    CANCESSTRY_TEST_CHECK_U64(fsm_test_tick(&fx), CANCESTRY_FSM_OK);
    CANCESSTRY_TEST_CHECK_U64(fsm_test_counters(&fx, "lab.one")->timer_expiries, 2u);
    CANCESSTRY_TEST_CHECK_U64(
        cancestry_fsm_instance_get_timer(&fx.engine, fx.instances, "beat", &timer),
        CANCESTRY_FSM_OK);
    CANCESSTRY_TEST_CHECK_U64(timer.deadline_us, 8000u);
    /* ...and that event reported no missed period. */
    CANCESSTRY_TEST_CHECK_I64(fsm_test_variable_int(&fx, "lab.one", "missed", &missed),
                              CANCESTRY_FSM_OK);
    CANCESSTRY_TEST_CHECK_I64(missed, 0);
    fsm_test_destroy(&fx);
}

/* FSM-TIMER-004: start, stop, reset and auto_start. */
static void case_timer_control(void)
{
    static fsm_test_fixture_t fx;
    cancestry_fsm_timer_state_t timer;
    cancestry_event_t event;

    CANCESSTRY_TEST_CASE("FSM-TIMER-004 start, stop and reset semantics");
    CANCESSTRY_TEST_CHECK(fsm_test_init(&fx, control_yaml));
    CANCESSTRY_TEST_CHECK_U64(fsm_test_start(&fx, "lab.one"), CANCESTRY_FSM_OK);

    /* auto_start: the 4 ms periodic timer is running without any action; the
     * manual one is not. */
    CANCESSTRY_TEST_CHECK_U64(
        cancestry_fsm_instance_get_timer(&fx.engine, fx.instances, "auto", &timer),
        CANCESTRY_FSM_OK);
    CANCESSTRY_TEST_CHECK(timer.running);
    CANCESSTRY_TEST_CHECK_U64(timer.deadline_us, 4000u);
    CANCESSTRY_TEST_CHECK_U64(
        cancestry_fsm_instance_get_timer(&fx.engine, fx.instances, "manual", &timer),
        CANCESTRY_FSM_OK);
    CANCESSTRY_TEST_CHECK(!timer.running);
    CANCESSTRY_TEST_CHECK_U64(timer.deadline_us, 0u);

    /* start_timer arms it for its declared 5 ms from the current time base. */
    fsm_test_event_can_rx(&event, 0u, FSM_TEST_IFACE_CAN0, FSM_TEST_MSG_STATUS, NULL, 0u);
    CANCESSTRY_TEST_CHECK_U64(fsm_test_process(&fx, &event), CANCESTRY_FSM_OK);
    CANCESSTRY_TEST_CHECK(fsm_test_has_line(&fx, "timer 1 start_timer manual duration=5 repeat=0"));
    CANCESSTRY_TEST_CHECK_U64(
        cancestry_fsm_instance_get_timer(&fx.engine, fx.instances, "manual", &timer),
        CANCESTRY_FSM_OK);
    CANCESSTRY_TEST_CHECK(timer.running);
    CANCESSTRY_TEST_CHECK_U64(timer.deadline_us, 5000u);

    /* stop_timer disarms it, and nothing fires while it is stopped. */
    fx.line_count = 0u;
    cancestry_event_init(&event);
    fsm_test_event_int(&event, 1000u, "CtlStop", 1);
    CANCESSTRY_TEST_CHECK_U64(fsm_test_process(&fx, &event), CANCESTRY_FSM_OK);
    CANCESSTRY_TEST_CHECK(fsm_test_has_line(&fx, "timer 1 stop_timer manual duration=5 repeat=0"));
    CANCESSTRY_TEST_CHECK_U64(fsm_test_ticks(&fx, 20u), CANCESTRY_FSM_OK);
    CANCESSTRY_TEST_CHECK_U64(
        cancestry_fsm_instance_get_timer(&fx.engine, fx.instances, "manual", &timer),
        CANCESTRY_FSM_OK);
    CANCESSTRY_TEST_CHECK(!timer.running);
    CANCESSTRY_TEST_CHECK_U64(timer.expires, 0u);

    /* reset_timer on a stopped timer re-arms the deadline but does not start it:
     * a reset is not a start (fsm-spec.md section 9 lists them apart). */
    fx.line_count = 0u;
    cancestry_event_init(&event);
    fsm_test_event_int(&event, 0u, "CtlReset", 1);
    CANCESSTRY_TEST_CHECK_U64(fsm_test_process(&fx, &event), CANCESTRY_FSM_OK);
    CANCESSTRY_TEST_CHECK_U64(
        cancestry_fsm_instance_get_timer(&fx.engine, fx.instances, "manual", &timer),
        CANCESTRY_FSM_OK);
    CANCESSTRY_TEST_CHECK(!timer.running);
    /* The clock has moved 20 ms + the 4 ms periodic timer's ticks: the reset
     * re-armed relative to the engine's time base, so the deadline moved. */
    CANCESSTRY_TEST_CHECK(timer.deadline_us > 5000u);
    CANCESSTRY_TEST_CHECK_U64(fsm_test_ticks(&fx, 30u), CANCESTRY_FSM_OK);
    CANCESSTRY_TEST_CHECK_U64(timer.expires, 0u);

    /* start_timer may override duration and repeat (schema-allowed fields). */
    fx.line_count = 0u;
    cancestry_event_init(&event);
    fsm_test_event_int(&event, 0u, "CtlOverride", 1);
    CANCESSTRY_TEST_CHECK_U64(fsm_test_process(&fx, &event), CANCESTRY_FSM_OK);
    CANCESSTRY_TEST_CHECK(fsm_test_has_line(&fx, "timer 1 start_timer manual duration=2 repeat=1"));
    CANCESSTRY_TEST_CHECK_U64(
        cancestry_fsm_instance_get_timer(&fx.engine, fx.instances, "manual", &timer),
        CANCESTRY_FSM_OK);
    CANCESSTRY_TEST_CHECK(timer.running);
    CANCESSTRY_TEST_CHECK(timer.repeat);
    CANCESSTRY_TEST_CHECK_U64(timer.duration_ms, 2u);
    fsm_test_destroy(&fx);
}

/* FSM-TIMER-005: expiry order is deadline, then instance, then declaration. */
static void case_expiry_order(void)
{
    static fsm_test_fixture_t fx;

    CANCESSTRY_TEST_CASE("FSM-TIMER-005 expiry order is deadline, instance, declaration");
    CANCESSTRY_TEST_CHECK(fsm_test_init(&fx, ordering_yaml));
    CANCESSTRY_TEST_CHECK_U64(fsm_test_start_all(&fx), 2u);
    fsm_test_reset(&fx);
    CANCESSTRY_TEST_CHECK_U64(fsm_test_ticks(&fx, 2u), CANCESTRY_FSM_OK);
    /* Both timers of both instances are due at 2 ms. Instance 1 is served first,
     * and inside it the timers run in declaration order (first, then second);
     * instance 1's whole chain completes before instance 2 starts. */
    CANCESSTRY_TEST_CHECK_U64(fsm_test_line_count(&fx), 4u);
    CANCESSTRY_TEST_CHECK_STRING(fsm_test_line_at(&fx, 0u), "state 1 IDLE->ONE");
    CANCESSTRY_TEST_CHECK_STRING(fsm_test_line_at(&fx, 1u), "state 1 ONE->TWO");
    CANCESSTRY_TEST_CHECK_STRING(fsm_test_line_at(&fx, 2u), "state 2 IDLE->ONE");
    CANCESSTRY_TEST_CHECK_STRING(fsm_test_line_at(&fx, 3u), "state 2 ONE->TWO");
    CANCESSTRY_TEST_CHECK_STRING(fsm_test_state(&fx, "lab.a"), "TWO");
    CANCESSTRY_TEST_CHECK_STRING(fsm_test_state(&fx, "lab.b"), "TWO");
    CANCESSTRY_TEST_CHECK_U64(fsm_test_counters(&fx, "lab.a")->timer_expiries, 2u);
    CANCESSTRY_TEST_CHECK_U64(fsm_test_counters(&fx, "lab.b")->timer_expiries, 2u);
    fsm_test_destroy(&fx);
}

/* FSM-TIMER-006: a suspended instance is not evaluated, and reports what it
 * missed when it resumes. */
static void case_suspended_timers(void)
{
    static fsm_test_fixture_t fx;
    cancestry_fsm_timer_state_t timer;
    int64_t beats = -1;

    CANCESSTRY_TEST_CASE("FSM-TIMER-006 suspension pauses evaluation only");
    CANCESSTRY_TEST_CHECK(fsm_test_init_clocked(&fx, periodic_yaml, 0u));
    CANCESSTRY_TEST_CHECK_U64(fsm_test_start(&fx, "lab.one"), CANCESTRY_FSM_OK);
    CANCESSTRY_TEST_CHECK_U64(
        cancestry_fsm_instance_get_timer(&fx.engine, fx.instances, "beat", &timer),
        CANCESTRY_FSM_OK);
    CANCESSTRY_TEST_CHECK_U64(timer.deadline_us, 2000u);

    CANCESSTRY_TEST_CHECK_U64(
        cancestry_fsm_instance_suspend(&fx.engine, fx.instances), CANCESTRY_FSM_OK);
    /* Time passes while suspended: the timer is still armed, nothing is evaluated. */
    fsm_test_advance_clock(&fx, 5000u);
    CANCESSTRY_TEST_CHECK_U64(fsm_test_tick(&fx), CANCESTRY_FSM_OK);
    CANCESSTRY_TEST_CHECK_U64(fsm_test_counters(&fx, "lab.one")->timer_expiries, 0u);
    CANCESSTRY_TEST_CHECK_U64(
        cancestry_fsm_instance_get_timer(&fx.engine, fx.instances, "beat", &timer),
        CANCESTRY_FSM_OK);
    CANCESSTRY_TEST_CHECK_U64(timer.deadline_us, 2000u);
    CANCESSTRY_TEST_CHECK(timer.running);

    /* On resume the skipped periods surface as ONE event whose missed_count
     * counts the periods that were skipped beyond the one being reported: 2 ms
     * and 4 ms were due at t = 5 ms, so one tick was missed and the next
     * deadline is 6 ms. */
    CANCESSTRY_TEST_CHECK_U64(cancestry_fsm_instance_resume(&fx.engine, fx.instances),
                              CANCESTRY_FSM_OK);
    CANCESSTRY_TEST_CHECK_U64(fsm_test_tick(&fx), CANCESTRY_FSM_OK);
    CANCESSTRY_TEST_CHECK_U64(fsm_test_counters(&fx, "lab.one")->timer_expiries, 1u);
    CANCESSTRY_TEST_CHECK_I64(fsm_test_variable_int(&fx, "lab.one", "beats", &beats),
                              CANCESTRY_FSM_OK);
    CANCESSTRY_TEST_CHECK_I64(beats, 1);
    CANCESSTRY_TEST_CHECK_U64(fsm_test_counters(&fx, "lab.one")->timer_missed_ticks, 1u);
    CANCESSTRY_TEST_CHECK_U64(
        cancestry_fsm_instance_get_timer(&fx.engine, fx.instances, "beat", &timer),
        CANCESTRY_FSM_OK);
    CANCESSTRY_TEST_CHECK_U64(timer.deadline_us, 6000u);
    fsm_test_destroy(&fx);
}

int main(void)
{
    CANCESSTRY_TEST_SUITE_BEGIN("fsm/timer_semantics");
    case_one_shot();
    case_periodic();
    case_missed_ticks();
    case_timer_control();
    case_expiry_order();
    case_suspended_timers();
    return CANCESSTRY_TEST_SUITE_END();
}
