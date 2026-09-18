/*
 * CANcestry static FSM demo over generated definitions (Phase 8, issue #22).
 *
 * This harness includes the yaml2c-generated headers (gateway_fsm_static.h,
 * gateway_codec_static.h) that CMake produces from gateway_fsm.yaml and
 * gateway_codec.yaml at build time, and runs the FSM engine directly on the
 * const static definitions - the runtime YAML loader is not linked in and no
 * heap allocation happens anywhere in this binary (SW-FR-TOOL-003). The
 * zero-allocation property is gated in CI by ci/check_no_alloc.py on this
 * translation unit (test cancestry_gateway_real_static_no_alloc_symbols,
 * SW-FR-TOOL-003 acceptance for examples/gateway_real/).
 *
 * Deterministic script (no real time, no I/O):
 *   1. start the static instance: STANDBY is entered;
 *   2. VehicleSpeed = 3 (below the guard): stays STANDBY;
 *   3. VehicleSpeed = 30: guard true -> ACTIVE, entry sends ClusterDisplay;
 *   4. 12 engine ticks (1 ms each): the 5 ms poll timer expires twice and
 *      self-transitions ACTIVE -> ACTIVE;
 *   5. VehicleSpeed = 0: guard true -> STANDBY, stop_timer + CLUSTER_IDLE.
 * The final state and counters are checked; the retained trace is printed.
 *
 * Implements: SW-FR-TOOL-001, SW-FR-TOOL-002, SW-FR-TOOL-003.
 * Test ids:    YAML2C-STATIC-LOOP-001, YAML2C-NOALLOC-001.
 */

#include "gateway_codec_static.h"
#include "gateway_fsm_static.h"

#include "cancestry/codec/namespace.h"
#include "cancestry/event/queue.h"
#include "cancestry/fsm/engine.h"
#include "cancestry/fsm/types.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------------- */
/* Caller-owned static storage (SYS-NF-002): nothing here is heap allocated. */
/* ------------------------------------------------------------------------- */

#define STATIC_IFACE_CAN0 ((cancestry_interface_id_t)1u)
#define STATIC_IFACE_CAN1 ((cancestry_interface_id_t)2u)
#define STATIC_INSTANCES ((size_t)1u)
#define STATIC_QUEUE_DEPTH ((uint16_t)16u)
#define STATIC_VARIABLES ((uint16_t)4u)
#define STATIC_TIMERS ((uint16_t)2u)
#define STATIC_TRACE ((uint16_t)64u)
#define STATIC_NAMESPACE_SLOTS ((uint16_t)1u)
#define STATIC_TRACE_TEXT ((size_t)2048u)

static cancestry_fsm_instance_t static_instances[STATIC_INSTANCES];
static cancestry_fsm_instance_storage_t static_storage[STATIC_INSTANCES];
static cancestry_event_t static_queue_slots[STATIC_INSTANCES][STATIC_QUEUE_DEPTH];
static cancestry_fsm_variable_slot_t static_variable_slots[STATIC_INSTANCES][STATIC_VARIABLES];
static cancestry_fsm_timer_state_t static_timer_slots[STATIC_INSTANCES][STATIC_TIMERS];
static cancestry_fsm_trace_record_t static_trace[STATIC_TRACE];
static const cancestry_codec_map_t *static_namespace_slots[STATIC_NAMESPACE_SLOTS];

static cancestry_codec_namespace_t static_namespace;
static cancestry_fsm_engine_t static_engine;

static const char *static_signal_read_allow[1] = {"VehicleSpeed"};
static const char *static_signal_write_allow[1] = {NULL};

static cancestry_fsm_interface_capability_t static_interface_caps[2];
static uint32_t static_can1_tx_ids[1] = {0x321u};
static cancestry_fsm_capabilities_t static_capabilities;

/* Counters captured by the sink callbacks. */
static struct {
    uint32_t frames_sent;
    uint32_t logs;
    uint32_t faults;
    uint32_t state_changes;
    uint32_t warnings;
} static_counts;

