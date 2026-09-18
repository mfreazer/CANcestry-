/*
 * CANcestry - portable FSM runtime: engine API.
 *
 * Normative references:
 *   docs/packages/fsm-spec.md      (v0.3.0) sections 2-12
 *   docs/system/event-ordering.md  sections 4, 7, 8, 9, 11
 *   docs/system/governor.md        (v0.2.1 stub level) approval flow
 *   docs/system/expression-language.md  guard and operand evaluation
 *   docs/system/mode-fault-state-machine.md  section 5 (runtime reactions)
 *   docs/software/SwAD.md          sections 4, 6, 7, 8 (runtime, event loop,
 *                                  transitions, timers)
 *   docs/software/SwRS.md          SW-FR-FSM-004, 010..055
 *
 * Design notes:
 *   - Zero heap allocation on the execution path. Every table the engine mutates
 *     (instances, per-instance queues, variables, timers, trace records) is
 *     caller-owned storage handed in at init; cancestry_fsm_engine_process_event()
 *     and cancestry_fsm_engine_tick() never allocate (SYS-NF-002). CI verifies
 *     this by symbol-scanning the built archive.
 *   - No direct hardware access (SW-FR-FSM-025). Observable effects leave the
 *     engine through two channels only: the governor (approval) and the sink
 *     (delivery). Neither is a CAN driver; the platform decides what happens.
 *   - Determinism (SYS-NF-001, SW-FR-FSM-046): instance order is declaration
 *     order, transition order is declaration order, per-instance queue pop order
 *     is the normative selection order, and there is no hashing of iteration
 *     order, no wall-clock read and no unordered map.
 *   - Isolation (SW-FR-FSM-047): each instance owns its state, variables,
 *     timers, queue and counters, so suspending one instance leaves the others
 *     untouched.
 *   - The engine is single-threaded; the caller serialises access.
 */

#ifndef CANCESTRY_FSM_ENGINE_H
#define CANCESTRY_FSM_ENGINE_H

#include "cancestry/codec/namespace.h"
#include "cancestry/event/clock.h"
#include "cancestry/event/queue.h"
#include "cancestry/fsm/types.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Maximum length of a trace record's detail text, including the NUL. */
#define CANCESTRY_FSM_TRACE_DETAIL_MAX ((size_t)48u)

/* ------------------------------------------------------------------------- */
/* Variable and timer runtime state                                          */
/* ------------------------------------------------------------------------- */

/**
 * One instance-scoped variable slot (SW-FR-FSM-031, SW-FR-FSM-034).
 *
 * The name is borrowed from the machine definition; the value is the live
 * runtime value. @c declared records the schema type so that assignments can be
 * type-checked (invalid coercion is a fault, not a truncation).
 */
typedef struct cancestry_fsm_variable_slot {
    const char *name;
    cancestry_value_kind_t declared;
    cancestry_value_t value;
    bool initialized;
} cancestry_fsm_variable_slot_t;

/** Timer runtime state snapshot, returned by the inspection hook. */
typedef struct cancestry_fsm_timer_state {
    /** Timer name, borrowed from the definition. NULL when the slot is unused. */
    const char *name;
    /** Stable per-machine timer id (1-based index into the machine's timers). */
    cancestry_timer_id_t id;
    bool running;
    bool repeat;
    /** Period in milliseconds. */
    uint32_t duration_ms;
    /** Absolute deadline of the next expiry, in microseconds. */
    cancestry_time_us_t deadline_us;
    /** Number of expiries emitted for this timer. */
    uint32_t expires;
    /** Cumulative missed periodic ticks (one event per expiry; fsm-spec 9). */
    uint32_t missed_ticks;
} cancestry_fsm_timer_state_t;

/* ------------------------------------------------------------------------- */
/* Capabilities (SW-FR-FSM-023, 039..041)                                    */
/* ------------------------------------------------------------------------- */

/** One declared interface capability, mirroring the package manifest. */
typedef struct cancestry_fsm_interface_capability {
    /** Physical interface name, e.g. "can0". Never NULL. */
    const char *name;
    /** Interned interface id; must match the id the events carry. */
    cancestry_interface_id_t id;
    /** Receive permission (informational for the FSM; TX is enforced here). */
    bool rx;
    /** Transmit permission. */
    bool tx;
    /** TX CAN id allowlist; NULL or @c tx_id_count 0 means "no id is allowed". */
    const uint32_t *tx_ids;
    uint16_t tx_id_count;
} cancestry_fsm_interface_capability_t;

