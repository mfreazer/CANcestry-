/*
 * Shared fixture for the CANcestry FSM conformance suite.
 *
 * The suite verifies *behavioural contracts*, so it runs the shipping code
 * end to end: FSM files are loaded through cancestry_fsm_set_load() (which
 * validates them against schemas/fsm-0.2.0.schema.json), the engine executes
 * them over caller-owned storage, and every observable effect is recorded here
 * instead of reaching hardware. A test that passes through this fixture has
 * therefore exercised the loader, the schema validation, the dispatcher, the
 * guard evaluator, the action executor, the governor checkpoint and the
 * capability gate at once.
 *
 * Everything is static inline: the suite has no build-time dependency beyond
 * the core libraries, and each test stays an independent executable (the
 * convention of tests/unit/support/cancestry_test.h).
 *
 * The demo environment the fixture installs is documented in
 * fsm_test_environment() below; tests refer to the same names so the YAML in
 * each test file stays readable.
 */

#ifndef CANCESTRY_FSM_CONFORMANCE_H
#define CANCESTRY_FSM_CONFORMANCE_H

#include "cancestry/codec/loader.h"
#include "cancestry/codec/namespace.h"
#include "cancestry/event/queue.h"
#include "cancestry/fsm/engine.h"
#include "cancestry/fsm/loader.h"

#include "cancestry_test.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------------- */
/* Sizes                                                                     */
/* ------------------------------------------------------------------------- */

#define FSM_TEST_MAX_INSTANCES ((size_t)6u)
/** The normative default per-FSM incoming queue depth (SW-FR-FSM-019). */
#define FSM_TEST_DEFAULT_QUEUE_DEPTH ((uint16_t)64u)
#define FSM_TEST_MAX_QUEUE_DEPTH ((uint16_t)80u)
#define FSM_TEST_MAX_VARIABLES ((uint16_t)8u)
#define FSM_TEST_MAX_TIMERS ((uint16_t)4u)
#define FSM_TEST_MAX_LINES ((size_t)48u)
#define FSM_TEST_LINE_MAX ((size_t)160u)
#define FSM_TEST_TRACE_RECORDS ((uint16_t)192u)
#define FSM_TEST_GLOBAL_QUEUE ((uint16_t)64u)
#define FSM_TEST_MAX_SIGNALS ((size_t)16u)

/* ------------------------------------------------------------------------- */
/* Demo environment                                                          */
/* ------------------------------------------------------------------------- */

#define FSM_TEST_IFACE_CAN0 ((cancestry_interface_id_t)1u)
#define FSM_TEST_IFACE_CAN1 ((cancestry_interface_id_t)2u)

#define FSM_TEST_MSG_STATUS ((uint32_t)0x100u)
#define FSM_TEST_MSG_CLUSTER ((uint32_t)0x200u)
#define FSM_TEST_MSG_ALERT ((uint32_t)0x300u)

/** Governor decision modes for the stub the suite installs. */
#define FSM_TEST_GOV_APPROVE_ALL ((int)0)
#define FSM_TEST_GOV_DENY_ALL ((int)1)
#define FSM_TEST_GOV_DENY_SEND ((int)2)
#define FSM_TEST_GOV_DENY_SET_SIGNAL ((int)3)
/** Approve at most @c governor_budget send_message requests, then deny: the
 * shape a token-bucket rate limit has (SW-FR-GOV-002 will use it). */
#define FSM_TEST_GOV_RATE_LIMIT ((int)4)
/** Deny one specific CAN id, the shape a TX allowlist has. */
#define FSM_TEST_GOV_DENY_CAN_ID ((int)5)

typedef struct fsm_test_signal_slot {
    const char *name;
    cancestry_value_t value;
    bool set;
} fsm_test_signal_slot_t;

typedef struct fsm_test_fixture {
    /* Definitions. */
    cancestry_fsm_set_t *set;
    cancestry_codec_map_t *codec_map;
    const cancestry_codec_map_t *namespace_slots[1];
    cancestry_codec_namespace_t signal_namespace;

    /* Engine and its caller-owned storage. */
    cancestry_fsm_engine_t engine;
    cancestry_fsm_instance_t instances[FSM_TEST_MAX_INSTANCES];
    cancestry_fsm_instance_storage_t storage[FSM_TEST_MAX_INSTANCES];
    cancestry_event_t queue_slots[FSM_TEST_MAX_INSTANCES][FSM_TEST_MAX_QUEUE_DEPTH];
    cancestry_fsm_variable_slot_t variables[FSM_TEST_MAX_INSTANCES][FSM_TEST_MAX_VARIABLES];
    cancestry_fsm_timer_state_t timers[FSM_TEST_MAX_INSTANCES][FSM_TEST_MAX_TIMERS];
    cancestry_fsm_trace_record_t trace[FSM_TEST_TRACE_RECORDS];
    uint16_t queue_depth;
    uint16_t max_chain_depth;
    uint16_t max_actions_per_event;
    bool record_events;

    /* Global event queue, where generated events are published. */
    cancestry_event_queue_t global_queue;
    cancestry_event_t global_slots[FSM_TEST_GLOBAL_QUEUE];
    bool use_global_queue;

    /* Time base. */
    cancestry_virtual_clock_t virtual_clock;
    cancestry_clock_t clock;
    bool use_clock;

    /* Capabilities, installed by default and adjustable per test. */
    cancestry_fsm_capabilities_t capabilities;
    cancestry_fsm_interface_capability_t interfaces[2];
    uint32_t can0_tx_ids[2];
    const char *signal_read[6];
    const char *signal_write[3];
    cancestry_fsm_binding_t bindings[2];
    bool use_capabilities;

    /* Signal bus. */
    cancestry_fsm_signal_bus_t bus;
    fsm_test_signal_slot_t signals[FSM_TEST_MAX_SIGNALS];
    size_t signal_count;

    /* Governor stub. */
    cancestry_fsm_governor_fn governor;
    int governor_mode;
    uint32_t governor_budget;
    uint32_t governor_deny_can_id;
    uint32_t governor_calls;
    uint32_t governor_approvals;
    uint32_t governor_denials;

    /* Recording sink. */
    cancestry_fsm_sink_t sink;
    char lines[FSM_TEST_MAX_LINES][FSM_TEST_LINE_MAX];
    size_t line_count;
    size_t line_overflow;
} fsm_test_fixture_t;