/* ------------------------------------------------------------------------- */
/* Runtime sinks: count side effects, never allocate.                        */
/* ------------------------------------------------------------------------- */

static void static_send(void *user_data, const cancestry_fsm_invocation_t *inv,
                        const char *interface_name,
                        cancestry_interface_id_t interface_id,
                        const char *message_name, uint32_t can_id,
                        const uint8_t *data, uint8_t length)
{
    (void)user_data; (void)inv; (void)interface_name; (void)message_name;
    printf("send: iface=%u id=0x%03X len=%u data[0]=%u data[1]=%u\n",
           (unsigned)interface_id, (unsigned)can_id, (unsigned)length,
           (unsigned)data[0], (unsigned)data[1]);
    static_counts.frames_sent++;
}

static void static_signal_write(void *user_data,
                                const cancestry_fsm_invocation_t *inv,
                                const char *name, cancestry_signal_id_t id,
                                const cancestry_value_t *old_value,
                                const cancestry_value_t *new_value)
{
    (void)user_data; (void)inv; (void)name; (void)id; (void)old_value;
    (void)new_value;
}

static void static_log(void *user_data, const cancestry_fsm_invocation_t *inv,
                       cancestry_fsm_log_level_t level, const char *message)
{
    (void)user_data; (void)inv;
    printf("log %u: %s\n", (unsigned)level, message);
    static_counts.logs++;
}

static void static_fault(void *user_data, const cancestry_fsm_invocation_t *inv,
                         const char *code, cancestry_fault_severity_t severity,
                         cancestry_fault_code_t numeric)
{
    (void)user_data; (void)inv; (void)severity; (void)numeric;
    printf("fault: %s\n", code);
    static_counts.faults++;
}

static void static_state(void *user_data, const cancestry_fsm_invocation_t *inv,
                         const char *from, const char *to)
{
    (void)user_data; (void)inv;
    printf("state: %s -> %s\n", from != NULL ? from : "(none)", to);
    static_counts.state_changes++;
}

static void static_warn(void *user_data, const cancestry_fsm_invocation_t *inv,
                        const char *text)
{
    (void)user_data; (void)inv;
    printf("warning: %s\n", text);
    static_counts.warnings++;
}

static void static_timer(void *user_data, const cancestry_fsm_invocation_t *inv,
                         cancestry_fsm_action_kind_t kind, const char *timer_name,
                         uint32_t duration_ms, bool repeat)
{
    (void)user_data; (void)inv; (void)kind; (void)timer_name;
    (void)duration_ms; (void)repeat;
}

/* Deterministic reads of the VehicleSpeed signal: the last injected value. */
static int64_t static_vehicle_speed;

static cancestry_fsm_status_t static_bus_read(void *user_data, const char *name,
                                              cancestry_value_t *out)
{
    (void)user_data;
    if (strcmp(name, "VehicleSpeed") != 0) {
        return CANCESTRY_FSM_ERR_NOT_FOUND;
    }
    out->kind = CANCESTRY_VALUE_KIND_INT;
    out->value.integer = static_vehicle_speed;
    return CANCESTRY_FSM_OK;
}

static cancestry_fsm_status_t static_bus_write(void *user_data, const char *name,
                                               const cancestry_value_t *value)
{
    (void)user_data; (void)name; (void)value;
    return CANCESTRY_FSM_ERR_DENIED;
}

static cancestry_fsm_status_t static_bus_resolve(void *user_data,
                                                 const char *name,
                                                 cancestry_signal_id_t *id_out)
{
    (void)user_data; (void)name;
    *id_out = 1u;
    return CANCESTRY_FSM_OK;
}

static cancestry_fsm_governor_decision_t static_governor(
    void *user_data, const cancestry_fsm_governor_request_t *request)
{
    (void)user_data; (void)request;
    return CANCESTRY_FSM_GOVERNOR_APPROVE;
}

/* ------------------------------------------------------------------------- */
/* Deterministic driver.                                                     */
/* ------------------------------------------------------------------------- */