/**
 * Package capabilities as they apply to an FSM instance.
 *
 * The package loader (a later phase) builds this from the manifest; the engine
 * only reads it. Absent capabilities deny every governed action, matching the
 * governor's fail-closed default (SW-FR-GOV-006).
 */
typedef struct cancestry_fsm_capabilities {
    const cancestry_fsm_interface_capability_t *interfaces;
    uint16_t interface_count;
    /** Signal read allowlist for sig.NAME in expressions; NULL means "no reads". */
    const char *const *signal_read;
    uint16_t signal_read_count;
    /** Signal write allowlist for set_signal; NULL means "no writes". */
    const char *const *signal_write;
    uint16_t signal_write_count;
} cancestry_fsm_capabilities_t;

/* ------------------------------------------------------------------------- */
/* Signal bus                                                                */
/* ------------------------------------------------------------------------- */

/**
 * Signal read/write adapter.
 *
 * The FSM runtime owns no signal storage: reading and writing shared signal
 * values goes through this adapter so the same runtime works against a host
 * simulation bus, a replay harness, or a firmware signal table. Every callback
 * must be allocation-free. @c resolve is optional and only used to attach a
 * stable signal id to the generated signal_changed event.
 */
typedef struct cancestry_fsm_signal_bus {
    void *user_data;
    /**
     * Read the current value of @p name.
     * @return CANCESTRY_FSM_OK, or CANCESTRY_FSM_ERR_NOT_FOUND when the signal
     *         has no value yet.
     */
    cancestry_fsm_status_t (*read)(void *user_data,
                                   const char *name,
                                   cancestry_value_t *value_out);
    /**
     * Write @p value to @p name.
     * @return CANCESTRY_FSM_OK or a negative status.
     */
    cancestry_fsm_status_t (*write)(void *user_data,
                                    const char *name,
                                    const cancestry_value_t *value);
    /** Optional: map a signal name to its stable id, for event payloads. */
    cancestry_fsm_status_t (*resolve)(void *user_data,
                                      const char *name,
                                      cancestry_signal_id_t *id_out);
} cancestry_fsm_signal_bus_t;

/* ------------------------------------------------------------------------- */
/* Governor stub (SW-FR-FSM-042, docs/system/governor.md)                    */
/* ------------------------------------------------------------------------- */

/** Governor decision. Anything but APPROVE is a denial; there is no "maybe". */
typedef enum cancestry_fsm_governor_decision {
    CANCESTRY_FSM_GOVERNOR_APPROVE = 0,
    CANCESTRY_FSM_GOVERNOR_DENY = 1
} cancestry_fsm_governor_decision_t;

/** Kind of side effect requesting approval. */
typedef enum cancestry_fsm_governor_request_kind {
    CANCESTRY_FSM_GOVERNOR_SEND_MESSAGE = 0,
    CANCESTRY_FSM_GOVERNOR_SET_SIGNAL = 1
} cancestry_fsm_governor_request_kind_t;

/**
 * One side-effect approval request.
 *
 * Mirrors cancestry_recipe_governor_request_t at the same stub level: the
 * engine builds it on the stack and calls the governor synchronously before the
 * effect happens, so a denial leaves no partial effect. All pointers are valid
 * only for the duration of the callback.
 */
typedef struct cancestry_fsm_governor_request {
    cancestry_fsm_governor_request_kind_t kind;
    /** Instance requesting the side effect. */
    const struct cancestry_fsm_instance *instance;
    cancestry_instance_id_t instance_id;
    /** Event that caused the action, or NULL (initial entry, tick-driven). */
    const cancestry_event_t *cause;
    /* send_message */
    /** Resolved physical interface name. */
    const char *interface_name;
    cancestry_interface_id_t interface_id;
    /** Message name as written in the FSM file. */
    const char *message_name;
    /** CAN id resolved through the codec map; 0 when unresolved. */
    uint32_t can_id;
    /** Encoded frame bytes, @c frame_length long. */
    const uint8_t *frame;
    uint8_t frame_length;
    /* set_signal */
    const char *signal_name;
    cancestry_signal_id_t signal_id;
    const cancestry_value_t *value;
} cancestry_fsm_governor_request_t;

