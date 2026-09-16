/*
 * CANcestry - portable FSM runtime: engine.
 *
 * Implementation notes:
 *   - Structure follows docs/software/SwAD.md section 4: an instance manager, an
 *     event dispatcher, a guard evaluator (expression.c), an action executor, a
 *     timer manager, a variable store and a trace logger. Each of them is a
 *     static function over the caller-owned engine struct; there is no global
 *     state, so two engines are independent (SYS-NF-001).
 *   - Nothing here allocates. The only external calls are into the codec
 *     encoder (allocation-free by contract) and vsnprintf for trace records,
 *     which writes into caller storage. CI symbol-scans this archive for
 *     allocator references (SYS-NF-002).
 *   - The clock is read only in cancestry_fsm_engine_tick(). Event processing is
 *     therefore a pure function of (definitions, instance state, event, tick
 *     history): the same event sequence and time base reproduce the same
 *     transitions, the same side effects and the same trace byte for byte
 *     (SW-FR-FSM-046).
 *   - Suspension and FAULT take effect at the end of the current step, never in
 *     the middle of an action sequence, so no half-executed side effect is left
 *     behind.
 *   - Effects never run before the governor approves them (SW-FR-FSM-042), and
 *     the capability check runs before the governor is consulted
 *     (SW-FR-FSM-039..041): an undeclared permission is not a request the
 *     governor should even consider.
 */

#include "cancestry/fsm/engine.h"

#include "cancestry/codec/encoder.h"
#include "fsm_expression.h"

#include <inttypes.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------------- */
/* Small helpers                                                             */
/* ------------------------------------------------------------------------- */

static bool fsm_name_equals(const char *text, const char *name)
{
    return text != NULL && name != NULL && strcmp(text, name) == 0;
}

static bool fsm_in_name_list(const char *const *list, uint16_t count, const char *name)
{
    uint16_t i;

    if (list == NULL || name == NULL) {
        return false;
    }
    for (i = 0u; i < count; ++i) {
        if (fsm_name_equals(list[i], name)) {
            return true;
        }
    }
    return false;
}

static bool fsm_in_id_list(const uint32_t *list, uint16_t count, uint32_t value)
{
    uint16_t i;

    if (list == NULL) {
        return false;
    }
    for (i = 0u; i < count; ++i) {
        if (list[i] == value) {
            return true;
        }
    }
    return false;
}

/** Saturating microsecond addition, so a long-running timer never wraps. */
static cancestry_time_us_t fsm_sat_add(cancestry_time_us_t base, cancestry_time_us_t delta)
{
    if (base > CANCESTRY_TIME_US_MAX - delta) {
        return CANCESTRY_TIME_US_MAX;
    }
    return base + delta;
}

/** Milliseconds to microseconds, saturating at the timestamp maximum. */
static cancestry_time_us_t fsm_us(uint32_t milliseconds)
{
    if (milliseconds > (uint32_t)(CANCESTRY_TIME_US_MAX / 1000u)) {
        return CANCESTRY_TIME_US_MAX;
    }
    return (cancestry_time_us_t)milliseconds * 1000u;
}

static const char *fsm_severity_name(cancestry_fault_severity_t severity)
{
    switch (severity) {
    case CANCESTRY_FAULT_SEVERITY_INFO:
        return "info";
    case CANCESTRY_FAULT_SEVERITY_WARNING:
        return "warning";
    case CANCESTRY_FAULT_SEVERITY_ERROR:
        return "error";
    case CANCESTRY_FAULT_SEVERITY_CRITICAL:
        return "critical";
    default:
        return "unknown";
    }
}

cancestry_fault_code_t cancestry_fsm_fault_code_hash(const char *code)
{
    uint32_t hash = 2166136261u; /* FNV-1a 32-bit offset basis */
    size_t i;

    if (code == NULL) {
        return 1u;
    }
    for (i = 0u; code[i] != '\0'; ++i) {
        hash ^= (uint32_t)(unsigned char)code[i];
        hash *= 16777619u; /* unsigned wrap is defined */
    }
    /* 0 is reserved for CANCESTRY_ID_NONE; remap it deterministically. */
    return (hash == 0u) ? 1u : (cancestry_fault_code_t)hash;
}

/** Round half away from zero, matching core/codec and core/recipe. */
static bool fsm_round_half_away(double value, int64_t *out)
{
    int64_t base;
    double truncated;

    if (!(value >= -9223372036854775808.0 && value < 9223372036854775808.0)) {
        return false;
    }
    base = (int64_t)value;
    truncated = (double)base;
    if (value > truncated) {
        if ((value - truncated) >= 0.5 && base < INT64_MAX) {
            base++;
        }
    } else if (truncated > value) {
        if ((truncated - value) >= 0.5 && base > INT64_MIN) {
            base--;
        }
    }
    *out = base;
    return true;
}

/** Normalise a literal or bus-supplied value into the runtime value model. */
static cancestry_value_t fsm_value_normalize(const cancestry_value_t *in)
{
    cancestry_value_t out = *in;

    if (in->kind == CANCESTRY_VALUE_KIND_UINT) {
        if (in->value.unsigned_integer <= (uint64_t)INT64_MAX) {
            out.kind = CANCESTRY_VALUE_KIND_INT;
            out.value.integer = (int64_t)in->value.unsigned_integer;
        } else {
            out.kind = CANCESTRY_VALUE_KIND_REAL;
            out.value.real = (double)in->value.unsigned_integer;
        }
    }
    return out;
}

/* ------------------------------------------------------------------------- */
/* Trace logger (SW-FR-FSM-048, SW-FR-FSM-049, SW-FR-FSM-053)                */
/* ------------------------------------------------------------------------- */

static void fsm_trace(cancestry_fsm_engine_t *engine,
                      const cancestry_fsm_instance_t *instance,
                      cancestry_fsm_trace_kind_t kind,
                      int32_t code,
                      const char *format,
                      ...)
{
    cancestry_fsm_trace_record_t *record;
    size_t slot;
    va_list args;

    if (engine == NULL || engine->trace == NULL || engine->trace_capacity == 0u) {
        return;
    }
    slot = engine->trace_next;
    if (engine->trace_count < engine->trace_capacity) {
        engine->trace_count++;
    } else {
        /* Ring: the oldest record is discarded, and the loss is counted. */
        engine->trace_dropped++;
    }
    record = &engine->trace[slot];
    memset(record, 0, sizeof(*record));
    record->timestamp_us = engine->now_us;
    record->instance_id = (instance != NULL) ? instance->id : CANCESTRY_ID_NONE;
    record->kind = kind;
    record->code = code;
    if (format != NULL) {
        va_start(args, format);
        (void)vsnprintf(record->detail, sizeof(record->detail), format, args);
        va_end(args);
    }
    engine->trace_next =
        (uint16_t)(((size_t)slot + 1u >= (size_t)engine->trace_capacity) ? 0u : slot + 1u);
}

static size_t fsm_trace_index_at(const cancestry_fsm_engine_t *engine, size_t index)
{
    size_t start = (engine->trace_count == engine->trace_capacity)
                       ? (size_t)engine->trace_next
                       : 0u;

    return (start + index) % (size_t)engine->trace_capacity;
}

size_t cancestry_fsm_engine_trace_count(const cancestry_fsm_engine_t *engine)
{
    if (engine == NULL || engine->trace == NULL) {
        return 0u;
    }
    return engine->trace_count;
}

const cancestry_fsm_trace_record_t *cancestry_fsm_engine_trace_record(
    const cancestry_fsm_engine_t *engine, size_t index)
{
    if (engine == NULL || engine->trace == NULL || index >= engine->trace_count) {
        return NULL;
    }
    return &engine->trace[fsm_trace_index_at(engine, index)];
}

uint32_t cancestry_fsm_engine_trace_dropped(const cancestry_fsm_engine_t *engine)
{
    return (engine == NULL) ? 0u : engine->trace_dropped;
}

void cancestry_fsm_engine_trace_clear(cancestry_fsm_engine_t *engine)
{
    if (engine == NULL) {
        return;
    }
    engine->trace_count = 0u;
    engine->trace_next = 0u;
}

size_t cancestry_fsm_engine_trace_render(const cancestry_fsm_engine_t *engine,
                                         char *buffer,
                                         size_t size)
{
    size_t written = 0u;
    size_t i;

    if (engine == NULL || buffer == NULL || size == 0u || engine->trace == NULL) {
        if (buffer != NULL && size > 0u) {
            buffer[0] = '\0';
        }
        return 0u;
    }
    buffer[0] = '\0';
    for (i = 0u; i < engine->trace_count; ++i) {
        const cancestry_fsm_trace_record_t *record =
            &engine->trace[fsm_trace_index_at(engine, i)];
        int used;

        if (written + 1u >= size) {
            break;
        }
        used = snprintf(buffer + written,
                        size - written,
                        "%" PRIu64 " %u %s %d %s\n",
                        (uint64_t)record->timestamp_us,
                        (unsigned)record->instance_id,
                        cancestry_fsm_trace_kind_name(record->kind),
                        (int)record->code,
                        record->detail);
        if (used <= 0) {
            break;
        }
        written += (size_t)used;
    }
    return written;
}

/* ------------------------------------------------------------------------- */
/* Reference resolution (all allocation-free, all fail closed)               */
/* ------------------------------------------------------------------------- */

static const cancestry_fsm_interface_capability_t *fsm_capability_interface(
    const cancestry_fsm_engine_t *engine, const char *name)
{
    uint16_t i;

    if (engine == NULL || engine->capabilities == NULL || name == NULL) {
        return NULL;
    }
    for (i = 0u; i < engine->capabilities->interface_count; ++i) {
        if (fsm_name_equals(engine->capabilities->interfaces[i].name, name)) {
            return &engine->capabilities->interfaces[i];
        }
    }
    return NULL;
}

/**
 * Resolve an interface name in the order docs/packages/package-spec.md
 * section 5 defines: physical name, then instance binding, then package
 * binding. A binding whose target is not a declared capability interface does
 * not resolve: an alias may not smuggle traffic onto an undeclared interface.
 */
static const cancestry_fsm_interface_capability_t *fsm_resolve_interface(
    const cancestry_fsm_engine_t *engine,
    const cancestry_fsm_instance_t *instance,
    const char *name,
    const char **canonical_out)
{
    const cancestry_fsm_interface_capability_t *capability;
    uint16_t i;

    if (canonical_out != NULL) {
        *canonical_out = NULL;
    }
    if (name == NULL) {
        return NULL;
    }
    capability = fsm_capability_interface(engine, name);
    if (capability != NULL) {
        if (canonical_out != NULL) {
            *canonical_out = capability->name;
        }
        return capability;
    }
    if (instance != NULL && instance->def != NULL) {
        for (i = 0u; i < instance->def->binding_count; ++i) {
            if (fsm_name_equals(instance->def->bindings[i].alias, name)) {
                capability = fsm_capability_interface(engine, instance->def->bindings[i].physical);
                if (capability != NULL && canonical_out != NULL) {
                    *canonical_out = capability->name;
                }
                return capability;
            }
        }
    }
    if (engine->bindings != NULL) {
        for (i = 0u; i < engine->binding_count; ++i) {
            if (fsm_name_equals(engine->bindings[i].alias, name)) {
                capability = fsm_capability_interface(engine, engine->bindings[i].physical);
                if (capability != NULL && canonical_out != NULL) {
                    *canonical_out = capability->name;
                }
                return capability;
            }
        }
    }
    return NULL;
}

/** Resolve a message name through the codec namespace (short or canonical). */
static cancestry_fsm_status_t fsm_resolve_message(const cancestry_fsm_engine_t *engine,
                                                  const char *name,
                                                  const cancestry_codec_map_t **map_out,
                                                  const cancestry_codec_message_t **message_out)
{
    const cancestry_codec_message_t *found = NULL;
    const cancestry_codec_map_t *found_map = NULL;
    const char *dot;
    uint16_t i;

    if (name == NULL) {
        return CANCESTRY_FSM_ERR_NULL;
    }
    if (engine->namespace == NULL || !cancestry_codec_namespace_is_valid(engine->namespace)) {
        /* Without a codec namespace a message name has no meaning: no CAN id,
         * no bit layout, no way to check the TX allowlist. Fail closed. */
        return CANCESTRY_FSM_ERR_NOT_FOUND;
    }
    dot = strchr(name, '.');
    for (i = 0u; i < engine->namespace->count; ++i) {
        const cancestry_codec_map_t *candidate = engine->namespace->slots[i];
        uint16_t m;

        if (candidate == NULL) {
            continue;
        }
        for (m = 0u; m < candidate->message_count; ++m) {
            const cancestry_codec_message_t *message = &candidate->messages[m];

            if (dot != NULL) {
                size_t map_length = (size_t)(dot - name);

                if (strlen(candidate->name) != map_length ||
                    memcmp(candidate->name, name, map_length) != 0) {
                    continue;
                }
                if (!fsm_name_equals(message->name, dot + 1)) {
                    continue;
                }
            } else if (!fsm_name_equals(message->name, name)) {
                continue;
            }
            if (found != NULL) {
                /* A short name defined by two registered maps is ambiguous,
                 * exactly as in the signal namespace. */
                return CANCESTRY_FSM_ERR_AMBIGUOUS;
            }
            found = message;
            found_map = candidate;
        }
    }
    if (found == NULL) {
        return CANCESTRY_FSM_ERR_NOT_FOUND;
    }
    if (map_out != NULL) {
        *map_out = found_map;
    }
    if (message_out != NULL) {
        *message_out = found;
    }
    return CANCESTRY_FSM_OK;
}

