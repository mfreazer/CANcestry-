/*
 * CANcestry FSM conformance: transition precedence.
 *
 * Contract under test (docs/packages/fsm-spec.md section 8 rules 1-6,
 * docs/software/SwAD.md section 7, docs/software/SwRS.md SW-FR-FSM-010..016,
 * SW-FR-FSM-021, SW-FR-FSM-045, SW-FR-FSM-054, SW-FR-FSM-055,
 * docs/system/mode-fault-state-machine.md section 5 "FSM transition chain
 * exceeded"):
 *
 *   - at most one event-driven transition is selected per event, and selection is
 *     the first declaration that matches (SW-FR-FSM-012);
 *   - that transition completes - exit actions, transition actions, entry actions
 *     - before anything else runs (SW-FR-FSM-014);
 *   - a transition action schedules a *deferred* transition, which runs after the
 *     current transition completed and before the next event is taken
 *     (SW-FR-FSM-054);
 *   - the chain counts toward the depth limit of 4, and exceeding it suspends the
 *     instance instead of continuing (SW-FR-FSM-016, SW-FR-FSM-021);
 *   - several transition actions in one sequence: the first wins, the rest are
 *     ignored with a warning (SW-FR-FSM-055);
 *   - a self-transition runs exit and entry actions (SW-FR-FSM-015).
 *
 * Test ids: FSM-TRANSITION-PRECEDENCE-001 .. -007.
 */

#include "cancestry_fsm_conformance.h"

static const char *const selection_yaml = "schema_version: \"0.2.0\"\n"
                                         "state_machines:\n"
                                         "  - name: lab\n"
                                         "    initial: S\n"
                                         "    states:\n"
                                         "      - name: S\n"
                                         "        transitions:\n"
                                         "          - event: can_rx\n"
                                         "            target: FIRST\n"
                                         "          - event: can_rx\n"
                                         "            target: SECOND\n"
                                         "      - name: FIRST\n"
                                         "      - name: SECOND\n"
                                         "instances:\n"
                                         "  - id: lab.one\n"
                                         "    machine: lab\n"
                                         "    enabled: true\n";

static const char *const order_yaml = "schema_version: \"0.2.0\"\n"
                                     "state_machines:\n"
                                     "  - name: lab\n"
                                     "    initial: X\n"
                                     "    states:\n"
                                     "      - name: X\n"
                                     "        exit:\n"
                                     "          - log:\n"
                                     "              level: info\n"
                                     "              message: exit X\n"
                                     "        transitions:\n"
                                     "          - event: can_rx\n"
                                     "            target: Y\n"
                                     "            actions:\n"
                                     "              - log:\n"
                                     "                  level: info\n"
                                     "                  message: transition action\n"
                                     "      - name: Y\n"
                                     "        entry:\n"
                                     "          - log:\n"
                                     "              level: info\n"
                                     "              message: entry Y\n"
                                     "        exit:\n"
                                     "          - log:\n"
                                     "              level: info\n"
                                     "              message: exit Y\n"
                                     "        transitions:\n"
                                     "          - event: can_rx\n"
                                     "            target: X\n"
                                     "      - name: Z\n"
                                     "instances:\n"
                                     "  - id: lab.one\n"
                                     "    machine: lab\n"
                                     "    enabled: true\n";

static const char *const deferred_yaml = "schema_version: \"0.2.0\"\n"
                                         "state_machines:\n"
                                         "  - name: lab\n"
                                         "    initial: D1\n"
                                         "    states:\n"
                                         "      - name: D1\n"
                                         "        transitions:\n"
                                         "          - event: can_rx\n"
                                         "            target: D2\n"
                                         "      - name: D2\n"
                                         "        entry:\n"
                                         "          - log:\n"
                                         "              level: info\n"
                                         "              message: entry D2\n"
                                         "          - transition:\n"
                                         "              target: D3\n"
                                         "        exit:\n"
                                         "          - log:\n"
                                         "              level: info\n"
                                         "              message: exit D2\n"
                                         "        transitions:\n"
                                         "          - event: state_entered\n"
                                         "            target: NEVER\n"
                                         "      - name: D3\n"
                                         "        entry:\n"
                                         "          - log:\n"
                                         "              level: info\n"
                                         "              message: entry D3\n"
                                         "      - name: NEVER\n"
                                         "instances:\n"
                                         "  - id: lab.one\n"
                                         "    machine: lab\n"
                                         "    enabled: true\n";

