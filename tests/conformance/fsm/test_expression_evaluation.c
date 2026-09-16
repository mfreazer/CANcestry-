/*
 * CANcestry FSM conformance: expression evaluation.
 *
 * Contract under test (docs/system/expression-language.md sections 1-7,
 * docs/software/SwRS.md SW-FR-FSM-011, SW-FR-FSM-035..038, SW-FR-FSM-041):
 *
 *   - guards and value operands use the documented grammar, precedence and type
 *     rules, with integer/float promotion to float and no implicit string or
 *     boolean conversion;
 *   - names must be namespaced (sig., var., evt.); a bare identifier is a
 *     definition error at load time, not a runtime lookup;
 *   - the five built-in functions are the only callables, with their documented
 *     domains (clamp needs low <= high, round returns an integer, a result
 *     outside the integer range is a fault);
 *   - integer overflow, division or modulo by zero, NaN or Infinity, an
 *     undefined identifier and an unauthorized signal read are faults;
 *   - evaluation is bounded in depth and allocation-free, and nothing in the
 *     language can execute code;
 *   - a guard fault suspends the instance (docs/system/mode-fault-state-machine.md
 *     section 5) instead of silently evaluating to false.
 *
 * Test ids: EXPR-GRAMMAR-001, EXPR-TYPES-001, EXPR-FAULTS-001, EXPR-NAMESPACE-001,
 * EXPR-FUNCTIONS-001, EXPR-DEPTH-001, EXPR-ACTIONS-001, EXPR-NO-CODE-001.
 */

#include "cancestry_fsm_conformance.h"

#define EXPR_MAX_TEXT ((size_t)512u)
#define EXPR_TAKEN ((int)0)
#define EXPR_FALSE ((int)1)
#define EXPR_FAULT ((int)2)
#define EXPR_REJECTED ((int)3)

/* Buffer for a generated document; one per case, reused by the helpers below. */
static char expr_document[8192];

/**
 * Build a single-guard machine: one signal_changed transition from S0 to TAKEN,
 * so a test only has to name the expression it cares about.
 *
 * The machine declares an integer, a float and a boolean variable (4, 2.5 and
 * true), which is all the namespaces need to be exercised.
 */
static const char *expr_guard_document(const char *guard)
{
    (void)snprintf(expr_document, sizeof(expr_document),
                   "schema_version: \"0.2.0\"\n"
                   "state_machines:\n"
                   "  - name: lab\n"
                   "    initial: S0\n"
                   "    variables:\n"
                   "      - name: n\n"
                   "        type: integer\n"
                   "        default: 4\n"
                   "      - name: f\n"
                   "        type: float\n"
                   "        default: 2.5\n"
                   "      - name: b\n"
                   "        type: boolean\n"
                   "        default: true\n"
                   "    states:\n"
                   "      - name: S0\n"
                   "        transitions:\n"
                   "          - event: signal_changed\n"
                   "            signal: Trigger\n"
                   "            guard: \"%s\"\n"
                   "            target: TAKEN\n"
                   "      - name: TAKEN\n"
                   "instances:\n"
                   "  - id: lab.one\n"
                   "    machine: lab\n"
                   "    enabled: true\n",
                   guard);
    return expr_document;
}

/**
 * Evaluate @p guard in a fresh instance and report what the runtime did.
 *
 * @return EXPR_TAKEN when the guard held, EXPR_FALSE when it evaluated false,
 *         EXPR_FAULT when the evaluation faulted (and the instance was
 *         suspended), EXPR_REJECTED when the document did not load.
 */
static int expr_guard_outcome(fsm_test_fixture_t *fx, const char *guard)
{
    cancestry_event_t event;
    const cancestry_fsm_instance_counters_t *counters;

    if (!fsm_test_init(fx, expr_guard_document(guard))) {
        return EXPR_REJECTED;
    }
    if (fsm_test_start(fx, "lab.one") != CANCESTRY_FSM_OK) {
        return EXPR_REJECTED;
    }
    /* The demo signal the guard examples read, so a sig. reference resolves. */
    fsm_test_set_int(fx, "VehicleSpeed", 42);
    fsm_test_event_int(&event, 1000u, "Trigger", 1);
    (void)fsm_test_process(fx, &event);
    counters = fsm_test_counters(fx, "lab.one");
    if (counters->guard_errors > 0u) {
        return EXPR_FAULT;
    }
    if (counters->guards_true > 0u) {
        return EXPR_TAKEN;
    }
    return EXPR_FALSE;
}

