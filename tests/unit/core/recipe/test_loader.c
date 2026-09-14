/*
 * Unit tests for the recipe file loader.
 *
 * Verifies:
 *   SW-FR-RECIPE-006  Recipes that use the transition action are rejected.
 *   SYS-NF-001        Deterministic parse/validation.
 *   SYS-NF-008        Every schema constraint of
 *                     schemas/recipe-0.2.0.schema.json is enforced.
 *
 * Traceability: RECIPE-NO-TRANSITION-001, RECIPE-SCHEMA-EVENT-001,
 *               RECIPE-CONFLICT-001
 *
 * Normative source: schemas/recipe-0.2.0.schema.json,
 *                   docs/packages/recipe-spec.md.
 */

#include "cancestry/recipe/loader.h"

#include "cancestry_recipe_test.h"

#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------------- */
/* Fixtures                                                                  */
/* ------------------------------------------------------------------------- */

static const char *const minimal_yaml =
    "schema_version: \"0.2.0\"\n"
    "recipes:\n"
    "  - name: forward_status\n"
    "    trigger:\n"
    "      event: can_rx\n"
    "    actions:\n"
    "      - log:\n"
    "          level: info\n"
    "          message: hello\n";

static const char *const full_yaml =
    "---\n"
    "# leading comment\n"
    "schema_version: \"0.2.0\"  # trailing comment\n"
    "recipes:\n"
    "  - name: gateway\n"
    "    description: 'forward and adapt'\n"
    "    enabled: true\n"
    "    trigger:\n"
    "      event: can_rx\n"
    "      interface: device\n"
    "      message: \"demo.StatusMsg\"\n"
    "    conditions:\n"
    "      - expression: sig.VehicleSpeed > 50\n"
    "      - expression: not (sig.IgnitionState == 0) and evt.can_id != 2047\n"
    "    actions:\n"
    "      - send_message:\n"
    "          interface: car\n"
    "          message: ClusterMsg\n"
    "          signals:\n"
    "            ClusterSpeed: sig.VehicleSpeed\n"
    "            \"ExtraValue\": 0.5\n"
    "      - set_signal:\n"
    "          signal: demo.ClusterSpeed\n"
    "          value: var.speed_out + 1\n"
    "      - set_variable:\n"
    "          variable: hits\n"
    "          value: var.hits + 1\n"
    "      - start_timer:\n"
    "          timer: tick\n"
    "          duration_ms: 250\n"
    "          repeat: true\n"
    "      - stop_timer:\n"
    "          timer: tick\n"
    "      - reset_timer:\n"
    "          timer: tock\n"
    "      - log:\n"
    "          level: warning\n"
    "          message: \"it's late\"\n"
    "      - raise_fault:\n"
    "          code: CLUSTER_TIMEOUT\n"
    "          severity: critical\n"
    "    on_error: continue\n"
    "  - name: disabled_watch\n"
    "    enabled: false\n"
    "    trigger:\n"
    "      event: signal_changed\n"
    "      signal: VehicleSpeed\n"
    "    actions:\n"
    "      - set_variable:\n"
    "          variable: seen\n"
    "          value: true\n";

/* A recipe using the forbidden transition action (SW-FR-RECIPE-006). */
static const char *const transition_yaml =
    "schema_version: \"0.2.0\"\n"
    "recipes:\n"
    "  - name: illegal\n"
    "    trigger:\n"
    "      event: power_mode_changed\n"
    "    actions:\n"
    "      - transition: RUNNING\n";

/** Load @p yaml and expect failure; returns true when the load failed. */
static bool expect_load_failure(const char *yaml,
                                cancestry_recipe_status_t expected_status,
                                const char *expected_substring)
{
    cancestry_recipe_load_error_t error;
    cancestry_recipe_set_t *set = cancestry_recipe_set_load(yaml, strlen(yaml), &error);

    if (set != NULL) {
        printf("    FAIL expected loader rejection, but the document loaded\n");
        fflush(stdout);
        cancestry_test_failures++;
        cancestry_recipe_set_free(set);
        return false;
    }
    if (error.status != expected_status) {
        printf("    FAIL expected status %s, got %s (%s)\n",
               cancestry_recipe_status_name(expected_status),
               cancestry_recipe_status_name(error.status), error.message);
        fflush(stdout);
        cancestry_test_failures++;
        return false;
    }
    if (expected_substring != NULL && strstr(error.message, expected_substring) == NULL) {
        printf("    FAIL expected message to contain \"%s\", got \"%s\"\n", expected_substring,
               error.message);
        fflush(stdout);
        cancestry_test_failures++;
        return false;
    }
    return true;
}

/* ------------------------------------------------------------------------- */
/* Tests                                                                     */
/* ------------------------------------------------------------------------- */

