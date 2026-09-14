/*
 * Shared helpers for the CANcestry recipe unit tests.
 *
 * Loads recipe files and codec maps from inline YAML through the real
 * loaders, and provides a recording sink plus event builders, so the engine
 * tests exercise the loaders as well.
 */

#ifndef CANCESTRY_RECIPE_TEST_H
#define CANCESTRY_RECIPE_TEST_H

#include "cancestry/codec/loader.h"
#include "cancestry/codec/namespace.h"
#include "cancestry/event/queue.h"
#include "cancestry/recipe/engine.h"
#include "cancestry/recipe/loader.h"

#include "cancestry_codec_test.h"

#include <string.h>

/** Load a recipe set from inline YAML; prints and counts a failure on error. */
static inline cancestry_recipe_set_t *cancestry_test_recipe_load(const char *yaml)
{
    cancestry_recipe_load_error_t error;
    cancestry_recipe_set_t *set = cancestry_recipe_set_load(yaml, strlen(yaml), &error);

    if (set == NULL) {
        printf("    FAIL to load test recipe set: %s (line %u, column %u)\n", error.message,
               (unsigned)error.line, (unsigned)error.column);
        fflush(stdout);
        cancestry_test_failures++;
    }
    return set;
}

/*
 * Demo codec map used by the engine and governor tests.
 *
 * Message 0x100 StatusMsg (dlc 8):
 *   VehicleSpeed  LE uint16 @ 0    (id 1 when registered first)
 *   IgnitionState LE uint8  @ 16   (id 2)
 * Message 0x200 ClusterMsg (dlc 2):
 *   ClusterSpeed  LE uint16 @ 0    (id 3)
 * Message 0x300 TinyMsg (dlc 1):
 *   WideSignal    LE uint16 @ 8    (id 4; spans bits 8..23, beyond the dlc,
 *                                   used for the truncation fail-closed test)
 */
#define TEST_MSG_STATUS ((uint32_t)0x100u)
#define TEST_MSG_CLUSTER ((uint32_t)0x200u)
#define TEST_MSG_TINY ((uint32_t)0x300u)

static const char *const cancestry_test_recipe_demo_codec_yaml =
    "schema_version: \"0.2.0\"\n"
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
    "      name: TinyMsg\n"
    "      dlc: 1\n"
    "      signals:\n"
    "        - name: WideSignal\n"
    "          start_bit: 8\n"
    "          bit_length: 16\n"
    "          type: uint\n"
    "          endianness: little\n";

/* Demo environment tables. */
#define TEST_IFACE_CAN0 ((cancestry_interface_id_t)1u)
#define TEST_IFACE_CAN1 ((cancestry_interface_id_t)2u)

static const cancestry_recipe_interface_t cancestry_test_interfaces[] = {
    {"can0", TEST_IFACE_CAN0},
    {"can1", TEST_IFACE_CAN1},
};

static const cancestry_recipe_binding_t cancestry_test_bindings[] = {
    {"car", "can0"},
    {"device", "can1"},
};

static const cancestry_recipe_timer_t cancestry_test_timers[] = {
    {"tick", 7u},
};

/* ------------------------------------------------------------------------- */
/* Recording sink                                                            */
/* ------------------------------------------------------------------------- */

#define CANCESTRY_TEST_TRACE_MAX ((size_t)64u)
#define CANCESTRY_TEST_TEXT_MAX ((size_t)256u)

typedef struct cancestry_test_trace_entry {
    const char *kind; /* "tx", "signal", "log", "fault", "timer" */
    const cancestry_recipe_t *recipe;
    uint32_t recipe_ordinal;
    /* tx */
    cancestry_interface_id_t interface_id;
    char interface_name[CANCESTRY_TEST_TEXT_MAX];
    uint32_t can_id;
    uint8_t frame[CANCESTRY_CAN_FRAME_MAX_LENGTH];
    uint8_t frame_length;
    /* signal */
    cancestry_signal_id_t signal_id;
    char signal_name[CANCESTRY_TEST_TEXT_MAX];
    cancestry_value_t old_value;
    cancestry_value_t new_value;
    /* log / fault */
    cancestry_recipe_log_level_t level;
    char message[CANCESTRY_TEST_TEXT_MAX];
    char code[CANCESTRY_TEST_TEXT_MAX];
    cancestry_fault_severity_t severity;
    cancestry_fault_code_t numeric_code;
    /* timer */
    cancestry_recipe_action_kind_t timer_kind;
    char timer_name[CANCESTRY_TEST_TEXT_MAX];
    uint32_t duration_ms;
    bool repeat;
} cancestry_test_trace_entry_t;