/** Assert one guard against the expected outcome, with the expression in the label. */
static void expr_check_guard(const char *guard, int expected)
{
    static fsm_test_fixture_t fx;
    int outcome;

    if (expected != EXPR_REJECTED) {
        outcome = expr_guard_outcome(&fx, guard);
    } else {
        /* Rejection is a load-time verdict, so the raw loader is called directly:
         * the fixture helper would count an expected failure as a test failure. */
        cancestry_fsm_load_error_t error;
        const char *document = expr_guard_document(guard);
        cancestry_fsm_set_t *set =
            cancestry_fsm_set_load(document, strlen(document), &error);

        outcome = (set == NULL) ? EXPR_REJECTED : EXPR_TAKEN;
        if (set != NULL) {
            cancestry_fsm_set_free(set);
        }
    }
    if (outcome != expected) {
        char label[EXPR_MAX_TEXT];

        (void)snprintf(label, sizeof(label), "guard \"%s\" outcome %d, expected %d", guard,
                       outcome, expected);
        (void)cancestry_test_check_impl(false, label, __FILE__, __LINE__);
        fsm_test_dump(&fx);
    } else {
        CANCESSTRY_TEST_CHECK(true);
    }
    fsm_test_destroy(&fx);
}

/** @return true when @p guard's fault reason renders as @p reason in the trace. */
static bool expr_fault_reason_is(const char *guard, const char *reason)
{
    static fsm_test_fixture_t fx;
    char trace[4096];
    bool found;

    if (expr_guard_outcome(&fx, guard) != EXPR_FAULT) {
        fsm_test_destroy(&fx);
        return false;
    }
    (void)fsm_test_render_trace(&fx, trace, sizeof(trace));
    found = strstr(trace, reason) != NULL;
    if (!found) {
        printf("    trace was:\n%s\n", trace);
    }
    fsm_test_destroy(&fx);
    return found;
}

/* EXPR-GRAMMAR-001: operators, precedence and parentheses. */
static void case_grammar(void)
{
    CANCESSTRY_TEST_CASE("EXPR-GRAMMAR-001 operators and precedence");
    expr_check_guard("1 + 2 * 3 == 7", EXPR_TAKEN);
    expr_check_guard("(1 + 2) * 3 == 9", EXPR_TAKEN);
    expr_check_guard("1 + 2 * 3 == 9", EXPR_FALSE);
    expr_check_guard("-2 * -3 == 6", EXPR_TAKEN);
    expr_check_guard("2 - -3 == 5", EXPR_TAKEN);
    expr_check_guard("7 % 3 == 1", EXPR_TAKEN);
    expr_check_guard("1 < 2 and 2 <= 2 and 3 > 2 and 3 >= 3", EXPR_TAKEN);
    expr_check_guard("1 == 1 and 1 != 2", EXPR_TAKEN);
    expr_check_guard("not (1 == 2)", EXPR_TAKEN);
    expr_check_guard("false or (true and not false)", EXPR_TAKEN);
    expr_check_guard("true and false", EXPR_FALSE);
    expr_check_guard("true or false", EXPR_TAKEN);
    /* Parenthesis and unary-minus precedence: -3 * -3 is 9, and the comparison
     * binds looser than the multiplication (section 3). */
    expr_check_guard("- -3 == 3", EXPR_TAKEN);
    expr_check_guard("(2) == 2", EXPR_TAKEN);
    /* Outside the grammar: an assignment, a semicolon, a trailing operator. */
    expr_check_guard("var.n = 5", EXPR_REJECTED);
    expr_check_guard("1 + ", EXPR_REJECTED);
    expr_check_guard("1 == 1; 2 == 2", EXPR_REJECTED);
    expr_check_guard("(1 == 1", EXPR_REJECTED);
    /* Empty guards are not expressions: the field is optional, so an empty
     * string is rejected rather than read as "always true". */
    expr_check_guard("", EXPR_REJECTED);
}