static void test_minimal_document(void)
{
    cancestry_recipe_load_error_t error;
    cancestry_recipe_set_t *set =
        cancestry_recipe_set_load(minimal_yaml, strlen(minimal_yaml), &error);

    CANCESSTRY_TEST_CASE("RECIPE-SCHEMA-LOAD-001: minimal document parses with defaults");
    CANCESSTRY_TEST_CHECK(set != NULL);
    if (set == NULL) {
        printf("    error: %s (line %u, column %u)\n", error.message, (unsigned)error.line,
               (unsigned)error.column);
        return;
    }
    CANCESSTRY_TEST_CHECK_U64(set->recipe_count, 1u);
    CANCESSTRY_TEST_CHECK_STRING(set->recipes[0].name, "forward_status");
    CANCESSTRY_TEST_CHECK(set->recipes[0].description == NULL);
    /* Schema defaults: enabled is true and on_error is stop. */
    CANCESSTRY_TEST_CHECK(set->recipes[0].enabled);
    CANCESSTRY_TEST_CHECK(set->recipes[0].on_error == CANCESTRY_RECIPE_ON_ERROR_STOP);
    CANCESSTRY_TEST_CHECK(set->recipes[0].trigger.event == CANCESTRY_RECIPE_TRIGGER_CAN_RX);
    CANCESSTRY_TEST_CHECK(set->recipes[0].trigger.interface == NULL);
    CANCESSTRY_TEST_CHECK(set->recipes[0].trigger.message == NULL);
    CANCESSTRY_TEST_CHECK(set->recipes[0].condition_count == 0u);
    CANCESSTRY_TEST_CHECK(set->recipes[0].conditions == NULL);
    CANCESSTRY_TEST_CHECK_U64(set->recipes[0].action_count, 1u);
    CANCESSTRY_TEST_CHECK(set->recipes[0].actions[0].kind == CANCESTRY_RECIPE_ACTION_LOG);
    CANCESSTRY_TEST_CHECK(set->recipes[0].actions[0].as.log.level == CANCESTRY_RECIPE_LOG_INFO);
    CANCESSTRY_TEST_CHECK_STRING(set->recipes[0].actions[0].as.log.message, "hello");
    cancestry_recipe_set_free(set);
}

