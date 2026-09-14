/*
 * CANcestry - portable recipe engine: shared types.
 *
 * Normative references:
 *   docs/packages/recipe-spec.md       (v0.2.1) sections 2-6
 *   schemas/recipe-0.2.0.schema.json   recipe file v0.2.0 schema
 *   docs/software/SwRS.md              SW-FR-RECIPE-001 .. SW-FR-RECIPE-007
 *   docs/system/SyRS.md                SYS-NF-001 (determinism), SYS-NF-002 (bounded resources)
 *   docs/system/event-ordering.md      sections 7-8 (dispatch and generated events)
 *
 * Design notes:
 *   - A loaded recipe set is a single, self-contained allocation produced by
 *     the loader (core/recipe/src/loader.c). Every array and string pointer
 *     inside it refers into that allocation, so the runtime path (the engine)
 *     never allocates: it only reads (SYS-NF-002).
 *   - Strings are borrowed pointers with the lifetime of the recipe set.
 *   - The transition action is strictly forbidden in recipes
 *     (SW-FR-RECIPE-006); it has no action kind, and the loader rejects it
 *     with a dedicated error.
 *   - Trigger matching, condition evaluation and action execution are pure
 *     functions of (engine configuration, recipe set, event); the same event
 *     sequence produces the same action sequence (SYS-NF-001).
 */

#ifndef CANCESTRY_RECIPE_TYPES_H
#define CANCESTRY_RECIPE_TYPES_H

#include "cancestry/event/types.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Maximum length of recipe, interface, message, signal, timer, variable and fault-code names. */
#define CANCESTRY_RECIPE_NAME_MAX ((size_t)64u)

/** Maximum length of a recipe description accepted by the loader. */
#define CANCESTRY_RECIPE_DESCRIPTION_MAX ((size_t)512u)

/** Maximum length of a condition or value expression accepted by the loader. */
#define CANCESTRY_RECIPE_EXPRESSION_MAX ((size_t)256u)

/** Maximum length of a log message accepted by the loader. */
#define CANCESTRY_RECIPE_LOG_MESSAGE_MAX ((size_t)256u)

/**
 * Operation results. Values >= 0 mean the operation succeeded; negative
 * values are failures. Governor denials are reported as
 * CANCESTRY_RECIPE_ERR_DENIED and are additionally counted in the engine's
 * governor_denials violation counter.
 */
typedef enum cancestry_recipe_status {
    /** Success. */
    CANCESTRY_RECIPE_OK = 0,
    /** A required pointer argument was NULL or the engine is not initialized. */
    CANCESTRY_RECIPE_ERR_NULL = -1,
    /** An argument is malformed (invalid event, wrong value kind, ...). */
    CANCESTRY_RECIPE_ERR_ARGUMENT = -2,
    /** No recipe, message, signal, interface, timer or variable matches. */
    CANCESTRY_RECIPE_ERR_NOT_FOUND = -3,
    /** A short name resolves to definitions from more than one codec map. */
    CANCESTRY_RECIPE_ERR_AMBIGUOUS = -4,
    /** The safety governor denied the action (violation counter incremented). */
    CANCESTRY_RECIPE_ERR_DENIED = -5,
    /** A condition or value expression failed to evaluate (undefined name, division by zero, ...). */
    CANCESTRY_RECIPE_ERR_EXPRESSION = -6,
    /** A send_message action could not encode its frame. */
    CANCESTRY_RECIPE_ERR_ENCODING = -7,
    /** Bounded storage is exhausted. */
    CANCESTRY_RECIPE_ERR_CAPACITY = -8,
    /** Loader: YAML syntax error or schema violation. */
    CANCESTRY_RECIPE_ERR_PARSE = -9,
    /** Loader: conflicting definitions (duplicate recipe names). */
    CANCESTRY_RECIPE_ERR_CONFLICT = -10,
    /** Loader: out of memory. */
    CANCESTRY_RECIPE_ERR_NO_MEMORY = -11,
    /** The action needs a runtime hook (sink callback) that is not configured. */
    CANCESTRY_RECIPE_ERR_UNSUPPORTED = -12
} cancestry_recipe_status_t;