/* EXPR-TYPES-001: types and coercion rules (section 6). */
static void case_types(void)
{
    CANCESSTRY_TEST_CASE("EXPR-TYPES-001 types and coercion");
    /* Integer and float operations promote to float. */
    expr_check_guard("1 + 0.5 == 1.5", EXPR_TAKEN);
    expr_check_guard("3 / 2 == 1", EXPR_TAKEN);
    expr_check_guard("3.0 / 2 == 1.5", EXPR_TAKEN);
    expr_check_guard("3 / 2 == 1.5", EXPR_FALSE);
    expr_check_guard("1 / 2 == 0", EXPR_TAKEN);
    /* Booleans are not numbers: arithmetic on them is a type fault. */
    expr_check_guard("true + 1 == 2", EXPR_FAULT);
    expr_check_guard("1 and 1", EXPR_FAULT);
    expr_check_guard("not 1", EXPR_FAULT);
    /* Comparing a boolean with a number is invalid coercion, but two booleans
     * may compare for equality (they are not ordered). */
    expr_check_guard("true == true", EXPR_TAKEN);
    expr_check_guard("true != false", EXPR_TAKEN);
    expr_check_guard("true < false", EXPR_FAULT);
    expr_check_guard("1 > true", EXPR_FAULT);
    /* Float modulo has no definition in the spec, so it faults rather than
     * guessing (documented in core/fsm/README.md). */
    expr_check_guard("3.5 % 2 == 1", EXPR_FAULT);
    /* Namespaced variables keep their declared kind. */
    expr_check_guard("var.n * 2 == 8", EXPR_TAKEN);
    expr_check_guard("var.f > var.n", EXPR_FALSE);
    expr_check_guard("var.b == true", EXPR_TAKEN);
    expr_check_guard("var.b and var.n == 4", EXPR_TAKEN);
}

/* EXPR-FAULTS-001: the failure rules of section 5, each with its reason. */
static void case_faults(void)
{
    CANCESSTRY_TEST_CASE("EXPR-FAULTS-001 failure rules");
    expr_check_guard("10 / 0 == 1", EXPR_FAULT);
    expr_check_guard("10 % 0 == 0", EXPR_FAULT);
    expr_check_guard("0 / 0 == 0", EXPR_FAULT);
    expr_check_guard("9223372036854775807 + 1 == 0", EXPR_FAULT);
    expr_check_guard("-9223372036854775807 - 2 == 0", EXPR_FAULT);
    expr_check_guard("9223372036854775807 * 2 == 0", EXPR_FAULT);
    expr_check_guard("1e400 > 1", EXPR_REJECTED);
    expr_check_guard("1.0 / 0.0 > 1", EXPR_FAULT);
    expr_check_guard("var.undefined == 1", EXPR_FAULT);
    /* An undefined identifier and an unauthorized read are distinguishable in the
     * trace, which is what makes a suspended instance explainable. */
    CANCESSTRY_TEST_CHECK(expr_fault_reason_is("10 / 0 == 1", "division by zero"));
    CANCESSTRY_TEST_CHECK(expr_fault_reason_is("9223372036854775807 + 1 == 0",
                                              "integer overflow"));
    CANCESSTRY_TEST_CHECK(expr_fault_reason_is("var.undefined == 1", "undefined identifier"));
    CANCESSTRY_TEST_CHECK(expr_fault_reason_is("sig.NotThere == 1", "unauthorized signal access"));
}