static int fsm_variable_index(const cancestry_fsm_instance_t *instance, const char *name)
{
    uint16_t i;

    if (instance == NULL || name == NULL) {
        return -1;
    }
    for (i = 0u; i < instance->variable_count; ++i) {
        if (fsm_name_equals(instance->variables[i].name, name)) {
            return (int)i;
        }
    }
    return -1;
}

static int fsm_timer_index(const cancestry_fsm_instance_t *instance, const char *name)
{
    uint16_t i;

    if (instance == NULL || name == NULL) {
        return -1;
    }
    for (i = 0u; i < instance->timer_count; ++i) {
        if (fsm_name_equals(instance->timers[i].name, name)) {
            return (int)i;
        }
    }
    return -1;
}

static const cancestry_fsm_state_t *fsm_current_state(const cancestry_fsm_instance_t *instance)
{
    if (instance == NULL || !instance->has_state || instance->machine == NULL) {
        return NULL;
    }
    return &instance->machine->states[instance->state_index];
}

/* ------------------------------------------------------------------------- */
/* Expression evaluation                                                     */
/* ------------------------------------------------------------------------- */

/** Resolution context shared by guards and operand expressions. */
typedef struct fsm_eval_env {
    const cancestry_fsm_engine_t *engine;
    const cancestry_fsm_instance_t *instance;
} fsm_eval_env_t;

static cancestry_fsm_status_t fsm_eval_read_signal(void *user_data,
                                                   const char *name,
                                                   cancestry_value_t *value_out)
{
    const fsm_eval_env_t *env = (const fsm_eval_env_t *)user_data;
    const cancestry_fsm_capabilities_t *capabilities = env->engine->capabilities;

    if (value_out == NULL) {
        return CANCESTRY_FSM_ERR_NOT_FOUND;
    }
    /*
     * sig.* access is capability-gated before the bus is touched: an
     * unauthorized read is an expression fault
     * (docs/system/expression-language.md section 5), never a silent zero. Note
     * that NULL capabilities mean "no reads", which is the fail-closed reading
     * of "a state machine shall only perform actions allowed by package
     * capabilities" (SW-FR-FSM-039) applied to observation as well.
     */
    if (!fsm_in_name_list(capabilities == NULL ? NULL : capabilities->signal_read,
                          capabilities == NULL ? 0u : capabilities->signal_read_count, name)) {
        return CANCESTRY_FSM_ERR_DENIED;
    }
    if (env->engine->signal_bus == NULL || env->engine->signal_bus->read == NULL) {
        return CANCESTRY_FSM_ERR_NOT_FOUND;
    }
    if (env->engine->signal_bus->read(env->engine->signal_bus->user_data, name, value_out) !=
        CANCESTRY_FSM_OK) {
        return CANCESTRY_FSM_ERR_NOT_FOUND;
    }
    *value_out = fsm_value_normalize(value_out);
    return CANCESTRY_FSM_OK;
}

static cancestry_fsm_status_t fsm_eval_read_variable(void *user_data,
                                                    const char *name,
                                                    cancestry_value_t *value_out)
{
    const fsm_eval_env_t *env = (const fsm_eval_env_t *)user_data;
    int index = fsm_variable_index(env->instance, name);

    if (index < 0 || value_out == NULL) {
        return CANCESTRY_FSM_ERR_NOT_FOUND;
    }
    if (!env->instance->variables[index].initialized) {
        /* Reading an uninitialized variable is undefined, not zero. */
        return CANCESTRY_FSM_ERR_NOT_FOUND;
    }
    *value_out = env->instance->variables[index].value;
    return CANCESTRY_FSM_OK;
}

static void fsm_expression_context(cancestry_fsm_engine_t *engine,
                                   cancestry_fsm_instance_t *instance,
                                   const cancestry_event_t *event,
                                   fsm_eval_env_t *env,
                                   cancestry_fsm_expression_context_t *context)
{
    memset(env, 0, sizeof(*env));
    env->engine = engine;
    env->instance = instance;
    memset(context, 0, sizeof(*context));
    context->user_data = env;
    context->read_signal = fsm_eval_read_signal;
    context->read_variable = fsm_eval_read_variable;
    context->event = event;
}

/* ------------------------------------------------------------------------- */
/* Activation context                                                        */
/* ------------------------------------------------------------------------- */

typedef struct fsm_activation {
    cancestry_fsm_engine_t *engine;
    cancestry_fsm_instance_t *instance;
    /** The event being processed; NULL for the initial entry. */
    const cancestry_event_t *event;
    /** Remaining action budget for this event (SW-FR-FSM-045). */
    uint16_t action_budget;
    /** Set by an expression fault or a chain-limit refusal. */
    bool suspend_requested;
    /** Reason of the last expression fault, for the trace record. */
    cancestry_fsm_expression_error_t expression_error;
} fsm_activation_t;

static cancestry_time_us_t fsm_activation_timestamp(const fsm_activation_t *act)
{
    if (act->event != NULL) {
        return act->event->timestamp_us;
    }
    return act->engine->now_us;
}

static cancestry_sequence_t fsm_activation_cause(const fsm_activation_t *act)
{
    if (act->event != NULL) {
        return act->event->sequence;
    }
    return CANCESTRY_SEQUENCE_NONE;
}

static void fsm_invocation_fill(cancestry_fsm_invocation_t *invocation,
                                fsm_activation_t *act,
                                const cancestry_fsm_action_t *action,
                                const cancestry_fsm_transition_t *transition,
                                const char *state,
                                uint16_t chain_depth)
{
    invocation->engine = act->engine;
    invocation->instance = act->instance;
    invocation->def = act->instance->def;
    invocation->instance_id = act->instance->id;
    invocation->event = act->event;
    invocation->action = action;
    invocation->state = state;
    invocation->transition = transition;
    invocation->chain_depth = chain_depth;
}

/** Stamp an event id for an event this engine produces (event-ordering.md 10). */
static cancestry_event_id_t fsm_next_event_id(cancestry_fsm_engine_t *engine)
{
    cancestry_event_id_t id = engine->counters.next_event_id;

    engine->counters.next_event_id = (id + 1u == CANCESTRY_EVENT_ID_NONE) ? 1u : (id + 1u);
    return id;
}

static void fsm_warn(fsm_activation_t *act, const char *text)
{
    const cancestry_fsm_sink_t *sink = act->engine->sink;

    fsm_trace(act->engine, act->instance, CANCESTRY_FSM_TRACE_WARNING, 0, "%s", text);
    if (sink != NULL && sink->on_warning != NULL) {
        cancestry_fsm_invocation_t invocation;

        fsm_invocation_fill(&invocation, act, NULL, NULL, NULL, 0u);
        sink->on_warning(sink->user_data, &invocation, text);
    }
}

/* ------------------------------------------------------------------------- */
/* Event delivery (docs/system/event-ordering.md sections 7, 8, 9)           */
/* ------------------------------------------------------------------------- */

static void fsm_queue_event(fsm_activation_t *act, const cancestry_event_t *event)
{
    cancestry_event_queue_status_t status = cancestry_event_queue_push(&act->instance->incoming,
                                                                       event);

    if (cancestry_event_queue_status_is_ok(status)) {
        act->instance->counters.events_queued++;
    } else {
        /* Overflow policy: drop-newest for non-fault events (SW-FR-FSM-020). */
        act->instance->counters.events_dropped++;
    }
}

static void fsm_publish(fsm_activation_t *act, const cancestry_event_t *event)
{
    cancestry_event_queue_t *global = act->engine->global_queue;

    if (global == NULL || !cancestry_event_queue_is_valid(global)) {
        return;
    }
    if (!cancestry_event_queue_status_is_ok(cancestry_event_queue_push(global, event))) {
        act->engine->counters.global_queue_drops++;
    }
}

/**
 * Emit a generated event: GENERATED priority class (FAULT for faults),
 * cause_sequence linked, timestamp inherited from the causing event
 * (event-ordering.md section 8). It is published to the global queue when one
 * is configured, and queued for the owning instance only when the instance
 * declares that subscription, so an FSM cannot loop on its own output unless
 * the declaration asks for it. Generated events are never applied
 * recursively.
 */
static void fsm_emit_generated(fsm_activation_t *act,
                               cancestry_event_type_t type,
                               const cancestry_event_payload_t *payload,
                               bool self_subscribe)
{
    cancestry_event_t event;

    cancestry_event_init(&event);
    /* One identity for one event, however many queues receive it
     * (event-ordering.md section 10). */
    event.event_id = fsm_next_event_id(act->engine);
    event.type = type;
    event.priority_class = (type == CANCESTRY_EVENT_TYPE_FAULT_RAISED)
                               ? CANCESTRY_PRIORITY_CLASS_FAULT
                               : CANCESTRY_PRIORITY_CLASS_GENERATED;
    event.timestamp_us = fsm_activation_timestamp(act);
    event.cause_sequence = fsm_activation_cause(act);
    if (payload != NULL) {
        event.payload = *payload;
    }
    fsm_publish(act, &event);
    if (self_subscribe) {
        fsm_queue_event(act, &event);
    }
}

static bool fsm_subscribes_signal(fsm_activation_t *act, const char *name)
{
    const cancestry_fsm_subscriptions_t *subscriptions = &act->instance->def->subscriptions;
    uint16_t i;

    if (!subscriptions->has_signals || name == NULL) {
        return false;
    }
    for (i = 0u; i < subscriptions->signal_count; ++i) {
        if (fsm_name_equals(subscriptions->signals[i], name)) {
            return true;
        }
    }
    return false;
}

static void fsm_emit_state_event(fsm_activation_t *act,
                                 cancestry_event_type_t type,
                                 const cancestry_fsm_state_t *state)
{
    cancestry_event_t event;
    cancestry_event_payload_t payload;

    memset(&payload, 0, sizeof(payload));
    payload.state.instance_id = act->instance->id;
    payload.state.state_id = (cancestry_state_id_t)(state - act->instance->machine->states) + 1u;
    payload.state.state_name = state->name;

    cancestry_event_init(&event);
    event.event_id = fsm_next_event_id(act->engine);
    event.type = type;
    event.priority_class = CANCESTRY_PRIORITY_CLASS_GENERATED;
    event.timestamp_us = fsm_activation_timestamp(act);
    event.cause_sequence = fsm_activation_cause(act);
    event.payload = payload;
    /* State events are queued for the owning instance, never applied
     * recursively: an FSM that reacts to its own entry has to declare a
     * state_entered transition, which then runs as an ordinary queued event
     * (bounded by the chain limit and the activation budget). */
    fsm_publish(act, &event);
    fsm_queue_event(act, &event);
}

/* ------------------------------------------------------------------------- */
/* Timers (SW-FR-FSM-026..030, fsm-spec.md section 9)                        */
/* ------------------------------------------------------------------------- */

static void fsm_timer_arm(cancestry_fsm_engine_t *engine,
                          cancestry_fsm_timer_state_t *slot,
                          uint32_t duration_ms,
                          bool repeat)
{
    slot->duration_ms = duration_ms;
    slot->repeat = repeat;
    slot->deadline_us = fsm_sat_add(engine->now_us, fsm_us(duration_ms));
    slot->running = true;
}

/**
 * Evaluate one timer at @p now.
 *
 * Periodic timers advance a scheduled deadline instead of re-basing on "now",
 * which keeps a long-running periodic timer drift-free. When the clock moved
 * past several periods at once, one event is emitted and the skipped periods
 * are reported in missed_count (fsm-spec.md section 9 and the "Timer overrun"
 * row of mode-fault-state-machine.md section 5).
 */
static bool fsm_timer_expired(cancestry_fsm_instance_t *instance,
                              cancestry_fsm_timer_state_t *slot,
                              cancestry_time_us_t now,
                              uint32_t *missed_out)
{
    cancestry_time_us_t duration_us = fsm_us(slot->duration_ms);
    uint32_t periods = 1u;

    *missed_out = 0u;
    if (!slot->running || now < slot->deadline_us) {
        return false;
    }
    if (duration_us == 0u) {
        duration_us = 1000u; /* the schema minimum is 1 ms; defensive */
    }
    if (slot->repeat) {
        periods = 1u + (uint32_t)((now - slot->deadline_us) / duration_us);
        *missed_out = periods - 1u;
        slot->deadline_us = fsm_sat_add(slot->deadline_us, duration_us * (cancestry_time_us_t)periods);
    } else {
        slot->running = false;
    }
    slot->expires++;
    slot->missed_ticks += *missed_out;
    instance->counters.timer_expiries++;
    instance->counters.timer_missed_ticks += *missed_out;
    return true;
}

/* ------------------------------------------------------------------------- */
/* Guards and transition selection (SW-FR-FSM-010..012)                      */
/* ------------------------------------------------------------------------- */

