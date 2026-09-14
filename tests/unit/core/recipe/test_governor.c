/*
 * Unit tests for the recipe engine's safety governor integration.
 *
 * Verifies:
 *   SW-FR-RECIPE-003  Governor denial fails the action and follows the
 *                     recipe's on_error policy.
 *   SW-FR-GOV-005     Violation counters are exposed.
 *   SW-FR-GOV-006     The governor fails closed (no governor configured
 *                     means every side effect is denied).
 *   docs/system/governor.md (v0.2.1 stub level): a denial produces no side
 *   effect and increments the violation counter.
 *
 * Traceability: RECIPE-GOV-DENY-001, RECIPE-GOV-FAIL-CLOSED-001
 *
 * Normative source: docs/system/governor.md (stub level),
 *                   docs/packages/recipe-spec.md section 6.
 */

#include "cancestry/recipe/engine.h"

#include "cancestry_recipe_test.h"

#include <stdio.h>
#include <string.h>

#define ID_VEHICLE_SPEED ((cancestry_signal_id_t)1u)

/* ------------------------------------------------------------------------- */
/* Test environment with an injectable governor                              */
/* ------------------------------------------------------------------------- */

typedef struct gov_env {
    cancestry_codec_map_t *map;
    const cancestry_codec_map_t *ns_slots[4];
    cancestry_codec_namespace_t ns;
    cancestry_recipe_set_t *set;
    cancestry_recipe_signal_store_t store;
    cancestry_recipe_signal_slot_t store_slots[16];
    cancestry_event_queue_t queue;
    cancestry_event_t queue_slots[32];
    cancestry_test_trace_t trace;
    cancestry_recipe_sink_t sink;
    cancestry_recipe_variable_t variables[8];
    cancestry_recipe_engine_t engine;
} gov_env_t;

typedef struct recording_governor {
    cancestry_recipe_governor_decision_t decision;
    uint32_t calls;
    cancestry_recipe_governor_request_t last_request;
    /* Deep copy of last_request.value: request pointers are only valid
     * during the callback itself. */
    cancestry_value_t last_value;
} recording_governor_t;

static cancestry_recipe_governor_decision_t recording_governor_fn(
    void *user_data,
    const cancestry_recipe_governor_request_t *request)
{
    recording_governor_t *governor = (recording_governor_t *)user_data;

    governor->calls++;
    governor->last_request = *request;
    if (request->value != NULL) {
        governor->last_value = *request->value;
        governor->last_request.value = &governor->last_value;
    } else {
        governor->last_request.value = NULL;
    }
    return governor->decision;
}

/**
 * Build an environment around one inline recipe file. When @p governor is
 * NULL the engine is configured without any governor (fail-closed mode);
 * otherwise the recording governor with @p governor's state is installed.
 */
static void gov_env_init(gov_env_t *env,
                         const char *recipe_yaml,
                         recording_governor_t *governor)
{
    cancestry_recipe_engine_config_t config;

    memset(env, 0, sizeof(*env));
    env->map = cancestry_test_load_map(cancestry_test_recipe_demo_codec_yaml);
    CANCESSTRY_TEST_CHECK(cancestry_codec_namespace_init(&env->ns, env->ns_slots, 4u));
    CANCESSTRY_TEST_CHECK(cancestry_codec_namespace_register(&env->ns, env->map) ==
                          CANCESTRY_CODEC_OK);
    CANCESSTRY_TEST_CHECK(cancestry_recipe_signal_store_init(&env->store, env->store_slots, 16u));
    CANCESSTRY_TEST_CHECK(cancestry_event_queue_init(&env->queue, env->queue_slots, 32u));
    cancestry_test_sink_record(&env->sink, &env->trace);
    env->set = cancestry_test_recipe_load(recipe_yaml);
    if (env->set == NULL) {
        cancestry_codec_map_free(env->map);
        env->map = NULL;
        return;
    }

    memset(&config, 0, sizeof(config));
    config.sets = env->set;
    config.set_count = 1u;
    config.signal_namespace = &env->ns;
    config.interfaces = cancestry_test_interfaces;
    config.interface_count = 2u;
    config.bindings = cancestry_test_bindings;
    config.binding_count = 2u;
    config.timers = cancestry_test_timers;
    config.timer_count = 1u;
    config.signal_store = &env->store;
    config.event_queue = &env->queue;
    config.sink = &env->sink;
    if (governor != NULL) {
        config.governor = recording_governor_fn;
        config.governor_user_data = governor;
    }
    config.variable_storage = env->variables;
    config.variable_capacity = 8u;
    CANCESSTRY_TEST_CHECK(cancestry_recipe_engine_init(&env->engine, &config));
}