/** Trigger events; mirrors the recipe schema "trigger.event" enum (recipe-spec.md section 2). */
typedef enum cancestry_recipe_trigger_event {
    CANCESTRY_RECIPE_TRIGGER_CAN_RX = 0,
    CANCESTRY_RECIPE_TRIGGER_SIGNAL_CHANGED = 1,
    CANCESTRY_RECIPE_TRIGGER_TIMER_EXPIRED = 2,
    /** Loads per schema, but never matches: the event model has no timeout event (SW-FR-RECIPE-004 is future work). */
    CANCESTRY_RECIPE_TRIGGER_TIMEOUT = 3,
    CANCESTRY_RECIPE_TRIGGER_FAULT_RAISED = 4,
    CANCESTRY_RECIPE_TRIGGER_POWER_MODE_CHANGED = 5,
    CANCESTRY_RECIPE_TRIGGER_COUNT
} cancestry_recipe_trigger_event_t;

/** Action kinds; mirrors recipe-spec.md section 4. The transition action is deliberately absent (SW-FR-RECIPE-006). */
typedef enum cancestry_recipe_action_kind {
    CANCESTRY_RECIPE_ACTION_SEND_MESSAGE = 0,
    CANCESTRY_RECIPE_ACTION_SET_SIGNAL = 1,
    CANCESTRY_RECIPE_ACTION_SET_VARIABLE = 2,
    CANCESTRY_RECIPE_ACTION_START_TIMER = 3,
    CANCESTRY_RECIPE_ACTION_STOP_TIMER = 4,
    CANCESTRY_RECIPE_ACTION_RESET_TIMER = 5,
    CANCESTRY_RECIPE_ACTION_LOG = 6,
    CANCESTRY_RECIPE_ACTION_RAISE_FAULT = 7,
    CANCESTRY_RECIPE_ACTION_COUNT
} cancestry_recipe_action_kind_t;

/** Action-sequence failure policy (recipe-spec.md section 5). Default is stop. */
typedef enum cancestry_recipe_on_error {
    /** Skip the remaining actions of this recipe invocation (the default). */
    CANCESTRY_RECIPE_ON_ERROR_STOP = 0,
    /** Execute the remaining actions anyway. */
    CANCESTRY_RECIPE_ON_ERROR_CONTINUE = 1
} cancestry_recipe_on_error_t;

/** Log levels; mirrors the recipe schema "log.level" enum. */
typedef enum cancestry_recipe_log_level {
    CANCESTRY_RECIPE_LOG_INFO = 0,
    CANCESTRY_RECIPE_LOG_WARNING = 1,
    CANCESTRY_RECIPE_LOG_ERROR = 2
} cancestry_recipe_log_level_t;

/**
 * An action value operand.
 *
 * Schema "expression_or_literal": a YAML number or boolean becomes a literal
 * @c value; a YAML string is an expression evaluated at execution time
 * (docs/system/expression-language.md namespaces: sig.*, var.*, evt.*).
 */
typedef struct cancestry_recipe_operand {
    /** true when @c expression is valid; false when @c literal is valid. */
    bool is_expression;
    /** Literal value; meaningful only when @c is_expression is false. */
    cancestry_value_t literal;
    /** NUL-terminated expression text; meaningful only when @c is_expression is true. */
    const char *expression;
} cancestry_recipe_operand_t;

/**
 * Recipe trigger (recipe-spec.md section 2).
 *
 * Optional filter fields are NULL when not declared. A filter that cannot
 * apply to the trigger's event type (for example a message filter on a
 * fault_raised trigger) makes the trigger unmatchable; see core/recipe/README.md.
 */
typedef struct cancestry_recipe_trigger {
    cancestry_recipe_trigger_event_t event;
    /** Interface filter (physical name or package-level alias), or NULL for any. */
    const char *interface;
    /** Message-name filter (can_rx only), or NULL for any. */
    const char *message;
    /** Signal-name filter (signal_changed only; required by the schema), or NULL. */
    const char *signal;
    /** Timer-name filter (timer_expired only; required by the schema), or NULL. */
    const char *timer;
    /** Watch duration for timeout triggers; required by the schema when event is timeout. */
    uint32_t timeout_ms;
    /** true when timeout_ms was declared. */
    bool has_timeout_ms;
} cancestry_recipe_trigger_t;

/** One condition: an expression that must evaluate to true (SW-FR-RECIPE-002). */
typedef struct cancestry_recipe_condition {
    /** NUL-terminated expression text. */
    const char *expression;
} cancestry_recipe_condition_t;

/** One named signal value of a send_message action, in declaration order. */
typedef struct cancestry_recipe_signal_value {
    /** Short signal name, unique within the referenced message's codec map. */
    const char *name;
    /** Value operand for this signal. */
    cancestry_recipe_operand_t operand;
} cancestry_recipe_signal_value_t;