static cancestry_fsm_status_t fsm_evaluate_guard(fsm_activation_t *act,
                                                 const char *guard,
                                                 bool *result_out)
{
    cancestry_fsm_engine_t *engine = act->engine;
    cancestry_fsm_instance_t *instance = act->instance;
    fsm_eval_env_t env;
    cancestry_fsm_expression_context_t context;
    cancestry_fsm_expression_error_t error = CANCESTRY_FSM_EXPRESSION_OK;
    cancestry_value_t value;

    memset(&value, 0, sizeof(value));
    act->expression_error = error;
    if (guard == NULL) {
        *result_out = true;
        return CANCESTRY_FSM_OK;
    }
    memset(&env, 0, sizeof(env));
    instance->counters.guards_evaluated++;
    fsm_expression_context(engine, instance, act->event, &env, &context);
    if (!cancestry_fsm_expression_evaluate(guard, &context, &value, &error) ||
        !cancestry_fsm_expression_result_is_guard(&value)) {
        instance->counters.guard_errors++;
        /*
         * A failed guard is an expression fault, and the normative reaction to
         * an expression evaluation error is to suspend the FSM
         * (docs/system/mode-fault-state-machine.md section 5). Treating it as
         * "false" would let a typo silently disable a safety transition, so the
         * fault is loud instead (SW-FR-FSM-038).
         */
        act->suspend_requested = true;
        if (error == CANCESTRY_FSM_EXPRESSION_ERR_UNAUTHORIZED) {
            instance->counters.capability_denials++;
        }
        fsm_trace(engine, instance, CANCESTRY_FSM_TRACE_GUARD_ERROR, (int32_t)error, "%s: %s",
                  guard, cancestry_fsm_expression_error_name(error));
        return CANCESTRY_FSM_ERR_EXPRESSION;
    }
    *result_out = value.value.boolean;
    if (*result_out) {
        instance->counters.guards_true++;
        fsm_trace(engine, instance, CANCESTRY_FSM_TRACE_GUARD_TRUE, 0, "%s", guard);
    } else {
        instance->counters.guards_false++;
        fsm_trace(engine, instance, CANCESTRY_FSM_TRACE_GUARD_FALSE, 0, "%s", guard);
    }
    return CANCESTRY_FSM_OK;
}

/** can_rx filter matching: interface and message names, both optional. */
static bool fsm_match_can_rx(fsm_activation_t *act,
                             const cancestry_fsm_transition_t *transition,
                             const cancestry_event_t *event)
{
    if (transition->interface != NULL) {
        const cancestry_fsm_interface_capability_t *capability =
            fsm_resolve_interface(act->engine, act->instance, transition->interface, NULL);

        if (capability == NULL || capability->id != event->payload.can_rx.interface_id) {
            return false;
        }
    }
    if (transition->message != NULL) {
        const cancestry_codec_message_t *message = NULL;

        if (fsm_resolve_message(act->engine, transition->message, NULL, &message) !=
                CANCESTRY_FSM_OK ||
            message == NULL || message->id != event->payload.can_rx.can_id) {
            return false;
        }
    }
    return true;
}

static bool fsm_transition_matches(fsm_activation_t *act,
                                   const cancestry_fsm_transition_t *transition,
                                   const cancestry_event_t *event)
{
    cancestry_fsm_instance_t *instance = act->instance;

    if (transition->event != event->type) {
        return false;
    }
    switch (event->type) {
    case CANCESTRY_EVENT_TYPE_CAN_RX:
        return fsm_match_can_rx(act, transition, event);
    case CANCESTRY_EVENT_TYPE_SIGNAL_CHANGED:
        /* The schema requires a signal filter, so a missing name cannot match. */
        return fsm_name_equals(transition->signal, event->payload.signal_changed.signal_name);
    case CANCESTRY_EVENT_TYPE_TIMER_EXPIRED: {
        int index = fsm_timer_index(instance, transition->timer);

        return index >= 0 && (cancestry_timer_id_t)(index + 1) ==
                                 event->payload.timer_expired.timer_id &&
               event->payload.timer_expired.instance_id == instance->id;
    }
    case CANCESTRY_EVENT_TYPE_STATE_ENTERED:
    case CANCESTRY_EVENT_TYPE_STATE_EXITED:
        return event->payload.state.instance_id == instance->id;
    case CANCESTRY_EVENT_TYPE_FAULT_RAISED:
    case CANCESTRY_EVENT_TYPE_POWER_MODE_CHANGED:
        /* These events have no filter fields in the schema: the instance's
         * subscription list is what gates them (fsm-spec.md section 5). */
        return true;
    default:
        return false;
    }
}

/**
 * Select the transition for one event: the first declaration whose selector and
 * filters match and whose guard holds (declaration order is selection order,
 * SW-FR-FSM-012; at most one transition per event, fsm-spec.md section 8).
 *
 * @return The selected transition, or NULL when the event changes nothing.
 */
static const cancestry_fsm_transition_t *fsm_select_transition(fsm_activation_t *act,
                                                                const cancestry_event_t *event)
{
    cancestry_fsm_instance_t *instance = act->instance;
    const cancestry_fsm_state_t *state = fsm_current_state(instance);
    uint16_t i;

    if (state == NULL) {
        return NULL;
    }
    for (i = 0u; i < state->transition_count; ++i) {
        const cancestry_fsm_transition_t *transition = &state->transitions[i];
        bool guard_result = false;

        if (!fsm_transition_matches(act, transition, event)) {
            continue;
        }
        if (fsm_evaluate_guard(act, transition->guard, &guard_result) != CANCESTRY_FSM_OK) {
            /* Expression fault: stop selecting, the instance is being suspended. */
            return NULL;
        }
        if (guard_result) {
            return transition;
        }
    }
    return NULL;
}

/* ------------------------------------------------------------------------- */
/* Action executor (SW-FR-FSM-022..025, SW-FR-FSM-039..042)                  */
/* ------------------------------------------------------------------------- */

static cancestry_fsm_status_t fsm_evaluate_operand(fsm_activation_t *act,
                                                   const cancestry_fsm_operand_t *operand,
                                                   cancestry_value_t *value_out)
{
    cancestry_fsm_instance_t *instance = act->instance;

    memset(value_out, 0, sizeof(*value_out));
    if (!operand->is_expression) {
        *value_out = fsm_value_normalize(&operand->literal);
        return CANCESTRY_FSM_OK;
    }
    {
        fsm_eval_env_t env;
        cancestry_fsm_expression_context_t context;
        cancestry_fsm_expression_error_t error = CANCESTRY_FSM_EXPRESSION_OK;

        memset(&env, 0, sizeof(env));
        fsm_expression_context(act->engine, instance, act->event, &env, &context);
        act->expression_error = CANCESTRY_FSM_EXPRESSION_OK;
        if (!cancestry_fsm_expression_evaluate(operand->expression, &context, value_out, &error)) {
            memset(value_out, 0, sizeof(*value_out));
            act->expression_error = error;
            if (error == CANCESTRY_FSM_EXPRESSION_ERR_UNAUTHORIZED) {
                instance->counters.capability_denials++;
            }
            /* Same fault model as a guard: an expression fault ends the
             * instance's execution rather than writing a guessed value. */
            act->suspend_requested = true;
            return CANCESTRY_FSM_ERR_EXPRESSION;
        }
    }
    return CANCESTRY_FSM_OK;
}

/**
 * Coerce an evaluated value to a variable's declared type, or refuse.
 *
 * The input is copied first because two of the callers pass the same object for
 * in and out; a fresh local keeps that aliasing harmless instead of surprising.
 */
static cancestry_fsm_status_t fsm_coerce_to_declared(cancestry_value_kind_t declared,
                                                     const cancestry_value_t *in,
                                                     cancestry_value_t *out)
{
    cancestry_value_t source = *in;

    in = &source;
    memset(out, 0, sizeof(*out));
    switch (declared) {
    case CANCESTRY_VALUE_KIND_BOOL:
        if (in->kind != CANCESTRY_VALUE_KIND_BOOL) {
            return CANCESTRY_FSM_ERR_ARGUMENT;
        }
        *out = *in;
        return CANCESTRY_FSM_OK;
    case CANCESTRY_VALUE_KIND_INT:
        if (in->kind == CANCESTRY_VALUE_KIND_INT) {
            *out = *in;
            return CANCESTRY_FSM_OK;
        }
        if (in->kind == CANCESTRY_VALUE_KIND_REAL) {
            int64_t rounded;

            /* Rounding half away from zero is the rounding rule core/codec and
             * core/recipe apply to physical values, so an integer variable
             * assigned a real expression uses the same one. Out of range is
             * refused, never wrapped. */
            if (!fsm_round_half_away(in->value.real, &rounded)) {
                return CANCESTRY_FSM_ERR_ARGUMENT;
            }
            out->kind = CANCESTRY_VALUE_KIND_INT;
            out->value.integer = rounded;
            return CANCESTRY_FSM_OK;
        }
        return CANCESTRY_FSM_ERR_ARGUMENT;
    case CANCESTRY_VALUE_KIND_REAL:
        if (in->kind == CANCESTRY_VALUE_KIND_INT) {
            out->kind = CANCESTRY_VALUE_KIND_REAL;
            out->value.real = (double)in->value.integer;
            return CANCESTRY_FSM_OK;
        }
        if (in->kind == CANCESTRY_VALUE_KIND_REAL) {
            *out = *in;
            return CANCESTRY_FSM_OK;
        }
        return CANCESTRY_FSM_ERR_ARGUMENT;
    default:
        return CANCESTRY_FSM_ERR_ARGUMENT;
    }
}

static bool fsm_value_equals(const cancestry_value_t *a, const cancestry_value_t *b)
{
    cancestry_value_t left = fsm_value_normalize(a);
    cancestry_value_t right = fsm_value_normalize(b);

    if (left.kind != right.kind) {
        return false;
    }
    switch (left.kind) {
    case CANCESTRY_VALUE_KIND_BOOL:
        return left.value.boolean == right.value.boolean;
    case CANCESTRY_VALUE_KIND_INT:
        return left.value.integer == right.value.integer;
    case CANCESTRY_VALUE_KIND_UINT:
        return left.value.unsigned_integer == right.value.unsigned_integer;
    case CANCESTRY_VALUE_KIND_REAL:
        return left.value.real == right.value.real;
    default:
        return false;
    }
}

