/*
 * Unit tests for the recipe execution engine.
 *
 * Verifies:
 *   SW-FR-RECIPE-001  Recipes execute triggered by events, in the normative
 *                     dispatch order (package load order, file order,
 *                     definition order).
 *   SW-FR-RECIPE-002  Conditions on signals and messages (minimal v0.3.0
 *                     expression evaluator subset).
 *   SW-FR-RECIPE-003  send_message, set_signal, set_variable, start_timer,
 *                     stop_timer, reset_timer, log and raise_fault actions.
 *   SW-FR-RECIPE-005  Directional filtering for gateway behavior.
 *   SW-FR-RECIPE-007  Interface resolution via physical names and bindings.
 *   SYS-NF-001        Same event sequence -> same action sequence.
 *
 * Traceability: RECIPE-EXEC-001, RECIPE-COND-001, RECIPE-ACTION-001,
 *               RECIPE-FILTER-001, RECIPE-IFACE-BINDING-001,
 *               RECIPE-DETERMINISM-001
 *               (RECIPE-NO-ALLOC-001 is the cancestry_recipe_no_malloc_symbols
 *               CTest symbol scan registered in tests/CMakeLists.txt.)
 *
 * Normative source: docs/packages/recipe-spec.md,
 *                   docs/system/event-ordering.md sections 7-8,
 *                   docs/system/expression-language.md.
 */

#include "cancestry/recipe/engine.h"

#include "cancestry_recipe_test.h"

#include <stdio.h>
#include <string.h>

/* Demo namespace ids, assigned in registration order. */
#define ID_VEHICLE_SPEED ((cancestry_signal_id_t)1u)
#define ID_IGNITION ((cancestry_signal_id_t)2u)
#define ID_CLUSTER_SPEED ((cancestry_signal_id_t)3u)

/* ------------------------------------------------------------------------- */
/* Test environment                                                          */
/* ------------------------------------------------------------------------- */

typedef struct engine_env {
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
} engine_env_t;

/** Governor state shared by the recording governor below. */
typedef struct test_governor {
    cancestry_recipe_governor_decision_t decision;
    uint32_t calls;
    cancestry_recipe_governor_request_t last_request;
    uint8_t last_frame[CANCESTRY_CAN_FRAME_MAX_LENGTH];
} test_governor_t;

static test_governor_t governor_state;

static cancestry_recipe_governor_decision_t test_governor_fn(
    void *user_data,
    const cancestry_recipe_governor_request_t *request)
{
    test_governor_t *state = (test_governor_t *)user_data;

    state->calls++;
    state->last_request = *request;
    memset(state->last_frame, 0, sizeof(state->last_frame));
    if (request->frame != NULL && request->frame_length > 0u) {
        memcpy(state->last_frame, request->frame, request->frame_length);
    }
    return state->decision;
}

/** Approving governor used by the engine tests. */
static cancestry_recipe_governor_decision_t approve_all(
    void *user_data,
    const cancestry_recipe_governor_request_t *request)
{
    return test_governor_fn(user_data, request);
}

/**
 * Build a full environment around one inline recipe file. The governor
 * approves everything (see test_governor.c for denials).
 */
static void env_init(engine_env_t *env, const char *recipe_yaml)
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

    memset(&governor_state, 0, sizeof(governor_state));
    governor_state.decision = CANCESTRY_RECIPE_GOVERNOR_APPROVE;

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
    config.governor = approve_all;
    config.governor_user_data = &governor_state;
    config.variable_storage = env->variables;
    config.variable_capacity = 8u;
    CANCESSTRY_TEST_CHECK(cancestry_recipe_engine_init(&env->engine, &config));
}

static void env_free(engine_env_t *env)
{
    cancestry_recipe_set_free(env->set);
    env->set = NULL;
    if (env->map != NULL) {
        cancestry_codec_map_free(env->map);
        env->map = NULL;
    }
}

/* ------------------------------------------------------------------------- */
/* Trigger matching and dispatch order                                       */
/* ------------------------------------------------------------------------- */

static void test_can_rx_trigger_basic(void)
{
    engine_env_t env;
    cancestry_event_t event;

    CANCESSTRY_TEST_CASE("RECIPE-EXEC-001: can_rx trigger invokes the recipe");
    env_init(&env, "schema_version: \"0.2.0\"\n"
                   "recipes:\n"
                   "  - name: watch\n"
                   "    trigger:\n"
                   "      event: can_rx\n"
                   "    actions:\n"
                   "      - log:\n"
                   "          level: info\n"
                   "          message: saw a frame\n");
    if (env.set == NULL) {
        return;
    }
    event = cancestry_test_can_rx_event(TEST_MSG_STATUS, TEST_IFACE_CAN0, 1000u, 1u);
    CANCESSTRY_TEST_CHECK(cancestry_recipe_engine_process_event(&env.engine, &event) ==
                          CANCESTRY_RECIPE_OK);
    CANCESSTRY_TEST_CHECK_U64(env.trace.count, 1u);
    CANCESSTRY_TEST_CHECK_STRING(env.trace.entries[0].kind, "log");
    CANCESSTRY_TEST_CHECK_STRING(env.trace.entries[0].message, "saw a frame");
    CANCESSTRY_TEST_CHECK(env.trace.entries[0].level == CANCESTRY_RECIPE_LOG_INFO);
    CANCESSTRY_TEST_CHECK_STRING(env.trace.entries[0].recipe->name, "watch");
    CANCESSTRY_TEST_CHECK_U64(env.engine.counters.events_processed, 1u);
    CANCESSTRY_TEST_CHECK_U64(env.engine.counters.recipes_invoked, 1u);
    CANCESSTRY_TEST_CHECK_U64(env.engine.counters.actions_executed, 1u);
    CANCESSTRY_TEST_CHECK_U64(env.engine.counters.logs_emitted, 1u);
    env_free(&env);
}

static void test_dispatch_order(void)
{
    engine_env_t env;
    cancestry_recipe_set_t *set_two_raw;
    cancestry_recipe_set_t sets[2];
    cancestry_recipe_engine_config_t config;
    cancestry_event_t event;

    CANCESSTRY_TEST_CASE("RECIPE-EXEC-002: set order then definition order (SYS-NF-001)");
    env_init(&env, "schema_version: \"0.2.0\"\n"
                   "recipes:\n"
                   "  - name: first\n"
                   "    trigger:\n"
                   "      event: can_rx\n"
                   "    actions:\n"
                   "      - log:\n"
                   "          level: info\n"
                   "          message: one\n"
                   "  - name: second\n"
                   "    trigger:\n"
                   "      event: can_rx\n"
                   "    actions:\n"
                   "      - log:\n"
                   "          level: info\n"
                   "          message: two\n");
    if (env.set == NULL) {
        return;
    }
    /* A second set takes dispatch priority after the first (file order). */
    set_two_raw = cancestry_test_recipe_load("schema_version: \"0.2.0\"\n"
                                             "recipes:\n"
                                             "  - name: third\n"
                                             "    trigger:\n"
                                             "      event: can_rx\n"
                                             "    actions:\n"
                                             "      - log:\n"
                                             "          level: info\n"
                                             "          message: three\n");
    if (set_two_raw == NULL) {
        env_free(&env);
        return;
    }
    sets[0] = *env.set;
    sets[1] = *set_two_raw;
    memset(&config, 0, sizeof(config));
    config.sets = sets;
    config.set_count = 2u;
    config.signal_namespace = &env.ns;
    config.interfaces = cancestry_test_interfaces;
    config.interface_count = 2u;
    config.bindings = cancestry_test_bindings;
    config.binding_count = 2u;
    config.signal_store = &env.store;
    config.event_queue = &env.queue;
    config.sink = &env.sink;
    config.governor = approve_all;
    config.governor_user_data = &governor_state;
    config.variable_storage = env.variables;
    config.variable_capacity = 8u;
    CANCESSTRY_TEST_CHECK(cancestry_recipe_engine_init(&env.engine, &config));

    event = cancestry_test_can_rx_event(TEST_MSG_STATUS, TEST_IFACE_CAN0, 100u, 1u);
    CANCESSTRY_TEST_CHECK(cancestry_recipe_engine_process_event(&env.engine, &event) ==
                          CANCESTRY_RECIPE_OK);
    CANCESSTRY_TEST_CHECK_U64(env.trace.count, 3u);
    if (env.trace.count == 3u) {
        CANCESSTRY_TEST_CHECK_STRING(env.trace.entries[0].message, "one");
        CANCESSTRY_TEST_CHECK_STRING(env.trace.entries[1].message, "two");
        CANCESSTRY_TEST_CHECK_STRING(env.trace.entries[2].message, "three");
        /* Recipe ordinals are 1-based in engine dispatch order. */
        CANCESSTRY_TEST_CHECK_U64(env.trace.entries[0].recipe_ordinal, 1u);
        CANCESSTRY_TEST_CHECK_U64(env.trace.entries[1].recipe_ordinal, 2u);
        CANCESSTRY_TEST_CHECK_U64(env.trace.entries[2].recipe_ordinal, 3u);
    }
    cancestry_recipe_set_free(set_two_raw);
    env_free(&env);
}

