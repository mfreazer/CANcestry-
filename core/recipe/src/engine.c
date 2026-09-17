/*
 * CANcestry - recipe execution engine.
 *
 * Implementation notes:
 *   - Zero heap allocation, no global mutable state (SYS-NF-002, SYS-NF-001).
 *     The engine struct and all stores live in caller-owned storage; the
 *     engine only borrows the recipe sets, the codec namespace, the
 *     environment tables and the sink.
 *   - Dispatch order is normative (event-ordering.md section 7): recipes run
 *     in engine set order (package load order, then recipe file order) and,
 *     within a set, in recipe definition order. Actions execute in
 *     declaration order. The same event sequence therefore always produces
 *     the same action sequence.
 *   - Governor policy (docs/system/governor.md, stub level): send_message and
 *     set_signal request approval before any side effect. A denial produces
 *     no side effect, increments the violation counter and fails the action,
 *     which then follows the recipe's on_error policy (SW-FR-RECIPE-003,
 *     SW-FR-GOV-005). With no governor configured, every request is denied
 *     (fail-closed, SW-FR-GOV-006).
 *   - The built-in expression evaluator implements the v0.2.1 expression
 *     language subset documented in core/recipe/README.md. It is deliberately
 *     replaceable through the engine configuration so the full v0.3.0
 *     evaluator can plug in later without touching the engine.
 */

#include "cancestry/recipe/engine.h"

#include "cancestry/codec/encoder.h"
#include "cancestry/recipe/types.h"

#include <float.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* ------------------------------------------------------------------------- */
/* Signal value store                                                        */
/* ------------------------------------------------------------------------- */

bool cancestry_recipe_signal_store_init(cancestry_recipe_signal_store_t *store,
                                        cancestry_recipe_signal_slot_t *slots,
                                        size_t capacity)
{
    if (store == NULL) {
        return false;
    }
    store->slots = slots;
    store->capacity = capacity;
    store->count = 0u;
    if (slots == NULL || capacity == 0u) {
        store->slots = NULL;
        store->capacity = 0u;
        return false;
    }
    memset(slots, 0, capacity * sizeof(*slots));
    return true;
}

bool cancestry_recipe_signal_store_is_valid(const cancestry_recipe_signal_store_t *store)
{
    return (store != NULL) && (store->slots != NULL) && (store->capacity > 0u) &&
           (store->count <= store->capacity);
}

size_t cancestry_recipe_signal_store_size(const cancestry_recipe_signal_store_t *store)
{
    if (!cancestry_recipe_signal_store_is_valid(store)) {
        return 0u;
    }
    return store->count;
}

cancestry_recipe_status_t cancestry_recipe_signal_store_get(
    const cancestry_recipe_signal_store_t *store,
    cancestry_signal_id_t signal_id,
    cancestry_value_t *value_out)
{
    size_t i;

    if (store == NULL || value_out == NULL) {
        return CANCESTRY_RECIPE_ERR_NULL;
    }
    if (!cancestry_recipe_signal_store_is_valid(store)) {
        return CANCESTRY_RECIPE_ERR_NULL;
    }
    for (i = 0u; i < store->count; ++i) {
        if (store->slots[i].signal_id == signal_id) {
            *value_out = store->slots[i].value;
            return CANCESTRY_RECIPE_OK;
        }
    }
    return CANCESTRY_RECIPE_ERR_NOT_FOUND;
}

cancestry_recipe_status_t cancestry_recipe_signal_store_set(cancestry_recipe_signal_store_t *store,
                                                            cancestry_signal_id_t signal_id,
                                                            const cancestry_value_t *value)
{
    size_t i;

    if (store == NULL || value == NULL) {
        return CANCESTRY_RECIPE_ERR_NULL;
    }
    if (!cancestry_recipe_signal_store_is_valid(store)) {
        return CANCESTRY_RECIPE_ERR_NULL;
    }
    for (i = 0u; i < store->count; ++i) {
        if (store->slots[i].signal_id == signal_id) {
            store->slots[i].value = *value;
            return CANCESTRY_RECIPE_OK;
        }
    }
    if (store->count >= store->capacity) {
        return CANCESTRY_RECIPE_ERR_CAPACITY;
    }
    store->slots[store->count].signal_id = signal_id;
    store->slots[store->count].value = *value;
    store->count++;
    return CANCESTRY_RECIPE_OK;
}

/* ------------------------------------------------------------------------- */
/* Fault code hash                                                           */
/* ------------------------------------------------------------------------- */

cancestry_fault_code_t cancestry_recipe_fault_code_hash(const char *code)
{
    uint32_t hash = 2166136261u; /* FNV-1a 32-bit offset basis */
    size_t i;

    if (code == NULL) {
        return 1u;
    }
    for (i = 0u; code[i] != '\0'; ++i) {
        hash ^= (uint32_t)(unsigned char)code[i];
        hash *= 16777619u; /* FNV-1a 32-bit prime; unsigned wrap is defined */
    }
    /* 0 is reserved for CANCESTRY_ID_NONE; remap it deterministically. */
    return (hash == 0u) ? 1u : (cancestry_fault_code_t)hash;
}

/* ------------------------------------------------------------------------- */
/* Name resolution                                                           */
/* ------------------------------------------------------------------------- */

/** @return true when @p text equals @p name, both non-NULL. */
static bool recipe_name_equals(const char *text, const char *name)
{
    if (text == NULL || name == NULL) {
        return false;
    }
    return strcmp(text, name) == 0;
}

/** Locate a message definition through the signal namespace. */
static cancestry_recipe_status_t recipe_resolve_message(
    const cancestry_recipe_engine_t *engine,
    const char *name,
    const cancestry_codec_map_t **map_out,
    const cancestry_codec_message_t **message_out)
{
    const cancestry_codec_message_t *found = NULL;
    const cancestry_codec_map_t *found_map = NULL;
    const char *dot;
    uint16_t i;

    if (engine == NULL || name == NULL) {
        return CANCESTRY_RECIPE_ERR_NULL;
    }
    if (engine->namespace == NULL || !cancestry_codec_namespace_is_valid(engine->namespace)) {
        return CANCESTRY_RECIPE_ERR_NOT_FOUND;
    }

    dot = strchr(name, '.');
    for (i = 0u; i < engine->namespace->count; ++i) {
        const cancestry_codec_map_t *candidate = engine->namespace->slots[i];
        uint16_t m;
        for (m = 0u; m < candidate->message_count; ++m) {
            const cancestry_codec_message_t *message = &candidate->messages[m];
            if (dot != NULL) {
                /* Canonical "<codec_map>.<message>" name: the map must match
                 * the part before the first dot and the message the rest. */
                size_t map_length = (size_t)(dot - name);
                if (strlen(candidate->name) != map_length ||
                    memcmp(candidate->name, name, map_length) != 0) {
                    continue;
                }
                if (!recipe_name_equals(message->name, dot + 1)) {
                    continue;
                }
            } else if (!recipe_name_equals(message->name, name)) {
                continue;
            }
            if (found != NULL) {
                /* A short name that matches more than one map is ambiguous,
                 * mirroring the signal namespace rule. */
                return CANCESTRY_RECIPE_ERR_AMBIGUOUS;
            }
            found = message;
            found_map = candidate;
        }
    }
    if (found == NULL) {
        return CANCESTRY_RECIPE_ERR_NOT_FOUND;
    }
    *map_out = found_map;
    *message_out = found;
    return CANCESTRY_RECIPE_OK;
}

/** Resolve a signal name through the signal namespace. */
static cancestry_recipe_status_t recipe_resolve_signal(const cancestry_recipe_engine_t *engine,
                                                       const char *name,
                                                       cancestry_codec_resolution_t *out)
{
    cancestry_codec_status_t status;

    if (engine == NULL || name == NULL || out == NULL) {
        return CANCESTRY_RECIPE_ERR_NULL;
    }
    if (engine->namespace == NULL || !cancestry_codec_namespace_is_valid(engine->namespace)) {
        return CANCESTRY_RECIPE_ERR_NOT_FOUND;
    }
    status = cancestry_codec_namespace_resolve(engine->namespace, name, out);
    switch (status) {
    case CANCESTRY_CODEC_OK:
        return CANCESTRY_RECIPE_OK;
    case CANCESTRY_CODEC_ERR_AMBIGUOUS:
        return CANCESTRY_RECIPE_ERR_AMBIGUOUS;
    default:
        return CANCESTRY_RECIPE_ERR_NOT_FOUND;
    }
}

/**
 * Resolve an interface name to its physical name and id.
 *
 * Resolution order (package-spec.md section 5): physical interface name
 * first, then package-level alias binding. Unknown names fail closed.
 */