/**
 * Governor stub: a function pointer the platform (or the test suite) provides.
 *
 * NULL means deny everything (fail closed). The real governor - TX allowlists,
 * token buckets, SAFE-mode escalation - plugs in here without engine changes.
 */
typedef cancestry_fsm_governor_decision_t (*cancestry_fsm_governor_fn)(
    void *user_data, const cancestry_fsm_governor_request_t *request);

/* ------------------------------------------------------------------------- */
/* Sink (observability and delivery)                                         */
/* ------------------------------------------------------------------------- */

/** One action execution, passed to the sink callbacks for context. */
typedef struct cancestry_fsm_invocation {
    const struct cancestry_fsm_engine *engine;
    struct cancestry_fsm_instance *instance;
    const cancestry_fsm_instance_def_t *def;
    cancestry_instance_id_t instance_id;
    /** Event being processed; NULL for initial entry and for tick-only work. */
    const cancestry_event_t *event;
    /** Action being executed; NULL when the engine is not inside an action. */
    const cancestry_fsm_action_t *action;
    /** State whose entry/exit actions are running, or NULL for a transition's. */
    const char *state;
    /** Transition that selected @c action, or NULL for state entry/exit actions. */
    const cancestry_fsm_transition_t *transition;
    /** 1-based position of this transition in the current chain (SW-FR-FSM-016). */
    uint16_t chain_depth;
} cancestry_fsm_invocation_t;

/**
 * Runtime sink: the engine's only delivery path for approved effects, logs,
 * faults and timer notifications.
 *
 * Every callback is optional. An action whose only effect is a missing callback
 * fails with CANCESTRY_FSM_ERR_UNSUPPORTED, which is recorded and counted
 * (fail-closed, SW-FR-FSM-024): the engine never pretends an effect happened.
 */
typedef struct cancestry_fsm_sink {
    void *user_data;
    /** Approved send_message, with the encoded frame (no hardware access here). */
    void (*on_send_message)(void *user_data,
                            const cancestry_fsm_invocation_t *invocation,
                            const char *interface_name,
                            cancestry_interface_id_t interface_id,
                            const char *message_name,
                            uint32_t can_id,
                            const uint8_t *frame,
                            uint8_t frame_length);
    /** Approved set_signal, after the write reached the signal bus. */
    void (*on_signal_write)(void *user_data,
                            const cancestry_fsm_invocation_t *invocation,
                            const char *signal_name,
                            cancestry_signal_id_t signal_id,
                            const cancestry_value_t *old_value,
                            const cancestry_value_t *new_value);
    /** log action. */
    void (*on_log)(void *user_data,
                   const cancestry_fsm_invocation_t *invocation,
                   cancestry_fsm_log_level_t level,
                   const char *message);
    /** raise_fault action; @c numeric_code is the stable hash of @c code. */
    void (*on_fault)(void *user_data,
                      const cancestry_fsm_invocation_t *invocation,
                      const char *code,
                      cancestry_fault_severity_t severity,
                      cancestry_fault_code_t numeric_code);
    /** Timer requests (start_timer / stop_timer / reset_timer). */
    void (*on_timer)(void *user_data,
                      const cancestry_fsm_invocation_t *invocation,
                      cancestry_fsm_action_kind_t kind,
                      const char *timer_name,
                      uint32_t duration_ms,
                      bool repeat);
    /** Completed transition: @p from is NULL for the initial entry. */
    void (*on_state_change)(void *user_data,
                            const cancestry_fsm_invocation_t *invocation,
                            const char *from,
                            const char *to);
    /**
     * Warning the runtime must surface but does not act on, such as a second
     * transition action in one action sequence (SW-FR-FSM-055) or an exhausted
     * execution budget (SW-FR-FSM-045).
     */
    void (*on_warning)(void *user_data,
                       const cancestry_fsm_invocation_t *invocation,
                       const char *text);
} cancestry_fsm_sink_t;

/* ------------------------------------------------------------------------- */
/* Trace (SW-FR-FSM-048, 049, 053)                                           */
/* ------------------------------------------------------------------------- */