/** The codec map the default fixture registers: see the ids above. */
static inline const char *fsm_test_codec_yaml(void)
{
    return "schema_version: \"0.2.0\"\n"
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
           "        - name: IgnitionState\n"
           "          start_bit: 16\n"
           "          bit_length: 8\n"
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
           "          endianness: little\n"
           "    - id: 0x300\n"
           "      name: AlertMsg\n"
           "      dlc: 1\n"
           "      signals:\n"
           "        - name: AlertLevel\n"
           "          start_bit: 0\n"
           "          bit_length: 8\n"
           "          type: uint\n"
           "          endianness: little\n";
}

/**
 * The demo codec map plus one CAN FD message (issue #20).
 *
 * `FdMsg` (id 0x400) declares dlc 64 with a signal in the last payload byte, so
 * any attempt to build it on the classic 8-byte egress path is a width error
 * and not a silent truncation. The map declares `can_fd: true`, so the fixture
 * loads it with CAN FD capabilities declared.
 */
static inline const char *fsm_test_codec_fd_yaml(void)
{
    return "schema_version: \"0.3.0\"\n"
           "codec_map:\n"
           "  name: demo\n"
           "  version: 1.0.0\n"
           "  can_fd: true\n"
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
           "        - name: IgnitionState\n"
           "          start_bit: 16\n"
           "          bit_length: 8\n"
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
           "          endianness: little\n"
           "    - id: 0x300\n"
           "      name: AlertMsg\n"
           "      dlc: 1\n"
           "      signals:\n"
           "        - name: AlertLevel\n"
           "          start_bit: 0\n"
           "          bit_length: 8\n"
           "          type: uint\n"
           "          endianness: little\n"
           "    - id: 0x400\n"
           "      name: FdMsg\n"
           "      dlc: 64\n"
           "      signals:\n"
           "        - name: FdTail\n"
           "          start_bit: 504\n"
           "          bit_length: 8\n"
           "          type: uint\n"
           "          endianness: little\n";
}

/** CAN id of the CAN FD message in fsm_test_codec_fd_yaml(). */
#define FSM_TEST_MSG_FD ((uint32_t)0x400u)

/* ------------------------------------------------------------------------- */
/* Recording helpers                                                         */
/* ------------------------------------------------------------------------- */

static inline void fsm_test_record(fsm_test_fixture_t *fx, const char *format, ...)
{
    va_list args;

    if (fx->line_count >= FSM_TEST_MAX_LINES) {
        fx->line_overflow++;
        return;
    }
    va_start(args, format);
    (void)vsnprintf(fx->lines[fx->line_count], FSM_TEST_LINE_MAX, format, args);
    va_end(args);
    fx->line_count++;
}

static inline void fsm_test_reset(fsm_test_fixture_t *fx)
{
    fx->line_count = 0u;
    fx->line_overflow = 0u;
    if (fx->set != NULL && fx->engine.ready) {
        cancestry_fsm_engine_trace_clear(&fx->engine);
    }
}

static inline size_t fsm_test_line_count(const fsm_test_fixture_t *fx)
{
    return fx->line_count;
}

static inline const char *fsm_test_line_at(const fsm_test_fixture_t *fx, size_t index)
{
    if (index >= fx->line_count) {
        return "";
    }
    return fx->lines[index];
}

/** Print every recorded line; used to make a failing assertion debuggable. */
static inline void fsm_test_dump(const fsm_test_fixture_t *fx)
{
    size_t i;

    printf("    ---- recorded effects (%u lines, %u dropped) ----\n", (unsigned)fx->line_count,
           (unsigned)fx->line_overflow);
    for (i = 0u; i < fx->line_count; ++i) {
        printf("    %3u | %s\n", (unsigned)i, fx->lines[i]);
    }
    printf("    ----------------------------------------------\n");
    fflush(stdout);
}

static inline bool fsm_test_has_line(const fsm_test_fixture_t *fx, const char *needle)
{
    size_t i;

    for (i = 0u; i < fx->line_count; ++i) {
        if (strstr(fx->lines[i], needle) != NULL) {
            return true;
        }
    }
    return false;
}

static inline size_t fsm_test_count_lines(const fsm_test_fixture_t *fx, const char *needle)
{
    size_t i;
    size_t count = 0u;

    for (i = 0u; i < fx->line_count; ++i) {
        if (strstr(fx->lines[i], needle) != NULL) {
            count++;
        }
    }
    return count;
}

/** Render one value the way the recorder does, so tests can build expectations. */
static inline void fsm_test_value_text(const cancestry_value_t *value, char *buffer, size_t size)
{
    switch (value->kind) {
    case CANCESTRY_VALUE_KIND_BOOL:
        (void)snprintf(buffer, size, "%s", value->value.boolean ? "true" : "false");
        return;
    case CANCESTRY_VALUE_KIND_INT:
    case CANCESTRY_VALUE_KIND_UINT:
        (void)snprintf(buffer, size, "%lld",
                       (long long)((value->kind == CANCESTRY_VALUE_KIND_UINT)
                                       ? (int64_t)value->value.unsigned_integer
                                       : value->value.integer));
        return;
    case CANCESTRY_VALUE_KIND_REAL:
        (void)snprintf(buffer, size, "%g", value->value.real);
        return;
    default:
        (void)snprintf(buffer, size, "unset");
        return;
    }
}