typedef struct cancestry_test_trace {
    cancestry_test_trace_entry_t entries[CANCESTRY_TEST_TRACE_MAX];
    size_t count;
} cancestry_test_trace_t;

static inline void cancestry_test_trace_reset(cancestry_test_trace_t *trace)
{
    memset(trace, 0, sizeof(*trace));
}

static inline void cancestry_test_trace_copy(char *destination, size_t capacity, const char *text)
{
    if (text == NULL) {
        destination[0] = '\0';
        return;
    }
    {
        size_t i;
        for (i = 0u; i + 1u < capacity && text[i] != '\0'; ++i) {
            destination[i] = text[i];
        }
        destination[i] = '\0';
    }
}

static inline cancestry_test_trace_entry_t *cancestry_test_trace_next(void *user_data)
{
    cancestry_test_trace_t *trace = (cancestry_test_trace_t *)user_data;

    if (trace->count >= CANCESTRY_TEST_TRACE_MAX) {
        return NULL;
    }
    memset(&trace->entries[trace->count], 0, sizeof(trace->entries[trace->count]));
    return &trace->entries[trace->count++];
}

#define CANCESTRY_TEST_TRACE_FILL(entry, invocation)                       \
    do {                                                                   \
        (entry)->recipe = (invocation)->recipe;                            \
        (entry)->recipe_ordinal = (invocation)->recipe_ordinal;            \
    } while (0)

static inline void cancestry_test_on_send_message(void *user_data,
                                                  const cancestry_recipe_invocation_t *invocation,
                                                  cancestry_interface_id_t interface_id,
                                                  const char *interface_name,
                                                  uint32_t can_id,
                                                  const uint8_t *frame,
                                                  uint8_t frame_length)
{
    cancestry_test_trace_entry_t *entry = cancestry_test_trace_next(user_data);

    if (entry == NULL) {
        return;
    }
    CANCESTRY_TEST_TRACE_FILL(entry, invocation);
    entry->kind = "tx";
    entry->interface_id = interface_id;
    cancestry_test_trace_copy(entry->interface_name, sizeof(entry->interface_name), interface_name);
    entry->can_id = can_id;
    entry->frame_length = frame_length;
    memcpy(entry->frame, frame, frame_length);
}

static inline void cancestry_test_on_signal_set(void *user_data,
                                                const cancestry_recipe_invocation_t *invocation,
                                                cancestry_signal_id_t signal_id,
                                                const char *signal_name,
                                                const cancestry_value_t *old_value,
                                                const cancestry_value_t *new_value)
{
    cancestry_test_trace_entry_t *entry = cancestry_test_trace_next(user_data);

    if (entry == NULL) {
        return;
    }
    CANCESTRY_TEST_TRACE_FILL(entry, invocation);
    entry->kind = "signal";
    entry->signal_id = signal_id;
    cancestry_test_trace_copy(entry->signal_name, sizeof(entry->signal_name), signal_name);
    entry->old_value = *old_value;
    entry->new_value = *new_value;
}

static inline void cancestry_test_on_log(void *user_data,
                                         const cancestry_recipe_invocation_t *invocation,
                                         cancestry_recipe_log_level_t level,
                                         const char *message)
{
    cancestry_test_trace_entry_t *entry = cancestry_test_trace_next(user_data);

    if (entry == NULL) {
        return;
    }
    CANCESTRY_TEST_TRACE_FILL(entry, invocation);
    entry->kind = "log";
    entry->level = level;
    cancestry_test_trace_copy(entry->message, sizeof(entry->message), message);
}