static cancestry_fsm_status_t fsm_action_send_message(fsm_activation_t *act,
                                                      const cancestry_fsm_action_t *action,
                                                      cancestry_fsm_invocation_t *invocation)
{
    cancestry_fsm_engine_t *engine = act->engine;
    cancestry_fsm_instance_t *instance = act->instance;
    const cancestry_fsm_interface_capability_t *capability;
    const cancestry_codec_map_t *map = NULL;
    const cancestry_codec_message_t *message = NULL;
    cancestry_fsm_governor_request_t request;
    uint8_t frame[CANCESTRY_CAN_FRAME_MAX_LENGTH];
    size_t span_bytes = 0u;
    size_t i;
    cancestry_fsm_status_t status;

    if (engine->capabilities == NULL) {
        /* No declared capabilities means no declared permission (fail closed). */
        instance->counters.capability_denials++;
        return CANCESTRY_FSM_ERR_DENIED;
    }
    capability = fsm_resolve_interface(engine, instance, action->as.send_message.interface, NULL);
    if (capability == NULL || !capability->tx) {
        instance->counters.capability_denials++;
        return CANCESTRY_FSM_ERR_DENIED;
    }
    status = fsm_resolve_message(engine, action->as.send_message.message, &map, &message);
    if (status != CANCESTRY_FSM_OK) {
        return status;
    }
    /* SW-FR-FSM-040: an id the package did not declare is never transmitted. */
    if (!fsm_in_id_list(capability->tx_ids, capability->tx_id_count, message->id)) {
        instance->counters.capability_denials++;
        return CANCESTRY_FSM_ERR_DENIED;
    }

    memset(frame, 0, sizeof(frame));
    for (i = 0u; i < action->as.send_message.value_count; ++i) {
        const cancestry_fsm_signal_value_t *spec = &action->as.send_message.values[i];
        const cancestry_codec_signal_t *signal = cancestry_codec_map_find_signal(map, spec->name);
        cancestry_value_t value;
        cancestry_value_t encoded;
        cancestry_codec_status_t codec_status;
        size_t signal_bytes;

        if (signal == NULL) {
            return CANCESTRY_FSM_ERR_NOT_FOUND;
        }
        status = fsm_evaluate_operand(act, &spec->operand, &value);
        if (status != CANCESTRY_FSM_OK) {
            return status;
        }
        memset(&encoded, 0, sizeof(encoded));
        if (signal->scale != 1.0 || signal->offset != 0.0) {
            if (value.kind == CANCESTRY_VALUE_KIND_BOOL) {
                return CANCESTRY_FSM_ERR_ENCODING;
            }
            encoded.kind = CANCESTRY_VALUE_KIND_REAL;
            encoded.value.real = (value.kind == CANCESTRY_VALUE_KIND_REAL)
                                     ? value.value.real
                                     : (double)value.value.integer;
        } else if (signal->type == CANCESTRY_CODEC_SIGNAL_TYPE_BOOLEAN) {
            if (value.kind != CANCESTRY_VALUE_KIND_BOOL) {
                return CANCESTRY_FSM_ERR_ENCODING;
            }
            encoded = value;
        } else if (signal->type == CANCESTRY_CODEC_SIGNAL_TYPE_INT) {
            if (value.kind == CANCESTRY_VALUE_KIND_REAL) {
                int64_t rounded;

                if (!fsm_round_half_away(value.value.real, &rounded)) {
                    return CANCESTRY_FSM_ERR_ENCODING;
                }
                encoded.kind = CANCESTRY_VALUE_KIND_INT;
                encoded.value.integer = rounded;
            } else if (value.kind == CANCESTRY_VALUE_KIND_INT) {
                encoded = value;
            } else {
                return CANCESTRY_FSM_ERR_ENCODING;
            }
        } else {
            /* uint and enum take unscaled, non-negative integers. */
            int64_t integral;

            if (value.kind == CANCESTRY_VALUE_KIND_INT) {
                integral = value.value.integer;
            } else if (value.kind == CANCESTRY_VALUE_KIND_REAL) {
                if (!fsm_round_half_away(value.value.real, &integral)) {
                    return CANCESTRY_FSM_ERR_ENCODING;
                }
            } else if (value.kind == CANCESTRY_VALUE_KIND_BOOL) {
                integral = value.value.boolean ? 1 : 0;
            } else {
                return CANCESTRY_FSM_ERR_ENCODING;
            }
            if (integral < 0) {
                return CANCESTRY_FSM_ERR_ENCODING;
            }
            encoded.kind = CANCESTRY_VALUE_KIND_UINT;
            encoded.value.unsigned_integer = (uint64_t)integral;
        }
        codec_status = cancestry_codec_encode_signal(signal, &encoded, frame, sizeof(frame), NULL);
        if (codec_status < 0) {
            return CANCESTRY_FSM_ERR_ENCODING;
        }
        signal_bytes = ((size_t)signal->last_bit >> 3) + 1u;
        if (signal_bytes > span_bytes) {
            span_bytes = signal_bytes;
        }
    }
    /* Fail closed rather than transmit a frame that would truncate a declared
     * signal (the same rule core/recipe applies). */
    if (span_bytes > (size_t)message->dlc) {
        return CANCESTRY_FSM_ERR_ENCODING;
    }

    memset(&request, 0, sizeof(request));
    request.kind = CANCESTRY_FSM_GOVERNOR_SEND_MESSAGE;
    request.instance = instance;
    request.instance_id = instance->id;
    request.cause = act->event;
    request.interface_name = capability->name;
    request.interface_id = capability->id;
    request.message_name = action->as.send_message.message;
    request.can_id = message->id;
    request.frame = frame;
    request.frame_length = message->dlc;
    /* The governor is the single approval point; NULL denies (SW-FR-GOV-006). */
    if (engine->governor == NULL ||
        engine->governor(engine->governor_user_data, &request) !=
            CANCESTRY_FSM_GOVERNOR_APPROVE) {
        instance->counters.governor_denials++;
        return CANCESTRY_FSM_ERR_DENIED;
    }
    if (engine->sink == NULL || engine->sink->on_send_message == NULL) {
        /* Approved, but there is nowhere to deliver it: reporting success would
         * be a lie, so the action fails (fail closed). */
        return CANCESTRY_FSM_ERR_UNSUPPORTED;
    }
    engine->sink->on_send_message(engine->sink->user_data, invocation, capability->name,
                                  capability->id, action->as.send_message.message, message->id,
                                  frame, message->dlc);
    instance->counters.messages_sent++;
    return CANCESTRY_FSM_OK;
}

static cancestry_fsm_status_t fsm_action_set_signal(fsm_activation_t *act,
                                                    const cancestry_fsm_action_t *action,
                                                    cancestry_fsm_invocation_t *invocation)
{
    cancestry_fsm_engine_t *engine = act->engine;
    cancestry_fsm_instance_t *instance = act->instance;
    const char *name = action->as.set_signal.signal;
    const cancestry_fsm_capabilities_t *capabilities = engine->capabilities;
    cancestry_value_t value;
    cancestry_value_t old_value;
    cancestry_signal_id_t signal_id = CANCESTRY_ID_NONE;
    bool had_old = false;
    cancestry_fsm_status_t status;
    cancestry_fsm_governor_request_t request;

    /* SW-FR-FSM-041: a signal the package may not write is blocked before the
     * governor is consulted. */
    if (!fsm_in_name_list(capabilities == NULL ? NULL : capabilities->signal_write,
                          capabilities == NULL ? 0u : capabilities->signal_write_count, name)) {
        instance->counters.capability_denials++;
        return CANCESTRY_FSM_ERR_DENIED;
    }
    status = fsm_evaluate_operand(act, &action->as.set_signal.operand, &value);
    if (status != CANCESTRY_FSM_OK) {
        return status;
    }
    if (engine->signal_bus == NULL || engine->signal_bus->write == NULL) {
        return CANCESTRY_FSM_ERR_UNSUPPORTED;
    }
    memset(&old_value, 0, sizeof(old_value));
    if (engine->signal_bus->read != NULL) {
        had_old = engine->signal_bus->read(engine->signal_bus->user_data, name, &old_value) ==
                  CANCESTRY_FSM_OK;
        if (had_old) {
            old_value = fsm_value_normalize(&old_value);
        }
    }
    if (engine->signal_bus->resolve != NULL) {
        (void)engine->signal_bus->resolve(engine->signal_bus->user_data, name, &signal_id);
    }

    memset(&request, 0, sizeof(request));
    request.kind = CANCESTRY_FSM_GOVERNOR_SET_SIGNAL;
    request.instance = instance;
    request.instance_id = instance->id;
    request.cause = act->event;
    request.signal_name = name;
    request.signal_id = signal_id;
    request.value = &value;
    if (engine->governor == NULL ||
        engine->governor(engine->governor_user_data, &request) !=
            CANCESTRY_FSM_GOVERNOR_APPROVE) {
        instance->counters.governor_denials++;
        return CANCESTRY_FSM_ERR_DENIED;
    }

    status = engine->signal_bus->write(engine->signal_bus->user_data, name, &value);
    if (status != CANCESTRY_FSM_OK) {
        return status;
    }
    instance->counters.signals_set++;
    if (engine->sink != NULL && engine->sink->on_signal_write != NULL) {
        cancestry_value_t none;

        memset(&none, 0, sizeof(none));
        engine->sink->on_signal_write(engine->sink->user_data, invocation, name, signal_id,
                                      had_old ? &old_value : &none, &value);
    }
    /*
     * signal_changed is generated only when the value actually changed, so an
     * FSM cannot keep a feedback loop alive by rewriting the same value.
     */
    if (!had_old || !fsm_value_equals(&old_value, &value)) {
        cancestry_event_payload_t payload;

        memset(&payload, 0, sizeof(payload));
        payload.signal_changed.signal_id = signal_id;
        payload.signal_changed.signal_name = name;
        payload.signal_changed.old_value = old_value;
        payload.signal_changed.new_value = value;
        if (!had_old) {
            payload.signal_changed.old_value.kind = CANCESTRY_VALUE_KIND_UNSET;
        }
        fsm_emit_generated(act, CANCESTRY_EVENT_TYPE_SIGNAL_CHANGED, &payload,
                           fsm_subscribes_signal(act, name));
    }
    return CANCESTRY_FSM_OK;
}

static cancestry_fsm_status_t fsm_action_set_variable(fsm_activation_t *act,
                                                      const cancestry_fsm_action_t *action)
{
    cancestry_fsm_instance_t *instance = act->instance;
    int index = fsm_variable_index(instance, action->as.set_variable.variable);
    cancestry_value_t value;
    cancestry_value_t stored;
    cancestry_fsm_status_t status;

    if (index < 0) {
        return CANCESTRY_FSM_ERR_NOT_FOUND;
    }
    status = fsm_evaluate_operand(act, &action->as.set_variable.operand, &value);
    if (status != CANCESTRY_FSM_OK) {
        return status;
    }
    status = fsm_coerce_to_declared(instance->variables[index].declared, &value, &stored);
    if (status != CANCESTRY_FSM_OK) {
        /* A value that cannot be represented in the declared type is refused,
         * never silently truncated (expression-language.md section 6). */
        return status;
    }
    instance->variables[index].value = stored;
    instance->variables[index].initialized = true;
    instance->counters.variables_set++;
    return CANCESTRY_FSM_OK;
}

static cancestry_fsm_status_t fsm_action_timer(fsm_activation_t *act,
                                               const cancestry_fsm_action_t *action,
                                               cancestry_fsm_invocation_t *invocation)
{
    cancestry_fsm_engine_t *engine = act->engine;
    cancestry_fsm_instance_t *instance = act->instance;
    const cancestry_fsm_timer_def_t *def;
    cancestry_fsm_timer_state_t *slot;
    const char *name;
    uint32_t duration_ms;
    bool repeat;
    int index;

    name = (action->kind == CANCESTRY_FSM_ACTION_START_TIMER) ? action->as.start_timer.timer
                                                              : action->as.timer.timer;
    index = fsm_timer_index(instance, name);
    if (index < 0) {
        return CANCESTRY_FSM_ERR_NOT_FOUND;
    }
    slot = &instance->timers[index];
    def = &instance->machine->timers[index];
    if (action->kind == CANCESTRY_FSM_ACTION_START_TIMER) {
        duration_ms = action->as.start_timer.has_duration_ms ? action->as.start_timer.duration_ms
                                                             : def->duration_ms;
        repeat = action->as.start_timer.has_repeat ? action->as.start_timer.repeat : def->repeat;
        fsm_timer_arm(engine, slot, duration_ms, repeat);
        instance->counters.timer_starts++;
    } else if (action->kind == CANCESTRY_FSM_ACTION_STOP_TIMER) {
        slot->running = false;
        instance->counters.timer_stops++;
        duration_ms = slot->duration_ms;
        repeat = slot->repeat;
    } else {
        /*
         * reset_timer re-arms the countdown and keeps the running state, so a
         * reset of a stopped timer cannot start a timer no declaration started
         * (fsm-spec.md section 9 lists stop and reset as distinct operations).
         */
        slot->deadline_us = fsm_sat_add(engine->now_us, fsm_us(slot->duration_ms));
        instance->counters.timer_resets++;
        duration_ms = slot->duration_ms;
        repeat = slot->repeat;
    }
    if (engine->sink != NULL && engine->sink->on_timer != NULL) {
        engine->sink->on_timer(engine->sink->user_data, invocation, action->kind, name,
                               duration_ms, repeat);
    }
    return CANCESTRY_FSM_OK;
}

static cancestry_fsm_status_t fsm_action_raise_fault(fsm_activation_t *act,
                                                     const cancestry_fsm_action_t *action)
{
    cancestry_fsm_engine_t *engine = act->engine;
    cancestry_fsm_instance_t *instance = act->instance;
    cancestry_event_payload_t payload;
    cancestry_fault_code_t code = cancestry_fsm_fault_code_hash(action->as.raise_fault.code);

    memset(&payload, 0, sizeof(payload));
    payload.fault.fault_code = code;
    payload.fault.severity = action->as.raise_fault.severity;
    payload.fault.source_id = instance->id;
    instance->counters.faults_raised++;
    /*
     * Faults are never dropped by policy (event-ordering.md section 9), which is
     * why a fault the instance raised for itself is queued for it - but only
     * when the instance declared the fault subscription, so a machine cannot
     * build an unbounded self-reaction loop by accident.
     */
    fsm_emit_generated(act, CANCESTRY_EVENT_TYPE_FAULT_RAISED, &payload,
                       instance->def->subscriptions.has_faults &&
                           instance->def->subscriptions.faults);
    fsm_trace(engine, instance, CANCESTRY_FSM_TRACE_FAULT, (int32_t)code, "%s/%s",
              action->as.raise_fault.code, fsm_severity_name(action->as.raise_fault.severity));
    if (engine->sink != NULL && engine->sink->on_fault != NULL) {
        cancestry_fsm_invocation_t invocation;

        fsm_invocation_fill(&invocation, act, action, NULL, NULL, 0u);
        engine->sink->on_fault(engine->sink->user_data, &invocation, action->as.raise_fault.code,
                               action->as.raise_fault.severity, code);
    }
    /*
     * A critical fault is contained at instance level: the lifecycle has a FAULT
     * state for exactly this, and it is the only state reset() recovers from
     * (fsm-spec.md section 3; mode-fault-state-machine.md section 2 escalates
     * critical faults to SAFE at system level, which is not the FSM's call).
     */
    if (action->as.raise_fault.severity == CANCESTRY_FAULT_SEVERITY_CRITICAL) {
        (void)cancestry_fsm_instance_fault(engine, instance);
    }
    return CANCESTRY_FSM_OK;
}

static cancestry_fsm_status_t fsm_action_transition(fsm_activation_t *act,
                                                    const cancestry_fsm_action_t *action)
{
    cancestry_fsm_instance_t *instance = act->instance;

    if (instance->deferred_pending) {
        /* fsm-spec.md section 8 rule 6, SW-FR-FSM-055: the first transition
         * action wins, later ones are ignored and reported. */
        instance->counters.deferred_ignored++;
        fsm_warn(act, "transition action ignored: one is already pending");
        return CANCESTRY_FSM_OK;
    }
    instance->deferred_pending = true;
    instance->deferred_target = action->as.transition.target_index;
    return CANCESTRY_FSM_OK;
}