/* ------------------------------------------------------------------------- */
/* Signal bus                                                                */
/* ------------------------------------------------------------------------- */

static inline const fsm_test_signal_slot_t *fsm_test_signal_at(const fsm_test_fixture_t *fx,
                                                                const char *name)
{
    size_t i;

    for (i = 0u; i < fx->signal_count; ++i) {
        if (strcmp(fx->signals[i].name, name) == 0) {
            return &fx->signals[i];
        }
    }
    return NULL;
}

static inline fsm_test_signal_slot_t *fsm_test_signal(fsm_test_fixture_t *fx, const char *name)
{
    size_t i;

    for (i = 0u; i < fx->signal_count; ++i) {
        if (strcmp(fx->signals[i].name, name) == 0) {
            return &fx->signals[i];
        }
    }
    return NULL;
}

static inline void fsm_test_install_signal(fsm_test_fixture_t *fx, const char *name)
{
    size_t i;

    for (i = 0u; i < fx->signal_count; ++i) {
        if (strcmp(fx->signals[i].name, name) == 0) {
            return;
        }
    }
    if (fx->signal_count < FSM_TEST_MAX_SIGNALS) {
        fx->signals[fx->signal_count].name = name;
        memset(&fx->signals[fx->signal_count].value, 0, sizeof(cancestry_value_t));
        fx->signals[fx->signal_count].set = false;
        fx->signal_count++;
    }
}

static inline cancestry_fsm_status_t fsm_test_bus_read(void *user_data,
                                                       const char *name,
                                                       cancestry_value_t *value_out)
{
    fsm_test_fixture_t *fx = (fsm_test_fixture_t *)user_data;
    fsm_test_signal_slot_t *slot;

    if (name == NULL || value_out == NULL) {
        return CANCESTRY_FSM_ERR_NOT_FOUND;
    }
    slot = fsm_test_signal(fx, name);
    if (slot == NULL || !slot->set) {
        return CANCESTRY_FSM_ERR_NOT_FOUND;
    }
    *value_out = slot->value;
    return CANCESTRY_FSM_OK;
}

static inline cancestry_fsm_status_t fsm_test_bus_write(void *user_data,
                                                        const char *name,
                                                        const cancestry_value_t *value)
{
    fsm_test_fixture_t *fx = (fsm_test_fixture_t *)user_data;
    fsm_test_signal_slot_t *slot;

    if (name == NULL || value == NULL) {
        return CANCESTRY_FSM_ERR_ARGUMENT;
    }
    slot = fsm_test_signal(fx, name);
    if (slot == NULL) {
        /* An unknown signal is refused rather than created silently: the FSM may
         * only write what the environment declares. */
        return CANCESTRY_FSM_ERR_NOT_FOUND;
    }
    slot->value = *value;
    slot->set = true;
    return CANCESTRY_FSM_OK;
}

static inline cancestry_fsm_status_t fsm_test_bus_resolve(void *user_data,
                                                          const char *name,
                                                          cancestry_signal_id_t *id_out)
{
    fsm_test_fixture_t *fx = (fsm_test_fixture_t *)user_data;
    size_t i;

    for (i = 0u; i < fx->signal_count; ++i) {
        if (strcmp(fx->signals[i].name, name) == 0) {
            *id_out = (cancestry_signal_id_t)(i + 1u);
            return CANCESTRY_FSM_OK;
        }
    }
    return CANCESTRY_FSM_ERR_NOT_FOUND;
}

/** Set a bus signal from a test, recording nothing (input, not output). */
static inline void fsm_test_set_int(fsm_test_fixture_t *fx, const char *name, int64_t value)
{
    cancestry_value_t v;

    memset(&v, 0, sizeof(v));
    v.kind = CANCESTRY_VALUE_KIND_INT;
    v.value.integer = value;
    fsm_test_install_signal(fx, name);
    (void)fsm_test_bus_write(fx, name, &v);
}

static inline void fsm_test_set_real(fsm_test_fixture_t *fx, const char *name, double value)
{
    cancestry_value_t v;

    memset(&v, 0, sizeof(v));
    v.kind = CANCESTRY_VALUE_KIND_REAL;
    v.value.real = value;
    fsm_test_install_signal(fx, name);
    (void)fsm_test_bus_write(fx, name, &v);
}

static inline void fsm_test_set_bool(fsm_test_fixture_t *fx, const char *name, bool value)
{
    cancestry_value_t v;

    memset(&v, 0, sizeof(v));
    v.kind = CANCESTRY_VALUE_KIND_BOOL;
    v.value.boolean = value;
    fsm_test_install_signal(fx, name);
    (void)fsm_test_bus_write(fx, name, &v);
}

static inline int64_t fsm_test_get_int(const fsm_test_fixture_t *fx, const char *name)
{
    const fsm_test_signal_slot_t *slot = fsm_test_signal_at(fx, name);

    if (slot == NULL || !slot->set) {
        return INT64_MIN; /* sentinel: "no value" */
    }
    switch (slot->value.kind) {
    case CANCESTRY_VALUE_KIND_INT:
        return slot->value.value.integer;
    case CANCESTRY_VALUE_KIND_UINT:
        return (int64_t)slot->value.value.unsigned_integer;
    case CANCESTRY_VALUE_KIND_REAL:
        return (int64_t)slot->value.value.real;
    case CANCESTRY_VALUE_KIND_BOOL:
        return slot->value.value.boolean ? 1 : 0;
    default:
        return INT64_MIN;
    }
}

