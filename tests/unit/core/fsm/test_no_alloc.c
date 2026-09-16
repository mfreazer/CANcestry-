/*
 * Verifies that the FSM runtime performs no dynamic allocation while executing.
 *
 * Verifies:
 *   SYS-NF-002       Bounded resource usage: no heap in the runtime path.
 *   issue #11        "No heap allocation during FSM execution (verified by
 *                    symbol scan)" - the companion check is
 *                    ci/check_no_alloc.py, run on the built archive by
 *                    tests/CMakeLists.txt; this test is the runtime tripwire over
 *                    every engine entry point.
 *   SW-FR-FSM-019    A per-instance queue runs on caller-owned storage.
 *   SW-FR-FSM-043..047 Sequential per-instance execution, budgets, isolation.
 *
 * Like the event core's equivalent test, the tripwire is glibc-only and is
 * disabled under AddressSanitizer, where overriding malloc would fight the
 * sanitizer runtime; the archive scan covers every build.
 */

#include "cancestry/codec/loader.h"
#include "cancestry/codec/namespace.h"
#include "cancestry/fsm/engine.h"
#include "cancestry/fsm/loader.h"

#include "cancestry_test.h"

#include <stdlib.h>
#include <string.h>

#if defined(__GLIBC__) && !defined(__SANITIZE_ADDRESS__)
#if defined(__has_feature)
#if __has_feature(address_sanitizer)
#define CANCESSTRY_TEST_SANITIZER_ACTIVE 1
#endif
#endif
#if !defined(CANCESSTRY_TEST_SANITIZER_ACTIVE)
#define CANCESSTRY_TEST_ALLOC_TRIPWIRE 1
#endif
#endif

#if defined(CANCESSTRY_TEST_ALLOC_TRIPWIRE)

/* glibc entry points that bypass the public allocator symbols. */
extern void *__libc_malloc(size_t size);
extern void *__libc_calloc(size_t count, size_t size);
extern void *__libc_realloc(void *ptr, size_t size);
extern void __libc_free(void *ptr);

static int cancestry_test_alloc_guard = 0;
static unsigned long cancestry_test_alloc_calls = 0u;

void *malloc(size_t size)
{
    if (cancestry_test_alloc_guard != 0) {
        cancestry_test_alloc_calls++;
    }
    return __libc_malloc(size);
}

void *calloc(size_t count, size_t size)
{
    if (cancestry_test_alloc_guard != 0) {
        cancestry_test_alloc_calls++;
    }
    return __libc_calloc(count, size);
}

void *realloc(void *ptr, size_t size)
{
    if (cancestry_test_alloc_guard != 0) {
        cancestry_test_alloc_calls++;
    }
    return __libc_realloc(ptr, size);
}

void free(void *ptr)
{
    if (cancestry_test_alloc_guard != 0 && ptr != NULL) {
        cancestry_test_alloc_calls++;
    }
    __libc_free(ptr);
}

#endif /* CANCESSTRY_TEST_ALLOC_TRIPWIRE */