/* A -> B, and every entry pushes the chain one further. */
static const char *const chain_yaml = "schema_version: \"0.2.0\"\n"
                                      "state_machines:\n"
                                      "  - name: lab\n"
                                      "    initial: A\n"
                                      "    states:\n"
                                      "      - name: A\n"
                                      "        transitions:\n"
                                      "          - event: can_rx\n"
                                      "            target: B\n"
                                      "      - name: B\n"
                                      "        entry:\n"
                                      "          - transition:\n"
                                      "              target: C\n"
                                      "      - name: C\n"
                                      "        entry:\n"
                                      "          - transition:\n"
                                      "              target: D\n"
                                      "      - name: D\n"
                                      "        entry:\n"
                                      "          - transition:\n"
                                      "              target: E\n"
                                      "      - name: E\n"
                                      "        entry:\n"
                                      "          - transition:\n"
                                      "              target: F\n"
                                      "      - name: F\n"
                                      "instances:\n"
                                      "  - id: lab.one\n"
                                      "    machine: lab\n"
                                      "    enabled: true\n";

/* A chain that stays inside the limit. */
static const char *const short_chain_yaml = "schema_version: \"0.2.0\"\n"
                                            "state_machines:\n"
                                            "  - name: lab\n"
                                            "    initial: A\n"
                                            "    states:\n"
                                            "      - name: A\n"
                                            "        transitions:\n"
                                            "          - event: can_rx\n"
                                            "            target: B\n"
                                            "      - name: B\n"
                                            "        entry:\n"
                                            "          - transition:\n"
                                            "              target: C\n"
                                            "      - name: C\n"
                                            "        entry:\n"
                                            "          - transition:\n"
                                            "              target: D\n"
                                            "      - name: D\n"
                                            "instances:\n"
                                            "  - id: lab.one\n"
                                            "    machine: lab\n"
                                            "    enabled: true\n";

static const char *const first_wins_yaml = "schema_version: \"0.2.0\"\n"
                                          "state_machines:\n"
                                          "  - name: lab\n"
                                          "    initial: A\n"
                                          "    states:\n"
                                          "      - name: A\n"
                                          "        transitions:\n"
                                          "          - event: can_rx\n"
                                          "            target: B\n"
                                          "            actions:\n"
                                          "              - transition:\n"
                                          "                  target: P\n"
                                          "              - log:\n"
                                          "                  level: warning\n"
                                          "                  message: between transitions\n"
                                          "              - transition:\n"
                                          "                  target: Q\n"
                                          "      - name: B\n"
                                          "      - name: P\n"
                                          "      - name: Q\n"
                                          "instances:\n"
                                          "  - id: lab.one\n"
                                          "    machine: lab\n"
                                          "    enabled: true\n";

static const char *const self_yaml = "schema_version: \"0.2.0\"\n"
                                     "state_machines:\n"
                                     "  - name: lab\n"
                                     "    initial: S\n"
                                     "    variables:\n"
                                     "      - name: passes\n"
                                     "        type: integer\n"
                                     "        default: 0\n"
                                     "    states:\n"
                                     "      - name: S\n"
                                     "        entry:\n"
                                     "          - log:\n"
                                     "              level: info\n"
                                     "              message: enter S\n"
                                     "        exit:\n"
                                     "          - set_variable:\n"
                                     "              variable: passes\n"
                                     "              value: \"var.passes + 1\"\n"
                                     "        transitions:\n"
                                     "          - event: can_rx\n"
                                     "            target: S\n"
                                     "instances:\n"
                                     "  - id: lab.one\n"
                                     "    machine: lab\n"
                                     "    enabled: true\n";

