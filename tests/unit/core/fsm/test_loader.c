/*
 * CANcestry FSM loader unit tests.
 *
 * Verifies:
 *   SW-FR-FSM-001  FSM definitions load from declarative files.
 *   SW-FR-FSM-002  definitions are compiled into an internal representation.
 *   SW-FR-FSM-003  invalid definitions are rejected.
 *   SW-FR-FSM-005  exactly one initial state, and it exists.
 *   SW-FR-FSM-006  states are named and unique within a state machine.
 *   SW-FR-FSM-011  guards are optional expressions.
 *   SW-FR-FSM-017  the seven event selectors, and only those.
 *   SW-FR-FSM-022  the nine action kinds, and only those.
 *   SW-FR-FSM-026..030 timer declaration fields.
 *   SW-FR-FSM-031..034 variable declarations.
 *   SW-FR-FSM-035, SW-FR-FSM-037 expressions are grammar-checked at load time.
 *   SYS-NF-002     loading allocates, and owns everything it returns.
 *   schemas/fsm-0.2.0.schema.json, enforced field by field in C.
 *
 * Test ids: FSM-LOAD-001 .. FSM-LOAD-006.
 */

#include "cancestry/fsm/loader.h"

#include "cancestry_test.h"

#include <stdlib.h>
#include <string.h>

/** A document that exercises every construct the schema allows. */
static const char *const full_document =
    "schema_version: \"0.2.0\"\n"
    "state_machines:\n"
    "  - name: cluster\n"
    "    description: Cluster emulator\n"
    "    initial: IDLE\n"
    "    variables:\n"
    "      - name: cycles\n"
    "        type: integer\n"
    "        default: 3\n"
    "      - name: awake\n"
    "        type: boolean\n"
    "        default: false\n"
    "      - name: ratio\n"
    "        type: float\n"
    "        default: \"var.cycles * 0.5\"\n"
    "    timers:\n"
    "      - name: beat\n"
    "        duration_ms: 25\n"
    "        repeat: true\n"
    "        auto_start: true\n"
    "      - name: once\n"
    "        duration_ms: 1\n"
    "        repeat: false\n"
    "        auto_start: false\n"
    "    states:\n"
    "      - name: IDLE\n"
    "        entry:\n"
    "          - log:\n"
    "              level: info\n"
    "              message: entering idle\n"
    "        exit:\n"
    "          - set_variable:\n"
    "              variable: awake\n"
    "              value: true\n"
    "        transitions:\n"
    "          - event: can_rx\n"
    "            interface: can0\n"
    "            message: StatusMsg\n"
    "            guard: \"sig.VehicleSpeed > 0\"\n"
    "            target: ACTIVE\n"
    "            actions:\n"
    "              - send_message:\n"
    "                  interface: car\n"
    "                  message: demo.ClusterMsg\n"
    "                  signals:\n"
    "                    ClusterSpeed: 10\n"
    "                    Other: \"var.cycles + 1\"\n"
    "              - start_timer:\n"
    "                  timer: once\n"
    "                  duration_ms: 5\n"
    "                  repeat: false\n"
    "              - transition:\n"
    "                  target: IDLE\n"
    "          - event: timer_expired\n"
    "            timer: beat\n"
    "            target: ACTIVE\n"
    "      - name: ACTIVE\n"
    "        transitions:\n"
    "          - event: signal_changed\n"
    "            signal: IgnitionState\n"
    "            target: IDLE\n"
    "          - event: fault_raised\n"
    "            actions:\n"
    "              - raise_fault:\n"
    "                  code: CLUSTER_LIMP\n"
    "                  severity: error\n"
    "              - stop_timer:\n"
    "                  timer: beat\n"
    "              - reset_timer:\n"
    "                  timer: once\n"
    "              - set_signal:\n"
    "                  signal: ClusterAlive\n"
    "                  value: 0\n"
    "            target: IDLE\n"
    "          - event: power_mode_changed\n"
    "            target: IDLE\n"
    "          - event: state_exited\n"
    "            target: IDLE\n"
    "instances:\n"
    "  - id: cluster.left\n"
    "    machine: cluster\n"
    "    enabled: true\n"
    "    bindings:\n"
    "      car: can1\n"
    "    variables:\n"
    "      cycles: 7\n"
    "      ratio: 1.25\n"
    "    subscriptions:\n"
    "      can_rx:\n"
    "        - interface: can0\n"
    "          id: 0x1A0\n"
    "        - interface: car\n"
    "          message: StatusMsg\n"
    "      signals:\n"
    "        - IgnitionState\n"
    "      timers: true\n"
    "      faults: true\n"
    "      power_mode: false\n";