static const cancestry_fsm_sink_t static_sink = {
    .user_data = NULL,
    .on_send_message = static_send,
    .on_signal_write = static_signal_write,
    .on_log = static_log,
    .on_fault = static_fault,
    .on_timer = static_timer,
    .on_state_change = static_state,
    .on_warning = static_warn
};

static cancestry_fsm_signal_bus_t static_bus = {
    NULL, static_bus_read, static_bus_write, static_bus_resolve
};

static bool static_world_init(void)
{
    cancestry_fsm_engine_config_t config;

    /* The generated codec map registers like a loaded one: identical type. */
    if (!cancestry_codec_namespace_init(&static_namespace,
                                        static_namespace_slots,
                                        STATIC_NAMESPACE_SLOTS)) {
        return false;
    }
    if (cancestry_codec_namespace_register(&static_namespace,
                                           &gateway_static_codec_map) !=
        CANCESTRY_CODEC_OK) {
        return false;
    }

    static_interface_caps[0].name = "can0";
    static_interface_caps[0].id = STATIC_IFACE_CAN0;
    static_interface_caps[0].rx = true;
    static_interface_caps[0].tx = false;
    static_interface_caps[0].tx_ids = NULL;
    static_interface_caps[0].tx_id_count = 0u;
    static_interface_caps[1].name = "can1";
    static_interface_caps[1].id = STATIC_IFACE_CAN1;
    static_interface_caps[1].rx = true;
    static_interface_caps[1].tx = true;
    static_interface_caps[1].tx_ids = static_can1_tx_ids;
    static_interface_caps[1].tx_id_count = 1u;
    static_capabilities.interfaces = static_interface_caps;
    static_capabilities.interface_count = 2u;
    static_capabilities.signal_read = static_signal_read_allow;
    static_capabilities.signal_read_count = 1u;
    static_capabilities.signal_write = static_signal_write_allow;
    static_capabilities.signal_write_count = 0u;

    static_storage[0].event_slots = static_queue_slots[0];
    static_storage[0].event_capacity = STATIC_QUEUE_DEPTH;
    static_storage[0].variables = static_variable_slots[0];
    static_storage[0].variable_capacity = STATIC_VARIABLES;
    static_storage[0].timers = static_timer_slots[0];
    static_storage[0].timer_capacity = STATIC_TIMERS;

    memset(&config, 0, sizeof(config));
    config.sets = &gateway_static_fsm_set;
    config.set_count = 1u;
    config.instances = static_instances;
    config.storage = static_storage;
    config.instance_capacity = STATIC_INSTANCES;
    /* clock == NULL selects the engine's internal deterministic 1 ms clock. */
    config.clock = NULL;
    config.capabilities = &static_capabilities;
    config.namespace = &static_namespace;
    config.signal_bus = &static_bus;
    config.governor = static_governor;
    config.sink = &static_sink;
    config.trace = static_trace;
    config.trace_capacity = STATIC_TRACE;
    config.record_events = true;
    return cancestry_fsm_engine_init(&static_engine, &config);
}

static void static_signal_event(cancestry_event_t *event, int64_t speed)
{
    cancestry_event_init(event);
    event->type = CANCESTRY_EVENT_TYPE_SIGNAL_CHANGED;
    event->priority_class = CANCESTRY_PRIORITY_CLASS_CAN_RX;
    event->payload.signal_changed.signal_name = "VehicleSpeed";
    event->payload.signal_changed.new_value.kind = CANCESTRY_VALUE_KIND_INT;
    event->payload.signal_changed.new_value.value.integer = speed;
}

