/*
 * CANcestry - portable FSM runtime: shared types.
 *
 * Normative references:
 *   docs/packages/fsm-spec.md          (v0.2.1) sections 2-11
 *   schemas/fsm-0.2.0.schema.json      FSM file v0.2.0 schema (the schema is law)
 *   docs/software/SwRS.md              SW-FR-FSM-001 .. SW-FR-FSM-055
 *   docs/software/SwAD.md              sections 4, 5, 7 (FSM runtime, data model, transitions)
 *   docs/system/SyRS.md                SYS-NF-001 (determinism), SYS-NF-002 (bounded resources)
 *
 * Design notes:
 *   - A loaded FSM file is a single, self-contained allocation produced by the
 *     loader (core/fsm/src/loader.c). Every array and string pointer inside it
 *     refers into that allocation, so the runtime path only reads borrowed
 *     pointers and never allocates (SYS-NF-002).
 *   - "Compiling" a definition (SW-FR-FSM-002) means resolving every state and
 *     machine reference to an index and validating every expression's grammar at
 *     load time. After loading, the runtime never looks up a name to find a
 *     state: name-to-index maps are built once, in the loader.
 *   - Strings are borrowed pointers with the lifetime of the FSM set; the
 *     runtime copies nothing and frees nothing.
 *   - Hierarchical statecharts are deliberately absent: the v0.3 runtime
 *     supports flat states only (SW-FR-FSM-009).
 *   - Every enumeration here mirrors a schema enumeration; adding a value here
 *     without the schema is out of spec and must be rejected by the loader.
 */

#ifndef CANCESTRY_FSM_TYPES_H
#define CANCESTRY_FSM_TYPES_H

#include "cancestry/event/types.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------------- */
/* Bounds (bounded resources: SYS-NF-002)                                    */
/* ------------------------------------------------------------------------- */

/** Maximum length of state, machine, instance, timer, variable, signal and message names. */
#define CANCESTRY_FSM_NAME_MAX ((size_t)64u)

/** Maximum length of an FSM or state description accepted by the loader. */
#define CANCESTRY_FSM_DESCRIPTION_MAX ((size_t)512u)

/** Maximum length of a guard or value expression accepted by the loader. */
#define CANCESTRY_FSM_EXPRESSION_MAX ((size_t)256u)

/** Maximum length of a log message accepted by the loader. */
#define CANCESTRY_FSM_LOG_MESSAGE_MAX ((size_t)256u)

/** Maximum length of a raise_fault code accepted by the loader. */
#define CANCESTRY_FSM_FAULT_CODE_MAX ((size_t)64u)

/**
 * Maximum transition chain depth (SW-FR-FSM-016).
 *
 * One chain counts the event-driven transition plus every deferred transition
 * it schedules. A fifth transition in one chain is refused: see
 * core/fsm/README.md.
 */
#define CANCESTRY_FSM_TRANSITION_CHAIN_MAX_DEPTH ((uint16_t)4u)

/** Default per-instance incoming queue depth (SW-FR-FSM-019, fsm-spec.md section 11). */
#define CANCESTRY_FSM_INCOMING_QUEUE_DEFAULT_CAPACITY ((uint16_t)64u)

/**
 * Default execution budgets (SW-FR-FSM-045, SW-FR-FSM-021).
 *
 * The budgets bound the work one external event can cause: transitions are
 * already bounded by the chain depth, so these two limits bound the number of
 * internally generated (state_entered / state_exited) events one activation
 * may consume and the number of actions one event may execute.
 */
#define CANCESTRY_FSM_MAX_EVENTS_PER_ACTIVATION ((uint16_t)64u)
#define CANCESTRY_FSM_MAX_ACTIONS_PER_EVENT ((uint16_t)128u)

/* ------------------------------------------------------------------------- */
/* Status                                                                    */
/* ------------------------------------------------------------------------- */

/**
 * Operation results. Values >= 0 mean the operation succeeded; negative values
 * are failures. A denied action is a *recorded* failure, not an API error: the
 * engine counts it and continues safely (SW-FR-FSM-024).
 */