static cancestry_fsm_set_t *load_ok(const char *text)
{
    cancestry_fsm_load_error_t error;
    cancestry_fsm_set_t *set = cancestry_fsm_set_load(text, strlen(text), &error);

    if (set == NULL) {
        printf("    FAIL expected the document to load: %s (line %u, column %u)\n", error.message,
               (unsigned)error.line, (unsigned)error.column);
        fflush(stdout);
        cancestry_test_failures++;
    }
    return set;
}

static void load_rejected(const char *text, cancestry_fsm_status_t expected, const char *label)
{
    cancestry_fsm_load_error_t error;
    cancestry_fsm_set_t *set;

    memset(&error, 0, sizeof(error));
    set = cancestry_fsm_set_load(text, strlen(text), &error);
    CANCESSTRY_TEST_CHECK(set == NULL);
    if (set != NULL) {
        printf("    FAIL %s was accepted\n", label);
        cancestry_fsm_set_free(set);
        cancestry_test_failures++;
        return;
    }
    CANCESSTRY_TEST_CHECK_U64(error.status, (uint64_t)(int64_t)expected);
    CANCESSTRY_TEST_CHECK(error.message[0] != '\0');
}

/** Build a one-state machine document around one line of a state machine body. */
static char scratch[8192];

static const char *machine_with(const char *body)
{
    (void)snprintf(scratch, sizeof(scratch),
                   "schema_version: \"0.2.0\"\n"
                   "state_machines:\n"
                   "  - name: m\n"
                   "    initial: A\n"
                   "    states:\n"
                   "%s"
                   "instances:\n"
                   "  - id: i\n"
                   "    machine: m\n"
                   "    enabled: true\n",
                   body);
    return scratch;
}

/** Build a document whose only instance body is @p body. */
static const char *instance_with(const char *body)
{
    (void)snprintf(scratch, sizeof(scratch),
                   "schema_version: \"0.2.0\"\n"
                   "state_machines:\n"
                   "  - name: m\n"
                   "    initial: A\n"
                   "    states:\n"
                   "      - name: A\n"
                   "instances:\n"
                   "  - id: i\n"
                   "    machine: m\n"
                   "%s",
                   body);
    return scratch;
}

/** Same as instance_with(), with the mandatory "enabled" flag already present. */
static const char *instance_with_declared(const char *body)
{
    static char composed[8192];

    (void)snprintf(composed, sizeof(composed), "%s", instance_with(body));
    return composed;
}

