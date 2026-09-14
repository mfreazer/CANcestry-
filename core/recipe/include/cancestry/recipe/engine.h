/*
 * CANcestry - recipe execution engine.
 *
 * Normative references:
 *   docs/packages/recipe-spec.md       (v0.2.1) sections 4-6
 *   docs/system/event-ordering.md      sections 7-8 (dispatch order, generated events)
 *   docs/system/governor.md            (v0.2.1 stub level) approval flow
 *   docs/system/expression-language.md (v0.2.1) namespaces and grammar
 *   docs/software/SwRS.md              SW-FR-RECIPE-001 .. SW-FR-RECIPE-007,
 *                                      SW-FR-GOV-005, SW-FR-GOV-006
 *   docs/system/SyRS.md                SYS-NF-001 (determinism), SYS-NF-002 (bounded resources)
 *
 * Design notes:
 *   - The engine is a caller-owned struct over borrowed configuration
 *     (recipe sets, codec signal namespace, interface table, bindings, event
 *     queue, sink callbacks, governor). It never allocates and has no global
 *     state (SYS-NF-002); CI symbol-scans the cancestry_recipe archive.
 *   - Dispatch order is normative: recipes run in engine set order (package
 *     load order, then recipe file order), and within a set in recipe
 *     definition order (event-ordering.md section 7). Actions execute
 *     sequentially; the default on_error policy is stop (recipe-spec.md
 *     section 5).
 *   - Side effects leave the engine only through:
 *       * the safety governor stub (approval requests, fail-closed),
 *       * the sink callbacks (TX requests, log/fault/timer notifications),
 *       * the shared signal value store (set_signal),
 *       * the event queue (generated signal_changed / fault_raised events).
 *     There is no direct hardware access.
 *   - Governor policy (docs/system/governor.md, stub level): when no
 *     governor is configured, every side-effect request is denied
 *     (fail-closed, SW-FR-GOV-006) and the violation counter increments.
 */

#ifndef CANCESTRY_RECIPE_ENGINE_H
#define CANCESTRY_RECIPE_ENGINE_H

#include "cancestry/codec/namespace.h"
#include "cancestry/event/queue.h"
#include "cancestry/recipe/types.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------------- */
/* Shared signal value store                                                 */
/* ------------------------------------------------------------------------- */

/** One slot of the shared signal value store. */
typedef struct cancestry_recipe_signal_slot {
    /** Interned signal id (codec namespace registration id). */
    cancestry_signal_id_t signal_id;
    /** Current value of the signal. */
    cancestry_value_t value;
} cancestry_recipe_signal_slot_t;

/**
 * Bounded, caller-owned store of current signal values ("the shared signal
 * namespace" at value level). The decode pipeline writes decoded values into
 * it; the recipe engine reads it (conditions, expressions) and writes
 * set_signal results into it after governor approval. Never allocates.
 */
typedef struct cancestry_recipe_signal_store {
    /** Caller-owned array of @c capacity slots. */
    cancestry_recipe_signal_slot_t *slots;
    /** Number of slots in @p slots. */
    size_t capacity;
    /** Number of slots in use, in first-write order. */
    size_t count;
} cancestry_recipe_signal_store_t;

/**
 * Initialize a signal store over caller-owned storage.
 *
 * @return true when the store is ready for use (non-NULL arguments and a
 *         capacity of at least 1).
 */
bool cancestry_recipe_signal_store_init(cancestry_recipe_signal_store_t *store,
                                        cancestry_recipe_signal_slot_t *slots,
                                        size_t capacity);

/** @return true when @p store is non-NULL and initialized. */
bool cancestry_recipe_signal_store_is_valid(const cancestry_recipe_signal_store_t *store);

/** @return Number of slots in use, or 0 when @p store is invalid. */
size_t cancestry_recipe_signal_store_size(const cancestry_recipe_signal_store_t *store);