typedef enum cancestry_fsm_status {
    /** Success. */
    CANCESTRY_FSM_OK = 0,
    /** A required pointer argument was NULL or the engine is not initialized. */
    CANCESTRY_FSM_ERR_NULL = -1,
    /** An argument is malformed (invalid event, wrong value kind, ...). */
    CANCESTRY_FSM_ERR_ARGUMENT = -2,
    /** No state, instance, message, signal, interface, timer or variable matches. */
    CANCESTRY_FSM_ERR_NOT_FOUND = -3,
    /** A short name resolves to definitions from more than one codec map. */
    CANCESTRY_FSM_ERR_AMBIGUOUS = -4,
    /** The safety governor denied the action (SW-FR-FSM-042, SW-FR-GOV-006). */
    CANCESTRY_FSM_ERR_DENIED = -5,
    /** A guard or value expression failed to evaluate (SW-FR-FSM-038). */
    CANCESTRY_FSM_ERR_EXPRESSION = -6,
    /** A send_message action could not encode its frame. */
    CANCESTRY_FSM_ERR_ENCODING = -7,
    /** Caller-provided bounded storage is exhausted. */
    CANCESTRY_FSM_ERR_CAPACITY = -8,
    /** Loader: YAML syntax error or schema violation (SW-FR-FSM-003). */
    CANCESTRY_FSM_ERR_PARSE = -9,
    /** Loader: conflicting definitions (duplicate state or instance names). */
    CANCESTRY_FSM_ERR_CONFLICT = -10,
    /** Loader: out of memory. */
    CANCESTRY_FSM_ERR_NO_MEMORY = -11,
    /** The action needs a runtime hook (sink callback) that is not configured. */
    CANCESTRY_FSM_ERR_UNSUPPORTED = -12,
    /** The instance lifecycle does not allow the requested operation. */
    CANCESTRY_FSM_ERR_STATE = -13,
    /** The transition chain limit was exceeded (SW-FR-FSM-016). */
    CANCESTRY_FSM_ERR_CHAIN_LIMIT = -14,
    /** The per-event execution budget was exhausted (SW-FR-FSM-045). */
    CANCESTRY_FSM_ERR_BUDGET = -15,
    /** The queue is full and the event was dropped by policy (SW-FR-FSM-020). */
    CANCESTRY_FSM_ERR_QUEUE_FULL = -16
} cancestry_fsm_status_t;

/* ------------------------------------------------------------------------- */
/* Instance lifecycle (fsm-spec.md section 3)                                */
/* ------------------------------------------------------------------------- */

/**
 * FSM instance lifecycle states.
 *
 * The five states are normative (fsm-spec.md section 3); the legal edges are
 * documented in core/fsm/README.md and enforced by the instance manager. Only
 * ::CANCESTRY_FSM_INSTANCE_LIFECYCLE_RUNNING instances process events and run
 * timers, which is what makes a suspended or faulted instance inert instead of
 * subtly still acting (fail-closed).
 */
typedef enum cancestry_fsm_instance_lifecycle {
    /** Not enabled by the declaration, or explicitly disabled. */
    CANCESTRY_FSM_INSTANCE_LIFECYCLE_DISABLED = 0,
    /** Loaded and validated; variables initialised, no state entered yet. */
    CANCESTRY_FSM_INSTANCE_LIFECYCLE_READY = 1,
    /** Executing: the initial state has been entered, events are processed. */
    CANCESTRY_FSM_INSTANCE_LIFECYCLE_RUNNING = 2,
    /** Halted by a runtime fault policy; state and variables are retained. */
    CANCESTRY_FSM_INSTANCE_LIFECYCLE_SUSPENDED = 3,
    /** Halted by a critical fault; only cancestry_fsm_instance_reset() recovers. */
    CANCESTRY_FSM_INSTANCE_LIFECYCLE_FAULT = 4,
    CANCESTRY_FSM_INSTANCE_LIFECYCLE_COUNT
} cancestry_fsm_instance_lifecycle_t;

/* ------------------------------------------------------------------------- */
/* Actions and transitions                                                   */
/* ------------------------------------------------------------------------- */