/* EXPR-NAMESPACE-001: only the three namespaces, only declared members. */
static void case_namespaces(void)
{
    static fsm_test_fixture_t fx;
    cancestry_event_t event;

    CANCESSTRY_TEST_CASE("EXPR-NAMESPACE-001 namespaces");
    /* Bare identifiers never resolve to a bus lookup. */
    expr_check_guard("VehicleSpeed == 42", EXPR_REJECTED);
    expr_check_guard("n == 4", EXPR_REJECTED);
    expr_check_guard("sig.VehicleSpeed == 42", EXPR_TAKEN);
    expr_check_guard("sig.VehicleSpeed == 43", EXPR_FALSE);
    /* Not a namespace CANcestry defines. */
    expr_check_guard("sig2.VehicleSpeed == 42", EXPR_REJECTED);
    expr_check_guard("mem.ram > 0", EXPR_REJECTED);
    /* evt. has a static field vocabulary; a field that does not apply to the
     * current event type is a fault, not a zero. */
    expr_check_guard("evt.timestamp_us == 1000", EXPR_TAKEN);
    expr_check_guard("evt.bogus_field == 1", EXPR_REJECTED);
    expr_check_guard("evt.missed_count == 0", EXPR_FAULT);

    /* A readable signal resolves through the bus; a signal outside the read
     * allowlist is an unauthorized access, counted as a capability denial. */
    CANCESSTRY_TEST_CHECK(fsm_test_init(&fx, expr_guard_document("sig.ClusterSpeed == 12")));
    CANCESSTRY_TEST_CHECK_U64(fsm_test_start(&fx, "lab.one"), CANCESTRY_FSM_OK);
    fsm_test_set_int(&fx, "ClusterSpeed", 12);
    fsm_test_event_int(&event, 2000u, "Trigger", 1);
    CANCESSTRY_TEST_CHECK_U64(fsm_test_process(&fx, &event), CANCESTRY_FSM_OK);
    CANCESSTRY_TEST_CHECK_STRING(fsm_test_state(&fx, "lab.one"), "TAKEN");
    fsm_test_destroy(&fx);

    {
        /* SpeedMPH is writable but not readable under the shrunken allowlist, so the
         * read is refused before the bus is consulted (SW-FR-FSM-039, -041). */
        fsm_test_options_t options = fsm_test_options_default();

        options.signal_read_count = 2u;
        CANCESSTRY_TEST_CHECK(
            fsm_test_init_with(&fx, expr_guard_document("sig.SpeedMPH == 1"), &options));
    }
    CANCESSTRY_TEST_CHECK_U64(fsm_test_start(&fx, "lab.one"), CANCESTRY_FSM_OK);
    fsm_test_set_int(&fx, "SpeedMPH", 1);
    fsm_test_event_int(&event, 2000u, "Trigger", 1);
    (void)fsm_test_process(&fx, &event);
    CANCESSTRY_TEST_CHECK_U64(fsm_test_lifecycle(&fx, "lab.one"),
                              CANCESTRY_FSM_INSTANCE_LIFECYCLE_SUSPENDED);
    CANCESSTRY_TEST_CHECK(fsm_test_counters(&fx, "lab.one")->capability_denials >= 1u);
    fsm_test_destroy(&fx);
}

/* EXPR-FUNCTIONS-001: the five built-ins and their domains (section 7). */
static void case_functions(void)
{
    CANCESSTRY_TEST_CASE("EXPR-FUNCTIONS-001 built-in functions");
    expr_check_guard("min(3, 4) == 3", EXPR_TAKEN);
    expr_check_guard("max(3, 4) == 4", EXPR_TAKEN);
    expr_check_guard("min(3.5, 4) == 3.5", EXPR_TAKEN);
    expr_check_guard("abs(-3) == 3", EXPR_TAKEN);
    expr_check_guard("abs(3) == 3", EXPR_TAKEN);
    expr_check_guard("clamp(10, 0, 5) == 5", EXPR_TAKEN);
    expr_check_guard("clamp(-1, 0, 5) == 0", EXPR_TAKEN);
    expr_check_guard("clamp(3, 0, 5) == 3", EXPR_TAKEN);
    /* round returns an integer; ties go away from zero, as everywhere else. */
    expr_check_guard("round(2.5) == 3", EXPR_TAKEN);
    expr_check_guard("round(-2.5) == -3", EXPR_TAKEN);
    expr_check_guard("round(2.4) == 2", EXPR_TAKEN);
    expr_check_guard("round(2) == 2", EXPR_TAKEN);
    /* Domains: clamp needs low <= high, and a round result must fit an integer. */
    expr_check_guard("clamp(2, 5, 1) == 2", EXPR_FAULT);
    expr_check_guard("round(9223372036854775808.0) == 0", EXPR_FAULT);
    expr_check_guard("round(1e30) == 0", EXPR_FAULT);
    /* Arity is checked at load time, so a wrong call never reaches evaluation. */
    expr_check_guard("min(1) == 1", EXPR_REJECTED);
    expr_check_guard("clamp(1, 2) == 1", EXPR_REJECTED);
    expr_check_guard("abs(1, 2) == 1", EXPR_REJECTED);
    /* Functions nest, and take namespaced arguments. */
    expr_check_guard("max(min(1, 2), var.n) == 4", EXPR_TAKEN);
    expr_check_guard("abs(var.f - var.n) < 2", EXPR_TAKEN);
    /* A function over a float domain still promotes. */
    expr_check_guard("clamp(2.5, 0, 5) == 2.5", EXPR_TAKEN);
}