static inline bool fsm_test_signal_is_set(const fsm_test_fixture_t *fx, const char *name)
{
    const fsm_test_signal_slot_t *slot = fsm_test_signal_at(fx, name);

    return slot != NULL && slot->set;
}

/* ------------------------------------------------------------------------- */
/* Governor stub                                                             */
/* ------------------------------------------------------------------------- */

static inline cancestry_fsm_governor_decision_t
fsm_test_governor(void *user_data, const cancestry_fsm_governor_request_t *request)
{
    fsm_test_fixture_t *fx = (fsm_test_fixture_t *)user_data;
    bool deny = false;

    fx->governor_calls++;
    switch (fx->governor_mode) {
    case FSM_TEST_GOV_DENY_ALL:
        deny = true;
        break;
    case FSM_TEST_GOV_DENY_SEND:
        deny = request->kind == CANCESTRY_FSM_GOVERNOR_SEND_MESSAGE;
        break;
    case FSM_TEST_GOV_DENY_SET_SIGNAL:
        deny = request->kind == CANCESTRY_FSM_GOVERNOR_SET_SIGNAL;
        break;
    case FSM_TEST_GOV_RATE_LIMIT:
        if (request->kind == CANCESTRY_FSM_GOVERNOR_SEND_MESSAGE) {
            if (fx->governor_budget == 0u) {
                deny = true;
            } else {
                fx->governor_budget--;
            }
        }
        break;
    case FSM_TEST_GOV_DENY_CAN_ID:
        deny = (request->kind == CANCESTRY_FSM_GOVERNOR_SEND_MESSAGE &&
                request->can_id == fx->governor_deny_can_id);
        break;
    case FSM_TEST_GOV_APPROVE_ALL:
    default:
        break;
    }
    if (deny) {
        fx->governor_denials++;
        fsm_test_record(fx, "governor deny kind=%s",
                        (request->kind == CANCESTRY_FSM_GOVERNOR_SEND_MESSAGE) ? "send_message"
                                                                              : "set_signal");
        return CANCESTRY_FSM_GOVERNOR_DENY;
    }
    fx->governor_approvals++;
    return CANCESTRY_FSM_GOVERNOR_APPROVE;
}

/* ------------------------------------------------------------------------- */
/* Sink                                                                      */
/* ------------------------------------------------------------------------- */

static inline void fsm_test_sink_send_message(void *user_data,
                                              const cancestry_fsm_invocation_t *invocation,
                                              const char *interface_name,
                                              cancestry_interface_id_t interface_id,
                                              const char *message_name,
                                              uint32_t can_id,
                                              const uint8_t *frame,
                                              uint8_t frame_length)
{
    fsm_test_fixture_t *fx = (fsm_test_fixture_t *)user_data;
    char bytes[40];
    size_t i;
    size_t used = 0u;

    (void)invocation;
    (void)interface_id;
    bytes[0] = '\0';
    for (i = 0u; i < frame_length && i < sizeof(bytes) / 2u; ++i) {
        used += (size_t)snprintf(bytes + used, sizeof(bytes) - used, "%02X", frame[i]);
    }
    fsm_test_record(fx, "tx %u %s %s 0x%X [%s]", (unsigned)invocation->instance_id,
                    interface_name, message_name, (unsigned)can_id, bytes);
}

static inline void fsm_test_sink_signal_write(void *user_data,
                                              const cancestry_fsm_invocation_t *invocation,
                                              const char *signal_name,
                                              cancestry_signal_id_t signal_id,
                                              const cancestry_value_t *old_value,
                                              const cancestry_value_t *new_value)
{
    fsm_test_fixture_t *fx = (fsm_test_fixture_t *)user_data;
    char before[32];
    char after[32];

    (void)invocation;
    (void)signal_id;
    fsm_test_value_text(old_value, before, sizeof(before));
    fsm_test_value_text(new_value, after, sizeof(after));
    fsm_test_record(fx, "signal %u %s %s->%s", (unsigned)invocation->instance_id, signal_name,
                    before, after);
}

static inline void fsm_test_sink_log(void *user_data,
                                     const cancestry_fsm_invocation_t *invocation,
                                     cancestry_fsm_log_level_t level,
                                     const char *message)
{
    fsm_test_fixture_t *fx = (fsm_test_fixture_t *)user_data;

    (void)invocation;
    fsm_test_record(fx, "log %u %s %s", (unsigned)invocation->instance_id,
                    cancestry_fsm_log_level_name(level), message);
}

static inline void fsm_test_sink_fault(void *user_data,
                                       const cancestry_fsm_invocation_t *invocation,
                                       const char *code,
                                       cancestry_fault_severity_t severity,
                                       cancestry_fault_code_t numeric_code)
{
    fsm_test_fixture_t *fx = (fsm_test_fixture_t *)user_data;

    (void)invocation;
    fsm_test_record(fx, "fault %u %s %s code=%u", (unsigned)invocation->instance_id, code,
                    (severity == CANCESTRY_FAULT_SEVERITY_INFO)
                        ? "info"
                        : ((severity == CANCESTRY_FAULT_SEVERITY_WARNING)
                               ? "warning"
                               : ((severity == CANCESTRY_FAULT_SEVERITY_ERROR) ? "error"
                                                                               : "critical")),
                    (unsigned)numeric_code);
}