/** Trace record kinds. Values are stable: golden output depends on them. */
typedef enum cancestry_fsm_trace_kind {
    CANCESTRY_FSM_TRACE_EVENT = 0,
    CANCESTRY_FSM_TRACE_TRANSITION = 1,
    CANCESTRY_FSM_TRACE_ACTION = 2,
    CANCESTRY_FSM_TRACE_ACTION_ERROR = 3,
    CANCESTRY_FSM_TRACE_GUARD_TRUE = 4,
    CANCESTRY_FSM_TRACE_GUARD_FALSE = 5,
    CANCESTRY_FSM_TRACE_GUARD_ERROR = 6,
    CANCESTRY_FSM_TRACE_DENIED = 7,
    CANCESTRY_FSM_TRACE_TIMER = 8,
    CANCESTRY_FSM_TRACE_LIFECYCLE = 9,
    CANCESTRY_FSM_TRACE_WARNING = 10,
    CANCESTRY_FSM_TRACE_FAULT = 11,
    CANCESTRY_FSM_TRACE_KIND_COUNT
} cancestry_fsm_trace_kind_t;

/**
 * One trace record.
 *
 * @c detail is a NUL-terminated rendering produced by the engine
 * deterministically (names, ids and state pairs), which is what makes golden
 * output tests possible without a serialiser (SW-FR-FSM-053).
 */
typedef struct cancestry_fsm_trace_record {
    cancestry_time_us_t timestamp_us;
    cancestry_instance_id_t instance_id;
    cancestry_fsm_trace_kind_t kind;
    /** Status code or count attached to the record; 0 when not applicable. */
    int32_t code;
    char detail[CANCESTRY_FSM_TRACE_DETAIL_MAX];
} cancestry_fsm_trace_record_t;

/** @return Stable name such as "transition". Never NULL. */
const char *cancestry_fsm_trace_kind_name(cancestry_fsm_trace_kind_t kind);

/* ------------------------------------------------------------------------- */
/* Counters (SW-FR-FSM-050)                                                  */
/* ------------------------------------------------------------------------- */

/** Per-instance counters. Monotonic; never reset by the engine. */
typedef struct cancestry_fsm_instance_counters {
    /** Events accepted into the instance's incoming queue. */
    uint32_t events_queued;
    /** Events popped and processed. */
    uint32_t events_processed;
    /** Events refused by the queue's overflow policy (SW-FR-FSM-020). */
    uint32_t events_dropped;
    /** Events of a class the instance does not subscribe to. */
    uint32_t events_suppressed;
    /** Event-driven and deferred transitions executed. */
    uint32_t transitions;
    /** Transitions executed because of a transition action. */
    uint32_t deferred_transitions;
    /** Chains refused by the depth limit (SW-FR-FSM-016). */
    uint32_t chain_limit_exceeded;
    /** transition actions ignored because one was already pending (SW-FR-FSM-055). */
    uint32_t deferred_ignored;
    /** Variable defaults that failed to initialize (SW-FR-FSM-033). */
    uint32_t initialization_errors;
    uint32_t guards_evaluated;
    uint32_t guards_true;
    uint32_t guards_false;
    /** Guard evaluations that faulted (SW-FR-FSM-038). */
    uint32_t guard_errors;
    uint32_t actions_executed;
    /** Actions that failed for any reason, including denials. */
    uint32_t action_errors;
    /** Denials by the governor (SW-FR-GOV-005 violation counter). */
    uint32_t governor_denials;
    /** Denials by the capability check, before the governor is consulted. */
    uint32_t capability_denials;
    uint32_t messages_sent;
    uint32_t signals_set;
    /** set_variable actions applied; a refused coercion is an action error. */
    uint32_t variables_set;
    uint32_t logs_emitted;
    uint32_t faults_raised;
    uint32_t timer_starts;
    uint32_t timer_stops;
    uint32_t timer_resets;
    uint32_t timer_expiries;
    /** Sum of missed_count over every periodic expiry. */
    uint32_t timer_missed_ticks;
    /** Activations cut short by an execution budget (SW-FR-FSM-045). */
    uint32_t budget_exhausted;
} cancestry_fsm_instance_counters_t;

/**
 * Engine counters. Per-instance quantities live in
 * ::cancestry_fsm_instance_counters_t; cancestry_fsm_engine_totals() sums them,
 * so no quantity is ever stored twice and can therefore disagree with itself.
 */