static bool recipe_resolve_interface(const cancestry_recipe_engine_t *engine,
                                     const char *name,
                                     cancestry_interface_id_t *id_out,
                                     const char **physical_name_out)
{
    size_t i;

    if (engine == NULL || name == NULL || id_out == NULL || physical_name_out == NULL) {
        return false;
    }
    /* 1. Physical interface name in the interface table. */
    for (i = 0u; i < engine->interface_count; ++i) {
        if (recipe_name_equals(engine->interfaces[i].name, name)) {
            *id_out = engine->interfaces[i].id;
            *physical_name_out = engine->interfaces[i].name;
            return true;
        }
    }
    /* 2. Package-level alias binding. */
    for (i = 0u; i < engine->binding_count; ++i) {
        if (recipe_name_equals(engine->bindings[i].alias, name)) {
            const char *physical = engine->bindings[i].physical;
            size_t j;
            for (j = 0u; j < engine->interface_count; ++j) {
                if (recipe_name_equals(engine->interfaces[j].name, physical)) {
                    *id_out = engine->interfaces[j].id;
                    *physical_name_out = engine->interfaces[j].name;
                    return true;
                }
            }
            return false; /* alias points at an unknown physical interface */
        }
    }
    return false; /* 3. invalid */
}

/** Resolve a timer name to its id through the timer table. */
static bool recipe_resolve_timer(const cancestry_recipe_engine_t *engine,
                                 const char *name,
                                 cancestry_timer_id_t *id_out)
{
    size_t i;

    if (engine == NULL || name == NULL || id_out == NULL) {
        return false;
    }
    for (i = 0u; i < engine->timer_count; ++i) {
        if (recipe_name_equals(engine->timers[i].name, name)) {
            *id_out = engine->timers[i].id;
            return true;
        }
    }
    return false;
}

/** Find a recipe-local variable slot. */
static cancestry_recipe_variable_t *recipe_find_variable(const cancestry_recipe_engine_t *engine,
                                                         const cancestry_recipe_t *recipe,
                                                         const char *name)
{
    size_t i;

    for (i = 0u; i < engine->variable_count; ++i) {
        if (engine->variables[i].recipe == recipe &&
            recipe_name_equals(engine->variables[i].name, name)) {
            return &engine->variables[i];
        }
    }
    return NULL;
}

/* ------------------------------------------------------------------------- */
/* Built-in expression evaluator (v0.3.0 stub)                               */
/* ------------------------------------------------------------------------- */

/*
 * Recursive-descent evaluator for the documented subset of
 * docs/system/expression-language.md: literals, sig.NAME, var.NAME and
 * evt.FIELD references, parentheses, unary minus, mul/div/mod, add/sub,
 * comparisons, not/and/or. No function
 * calls yet. Operates in place over the expression text; no allocation.
 *
 * Failure rules (spec section 5): integer overflow, division or modulo by
 * zero, NaN/Infinity results, invalid type coercion and undefined
 * identifiers are expression faults. and/or/not require boolean operands
 * (spec section 6); both sides are always evaluated (no short-circuit in the
 * stub; documented in core/recipe/README.md).
 */

typedef struct recipe_expr_parser {
    const char *text;
    size_t pos;
    const cancestry_recipe_expression_context_t *ctx;
    bool failed;
} recipe_expr_parser_t;

static void recipe_expr_skip_ws(recipe_expr_parser_t *parser)
{
    while (parser->text[parser->pos] == ' ' || parser->text[parser->pos] == '\t') {
        parser->pos++;
    }
}

static char recipe_expr_peek(recipe_expr_parser_t *parser)
{
    recipe_expr_skip_ws(parser);
    return parser->text[parser->pos];
}

static bool recipe_expr_is_ident_char(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
           (c == '_') || (c == '.');
}

static bool recipe_expr_accept_char(recipe_expr_parser_t *parser, char expected)
{
    if (recipe_expr_peek(parser) == expected) {
        parser->pos++;
        return true;
    }
    return false;
}

/** Accept a word operator ("and", "or", "not") not followed by ident chars. */
static bool recipe_expr_accept_word(recipe_expr_parser_t *parser, const char *word)
{
    size_t i;

    recipe_expr_skip_ws(parser);
    for (i = 0u; word[i] != '\0'; ++i) {
        if (parser->text[parser->pos + i] != word[i]) {
            return false;
        }
    }
    if (recipe_expr_is_ident_char(parser->text[parser->pos + i])) {
        return false;
    }
    parser->pos += i;
    return true;
}

/** Convert an event-model value into an expression value. */
static bool recipe_expr_from_value(const cancestry_value_t *in, cancestry_value_t *out)
{
    memset(out, 0, sizeof(*out));
    switch (in->kind) {
    case CANCESTRY_VALUE_KIND_BOOL:
    case CANCESTRY_VALUE_KIND_INT:
    case CANCESTRY_VALUE_KIND_REAL:
        *out = *in;
        return true;
    case CANCESTRY_VALUE_KIND_UINT:
        if (in->value.unsigned_integer <= (uint64_t)INT64_MAX) {
            out->kind = CANCESTRY_VALUE_KIND_INT;
            out->value.integer = (int64_t)in->value.unsigned_integer;
        } else {
            out->kind = CANCESTRY_VALUE_KIND_REAL;
            out->value.real = (double)in->value.unsigned_integer;
        }
        return true;
    default:
        return false;
    }
}

/** Convert an unsigned event field into an expression value. */
static cancestry_value_t recipe_expr_from_u64(uint64_t value)
{
    cancestry_value_t out;

    memset(&out, 0, sizeof(out));
    if (value <= (uint64_t)INT64_MAX) {
        out.kind = CANCESTRY_VALUE_KIND_INT;
        out.value.integer = (int64_t)value;
    } else {
        out.kind = CANCESTRY_VALUE_KIND_REAL;
        out.value.real = (double)value;
    }
    return out;
}

/** Resolve sig.NAME / var.NAME / evt.FIELD for the current invocation. */
static bool recipe_expr_resolve_name(recipe_expr_parser_t *parser,
                                     const char *name,
                                     size_t length,
                                     cancestry_value_t *out)
{
    const cancestry_recipe_expression_context_t *ctx = parser->ctx;
    char buffer[CANCESTRY_RECIPE_EXPRESSION_MAX + 1u];
    const char *dot;
    const char *rest;

    if (length == 0u || length > CANCESTRY_RECIPE_EXPRESSION_MAX) {
        parser->failed = true;
        return false;
    }
    memcpy(buffer, name, length);
    buffer[length] = '\0';

    dot = strchr(buffer, '.');
    if (dot == NULL || dot == buffer || dot[1] == '\0') {
        /* Expression-language.md section 1: bare identifiers are not
         * permitted; namespaced identifiers are "ns.name". */
        parser->failed = true;
        return false;
    }
    rest = dot + 1;

    if ((size_t)(dot - buffer) == 3u && memcmp(buffer, "sig", 3u) == 0) {
        cancestry_codec_resolution_t resolution;
        cancestry_value_t current;

        if (ctx->namespace == NULL) {
            parser->failed = true;
            return false;
        }
        {
            cancestry_codec_status_t codec_status =
                cancestry_codec_namespace_resolve(ctx->namespace, rest, &resolution);
            if (codec_status != CANCESTRY_CODEC_OK) {
                parser->failed = true;
                return false;
            }
        }
        /* The triggering signal's freshest value is the event payload. */
        if (ctx->event != NULL && ctx->event->type == CANCESTRY_EVENT_TYPE_SIGNAL_CHANGED &&
            ctx->event->payload.signal_changed.signal_id == resolution.signal_id) {
            return recipe_expr_from_value(&ctx->event->payload.signal_changed.new_value, out);
        }
        if (ctx->signals == NULL ||
            cancestry_recipe_signal_store_get(ctx->signals, resolution.signal_id, &current) !=
                CANCESTRY_RECIPE_OK) {
            parser->failed = true; /* undefined identifier */
            return false;
        }
        return recipe_expr_from_value(&current, out);
    }

    if ((size_t)(dot - buffer) == 3u && memcmp(buffer, "var", 3u) == 0) {
        size_t i;

        for (i = 0u; i < ctx->variable_count; ++i) {
            if (ctx->variables[i].recipe == ctx->recipe &&
                recipe_name_equals(ctx->variables[i].name, rest)) {
                return recipe_expr_from_value(&ctx->variables[i].value, out);
            }
        }
        parser->failed = true; /* undefined identifier */
        return false;
    }

    if ((size_t)(dot - buffer) == 3u && memcmp(buffer, "evt", 3u) == 0) {
        const cancestry_event_t *event = ctx->event;

        if (event == NULL) {
            parser->failed = true;
            return false;
        }
        if (strcmp(rest, "timestamp_us") == 0) {
            *out = recipe_expr_from_u64((uint64_t)event->timestamp_us);
            return true;
        }
        if (strcmp(rest, "sequence") == 0) {
            *out = recipe_expr_from_u64((uint64_t)event->sequence);
            return true;
        }
        if (strcmp(rest, "cause_sequence") == 0) {
            *out = recipe_expr_from_u64((uint64_t)event->cause_sequence);
            return true;
        }
        if (strcmp(rest, "can_id") == 0 || strcmp(rest, "interface_id") == 0) {
            if (event->type != CANCESTRY_EVENT_TYPE_CAN_RX) {
                parser->failed = true;
                return false;
            }
            if (rest[0] == 'c') {
                *out = recipe_expr_from_u64((uint64_t)event->payload.can_rx.can_id);
            } else {
                *out = recipe_expr_from_u64((uint64_t)event->payload.can_rx.interface_id);
            }
            return true;
        }
        if (strcmp(rest, "signal_value") == 0) {
            if (event->type != CANCESTRY_EVENT_TYPE_SIGNAL_CHANGED) {
                parser->failed = true;
                return false;
            }
            return recipe_expr_from_value(&event->payload.signal_changed.new_value, out);
        }
        if (strcmp(rest, "fault_code") == 0) {
            if (event->type != CANCESTRY_EVENT_TYPE_FAULT_RAISED) {
                parser->failed = true;
                return false;
            }
            *out = recipe_expr_from_u64((uint64_t)event->payload.fault.fault_code);
            return true;
        }
        if (strcmp(rest, "missed_count") == 0) {
            if (event->type != CANCESTRY_EVENT_TYPE_TIMER_EXPIRED) {
                parser->failed = true;
                return false;
            }
            *out = recipe_expr_from_u64((uint64_t)event->payload.timer_expired.missed_count);
            return true;
        }
        if (strcmp(rest, "to_mode") == 0) {
            if (event->type != CANCESTRY_EVENT_TYPE_POWER_MODE_CHANGED) {
                parser->failed = true;
                return false;
            }
            *out = recipe_expr_from_u64((uint64_t)event->payload.mode.to_mode);
            return true;
        }
        parser->failed = true; /* unknown evt field */
        return false;
    }

    parser->failed = true; /* unknown namespace */
    return false;
}