/* The document under test: timers, guards, sends, signal writes and a chain. */
static const char *const fsm_text = "schema_version: \"0.2.0\"\n"
                                    "state_machines:\n"
                                    "  - name: lab\n"
                                    "    initial: IDLE\n"
                                    "    variables:\n"
                                    "      - name: n\n"
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
                                    "          - event: can_rx\n"
                                    "            message: StatusMsg\n"
                                    "            guard: \"sig.VehicleSpeed > 10\"\n"
                                    "            target: ACTIVE\n"
                                    "            actions:\n"
                                    "              - set_variable:\n"
                                    "                  variable: n\n"
                                    "                  value: \"var.n + 1\"\n"
                                    "              - send_message:\n"
                                    "                  interface: can0\n"
                                    "                  message: ClusterMsg\n"
                                    "                  signals:\n"
                                    "                    ClusterSpeed: \"var.n * 10\"\n"
                                    "              - set_signal:\n"
                                    "                  signal: ClusterAlive\n"
                                    "                  value: 1\n"
                                    "              - raise_fault:\n"
                                    "                  code: GUARD_SEEN\n"
                                    "                  severity: warning\n"
                                    "              - transition:\n"
                                    "                  target: IDLE\n"
                                    "          - event: timer_expired\n"
                                    "            timer: beat\n"
                                    "            target: IDLE\n"
                                    "            actions:\n"
                                    "              - set_variable:\n"
                                    "                  variable: n\n"
                                    "                  value: \"min(var.n + 1, 100)\"\n"
                                    "              - log:\n"
                                    "                  level: info\n"
                                    "                  message: beat\n"
                                    "      - name: ACTIVE\n"
                                    "        exit:\n"
                                    "          - stop_timer:\n"
                                    "              timer: beat\n"
                                    "instances:\n"
                                    "  - id: lab.one\n"
                                    "    machine: lab\n"
                                    "    enabled: true\n"
                                    "    subscriptions:\n"
                                    "      faults: true\n"
                                    "      signals:\n"
                                    "        - ClusterAlive\n"
                                    "  - id: lab.two\n"
                                    "    machine: lab\n"
                                    "    enabled: true\n";

static const char *const codec_text = "schema_version: \"0.2.0\"\n"
                                      "codec_map:\n"
                                      "  name: demo\n"
                                      "  version: 1.0.0\n"
                                      "  messages:\n"
                                      "    - id: 0x100\n"
                                      "      name: StatusMsg\n"
                                      "      dlc: 8\n"
                                      "      signals:\n"
                                      "        - name: VehicleSpeed\n"
                                      "          start_bit: 0\n"
                                      "          bit_length: 16\n"
                                      "          type: uint\n"
                                      "          endianness: little\n"
                                      "    - id: 0x200\n"
                                      "      name: ClusterMsg\n"
                                      "      dlc: 2\n"
                                      "      signals:\n"
                                      "        - name: ClusterSpeed\n"
                                      "          start_bit: 0\n"
                                      "          bit_length: 16\n"
                                      "          type: uint\n"
                                      "          endianness: little\n";

/* Caller-owned runtime storage, exactly as a target would provide it. */
#define INSTANCE_COUNT 2u
#define QUEUE_CAPACITY 64u
#define VARIABLE_CAPACITY 4u
#define TIMER_CAPACITY 2u

static cancestry_fsm_instance_t instances[INSTANCE_COUNT];
static cancestry_fsm_instance_storage_t storage[INSTANCE_COUNT];
static cancestry_event_t queue_slots[INSTANCE_COUNT][QUEUE_CAPACITY];
static cancestry_fsm_variable_slot_t variable_slots[INSTANCE_COUNT][VARIABLE_CAPACITY];
static cancestry_fsm_timer_state_t timer_slots[INSTANCE_COUNT][TIMER_CAPACITY];
static cancestry_fsm_trace_record_t trace_records[64u];
static cancestry_event_t global_slots[64u];
static cancestry_event_queue_t global_queue;
static cancestry_fsm_engine_t engine;

static uint32_t signal_writes = 0u;
static uint32_t sends = 0u;

typedef struct {
    cancestry_value_t value;
    bool set;
} signal_cell_t;

static signal_cell_t signals[4];
static const char *const signal_names[4] = {"VehicleSpeed", "ClusterSpeed", "ClusterAlive",
                                            "IgnitionState"};

static cancestry_fsm_status_t bus_read(void *user_data, const char *name, cancestry_value_t *out)
{
    size_t i;

    (void)user_data;
    for (i = 0u; i < sizeof(signal_names) / sizeof(signal_names[0]); ++i) {
        if (strcmp(signal_names[i], name) == 0) {
            if (!signals[i].set) {
                return CANCESTRY_FSM_ERR_NOT_FOUND;
            }
            *out = signals[i].value;
            return CANCESTRY_FSM_OK;
        }
    }
    return CANCESTRY_FSM_ERR_NOT_FOUND;
}