static void test_disabled_recipe(void)
{
    engine_env_t env;
    cancestry_event_t event;

    CANCESSTRY_TEST_CASE("RECIPE-EXEC-003: disabled recipes never invoke");
    env_init(&env, "schema_version: \"0.2.0\"\n"
                   "recipes:\n"
                   "  - name: off\n"
                   "    enabled: false\n"
                   "    trigger:\n"
                   "      event: can_rx\n"
                   "    actions:\n"
                   "      - log:\n"
                   "          level: info\n"
                   "          message: nope\n");
    if (env.set == NULL) {
        return;
    }
    event = cancestry_test_can_rx_event(TEST_MSG_STATUS, TEST_IFACE_CAN0, 100u, 1u);
    CANCESSTRY_TEST_CHECK(cancestry_recipe_engine_process_event(&env.engine, &event) ==
                          CANCESTRY_RECIPE_OK);
    CANCESSTRY_TEST_CHECK_U64(env.trace.count, 0u);
    CANCESSTRY_TEST_CHECK_U64(env.engine.counters.recipes_invoked, 0u);
    env_free(&env);
}

static void test_interface_filter(void)
{
    engine_env_t env;
    cancestry_event_t event;

    CANCESSTRY_TEST_CASE("RECIPE-FILTER-001a: trigger interface filter and alias resolution");
    env_init(&env, "schema_version: \"0.2.0\"\n"
                   "recipes:\n"
                   "  - name: from_device\n"
                   "    trigger:\n"
                   "      event: can_rx\n"
                   "      interface: device\n"
                   "    actions:\n"
                   "      - log:\n"
                   "          level: info\n"
                   "          message: device frame\n"
                   "  - name: from_ghost\n"
                   "    trigger:\n"
                   "      event: can_rx\n"
                   "      interface: ghost\n"
                   "    actions:\n"
                   "      - log:\n"
                   "          level: info\n"
                   "          message: never\n");
    if (env.set == NULL) {
        return;
    }
    /* "device" binds to can1 (id 2): a frame on can1 matches... */
    event = cancestry_test_can_rx_event(TEST_MSG_STATUS, TEST_IFACE_CAN1, 100u, 1u);
    CANCESSTRY_TEST_CHECK(cancestry_recipe_engine_process_event(&env.engine, &event) ==
                          CANCESTRY_RECIPE_OK);
    CANCESSTRY_TEST_CHECK_U64(env.trace.count, 1u);
    /* ...a frame on can0 does not. */
    event = cancestry_test_can_rx_event(TEST_MSG_STATUS, TEST_IFACE_CAN0, 200u, 2u);
    CANCESSTRY_TEST_CHECK(cancestry_recipe_engine_process_event(&env.engine, &event) ==
                          CANCESTRY_RECIPE_OK);
    CANCESSTRY_TEST_CHECK_U64(env.trace.count, 1u);
    /* The ghost recipe's interface never resolves: fail closed, counted. */
    event = cancestry_test_can_rx_event(TEST_MSG_STATUS, TEST_IFACE_CAN1, 300u, 3u);
    CANCESSTRY_TEST_CHECK(cancestry_recipe_engine_process_event(&env.engine, &event) ==
                          CANCESTRY_RECIPE_OK);
    CANCESSTRY_TEST_CHECK_U64(env.trace.count, 2u);
    CANCESSTRY_TEST_CHECK_STRING(env.trace.entries[1].message, "device frame");
    /* The ghost trigger fails resolution on every can_rx event (3 so far). */
    CANCESSTRY_TEST_CHECK_U64(env.engine.counters.trigger_unresolved, 3u);
    env_free(&env);
}

static void test_message_filter(void)
{
    engine_env_t env;
    cancestry_event_t event;

    CANCESSTRY_TEST_CASE("RECIPE-FILTER-001b: trigger message filter");
    env_init(&env, "schema_version: \"0.2.0\"\n"
                   "recipes:\n"
                   "  - name: status_only\n"
                   "    trigger:\n"
                   "      event: can_rx\n"
                   "      message: StatusMsg\n"
                   "    actions:\n"
                   "      - log:\n"
                   "          level: info\n"
                   "          message: status\n"
                   "  - name: canonical\n"
                   "    trigger:\n"
                   "      event: can_rx\n"
                   "      message: demo.StatusMsg\n"
                   "    actions:\n"
                   "      - log:\n"
                   "          level: info\n"
                   "          message: canonical\n"
                   "  - name: missing\n"
                   "    trigger:\n"
                   "      event: can_rx\n"
                   "      message: NoSuchMessage\n"
                   "    actions:\n"
                   "      - log:\n"
                   "          level: info\n"
                   "          message: never\n");
    if (env.set == NULL) {
        return;
    }
    event = cancestry_test_can_rx_event(TEST_MSG_STATUS, TEST_IFACE_CAN0, 100u, 1u);
    CANCESSTRY_TEST_CHECK(cancestry_recipe_engine_process_event(&env.engine, &event) ==
                          CANCESTRY_RECIPE_OK);
    /* Short and canonical names both match; the unknown name does not. */
    CANCESSTRY_TEST_CHECK_U64(env.trace.count, 2u);
    event = cancestry_test_can_rx_event(TEST_MSG_CLUSTER, TEST_IFACE_CAN0, 200u, 2u);
    CANCESSTRY_TEST_CHECK(cancestry_recipe_engine_process_event(&env.engine, &event) ==
                          CANCESTRY_RECIPE_OK);
    CANCESSTRY_TEST_CHECK_U64(env.trace.count, 2u); /* ClusterMsg matches nothing */
    /* The unknown message name failed resolution on both can_rx events. */
    CANCESSTRY_TEST_CHECK_U64(env.engine.counters.trigger_unresolved, 2u);
    env_free(&env);
}

static void test_gateway_directional(void)
{
    engine_env_t env;
    cancestry_event_t event;
    const cancestry_test_trace_entry_t *tx;

    CANCESSTRY_TEST_CASE("RECIPE-FILTER-001c: directional gateway (device -> car)");
    env_init(&env, "schema_version: \"0.2.0\"\n"
                   "recipes:\n"
                   "  - name: forward_to_car\n"
                   "    description: whitelist StatusMsg from device, forward to car\n"
                   "    trigger:\n"
                   "      event: can_rx\n"
                   "      interface: device\n"
                   "      message: StatusMsg\n"
                   "    actions:\n"
                   "      - send_message:\n"
                   "          interface: car\n"
                   "          message: ClusterMsg\n"
                   "          signals:\n"
                   "            ClusterSpeed: 1000\n");
    if (env.set == NULL) {
        return;
    }
    /* Whitelisted direction: StatusMsg on the device bus forwards to car. */
    event = cancestry_test_can_rx_event(TEST_MSG_STATUS, TEST_IFACE_CAN1, 100u, 1u);
    CANCESSTRY_TEST_CHECK(cancestry_recipe_engine_process_event(&env.engine, &event) ==
                          CANCESTRY_RECIPE_OK);
    CANCESSTRY_TEST_CHECK_U64(env.trace.count, 1u);
    tx = &env.trace.entries[0];
    CANCESSTRY_TEST_CHECK_STRING(tx->kind, "tx");
    /* "car" resolved to the physical interface can0. */
    CANCESSTRY_TEST_CHECK_U64(tx->interface_id, (uint64_t)TEST_IFACE_CAN0);
    CANCESSTRY_TEST_CHECK_STRING(tx->interface_name, "can0");
    CANCESSTRY_TEST_CHECK_U64(tx->can_id, (uint64_t)TEST_MSG_CLUSTER);
    CANCESSTRY_TEST_CHECK_U64(tx->frame_length, 2u);
    /* ClusterSpeed = 1000, little-endian uint16: 0x03E8. */
    CANCESSTRY_TEST_CHECK_U64(tx->frame[0], 0xE8u);
    CANCESSTRY_TEST_CHECK_U64(tx->frame[1], 0x03u);
    /* Blacklisted direction: frames on the car bus are not forwarded. */
    event = cancestry_test_can_rx_event(TEST_MSG_STATUS, TEST_IFACE_CAN0, 200u, 2u);
    CANCESSTRY_TEST_CHECK(cancestry_recipe_engine_process_event(&env.engine, &event) ==
                          CANCESTRY_RECIPE_OK);
    CANCESSTRY_TEST_CHECK_U64(env.trace.count, 1u);
    /* Off-whitelist message on the device bus is not forwarded either. */
    event = cancestry_test_can_rx_event(TEST_MSG_CLUSTER, TEST_IFACE_CAN1, 300u, 3u);
    CANCESSTRY_TEST_CHECK(cancestry_recipe_engine_process_event(&env.engine, &event) ==
                          CANCESTRY_RECIPE_OK);
    CANCESSTRY_TEST_CHECK_U64(env.trace.count, 1u);
    CANCESSTRY_TEST_CHECK_U64(env.engine.counters.messages_sent, 1u);
    env_free(&env);
}