static bool recipe_expr_parse_or(recipe_expr_parser_t *parser, cancestry_value_t *out);

static bool recipe_expr_parse_primary(recipe_expr_parser_t *parser, cancestry_value_t *out)
{
    char c = recipe_expr_peek(parser);

    memset(out, 0, sizeof(*out));
    if (c == '(') {
        parser->pos++;
        if (!recipe_expr_parse_or(parser, out)) {
            return false;
        }
        if (!recipe_expr_accept_char(parser, ')')) {
            parser->failed = true;
            return false;
        }
        return true;
    }
    if (c >= '0' && c <= '9') {
        /* Number: decimal integer or float with '.' and/or exponent. */
        uint64_t magnitude = 0u;
        bool overflow = false;
        bool is_real = false;
        double real_value = 0.0;

        while (parser->text[parser->pos] >= '0' && parser->text[parser->pos] <= '9') {
            char digit = parser->text[parser->pos];
            if (magnitude > (UINT64_MAX - (uint64_t)(digit - '0')) / 10u) {
                overflow = true;
            } else if (!overflow) {
                magnitude = (magnitude * 10u) + (uint64_t)(digit - '0');
            }
            real_value = (real_value * 10.0) + (double)(digit - '0');
            parser->pos++;
        }
        if (parser->text[parser->pos] == '.') {
            double scale = 0.1;
            is_real = true;
            parser->pos++;
            if (!(parser->text[parser->pos] >= '0' && parser->text[parser->pos] <= '9')) {
                parser->failed = true;
                return false;
            }
            while (parser->text[parser->pos] >= '0' && parser->text[parser->pos] <= '9') {
                real_value += (double)(parser->text[parser->pos] - '0') * scale;
                scale /= 10.0;
                parser->pos++;
            }
        }
        if (parser->text[parser->pos] == 'e' || parser->text[parser->pos] == 'E') {
            double exponent = 0.0;
            bool negative = false;
            double multiplier = 1.0;
            int steps;

            is_real = true;
            parser->pos++;
            if (parser->text[parser->pos] == '+' || parser->text[parser->pos] == '-') {
                negative = (parser->text[parser->pos] == '-');
                parser->pos++;
            }
            if (!(parser->text[parser->pos] >= '0' && parser->text[parser->pos] <= '9')) {
                parser->failed = true;
                return false;
            }
            while (parser->text[parser->pos] >= '0' && parser->text[parser->pos] <= '9') {
                exponent = (exponent * 10.0) + (double)(parser->text[parser->pos] - '0');
                parser->pos++;
            }
            if (exponent > 308.0) {
                parser->failed = true; /* would overflow to infinity */
                return false;
            }
            for (steps = 0; steps < (int)exponent; ++steps) {
                multiplier *= 10.0;
            }
            real_value = negative ? real_value / multiplier : real_value * multiplier;
        }
        if (is_real || overflow) {
            out->kind = CANCESTRY_VALUE_KIND_REAL;
            out->value.real = real_value;
            if (!(real_value >= -DBL_MAX && real_value <= DBL_MAX)) {
                parser->failed = true;
                return false;
            }
            return true;
        }
        out->kind = CANCESTRY_VALUE_KIND_INT;
        out->value.integer = (int64_t)magnitude; /* fits: no overflow and <= INT64_MAX */
        if (magnitude > (uint64_t)INT64_MAX) {
            out->kind = CANCESTRY_VALUE_KIND_REAL;
            out->value.real = real_value;
        }
        return true;
    }
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_') {
        size_t start = parser->pos;
        size_t length;
        char first = parser->text[parser->pos];

        while (recipe_expr_is_ident_char(parser->text[parser->pos])) {
            parser->pos++;
        }
        length = parser->pos - start;
        if (length == 4u && (first == 't' || first == 'T') &&
            memcmp(parser->text + start, "true", 4u) == 0) {
            out->kind = CANCESTRY_VALUE_KIND_BOOL;
            out->value.boolean = true;
            return true;
        }
        if (length == 5u && (first == 'f' || first == 'F') &&
            memcmp(parser->text + start, "false", 5u) == 0) {
            out->kind = CANCESTRY_VALUE_KIND_BOOL;
            out->value.boolean = false;
            return true;
        }
        return recipe_expr_resolve_name(parser, parser->text + start, length, out);
    }
    parser->failed = true;
    return false;
}

static bool recipe_expr_parse_unary(recipe_expr_parser_t *parser, cancestry_value_t *out)
{
    if (recipe_expr_accept_char(parser, '-')) {
        if (!recipe_expr_parse_unary(parser, out)) {
            return false;
        }
        if (out->kind == CANCESTRY_VALUE_KIND_INT) {
            if (out->value.integer == INT64_MIN) {
                parser->failed = true; /* integer overflow */
                return false;
            }
            out->value.integer = -out->value.integer;
            return true;
        }
        if (out->kind == CANCESTRY_VALUE_KIND_REAL) {
            out->value.real = -out->value.real;
            return true;
        }
        parser->failed = true; /* invalid coercion */
        return false;
    }
    return recipe_expr_parse_primary(parser, out);
}