static cancestry_fsm_status_t bus_write(void *user_data, const char *name,
                                        const cancestry_value_t *value)
{
    size_t i;

    (void)user_data;
    for (i = 0u; i < sizeof(signal_names) / sizeof(signal_names[0]); ++i) {
        if (strcmp(signal_names[i], name) == 0) {
            signals[i].value = *value;
            signals[i].set = true;
            signal_writes++;
            return CANCESTRY_FSM_OK;
        }
    }
    return CANCESTRY_FSM_ERR_NOT_FOUND;
}

static cancestry_fsm_status_t bus_resolve(void *user_data, const char *name,
                                          cancestry_signal_id_t *id_out)
{
    size_t i;

    (void)user_data;
    for (i = 0u; i < sizeof(signal_names) / sizeof(signal_names[0]); ++i) {
        if (strcmp(signal_names[i], name) == 0) {
            *id_out = (cancestry_signal_id_t)(i + 1u);
            return CANCESTRY_FSM_OK;
        }
    }
    return CANCESTRY_FSM_ERR_NOT_FOUND;
}

static cancestry_fsm_governor_decision_t governor(void *user_data,
                                                 const cancestry_fsm_governor_request_t *request)
{
    (void)user_data;
    (void)request;
    return CANCESTRY_FSM_GOVERNOR_APPROVE;
}

static void sink_send(void *user_data,
                      const cancestry_fsm_invocation_t *invocation,
                      const char *interface_name,
                      cancestry_interface_id_t interface_id,
                      const char *message_name,
                      uint32_t can_id,
                      const uint8_t *frame,
                      uint8_t frame_length)
{
    (void)user_data;
    (void)invocation;
    (void)interface_name;
    (void)interface_id;
    (void)message_name;
    (void)can_id;
    (void)frame;
    (void)frame_length;
    sends++;
}

static cancestry_fsm_interface_capability_t interface_capability;
static uint32_t tx_ids[1];
static const char *read_list[3];
static const char *write_list[2];
static cancestry_fsm_capabilities_t capability_table;

/** @return true when a lifecycle or injection call was accepted or cleanly refused. */
static bool fsm_status_tolerable(cancestry_fsm_status_t status)
{
    /* The exercise drives the lifecycle in a loop, so "not legal from this state"
     * and "the queue was full" are expected answers; what must not happen is a
     * crash, a leak, or an allocation. */
    return status == CANCESTRY_FSM_OK || status == CANCESTRY_FSM_ERR_STATE ||
           status == CANCESTRY_FSM_ERR_QUEUE_FULL || status == CANCESTRY_FSM_ERR_NOT_FOUND;
}

/**
 * Exercise every engine entry point. Nothing in here may print or allocate on
 * its own account: stdout buffering allocates on first use, which would trip the
 * guard for reasons unrelated to the engine, so all reporting happens after the
 * guard is released.
 */