static void test_signal_trigger(void)
{
    engine_env_t env;
    cancestry_event_t event;

    CANCESSTRY_TEST_CASE("RECIPE-EXEC-004: signal_changed trigger matches by signal id");
    env_init(&env, "schema_version: \"0.2.0\"\n"
                   "recipes:\n"
                   "  - name: speed_watch\n"
                   "    trigger:\n"
                   "      event: signal_changed\n"
                   "      signal: VehicleSpeed\n"
                   "    actions:\n"
                   "      - log:\n"
                   "          level: info\n"
                   "          message: speed changed\n"
                   "  - name: unknown_signal\n"
                   "    trigger:\n"
                   "      event: signal_changed\n"
                   "      signal: NoSignal\n"
                   "    actions:\n"
                   "      - log:\n"
                   "          level: info\n"
                   "          message: never\n");
    if (env.set == NULL) {
        return;
    }
    event = cancestry_test_signal_event(ID_VEHICLE_SPEED, "VehicleSpeed",
                                        cancestry_test_uint_value(60u), 1000u, 1u);
    CANCESSTRY_TEST_CHECK(cancestry_recipe_engine_process_event(&env.engine, &event) ==
                          CANCESTRY_RECIPE_OK);
    CANCESSTRY_TEST_CHECK_U64(env.trace.count, 1u);
    /* A different signal does not match. */
    event = cancestry_test_signal_event(ID_IGNITION, "IgnitionState",
                                        cancestry_test_uint_value(1u), 2000u, 2u);
    CANCESSTRY_TEST_CHECK(cancestry_recipe_engine_process_event(&env.engine, &event) ==
                          CANCESTRY_RECIPE_OK);
    CANCESSTRY_TEST_CHECK_U64(env.trace.count, 1u);
    /* The unknown-signal recipe never fires: fail closed, counted. */
    event = cancestry_test_signal_event(ID_VEHICLE_SPEED, "VehicleSpeed",
                                        cancestry_test_uint_value(70u), 3000u, 3u);
    CANCESSTRY_TEST_CHECK(cancestry_recipe_engine_process_event(&env.engine, &event) ==
                          CANCESTRY_RECIPE_OK);
    CANCESSTRY_TEST_CHECK_U64(env.trace.count, 2u);
    CANCESSTRY_TEST_CHECK_STRING(env.trace.entries[1].message, "speed changed");
    /* The unknown signal name failed resolution on all three events. */
    CANCESSTRY_TEST_CHECK_U64(env.engine.counters.trigger_unresolved, 3u);
    env_free(&env);
}

static void test_timer_fault_mode_triggers(void)
{
    engine_env_t env;
    cancestry_event_t event;

    CANCESSTRY_TEST_CASE("RECIPE-EXEC-005: timer, fault and power-mode triggers");
    env_init(&env, "schema_version: \"0.2.0\"\n"
                   "recipes:\n"
                   "  - name: on_tick\n"
                   "    trigger:\n"
                   "      event: timer_expired\n"
                   "      timer: tick\n"
                   "    actions:\n"
                   "      - log:\n"
                   "          level: info\n"
                   "          message: tick\n"
                   "  - name: on_fault\n"
                   "    trigger:\n"
                   "      event: fault_raised\n"
                   "    actions:\n"
                   "      - log:\n"
                   "          level: error\n"
                   "          message: fault\n"
                   "  - name: on_mode\n"
                   "    trigger:\n"
                   "      event: power_mode_changed\n"
                   "    actions:\n"
                   "      - log:\n"
                   "          level: info\n"
                   "          message: mode\n");
    if (env.set == NULL) {
        return;
    }
    /* tick is bound to timer id 7 in the timer table. */
    event = cancestry_test_can_rx_event(0u, 0u, 0u, 0u); /* reuse the builder shape */
    cancestry_event_init(&event);
    event.type = CANCESTRY_EVENT_TYPE_TIMER_EXPIRED;
    event.priority_class = CANCESTRY_PRIORITY_CLASS_TIMER;
    event.timestamp_us = 100u;
    event.sequence = 1u;
    event.payload.timer_expired.timer_id = 7u;
    CANCESSTRY_TEST_CHECK(cancestry_recipe_engine_process_event(&env.engine, &event) ==
                          CANCESTRY_RECIPE_OK);
    CANCESSTRY_TEST_CHECK_U64(env.trace.count, 1u);

    cancestry_event_init(&event);
    event.type = CANCESTRY_EVENT_TYPE_TIMER_EXPIRED;
    event.priority_class = CANCESTRY_PRIORITY_CLASS_TIMER;
    event.timestamp_us = 200u;
    event.sequence = 2u;
    event.payload.timer_expired.timer_id = 8u; /* not in the table */
    CANCESSTRY_TEST_CHECK(cancestry_recipe_engine_process_event(&env.engine, &event) ==
                          CANCESTRY_RECIPE_OK);
    CANCESSTRY_TEST_CHECK_U64(env.trace.count, 1u);

    cancestry_event_init(&event);
    event.type = CANCESTRY_EVENT_TYPE_FAULT_RAISED;
    event.priority_class = CANCESTRY_PRIORITY_CLASS_FAULT;
    event.timestamp_us = 300u;
    event.sequence = 3u;
    event.payload.fault.fault_code = 42u;
    CANCESSTRY_TEST_CHECK(cancestry_recipe_engine_process_event(&env.engine, &event) ==
                          CANCESTRY_RECIPE_OK);
    CANCESSTRY_TEST_CHECK_U64(env.trace.count, 2u);

    cancestry_event_init(&event);
    event.type = CANCESTRY_EVENT_TYPE_POWER_MODE_CHANGED;
    event.priority_class = CANCESTRY_PRIORITY_CLASS_MODE;
    event.timestamp_us = 400u;
    event.sequence = 4u;
    event.payload.mode.to_mode = CANCESTRY_MODE_ACTIVE;
    CANCESSTRY_TEST_CHECK(cancestry_recipe_engine_process_event(&env.engine, &event) ==
                          CANCESTRY_RECIPE_OK);
    CANCESSTRY_TEST_CHECK_U64(env.trace.count, 3u);
    env_free(&env);
}

static void test_timeout_and_irrelevant_filters(void)
{
    engine_env_t env;
    cancestry_event_t event;

    CANCESSTRY_TEST_CASE("RECIPE-EXEC-006: timeout triggers never match; inapplicable "
                         "filters fail closed");
    env_init(&env, "schema_version: \"0.2.0\"\n"
                   "recipes:\n"
                   "  - name: watcher\n"
                   "    trigger:\n"
                   "      event: timeout\n"
                   "      timeout_ms: 500\n"
                   "    actions:\n"
                   "      - log:\n"
                   "          level: info\n"
                   "          message: timeout\n"
                   "  - name: strict\n"
                   "    trigger:\n"
                   "      event: signal_changed\n"
                   "      signal: VehicleSpeed\n"
                   "      message: StatusMsg\n"
                   "    actions:\n"
                   "      - log:\n"
                   "          level: info\n"
                   "          message: strict\n");
    if (env.set == NULL) {
        return;
    }
    /* No event type maps to "timeout": the recipe can never fire. */
    event = cancestry_test_can_rx_event(TEST_MSG_STATUS, TEST_IFACE_CAN0, 100u, 1u);
    CANCESSTRY_TEST_CHECK(cancestry_recipe_engine_process_event(&env.engine, &event) ==
                          CANCESTRY_RECIPE_OK);
    event = cancestry_test_signal_event(ID_VEHICLE_SPEED, "VehicleSpeed",
                                        cancestry_test_uint_value(1u), 200u, 2u);
    CANCESSTRY_TEST_CHECK(cancestry_recipe_engine_process_event(&env.engine, &event) ==
                          CANCESTRY_RECIPE_OK);
    /* The second recipe declares a message filter that cannot apply to a
     * signal_changed event: it never fires (fail closed, not silently
     * ignored). */
    CANCESSTRY_TEST_CHECK_U64(env.trace.count, 0u);
    CANCESSTRY_TEST_CHECK_U64(env.engine.counters.recipes_invoked, 0u);
    env_free(&env);
}