static bool recipe_expr_parse_mul(recipe_expr_parser_t *parser, cancestry_value_t *out)
{
    if (!recipe_expr_parse_unary(parser, out)) {
        return false;
    }
    for (;;) {
        char op = recipe_expr_peek(parser);

        if (op != '*' && op != '/' && op != '%') {
            return true;
        }
        parser->pos++;
        {
            cancestry_value_t rhs;

            if (!recipe_expr_parse_unary(parser, &rhs)) {
                return false;
            }
            if (out->kind == CANCESTRY_VALUE_KIND_REAL || rhs.kind == CANCESTRY_VALUE_KIND_REAL) {
                double lhs_real =
                    (out->kind == CANCESTRY_VALUE_KIND_REAL)
                        ? out->value.real
                        : (double)out->value.integer;
                double rhs_real = (rhs.kind == CANCESTRY_VALUE_KIND_REAL)
                                      ? rhs.value.real
                                      : (double)rhs.value.integer;

                if (out->kind == CANCESTRY_VALUE_KIND_BOOL ||
                    rhs.kind == CANCESTRY_VALUE_KIND_BOOL) {
                    parser->failed = true; /* invalid coercion */
                    return false;
                }
                if (op == '*') {
                    lhs_real *= rhs_real;
                } else if (op == '/') {
                    if (rhs_real == 0.0) {
                        parser->failed = true; /* division by zero */
                        return false;
                    }
                    lhs_real /= rhs_real;
                } else {
                    parser->failed = true; /* modulo is not defined for floats */
                    return false;
                }
                if (!(lhs_real >= -DBL_MAX && lhs_real <= DBL_MAX)) {
                    parser->failed = true; /* NaN or infinity */
                    return false;
                }
                out->kind = CANCESTRY_VALUE_KIND_REAL;
                out->value.real = lhs_real;
            } else if (out->kind == CANCESTRY_VALUE_KIND_INT && rhs.kind == CANCESTRY_VALUE_KIND_INT) {
                int64_t lhs_int = out->value.integer;
                int64_t rhs_int = rhs.value.integer;

                if (op == '*') {
                    /* Overflow check via 64-bit magnitudes. */
                    uint64_t limit = (uint64_t)INT64_MAX + 1u;
                    uint64_t lhs_mag = (lhs_int < 0)
                                           ? (uint64_t)(-(lhs_int + 1)) + 1u
                                           : (uint64_t)lhs_int;
                    uint64_t rhs_mag =
                        (rhs_int < 0) ? (uint64_t)(-(rhs_int + 1)) + 1u : (uint64_t)rhs_int;
                    uint64_t result_mag;
                    bool negative = (lhs_int < 0) != (rhs_int < 0);

                    if (lhs_mag != 0u && rhs_mag != 0u && lhs_mag > limit / rhs_mag) {
                        parser->failed = true; /* integer overflow */
                        return false;
                    }
                    result_mag = lhs_mag * rhs_mag;
                    if (result_mag > limit) {
                        parser->failed = true; /* integer overflow */
                        return false;
                    }
                    if (negative) {
                        out->value.integer =
                            (result_mag == limit) ? INT64_MIN : -((int64_t)result_mag);
                    } else {
                        if (result_mag > (uint64_t)INT64_MAX) {
                            parser->failed = true; /* integer overflow */
                            return false;
                        }
                        out->value.integer = (int64_t)result_mag;
                    }
                } else if (op == '/') {
                    if (rhs_int == 0 || (lhs_int == INT64_MIN && rhs_int == -1)) {
                        parser->failed = true; /* division by zero or overflow */
                        return false;
                    }
                    out->value.integer = lhs_int / rhs_int;
                } else {
                    if (rhs_int == 0 || (lhs_int == INT64_MIN && rhs_int == -1)) {
                        parser->failed = true; /* modulo by zero or overflow */
                        return false;
                    }
                    out->value.integer = lhs_int % rhs_int;
                }
            } else {
                parser->failed = true; /* boolean operand in arithmetic */
                return false;
            }
        }
    }
}

static bool recipe_expr_parse_add(recipe_expr_parser_t *parser, cancestry_value_t *out)
{
    if (!recipe_expr_parse_mul(parser, out)) {
        return false;
    }
    for (;;) {
        char op = recipe_expr_peek(parser);

        if (op != '+' && op != '-') {
            return true;
        }
        parser->pos++;
        {
            cancestry_value_t rhs;

            if (!recipe_expr_parse_mul(parser, &rhs)) {
                return false;
            }
            if (out->kind == CANCESTRY_VALUE_KIND_REAL || rhs.kind == CANCESTRY_VALUE_KIND_REAL) {
                double lhs_real = (out->kind == CANCESTRY_VALUE_KIND_REAL)
                                      ? out->value.real
                                      : (double)out->value.integer;
                double rhs_real = (rhs.kind == CANCESTRY_VALUE_KIND_REAL)
                                      ? rhs.value.real
                                      : (double)rhs.value.integer;

                if (out->kind == CANCESTRY_VALUE_KIND_BOOL ||
                    rhs.kind == CANCESTRY_VALUE_KIND_BOOL) {
                    parser->failed = true; /* invalid coercion */
                    return false;
                }
                lhs_real = (op == '+') ? lhs_real + rhs_real : lhs_real - rhs_real;
                if (!(lhs_real >= -DBL_MAX && lhs_real <= DBL_MAX)) {
                    parser->failed = true; /* NaN or infinity */
                    return false;
                }
                out->kind = CANCESTRY_VALUE_KIND_REAL;
                out->value.real = lhs_real;
            } else if (out->kind == CANCESTRY_VALUE_KIND_INT && rhs.kind == CANCESTRY_VALUE_KIND_INT) {
                int64_t lhs_int = out->value.integer;
                int64_t rhs_int = rhs.value.integer;

                if (op == '+') {
                    if ((rhs_int > 0 && lhs_int > INT64_MAX - rhs_int) ||
                        (rhs_int < 0 && lhs_int < INT64_MIN - rhs_int)) {
                        parser->failed = true; /* integer overflow */
                        return false;
                    }
                    out->value.integer = lhs_int + rhs_int;
                } else {
                    if ((rhs_int < 0 && lhs_int > INT64_MAX + rhs_int) ||
                        (rhs_int > 0 && lhs_int < INT64_MIN + rhs_int)) {
                        parser->failed = true; /* integer overflow */
                        return false;
                    }
                    out->value.integer = lhs_int - rhs_int;
                }
            } else {
                parser->failed = true; /* boolean operand in arithmetic */
                return false;
            }
        }
    }
}

static bool recipe_expr_parse_cmp(recipe_expr_parser_t *parser, cancestry_value_t *out)
{
    if (!recipe_expr_parse_add(parser, out)) {
        return false;
    }
    {
        const char *op_text = NULL;
        size_t op_length = 0u;
        char c1 = recipe_expr_peek(parser);
        char c2;

        if (c1 != '=' && c1 != '!' && c1 != '<' && c1 != '>') {
            return true;
        }
        c2 = parser->text[parser->pos + 1u];
        if (c1 == '=' && c2 == '=') {
            op_text = "==";
            op_length = 2u;
        } else if (c1 == '!' && c2 == '=') {
            op_text = "!=";
            op_length = 2u;
        } else if (c1 == '<' && c2 == '=') {
            op_text = "<=";
            op_length = 2u;
        } else if (c1 == '>' && c2 == '=') {
            op_text = ">=";
            op_length = 2u;
        } else if (c1 == '<') {
            op_text = "<";
            op_length = 1u;
        } else if (c1 == '>') {
            op_text = ">";
            op_length = 1u;
        } else {
            parser->failed = true;
            return false;
        }
        parser->pos += op_length;
        {
            cancestry_value_t rhs;
            bool result;

            if (!recipe_expr_parse_add(parser, &rhs)) {
                return false;
            }
            if (out->kind == CANCESTRY_VALUE_KIND_BOOL && rhs.kind == CANCESTRY_VALUE_KIND_BOOL) {
                if (op_length != 2u) {
                    parser->failed = true; /* ordered comparison of booleans */
                    return false;
                }
                result = (op_text[0] == '=') ? (out->value.boolean == rhs.value.boolean)
                                             : (out->value.boolean != rhs.value.boolean);
            } else if (out->kind != CANCESTRY_VALUE_KIND_BOOL &&
                       rhs.kind != CANCESTRY_VALUE_KIND_BOOL) {
                double lhs_real = (out->kind == CANCESTRY_VALUE_KIND_REAL)
                                      ? out->value.real
                                      : (double)out->value.integer;
                double rhs_real = (rhs.kind == CANCESTRY_VALUE_KIND_REAL)
                                      ? rhs.value.real
                                      : (double)rhs.value.integer;

                if (op_text[0] == '=') {
                    result = lhs_real == rhs_real;
                } else if (op_text[0] == '!') {
                    result = lhs_real != rhs_real;
                } else if (op_text[0] == '<') {
                    result = (op_length == 2u) ? (lhs_real <= rhs_real) : (lhs_real < rhs_real);
                } else {
                    result = (op_length == 2u) ? (lhs_real >= rhs_real) : (lhs_real > rhs_real);
                }
            } else {
                parser->failed = true; /* invalid coercion */
                return false;
            }
            out->kind = CANCESTRY_VALUE_KIND_BOOL;
            out->value.boolean = result;
            return true;
        }
    }
}

static bool recipe_expr_parse_not(recipe_expr_parser_t *parser, cancestry_value_t *out)
{
    if (recipe_expr_accept_word(parser, "not")) {
        if (!recipe_expr_parse_not(parser, out)) {
            return false;
        }
        if (out->kind != CANCESTRY_VALUE_KIND_BOOL) {
            parser->failed = true; /* not requires a boolean */
            return false;
        }
        out->value.boolean = !out->value.boolean;
        return true;
    }
    return recipe_expr_parse_cmp(parser, out);
}