/**
 * Execute an action list.
 *
 * A failing action is recorded and execution continues with the next one
 * (SW-FR-FSM-024: record the failure and continue safely); only an expression
 * fault stops the sequence, because that is the one failure the normative
 * reaction table escalates (mode-fault-state-machine.md section 5).
 */
static void fsm_run_actions(fsm_activation_t *act,
                            const cancestry_fsm_transition_t *transition,
                            const char *state,
                            const cancestry_fsm_action_t *actions,
                            uint16_t count,
                            uint16_t chain_depth)
{
    cancestry_fsm_engine_t *engine = act->engine;
    cancestry_fsm_instance_t *instance = act->instance;
    uint16_t i;

    for (i = 0u; i < count; ++i) {
        const cancestry_fsm_action_t *action = &actions[i];
        cancestry_fsm_invocation_t invocation;
        cancestry_fsm_status_t status;

        if (act->suspend_requested) {
            return;
        }
        if (act->action_budget == 0u) {
            instance->counters.budget_exhausted++;
            fsm_warn(act, "action budget exhausted");
            return;
        }
        act->action_budget--;
        fsm_invocation_fill(&invocation, act, action, transition, state, chain_depth);
        switch (action->kind) {
        case CANCESTRY_FSM_ACTION_SEND_MESSAGE:
            status = fsm_action_send_message(act, action, &invocation);
            break;
        case CANCESTRY_FSM_ACTION_SET_SIGNAL:
            status = fsm_action_set_signal(act, action, &invocation);
            break;
        case CANCESTRY_FSM_ACTION_SET_VARIABLE:
            status = fsm_action_set_variable(act, action);
            break;
        case CANCESTRY_FSM_ACTION_START_TIMER:
        case CANCESTRY_FSM_ACTION_STOP_TIMER:
        case CANCESTRY_FSM_ACTION_RESET_TIMER:
            status = fsm_action_timer(act, action, &invocation);
            break;
        case CANCESTRY_FSM_ACTION_LOG:
            instance->counters.logs_emitted++;
            fsm_trace(engine, instance, CANCESTRY_FSM_TRACE_ACTION, (int32_t)action->as.log.level,
                      "log/%s: %s", cancestry_fsm_log_level_name(action->as.log.level),
                      action->as.log.message);
            if (engine->sink != NULL && engine->sink->on_log != NULL) {
                engine->sink->on_log(engine->sink->user_data, &invocation, action->as.log.level,
                                     action->as.log.message);
            }
            status = CANCESTRY_FSM_OK;
            break;
        case CANCESTRY_FSM_ACTION_RAISE_FAULT:
            status = fsm_action_raise_fault(act, action);
            break;
        case CANCESTRY_FSM_ACTION_TRANSITION:
            status = fsm_action_transition(act, action);
            break;
        default:
            status = CANCESTRY_FSM_ERR_ARGUMENT;
            break;
        }
        if (status == CANCESTRY_FSM_OK) {
            instance->counters.actions_executed++;
            fsm_trace(engine, instance, CANCESTRY_FSM_TRACE_ACTION, (int32_t)action->kind, "%s ok",
                      cancestry_fsm_action_kind_name(action->kind));
        } else {
            instance->counters.action_errors++;
            fsm_trace(engine, instance, CANCESTRY_FSM_TRACE_ACTION_ERROR, (int32_t)status, "%s: %s",
                      cancestry_fsm_action_kind_name(action->kind), cancestry_fsm_status_name(status));
            if (status == CANCESTRY_FSM_ERR_EXPRESSION) {
                /* One more record naming the fault reason, so a trace alone
                 * explains why an instance stopped. */
                fsm_trace(engine, instance, CANCESTRY_FSM_TRACE_ACTION_ERROR,
                          (int32_t)act->expression_error, "%s/expr: %s",
                          cancestry_fsm_action_kind_name(action->kind),
                          cancestry_fsm_expression_error_name(act->expression_error));
            }
            if (status == CANCESTRY_FSM_ERR_DENIED) {
                fsm_trace(engine,
                          instance,
                          CANCESTRY_FSM_TRACE_DENIED,
                          (int32_t)(instance->counters.governor_denials +
                                    instance->counters.capability_denials),
                          "%s/%s",
                          cancestry_fsm_action_kind_name(action->kind),
                          (instance->counters.governor_denials > 0u) ? "governor" : "capability");
            }
        }
    }
}

/* ------------------------------------------------------------------------- */
/* Transition execution (fsm-spec.md section 8, SwAD.md section 7)            */
/* ------------------------------------------------------------------------- */

/**
 * Execute one transition step: exit actions, transition actions, entry actions,
 * in that order (SW-FR-FSM-014). A self-transition runs both action sets as
 * well (SW-FR-FSM-015), because v0.2 has no internal transitions that would
 * allow staying without re-entry.
 *
 * @param transition  The event-driven transition, or NULL for a deferred
 *                    transition requested by a transition action (which has no
 *                    guard and no actions of its own).
 */
static void fsm_execute_transition(fsm_activation_t *act,
                                   const cancestry_fsm_transition_t *transition,
                                   uint16_t chain_depth)
{
    cancestry_fsm_engine_t *engine = act->engine;
    cancestry_fsm_instance_t *instance = act->instance;
    const cancestry_fsm_state_t *from = fsm_current_state(instance);
    const cancestry_fsm_state_t *to;
    uint16_t target_index;

    target_index = (transition != NULL) ? transition->target_index : instance->deferred_target;
    if (instance->machine == NULL || target_index >= instance->machine->state_count) {
        /* Unreachable for loaded definitions; refuse rather than index wildly. */
        act->suspend_requested = true;
        return;
    }
    to = &instance->machine->states[target_index];

    if (from != NULL) {
        fsm_run_actions(act, transition, from->name, from->exit, from->exit_count, chain_depth);
        if (!act->suspend_requested) {
            fsm_emit_state_event(act, CANCESTRY_EVENT_TYPE_STATE_EXITED, from);
        }
    }
    if (transition != NULL) {
        fsm_run_actions(act, transition, NULL, transition->actions, transition->action_count,
                        chain_depth);
    }
    if (act->suspend_requested) {
        /* The state change is not committed after a fault: no half-executed
         * transition is observable (fail closed). */
        return;
    }
    instance->state_index = target_index;
    instance->has_state = true;
    instance->counters.transitions++;
    if (transition == NULL) {
        instance->counters.deferred_transitions++;
    }
    fsm_trace(engine, instance, CANCESTRY_FSM_TRACE_TRANSITION, (int32_t)chain_depth, "%s->%s",
              (from != NULL) ? from->name : "(initial)", to->name);
    if (engine->sink != NULL && engine->sink->on_state_change != NULL) {
        cancestry_fsm_invocation_t invocation;

        fsm_invocation_fill(&invocation, act, NULL, transition, to->name, chain_depth);
        engine->sink->on_state_change(engine->sink->user_data, &invocation,
                                      (from != NULL) ? from->name : NULL, to->name);
    }
    fsm_emit_state_event(act, CANCESTRY_EVENT_TYPE_STATE_ENTERED, to);
    fsm_run_actions(act, transition, to->name, to->entry, to->entry_count, chain_depth);
}

/** Enter the initial state; entry actions run with the state already current. */
static void fsm_enter_initial(fsm_activation_t *act)
{
    cancestry_fsm_instance_t *instance = act->instance;
    const cancestry_fsm_machine_t *machine = instance->machine;
    const cancestry_fsm_state_t *initial;

    if (machine == NULL || machine->initial_index >= machine->state_count) {
        act->suspend_requested = true;
        return;
    }
    initial = &machine->states[machine->initial_index];
    instance->state_index = machine->initial_index;
    instance->has_state = true;
    instance->counters.transitions++;
    fsm_trace(act->engine, instance, CANCESTRY_FSM_TRACE_TRANSITION, 1, "(initial)->%s",
              initial->name);
    if (act->engine->sink != NULL && act->engine->sink->on_state_change != NULL) {
        cancestry_fsm_invocation_t invocation;

        fsm_invocation_fill(&invocation, act, NULL, NULL, initial->name, 1u);
        act->engine->sink->on_state_change(act->engine->sink->user_data, &invocation, NULL,
                                           initial->name);
    }
    fsm_emit_state_event(act, CANCESTRY_EVENT_TYPE_STATE_ENTERED, initial);
    fsm_run_actions(act, NULL, initial->name, initial->entry, initial->entry_count, 1u);
}

/**
 * Run the deferred transitions a chain of actions requested, counting every step
 * toward the chain limit (fsm-spec.md section 8 rules 3-5, SW-FR-FSM-016,
 * SW-FR-FSM-054).
 */
static void fsm_run_deferred_chain(fsm_activation_t *act, uint16_t depth)
{
    cancestry_fsm_engine_t *engine = act->engine;
    cancestry_fsm_instance_t *instance = act->instance;

    while (instance->deferred_pending) {
        if (act->suspend_requested) {
            instance->deferred_pending = false;
            return;
        }
        if (depth >= engine->max_chain_depth) {
            /*
             * The limit is exceeded. The normative reaction to "FSM transition
             * chain exceeded" is to suspend the instance
             * (mode-fault-state-machine.md section 5), so the pending request is
             * dropped and the instance stops rather than continuing a runaway
             * chain (SW-FR-FSM-016, SW-FR-FSM-021).
             */
            instance->deferred_pending = false;
            instance->counters.chain_limit_exceeded++;
            fsm_warn(act, "transition chain limit exceeded");
            act->suspend_requested = true;
            return;
        }
        depth++;
        instance->deferred_pending = false;
        fsm_execute_transition(act, NULL, depth);
    }
}

/* ------------------------------------------------------------------------- */
/* Subscription gate                                                         */
/* ------------------------------------------------------------------------- */

/** @return true when any transition of the machine selects @p type. */
static bool fsm_declares_event(const cancestry_fsm_instance_t *instance,
                               cancestry_event_type_t type)
{
    const cancestry_fsm_machine_t *machine = instance->machine;
    uint16_t s;
    uint16_t t;

    if (machine == NULL) {
        return false;
    }
    for (s = 0u; s < machine->state_count; ++s) {
        for (t = 0u; t < machine->states[s].transition_count; ++t) {
            if (machine->states[s].transitions[t].event == type) {
                return true;
            }
        }
    }
    return false;
}

/**
 * Decide whether an event reaches this instance's incoming queue.
 *
 * A declared subscription list is a filter. An undeclared class falls back to
 * "the classes the machine's transitions select", which keeps a minimal
 * declaration working while "timers: false" or an empty "signals: []" really
 * means "never deliver this class" (fsm-spec.md sections 5 and 11).
 */
static bool fsm_instance_accepts(const cancestry_fsm_engine_t *engine,
                                 const cancestry_fsm_instance_t *instance,
                                 const cancestry_event_t *event,
                                 bool *suppressed)
{
    const cancestry_fsm_subscriptions_t *subscriptions = &instance->def->subscriptions;

    *suppressed = false;
    switch (event->type) {
    case CANCESTRY_EVENT_TYPE_CAN_RX: {
        uint16_t i;

        if (!subscriptions->has_can_rx) {
            return fsm_declares_event(instance, CANCESTRY_EVENT_TYPE_CAN_RX);
        }
        for (i = 0u; i < subscriptions->can_rx_count; ++i) {
            const cancestry_fsm_can_rx_subscription_t *subscription = &subscriptions->can_rx[i];
            const cancestry_fsm_interface_capability_t *capability =
                fsm_resolve_interface(engine, instance, subscription->interface, NULL);

            if (capability == NULL || capability->id != event->payload.can_rx.interface_id) {
                continue;
            }
            if (subscription->has_can_id && subscription->can_id != event->payload.can_rx.can_id) {
                continue;
            }
            if (subscription->message != NULL) {
                const cancestry_codec_message_t *message = NULL;

                if (fsm_resolve_message(engine, subscription->message, NULL, &message) !=
                        CANCESTRY_FSM_OK ||
                    message == NULL || message->id != event->payload.can_rx.can_id) {
                    continue;
                }
            }
            return true;
        }
        return false;
    }
    case CANCESTRY_EVENT_TYPE_SIGNAL_CHANGED:
        if (!subscriptions->has_signals) {
            return fsm_declares_event(instance, CANCESTRY_EVENT_TYPE_SIGNAL_CHANGED);
        }
        if (subscriptions->signal_count == 0u) {
            *suppressed = true;
            return false;
        }
        return fsm_in_name_list(subscriptions->signals, subscriptions->signal_count,
                                event->payload.signal_changed.signal_name);
    case CANCESTRY_EVENT_TYPE_TIMER_EXPIRED:
        if (subscriptions->has_timers && !subscriptions->timers) {
            *suppressed = true;
            return false;
        }
        return true;
    case CANCESTRY_EVENT_TYPE_FAULT_RAISED:
        if (subscriptions->has_faults) {
            if (!subscriptions->faults) {
                *suppressed = true;
            }
            return subscriptions->faults;
        }
        return fsm_declares_event(instance, CANCESTRY_EVENT_TYPE_FAULT_RAISED);
    case CANCESTRY_EVENT_TYPE_POWER_MODE_CHANGED:
        if (subscriptions->has_power_mode) {
            if (!subscriptions->power_mode) {
                *suppressed = true;
            }
            return subscriptions->power_mode;
        }
        return fsm_declares_event(instance, CANCESTRY_EVENT_TYPE_POWER_MODE_CHANGED);
    default:
        return false;
    }
}

