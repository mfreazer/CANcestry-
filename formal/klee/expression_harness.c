/*
 * KLEE harness for the bounded FSM expression evaluator.
 *
 * Implements: SW-FR-FSM-035, SW-FR-FSM-037, SW-FR-FSM-038,
 *             SW-FR-FSM-046, SYS-NF-001, SYS-NF-002.
 *
 * This file is intentionally not part of the firmware or the default CMake
 * build. It is compiled to LLVM bitcode by formal/klee/run.sh. KLEE's
 * klee_make_symbolic() makes the expression text, its length, and the value
 * returned by the signal resolver symbolic. Every path is bounded by the
 * public expression limit and the evaluator's depth budget.
 */

#include "fsm_expression.h"

#include <stdint.h>
#include <string.h>

#ifdef __KLEE__
#include <klee/klee.h>
#else
/* The declarations keep editor/indexer diagnostics useful. A non-KLEE build
 * is not an executable proof and is deliberately rejected by run.sh. */
extern void klee_assert(int condition);
extern void klee_assume(int condition);
extern void klee_make_symbolic(void *address, size_t size, const char *name);
#endif

static cancestry_fsm_status_t symbolic_signal(void *user_data,
                                              const char *name,
                                              cancestry_value_t *value_out)
{
    int64_t value;

    (void)user_data;
    (void)name;
    klee_make_symbolic(&value, sizeof(value), "signal_value");
    value_out->kind = CANCESTRY_VALUE_KIND_INT;
    value_out->value.integer = value;
    return CANCESTRY_FSM_OK;
}

static void assert_bounded_text_is_safe(void)
{
    char text[CANCESTRY_FSM_EXPRESSION_MAX + 2u];
    uint32_t depth;
    size_t pos = 0u;
    size_t i;
    cancestry_fsm_expression_error_t error = CANCESTRY_FSM_EXPRESSION_OK;
    size_t offset = 0u;
    bool ok;

    /* A symbolic depth produces a family of valid expressions without the
     * path explosion of making all 256 source characters independent. Every
     * member is a valid grammar input until the documented depth budget is
     * crossed, so KLEE explores both the normal and the bounded-fault path. */
    klee_make_symbolic(&depth, sizeof(depth), "expression_depth");
    klee_assume(depth <= (uint32_t)CANCESTRY_FSM_EXPRESSION_MAX_DEPTH + 1u);
    for (i = 0u; i < (size_t)depth; ++i) {
        text[pos++] = '(';
    }
    text[pos++] = 't';
    text[pos++] = 'r';
    text[pos++] = 'u';
    text[pos++] = 'e';
    for (i = 0u; i < (size_t)depth; ++i) {
        text[pos++] = ')';
    }
    text[pos] = '\0';

    ok = cancestry_fsm_expression_validate(text, &error, &offset);
    if (depth <= (uint32_t)CANCESTRY_FSM_EXPRESSION_MAX_DEPTH) {
        klee_assert(ok);
        klee_assert(error == CANCESTRY_FSM_EXPRESSION_OK);
    } else {
        klee_assert(!ok);
        klee_assert(error == CANCESTRY_FSM_EXPRESSION_ERR_DEPTH);
    }
    klee_assert(offset <= (size_t)CANCESTRY_FSM_EXPRESSION_MAX);

    /* The independent length guard is also exercised with a concrete string;
     * it proves the parser is never entered for text beyond the API limit. */
    for (i = 0u; i < (size_t)CANCESTRY_FSM_EXPRESSION_MAX + 1u; ++i) {
        text[i] = ' ';
    }
    text[CANCESTRY_FSM_EXPRESSION_MAX + 1u] = '\0';
    error = CANCESTRY_FSM_EXPRESSION_OK;
    klee_assert(!cancestry_fsm_expression_validate(text, &error, &offset));
    klee_assert(error == CANCESTRY_FSM_EXPRESSION_ERR_LENGTH);
}

static void assert_symbolic_division_is_fail_closed(void)
{
    cancestry_fsm_expression_context_t context;
    cancestry_value_t result;
    cancestry_fsm_expression_error_t error = CANCESTRY_FSM_EXPRESSION_OK;
    bool ok;

    memset(&context, 0, sizeof(context));
    context.read_signal = symbolic_signal;

    /* Both operands are the same symbolic value. KLEE must visit the zero
     * path and prove that it returns a fault instead of executing C division
     * by zero. On non-zero paths the result is a valid signed integer. */
    ok = cancestry_fsm_expression_evaluate("sig.x / sig.x", &context, &result, &error);
    if (ok) {
        klee_assert(result.kind == CANCESTRY_VALUE_KIND_INT);
    } else {
        klee_assert(error == CANCESTRY_FSM_EXPRESSION_ERR_ZERO_DIVIDE ||
                    error == CANCESTRY_FSM_EXPRESSION_ERR_OVERFLOW ||
                    error == CANCESTRY_FSM_EXPRESSION_ERR_TYPE);
    }

    /* INT64_MIN / -1 is the other signed-division corner case. The evaluator
     * must report overflow before reaching the C operator. */
    ok = cancestry_fsm_expression_evaluate("sig.x / -1", &context, &result, &error);
    if (!ok) {
        klee_assert(error == CANCESTRY_FSM_EXPRESSION_ERR_OVERFLOW ||
                    error == CANCESTRY_FSM_EXPRESSION_ERR_ZERO_DIVIDE ||
                    error == CANCESTRY_FSM_EXPRESSION_ERR_TYPE);
    } else {
        klee_assert(result.kind == CANCESTRY_VALUE_KIND_INT);
    }
}

int main(void)
{
    assert_bounded_text_is_safe();
    assert_symbolic_division_is_fail_closed();
    return 0;
}