static void gov_env_free(gov_env_t *env)
{
    cancestry_recipe_set_free(env->set);
    env->set = NULL;
    if (env->map != NULL) {
        cancestry_codec_map_free(env->map);
        env->map = NULL;
    }
}

/* ------------------------------------------------------------------------- */
/* Tests                                                                     */
/* ------------------------------------------------------------------------- */

static void test_deny_send_message(void)
{
    recording_governor_t governor;
    gov_env_t env;
    cancestry_event_t event;

    CANCESSTRY_TEST_CASE("RECIPE-GOV-DENY-001a: denied send_message has no side effect");
    memset(&governor, 0, sizeof(governor));
    governor.decision = CANCESTRY_RECIPE_GOVERNOR_DENY;
    gov_env_init(&env, "schema_version: \"0.2.0\"\n"
                       "recipes:\n"
                       "  - name: forward\n"
                       "    trigger:\n"
                       "      event: can_rx\n"
                       "    actions:\n"
                       "      - send_message:\n"
                       "          interface: car\n"
                       "          message: ClusterMsg\n"
                       "          signals:\n"
                       "            ClusterSpeed: 100\n"
                       "      - log:\n"
                       "          level: info\n"
                       "          message: never runs\n",
                &governor);
    if (env.set == NULL) {
        return;
    }
    event = cancestry_test_can_rx_event(TEST_MSG_STATUS, TEST_IFACE_CAN1, 100u, 1u);
    CANCESSTRY_TEST_CHECK(cancestry_recipe_engine_process_event(&env.engine, &event) ==
                          CANCESTRY_RECIPE_OK);
    /* The governor was consulted exactly once. */
    CANCESSTRY_TEST_CHECK_U64(governor.calls, 1u);
    /* No TX, and the default on_error policy halted the recipe. */
    CANCESSTRY_TEST_CHECK_U64(env.trace.count, 0u);
    CANCESSTRY_TEST_CHECK_U64(env.engine.counters.messages_sent, 0u);
    CANCESSTRY_TEST_CHECK_U64(env.engine.counters.governor_denials, 1u);
    CANCESSTRY_TEST_CHECK_U64(env.engine.counters.action_errors, 1u);
    CANCESSTRY_TEST_CHECK_U64(env.engine.counters.recipes_halted, 1u);
    gov_env_free(&env);
}