static bool recipe_expr_parse_and(recipe_expr_parser_t *parser, cancestry_value_t *out)
{
    if (!recipe_expr_parse_not(parser, out)) {
        return false;
    }
    while (recipe_expr_accept_word(parser, "and")) {
        cancestry_value_t rhs;

        if (!recipe_expr_parse_not(parser, &rhs)) {
            return false;
        }
        if (out->kind != CANCESTRY_VALUE_KIND_BOOL || rhs.kind != CANCESTRY_VALUE_KIND_BOOL) {
            parser->failed = true; /* and requires boolean operands */
            return false;
        }
        out->value.boolean = out->value.boolean && rhs.value.boolean;
    }
    return true;
}

static bool recipe_expr_parse_or(recipe_expr_parser_t *parser, cancestry_value_t *out)
{
    if (!recipe_expr_parse_and(parser, out)) {
        return false;
    }
    while (recipe_expr_accept_word(parser, "or")) {
        cancestry_value_t rhs;

        if (!recipe_expr_parse_and(parser, &rhs)) {
            return false;
        }
        if (out->kind != CANCESTRY_VALUE_KIND_BOOL || rhs.kind != CANCESTRY_VALUE_KIND_BOOL) {
            parser->failed = true; /* or requires boolean operands */
            return false;
        }
        out->value.boolean = out->value.boolean || rhs.value.boolean;
    }
    return true;
}

/** Built-in evaluator entry point. */
static bool recipe_expr_evaluate(const char *expression,
                                 const cancestry_recipe_expression_context_t *context,
                                 cancestry_value_t *result_out)
{
    recipe_expr_parser_t parser;

    if (expression == NULL || context == NULL || result_out == NULL) {
        return false;
    }
    memset(&parser, 0, sizeof(parser));
    parser.text = expression;
    parser.pos = 0u;
    parser.ctx = context;
    parser.failed = false;

    if (!recipe_expr_parse_or(&parser, result_out)) {
        return false;
    }
    recipe_expr_skip_ws(&parser);
    if (parser.text[parser.pos] != '\0' || parser.failed) {
        return false;
    }
    return true;
}

/* ------------------------------------------------------------------------- */
/* Engine lifecycle                                                          */
/* ------------------------------------------------------------------------- */

bool cancestry_recipe_engine_init(cancestry_recipe_engine_t *engine,
                                  const cancestry_recipe_engine_config_t *config)
{
    if (engine == NULL || config == NULL) {
        return false;
    }
    memset(engine, 0, sizeof(*engine));
    if (config->sets == NULL || config->set_count == 0u) {
        return false;
    }
    if (config->variable_storage == NULL && config->variable_capacity != 0u) {
        return false;
    }
    engine->sets = config->sets;
    engine->set_count = config->set_count;
    engine->namespace = config->signal_namespace;
    engine->interfaces = config->interfaces;
    engine->interface_count = config->interface_count;
    engine->bindings = config->bindings;
    engine->binding_count = config->binding_count;
    engine->timers = config->timers;
    engine->timer_count = config->timer_count;
    engine->signals = config->signal_store;
    engine->queue = config->event_queue;
    engine->sink = config->sink;
    engine->governor = config->governor;
    engine->governor_user_data = config->governor_user_data;
    engine->evaluator = config->evaluator;
    engine->evaluator_user_data = config->evaluator_user_data;
    engine->variables = config->variable_storage;
    engine->variable_capacity = config->variable_capacity;
    engine->variable_count = 0u;
    memset(&engine->counters, 0, sizeof(engine->counters));
    return true;
}

bool cancestry_recipe_engine_is_valid(const cancestry_recipe_engine_t *engine)
{
    return (engine != NULL) && (engine->sets != NULL) && (engine->set_count > 0u) &&
           (engine->variable_count <= engine->variable_capacity);
}

size_t cancestry_recipe_engine_recipe_count(const cancestry_recipe_engine_t *engine)
{
    size_t total = 0u;
    size_t i;

    if (!cancestry_recipe_engine_is_valid(engine)) {
        return 0u;
    }
    for (i = 0u; i < engine->set_count; ++i) {
        total += engine->sets[i].recipe_count;
    }
    return total;
}

const cancestry_recipe_engine_counters_t *cancestry_recipe_engine_counters(
    const cancestry_recipe_engine_t *engine)
{
    if (!cancestry_recipe_engine_is_valid(engine)) {
        return NULL;
    }
    return &engine->counters;
}

cancestry_recipe_status_t cancestry_recipe_engine_get_variable(
    const cancestry_recipe_engine_t *engine,
    const cancestry_recipe_t *recipe,
    const char *name,
    cancestry_value_t *value_out)
{
    const cancestry_recipe_variable_t *slot;

    if (engine == NULL || recipe == NULL || name == NULL || value_out == NULL) {
        return CANCESTRY_RECIPE_ERR_NULL;
    }
    if (!cancestry_recipe_engine_is_valid(engine)) {
        return CANCESTRY_RECIPE_ERR_NULL;
    }
    slot = recipe_find_variable(engine, recipe, name);
    if (slot == NULL) {
        return CANCESTRY_RECIPE_ERR_NOT_FOUND;
    }
    *value_out = slot->value;
    return CANCESTRY_RECIPE_OK;
}

/* ------------------------------------------------------------------------- */
/* Governor                                                                  */
/* ------------------------------------------------------------------------- */

/**
 * Request approval for a side effect. Fails closed when no governor is
 * configured (SW-FR-GOV-006). Every denial increments the violation counter.
 */
static bool recipe_governor_check(cancestry_recipe_engine_t *engine,
                                  const cancestry_recipe_governor_request_t *request)
{
    if (engine->governor == NULL ||
        engine->governor(engine->governor_user_data, request) !=
            CANCESTRY_RECIPE_GOVERNOR_APPROVE) {
        engine->counters.governor_denials++;
        return false;
    }
    return true;
}

/* ------------------------------------------------------------------------- */
/* Trigger matching and conditions                                           */
/* ------------------------------------------------------------------------- */

/**
 * Whether the trigger declares only filter fields that apply to its event
 * type. A filter that cannot apply (for example a message filter on a
 * fault_raised trigger) makes the trigger unmatchable: fail closed rather
 * than silently ignoring the author's filter.
 */
static bool recipe_trigger_filters_applicable(const cancestry_recipe_trigger_t *trigger)
{
    switch (cancestry_recipe_trigger_event_type(trigger->event)) {
    case CANCESTRY_EVENT_TYPE_CAN_RX:
        return trigger->signal == NULL && trigger->timer == NULL && !trigger->has_timeout_ms;
    case CANCESTRY_EVENT_TYPE_SIGNAL_CHANGED:
        return trigger->interface == NULL && trigger->message == NULL && trigger->timer == NULL &&
               !trigger->has_timeout_ms;
    case CANCESTRY_EVENT_TYPE_TIMER_EXPIRED:
        return trigger->interface == NULL && trigger->message == NULL && trigger->signal == NULL &&
               !trigger->has_timeout_ms;
    case CANCESTRY_EVENT_TYPE_FAULT_RAISED:
    case CANCESTRY_EVENT_TYPE_POWER_MODE_CHANGED:
        return trigger->interface == NULL && trigger->message == NULL && trigger->signal == NULL &&
               trigger->timer == NULL && !trigger->has_timeout_ms;
    default:
        return false; /* timeout triggers never match (no event type) */
    }
}