/* FSM-LOAD-001: the full document compiles into indices, not names. */
static void case_full_document(void)
{
    cancestry_fsm_set_t *set = load_ok(full_document);
    const cancestry_fsm_machine_t *machine;
    const cancestry_fsm_instance_def_t *instance;
    const cancestry_fsm_state_t *idle;
    const cancestry_fsm_state_t *active;
    const cancestry_fsm_transition_t *first;

    CANCESSTRY_TEST_CASE("FSM-LOAD-001 full document");
    CANCESSTRY_TEST_CHECK(set != NULL);
    if (set == NULL) {
        return;
    }
    CANCESSTRY_TEST_CHECK_U64(set->machine_count, 1u);
    CANCESSTRY_TEST_CHECK_U64(set->instance_count, 1u);
    machine = &set->machines[0];
    CANCESSTRY_TEST_CHECK_STRING(machine->name, "cluster");
    CANCESSTRY_TEST_CHECK_STRING(machine->description, "Cluster emulator");
    CANCESSTRY_TEST_CHECK_U64(machine->state_count, 2u);
    CANCESSTRY_TEST_CHECK_U64(machine->variable_count, 3u);
    CANCESSTRY_TEST_CHECK_U64(machine->timer_count, 2u);
    CANCESSTRY_TEST_CHECK_STRING(machine->initial, "IDLE");
    /* Compiled: the initial state is an index (SW-FR-FSM-002, SW-FR-FSM-005). */
    CANCESSTRY_TEST_CHECK_U64(machine->initial_index, 0u);

    idle = &machine->states[0];
    active = &machine->states[1];
    CANCESSTRY_TEST_CHECK_STRING(idle->name, "IDLE");
    CANCESSTRY_TEST_CHECK_STRING(active->name, "ACTIVE");
    CANCESSTRY_TEST_CHECK_U64(idle->entry_count, 1u);
    CANCESSTRY_TEST_CHECK_U64(idle->exit_count, 1u);
    CANCESSTRY_TEST_CHECK_U64(idle->transition_count, 2u);
    /* signal_changed, fault_raised, power_mode_changed and state_exited. */
    CANCESSTRY_TEST_CHECK_U64(active->transition_count, 4u);

    first = &idle->transitions[0];
    CANCESSTRY_TEST_CHECK_U64(first->event, CANCESTRY_EVENT_TYPE_CAN_RX);
    CANCESSTRY_TEST_CHECK_STRING(first->interface, "can0");
    CANCESSTRY_TEST_CHECK_STRING(first->message, "StatusMsg");
    CANCESSTRY_TEST_CHECK_STRING(first->guard, "sig.VehicleSpeed > 0");
    CANCESSTRY_TEST_CHECK_STRING(first->target, "ACTIVE");
    /* Compiled: the target index was resolved at load, not at run time. */
    CANCESSTRY_TEST_CHECK_U64(first->target_index, 1u);
    CANCESSTRY_TEST_CHECK_U64(first->action_count, 3u);
    CANCESSTRY_TEST_CHECK_U64(first->actions[0].kind, CANCESTRY_FSM_ACTION_SEND_MESSAGE);
    CANCESSTRY_TEST_CHECK_U64(first->actions[0].as.send_message.value_count, 2u);
    CANCESSTRY_TEST_CHECK(first->actions[0].as.send_message.values[0].operand.is_expression ==
                          false);
    CANCESSTRY_TEST_CHECK_U64(first->actions[0].as.send_message.values[0].operand.literal.kind,
                           CANCESTRY_VALUE_KIND_INT);
    CANCESSTRY_TEST_CHECK_U64(
        first->actions[0].as.send_message.values[0].operand.literal.value.integer, 10);
    CANCESSTRY_TEST_CHECK(first->actions[0].as.send_message.values[1].operand.is_expression);
    CANCESSTRY_TEST_CHECK_STRING(first->actions[0].as.send_message.values[1].operand.expression,
                                 "var.cycles + 1");
    CANCESSTRY_TEST_CHECK_U64(first->actions[1].kind, CANCESTRY_FSM_ACTION_START_TIMER);
    CANCESSTRY_TEST_CHECK(first->actions[1].as.start_timer.has_duration_ms);
    CANCESSTRY_TEST_CHECK_U64(first->actions[1].as.start_timer.duration_ms, 5u);
    CANCESSTRY_TEST_CHECK_U64(first->actions[2].kind, CANCESTRY_FSM_ACTION_TRANSITION);
    CANCESSTRY_TEST_CHECK_U64(first->actions[2].as.transition.target_index, 0u);

    /* Variables keep their declared kinds (SW-FR-FSM-032) and defaults. */
    CANCESSTRY_TEST_CHECK_U64(machine->variables[0].type, CANCESTRY_VALUE_KIND_INT);
    CANCESSTRY_TEST_CHECK(machine->variables[0].has_default);
    CANCESSTRY_TEST_CHECK_U64(machine->variables[1].type, CANCESTRY_VALUE_KIND_BOOL);
    CANCESSTRY_TEST_CHECK(machine->variables[1].default_value.literal.value.boolean == false);
    CANCESSTRY_TEST_CHECK_U64(machine->variables[2].type, CANCESTRY_VALUE_KIND_REAL);
    CANCESSTRY_TEST_CHECK(machine->variables[2].default_value.is_expression);

    /* Timers: required fields, and the schema's 1 ms minimum. */
    CANCESSTRY_TEST_CHECK(machine->timers[0].repeat);
    CANCESSTRY_TEST_CHECK(machine->timers[0].auto_start);
    CANCESSTRY_TEST_CHECK_U64(machine->timers[0].duration_ms, 25u);
    CANCESSTRY_TEST_CHECK(machine->timers[1].auto_start == false);

    /* Instance: bindings, overrides and subscriptions (fsm-spec.md section 2). */
    instance = &set->instances[0];
    CANCESSTRY_TEST_CHECK_STRING(instance->id, "cluster.left");
    CANCESSTRY_TEST_CHECK_STRING(instance->machine, "cluster");
    CANCESSTRY_TEST_CHECK_U64(instance->machine_index, 0u);
    CANCESSTRY_TEST_CHECK(instance->enabled);
    CANCESSTRY_TEST_CHECK_U64(instance->binding_count, 1u);
    CANCESSTRY_TEST_CHECK_STRING(instance->bindings[0].alias, "car");
    CANCESSTRY_TEST_CHECK_STRING(instance->bindings[0].physical, "can1");
    CANCESSTRY_TEST_CHECK_U64(instance->variable_count, 2u);
    CANCESSTRY_TEST_CHECK(instance->variables[0].operand.is_expression == false);
    CANCESSTRY_TEST_CHECK_U64(instance->variables[0].operand.literal.value.integer, 7u);
    CANCESSTRY_TEST_CHECK_U64(instance->variables[1].operand.literal.kind,
                           CANCESTRY_VALUE_KIND_REAL);
    CANCESSTRY_TEST_CHECK(instance->subscriptions.has_can_rx);
    CANCESSTRY_TEST_CHECK_U64(instance->subscriptions.can_rx_count, 2u);
    CANCESSTRY_TEST_CHECK(instance->subscriptions.can_rx[0].has_can_id);
    CANCESSTRY_TEST_CHECK_U64(instance->subscriptions.can_rx[0].can_id, 0x1A0u);
    CANCESSTRY_TEST_CHECK(instance->subscriptions.can_rx[1].message != NULL);
    CANCESSTRY_TEST_CHECK(instance->subscriptions.has_signals);
    CANCESSTRY_TEST_CHECK_U64(instance->subscriptions.signal_count, 1u);
    CANCESSTRY_TEST_CHECK_STRING(instance->subscriptions.signals[0], "IgnitionState");
    CANCESSTRY_TEST_CHECK(instance->subscriptions.has_timers);
    CANCESSTRY_TEST_CHECK(instance->subscriptions.timers);
    CANCESSTRY_TEST_CHECK(instance->subscriptions.has_faults);
    CANCESSTRY_TEST_CHECK(instance->subscriptions.faults);
    CANCESSTRY_TEST_CHECK(instance->subscriptions.has_power_mode);
    CANCESSTRY_TEST_CHECK(instance->subscriptions.power_mode == false);
    cancestry_fsm_set_free(set);
}