/* EXPR-DEPTH-001: bounded nesting, no unbounded work or stack growth. */
static void case_depth(void)
{
    static char deep[EXPR_MAX_TEXT];
    static char shallow[EXPR_MAX_TEXT];
    size_t i;
    size_t position = 0u;

    CANCESSTRY_TEST_CASE("EXPR-DEPTH-001 bounded depth");
    /* A handful of nesting levels is fine. */
    position = 0u;
    for (i = 0u; i < 4u; ++i) {
        deep[position++] = '(';
    }
    position += (size_t)snprintf(deep + position, sizeof(deep) - position, "1 == 1");
    for (i = 0u; i < 4u; ++i) {
        deep[position++] = ')';
    }
    deep[position] = '\0';
    expr_check_guard(deep, EXPR_TAKEN);

    /* Deep nesting is refused at load time, not at evaluation time. */
    position = 0u;
    for (i = 0u; i < 60u && position + 8u < sizeof(shallow); ++i) {
        shallow[position++] = '(';
    }
    position += (size_t)snprintf(shallow + position, sizeof(shallow) - position, "1 == 1");
    for (i = 0u; i < 60u && position + 1u < sizeof(shallow); ++i) {
        shallow[position++] = ')';
    }
    shallow[position] = '\0';
    expr_check_guard(shallow, EXPR_REJECTED);

    /* Long text is refused as well, so the parser's work is bounded twice over. */
    position = 0u;
    while (position + 32u < EXPR_MAX_TEXT - 4u) {
        (void)memcpy(shallow + position, "var.n + var.n + ", 14u);
        position += 14u;
    }
    (void)snprintf(shallow + position, sizeof(shallow) - position, "== 0");
    expr_check_guard(shallow, EXPR_REJECTED);
}

/* EXPR-ACTIONS-001: the same evaluator runs value operands. */
static void case_operands(void)
{
    static fsm_test_fixture_t fx;
    cancestry_event_t event;
    int64_t value = -1;
    char document[4096];

    CANCESSTRY_TEST_CASE("EXPR-ACTIONS-001 value operands");
    (void)snprintf(document, sizeof(document),
                   "schema_version: \"0.2.0\"\n"
                   "state_machines:\n"
                   "  - name: lab\n"
                   "    initial: S0\n"
                   "    variables:\n"
                   "      - name: n\n"
                   "        type: integer\n"
                   "        default: 4\n"
                   "      - name: f\n"
                   "        type: float\n"
                   "        default: 2.5\n"
                   "      - name: b\n"
                   "        type: boolean\n"
                   "        default: true\n"
                   "    states:\n"
                   "      - name: S0\n"
                   "        transitions:\n"
                   "          - event: signal_changed\n"
                   "            signal: Add\n"
                   "            target: S0\n"
                   "            actions:\n"
                   "              - set_variable:\n"
                   "                  variable: n\n"
                   "                  value: \"var.n + 1\"\n"
                   "          - event: signal_changed\n"
                   "            signal: Round\n"
                   "            target: S0\n"
                   "            actions:\n"
                   "              - set_variable:\n"
                   "                  variable: n\n"
                   "                  value: \"var.f\"\n"
                   "          - event: signal_changed\n"
                   "            signal: WrongType\n"
                   "            target: S0\n"
                   "            actions:\n"
                   "              - set_variable:\n"
                   "                  variable: b\n"
                   "                  value: 3\n"
                   "          - event: signal_changed\n"
                   "            signal: Fault\n"
                   "            target: S0\n"
                   "            actions:\n"
                   "              - set_variable:\n"
                   "                  variable: n\n"
                   "                  value: \"1 / 0\"\n"
                   "          - event: signal_changed\n"
                   "            signal: Write\n"
                   "            target: S0\n"
                   "            actions:\n"
                   "              - set_signal:\n"
                   "                  signal: ClusterSpeed\n"
                   "                  value: \"var.n * 3\"\n"
                   "instances:\n"
                   "  - id: lab.one\n"
                   "    machine: lab\n"
                   "    enabled: true\n");
    CANCESSTRY_TEST_CHECK(fsm_test_init(&fx, document));
    CANCESSTRY_TEST_CHECK_U64(fsm_test_start(&fx, "lab.one"), CANCESTRY_FSM_OK);

    /* Literal and expression operands both reach the variable. */
    fsm_test_event_int(&event, 1000u, "Add", 1);
    (void)fsm_test_process(&fx, &event);
    CANCESSTRY_TEST_CHECK_I64(fsm_test_variable_int(&fx, "lab.one", "n", &value),
                              CANCESTRY_FSM_OK);
    CANCESSTRY_TEST_CHECK_I64(value, 5);

    /* A float into an integer variable is rounded half away from zero, the same
     * rule the codec and recipe engines apply: 2.5 becomes 3, not 2. */
    fx.line_count = 0u;
    cancestry_event_init(&event);
    fsm_test_event_int(&event, 1000u, "Round", 1);
    (void)fsm_test_process(&fx, &event);
    CANCESSTRY_TEST_CHECK_I64(fsm_test_variable_int(&fx, "lab.one", "n", &value),
                              CANCESTRY_FSM_OK);
    CANCESSTRY_TEST_CHECK_I64(value, 3);

    /* A number into a boolean variable is refused as an action error, and the
     * instance keeps running: the value was evaluated, only the assignment is
     * impossible (SW-FR-FSM-024). */
    cancestry_event_init(&event);
    fsm_test_event_int(&event, 1000u, "WrongType", 1);
    (void)fsm_test_process(&fx, &event);
    CANCESSTRY_TEST_CHECK_U64(fsm_test_counters(&fx, "lab.one")->action_errors, 1u);
    CANCESSTRY_TEST_CHECK_U64(fsm_test_lifecycle(&fx, "lab.one"),
                              CANCESTRY_FSM_INSTANCE_LIFECYCLE_RUNNING);

    /* A value operand that faults follows the guard rule: the instance stops. */
    cancestry_event_init(&event);
    fsm_test_event_int(&event, 1000u, "Fault", 1);
    (void)fsm_test_process(&fx, &event);
    CANCESSTRY_TEST_CHECK_U64(fsm_test_lifecycle(&fx, "lab.one"),
                              CANCESTRY_FSM_INSTANCE_LIFECYCLE_SUSPENDED);
    CANCESSTRY_TEST_CHECK(fsm_test_has_line(&fx, "state 1 S0->S0"));
    fsm_test_destroy(&fx);

    /* A signal write takes the same expression language. */
    CANCESSTRY_TEST_CHECK(fsm_test_init(&fx, document));
    CANCESSTRY_TEST_CHECK_U64(fsm_test_start(&fx, "lab.one"), CANCESTRY_FSM_OK);
    cancestry_event_init(&event);
    fsm_test_event_int(&event, 1000u, "Write", 1);
    (void)fsm_test_process(&fx, &event);
    CANCESSTRY_TEST_CHECK_I64(fsm_test_get_int(&fx, "ClusterSpeed"), 12);
    CANCESSTRY_TEST_CHECK(fsm_test_has_line(&fx, "signal 1 ClusterSpeed unset->12"));
    fsm_test_destroy(&fx);
}