static void test_full_document(void)
{
    cancestry_recipe_load_error_t error;
    cancestry_recipe_set_t *set = cancestry_recipe_set_load(full_yaml, strlen(full_yaml), &error);
    const cancestry_recipe_t *first;
    const cancestry_recipe_t *second;

    CANCESSTRY_TEST_CASE("RECIPE-SCHEMA-LOAD-002: full document with every action kind");
    CANCESSTRY_TEST_CHECK(set != NULL);
    if (set == NULL) {
        printf("    error: %s\n", error.message);
        return;
    }
    CANCESSTRY_TEST_CHECK_U64(set->recipe_count, 2u);
    first = &set->recipes[0];
    second = &set->recipes[1];

    CANCESSTRY_TEST_CHECK_STRING(first->name, "gateway");
    CANCESSTRY_TEST_CHECK_STRING(first->description, "forward and adapt");
    CANCESSTRY_TEST_CHECK(first->enabled);
    CANCESSTRY_TEST_CHECK(first->on_error == CANCESTRY_RECIPE_ON_ERROR_CONTINUE);
    CANCESSTRY_TEST_CHECK(first->trigger.event == CANCESTRY_RECIPE_TRIGGER_CAN_RX);
    CANCESSTRY_TEST_CHECK_STRING(first->trigger.interface, "device");
    CANCESSTRY_TEST_CHECK_STRING(first->trigger.message, "demo.StatusMsg");

    CANCESSTRY_TEST_CHECK_U64(first->condition_count, 2u);
    CANCESSTRY_TEST_CHECK_STRING(first->conditions[0].expression, "sig.VehicleSpeed > 50");
    CANCESSTRY_TEST_CHECK_STRING(first->conditions[1].expression,
                                 "not (sig.IgnitionState == 0) and evt.can_id != 2047");

    CANCESSTRY_TEST_CHECK_U64(first->action_count, 8u);
    CANCESSTRY_TEST_CHECK(first->actions[0].kind == CANCESTRY_RECIPE_ACTION_SEND_MESSAGE);
    CANCESSTRY_TEST_CHECK_STRING(first->actions[0].as.send_message.interface, "car");
    CANCESSTRY_TEST_CHECK_STRING(first->actions[0].as.send_message.message, "ClusterMsg");
    CANCESSTRY_TEST_CHECK_U64(first->actions[0].as.send_message.value_count, 2u);
    CANCESSTRY_TEST_CHECK_STRING(first->actions[0].as.send_message.values[0].name, "ClusterSpeed");
    /* A string operand is an expression, preserved verbatim. */
    CANCESSTRY_TEST_CHECK(first->actions[0].as.send_message.values[0].operand.is_expression);
    CANCESSTRY_TEST_CHECK_STRING(first->actions[0].as.send_message.values[0].operand.expression,
                                 "sig.VehicleSpeed");
    /* A float operand is a REAL literal. */
    CANCESSTRY_TEST_CHECK_STRING(first->actions[0].as.send_message.values[1].name, "ExtraValue");
    CANCESSTRY_TEST_CHECK(!first->actions[0].as.send_message.values[1].operand.is_expression);
    CANCESSTRY_TEST_CHECK(first->actions[0].as.send_message.values[1].operand.literal.kind ==
                          CANCESTRY_VALUE_KIND_REAL);
    CANCESSTRY_TEST_CHECK_DOUBLE(
        first->actions[0].as.send_message.values[1].operand.literal.value.real, 0.5, 1e-9);

    CANCESSTRY_TEST_CHECK(first->actions[1].kind == CANCESTRY_RECIPE_ACTION_SET_SIGNAL);
    CANCESSTRY_TEST_CHECK_STRING(first->actions[1].as.set_signal.signal, "demo.ClusterSpeed");
    CANCESSTRY_TEST_CHECK(first->actions[1].as.set_signal.operand.is_expression);

    CANCESSTRY_TEST_CHECK(first->actions[2].kind == CANCESTRY_RECIPE_ACTION_SET_VARIABLE);
    CANCESSTRY_TEST_CHECK_STRING(first->actions[2].as.set_variable.variable, "hits");
    CANCESSTRY_TEST_CHECK(first->actions[2].as.set_variable.operand.is_expression);
    CANCESSTRY_TEST_CHECK_STRING(first->actions[2].as.set_variable.operand.expression,
                                 "var.hits + 1");

    CANCESSTRY_TEST_CHECK(first->actions[3].kind == CANCESTRY_RECIPE_ACTION_START_TIMER);
    CANCESSTRY_TEST_CHECK_STRING(first->actions[3].as.start_timer.timer, "tick");
    CANCESSTRY_TEST_CHECK_U64(first->actions[3].as.start_timer.duration_ms, 250u);
    CANCESSTRY_TEST_CHECK(first->actions[3].as.start_timer.has_duration_ms);
    CANCESSTRY_TEST_CHECK(first->actions[3].as.start_timer.repeat);

    CANCESSTRY_TEST_CHECK(first->actions[4].kind == CANCESTRY_RECIPE_ACTION_STOP_TIMER);
    CANCESSTRY_TEST_CHECK_STRING(first->actions[4].as.timer.timer, "tick");

    CANCESSTRY_TEST_CHECK(first->actions[5].kind == CANCESTRY_RECIPE_ACTION_RESET_TIMER);
    CANCESSTRY_TEST_CHECK_STRING(first->actions[5].as.timer.timer, "tock");

    CANCESSTRY_TEST_CHECK(first->actions[6].kind == CANCESTRY_RECIPE_ACTION_LOG);
    CANCESSTRY_TEST_CHECK(first->actions[6].as.log.level == CANCESTRY_RECIPE_LOG_WARNING);
    CANCESSTRY_TEST_CHECK_STRING(first->actions[6].as.log.message, "it's late");

    CANCESSTRY_TEST_CHECK(first->actions[7].kind == CANCESTRY_RECIPE_ACTION_RAISE_FAULT);
    CANCESSTRY_TEST_CHECK_STRING(first->actions[7].as.raise_fault.code, "CLUSTER_TIMEOUT");
    CANCESSTRY_TEST_CHECK(first->actions[7].as.raise_fault.severity ==
                          CANCESTRY_FAULT_SEVERITY_CRITICAL);

    CANCESSTRY_TEST_CHECK_STRING(second->name, "disabled_watch");
    CANCESSTRY_TEST_CHECK(!second->enabled);
    CANCESSTRY_TEST_CHECK(second->trigger.event == CANCESTRY_RECIPE_TRIGGER_SIGNAL_CHANGED);
    CANCESSTRY_TEST_CHECK_STRING(second->trigger.signal, "VehicleSpeed");
    CANCESSTRY_TEST_CHECK(second->actions[0].kind == CANCESTRY_RECIPE_ACTION_SET_VARIABLE);
    CANCESSTRY_TEST_CHECK(!second->actions[0].as.set_variable.operand.is_expression);
    CANCESSTRY_TEST_CHECK(second->actions[0].as.set_variable.operand.literal.kind ==
                          CANCESTRY_VALUE_KIND_BOOL);
    CANCESSTRY_TEST_CHECK(second->actions[0].as.set_variable.operand.literal.value.boolean);

    cancestry_recipe_set_free(set);
}