/** Whether a recipe's trigger matches an event (SW-FR-RECIPE-001, SW-FR-RECIPE-005). */
static bool recipe_trigger_matches(cancestry_recipe_engine_t *engine,
                                   const cancestry_recipe_t *recipe,
                                   const cancestry_event_t *event)
{
    const cancestry_recipe_trigger_t *trigger = &recipe->trigger;
    cancestry_event_type_t type = cancestry_recipe_trigger_event_type(trigger->event);

    if (type == CANCESTRY_EVENT_TYPE_INVALID || event->type != type) {
        return false;
    }
    if (!recipe_trigger_filters_applicable(trigger)) {
        return false;
    }

    switch (type) {
    case CANCESTRY_EVENT_TYPE_CAN_RX:
        if (trigger->interface != NULL) {
            cancestry_interface_id_t interface_id = 0u;
            const char *physical = NULL;

            if (!recipe_resolve_interface(engine, trigger->interface, &interface_id, &physical)) {
                engine->counters.trigger_unresolved++;
                return false;
            }
            if (event->payload.can_rx.interface_id != interface_id) {
                return false;
            }
        }
        if (trigger->message != NULL) {
            const cancestry_codec_map_t *map = NULL;
            const cancestry_codec_message_t *message = NULL;

            if (recipe_resolve_message(engine, trigger->message, &map, &message) !=
                CANCESTRY_RECIPE_OK) {
                engine->counters.trigger_unresolved++;
                return false;
            }
            if (event->payload.can_rx.can_id != message->id) {
                return false;
            }
        }
        return true;
    case CANCESTRY_EVENT_TYPE_SIGNAL_CHANGED:
        if (trigger->signal != NULL) {
            cancestry_codec_resolution_t resolution;

            if (recipe_resolve_signal(engine, trigger->signal, &resolution) !=
                CANCESTRY_RECIPE_OK) {
                engine->counters.trigger_unresolved++;
                return false;
            }
            if (event->payload.signal_changed.signal_id != resolution.signal_id) {
                return false;
            }
        }
        return true;
    case CANCESTRY_EVENT_TYPE_TIMER_EXPIRED:
        if (trigger->timer != NULL) {
            cancestry_timer_id_t timer_id = 0u;

            if (!recipe_resolve_timer(engine, trigger->timer, &timer_id)) {
                engine->counters.trigger_unresolved++;
                return false;
            }
            if (event->payload.timer_expired.timer_id != timer_id) {
                return false;
            }
        }
        return true;
    default:
        return true;
    }
}

/** Evaluate an expression with the configured (or built-in) evaluator. */
static bool recipe_evaluate_expression(cancestry_recipe_engine_t *engine,
                                       const cancestry_recipe_expression_context_t *context,
                                       const char *expression,
                                       cancestry_value_t *result_out)
{
    if (engine->evaluator != NULL) {
        return engine->evaluator(engine->evaluator_user_data, expression, context, result_out);
    }
    return recipe_expr_evaluate(expression, context, result_out);
}

/** Whether every condition of the recipe holds for the event (SW-FR-RECIPE-002). */
static bool recipe_conditions_hold(cancestry_recipe_engine_t *engine,
                                   const cancestry_recipe_t *recipe,
                                   const cancestry_event_t *event)
{
    cancestry_recipe_expression_context_t context;
    size_t i;

    if (recipe->condition_count == 0u) {
        return true;
    }
    context.recipe = recipe;
    context.event = event;
    context.namespace = engine->namespace;
    context.signals = engine->signals;
    context.variables = engine->variables;
    context.variable_count = engine->variable_count;

    for (i = 0u; i < recipe->condition_count; ++i) {
        cancestry_value_t result;

        engine->counters.conditions_evaluated++;
        if (!recipe_evaluate_expression(engine, &context, recipe->conditions[i].expression,
                                        &result)) {
            /* Expression faults fail closed: the recipe is skipped and the
             * fault is counted (see core/recipe/README.md). */
            engine->counters.condition_errors++;
            return false;
        }
        if (result.kind != CANCESTRY_VALUE_KIND_BOOL) {
            engine->counters.condition_errors++;
            return false;
        }
        if (!result.value.boolean) {
            return false;
        }
    }
    return true;
}

/** Evaluate an action operand (literal or expression). */
static cancestry_recipe_status_t recipe_evaluate_operand(
    cancestry_recipe_engine_t *engine,
    const cancestry_recipe_t *recipe,
    const cancestry_event_t *event,
    const cancestry_recipe_operand_t *operand,
    cancestry_value_t *value_out)
{
    cancestry_recipe_expression_context_t context;

    if (!operand->is_expression) {
        *value_out = operand->literal;
        return CANCESTRY_RECIPE_OK;
    }
    context.recipe = recipe;
    context.event = event;
    context.namespace = engine->namespace;
    context.signals = engine->signals;
    context.variables = engine->variables;
    context.variable_count = engine->variable_count;
    if (!recipe_evaluate_expression(engine, &context, operand->expression, value_out)) {
        return CANCESTRY_RECIPE_ERR_EXPRESSION;
    }
    return CANCESTRY_RECIPE_OK;
}

/** Exact value equality, used for set_signal change detection. */
static bool recipe_values_equal(const cancestry_value_t *lhs, const cancestry_value_t *rhs)
{
    if (lhs->kind != rhs->kind) {
        return false;
    }
    switch (lhs->kind) {
    case CANCESTRY_VALUE_KIND_BOOL:
        return lhs->value.boolean == rhs->value.boolean;
    case CANCESTRY_VALUE_KIND_INT:
        return lhs->value.integer == rhs->value.integer;
    case CANCESTRY_VALUE_KIND_UINT:
        return lhs->value.unsigned_integer == rhs->value.unsigned_integer;
    case CANCESTRY_VALUE_KIND_REAL:
        return lhs->value.real == rhs->value.real;
    default:
        return true; /* both unset */
    }
}

/* ------------------------------------------------------------------------- */
/* Action execution                                                          */
/* ------------------------------------------------------------------------- */

/** 2^63 as a double; the guard bound for int64 conversion (mirrors encoder.c). */
#define RECIPE_INT64_LIMIT_D (9223372036854775808.0)

/**
 * Round to nearest, ties away from zero, without libm (codec-map-spec.md
 * section 7; same technique as the codec encoder).
 */
static bool recipe_round_half_away(double q, int64_t *out)
{
    double rounded;

    if (!(q > -RECIPE_INT64_LIMIT_D && q < RECIPE_INT64_LIMIT_D)) {
        return false; /* outside the exact int64 range */
    }
    rounded = (q >= 0.0) ? (double)(int64_t)(q + 0.5) : (double)(int64_t)(q - 0.5);
    *out = (int64_t)rounded;
    return true;
}

/**
 * Coerce an evaluated recipe value into the kinds the codec encoder accepts
 * for the target signal type (core/codec/README.md):
 *
 *   uint unscaled: UINT        uint scaled: UINT or REAL
 *   int  unscaled: INT         int  scaled: INT or REAL
 *   boolean:       BOOL or INT/UINT equal to 0 or 1
 *   enum:          INT or UINT (non-negative)
 *
 * Recipe values are authored as generic numbers, so INT literals intended
 * for uint signals and REAL results on unscaled signals are adapted here:
 * REAL values on unscaled signals are rounded to nearest, ties away from
 * zero, mirroring the encoder's own physical-to-raw conversion for scale 1.
 * Coercion never guesses silently: impossible coercions fail closed.
 */
static cancestry_recipe_status_t recipe_coerce_for_signal(
    const cancestry_codec_signal_t *signal,
    const cancestry_value_t *in,
    cancestry_value_t *out)
{
    bool scaled = (signal->scale != 1.0 || signal->offset != 0.0);
    int64_t rounded;

    memset(out, 0, sizeof(*out));
    switch (signal->type) {
    case CANCESTRY_CODEC_SIGNAL_TYPE_UINT:
        if (in->kind == CANCESTRY_VALUE_KIND_UINT) {
            *out = *in;
            return CANCESTRY_RECIPE_OK;
        }
        if (in->kind == CANCESTRY_VALUE_KIND_INT) {
            if (in->value.integer < 0) {
                return CANCESTRY_RECIPE_ERR_ENCODING;
            }
            out->kind = CANCESTRY_VALUE_KIND_UINT;
            out->value.unsigned_integer = (uint64_t)in->value.integer;
            return CANCESTRY_RECIPE_OK;
        }
        if (in->kind == CANCESTRY_VALUE_KIND_REAL) {
            if (scaled) {
                *out = *in;
                return CANCESTRY_RECIPE_OK;
            }
            if (!recipe_round_half_away(in->value.real, &rounded) || rounded < 0) {
                return CANCESTRY_RECIPE_ERR_ENCODING;
            }
            out->kind = CANCESTRY_VALUE_KIND_UINT;
            out->value.unsigned_integer = (uint64_t)rounded;
            return CANCESTRY_RECIPE_OK;
        }
        return CANCESTRY_RECIPE_ERR_ENCODING;
    case CANCESTRY_CODEC_SIGNAL_TYPE_INT:
        if (in->kind == CANCESTRY_VALUE_KIND_INT) {
            *out = *in;
            return CANCESTRY_RECIPE_OK;
        }
        if (in->kind == CANCESTRY_VALUE_KIND_UINT) {
            if (in->value.unsigned_integer > (uint64_t)INT64_MAX) {
                return CANCESTRY_RECIPE_ERR_ENCODING;
            }
            out->kind = CANCESTRY_VALUE_KIND_INT;
            out->value.integer = (int64_t)in->value.unsigned_integer;
            return CANCESTRY_RECIPE_OK;
        }
        if (in->kind == CANCESTRY_VALUE_KIND_REAL) {
            if (scaled) {
                *out = *in;
                return CANCESTRY_RECIPE_OK;
            }
            if (!recipe_round_half_away(in->value.real, &rounded)) {
                return CANCESTRY_RECIPE_ERR_ENCODING;
            }
            out->kind = CANCESTRY_VALUE_KIND_INT;
            out->value.integer = rounded;
            return CANCESTRY_RECIPE_OK;
        }
        return CANCESTRY_RECIPE_ERR_ENCODING;
    case CANCESTRY_CODEC_SIGNAL_TYPE_BOOLEAN:
        if (in->kind == CANCESTRY_VALUE_KIND_BOOL) {
            *out = *in;
            return CANCESTRY_RECIPE_OK;
        }
        if (in->kind == CANCESTRY_VALUE_KIND_INT &&
            (in->value.integer == 0 || in->value.integer == 1)) {
            out->kind = CANCESTRY_VALUE_KIND_BOOL;
            out->value.boolean = (in->value.integer != 0);
            return CANCESTRY_RECIPE_OK;
        }
        return CANCESTRY_RECIPE_ERR_ENCODING;
    case CANCESTRY_CODEC_SIGNAL_TYPE_ENUM:
        if (in->kind == CANCESTRY_VALUE_KIND_INT) {
            if (in->value.integer < 0) {
                return CANCESTRY_RECIPE_ERR_ENCODING;
            }
            *out = *in;
            return CANCESTRY_RECIPE_OK;
        }
        if (in->kind == CANCESTRY_VALUE_KIND_UINT) {
            if (in->value.unsigned_integer > (uint64_t)INT64_MAX) {
                return CANCESTRY_RECIPE_ERR_ENCODING;
            }
            out->kind = CANCESTRY_VALUE_KIND_INT;
            out->value.integer = (int64_t)in->value.unsigned_integer;
            return CANCESTRY_RECIPE_OK;
        }
        return CANCESTRY_RECIPE_ERR_ENCODING;
    default:
        return CANCESTRY_RECIPE_ERR_ENCODING;
    }
}