static inline void fsm_test_sink_timer(void *user_data,
                                       const cancestry_fsm_invocation_t *invocation,
                                       cancestry_fsm_action_kind_t kind,
                                       const char *timer_name,
                                       uint32_t duration_ms,
                                       bool repeat)
{
    fsm_test_fixture_t *fx = (fsm_test_fixture_t *)user_data;

    (void)invocation;
    fsm_test_record(fx, "timer %u %s %s duration=%u repeat=%u", (unsigned)invocation->instance_id,
                    cancestry_fsm_action_kind_name(kind), timer_name, (unsigned)duration_ms,
                    repeat ? 1u : 0u);
}

static inline void fsm_test_sink_state(void *user_data,
                                       const cancestry_fsm_invocation_t *invocation,
                                       const char *from,
                                       const char *to)
{
    fsm_test_fixture_t *fx = (fsm_test_fixture_t *)user_data;

    (void)invocation;
    fsm_test_record(fx, "state %u %s->%s", (unsigned)invocation->instance_id,
                    (from != NULL) ? from : "(initial)", to);
}

static inline void fsm_test_sink_warning(void *user_data,
                                         const cancestry_fsm_invocation_t *invocation,
                                         const char *text)
{
    fsm_test_fixture_t *fx = (fsm_test_fixture_t *)user_data;

    (void)invocation;
    fsm_test_record(fx, "warn %u %s", (unsigned)invocation->instance_id, text);
}

/* ------------------------------------------------------------------------- */
/* Fixture construction                                                      */
/* ------------------------------------------------------------------------- */

/**
 * Install the default demo capabilities.
 *
 * can0: rx + tx, TX allowlist {0x100, 0x200}. can1: rx only, so a
 * send_message on can1 is a capability violation before the governor is ever
 * asked. Signal reads are allowed for the decoded demo signals, writes only for
 * the two cluster signals: SpeedMPH is deliberately *not* writable, and
 * VehicleSpeed is deliberately not readable by default in the strict variant.
 */
static inline void fsm_test_install_capabilities_ex(fsm_test_fixture_t *fx,
                                                    uint16_t read_count,
                                                    uint16_t write_count)
{
    static const char *const read_list[] = {"VehicleSpeed", "IgnitionState", "ClusterSpeed",
                                           "ClusterAlive", "SpeedMPH", "AlertLevel"};
    static const char *const write_list[] = {"ClusterSpeed", "ClusterAlive", "SpeedMPH"};
    size_t i;

    fx->can0_tx_ids[0] = FSM_TEST_MSG_STATUS;
    fx->can0_tx_ids[1] = FSM_TEST_MSG_CLUSTER;
    fx->interfaces[0].name = "can0";
    fx->interfaces[0].id = FSM_TEST_IFACE_CAN0;
    fx->interfaces[0].rx = true;
    fx->interfaces[0].tx = true;
    fx->interfaces[0].tx_ids = fx->can0_tx_ids;
    fx->interfaces[0].tx_id_count = 2u;
    fx->interfaces[1].name = "can1";
    fx->interfaces[1].id = FSM_TEST_IFACE_CAN1;
    fx->interfaces[1].rx = true;
    fx->interfaces[1].tx = false;
    fx->interfaces[1].tx_ids = NULL;
    fx->interfaces[1].tx_id_count = 0u;

    for (i = 0u; i < sizeof(read_list) / sizeof(read_list[0]); ++i) {
        fx->signal_read[i] = read_list[i];
    }
    for (i = 0u; i < sizeof(write_list) / sizeof(write_list[0]); ++i) {
        fx->signal_write[i] = write_list[i];
    }
    fx->capabilities.interfaces = fx->interfaces;
    fx->capabilities.interface_count = 2u;
    fx->capabilities.signal_read = fx->signal_read;
    fx->capabilities.signal_read_count =
        (read_count != 0u) ? read_count : (uint16_t)(sizeof(read_list) / sizeof(read_list[0]));
    fx->capabilities.signal_write = fx->signal_write;
    fx->capabilities.signal_write_count =
        (write_count != 0u) ? write_count
                            : (uint16_t)(sizeof(write_list) / sizeof(write_list[0]));
    fx->use_capabilities = true;

    /* Package-level aliases; instance bindings override these by name. */
    fx->bindings[0].alias = "car";
    fx->bindings[0].physical = "can0";
    fx->bindings[1].alias = "device";
    fx->bindings[1].physical = "can1";
}

/** Load an FSM document, failing the test with the loader's diagnostic. */
static inline cancestry_fsm_set_t *fsm_test_load(const char *yaml)
{
    cancestry_fsm_load_error_t error;
    cancestry_fsm_set_t *set = cancestry_fsm_set_load(yaml, strlen(yaml), &error);

    if (set == NULL) {
        printf("    FAIL to load test FSM: %s (status %s, line %u, column %u)\n", error.message,
               cancestry_fsm_status_name(error.status), (unsigned)error.line,
               (unsigned)error.column);
        fflush(stdout);
        cancestry_test_failures++;
    }
    return set;
}

/**
 * Fixture construction options.
 *
 * Every conformance test starts from the defaults and flips only what its
 * contract is about, which keeps the assertions about "one variable changed"
 * meaningful.
 */
typedef struct fsm_test_options {
    /** Drive the engine from an explicit virtual clock instead of the internal
     * 1 ms tick clock. */
    bool clocked;
    cancestry_time_us_t start_us;
    /** Per-instance incoming queue depth (default: the normative 64). */
    uint16_t queue_depth;
    /** Transition chain limit; 0 selects the normative default of 4. */
    uint16_t max_chain_depth;
    /** Action budget per event; 0 selects the engine default. */
    uint16_t max_actions_per_event;
    /** Internally generated events per activation; 0 selects the engine default. */
    uint16_t max_events_per_activation;
    /** Record one trace record per processed event (SW-FR-FSM-049). */
    bool record_events;
    /** Publish generated events on a global queue. */
    bool global_queue;
    /** Leave the governor unset, which must deny every side effect. */
    bool no_governor;
    /** Leave capabilities unset, which must deny before the governor. */
    bool no_capabilities;
    /** Do not register the demo codec map, so message names cannot resolve. */
    bool no_codec;
    /** Shrink the signal read allowlist to this many entries (0 keeps the six
     * demo signals). Capability tests use it to make a signal unreadable. */
    uint16_t signal_read_count;
    /** Shrink the signal write allowlist the same way (0 keeps the three). */
    uint16_t signal_write_count;
    /** Install no sink at all: approved effects then fail as unsupported, which is
     * how a test proves the engine has no other path to the outside world. */
    bool no_sink;
} fsm_test_options_t;