typedef struct cancestry_fsm_engine_counters {
    /** External events handed to cancestry_fsm_engine_process_event(). */
    uint32_t events_received;
    /** Events that reached at least one instance queue. */
    uint32_t events_delivered;
    /** Events that reached no instance (all inactive or all filtered out). */
    uint32_t events_ignored;
    /** Events refused by the global queue's overflow policy. */
    uint32_t global_queue_drops;
    /** Timer ticks performed by cancestry_fsm_engine_tick(). */
    uint32_t ticks;
    /** Monotonic id source for events this engine produces (event-ordering.md 10). */
    cancestry_event_id_t next_event_id;
} cancestry_fsm_engine_counters_t;

/* ------------------------------------------------------------------------- */
/* Instance                                                                  */
/* ------------------------------------------------------------------------- */

/**
 * Caller-owned per-instance storage.
 *
 * Each instance needs three bounded tables. Sizes are the caller's choice; the
 * engine refuses (ERR_CAPACITY) when a table cannot hold the machine's timers
 * or variables. The incoming queue uses the shared Path A reserve policy
 * (SW-FR-FSM-019: default depth 64; SW-FR-FSM-020).
 */
typedef struct cancestry_fsm_instance_storage {
    /** Incoming event queue storage. */
    cancestry_event_t *event_slots;
    uint16_t event_capacity;
    /** Variable storage; at least machine->variable_count. */
    cancestry_fsm_variable_slot_t *variables;
    uint16_t variable_capacity;
    /** Timer storage; at least machine->timer_count. */
    cancestry_fsm_timer_state_t *timers;
    uint16_t timer_capacity;
} cancestry_fsm_instance_storage_t;

/**
 * A running FSM instance: definition plus mutable state (SwAD.md section 5).
 *
 * Instances live in caller-owned storage; the engine initialises them and owns
 * their contents afterwards.
 */
typedef struct cancestry_fsm_instance {
    const cancestry_fsm_instance_def_t *def;
    const cancestry_fsm_machine_t *machine;
    /** 0-based index in the engine's instance array (dispatch order). */
    uint16_t index;
    /** Stable runtime id: index + 1, matching cancestry_instance_id_t. */
    cancestry_instance_id_t id;
    cancestry_fsm_instance_lifecycle_t lifecycle;
    /** Current state index; only meaningful when @c has_state is true. */
    uint16_t state_index;
    bool has_state;
    /** Bounded incoming queue (SW-FR-FSM-019/020). */
    cancestry_event_queue_t incoming;
    cancestry_fsm_variable_slot_t *variables;
    uint16_t variable_capacity;
    uint16_t variable_count;
    cancestry_fsm_timer_state_t *timers;
    uint16_t timer_capacity;
    uint16_t timer_count;
    /** Deferred transition requested by a transition action (SW-FR-FSM-054). */
    bool deferred_pending;
    uint16_t deferred_target;
    /** Set while an activation is in progress, to reject re-entrant entry. */
    bool processing;
    cancestry_fsm_instance_counters_t counters;
} cancestry_fsm_instance_t;

/* ------------------------------------------------------------------------- */
/* Engine                                                                    */
/* ------------------------------------------------------------------------- */

/** Borrowed engine configuration; copied into the engine at init. */
typedef struct cancestry_fsm_engine_config {
    /**
     * Loaded definitions, in load order (SW-FR-FSM-001). A package may ship
     * several FSM files; their instances form one dispatch sequence: set order,
     * then instance declaration order within the set
     * (docs/system/event-ordering.md section 7). At least one set is required.
     */
    const cancestry_fsm_set_t *sets;
    size_t set_count;
    /** Caller-owned instance array and per-instance storage, parallel to it. */
    cancestry_fsm_instance_t *instances;
    const cancestry_fsm_instance_storage_t *storage;
    size_t instance_capacity;
    /** Monotonic clock. NULL selects the engine's internal 1 ms tick clock. */
    const cancestry_clock_t *clock;
    /** Package capabilities. NULL denies every governed action (fail closed). */
    const cancestry_fsm_capabilities_t *capabilities;
    /** Codec namespace for send_message message/signal resolution; may be NULL. */
    const cancestry_codec_namespace_t *namespace;
    /** Signal bus; NULL makes sig.NAME reads undefined and set_signal fail. */
    const cancestry_fsm_signal_bus_t *signal_bus;
    /** Global event queue for generated events; may be NULL. */
    cancestry_event_queue_t *global_queue;
    /** Package-level interface aliases; instance bindings override these. */
    const cancestry_fsm_binding_t *bindings;
    uint16_t binding_count;
    /** Governor stub; NULL denies every side effect (SW-FR-GOV-006). */
    cancestry_fsm_governor_fn governor;
    void *governor_user_data;
    /** Runtime sink; may be NULL, which makes effects fail as unsupported. */
    const cancestry_fsm_sink_t *sink;
    /** Trace ring storage; NULL disables tracing. */
    cancestry_fsm_trace_record_t *trace;
    uint16_t trace_capacity;
    /** Transition chain limit; 0 selects CANCESTRY_FSM_TRANSITION_CHAIN_MAX_DEPTH. */
    uint16_t max_chain_depth;
    /** Actions per event budget; 0 selects CANCESTRY_FSM_MAX_ACTIONS_PER_EVENT. */
    uint16_t max_actions_per_event;
    /** Internally generated events consumed per activation; 0 selects the default. */
    uint16_t max_events_per_activation;
    /** Record one trace EVENT record per processed event (SW-FR-FSM-049). */
    bool record_events;
} cancestry_fsm_engine_config_t;