/* ------------------------------------------------------------------------- */
/* Event processing                                                          */
/* ------------------------------------------------------------------------- */

static void fsm_process_queued_event(fsm_activation_t *act, const cancestry_event_t *event)
{
    cancestry_fsm_engine_t *engine = act->engine;
    cancestry_fsm_instance_t *instance = act->instance;
    const cancestry_fsm_transition_t *transition;
    uint16_t depth = 0u;

    instance->counters.events_processed++;
    if (engine->record_events) {
        /* SW-FR-FSM-049: optional event recording, off by default. */
        fsm_trace(engine, instance, CANCESTRY_FSM_TRACE_EVENT, (int32_t)event->type, "%s seq=%" PRIu32,
                  cancestry_fsm_event_type_name(event->type), (uint32_t)event->sequence);
    }
    if (!instance->has_state) {
        return;
    }
    transition = fsm_select_transition(act, event);
    if (transition != NULL) {
        depth = 1u;
        fsm_execute_transition(act, transition, depth);
    }
    /*
     * Deferred transitions run before the next event is taken, which here means
     * "before this function returns", as fsm-spec.md section 8 rule 4 requires:
     * a deferred chain is never interrupted by another event of the same batch
     * (SW-FR-FSM-054).
     */
    if (instance->deferred_pending) {
        fsm_run_deferred_chain(act, depth);
    }
}

static void fsm_drain_instance(cancestry_fsm_engine_t *engine,
                               cancestry_fsm_instance_t *instance)
{
    uint16_t processed = 0u;

    if (instance->processing) {
        /* Re-entrant call from a sink callback: refuse instead of nesting, so no
         * call stack can grow without bound (SW-FR-FSM-021). */
        return;
    }
    instance->processing = true;
    while (!cancestry_event_queue_is_empty(&instance->incoming)) {
        cancestry_event_t event;
        fsm_activation_t act;

        if (!cancestry_fsm_lifecycle_is_active(instance->lifecycle)) {
            break;
        }
        if (processed >= engine->max_events_per_activation) {
            instance->counters.budget_exhausted++;
            fsm_trace(engine, instance, CANCESTRY_FSM_TRACE_WARNING, 0, "event budget exhausted");
            break;
        }
        processed++;
        if (cancestry_event_queue_pop(&instance->incoming, &event) != CANCESTRY_EVENT_QUEUE_OK) {
            break;
        }
        memset(&act, 0, sizeof(act));
        act.engine = engine;
        act.instance = instance;
        act.event = &event;
        act.action_budget = engine->max_actions_per_event;
        fsm_process_queued_event(&act, &event);
        if (act.suspend_requested) {
            (void)cancestry_fsm_instance_suspend(engine, instance);
        }
    }
    instance->processing = false;
}

/* ------------------------------------------------------------------------- */
/* Instance manager (fsm-spec.md sections 2, 3)                               */
/* ------------------------------------------------------------------------- */

/**
 * (Re)initialise an instance's tables from its declaration: variable slots with
 * defaults and overrides, timer slots armed but not running, and no state
 * entered. Counters are deliberately not reset: the engine never rewinds a
 * counter, so a reset instance's history stays readable.
 */
static void fsm_instance_initialize(cancestry_fsm_engine_t *engine,
                                    cancestry_fsm_instance_t *instance)
{
    const cancestry_fsm_machine_t *machine = instance->machine;
    const cancestry_fsm_instance_def_t *def = instance->def;
    uint16_t i;

    instance->variable_count = 0u;
    instance->timer_count = 0u;
    instance->has_state = false;
    instance->state_index = 0u;
    instance->deferred_pending = false;
    cancestry_event_queue_clear(&instance->incoming);
    if (machine == NULL) {
        return;
    }
    for (i = 0u; i < machine->variable_count && i < instance->variable_capacity; ++i) {
        cancestry_fsm_variable_slot_t *slot = &instance->variables[i];

        slot->name = machine->variables[i].name;
        slot->declared = machine->variables[i].type;
        memset(&slot->value, 0, sizeof(slot->value));
        slot->initialized = false;
        instance->variable_count++;
    }
    for (i = 0u; i < machine->timer_count && i < instance->timer_capacity; ++i) {
        cancestry_fsm_timer_state_t *slot = &instance->timers[i];

        slot->name = machine->timers[i].name;
        slot->id = (cancestry_timer_id_t)(i + 1u);
        slot->duration_ms = machine->timers[i].duration_ms;
        slot->repeat = machine->timers[i].repeat;
        slot->running = false;
        slot->deadline_us = 0u;
        instance->timer_count++;
    }
    {
        /* Defaults, then instance overrides: both are evaluated with no causing
         * event, so evt.* is not available at initialization time. */
        fsm_activation_t act;

        memset(&act, 0, sizeof(act));
        act.engine = engine;
        act.instance = instance;
        act.action_budget = engine->max_actions_per_event;

        for (i = 0u; i < machine->variable_count && i < instance->variable_capacity; ++i) {
            const cancestry_fsm_variable_def_t *variable = &machine->variables[i];
            cancestry_value_t stored;
            cancestry_value_t coerced;

            if (!variable->has_default) {
                continue;
            }
            if (fsm_evaluate_operand(&act, &variable->default_value, &stored) !=
                CANCESTRY_FSM_OK) {
                /* An expression default that faults is a definition-level
                 * problem: record it, leave the variable uninitialized (so any
                 * later read is undefined and fails closed), and clear the
                 * suspend request because initialization is not event execution. */
                act.suspend_requested = false;
                instance->counters.initialization_errors++;
                fsm_trace(engine, instance, CANCESTRY_FSM_TRACE_ACTION_ERROR,
                          (int32_t)CANCESTRY_FSM_ERR_EXPRESSION, "default %s", variable->name);
                continue;
            }
            if (fsm_coerce_to_declared(variable->type, &stored, &coerced) != CANCESTRY_FSM_OK) {
                instance->counters.initialization_errors++;
                fsm_trace(engine, instance, CANCESTRY_FSM_TRACE_ACTION_ERROR,
                          (int32_t)CANCESTRY_FSM_ERR_ARGUMENT, "default %s type", variable->name);
                continue;
            }
            instance->variables[i].value = coerced;
            instance->variables[i].initialized = true;
        }
        for (i = 0u; i < def->variable_count; ++i) {
            const cancestry_fsm_variable_override_t *override = &def->variables[i];
            int index = fsm_variable_index(instance, override->name);
            cancestry_value_t stored;
            cancestry_value_t coerced;

            if (index < 0) {
                instance->counters.initialization_errors++;
                continue;
            }
            if (fsm_evaluate_operand(&act, &override->operand, &stored) != CANCESTRY_FSM_OK) {
                act.suspend_requested = false;
                instance->counters.initialization_errors++;
                continue;
            }
            if (fsm_coerce_to_declared(instance->variables[index].declared, &stored, &coerced) !=
                CANCESTRY_FSM_OK) {
                instance->counters.initialization_errors++;
                continue;
            }
            instance->variables[index].value = coerced;
            instance->variables[index].initialized = true;
        }
    }
}

cancestry_fsm_status_t cancestry_fsm_instance_enable(cancestry_fsm_engine_t *engine,
                                                    cancestry_fsm_instance_t *instance)
{
    if (!cancestry_fsm_engine_is_valid(engine) || instance == NULL) {
        return CANCESTRY_FSM_ERR_NULL;
    }
    if (instance->lifecycle != CANCESTRY_FSM_INSTANCE_LIFECYCLE_DISABLED) {
        return CANCESTRY_FSM_ERR_STATE;
    }
    fsm_instance_initialize(engine, instance);
    instance->lifecycle = CANCESTRY_FSM_INSTANCE_LIFECYCLE_READY;
    fsm_trace(engine, instance, CANCESTRY_FSM_TRACE_LIFECYCLE, (int32_t)instance->lifecycle,
              "ready");
    return CANCESTRY_FSM_OK;
}

cancestry_fsm_status_t cancestry_fsm_instance_start(cancestry_fsm_engine_t *engine,
                                                    cancestry_fsm_instance_t *instance)
{
    fsm_activation_t act;
    uint16_t i;

    if (!cancestry_fsm_engine_is_valid(engine) || instance == NULL) {
        return CANCESTRY_FSM_ERR_NULL;
    }
    if (instance->lifecycle != CANCESTRY_FSM_INSTANCE_LIFECYCLE_READY) {
        return CANCESTRY_FSM_ERR_STATE;
    }
    instance->lifecycle = CANCESTRY_FSM_INSTANCE_LIFECYCLE_RUNNING;
    cancestry_event_queue_clear(&instance->incoming);
    instance->deferred_pending = false;
    instance->has_state = false;
    fsm_trace(engine, instance, CANCESTRY_FSM_TRACE_LIFECYCLE, (int32_t)instance->lifecycle,
              "running");

    /*
     * auto_start timers are armed before the initial entry actions, so an entry
     * action that starts the same timer wins deterministically (SW-FR-FSM-029).
     */
    for (i = 0u; i < instance->timer_count; ++i) {
        const cancestry_fsm_timer_def_t *def = &instance->machine->timers[i];

        if (def->auto_start) {
            fsm_timer_arm(engine, &instance->timers[i], def->duration_ms, def->repeat);
        }
    }

    memset(&act, 0, sizeof(act));
    act.engine = engine;
    act.instance = instance;
    act.action_budget = engine->max_actions_per_event;
    fsm_enter_initial(&act);
    if (instance->deferred_pending) {
        fsm_run_deferred_chain(&act, 1u);
    }
    if (act.suspend_requested) {
        (void)cancestry_fsm_instance_suspend(engine, instance);
    }
    fsm_drain_instance(engine, instance);
    return CANCESTRY_FSM_OK;
}

cancestry_fsm_status_t cancestry_fsm_instance_suspend(cancestry_fsm_engine_t *engine,
                                                      cancestry_fsm_instance_t *instance)
{
    if (!cancestry_fsm_engine_is_valid(engine) || instance == NULL) {
        return CANCESTRY_FSM_ERR_NULL;
    }
    if (instance->lifecycle != CANCESTRY_FSM_INSTANCE_LIFECYCLE_RUNNING) {
        return CANCESTRY_FSM_ERR_STATE;
    }
    /*
     * Queued events stay queued: resume continues with them. Suspending is a
     * pause, not a transition, so no exit actions run and no state is left
     * behind (fsm-spec.md sections 3 and 4). Timers keep running in the time
     * base but their deadlines are not evaluated while suspended, so a resumed
     * instance sees the missed periodic ticks in missed_count rather than losing
     * them.
     */
    instance->lifecycle = CANCESTRY_FSM_INSTANCE_LIFECYCLE_SUSPENDED;
    instance->deferred_pending = false;
    fsm_trace(engine, instance, CANCESTRY_FSM_TRACE_LIFECYCLE, (int32_t)instance->lifecycle,
              "suspended");
    return CANCESTRY_FSM_OK;
}

cancestry_fsm_status_t cancestry_fsm_instance_resume(cancestry_fsm_engine_t *engine,
                                                     cancestry_fsm_instance_t *instance)
{
    if (!cancestry_fsm_engine_is_valid(engine) || instance == NULL) {
        return CANCESTRY_FSM_ERR_NULL;
    }
    if (instance->lifecycle != CANCESTRY_FSM_INSTANCE_LIFECYCLE_SUSPENDED) {
        return CANCESTRY_FSM_ERR_STATE;
    }
    instance->lifecycle = CANCESTRY_FSM_INSTANCE_LIFECYCLE_RUNNING;
    fsm_trace(engine, instance, CANCESTRY_FSM_TRACE_LIFECYCLE, (int32_t)instance->lifecycle,
              "running");
    fsm_drain_instance(engine, instance);
    return CANCESTRY_FSM_OK;
}

cancestry_fsm_status_t cancestry_fsm_instance_fault(cancestry_fsm_engine_t *engine,
                                                    cancestry_fsm_instance_t *instance)
{
    uint16_t i;

    if (!cancestry_fsm_engine_is_valid(engine) || instance == NULL) {
        return CANCESTRY_FSM_ERR_NULL;
    }
    if (instance->lifecycle != CANCESTRY_FSM_INSTANCE_LIFECYCLE_RUNNING &&
        instance->lifecycle != CANCESTRY_FSM_INSTANCE_LIFECYCLE_SUSPENDED) {
        return CANCESTRY_FSM_ERR_STATE;
    }
    instance->lifecycle = CANCESTRY_FSM_INSTANCE_LIFECYCLE_FAULT;
    instance->deferred_pending = false;
    /* A faulted instance stops asking for time: no expiry while contained. */
    for (i = 0u; i < instance->timer_count; ++i) {
        instance->timers[i].running = false;
    }
    fsm_trace(engine, instance, CANCESTRY_FSM_TRACE_LIFECYCLE, (int32_t)instance->lifecycle,
              "fault");
    return CANCESTRY_FSM_OK;
}