/* ------------------------------------------------------------------------- */
/* Conditions                                                                */
/* ------------------------------------------------------------------------- */

/** Load a one-recipe file whose single condition is @p expression. */
static void run_condition_case(const char *expression,
                               const cancestry_event_t *event,
                               bool expect_invoked,
                               bool expect_condition_error)
{
    char yaml[512];
    engine_env_t env;
    cancestry_recipe_status_t status;

    (void)snprintf(yaml, sizeof(yaml),
                   "schema_version: \"0.2.0\"\n"
                   "recipes:\n"
                   "  - name: cond\n"
                   "    trigger:\n"
                   "      event: signal_changed\n"
                   "      signal: VehicleSpeed\n"
                   "    conditions:\n"
                   "      - expression: \"%s\"\n"
                   "    actions:\n"
                   "      - log:\n"
                   "          level: info\n"
                   "          message: hit\n",
                   expression);
    env_init(&env, yaml);
    if (env.set == NULL) {
        return;
    }
    status = cancestry_recipe_engine_process_event(&env.engine, event);
    CANCESSTRY_TEST_CHECK(status == CANCESTRY_RECIPE_OK);
    CANCESSTRY_TEST_CHECK_U64(env.trace.count, expect_invoked ? 1u : 0u);
    CANCESSTRY_TEST_CHECK_U64(env.engine.counters.condition_errors,
                              expect_condition_error ? 1u : 0u);
    env_free(&env);
}

static void test_conditions(void)
{
    CANCESSTRY_TEST_CASE("RECIPE-COND-001: condition evaluation (SW-FR-RECIPE-002)");
    /* The triggering signal's value comes from the event payload. */
    {
        cancestry_event_t fast = cancestry_test_signal_event(
            ID_VEHICLE_SPEED, "VehicleSpeed", cancestry_test_uint_value(60u), 1000u, 1u);
        cancestry_event_t slow = cancestry_test_signal_event(
            ID_VEHICLE_SPEED, "VehicleSpeed", cancestry_test_uint_value(30u), 1000u, 1u);

        run_condition_case("sig.VehicleSpeed > 50", &fast, true, false);
        run_condition_case("sig.VehicleSpeed > 50", &slow, false, false);
        run_condition_case("sig.VehicleSpeed == 60", &fast, true, false);
        run_condition_case("sig.VehicleSpeed != 60", &fast, false, false);
        run_condition_case("sig.VehicleSpeed >= 60", &fast, true, false);
        run_condition_case("sig.VehicleSpeed <= 59", &fast, false, false);
        run_condition_case("sig.VehicleSpeed < 60", &fast, false, false);
        run_condition_case("sig.VehicleSpeed * 2 + 1 > 100", &fast, true, false);
        run_condition_case("sig.VehicleSpeed * 2 + 1 > 121", &fast, false, false);
        run_condition_case("sig.VehicleSpeed / 7 == 8", &fast, true, false); /* int division */
        run_condition_case("sig.VehicleSpeed / 7.0 > 8.5", &fast, true, false);
        run_condition_case("sig.VehicleSpeed % 7 == 4", &fast, true, false);
        run_condition_case("not (sig.VehicleSpeed < 50)", &fast, true, false);
        run_condition_case("sig.VehicleSpeed > 50 and sig.VehicleSpeed < 100", &fast, true,
                           false);
        run_condition_case("sig.VehicleSpeed > 100 or sig.VehicleSpeed > 50", &fast, true, false);
        run_condition_case("evt.signal_value == 60", &fast, true, false);
        run_condition_case("evt.timestamp_us == 1000", &fast, true, false);
        run_condition_case("evt.sequence == 1", &fast, true, false);
        run_condition_case("evt.sequence / 2 == 0", &fast, true, false);
        run_condition_case("(sig.VehicleSpeed - 10) * 2 == 100", &fast, true, false);
        run_condition_case("1.5 + 1.5 == 3.0", &fast, true, false);
        run_condition_case("true", &fast, true, false);
        run_condition_case("false", &fast, false, false);

        /* Expression faults fail closed: the recipe is skipped and counted. */
        run_condition_case("var.nothing > 1", &fast, false, true);     /* undefined variable */
        run_condition_case("sig.Missing > 1", &fast, false, true);     /* undefined signal */
        run_condition_case("VehicleSpeed > 1", &fast, false, true);    /* bare identifier */
        run_condition_case("bogus.VehicleSpeed > 1", &fast, false, true); /* unknown namespace */
        run_condition_case("sig.VehicleSpeed", &fast, false, true);    /* not a boolean */
        run_condition_case("1 / 0 == 1", &fast, false, true);          /* division by zero */
        run_condition_case("5 % 0 == 1", &fast, false, true);          /* modulo by zero */
        run_condition_case("9223372036854775807 + 1 == 0", &fast, false, true); /* overflow */
        run_condition_case("sig.VehicleSpeed and true", &fast, false, true);   /* bool operand */
        run_condition_case("evt.can_id == 1", &fast, false, true);     /* field not applicable */
        run_condition_case("evt.bogus == 1", &fast, false, true);      /* unknown evt field */
        run_condition_case("sig.VehicleSpeed >", &fast, false, true);  /* syntax error */
    }
}

static void test_condition_on_store_and_variables(void)
{
    engine_env_t env;
    cancestry_event_t event;
    cancestry_value_t value;

    CANCESSTRY_TEST_CASE("RECIPE-COND-002: conditions read the signal store and variables");
    env_init(&env, "schema_version: \"0.2.0\"\n"
                   "recipes:\n"
                   "  - name: ignition_and_speed\n"
                   "    trigger:\n"
                   "      event: signal_changed\n"
                   "      signal: VehicleSpeed\n"
                   "    conditions:\n"
                   "      - expression: sig.IgnitionState == 1\n"
                   "      - expression: var.hits + 1 >= 1\n"
                   "    actions:\n"
                   "      - set_variable:\n"
                   "          variable: hits\n"
                   "          value: var.hits + 1\n"
                   "      - log:\n"
                   "          level: info\n"
                   "          message: rolling\n");
    if (env.set == NULL) {
        return;
    }
    /* Seed the shared signal store the way the decode pipeline would. */
    value = cancestry_test_uint_value(1u);
    CANCESSTRY_TEST_CHECK(cancestry_recipe_signal_store_set(&env.store, ID_IGNITION, &value) ==
                          CANCESTRY_RECIPE_OK);
    /* var.hits is undefined on the first event: condition faults -> skipped. */
    event = cancestry_test_signal_event(ID_VEHICLE_SPEED, "VehicleSpeed",
                                        cancestry_test_uint_value(60u), 100u, 1u);
    CANCESSTRY_TEST_CHECK(cancestry_recipe_engine_process_event(&env.engine, &event) ==
                          CANCESTRY_RECIPE_OK);
    CANCESSTRY_TEST_CHECK_U64(env.trace.count, 0u);
    CANCESSTRY_TEST_CHECK_U64(env.engine.counters.condition_errors, 1u);

    /* Seed the variable, then the same event passes. */
    value = cancestry_test_int_value(4);
    {
        cancestry_recipe_variable_t *slot = &env.variables[0];
        slot->recipe = &env.set->recipes[0];
        slot->name = "hits";
        slot->value = value;
        env.engine.variable_count = 1u;
    }
    CANCESSTRY_TEST_CHECK(cancestry_recipe_engine_process_event(&env.engine, &event) ==
                          CANCESTRY_RECIPE_OK);
    CANCESSTRY_TEST_CHECK_U64(env.trace.count, 1u);
    CANCESSTRY_TEST_CHECK_U64(env.engine.counters.variables_set, 1u);
    /* var.hits was 4 and is now 5. */
    CANCESSTRY_TEST_CHECK(
        cancestry_recipe_engine_get_variable(&env.engine, &env.set->recipes[0], "hits", &value) ==
        CANCESTRY_RECIPE_OK);
    CANCESSTRY_TEST_CHECK(value.kind == CANCESTRY_VALUE_KIND_INT);
    CANCESSTRY_TEST_CHECK_I64(value.value.integer, 5);
    env_free(&env);
}