/** Action kinds; mirrors the fsm-0.2.0 schema "fsm_action" oneOf keys. */
typedef enum cancestry_fsm_action_kind {
    CANCESTRY_FSM_ACTION_SEND_MESSAGE = 0,
    CANCESTRY_FSM_ACTION_SET_SIGNAL = 1,
    CANCESTRY_FSM_ACTION_SET_VARIABLE = 2,
    CANCESTRY_FSM_ACTION_START_TIMER = 3,
    CANCESTRY_FSM_ACTION_STOP_TIMER = 4,
    CANCESTRY_FSM_ACTION_RESET_TIMER = 5,
    CANCESTRY_FSM_ACTION_LOG = 6,
    CANCESTRY_FSM_ACTION_RAISE_FAULT = 7,
    CANCESTRY_FSM_ACTION_TRANSITION = 8,
    CANCESTRY_FSM_ACTION_COUNT
} cancestry_fsm_action_kind_t;

/** Log levels; mirrors the schema "log.level" enum. */
typedef enum cancestry_fsm_log_level {
    CANCESTRY_FSM_LOG_INFO = 0,
    CANCESTRY_FSM_LOG_WARNING = 1,
    CANCESTRY_FSM_LOG_ERROR = 2
} cancestry_fsm_log_level_t;

/**
 * An action value operand.
 *
 * Schema "expression_or_literal": a YAML number or boolean is a literal, a YAML
 * string is an expression evaluated at run time against the namespaces of
 * docs/system/expression-language.md (sig., var., evt.).
 */
typedef struct cancestry_fsm_operand {
    /** true when @c expression is valid; false when @c literal is valid. */
    bool is_expression;
    /** Literal value; meaningful only when @c is_expression is false. */
    cancestry_value_t literal;
    /** NUL-terminated expression text; meaningful only when @c is_expression is true. */
    const char *expression;
} cancestry_fsm_operand_t;

/** One named signal value of a send_message action, in declaration order. */
typedef struct cancestry_fsm_signal_value {
    /** Short signal name, unique within the referenced message's codec map. */
    const char *name;
    /** Value operand for this signal. */
    cancestry_fsm_operand_t operand;
} cancestry_fsm_signal_value_t;

/**
 * One FSM action.
 *
 * The @c as member valid for @c kind follows the schema:
 *   SEND_MESSAGE  -> as.send_message
 *   SET_SIGNAL    -> as.set_signal
 *   SET_VARIABLE  -> as.set_variable
 *   START_TIMER   -> as.start_timer
 *   STOP_TIMER    -> as.timer
 *   RESET_TIMER   -> as.timer
 *   LOG           -> as.log
 *   RAISE_FAULT   -> as.raise_fault
 *   TRANSITION    -> as.transition
 */
typedef struct cancestry_fsm_action {
    cancestry_fsm_action_kind_t kind;
    /** 1-based line of the action in the source file, for trace correlation. */
    uint16_t source_line;
    union {
        struct {
            /** Destination interface: physical name or package-level alias. */
            const char *interface;
            /** Message name (short or "<codec_map>.<message>" canonical form). */
            const char *message;
            /** Signal values, in declaration order; at least one. */
            const cancestry_fsm_signal_value_t *values;
            uint16_t value_count;
        } send_message;
        struct {
            /** Signal name. */
            const char *signal;
            cancestry_fsm_operand_t operand;
        } set_signal;
        struct {
            /** Instance-scoped variable name. */
            const char *variable;
            cancestry_fsm_operand_t operand;
        } set_variable;
        struct {
            /** Timer name. */
            const char *timer;
            /** Duration in milliseconds; 0 when not declared. */
            uint32_t duration_ms;
            bool has_duration_ms;
            /** Repeat flag; only meaningful when @c has_repeat is true. */
            bool repeat;
            bool has_repeat;
        } start_timer;
        struct {
            /** Timer name for stop_timer / reset_timer. */
            const char *timer;
        } timer;
        struct {
            cancestry_fsm_log_level_t level;
            const char *message;
        } log;
        struct {
            /** Fault code text, e.g. "CLUSTER_TIMEOUT". */
            const char *code;
            cancestry_fault_severity_t severity;
        } raise_fault;
        struct {
            /** Target state name; never NULL (schema required). */
            const char *target;
            /** Compiled: index of @c target in the owning machine's states. */
            uint16_t target_index;
        } transition;
    } as;
} cancestry_fsm_action_t;

/**
 * One transition.
 *
 * The event selector mirrors the schema enum (fsm-spec.md section 5) and is a
 * ::cancestry_event_type_t, so a transition subscribes to exactly one of the
 * seven normative event types; optional filter fields narrow the match.
 */