static void test_operand_literal_kinds(void)
{
    const char *yaml =
        "schema_version: \"0.2.0\"\n"
        "recipes:\n"
        "  - name: literals\n"
        "    trigger:\n"
        "      event: can_rx\n"
        "    actions:\n"
        "      - set_variable:\n"
        "          variable: a\n"
        "          value: 42\n"
        "      - set_variable:\n"
        "          variable: b\n"
        "          value: -7\n"
        "      - set_variable:\n"
        "          variable: c\n"
        "          value: 1.5\n"
        "      - set_variable:\n"
        "          variable: d\n"
        "          value: false\n"
        "      - set_variable:\n"
        "          variable: e\n"
        "          value: \"0x10\"\n"
        "      - set_variable:\n"
        "          variable: f\n"
        "          value: 99999999999999999999\n";
    cancestry_recipe_set_t *set = cancestry_test_recipe_load(yaml);
    const cancestry_recipe_t *recipe;

    CANCESSTRY_TEST_CASE("RECIPE-SCHEMA-LOAD-003: operand literal kinds");
    CANCESSTRY_TEST_CHECK(set != NULL);
    if (set == NULL) {
        return;
    }
    recipe = &set->recipes[0];
    CANCESSTRY_TEST_CHECK_U64(recipe->action_count, 6u);
    CANCESSTRY_TEST_CHECK(recipe->actions[0].as.set_variable.operand.literal.kind ==
                          CANCESTRY_VALUE_KIND_INT);
    CANCESSTRY_TEST_CHECK_I64(recipe->actions[0].as.set_variable.operand.literal.value.integer, 42);
    CANCESSTRY_TEST_CHECK(recipe->actions[1].as.set_variable.operand.literal.kind ==
                          CANCESTRY_VALUE_KIND_INT);
    CANCESSTRY_TEST_CHECK_I64(recipe->actions[1].as.set_variable.operand.literal.value.integer, -7);
    CANCESSTRY_TEST_CHECK(recipe->actions[2].as.set_variable.operand.literal.kind ==
                          CANCESTRY_VALUE_KIND_REAL);
    CANCESSTRY_TEST_CHECK_DOUBLE(recipe->actions[2].as.set_variable.operand.literal.value.real,
                                 1.5, 1e-9);
    CANCESSTRY_TEST_CHECK(recipe->actions[3].as.set_variable.operand.literal.kind ==
                          CANCESTRY_VALUE_KIND_BOOL);
    CANCESSTRY_TEST_CHECK(!recipe->actions[3].as.set_variable.operand.literal.value.boolean);
    /* Quoted scalars are strings, i.e. expressions, even when they look numeric. */
    CANCESSTRY_TEST_CHECK(recipe->actions[4].as.set_variable.operand.is_expression);
    CANCESSTRY_TEST_CHECK_STRING(recipe->actions[4].as.set_variable.operand.expression, "0x10");
    /* Integers beyond int64 degrade to REAL literals rather than being rejected. */
    CANCESSTRY_TEST_CHECK(recipe->actions[5].as.set_variable.operand.literal.kind ==
                          CANCESTRY_VALUE_KIND_REAL);
    cancestry_recipe_set_free(set);
}

static void test_transition_rejected(void)
{
    CANCESSTRY_TEST_CASE("RECIPE-NO-TRANSITION-001: transition action is rejected");
    CANCESSTRY_TEST_CHECK(expect_load_failure(transition_yaml, CANCESTRY_RECIPE_ERR_PARSE,
                                              "transition"));
    CANCESSTRY_TEST_CHECK(expect_load_failure(transition_yaml, CANCESTRY_RECIPE_ERR_PARSE,
                                              "SW-FR-RECIPE-006"));
}

static void test_trigger_type_field_rejected(void)
{
    const char *yaml =
        "schema_version: \"0.2.0\"\n"
        "recipes:\n"
        "  - name: typed\n"
        "    trigger:\n"
        "      type: message\n"
        "      event: can_rx\n"
        "    actions:\n"
        "      - log:\n"
        "          level: info\n"
        "          message: hi\n";

    CANCESSTRY_TEST_CASE("RECIPE-SCHEMA-LOAD-004: trigger 'type' field is rejected");
    CANCESSTRY_TEST_CHECK(expect_load_failure(yaml, CANCESTRY_RECIPE_ERR_PARSE,
                                              "'type' is not allowed"));
}