/* ------------------------------------------------------------------------- */
/* Actions                                                                   */
/* ------------------------------------------------------------------------- */

static void test_send_message_action(void)
{
    engine_env_t env;
    cancestry_event_t event;

    CANCESSTRY_TEST_CASE("RECIPE-ACTION-001a: send_message encodes and emits via the sink");
    env_init(&env, "schema_version: \"0.2.0\"\n"
                   "recipes:\n"
                   "  - name: adapt\n"
                   "    trigger:\n"
                   "      event: can_rx\n"
                   "    actions:\n"
                   "      - send_message:\n"
                   "          interface: can1\n"
                   "          message: ClusterMsg\n"
                   "          signals:\n"
                   "            ClusterSpeed: 4660\n");
    if (env.set == NULL) {
        return;
    }
    event = cancestry_test_can_rx_event(TEST_MSG_STATUS, TEST_IFACE_CAN0, 100u, 1u);
    CANCESSTRY_TEST_CHECK(cancestry_recipe_engine_process_event(&env.engine, &event) ==
                          CANCESTRY_RECIPE_OK);
    CANCESSTRY_TEST_CHECK_U64(env.trace.count, 1u);
    CANCESSTRY_TEST_CHECK_STRING(env.trace.entries[0].kind, "tx");
    CANCESSTRY_TEST_CHECK_STRING(env.trace.entries[0].interface_name, "can1");
    /* 4660 = 0x1234, little-endian. */
    CANCESSTRY_TEST_CHECK_U64(env.trace.entries[0].frame[0], 0x34u);
    CANCESSTRY_TEST_CHECK_U64(env.trace.entries[0].frame[1], 0x12u);
    CANCESSTRY_TEST_CHECK_U64(env.engine.counters.messages_sent, 1u);
    env_free(&env);
}

static void test_send_message_expression_and_coercion(void)
{
    engine_env_t env;
    cancestry_event_t event;
    cancestry_value_t value;

    CANCESSTRY_TEST_CASE("RECIPE-ACTION-001b: expression values and numeric coercion");
    env_init(&env, "schema_version: \"0.2.0\"\n"
                   "recipes:\n"
                   "  - name: compute\n"
                   "    trigger:\n"
                   "      event: can_rx\n"
                   "    actions:\n"
                   "      - send_message:\n"
                   "          interface: can1\n"
                   "          message: ClusterMsg\n"
                   "          signals:\n"
                   "            ClusterSpeed: sig.VehicleSpeed * 0.5\n");
    if (env.set == NULL) {
        return;
    }
    /* 121 * 0.5 = 60.5, which rounds ties away from zero to 61. */
    value = cancestry_test_uint_value(121u);
    CANCESSTRY_TEST_CHECK(cancestry_recipe_signal_store_set(&env.store, ID_VEHICLE_SPEED,
                                                            &value) == CANCESTRY_RECIPE_OK);
    event = cancestry_test_can_rx_event(TEST_MSG_STATUS, TEST_IFACE_CAN0, 100u, 1u);
    CANCESSTRY_TEST_CHECK(cancestry_recipe_engine_process_event(&env.engine, &event) ==
                          CANCESTRY_RECIPE_OK);
    CANCESSTRY_TEST_CHECK_U64(env.trace.count, 1u);
    CANCESSTRY_TEST_CHECK_U64(env.trace.entries[0].frame[0], 61u);
    CANCESSTRY_TEST_CHECK_U64(env.trace.entries[0].frame[1], 0u);
    env_free(&env);
}

static void test_send_message_failures(void)
{
    engine_env_t env;
    cancestry_event_t event;

    CANCESSTRY_TEST_CASE("RECIPE-ACTION-001c: send_message fails closed (unknown message, "
                         "unknown signal, truncating dlc)");
    env_init(&env, "schema_version: \"0.2.0\"\n"
                   "recipes:\n"
                   "  - name: bad_message\n"
                   "    trigger:\n"
                   "      event: can_rx\n"
                   "    actions:\n"
                   "      - send_message:\n"
                   "          interface: can1\n"
                   "          message: NoSuchMsg\n"
                   "          signals:\n"
                   "            ClusterSpeed: 1\n"
                   "      - log:\n"
                   "          level: info\n"
                   "          message: never\n"
                   "  - name: bad_signal\n"
                   "    trigger:\n"
                   "      event: can_rx\n"
                   "    actions:\n"
                   "      - send_message:\n"
                   "          interface: can1\n"
                   "          message: ClusterMsg\n"
                   "          signals:\n"
                   "            NoSignal: 1\n"
                   "  - name: truncating\n"
                   "    trigger:\n"
                   "      event: can_rx\n"
                   "    actions:\n"
                   "      - send_message:\n"
                   "          interface: can1\n"
                   "          message: TinyMsg\n"
                   "          signals:\n"
                   "            WideSignal: 1\n"
                   "  - name: bad_iface\n"
                   "    trigger:\n"
                   "      event: can_rx\n"
                   "    actions:\n"
                   "      - send_message:\n"
                   "          interface: ghost\n"
                   "          message: ClusterMsg\n"
                   "          signals:\n"
                   "            ClusterSpeed: 1\n");
    if (env.set == NULL) {
        return;
    }
    event = cancestry_test_can_rx_event(TEST_MSG_STATUS, TEST_IFACE_CAN0, 100u, 1u);
    CANCESSTRY_TEST_CHECK(cancestry_recipe_engine_process_event(&env.engine, &event) ==
                          CANCESTRY_RECIPE_OK);
    /* Every recipe's send fails; the default on_error policy halts each one,
     * so no log and no TX happen at all. */
    CANCESSTRY_TEST_CHECK_U64(env.trace.count, 0u);
    CANCESSTRY_TEST_CHECK_U64(env.engine.counters.action_errors, 4u);
    CANCESSTRY_TEST_CHECK_U64(env.engine.counters.recipes_halted, 4u);
    CANCESSTRY_TEST_CHECK_U64(env.engine.counters.messages_sent, 0u);
    CANCESSTRY_TEST_CHECK_U64(governor_state.calls, 0u); /* nothing reached the governor */
    env_free(&env);
}

static void test_set_signal_action(void)
{
    engine_env_t env;
    cancestry_event_t event;
    cancestry_value_t value;
    cancestry_event_t generated;

    CANCESSTRY_TEST_CASE("RECIPE-ACTION-001d: set_signal updates the store and emits "
                         "signal_changed");
    env_init(&env, "schema_version: \"0.2.0\"\n"
                   "recipes:\n"
                   "  - name: publish\n"
                   "    trigger:\n"
                   "      event: can_rx\n"
                   "    actions:\n"
                   "      - set_signal:\n"
                   "          signal: ClusterSpeed\n"
                   "          value: 300\n");
    if (env.set == NULL) {
        return;
    }
    event = cancestry_test_can_rx_event(TEST_MSG_STATUS, TEST_IFACE_CAN0, 5000u, 9u);
    CANCESSTRY_TEST_CHECK(cancestry_recipe_engine_process_event(&env.engine, &event) ==
                          CANCESTRY_RECIPE_OK);
    CANCESSTRY_TEST_CHECK_U64(env.trace.count, 1u);
    CANCESSTRY_TEST_CHECK_STRING(env.trace.entries[0].kind, "signal");
    CANCESSTRY_TEST_CHECK_STRING(env.trace.entries[0].signal_name, "ClusterSpeed");
    CANCESSTRY_TEST_CHECK(env.trace.entries[0].old_value.kind == CANCESTRY_VALUE_KIND_UNSET);
    CANCESSTRY_TEST_CHECK(env.trace.entries[0].new_value.kind == CANCESTRY_VALUE_KIND_INT);
    CANCESSTRY_TEST_CHECK_I64(env.trace.entries[0].new_value.value.integer, 300);

    /* The shared store now holds the new value. */
    CANCESSTRY_TEST_CHECK(cancestry_recipe_signal_store_get(&env.store, ID_CLUSTER_SPEED,
                                                            &value) == CANCESTRY_RECIPE_OK);
    CANCESSTRY_TEST_CHECK(value.kind == CANCESTRY_VALUE_KIND_INT);
    CANCESSTRY_TEST_CHECK_I64(value.value.integer, 300);

    /* A generated signal_changed event was queued (event-ordering.md
     * section 8: GENERATED class, cause_sequence, inherited timestamp). */
    CANCESSTRY_TEST_CHECK_U64(cancestry_event_queue_size(&env.queue), 1u);
    CANCESSTRY_TEST_CHECK(cancestry_event_queue_pop(&env.queue, &generated) ==
                          CANCESTRY_EVENT_QUEUE_OK);
    CANCESSTRY_TEST_CHECK(generated.type == CANCESTRY_EVENT_TYPE_SIGNAL_CHANGED);
    CANCESSTRY_TEST_CHECK(generated.priority_class == CANCESTRY_PRIORITY_CLASS_GENERATED);
    CANCESSTRY_TEST_CHECK_U64(generated.timestamp_us, 5000u);
    CANCESSTRY_TEST_CHECK_U64(generated.cause_sequence, 9u);
    CANCESSTRY_TEST_CHECK_U64(generated.payload.signal_changed.signal_id, ID_CLUSTER_SPEED);
    CANCESSTRY_TEST_CHECK_STRING(generated.payload.signal_changed.signal_name, "ClusterSpeed");

    /* Setting the same value again changes nothing: no event, no sink call. */
    CANCESSTRY_TEST_CHECK(cancestry_recipe_engine_process_event(&env.engine, &event) ==
                          CANCESTRY_RECIPE_OK);
    CANCESSTRY_TEST_CHECK_U64(env.trace.count, 1u);
    CANCESSTRY_TEST_CHECK_U64(cancestry_event_queue_size(&env.queue), 0u);
    CANCESSTRY_TEST_CHECK_U64(env.engine.counters.signals_set, 2u);
    env_free(&env);
}