/* FSM-LOAD-002: the loader owns its strings. */
static void case_owns_strings(void)
{
    char *mutable_text;
    cancestry_fsm_set_t *set;
    const char *loaded_name;

    CANCESSTRY_TEST_CASE("FSM-LOAD-002 the set does not alias the input text");
    mutable_text = malloc(strlen(full_document) + 1u);
    CANCESSTRY_TEST_CHECK(mutable_text != NULL);
    if (mutable_text == NULL) {
        return;
    }
    memcpy(mutable_text, full_document, strlen(full_document) + 1u);
    set = load_ok(mutable_text);
    if (set == NULL) {
        free(mutable_text);
        return;
    }
    loaded_name = set->machines[0].name;
    CANCESSTRY_TEST_CHECK(loaded_name < mutable_text ||
                          loaded_name >= mutable_text + strlen(mutable_text) + 1u);
    /* Overwriting the caller's buffer must not change what was loaded. */
    memset(mutable_text, 'x', strlen(mutable_text));
    CANCESSTRY_TEST_CHECK_STRING(set->machines[0].name, "cluster");
    CANCESSTRY_TEST_CHECK_STRING(set->instances[0].id, "cluster.left");
    cancestry_fsm_set_free(set);
    free(mutable_text);
}

/* FSM-LOAD-003: the schema's structural rules. */
static void case_schema_rules(void)
{
    CANCESSTRY_TEST_CASE("FSM-LOAD-003 schema structure is enforced");

    /* schema_version is required and pinned to 0.2.0. */
    load_rejected("state_machines:\n  - name: m\n    initial: A\n    states:\n      - name: A\n"
                  "instances:\n  - id: i\n    machine: m\n    enabled: true\n",
                  CANCESTRY_FSM_ERR_PARSE, "a missing schema_version");
    load_rejected("schema_version: \"0.3.0\"\nstate_machines:\n  - name: m\n    initial: A\n"
                  "    states:\n      - name: A\ninstances:\n  - id: i\n    machine: m\n"
                  "    enabled: true\n",
                  CANCESTRY_FSM_ERR_PARSE, "a 0.3.0 schema_version");
    /* An unquoted 0.2.0 is still a YAML string, so the schema's const is satisfied:
     * the loader accepts it, and only a value that is not "0.2.0" is refused. */
    {
        cancestry_fsm_set_t *set =
            load_ok("schema_version: 0.2.0\nstate_machines:\n  - name: m\n    initial: A\n"
                    "    states:\n      - name: A\ninstances:\n  - id: i\n    machine: m\n"
                    "    enabled: true\n");

        CANCESSTRY_TEST_CHECK(set != NULL);
        if (set != NULL) {
            cancestry_fsm_set_free(set);
        }
    }
    /* additionalProperties: false at every level. */
    load_rejected("schema_version: \"0.2.0\"\nlayout:\n  x: 1\nstate_machines:\n  - name: m\n"
                  "    initial: A\n    states:\n      - name: A\ninstances:\n  - id: i\n"
                  "    machine: m\n    enabled: true\n",
                  CANCESTRY_FSM_ERR_PARSE, "an unknown top-level field");
    load_rejected(machine_with("      - name: A\n        substate: B\n"), CANCESTRY_FSM_ERR_PARSE,
                  "an unknown state field");
    load_rejected(machine_with("      - name: A\n        on_entry: []\n"),
                  CANCESTRY_FSM_ERR_PARSE, "an on_entry field instead of entry");
    /* minItems: 1 on state_machines, instances and states. */
    load_rejected("schema_version: \"0.2.0\"\nstate_machines:\ninstances:\n  - id: i\n"
                  "    machine: m\n    enabled: true\n",
                  CANCESTRY_FSM_ERR_PARSE, "an empty state_machines list");
    load_rejected("schema_version: \"0.2.0\"\nstate_machines:\n  - name: m\n    initial: A\n"
                  "    states:\ninstances:\n  - id: i\n    machine: m\n    enabled: true\n",
                  CANCESTRY_FSM_ERR_PARSE, "a state machine without states");
    /* required fields. */
    load_rejected(machine_with("      - transitions:\n          - event: can_rx\n"
                               "            target: A\n"),
                  CANCESTRY_FSM_ERR_PARSE, "a state without a name");
    load_rejected(machine_with("      - name: A\n        transitions:\n          - target: A\n"),
                  CANCESTRY_FSM_ERR_PARSE, "a transition without an event");
    load_rejected(machine_with("      - name: A\n        transitions:\n          - event: can_rx\n"),
                  CANCESTRY_FSM_ERR_PARSE, "a transition without a target");
    load_rejected("schema_version: \"0.2.0\"\nstate_machines:\n  - name: m\n    states:\n"
                  "      - name: A\ninstances:\n  - id: i\n    machine: m\n    enabled: true\n",
                  CANCESTRY_FSM_ERR_PARSE, "a machine without an initial state");
    load_rejected("schema_version: \"0.2.0\"\nstate_machines:\n  - name: m\n    initial: A\n"
                  "    states:\n      - name: A\ninstances:\n  - id: i\n    machine: m\n",
                  CANCESTRY_FSM_ERR_PARSE, "an instance without enabled");
    /* The event enum is closed: no timeout, no user_event. */
    load_rejected(machine_with("      - name: A\n        transitions:\n          - event: timeout\n"
                               "            target: A\n"),
                  CANCESTRY_FSM_ERR_PARSE, "a timeout event selector");
    load_rejected(machine_with("      - name: A\n        transitions:\n          - event: any\n"
                               "            target: A\n"),
                  CANCESTRY_FSM_ERR_PARSE, "an any event selector");
    /* Conditional required fields. */
    load_rejected(machine_with("      - name: A\n        transitions:\n          - event: timer_expired\n"
                               "            target: A\n"),
                  CANCESTRY_FSM_ERR_PARSE, "timer_expired without a timer");
    load_rejected(machine_with("      - name: A\n        transitions:\n          - event: signal_changed\n"
                               "            target: A\n"),
                  CANCESTRY_FSM_ERR_PARSE, "signal_changed without a signal");
    /* Numeric bounds from the schema. */
    load_rejected(machine_with("      - name: A\n        timers: []\n"),
                  CANCESTRY_FSM_ERR_PARSE, "timers inside a state");
    /* Action keys are a closed oneOf set. */
    load_rejected(machine_with("      - name: A\n        entry:\n          - send_frame:\n"
                               "              interface: can0\n"),
                  CANCESTRY_FSM_ERR_PARSE, "an unknown action key");
    load_rejected(machine_with("      - name: A\n        entry:\n          - log:\n"
                               "              level: info\n              message: hi\n"
                               "              set_variable:\n                variable: x\n"
                               "                value: 1\n"),
                  CANCESTRY_FSM_ERR_PARSE, "two action keys in one action");
    load_rejected(machine_with("      - name: A\n        entry:\n          - log:\n"
                               "              level: debug\n              message: hi\n"),
                  CANCESTRY_FSM_ERR_PARSE, "a log level outside the enum");
    load_rejected(machine_with("      - name: A\n        entry:\n          - raise_fault:\n"
                               "              code: X\n              severity: fatal\n"),
                  CANCESTRY_FSM_ERR_PARSE, "a severity outside the enum");
    load_rejected(machine_with("      - name: A\n        entry:\n          - start_timer:\n"
                               "              timer: t\n              duration_ms: 0\n"),
                  CANCESTRY_FSM_ERR_PARSE, "a timer duration below the minimum");
    /* Duplicate keys inside one mapping are ambiguous, so they are refused. */
    load_rejected(machine_with("      - name: A\n        name: B\n"), CANCESTRY_FSM_ERR_PARSE,
                  "a duplicate mapping key");
    /* Outside the documented YAML subset. */
    load_rejected(machine_with("      - {name: A}\n"), CANCESTRY_FSM_ERR_PARSE,
                  "a flow-style mapping");
    load_rejected(machine_with("      - name: A\n        transitions: []\n"),
                  CANCESTRY_FSM_ERR_PARSE, "a flow-style sequence");
    load_rejected("schema_version: \"0.2.0\"\t\nstate_machines:\n", CANCESTRY_FSM_ERR_PARSE,
                  "a tab after a scalar");
    /* Empty and absent input. */
    load_rejected("", CANCESTRY_FSM_ERR_PARSE, "an empty document");
    CANCESSTRY_TEST_CHECK(cancestry_fsm_set_load(NULL, 0u, NULL) == NULL);
}