static int static_run(void)
{
    /* instance_by_id returns the non-const handle instance_start needs. */
    cancestry_fsm_instance_t *instance;
    char trace_text[STATIC_TRACE_TEXT];
    size_t printed;

    instance = cancestry_fsm_instance_by_id(&static_engine, "cluster.static");
    if (instance == NULL ||
        cancestry_fsm_instance_start(&static_engine, instance) !=
            CANCESTRY_FSM_OK) {
        printf("FAIL: instance start\n");
        return 1;
    }
    if (strcmp(cancestry_fsm_instance_state_name(instance), "STANDBY") != 0) {
        printf("FAIL: expected STANDBY after start\n");
        return 1;
    }

    /* Below the guard: no transition. The guard expression reads the signal
     * bus, so the injected value goes through static_bus_read. */
    {
        cancestry_event_t event;
        static_vehicle_speed = 3;
        static_signal_event(&event, 3);
        if (cancestry_fsm_engine_process_event(&static_engine, &event) !=
            CANCESTRY_FSM_OK) {
            printf("FAIL: process event 3\n");
            return 1;
        }
    }
    instance = cancestry_fsm_instance_by_id(&static_engine, "cluster.static");
    if (strcmp(cancestry_fsm_instance_state_name(instance), "STANDBY") != 0) {
        printf("FAIL: guard must keep the machine in STANDBY\n");
        return 1;
    }

    /* Above the guard: STANDBY -> ACTIVE, entry sends the display frame. */
    {
        cancestry_event_t event;
        static_vehicle_speed = 30;
        static_signal_event(&event, 30);
        if (cancestry_fsm_engine_process_event(&static_engine, &event) !=
            CANCESTRY_FSM_OK) {
            printf("FAIL: process event 30\n");
            return 1;
        }
    }
    instance = cancestry_fsm_instance_by_id(&static_engine, "cluster.static");
    if (strcmp(cancestry_fsm_instance_state_name(instance), "ACTIVE") != 0 ||
        static_counts.frames_sent != 1u) {
        printf("FAIL: expected ACTIVE after the guarded transition\n");
        return 1;
    }

    /* 12 x 1 ms ticks: the 5 ms periodic poll timer expires twice and runs
     * the ACTIVE self-transition (generated timer reference resolves). */
    if (cancestry_fsm_engine_advance(&static_engine, 12u) != CANCESTRY_FSM_OK) {
        printf("FAIL: engine advance\n");
        return 1;
    }
    instance = cancestry_fsm_instance_by_id(&static_engine, "cluster.static");
    if (instance->counters.transitions < 2u) {
        printf("FAIL: expected the poll self-transitions, saw %u\n",
               (unsigned)instance->counters.transitions);
        return 1;
    }

    /* Back below the lower guard: ACTIVE -> STANDBY, timer stopped, fault. */
    {
        cancestry_event_t event;
        static_vehicle_speed = 0;
        static_signal_event(&event, 0);
        if (cancestry_fsm_engine_process_event(&static_engine, &event) !=
            CANCESTRY_FSM_OK) {
            printf("FAIL: process event 0\n");
            return 1;
        }
    }
    instance = cancestry_fsm_instance_by_id(&static_engine, "cluster.static");
    if (strcmp(cancestry_fsm_instance_state_name(instance), "STANDBY") != 0 ||
        static_counts.faults != 1u) {
        printf("FAIL: expected STANDBY and one fault after the return\n");
        return 1;
    }

    /* The generated set_variable action ran and was applied. */
    if (instance->counters.variables_set != 1u) {
        printf("FAIL: set_variable did not apply (counters.variables_set=%u)\n",
               (unsigned)instance->counters.variables_set);
        return 1;
    }

    printed = cancestry_fsm_engine_trace_render(&static_engine, trace_text,
                                                sizeof(trace_text));
    printf("--- retained trace (golden-render, SW-FR-FSM-053) ---\n%s",
           (printed > 0u) ? trace_text : "(empty)\n");

    printf("static demo: transitions=%u frames=%u logs=%u faults=%u "
           "state_changes=%u PASS\n",
           (unsigned)instance->counters.transitions,
           static_counts.frames_sent, static_counts.logs,
           static_counts.faults, static_counts.state_changes);
    return 0;
}

int main(void)
{
    int status;

    if (!static_world_init()) {
        printf("FAIL: static world init\n");
        return 1;
    }
    status = static_run();
    /* Nothing to free: every definition is const static data. */
    return status;
}