static void exercise_engine(bool *ok)
{
    cancestry_event_t event;
    cancestry_event_t cause;
    cancestry_fsm_instance_t *first;
    cancestry_fsm_instance_t *second;
    cancestry_value_t value;
    cancestry_fsm_timer_state_t timer;
    cancestry_fsm_instance_counters_t totals;
    char buffer[256];
    uint32_t step;

    first = cancestry_fsm_instance_at(&engine, 0u);
    second = cancestry_fsm_instance_at(&engine, 1u);
    *ok = (first != NULL) && (second != NULL) && *ok;
    if (first == NULL || second == NULL) {
        return;
    }

    for (step = 0u; step < 40u; ++step) {
        cancestry_event_init(&event);
        event.type = CANCESTRY_EVENT_TYPE_CAN_RX;
        event.priority_class = CANCESTRY_PRIORITY_CLASS_CAN_RX;
        event.timestamp_us = (cancestry_time_us_t)step * 1000u;
        event.payload.can_rx.interface_id = 1u;
        event.payload.can_rx.can_id = 0x100u;
        event.payload.can_rx.length = 8u;
        *ok = (cancestry_fsm_engine_process_event(&engine, &event) == CANCESTRY_FSM_OK) && *ok;

        cancestry_event_init(&cause);
        cause.type = CANCESTRY_EVENT_TYPE_SIGNAL_CHANGED;
        cause.priority_class = CANCESTRY_PRIORITY_CLASS_CAN_RX;
        cause.timestamp_us = (cancestry_time_us_t)step * 1000u + 1u;
        cause.payload.signal_changed.signal_name = "VehicleSpeed";
        cause.payload.signal_changed.new_value.kind = CANCESTRY_VALUE_KIND_INT;
        cause.payload.signal_changed.new_value.value.integer = (int64_t)(step * 3u);
        *ok = (cancestry_fsm_engine_process_event(&engine, &cause) == CANCESTRY_FSM_OK) && *ok;

        *ok = (cancestry_fsm_engine_tick(&engine) == CANCESTRY_FSM_OK) && *ok;
        *ok = (cancestry_fsm_engine_advance(&engine, 3u) == CANCESTRY_FSM_OK) && *ok;

        *ok = cancestry_fsm_instance_get_variable(&engine, first, "n", &value) ==
                      CANCESTRY_FSM_OK &&
              *ok;
        *ok = cancestry_fsm_instance_get_timer(&engine, first, "beat", &timer) ==
                      CANCESTRY_FSM_OK &&
              *ok;
        *ok = (cancestry_fsm_engine_trace_render(&engine, buffer, sizeof(buffer)) > 0u) && *ok;
        *ok = (cancestry_fsm_engine_totals(&engine, &totals) == CANCESTRY_FSM_OK) && *ok;
        *ok = fsm_status_tolerable(cancestry_fsm_engine_inject_event(&engine, second, &event)) &&
              *ok;

        if ((step % 7u) == 0u) {
            *ok = fsm_status_tolerable(cancestry_fsm_instance_suspend(&engine, first)) && *ok;
            *ok = fsm_status_tolerable(cancestry_fsm_instance_resume(&engine, first)) && *ok;
            *ok = fsm_status_tolerable(cancestry_fsm_instance_fault(&engine, first)) && *ok;
            *ok = fsm_status_tolerable(cancestry_fsm_instance_reset(&engine, first)) && *ok;
            *ok = fsm_status_tolerable(cancestry_fsm_instance_start(&engine, first)) && *ok;
            *ok = fsm_status_tolerable(cancestry_fsm_instance_disable(&engine, first)) && *ok;
            *ok = fsm_status_tolerable(cancestry_fsm_instance_enable(&engine, first)) && *ok;
            *ok = fsm_status_tolerable(cancestry_fsm_instance_start(&engine, first)) && *ok;
        }
    }
    cancestry_fsm_engine_trace_clear(&engine);
}