/* FSM-LOAD-004: names, references and uniqueness. */
static void case_references(void)
{
    CANCESSTRY_TEST_CASE("FSM-LOAD-004 references are resolved or refused");
    /* SW-FR-FSM-006: state names are unique within a machine. */
    load_rejected(machine_with("      - name: A\n      - name: A\n"), CANCESTRY_FSM_ERR_PARSE,
                  "two states named A");
    /* SW-FR-FSM-005: the initial state must exist. */
    load_rejected("schema_version: \"0.2.0\"\nstate_machines:\n  - name: m\n    initial: MISSING\n"
                  "    states:\n      - name: A\ninstances:\n  - id: i\n    machine: m\n"
                  "    enabled: true\n",
                  CANCESTRY_FSM_ERR_PARSE, "an initial state that is not declared");
    /* A transition target must exist. */
    load_rejected(machine_with("      - name: A\n        transitions:\n          - event: can_rx\n"
                               "            target: ELSEWHERE\n"),
                  CANCESTRY_FSM_ERR_PARSE, "a transition target that is not declared");
    /* A deferred transition target must exist too. */
    load_rejected(machine_with("      - name: A\n        entry:\n          - transition:\n"
                               "              target: ELSEWHERE\n"),
                  CANCESTRY_FSM_ERR_PARSE, "a transition action targeting a missing state");
    /* The machine an instance names must exist. */
    load_rejected("schema_version: \"0.2.0\"\nstate_machines:\n  - name: m\n    initial: A\n"
                  "    states:\n      - name: A\ninstances:\n  - id: i\n    machine: other\n"
                  "    enabled: true\n",
                  CANCESTRY_FSM_ERR_PARSE, "an instance naming an unknown machine");
    /* Timer names used by a transition or an action must exist (SwAD.md section 10:
     * unknown references are definition errors). */
    load_rejected(machine_with("      - name: A\n        transitions:\n          - event: timer_expired\n"
                               "            timer: nosuch\n            target: A\n"),
                  CANCESTRY_FSM_ERR_PARSE, "a transition on an undeclared timer");
    load_rejected(machine_with("      - name: A\n        entry:\n          - stop_timer:\n"
                               "              timer: nosuch\n"),
                  CANCESTRY_FSM_ERR_PARSE, "a stop_timer for an undeclared timer");
    /* Variable names are instance-scoped, and must be declared (SW-FR-FSM-031,
     * SW-FR-FSM-034). */
    load_rejected(machine_with("      - name: A\n        entry:\n          - set_variable:\n"
                               "              variable: nosuch\n              value: 1\n"),
                  CANCESTRY_FSM_ERR_PARSE, "a set_variable for an undeclared variable");
    /* Timer and variable names are unique inside a machine, like states. */
    load_rejected("schema_version: \"0.2.0\"\nstate_machines:\n  - name: m\n    initial: A\n"
                  "    timers:\n      - name: t\n        duration_ms: 1\n        repeat: false\n"
                  "        auto_start: false\n      - name: t\n        duration_ms: 2\n"
                  "        repeat: false\n        auto_start: false\n"
                  "    states:\n      - name: A\ninstances:\n  - id: i\n    machine: m\n"
                  "    enabled: true\n",
                  CANCESTRY_FSM_ERR_PARSE, "two timers named t");
    /* Instance ids are the runtime handle, so duplicates are a conflict. */
    load_rejected("schema_version: \"0.2.0\"\nstate_machines:\n  - name: m\n    initial: A\n"
                  "    states:\n      - name: A\ninstances:\n  - id: i\n    machine: m\n"
                  "    enabled: true\n  - id: i\n    machine: m\n    enabled: false\n",
                  CANCESTRY_FSM_ERR_CONFLICT, "two instances with the same id");
    /* Two machines with the same name would make an instance reference ambiguous. */
    load_rejected("schema_version: \"0.2.0\"\nstate_machines:\n  - name: m\n    initial: A\n"
                  "    states:\n      - name: A\n  - name: m\n    initial: A\n    states:\n"
                  "      - name: A\ninstances:\n  - id: i\n    machine: m\n    enabled: true\n",
                  CANCESTRY_FSM_ERR_CONFLICT, "two machines with the same name");
    /* Long names are refused rather than truncated. */
    {
        char long_name[200];
        char *body;
        size_t i;

        for (i = 0u; i < sizeof(long_name) - 1u; ++i) {
            long_name[i] = 'x';
        }
        long_name[sizeof(long_name) - 1u] = '\0';
        body = malloc(strlen(long_name) + 64u);
        CANCESSTRY_TEST_CHECK(body != NULL);
        if (body != NULL) {
            (void)sprintf(body, "      - name: %s\n", long_name);
            load_rejected(machine_with(body), CANCESTRY_FSM_ERR_PARSE, "a name beyond the limit");
            free(body);
        }
    }
    /* Schema bounds on the instance side, not only the machine side. */
    load_rejected(instance_with("    enabled: true\n"
                                "    subscriptions:\n"
                                "      can_rx:\n"
                                "        - interface: can0\n"
                                "          id: 536870912\n"),
                  CANCESTRY_FSM_ERR_PARSE, "a CAN id above the 29-bit maximum");
    load_rejected(instance_with("    enabled: yes\n"), CANCESTRY_FSM_ERR_PARSE,
                  "yes instead of true for enabled");
    load_rejected(instance_with("    enabled: true\n"
                                "    subscriptions:\n"
                                "      signals: true\n"),
                  CANCESTRY_FSM_ERR_PARSE, "signals as a scalar instead of a list");
    load_rejected(instance_with("    enabled: true\n"
                                "    variables:\n"
                                "      undeclared: 1\n"),
                  CANCESTRY_FSM_ERR_PARSE, "an override for an undeclared variable");
    load_rejected(instance_with("    bindings:\n"
                                "      car: can0\n"),
                  CANCESTRY_FSM_ERR_PARSE, "an instance without enabled");
    /* A subscription list may be declared empty, which is different from being
     * absent, so the loader records the distinction. */
    {
        cancestry_fsm_set_t *set =
            load_ok(instance_with_declared("    enabled: true\n"
                                           "    subscriptions:\n"
                                           "      timers: false\n"
                                           "      faults: false\n"
                                           "      power_mode: false\n"));

        CANCESSTRY_TEST_CHECK(set != NULL);
        if (set != NULL) {
            const cancestry_fsm_subscriptions_t *subscriptions = &set->instances[0].subscriptions;

            CANCESSTRY_TEST_CHECK(subscriptions->has_timers);
            CANCESSTRY_TEST_CHECK(subscriptions->timers == false);
            CANCESSTRY_TEST_CHECK(subscriptions->has_faults);
            CANCESSTRY_TEST_CHECK(subscriptions->faults == false);
            CANCESSTRY_TEST_CHECK(subscriptions->has_power_mode);
            CANCESSTRY_TEST_CHECK(subscriptions->power_mode == false);
            CANCESSTRY_TEST_CHECK(!subscriptions->has_can_rx);
            CANCESSTRY_TEST_CHECK(!subscriptions->has_signals);
            cancestry_fsm_set_free(set);
        }
    }
    /* A machine with one state and no transitions loads: a terminal machine is
     * legal, it just never moves. */
    {
        cancestry_fsm_set_t *set = load_ok(machine_with("      - name: A\n"));

        CANCESSTRY_TEST_CHECK(set != NULL);
        if (set != NULL) {
            CANCESSTRY_TEST_CHECK_U64(set->instances[0].machine_index, 0u);
            CANCESSTRY_TEST_CHECK_U64(set->machines[0].states[0].transition_count, 0u);
            cancestry_fsm_set_free(set);
        }
    }
}