static void test_log_and_fault_actions(void)
{
    engine_env_t env;
    cancestry_event_t event;
    cancestry_event_t fault;

    CANCESSTRY_TEST_CASE("RECIPE-ACTION-001e: log and raise_fault emit through sink and bus");
    env_init(&env, "schema_version: \"0.2.0\"\n"
                   "recipes:\n"
                   "  - name: report\n"
                   "    trigger:\n"
                   "      event: can_rx\n"
                   "    actions:\n"
                   "      - log:\n"
                   "          level: warning\n"
                   "          message: adapt failed\n"
                   "      - raise_fault:\n"
                   "          code: CLUSTER_TIMEOUT\n"
                   "          severity: error\n");
    if (env.set == NULL) {
        return;
    }
    event = cancestry_test_can_rx_event(TEST_MSG_STATUS, TEST_IFACE_CAN0, 4242u, 3u);
    CANCESSTRY_TEST_CHECK(cancestry_recipe_engine_process_event(&env.engine, &event) ==
                          CANCESTRY_RECIPE_OK);
    CANCESSTRY_TEST_CHECK_U64(env.trace.count, 2u);
    CANCESSTRY_TEST_CHECK_STRING(env.trace.entries[0].kind, "log");
    CANCESSTRY_TEST_CHECK(env.trace.entries[0].level == CANCESTRY_RECIPE_LOG_WARNING);
    CANCESSTRY_TEST_CHECK_STRING(env.trace.entries[0].message, "adapt failed");
    CANCESSTRY_TEST_CHECK_STRING(env.trace.entries[1].kind, "fault");
    CANCESSTRY_TEST_CHECK_STRING(env.trace.entries[1].code, "CLUSTER_TIMEOUT");
    CANCESSTRY_TEST_CHECK(env.trace.entries[1].severity == CANCESTRY_FAULT_SEVERITY_ERROR);
    CANCESSTRY_TEST_CHECK(env.trace.entries[1].numeric_code ==
                          cancestry_recipe_fault_code_hash("CLUSTER_TIMEOUT"));
    CANCESSTRY_TEST_CHECK(env.trace.entries[1].numeric_code != 0u);

    /* The fault was also emitted on the event bus: FAULT class, cause linked. */
    CANCESSTRY_TEST_CHECK(cancestry_event_queue_pop(&env.queue, &fault) ==
                          CANCESTRY_EVENT_QUEUE_OK);
    CANCESSTRY_TEST_CHECK(fault.type == CANCESTRY_EVENT_TYPE_FAULT_RAISED);
    CANCESSTRY_TEST_CHECK(fault.priority_class == CANCESTRY_PRIORITY_CLASS_FAULT);
    CANCESSTRY_TEST_CHECK_U64(fault.timestamp_us, 4242u);
    CANCESSTRY_TEST_CHECK_U64(fault.cause_sequence, 3u);
    CANCESSTRY_TEST_CHECK_U64(fault.payload.fault.source_id, 1u); /* recipe ordinal */
    CANCESSTRY_TEST_CHECK(fault.payload.fault.severity == CANCESTRY_FAULT_SEVERITY_ERROR);
    CANCESSTRY_TEST_CHECK(fault.payload.fault.fault_code ==
                          cancestry_recipe_fault_code_hash("CLUSTER_TIMEOUT"));
    CANCESSTRY_TEST_CHECK_U64(env.engine.counters.logs_emitted, 1u);
    CANCESSTRY_TEST_CHECK_U64(env.engine.counters.faults_raised, 1u);
    env_free(&env);
}

static void test_timer_actions(void)
{
    engine_env_t env;
    cancestry_event_t event;

    CANCESSTRY_TEST_CASE("RECIPE-ACTION-001f: timer actions request through the sink");
    env_init(&env, "schema_version: \"0.2.0\"\n"
                   "recipes:\n"
                   "  - name: timers\n"
                   "    trigger:\n"
                   "      event: can_rx\n"
                   "    actions:\n"
                   "      - start_timer:\n"
                   "          timer: tick\n"
                   "          duration_ms: 250\n"
                   "          repeat: true\n"
                   "      - stop_timer:\n"
                   "          timer: tick\n"
                   "      - reset_timer:\n"
                   "          timer: tock\n");
    if (env.set == NULL) {
        return;
    }
    event = cancestry_test_can_rx_event(TEST_MSG_STATUS, TEST_IFACE_CAN0, 100u, 1u);
    CANCESSTRY_TEST_CHECK(cancestry_recipe_engine_process_event(&env.engine, &event) ==
                          CANCESTRY_RECIPE_OK);
    CANCESSTRY_TEST_CHECK_U64(env.trace.count, 3u);
    CANCESSTRY_TEST_CHECK_STRING(env.trace.entries[0].kind, "timer");
    CANCESSTRY_TEST_CHECK(env.trace.entries[0].timer_kind == CANCESTRY_RECIPE_ACTION_START_TIMER);
    CANCESSTRY_TEST_CHECK_STRING(env.trace.entries[0].timer_name, "tick");
    CANCESSTRY_TEST_CHECK_U64(env.trace.entries[0].duration_ms, 250u);
    CANCESSTRY_TEST_CHECK(env.trace.entries[0].repeat);
    CANCESSTRY_TEST_CHECK(env.trace.entries[1].timer_kind == CANCESTRY_RECIPE_ACTION_STOP_TIMER);
    CANCESSTRY_TEST_CHECK_STRING(env.trace.entries[1].timer_name, "tick");
    CANCESSTRY_TEST_CHECK_U64(env.trace.entries[1].duration_ms, 0u);
    CANCESSTRY_TEST_CHECK(env.trace.entries[2].timer_kind == CANCESTRY_RECIPE_ACTION_RESET_TIMER);
    CANCESSTRY_TEST_CHECK_STRING(env.trace.entries[2].timer_name, "tock");
    CANCESSTRY_TEST_CHECK_U64(env.engine.counters.timers_requested, 3u);
    env_free(&env);
}

static void test_on_error_policies(void)
{
    engine_env_t env;
    cancestry_event_t event;

    CANCESSTRY_TEST_CASE("RECIPE-ACTION-002: on_error stop halts, continue proceeds "
                         "(recipe-spec.md section 5)");
    env_init(&env, "schema_version: \"0.2.0\"\n"
                   "recipes:\n"
                   "  - name: stopper\n"
                   "    trigger:\n"
                   "      event: can_rx\n"
                   "    actions:\n"
                   "      - set_signal:\n"
                   "          signal: NoSignal\n"
                   "          value: 1\n"
                   "      - log:\n"
                   "          level: info\n"
                   "          message: after failure\n"
                   "  - name: continuer\n"
                   "    trigger:\n"
                   "      event: can_rx\n"
                   "    on_error: continue\n"
                   "    actions:\n"
                   "      - set_signal:\n"
                   "          signal: NoSignal\n"
                   "          value: 1\n"
                   "      - log:\n"
                   "          level: info\n"
                   "          message: still running\n");
    if (env.set == NULL) {
        return;
    }
    event = cancestry_test_can_rx_event(TEST_MSG_STATUS, TEST_IFACE_CAN0, 100u, 1u);
    CANCESSTRY_TEST_CHECK(cancestry_recipe_engine_process_event(&env.engine, &event) ==
                          CANCESTRY_RECIPE_OK);
    /* stopper: the failing set_signal halts the recipe, its log never runs. */
    /* continuer: the log runs despite the failure. */
    CANCESSTRY_TEST_CHECK_U64(env.trace.count, 1u);
    CANCESSTRY_TEST_CHECK_STRING(env.trace.entries[0].message, "still running");
    CANCESSTRY_TEST_CHECK_U64(env.engine.counters.action_errors, 2u);
    CANCESSTRY_TEST_CHECK_U64(env.engine.counters.recipes_halted, 1u);
    env_free(&env);
}