/**
 * Read the current value of a signal.
 *
 * @return CANCESTRY_RECIPE_OK, CANCESTRY_RECIPE_ERR_NULL, or
 *         CANCESTRY_RECIPE_ERR_NOT_FOUND when the signal has no value yet.
 */
cancestry_recipe_status_t cancestry_recipe_signal_store_get(
    const cancestry_recipe_signal_store_t *store,
    cancestry_signal_id_t signal_id,
    cancestry_value_t *value_out);

/**
 * Write a signal value, creating a slot on first write.
 *
 * @return CANCESTRY_RECIPE_OK, CANCESTRY_RECIPE_ERR_NULL, or
 *         CANCESTRY_RECIPE_ERR_CAPACITY when the store is full.
 */
cancestry_recipe_status_t cancestry_recipe_signal_store_set(cancestry_recipe_signal_store_t *store,
                                                            cancestry_signal_id_t signal_id,
                                                            const cancestry_value_t *value);

/* ------------------------------------------------------------------------- */
/* Environment tables                                                        */
/* ------------------------------------------------------------------------- */

/** Physical interface registry entry: name to interned interface id. */
typedef struct cancestry_recipe_interface {
    const char *name;
    cancestry_interface_id_t id;
} cancestry_recipe_interface_t;

/** Package-level interface binding: alias to physical name (package-spec.md section 5). */
typedef struct cancestry_recipe_binding {
    const char *alias;
    const char *physical;
} cancestry_recipe_binding_t;

/** Timer registry entry: timer name to interned timer id. */
typedef struct cancestry_recipe_timer {
    const char *name;
    cancestry_timer_id_t id;
} cancestry_recipe_timer_t;

/** One recipe-local variable slot. Storage and lifetime are engine-owned. */
typedef struct cancestry_recipe_variable {
    /** Owning recipe; variables are scoped per recipe. */
    const cancestry_recipe_t *recipe;
    /** Variable name. */
    const char *name;
    cancestry_value_t value;
} cancestry_recipe_variable_t;

/* ------------------------------------------------------------------------- */
/* Governor stub                                                             */
/* ------------------------------------------------------------------------- */

/** Governor decision (docs/system/governor.md, stub level). */
typedef enum cancestry_recipe_governor_decision {
    CANCESTRY_RECIPE_GOVERNOR_APPROVE = 0,
    CANCESTRY_RECIPE_GOVERNOR_DENY = 1
} cancestry_recipe_governor_decision_t;

/** Kind of side effect requesting approval. */
typedef enum cancestry_recipe_governor_request_kind {
    CANCESTRY_RECIPE_GOVERNOR_SEND_MESSAGE = 0,
    CANCESTRY_RECIPE_GOVERNOR_SET_SIGNAL = 1
} cancestry_recipe_governor_request_kind_t;

/**
 * One approval request. Fields are meaningful per @c kind; unused fields are
 * NULL or 0. send_message requests carry the fully encoded frame, so the
 * governor can enforce TX permissions by CAN id and inspect payload bytes.
 */
/**
 * A side-effect request passed to the governor.
 *
 * All pointers (recipe, cause, names, frame, value) point into storage that
 * is only valid for the duration of the governor callback; a governor that
 * needs the data afterwards must copy it.
 */
typedef struct cancestry_recipe_governor_request {
    cancestry_recipe_governor_request_kind_t kind;
    /** Recipe requesting the side effect. */
    const cancestry_recipe_t *recipe;
    /** Event that triggered the recipe. */
    const cancestry_event_t *cause;
    /* send_message */
    /** Resolved physical interface name. */
    const char *interface_name;
    cancestry_interface_id_t interface_id;
    /** Message name as written in the recipe. */
    const char *message_name;
    uint32_t can_id;
    /** Encoded frame bytes, @c frame_length long. */
    const uint8_t *frame;
    uint8_t frame_length;
    /* set_signal */
    /** Signal name as written in the recipe. */
    const char *signal_name;
    cancestry_signal_id_t signal_id;
    /** New signal value. */
    const cancestry_value_t *value;
} cancestry_recipe_governor_request_t;