static inline fsm_test_options_t fsm_test_options_default(void)
{
    fsm_test_options_t options;

    memset(&options, 0, sizeof(options));
    options.queue_depth = FSM_TEST_DEFAULT_QUEUE_DEPTH;
    options.global_queue = true;
    return options;
}

static inline void fsm_test_install_capabilities(fsm_test_fixture_t *fx)
{
    fsm_test_install_capabilities_ex(fx, 0u, 0u);
}

/**
 * Initialize the fixture with an explicit codec map document.
 *
 * The document is loaded with CAN FD capabilities declared, so both the
 * classic demo map and fsm_test_codec_fd_yaml() load unchanged; the load-time
 * capability gate itself is covered by tests/conformance/codec/.
 */
static inline bool fsm_test_init_codec(fsm_test_fixture_t *fx,
                                       const char *yaml,
                                       const fsm_test_options_t *options,
                                       const char *codec_yaml)
{
    cancestry_fsm_engine_config_t config;
    cancestry_codec_platform_caps_t codec_caps;
    cancestry_codec_load_error_t codec_error;
    fsm_test_options_t opts;
    size_t declared;
    size_t i;

    opts = (options != NULL) ? *options : fsm_test_options_default();

    memset(fx, 0, sizeof(*fx));
    fx->queue_depth = (opts.queue_depth != 0u) ? opts.queue_depth : FSM_TEST_DEFAULT_QUEUE_DEPTH;
    fx->max_chain_depth = opts.max_chain_depth;
    fx->max_actions_per_event = opts.max_actions_per_event;
    fx->record_events = opts.record_events;
    fx->use_global_queue = opts.global_queue;
    fx->use_clock = opts.clocked;
    fx->use_capabilities = !opts.no_capabilities;
    fx->governor_mode = FSM_TEST_GOV_APPROVE_ALL;
    fx->governor_budget = 0u;
    if (!opts.no_governor) {
        fx->governor = fsm_test_governor;
    }

    fx->bus.user_data = fx;
    fx->bus.read = fsm_test_bus_read;
    fx->bus.write = fsm_test_bus_write;
    fx->bus.resolve = fsm_test_bus_resolve;

    fx->sink.user_data = fx;
    fx->sink.on_send_message = fsm_test_sink_send_message;
    fx->sink.on_signal_write = fsm_test_sink_signal_write;
    fx->sink.on_log = fsm_test_sink_log;
    fx->sink.on_fault = fsm_test_sink_fault;
    fx->sink.on_timer = fsm_test_sink_timer;
    fx->sink.on_state_change = fsm_test_sink_state;
    fx->sink.on_warning = fsm_test_sink_warning;

    /* The demo signals every test may read or write. */
    fsm_test_install_signal(fx, "VehicleSpeed");
    fsm_test_install_signal(fx, "IgnitionState");
    fsm_test_install_signal(fx, "ClusterSpeed");
    fsm_test_install_signal(fx, "ClusterAlive");
    fsm_test_install_signal(fx, "SpeedMPH");
    fsm_test_install_signal(fx, "AlertLevel");

    if (fx->use_clock) {
        cancestry_virtual_clock_init(&fx->virtual_clock, opts.start_us);
        fx->clock = cancestry_clock_from_virtual(&fx->virtual_clock);
    }

    if (!opts.no_codec) {
        codec_caps.can_fd = true;
        fx->codec_map = cancestry_codec_map_load_checked(codec_yaml, strlen(codec_yaml),
                                                         &codec_caps, &codec_error);
        if (fx->codec_map == NULL) {
            printf("    FAIL to load fixture codec map: %s\n", codec_error.message);
            fflush(stdout);
            cancestry_test_failures++;
            return false;
        }
    }
    fx->set = fsm_test_load(yaml);
    if (fx->set == NULL) {
        return false;
    }
    if (fx->use_capabilities) {
        fsm_test_install_capabilities_ex(fx, opts.signal_read_count, opts.signal_write_count);
    }

    declared = fx->set->instance_count;
    if (declared > FSM_TEST_MAX_INSTANCES) {
        printf("    FAIL the fixture holds %u instances; this file declares %u\n",
               (unsigned)FSM_TEST_MAX_INSTANCES, (unsigned)declared);
        fflush(stdout);
        cancestry_test_failures++;
        return false;
    }

    memset(&config, 0, sizeof(config));
    for (i = 0u; i < declared; ++i) {
        config.instance_capacity++;
        fx->storage[i].event_slots = fx->queue_slots[i];
        fx->storage[i].event_capacity = fx->queue_depth;
        fx->storage[i].variables = fx->variables[i];
        fx->storage[i].variable_capacity = FSM_TEST_MAX_VARIABLES;
        fx->storage[i].timers = fx->timers[i];
        fx->storage[i].timer_capacity = FSM_TEST_MAX_TIMERS;
    }
    config.sets = fx->set;
    config.set_count = 1u;
    config.instances = fx->instances;
    config.storage = fx->storage;
    config.clock = fx->use_clock ? &fx->clock : NULL;
    config.capabilities = fx->use_capabilities ? &fx->capabilities : NULL;
    config.namespace = (fx->codec_map != NULL) ? &fx->signal_namespace : NULL;
    config.signal_bus = &fx->bus;
    config.global_queue = fx->use_global_queue ? &fx->global_queue : NULL;
    config.bindings = fx->bindings;
    config.binding_count = 2u;
    config.governor = fx->governor;
    config.governor_user_data = fx;
    config.sink = opts.no_sink ? NULL : &fx->sink;
    config.trace = fx->trace;
    config.trace_capacity = FSM_TEST_TRACE_RECORDS;
    config.max_chain_depth = fx->max_chain_depth;
    config.max_actions_per_event = fx->max_actions_per_event;
    config.max_events_per_activation = opts.max_events_per_activation;
    config.record_events = fx->record_events;

    if (fx->use_global_queue &&
        !cancestry_event_queue_init(&fx->global_queue, fx->global_slots, FSM_TEST_GLOBAL_QUEUE)) {
        printf("    FAIL to initialize the fixture global queue\n");
        fflush(stdout);
        cancestry_test_failures++;
        return false;
    }
    if (fx->codec_map != NULL) {
        if (!cancestry_codec_namespace_init(&fx->signal_namespace, fx->namespace_slots, 1u) ||
            cancestry_codec_namespace_register(&fx->signal_namespace, fx->codec_map) !=
                CANCESTRY_CODEC_OK) {
            printf("    FAIL to register the fixture codec map\n");
            fflush(stdout);
            cancestry_test_failures++;
            return false;
        }
    }
    if (!cancestry_fsm_engine_init(&fx->engine, &config)) {
        printf("    FAIL to initialize the FSM engine over the fixture storage\n");
        fflush(stdout);
        cancestry_test_failures++;
        return false;
    }
    return true;
}