static void test_deny_set_signal(void)
{
    recording_governor_t governor;
    gov_env_t env;
    cancestry_event_t event;
    cancestry_value_t value;

    CANCESSTRY_TEST_CASE("RECIPE-GOV-DENY-001b: denied set_signal leaves the store untouched");
    memset(&governor, 0, sizeof(governor));
    governor.decision = CANCESTRY_RECIPE_GOVERNOR_DENY;
    gov_env_init(&env, "schema_version: \"0.2.0\"\n"
                       "recipes:\n"
                       "  - name: publish\n"
                       "    trigger:\n"
                       "      event: can_rx\n"
                       "    actions:\n"
                       "      - set_signal:\n"
                       "          signal: ClusterSpeed\n"
                       "          value: 300\n",
                &governor);
    if (env.set == NULL) {
        return;
    }
    event = cancestry_test_can_rx_event(TEST_MSG_STATUS, TEST_IFACE_CAN1, 100u, 1u);
    CANCESSTRY_TEST_CHECK(cancestry_recipe_engine_process_event(&env.engine, &event) ==
                          CANCESTRY_RECIPE_OK);
    CANCESSTRY_TEST_CHECK_U64(governor.calls, 1u);
    CANCESSTRY_TEST_CHECK_U64(env.trace.count, 0u);
    CANCESSTRY_TEST_CHECK_U64(cancestry_event_queue_size(&env.queue), 0u);
    CANCESSTRY_TEST_CHECK(cancestry_recipe_signal_store_get(&env.store, 3u, &value) ==
                          CANCESTRY_RECIPE_ERR_NOT_FOUND);
    CANCESSTRY_TEST_CHECK_U64(env.engine.counters.signals_set, 0u);
    CANCESSTRY_TEST_CHECK_U64(env.engine.counters.governor_denials, 1u);
    gov_env_free(&env);
}

static void test_approve_allows_effects(void)
{
    recording_governor_t governor;
    gov_env_t env;
    cancestry_event_t event;

    CANCESSTRY_TEST_CASE("RECIPE-GOV-DENY-001c: approval allows the side effect");
    memset(&governor, 0, sizeof(governor));
    governor.decision = CANCESTRY_RECIPE_GOVERNOR_APPROVE;
    gov_env_init(&env, "schema_version: \"0.2.0\"\n"
                       "recipes:\n"
                       "  - name: forward\n"
                       "    trigger:\n"
                       "      event: can_rx\n"
                       "    actions:\n"
                       "      - send_message:\n"
                       "          interface: car\n"
                       "          message: ClusterMsg\n"
                       "          signals:\n"
                       "            ClusterSpeed: 100\n"
                       "      - set_signal:\n"
                       "          signal: ClusterSpeed\n"
                       "          value: 100\n",
                &governor);
    if (env.set == NULL) {
        return;
    }
    event = cancestry_test_can_rx_event(TEST_MSG_STATUS, TEST_IFACE_CAN1, 100u, 1u);
    CANCESSTRY_TEST_CHECK(cancestry_recipe_engine_process_event(&env.engine, &event) ==
                          CANCESTRY_RECIPE_OK);
    CANCESSTRY_TEST_CHECK_U64(governor.calls, 2u); /* send_message + set_signal */
    CANCESSTRY_TEST_CHECK_U64(env.trace.count, 2u); /* tx + signal notifications */
    CANCESSTRY_TEST_CHECK_U64(env.engine.counters.governor_denials, 0u);
    CANCESSTRY_TEST_CHECK_U64(env.engine.counters.action_errors, 0u);
    CANCESSTRY_TEST_CHECK_U64(env.engine.counters.messages_sent, 1u);
    CANCESSTRY_TEST_CHECK_U64(env.engine.counters.signals_set, 1u);
    gov_env_free(&env);
}

static void test_deny_with_on_error_continue(void)
{
    recording_governor_t governor;
    gov_env_t env;
    cancestry_event_t event;

    CANCESSTRY_TEST_CASE("RECIPE-GOV-DENY-001d: denial with on_error: continue proceeds");
    memset(&governor, 0, sizeof(governor));
    governor.decision = CANCESTRY_RECIPE_GOVERNOR_DENY;
    gov_env_init(&env, "schema_version: \"0.2.0\"\n"
                       "recipes:\n"
                       "  - name: resilient\n"
                       "    trigger:\n"
                       "      event: can_rx\n"
                       "    on_error: continue\n"
                       "    actions:\n"
                       "      - send_message:\n"
                       "          interface: car\n"
                       "          message: ClusterMsg\n"
                       "          signals:\n"
                       "            ClusterSpeed: 100\n"
                       "      - log:\n"
                       "          level: warning\n"
                       "          message: tx denied, continuing\n",
                &governor);
    if (env.set == NULL) {
        return;
    }
    event = cancestry_test_can_rx_event(TEST_MSG_STATUS, TEST_IFACE_CAN1, 100u, 1u);
    CANCESSTRY_TEST_CHECK(cancestry_recipe_engine_process_event(&env.engine, &event) ==
                          CANCESTRY_RECIPE_OK);
    /* No TX, but the log action still ran. */
    CANCESSTRY_TEST_CHECK_U64(env.trace.count, 1u);
    CANCESSTRY_TEST_CHECK_STRING(env.trace.entries[0].kind, "log");
    CANCESSTRY_TEST_CHECK_U64(env.engine.counters.governor_denials, 1u);
    CANCESSTRY_TEST_CHECK_U64(env.engine.counters.action_errors, 1u);
    CANCESSTRY_TEST_CHECK_U64(env.engine.counters.recipes_halted, 0u);
    gov_env_free(&env);
}

