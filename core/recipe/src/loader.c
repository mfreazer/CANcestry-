/*
 * CANcestry - recipe file loader.
 *
 * Implementation notes:
 *   - The loader speaks the same strict, documented YAML subset as the codec
 *     map loader (see core/recipe/README.md): block mappings and block
 *     sequences only, no flow collections, anchors, aliases, tags, directives
 *     or multi-line scalars. Anything outside the subset is rejected with a
 *     line/column error instead of being guessed at. The parser is
 *     deliberately a copy of the proven codec loader parser so the recipe
 *     module stays self-contained; consolidating the two parsers is a
 *     recorded follow-up (see README.md).
 *   - Validation implements every constraint of
 *     schemas/recipe-0.2.0.schema.json in C (required fields, field types,
 *     ranges, enums, additionalProperties, oneOf action keys, conditional
 *     trigger rules), so no external JSON Schema validator runs at load
 *     time. Recipes that use the forbidden transition action are rejected
 *     with a dedicated error (SW-FR-RECIPE-006).
 *   - Parsing works in two passes over an AST held in a scratch arena:
 *     first the AST is built and validated, then one block is allocated that
 *     holds the finished cancestry_recipe_set_t and everything it points to.
 *     The scratch arena is freed before the loader returns, so a loaded set
 *     is exactly one allocation (SYS-NF-002: loading may allocate; the
 *     runtime path never does).
 *   - Floating-point scalars are converted with strtod, which follows the
 *     active C locale's decimal separator. Hosts and targets shall run with
 *     LC_NUMERIC=C (the default on every platform CANcestry currently
 *     targets); see README.md.
 */

#include "cancestry/recipe/loader.h"

#include <errno.h>
#include <float.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------------- */
/* Scratch arena                                                             */
/* ------------------------------------------------------------------------- */

/*
 * Chunked bump allocator. Once a chunk is full a new chunk is allocated and
 * old chunks are never moved, so every pointer returned by the arena stays
 * valid until the arena is freed (a single realloc'd block would invalidate
 * the AST node pointers on growth).
 */

typedef struct recipe_arena_chunk {
    struct recipe_arena_chunk *next;
    size_t capacity;
    /* storage follows the header */
} recipe_arena_chunk_t;

typedef struct recipe_arena {
    recipe_arena_chunk_t *head;
    char *cursor;
    size_t remaining;
} recipe_arena_t;

static void *recipe_arena_alloc(recipe_arena_t *arena, size_t size)
{
    size_t aligned = (size + 7u) & ~((size_t)7u);

    if (aligned > arena->remaining) {
        size_t chunk_bytes =
            (aligned > 4096u) ? aligned + sizeof(recipe_arena_chunk_t)
                              : 4096u + sizeof(recipe_arena_chunk_t);
        recipe_arena_chunk_t *chunk = (recipe_arena_chunk_t *)malloc(chunk_bytes);

        if (chunk == NULL) {
            return NULL;
        }
        chunk->next = arena->head;
        chunk->capacity = chunk_bytes - sizeof(*chunk);
        arena->head = chunk;
        arena->cursor = (char *)(void *)(chunk + 1);
        arena->remaining = chunk->capacity;
    }
    {
        void *result = (void *)arena->cursor;
        arena->cursor += aligned;
        arena->remaining -= aligned;
        return result;
    }
}

static void recipe_arena_free(recipe_arena_t *arena)
{
    recipe_arena_chunk_t *chunk = arena->head;

    while (chunk != NULL) {
        recipe_arena_chunk_t *next = chunk->next;
        free(chunk);
        chunk = next;
    }
    memset(arena, 0, sizeof(*arena));
}

/* ------------------------------------------------------------------------- */
/* AST                                                                       */
/* ------------------------------------------------------------------------- */

typedef enum recipe_ast_kind {
    RECIPE_AST_SCALAR = 0,
    RECIPE_AST_MAP = 1,
    RECIPE_AST_SEQ = 2,
    RECIPE_AST_NULL = 3
} recipe_ast_kind_t;

typedef struct recipe_ast_pair {
    const char *key;
    size_t key_length;
    struct recipe_ast_node *value;
    size_t line;
    size_t column;
} recipe_ast_pair_t;

typedef struct recipe_ast_node {
    recipe_ast_kind_t kind;
    /* scalar: NUL-terminated text (owned by the arena), and whether it was
     * quoted (quoted scalars are always YAML strings, whatever they look
     * like). */
    const char *text;
    size_t text_length;
    bool quoted;
    /* map */
    recipe_ast_pair_t *pairs;
    size_t pair_count;
    size_t pair_capacity;
    /* seq */
    struct recipe_ast_node **items;
    size_t item_count;
    size_t item_capacity;
    size_t line;
    size_t column;
} recipe_ast_node_t;

/* ------------------------------------------------------------------------- */
/* Parse context and error reporting                                         */
/* ------------------------------------------------------------------------- */

typedef struct recipe_load_context {
    const char *text;
    size_t length;
    size_t pos;
    size_t line;
    size_t column;
    recipe_arena_t arena;
} recipe_load_context_t;

static char recipe_load_peek(const recipe_load_context_t *context)
{
    if (context->pos >= context->length) {
        return '\0';
    }
    return context->text[context->pos];
}

static void recipe_load_advance(recipe_load_context_t *context)
{
    char c = context->text[context->pos];

    if (c == '\n') {
        context->pos++;
        context->line++;
        context->column = 1u;
    } else if (c == '\r') {
        context->pos++;
        if (context->pos < context->length && context->text[context->pos] == '\n') {
            context->pos++;
        }
        context->line++;
        context->column = 1u;
    } else {
        context->pos++;
        context->column++;
    }
}

static bool recipe_load_is_eol(char c)
{
    return c == '\n' || c == '\r' || c == '\0';
}

static bool recipe_load_fail(cancestry_recipe_load_error_t *error,
                             cancestry_recipe_status_t status,
                             size_t line,
                             size_t column,
                             const char *format,
                             ...)
{
    va_list args;

    if (error == NULL) {
        return false;
    }
    error->status = status;
    error->line = line;
    error->column = column;
    va_start(args, format);
    (void)vsnprintf(error->message, sizeof(error->message), format, args);
    va_end(args);
    return false;
}

/** Report a failure at the context's current line and column. */
static bool recipe_load_fail_here(const recipe_load_context_t *context,
                                  cancestry_recipe_load_error_t *error,
                                  cancestry_recipe_status_t status,
                                  const char *format,
                                  ...)
{
    va_list args;

    if (error == NULL) {
        return false;
    }
    error->status = status;
    error->line = context->line;
    error->column = context->column;
    va_start(args, format);
    (void)vsnprintf(error->message, sizeof(error->message), format, args);
    va_end(args);
    return false;
}

/* ------------------------------------------------------------------------- */
/* AST construction helpers                                                  */
/* ------------------------------------------------------------------------- */

static recipe_ast_node_t *recipe_ast_new(recipe_arena_t *arena,
                                         recipe_ast_kind_t kind,
                                         size_t line,
                                         size_t column)
{
    recipe_ast_node_t *node = (recipe_ast_node_t *)recipe_arena_alloc(arena, sizeof(*node));

    if (node == NULL) {
        return NULL;
    }
    memset(node, 0, sizeof(*node));
    node->kind = kind;
    node->line = line;
    node->column = column;
    return node;
}

static bool recipe_ast_map_add(recipe_arena_t *arena,
                               recipe_ast_node_t *map,
                               const char *key,
                               size_t key_length,
                               recipe_ast_node_t *value,
                               size_t line,
                               size_t column)
{
    if (map->pair_count == map->pair_capacity) {
        size_t new_capacity = (map->pair_capacity == 0u) ? 4u : map->pair_capacity * 2u;
        recipe_ast_pair_t *pairs =
            (recipe_ast_pair_t *)recipe_arena_alloc(arena, new_capacity * sizeof(*pairs));

        if (pairs == NULL) {
            return false;
        }
        if (map->pairs != NULL) {
            memcpy(pairs, map->pairs, map->pair_count * sizeof(*pairs));
        }
        map->pairs = pairs;
        map->pair_capacity = new_capacity;
    }
    map->pairs[map->pair_count].key = key;
    map->pairs[map->pair_count].key_length = key_length;
    map->pairs[map->pair_count].value = value;
    map->pairs[map->pair_count].line = line;
    map->pairs[map->pair_count].column = column;
    map->pair_count++;
    return true;
}

static bool recipe_ast_seq_add(recipe_arena_t *arena, recipe_ast_node_t *seq, recipe_ast_node_t *item)
{
    if (seq->item_count == seq->item_capacity) {
        size_t new_capacity = (seq->item_capacity == 0u) ? 4u : seq->item_capacity * 2u;
        recipe_ast_node_t **items =
            (recipe_ast_node_t **)recipe_arena_alloc(arena, new_capacity * sizeof(*items));

        if (items == NULL) {
            return false;
        }
        if (seq->items != NULL) {
            memcpy(items, seq->items, seq->item_count * sizeof(*items));
        }
        seq->items = items;
        seq->item_capacity = new_capacity;
    }
    seq->items[seq->item_count] = item;
    seq->item_count++;
    return true;
}

static bool recipe_ast_find_pair(const recipe_ast_node_t *map, const char *key, size_t *index_out)
{
    size_t i;
    size_t key_length = strlen(key);

    for (i = 0u; i < map->pair_count; ++i) {
        if (map->pairs[i].key_length == key_length &&
            memcmp(map->pairs[i].key, key, key_length) == 0) {
            if (index_out != NULL) {
                *index_out = i;
            }
            return true;
        }
    }
    return false;
}

/* ------------------------------------------------------------------------- */
/* Low-level scalar utilities                                                */
/* ------------------------------------------------------------------------- */

/** Copy a text slice into the arena as a NUL-terminated string. */
static char *recipe_string_copy(recipe_arena_t *arena, const char *text, size_t length)
{
    char *copy = (char *)recipe_arena_alloc(arena, length + 1u);

    if (copy == NULL) {
        return NULL;
    }
    memcpy(copy, text, length);
    copy[length] = '\0';
    return copy;
}

/** Append one UTF-8 encoded code point to a string being built. */
static void recipe_utf8_emit(char *buffer, size_t *used, uint32_t codepoint)
{
    if (codepoint < 0x80u) {
        buffer[(*used)++] = (char)codepoint;
    } else if (codepoint < 0x800u) {
        buffer[(*used)++] = (char)(0xC0u | (codepoint >> 6u));
        buffer[(*used)++] = (char)(0x80u | (codepoint & 0x3Fu));
    } else if (codepoint < 0x10000u) {
        buffer[(*used)++] = (char)(0xE0u | (codepoint >> 12u));
        buffer[(*used)++] = (char)(0x80u | ((codepoint >> 6u) & 0x3Fu));
        buffer[(*used)++] = (char)(0x80u | (codepoint & 0x3Fu));
    } else {
        buffer[(*used)++] = (char)(0xF0u | (codepoint >> 18u));
        buffer[(*used)++] = (char)(0x80u | ((codepoint >> 12u) & 0x3Fu));
        buffer[(*used)++] = (char)(0x80u | ((codepoint >> 6u) & 0x3Fu));
        buffer[(*used)++] = (char)(0x80u | (codepoint & 0x3Fu));
    }
}