/** Same as fsm_test_init_codec() with the classic demo codec map. */
static inline bool fsm_test_init_with(fsm_test_fixture_t *fx,
                                      const char *yaml,
                                      const fsm_test_options_t *options)
{
    return fsm_test_init_codec(fx, yaml, options, fsm_test_codec_yaml());
}

/**
 * Build a fixture over one FSM document.
 *
 * The default environment is what every conformance test shares: the demo codec
 * map registered as "demo", interfaces can0 (TX allowed for 0x100 and 0x200)
 * and can1 (RX only), a governor that approves everything, a recording sink, the
 * global event queue on, and the engine's internal 1 ms tick clock, so timer
 * tests need no external time source.
 */
static inline bool fsm_test_init(fsm_test_fixture_t *fx, const char *yaml)
{
    return fsm_test_init_with(fx, yaml, NULL);
}

/** Same as fsm_test_init(), driven by an explicit virtual clock. */
static inline bool fsm_test_init_clocked(fsm_test_fixture_t *fx,
                                         const char *yaml,
                                         cancestry_time_us_t start_us)
{
    fsm_test_options_t options = fsm_test_options_default();

    options.clocked = true;
    options.start_us = start_us;
    return fsm_test_init_with(fx, yaml, &options);
}

/** Same as fsm_test_init(), with a custom per-instance queue depth. */
static inline bool fsm_test_init_queue(fsm_test_fixture_t *fx,
                                       const char *yaml,
                                       uint16_t queue_depth)
{
    fsm_test_options_t options = fsm_test_options_default();

    options.queue_depth = queue_depth;
    return fsm_test_init_with(fx, yaml, &options);
}

static inline void fsm_test_destroy(fsm_test_fixture_t *fx)
{
    if (fx->set != NULL) {
        cancestry_fsm_set_free(fx->set);
        fx->set = NULL;
    }
    if (fx->codec_map != NULL) {
        cancestry_codec_map_free(fx->codec_map);
        fx->codec_map = NULL;
    }
    memset(&fx->engine, 0, sizeof(fx->engine));
}

/** Advance the virtual clock; only meaningful with fsm_test_init_clocked(). */
static inline void fsm_test_advance_clock(fsm_test_fixture_t *fx, cancestry_time_us_t delta_us)
{
    cancestry_virtual_clock_advance(&fx->virtual_clock, delta_us);
}

/* ------------------------------------------------------------------------- */
/* Instance helpers                                                          */
/* ------------------------------------------------------------------------- */

static inline cancestry_fsm_instance_t *fsm_test_instance(fsm_test_fixture_t *fx, const char *id)
{
    return cancestry_fsm_instance_by_id(&fx->engine, id);
}

static inline const char *fsm_test_state(const fsm_test_fixture_t *fx, const char *id)
{
    const cancestry_fsm_instance_t *instance =
        cancestry_fsm_instance_by_id(&fx->engine, id);

    return cancestry_fsm_instance_state_name(instance);
}

static inline cancestry_fsm_instance_lifecycle_t fsm_test_lifecycle(const fsm_test_fixture_t *fx,
                                                                    const char *id)
{
    const cancestry_fsm_instance_t *instance =
        cancestry_fsm_instance_by_id(&fx->engine, id);

    return cancestry_fsm_instance_lifecycle(instance);
}

static inline const cancestry_fsm_instance_counters_t *
fsm_test_counters(const fsm_test_fixture_t *fx, const char *id)
{
    const cancestry_fsm_instance_t *instance =
        cancestry_fsm_instance_by_id(&fx->engine, id);

    return cancestry_fsm_instance_counters(instance);
}

/** Move an instance from READY to RUNNING. */
static inline cancestry_fsm_status_t fsm_test_start(fsm_test_fixture_t *fx, const char *id)
{
    return cancestry_fsm_instance_start(&fx->engine, fsm_test_instance(fx, id));
}