static void test_trigger_event_enum_and_conditionals(void)
{
    const char *bad_event =
        "schema_version: \"0.2.0\"\n"
        "recipes:\n"
        "  - name: r\n"
        "    trigger:\n"
        "      event: can_tx\n"
        "    actions:\n"
        "      - log:\n"
        "          level: info\n"
        "          message: hi\n";
    const char *signal_changed_without_signal =
        "schema_version: \"0.2.0\"\n"
        "recipes:\n"
        "  - name: r\n"
        "    trigger:\n"
        "      event: signal_changed\n"
        "    actions:\n"
        "      - log:\n"
        "          level: info\n"
        "          message: hi\n";
    const char *timer_expired_without_timer =
        "schema_version: \"0.2.0\"\n"
        "recipes:\n"
        "  - name: r\n"
        "    trigger:\n"
        "      event: timer_expired\n"
        "    actions:\n"
        "      - log:\n"
        "          level: info\n"
        "          message: hi\n";
    const char *timeout_without_ms =
        "schema_version: \"0.2.0\"\n"
        "recipes:\n"
        "  - name: r\n"
        "    trigger:\n"
        "      event: timeout\n"
        "    actions:\n"
        "      - log:\n"
        "          level: info\n"
        "          message: hi\n";
    const char *timeout_zero_ms =
        "schema_version: \"0.2.0\"\n"
        "recipes:\n"
        "  - name: r\n"
        "    trigger:\n"
        "      event: timeout\n"
        "      timeout_ms: 0\n"
        "    actions:\n"
        "      - log:\n"
        "          level: info\n"
        "          message: hi\n";
    const char *timeout_ok =
        "schema_version: \"0.2.0\"\n"
        "recipes:\n"
        "  - name: r\n"
        "    trigger:\n"
        "      event: timeout\n"
        "      timeout_ms: 500\n"
        "    actions:\n"
        "      - log:\n"
        "          level: info\n"
        "          message: hi\n";

    CANCESSTRY_TEST_CASE("RECIPE-SCHEMA-EVENT-001: trigger event rules (schema trigger allOf)");
    CANCESSTRY_TEST_CHECK(expect_load_failure(bad_event, CANCESTRY_RECIPE_ERR_PARSE, "event"));
    CANCESSTRY_TEST_CHECK(expect_load_failure(signal_changed_without_signal,
                                              CANCESTRY_RECIPE_ERR_PARSE, "signal"));
    CANCESSTRY_TEST_CHECK(expect_load_failure(timer_expired_without_timer,
                                              CANCESTRY_RECIPE_ERR_PARSE, "timer"));
    CANCESSTRY_TEST_CHECK(expect_load_failure(timeout_without_ms, CANCESTRY_RECIPE_ERR_PARSE,
                                              "timeout_ms"));
    CANCESSTRY_TEST_CHECK(expect_load_failure(timeout_zero_ms, CANCESTRY_RECIPE_ERR_PARSE,
                                              "out of range"));
    {
        cancestry_recipe_set_t *set = cancestry_test_recipe_load(timeout_ok);
        CANCESSTRY_TEST_CHECK(set != NULL);
        if (set != NULL) {
            CANCESSTRY_TEST_CHECK(set->recipes[0].trigger.event == CANCESTRY_RECIPE_TRIGGER_TIMEOUT);
            CANCESSTRY_TEST_CHECK_U64(set->recipes[0].trigger.timeout_ms, 500u);
            CANCESSTRY_TEST_CHECK(set->recipes[0].trigger.has_timeout_ms);
            cancestry_recipe_set_free(set);
        }
    }
}

static void test_unknown_fields(void)
{
    const char *unknown_root =
        "schema_version: \"0.2.0\"\n"
        "extra: 1\n"
        "recipes:\n"
        "  - name: r\n"
        "    trigger:\n"
        "      event: can_rx\n"
        "    actions:\n"
        "      - log:\n"
        "          level: info\n"
        "          message: hi\n";
    const char *unknown_recipe =
        "schema_version: \"0.2.0\"\n"
        "recipes:\n"
        "  - name: r\n"
        "    priority: high\n"
        "    trigger:\n"
        "      event: can_rx\n"
        "    actions:\n"
        "      - log:\n"
        "          level: info\n"
        "          message: hi\n";
    const char *unknown_trigger =
        "schema_version: \"0.2.0\"\n"
        "recipes:\n"
        "  - name: r\n"
        "    trigger:\n"
        "      event: can_rx\n"
        "      source: bus\n"
        "    actions:\n"
        "      - log:\n"
        "          level: info\n"
        "          message: hi\n";
    const char *unknown_condition =
        "schema_version: \"0.2.0\"\n"
        "recipes:\n"
        "  - name: r\n"
        "    trigger:\n"
        "      event: can_rx\n"
        "    conditions:\n"
        "      - expression: sig.A > 1\n"
        "        tag: main\n"
        "    actions:\n"
        "      - log:\n"
        "          level: info\n"
        "          message: hi\n";
    const char *unknown_send_field =
        "schema_version: \"0.2.0\"\n"
        "recipes:\n"
        "  - name: r\n"
        "    trigger:\n"
        "      event: can_rx\n"
        "    actions:\n"
        "      - send_message:\n"
        "          interface: can0\n"
        "          message: M\n"
        "          signals:\n"
        "            A: 1\n"
        "          count: 3\n";
    const char *unknown_action =
        "schema_version: \"0.2.0\"\n"
        "recipes:\n"
        "  - name: r\n"
        "    trigger:\n"
        "      event: can_rx\n"
        "    actions:\n"
        "      - teleport:\n"
        "          where: home\n";

    CANCESSTRY_TEST_CASE("RECIPE-SCHEMA-LOAD-005: unknown fields are rejected everywhere");
    CANCESSTRY_TEST_CHECK(expect_load_failure(unknown_root, CANCESTRY_RECIPE_ERR_PARSE,
                                              "unknown field"));
    CANCESSTRY_TEST_CHECK(expect_load_failure(unknown_recipe, CANCESTRY_RECIPE_ERR_PARSE,
                                              "unknown field"));
    CANCESSTRY_TEST_CHECK(expect_load_failure(unknown_trigger, CANCESTRY_RECIPE_ERR_PARSE,
                                              "unknown field"));
    CANCESSTRY_TEST_CHECK(expect_load_failure(unknown_condition, CANCESTRY_RECIPE_ERR_PARSE,
                                              "unknown field"));
    CANCESSTRY_TEST_CHECK(expect_load_failure(unknown_send_field, CANCESTRY_RECIPE_ERR_PARSE,
                                              "unknown field"));
    CANCESSTRY_TEST_CHECK(expect_load_failure(unknown_action, CANCESTRY_RECIPE_ERR_PARSE,
                                              "unknown action 'teleport'"));
}