static int recipe_hex_digit(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

/* ------------------------------------------------------------------------- */
/* Scalar interpretation                                                     */
/* ------------------------------------------------------------------------- */

typedef enum recipe_scalar_kind {
    RECIPE_SCALAR_STRING = 0,
    RECIPE_SCALAR_INT = 1,
    RECIPE_SCALAR_FLOAT = 2,
    RECIPE_SCALAR_BOOL = 3
} recipe_scalar_kind_t;

/** Classify a plain scalar's implicit YAML type. */
static recipe_scalar_kind_t recipe_scalar_classify(const char *text, size_t length)
{
    size_t i = 0u;

    if ((length == 4u && memcmp(text, "true", 4u) == 0) ||
        (length == 5u && memcmp(text, "false", 5u) == 0)) {
        return RECIPE_SCALAR_BOOL;
    }
    if (length == 0u) {
        return RECIPE_SCALAR_STRING;
    }
    if (text[i] == '+' || text[i] == '-') {
        i++;
    }
    if (i < length && text[i] == '0' && i + 1u < length) {
        if (text[i + 1u] == 'x' || text[i + 1u] == 'X') {
            size_t digits = 0u;
            size_t j;
            for (j = i + 2u; j < length; ++j) {
                if (recipe_hex_digit(text[j]) < 0) {
                    return RECIPE_SCALAR_STRING;
                }
                digits++;
            }
            return digits > 0u ? RECIPE_SCALAR_INT : RECIPE_SCALAR_STRING;
        }
        if (text[i + 1u] == 'o' || text[i + 1u] == 'O') {
            size_t digits = 0u;
            size_t j;
            for (j = i + 2u; j < length; ++j) {
                if (text[j] < '0' || text[j] > '7') {
                    return RECIPE_SCALAR_STRING;
                }
                digits++;
            }
            return digits > 0u ? RECIPE_SCALAR_INT : RECIPE_SCALAR_STRING;
        }
    }
    {
        bool saw_dot = false;
        bool saw_exp = false;
        bool saw_digit = false;
        for (; i < length; ++i) {
            char c = text[i];
            if (c >= '0' && c <= '9') {
                saw_digit = true;
            } else if (c == '.') {
                if (saw_dot || saw_exp) {
                    return RECIPE_SCALAR_STRING;
                }
                saw_dot = true;
            } else if (c == 'e' || c == 'E') {
                if (saw_exp || !saw_digit) {
                    return RECIPE_SCALAR_STRING;
                }
                saw_exp = true;
                saw_digit = false;
                if (i + 1u < length && (text[i + 1u] == '+' || text[i + 1u] == '-')) {
                    i++;
                }
            } else {
                return RECIPE_SCALAR_STRING;
            }
        }
        if (!saw_digit) {
            return RECIPE_SCALAR_STRING;
        }
        return (saw_dot || saw_exp) ? RECIPE_SCALAR_FLOAT : RECIPE_SCALAR_INT;
    }
}

static bool recipe_parse_uint64(const char *text, size_t length, uint64_t *value_out)
{
    uint64_t value = 0u;
    size_t i = 0u;

    if (length == 0u) {
        return false;
    }
    for (; i < length; ++i) {
        char c = text[i];
        if (c < '0' || c > '9') {
            return false;
        }
        if (value > (UINT64_MAX - (uint64_t)(c - '0')) / 10u) {
            return false; /* overflow */
        }
        value = (value * 10u) + (uint64_t)(c - '0');
    }
    *value_out = value;
    return true;
}

static bool recipe_parse_double(const char *text, size_t length, double *value_out)
{
    char buffer[64];
    char *end = NULL;
    double value;

    if (length == 0u || length >= sizeof(buffer)) {
        return false;
    }
    memcpy(buffer, text, length);
    buffer[length] = '\0';
    value = strtod(buffer, &end);
    if (end != buffer + length) {
        return false;
    }
    *value_out = value;
    return true;
}

/* ------------------------------------------------------------------------- */
/* YAML subset parser                                                        */
/* ------------------------------------------------------------------------- */

/**
 * Skip blank lines and comment-only lines. Leaves the position at the first
 * content character of the next content line, or at end of input.
 */
static void recipe_load_skip_blank_lines(recipe_load_context_t *context)
{
    for (;;) {
        size_t p = context->pos;

        while (p < context->length && context->text[p] == ' ') {
            p++;
        }
        if (p >= context->length) {
            context->pos = p;
            return;
        }
        if (context->text[p] == '\n' || context->text[p] == '\r') {
            while (context->pos < context->length && context->text[context->pos] != '\n' &&
                   context->text[context->pos] != '\r') {
                recipe_load_advance(context);
            }
            while (recipe_load_is_eol(recipe_load_peek(context)) &&
                   recipe_load_peek(context) != '\0') {
                recipe_load_advance(context);
            }
            continue;
        }
        if (context->text[p] == '#') {
            while (!recipe_load_is_eol(recipe_load_peek(context))) {
                recipe_load_advance(context);
            }
            while (recipe_load_is_eol(recipe_load_peek(context)) &&
                   recipe_load_peek(context) != '\0') {
                recipe_load_advance(context);
            }
            continue;
        }
        return;
    }
}

/**
 * Measure the 1-based column of the first content character of the current
 * line without consuming anything. Returns false on tab indentation.
 */
static bool recipe_load_content_column(recipe_load_context_t *context,
                                       size_t *column_out,
                                       cancestry_recipe_load_error_t *error)
{
    size_t p = context->pos;
    size_t column = context->column;

    while (p < context->length && context->text[p] == ' ') {
        p++;
        column++;
    }
    if (p < context->length && context->text[p] == '\t') {
        return recipe_load_fail_here(context, error, CANCESTRY_RECIPE_ERR_PARSE,
                                    "tab characters are not allowed in indentation");
    }
    *column_out = column;
    return true;
}

/**
 * Peek whether the rest of the line contains only spaces and a comment.
 * The position must be at (or before) the first space to check.
 */
static bool recipe_load_rest_is_comment(const recipe_load_context_t *context)
{
    size_t p = context->pos;

    while (p < context->length && context->text[p] == ' ') {
        p++;
    }
    return p < context->length && context->text[p] == '#';
}

/**
 * Parse a double-quoted scalar starting at the opening quote. Decodes escape
 * sequences into the arena.
 */
static bool recipe_load_parse_double_quoted(recipe_load_context_t *context,
                                            recipe_arena_t *arena,
                                            char **text_out,
                                            size_t *length_out,
                                            cancestry_recipe_load_error_t *error)
{
    size_t line = context->line;
    size_t column = context->column;
    char *buffer = (char *)recipe_arena_alloc(arena, 64u);
    size_t used = 0u;
    size_t capacity = 64u;

    if (buffer == NULL) {
        return recipe_load_fail_here(context, error, CANCESTRY_RECIPE_ERR_NO_MEMORY,
                                     "out of memory");
    }
    recipe_load_advance(context); /* opening quote */
    for (;;) {
        char c = recipe_load_peek(context);
        uint32_t codepoint;

        if (c == '\0' || c == '\n' || c == '\r') {
            return recipe_load_fail(error, CANCESTRY_RECIPE_ERR_PARSE, line, column,
                                    "unterminated double-quoted string");
        }
        if (c == '"') {
            recipe_load_advance(context);
            break;
        }
        if (c != '\\') {
            codepoint = (uint32_t)(unsigned char)c;
            recipe_load_advance(context);
        } else {
            int digit;

            recipe_load_advance(context); /* backslash */
            c = recipe_load_peek(context);
            recipe_load_advance(context);
            switch (c) {
            case 'a':
                codepoint = 0x07u;
                break;
            case 'b':
                codepoint = 0x08u;
                break;
            case 't':
                codepoint = 0x09u;
                break;
            case 'n':
                codepoint = 0x0Au;
                break;
            case 'v':
                codepoint = 0x0Bu;
                break;
            case 'f':
                codepoint = 0x0Cu;
                break;
            case 'r':
                codepoint = 0x0Du;
                break;
            case 'e':
                codepoint = 0x1Bu;
                break;
            case ' ':
                codepoint = 0x20u;
                break;
            case '"':
                codepoint = 0x22u;
                break;
            case '/':
                codepoint = 0x2Fu;
                break;
            case '\\':
                codepoint = 0x5Cu;
                break;
            case 'N':
                codepoint = 0x85u;
                break;
            case '_':
                codepoint = 0xA0u;
                break;
            case 'L':
                codepoint = 0x2028u;
                break;
            case 'P':
                codepoint = 0x2029u;
                break;
            case 'x':
                digit = recipe_hex_digit(recipe_load_peek(context));
                if (digit < 0) {
                    return recipe_load_fail(error, CANCESTRY_RECIPE_ERR_PARSE, context->line,
                                            context->column, "invalid \\x escape");
                }
                codepoint = (uint32_t)digit;
                recipe_load_advance(context);
                digit = recipe_hex_digit(recipe_load_peek(context));
                if (digit < 0) {
                    return recipe_load_fail(error, CANCESTRY_RECIPE_ERR_PARSE, context->line,
                                            context->column, "invalid \\x escape");
                }
                codepoint = (codepoint << 4u) | (uint32_t)digit;
                recipe_load_advance(context);
                break;
            case 'u':
            case 'U': {
                size_t digits = (c == 'u') ? 4u : 8u;
                size_t i;
                codepoint = 0u;
                for (i = 0u; i < digits; ++i) {
                    digit = recipe_hex_digit(recipe_load_peek(context));
                    if (digit < 0) {
                        return recipe_load_fail(error, CANCESTRY_RECIPE_ERR_PARSE, context->line,
                                                context->column, "invalid \\u escape");
                    }
                    codepoint = (codepoint << 4u) | (uint32_t)digit;
                    recipe_load_advance(context);
                }
                if ((codepoint >= 0xD800u && codepoint <= 0xDFFFu) || codepoint > 0x10FFFFu) {
                    return recipe_load_fail(error, CANCESTRY_RECIPE_ERR_PARSE, context->line,
                                            context->column, "invalid unicode code point");
                }
                break;
            }
            default:
                return recipe_load_fail(error, CANCESTRY_RECIPE_ERR_PARSE, context->line,
                                        context->column, "invalid escape sequence '\\%c'", c);
            }
        }
        /* Grow if needed: worst case 4 bytes per code point. */
        if (used + 4u >= capacity) {
            char *grown = (char *)recipe_arena_alloc(arena, capacity * 2u);
            if (grown == NULL) {
                return recipe_load_fail_here(context, error, CANCESTRY_RECIPE_ERR_NO_MEMORY,
                                             "out of memory");
            }
            memcpy(grown, buffer, used);
            buffer = grown;
            capacity *= 2u;
        }
        recipe_utf8_emit(buffer, &used, codepoint);
    }
    buffer[used] = '\0';
    *text_out = buffer;
    *length_out = used;
    return true;
}

/** Parse a single-quoted scalar starting at the opening quote. */
static bool recipe_load_parse_single_quoted(recipe_load_context_t *context,
                                            recipe_arena_t *arena,
                                            char **text_out,
                                            size_t *length_out,
                                            cancestry_recipe_load_error_t *error)
{
    size_t line = context->line;
    size_t column = context->column;
    char *buffer = (char *)recipe_arena_alloc(arena, 64u);
    size_t used = 0u;
    size_t capacity = 64u;

    if (buffer == NULL) {
        return recipe_load_fail_here(context, error, CANCESTRY_RECIPE_ERR_NO_MEMORY,
                                     "out of memory");
    }
    recipe_load_advance(context); /* opening quote */
    for (;;) {
        char c = recipe_load_peek(context);

        if (c == '\0' || c == '\n' || c == '\r') {
            return recipe_load_fail(error, CANCESTRY_RECIPE_ERR_PARSE, line, column,
                                    "unterminated single-quoted string");
        }
        if (c == '\'') {
            recipe_load_advance(context);
            if (recipe_load_peek(context) == '\'') {
                c = '\'';
                recipe_load_advance(context);
            } else {
                break;
            }
        } else {
            recipe_load_advance(context);
        }
        if (used + 1u >= capacity) {
            char *grown = (char *)recipe_arena_alloc(arena, capacity * 2u);
            if (grown == NULL) {
                return recipe_load_fail_here(context, error, CANCESTRY_RECIPE_ERR_NO_MEMORY,
                                             "out of memory");
            }
            memcpy(grown, buffer, used);
            buffer = grown;
            capacity *= 2u;
        }
        buffer[used++] = c;
    }
    buffer[used] = '\0';
    *text_out = buffer;
    *length_out = used;
    return true;
}

/**
 * Parse the rest of the current line as a scalar value node. The caller has
 * already skipped the spaces after ':' or '-'. Trailing comment and spaces
 * are consumed.
 */
static bool recipe_load_parse_scalar_value(recipe_load_context_t *context,
                                           recipe_ast_node_t *node,
                                           cancestry_recipe_load_error_t *error)
{
    size_t line = context->line;
    size_t column = context->column;
    char c = recipe_load_peek(context);

    node->kind = RECIPE_AST_SCALAR;
    node->line = line;
    node->column = column;

    if (c == '"') {
        char *text = NULL;
        size_t length = 0u;
        if (!recipe_load_parse_double_quoted(context, &context->arena, &text, &length, error)) {
            return false;
        }
        node->text = text;
        node->text_length = length;
        node->quoted = true;
    } else if (c == '\'') {
        char *text = NULL;
        size_t length = 0u;
        if (!recipe_load_parse_single_quoted(context, &context->arena, &text, &length, error)) {
            return false;
        }
        node->text = text;
        node->text_length = length;
        node->quoted = true;
    } else {
        const char *start = context->text + context->pos;
        size_t start_column = context->column;
        const char *end;

        /* Plain scalar: runs to end of line or a " #" comment. */
        for (;;) {
            c = recipe_load_peek(context);
            if (recipe_load_is_eol(c)) {
                break;
            }
            if (c == ' ' && recipe_load_rest_is_comment(context)) {
                break;
            }
            recipe_load_advance(context);
        }
        end = context->text + context->pos;
        while (end > start && end[-1] == ' ') {
            end--;
        }
        if (end == start) {
            return recipe_load_fail(error, CANCESTRY_RECIPE_ERR_PARSE, line, start_column,
                                    "expected a scalar value");
        }
        node->text = recipe_string_copy(&context->arena, start, (size_t)(end - start));
        if (node->text == NULL) {
            return recipe_load_fail_here(context, error, CANCESTRY_RECIPE_ERR_NO_MEMORY,
                                         "out of memory");
        }
        node->text_length = (size_t)(end - start);
    }

    /* A scalar ends the value; only a comment or end of line may follow. */
    for (;;) {
        c = recipe_load_peek(context);
        if (recipe_load_is_eol(c)) {
            break;
        }
        if (c == ' ') {
            recipe_load_advance(context);
            continue;
        }
        if (c == '#') {
            while (!recipe_load_is_eol(recipe_load_peek(context))) {
                recipe_load_advance(context);
            }
            break;
        }
        return recipe_load_fail_here(context, error, CANCESTRY_RECIPE_ERR_PARSE,
                                     "unexpected content after scalar value");
    }
    while (recipe_load_is_eol(recipe_load_peek(context)) && recipe_load_peek(context) != '\0') {
        recipe_load_advance(context);
    }
    return true;
}

/**
 * Parse a mapping key and its ':' delimiter. The position must be at the key
 * start. On success the position is just past the ':'.
 */
static bool recipe_load_parse_key(recipe_load_context_t *context,
                                  recipe_arena_t *arena,
                                  char **key_out,
                                  size_t *key_length_out,
                                  size_t *line_out,
                                  size_t *column_out,
                                  cancestry_recipe_load_error_t *error)
{
    char *key;
    size_t key_length;
    size_t line = context->line;
    size_t column = context->column;
    char c = recipe_load_peek(context);

    if (c == '"') {
        if (!recipe_load_parse_double_quoted(context, arena, &key, &key_length, error)) {
            return false;
        }
    } else if (c == '\'') {
        if (!recipe_load_parse_single_quoted(context, arena, &key, &key_length, error)) {
            return false;
        }
    } else {
        const char *start = context->text + context->pos;
        const char *end;

        for (;;) {
            c = recipe_load_peek(context);
            if (recipe_load_is_eol(c)) {
                break;
            }
            if (c == ':' && (context->pos + 1u >= context->length ||
                             context->text[context->pos + 1u] == ' ' ||
                             recipe_load_is_eol(context->text[context->pos + 1u]))) {
                break;
            }
            recipe_load_advance(context);
        }
        end = context->text + context->pos;
        while (end > start && end[-1] == ' ') {
            end--;
        }
        if (end == start) {
            return recipe_load_fail(error, CANCESTRY_RECIPE_ERR_PARSE, line, column,
                                    "expected a mapping key");
        }
        key = recipe_string_copy(arena, start, (size_t)(end - start));
        if (key == NULL) {
            return recipe_load_fail_here(context, error, CANCESTRY_RECIPE_ERR_NO_MEMORY,
                                         "out of memory");
        }
        key_length = (size_t)(end - start);
    }

    /* Optional spaces between the key and ':'. */
    while (recipe_load_peek(context) == ' ') {
        recipe_load_advance(context);
    }
    if (recipe_load_peek(context) != ':') {
        return recipe_load_fail(error, CANCESTRY_RECIPE_ERR_PARSE, line, column,
                                "expected ':' after mapping key");
    }
    recipe_load_advance(context);

    if (key_out != NULL) {
        *key_out = key;
    }
    if (key_length_out != NULL) {
        *key_length_out = key_length;
    }
    if (line_out != NULL) {
        *line_out = line;
    }
    if (column_out != NULL) {
        *column_out = column;
    }
    return true;
}

/*
 * Forward declarations: block parsers call each other through the nested
 * value helper, and both check for the end-of-document marker.
 */
static bool recipe_load_line_is_marker(const recipe_load_context_t *context, const char *marker);

static bool recipe_load_parse_block_mapping(recipe_load_context_t *context,
                                            size_t column,
                                            recipe_ast_node_t **map_out,
                                            cancestry_recipe_load_error_t *error);

static bool recipe_load_parse_block_sequence(recipe_load_context_t *context,
                                             size_t column,
                                             recipe_ast_node_t **sequence_out,
                                             cancestry_recipe_load_error_t *error);

/**
 * Parse the nested block that follows a "key:" or "-" line with nothing after
 * it. The next content line must be deeper than @p parent_column.
 */
static bool recipe_load_parse_nested(recipe_load_context_t *context,
                                     size_t parent_column,
                                     recipe_ast_node_t **value_out,
                                     cancestry_recipe_load_error_t *error)
{
    size_t column = 0u;
    size_t p;
    char c;

    while (recipe_load_is_eol(recipe_load_peek(context)) && recipe_load_peek(context) != '\0') {
        recipe_load_advance(context);
    }
    recipe_load_skip_blank_lines(context);
    if (recipe_load_peek(context) == '\0') {
        *value_out =
            recipe_ast_new(&context->arena, RECIPE_AST_NULL, context->line, context->column);
        return *value_out != NULL
                   ? true
                   : recipe_load_fail_here(context, error, CANCESTRY_RECIPE_ERR_NO_MEMORY,
                                           "out of memory");
    }
    if (!recipe_load_content_column(context, &column, error)) {
        return false;
    }
    if (column <= parent_column) {
        *value_out =
            recipe_ast_new(&context->arena, RECIPE_AST_NULL, context->line, context->column);
        return *value_out != NULL
                   ? true
                   : recipe_load_fail_here(context, error, CANCESTRY_RECIPE_ERR_NO_MEMORY,
                                           "out of memory");
    }
    /* Peek at the content character to decide mapping vs sequence. */
    p = context->pos;
    while (p < context->length && context->text[p] == ' ') {
        p++;
    }
    c = (p < context->length) ? context->text[p] : '\0';
    if (c == '-') {
        return recipe_load_parse_block_sequence(context, column, value_out, error);
    }
    return recipe_load_parse_block_mapping(context, column, value_out, error);
}

/**
 * Parse one "key: value" line whose key starts at @p column, adding the pair
 * to @p map. The position must be at the key's first character.
 */
static bool recipe_load_parse_pair_line(recipe_load_context_t *context,
                                        size_t column,
                                        recipe_ast_node_t *map,
                                        cancestry_recipe_load_error_t *error)
{
    char *key;
    size_t key_length;
    size_t line;
    size_t key_column;
    recipe_ast_node_t *value;
    char c;

    if (recipe_load_peek(context) == '-') {
        return recipe_load_fail_here(context, error, CANCESTRY_RECIPE_ERR_PARSE,
                                     "expected a mapping entry, found '-'");
    }
    if (!recipe_load_parse_key(context, &context->arena, &key, &key_length, &line, &key_column,
                               error)) {
        return false;
    }
    {
        size_t i;
        for (i = 0u; i < map->pair_count; ++i) {
            if (map->pairs[i].key_length == key_length &&
                memcmp(map->pairs[i].key, key, key_length) == 0) {
                return recipe_load_fail(error, CANCESTRY_RECIPE_ERR_PARSE, line, key_column,
                                        "duplicate mapping key '%.*s'", (int)key_length, key);
            }
        }
    }
    while (recipe_load_peek(context) == ' ') {
        recipe_load_advance(context);
    }
    c = recipe_load_peek(context);
    value = recipe_ast_new(&context->arena, RECIPE_AST_NULL, context->line, context->column);
    if (value == NULL) {
        return recipe_load_fail_here(context, error, CANCESTRY_RECIPE_ERR_NO_MEMORY,
                                     "out of memory");
    }
    if (recipe_load_is_eol(c) || c == '#') {
        recipe_ast_node_t *nested = NULL;
        if (c == '#') {
            while (!recipe_load_is_eol(recipe_load_peek(context))) {
                recipe_load_advance(context);
            }
        }
        if (!recipe_load_parse_nested(context, column, &nested, error)) {
            return false;
        }
        if (nested->kind != RECIPE_AST_NULL) {
            value = nested;
        }
    } else {
        if (!recipe_load_parse_scalar_value(context, value, error)) {
            return false;
        }
    }
    return recipe_ast_map_add(&context->arena, map, key, key_length, value, line, key_column);
}

static bool recipe_load_parse_block_mapping(recipe_load_context_t *context,
                                            size_t column,
                                            recipe_ast_node_t **map_out,
                                            cancestry_recipe_load_error_t *error)
{
    recipe_ast_node_t *map =
        recipe_ast_new(&context->arena, RECIPE_AST_MAP, context->line, context->column);

    if (map == NULL) {
        return recipe_load_fail_here(context, error, CANCESTRY_RECIPE_ERR_NO_MEMORY,
                                     "out of memory");
    }
    for (;;) {
        size_t entry_column = 0u;

        recipe_load_skip_blank_lines(context);
        if (recipe_load_peek(context) == '\0') {
            break;
        }
        if (!recipe_load_content_column(context, &entry_column, error)) {
            return false;
        }
        if (entry_column < column) {
            break; /* back to the parent block */
        }
        if (entry_column > column) {
            return recipe_load_fail_here(context, error, CANCESTRY_RECIPE_ERR_PARSE,
                                         "unexpected indentation");
        }
        while (recipe_load_peek(context) == ' ') {
            recipe_load_advance(context);
        }
        if (recipe_load_line_is_marker(context, "...")) {
            break; /* end of document */
        }
        if (!recipe_load_parse_pair_line(context, column, map, error)) {
            return false;
        }
    }
    *map_out = map;
    return true;
}

/**
 * Parse the "key: value" pair of an inline sequence item mapping whose key
 * column is @p key_column, then the item's continuation lines.
 */
static bool recipe_load_parse_seq_item_map(recipe_load_context_t *context,
                                           size_t key_column,
                                           recipe_ast_node_t **map_out,
                                           cancestry_recipe_load_error_t *error)
{
    recipe_ast_node_t *map =
        recipe_ast_new(&context->arena, RECIPE_AST_MAP, context->line, context->column);

    if (map == NULL) {
        return recipe_load_fail_here(context, error, CANCESTRY_RECIPE_ERR_NO_MEMORY,
                                     "out of memory");
    }
    if (!recipe_load_parse_pair_line(context, key_column, map, error)) {
        return false;
    }
    for (;;) {
        size_t next_column = 0u;

        recipe_load_skip_blank_lines(context);
        if (recipe_load_peek(context) == '\0') {
            break;
        }
        if (!recipe_load_content_column(context, &next_column, error)) {
            return false;
        }
        if (next_column < key_column) {
            break;
        }
        if (next_column > key_column) {
            return recipe_load_fail_here(context, error, CANCESTRY_RECIPE_ERR_PARSE,
                                         "unexpected indentation");
        }
        while (recipe_load_peek(context) == ' ') {
            recipe_load_advance(context);
        }
        if (recipe_load_peek(context) == '-') {
            break;
        }
        if (!recipe_load_parse_pair_line(context, key_column, map, error)) {
            return false;
        }
    }
    *map_out = map;
    return true;
}

/**
 * Parse one "- ..." line (the '-' has already been consumed) into a sequence
 * item.
 */
static bool recipe_load_parse_sequence_item(recipe_load_context_t *context,
                                            size_t sequence_column,
                                            recipe_ast_node_t **item_out,
                                            cancestry_recipe_load_error_t *error)
{
    recipe_ast_node_t *item =
        recipe_ast_new(&context->arena, RECIPE_AST_NULL, context->line, context->column);
    char c;

    if (item == NULL) {
        return recipe_load_fail_here(context, error, CANCESTRY_RECIPE_ERR_NO_MEMORY,
                                     "out of memory");
    }
    c = recipe_load_peek(context);
    if (recipe_load_is_eol(c)) {
        recipe_ast_node_t *nested = NULL;
        if (!recipe_load_parse_nested(context, sequence_column, &nested, error)) {
            return false;
        }
        if (nested->kind != RECIPE_AST_NULL) {
            item = nested;
        }
    } else if (c == ' ') {
        while (recipe_load_peek(context) == ' ') {
            recipe_load_advance(context);
        }
        c = recipe_load_peek(context);
        if (recipe_load_is_eol(c)) {
            recipe_ast_node_t *nested = NULL;
            if (!recipe_load_parse_nested(context, sequence_column, &nested, error)) {
                return false;
            }
            if (nested->kind != RECIPE_AST_NULL) {
                item = nested;
            }
        } else if (c == '#') {
            while (!recipe_load_is_eol(recipe_load_peek(context))) {
                recipe_load_advance(context);
            }
        } else if (c == '-' || c == '[' || c == '{') {
            return recipe_load_fail_here(
                context, error, CANCESTRY_RECIPE_ERR_PARSE,
                "nested sequences, flow sequences and flow mappings are not supported; start "
                "the nested block on a new line");
        } else {
            bool mapping_form = false;

            /*
             * Either an inline mapping ("- key: value") or a scalar item.
             * Detect the mapping form by looking for a ':' key delimiter on
             * this line without consuming it.
             */
            if (c == '"' || c == '\'') {
                size_t save_pos = context->pos;
                size_t save_line = context->line;
                size_t save_column = context->column;

                mapping_form =
                    recipe_load_parse_key(context, &context->arena, NULL, NULL, NULL, NULL, NULL);
                context->pos = save_pos;
                context->line = save_line;
                context->column = save_column;
            } else {
                size_t p;
                for (p = context->pos; p < context->length; ++p) {
                    char pc = context->text[p];
                    if (recipe_load_is_eol(pc)) {
                        break;
                    }
                    if (pc == ':' && (p + 1u >= context->length ||
                                      context->text[p + 1u] == ' ' ||
                                      recipe_load_is_eol(context->text[p + 1u]))) {
                        mapping_form = true;
                        break;
                    }
                }
            }
            if (mapping_form) {
                size_t key_column = context->column;
                recipe_ast_node_t *map = NULL;
                if (!recipe_load_parse_seq_item_map(context, key_column, &map, error)) {
                    return false;
                }
                item = map;
            } else {
                if (!recipe_load_parse_scalar_value(context, item, error)) {
                    return false;
                }
            }
        }
    } else {
        return recipe_load_fail_here(context, error, CANCESTRY_RECIPE_ERR_PARSE,
                                     "expected a space after '-'");
    }
    *item_out = item;
    return true;
}

static bool recipe_load_parse_block_sequence(recipe_load_context_t *context,
                                             size_t column,
                                             recipe_ast_node_t **sequence_out,
                                             cancestry_recipe_load_error_t *error)
{
    recipe_ast_node_t *sequence =
        recipe_ast_new(&context->arena, RECIPE_AST_SEQ, context->line, context->column);

    if (sequence == NULL) {
        return recipe_load_fail_here(context, error, CANCESTRY_RECIPE_ERR_NO_MEMORY,
                                     "out of memory");
    }
    for (;;) {
        size_t entry_column = 0u;
        recipe_ast_node_t *item = NULL;

        recipe_load_skip_blank_lines(context);
        if (recipe_load_peek(context) == '\0') {
            break;
        }
        if (!recipe_load_content_column(context, &entry_column, error)) {
            return false;
        }
        if (entry_column < column) {
            break;
        }
        if (entry_column > column) {
            return recipe_load_fail_here(context, error, CANCESTRY_RECIPE_ERR_PARSE,
                                         "unexpected indentation");
        }
        while (recipe_load_peek(context) == ' ') {
            recipe_load_advance(context);
        }
        if (recipe_load_line_is_marker(context, "...")) {
            break; /* end of document */
        }
        if (recipe_load_peek(context) != '-') {
            break; /* end of the sequence; the parent block continues */
        }
        recipe_load_advance(context);
        if (!recipe_load_parse_sequence_item(context, column, &item, error)) {
            return false;
        }
        if (!recipe_ast_seq_add(&context->arena, sequence, item)) {
            return recipe_load_fail_here(context, error, CANCESTRY_RECIPE_ERR_NO_MEMORY,
                                         "out of memory");
        }
    }
    *sequence_out = sequence;
    return true;
}

static bool recipe_load_line_is_marker(const recipe_load_context_t *context, const char *marker)
{
    size_t p = context->pos;
    size_t length = strlen(marker);

    if (p + length > context->length || memcmp(context->text + p, marker, length) != 0) {
        return false;
    }
    p += length;
    if (p < context->length && context->text[p] != ' ' && context->text[p] != '\t' &&
        context->text[p] != '#' && !recipe_load_is_eol(context->text[p])) {
        return false;
    }
    return true;
}

static void recipe_load_consume_line(recipe_load_context_t *context)
{
    while (!recipe_load_is_eol(recipe_load_peek(context))) {
        recipe_load_advance(context);
    }
    while (recipe_load_is_eol(recipe_load_peek(context)) && recipe_load_peek(context) != '\0') {
        recipe_load_advance(context);
    }
}

static bool recipe_load_parse_document(recipe_load_context_t *context,
                                       recipe_ast_node_t **root_out,
                                       cancestry_recipe_load_error_t *error)
{
    size_t column = 0u;
    recipe_ast_node_t *root = NULL;

    recipe_load_skip_blank_lines(context);
    if (recipe_load_peek(context) == '\0') {
        return recipe_load_fail(error, CANCESTRY_RECIPE_ERR_PARSE, context->line, context->column,
                                "document is empty");
    }
    if (recipe_load_line_is_marker(context, "---")) {
        recipe_load_consume_line(context);
        recipe_load_skip_blank_lines(context);
    }
    if (recipe_load_peek(context) == '\0' || recipe_load_line_is_marker(context, "...")) {
        return recipe_load_fail(error, CANCESTRY_RECIPE_ERR_PARSE, context->line, context->column,
                                "document has no content");
    }
    if (!recipe_load_content_column(context, &column, error)) {
        return false;
    }
    if (!recipe_load_parse_block_mapping(context, column, &root, error)) {
        return false;
    }
    recipe_load_skip_blank_lines(context);
    if (recipe_load_line_is_marker(context, "...")) {
        recipe_load_consume_line(context);
        recipe_load_skip_blank_lines(context);
    }
    if (recipe_load_peek(context) != '\0') {
        return recipe_load_fail_here(context, error, CANCESTRY_RECIPE_ERR_PARSE,
                                     "unexpected content after the document");
    }
    *root_out = root;
    return true;
}

/* ------------------------------------------------------------------------- */
/* Schema-field readers                                                      */
/* ------------------------------------------------------------------------- */

static bool recipe_field_string(const recipe_ast_node_t *node,
                                recipe_arena_t *arena,
                                char **text_out,
                                size_t *length_out)
{
    char *copy;

    if (node == NULL || node->kind != RECIPE_AST_SCALAR) {
        return false;
    }
    /* Per the JSON Schema all string fields require a YAML string scalar: an
     * unquoted scalar that parses as an integer, float or boolean is not one.
     * Quoted scalars are strings no matter what they look like. */
    if (!node->quoted &&
        recipe_scalar_classify(node->text, node->text_length) != RECIPE_SCALAR_STRING) {
        return false;
    }
    copy = recipe_string_copy(arena, node->text, node->text_length);
    if (copy == NULL) {
        return false;
    }
    *text_out = copy;
    if (length_out != NULL) {
        *length_out = node->text_length;
    }
    return true;
}

/**
 * Read a non-negative integer field in [@p min_value, @p max_value] into a
 * uint32_t.
 */
static bool recipe_field_uint(const recipe_ast_node_t *node,
                              int64_t min_value,
                              int64_t max_value,
                              uint32_t *value_out,
                              cancestry_recipe_load_error_t *error,
                              const char *field)
{
    uint64_t magnitude = 0u;
    const char *text;
    size_t length;
    size_t offset = 0u;
    bool negative = false;

    if (node == NULL || node->kind != RECIPE_AST_SCALAR) {
        return recipe_load_fail(error, CANCESTRY_RECIPE_ERR_PARSE, node ? node->line : 0u,
                                node ? node->column : 0u, "'%s' must be an integer", field);
    }
    text = node->text;
    length = node->text_length;
    if (recipe_scalar_classify(text, length) != RECIPE_SCALAR_INT) {
        return recipe_load_fail(error, CANCESTRY_RECIPE_ERR_PARSE, node->line, node->column,
                                "'%s' must be an integer", field);
    }
    if (text[0] == '+' || text[0] == '-') {
        negative = (text[0] == '-');
        offset = 1u;
    }
    if (length - offset > 2u && text[offset] == '0' &&
        (text[offset + 1u] == 'x' || text[offset + 1u] == 'X')) {
        size_t i;
        for (i = offset + 2u; i < length; ++i) {
            int digit = recipe_hex_digit(text[i]);
            if (digit < 0 || magnitude > (UINT64_MAX - (uint64_t)digit) / 16u) {
                return recipe_load_fail(error, CANCESTRY_RECIPE_ERR_PARSE, node->line,
                                        node->column, "'%s' is out of range", field);
            }
            magnitude = (magnitude * 16u) + (uint64_t)digit;
        }
    } else if (length - offset > 2u && text[offset] == '0' &&
               (text[offset + 1u] == 'o' || text[offset + 1u] == 'O')) {
        size_t i;
        for (i = offset + 2u; i < length; ++i) {
            int digit = text[i] - '0';
            if (digit < 0 || magnitude > (UINT64_MAX - (uint64_t)digit) / 8u) {
                return recipe_load_fail(error, CANCESTRY_RECIPE_ERR_PARSE, node->line,
                                        node->column, "'%s' is out of range", field);
            }
            magnitude = (magnitude * 8u) + (uint64_t)digit;
        }
    } else {
        if (!recipe_parse_uint64(text + offset, length - offset, &magnitude)) {
            return recipe_load_fail(error, CANCESTRY_RECIPE_ERR_PARSE, node->line, node->column,
                                    "'%s' is out of range", field);
        }
    }
    if (negative) {
        return recipe_load_fail(error, CANCESTRY_RECIPE_ERR_PARSE, node->line, node->column,
                                "'%s' is out of range", field);
    }
    if (min_value > 0 && magnitude < (uint64_t)min_value) {
        return recipe_load_fail(error, CANCESTRY_RECIPE_ERR_PARSE, node->line, node->column,
                                "'%s' is out of range", field);
    }
    if (magnitude > (uint64_t)max_value) {
        return recipe_load_fail(error, CANCESTRY_RECIPE_ERR_PARSE, node->line, node->column,
                                "'%s' is out of range", field);
    }
    *value_out = (uint32_t)magnitude;
    return true;
}

static bool recipe_field_double(const recipe_ast_node_t *node,
                                double *value_out,
                                cancestry_recipe_load_error_t *error,
                                const char *field)
{
    if (node == NULL || node->kind != RECIPE_AST_SCALAR) {
        return recipe_load_fail(error, CANCESTRY_RECIPE_ERR_PARSE, node ? node->line : 0u,
                                node ? node->column : 0u, "'%s' must be a number", field);
    }
    switch (recipe_scalar_classify(node->text, node->text_length)) {
    case RECIPE_SCALAR_INT: {
        uint64_t magnitude = 0u;
        bool negative = false;
        size_t offset = 0u;
        double value;
        if (node->text[0] == '+' || node->text[0] == '-') {
            negative = (node->text[0] == '-');
            offset = 1u;
        }
        if (!recipe_parse_uint64(node->text + offset, node->text_length - offset, &magnitude)) {
            return recipe_load_fail(error, CANCESTRY_RECIPE_ERR_PARSE, node->line, node->column,
                                    "'%s' is out of range", field);
        }
        value = negative ? -(double)magnitude : (double)magnitude;
        if (!(value >= -DBL_MAX && value <= DBL_MAX)) {
            return recipe_load_fail(error, CANCESTRY_RECIPE_ERR_PARSE, node->line, node->column,
                                    "'%s' is out of range", field);
        }
        *value_out = value;
        return true;
    }
    case RECIPE_SCALAR_FLOAT:
        if (!recipe_parse_double(node->text, node->text_length, value_out)) {
            return recipe_load_fail(error, CANCESTRY_RECIPE_ERR_PARSE, node->line, node->column,
                                    "'%s' is not a valid number", field);
        }
        if (!(*value_out >= -DBL_MAX && *value_out <= DBL_MAX)) {
            return recipe_load_fail(error, CANCESTRY_RECIPE_ERR_PARSE, node->line, node->column,
                                    "'%s' must be finite", field);
        }
        return true;
    default:
        return recipe_load_fail(error, CANCESTRY_RECIPE_ERR_PARSE, node->line, node->column,
                                "'%s' must be a number", field);
    }
}

static bool recipe_field_bool(const recipe_ast_node_t *node,
                              bool *value_out,
                              cancestry_recipe_load_error_t *error,
                              const char *field)
{
    if (node == NULL || node->kind != RECIPE_AST_SCALAR) {
        return recipe_load_fail(error, CANCESTRY_RECIPE_ERR_PARSE, node ? node->line : 0u,
                                node ? node->column : 0u, "'%s' must be a boolean", field);
    }
    if (recipe_scalar_classify(node->text, node->text_length) != RECIPE_SCALAR_BOOL) {
        return recipe_load_fail(error, CANCESTRY_RECIPE_ERR_PARSE, node->line, node->column,
                                "'%s' must be a boolean", field);
    }
    *value_out = (node->text_length == 4u);
    return true;
}

/**
 * Read a schema "expression_or_literal" field: a YAML number or boolean
 * becomes a literal value; a YAML string becomes an expression (see
 * core/recipe/README.md). Integers that exceed int64 become REAL literals.
 */
static bool recipe_field_operand(const recipe_ast_node_t *node,
                                 recipe_arena_t *arena,
                                 bool *is_expression_out,
                                 cancestry_value_t *literal_out,
                                 char **expression_out,
                                 cancestry_recipe_load_error_t *error,
                                 const char *field)
{
    if (node == NULL || node->kind != RECIPE_AST_SCALAR) {
        return recipe_load_fail(error, CANCESTRY_RECIPE_ERR_PARSE, node ? node->line : 0u,
                                node ? node->column : 0u,
                                "'%s' must be a number, boolean or string", field);
    }

    memset(literal_out, 0, sizeof(*literal_out));
    *expression_out = NULL;
    *is_expression_out = false;

    if (node->quoted ||
        recipe_scalar_classify(node->text, node->text_length) == RECIPE_SCALAR_STRING) {
        /* A string is an expression to evaluate at execution time. */
        char *copy;
        if (node->text_length == 0u) {
            return recipe_load_fail(error, CANCESTRY_RECIPE_ERR_PARSE, node->line, node->column,
                                    "'%s' must not be an empty string", field);
        }
        if (node->text_length > CANCESTRY_RECIPE_EXPRESSION_MAX) {
            return recipe_load_fail(error, CANCESTRY_RECIPE_ERR_PARSE, node->line, node->column,
                                    "'%s' expression too long (max %u)", field,
                                    (unsigned)CANCESTRY_RECIPE_EXPRESSION_MAX);
        }
        copy = recipe_string_copy(arena, node->text, node->text_length);
        if (copy == NULL) {
            return recipe_load_fail_here(NULL, error, CANCESTRY_RECIPE_ERR_NO_MEMORY, 0u, 0u, "%s",
                                         "out of memory");
        }
        *is_expression_out = true;
        *expression_out = copy;
        return true;
    }

    if (recipe_scalar_classify(node->text, node->text_length) == RECIPE_SCALAR_BOOL) {
        literal_out->kind = CANCESTRY_VALUE_KIND_BOOL;
        literal_out->value.boolean = (node->text_length == 4u);
        return true;
    }

    if (recipe_scalar_classify(node->text, node->text_length) == RECIPE_SCALAR_INT) {
        uint64_t magnitude = 0u;
        bool negative = false;
        size_t offset = 0u;
        if (node->text[0] == '+' || node->text[0] == '-') {
            negative = (node->text[0] == '-');
            offset = 1u;
        }
        if (node->text_length - offset > 2u && node->text[offset] == '0' &&
            (node->text[offset + 1u] == 'x' || node->text[offset + 1u] == 'X')) {
            size_t i;
            for (i = offset + 2u; i < node->text_length; ++i) {
                int digit = recipe_hex_digit(node->text[i]);
                if (digit < 0 || magnitude > (UINT64_MAX - (uint64_t)digit) / 16u) {
                    return recipe_load_fail(error, CANCESTRY_RECIPE_ERR_PARSE, node->line,
                                            node->column, "'%s' is out of range", field);
                }
                magnitude = (magnitude * 16u) + (uint64_t)digit;
            }
            if ((negative && magnitude > (uint64_t)INT64_MAX + 1u) ||
                (!negative && magnitude > (uint64_t)INT64_MAX)) {
                /* Hex magnitudes beyond int64 have no exact REAL form. */
                return recipe_load_fail(error, CANCESTRY_RECIPE_ERR_PARSE, node->line,
                                        node->column, "'%s' is out of range", field);
            }
        } else {
            /* Decimal integers beyond int64 (even beyond uint64) degrade to
             * REAL literals rather than being rejected. */
            bool fits_u64 = recipe_parse_uint64(node->text + offset,
                                                node->text_length - offset, &magnitude);
            if (fits_u64 &&
                ((negative && magnitude <= (uint64_t)INT64_MAX + 1u) ||
                 (!negative && magnitude <= (uint64_t)INT64_MAX))) {
                literal_out->kind = CANCESTRY_VALUE_KIND_INT;
                literal_out->value.integer =
                    (negative && magnitude == (uint64_t)INT64_MAX + 1u)
                        ? INT64_MIN
                        : (negative ? -((int64_t)magnitude) : (int64_t)magnitude);
                return true;
            }
            literal_out->kind = CANCESTRY_VALUE_KIND_REAL;
            if (fits_u64) {
                /* Fits uint64 but not int64: convert through uint64. */
                literal_out->value.real = negative ? -((double)magnitude) : (double)magnitude;
            } else {
                /* Beyond uint64: parse the digits directly as a double. */
                double real_value = 0.0;
                char *end = NULL;
                char buffer[32];
                size_t digits = node->text_length - offset;
                if (digits >= sizeof(buffer)) {
                    return recipe_load_fail(error, CANCESTRY_RECIPE_ERR_PARSE, node->line,
                                            node->column, "'%s' is out of range", field);
                }
                memcpy(buffer, node->text + offset, digits);
                buffer[digits] = '\0';
                errno = 0;
                real_value = strtod(buffer, &end);
                if (end == buffer || *end != '\0' || errno == ERANGE ||
                    real_value < -DBL_MAX || real_value > DBL_MAX) {
                    return recipe_load_fail(error, CANCESTRY_RECIPE_ERR_PARSE, node->line,
                                            node->column, "'%s' is out of range", field);
                }
                literal_out->value.real = negative ? -real_value : real_value;
            }
            return true;
        }
        if (negative && magnitude <= (uint64_t)INT64_MAX + 1u) {
            literal_out->kind = CANCESTRY_VALUE_KIND_INT;
            literal_out->value.integer =
                (magnitude == (uint64_t)INT64_MAX + 1u)
                    ? INT64_MIN
                    : -((int64_t)magnitude);
            return true;
        }
        if (!negative && magnitude <= (uint64_t)INT64_MAX) {
            literal_out->kind = CANCESTRY_VALUE_KIND_INT;
            literal_out->value.integer = (int64_t)magnitude;
            return true;
        }
        return true;
    }

    /* Float scalar. */
    {
        double value = 0.0;
        if (!recipe_field_double(node, &value, error, field)) {
            return false;
        }
        literal_out->kind = CANCESTRY_VALUE_KIND_REAL;
        literal_out->value.real = value;
        return true;
    }
}

static bool recipe_check_unknown_keys(const recipe_ast_node_t *map,
                                      const char *const *allowed,
                                      size_t allowed_count,
                                      const char *where,
                                      cancestry_recipe_load_error_t *error)
{
    size_t i;

    for (i = 0u; i < map->pair_count; ++i) {
        size_t a;
        bool known = false;
        for (a = 0u; a < allowed_count; ++a) {
            if (strlen(allowed[a]) == map->pairs[i].key_length &&
                memcmp(allowed[a], map->pairs[i].key, map->pairs[i].key_length) == 0) {
                known = true;
                break;
            }
        }
        if (!known) {
            return recipe_load_fail(error, CANCESTRY_RECIPE_ERR_PARSE, map->pairs[i].line,
                                    map->pairs[i].column, "unknown field '%.*s' in %s",
                                    (int)map->pairs[i].key_length, map->pairs[i].key, where);
        }
    }
    return true;
}

/* ------------------------------------------------------------------------- */
/* Validation and arena build                                                */
/* ------------------------------------------------------------------------- */

typedef struct recipe_trigger_spec {
    cancestry_recipe_trigger_event_t event;
    char *interface;
    char *message;
    char *signal;
    char *timer;
    uint32_t timeout_ms;
    bool has_timeout_ms;
} recipe_trigger_spec_t;

typedef struct recipe_condition_spec {
    char *expression;
} recipe_condition_spec_t;

typedef struct recipe_operand_spec {
    bool is_expression;
    cancestry_value_t literal;
    char *expression;
} recipe_operand_spec_t;

typedef struct recipe_signal_value_spec {
    char *name;
    recipe_operand_spec_t operand;
} recipe_signal_value_spec_t;

/*
 * Loader-side action representation. The fields are flat; the final build
 * maps them onto the public cancestry_recipe_action_t union:
 *   interface  send_message interface, or NULL
 *   message    send_message message name / log message text, or NULL
 *   target     set_signal signal, set_variable variable, *_timer timer,
 *              raise_fault code, or NULL
 *   operand    set_signal / set_variable value
 */
typedef struct recipe_action_spec {
    const recipe_ast_node_t *node;
    cancestry_recipe_action_kind_t kind;
    char *interface;
    char *message;
    char *target;
    recipe_operand_spec_t operand;
    recipe_signal_value_spec_t *values;
    size_t value_count;
    uint32_t duration_ms;
    bool has_duration_ms;
    bool repeat;
    cancestry_recipe_log_level_t level;
    cancestry_fault_severity_t severity;
} recipe_action_spec_t;

typedef struct recipe_spec {
    const recipe_ast_node_t *node;
    char *name;
    char *description;
    bool enabled;
    recipe_trigger_spec_t trigger;
    recipe_condition_spec_t *conditions;
    size_t condition_count;
    recipe_action_spec_t *actions;
    size_t action_count;
    cancestry_recipe_on_error_t on_error;
} recipe_spec_t;

typedef struct recipe_set_spec {
    recipe_spec_t *recipes;
    size_t recipe_count;
    size_t total_conditions;
    size_t total_actions;
    size_t total_values;
} recipe_set_spec_t;

static const char *const RECIPE_TOP_KEYS[] = {"schema_version", "recipes"};
static const char *const RECIPE_RECIPE_KEYS[] = {"name",       "description", "enabled",
                                                 "trigger",    "conditions",  "actions",
                                                 "on_error"};
static const char *const RECIPE_TRIGGER_KEYS[] = {"event",    "interface", "message",
                                                  "signal",   "timer",     "timeout_ms"};
static const char *const RECIPE_CONDITION_KEYS[] = {"expression"};
static const char *const RECIPE_SEND_KEYS[] = {"interface", "message", "signals"};
static const char *const RECIPE_SET_SIGNAL_KEYS[] = {"signal", "value"};
static const char *const RECIPE_SET_VARIABLE_KEYS[] = {"variable", "value"};
static const char *const RECIPE_START_TIMER_KEYS[] = {"timer", "duration_ms", "repeat"};
static const char *const RECIPE_TIMER_NAME_KEYS[] = {"timer"};
static const char *const RECIPE_LOG_KEYS[] = {"level", "message"};
static const char *const RECIPE_RAISE_FAULT_KEYS[] = {"code", "severity"};

/** Fail with a "missing required field" error positioned at @p node. */
static bool recipe_missing(const recipe_ast_node_t *node,
                           cancestry_recipe_load_error_t *error,
                           const char *format,
                           ...)
{
    va_list args;

    if (error == NULL) {
        return false;
    }
    error->status = CANCESTRY_RECIPE_ERR_PARSE;
    error->line = (node != NULL) ? node->line : 0u;
    error->column = (node != NULL) ? node->column : 0u;
    va_start(args, format);
    (void)vsnprintf(error->message, sizeof(error->message), format, args);
    va_end(args);
    return false;
}

/** Read a required non-empty string field bounded by CANCESTRY_RECIPE_NAME_MAX. */
static bool recipe_read_name(const recipe_ast_node_t *map,
                             const char *key,
                             recipe_arena_t *arena,
                             char **value_out,
                             cancestry_recipe_load_error_t *error,
                             const char *what)
{
    size_t index = 0u;
    size_t length = 0u;

    if (!recipe_ast_find_pair(map, key, &index)) {
        return recipe_missing(map, error, "%s is missing required field '%s'", what, key);
    }
    if (!recipe_field_string(map->pairs[index].value, arena, value_out, &length) ||
        length == 0u) {
        return recipe_load_fail(error, CANCESTRY_RECIPE_ERR_PARSE, map->pairs[index].line,
                                map->pairs[index].column, "%s '%s' must be a non-empty string",
                                what, key);
    }
    if (length > CANCESTRY_RECIPE_NAME_MAX) {
        return recipe_load_fail(error, CANCESTRY_RECIPE_ERR_PARSE, map->pairs[index].line,
                                map->pairs[index].column, "%s '%s' too long (max %u)", what, key,
                                (unsigned)CANCESTRY_RECIPE_NAME_MAX);
    }
    return true;
}

static bool recipe_parse_trigger(const recipe_ast_node_t *node,
                                 recipe_arena_t *arena,
                                 recipe_trigger_spec_t *spec,
                                 cancestry_recipe_load_error_t *error)
{
    size_t index = 0u;

    memset(spec, 0, sizeof(*spec));
    if (node->kind != RECIPE_AST_MAP) {
        return recipe_load_fail(error, CANCESTRY_RECIPE_ERR_PARSE, node->line, node->column,
                                "trigger must be a mapping");
    }
    /* recipe-spec.md section 2: the field "type" is not allowed in v0.2.1. */
    if (recipe_ast_find_pair(node, "type", &index)) {
        return recipe_load_fail(error, CANCESTRY_RECIPE_ERR_PARSE, node->pairs[index].line,
                                node->pairs[index].column,
                                "the field 'type' is not allowed in a trigger; use 'event' "
                                "(recipe-spec.md section 2)");
    }
    if (!recipe_check_unknown_keys(node, RECIPE_TRIGGER_KEYS,
                                   sizeof(RECIPE_TRIGGER_KEYS) / sizeof(RECIPE_TRIGGER_KEYS[0]),
                                   "trigger", error)) {
        return false;
    }
    if (!recipe_ast_find_pair(node, "event", &index)) {
        return recipe_missing(node, error, "trigger is missing required field 'event'");
    }
    {
        char *text = NULL;
        size_t length = 0u;
        if (!recipe_field_string(node->pairs[index].value, arena, &text, &length)) {
            return recipe_load_fail(error, CANCESTRY_RECIPE_ERR_PARSE, node->pairs[index].line,
                                    node->pairs[index].column,
                                    "trigger 'event' must be a string");
        }
        if (length == 6u && memcmp(text, "can_rx", 6u) == 0) {
            spec->event = CANCESTRY_RECIPE_TRIGGER_CAN_RX;
        } else if (length == 14u && memcmp(text, "signal_changed", 14u) == 0) {
            spec->event = CANCESTRY_RECIPE_TRIGGER_SIGNAL_CHANGED;
        } else if (length == 13u && memcmp(text, "timer_expired", 13u) == 0) {
            spec->event = CANCESTRY_RECIPE_TRIGGER_TIMER_EXPIRED;
        } else if (length == 7u && memcmp(text, "timeout", 7u) == 0) {
            spec->event = CANCESTRY_RECIPE_TRIGGER_TIMEOUT;
        } else if (length == 12u && memcmp(text, "fault_raised", 12u) == 0) {
            spec->event = CANCESTRY_RECIPE_TRIGGER_FAULT_RAISED;
        } else if (length == 18u && memcmp(text, "power_mode_changed", 18u) == 0) {
            spec->event = CANCESTRY_RECIPE_TRIGGER_POWER_MODE_CHANGED;
        } else {
            return recipe_load_fail(
                error, CANCESTRY_RECIPE_ERR_PARSE, node->pairs[index].line,
                node->pairs[index].column,
                "trigger 'event' must be one of can_rx, signal_changed, timer_expired, "
                "timeout, fault_raised, power_mode_changed");
        }
    }
    if (recipe_ast_find_pair(node, "interface", &index)) {
        if (!recipe_field_string(node->pairs[index].value, arena, &spec->interface, NULL) ||
            spec->interface[0] == '\0') {
            return recipe_load_fail(error, CANCESTRY_RECIPE_ERR_PARSE, node->pairs[index].line,
                                    node->pairs[index].column,
                                    "trigger 'interface' must be a non-empty string");
        }
        if (strlen(spec->interface) > CANCESTRY_RECIPE_NAME_MAX) {
            return recipe_load_fail(error, CANCESTRY_RECIPE_ERR_PARSE, node->pairs[index].line,
                                    node->pairs[index].column,
                                    "trigger 'interface' too long (max %u)",
                                    (unsigned)CANCESTRY_RECIPE_NAME_MAX);
        }
    }
    if (recipe_ast_find_pair(node, "message", &index)) {
        if (!recipe_field_string(node->pairs[index].value, arena, &spec->message, NULL) ||
            spec->message[0] == '\0') {
            return recipe_load_fail(error, CANCESTRY_RECIPE_ERR_PARSE, node->pairs[index].line,
                                    node->pairs[index].column,
                                    "trigger 'message' must be a non-empty string");
        }
        if (strlen(spec->message) > CANCESTRY_RECIPE_NAME_MAX) {
            return recipe_load_fail(error, CANCESTRY_RECIPE_ERR_PARSE, node->pairs[index].line,
                                    node->pairs[index].column,
                                    "trigger 'message' too long (max %u)",
                                    (unsigned)CANCESTRY_RECIPE_NAME_MAX);
        }
    }
    if (recipe_ast_find_pair(node, "signal", &index)) {
        if (!recipe_field_string(node->pairs[index].value, arena, &spec->signal, NULL) ||
            spec->signal[0] == '\0') {
            return recipe_load_fail(error, CANCESTRY_RECIPE_ERR_PARSE, node->pairs[index].line,
                                    node->pairs[index].column,
                                    "trigger 'signal' must be a non-empty string");
        }
        if (strlen(spec->signal) > CANCESTRY_RECIPE_NAME_MAX) {
            return recipe_load_fail(error, CANCESTRY_RECIPE_ERR_PARSE, node->pairs[index].line,
                                    node->pairs[index].column,
                                    "trigger 'signal' too long (max %u)",
                                    (unsigned)CANCESTRY_RECIPE_NAME_MAX);
        }
    }
    if (recipe_ast_find_pair(node, "timer", &index)) {
        if (!recipe_field_string(node->pairs[index].value, arena, &spec->timer, NULL) ||
            spec->timer[0] == '\0') {
            return recipe_load_fail(error, CANCESTRY_RECIPE_ERR_PARSE, node->pairs[index].line,
                                    node->pairs[index].column,
                                    "trigger 'timer' must be a non-empty string");
        }
        if (strlen(spec->timer) > CANCESTRY_RECIPE_NAME_MAX) {
            return recipe_load_fail(error, CANCESTRY_RECIPE_ERR_PARSE, node->pairs[index].line,
                                    node->pairs[index].column,
                                    "trigger 'timer' too long (max %u)",
                                    (unsigned)CANCESTRY_RECIPE_NAME_MAX);
        }
    }
    if (recipe_ast_find_pair(node, "timeout_ms", &index)) {
        if (!recipe_field_uint(node->pairs[index].value, 1, (int64_t)0xFFFFFFFF, &spec->timeout_ms,
                               error, "timeout_ms")) {
            return false;
        }
        spec->has_timeout_ms = true;
    }

    /* Schema conditional rules (trigger allOf). */
    if (spec->event == CANCESTRY_RECIPE_TRIGGER_SIGNAL_CHANGED && spec->signal == NULL) {
        return recipe_missing(node, error,
                              "a signal_changed trigger requires the 'signal' field");
    }
    if (spec->event == CANCESTRY_RECIPE_TRIGGER_TIMER_EXPIRED && spec->timer == NULL) {
        return recipe_missing(node, error,
                              "a timer_expired trigger requires the 'timer' field");
    }
    if (spec->event == CANCESTRY_RECIPE_TRIGGER_TIMEOUT && !spec->has_timeout_ms) {
        return recipe_missing(node, error, "a timeout trigger requires the 'timeout_ms' field");
    }
    return true;
}

static bool recipe_parse_condition(const recipe_ast_node_t *node,
                                   recipe_arena_t *arena,
                                   recipe_condition_spec_t *spec,
                                   cancestry_recipe_load_error_t *error)
{
    size_t index = 0u;

    memset(spec, 0, sizeof(*spec));
    if (node->kind != RECIPE_AST_MAP) {
        return recipe_load_fail(error, CANCESTRY_RECIPE_ERR_PARSE, node->line, node->column,
                                "condition must be a mapping");
    }
    if (!recipe_check_unknown_keys(
            node, RECIPE_CONDITION_KEYS,
            sizeof(RECIPE_CONDITION_KEYS) / sizeof(RECIPE_CONDITION_KEYS[0]), "condition",
            error)) {
        return false;
    }
    if (!recipe_ast_find_pair(node, "expression", &index)) {
        return recipe_missing(node, error, "condition is missing required field 'expression'");
    }
    if (!recipe_field_string(node->pairs[index].value, arena, &spec->expression, NULL) ||
        spec->expression[0] == '\0') {
        return recipe_load_fail(error, CANCESTRY_RECIPE_ERR_PARSE, node->pairs[index].line,
                                node->pairs[index].column,
                                "condition 'expression' must be a non-empty string");
    }
    if (strlen(spec->expression) > CANCESTRY_RECIPE_EXPRESSION_MAX) {
        return recipe_load_fail(error, CANCESTRY_RECIPE_ERR_PARSE, node->pairs[index].line,
                                node->pairs[index].column,
                                "condition 'expression' too long (max %u)",
                                (unsigned)CANCESTRY_RECIPE_EXPRESSION_MAX);
    }
    return true;
}

/** Parse the signals map of a send_message action. */
static bool recipe_parse_signal_values(const recipe_ast_node_t *node,
                                       recipe_arena_t *arena,
                                       recipe_action_spec_t *spec,
                                       cancestry_recipe_load_error_t *error)
{
    size_t i;

    if (node->kind != RECIPE_AST_MAP || node->pair_count == 0u) {
        return recipe_load_fail(error, CANCESTRY_RECIPE_ERR_PARSE, node->line, node->column,
                                "send_message 'signals' must be a non-empty mapping");
    }
    spec->value_count = node->pair_count;
    spec->values = (recipe_signal_value_spec_t *)recipe_arena_alloc(
        arena, node->pair_count * sizeof(*spec->values));
    if (spec->values == NULL) {
        return recipe_load_fail(error, CANCESTRY_RECIPE_ERR_NO_MEMORY, node->line, node->column,
                                "out of memory");
    }
    for (i = 0u; i < node->pair_count; ++i) {
        const recipe_ast_pair_t *pair = &node->pairs[i];

        memset(&spec->values[i], 0, sizeof(spec->values[i]));
        if (pair->key_length == 0u || pair->key_length > CANCESTRY_RECIPE_NAME_MAX) {
            return recipe_load_fail(error, CANCESTRY_RECIPE_ERR_PARSE, pair->line, pair->column,
                                    "send_message signal names must be 1..%u bytes",
                                    (unsigned)CANCESTRY_RECIPE_NAME_MAX);
        }
        spec->values[i].name = recipe_string_copy(arena, pair->key, pair->key_length);
        if (spec->values[i].name == NULL) {
            return recipe_load_fail(error, CANCESTRY_RECIPE_ERR_NO_MEMORY, pair->line,
                                    pair->column, "out of memory");
        }
        if (!recipe_field_operand(pair->value, arena, &spec->values[i].operand.is_expression,
                                  &spec->values[i].operand.literal, &spec->values[i].operand.expression,
                                  error, "signals value")) {
            return false;
        }
    }
    return true;
}

static bool recipe_parse_action(const recipe_ast_node_t *node,
                                recipe_arena_t *arena,
                                recipe_action_spec_t *spec,
                                cancestry_recipe_load_error_t *error)
{
    const recipe_ast_pair_t *pair;
    const recipe_ast_node_t *body;

    memset(spec, 0, sizeof(*spec));
    spec->node = node;
    if (node->kind != RECIPE_AST_MAP) {
        return recipe_load_fail(error, CANCESTRY_RECIPE_ERR_PARSE, node->line, node->column,
                                "action must be a mapping");
    }
    /* The schema's recipe_action oneOf: exactly one action key per item. */
    if (node->pair_count != 1u) {
        return recipe_load_fail(error, CANCESTRY_RECIPE_ERR_PARSE, node->line, node->column,
                                "an action must have exactly one key, found %u",
                                (unsigned)node->pair_count);
    }
    pair = &node->pairs[0];

    if (pair->key_length == 10u && memcmp(pair->key, "transition", 10u) == 0) {
        /* SW-FR-RECIPE-006: explicitly rejected, with a dedicated error,
         * regardless of the body's shape. */
        return recipe_load_fail(error, CANCESTRY_RECIPE_ERR_PARSE, pair->line, pair->column,
                                "the transition action is not allowed in recipes "
                                "(SW-FR-RECIPE-006)");
    }

    body = pair->value;
    if (body == NULL || body->kind != RECIPE_AST_MAP) {
        return recipe_load_fail(error, CANCESTRY_RECIPE_ERR_PARSE, pair->line, pair->column,
                                "action '%.*s' must be a mapping", (int)pair->key_length,
                                pair->key);
    }

    if (pair->key_length == 12u && memcmp(pair->key, "send_message", 12u) == 0) {
        spec->kind = CANCESTRY_RECIPE_ACTION_SEND_MESSAGE;
        if (!recipe_check_unknown_keys(
                body, RECIPE_SEND_KEYS,
                sizeof(RECIPE_SEND_KEYS) / sizeof(RECIPE_SEND_KEYS[0]), "send_message", error)) {
            return false;
        }
        if (!recipe_read_name(body, "interface", arena, &spec->interface, error,
                              "send_message")) {
            return false;
        }
        if (!recipe_read_name(body, "message", arena, &spec->message, error, "send_message")) {
            return false;
        }
        {
            size_t index = 0u;
            if (!recipe_ast_find_pair(body, "signals", &index)) {
                return recipe_missing(body, error,
                                      "send_message is missing required field 'signals'");
            }
            if (!recipe_parse_signal_values(body->pairs[index].value, arena, spec, error)) {
                return false;
            }
        }
        return true;
    }

    if (pair->key_length == 10u && memcmp(pair->key, "set_signal", 10u) == 0) {
        size_t index = 0u;
        spec->kind = CANCESTRY_RECIPE_ACTION_SET_SIGNAL;
        if (!recipe_check_unknown_keys(body, RECIPE_SET_SIGNAL_KEYS,
                                       sizeof(RECIPE_SET_SIGNAL_KEYS) /
                                           sizeof(RECIPE_SET_SIGNAL_KEYS[0]),
                                       "set_signal", error)) {
            return false;
        }
        if (!recipe_read_name(body, "signal", arena, &spec->target, error, "set_signal")) {
            return false;
        }
        if (!recipe_ast_find_pair(body, "value", &index)) {
            return recipe_missing(body, error, "set_signal is missing required field 'value'");
        }
        return recipe_field_operand(body->pairs[index].value, arena, &spec->operand.is_expression,
                                    &spec->operand.literal, &spec->operand.expression, error,
                                    "value");
    }

    if (pair->key_length == 12u && memcmp(pair->key, "set_variable", 12u) == 0) {
        size_t index = 0u;
        spec->kind = CANCESTRY_RECIPE_ACTION_SET_VARIABLE;
        if (!recipe_check_unknown_keys(body, RECIPE_SET_VARIABLE_KEYS,
                                       sizeof(RECIPE_SET_VARIABLE_KEYS) /
                                           sizeof(RECIPE_SET_VARIABLE_KEYS[0]),
                                       "set_variable", error)) {
            return false;
        }
        if (!recipe_read_name(body, "variable", arena, &spec->target, error, "set_variable")) {
            return false;
        }
        if (!recipe_ast_find_pair(body, "value", &index)) {
            return recipe_missing(body, error,
                                  "set_variable is missing required field 'value'");
        }
        return recipe_field_operand(body->pairs[index].value, arena, &spec->operand.is_expression,
                                    &spec->operand.literal, &spec->operand.expression, error,
                                    "value");
    }

    if (pair->key_length == 11u && memcmp(pair->key, "start_timer", 11u) == 0) {
        size_t index = 0u;
        spec->kind = CANCESTRY_RECIPE_ACTION_START_TIMER;
        if (!recipe_check_unknown_keys(body, RECIPE_START_TIMER_KEYS,
                                       sizeof(RECIPE_START_TIMER_KEYS) /
                                           sizeof(RECIPE_START_TIMER_KEYS[0]),
                                       "start_timer", error)) {
            return false;
        }
        if (!recipe_read_name(body, "timer", arena, &spec->target, error, "start_timer")) {
            return false;
        }
        if (recipe_ast_find_pair(body, "duration_ms", &index)) {
            if (!recipe_field_uint(body->pairs[index].value, 1, (int64_t)0xFFFFFFFF,
                                   &spec->duration_ms, error, "duration_ms")) {
                return false;
            }
            spec->has_duration_ms = true;
        }
        if (recipe_ast_find_pair(body, "repeat", &index)) {
            if (!recipe_field_bool(body->pairs[index].value, &spec->repeat, error, "repeat")) {
                return false;
            }
        }
        return true;
    }

    if ((pair->key_length == 10u && memcmp(pair->key, "stop_timer", 10u) == 0) ||
        (pair->key_length == 11u && memcmp(pair->key, "reset_timer", 11u) == 0)) {
        spec->kind = (pair->key_length == 10u) ? CANCESTRY_RECIPE_ACTION_STOP_TIMER
                                               : CANCESTRY_RECIPE_ACTION_RESET_TIMER;
        if (!recipe_check_unknown_keys(
                body, RECIPE_TIMER_NAME_KEYS,
                sizeof(RECIPE_TIMER_NAME_KEYS) / sizeof(RECIPE_TIMER_NAME_KEYS[0]),
                (spec->kind == CANCESTRY_RECIPE_ACTION_STOP_TIMER) ? "stop_timer" : "reset_timer",
                error)) {
            return false;
        }
        return recipe_read_name(body, "timer", arena, &spec->target, error,
                                (spec->kind == CANCESTRY_RECIPE_ACTION_STOP_TIMER) ? "stop_timer"
                                                                                    : "reset_timer");
    }

    if (pair->key_length == 3u && memcmp(pair->key, "log", 3u) == 0) {
        size_t index = 0u;
        char *level_text = NULL;
        size_t level_length = 0u;

        spec->kind = CANCESTRY_RECIPE_ACTION_LOG;
        if (!recipe_check_unknown_keys(body, RECIPE_LOG_KEYS,
                                       sizeof(RECIPE_LOG_KEYS) / sizeof(RECIPE_LOG_KEYS[0]),
                                       "log", error)) {
            return false;
        }
        if (!recipe_ast_find_pair(body, "level", &index)) {
            return recipe_missing(body, error, "log is missing required field 'level'");
        }
        if (!recipe_field_string(body->pairs[index].value, arena, &level_text, &level_length)) {
            return recipe_load_fail(error, CANCESTRY_RECIPE_ERR_PARSE, body->pairs[index].line,
                                    body->pairs[index].column, "log 'level' must be a string");
        }
        if (level_length == 4u && memcmp(level_text, "info", 4u) == 0) {
            spec->level = CANCESTRY_RECIPE_LOG_INFO;
        } else if (level_length == 7u && memcmp(level_text, "warning", 7u) == 0) {
            spec->level = CANCESTRY_RECIPE_LOG_WARNING;
        } else if (level_length == 5u && memcmp(level_text, "error", 5u) == 0) {
            spec->level = CANCESTRY_RECIPE_LOG_ERROR;
        } else {
            return recipe_load_fail(error, CANCESTRY_RECIPE_ERR_PARSE, body->pairs[index].line,
                                    body->pairs[index].column,
                                    "log 'level' must be one of info, warning, error");
        }
        if (!recipe_ast_find_pair(body, "message", &index)) {
            return recipe_missing(body, error, "log is missing required field 'message'");
        }
        if (!recipe_field_string(body->pairs[index].value, arena, &spec->message, NULL) ||
            spec->message[0] == '\0') {
            return recipe_load_fail(error, CANCESTRY_RECIPE_ERR_PARSE, body->pairs[index].line,
                                    body->pairs[index].column,
                                    "log 'message' must be a non-empty string");
        }
        if (strlen(spec->message) > CANCESTRY_RECIPE_LOG_MESSAGE_MAX) {
            return recipe_load_fail(error, CANCESTRY_RECIPE_ERR_PARSE, body->pairs[index].line,
                                    body->pairs[index].column,
                                    "log 'message' too long (max %u)",
                                    (unsigned)CANCESTRY_RECIPE_LOG_MESSAGE_MAX);
        }
        return true;
    }

    if (pair->key_length == 11u && memcmp(pair->key, "raise_fault", 11u) == 0) {
        size_t index = 0u;
        char *severity_text = NULL;
        size_t severity_length = 0u;

        spec->kind = CANCESTRY_RECIPE_ACTION_RAISE_FAULT;
        if (!recipe_check_unknown_keys(body, RECIPE_RAISE_FAULT_KEYS,
                                       sizeof(RECIPE_RAISE_FAULT_KEYS) /
                                           sizeof(RECIPE_RAISE_FAULT_KEYS[0]),
                                       "raise_fault", error)) {
            return false;
        }
        if (!recipe_read_name(body, "code", arena, &spec->target, error, "raise_fault")) {
            return false;
        }
        if (!recipe_ast_find_pair(body, "severity", &index)) {
            return recipe_missing(body, error, "raise_fault is missing required field 'severity'");
        }
        if (!recipe_field_string(body->pairs[index].value, arena, &severity_text,
                                 &severity_length)) {
            return recipe_load_fail(error, CANCESTRY_RECIPE_ERR_PARSE, body->pairs[index].line,
                                    body->pairs[index].column,
                                    "raise_fault 'severity' must be a string");
        }
        if (severity_length == 7u && memcmp(severity_text, "warning", 7u) == 0) {
            spec->severity = CANCESTRY_FAULT_SEVERITY_WARNING;
        } else if (severity_length == 5u && memcmp(severity_text, "error", 5u) == 0) {
            spec->severity = CANCESTRY_FAULT_SEVERITY_ERROR;
        } else if (severity_length == 8u && memcmp(severity_text, "critical", 8u) == 0) {
            spec->severity = CANCESTRY_FAULT_SEVERITY_CRITICAL;
        } else {
            return recipe_load_fail(error, CANCESTRY_RECIPE_ERR_PARSE, body->pairs[index].line,
                                    body->pairs[index].column,
                                    "raise_fault 'severity' must be one of warning, error, "
                                    "critical");
        }
        return true;
    }

    return recipe_load_fail(error, CANCESTRY_RECIPE_ERR_PARSE, pair->line, pair->column,
                            "unknown action '%.*s'", (int)pair->key_length, pair->key);
}

static bool recipe_parse_recipe(const recipe_ast_node_t *node,
                                recipe_arena_t *arena,
                                recipe_spec_t *spec,
                                cancestry_recipe_load_error_t *error)
{
    size_t index = 0u;
    size_t i;

    memset(spec, 0, sizeof(*spec));
    spec->node = node;
    spec->enabled = true;
    spec->on_error = CANCESTRY_RECIPE_ON_ERROR_STOP;

    if (node->kind != RECIPE_AST_MAP) {
        return recipe_load_fail(error, CANCESTRY_RECIPE_ERR_PARSE, node->line, node->column,
                                "recipe must be a mapping");
    }
    if (!recipe_check_unknown_keys(node, RECIPE_RECIPE_KEYS,
                                   sizeof(RECIPE_RECIPE_KEYS) / sizeof(RECIPE_RECIPE_KEYS[0]),
                                   "recipe", error)) {
        return false;
    }
    if (!recipe_read_name(node, "name", arena, &spec->name, error, "recipe")) {
        return false;
    }
    if (recipe_ast_find_pair(node, "description", &index)) {
        if (!recipe_field_string(node->pairs[index].value, arena, &spec->description, NULL)) {
            return recipe_load_fail(error, CANCESTRY_RECIPE_ERR_PARSE, node->pairs[index].line,
                                    node->pairs[index].column,
                                    "recipe 'description' must be a string");
        }
        if (strlen(spec->description) > CANCESTRY_RECIPE_DESCRIPTION_MAX) {
            return recipe_load_fail(error, CANCESTRY_RECIPE_ERR_PARSE, node->pairs[index].line,
                                    node->pairs[index].column, "recipe description too long");
        }
    }
    if (recipe_ast_find_pair(node, "enabled", &index)) {
        if (!recipe_field_bool(node->pairs[index].value, &spec->enabled, error, "enabled")) {
            return false;
        }
    }
    if (!recipe_ast_find_pair(node, "trigger", &index)) {
        return recipe_missing(node, error, "recipe '%s' is missing required field 'trigger'",
                              spec->name);
    }
    if (!recipe_parse_trigger(node->pairs[index].value, arena, &spec->trigger, error)) {
        return false;
    }
    if (recipe_ast_find_pair(node, "conditions", &index)) {
        const recipe_ast_node_t *conditions = node->pairs[index].value;

        if (conditions->kind != RECIPE_AST_SEQ) {
            return recipe_load_fail(error, CANCESTRY_RECIPE_ERR_PARSE, conditions->line,
                                    conditions->column,
                                    "recipe 'conditions' must be a sequence");
        }
        spec->condition_count = conditions->item_count;
        if (spec->condition_count > 0u) {
            spec->conditions = (recipe_condition_spec_t *)recipe_arena_alloc(
                arena, spec->condition_count * sizeof(*spec->conditions));
            if (spec->conditions == NULL) {
                return recipe_load_fail(error, CANCESTRY_RECIPE_ERR_NO_MEMORY, conditions->line,
                                        conditions->column, "out of memory");
            }
            for (i = 0u; i < spec->condition_count; ++i) {
                if (!recipe_parse_condition(conditions->items[i], arena, &spec->conditions[i],
                                            error)) {
                    return false;
                }
            }
        }
    }
    if (!recipe_ast_find_pair(node, "actions", &index)) {
        return recipe_missing(node, error, "recipe '%s' is missing required field 'actions'",
                              spec->name);
    }
    {
        const recipe_ast_node_t *actions = node->pairs[index].value;

        if (actions->kind != RECIPE_AST_SEQ || actions->item_count == 0u) {
            return recipe_load_fail(error, CANCESTRY_RECIPE_ERR_PARSE, actions->line,
                                    actions->column,
                                    "recipe 'actions' must be a non-empty sequence");
        }
        if (actions->item_count > UINT16_MAX) {
            return recipe_load_fail(error, CANCESTRY_RECIPE_ERR_PARSE, actions->line,
                                    actions->column, "too many actions (max %u)",
                                    (unsigned)UINT16_MAX);
        }
        spec->action_count = actions->item_count;
        spec->actions = (recipe_action_spec_t *)recipe_arena_alloc(
            arena, spec->action_count * sizeof(*spec->actions));
        if (spec->actions == NULL) {
            return recipe_load_fail(error, CANCESTRY_RECIPE_ERR_NO_MEMORY, actions->line,
                                    actions->column, "out of memory");
        }
        for (i = 0u; i < spec->action_count; ++i) {
            if (!recipe_parse_action(actions->items[i], arena, &spec->actions[i], error)) {
                return false;
            }
        }
    }
    if (recipe_ast_find_pair(node, "on_error", &index)) {
        char *text = NULL;
        size_t length = 0u;

        if (!recipe_field_string(node->pairs[index].value, arena, &text, &length)) {
            return recipe_load_fail(error, CANCESTRY_RECIPE_ERR_PARSE, node->pairs[index].line,
                                    node->pairs[index].column, "recipe 'on_error' must be a "
                                                                "string");
        }
        if (length == 4u && memcmp(text, "stop", 4u) == 0) {
            spec->on_error = CANCESTRY_RECIPE_ON_ERROR_STOP;
        } else if (length == 8u && memcmp(text, "continue", 8u) == 0) {
            spec->on_error = CANCESTRY_RECIPE_ON_ERROR_CONTINUE;
        } else {
            return recipe_load_fail(error, CANCESTRY_RECIPE_ERR_PARSE, node->pairs[index].line,
                                    node->pairs[index].column,
                                    "recipe 'on_error' must be 'stop' or 'continue'");
        }
    }
    return true;
}

static bool recipe_validate_set(const recipe_ast_node_t *root,
                                recipe_arena_t *arena,
                                recipe_set_spec_t *spec,
                                cancestry_recipe_load_error_t *error)
{
    size_t index = 0u;
    const recipe_ast_node_t *recipes;
    size_t i;

    memset(spec, 0, sizeof(*spec));
    if (root->kind != RECIPE_AST_MAP) {
        return recipe_load_fail(error, CANCESTRY_RECIPE_ERR_PARSE, root->line, root->column,
                                "the document root must be a mapping");
    }
    if (!recipe_check_unknown_keys(root, RECIPE_TOP_KEYS,
                                   sizeof(RECIPE_TOP_KEYS) / sizeof(RECIPE_TOP_KEYS[0]),
                                   "document root", error)) {
        return false;
    }
    if (!recipe_ast_find_pair(root, "schema_version", &index)) {
        return recipe_missing(root, error, "missing required field 'schema_version'");
    }
    {
        const recipe_ast_node_t *version = root->pairs[index].value;
        if (version->kind != RECIPE_AST_SCALAR || version->text_length != 5u ||
            memcmp(version->text, "0.2.0", 5u) != 0) {
            return recipe_load_fail(error, CANCESTRY_RECIPE_ERR_PARSE, version->line,
                                    version->column, "'schema_version' must be \"0.2.0\"");
        }
    }
    if (!recipe_ast_find_pair(root, "recipes", &index)) {
        return recipe_missing(root, error, "missing required field 'recipes'");
    }
    recipes = root->pairs[index].value;
    if (recipes->kind != RECIPE_AST_SEQ || recipes->item_count == 0u) {
        return recipe_load_fail(error, CANCESTRY_RECIPE_ERR_PARSE, recipes->line, recipes->column,
                                "'recipes' must be a non-empty sequence");
    }
    if (recipes->item_count > UINT16_MAX) {
        return recipe_load_fail(error, CANCESTRY_RECIPE_ERR_PARSE, recipes->line, recipes->column,
                                "too many recipes (max %u)", (unsigned)UINT16_MAX);
    }
    spec->recipe_count = recipes->item_count;
    spec->recipes =
        (recipe_spec_t *)recipe_arena_alloc(arena, spec->recipe_count * sizeof(*spec->recipes));
    if (spec->recipes == NULL) {
        return recipe_load_fail(error, CANCESTRY_RECIPE_ERR_NO_MEMORY, recipes->line,
                                recipes->column, "out of memory");
    }
    for (i = 0u; i < spec->recipe_count; ++i) {
        if (!recipe_parse_recipe(recipes->items[i], arena, &spec->recipes[i], error)) {
            return false;
        }
    }

    /* Duplicate recipe names within one file are a conflict. */
    for (i = 0u; i < spec->recipe_count; ++i) {
        size_t j;
        for (j = i + 1u; j < spec->recipe_count; ++j) {
            if (strcmp(spec->recipes[i].name, spec->recipes[j].name) == 0) {
                return recipe_load_fail(error, CANCESTRY_RECIPE_ERR_CONFLICT,
                                        spec->recipes[j].node->line,
                                        spec->recipes[j].node->column,
                                        "duplicate recipe name '%s'", spec->recipes[j].name);
            }
        }
    }

    for (i = 0u; i < spec->recipe_count; ++i) {
        size_t a;
        spec->total_conditions += spec->recipes[i].condition_count;
        spec->total_actions += spec->recipes[i].action_count;
        for (a = 0u; a < spec->recipes[i].action_count; ++a) {
            spec->total_values += spec->recipes[i].actions[a].value_count;
        }
    }
    if (spec->total_actions > UINT16_MAX || spec->total_conditions > UINT16_MAX) {
        return recipe_load_fail(error, CANCESTRY_RECIPE_ERR_PARSE, root->line, root->column,
                                "too many actions or conditions in one recipe file (max %u)",
                                (unsigned)UINT16_MAX);
    }
    return true;
}

/* ------------------------------------------------------------------------- */
/* Final block build                                                         */
/* ------------------------------------------------------------------------- */

static size_t recipe_align8(size_t value)
{
    return (value + 7u) & ~((size_t)7u);
}

/** Reserve @p size bytes at an 8-byte-aligned offset. */
static void *recipe_blob_reserve(char *blob, size_t *cursor, size_t size)
{
    char *result = blob + recipe_align8(*cursor);

    *cursor = recipe_align8(*cursor) + size;
    return result;
}

static char *recipe_blob_string(char *blob, size_t *cursor, const char *text)
{
    size_t length = strlen(text);
    char *result = blob + *cursor;

    memcpy(result, text, length + 1u);
    *cursor += length + 1u;
    return result;
}

static void recipe_add_string_size(size_t *size, const char *text)
{
    if (text != NULL) {
        *size += strlen(text) + 1u;
    }
}

static char *recipe_blob_operand_string(char *blob,
                                        size_t *cursor,
                                        const recipe_operand_spec_t *operand)
{
    if (operand->is_expression && operand->expression != NULL) {
        return recipe_blob_string(blob, cursor, operand->expression);
    }
    return NULL;
}

static cancestry_recipe_set_t *recipe_build_set(const recipe_set_spec_t *spec,
                                                cancestry_recipe_load_error_t *error)
{
    size_t cursor = 0u;
    size_t size = 0u;
    cancestry_recipe_set_t *set;
    cancestry_recipe_t *recipes;
    cancestry_recipe_condition_t *conditions;
    cancestry_recipe_action_t *actions;
    cancestry_recipe_signal_value_t *values;
    size_t i;

    size = recipe_align8(sizeof(*set));
    size += recipe_align8(spec->recipe_count * sizeof(*recipes));
    size += recipe_align8(spec->total_conditions * sizeof(*conditions));
    size += recipe_align8(spec->total_actions * sizeof(*actions));
    size += recipe_align8(spec->total_values * sizeof(*values));
    for (i = 0u; i < spec->recipe_count; ++i) {
        const recipe_spec_t *rs = &spec->recipes[i];
        size_t a;
        size_t c;
        recipe_add_string_size(&size, rs->name);
        recipe_add_string_size(&size, rs->description);
        recipe_add_string_size(&size, rs->trigger.interface);
        recipe_add_string_size(&size, rs->trigger.message);
        recipe_add_string_size(&size, rs->trigger.signal);
        recipe_add_string_size(&size, rs->trigger.timer);
        for (c = 0u; c < rs->condition_count; ++c) {
            recipe_add_string_size(&size, rs->conditions[c].expression);
        }
        for (a = 0u; a < rs->action_count; ++a) {
            const recipe_action_spec_t *as = &rs->actions[a];
            size_t v;
            recipe_add_string_size(&size, as->interface);
            recipe_add_string_size(&size, as->message);
            recipe_add_string_size(&size, as->target);
            recipe_add_string_size(&size, as->operand.expression);
            for (v = 0u; v < as->value_count; ++v) {
                recipe_add_string_size(&size, as->values[v].name);
                recipe_add_string_size(&size, as->values[v].operand.expression);
            }
        }
    }

    set = (cancestry_recipe_set_t *)malloc(size);
    if (set == NULL) {
        recipe_load_fail(error, CANCESTRY_RECIPE_ERR_NO_MEMORY, 0u, 0u, "out of memory");
        return NULL;
    }
    memset(set, 0, size);

    cursor = recipe_align8(sizeof(*set));
    recipes = (cancestry_recipe_t *)recipe_blob_reserve((char *)set, &cursor,
                                                        spec->recipe_count * sizeof(*recipes));
    conditions = (cancestry_recipe_condition_t *)recipe_blob_reserve(
        (char *)set, &cursor, spec->total_conditions * sizeof(*conditions));
    actions = (cancestry_recipe_action_t *)recipe_blob_reserve(
        (char *)set, &cursor, spec->total_actions * sizeof(*actions));
    values = (cancestry_recipe_signal_value_t *)recipe_blob_reserve(
        (char *)set, &cursor, spec->total_values * sizeof(*values));

    set->recipes = recipes;
    set->recipe_count = (uint16_t)spec->recipe_count;

    {
        size_t condition_cursor = 0u;
        size_t action_cursor = 0u;
        size_t value_cursor = 0u;

        for (i = 0u; i < spec->recipe_count; ++i) {
            const recipe_spec_t *rs = &spec->recipes[i];
            cancestry_recipe_t *recipe = &recipes[i];
            size_t a;
            size_t c;

            memset(recipe, 0, sizeof(*recipe));
            recipe->name = recipe_blob_string((char *)set, &cursor, rs->name);
            recipe->description = (rs->description != NULL)
                                      ? recipe_blob_string((char *)set, &cursor, rs->description)
                                      : NULL;
            recipe->enabled = rs->enabled;
            recipe->trigger.event = rs->trigger.event;
            recipe->trigger.interface =
                (rs->trigger.interface != NULL)
                    ? recipe_blob_string((char *)set, &cursor, rs->trigger.interface)
                    : NULL;
            recipe->trigger.message =
                (rs->trigger.message != NULL)
                    ? recipe_blob_string((char *)set, &cursor, rs->trigger.message)
                    : NULL;
            recipe->trigger.signal =
                (rs->trigger.signal != NULL)
                    ? recipe_blob_string((char *)set, &cursor, rs->trigger.signal)
                    : NULL;
            recipe->trigger.timer =
                (rs->trigger.timer != NULL)
                    ? recipe_blob_string((char *)set, &cursor, rs->trigger.timer)
                    : NULL;
            recipe->trigger.timeout_ms = rs->trigger.timeout_ms;
            recipe->trigger.has_timeout_ms = rs->trigger.has_timeout_ms;

            if (rs->condition_count > 0u) {
                recipe->conditions = &conditions[condition_cursor];
                recipe->condition_count = (uint16_t)rs->condition_count;
                for (c = 0u; c < rs->condition_count; ++c) {
                    conditions[condition_cursor + c].expression =
                        recipe_blob_string((char *)set, &cursor, rs->conditions[c].expression);
                }
                condition_cursor += rs->condition_count;
            }

            recipe->actions = &actions[action_cursor];
            recipe->action_count = (uint16_t)rs->action_count;
            for (a = 0u; a < rs->action_count; ++a) {
                const recipe_action_spec_t *as = &rs->actions[a];
                cancestry_recipe_action_t *action = &actions[action_cursor + a];

                memset(action, 0, sizeof(*action));
                action->kind = as->kind;
                switch (as->kind) {
                case CANCESTRY_RECIPE_ACTION_SEND_MESSAGE:
                    action->as.send_message.interface =
                        recipe_blob_string((char *)set, &cursor, as->interface);
                    action->as.send_message.message =
                        recipe_blob_string((char *)set, &cursor, as->message);
                    action->as.send_message.values = &values[value_cursor];
                    action->as.send_message.value_count = (uint16_t)as->value_count;
                    {
                        size_t v;
                        for (v = 0u; v < as->value_count; ++v) {
                            cancestry_recipe_signal_value_t *value = &values[value_cursor + v];

                            value->name =
                                recipe_blob_string((char *)set, &cursor, as->values[v].name);
                            value->operand.is_expression = as->values[v].operand.is_expression;
                            value->operand.literal = as->values[v].operand.literal;
                            value->operand.expression = recipe_blob_operand_string(
                                (char *)set, &cursor, &as->values[v].operand);
                        }
                        value_cursor += as->value_count;
                    }
                    break;
                case CANCESTRY_RECIPE_ACTION_SET_SIGNAL:
                    action->as.set_signal.signal =
                        recipe_blob_string((char *)set, &cursor, as->target);
                    action->as.set_signal.operand.is_expression = as->operand.is_expression;
                    action->as.set_signal.operand.literal = as->operand.literal;
                    action->as.set_signal.operand.expression =
                        recipe_blob_operand_string((char *)set, &cursor, &as->operand);
                    break;
                case CANCESTRY_RECIPE_ACTION_SET_VARIABLE:
                    action->as.set_variable.variable =
                        recipe_blob_string((char *)set, &cursor, as->target);
                    action->as.set_variable.operand.is_expression = as->operand.is_expression;
                    action->as.set_variable.operand.literal = as->operand.literal;
                    action->as.set_variable.operand.expression =
                        recipe_blob_operand_string((char *)set, &cursor, &as->operand);
                    break;
                case CANCESTRY_RECIPE_ACTION_START_TIMER:
                    action->as.start_timer.timer =
                        recipe_blob_string((char *)set, &cursor, as->target);
                    action->as.start_timer.duration_ms = as->duration_ms;
                    action->as.start_timer.has_duration_ms = as->has_duration_ms;
                    action->as.start_timer.repeat = as->repeat;
                    break;
                case CANCESTRY_RECIPE_ACTION_STOP_TIMER:
                case CANCESTRY_RECIPE_ACTION_RESET_TIMER:
                    action->as.timer.timer = recipe_blob_string((char *)set, &cursor, as->target);
                    break;
                case CANCESTRY_RECIPE_ACTION_LOG:
                    action->as.log.level = as->level;
                    action->as.log.message =
                        recipe_blob_string((char *)set, &cursor, as->message);
                    break;
                case CANCESTRY_RECIPE_ACTION_RAISE_FAULT:
                    action->as.raise_fault.code =
                        recipe_blob_string((char *)set, &cursor, as->target);
                    action->as.raise_fault.severity = as->severity;
                    break;
                default:
                    /* Unreachable: the validator only emits known kinds. */
                    break;
                }
            }
            action_cursor += rs->action_count;

            recipe->on_error = rs->on_error;
        }
    }

    return set;
}

/* ------------------------------------------------------------------------- */
/* Public API                                                                */
/* ------------------------------------------------------------------------- */

cancestry_recipe_set_t *cancestry_recipe_set_load(const char *text,
                                                  size_t length,
                                                  cancestry_recipe_load_error_t *error)
{
    recipe_load_context_t context;
    recipe_ast_node_t *root = NULL;
    recipe_set_spec_t spec;
    cancestry_recipe_set_t *set;

    if (text == NULL) {
        recipe_load_fail(error, CANCESTRY_RECIPE_ERR_NULL, 0u, 0u, "text is NULL");
        return NULL;
    }
    if (length == 0u) {
        recipe_load_fail(error, CANCESTRY_RECIPE_ERR_PARSE, 1u, 1u, "document is empty");
        return NULL;
    }
    if (memchr(text, '\0', length) != NULL) {
        recipe_load_fail(error, CANCESTRY_RECIPE_ERR_PARSE, 1u, 1u,
                         "NUL bytes are not allowed in a recipe document");
        return NULL;
    }

    memset(&context, 0, sizeof(context));
    context.text = text;
    context.length = length;
    context.pos = 0u;
    context.line = 1u;
    context.column = 1u;

    if (!recipe_load_parse_document(&context, &root, error)) {
        recipe_arena_free(&context.arena);
        return NULL;
    }
    if (!recipe_validate_set(root, &context.arena, &spec, error)) {
        recipe_arena_free(&context.arena);
        return NULL;
    }
    set = recipe_build_set(&spec, error);
    recipe_arena_free(&context.arena);
    if (set != NULL && error != NULL) {
        memset(error, 0, sizeof(*error));
        error->status = CANCESTRY_RECIPE_OK;
    }
    return set;
}

void cancestry_recipe_set_free(cancestry_recipe_set_t *set)
{
    free(set);
}