typedef struct cancestry_fsm_transition {
    cancestry_event_type_t event;
    /** Interface filter (physical name or package alias), or NULL for any. */
    const char *interface;
    /** Message-name filter (can_rx), or NULL for any. */
    const char *message;
    /** Signal-name filter (signal_changed; required by the schema). */
    const char *signal;
    /** Timer-name filter (timer_expired; required by the schema). */
    const char *timer;
    /** Guard expression text, or NULL when the transition is unconditional. */
    const char *guard;
    /** Actions, in declaration order; may be NULL when @c action_count is 0. */
    const cancestry_fsm_action_t *actions;
    uint16_t action_count;
    /** Target state name; never NULL (schema required). */
    const char *target;
    /** Compiled: index of @c target in the owning machine's states. */
    uint16_t target_index;
} cancestry_fsm_transition_t;

/** One state (flat: no substates in v0.3, SW-FR-FSM-009). */
typedef struct cancestry_fsm_state {
    const char *name;
    /** Entry actions, in declaration order; may be NULL/0. */
    const cancestry_fsm_action_t *entry;
    uint16_t entry_count;
    /** Exit actions, in declaration order; may be NULL/0. */
    const cancestry_fsm_action_t *exit;
    uint16_t exit_count;
    /** Transitions, in declaration order (selection order; SW-FR-FSM-012). */
    const cancestry_fsm_transition_t *transitions;
    uint16_t transition_count;
} cancestry_fsm_state_t;

/** A variable declaration (fsm-spec.md section 10, SW-FR-FSM-031..033). */
typedef struct cancestry_fsm_variable_def {
    /** Variable name; unique within the machine. */
    const char *name;
    /** Declared type: BOOL, INT or REAL (the schema's boolean/integer/float). */
    cancestry_value_kind_t type;
    /** Default initializer; only meaningful when @c has_default is true. */
    cancestry_fsm_operand_t default_value;
    bool has_default;
} cancestry_fsm_variable_def_t;

/** A timer declaration (fsm-spec.md section 9, SW-FR-FSM-026..030). */
typedef struct cancestry_fsm_timer_def {
    /** Timer name; unique within the machine. */
    const char *name;
    /** Period in milliseconds, at least 1 (schema minimum). */
    uint32_t duration_ms;
    /** true for a periodic timer, false for one-shot. */
    bool repeat;
    /** true when the timer starts when the instance starts. */
    bool auto_start;
} cancestry_fsm_timer_def_t;

/**
 * A state machine definition (template; SwAD.md section 5).
 *
 * An instance is a runnable object; the definition itself is immutable and
 * shared by every instance of the machine.
 */
typedef struct cancestry_fsm_machine {
    const char *name;
    const char *description;
    /** Initial state name; never NULL (SW-FR-FSM-005). */
    const char *initial;
    /** Compiled: index of @c initial in @c states. */
    uint16_t initial_index;
    /** Variables, in declaration order; may be NULL/0. */
    const cancestry_fsm_variable_def_t *variables;
    uint16_t variable_count;
    /** Timers, in declaration order; may be NULL/0. */
    const cancestry_fsm_timer_def_t *timers;
    uint16_t timer_count;
    /** States, in declaration order; at least one, names unique (SW-FR-FSM-006). */
    const cancestry_fsm_state_t *states;
    uint16_t state_count;
} cancestry_fsm_machine_t;

/** One can_rx subscription of an instance (schema "can_rx_subscription"). */
typedef struct cancestry_fsm_can_rx_subscription {
    /** Interface name or alias; never NULL (schema required). */
    const char *interface;
    /** Exact CAN id filter; only meaningful when @c has_can_id is true. */
    uint32_t can_id;
    bool has_can_id;
    /** Message-name filter; NULL when not declared. */
    const char *message;
} cancestry_fsm_can_rx_subscription_t;

/** Instance-level interface binding: alias to physical name (package-spec.md section 5). */
typedef struct cancestry_fsm_binding {
    const char *alias;
    const char *physical;
} cancestry_fsm_binding_t;