cancestry_fsm_status_t cancestry_fsm_instance_reset(cancestry_fsm_engine_t *engine,
                                                    cancestry_fsm_instance_t *instance)
{
    if (!cancestry_fsm_engine_is_valid(engine) || instance == NULL) {
        return CANCESTRY_FSM_ERR_NULL;
    }
    if (instance->lifecycle != CANCESTRY_FSM_INSTANCE_LIFECYCLE_FAULT &&
        instance->lifecycle != CANCESTRY_FSM_INSTANCE_LIFECYCLE_SUSPENDED) {
        return CANCESTRY_FSM_ERR_STATE;
    }
    fsm_instance_initialize(engine, instance);
    instance->lifecycle = CANCESTRY_FSM_INSTANCE_LIFECYCLE_READY;
    fsm_trace(engine, instance, CANCESTRY_FSM_TRACE_LIFECYCLE, (int32_t)instance->lifecycle,
              "ready (reset)");
    return CANCESTRY_FSM_OK;
}

cancestry_fsm_status_t cancestry_fsm_instance_disable(cancestry_fsm_engine_t *engine,
                                                      cancestry_fsm_instance_t *instance)
{
    uint16_t i;

    if (!cancestry_fsm_engine_is_valid(engine) || instance == NULL) {
        return CANCESTRY_FSM_ERR_NULL;
    }
    instance->lifecycle = CANCESTRY_FSM_INSTANCE_LIFECYCLE_DISABLED;
    instance->deferred_pending = false;
    cancestry_event_queue_clear(&instance->incoming);
    for (i = 0u; i < instance->timer_count; ++i) {
        instance->timers[i].running = false;
    }
    fsm_trace(engine, instance, CANCESTRY_FSM_TRACE_LIFECYCLE, (int32_t)instance->lifecycle,
              "disabled");
    return CANCESTRY_FSM_OK;
}

/* ------------------------------------------------------------------------- */
/* Public API: dispatcher and tick                                           */
/* ------------------------------------------------------------------------- */

cancestry_fsm_status_t cancestry_fsm_engine_process_event(cancestry_fsm_engine_t *engine,
                                                           const cancestry_event_t *event)
{
    size_t i;
    size_t delivered = 0u;

    if (!cancestry_fsm_engine_is_valid(engine) || event == NULL) {
        return CANCESTRY_FSM_ERR_NULL;
    }
    if (!cancestry_event_is_valid(event)) {
        return CANCESTRY_FSM_ERR_ARGUMENT;
    }
    engine->counters.events_received++;

    /*
     * Deliver first, then process, instance by instance in declaration order
     * (docs/system/event-ordering.md section 7). Each instance then works
     * through its own queue sequentially (SW-FR-FSM-043), so one instance's
     * fault, suspension or long chain cannot interleave with another's
     * (SW-FR-FSM-047).
     */
    for (i = 0u; i < engine->instance_count; ++i) {
        cancestry_fsm_instance_t *instance = &engine->instances[i];
        bool suppressed = false;

        if (!cancestry_fsm_lifecycle_is_active(instance->lifecycle)) {
            continue;
        }
        if (!fsm_instance_accepts(engine, instance, event, &suppressed)) {
            if (suppressed) {
                instance->counters.events_suppressed++;
            }
            continue;
        }
        if (!cancestry_event_queue_status_is_ok(cancestry_event_queue_push(&instance->incoming,
                                                                          event))) {
            instance->counters.events_dropped++;
            continue;
        }
        instance->counters.events_queued++;
        delivered++;
    }
    if (delivered == 0u) {
        engine->counters.events_ignored++;
    } else {
        engine->counters.events_delivered++;
    }
    /*
     * Drain unconditionally, even when this event reached nobody: an earlier call may
     * have left work queued (a bounded activation, an overflow that dropped the new
     * event while older ones waited), and an FSM must not stall until its next
     * input arrives. Empty queues make this a no-op.
     */
    for (i = 0u; i < engine->instance_count; ++i) {
        fsm_drain_instance(engine, &engine->instances[i]);
    }
    return CANCESTRY_FSM_OK;
}

cancestry_fsm_status_t cancestry_fsm_engine_inject_event(cancestry_fsm_engine_t *engine,
                                                         cancestry_fsm_instance_t *instance,
                                                         const cancestry_event_t *event)
{
    bool suppressed = false;

    if (!cancestry_fsm_engine_is_valid(engine) || instance == NULL || event == NULL) {
        return CANCESTRY_FSM_ERR_NULL;
    }
    if (!cancestry_event_is_valid(event)) {
        return CANCESTRY_FSM_ERR_ARGUMENT;
    }
    if (!cancestry_fsm_lifecycle_is_active(instance->lifecycle)) {
        return CANCESTRY_FSM_ERR_STATE;
    }
    if (!fsm_instance_accepts(engine, instance, event, &suppressed)) {
        if (suppressed) {
            instance->counters.events_suppressed++;
        }
        return CANCESTRY_FSM_ERR_NOT_FOUND;
    }
    if (!cancestry_event_queue_status_is_ok(cancestry_event_queue_push(&instance->incoming, event))) {
        instance->counters.events_dropped++;
        return CANCESTRY_FSM_ERR_QUEUE_FULL;
    }
    engine->counters.events_received++;
    engine->counters.events_delivered++;
    instance->counters.events_queued++;
    fsm_drain_instance(engine, instance);
    return CANCESTRY_FSM_OK;
}

cancestry_fsm_status_t cancestry_fsm_engine_tick(cancestry_fsm_engine_t *engine)
{
    size_t i;

    if (engine == NULL || !engine->ready) {
        return CANCESTRY_FSM_ERR_NULL;
    }
    engine->ticks++;
    engine->counters.ticks++;
    if (engine->owns_time_base) {
        /* No injected clock: the tick itself is the time base, exactly 1 ms
         * (SW-FR-FSM-030). */
        engine->now_us = fsm_sat_add(engine->now_us, 1000u);
    } else {
        engine->now_us = cancestry_clock_now_us(engine->clock);
    }

    for (i = 0u; i < engine->instance_count; ++i) {
        cancestry_fsm_instance_t *instance = &engine->instances[i];
        uint16_t t;

        if (!cancestry_fsm_lifecycle_is_active(instance->lifecycle)) {
            continue;
        }
        for (t = 0u; t < instance->timer_count; ++t) {
            cancestry_fsm_timer_state_t *slot = &instance->timers[t];
            cancestry_event_t event;
            uint32_t missed = 0u;
            bool suppressed = false;

            if (!fsm_timer_expired(instance, slot, engine->now_us, &missed)) {
                continue;
            }
            cancestry_event_init(&event);
            event.event_id = fsm_next_event_id(engine);
            event.type = CANCESTRY_EVENT_TYPE_TIMER_EXPIRED;
            event.priority_class = CANCESTRY_PRIORITY_CLASS_TIMER;
            event.timestamp_us = engine->now_us;
            event.cause_sequence = CANCESTRY_SEQUENCE_NONE;
            event.payload.timer_expired.timer_id = slot->id;
            event.payload.timer_expired.instance_id = instance->id;
            event.payload.timer_expired.missed_count = missed;
            event.payload.timer_expired.is_periodic = slot->repeat ? 1u : 0u;
            if (engine->global_queue != NULL &&
                cancestry_event_queue_is_valid(engine->global_queue)) {
                if (!cancestry_event_queue_status_is_ok(
                        cancestry_event_queue_push(engine->global_queue, &event))) {
                    engine->counters.global_queue_drops++;
                }
            }
            if (!fsm_instance_accepts(engine, instance, &event, &suppressed)) {
                if (suppressed) {
                    instance->counters.events_suppressed++;
                }
                continue;
            }
            if (!cancestry_event_queue_status_is_ok(
                    cancestry_event_queue_push(&instance->incoming, &event))) {
                instance->counters.events_dropped++;
            } else {
                instance->counters.events_queued++;
            }
            fsm_trace(engine, instance, CANCESTRY_FSM_TRACE_TIMER, (int32_t)missed, "%s expired",
                      slot->name);
        }
        fsm_drain_instance(engine, instance);
    }
    return CANCESTRY_FSM_OK;
}

cancestry_fsm_status_t cancestry_fsm_engine_advance(cancestry_fsm_engine_t *engine,
                                                     uint32_t tick_count)
{
    uint32_t i;

    if (engine == NULL) {
        return CANCESTRY_FSM_ERR_NULL;
    }
    for (i = 0u; i < tick_count; ++i) {
        cancestry_fsm_status_t status = cancestry_fsm_engine_tick(engine);

        if (status != CANCESTRY_FSM_OK) {
            return status;
        }
    }
    return CANCESTRY_FSM_OK;
}

/* ------------------------------------------------------------------------- */
/* Public API: engine and inspection                                         */
/* ------------------------------------------------------------------------- */

bool cancestry_fsm_engine_init(cancestry_fsm_engine_t *engine,
                               const cancestry_fsm_engine_config_t *config)
{
    size_t bound = 0u;
    size_t s;

    if (engine == NULL) {
        return false;
    }
    /*
     * Zero first, always: a failed init must leave the engine inert, and leaving
     * a previously valid engine in place after a rejected call would let callers
     * keep processing events on a half-configured engine (fail closed).
     */
    memset(engine, 0, sizeof(*engine));
    if (config == NULL) {
        return false;
    }
    if (config->sets == NULL || config->set_count == 0u || config->instances == NULL ||
        config->storage == NULL || config->instance_capacity == 0u) {
        return false;
    }
    if (config->trace_capacity != 0u && config->trace == NULL) {
        return false;
    }
    if (config->bindings == NULL && config->binding_count != 0u) {
        return false;
    }

    engine->sets = config->sets;
    engine->set_count = config->set_count;
    engine->instances = config->instances;
    engine->clock = config->clock;
    engine->capabilities = config->capabilities;
    engine->namespace = config->namespace;
    engine->signal_bus = config->signal_bus;
    engine->global_queue = config->global_queue;
    engine->bindings = config->bindings;
    engine->binding_count = config->binding_count;
    engine->governor = config->governor;
    engine->governor_user_data = config->governor_user_data;
    engine->sink = config->sink;
    engine->trace = config->trace;
    engine->trace_capacity = config->trace_capacity;
    engine->max_chain_depth = (config->max_chain_depth != 0u)
                                  ? config->max_chain_depth
                                  : CANCESTRY_FSM_TRANSITION_CHAIN_MAX_DEPTH;
    engine->max_actions_per_event = (config->max_actions_per_event != 0u)
                                        ? config->max_actions_per_event
                                        : CANCESTRY_FSM_MAX_ACTIONS_PER_EVENT;
    engine->max_events_per_activation =
        (config->max_events_per_activation != 0u) ? config->max_events_per_activation
                                                   : CANCESTRY_FSM_MAX_EVENTS_PER_ACTIVATION;
    engine->record_events = config->record_events;
    engine->now_us = (config->clock != NULL) ? cancestry_clock_now_us(config->clock) : 0u;
    engine->owns_time_base = (config->clock == NULL);
    engine->counters.next_event_id = 1u;

    for (s = 0u; s < config->set_count; ++s) {
        const cancestry_fsm_set_t *set = &config->sets[s];
        uint16_t j;

        for (j = 0u; j < set->instance_count; ++j) {
            const cancestry_fsm_instance_def_t *def = &set->instances[j];
            const cancestry_fsm_machine_t *machine;
            const cancestry_fsm_instance_storage_t *storage;
            cancestry_fsm_instance_t *instance;
            uint16_t k;

            if (set->machines == NULL || def->id == NULL || def->machine == NULL ||
                def->machine_index >= set->machine_count || bound >= config->instance_capacity) {
                memset(engine, 0, sizeof(*engine));
                return false;
            }
            machine = &set->machines[def->machine_index];
            storage = &config->storage[bound];
            /* Every table the machine needs must fit; refusing here is the only
             * honest option, since the engine may not allocate. */
            if (storage->event_slots == NULL || storage->event_capacity == 0u ||
                (machine->variable_count != 0u &&
                 (storage->variables == NULL ||
                  storage->variable_capacity < machine->variable_count)) ||
                (machine->timer_count != 0u &&
                 (storage->timers == NULL || storage->timer_capacity < machine->timer_count))) {
                memset(engine, 0, sizeof(*engine));
                return false;
            }
            /* Instance ids are the runtime handle, so they must be unique across
             * the whole engine, not merely within a file. */
            for (k = 0u; k < bound; ++k) {
                if (fsm_name_equals(engine->instances[k].def->id, def->id)) {
                    memset(engine, 0, sizeof(*engine));
                    return false;
                }
            }

            instance = &engine->instances[bound];
            memset(instance, 0, sizeof(*instance));
            instance->def = def;
            instance->machine = machine;
            instance->index = (uint16_t)bound;
            instance->id = (cancestry_instance_id_t)(bound + 1u);
            instance->variables = storage->variables;
            instance->variable_capacity = storage->variable_capacity;
            instance->timers = storage->timers;
            instance->timer_capacity = storage->timer_capacity;
            if (!cancestry_event_queue_init(&instance->incoming, storage->event_slots,
                                            storage->event_capacity)) {
                memset(engine, 0, sizeof(*engine));
                return false;
            }
            bound++;
        }
    }
    engine->instance_count = bound;
    engine->ready = true;

    /* Prepare every declared instance: variables, timers, lifecycle. An instance
     * declared enabled lands in READY, one declared disabled stays DISABLED
     * (SW-FR-FSM-004, fsm-spec.md section 3). */
    {
        size_t i;

        for (i = 0u; i < bound; ++i) {
            cancestry_fsm_instance_t *instance = &engine->instances[i];

            if (instance->def->enabled) {
                (void)cancestry_fsm_instance_enable(engine, instance);
            }
        }
    }
    return true;
}