/** The FSM engine. */
typedef struct cancestry_fsm_engine {
    const cancestry_fsm_set_t *sets;
    size_t set_count;
    cancestry_fsm_instance_t *instances;
    size_t instance_count;
    const cancestry_clock_t *clock;
    cancestry_time_us_t now_us;
    uint64_t ticks;
    bool owns_time_base;
    const cancestry_fsm_capabilities_t *capabilities;
    const cancestry_codec_namespace_t *namespace;
    const cancestry_fsm_signal_bus_t *signal_bus;
    cancestry_event_queue_t *global_queue;
    const cancestry_fsm_binding_t *bindings;
    uint16_t binding_count;
    cancestry_fsm_governor_fn governor;
    void *governor_user_data;
    const cancestry_fsm_sink_t *sink;
    cancestry_fsm_trace_record_t *trace;
    uint16_t trace_capacity;
    uint16_t trace_count;
    uint16_t trace_next;
    uint32_t trace_dropped;
    uint16_t max_chain_depth;
    uint16_t max_actions_per_event;
    uint16_t max_events_per_activation;
    bool record_events;
    cancestry_fsm_engine_counters_t counters;
    bool ready;
} cancestry_fsm_engine_t;

/**
 * Initialize an engine over borrowed storage and definitions.
 *
 * @p sets (with @p set_count at least 1) and @p instances are required;
 * @p storage may be NULL only when
 * @p instance_capacity is 0. Every instance is prepared (variables
 * initialised, timer table bound) and left DISABLED or READY according to its
 * declaration (SW-FR-FSM-004).
 *
 * @return true when the engine is usable. On failure the engine is zeroed,
 *         which leaves it safely inert.
 */
bool cancestry_fsm_engine_init(cancestry_fsm_engine_t *engine,
                               const cancestry_fsm_engine_config_t *config);

/** @return true when @p engine is non-NULL and initialized. */
bool cancestry_fsm_engine_is_valid(const cancestry_fsm_engine_t *engine);

/** @return Number of bound instances, or 0 when @p engine is invalid. */
size_t cancestry_fsm_engine_instance_count(const cancestry_fsm_engine_t *engine);

/** @return Pointer to the live engine counters, or NULL when invalid. */
const cancestry_fsm_engine_counters_t *cancestry_fsm_engine_counters(
    const cancestry_fsm_engine_t *engine);

/**
 * Sum the per-instance counters of every bound instance.
 *
 * @return CANCESTRY_FSM_OK, or CANCESTRY_FSM_ERR_NULL for a NULL engine or
 *         @p out. @p out is zeroed before accumulation, so a failed call
 *         leaves it defined.
 */
cancestry_fsm_status_t cancestry_fsm_engine_totals(const cancestry_fsm_engine_t *engine,
                                                   cancestry_fsm_instance_counters_t *out);

/** @return Current engine time in microseconds (the tick clock or the injected one). */
cancestry_time_us_t cancestry_fsm_engine_now_us(const cancestry_fsm_engine_t *engine);

/** @return Number of retained trace records, oldest first. */
size_t cancestry_fsm_engine_trace_count(const cancestry_fsm_engine_t *engine);

/** @return Trace record @p index, or NULL when out of range. */
const cancestry_fsm_trace_record_t *cancestry_fsm_engine_trace_record(
    const cancestry_fsm_engine_t *engine, size_t index);