/** Start every instance the declaration enabled, in declaration order. */
static inline size_t fsm_test_start_all(fsm_test_fixture_t *fx)
{
    size_t started = 0u;
    size_t i;

    for (i = 0u; i < cancestry_fsm_engine_instance_count(&fx->engine); ++i) {
        cancestry_fsm_instance_t *instance = cancestry_fsm_instance_at(&fx->engine, i);

        if (instance != NULL &&
            instance->lifecycle == CANCESTRY_FSM_INSTANCE_LIFECYCLE_READY &&
            cancestry_fsm_instance_start(&fx->engine, instance) == CANCESTRY_FSM_OK) {
            started++;
        }
    }
    return started;
}

static inline cancestry_fsm_status_t fsm_test_variable_int(const fsm_test_fixture_t *fx,
                                                           const char *id,
                                                           const char *name,
                                                           int64_t *out)
{
    cancestry_value_t value;
    cancestry_fsm_status_t status = cancestry_fsm_instance_get_variable(
        &fx->engine, cancestry_fsm_instance_by_id(&fx->engine, id), name, &value);

    if (status != CANCESTRY_FSM_OK) {
        return status;
    }
    if (value.kind == CANCESTRY_VALUE_KIND_INT) {
        *out = value.value.integer;
    } else if (value.kind == CANCESTRY_VALUE_KIND_REAL) {
        *out = (int64_t)value.value.real;
    } else if (value.kind == CANCESTRY_VALUE_KIND_BOOL) {
        *out = value.value.boolean ? 1 : 0;
    } else {
        return CANCESTRY_FSM_ERR_NOT_FOUND;
    }
    return CANCESTRY_FSM_OK;
}

/** Trace rendering, for the golden-output assertions (SW-FR-FSM-053). */
static inline size_t fsm_test_render_trace(const fsm_test_fixture_t *fx, char *buffer, size_t size)
{
    return cancestry_fsm_engine_trace_render(&fx->engine, buffer, size);
}

/* ------------------------------------------------------------------------- */
/* Event builders                                                            */
/* ------------------------------------------------------------------------- */

static inline void fsm_test_event_can_rx(cancestry_event_t *event,
                                         cancestry_time_us_t timestamp_us,
                                         cancestry_interface_id_t interface_id,
                                         uint32_t can_id,
                                         const uint8_t *data,
                                         uint8_t length)
{
    cancestry_event_init(event);
    event->type = CANCESTRY_EVENT_TYPE_CAN_RX;
    event->priority_class = CANCESTRY_PRIORITY_CLASS_CAN_RX;
    event->timestamp_us = timestamp_us;
    event->payload.can_rx.interface_id = interface_id;
    event->payload.can_rx.can_id = can_id;
    event->payload.can_rx.length = length;
    if (data != NULL) {
        memcpy(event->payload.can_rx.data, data,
               (length <= CANCESTRY_CAN_FRAME_MAX_LENGTH) ? length : 0u);
    }
}

static inline void fsm_test_event_int(cancestry_event_t *event,
                                      cancestry_time_us_t timestamp_us,
                                      const char *signal_name,
                                      int64_t value)
{
    cancestry_event_init(event);
    event->type = CANCESTRY_EVENT_TYPE_SIGNAL_CHANGED;
    event->priority_class = CANCESTRY_PRIORITY_CLASS_CAN_RX;
    event->timestamp_us = timestamp_us;
    event->payload.signal_changed.signal_name = signal_name;
    event->payload.signal_changed.new_value.kind = CANCESTRY_VALUE_KIND_INT;
    event->payload.signal_changed.new_value.value.integer = value;
}

static inline void fsm_test_event_fault(cancestry_event_t *event,
                                        cancestry_time_us_t timestamp_us,
                                        cancestry_fault_code_t code,
                                        cancestry_fault_severity_t severity)
{
    cancestry_event_init(event);
    event->type = CANCESTRY_EVENT_TYPE_FAULT_RAISED;
    event->priority_class = CANCESTRY_PRIORITY_CLASS_FAULT;
    event->timestamp_us = timestamp_us;
    event->payload.fault.fault_code = code;
    event->payload.fault.severity = severity;
    event->payload.fault.source_id = 99u;
}

static inline void fsm_test_event_mode(cancestry_event_t *event,
                                       cancestry_time_us_t timestamp_us,
                                       cancestry_mode_t from_mode,
                                       cancestry_mode_t to_mode)
{
    cancestry_event_init(event);
    event->type = CANCESTRY_EVENT_TYPE_POWER_MODE_CHANGED;
    event->priority_class = CANCESTRY_PRIORITY_CLASS_MODE;
    event->timestamp_us = timestamp_us;
    event->payload.mode.from_mode = from_mode;
    event->payload.mode.to_mode = to_mode;
}

/** Deliver an event to every active instance. */
static inline cancestry_fsm_status_t fsm_test_process(fsm_test_fixture_t *fx,
                                                      const cancestry_event_t *event)
{
    return cancestry_fsm_engine_process_event(&fx->engine, event);
}

static inline cancestry_fsm_status_t fsm_test_tick(fsm_test_fixture_t *fx)
{
    return cancestry_fsm_engine_tick(&fx->engine);
}

static inline cancestry_fsm_status_t fsm_test_ticks(fsm_test_fixture_t *fx, uint32_t count)
{
    return cancestry_fsm_engine_advance(&fx->engine, count);
}

/** @return Number of events waiting in the global queue. */
static inline uint16_t fsm_test_global_size(const fsm_test_fixture_t *fx)
{
    return cancestry_event_queue_size(&fx->global_queue);
}

#endif /* CANCESTRY_FSM_CONFORMANCE_H */