static const char *const budget_yaml = "schema_version: \"0.2.0\"\n"
                                       "state_machines:\n"
                                       "  - name: lab\n"
                                       "    initial: A\n"
                                       "    states:\n"
                                       "      - name: A\n"
                                       "        transitions:\n"
                                       "          - event: can_rx\n"
                                       "            target: B\n"
                                       "            actions:\n"
                                       "              - log:\n"
                                       "                  level: info\n"
                                       "                  message: one\n"
                                       "              - log:\n"
                                       "                  level: info\n"
                                       "                  message: two\n"
                                       "              - log:\n"
                                       "                  level: info\n"
                                       "                  message: three\n"
                                       "              - log:\n"
                                       "                  level: info\n"
                                       "                  message: four\n"
                                       "              - log:\n"
                                       "                  level: info\n"
                                       "                  message: five\n"
                                       "              - log:\n"
                                       "                  level: info\n"
                                       "                  message: six\n"
                                       "      - name: B\n"
                                       "instances:\n"
                                       "  - id: lab.one\n"
                                       "    machine: lab\n"
                                       "    enabled: true\n";

static void deliver(cancestry_event_t *event)
{
    fsm_test_event_can_rx(event, 1000u, FSM_TEST_IFACE_CAN0, FSM_TEST_MSG_STATUS, NULL, 0u);
}

/* FSM-TRANSITION-PRECEDENCE-001: at most one transition is selected. */
static void case_single_selection(void)
{
    static fsm_test_fixture_t fx;
    cancestry_event_t event;

    CANCESSTRY_TEST_CASE("FSM-PRECEDENCE-001 the first matching declaration wins");
    CANCESSTRY_TEST_CHECK(fsm_test_init(&fx, selection_yaml));
    CANCESSTRY_TEST_CHECK_U64(fsm_test_start(&fx, "lab.one"), CANCESTRY_FSM_OK);
    fsm_test_reset(&fx);
    deliver(&event);
    CANCESSTRY_TEST_CHECK_U64(fsm_test_process(&fx, &event), CANCESTRY_FSM_OK);
    CANCESSTRY_TEST_CHECK_STRING(fsm_test_state(&fx, "lab.one"), "FIRST");
    CANCESSTRY_TEST_CHECK(!fsm_test_has_line(&fx, "state 1 S->SECOND"));
    /* One transition executed, and selection stopped there: the second
     * declaration was not even considered (no guard work for it). */
    CANCESSTRY_TEST_CHECK_U64(fsm_test_counters(&fx, "lab.one")->transitions, 2u);
    CANCESSTRY_TEST_CHECK_U64(fsm_test_line_count(&fx), 1u);
    fsm_test_destroy(&fx);
}

/* FSM-TRANSITION-PRECEDENCE-002: exit, transition, then entry actions. */
static void case_action_order(void)
{
    static fsm_test_fixture_t fx;
    cancestry_event_t event;

    CANCESSTRY_TEST_CASE("FSM-PRECEDENCE-002 exit, action, entry order");
    CANCESSTRY_TEST_CHECK(fsm_test_init(&fx, order_yaml));
    CANCESSTRY_TEST_CHECK_U64(fsm_test_start(&fx, "lab.one"), CANCESTRY_FSM_OK);
    /* start() ran X's entry actions (none) and nothing else. */
    fsm_test_reset(&fx);
    deliver(&event);
    CANCESSTRY_TEST_CHECK_U64(fsm_test_process(&fx, &event), CANCESTRY_FSM_OK);
    CANCESSTRY_TEST_CHECK(fsm_test_has_line(&fx, "log 1 info exit X"));
    CANCESSTRY_TEST_CHECK(fsm_test_has_line(&fx, "log 1 info transition action"));
    CANCESSTRY_TEST_CHECK(fsm_test_has_line(&fx, "log 1 info entry Y"));
    CANCESSTRY_TEST_CHECK_STRING(fsm_test_line_at(&fx, 0u), "log 1 info exit X");
    CANCESSTRY_TEST_CHECK_STRING(fsm_test_line_at(&fx, 1u), "log 1 info transition action");
    CANCESSTRY_TEST_CHECK_STRING(fsm_test_line_at(&fx, 2u), "state 1 X->Y");
    CANCESSTRY_TEST_CHECK_STRING(fsm_test_line_at(&fx, 3u), "log 1 info entry Y");
    CANCESSTRY_TEST_CHECK_U64(fsm_test_line_count(&fx), 4u);
    /* The next event runs Y's exit actions before X's entry again. */
    fsm_test_reset(&fx);
    cancestry_event_init(&event);
    deliver(&event);
    CANCESSTRY_TEST_CHECK_U64(fsm_test_process(&fx, &event), CANCESTRY_FSM_OK);
    CANCESSTRY_TEST_CHECK_STRING(fsm_test_line_at(&fx, 0u), "log 1 info exit Y");
    CANCESSTRY_TEST_CHECK_STRING(fsm_test_state(&fx, "lab.one"), "X");
    fsm_test_destroy(&fx);
}

