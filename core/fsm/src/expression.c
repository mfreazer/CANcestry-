/*
 * CANcestry - safe expression evaluator.
 *
 * Implementation notes:
 *   - Recursive descent over the grammar of docs/system/expression-language.md
 *     section 2, with one parser function per grammar level and a depth budget
 *     (CANCESTRY_FSM_EXPRESSION_MAX_DEPTH). There is no eval(), no user code,
 *     and no allocation, so an expression cannot execute anything and cannot
 *     run away (SW-FR-FSM-037, SYS-NF-002).
 *   - Names must carry a namespace (spec section 1). A bare identifier is a
 *     parse failure, not a lookup, so the evaluator never guesses which bus a
 *     name belongs to.
 *   - The only callable names are the five built-ins of spec section 7; any
 *     other "name(" is rejected, so no user-callable hook exists.
 *   - Values are the event model's cancestry_value_t: BOOL, INT (signed 64),
 *     REAL (IEEE-754 binary64). UINT inputs widen to INT, or to REAL above
 *     INT64_MAX, matching core/recipe.
 *   - Fault model (spec section 5): checked integer arithmetic, division or
 *     modulo by zero, overflow, NaN/Infinity results, invalid coercion and
 *     undefined or unauthorized names all fail. Mixed integer/float promotes
 *     to float (section 6); `%` is integer-only, since the spec defines no
 *     float remainder.
 *   - No short-circuit: `and`/`or` evaluate both operands, so a fault on the
 *     right side is reported even when the left already decides the result.
 *     This matches core/recipe and keeps the fault model uniform; the cost is
 *     bounded by the expression length.
 *   - Validation mode (NULL context) checks grammar, namespaces, evt. field
 *     names and built-in arity, and resolves no names: a name lookup yields the
 *     wildcard kind (UNSET), which propagates through operators whose checks
 *     would need a concrete type. The loader uses this mode so a syntactically
 *     invalid guard is a definition error (SwAD.md section 10) while guards that
 *     are only meaningful at run time still load.
 *   - Integer division truncates toward zero and `%` follows it (C99
 *     semantics); this is the same rule core/recipe applies, so a guard and a
 *     recipe condition written identically behave identically.
 */

#include "fsm_expression.h"

#include <errno.h>
#include <float.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Largest value a double can hold that is still < INT64_MAX + 1 in the
 * rounding sense; used for the round() domain check. */
#define FSM_EXPR_INT64_MIN_AS_REAL (-9223372036854775808.0)
#define FSM_EXPR_INT64_MAX_PLUS_AS_REAL (9223372036854775808.0)

/** Largest digit run accepted in a numeric literal, keeping the token small. */
#define FSM_EXPR_NUMBER_TOKEN_MAX ((size_t)48u)

/* ------------------------------------------------------------------------- */
/* Parser state                                                              */
/* ------------------------------------------------------------------------- */

typedef struct fsm_expr_parser {
    const char *text;
    size_t length;
    size_t pos;
    const cancestry_fsm_expression_context_t *context;
    bool validate_only;
    unsigned depth;
    cancestry_fsm_expression_error_t error;
    size_t error_offset;
} fsm_expr_parser_t;

static void fsm_expr_fail(fsm_expr_parser_t *parser, cancestry_fsm_expression_error_t error)
{
    if (parser->error == CANCESTRY_FSM_EXPRESSION_OK) {
        parser->error = error;
        parser->error_offset = parser->pos;
    }
}

static bool fsm_expr_enter(fsm_expr_parser_t *parser)
{
    if (parser->depth >= CANCESTRY_FSM_EXPRESSION_MAX_DEPTH) {
        fsm_expr_fail(parser, CANCESTRY_FSM_EXPRESSION_ERR_DEPTH);
        return false;
    }
    parser->depth++;
    return true;
}

static void fsm_expr_leave(fsm_expr_parser_t *parser)
{
    parser->depth--;
}

static void fsm_expr_skip_space(fsm_expr_parser_t *parser)
{
    while (parser->pos < parser->length) {
        char c = parser->text[parser->pos];
        if (c == ' ' || c == '\t') {
            parser->pos++;
        } else {
            break;
        }
    }
}

static char fsm_expr_peek(fsm_expr_parser_t *parser)
{
    fsm_expr_skip_space(parser);
    if (parser->pos >= parser->length) {
        return '\0';
    }
    return parser->text[parser->pos];
}

static char fsm_expr_peek_ahead(fsm_expr_parser_t *parser, size_t offset)
{
    if (parser->pos + offset >= parser->length) {
        return '\0';
    }
    return parser->text[parser->pos + offset];
}

static bool fsm_expr_accept_char(fsm_expr_parser_t *parser, char expected)
{
    if (fsm_expr_peek(parser) == expected) {
        parser->pos++;
        return true;
    }
    return false;
}

static bool fsm_expr_is_digit(char c)
{
    return c >= '0' && c <= '9';
}