static void test_action_shape_rules(void)
{
    const char *two_keys =
        "schema_version: \"0.2.0\"\n"
        "recipes:\n"
        "  - name: r\n"
        "    trigger:\n"
        "      event: can_rx\n"
        "    actions:\n"
        "      - log:\n"
        "          level: info\n"
        "          message: hi\n"
        "        set_variable:\n"
        "          variable: x\n"
        "          value: 1\n";
    const char *send_missing_signals =
        "schema_version: \"0.2.0\"\n"
        "recipes:\n"
        "  - name: r\n"
        "    trigger:\n"
        "      event: can_rx\n"
        "    actions:\n"
        "      - send_message:\n"
        "          interface: can0\n"
        "          message: M\n";
    const char *send_empty_signals =
        "schema_version: \"0.2.0\"\n"
        "recipes:\n"
        "  - name: r\n"
        "    trigger:\n"
        "      event: can_rx\n"
        "    actions:\n"
        "      - send_message:\n"
        "          interface: can0\n"
        "          message: M\n"
        "          signals:\n";
    const char *set_signal_missing_value =
        "schema_version: \"0.2.0\"\n"
        "recipes:\n"
        "  - name: r\n"
        "    trigger:\n"
        "      event: can_rx\n"
        "    actions:\n"
        "      - set_signal:\n"
        "          signal: A\n";
    const char *log_bad_level =
        "schema_version: \"0.2.0\"\n"
        "recipes:\n"
        "  - name: r\n"
        "    trigger:\n"
        "      event: can_rx\n"
        "    actions:\n"
        "      - log:\n"
        "          level: debug\n"
        "          message: hi\n";
    const char *fault_bad_severity =
        "schema_version: \"0.2.0\"\n"
        "recipes:\n"
        "  - name: r\n"
        "    trigger:\n"
        "      event: can_rx\n"
        "    actions:\n"
        "      - raise_fault:\n"
        "          code: X\n"
        "          severity: fatal\n";
    const char *timer_zero_duration =
        "schema_version: \"0.2.0\"\n"
        "recipes:\n"
        "  - name: r\n"
        "    trigger:\n"
        "      event: can_rx\n"
        "    actions:\n"
        "      - start_timer:\n"
        "          timer: t\n"
        "          duration_ms: 0\n";
    const char *bad_on_error =
        "schema_version: \"0.2.0\"\n"
        "recipes:\n"
        "  - name: r\n"
        "    trigger:\n"
        "      event: can_rx\n"
        "    actions:\n"
        "      - log:\n"
        "          level: info\n"
        "          message: hi\n"
        "    on_error: abort\n";

    CANCESSTRY_TEST_CASE("RECIPE-SCHEMA-LOAD-006: action shape rules");
    CANCESSTRY_TEST_CHECK(
        expect_load_failure(two_keys, CANCESTRY_RECIPE_ERR_PARSE, "exactly one key"));
    CANCESSTRY_TEST_CHECK(
        expect_load_failure(send_missing_signals, CANCESTRY_RECIPE_ERR_PARSE, "signals"));
    CANCESSTRY_TEST_CHECK(
        expect_load_failure(send_empty_signals, CANCESTRY_RECIPE_ERR_PARSE, "non-empty"));
    CANCESSTRY_TEST_CHECK(
        expect_load_failure(set_signal_missing_value, CANCESTRY_RECIPE_ERR_PARSE, "value"));
    CANCESSTRY_TEST_CHECK(expect_load_failure(log_bad_level, CANCESTRY_RECIPE_ERR_PARSE, "level"));
    CANCESSTRY_TEST_CHECK(
        expect_load_failure(fault_bad_severity, CANCESTRY_RECIPE_ERR_PARSE, "severity"));
    CANCESSTRY_TEST_CHECK(
        expect_load_failure(timer_zero_duration, CANCESTRY_RECIPE_ERR_PARSE, "out of range"));
    CANCESSTRY_TEST_CHECK(expect_load_failure(bad_on_error, CANCESTRY_RECIPE_ERR_PARSE,
                                              "on_error"));
}