static cancestry_recipe_status_t recipe_action_send_message(
    cancestry_recipe_engine_t *engine,
    const cancestry_recipe_invocation_t *invocation,
    const cancestry_recipe_action_t *action)
{
    const cancestry_recipe_t *recipe = invocation->recipe;
    const cancestry_event_t *event = invocation->event;
    const char *interface_name = NULL;
    cancestry_interface_id_t interface_id = 0u;
    const cancestry_codec_map_t *map = NULL;
    const cancestry_codec_message_t *message = NULL;
    uint8_t frame[CANCESTRY_CAN_FRAME_MAX_LENGTH];
    size_t span_bytes = 0u;
    size_t i;
    cancestry_recipe_status_t status;
    cancestry_recipe_governor_request_t request;

    if (!recipe_resolve_interface(engine, action->as.send_message.interface, &interface_id,
                                  &interface_name)) {
        return CANCESTRY_RECIPE_ERR_NOT_FOUND;
    }
    status = recipe_resolve_message(engine, action->as.send_message.message, &map, &message);
    if (status != CANCESTRY_RECIPE_OK) {
        return status;
    }
    /*
     * SW-FR-CANFD-006: the declarative egress path builds classic 8-byte
     * frames. A message from a CAN FD codec map needs a wider frame than this
     * path owns, so it is refused here rather than truncated on the way out
     * (fail closed, agents.md section 2). FD transmit goes through the HAL
     * (cancestry_hal_send_tx with is_fd set).
     */
    if ((size_t)message->dlc > (size_t)CANCESTRY_CAN_FRAME_MAX_LENGTH) {
        return CANCESTRY_RECIPE_ERR_UNSUPPORTED;
    }

    memset(frame, 0, sizeof(frame));
    for (i = 0u; i < action->as.send_message.value_count; ++i) {
        const cancestry_recipe_signal_value_t *spec = &action->as.send_message.values[i];
        const cancestry_codec_signal_t *signal = cancestry_codec_map_find_signal(map, spec->name);
        cancestry_value_t value;
        cancestry_value_t coerced;
        cancestry_codec_status_t codec_status;
        size_t signal_bytes;

        if (signal == NULL) {
            return CANCESTRY_RECIPE_ERR_NOT_FOUND;
        }
        status = recipe_evaluate_operand(engine, recipe, event, &spec->operand, &value);
        if (status != CANCESTRY_RECIPE_OK) {
            return status;
        }
        status = recipe_coerce_for_signal(signal, &value, &coerced);
        if (status != CANCESTRY_RECIPE_OK) {
            return status;
        }
        codec_status = cancestry_codec_encode_signal(signal, &coerced, frame, sizeof(frame),
                                                     &engine->counters.codec_warnings);
        if (codec_status < 0) {
            return CANCESTRY_RECIPE_ERR_ENCODING;
        }
        signal_bytes = ((size_t)signal->last_bit >> 3) + 1u;
        if (signal_bytes > span_bytes) {
            span_bytes = signal_bytes;
        }
    }
    /* Fail closed rather than transmitting a frame that would truncate a
     * declared signal (see core/recipe/README.md). */
    if (span_bytes > (size_t)message->dlc) {
        return CANCESTRY_RECIPE_ERR_ENCODING;
    }

    memset(&request, 0, sizeof(request));
    request.kind = CANCESTRY_RECIPE_GOVERNOR_SEND_MESSAGE;
    request.recipe = recipe;
    request.cause = event;
    request.interface_name = interface_name;
    request.interface_id = interface_id;
    request.message_name = action->as.send_message.message;
    request.can_id = message->id;
    request.frame = frame;
    request.frame_length = (uint8_t)message->dlc;
    if (!recipe_governor_check(engine, &request)) {
        return CANCESTRY_RECIPE_ERR_DENIED;
    }

    if (engine->sink == NULL || engine->sink->on_send_message == NULL) {
        return CANCESTRY_RECIPE_ERR_UNSUPPORTED;
    }
    engine->sink->on_send_message(engine->sink->user_data, invocation, interface_id,
                                  interface_name, message->id, frame, (uint8_t)message->dlc);
    engine->counters.messages_sent++;
    return CANCESTRY_RECIPE_OK;
}

static cancestry_recipe_status_t recipe_action_set_signal(
    cancestry_recipe_engine_t *engine,
    const cancestry_recipe_invocation_t *invocation,
    const cancestry_recipe_action_t *action)
{
    const cancestry_recipe_t *recipe = invocation->recipe;
    const cancestry_event_t *event = invocation->event;
    cancestry_codec_resolution_t resolution;
    cancestry_value_t value;
    cancestry_value_t old_value;
    bool had_old = false;
    cancestry_recipe_status_t status;
    cancestry_recipe_governor_request_t request;

    status = recipe_resolve_signal(engine, action->as.set_signal.signal, &resolution);
    if (status != CANCESTRY_RECIPE_OK) {
        return status;
    }
    status = recipe_evaluate_operand(engine, recipe, event, &action->as.set_signal.operand, &value);
    if (status != CANCESTRY_RECIPE_OK) {
        return status;
    }

    memset(&request, 0, sizeof(request));
    request.kind = CANCESTRY_RECIPE_GOVERNOR_SET_SIGNAL;
    request.recipe = recipe;
    request.cause = event;
    request.signal_name = action->as.set_signal.signal;
    request.signal_id = resolution.signal_id;
    request.value = &value;
    if (!recipe_governor_check(engine, &request)) {
        return CANCESTRY_RECIPE_ERR_DENIED;
    }

    if (engine->signals == NULL) {
        return CANCESTRY_RECIPE_ERR_UNSUPPORTED;
    }
    memset(&old_value, 0, sizeof(old_value));
    if (cancestry_recipe_signal_store_get(engine->signals, resolution.signal_id, &old_value) ==
        CANCESTRY_RECIPE_OK) {
        had_old = true;
    }
    status = cancestry_recipe_signal_store_set(engine->signals, resolution.signal_id, &value);
    if (status != CANCESTRY_RECIPE_OK) {
        return status;
    }

    /* Only changed signals generate signal_changed events, mirroring the
     * decode pipeline rule (event-ordering.md section 6). */
    if (!had_old || !recipe_values_equal(&old_value, &value)) {
        if (engine->queue != NULL && cancestry_event_queue_is_valid(engine->queue)) {
            cancestry_event_payload_t payload;

            memset(&payload, 0, sizeof(payload));
            payload.signal_changed.signal_id = resolution.signal_id;
            payload.signal_changed.signal_name = resolution.signal->name;
            payload.signal_changed.old_value = old_value;
            payload.signal_changed.new_value = value;
            (void)cancestry_event_queue_emit_generated(engine->queue, event,
                                                       CANCESTRY_EVENT_TYPE_SIGNAL_CHANGED,
                                                       &payload, 0u);
        }
        if (engine->sink != NULL && engine->sink->on_signal_set != NULL) {
            engine->sink->on_signal_set(engine->sink->user_data, invocation,
                                        resolution.signal_id, resolution.signal->name, &old_value,
                                        &value);
        }
    }
    engine->counters.signals_set++;
    return CANCESTRY_RECIPE_OK;
}