static void test_variable_capacity(void)
{
    engine_env_t env;
    cancestry_recipe_engine_config_t config;
    cancestry_event_t event;
    cancestry_recipe_variable_t one_slot[1];

    CANCESSTRY_TEST_CASE("RECIPE-ACTION-003: variable storage is bounded (SYS-NF-002)");
    env_init(&env, "schema_version: \"0.2.0\"\n"
                   "recipes:\n"
                   "  - name: count_things\n"
                   "    trigger:\n"
                   "      event: can_rx\n"
                   "    actions:\n"
                   "      - set_variable:\n"
                   "          variable: a\n"
                   "          value: 1\n"
                   "      - set_variable:\n"
                   "          variable: b\n"
                   "          value: 2\n"
                   "      - set_variable:\n"
                   "          variable: a\n"
                   "          value: 3\n");
    if (env.set == NULL) {
        return;
    }
    /* Reconfigure the engine with a single variable slot. */
    memset(&config, 0, sizeof(config));
    config.sets = env.set;
    config.set_count = 1u;
    config.signal_namespace = &env.ns;
    config.interfaces = cancestry_test_interfaces;
    config.interface_count = 2u;
    config.bindings = cancestry_test_bindings;
    config.binding_count = 2u;
    config.timers = cancestry_test_timers;
    config.timer_count = 1u;
    config.signal_store = &env.store;
    config.event_queue = &env.queue;
    config.sink = &env.sink;
    config.governor = approve_all;
    config.governor_user_data = &governor_state;
    config.variable_storage = one_slot;
    config.variable_capacity = 1u;
    CANCESSTRY_TEST_CHECK(cancestry_recipe_engine_init(&env.engine, &config));

    event = cancestry_test_can_rx_event(TEST_MSG_STATUS, TEST_IFACE_CAN0, 100u, 1u);
    CANCESSTRY_TEST_CHECK(cancestry_recipe_engine_process_event(&env.engine, &event) ==
                          CANCESTRY_RECIPE_OK);
    /* 'a' is created; 'b' does not fit (capacity 1, fail closed); the third
     * action updates 'a' in place and succeeds. Default on_error stops the
     * recipe after the failed action, so only two set_variable actions ran. */
    CANCESSTRY_TEST_CHECK_U64(env.engine.counters.variables_set, 1u);
    CANCESSTRY_TEST_CHECK_U64(env.engine.counters.action_errors, 1u);
    CANCESSTRY_TEST_CHECK_U64(env.engine.counters.recipes_halted, 1u);
    CANCESSTRY_TEST_CHECK_U64(env.engine.variable_count, 1u);
    env_free(&env);
}

static void test_interface_binding_resolution(void)
{
    engine_env_t env;
    cancestry_event_t event;

    CANCESSTRY_TEST_CASE("RECIPE-IFACE-BINDING-001: aliases resolve to physical interfaces "
                         "(SW-FR-RECIPE-007)");
    env_init(&env, "schema_version: \"0.2.0\"\n"
                   "recipes:\n"
                   "  - name: via_alias\n"
                   "    trigger:\n"
                   "      event: can_rx\n"
                   "    actions:\n"
                   "      - send_message:\n"
                   "          interface: car\n"
                   "          message: ClusterMsg\n"
                   "          signals:\n"
                   "            ClusterSpeed: 7\n");
    if (env.set == NULL) {
        return;
    }
    event = cancestry_test_can_rx_event(TEST_MSG_STATUS, TEST_IFACE_CAN0, 100u, 1u);
    CANCESSTRY_TEST_CHECK(cancestry_recipe_engine_process_event(&env.engine, &event) ==
                          CANCESTRY_RECIPE_OK);
    CANCESSTRY_TEST_CHECK_U64(env.trace.count, 1u);
    /* The sink saw the physical interface, not the alias. */
    CANCESSTRY_TEST_CHECK_STRING(env.trace.entries[0].interface_name, "can0");
    CANCESSTRY_TEST_CHECK_U64(env.trace.entries[0].interface_id, (uint64_t)TEST_IFACE_CAN0);
    /* The governor request carries the resolved physical interface too. */
    CANCESSTRY_TEST_CHECK_U64(governor_state.calls, 1u);
    CANCESSTRY_TEST_CHECK(governor_state.last_request.kind ==
                          CANCESTRY_RECIPE_GOVERNOR_SEND_MESSAGE);
    CANCESSTRY_TEST_CHECK_STRING(governor_state.last_request.interface_name, "can0");
    CANCESSTRY_TEST_CHECK_U64(governor_state.last_request.interface_id,
                              (uint64_t)TEST_IFACE_CAN0);
    CANCESSTRY_TEST_CHECK_STRING(governor_state.last_request.message_name, "ClusterMsg");
    CANCESSTRY_TEST_CHECK_U64(governor_state.last_request.can_id, (uint64_t)TEST_MSG_CLUSTER);
    CANCESSTRY_TEST_CHECK_U64(governor_state.last_request.frame_length, 2u);
    CANCESSTRY_TEST_CHECK_U64(governor_state.last_frame[0], 7u);
    CANCESSTRY_TEST_CHECK(governor_state.last_request.recipe == &env.set->recipes[0]);
    env_free(&env);
}

/* ------------------------------------------------------------------------- */
/* Determinism                                                               */
/* ------------------------------------------------------------------------- */

static bool trace_entries_equal(const cancestry_test_trace_entry_t *a,
                                const cancestry_test_trace_entry_t *b)
{
    if (strcmp(a->kind, b->kind) != 0 || a->recipe_ordinal != b->recipe_ordinal ||
        a->interface_id != b->interface_id || a->can_id != b->can_id ||
        a->frame_length != b->frame_length || a->signal_id != b->signal_id ||
        a->level != b->level || a->severity != b->severity ||
        a->numeric_code != b->numeric_code || a->timer_kind != b->timer_kind ||
        a->duration_ms != b->duration_ms || a->repeat != b->repeat) {
        return false;
    }
    if (strcmp(a->interface_name, b->interface_name) != 0 ||
        strcmp(a->signal_name, b->signal_name) != 0 ||
        strcmp(a->message, b->message) != 0 || strcmp(a->code, b->code) != 0 ||
        strcmp(a->timer_name, b->timer_name) != 0) {
        return false;
    }
    if (memcmp(a->frame, b->frame, sizeof(a->frame)) != 0) {
        return false;
    }
    if (a->old_value.kind != b->old_value.kind || a->new_value.kind != b->new_value.kind) {
        return false;
    }
    if (a->new_value.kind == CANCESTRY_VALUE_KIND_INT &&
        a->new_value.value.integer != b->new_value.value.integer) {
        return false;
    }
    if (a->new_value.kind == CANCESTRY_VALUE_KIND_REAL &&
        a->new_value.value.real != b->new_value.value.real) {
        return false;
    }
    return true;
}