static bool fsm_expr_is_ident_start(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

static bool fsm_expr_is_ident_char(char c)
{
    return fsm_expr_is_ident_start(c) || fsm_expr_is_digit(c);
}

/** Accept a keyword only when it is not a prefix of a longer name. */
static bool fsm_expr_accept_word(fsm_expr_parser_t *parser, const char *word)
{
    size_t i = 0u;

    (void)fsm_expr_peek(parser);
    while (word[i] != '\0') {
        if (parser->pos + i >= parser->length || parser->text[parser->pos + i] != word[i]) {
            return false;
        }
        i++;
    }
    if (parser->pos + i < parser->length) {
        char after = parser->text[parser->pos + i];
        if (fsm_expr_is_ident_char(after) || after == '.') {
            return false;
        }
    }
    parser->pos += i;
    return true;
}

/* ------------------------------------------------------------------------- */
/* Values                                                                    */
/* ------------------------------------------------------------------------- */

static cancestry_value_t fsm_expr_bool(bool value)
{
    cancestry_value_t out;

    memset(&out, 0, sizeof(out));
    out.kind = CANCESTRY_VALUE_KIND_BOOL;
    out.value.boolean = value;
    return out;
}

static cancestry_value_t fsm_expr_int(int64_t value)
{
    cancestry_value_t out;

    memset(&out, 0, sizeof(out));
    out.kind = CANCESTRY_VALUE_KIND_INT;
    out.value.integer = value;
    return out;
}

static cancestry_value_t fsm_expr_real(double value)
{
    cancestry_value_t out;

    memset(&out, 0, sizeof(out));
    out.kind = CANCESTRY_VALUE_KIND_REAL;
    out.value.real = value;
    return out;
}

/** Wildcard value, produced only in validation mode. */
static cancestry_value_t fsm_expr_wild(void)
{
    cancestry_value_t out;

    memset(&out, 0, sizeof(out));
    out.kind = CANCESTRY_VALUE_KIND_UNSET;
    return out;
}

static bool fsm_expr_is_wild(const cancestry_value_t *value)
{
    return value->kind == CANCESTRY_VALUE_KIND_UNSET;
}

static bool fsm_expr_is_numeric(const cancestry_value_t *value)
{
    return value->kind == CANCESTRY_VALUE_KIND_INT || value->kind == CANCESTRY_VALUE_KIND_REAL;
}

static bool fsm_expr_is_finite(double value)
{
    /* Rejects NaN (no comparison holds) and +/- infinity. */
    return value >= -DBL_MAX && value <= DBL_MAX;
}

static bool fsm_expr_any_real(size_t count, const cancestry_value_t *values)
{
    size_t i;

    for (i = 0u; i < count; ++i) {
        if (values[i].kind == CANCESTRY_VALUE_KIND_REAL) {
            return true;
        }
    }
    return false;
}

static double fsm_expr_to_real(const cancestry_value_t *value)
{
    if (value->kind == CANCESTRY_VALUE_KIND_REAL) {
        return value->value.real;
    }
    return (double)value->value.integer;
}

static bool fsm_expr_from_value(const cancestry_value_t *in, cancestry_value_t *out)
{
    cancestry_value_t copy = *in;

    switch (in->kind) {
    case CANCESTRY_VALUE_KIND_BOOL:
    case CANCESTRY_VALUE_KIND_INT:
    case CANCESTRY_VALUE_KIND_REAL:
        *out = copy;
        return true;
    case CANCESTRY_VALUE_KIND_UINT:
        if (in->value.unsigned_integer <= (uint64_t)INT64_MAX) {
            *out = fsm_expr_int((int64_t)in->value.unsigned_integer);
        } else {
            *out = fsm_expr_real((double)in->value.unsigned_integer);
        }
        return true;
    default:
        memset(out, 0, sizeof(*out));
        return false;
    }
}

static cancestry_value_t fsm_expr_from_u64(uint64_t value)
{
    if (value <= (uint64_t)INT64_MAX) {
        return fsm_expr_int((int64_t)value);
    }
    return fsm_expr_real((double)value);
}

/** Round half away from zero, staying inside the int64 range. */
static bool fsm_expr_round(double value, int64_t *out)
{
    int64_t base;
    double truncated;

    if (!(value >= FSM_EXPR_INT64_MIN_AS_REAL && value < FSM_EXPR_INT64_MAX_PLUS_AS_REAL)) {
        return false;
    }
    base = (int64_t)value; /* truncation toward zero, defined inside the range */
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

/* ------------------------------------------------------------------------- */
/* Names                                                                     */
/* ------------------------------------------------------------------------- */

/** The static vocabulary of evt.FIELD names (spec section 1). */
static const char *const fsm_expr_event_fields[] = {
    "event_id",     "timestamp_us",   "sequence",       "cause_sequence", "priority_class",
    "type",         "interface_id",   "can_id",         "length",         "signal_id",
    "value",        "timer_id",       "instance_id",    "missed_count",   "state_id",
    "fault_code",   "severity",       "from_mode",      "to_mode",        "reason_code",
    NULL
};

static bool fsm_expr_field_known(const char *name, size_t length)
{
    size_t i;

    for (i = 0u; fsm_expr_event_fields[i] != NULL; ++i) {
        if (strlen(fsm_expr_event_fields[i]) == length &&
            memcmp(fsm_expr_event_fields[i], name, length) == 0) {
            return true;
        }
    }
    return false;
}

#define FSM_EXPR_FIELD(event_ptr, literal, expr)                                            \
    if (length == sizeof(literal) - 1u && memcmp(name, literal, sizeof(literal) - 1u) == 0) { \
        cancestry_value_t candidate = (expr);                                               \
        (void)(event_ptr);                                                                  \
        *out = candidate;                                                                   \
        return true;                                                                        \
    }

static bool fsm_expr_field_value(const cancestry_event_t *event,
                                const char *name,
                                size_t length,
                                cancestry_value_t *out)
{
    const cancestry_event_payload_t *payload = &event->payload;

    FSM_EXPR_FIELD(event, "event_id", fsm_expr_from_u64((uint64_t)event->event_id));
    FSM_EXPR_FIELD(event, "timestamp_us", fsm_expr_from_u64((uint64_t)event->timestamp_us));
    FSM_EXPR_FIELD(event, "sequence", fsm_expr_from_u64((uint64_t)event->sequence));
    FSM_EXPR_FIELD(event, "cause_sequence", fsm_expr_from_u64((uint64_t)event->cause_sequence));
    FSM_EXPR_FIELD(event, "priority_class", fsm_expr_from_u64((uint64_t)event->priority_class));
    FSM_EXPR_FIELD(event, "type", fsm_expr_from_u64((uint64_t)event->type));

    switch (event->type) {
    case CANCESTRY_EVENT_TYPE_CAN_RX:
        FSM_EXPR_FIELD(event, "interface_id",
                       fsm_expr_from_u64((uint64_t)payload->can_rx.interface_id));
        FSM_EXPR_FIELD(event, "can_id", fsm_expr_from_u64((uint64_t)payload->can_rx.can_id));
        FSM_EXPR_FIELD(event, "length", fsm_expr_from_u64((uint64_t)payload->can_rx.length));
        break;
    case CANCESTRY_EVENT_TYPE_SIGNAL_CHANGED:
        FSM_EXPR_FIELD(event, "signal_id",
                       fsm_expr_from_u64((uint64_t)payload->signal_changed.signal_id));
        if (length == 5u && memcmp(name, "value", 5u) == 0) {
            return fsm_expr_from_value(&payload->signal_changed.new_value, out);
        }
        break;
    case CANCESTRY_EVENT_TYPE_TIMER_EXPIRED:
        FSM_EXPR_FIELD(event, "timer_id",
                       fsm_expr_from_u64((uint64_t)payload->timer_expired.timer_id));
        FSM_EXPR_FIELD(event, "instance_id",
                       fsm_expr_from_u64((uint64_t)payload->timer_expired.instance_id));
        FSM_EXPR_FIELD(event, "missed_count",
                       fsm_expr_from_u64((uint64_t)payload->timer_expired.missed_count));
        break;
    case CANCESTRY_EVENT_TYPE_STATE_ENTERED:
    case CANCESTRY_EVENT_TYPE_STATE_EXITED:
        FSM_EXPR_FIELD(event, "instance_id",
                       fsm_expr_from_u64((uint64_t)payload->state.instance_id));
        FSM_EXPR_FIELD(event, "state_id", fsm_expr_from_u64((uint64_t)payload->state.state_id));
        break;
    case CANCESTRY_EVENT_TYPE_FAULT_RAISED:
        FSM_EXPR_FIELD(event, "fault_code", fsm_expr_from_u64((uint64_t)payload->fault.fault_code));
        FSM_EXPR_FIELD(event, "severity", fsm_expr_from_u64((uint64_t)payload->fault.severity));
        break;
    case CANCESTRY_EVENT_TYPE_POWER_MODE_CHANGED:
        FSM_EXPR_FIELD(event, "from_mode", fsm_expr_from_u64((uint64_t)payload->mode.from_mode));
        FSM_EXPR_FIELD(event, "to_mode", fsm_expr_from_u64((uint64_t)payload->mode.to_mode));
        FSM_EXPR_FIELD(event, "reason_code", fsm_expr_from_u64((uint64_t)payload->mode.reason_code));
        break;
    default:
        break;
    }
    return false;
}

#undef FSM_EXPR_FIELD

/** Copy a possibly dotted name out of the expression text into a buffer. */
static bool fsm_expr_copy_name(fsm_expr_parser_t *parser,
                              const char *name,
                              size_t length,
                              char *buffer,
                              size_t capacity)
{
    if (length == 0u || length >= capacity) {
        fsm_expr_fail(parser, CANCESTRY_FSM_EXPRESSION_ERR_NAMESPACE);
        return false;
    }
    memcpy(buffer, name, length);
    buffer[length] = '\0';
    return true;
}

static bool fsm_expr_resolve(fsm_expr_parser_t *parser,
                            const char *name,
                            size_t length,
                            cancestry_value_t *out)
{
    char buffer[CANCESTRY_FSM_NAME_MAX];
    cancestry_fsm_status_t status;

    if (length > 4u && memcmp(name, "sig.", 4u) == 0) {
        if (!fsm_expr_copy_name(parser, name + 4u, length - 4u, buffer, sizeof(buffer))) {
            return false;
        }
        if (parser->validate_only) {
            *out = fsm_expr_wild();
            return true;
        }
        if (parser->context == NULL || parser->context->read_signal == NULL) {
            fsm_expr_fail(parser, CANCESTRY_FSM_EXPRESSION_ERR_UNDEFINED);
            return false;
        }
        status = parser->context->read_signal(parser->context->user_data, buffer, out);
        if (status == CANCESTRY_FSM_ERR_DENIED) {
            /* "unauthorized signal access" is a spec section 5 fault. */
            fsm_expr_fail(parser, CANCESTRY_FSM_EXPRESSION_ERR_UNAUTHORIZED);
            return false;
        }
        if (status != CANCESTRY_FSM_OK || !fsm_expr_from_value(out, out)) {
            fsm_expr_fail(parser, CANCESTRY_FSM_EXPRESSION_ERR_UNDEFINED);
            return false;
        }
        return true;
    }

    if (length > 4u && memcmp(name, "var.", 4u) == 0) {
        if (!fsm_expr_copy_name(parser, name + 4u, length - 4u, buffer, sizeof(buffer))) {
            return false;
        }
        if (parser->validate_only) {
            *out = fsm_expr_wild();
            return true;
        }
        if (parser->context == NULL || parser->context->read_variable == NULL) {
            fsm_expr_fail(parser, CANCESTRY_FSM_EXPRESSION_ERR_UNDEFINED);
            return false;
        }
        status = parser->context->read_variable(parser->context->user_data, buffer, out);
        if (status != CANCESTRY_FSM_OK || !fsm_expr_from_value(out, out)) {
            fsm_expr_fail(parser, CANCESTRY_FSM_EXPRESSION_ERR_UNDEFINED);
            return false;
        }
        return true;
    }

    if (length > 4u && memcmp(name, "evt.", 4u) == 0) {
        const char *field = name + 4u;
        size_t field_length = length - 4u;

        /* The evt. vocabulary is static, so an unknown field is a definition
         * error and is reported even while validating. */
        if (!fsm_expr_field_known(field, field_length)) {
            fsm_expr_fail(parser, CANCESTRY_FSM_EXPRESSION_ERR_NAMESPACE);
            return false;
        }
        if (parser->validate_only) {
            *out = fsm_expr_wild();
            return true;
        }
        if (parser->context == NULL || parser->context->event == NULL) {
            fsm_expr_fail(parser, CANCESTRY_FSM_EXPRESSION_ERR_UNDEFINED);
            return false;
        }
        if (!fsm_expr_field_value(parser->context->event, field, field_length, out)) {
            /* The field exists but does not apply to this event type. */
            fsm_expr_fail(parser, CANCESTRY_FSM_EXPRESSION_ERR_UNDEFINED);
            return false;
        }
        return true;
    }

    /* Bare identifier, unknown namespace, or an empty name after the prefix. */
    fsm_expr_fail(parser, CANCESTRY_FSM_EXPRESSION_ERR_NAMESPACE);
    return false;
}

/* ------------------------------------------------------------------------- */
/* Grammar                                                                   */
/* ------------------------------------------------------------------------- */

static bool fsm_expr_parse_or(fsm_expr_parser_t *parser, cancestry_value_t *out);
static bool fsm_expr_parse_cmp(fsm_expr_parser_t *parser, cancestry_value_t *out);
static bool fsm_expr_parse_add(fsm_expr_parser_t *parser, cancestry_value_t *out);
static bool fsm_expr_parse_unary(fsm_expr_parser_t *parser, cancestry_value_t *out);
static bool fsm_expr_parse_primary(fsm_expr_parser_t *parser, cancestry_value_t *out);

/** Numeric binary operation; @p op is one of * / % + -. */
static bool fsm_expr_arith(fsm_expr_parser_t *parser,
                          cancestry_value_t *left,
                          char op,
                          const cancestry_value_t *right)
{
    if (fsm_expr_is_wild(left) || fsm_expr_is_wild(right)) {
        *left = fsm_expr_wild();
        return true;
    }
    if (!fsm_expr_is_numeric(left) || !fsm_expr_is_numeric(right)) {
        fsm_expr_fail(parser, CANCESTRY_FSM_EXPRESSION_ERR_TYPE);
        return false;
    }
    if (left->kind == CANCESTRY_VALUE_KIND_REAL || right->kind == CANCESTRY_VALUE_KIND_REAL) {
        double lhs = fsm_expr_to_real(left);
        double rhs = fsm_expr_to_real(right);
        double result;

        if (op == '*') {
            result = lhs * rhs;
        } else if (op == '/') {
            if (rhs == 0.0) {
                fsm_expr_fail(parser, CANCESTRY_FSM_EXPRESSION_ERR_ZERO_DIVIDE);
                return false;
            }
            result = lhs / rhs;
        } else if (op == '%') {
            /* No float remainder is defined by the spec; refusing is the safe
             * reading and matches core/recipe. */
            fsm_expr_fail(parser, CANCESTRY_FSM_EXPRESSION_ERR_TYPE);
            return false;
        } else if (op == '+') {
            result = lhs + rhs;
        } else {
            result = lhs - rhs;
        }
        if (!fsm_expr_is_finite(result)) {
            fsm_expr_fail(parser, CANCESTRY_FSM_EXPRESSION_ERR_NOT_A_NUMBER);
            return false;
        }
        *left = fsm_expr_real(result);
        return true;
    }
    {
        int64_t lhs = left->value.integer;
        int64_t rhs = right->value.integer;
        int64_t result;

        if (op == '+') {
            if ((rhs > 0 && lhs > INT64_MAX - rhs) || (rhs < 0 && lhs < INT64_MIN - rhs)) {
                fsm_expr_fail(parser, CANCESTRY_FSM_EXPRESSION_ERR_OVERFLOW);
                return false;
            }
            result = lhs + rhs;
        } else if (op == '-') {
            if ((rhs < 0 && lhs > INT64_MAX + rhs) || (rhs > 0 && lhs < INT64_MIN + rhs)) {
                fsm_expr_fail(parser, CANCESTRY_FSM_EXPRESSION_ERR_OVERFLOW);
                return false;
            }
            result = lhs - rhs;
        } else if (op == '*') {
            uint64_t limit = (uint64_t)INT64_MAX + 1u;
            uint64_t lhs_mag = (lhs < 0) ? (uint64_t)(-(lhs + 1)) + 1u : (uint64_t)lhs;
            uint64_t rhs_mag = (rhs < 0) ? (uint64_t)(-(rhs + 1)) + 1u : (uint64_t)rhs;
            uint64_t result_mag;
            bool negative = (lhs < 0) != (rhs < 0);

            if (lhs_mag != 0u && rhs_mag != 0u && lhs_mag > limit / rhs_mag) {
                fsm_expr_fail(parser, CANCESTRY_FSM_EXPRESSION_ERR_OVERFLOW);
                return false;
            }
            result_mag = lhs_mag * rhs_mag;
            if (result_mag > limit) {
                fsm_expr_fail(parser, CANCESTRY_FSM_EXPRESSION_ERR_OVERFLOW);
                return false;
            }
            if (negative) {
                result = (result_mag == limit) ? INT64_MIN : -((int64_t)result_mag);
            } else {
                if (result_mag > (uint64_t)INT64_MAX) {
                    fsm_expr_fail(parser, CANCESTRY_FSM_EXPRESSION_ERR_OVERFLOW);
                    return false;
                }
                result = (int64_t)result_mag;
            }
        } else {
            if (rhs == 0) {
                fsm_expr_fail(parser, CANCESTRY_FSM_EXPRESSION_ERR_ZERO_DIVIDE);
                return false;
            }
            if (lhs == INT64_MIN && rhs == -1) {
                /* INT64_MIN / -1 and INT64_MIN % -1 are not representable. */
                fsm_expr_fail(parser, CANCESTRY_FSM_EXPRESSION_ERR_OVERFLOW);
                return false;
            }
            result = (op == '/') ? (lhs / rhs) : (lhs % rhs);
        }
        *left = fsm_expr_int(result);
    }
    return true;
}

static bool fsm_expr_parse_mul(fsm_expr_parser_t *parser, cancestry_value_t *out)
{
    if (!fsm_expr_enter(parser)) {
        return false;
    }
    if (!fsm_expr_parse_unary(parser, out)) {
        fsm_expr_leave(parser);
        return false;
    }
    for (;;) {
        char op = fsm_expr_peek(parser);
        cancestry_value_t rhs;

        if (op != '*' && op != '/' && op != '%') {
            break;
        }
        parser->pos++;
        if (!fsm_expr_parse_unary(parser, &rhs)) {
            fsm_expr_leave(parser);
            return false;
        }
        if (!fsm_expr_arith(parser, out, op, &rhs)) {
            fsm_expr_leave(parser);
            return false;
        }
    }
    fsm_expr_leave(parser);
    return true;
}

static bool fsm_expr_parse_add(fsm_expr_parser_t *parser, cancestry_value_t *out)
{
    if (!fsm_expr_enter(parser)) {
        return false;
    }
    if (!fsm_expr_parse_mul(parser, out)) {
        fsm_expr_leave(parser);
        return false;
    }
    for (;;) {
        char op = fsm_expr_peek(parser);
        cancestry_value_t rhs;

        if (op != '+' && op != '-') {
            break;
        }
        parser->pos++;
        if (!fsm_expr_parse_mul(parser, &rhs)) {
            fsm_expr_leave(parser);
            return false;
        }
        if (!fsm_expr_arith(parser, out, op, &rhs)) {
            fsm_expr_leave(parser);
            return false;
        }
    }
    fsm_expr_leave(parser);
    return true;
}

/* cmp_expr := add_expr (op add_expr)?, per expression-language.md section 2. */
static bool fsm_expr_parse_cmp(fsm_expr_parser_t *parser, cancestry_value_t *out)
{
    cancestry_value_t lhs;
    char first;
    char second;
    char op = '\0';
    bool or_equal = false;
    cancestry_value_t rhs;
    bool result;

    if (!fsm_expr_parse_add(parser, out)) {
        return false;
    }
    lhs = *out;
    first = fsm_expr_peek(parser);
    second = fsm_expr_peek_ahead(parser, 1u);

    if (first != '=' && first != '!' && first != '<' && first != '>') {
        return true;
    }
    if (first == '=' && second == '=') {
        op = '=';
        parser->pos += 2u;
    } else if (first == '!' && second == '=') {
        op = '!';
        parser->pos += 2u;
    } else if (first == '<' && second == '=') {
        op = '<';
        or_equal = true;
        parser->pos += 2u;
    } else if (first == '>' && second == '=') {
        op = '>';
        or_equal = true;
        parser->pos += 2u;
    } else if (first == '<' || first == '>') {
        op = first;
        parser->pos++;
    } else {
        fsm_expr_fail(parser, CANCESTRY_FSM_EXPRESSION_ERR_SYNTAX);
        return false;
    }
    if (!fsm_expr_parse_add(parser, &rhs)) {
        return false;
    }
    if (fsm_expr_is_wild(&lhs) || fsm_expr_is_wild(&rhs)) {
        /* Validation mode: the result is a boolean, its operands are unknown. */
        *out = fsm_expr_bool(false);
        return true;
    }
    if (lhs.kind == CANCESTRY_VALUE_KIND_BOOL && rhs.kind == CANCESTRY_VALUE_KIND_BOOL) {
        if (op != '=' && op != '!') {
            fsm_expr_fail(parser, CANCESTRY_FSM_EXPRESSION_ERR_TYPE);
            return false;
        }
        result = (op == '=') ? (lhs.value.boolean == rhs.value.boolean)
                             : (lhs.value.boolean != rhs.value.boolean);
    } else if (fsm_expr_is_numeric(&lhs) && fsm_expr_is_numeric(&rhs)) {
        double left_real = fsm_expr_to_real(&lhs);
        double right_real = fsm_expr_to_real(&rhs);

        if (op == '=') {
            result = left_real == right_real;
        } else if (op == '!') {
            result = left_real != right_real;
        } else if (op == '<') {
            result = or_equal ? (left_real <= right_real) : (left_real < right_real);
        } else {
            result = or_equal ? (left_real >= right_real) : (left_real > right_real);
        }
    } else {
        /* A boolean compared with a number: invalid coercion (spec section 6). */
        fsm_expr_fail(parser, CANCESTRY_FSM_EXPRESSION_ERR_TYPE);
        return false;
    }
    *out = fsm_expr_bool(result);
    return true;
}

static bool fsm_expr_parse_not(fsm_expr_parser_t *parser, cancestry_value_t *out)
{
    if (!fsm_expr_enter(parser)) {
        return false;
    }
    if (fsm_expr_accept_word(parser, "not")) {
        if (!fsm_expr_parse_not(parser, out)) {
            fsm_expr_leave(parser);
            return false;
        }
        if (!fsm_expr_is_wild(out)) {
            if (out->kind != CANCESTRY_VALUE_KIND_BOOL) {
                fsm_expr_fail(parser, CANCESTRY_FSM_EXPRESSION_ERR_TYPE);
                fsm_expr_leave(parser);
                return false;
            }
            out->value.boolean = !out->value.boolean;
        }
        fsm_expr_leave(parser);
        return true;
    }
    if (!fsm_expr_parse_cmp(parser, out)) {
        fsm_expr_leave(parser);
        return false;
    }
    fsm_expr_leave(parser);
    return true;
}

static bool fsm_expr_parse_and(fsm_expr_parser_t *parser, cancestry_value_t *out)
{
    if (!fsm_expr_parse_not(parser, out)) {
        return false;
    }
    while (fsm_expr_accept_word(parser, "and")) {
        cancestry_value_t lhs = *out;
        cancestry_value_t rhs;

        if (!fsm_expr_parse_not(parser, &rhs)) {
            return false;
        }
        if (fsm_expr_is_wild(&lhs) || fsm_expr_is_wild(&rhs)) {
            *out = fsm_expr_bool(false);
            continue;
        }
        if (lhs.kind != CANCESTRY_VALUE_KIND_BOOL || rhs.kind != CANCESTRY_VALUE_KIND_BOOL) {
            fsm_expr_fail(parser, CANCESTRY_FSM_EXPRESSION_ERR_TYPE);
            return false;
        }
        *out = fsm_expr_bool(lhs.value.boolean && rhs.value.boolean);
    }
    return true;
}

static bool fsm_expr_parse_or(fsm_expr_parser_t *parser, cancestry_value_t *out)
{
    if (!fsm_expr_enter(parser)) {
        return false;
    }
    if (!fsm_expr_parse_and(parser, out)) {
        fsm_expr_leave(parser);
        return false;
    }
    while (fsm_expr_accept_word(parser, "or")) {
        cancestry_value_t lhs = *out;
        cancestry_value_t rhs;

        if (!fsm_expr_parse_and(parser, &rhs)) {
            fsm_expr_leave(parser);
            return false;
        }
        if (fsm_expr_is_wild(&lhs) || fsm_expr_is_wild(&rhs)) {
            *out = fsm_expr_bool(false);
            continue;
        }
        if (lhs.kind != CANCESTRY_VALUE_KIND_BOOL || rhs.kind != CANCESTRY_VALUE_KIND_BOOL) {
            fsm_expr_fail(parser, CANCESTRY_FSM_EXPRESSION_ERR_TYPE);
            fsm_expr_leave(parser);
            return false;
        }
        *out = fsm_expr_bool(lhs.value.boolean || rhs.value.boolean);
    }
    fsm_expr_leave(parser);
    return true;
}

static bool fsm_expr_parse_unary(fsm_expr_parser_t *parser, cancestry_value_t *out)
{
    if (!fsm_expr_enter(parser)) {
        return false;
    }
    if (fsm_expr_accept_char(parser, '-')) {
        if (!fsm_expr_parse_unary(parser, out)) {
            fsm_expr_leave(parser);
            return false;
        }
        if (!fsm_expr_is_wild(out)) {
            if (out->kind == CANCESTRY_VALUE_KIND_BOOL) {
                fsm_expr_fail(parser, CANCESTRY_FSM_EXPRESSION_ERR_TYPE);
                fsm_expr_leave(parser);
                return false;
            }
            if (out->kind == CANCESTRY_VALUE_KIND_INT) {
                if (out->value.integer == INT64_MIN) {
                    fsm_expr_fail(parser, CANCESTRY_FSM_EXPRESSION_ERR_OVERFLOW);
                    fsm_expr_leave(parser);
                    return false;
                }
                out->value.integer = -out->value.integer;
            } else {
                out->value.real = -out->value.real;
            }
        }
        fsm_expr_leave(parser);
        return true;
    }
    if (fsm_expr_accept_char(parser, '+')) {
        /* A leading '+' is not in the grammar; rejecting it keeps the accepted
         * language exactly the documented one. */
        fsm_expr_fail(parser, CANCESTRY_FSM_EXPRESSION_ERR_SYNTAX);
        fsm_expr_leave(parser);
        return false;
    }
    if (!fsm_expr_parse_primary(parser, out)) {
        fsm_expr_leave(parser);
        return false;
    }
    fsm_expr_leave(parser);
    return true;
}

/** Numeric literal: integer, or real when it carries a fraction or exponent. */
/**
 * Numeric literal.
 *
 * Representability is checked in both modes (an unrepresentable literal is a
 * definition error), but the *value* is only produced while evaluating: in
 * validation mode every literal becomes the wildcard, so a constant expression
 * such as "10 / 0" is judged by the same rule as "var.n / 0" - a run-time
 * expression fault. Keeping the two paths uniform is what stops a guard from
 * behaving differently depending on whether a number was written inline.
 */
static bool fsm_expr_parse_number_impl(fsm_expr_parser_t *parser, cancestry_value_t *out)
{
    char token[FSM_EXPR_NUMBER_TOKEN_MAX];
    size_t length = 0u;
    bool is_real = false;
    bool seen_digit = false;
    bool seen_fraction_digit = false;
    uint64_t magnitude = 0u;
    bool magnitude_overflow = false;

    while (parser->pos < parser->length) {
        char c = parser->text[parser->pos];

        if (fsm_expr_is_digit(c)) {
            if (!is_real) {
                unsigned digit = (unsigned)(c - '0');

                if (magnitude > (UINT64_MAX - digit) / 10u) {
                    magnitude_overflow = true;
                } else {
                    magnitude = (magnitude * 10u) + digit;
                }
            }
            seen_digit = true;
            if (length + 1u >= sizeof(token)) {
                fsm_expr_fail(parser, CANCESTRY_FSM_EXPRESSION_ERR_SYNTAX);
                return false;
            }
            token[length++] = c;
            parser->pos++;
            continue;
        }
        if (!is_real && c == '.' && fsm_expr_is_digit(fsm_expr_peek_ahead(parser, 1u))) {
            is_real = true;
            if (length + 1u >= sizeof(token)) {
                fsm_expr_fail(parser, CANCESTRY_FSM_EXPRESSION_ERR_SYNTAX);
                return false;
            }
            token[length++] = c;
            parser->pos++;
            seen_fraction_digit = true;
            continue;
        }
        if (seen_digit && (c == 'e' || c == 'E')) {
            size_t next = parser->pos + 1u;
            bool negative = false;
            bool exponent_digit = false;

            is_real = true;
            if (next < parser->length && (parser->text[next] == '+' || parser->text[next] == '-')) {
                negative = parser->text[next] == '-';
                next++;
            }
            while (next < parser->length && fsm_expr_is_digit(parser->text[next])) {
                exponent_digit = true;
                next++;
            }
            if (!exponent_digit) {
                fsm_expr_fail(parser, CANCESTRY_FSM_EXPRESSION_ERR_SYNTAX);
                return false;
            }
            if (length + (next - parser->pos) >= sizeof(token)) {
                fsm_expr_fail(parser, CANCESTRY_FSM_EXPRESSION_ERR_SYNTAX);
                return false;
            }
            while (parser->pos < next) {
                token[length++] = parser->text[parser->pos];
                parser->pos++;
            }
            (void)negative;
            continue;
        }
        break;
    }
    if (!seen_digit) {
        fsm_expr_fail(parser, CANCESTRY_FSM_EXPRESSION_ERR_SYNTAX);
        return false;
    }
    token[length] = '\0';

    if (!is_real) {
        if (magnitude_overflow || magnitude > (uint64_t)INT64_MAX) {
            /* An integer literal beyond int64 becomes a real, exactly as the
             * loaders do, so 18446744073709551616 evaluates instead of failing. */
            *out = fsm_expr_real(strtod(token, NULL));
            if (!fsm_expr_is_finite(out->value.real)) {
                fsm_expr_fail(parser, CANCESTRY_FSM_EXPRESSION_ERR_OVERFLOW);
                return false;
            }
            out->kind = CANCESTRY_VALUE_KIND_REAL;
            return true;
        }
        *out = fsm_expr_int((int64_t)magnitude);
        return true;
    }
    {
        char *end = NULL;
        double value;

        errno = 0;
        value = strtod(token, &end);
        if (end == NULL || *end != '\0' || errno == ERANGE || !fsm_expr_is_finite(value)) {
            fsm_expr_fail(parser, seen_fraction_digit
                                       ? CANCESTRY_FSM_EXPRESSION_ERR_NOT_A_NUMBER
                                       : CANCESTRY_FSM_EXPRESSION_ERR_OVERFLOW);
            return false;
        }
        *out = fsm_expr_real(value);
    }
    return true;
}

static bool fsm_expr_parse_number(fsm_expr_parser_t *parser, cancestry_value_t *out)
{
    if (!fsm_expr_parse_number_impl(parser, out)) {
        return false;
    }
    if (parser->validate_only) {
        *out = fsm_expr_wild();
    }
    return true;
}

/** Apply one built-in (spec section 7). */
static bool fsm_expr_apply_call(fsm_expr_parser_t *parser,
                               const char *name,
                               size_t length,
                               cancestry_value_t *args,
                               size_t argc,
                               cancestry_value_t *out)
{
    size_t i;
    bool any_wild = false;

    for (i = 0u; i < argc; ++i) {
        if (fsm_expr_is_wild(&args[i])) {
            any_wild = true;
        }
    }
    if (length == 5u && memcmp(name, "round", 5u) == 0) {
        int64_t rounded = 0;

        if (argc != 1u) {
            goto arity;
        }
        if (any_wild) {
            /* round() always returns an integer, even when the input is a
             * value that only the run time can resolve. */
            *out = fsm_expr_int(0);
            return true;
        }
        if (args[0].kind == CANCESTRY_VALUE_KIND_INT) {
            *out = args[0];
            return true;
        }
        if (args[0].kind != CANCESTRY_VALUE_KIND_REAL ||
            !fsm_expr_round(args[0].value.real, &rounded)) {
            /* Out of the integer range, or a non-numeric operand. */
            fsm_expr_fail(parser, fsm_expr_is_numeric(&args[0])
                                      ? CANCESTRY_FSM_EXPRESSION_ERR_DOMAIN
                                      : CANCESTRY_FSM_EXPRESSION_ERR_TYPE);
            return false;
        }
        *out = fsm_expr_int(rounded);
        return true;
    }
    if (length == 3u && memcmp(name, "abs", 3u) == 0) {
        if (argc != 1u) {
            goto arity;
        }
        if (any_wild) {
            *out = fsm_expr_wild();
            return true;
        }
        if (!fsm_expr_is_numeric(&args[0])) {
            fsm_expr_fail(parser, CANCESTRY_FSM_EXPRESSION_ERR_TYPE);
            return false;
        }
        if (args[0].kind == CANCESTRY_VALUE_KIND_INT) {
            if (args[0].value.integer == INT64_MIN) {
                fsm_expr_fail(parser, CANCESTRY_FSM_EXPRESSION_ERR_OVERFLOW);
                return false;
            }
            *out = fsm_expr_int(args[0].value.integer < 0 ? -args[0].value.integer
                                                          : args[0].value.integer);
        } else {
            double value = args[0].value.real;

            *out = fsm_expr_real(value < 0.0 ? -value : value);
        }
        return true;
    }
    if ((length == 3u && memcmp(name, "min", 3u) == 0) ||
        (length == 3u && memcmp(name, "max", 3u) == 0)) {
        bool want_min = name[0] == 'm' && name[1] == 'i';

        if (argc != 2u) {
            goto arity;
        }
        if (any_wild) {
            *out = fsm_expr_wild();
            return true;
        }
        if (!fsm_expr_is_numeric(&args[0]) || !fsm_expr_is_numeric(&args[1])) {
            fsm_expr_fail(parser, CANCESTRY_FSM_EXPRESSION_ERR_TYPE);
            return false;
        }
        {
            bool first_smaller = fsm_expr_to_real(&args[0]) < fsm_expr_to_real(&args[1]);
            bool equal = fsm_expr_to_real(&args[0]) == fsm_expr_to_real(&args[1]);
            size_t pick = (want_min ? (first_smaller || equal) : (!first_smaller || equal))
                              ? 0u
                              : 1u;

            if (fsm_expr_any_real(2u, args)) {
                *out = fsm_expr_real(fsm_expr_to_real(&args[pick]));
            } else {
                *out = fsm_expr_int(args[pick].value.integer);
            }
        }
        return true;
    }
    if (length == 5u && memcmp(name, "clamp", 5u) == 0) {
        double value;
        double low;
        double high;
        bool real_result;

        if (argc != 3u) {
            goto arity;
        }
        if (any_wild) {
            *out = fsm_expr_wild();
            return true;
        }
        for (i = 0u; i < 3u; ++i) {
            if (!fsm_expr_is_numeric(&args[i])) {
                fsm_expr_fail(parser, CANCESTRY_FSM_EXPRESSION_ERR_TYPE);
                return false;
            }
        }
        low = fsm_expr_to_real(&args[1]);
        high = fsm_expr_to_real(&args[2]);
        if (low > high) {
            /* "clamp requires low <= high" (spec section 7). */
            fsm_expr_fail(parser, CANCESTRY_FSM_EXPRESSION_ERR_DOMAIN);
            return false;
        }
        value = fsm_expr_to_real(&args[0]);
        if (value < low) {
            value = low;
        } else if (value > high) {
            value = high;
        }
        real_result = fsm_expr_any_real(3u, args);
        if (real_result) {
            *out = fsm_expr_real(value);
        } else {
            int64_t clamped;

            if (!fsm_expr_round(value, &clamped)) {
                fsm_expr_fail(parser, CANCESTRY_FSM_EXPRESSION_ERR_DOMAIN);
                return false;
            }
            *out = fsm_expr_int(clamped);
        }
        return true;
    }
    /* Not one of the five permitted built-ins: no arbitrary code execution. */
    (void)parser;
    return false;

arity:
    fsm_expr_fail(parser, CANCESTRY_FSM_EXPRESSION_ERR_FUNCTION);
    return false;
}

static bool fsm_expr_parse_call(fsm_expr_parser_t *parser,
                               const char *name,
                               size_t length,
                               cancestry_value_t *out)
{
    static const char *const allowed[] = {"min", "max", "clamp", "abs", "round", NULL};
    cancestry_value_t args[3];
    size_t argc = 0u;
    size_t i;
    bool known = false;

    for (i = 0u; allowed[i] != NULL; ++i) {
        if (strlen(allowed[i]) == length && memcmp(allowed[i], name, length) == 0) {
            known = true;
            break;
        }
    }
    if (!known) {
        /* Reject the call before consuming input: the name is not callable. */
        fsm_expr_fail(parser, CANCESTRY_FSM_EXPRESSION_ERR_FUNCTION);
        return false;
    }
    if (!fsm_expr_accept_char(parser, '(')) {
        fsm_expr_fail(parser, CANCESTRY_FSM_EXPRESSION_ERR_SYNTAX);
        return false;
    }
    if (fsm_expr_peek(parser) != ')') {
        for (;;) {
            if (argc >= sizeof(args) / sizeof(args[0])) {
                fsm_expr_fail(parser, CANCESTRY_FSM_EXPRESSION_ERR_FUNCTION);
                return false;
            }
            if (!fsm_expr_enter(parser)) {
                return false;
            }
            if (!fsm_expr_parse_or(parser, &args[argc])) {
                fsm_expr_leave(parser);
                return false;
            }
            fsm_expr_leave(parser);
            argc++;
            if (!fsm_expr_accept_char(parser, ',')) {
                break;
            }
        }
    }
    if (!fsm_expr_accept_char(parser, ')')) {
        fsm_expr_fail(parser, CANCESTRY_FSM_EXPRESSION_ERR_SYNTAX);
        return false;
    }
    if (!fsm_expr_apply_call(parser, name, length, args, argc, out)) {
        if (parser->error == CANCESTRY_FSM_EXPRESSION_OK) {
            fsm_expr_fail(parser, CANCESTRY_FSM_EXPRESSION_ERR_FUNCTION);
        }
        return false;
    }
    return true;
}

static bool fsm_expr_parse_primary(fsm_expr_parser_t *parser, cancestry_value_t *out)
{
    char c = fsm_expr_peek(parser);

    if (c == '\0') {
        fsm_expr_fail(parser, CANCESTRY_FSM_EXPRESSION_ERR_SYNTAX);
        return false;
    }
    if (c == '(') {
        parser->pos++;
        if (!fsm_expr_parse_or(parser, out)) {
            return false;
        }
        if (!fsm_expr_accept_char(parser, ')')) {
            fsm_expr_fail(parser, CANCESTRY_FSM_EXPRESSION_ERR_SYNTAX);
            return false;
        }
        return true;
    }
    if (fsm_expr_is_digit(c) || (c == '.' && fsm_expr_is_digit(fsm_expr_peek_ahead(parser, 1u)))) {
        return fsm_expr_parse_number(parser, out);
    }
    if (fsm_expr_accept_word(parser, "true")) {
        *out = parser->validate_only ? fsm_expr_wild() : fsm_expr_bool(true);
        return true;
    }
    if (fsm_expr_accept_word(parser, "false")) {
        *out = parser->validate_only ? fsm_expr_wild() : fsm_expr_bool(false);
        return true;
    }
    if (fsm_expr_is_ident_start(c)) {
        size_t start = parser->pos;
        size_t length;

        while (parser->pos < parser->length &&
               (fsm_expr_is_ident_char(parser->text[parser->pos]) ||
                parser->text[parser->pos] == '.')) {
            parser->pos++;
        }
        /*
         * The length must be captured before the next peek: peeking skips
         * whitespace, which would otherwise extend the name past its real end.
         */
        length = parser->pos - start;
        if (fsm_expr_peek(parser) == '(') {
            return fsm_expr_parse_call(parser, parser->text + start, length, out);
        }
        return fsm_expr_resolve(parser, parser->text + start, length, out);
    }
    fsm_expr_fail(parser, CANCESTRY_FSM_EXPRESSION_ERR_SYNTAX);
    return false;
}

/* ------------------------------------------------------------------------- */
/* Public API                                                                */
/* ------------------------------------------------------------------------- */

bool cancestry_fsm_expression_evaluate(const char *text,
                                      const cancestry_fsm_expression_context_t *context,
                                      cancestry_value_t *result_out,
                                      cancestry_fsm_expression_error_t *error_out)
{
    fsm_expr_parser_t parser;
    cancestry_value_t result;

    if (error_out != NULL) {
        *error_out = CANCESTRY_FSM_EXPRESSION_OK;
    }
    if (text == NULL) {
        if (error_out != NULL) {
            *error_out = CANCESTRY_FSM_EXPRESSION_ERR_LENGTH;
        }
        return false;
    }
    if (strlen(text) > CANCESTRY_FSM_EXPRESSION_MAX) {
        if (error_out != NULL) {
            *error_out = CANCESTRY_FSM_EXPRESSION_ERR_LENGTH;
        }
        return false;
    }

    memset(&parser, 0, sizeof(parser));
    parser.text = text;
    parser.length = strlen(text);
    parser.context = context;
    parser.validate_only = context == NULL;
    memset(&result, 0, sizeof(result));

    if (!fsm_expr_parse_or(&parser, &result)) {
        if (error_out != NULL) {
            *error_out = (parser.error != CANCESTRY_FSM_EXPRESSION_OK)
                             ? parser.error
                             : CANCESTRY_FSM_EXPRESSION_ERR_SYNTAX;
        }
        return false;
    }
    if (parser.pos < parser.length) {
        if (error_out != NULL) {
            *error_out = CANCESTRY_FSM_EXPRESSION_ERR_TRAILING;
        }
        return false;
    }
    if (result.kind == CANCESTRY_VALUE_KIND_UNSET) {
        /* A wholly unresolved expression cannot happen in run-time mode, where
         * every leaf is either concrete or an error. Fail closed anyway. */
        if (error_out != NULL) {
            *error_out = CANCESTRY_FSM_EXPRESSION_ERR_UNDEFINED;
        }
        return false;
    }
    if (result_out != NULL) {
        *result_out = result;
    }
    return true;
}

bool cancestry_fsm_expression_validate(const char *text,
                                      cancestry_fsm_expression_error_t *error_out,
                                      size_t *offset_out)
{
    fsm_expr_parser_t parser;
    cancestry_value_t result;

    if (error_out != NULL) {
        *error_out = CANCESTRY_FSM_EXPRESSION_OK;
    }
    if (offset_out != NULL) {
        *offset_out = 0u;
    }
    if (text == NULL) {
        if (error_out != NULL) {
            *error_out = CANCESTRY_FSM_EXPRESSION_ERR_LENGTH;
        }
        return false;
    }
    if (strlen(text) > CANCESTRY_FSM_EXPRESSION_MAX) {
        if (error_out != NULL) {
            *error_out = CANCESTRY_FSM_EXPRESSION_ERR_LENGTH;
        }
        return false;
    }

    memset(&parser, 0, sizeof(parser));
    parser.text = text;
    parser.length = strlen(text);
    parser.validate_only = true;
    memset(&result, 0, sizeof(result));

    if (!fsm_expr_parse_or(&parser, &result)) {
        if (error_out != NULL) {
            *error_out = (parser.error != CANCESTRY_FSM_EXPRESSION_OK)
                             ? parser.error
                             : CANCESTRY_FSM_EXPRESSION_ERR_SYNTAX;
        }
        if (offset_out != NULL) {
            *offset_out = parser.error_offset;
        }
        return false;
    }
    if (parser.pos < parser.length) {
        if (error_out != NULL) {
            *error_out = CANCESTRY_FSM_EXPRESSION_ERR_TRAILING;
        }
        if (offset_out != NULL) {
            *offset_out = parser.pos;
        }
        return false;
    }
    return true;
}

const char *cancestry_fsm_expression_error_name(cancestry_fsm_expression_error_t error)
{
    switch (error) {
    case CANCESTRY_FSM_EXPRESSION_OK:
        return "ok";
    case CANCESTRY_FSM_EXPRESSION_ERR_SYNTAX:
        return "syntax";
    case CANCESTRY_FSM_EXPRESSION_ERR_TRAILING:
        return "trailing input";
    case CANCESTRY_FSM_EXPRESSION_ERR_DEPTH:
        return "nesting too deep";
    case CANCESTRY_FSM_EXPRESSION_ERR_NAMESPACE:
        return "namespace";
    case CANCESTRY_FSM_EXPRESSION_ERR_UNDEFINED:
        return "undefined identifier";
    case CANCESTRY_FSM_EXPRESSION_ERR_UNAUTHORIZED:
        return "unauthorized signal access";
    case CANCESTRY_FSM_EXPRESSION_ERR_TYPE:
        return "invalid type coercion";
    case CANCESTRY_FSM_EXPRESSION_ERR_OVERFLOW:
        return "integer overflow";
    case CANCESTRY_FSM_EXPRESSION_ERR_ZERO_DIVIDE:
        return "division by zero";
    case CANCESTRY_FSM_EXPRESSION_ERR_NOT_A_NUMBER:
        return "nan or infinity result";
    case CANCESTRY_FSM_EXPRESSION_ERR_FUNCTION:
        return "unknown function or bad arity";
    case CANCESTRY_FSM_EXPRESSION_ERR_DOMAIN:
        return "domain";
    case CANCESTRY_FSM_EXPRESSION_ERR_LENGTH:
        return "expression length";
    default:
        return "unknown";
    }
}

bool cancestry_fsm_expression_result_is_guard(const cancestry_value_t *result)
{
    return result != NULL && result->kind == CANCESTRY_VALUE_KIND_BOOL;
}