static void test_fail_closed_without_governor(void)
{
    gov_env_t env;
    cancestry_event_t event;

    CANCESSTRY_TEST_CASE("RECIPE-GOV-FAIL-CLOSED-001: no governor means everything is denied "
                         "(SW-FR-GOV-006)");
    gov_env_init(&env, "schema_version: \"0.2.0\"\n"
                       "recipes:\n"
                       "  - name: forward\n"
                       "    trigger:\n"
                       "      event: can_rx\n"
                       "    on_error: continue\n"
                       "    actions:\n"
                       "      - send_message:\n"
                       "          interface: car\n"
                       "          message: ClusterMsg\n"
                       "          signals:\n"
                       "            ClusterSpeed: 100\n"
                       "      - set_signal:\n"
                       "          signal: ClusterSpeed\n"
                       "          value: 100\n"
                       "  - name: bystander\n"
                       "    trigger:\n"
                       "      event: can_rx\n"
                       "    actions:\n"
                       "      - log:\n"
                       "          level: info\n"
                       "          message: unaffected\n",
                NULL);
    if (env.set == NULL) {
        return;
    }
    event = cancestry_test_can_rx_event(TEST_MSG_STATUS, TEST_IFACE_CAN1, 100u, 1u);
    CANCESSTRY_TEST_CHECK(cancestry_recipe_engine_process_event(&env.engine, &event) ==
                          CANCESTRY_RECIPE_OK);
    /* Both side-effect actions were denied... */
    CANCESSTRY_TEST_CHECK_U64(env.engine.counters.governor_denials, 2u);
    CANCESSTRY_TEST_CHECK_U64(env.engine.counters.messages_sent, 0u);
    CANCESSTRY_TEST_CHECK_U64(env.engine.counters.signals_set, 0u);
    CANCESSTRY_TEST_CHECK_U64(env.trace.count, 1u);
    /* ...but actions without side effects (log) are unaffected. */
    CANCESSTRY_TEST_CHECK_STRING(env.trace.entries[0].message, "unaffected");
    CANCESSTRY_TEST_CHECK_U64(env.engine.counters.logs_emitted, 1u);
    gov_env_free(&env);
}