/* FSM-LOAD-005: expressions are grammar-checked at load time. */
static void case_expression_checks(void)
{
    CANCESSTRY_TEST_CASE("FSM-LOAD-005 expressions are compiled, not guessed");
    /* A valid guard loads. */
    {
        cancestry_fsm_set_t *set =
            load_ok(machine_with("      - name: A\n        transitions:\n"
                                 "          - event: can_rx\n            guard: \"min(sig.A, 1) <= "
                                 "max(var.b, 0)\"\n            target: A\n"));

        CANCESSTRY_TEST_CHECK(set != NULL);
        if (set != NULL) {
            cancestry_fsm_set_free(set);
        }
    }
    /* Bare identifiers are not permitted (expression-language.md section 1). */
    load_rejected(machine_with("      - name: A\n        transitions:\n          - event: can_rx\n"
                               "            guard: \"speed > 0\"\n            target: A\n"),
                  CANCESTRY_FSM_ERR_PARSE, "a bare identifier in a guard");
    /* Unknown namespaces. */
    load_rejected(machine_with("      - name: A\n        transitions:\n          - event: can_rx\n"
                               "            guard: \"mem.x > 0\"\n            target: A\n"),
                  CANCESTRY_FSM_ERR_PARSE, "an unknown namespace in a guard");
    /* Arbitrary code: a call outside the five built-ins. */
    load_rejected(machine_with("      - name: A\n        transitions:\n          - event: can_rx\n"
                               "            guard: \"system(0) == 0\"\n            target: A\n"),
                  CANCESTRY_FSM_ERR_PARSE, "a call to a non-built-in function");
    /* Broken syntax. */
    load_rejected(machine_with("      - name: A\n        transitions:\n          - event: can_rx\n"
                               "            guard: \"sig.a ==\"\n            target: A\n"),
                  CANCESTRY_FSM_ERR_PARSE, "a truncated guard");
    load_rejected(machine_with("      - name: A\n        transitions:\n          - event: can_rx\n"
                               "            guard: \"sig.a ;; sig.b\"\n            target: A\n"),
                  CANCESTRY_FSM_ERR_PARSE, "a guard with a statement separator");
    /* Nesting beyond the evaluator's budget is a definition error, so the runtime
     * never sees an expression it cannot evaluate within its bounds. */
    {
        char deep[1024];
        char body[1280];
        size_t i;
        size_t used = 0u;

        for (i = 0u; i < 70u; ++i) {
            deep[used++] = '(';
        }
        used += (size_t)sprintf(deep + used, "sig.a == 1");
        for (i = 0u; i < 70u; ++i) {
            deep[used++] = ')';
        }
        deep[used] = '\0';
        (void)sprintf(body,
                      "      - name: A\n        transitions:\n          - event: can_rx\n"
                      "            guard: \"%s\"\n            target: A\n",
                      deep);
        load_rejected(machine_with(body), CANCESTRY_FSM_ERR_PARSE, "a guard nested too deeply");
    }
}