/** Per-instance variable override (schema "instance.variables"). */
typedef struct cancestry_fsm_variable_override {
    const char *name;
    cancestry_fsm_operand_t operand;
} cancestry_fsm_variable_override_t;

/**
 * Instance subscriptions (fsm-spec.md sections 5, 11).
 *
 * A subscription list is a delivery gate applied before transition matching.
 * The @c has_* flags distinguish "not declared" (fall back to the event
 * classes the machine's transitions select) from "declared empty" (that class
 * is never delivered). See core/fsm/README.md.
 */
typedef struct cancestry_fsm_subscriptions {
    const cancestry_fsm_can_rx_subscription_t *can_rx;
    uint16_t can_rx_count;
    bool has_can_rx;
    /** signal_changed allowlist of signal names. */
    const char *const *signals;
    uint16_t signal_count;
    bool has_signals;
    /** Timer events of this instance (own timers only). */
    bool timers;
    bool has_timers;
    /** fault_raised events. */
    bool faults;
    bool has_faults;
    /** power_mode_changed events. */
    bool power_mode;
    bool has_power_mode;
} cancestry_fsm_subscriptions_t;

/** A runnable instance declaration (fsm-spec.md section 2). */
typedef struct cancestry_fsm_instance_def {
    /** Instance id; unique within the file. */
    const char *id;
    /** Machine name; never NULL. */
    const char *machine;
    /** Compiled: index of @c machine in the set's machines. */
    uint16_t machine_index;
    /** When false the instance loads into DISABLED and never runs. */
    bool enabled;
    /** Instance-level interface bindings, in declaration order; may be NULL/0. */
    const cancestry_fsm_binding_t *bindings;
    uint16_t binding_count;
    /** Variable initializers, in declaration order; may be NULL/0. */
    const cancestry_fsm_variable_override_t *variables;
    uint16_t variable_count;
    /** Subscriptions; every field's @c has_* flag records what was declared. */
    cancestry_fsm_subscriptions_t subscriptions;
} cancestry_fsm_instance_def_t;

/**
 * A loaded FSM file: machines plus instance declarations.
 *
 * Produced by cancestry_fsm_set_load() (one heap block that owns the set and
 * everything it points to) and released by cancestry_fsm_set_free().
 */
typedef struct cancestry_fsm_set {
    /** State machines, in declaration order; at least one. */
    const cancestry_fsm_machine_t *machines;
    uint16_t machine_count;
    /** Instances, in declaration order; the dispatch order of the runtime. */
    const cancestry_fsm_instance_def_t *instances;
    uint16_t instance_count;
} cancestry_fsm_set_t;

/* ------------------------------------------------------------------------- */
/* Helpers                                                                   */
/* ------------------------------------------------------------------------- */

/** @return true when @p status indicates success (zero or positive). */
bool cancestry_fsm_status_is_ok(cancestry_fsm_status_t status);

/** @return Stable, statically allocated name for @p status. Never NULL. */
const char *cancestry_fsm_status_name(cancestry_fsm_status_t status);

/** @return Stable name such as "running"; "invalid" for out-of-range input. */
const char *cancestry_fsm_lifecycle_name(cancestry_fsm_instance_lifecycle_t lifecycle);

/** @return Stable name such as "send_message". Never NULL. */
const char *cancestry_fsm_action_kind_name(cancestry_fsm_action_kind_t kind);

/** @return Stable name such as "warning". Never NULL. */
const char *cancestry_fsm_log_level_name(cancestry_fsm_log_level_t level);

/** @return Stable schema name such as "timer_expired". Never NULL. */
const char *cancestry_fsm_event_type_name(cancestry_event_type_t type);

/**
 * @return The declared variable type for a schema type name
 *         ("boolean"/"integer"/"float"), or
 *         ::CANCESTRY_VALUE_KIND_UNSET when the name is unknown.
 */
cancestry_value_kind_t cancestry_fsm_variable_type_from_name(const char *name);

/**
 * @return true when @p lifecycle allows event processing and timer execution
 *         (i.e. it is ::CANCESTRY_FSM_INSTANCE_LIFECYCLE_RUNNING).
 */
bool cancestry_fsm_lifecycle_is_active(cancestry_fsm_instance_lifecycle_t lifecycle);

#ifdef __cplusplus
}
#endif

#endif /* CANCESTRY_FSM_TYPES_H */