/* FSM-TRANSITION-PRECEDENCE-003: a deferred transition does not interrupt the
 * current one, and runs before the next event. */
static void case_deferred(void)
{
    static fsm_test_fixture_t fx;
    cancestry_event_t event;

    CANCESSTRY_TEST_CASE("FSM-PRECEDENCE-003 deferred runs after, not during");
    CANCESSTRY_TEST_CHECK(fsm_test_init(&fx, deferred_yaml));
    CANCESSTRY_TEST_CHECK_U64(fsm_test_start(&fx, "lab.one"), CANCESTRY_FSM_OK);
    fsm_test_reset(&fx);
    deliver(&event);
    CANCESSTRY_TEST_CHECK_U64(fsm_test_process(&fx, &event), CANCESTRY_FSM_OK);
    /*
     * D1->D2 completed (its entry action ran) before D2->D3 started, and the
     * deferred step ran before D2's queued state_entered event could select the
     * NEVER transition - which therefore never fires.
     */
    CANCESSTRY_TEST_CHECK_STRING(fsm_test_line_at(&fx, 0u), "state 1 D1->D2");
    CANCESSTRY_TEST_CHECK_STRING(fsm_test_line_at(&fx, 1u), "log 1 info entry D2");
    CANCESSTRY_TEST_CHECK_STRING(fsm_test_line_at(&fx, 2u), "log 1 info exit D2");
    CANCESSTRY_TEST_CHECK_STRING(fsm_test_line_at(&fx, 3u), "state 1 D2->D3");
    CANCESSTRY_TEST_CHECK_STRING(fsm_test_line_at(&fx, 4u), "log 1 info entry D3");
    CANCESSTRY_TEST_CHECK_U64(fsm_test_line_count(&fx), 5u);
    CANCESSTRY_TEST_CHECK_STRING(fsm_test_state(&fx, "lab.one"), "D3");
    CANCESSTRY_TEST_CHECK_U64(fsm_test_counters(&fx, "lab.one")->deferred_transitions, 1u);
    CANCESSTRY_TEST_CHECK_U64(fsm_test_counters(&fx, "lab.one")->transitions, 3u);
    fsm_test_destroy(&fx);
}

/* FSM-TRANSITION-PRECEDENCE-004: the chain limit is 4, and exceeding it
 * suspends the instance (mode-fault-state-machine.md section 5). */