static void test_determinism(void)
{
    const char *yaml =
        "schema_version: \"0.2.0\"\n"
        "recipes:\n"
        "  - name: forward\n"
        "    trigger:\n"
        "      event: can_rx\n"
        "      interface: device\n"
        "      message: StatusMsg\n"
        "    actions:\n"
        "      - send_message:\n"
        "          interface: car\n"
        "          message: ClusterMsg\n"
        "          signals:\n"
        "            ClusterSpeed: sig.VehicleSpeed\n"
        "      - set_signal:\n"
        "          signal: ClusterSpeed\n"
        "          value: sig.VehicleSpeed\n"
        "      - set_variable:\n"
        "          variable: count\n"
        "          value: var.count + 1\n"
        "  - name: watch_speed\n"
        "    trigger:\n"
        "      event: signal_changed\n"
        "      signal: VehicleSpeed\n"
        "    conditions:\n"
        "      - expression: sig.VehicleSpeed > 50\n"
        "    actions:\n"
        "      - log:\n"
        "          level: warning\n"
        "          message: fast\n"
        "      - raise_fault:\n"
        "          code: SPEED_HIGH\n"
        "          severity: warning\n"
        "  - name: on_any\n"
        "    trigger:\n"
        "      event: can_rx\n"
        "    actions:\n"
        "      - start_timer:\n"
        "          timer: tick\n"
        "          duration_ms: 100\n"
        "          repeat: false\n";
    engine_env_t first;
    engine_env_t second;
    cancestry_value_t speed;
    cancestry_event_t events[3];
    size_t i;

    CANCESSTRY_TEST_CASE("RECIPE-DETERMINISM-001: same event sequence, same action sequence "
                         "(SYS-NF-001)");
    env_init(&first, yaml);
    env_init(&second, yaml);
    if (first.set == NULL || second.set == NULL) {
        env_free(&first);
        env_free(&second);
        return;
    }
    speed = cancestry_test_uint_value(120u);
    CANCESSTRY_TEST_CHECK(
        cancestry_recipe_signal_store_set(&first.store, ID_VEHICLE_SPEED, &speed) ==
        CANCESTRY_RECIPE_OK);
    CANCESSTRY_TEST_CHECK(
        cancestry_recipe_signal_store_set(&second.store, ID_VEHICLE_SPEED, &speed) ==
        CANCESTRY_RECIPE_OK);

    events[0] = cancestry_test_can_rx_event(TEST_MSG_STATUS, TEST_IFACE_CAN1, 100u, 1u);
    events[1] = cancestry_test_signal_event(ID_VEHICLE_SPEED, "VehicleSpeed", speed, 200u, 2u);
    events[2] = cancestry_test_can_rx_event(TEST_MSG_STATUS, TEST_IFACE_CAN1, 300u, 3u);

    for (i = 0u; i < 3u; ++i) {
        CANCESSTRY_TEST_CHECK(cancestry_recipe_engine_process_event(&first.engine, &events[i]) ==
                              CANCESTRY_RECIPE_OK);
        CANCESSTRY_TEST_CHECK(cancestry_recipe_engine_process_event(&second.engine, &events[i]) ==
                              CANCESTRY_RECIPE_OK);
    }
    CANCESSTRY_TEST_CHECK_U64(first.trace.count, second.trace.count);
    for (i = 0u; i < first.trace.count && i < second.trace.count; ++i) {
        CANCESSTRY_TEST_CHECK(trace_entries_equal(&first.trace.entries[i],
                                                  &second.trace.entries[i]));
    }
    CANCESSTRY_TEST_CHECK_U64(first.engine.counters.recipes_invoked,
                              second.engine.counters.recipes_invoked);
    CANCESSTRY_TEST_CHECK_U64(first.engine.counters.actions_executed,
                              second.engine.counters.actions_executed);
    CANCESSTRY_TEST_CHECK_U64(first.engine.counters.messages_sent,
                              second.engine.counters.messages_sent);
    CANCESSTRY_TEST_CHECK_U64(first.engine.counters.signals_set,
                              second.engine.counters.signals_set);
    CANCESSTRY_TEST_CHECK_U64(first.engine.counters.variables_set,
                              second.engine.counters.variables_set);
    CANCESSTRY_TEST_CHECK_U64(first.engine.counters.faults_raised,
                              second.engine.counters.faults_raised);
    CANCESSTRY_TEST_CHECK_U64(cancestry_event_queue_size(&first.queue),
                              cancestry_event_queue_size(&second.queue));
    env_free(&first);
    env_free(&second);
}

/* ------------------------------------------------------------------------- */
/* API robustness                                                            */
/* ------------------------------------------------------------------------- */

static void test_api_robustness(void)
{
    engine_env_t env;
    cancestry_event_t event;
    cancestry_value_t value;

    CANCESSTRY_TEST_CASE("RECIPE-EXEC-007: argument validation");
    env_init(&env, "schema_version: \"0.2.0\"\n"
                   "recipes:\n"
                   "  - name: only\n"
                   "    trigger:\n"
                   "      event: can_rx\n"
                   "    actions:\n"
                   "      - log:\n"
                   "          level: info\n"
                   "          message: hi\n");
    if (env.set == NULL) {
        return;
    }
    event = cancestry_test_can_rx_event(TEST_MSG_STATUS, TEST_IFACE_CAN0, 100u, 1u);
    CANCESSTRY_TEST_CHECK(cancestry_recipe_engine_process_event(NULL, &event) ==
                          CANCESTRY_RECIPE_ERR_NULL);
    CANCESSTRY_TEST_CHECK(cancestry_recipe_engine_process_event(&env.engine, NULL) ==
                          CANCESTRY_RECIPE_ERR_NULL);
    cancestry_event_init(&event);
    CANCESSTRY_TEST_CHECK(cancestry_recipe_engine_process_event(&env.engine, &event) ==
                          CANCESTRY_RECIPE_ERR_ARGUMENT);
    CANCESSTRY_TEST_CHECK_U64(cancestry_recipe_engine_recipe_count(&env.engine), 1u);
    CANCESSTRY_TEST_CHECK(cancestry_recipe_engine_counters(&env.engine) != NULL);
    CANCESSTRY_TEST_CHECK(cancestry_recipe_engine_counters(NULL) == NULL);

    /* get_variable for an unknown name or NULL arguments. */
    CANCESSTRY_TEST_CHECK(cancestry_recipe_engine_get_variable(&env.engine, &env.set->recipes[0],
                                                               "nope", &value) ==
                          CANCESTRY_RECIPE_ERR_NOT_FOUND);
    CANCESSTRY_TEST_CHECK(cancestry_recipe_engine_get_variable(&env.engine, &env.set->recipes[0],
                                                               "nope", NULL) ==
                          CANCESTRY_RECIPE_ERR_NULL);
    CANCESSTRY_TEST_CHECK(cancestry_recipe_engine_get_variable(NULL, &env.set->recipes[0], "nope",
                                                               &value) == CANCESTRY_RECIPE_ERR_NULL);

    /* Fault code hash is stable and non-zero. */
    CANCESSTRY_TEST_CHECK_U64(cancestry_recipe_fault_code_hash("CLUSTER_TIMEOUT"),
                              (uint64_t)cancestry_recipe_fault_code_hash("CLUSTER_TIMEOUT"));
    CANCESSTRY_TEST_CHECK(cancestry_recipe_fault_code_hash("A") !=
                          cancestry_recipe_fault_code_hash("B"));

    /* Signal store API bounds. */
    CANCESSTRY_TEST_CHECK(!cancestry_recipe_signal_store_init(&env.store, NULL, 4u));
    CANCESSTRY_TEST_CHECK(cancestry_recipe_signal_store_init(&env.store, env.store_slots, 16u));
    value = cancestry_test_uint_value(1u);
    CANCESSTRY_TEST_CHECK(cancestry_recipe_signal_store_set(&env.store, ID_IGNITION, &value) ==
                          CANCESTRY_RECIPE_OK);
    CANCESSTRY_TEST_CHECK(cancestry_recipe_signal_store_get(&env.store, ID_IGNITION, &value) ==
                          CANCESTRY_RECIPE_OK);
    CANCESSTRY_TEST_CHECK(cancestry_recipe_signal_store_get(&env.store, ID_CLUSTER_SPEED,
                                                            &value) ==
                          CANCESTRY_RECIPE_ERR_NOT_FOUND);
    CANCESSTRY_TEST_CHECK_U64(cancestry_recipe_signal_store_size(&env.store), 1u);
    env_free(&env);
}

int main(void)
{
    CANCESSTRY_TEST_SUITE_BEGIN("recipe engine");
    test_can_rx_trigger_basic();
    test_dispatch_order();
    test_disabled_recipe();
    test_interface_filter();
    test_message_filter();
    test_gateway_directional();
    test_signal_trigger();
    test_timer_fault_mode_triggers();
    test_timeout_and_irrelevant_filters();
    test_conditions();
    test_condition_on_store_and_variables();
    test_send_message_action();
    test_send_message_expression_and_coercion();
    test_send_message_failures();
    test_set_signal_action();
    test_log_and_fault_actions();
    test_timer_actions();
    test_on_error_policies();
    test_variable_capacity();
    test_interface_binding_resolution();
    test_determinism();
    test_api_robustness();
    return CANCESSTRY_TEST_SUITE_END();
}
