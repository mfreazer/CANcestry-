/*
 * CANcestry - safe expression evaluator (internal to core/fsm).
 *
 * Normative references:
 *   docs/system/expression-language.md  sections 1-7 (grammar, precedence, types,
 *                                       failure rules, coercion, built-in functions)
 *   docs/software/SwRS.md               SW-FR-FSM-035 .. SW-FR-FSM-038
 *   docs/software/SwAD.md               section 9 (evaluation is bounded and
 *                                       allocation-free)
 *
 * This header is private to the core/fsm library: the FSM runtime exposes
 * evaluation through guards and action operands, not through a public
 * expression API. The loader includes it to validate expression grammar at
 * load time (SwAD.md section 10: invalid expressions are definition errors).
 */

#ifndef CANCESTRY_FSM_EXPRESSION_H
#define CANCESTRY_FSM_EXPRESSION_H

#include "cancestry/fsm/types.h"

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Maximum nesting depth of the recursive-descent evaluator.
 *
 * The evaluator recurses once per grammar level (parenthesis, unary operator,
 * logical operator), so bounding the depth bounds both the stack use and the
 * worst-case work; no allocation and no runaway recursion are possible
 * (SW-FR-FSM-037, SYS-NF-002).
 */
#define CANCESTRY_FSM_EXPRESSION_MAX_DEPTH ((unsigned)32u)

/** Evaluation failures; each maps to an expression fault in the spec. */
typedef enum cancestry_fsm_expression_error {
    CANCESTRY_FSM_EXPRESSION_OK = 0,
    /** Malformed text: unexpected character, missing operand or operator. */
    CANCESTRY_FSM_EXPRESSION_ERR_SYNTAX = 1,
    /** Well-formed prefix followed by trailing input. */
    CANCESTRY_FSM_EXPRESSION_ERR_TRAILING = 2,
    /** Nesting deeper than ::CANCESTRY_FSM_EXPRESSION_MAX_DEPTH. */
    CANCESTRY_FSM_EXPRESSION_ERR_DEPTH = 3,
    /** Identifier without a sig./var./evt. namespace, or an unknown namespace. */
    CANCESTRY_FSM_EXPRESSION_ERR_NAMESPACE = 4,
    /** sig./var./evt. name does not exist at run time. */
    CANCESTRY_FSM_EXPRESSION_ERR_UNDEFINED = 5,
    /** Signal read is not in the package's read allowlist. */
    CANCESTRY_FSM_EXPRESSION_ERR_UNAUTHORIZED = 6,
    /** Invalid type coercion (spec section 6). */
    CANCESTRY_FSM_EXPRESSION_ERR_TYPE = 7,
    /** Signed integer overflow (spec section 5). */
    CANCESTRY_FSM_EXPRESSION_ERR_OVERFLOW = 8,
    /** Division or modulo by zero (spec section 5). */
    CANCESTRY_FSM_EXPRESSION_ERR_ZERO_DIVIDE = 9,
    /** NaN or Infinity result (spec section 5). */
    CANCESTRY_FSM_EXPRESSION_ERR_NOT_A_NUMBER = 10,
    /** Call of a name that is not a built-in, or a wrong argument count. */
    CANCESTRY_FSM_EXPRESSION_ERR_FUNCTION = 11,
    /** clamp() with low > high, or a result outside the integer range. */
    CANCESTRY_FSM_EXPRESSION_ERR_DOMAIN = 12,
    /** NULL text, or text longer than ::CANCESTRY_FSM_EXPRESSION_MAX. */
    CANCESTRY_FSM_EXPRESSION_ERR_LENGTH = 13
} cancestry_fsm_expression_error_t;

/**
 * Name resolution inputs for one evaluation.
 *
 * The callbacks are optional; a missing callback resolves to
 * ::CANCESTRY_FSM_EXPRESSION_ERR_UNDEFINED, which is the fail-closed direction
 * (an unresolvable name is a fault, never a silent zero).
 */
typedef struct cancestry_fsm_expression_context {
    /** Opaque context passed to both callbacks. */
    void *user_data;
    /**
     * Read sig.NAME.
     *
     * @return CANCESTRY_FSM_OK, CANCESTRY_FSM_ERR_NOT_FOUND (no such signal) or
     *         CANCESTRY_FSM_ERR_DENIED (read not permitted by capabilities).
     */
    cancestry_fsm_status_t (*read_signal)(void *user_data,
                                         const char *name,
                                         cancestry_value_t *value_out);
    /**
     * Read var.NAME (instance-scoped variable).
     *
     * @return CANCESTRY_FSM_OK or CANCESTRY_FSM_ERR_NOT_FOUND.
     */
    cancestry_fsm_status_t (*read_variable)(void *user_data,
                                           const char *name,
                                           cancestry_value_t *value_out);
    /** Event that caused the evaluation, for evt.*; NULL when there is none. */
    const cancestry_event_t *event;
} cancestry_fsm_expression_context_t;

/**
 * Evaluate an expression.
 *
 * @param text       NUL-terminated expression, at most
 *                   ::CANCESTRY_FSM_EXPRESSION_MAX characters.
 * @param context    Name resolution inputs; may be NULL, which selects
 *                   grammar-only validation (names are accepted, not resolved).
 * @param result_out Receives the value; NULL is allowed in validation mode.
 * @param error_out  Receives the failure reason; may be NULL.
 * @return true when the expression evaluated to a value.
 */
bool cancestry_fsm_expression_evaluate(const char *text,
                                      const cancestry_fsm_expression_context_t *context,
                                      cancestry_value_t *result_out,
                                      cancestry_fsm_expression_error_t *error_out);

/**
 * Validate expression grammar without resolving names (load-time check).
 *
 * @param text       Expression text.
 * @param error_out  Receives the failure reason; may be NULL.
 * @param offset_out Receives the 0-based offset of the failure; may be NULL.
 * @return true when the grammar is valid.
 */
bool cancestry_fsm_expression_validate(const char *text,
                                      cancestry_fsm_expression_error_t *error_out,
                                      size_t *offset_out);

/** @return Stable, statically allocated name for @p error. Never NULL. */
const char *cancestry_fsm_expression_error_name(cancestry_fsm_expression_error_t error);

/**
 * @return true when @p result is usable as a guard: a BOOL value. The guard
 *         result must be boolean (docs/system/expression-language.md section 6);
 *         anything else is a type fault.
 */
bool cancestry_fsm_expression_result_is_guard(const cancestry_value_t *result);

#ifdef __cplusplus
}
#endif

#endif /* CANCESTRY_FSM_EXPRESSION_H */