/* EXPR-NO-CODE-001: nothing in the language reaches outside it. */
static void case_no_code(void)
{
    CANCESSTRY_TEST_CASE("EXPR-NO-CODE-001 no arbitrary code execution");
    /* Only the five built-ins are callable; everything else is a definition
     * error, so there is no hook to reach (SW-FR-FSM-037). */
    expr_check_guard("system(0) == 0", EXPR_REJECTED);
    expr_check_guard("exec(1) == 1", EXPR_REJECTED);
    expr_check_guard("printf(1) == 1", EXPR_REJECTED);
    expr_check_guard("eval(\"1\") == 1", EXPR_REJECTED);
    /* No dereference, no member access beyond the namespace prefix, no pointer
     * syntax, no statement separator, no comment smuggling. */
    expr_check_guard("var.n->value == 4", EXPR_REJECTED);
    expr_check_guard("*var.n == 4", EXPR_REJECTED);
    expr_check_guard("var.n == 4 /* comment */", EXPR_REJECTED);
    expr_check_guard("var.n == 4 # 1", EXPR_REJECTED);
    /* A namespaced name is not callable either. */
    expr_check_guard("sig.n(1) == 1", EXPR_REJECTED);
    /* Strings are not part of the value language. */
    expr_check_guard("'abc' == 'abc'", EXPR_REJECTED);
}

int main(void)
{
    CANCESSTRY_TEST_SUITE_BEGIN("fsm/expression_evaluation");
    case_grammar();
    case_types();
    case_faults();
    case_namespaces();
    case_functions();
    case_depth();
    case_operands();
    case_no_code();
    return CANCESSTRY_TEST_SUITE_END();
}