static void test_governor_request_contents(void)
{
    recording_governor_t governor;
    gov_env_t env;
    cancestry_event_t event;

    CANCESSTRY_TEST_CASE("RECIPE-GOV-DENY-001e: requests carry the resolved side effect");
    memset(&governor, 0, sizeof(governor));
    governor.decision = CANCESTRY_RECIPE_GOVERNOR_APPROVE;
    gov_env_init(&env, "schema_version: \"0.2.0\"\n"
                       "recipes:\n"
                       "  - name: forward\n"
                       "    trigger:\n"
                       "      event: can_rx\n"
                       "    actions:\n"
                       "      - send_message:\n"
                       "          interface: car\n"
                       "          message: demo.ClusterMsg\n"
                       "          signals:\n"
                       "            ClusterSpeed: 4660\n"
                       "      - set_signal:\n"
                       "          signal: VehicleSpeed\n"
                       "          value: 4660\n",
                &governor);
    if (env.set == NULL) {
        return;
    }
    event = cancestry_test_can_rx_event(TEST_MSG_STATUS, TEST_IFACE_CAN1, 700u, 5u);
    CANCESSTRY_TEST_CHECK(cancestry_recipe_engine_process_event(&env.engine, &event) ==
                          CANCESTRY_RECIPE_OK);
    CANCESSTRY_TEST_CHECK_U64(governor.calls, 2u);

    /* The set_signal request (the last one) carries name, id and value. */
    CANCESSTRY_TEST_CHECK(governor.last_request.kind == CANCESTRY_RECIPE_GOVERNOR_SET_SIGNAL);
    CANCESSTRY_TEST_CHECK_STRING(governor.last_request.signal_name, "VehicleSpeed");
    CANCESSTRY_TEST_CHECK_U64(governor.last_request.signal_id, ID_VEHICLE_SPEED);
    CANCESSTRY_TEST_CHECK(governor.last_request.value != NULL);
    CANCESSTRY_TEST_CHECK(governor.last_request.value->kind == CANCESTRY_VALUE_KIND_INT);
    CANCESSTRY_TEST_CHECK_I64(governor.last_request.value->value.integer, 4660);
    CANCESSTRY_TEST_CHECK(governor.last_request.recipe == &env.set->recipes[0]);
    CANCESSTRY_TEST_CHECK(governor.last_request.cause == &event);
    gov_env_free(&env);
}

static void test_violations_accumulate(void)
{
    recording_governor_t governor;
    gov_env_t env;
    cancestry_event_t event;
    uint32_t i;

    CANCESSTRY_TEST_CASE("RECIPE-GOV-DENY-001f: violation counter accumulates (SW-FR-GOV-005)");
    memset(&governor, 0, sizeof(governor));
    governor.decision = CANCESTRY_RECIPE_GOVERNOR_DENY;
    gov_env_init(&env, "schema_version: \"0.2.0\"\n"
                       "recipes:\n"
                       "  - name: forward\n"
                       "    trigger:\n"
                       "      event: can_rx\n"
                       "    on_error: continue\n"
                       "    actions:\n"
                       "      - send_message:\n"
                       "          interface: car\n"
                       "          message: ClusterMsg\n"
                       "          signals:\n"
                       "            ClusterSpeed: 1\n"
                       "      - set_signal:\n"
                       "          signal: ClusterSpeed\n"
                       "          value: 2\n",
                &governor);
    if (env.set == NULL) {
        return;
    }
    for (i = 0u; i < 3u; ++i) {
        event = cancestry_test_can_rx_event(TEST_MSG_STATUS, TEST_IFACE_CAN1, 100u + (uint64_t)i,
                                            i + 1u);
        CANCESSTRY_TEST_CHECK(cancestry_recipe_engine_process_event(&env.engine, &event) ==
                              CANCESTRY_RECIPE_OK);
    }
    /* Two denied side effects per event, three events. */
    CANCESSTRY_TEST_CHECK_U64(governor.calls, 6u);
    CANCESSTRY_TEST_CHECK_U64(env.engine.counters.governor_denials, 6u);
    CANCESSTRY_TEST_CHECK_U64(env.engine.counters.action_errors, 6u);
    CANCESSTRY_TEST_CHECK(cancestry_recipe_engine_counters(&env.engine) !=
                         NULL);
    CANCESSTRY_TEST_CHECK_U64(cancestry_recipe_engine_counters(&env.engine)->governor_denials,
                              6u);
    gov_env_free(&env);
}

int main(void)
{
    CANCESSTRY_TEST_SUITE_BEGIN("recipe governor");
    test_deny_send_message();
    test_deny_set_signal();
    test_approve_allows_effects();
    test_deny_with_on_error_continue();
    test_fail_closed_without_governor();
    test_governor_request_contents();
    test_violations_accumulate();
    return CANCESSTRY_TEST_SUITE_END();
}