static inline void cancestry_test_on_fault(void *user_data,
                                           const cancestry_recipe_invocation_t *invocation,
                                           const char *code,
                                           cancestry_fault_severity_t severity,
                                           cancestry_fault_code_t numeric_code)
{
    cancestry_test_trace_entry_t *entry = cancestry_test_trace_next(user_data);

    if (entry == NULL) {
        return;
    }
    CANCESTRY_TEST_TRACE_FILL(entry, invocation);
    entry->kind = "fault";
    cancestry_test_trace_copy(entry->code, sizeof(entry->code), code);
    entry->severity = severity;
    entry->numeric_code = numeric_code;
}

static inline void cancestry_test_on_timer(void *user_data,
                                           const cancestry_recipe_invocation_t *invocation,
                                           cancestry_recipe_action_kind_t kind,
                                           const char *timer_name,
                                           uint32_t duration_ms,
                                           bool repeat)
{
    cancestry_test_trace_entry_t *entry = cancestry_test_trace_next(user_data);

    if (entry == NULL) {
        return;
    }
    CANCESTRY_TEST_TRACE_FILL(entry, invocation);
    entry->kind = "timer";
    entry->timer_kind = kind;
    cancestry_test_trace_copy(entry->timer_name, sizeof(entry->timer_name), timer_name);
    entry->duration_ms = duration_ms;
    entry->repeat = repeat;
}

/** Attach all recording callbacks to @p sink backed by @p trace. */
static inline void cancestry_test_sink_record(cancestry_recipe_sink_t *sink,
                                              cancestry_test_trace_t *trace)
{
    memset(sink, 0, sizeof(*sink));
    sink->user_data = trace;
    sink->on_send_message = cancestry_test_on_send_message;
    sink->on_signal_set = cancestry_test_on_signal_set;
    sink->on_log = cancestry_test_on_log;
    sink->on_fault = cancestry_test_on_fault;
    sink->on_timer = cancestry_test_on_timer;
}

/* ------------------------------------------------------------------------- */
/* Event builders                                                            */
/* ------------------------------------------------------------------------- */

static inline cancestry_event_t cancestry_test_can_rx_event(uint32_t can_id,
                                                            cancestry_interface_id_t interface_id,
                                                            cancestry_time_us_t timestamp_us,
                                                            cancestry_sequence_t sequence)
{
    cancestry_event_t event;

    cancestry_event_init(&event);
    event.type = CANCESTRY_EVENT_TYPE_CAN_RX;
    event.priority_class = CANCESTRY_PRIORITY_CLASS_CAN_RX;
    event.timestamp_us = timestamp_us;
    event.sequence = sequence;
    event.payload.can_rx.interface_id = interface_id;
    event.payload.can_rx.can_id = can_id;
    event.payload.can_rx.length = 8u;
    return event;
}

static inline cancestry_event_t cancestry_test_signal_event(cancestry_signal_id_t signal_id,
                                                            const char *signal_name,
                                                            cancestry_value_t new_value,
                                                            cancestry_time_us_t timestamp_us,
                                                            cancestry_sequence_t sequence)
{
    cancestry_event_t event;

    cancestry_event_init(&event);
    event.type = CANCESTRY_EVENT_TYPE_SIGNAL_CHANGED;
    event.priority_class = CANCESTRY_PRIORITY_CLASS_CAN_RX;
    event.timestamp_us = timestamp_us;
    event.sequence = sequence;
    event.payload.signal_changed.signal_id = signal_id;
    event.payload.signal_changed.signal_name = signal_name;
    event.payload.signal_changed.new_value = new_value;
    return event;
}

static inline cancestry_value_t cancestry_test_uint_value(uint64_t value)
{
    cancestry_value_t out;

    memset(&out, 0, sizeof(out));
    out.kind = CANCESTRY_VALUE_KIND_UINT;
    out.value.unsigned_integer = value;
    return out;
}

static inline cancestry_value_t cancestry_test_int_value(int64_t value)
{
    cancestry_value_t out;

    memset(&out, 0, sizeof(out));
    out.kind = CANCESTRY_VALUE_KIND_INT;
    out.value.integer = value;
    return out;
}

#endif /* CANCESTRY_RECIPE_TEST_H */