int main(void)
{
    cancestry_fsm_set_t *set;
    cancestry_codec_map_t *map;
    const cancestry_codec_map_t *namespace_slots[1];
    cancestry_codec_namespace_t signal_namespace;
    cancestry_fsm_engine_config_t config;
    cancestry_fsm_signal_bus_t bus;
    cancestry_fsm_sink_t sink;
    bool exercise_ok = true;
    cancestry_fsm_load_error_t load_error;
    cancestry_codec_load_error_t codec_error;

    CANCESSTRY_TEST_SUITE_BEGIN("core/fsm/no_alloc");
    CANCESSTRY_TEST_CASE("FSM-NO-ALLOC-001 execution allocates nothing");

    /* Everything that may allocate happens before the guard is armed. */
    set = cancestry_fsm_set_load(fsm_text, strlen(fsm_text), &load_error);
    CANCESSTRY_TEST_CHECK(set != NULL);
    map = cancestry_codec_map_load(codec_text, strlen(codec_text), &codec_error);
    CANCESSTRY_TEST_CHECK(map != NULL);
    if (set == NULL || map == NULL) {
        return CANCESSTRY_TEST_SUITE_END();
    }
    namespace_slots[0] = map;
    (void)cancestry_codec_namespace_init(&signal_namespace, namespace_slots, 1u);
    (void)cancestry_codec_namespace_register(&signal_namespace, map);

    tx_ids[0] = 0x200u;
    interface_capability.name = "can0";
    interface_capability.id = 1u;
    interface_capability.rx = true;
    interface_capability.tx = true;
    interface_capability.tx_ids = tx_ids;
    interface_capability.tx_id_count = 1u;
    read_list[0] = "VehicleSpeed";
    read_list[1] = "ClusterSpeed";
    read_list[2] = "IgnitionState";
    write_list[0] = "ClusterAlive";
    write_list[1] = "ClusterSpeed";
    capability_table.interfaces = &interface_capability;
    capability_table.interface_count = 1u;
    capability_table.signal_read = read_list;
    capability_table.signal_read_count = 3u;
    capability_table.signal_write = write_list;
    capability_table.signal_write_count = 2u;

    memset(&bus, 0, sizeof(bus));
    bus.user_data = NULL;
    bus.read = bus_read;
    bus.write = bus_write;
    bus.resolve = bus_resolve;
    memset(&sink, 0, sizeof(sink));
    sink.on_send_message = sink_send;

    (void)cancestry_event_queue_init(&global_queue, global_slots, 64u);
    memset(&config, 0, sizeof(config));
    config.sets = set;
    config.set_count = 1u;
    config.instances = instances;
    config.storage = storage;
    config.instance_capacity = INSTANCE_COUNT;
    config.capabilities = &capability_table;
    config.namespace = &signal_namespace;
    config.signal_bus = &bus;
    config.global_queue = &global_queue;
    config.governor = governor;
    config.sink = &sink;
    config.trace = trace_records;
    config.trace_capacity = 64u;
    config.record_events = true;
    {
        size_t i;

        for (i = 0u; i < INSTANCE_COUNT; ++i) {
            storage[i].event_slots = queue_slots[i];
            storage[i].event_capacity = QUEUE_CAPACITY;
            storage[i].variables = variable_slots[i];
            storage[i].variable_capacity = VARIABLE_CAPACITY;
            storage[i].timers = timer_slots[i];
            storage[i].timer_capacity = TIMER_CAPACITY;
        }
    }
    CANCESSTRY_TEST_CHECK(cancestry_fsm_engine_init(&engine, &config));
    /* Seed the bus so the guard reads a real value instead of faulting. */
    signals[0].value.kind = CANCESTRY_VALUE_KIND_INT;
    signals[0].value.value.integer = 42;
    signals[0].set = true;
    signals[1].value.kind = CANCESTRY_VALUE_KIND_INT;
    signals[1].value.value.integer = 0;
    signals[1].set = true;
    sends = 0u;
    signal_writes = 0u;
    /* Warm up the stdio buffer so the first printf is not charged to the engine. */
    {
        char warm[256];

        (void)cancestry_fsm_engine_trace_render(&engine, warm, sizeof(warm));
    }

#if defined(CANCESSTRY_TEST_ALLOC_TRIPWIRE)
    cancestry_test_alloc_calls = 0u;
    cancestry_test_alloc_guard = 1;
    exercise_engine(&exercise_ok);
    cancestry_test_alloc_guard = 0;

    CANCESSTRY_TEST_CHECK(exercise_ok);
    CANCESSTRY_TEST_CHECK_U64(cancestry_test_alloc_calls, 0u);
    CANCESSTRY_TEST_CHECK(sends > 0u);
    CANCESSTRY_TEST_CHECK(signal_writes > 0u);
    printf("    (allocator tripwire active: %lu allocation calls observed while armed)\n",
           cancestry_test_alloc_calls);
#else
    /*
     * Without the tripwire (ASan builds, non-glibc hosts) the runtime path is
     * still exercised, and the archive scan in CI is what proves the property.
     */
    exercise_engine(&exercise_ok);
    CANCESSTRY_TEST_CHECK(exercise_ok);
    CANCESSTRY_TEST_CHECK(sends > 0u);
    CANCESSTRY_TEST_CHECK(signal_writes > 0u);
    printf("    (allocator tripwire unavailable on this platform; archive scan covers it)\n");
#endif

    cancestry_fsm_set_free(set);
    cancestry_codec_map_free(map);
    return CANCESSTRY_TEST_SUITE_END();
}