/** @return Number of trace records discarded because the ring was full. */
uint32_t cancestry_fsm_engine_trace_dropped(const cancestry_fsm_engine_t *engine);

/**
 * Render retained trace records as deterministic text, one per line.
 *
 * This is the golden-output hook (SW-FR-FSM-053): the rendering depends on
 * nothing but the records, so the same event sequence and time base reproduce
 * byte-identical output.
 *
 * @return Number of characters written, excluding the NUL, or 0 on bad
 *         arguments. Output is truncated to @p size - 1 characters.
 */
size_t cancestry_fsm_engine_trace_render(const cancestry_fsm_engine_t *engine,
                                         char *buffer,
                                         size_t size);

/** Discard retained trace records; counters and dropped count are kept. */
void cancestry_fsm_engine_trace_clear(cancestry_fsm_engine_t *engine);

/* ------------------------------------------------------------------------- */
/* Instance manager: lifecycle (fsm-spec.md section 3)                       */
/* ------------------------------------------------------------------------- */

/** @return Instance at @p index in declaration order, or NULL. */
cancestry_fsm_instance_t *cancestry_fsm_instance_at(const cancestry_fsm_engine_t *engine,
                                                    size_t index);

/** @return Instance whose declaration id equals @p id, or NULL. */
cancestry_fsm_instance_t *cancestry_fsm_instance_by_id(const cancestry_fsm_engine_t *engine,
                                                       const char *id);

/** @return Current lifecycle state, or DISABLED for a NULL instance. */
cancestry_fsm_instance_lifecycle_t cancestry_fsm_instance_lifecycle(
    const cancestry_fsm_instance_t *instance);

/** @return Current state name, or NULL when no state has been entered. */
const char *cancestry_fsm_instance_state_name(const cancestry_fsm_instance_t *instance);

/** @return Pointer to the live per-instance counters, or NULL. */
const cancestry_fsm_instance_counters_t *cancestry_fsm_instance_counters(
    const cancestry_fsm_instance_t *instance);

/**
 * DISABLED -> READY: validate the binding of definitions and initialise
 * variables and timers to their declared defaults (SW-FR-FSM-033).
 */
cancestry_fsm_status_t cancestry_fsm_instance_enable(cancestry_fsm_engine_t *engine,
                                                     cancestry_fsm_instance_t *instance);

/**
 * READY -> RUNNING: enter the initial state, running its entry actions and
 * starting auto_start timers. The instance's incoming queue is cleared first,
 * so a start never replays stale events (fail-closed).
 */
cancestry_fsm_status_t cancestry_fsm_instance_start(cancestry_fsm_engine_t *engine,
                                                    cancestry_fsm_instance_t *instance);

/** RUNNING -> SUSPENDED: the instance becomes inert; state and variables stay. */
cancestry_fsm_status_t cancestry_fsm_instance_suspend(cancestry_fsm_engine_t *engine,
                                                      cancestry_fsm_instance_t *instance);

/** SUSPENDED -> RUNNING: continue where the instance stopped. */
cancestry_fsm_status_t cancestry_fsm_instance_resume(cancestry_fsm_engine_t *engine,
                                                      cancestry_fsm_instance_t *instance);

/** RUNNING or SUSPENDED -> FAULT: critical containment, recovered only by reset. */
cancestry_fsm_status_t cancestry_fsm_instance_fault(cancestry_fsm_engine_t *engine,
                                                    cancestry_fsm_instance_t *instance);

/** FAULT or SUSPENDED -> READY: drop queued events, reinitialise, stop timers. */
cancestry_fsm_status_t cancestry_fsm_instance_reset(cancestry_fsm_engine_t *engine,
                                                     cancestry_fsm_instance_t *instance);

/** Any -> DISABLED: the instance stops and may not process events. */
cancestry_fsm_status_t cancestry_fsm_instance_disable(cancestry_fsm_engine_t *engine,
                                                       cancestry_fsm_instance_t *instance);

/* ------------------------------------------------------------------------- */
/* Event dispatcher (SW-FR-FSM-010, 012, 043, 054)                           */
/* ------------------------------------------------------------------------- */