static void test_missing_required_fields(void)
{
    const char *no_version =
        "recipes:\n"
        "  - name: r\n"
        "    trigger:\n"
        "      event: can_rx\n"
        "    actions:\n"
        "      - log:\n"
        "          level: info\n"
        "          message: hi\n";
    const char *bad_version =
        "schema_version: \"0.3.0\"\n"
        "recipes:\n"
        "  - name: r\n"
        "    trigger:\n"
        "      event: can_rx\n"
        "    actions:\n"
        "      - log:\n"
        "          level: info\n"
        "          message: hi\n";
    const char *no_recipes = "schema_version: \"0.2.0\"\n";
    const char *recipe_no_trigger =
        "schema_version: \"0.2.0\"\n"
        "recipes:\n"
        "  - name: r\n"
        "    actions:\n"
        "      - log:\n"
        "          level: info\n"
        "          message: hi\n";
    const char *recipe_no_actions =
        "schema_version: \"0.2.0\"\n"
        "recipes:\n"
        "  - name: r\n"
        "    trigger:\n"
        "      event: can_rx\n";
    const char *empty_actions =
        "schema_version: \"0.2.0\"\n"
        "recipes:\n"
        "  - name: r\n"
        "    trigger:\n"
        "      event: can_rx\n"
        "    actions:\n";

    CANCESSTRY_TEST_CASE("RECIPE-SCHEMA-LOAD-007: required fields");
    CANCESSTRY_TEST_CHECK(expect_load_failure(no_version, CANCESTRY_RECIPE_ERR_PARSE, NULL));
    CANCESSTRY_TEST_CHECK(
        expect_load_failure(bad_version, CANCESTRY_RECIPE_ERR_PARSE, "schema_version"));
    CANCESSTRY_TEST_CHECK(expect_load_failure(no_recipes, CANCESTRY_RECIPE_ERR_PARSE, "recipes"));
    CANCESSTRY_TEST_CHECK(expect_load_failure(recipe_no_trigger, CANCESTRY_RECIPE_ERR_PARSE,
                                              "trigger"));
    CANCESSTRY_TEST_CHECK(
        expect_load_failure(recipe_no_actions, CANCESTRY_RECIPE_ERR_PARSE, "actions"));
    CANCESSTRY_TEST_CHECK(
        expect_load_failure(empty_actions, CANCESTRY_RECIPE_ERR_PARSE, "non-empty"));
}

static void test_duplicate_recipe_names(void)
{
    const char *yaml =
        "schema_version: \"0.2.0\"\n"
        "recipes:\n"
        "  - name: same\n"
        "    trigger:\n"
        "      event: can_rx\n"
        "    actions:\n"
        "      - log:\n"
        "          level: info\n"
        "          message: one\n"
        "  - name: other\n"
        "    trigger:\n"
        "      event: can_rx\n"
        "    actions:\n"
        "      - log:\n"
        "          level: info\n"
        "          message: two\n"
        "  - name: same\n"
        "    trigger:\n"
        "      event: can_rx\n"
        "    actions:\n"
        "      - log:\n"
        "          level: info\n"
        "          message: three\n";

    CANCESSTRY_TEST_CASE("RECIPE-CONFLICT-001: duplicate recipe names are a conflict");
    CANCESSTRY_TEST_CHECK(
        expect_load_failure(yaml, CANCESTRY_RECIPE_ERR_CONFLICT, "duplicate recipe name"));
}

static void test_yaml_subset_rejections(void)
{
    const char *tab_indent =
        "schema_version: \"0.2.0\"\n"
        "recipes:\n"
        "\t- name: r\n";
    const char *duplicate_key =
        "schema_version: \"0.2.0\"\n"
        "recipes:\n"
        "  - name: r\n"
        "    name: r2\n"
        "    trigger:\n"
        "      event: can_rx\n"
        "    actions:\n"
        "      - log:\n"
        "          level: info\n"
        "          message: hi\n";
    const char *unterminated =
        "schema_version: \"0.2.0\n"
        "recipes:\n";
    const char *empty = "";
    const char *ok_prefix = "schema_version: \"0.2.0\"\nrecipes:\n";
    cancestry_recipe_load_error_t error;

    CANCESSTRY_TEST_CASE("RECIPE-SCHEMA-LOAD-008: YAML subset rejections");
    CANCESSTRY_TEST_CHECK(
        expect_load_failure(tab_indent, CANCESTRY_RECIPE_ERR_PARSE, "tab characters"));
    CANCESSTRY_TEST_CHECK(
        expect_load_failure(duplicate_key, CANCESTRY_RECIPE_ERR_PARSE, "duplicate mapping key"));
    CANCESSTRY_TEST_CHECK(
        expect_load_failure(unterminated, CANCESTRY_RECIPE_ERR_PARSE, "unterminated"));
    CANCESSTRY_TEST_CHECK(expect_load_failure(empty, CANCESTRY_RECIPE_ERR_PARSE, "empty"));
    CANCESSTRY_TEST_CHECK(cancestry_recipe_set_load(NULL, 1u, &error) == NULL);
    CANCESSTRY_TEST_CHECK(error.status == CANCESTRY_RECIPE_ERR_NULL);
    CANCESSTRY_TEST_CHECK(cancestry_recipe_set_load(ok_prefix, strlen(ok_prefix) + 1u, &error) ==
                          NULL);
    CANCESSTRY_TEST_CHECK(error.status == CANCESTRY_RECIPE_ERR_PARSE);
}