static cancestry_recipe_status_t recipe_action_set_variable(
    cancestry_recipe_engine_t *engine,
    const cancestry_recipe_invocation_t *invocation,
    const cancestry_recipe_action_t *action)
{
    const cancestry_recipe_t *recipe = invocation->recipe;
    cancestry_value_t value;
    cancestry_recipe_status_t status;
    cancestry_recipe_variable_t *slot;

    status = recipe_evaluate_operand(engine, recipe, invocation->event,
                                     &action->as.set_variable.operand, &value);
    if (status != CANCESTRY_RECIPE_OK) {
        return status;
    }
    slot = recipe_find_variable(engine, recipe, action->as.set_variable.variable);
    if (slot != NULL) {
        slot->value = value;
    } else {
        if (engine->variable_count >= engine->variable_capacity) {
            return CANCESTRY_RECIPE_ERR_CAPACITY;
        }
        engine->variables[engine->variable_count].recipe = recipe;
        engine->variables[engine->variable_count].name = action->as.set_variable.variable;
        engine->variables[engine->variable_count].value = value;
        engine->variable_count++;
    }
    engine->counters.variables_set++;
    return CANCESTRY_RECIPE_OK;
}

static cancestry_recipe_status_t recipe_action_timer(cancestry_recipe_engine_t *engine,
                                                     const cancestry_recipe_invocation_t *invocation,
                                                     const cancestry_recipe_action_t *action)
{
    const char *timer_name;
    uint32_t duration_ms = 0u;
    bool repeat = false;

    if (engine->sink == NULL || engine->sink->on_timer == NULL) {
        return CANCESTRY_RECIPE_ERR_UNSUPPORTED;
    }
    if (action->kind == CANCESTRY_RECIPE_ACTION_START_TIMER) {
        timer_name = action->as.start_timer.timer;
        duration_ms = action->as.start_timer.duration_ms;
        repeat = action->as.start_timer.repeat;
    } else {
        timer_name = action->as.timer.timer;
    }
    engine->sink->on_timer(engine->sink->user_data, invocation, action->kind, timer_name,
                           duration_ms, repeat);
    engine->counters.timers_requested++;
    return CANCESTRY_RECIPE_OK;
}

static cancestry_recipe_status_t recipe_action_log(cancestry_recipe_engine_t *engine,
                                                   const cancestry_recipe_invocation_t *invocation,
                                                   const cancestry_recipe_action_t *action)
{
    if (engine->sink == NULL || engine->sink->on_log == NULL) {
        return CANCESTRY_RECIPE_ERR_UNSUPPORTED;
    }
    engine->sink->on_log(engine->sink->user_data, invocation, action->as.log.level,
                         action->as.log.message);
    engine->counters.logs_emitted++;
    return CANCESTRY_RECIPE_OK;
}

static cancestry_recipe_status_t recipe_action_raise_fault(
    cancestry_recipe_engine_t *engine,
    const cancestry_recipe_invocation_t *invocation,
    const cancestry_recipe_action_t *action)
{
    cancestry_fault_code_t numeric_code = cancestry_recipe_fault_code_hash(action->as.raise_fault.code);
    bool emitted = false;

    if (engine->queue != NULL && cancestry_event_queue_is_valid(engine->queue)) {
        cancestry_event_t fault_event;
        cancestry_event_queue_status_t status;

        cancestry_event_init(&fault_event);
        fault_event.type = CANCESTRY_EVENT_TYPE_FAULT_RAISED;
        fault_event.priority_class = CANCESTRY_PRIORITY_CLASS_FAULT;
        fault_event.timestamp_us = invocation->event->timestamp_us;
        fault_event.cause_sequence = invocation->event->sequence;
        fault_event.payload.fault.fault_code = numeric_code;
        fault_event.payload.fault.severity = action->as.raise_fault.severity;
        fault_event.payload.fault.source_id = invocation->recipe_ordinal;
        status = cancestry_event_queue_push(engine->queue, &fault_event);
        if (cancestry_event_queue_status_is_ok(status)) {
            emitted = true;
        }
    }
    if (engine->sink != NULL && engine->sink->on_fault != NULL) {
        engine->sink->on_fault(engine->sink->user_data, invocation, action->as.raise_fault.code,
                               action->as.raise_fault.severity, numeric_code);
        emitted = true;
    }
    if (!emitted) {
        /* Fail closed: the fault would be lost entirely. */
        return CANCESTRY_RECIPE_ERR_UNSUPPORTED;
    }
    engine->counters.faults_raised++;
    return CANCESTRY_RECIPE_OK;
}

static cancestry_recipe_status_t recipe_execute_action(cancestry_recipe_engine_t *engine,
                                                       const cancestry_recipe_invocation_t *invocation,
                                                       const cancestry_recipe_action_t *action)
{
    switch (action->kind) {
    case CANCESTRY_RECIPE_ACTION_SEND_MESSAGE:
        return recipe_action_send_message(engine, invocation, action);
    case CANCESTRY_RECIPE_ACTION_SET_SIGNAL:
        return recipe_action_set_signal(engine, invocation, action);
    case CANCESTRY_RECIPE_ACTION_SET_VARIABLE:
        return recipe_action_set_variable(engine, invocation, action);
    case CANCESTRY_RECIPE_ACTION_START_TIMER:
    case CANCESTRY_RECIPE_ACTION_STOP_TIMER:
    case CANCESTRY_RECIPE_ACTION_RESET_TIMER:
        return recipe_action_timer(engine, invocation, action);
    case CANCESTRY_RECIPE_ACTION_LOG:
        return recipe_action_log(engine, invocation, action);
    case CANCESTRY_RECIPE_ACTION_RAISE_FAULT:
        return recipe_action_raise_fault(engine, invocation, action);
    default:
        return CANCESTRY_RECIPE_ERR_ARGUMENT;
    }
}

/* ------------------------------------------------------------------------- */
/* Event processing                                                          */
/* ------------------------------------------------------------------------- */

cancestry_recipe_status_t cancestry_recipe_engine_process_event(cancestry_recipe_engine_t *engine,
                                                                const cancestry_event_t *event)
{
    size_t s;
    uint32_t ordinal = 0u;

    if (engine == NULL) {
        return CANCESTRY_RECIPE_ERR_NULL;
    }
    if (!cancestry_recipe_engine_is_valid(engine)) {
        return CANCESTRY_RECIPE_ERR_NULL;
    }
    if (event == NULL) {
        return CANCESTRY_RECIPE_ERR_NULL;
    }
    if (!cancestry_event_is_valid(event)) {
        return CANCESTRY_RECIPE_ERR_ARGUMENT;
    }
    engine->counters.events_processed++;

    /* Normative dispatch order: set order (package load order, recipe file
     * order), then recipe definition order within each set
     * (event-ordering.md section 7). */
    for (s = 0u; s < engine->set_count; ++s) {
        const cancestry_recipe_set_t *set = &engine->sets[s];
        uint16_t r;

        for (r = 0u; r < set->recipe_count; ++r) {
            const cancestry_recipe_t *recipe = &set->recipes[r];
            cancestry_recipe_invocation_t invocation;
            uint16_t a;

            ordinal++;
            if (!recipe->enabled) {
                continue;
            }
            if (!recipe_trigger_matches(engine, recipe, event)) {
                continue;
            }
            engine->counters.recipes_invoked++;
            if (!recipe_conditions_hold(engine, recipe, event)) {
                engine->counters.recipes_skipped_conditions++;
                continue;
            }

            invocation.engine = engine;
            invocation.recipe = recipe;
            invocation.recipe_ordinal = ordinal;
            invocation.event = event;
            for (a = 0u; a < recipe->action_count; ++a) {
                cancestry_recipe_status_t status;

                invocation.action = &recipe->actions[a];
                status = recipe_execute_action(engine, &invocation, &recipe->actions[a]);
                if (status == CANCESTRY_RECIPE_OK) {
                    engine->counters.actions_executed++;
                    continue;
                }
                engine->counters.action_errors++;
                if (recipe->on_error == CANCESTRY_RECIPE_ON_ERROR_STOP) {
                    engine->counters.recipes_halted++;
                    break;
                }
                /* on_error: continue executes the remaining actions. */
            }
        }
    }
    return CANCESTRY_RECIPE_OK;
}