static void case_chain_limit(void)
{
    static fsm_test_fixture_t fx;
    cancestry_event_t event;

    CANCESSTRY_TEST_CASE("FSM-PRECEDENCE-004 chain depth limit of four");
    CANCESSTRY_TEST_CHECK(fsm_test_init(&fx, chain_yaml));
    CANCESSTRY_TEST_CHECK_U64(fsm_test_start(&fx, "lab.one"), CANCESTRY_FSM_OK);
    fsm_test_reset(&fx);
    deliver(&event);
    CANCESSTRY_TEST_CHECK_U64(fsm_test_process(&fx, &event), CANCESTRY_FSM_OK);
    /* A->B (1), B->C (2), C->D (3), D->E (4); E->F would be the fifth. */
    CANCESSTRY_TEST_CHECK_STRING(fsm_test_state(&fx, "lab.one"), "E");
    CANCESSTRY_TEST_CHECK(!fsm_test_has_line(&fx, "state 1 E->F"));
    CANCESSTRY_TEST_CHECK_U64(fsm_test_counters(&fx, "lab.one")->transitions, 5u);
    CANCESSTRY_TEST_CHECK_U64(fsm_test_counters(&fx, "lab.one")->chain_limit_exceeded, 1u);
    CANCESSTRY_TEST_CHECK(fsm_test_has_line(&fx, "warn 1 transition chain limit exceeded"));
    CANCESSTRY_TEST_CHECK_U64(fsm_test_lifecycle(&fx, "lab.one"),
                              CANCESTRY_FSM_INSTANCE_LIFECYCLE_SUSPENDED);
    /* A refused chain leaves no pending request behind for the next event. */
    CANCESSTRY_TEST_CHECK(fx.instances[0].deferred_pending == false);
    fsm_test_destroy(&fx);

    /* The same shape one step shorter completes and stays running. */
    CANCESSTRY_TEST_CHECK(fsm_test_init(&fx, short_chain_yaml));
    CANCESSTRY_TEST_CHECK_U64(fsm_test_start(&fx, "lab.one"), CANCESTRY_FSM_OK);
    cancestry_event_init(&event);
    deliver(&event);
    CANCESSTRY_TEST_CHECK_U64(fsm_test_process(&fx, &event), CANCESTRY_FSM_OK);
    CANCESSTRY_TEST_CHECK_STRING(fsm_test_state(&fx, "lab.one"), "D");
    CANCESSTRY_TEST_CHECK_U64(fsm_test_counters(&fx, "lab.one")->chain_limit_exceeded, 0u);
    CANCESSTRY_TEST_CHECK_U64(fsm_test_lifecycle(&fx, "lab.one"),
                              CANCESTRY_FSM_INSTANCE_LIFECYCLE_RUNNING);
    CANCESSTRY_TEST_CHECK_U64(fsm_test_counters(&fx, "lab.one")->deferred_transitions, 2u);
    fsm_test_destroy(&fx);
}

/* FSM-TRANSITION-PRECEDENCE-005: the first transition action wins. */
static void case_first_wins(void)
{
    static fsm_test_fixture_t fx;
    cancestry_event_t event;

    CANCESSTRY_TEST_CASE("FSM-PRECEDENCE-005 first transition action wins");
    CANCESSTRY_TEST_CHECK(fsm_test_init(&fx, first_wins_yaml));
    CANCESSTRY_TEST_CHECK_U64(fsm_test_start(&fx, "lab.one"), CANCESTRY_FSM_OK);
    fsm_test_reset(&fx);
    deliver(&event);
    CANCESSTRY_TEST_CHECK_U64(fsm_test_process(&fx, &event), CANCESTRY_FSM_OK);
    CANCESSTRY_TEST_CHECK_STRING(fsm_test_state(&fx, "lab.one"), "P");
    CANCESSTRY_TEST_CHECK(!fsm_test_has_line(&fx, "state 1 B->Q"));
    CANCESSTRY_TEST_CHECK_U64(fsm_test_counters(&fx, "lab.one")->deferred_ignored, 1u);
    CANCESSTRY_TEST_CHECK(
        fsm_test_has_line(&fx, "warn 1 transition action ignored: one is already pending"));
    /* Ignoring the late request is not an action failure: the sequence kept
     * running, so the log between the two requests is present. */
    CANCESSTRY_TEST_CHECK(fsm_test_has_line(&fx, "log 1 warning between transitions"));
    CANCESSTRY_TEST_CHECK_U64(fsm_test_counters(&fx, "lab.one")->action_errors, 0u);
    fsm_test_destroy(&fx);
}