/* FSM-LOAD-006: value typing rules from the schema. */
static void case_value_types(void)
{
    cancestry_fsm_set_t *set;
    const cancestry_fsm_variable_def_t *variables;

    CANCESSTRY_TEST_CASE("FSM-LOAD-006 literal and expression typing");
    set = load_ok("schema_version: \"0.2.0\"\nstate_machines:\n  - name: m\n    initial: A\n"
                  "    variables:\n      - name: i\n        type: integer\n        default: -5\n"
                  "      - name: h\n        type: integer\n        default: 0x10\n"
                  "      - name: o\n        type: integer\n        default: 0o17\n"
                  "      - name: r\n        type: float\n        default: 1.5\n"
                  "      - name: e\n        type: float\n        default: 1e3\n"
                  "      - name: t\n        type: boolean\n        default: true\n"
                  "      - name: q\n        type: integer\n        default: \"42\"\n"
                  "      - name: n\n        type: integer\n"
                  "    states:\n      - name: A\n"
                  "instances:\n  - id: i\n    machine: m\n    enabled: true\n");
    if (set == NULL) {
        return;
    }
    variables = set->machines[0].variables;
    CANCESSTRY_TEST_CHECK_U64(set->machines[0].variable_count, 8u);
    CANCESSTRY_TEST_CHECK_U64(variables[0].default_value.literal.kind, CANCESTRY_VALUE_KIND_INT);
    CANCESSTRY_TEST_CHECK_U64(variables[0].default_value.literal.value.integer,
                              (uint64_t)(int64_t)-5);
    CANCESSTRY_TEST_CHECK_U64(variables[1].default_value.literal.value.integer, 16u);
    CANCESSTRY_TEST_CHECK_U64(variables[2].default_value.literal.value.integer, 15u);
    CANCESSTRY_TEST_CHECK_U64(variables[3].default_value.literal.kind, CANCESTRY_VALUE_KIND_REAL);
    CANCESSTRY_TEST_CHECK_DOUBLE(variables[4].default_value.literal.value.real, 1000.0, 1e-9);
    CANCESSTRY_TEST_CHECK_U64(variables[5].default_value.literal.kind, CANCESTRY_VALUE_KIND_BOOL);
    CANCESSTRY_TEST_CHECK(variables[5].default_value.literal.value.boolean);
    /* A quoted number is an expression, not a literal: the same rule the recipe
     * loader documents, so "42" and 42 do not mean the same thing. */
    CANCESSTRY_TEST_CHECK(variables[6].default_value.is_expression);
    CANCESSTRY_TEST_CHECK(variables[7].has_default == false);
    cancestry_fsm_set_free(set);

    /* A leading zero is not octal in YAML 1.2 plain scalars: the loader refuses it
     * instead of guessing, matching the codec and recipe loaders. */
    load_rejected("schema_version: \"0.2.0\"\nstate_machines:\n  - name: m\n    initial: A\n"
                  "    variables:\n      - name: i\n        type: integer\n        default: 010\n"
                  "    states:\n      - name: A\ninstances:\n  - id: i\n    machine: m\n"
                  "    enabled: true\n",
                  CANCESTRY_FSM_ERR_PARSE, "a leading-zero integer literal");
    /* An unsupported type name is refused (the schema enumerates three). */
    load_rejected("schema_version: \"0.2.0\"\nstate_machines:\n  - name: m\n    initial: A\n"
                  "    variables:\n      - name: i\n        type: string\n        default: x\n"
                  "    states:\n      - name: A\ninstances:\n  - id: i\n    machine: m\n"
                  "    enabled: true\n",
                  CANCESTRY_FSM_ERR_PARSE, "a string variable type");
}

int main(void)
{
    CANCESSTRY_TEST_SUITE_BEGIN("core/fsm/loader");
    case_full_document();
    case_owns_strings();
    case_schema_rules();
    case_references();
    case_expression_checks();
    case_value_types();
    return CANCESSTRY_TEST_SUITE_END();
}