bool cancestry_fsm_engine_is_valid(const cancestry_fsm_engine_t *engine)
{
    return engine != NULL && engine->ready;
}

size_t cancestry_fsm_engine_instance_count(const cancestry_fsm_engine_t *engine)
{
    if (engine == NULL || !engine->ready) {
        return 0u;
    }
    return engine->instance_count;
}

const cancestry_fsm_engine_counters_t *cancestry_fsm_engine_counters(
    const cancestry_fsm_engine_t *engine)
{
    if (engine == NULL || !engine->ready) {
        return NULL;
    }
    return &engine->counters;
}

cancestry_fsm_status_t cancestry_fsm_engine_totals(const cancestry_fsm_engine_t *engine,
                                                   cancestry_fsm_instance_counters_t *out)
{
    size_t i;

    if (out == NULL) {
        return CANCESTRY_FSM_ERR_NULL;
    }
    memset(out, 0, sizeof(*out));
    if (!cancestry_fsm_engine_is_valid(engine)) {
        return CANCESTRY_FSM_ERR_NULL;
    }
    for (i = 0u; i < engine->instance_count; ++i) {
        const cancestry_fsm_instance_counters_t *counters = &engine->instances[i].counters;

#define FSM_TOTAL(field) out->field += counters->field
        FSM_TOTAL(events_queued);
        FSM_TOTAL(events_processed);
        FSM_TOTAL(events_dropped);
        FSM_TOTAL(events_suppressed);
        FSM_TOTAL(transitions);
        FSM_TOTAL(deferred_transitions);
        FSM_TOTAL(chain_limit_exceeded);
        FSM_TOTAL(deferred_ignored);
        FSM_TOTAL(initialization_errors);
        FSM_TOTAL(guards_evaluated);
        FSM_TOTAL(guards_true);
        FSM_TOTAL(guards_false);
        FSM_TOTAL(guard_errors);
        FSM_TOTAL(actions_executed);
        FSM_TOTAL(action_errors);
        FSM_TOTAL(governor_denials);
        FSM_TOTAL(capability_denials);
        FSM_TOTAL(messages_sent);
        FSM_TOTAL(signals_set);
        FSM_TOTAL(variables_set);
        FSM_TOTAL(logs_emitted);
        FSM_TOTAL(faults_raised);
        FSM_TOTAL(timer_starts);
        FSM_TOTAL(timer_stops);
        FSM_TOTAL(timer_resets);
        FSM_TOTAL(timer_expiries);
        FSM_TOTAL(timer_missed_ticks);
        FSM_TOTAL(budget_exhausted);
#undef FSM_TOTAL
    }
    return CANCESTRY_FSM_OK;
}

cancestry_time_us_t cancestry_fsm_engine_now_us(const cancestry_fsm_engine_t *engine)
{
    return (engine == NULL) ? 0u : engine->now_us;
}

cancestry_fsm_instance_t *cancestry_fsm_instance_at(const cancestry_fsm_engine_t *engine,
                                                    size_t index)
{
    if (engine == NULL || !engine->ready || index >= engine->instance_count) {
        return NULL;
    }
    return &engine->instances[index];
}

cancestry_fsm_instance_t *cancestry_fsm_instance_by_id(const cancestry_fsm_engine_t *engine,
                                                       const char *id)
{
    size_t i;

    if (engine == NULL || !engine->ready || id == NULL) {
        return NULL;
    }
    for (i = 0u; i < engine->instance_count; ++i) {
        if (fsm_name_equals(engine->instances[i].def->id, id)) {
            return &engine->instances[i];
        }
    }
    return NULL;
}

cancestry_fsm_instance_lifecycle_t
cancestry_fsm_instance_lifecycle(const cancestry_fsm_instance_t *instance)
{
    if (instance == NULL) {
        return CANCESTRY_FSM_INSTANCE_LIFECYCLE_DISABLED;
    }
    return instance->lifecycle;
}

const char *cancestry_fsm_instance_state_name(const cancestry_fsm_instance_t *instance)
{
    const cancestry_fsm_state_t *state = fsm_current_state(instance);

    return (state != NULL) ? state->name : NULL;
}

const cancestry_fsm_instance_counters_t *cancestry_fsm_instance_counters(
    const cancestry_fsm_instance_t *instance)
{
    return (instance == NULL) ? NULL : &instance->counters;
}

cancestry_fsm_status_t cancestry_fsm_instance_get_variable(const cancestry_fsm_engine_t *engine,
                                                           cancestry_fsm_instance_t *instance,
                                                           const char *name,
                                                           cancestry_value_t *value_out)
{
    int index;

    if (!cancestry_fsm_engine_is_valid(engine) || instance == NULL || name == NULL ||
        value_out == NULL) {
        return CANCESTRY_FSM_ERR_NULL;
    }
    index = fsm_variable_index(instance, name);
    if (index < 0) {
        return CANCESTRY_FSM_ERR_NOT_FOUND;
    }
    *value_out = instance->variables[index].value;
    return CANCESTRY_FSM_OK;
}

cancestry_fsm_status_t cancestry_fsm_instance_get_timer(const cancestry_fsm_engine_t *engine,
                                                         cancestry_fsm_instance_t *instance,
                                                         const char *name,
                                                         cancestry_fsm_timer_state_t *state_out)
{
    int index;

    if (!cancestry_fsm_engine_is_valid(engine) || instance == NULL || name == NULL ||
        state_out == NULL) {
        return CANCESTRY_FSM_ERR_NULL;
    }
    index = fsm_timer_index(instance, name);
    if (index < 0) {
        return CANCESTRY_FSM_ERR_NOT_FOUND;
    }
    *state_out = instance->timers[index];
    return CANCESTRY_FSM_OK;
}

/* ------------------------------------------------------------------------- */
/* Public API: names                                                         */
/* ------------------------------------------------------------------------- */

const char *cancestry_fsm_trace_kind_name(cancestry_fsm_trace_kind_t kind)
{
    switch (kind) {
    case CANCESTRY_FSM_TRACE_EVENT:
        return "event";
    case CANCESTRY_FSM_TRACE_TRANSITION:
        return "transition";
    case CANCESTRY_FSM_TRACE_ACTION:
        return "action";
    case CANCESTRY_FSM_TRACE_ACTION_ERROR:
        return "action_error";
    case CANCESTRY_FSM_TRACE_GUARD_TRUE:
        return "guard_true";
    case CANCESTRY_FSM_TRACE_GUARD_FALSE:
        return "guard_false";
    case CANCESTRY_FSM_TRACE_GUARD_ERROR:
        return "guard_error";
    case CANCESTRY_FSM_TRACE_DENIED:
        return "denied";
    case CANCESTRY_FSM_TRACE_TIMER:
        return "timer";
    case CANCESTRY_FSM_TRACE_LIFECYCLE:
        return "lifecycle";
    case CANCESTRY_FSM_TRACE_WARNING:
        return "warning";
    case CANCESTRY_FSM_TRACE_FAULT:
        return "fault";
    default:
        return "unknown";
    }
}

const char *cancestry_fsm_status_name(cancestry_fsm_status_t status)
{
    switch (status) {
    case CANCESTRY_FSM_OK:
        return "ok";
    case CANCESTRY_FSM_ERR_NULL:
        return "null";
    case CANCESTRY_FSM_ERR_ARGUMENT:
        return "argument";
    case CANCESTRY_FSM_ERR_NOT_FOUND:
        return "not_found";
    case CANCESTRY_FSM_ERR_AMBIGUOUS:
        return "ambiguous";
    case CANCESTRY_FSM_ERR_DENIED:
        return "denied";
    case CANCESTRY_FSM_ERR_EXPRESSION:
        return "expression";
    case CANCESTRY_FSM_ERR_ENCODING:
        return "encoding";
    case CANCESTRY_FSM_ERR_CAPACITY:
        return "capacity";
    case CANCESTRY_FSM_ERR_PARSE:
        return "parse";
    case CANCESTRY_FSM_ERR_CONFLICT:
        return "conflict";
    case CANCESTRY_FSM_ERR_NO_MEMORY:
        return "no_memory";
    case CANCESTRY_FSM_ERR_UNSUPPORTED:
        return "unsupported";
    case CANCESTRY_FSM_ERR_STATE:
        return "state";
    case CANCESTRY_FSM_ERR_CHAIN_LIMIT:
        return "chain_limit";
    case CANCESTRY_FSM_ERR_BUDGET:
        return "budget";
    case CANCESTRY_FSM_ERR_QUEUE_FULL:
        return "queue_full";
    default:
        return "unknown";
    }
}

bool cancestry_fsm_status_is_ok(cancestry_fsm_status_t status)
{
    return status >= 0;
}

const char *cancestry_fsm_lifecycle_name(cancestry_fsm_instance_lifecycle_t lifecycle)
{
    switch (lifecycle) {
    case CANCESTRY_FSM_INSTANCE_LIFECYCLE_DISABLED:
        return "disabled";
    case CANCESTRY_FSM_INSTANCE_LIFECYCLE_READY:
        return "ready";
    case CANCESTRY_FSM_INSTANCE_LIFECYCLE_RUNNING:
        return "running";
    case CANCESTRY_FSM_INSTANCE_LIFECYCLE_SUSPENDED:
        return "suspended";
    case CANCESTRY_FSM_INSTANCE_LIFECYCLE_FAULT:
        return "fault";
    default:
        return "invalid";
    }
}

const char *cancestry_fsm_action_kind_name(cancestry_fsm_action_kind_t kind)
{
    switch (kind) {
    case CANCESTRY_FSM_ACTION_SEND_MESSAGE:
        return "send_message";
    case CANCESTRY_FSM_ACTION_SET_SIGNAL:
        return "set_signal";
    case CANCESTRY_FSM_ACTION_SET_VARIABLE:
        return "set_variable";
    case CANCESTRY_FSM_ACTION_START_TIMER:
        return "start_timer";
    case CANCESTRY_FSM_ACTION_STOP_TIMER:
        return "stop_timer";
    case CANCESTRY_FSM_ACTION_RESET_TIMER:
        return "reset_timer";
    case CANCESTRY_FSM_ACTION_LOG:
        return "log";
    case CANCESTRY_FSM_ACTION_RAISE_FAULT:
        return "raise_fault";
    case CANCESTRY_FSM_ACTION_TRANSITION:
        return "transition";
    default:
        return "invalid";
    }
}

const char *cancestry_fsm_log_level_name(cancestry_fsm_log_level_t level)
{
    switch (level) {
    case CANCESTRY_FSM_LOG_INFO:
        return "info";
    case CANCESTRY_FSM_LOG_WARNING:
        return "warning";
    case CANCESTRY_FSM_LOG_ERROR:
        return "error";
    default:
        return "invalid";
    }
}

const char *cancestry_fsm_event_type_name(cancestry_event_type_t type)
{
    switch (type) {
    case CANCESTRY_EVENT_TYPE_CAN_RX:
        return "can_rx";
    case CANCESTRY_EVENT_TYPE_SIGNAL_CHANGED:
        return "signal_changed";
    case CANCESTRY_EVENT_TYPE_TIMER_EXPIRED:
        return "timer_expired";
    case CANCESTRY_EVENT_TYPE_STATE_ENTERED:
        return "state_entered";
    case CANCESTRY_EVENT_TYPE_STATE_EXITED:
        return "state_exited";
    case CANCESTRY_EVENT_TYPE_FAULT_RAISED:
        return "fault_raised";
    case CANCESTRY_EVENT_TYPE_POWER_MODE_CHANGED:
        return "power_mode_changed";
    default:
        return "invalid";
    }
}

bool cancestry_fsm_lifecycle_is_active(cancestry_fsm_instance_lifecycle_t lifecycle)
{
    /*
     * Only RUNNING is active. READY has not entered its initial state yet,
     * SUSPENDED is a deliberate pause, FAULT is containment, and DISABLED never
     * runs (fsm-spec.md section 3). Keeping this one predicate is what makes the
     * "an inert instance cannot act" claim auditable: every delivery path and the
     * timer pass consult it.
     */
    return lifecycle == CANCESTRY_FSM_INSTANCE_LIFECYCLE_RUNNING;
}

cancestry_value_kind_t cancestry_fsm_variable_type_from_name(const char *name)
{
    if (fsm_name_equals(name, "boolean")) {
        return CANCESTRY_VALUE_KIND_BOOL;
    }
    if (fsm_name_equals(name, "integer")) {
        return CANCESTRY_VALUE_KIND_INT;
    }
    if (fsm_name_equals(name, "float")) {
        return CANCESTRY_VALUE_KIND_REAL;
    }
    return CANCESTRY_VALUE_KIND_UNSET;
}