/**
 * Governor stub: a function pointer the platform (or the test suite)
 * provides. Returning DENY blocks the side effect and increments the
 * engine's violation counter (governor_denials).
 */
typedef cancestry_recipe_governor_decision_t (*cancestry_recipe_governor_fn)(
    void *user_data, const cancestry_recipe_governor_request_t *request);

/* ------------------------------------------------------------------------- */
/* Sink                                                                      */
/* ------------------------------------------------------------------------- */

/**
 * One recipe invocation, passed to sink callbacks for context.
 */
typedef struct cancestry_recipe_invocation {
    /** Engine executing the recipe. */
    const struct cancestry_recipe_engine *engine;
    /** Recipe being executed. */
    const cancestry_recipe_t *recipe;
    /** 1-based ordinal of the recipe in engine dispatch order. */
    uint32_t recipe_ordinal;
    /** Event that triggered the recipe. */
    const cancestry_event_t *event;
    /** Action being executed, or NULL. */
    const cancestry_recipe_action_t *action;
} cancestry_recipe_invocation_t;

/**
 * Runtime sink: the engine's only path to the outside world. Every callback
 * is optional (NULL); an action whose only effect is a missing callback fails
 * with CANCESTRY_RECIPE_ERR_UNSUPPORTED (fail-closed). See core/recipe/README.md
 * for which actions require which callback.
 */
typedef struct cancestry_recipe_sink {
    /** Opaque context passed to every callback. */
    void *user_data;
    /** TX request after governor approval (send_message). */
    void (*on_send_message)(void *user_data,
                            const cancestry_recipe_invocation_t *invocation,
                            cancestry_interface_id_t interface_id,
                            const char *interface_name,
                            uint32_t can_id,
                            const uint8_t *frame,
                            uint8_t frame_length);
    /** Notification after set_signal was applied to the signal store. */
    void (*on_signal_set)(void *user_data,
                          const cancestry_recipe_invocation_t *invocation,
                          cancestry_signal_id_t signal_id,
                          const char *signal_name,
                          const cancestry_value_t *old_value,
                          const cancestry_value_t *new_value);
    /** Log action emission. */
    void (*on_log)(void *user_data,
                   const cancestry_recipe_invocation_t *invocation,
                   cancestry_recipe_log_level_t level,
                   const char *message);
    /** Fault action emission; @c numeric_code is the stable hash of @c code. */
    void (*on_fault)(void *user_data,
                     const cancestry_recipe_invocation_t *invocation,
                     const char *code,
                     cancestry_fault_severity_t severity,
                     cancestry_fault_code_t numeric_code);
    /** Timer requests (start_timer / stop_timer / reset_timer). */
    void (*on_timer)(void *user_data,
                     const cancestry_recipe_invocation_t *invocation,
                     cancestry_recipe_action_kind_t kind,
                     const char *timer_name,
                     uint32_t duration_ms,
                     bool repeat);
} cancestry_recipe_sink_t;

/* ------------------------------------------------------------------------- */
/* Expression evaluator hook                                                 */
/* ------------------------------------------------------------------------- */

/**
 * Resolution inputs for expression evaluation. The engine fills this per
 * invocation; a custom evaluator resolves sig.NAME, var.NAME and evt.FIELD
 * references against it.
 */
typedef struct cancestry_recipe_expression_context {
    const cancestry_recipe_t *recipe;
    const cancestry_event_t *event;
    const cancestry_codec_namespace_t *namespace;
    const cancestry_recipe_signal_store_t *signals;
    const cancestry_recipe_variable_t *variables;
    size_t variable_count;
} cancestry_recipe_expression_context_t;