/**
 * One recipe action. The @c as member valid for @c kind follows the schema:
 *   SEND_MESSAGE  -> as.send_message
 *   SET_SIGNAL    -> as.set_signal
 *   SET_VARIABLE  -> as.set_variable
 *   START_TIMER   -> as.start_timer
 *   STOP_TIMER    -> as.timer
 *   RESET_TIMER   -> as.timer
 *   LOG           -> as.log
 *   RAISE_FAULT   -> as.raise_fault
 */
typedef struct cancestry_recipe_action {
    cancestry_recipe_action_kind_t kind;
    union {
        struct {
            /** Destination interface: physical name or package-level alias. */
            const char *interface;
            /** Message name (short or "<codec_map>.<message>" canonical form). */
            const char *message;
            /** Signal values, in declaration order; at least one. */
            const cancestry_recipe_signal_value_t *values;
            uint16_t value_count;
        } send_message;
        struct {
            /** Signal name (short or canonical). */
            const char *signal;
            cancestry_recipe_operand_t operand;
        } set_signal;
        struct {
            /** Recipe-local variable name. */
            const char *variable;
            cancestry_recipe_operand_t operand;
        } set_variable;
        struct {
            /** Timer name. */
            const char *timer;
            /** Duration in milliseconds; 0 when not declared. */
            uint32_t duration_ms;
            bool has_duration_ms;
            bool repeat;
        } start_timer;
        struct {
            /** Timer name for stop_timer / reset_timer. */
            const char *timer;
        } timer;
        struct {
            cancestry_recipe_log_level_t level;
            const char *message;
        } log;
        struct {
            /** Fault code text, e.g. "CLUSTER_TIMEOUT". */
            const char *code;
            cancestry_fault_severity_t severity;
        } raise_fault;
    } as;
} cancestry_recipe_action_t;

/**
 * One recipe (recipe-spec.md sections 2-5).
 *
 * Instances are owned by their recipe set and remain valid until the set is
 * freed. All pointers are borrowed from the set allocation.
 */
typedef struct cancestry_recipe {
    /** Recipe name; unique within its file (enforced by the loader). */
    const char *name;
    /** Description, or NULL when not declared. */
    const char *description;
    /** Whether the recipe may be invoked; defaults to true. */
    bool enabled;
    cancestry_recipe_trigger_t trigger;
    /** Conditions, in declaration order; all must hold. May be NULL/0. */
    const cancestry_recipe_condition_t *conditions;
    uint16_t condition_count;
    /** Actions, in declaration order; at least one. */
    const cancestry_recipe_action_t *actions;
    uint16_t action_count;
    /** Failure policy for the action sequence; defaults to stop. */
    cancestry_recipe_on_error_t on_error;
} cancestry_recipe_t;

/**
 * A loaded recipe file.
 *
 * Produced by cancestry_recipe_set_load() (one heap block that owns the set
 * and everything it points to) and released by cancestry_recipe_set_free().
 */
typedef struct cancestry_recipe_set {
    /** Recipes, in file (definition) order; at least one. */
    const cancestry_recipe_t *recipes;
    uint16_t recipe_count;
} cancestry_recipe_set_t;

/* ------------------------------------------------------------------------- */
/* Helpers                                                                    */
/* ------------------------------------------------------------------------- */

/** @return true when @p status indicates success (zero or positive). */
bool cancestry_recipe_status_is_ok(cancestry_recipe_status_t status);

/** @return Stable, statically allocated name for @p status. Never NULL. */
const char *cancestry_recipe_status_name(cancestry_recipe_status_t status);

/** @return Stable, statically allocated schema name such as "can_rx". Never NULL. */
const char *cancestry_recipe_trigger_event_name(cancestry_recipe_trigger_event_t event);

/**
 * Map a trigger event to the event-model type it matches.
 *
 * @return The matching cancestry_event_type_t, or CANCESTRY_EVENT_TYPE_INVALID
 *         for CANCESTRY_RECIPE_TRIGGER_TIMEOUT (the event model defines no
 *         timeout event; such triggers load but never match) or an invalid
 *         value.
 */
cancestry_event_type_t cancestry_recipe_trigger_event_type(cancestry_recipe_trigger_event_t event);

/** @return Stable, statically allocated name such as "send_message". Never NULL. */
const char *cancestry_recipe_action_kind_name(cancestry_recipe_action_kind_t kind);

/** @return Stable, statically allocated name such as "stop". Never NULL. */
const char *cancestry_recipe_on_error_name(cancestry_recipe_on_error_t on_error);

/** @return Stable, statically allocated name such as "warning". Never NULL. */
const char *cancestry_recipe_log_level_name(cancestry_recipe_log_level_t level);

#ifdef __cplusplus
}
#endif

#endif /* CANCESTRY_RECIPE_TYPES_H */