static void test_error_position_reporting(void)
{
    const char *yaml =
        "schema_version: \"0.2.0\"\n"     /* line 1 */
        "recipes:\n"                       /* line 2 */
        "  - name: r\n"                    /* line 3 */
        "    bogus: 1\n"                   /* line 4 */
        "    trigger:\n"
        "      event: can_rx\n"
        "    actions:\n"
        "      - log:\n"
        "          level: info\n"
        "          message: hi\n";
    cancestry_recipe_load_error_t error;
    cancestry_recipe_set_t *set = cancestry_recipe_set_load(yaml, strlen(yaml), &error);

    CANCESSTRY_TEST_CASE("RECIPE-SCHEMA-LOAD-009: errors carry a source position");
    CANCESSTRY_TEST_CHECK(set == NULL);
    CANCESSTRY_TEST_CHECK(error.status == CANCESTRY_RECIPE_ERR_PARSE);
    CANCESSTRY_TEST_CHECK_U64(error.line, 4u);
    CANCESSTRY_TEST_CHECK_U64(error.column, 5u);
}

static void test_string_type_strictness(void)
{
    /* Unquoted scalars that parse as numbers are not strings. */
    const char *numeric_name =
        "schema_version: \"0.2.0\"\n"
        "recipes:\n"
        "  - name: 123\n"
        "    trigger:\n"
        "      event: can_rx\n"
        "    actions:\n"
        "      - log:\n"
        "          level: info\n"
        "          message: hi\n";
    const char *long_name =
        "schema_version: \"0.2.0\"\n"
        "recipes:\n"
        "  - name: aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\n"
        "    trigger:\n"
        "      event: can_rx\n"
        "    actions:\n"
        "      - log:\n"
        "          level: info\n"
        "          message: hi\n";

    CANCESSTRY_TEST_CASE("RECIPE-SCHEMA-LOAD-010: string fields and bounds");
    CANCESSTRY_TEST_CHECK(
        expect_load_failure(numeric_name, CANCESTRY_RECIPE_ERR_PARSE, "non-empty string"));
    CANCESSTRY_TEST_CHECK(expect_load_failure(long_name, CANCESTRY_RECIPE_ERR_PARSE, "too long"));
}

static void test_helper_names(void)
{
    CANCESSTRY_TEST_CASE("RECIPE-SCHEMA-LOAD-011: name helpers");
    CANCESSTRY_TEST_CHECK_STRING(cancestry_recipe_status_name(CANCESTRY_RECIPE_ERR_DENIED),
                                 "ERR_DENIED");
    CANCESSTRY_TEST_CHECK_STRING(
        cancestry_recipe_trigger_event_name(CANCESTRY_RECIPE_TRIGGER_SIGNAL_CHANGED),
        "signal_changed");
    CANCESSTRY_TEST_CHECK(cancestry_recipe_trigger_event_type(CANCESTRY_RECIPE_TRIGGER_CAN_RX) ==
                          CANCESTRY_EVENT_TYPE_CAN_RX);
    CANCESSTRY_TEST_CHECK(
        cancestry_recipe_trigger_event_type(CANCESTRY_RECIPE_TRIGGER_TIMEOUT) ==
        CANCESTRY_EVENT_TYPE_INVALID);
    CANCESSTRY_TEST_CHECK_STRING(cancestry_recipe_action_kind_name(CANCESTRY_RECIPE_ACTION_LOG),
                                 "log");
    CANCESSTRY_TEST_CHECK_STRING(cancestry_recipe_on_error_name(CANCESTRY_RECIPE_ON_ERROR_STOP),
                                 "stop");
    CANCESSTRY_TEST_CHECK_STRING(cancestry_recipe_log_level_name(CANCESTRY_RECIPE_LOG_WARNING),
                                 "warning");
    CANCESSTRY_TEST_CHECK(cancestry_recipe_status_is_ok(CANCESTRY_RECIPE_OK));
    CANCESSTRY_TEST_CHECK(!cancestry_recipe_status_is_ok(CANCESTRY_RECIPE_ERR_PARSE));
}

int main(void)
{
    CANCESSTRY_TEST_SUITE_BEGIN("recipe loader");
    test_minimal_document();
    test_full_document();
    test_operand_literal_kinds();
    test_transition_rejected();
    test_trigger_type_field_rejected();
    test_trigger_event_enum_and_conditionals();
    test_unknown_fields();
    test_action_shape_rules();
    test_missing_required_fields();
    test_duplicate_recipe_names();
    test_yaml_subset_rejections();
    test_error_position_reporting();
    test_string_type_strictness();
    test_helper_names();
    return CANCESSTRY_TEST_SUITE_END();
}