/**
 * Expression evaluator hook. When the engine configuration leaves @c evaluator
 * NULL, the built-in minimal evaluator runs (docs/system/expression-language.md
 * subset: literals, sig./var./evt. references, unary minus, * / % + -,
 * comparisons, not/and/or, parentheses; no function calls yet). The full
 * v0.3.0 evaluator plugs in here later.
 *
 * @return true and @c *result_out when the expression evaluated; false on any
 *         expression fault (undefined identifier, division by zero, invalid
 *         coercion, overflow, ...).
 */
typedef bool (*cancestry_recipe_expression_fn)(void *user_data,
                                               const char *expression,
                                               const cancestry_recipe_expression_context_t *context,
                                               cancestry_value_t *result_out);

/* ------------------------------------------------------------------------- */
/* Engine                                                                    */
/* ------------------------------------------------------------------------- */

/** Engine counters. Monotonic; never reset by the engine. */
typedef struct cancestry_recipe_engine_counters {
    /** Events accepted by cancestry_recipe_engine_process_event(). */
    uint32_t events_processed;
    /** Recipes whose trigger matched (including condition-skipped ones). */
    uint32_t recipes_invoked;
    /** Recipes skipped because a condition evaluated false. */
    uint32_t recipes_skipped_conditions;
    /** Trigger filters that could not resolve a name (fail-closed: no match). */
    uint32_t trigger_unresolved;
    /** Individual condition expressions evaluated. */
    uint32_t conditions_evaluated;
    /** Condition expressions that failed evaluation or were not boolean. */
    uint32_t condition_errors;
    /** Actions that completed successfully. */
    uint32_t actions_executed;
    /** Actions that failed (any reason, including governor denial). */
    uint32_t action_errors;
    /** Governor denials; the violation counter (SW-FR-GOV-005). */
    uint32_t governor_denials;
    /** Recipes truncated by the on_error: stop policy. */
    uint32_t recipes_halted;
    /** send_message actions approved and emitted. */
    uint32_t messages_sent;
    /** set_signal actions approved and applied. */
    uint32_t signals_set;
    /** set_variable actions applied. */
    uint32_t variables_set;
    /** Timer requests emitted to the sink. */
    uint32_t timers_requested;
    /** Log actions emitted. */
    uint32_t logs_emitted;
    /** Fault actions emitted. */
    uint32_t faults_raised;
    /** Codec warnings accumulated while encoding send_message frames. */
    cancestry_codec_warnings_t codec_warnings;
} cancestry_recipe_engine_counters_t;

/** Recipe engine over borrowed configuration. Never allocates. */
typedef struct cancestry_recipe_engine {
    /** Recipe sets in dispatch order (package load order, then file order). */
    const cancestry_recipe_set_t *sets;
    size_t set_count;
    /** Codec signal namespace for name resolution; may be NULL. */
    const cancestry_codec_namespace_t *namespace;
    /** Physical interface registry; may be NULL. */
    const cancestry_recipe_interface_t *interfaces;
    size_t interface_count;
    /** Package-level alias bindings; may be NULL. */
    const cancestry_recipe_binding_t *bindings;
    size_t binding_count;
    /** Timer registry; may be NULL. */
    const cancestry_recipe_timer_t *timers;
    size_t timer_count;
    /** Shared signal value store; may be NULL. */
    cancestry_recipe_signal_store_t *signals;
    /** Event queue for generated events; may be NULL. */
    cancestry_event_queue_t *queue;
    /** Runtime sink; may be NULL. */
    const cancestry_recipe_sink_t *sink;
    /** Governor stub; NULL means fail-closed (deny everything). */
    cancestry_recipe_governor_fn governor;
    void *governor_user_data;
    /** Expression evaluator hook; NULL means the built-in minimal evaluator. */
    cancestry_recipe_expression_fn evaluator;
    void *evaluator_user_data;
    /** Recipe-local variable storage (engine-owned contents). */
    cancestry_recipe_variable_t *variables;
    size_t variable_capacity;
    size_t variable_count;
    /** Monotonic counters. */
    cancestry_recipe_engine_counters_t counters;
} cancestry_recipe_engine_t;