/**
 * Deliver one external event to every active instance and process the results.
 *
 * The event is pushed into each subscribing, running instance's incoming queue
 * (queue policy applies), then every instance drains its queue in declaration
 * order, and each instance processes its events one at a time in the normative
 * selection order (SW-FR-FSM-043). Deferred transitions requested by a
 * transition action run to completion before this function returns, which is
 * what "deferred transitions run before the next external event" means
 * (fsm-spec.md section 8).
 *
 * Per-instance failures are recorded and confined to that instance
 * (SW-FR-FSM-024, SW-FR-FSM-047); the return status only reports engine-level
 * problems.
 *
 * @return CANCESTRY_FSM_OK, CANCESTRY_FSM_ERR_NULL, or
 *         CANCESTRY_FSM_ERR_ARGUMENT for a malformed event.
 */
cancestry_fsm_status_t cancestry_fsm_engine_process_event(cancestry_fsm_engine_t *engine,
                                                          const cancestry_event_t *event);

/**
 * Test/simulation hook: push one event into a single instance's queue and drain
 * that instance only (SW-FR-FSM-052). The same rules as
 * cancestry_fsm_engine_process_event() apply to the instance.
 *
 * @return CANCESTRY_FSM_OK, a negative status for bad arguments, or
 *         CANCESTRY_FSM_ERR_QUEUE_FULL when the event was dropped by policy.
 */
cancestry_fsm_status_t cancestry_fsm_engine_inject_event(cancestry_fsm_engine_t *engine,
                                                         cancestry_fsm_instance_t *instance,
                                                         const cancestry_event_t *event);

/**
 * One 1 ms timer tick (SW-FR-FSM-030, SW-FR-FSM-044).
 *
 * Advances the time base (the injected clock is read; without one the engine
 * advances its own clock by exactly 1000 us), evaluates every active instance's
 * timers, queues one timer_expired event per expired timer - carrying
 * @c missed_count for skipped periodic periods - and then drains the instance
 * queues.
 *
 * @return CANCESTRY_FSM_OK, or CANCESTRY_FSM_ERR_NULL for a NULL engine.
 */
cancestry_fsm_status_t cancestry_fsm_engine_tick(cancestry_fsm_engine_t *engine);

/**
 * Convenience: @p tick_count ticks in sequence, each one evaluated and drained
 * before the next (no coalescing across ticks).
 *
 * @return CANCESTRY_FSM_OK or CANCESTRY_FSM_ERR_NULL.
 */
cancestry_fsm_status_t cancestry_fsm_engine_advance(cancestry_fsm_engine_t *engine,
                                                    uint32_t tick_count);

/* ------------------------------------------------------------------------- */
/* Inspection hooks (SW-FR-FSM-052)                                          */
/* ------------------------------------------------------------------------- */

/**
 * Read an instance-scoped variable.
 *
 * @return CANCESTRY_FSM_OK, CANCESTRY_FSM_ERR_NULL, or
 *         CANCESTRY_FSM_ERR_NOT_FOUND when the machine declares no such variable.
 */
cancestry_fsm_status_t cancestry_fsm_instance_get_variable(const cancestry_fsm_engine_t *engine,
                                                           cancestry_fsm_instance_t *instance,
                                                           const char *name,
                                                           cancestry_value_t *value_out);

/**
 * Read a timer's runtime state.
 *
 * @return CANCESTRY_FSM_OK or CANCESTRY_FSM_ERR_NOT_FOUND.
 */
cancestry_fsm_status_t cancestry_fsm_instance_get_timer(const cancestry_fsm_engine_t *engine,
                                                        cancestry_fsm_instance_t *instance,
                                                        const char *name,
                                                        cancestry_fsm_timer_state_t *state_out);

/*
 * Expression evaluation is deliberately not part of this header: guards and
 * action operands are the only expressions an FSM may contain
 * (docs/system/expression-language.md section 1), and both are evaluated by the
 * engine. The conformance suite therefore exercises the shipping path rather
 * than a parallel public evaluator API.
 */

/**
 * @return Stable 32-bit FNV-1a hash of a fault code string, never 0.
 *
 * raise_fault events carry a numeric fault code; this hash maps the FSM's
 * string code into it deterministically, identically to core/recipe so one
 * fault code string yields one numeric code runtime-wide.
 */
cancestry_fault_code_t cancestry_fsm_fault_code_hash(const char *code);

#ifdef __cplusplus
}
#endif

#endif /* CANCESTRY_FSM_ENGINE_H */