/* FSM-TRANSITION-PRECEDENCE-006: a self-transition runs both action sets. */
static void case_self_transition(void)
{
    static fsm_test_fixture_t fx;
    cancestry_event_t event;
    int64_t passes = -1;

    CANCESSTRY_TEST_CASE("FSM-PRECEDENCE-006 self-transition re-enters");
    CANCESSTRY_TEST_CHECK(fsm_test_init(&fx, self_yaml));
    CANCESSTRY_TEST_CHECK_U64(fsm_test_start(&fx, "lab.one"), CANCESTRY_FSM_OK);
    CANCESSTRY_TEST_CHECK_STRING(fsm_test_state(&fx, "lab.one"), "S");
    CANCESSTRY_TEST_CHECK(fsm_test_has_line(&fx, "log 1 info enter S"));
    fsm_test_reset(&fx);
    deliver(&event);
    CANCESSTRY_TEST_CHECK_U64(fsm_test_process(&fx, &event), CANCESTRY_FSM_OK);
    /* Still S, but the exit actions ran, so the counter moved and the entry action
     * ran again (SW-FR-FSM-015). */
    CANCESSTRY_TEST_CHECK_STRING(fsm_test_state(&fx, "lab.one"), "S");
    CANCESSTRY_TEST_CHECK_I64(fsm_test_variable_int(&fx, "lab.one", "passes", &passes),
                              CANCESTRY_FSM_OK);
    CANCESSTRY_TEST_CHECK_I64(passes, 1);
    CANCESSTRY_TEST_CHECK(fsm_test_has_line(&fx, "state 1 S->S"));
    CANCESSTRY_TEST_CHECK(fsm_test_has_line(&fx, "log 1 info enter S"));
    fsm_test_destroy(&fx);
}

/* FSM-TRANSITION-PRECEDENCE-007: the per-event action budget is enforced and
 * recorded, and it stops the run without corrupting state. */
static void case_action_budget(void)
{
    static fsm_test_fixture_t fx;
    cancestry_event_t event;
    fsm_test_options_t options = fsm_test_options_default();

    CANCESSTRY_TEST_CASE("FSM-PRECEDENCE-007 execution budget per event");
    options.max_actions_per_event = 3u;
    CANCESSTRY_TEST_CHECK(fsm_test_init_with(&fx, budget_yaml, &options));
    CANCESSTRY_TEST_CHECK_U64(fsm_test_start(&fx, "lab.one"), CANCESTRY_FSM_OK);
    fsm_test_reset(&fx);
    deliver(&event);
    CANCESSTRY_TEST_CHECK_U64(fsm_test_process(&fx, &event), CANCESTRY_FSM_OK);
    /* Three of the six actions ran; the transition itself completed. */
    CANCESSTRY_TEST_CHECK(fsm_test_has_line(&fx, "log 1 info one"));
    CANCESSTRY_TEST_CHECK(fsm_test_has_line(&fx, "log 1 info three"));
    CANCESSTRY_TEST_CHECK(!fsm_test_has_line(&fx, "log 1 info four"));
    CANCESSTRY_TEST_CHECK_STRING(fsm_test_state(&fx, "lab.one"), "B");
    CANCESSTRY_TEST_CHECK_U64(fsm_test_counters(&fx, "lab.one")->budget_exhausted, 1u);
    CANCESSTRY_TEST_CHECK(fsm_test_has_line(&fx, "warn 1 action budget exhausted"));
    /* The instance stays usable, so the budget throttles rather than punishes. */
    CANCESSTRY_TEST_CHECK_U64(fsm_test_lifecycle(&fx, "lab.one"),
                              CANCESTRY_FSM_INSTANCE_LIFECYCLE_RUNNING);
    CANCESSTRY_TEST_CHECK_U64(fsm_test_counters(&fx, "lab.one")->actions_executed, 3u);
    fsm_test_destroy(&fx);
}

int main(void)
{
    CANCESSTRY_TEST_SUITE_BEGIN("fsm/transition_precedence");
    case_single_selection();
    case_action_order();
    case_deferred();
    case_chain_limit();
    case_first_wins();
    case_self_transition();
    case_action_budget();
    return CANCESSTRY_TEST_SUITE_END();
}