/** Borrowed engine configuration, copied into the engine at init time. */
typedef struct cancestry_recipe_engine_config {
    const cancestry_recipe_set_t *sets;
    size_t set_count;
    const cancestry_codec_namespace_t *signal_namespace;
    const cancestry_recipe_interface_t *interfaces;
    size_t interface_count;
    const cancestry_recipe_binding_t *bindings;
    size_t binding_count;
    const cancestry_recipe_timer_t *timers;
    size_t timer_count;
    cancestry_recipe_signal_store_t *signal_store;
    cancestry_event_queue_t *event_queue;
    const cancestry_recipe_sink_t *sink;
    cancestry_recipe_governor_fn governor;
    void *governor_user_data;
    cancestry_recipe_expression_fn evaluator;
    void *evaluator_user_data;
    cancestry_recipe_variable_t *variable_storage;
    size_t variable_capacity;
} cancestry_recipe_engine_config_t;

/**
 * Initialize an engine over borrowed configuration.
 *
 * All arrays and objects referenced by @p config must outlive the engine.
 * @p sets may be NULL only when @p set_count is 0; @p variable_storage may be
 * NULL only when @p variable_capacity is 0.
 *
 * @return true when the engine is ready for use. On failure the engine is
 *         left zeroed (safely unusable).
 */
bool cancestry_recipe_engine_init(cancestry_recipe_engine_t *engine,
                                  const cancestry_recipe_engine_config_t *config);

/** @return true when @p engine is non-NULL and initialized. */
bool cancestry_recipe_engine_is_valid(const cancestry_recipe_engine_t *engine);

/** @return Total number of recipes across all sets, or 0 when invalid. */
size_t cancestry_recipe_engine_recipe_count(const cancestry_recipe_engine_t *engine);

/** @return Pointer to the live counters, or NULL when @p engine is invalid. */
const cancestry_recipe_engine_counters_t *cancestry_recipe_engine_counters(
    const cancestry_recipe_engine_t *engine);

/**
 * Process one event through every recipe in dispatch order (SYS-NF-001:
 * same event sequence, same action sequence).
 *
 * For each enabled recipe whose trigger matches, the engine evaluates the
 * conditions (all must hold) and then executes the actions sequentially. A
 * failing action either halts the recipe (on_error: stop, the default) or is
 * skipped (on_error: continue); other recipes are unaffected.
 *
 * Recipe-level failures are recorded in the engine counters; this function
 * returns a negative status only for invalid arguments.
 *
 * @return CANCESTRY_RECIPE_OK, CANCESTRY_RECIPE_ERR_NULL for a NULL engine
 *         or event, or CANCESTRY_RECIPE_ERR_ARGUMENT for an invalid event.
 */
cancestry_recipe_status_t cancestry_recipe_engine_process_event(cancestry_recipe_engine_t *engine,
                                                                const cancestry_event_t *event);

/**
 * Read a recipe-local variable.
 *
 * @return CANCESTRY_RECIPE_OK, CANCESTRY_RECIPE_ERR_NULL, or
 *         CANCESTRY_RECIPE_ERR_NOT_FOUND when the recipe has no such
 *         variable yet.
 */
cancestry_recipe_status_t cancestry_recipe_engine_get_variable(
    const cancestry_recipe_engine_t *engine,
    const cancestry_recipe_t *recipe,
    const char *name,
    cancestry_value_t *value_out);

/**
 * @return Stable 32-bit FNV-1a hash of a fault code string, never 0.
 *
 * raise_fault events carry a numeric fault code; this hash maps the recipe's
 * string code into it deterministically. The string itself is reported
 * through the sink's on_fault callback (see core/recipe/README.md).
 */
cancestry_fault_code_t cancestry_recipe_fault_code_hash(const char *code);

#ifdef __cplusplus
}
#endif

#endif /* CANCESTRY_RECIPE_ENGINE_H */
