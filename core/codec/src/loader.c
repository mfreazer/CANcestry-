/*
 * CANcestry - codec map loader.
 *
 * Implementation notes:
 *   - The loader speaks a strict, documented YAML subset (see
 *     core/codec/README.md): block mappings and block sequences only, no
 *     flow collections, anchors, aliases, tags, directives or multi-line
 *     scalars. Anything outside the subset is rejected with a line/column
 *     error instead of being guessed at.
 *   - Validation implements every constraint of
 *     schemas/codec-map-0.2.0.schema.json in C (required fields, field
 *     types, ranges, patterns, additionalProperties, conditional rules), so
 *     no external JSON Schema validator runs at load time.
 *   - Parsing works in two passes over an AST held in a scratch arena:
 *     first the AST is built and validated, then one block is allocated that
 *     holds the finished cancestry_codec_map_t and everything it points to.
 *     The scratch arena is freed before the loader returns, so a loaded map
 *     is exactly one allocation (SYS-NF-002: loading may allocate; the
 *     runtime path never does).
 *   - Floating-point scalars are converted with strtod, which follows the
 *     active C locale's decimal separator. Hosts and targets shall run with
 *     LC_NUMERIC=C (the default on every platform CANcestry currently
 *     targets); see README.md.
 */

#include "cancestry/codec/loader.h"

#include <float.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------------- */
/* Limits (bounds on load-time allocation; see README.md)                    */
/* ------------------------------------------------------------------------- */

#define CODEC_LOAD_VERSION_MAX ((size_t)32u)
#define CODEC_LOAD_DESCRIPTION_MAX ((size_t)512u)
#define CODEC_LOAD_CAN_ID_MAX ((int64_t)536870911)

/* ------------------------------------------------------------------------- */
/* Scratch arena                                                             */
/* ------------------------------------------------------------------------- */

/*
 * Chunked bump allocator. Once a chunk is full a new chunk is allocated and
 * old chunks are never moved, so every pointer returned by the arena stays
 * valid until the arena is freed (a single realloc'd block would invalidate
 * the AST node pointers on growth).
 */

typedef struct codec_arena_chunk {
    struct codec_arena_chunk *next;
    size_t capacity;
    /* storage follows the header */
} codec_arena_chunk_t;

typedef struct codec_arena {
    codec_arena_chunk_t *head;
    char *cursor;
    size_t remaining;
} codec_arena_t;

static void *codec_arena_alloc(codec_arena_t *arena, size_t size)
{
    size_t aligned = (size + 7u) & ~((size_t)7u);

    if (aligned > arena->remaining) {
        size_t chunk_bytes =
            (aligned > 4096u) ? aligned + sizeof(codec_arena_chunk_t)
                              : 4096u + sizeof(codec_arena_chunk_t);
        codec_arena_chunk_t *chunk = (codec_arena_chunk_t *)malloc(chunk_bytes);

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

static void codec_arena_free(codec_arena_t *arena)
{
    codec_arena_chunk_t *chunk = arena->head;

    while (chunk != NULL) {
        codec_arena_chunk_t *next = chunk->next;
        free(chunk);
        chunk = next;
    }
    memset(arena, 0, sizeof(*arena));
}

/* ------------------------------------------------------------------------- */
/* AST                                                                       */
/* ------------------------------------------------------------------------- */

typedef enum codec_ast_kind {
    CODEC_AST_SCALAR = 0,
    CODEC_AST_MAP = 1,
    CODEC_AST_SEQ = 2,
    CODEC_AST_NULL = 3
} codec_ast_kind_t;

typedef struct codec_ast_pair {
    const char *key;
    size_t key_length;
    struct codec_ast_node *value;
    size_t line;
    size_t column;
} codec_ast_pair_t;

typedef struct codec_ast_node {
    codec_ast_kind_t kind;
    /* scalar: NUL-terminated text (owned by the arena), and whether it was
     * quoted (quoted scalars are always YAML strings, whatever they look
     * like). */
    const char *text;
    size_t text_length;
    bool quoted;
    /* map */
    codec_ast_pair_t *pairs;
    size_t pair_count;
    size_t pair_capacity;
    /* seq */
    struct codec_ast_node **items;
    size_t item_count;
    size_t item_capacity;
    size_t line;
    size_t column;
} codec_ast_node_t;

/* ------------------------------------------------------------------------- */
/* Parse context and error reporting                                         */
/* ------------------------------------------------------------------------- */

typedef struct codec_load_context {
    const char *text;
    size_t length;
    size_t pos;
    size_t line;
    size_t column;
    codec_arena_t arena;
} codec_load_context_t;

static char codec_load_peek(const codec_load_context_t *context)
{
    if (context->pos >= context->length) {
        return '\0';
    }
    return context->text[context->pos];
}

static void codec_load_advance(codec_load_context_t *context)
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

static bool codec_load_is_eol(char c)
{
    return c == '\n' || c == '\r' || c == '\0';
}

static bool codec_load_fail(cancestry_codec_load_error_t *error,
                            cancestry_codec_status_t status,
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
static bool codec_load_fail_here(const codec_load_context_t *context,
                                 cancestry_codec_load_error_t *error,
                                 cancestry_codec_status_t status,
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

static codec_ast_node_t *codec_ast_new(codec_arena_t *arena,
                                       codec_ast_kind_t kind,
                                       size_t line,
                                       size_t column)
{
    codec_ast_node_t *node = (codec_ast_node_t *)codec_arena_alloc(arena, sizeof(*node));

    if (node == NULL) {
        return NULL;
    }
    memset(node, 0, sizeof(*node));
    node->kind = kind;
    node->line = line;
    node->column = column;
    return node;
}

static bool codec_ast_map_add(codec_arena_t *arena,
                              codec_ast_node_t *map,
                              const char *key,
                              size_t key_length,
                              codec_ast_node_t *value,
                              size_t line,
                              size_t column)
{
    if (map->pair_count == map->pair_capacity) {
        size_t new_capacity = (map->pair_capacity == 0u) ? 4u : map->pair_capacity * 2u;
        codec_ast_pair_t *pairs =
            (codec_ast_pair_t *)codec_arena_alloc(arena, new_capacity * sizeof(*pairs));

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

static bool codec_ast_seq_add(codec_arena_t *arena, codec_ast_node_t *seq, codec_ast_node_t *item)
{
    if (seq->item_count == seq->item_capacity) {
        size_t new_capacity = (seq->item_capacity == 0u) ? 4u : seq->item_capacity * 2u;
        codec_ast_node_t **items =
            (codec_ast_node_t **)codec_arena_alloc(arena, new_capacity * sizeof(*items));

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

static bool codec_ast_find_pair(const codec_ast_node_t *map, const char *key, size_t *index_out)
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

static bool codec_ast_has_duplicate_key(const codec_ast_node_t *map,
                                        const char *key,
                                        size_t key_length)
{
    size_t i;

    for (i = 0u; i < map->pair_count; ++i) {
        if (map->pairs[i].key_length == key_length &&
            memcmp(map->pairs[i].key, key, key_length) == 0) {
            return true;
        }
    }
    return false;
}

/* ------------------------------------------------------------------------- */
/* Low-level scalar utilities                                                */
/* ------------------------------------------------------------------------- */

/** Copy a text slice into the arena as a NUL-terminated string. */
static char *codec_string_copy(codec_arena_t *arena, const char *text, size_t length)
{
    char *copy = (char *)codec_arena_alloc(arena, length + 1u);

    if (copy == NULL) {
        return NULL;
    }
    memcpy(copy, text, length);
    copy[length] = '\0';
    return copy;
}

/** Append one UTF-8 encoded code point to a string being built. */
static void codec_utf8_emit(char *buffer, size_t *used, uint32_t codepoint)
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

static int codec_hex_digit(char c)
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

typedef enum codec_scalar_kind {
    CODEC_SCALAR_STRING = 0,
    CODEC_SCALAR_INT = 1,
    CODEC_SCALAR_FLOAT = 2,
    CODEC_SCALAR_BOOL = 3
} codec_scalar_kind_t;

/** Classify a plain scalar's implicit YAML type. */
static codec_scalar_kind_t codec_scalar_classify(const char *text, size_t length)
{
    size_t i = 0u;

    if ((length == 4u && memcmp(text, "true", 4u) == 0) ||
        (length == 5u && memcmp(text, "false", 5u) == 0)) {
        return CODEC_SCALAR_BOOL;
    }
    if (length == 0u) {
        return CODEC_SCALAR_STRING;
    }
    if (text[i] == '+' || text[i] == '-') {
        i++;
    }
    if (i < length && text[i] == '0' && i + 1u < length) {
        if (text[i + 1u] == 'x' || text[i + 1u] == 'X') {
            size_t digits = 0u;
            size_t j;
            for (j = i + 2u; j < length; ++j) {
                if (codec_hex_digit(text[j]) < 0) {
                    return CODEC_SCALAR_STRING;
                }
                digits++;
            }
            return digits > 0u ? CODEC_SCALAR_INT : CODEC_SCALAR_STRING;
        }
        if (text[i + 1u] == 'o' || text[i + 1u] == 'O') {
            size_t digits = 0u;
            size_t j;
            for (j = i + 2u; j < length; ++j) {
                if (text[j] < '0' || text[j] > '7') {
                    return CODEC_SCALAR_STRING;
                }
                digits++;
            }
            return digits > 0u ? CODEC_SCALAR_INT : CODEC_SCALAR_STRING;
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
                    return CODEC_SCALAR_STRING;
                }
                saw_dot = true;
            } else if (c == 'e' || c == 'E') {
                if (saw_exp || !saw_digit) {
                    return CODEC_SCALAR_STRING;
                }
                saw_exp = true;
                saw_digit = false;
                if (i + 1u < length && (text[i + 1u] == '+' || text[i + 1u] == '-')) {
                    i++;
                }
            } else {
                return CODEC_SCALAR_STRING;
            }
        }
        if (!saw_digit) {
            return CODEC_SCALAR_STRING;
        }
        return (saw_dot || saw_exp) ? CODEC_SCALAR_FLOAT : CODEC_SCALAR_INT;
    }
}

static bool codec_parse_uint64(const char *text, size_t length, uint64_t *value_out)
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

static bool codec_parse_double(const char *text, size_t length, double *value_out)
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
static void codec_load_skip_blank_lines(codec_load_context_t *context)
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
                codec_load_advance(context);
            }
            while (codec_load_is_eol(codec_load_peek(context)) &&
                   codec_load_peek(context) != '\0') {
                codec_load_advance(context);
            }
            continue;
        }
        if (context->text[p] == '#') {
            while (!codec_load_is_eol(codec_load_peek(context))) {
                codec_load_advance(context);
            }
            while (codec_load_is_eol(codec_load_peek(context)) &&
                   codec_load_peek(context) != '\0') {
                codec_load_advance(context);
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
static bool codec_load_content_column(codec_load_context_t *context,
                                      size_t *column_out,
                                      cancestry_codec_load_error_t *error)
{
    size_t p = context->pos;
    size_t column = context->column;

    while (p < context->length && context->text[p] == ' ') {
        p++;
        column++;
    }
    if (p < context->length && context->text[p] == '\t') {
        return codec_load_fail_here(context, error, CANCESTRY_CODEC_ERR_PARSE,
                                    "tab characters are not allowed in indentation");
    }
    *column_out = column;
    return true;
}

/**
 * Peek whether the rest of the line contains only spaces and a comment.
 * The position must be at (or before) the first space to check.
 */
static bool codec_load_rest_is_comment(const codec_load_context_t *context)
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
static bool codec_load_parse_double_quoted(codec_load_context_t *context,
                                           codec_arena_t *arena,
                                           char **text_out,
                                           size_t *length_out,
                                           cancestry_codec_load_error_t *error)
{
    size_t line = context->line;
    size_t column = context->column;
    char *buffer = (char *)codec_arena_alloc(arena, 64u);
    size_t used = 0u;
    size_t capacity = 64u;

    if (buffer == NULL) {
        return codec_load_fail_here(context, error, CANCESTRY_CODEC_ERR_NO_MEMORY,
                                    "out of memory");
    }
    codec_load_advance(context); /* opening quote */
    for (;;) {
        char c = codec_load_peek(context);
        uint32_t codepoint;

        if (c == '\0' || c == '\n' || c == '\r') {
            return codec_load_fail(error, CANCESTRY_CODEC_ERR_PARSE, line, column,
                                   "unterminated double-quoted string");
        }
        if (c == '"') {
            codec_load_advance(context);
            break;
        }
        if (c != '\\') {
            codepoint = (uint32_t)(unsigned char)c;
            codec_load_advance(context);
        } else {
            int digit;

            codec_load_advance(context); /* backslash */
            c = codec_load_peek(context);
            codec_load_advance(context);
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
                digit = codec_hex_digit(codec_load_peek(context));
                if (digit < 0) {
                    return codec_load_fail(error, CANCESTRY_CODEC_ERR_PARSE, context->line,
                                           context->column, "invalid \\x escape");
                }
                codepoint = (uint32_t)digit;
                codec_load_advance(context);
                digit = codec_hex_digit(codec_load_peek(context));
                if (digit < 0) {
                    return codec_load_fail(error, CANCESTRY_CODEC_ERR_PARSE, context->line,
                                           context->column, "invalid \\x escape");
                }
                codepoint = (codepoint << 4u) | (uint32_t)digit;
                codec_load_advance(context);
                break;
            case 'u':
            case 'U': {
                size_t digits = (c == 'u') ? 4u : 8u;
                size_t i;
                codepoint = 0u;
                for (i = 0u; i < digits; ++i) {
                    digit = codec_hex_digit(codec_load_peek(context));
                    if (digit < 0) {
                        return codec_load_fail(error, CANCESTRY_CODEC_ERR_PARSE, context->line,
                                               context->column, "invalid \\u escape");
                    }
                    codepoint = (codepoint << 4u) | (uint32_t)digit;
                    codec_load_advance(context);
                }
                if ((codepoint >= 0xD800u && codepoint <= 0xDFFFu) || codepoint > 0x10FFFFu) {
                    return codec_load_fail(error, CANCESTRY_CODEC_ERR_PARSE, context->line,
                                           context->column, "invalid unicode code point");
                }
                break;
            }
            default:
                return codec_load_fail(error, CANCESTRY_CODEC_ERR_PARSE, context->line,
                                       context->column, "invalid escape sequence '\\%c'", c);
            }
        }
        /* Grow if needed: worst case 4 bytes per code point. */
        if (used + 4u >= capacity) {
            char *grown = (char *)codec_arena_alloc(arena, capacity * 2u);
            if (grown == NULL) {
                return codec_load_fail_here(context, error, CANCESTRY_CODEC_ERR_NO_MEMORY,
                                            "out of memory");
            }
            memcpy(grown, buffer, used);
            buffer = grown;
            capacity *= 2u;
        }
        codec_utf8_emit(buffer, &used, codepoint);
    }
    buffer[used] = '\0';
    *text_out = buffer;
    *length_out = used;
    return true;
}

/** Parse a single-quoted scalar starting at the opening quote. */
static bool codec_load_parse_single_quoted(codec_load_context_t *context,
                                           codec_arena_t *arena,
                                           char **text_out,
                                           size_t *length_out,
                                           cancestry_codec_load_error_t *error)
{
    size_t line = context->line;
    size_t column = context->column;
    char *buffer = (char *)codec_arena_alloc(arena, 64u);
    size_t used = 0u;
    size_t capacity = 64u;

    if (buffer == NULL) {
        return codec_load_fail_here(context, error, CANCESTRY_CODEC_ERR_NO_MEMORY,
                                    "out of memory");
    }
    codec_load_advance(context); /* opening quote */
    for (;;) {
        char c = codec_load_peek(context);

        if (c == '\0' || c == '\n' || c == '\r') {
            return codec_load_fail(error, CANCESTRY_CODEC_ERR_PARSE, line, column,
                                   "unterminated single-quoted string");
        }
        if (c == '\'') {
            codec_load_advance(context);
            if (codec_load_peek(context) == '\'') {
                c = '\'';
                codec_load_advance(context);
            } else {
                break;
            }
        } else {
            codec_load_advance(context);
        }
        if (used + 1u >= capacity) {
            char *grown = (char *)codec_arena_alloc(arena, capacity * 2u);
            if (grown == NULL) {
                return codec_load_fail_here(context, error, CANCESTRY_CODEC_ERR_NO_MEMORY,
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
static bool codec_load_parse_scalar_value(codec_load_context_t *context,
                                          codec_ast_node_t *node,
                                          cancestry_codec_load_error_t *error)
{
    size_t line = context->line;
    size_t column = context->column;
    char c = codec_load_peek(context);

    node->kind = CODEC_AST_SCALAR;
    node->line = line;
    node->column = column;

    if (c == '"') {
        char *text = NULL;
        size_t length = 0u;
        if (!codec_load_parse_double_quoted(context, &context->arena, &text, &length, error)) {
            return false;
        }
        node->text = text;
        node->text_length = length;
        node->quoted = true;
    } else if (c == '\'') {
        char *text = NULL;
        size_t length = 0u;
        if (!codec_load_parse_single_quoted(context, &context->arena, &text, &length, error)) {
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
            c = codec_load_peek(context);
            if (codec_load_is_eol(c)) {
                break;
            }
            if (c == ' ' && codec_load_rest_is_comment(context)) {
                break;
            }
            codec_load_advance(context);
        }
        end = context->text + context->pos;
        while (end > start && end[-1] == ' ') {
            end--;
        }
        if (end == start) {
            return codec_load_fail(error, CANCESTRY_CODEC_ERR_PARSE, line, start_column,
                                   "expected a scalar value");
        }
        node->text = codec_string_copy(&context->arena, start, (size_t)(end - start));
        if (node->text == NULL) {
            return codec_load_fail_here(context, error, CANCESTRY_CODEC_ERR_NO_MEMORY,
                                        "out of memory");
        }
        node->text_length = (size_t)(end - start);
    }

    /* A scalar ends the value; only a comment or end of line may follow. */
    for (;;) {
        c = codec_load_peek(context);
        if (codec_load_is_eol(c)) {
            break;
        }
        if (c == ' ') {
            codec_load_advance(context);
            continue;
        }
        if (c == '#') {
            while (!codec_load_is_eol(codec_load_peek(context))) {
                codec_load_advance(context);
            }
            break;
        }
        return codec_load_fail_here(context, error, CANCESTRY_CODEC_ERR_PARSE,
                                    "unexpected content after scalar value");
    }
    while (codec_load_is_eol(codec_load_peek(context)) && codec_load_peek(context) != '\0') {
        codec_load_advance(context);
    }
    return true;
}

/**
 * Parse a mapping key and its ':' delimiter. The position must be at the key
 * start. On success the position is just past the ':'.
 */
static bool codec_load_parse_key(codec_load_context_t *context,
                                 codec_arena_t *arena,
                                 char **key_out,
                                 size_t *key_length_out,
                                 size_t *line_out,
                                 size_t *column_out,
                                 cancestry_codec_load_error_t *error)
{
    char *key;
    size_t key_length;
    size_t line = context->line;
    size_t column = context->column;
    char c = codec_load_peek(context);

    if (c == '"') {
        if (!codec_load_parse_double_quoted(context, arena, &key, &key_length, error)) {
            return false;
        }
    } else if (c == '\'') {
        if (!codec_load_parse_single_quoted(context, arena, &key, &key_length, error)) {
            return false;
        }
    } else {
        const char *start = context->text + context->pos;
        const char *end;

        for (;;) {
            c = codec_load_peek(context);
            if (codec_load_is_eol(c)) {
                break;
            }
            if (c == ':' && (context->pos + 1u >= context->length ||
                             context->text[context->pos + 1u] == ' ' ||
                             codec_load_is_eol(context->text[context->pos + 1u]))) {
                break;
            }
            codec_load_advance(context);
        }
        end = context->text + context->pos;
        while (end > start && end[-1] == ' ') {
            end--;
        }
        if (end == start) {
            return codec_load_fail(error, CANCESTRY_CODEC_ERR_PARSE, line, column,
                                   "expected a mapping key");
        }
        key = codec_string_copy(arena, start, (size_t)(end - start));
        if (key == NULL) {
            return codec_load_fail_here(context, error, CANCESTRY_CODEC_ERR_NO_MEMORY,
                                        "out of memory");
        }
        key_length = (size_t)(end - start);
    }

    /* Optional spaces between the key and ':'. */
    while (codec_load_peek(context) == ' ') {
        codec_load_advance(context);
    }
    if (codec_load_peek(context) != ':') {
        return codec_load_fail(error, CANCESTRY_CODEC_ERR_PARSE, line, column,
                               "expected ':' after mapping key");
    }
    codec_load_advance(context);

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
static bool codec_load_line_is_marker(const codec_load_context_t *context, const char *marker);

static bool codec_load_parse_block_mapping(codec_load_context_t *context,
                                           size_t column,
                                           codec_ast_node_t **map_out,
                                           cancestry_codec_load_error_t *error);

static bool codec_load_parse_block_sequence(codec_load_context_t *context,
                                            size_t column,
                                            codec_ast_node_t **sequence_out,
                                            cancestry_codec_load_error_t *error);

/**
 * Parse the nested block that follows a "key:" or "-" line with nothing after
 * it. The next content line must be deeper than @p parent_column.
 */
static bool codec_load_parse_nested(codec_load_context_t *context,
                                    size_t parent_column,
                                    codec_ast_node_t **value_out,
                                    cancestry_codec_load_error_t *error)
{
    size_t column = 0u;
    size_t p;
    char c;

    while (codec_load_is_eol(codec_load_peek(context)) && codec_load_peek(context) != '\0') {
        codec_load_advance(context);
    }
    codec_load_skip_blank_lines(context);
    if (codec_load_peek(context) == '\0') {
        *value_out =
            codec_ast_new(&context->arena, CODEC_AST_NULL, context->line, context->column);
        return *value_out != NULL
                   ? true
                   : codec_load_fail_here(context, error, CANCESTRY_CODEC_ERR_NO_MEMORY,
                                          "out of memory");
    }
    if (!codec_load_content_column(context, &column, error)) {
        return false;
    }
    if (column <= parent_column) {
        *value_out =
            codec_ast_new(&context->arena, CODEC_AST_NULL, context->line, context->column);
        return *value_out != NULL
                   ? true
                   : codec_load_fail_here(context, error, CANCESTRY_CODEC_ERR_NO_MEMORY,
                                          "out of memory");
    }
    /* Peek at the content character to decide mapping vs sequence. */
    p = context->pos;
    while (p < context->length && context->text[p] == ' ') {
        p++;
    }
    c = (p < context->length) ? context->text[p] : '\0';
    if (c == '-') {
        return codec_load_parse_block_sequence(context, column, value_out, error);
    }
    return codec_load_parse_block_mapping(context, column, value_out, error);
}

/**
 * Parse one "key: value" line whose key starts at @p column, adding the pair
 * to @p map. The position must be at the key's first character.
 */
static bool codec_load_parse_pair_line(codec_load_context_t *context,
                                       size_t column,
                                       codec_ast_node_t *map,
                                       cancestry_codec_load_error_t *error)
{
    char *key;
    size_t key_length;
    size_t line;
    size_t key_column;
    codec_ast_node_t *value;
    char c;

    if (codec_load_peek(context) == '-') {
        return codec_load_fail_here(context, error, CANCESTRY_CODEC_ERR_PARSE,
                                    "expected a mapping entry, found '-'");
    }
    if (!codec_load_parse_key(context, &context->arena, &key, &key_length, &line, &key_column,
                              error)) {
        return false;
    }
    if (codec_ast_has_duplicate_key(map, key, key_length)) {
        return codec_load_fail(error, CANCESTRY_CODEC_ERR_PARSE, line, key_column,
                               "duplicate mapping key '%.*s'", (int)key_length, key);
    }
    while (codec_load_peek(context) == ' ') {
        codec_load_advance(context);
    }
    c = codec_load_peek(context);
    value = codec_ast_new(&context->arena, CODEC_AST_NULL, context->line, context->column);
    if (value == NULL) {
        return codec_load_fail_here(context, error, CANCESTRY_CODEC_ERR_NO_MEMORY,
                                    "out of memory");
    }
    if (codec_load_is_eol(c) || c == '#') {
        codec_ast_node_t *nested = NULL;
        if (c == '#') {
            while (!codec_load_is_eol(codec_load_peek(context))) {
                codec_load_advance(context);
            }
        }
        if (!codec_load_parse_nested(context, column, &nested, error)) {
            return false;
        }
        if (nested->kind != CODEC_AST_NULL) {
            value = nested;
        }
    } else {
        if (!codec_load_parse_scalar_value(context, value, error)) {
            return false;
        }
    }
    return codec_ast_map_add(&context->arena, map, key, key_length, value, line, key_column);
}

static bool codec_load_parse_block_mapping(codec_load_context_t *context,
                                           size_t column,
                                           codec_ast_node_t **map_out,
                                           cancestry_codec_load_error_t *error)
{
    codec_ast_node_t *map =
        codec_ast_new(&context->arena, CODEC_AST_MAP, context->line, context->column);

    if (map == NULL) {
        return codec_load_fail_here(context, error, CANCESTRY_CODEC_ERR_NO_MEMORY,
                                    "out of memory");
    }
    for (;;) {
        size_t entry_column = 0u;

        codec_load_skip_blank_lines(context);
        if (codec_load_peek(context) == '\0') {
            break;
        }
        if (!codec_load_content_column(context, &entry_column, error)) {
            return false;
        }
        if (entry_column < column) {
            break; /* back to the parent block */
        }
        if (entry_column > column) {
            return codec_load_fail_here(context, error, CANCESTRY_CODEC_ERR_PARSE,
                                        "unexpected indentation");
        }
        while (codec_load_peek(context) == ' ') {
            codec_load_advance(context);
        }
        if (codec_load_line_is_marker(context, "...")) {
            break; /* end of document */
        }
        if (!codec_load_parse_pair_line(context, column, map, error)) {
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
static bool codec_load_parse_seq_item_map(codec_load_context_t *context,
                                          size_t key_column,
                                          codec_ast_node_t **map_out,
                                          cancestry_codec_load_error_t *error)
{
    codec_ast_node_t *map =
        codec_ast_new(&context->arena, CODEC_AST_MAP, context->line, context->column);

    if (map == NULL) {
        return codec_load_fail_here(context, error, CANCESTRY_CODEC_ERR_NO_MEMORY,
                                    "out of memory");
    }
    if (!codec_load_parse_pair_line(context, key_column, map, error)) {
        return false;
    }
    for (;;) {
        size_t next_column = 0u;

        codec_load_skip_blank_lines(context);
        if (codec_load_peek(context) == '\0') {
            break;
        }
        if (!codec_load_content_column(context, &next_column, error)) {
            return false;
        }
        if (next_column < key_column) {
            break;
        }
        if (next_column > key_column) {
            return codec_load_fail_here(context, error, CANCESTRY_CODEC_ERR_PARSE,
                                        "unexpected indentation");
        }
        while (codec_load_peek(context) == ' ') {
            codec_load_advance(context);
        }
        if (codec_load_peek(context) == '-') {
            break;
        }
        if (!codec_load_parse_pair_line(context, key_column, map, error)) {
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
static bool codec_load_parse_sequence_item(codec_load_context_t *context,
                                           size_t sequence_column,
                                           codec_ast_node_t **item_out,
                                           cancestry_codec_load_error_t *error)
{
    codec_ast_node_t *item =
        codec_ast_new(&context->arena, CODEC_AST_NULL, context->line, context->column);
    char c;

    if (item == NULL) {
        return codec_load_fail_here(context, error, CANCESTRY_CODEC_ERR_NO_MEMORY,
                                    "out of memory");
    }
    c = codec_load_peek(context);
    if (codec_load_is_eol(c)) {
        codec_ast_node_t *nested = NULL;
        if (!codec_load_parse_nested(context, sequence_column, &nested, error)) {
            return false;
        }
        if (nested->kind != CODEC_AST_NULL) {
            item = nested;
        }
    } else if (c == ' ') {
        while (codec_load_peek(context) == ' ') {
            codec_load_advance(context);
        }
        c = codec_load_peek(context);
        if (codec_load_is_eol(c)) {
            codec_ast_node_t *nested = NULL;
            if (!codec_load_parse_nested(context, sequence_column, &nested, error)) {
                return false;
            }
            if (nested->kind != CODEC_AST_NULL) {
                item = nested;
            }
        } else if (c == '#') {
            while (!codec_load_is_eol(codec_load_peek(context))) {
                codec_load_advance(context);
            }
        } else if (c == '-' || c == '[' || c == '{') {
            return codec_load_fail_here(
                context, error, CANCESTRY_CODEC_ERR_PARSE,
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
                    codec_load_parse_key(context, &context->arena, NULL, NULL, NULL, NULL, NULL);
                context->pos = save_pos;
                context->line = save_line;
                context->column = save_column;
            } else {
                size_t p;
                for (p = context->pos; p < context->length; ++p) {
                    char pc = context->text[p];
                    if (codec_load_is_eol(pc)) {
                        break;
                    }
                    if (pc == ':' && (p + 1u >= context->length ||
                                      context->text[p + 1u] == ' ' ||
                                      codec_load_is_eol(context->text[p + 1u]))) {
                        mapping_form = true;
                        break;
                    }
                }
            }
            if (mapping_form) {
                size_t key_column = context->column;
                codec_ast_node_t *map = NULL;
                if (!codec_load_parse_seq_item_map(context, key_column, &map, error)) {
                    return false;
                }
                item = map;
            } else {
                if (!codec_load_parse_scalar_value(context, item, error)) {
                    return false;
                }
            }
        }
    } else {
        return codec_load_fail_here(context, error, CANCESTRY_CODEC_ERR_PARSE,
                                    "expected a space after '-'");
    }
    *item_out = item;
    return true;
}

static bool codec_load_parse_block_sequence(codec_load_context_t *context,
                                            size_t column,
                                            codec_ast_node_t **sequence_out,
                                            cancestry_codec_load_error_t *error)
{
    codec_ast_node_t *sequence =
        codec_ast_new(&context->arena, CODEC_AST_SEQ, context->line, context->column);

    if (sequence == NULL) {
        return codec_load_fail_here(context, error, CANCESTRY_CODEC_ERR_NO_MEMORY,
                                    "out of memory");
    }
    for (;;) {
        size_t entry_column = 0u;
        codec_ast_node_t *item = NULL;

        codec_load_skip_blank_lines(context);
        if (codec_load_peek(context) == '\0') {
            break;
        }
        if (!codec_load_content_column(context, &entry_column, error)) {
            return false;
        }
        if (entry_column < column) {
            break;
        }
        if (entry_column > column) {
            return codec_load_fail_here(context, error, CANCESTRY_CODEC_ERR_PARSE,
                                        "unexpected indentation");
        }
        while (codec_load_peek(context) == ' ') {
            codec_load_advance(context);
        }
        if (codec_load_line_is_marker(context, "...")) {
            break; /* end of document */
        }
        if (codec_load_peek(context) != '-') {
            break; /* end of the sequence; the parent block continues */
        }
        codec_load_advance(context);
        if (!codec_load_parse_sequence_item(context, column, &item, error)) {
            return false;
        }
        if (!codec_ast_seq_add(&context->arena, sequence, item)) {
            return codec_load_fail_here(context, error, CANCESTRY_CODEC_ERR_NO_MEMORY,
                                        "out of memory");
        }
    }
    *sequence_out = sequence;
    return true;
}

static bool codec_load_line_is_marker(const codec_load_context_t *context, const char *marker)
{
    size_t p = context->pos;
    size_t length = strlen(marker);

    if (p + length > context->length || memcmp(context->text + p, marker, length) != 0) {
        return false;
    }
    p += length;
    if (p < context->length && context->text[p] != ' ' && context->text[p] != '\t' &&
        context->text[p] != '#' && !codec_load_is_eol(context->text[p])) {
        return false;
    }
    return true;
}

static void codec_load_consume_line(codec_load_context_t *context)
{
    while (!codec_load_is_eol(codec_load_peek(context))) {
        codec_load_advance(context);
    }
    while (codec_load_is_eol(codec_load_peek(context)) && codec_load_peek(context) != '\0') {
        codec_load_advance(context);
    }
}

static bool codec_load_parse_document(codec_load_context_t *context,
                                      codec_ast_node_t **root_out,
                                      cancestry_codec_load_error_t *error)
{
    size_t column = 0u;
    codec_ast_node_t *root = NULL;

    codec_load_skip_blank_lines(context);
    if (codec_load_peek(context) == '\0') {
        return codec_load_fail(error, CANCESTRY_CODEC_ERR_PARSE, context->line, context->column,
                               "document is empty");
    }
    if (codec_load_line_is_marker(context, "---")) {
        codec_load_consume_line(context);
        codec_load_skip_blank_lines(context);
    }
    if (codec_load_peek(context) == '\0' || codec_load_line_is_marker(context, "...")) {
        return codec_load_fail(error, CANCESTRY_CODEC_ERR_PARSE, context->line, context->column,
                               "document has no content");
    }
    if (!codec_load_content_column(context, &column, error)) {
        return false;
    }
    if (!codec_load_parse_block_mapping(context, column, &root, error)) {
        return false;
    }
    codec_load_skip_blank_lines(context);
    if (codec_load_line_is_marker(context, "...")) {
        codec_load_consume_line(context);
        codec_load_skip_blank_lines(context);
    }
    if (codec_load_peek(context) != '\0') {
        return codec_load_fail_here(context, error, CANCESTRY_CODEC_ERR_PARSE,
                                    "unexpected content after the document");
    }
    *root_out = root;
    return true;
}

/* ------------------------------------------------------------------------- */
/* Schema-field readers                                                      */
/* ------------------------------------------------------------------------- */

static bool codec_field_string(const codec_ast_node_t *node,
                               codec_arena_t *arena,
                               char **text_out,
                               size_t *length_out)
{
    char *copy;

    if (node == NULL || node->kind != CODEC_AST_SCALAR) {
        return false;
    }
    /* Per the JSON Schema all string fields require a YAML string scalar: an
     * unquoted scalar that parses as an integer, float or boolean is not one.
     * Quoted scalars are strings no matter what they look like. */
    if (!node->quoted &&
        codec_scalar_classify(node->text, node->text_length) != CODEC_SCALAR_STRING) {
        return false;
    }
    copy = codec_string_copy(arena, node->text, node->text_length);
    if (copy == NULL) {
        return false;
    }
    *text_out = copy;
    if (length_out != NULL) {
        *length_out = node->text_length;
    }
    return true;
}

static bool codec_field_int(const codec_ast_node_t *node,
                            bool allow_negative,
                            int64_t min_value,
                            int64_t max_value,
                            int64_t *value_out,
                            cancestry_codec_load_error_t *error,
                            const char *field)
{
    uint64_t magnitude = 0u;
    const char *text;
    size_t length;
    size_t offset = 0u;
    bool negative = false;

    if (node == NULL || node->kind != CODEC_AST_SCALAR) {
        return codec_load_fail(error, CANCESTRY_CODEC_ERR_PARSE, node ? node->line : 0u,
                               node ? node->column : 0u, "'%s' must be an integer", field);
    }
    text = node->text;
    length = node->text_length;
    if (codec_scalar_classify(text, length) != CODEC_SCALAR_INT) {
        return codec_load_fail(error, CANCESTRY_CODEC_ERR_PARSE, node->line, node->column,
                               "'%s' must be an integer", field);
    }
    if (text[0] == '+' || text[0] == '-') {
        negative = (text[0] == '-');
        offset = 1u;
    }
    if (length - offset > 2u && text[offset] == '0' &&
        (text[offset + 1u] == 'x' || text[offset + 1u] == 'X')) {
        size_t i;
        uint64_t value = 0u;
        for (i = offset + 2u; i < length; ++i) {
            int digit = codec_hex_digit(text[i]);
            if (value > (UINT64_MAX - (uint64_t)digit) / 16u) {
                return codec_load_fail(error, CANCESTRY_CODEC_ERR_PARSE, node->line,
                                       node->column, "'%s' is out of range", field);
            }
            value = (value * 16u) + (uint64_t)digit;
        }
        magnitude = value;
    } else if (length - offset > 2u && text[offset] == '0' &&
               (text[offset + 1u] == 'o' || text[offset + 1u] == 'O')) {
        size_t i;
        uint64_t value = 0u;
        for (i = offset + 2u; i < length; ++i) {
            int digit = text[i] - '0';
            if (value > (UINT64_MAX - (uint64_t)digit) / 8u) {
                return codec_load_fail(error, CANCESTRY_CODEC_ERR_PARSE, node->line,
                                       node->column, "'%s' is out of range", field);
            }
            value = (value * 8u) + (uint64_t)digit;
        }
        magnitude = value;
    } else {
        if (!codec_parse_uint64(text + offset, length - offset, &magnitude)) {
            return codec_load_fail(error, CANCESTRY_CODEC_ERR_PARSE, node->line, node->column,
                                   "'%s' is out of range", field);
        }
    }
    if (negative) {
        if (!allow_negative) {
            return codec_load_fail(error, CANCESTRY_CODEC_ERR_PARSE, node->line, node->column,
                                   "'%s' is out of range", field);
        }
        if (magnitude > (uint64_t)INT64_MAX + 1u) {
            return codec_load_fail(error, CANCESTRY_CODEC_ERR_PARSE, node->line, node->column,
                                   "'%s' is out of range", field);
        }
        if (magnitude == (uint64_t)INT64_MAX + 1u) {
            if (min_value > INT64_MIN) {
                return codec_load_fail(error, CANCESTRY_CODEC_ERR_PARSE, node->line,
                                       node->column, "'%s' is out of range", field);
            }
            *value_out = INT64_MIN;
        } else {
            if (-(int64_t)magnitude < min_value) {
                return codec_load_fail(error, CANCESTRY_CODEC_ERR_PARSE, node->line,
                                       node->column, "'%s' is out of range", field);
            }
            *value_out = -(int64_t)magnitude;
        }
    } else {
        if (min_value > 0 && magnitude < (uint64_t)min_value) {
            return codec_load_fail(error, CANCESTRY_CODEC_ERR_PARSE, node->line, node->column,
                                   "'%s' is out of range", field);
        }
        if (magnitude > (uint64_t)max_value) {
            return codec_load_fail(error, CANCESTRY_CODEC_ERR_PARSE, node->line, node->column,
                                   "'%s' is out of range", field);
        }
        *value_out = (int64_t)magnitude;
    }
    return true;
}

static bool codec_field_double(const codec_ast_node_t *node,
                               double *value_out,
                               cancestry_codec_load_error_t *error,
                               const char *field)
{
    if (node == NULL || node->kind != CODEC_AST_SCALAR) {
        return codec_load_fail(error, CANCESTRY_CODEC_ERR_PARSE, node ? node->line : 0u,
                               node ? node->column : 0u, "'%s' must be a number", field);
    }
    switch (codec_scalar_classify(node->text, node->text_length)) {
    case CODEC_SCALAR_INT: {
        uint64_t magnitude = 0u;
        bool negative = false;
        size_t offset = 0u;
        double value;
        if (node->text[0] == '+' || node->text[0] == '-') {
            negative = (node->text[0] == '-');
            offset = 1u;
        }
        if (!codec_parse_uint64(node->text + offset, node->text_length - offset, &magnitude)) {
            return codec_load_fail(error, CANCESTRY_CODEC_ERR_PARSE, node->line, node->column,
                                   "'%s' is out of range", field);
        }
        value = negative ? -(double)magnitude : (double)magnitude;
        if (!(value >= -DBL_MAX && value <= DBL_MAX)) {
            return codec_load_fail(error, CANCESTRY_CODEC_ERR_PARSE, node->line, node->column,
                                   "'%s' is out of range", field);
        }
        *value_out = value;
        return true;
    }
    case CODEC_SCALAR_FLOAT:
        if (!codec_parse_double(node->text, node->text_length, value_out)) {
            return codec_load_fail(error, CANCESTRY_CODEC_ERR_PARSE, node->line, node->column,
                                   "'%s' is not a valid number", field);
        }
        if (!(*value_out >= -DBL_MAX && *value_out <= DBL_MAX)) {
            return codec_load_fail(error, CANCESTRY_CODEC_ERR_PARSE, node->line, node->column,
                                   "'%s' must be finite", field);
        }
        return true;
    default:
        return codec_load_fail(error, CANCESTRY_CODEC_ERR_PARSE, node->line, node->column,
                               "'%s' must be a number", field);
    }
}

static bool codec_field_bool(const codec_ast_node_t *node,
                             bool *value_out,
                             cancestry_codec_load_error_t *error,
                             const char *field)
{
    if (node == NULL || node->kind != CODEC_AST_SCALAR) {
        return codec_load_fail(error, CANCESTRY_CODEC_ERR_PARSE, node ? node->line : 0u,
                               node ? node->column : 0u, "'%s' must be a boolean", field);
    }
    if (codec_scalar_classify(node->text, node->text_length) != CODEC_SCALAR_BOOL) {
        return codec_load_fail(error, CANCESTRY_CODEC_ERR_PARSE, node->line, node->column,
                               "'%s' must be a boolean", field);
    }
    *value_out = (node->text_length == 4u);
    return true;
}

static bool codec_version_pattern_ok(const char *text, size_t length)
{
    size_t i = 0u;
    int groups = 0;
    int digits = 0;

    for (; i < length; ++i) {
        char c = text[i];
        if (c >= '0' && c <= '9') {
            digits++;
        } else if (c == '.') {
            if (digits == 0) {
                return false;
            }
            groups++;
            digits = 0;
        } else {
            return false;
        }
    }
    return groups == 2 && digits > 0;
}

static bool codec_check_unknown_keys(const codec_ast_node_t *map,
                                     const char *const *allowed,
                                     size_t allowed_count,
                                     const char *where,
                                     cancestry_codec_load_error_t *error)
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
            return codec_load_fail(error, CANCESTRY_CODEC_ERR_PARSE, map->pairs[i].line,
                                   map->pairs[i].column, "unknown field '%.*s' in %s",
                                   (int)map->pairs[i].key_length, map->pairs[i].key, where);
        }
    }
    return true;
}

/* ------------------------------------------------------------------------- */
/* Validation and arena build                                                */
/* ------------------------------------------------------------------------- */

typedef struct codec_value_spec {
    uint64_t raw;
    char *label;
    size_t label_length;
} codec_value_spec_t;

typedef struct codec_signal_spec {
    const codec_ast_node_t *node;
    char *name;
    size_t name_length;
    char *unit;
    uint32_t start_bit;
    uint32_t bit_length;
    cancestry_codec_signal_type_t type;
    cancestry_codec_endianness_t endianness;
    cancestry_codec_layout_t layout;
    double scale;
    double offset;
    bool has_min;
    bool has_max;
    double min;
    double max;
    bool strict;
    codec_value_spec_t *mappings;
    size_t mapping_count;
    uint32_t message_id;
} codec_signal_spec_t;

typedef struct codec_message_spec {
    const codec_ast_node_t *node;
    uint32_t id;
    char *name;
    size_t name_length;
    char *description;
    uint8_t dlc;
    uint32_t period_ms;
    codec_signal_spec_t *signals;
    size_t signal_count;
} codec_message_spec_t;

typedef struct codec_map_spec {
    char *name;
    size_t name_length;
    char *version;
    char *description;
    codec_message_spec_t *messages;
    size_t message_count;
    size_t total_signals;
    size_t total_mappings;
} codec_map_spec_t;

static const char *const CODEC_TOP_KEYS[] = {"schema_version", "codec_map"};
static const char *const CODEC_MAP_KEYS[] = {"name", "version", "description", "messages"};
static const char *const CODEC_MESSAGE_KEYS[] = {"id", "name", "dlc", "period_ms",
                                                 "description", "signals"};
static const char *const CODEC_SIGNAL_KEYS[] = {"name",       "start_bit", "bit_length",
                                                "type",       "endianness", "layout",
                                                "bit_layout", "scale",     "offset",
                                                "unit",       "values",    "min",
                                                "max",        "strict"};

static bool codec_parse_signal(const codec_ast_node_t *node,
                               codec_arena_t *arena,
                               uint32_t message_id,
                               codec_signal_spec_t *spec,
                               cancestry_codec_load_error_t *error)
{
    size_t index = 0u;
    char *type_text;
    size_t type_length;
    char *endianness_text;
    size_t endianness_length;

    memset(spec, 0, sizeof(*spec));
    spec->node = node;
    spec->scale = 1.0;
    spec->offset = 0.0;
    spec->strict = true;
    spec->message_id = message_id;

    if (node->kind != CODEC_AST_MAP) {
        return codec_load_fail(error, CANCESTRY_CODEC_ERR_PARSE, node->line, node->column,
                               "signal must be a mapping");
    }
    if (!codec_check_unknown_keys(node, CODEC_SIGNAL_KEYS,
                                  sizeof(CODEC_SIGNAL_KEYS) / sizeof(CODEC_SIGNAL_KEYS[0]),
                                  "signal", error)) {
        return false;
    }
    if (!codec_ast_find_pair(node, "name", &index)) {
        return codec_load_fail(error, CANCESTRY_CODEC_ERR_PARSE, node->line, node->column,
                               "signal is missing required field 'name'");
    }
    if (!codec_field_string(node->pairs[index].value, arena, &spec->name, &spec->name_length) ||
        spec->name_length == 0u) {
        return codec_load_fail(error, CANCESTRY_CODEC_ERR_PARSE, node->pairs[index].line,
                               node->pairs[index].column, "signal 'name' must be a string");
    }
    if (spec->name_length > CANCESTRY_CODEC_NAME_MAX) {
        return codec_load_fail(error, CANCESTRY_CODEC_ERR_PARSE, node->pairs[index].line,
                               node->pairs[index].column, "signal name too long (max %u)",
                               (unsigned)CANCESTRY_CODEC_NAME_MAX);
    }
    if (!codec_ast_find_pair(node, "start_bit", &index)) {
        return codec_load_fail(error, CANCESTRY_CODEC_ERR_PARSE, node->line, node->column,
                               "signal '%s' is missing required field 'start_bit'", spec->name);
    }
    {
        int64_t value = 0;
        if (!codec_field_int(node->pairs[index].value, false, 0, 63, &value, error,
                             "start_bit")) {
            return false;
        }
        spec->start_bit = (uint32_t)value;
    }
    if (!codec_ast_find_pair(node, "bit_length", &index)) {
        return codec_load_fail(error, CANCESTRY_CODEC_ERR_PARSE, node->line, node->column,
                               "signal '%s' is missing required field 'bit_length'", spec->name);
    }
    {
        int64_t value = 0;
        if (!codec_field_int(node->pairs[index].value, false, 1, 64, &value, error,
                             "bit_length")) {
            return false;
        }
        spec->bit_length = (uint32_t)value;
    }
    if (!codec_ast_find_pair(node, "type", &index)) {
        return codec_load_fail(error, CANCESTRY_CODEC_ERR_PARSE, node->line, node->column,
                               "signal '%s' is missing required field 'type'", spec->name);
    }
    if (!codec_field_string(node->pairs[index].value, arena, &type_text, &type_length)) {
        return codec_load_fail(error, CANCESTRY_CODEC_ERR_PARSE, node->pairs[index].line,
                               node->pairs[index].column, "signal 'type' must be a string");
    }
    if (type_length == 4u && memcmp(type_text, "uint", 4u) == 0) {
        spec->type = CANCESTRY_CODEC_SIGNAL_TYPE_UINT;
    } else if (type_length == 3u && memcmp(type_text, "int", 3u) == 0) {
        spec->type = CANCESTRY_CODEC_SIGNAL_TYPE_INT;
    } else if (type_length == 7u && memcmp(type_text, "boolean", 7u) == 0) {
        spec->type = CANCESTRY_CODEC_SIGNAL_TYPE_BOOLEAN;
    } else if (type_length == 4u && memcmp(type_text, "enum", 4u) == 0) {
        spec->type = CANCESTRY_CODEC_SIGNAL_TYPE_ENUM;
    } else {
        return codec_load_fail(error, CANCESTRY_CODEC_ERR_PARSE, node->pairs[index].line,
                               node->pairs[index].column,
                               "signal 'type' must be one of uint, int, boolean, enum");
    }
    if (!codec_ast_find_pair(node, "endianness", &index)) {
        return codec_load_fail(error, CANCESTRY_CODEC_ERR_PARSE, node->line, node->column,
                               "signal '%s' is missing required field 'endianness'", spec->name);
    }
    if (!codec_field_string(node->pairs[index].value, arena, &endianness_text,
                            &endianness_length)) {
        return codec_load_fail(error, CANCESTRY_CODEC_ERR_PARSE, node->pairs[index].line,
                               node->pairs[index].column, "signal 'endianness' must be a string");
    }
    if (endianness_length == 6u && memcmp(endianness_text, "little", 6u) == 0) {
        spec->endianness = CANCESTRY_CODEC_ENDIANNESS_LITTLE;
    } else if (endianness_length == 3u && memcmp(endianness_text, "big", 3u) == 0) {
        spec->endianness = CANCESTRY_CODEC_ENDIANNESS_BIG;
    } else {
        return codec_load_fail(error, CANCESTRY_CODEC_ERR_PARSE, node->pairs[index].line,
                               node->pairs[index].column,
                               "signal 'endianness' must be 'little' or 'big'");
    }

    /* Layout: optional, defaults to contiguous. Accept "layout" and alias "bit_layout". */
    spec->layout = CANCESTRY_CODEC_LAYOUT_CONTIGUOUS;
    {
        size_t layout_index;
        bool has_layout = false;
        char *layout_text = NULL;
        size_t layout_length = 0u;
        if (codec_ast_find_pair(node, "layout", &layout_index)) {
            has_layout = true;
            if (!codec_field_string(node->pairs[layout_index].value, arena, &layout_text,
                                    &layout_length)) {
                return codec_load_fail(error, CANCESTRY_CODEC_ERR_PARSE,
                                       node->pairs[layout_index].line,
                                       node->pairs[layout_index].column,
                                       "signal 'layout' must be a string");
            }
        }
        if (codec_ast_find_pair(node, "bit_layout", &layout_index)) {
            if (has_layout) {
                return codec_load_fail(error, CANCESTRY_CODEC_ERR_PARSE,
                                       node->pairs[layout_index].line,
                                       node->pairs[layout_index].column,
                                       "signal must not specify both 'layout' and 'bit_layout'");
            }
            if (!codec_field_string(node->pairs[layout_index].value, arena, &layout_text,
                                    &layout_length)) {
                return codec_load_fail(error, CANCESTRY_CODEC_ERR_PARSE,
                                       node->pairs[layout_index].line,
                                       node->pairs[layout_index].column,
                                       "signal 'layout' must be a string");
            }
            has_layout = true;
        }
        if (has_layout) {
            if (layout_length == 10u && memcmp(layout_text, "contiguous", 10u) == 0) {
                spec->layout = CANCESTRY_CODEC_LAYOUT_CONTIGUOUS;
            } else if (layout_length == 8u && memcmp(layout_text, "sawtooth", 8u) == 0) {
                spec->layout = CANCESTRY_CODEC_LAYOUT_SAWTOOTH;
            } else {
                return codec_load_fail(error, CANCESTRY_CODEC_ERR_PARSE,
                                       node->pairs[layout_index].line,
                                       node->pairs[layout_index].column,
                                       "signal 'layout' must be 'contiguous' or 'sawtooth'");
            }
            if (spec->layout == CANCESTRY_CODEC_LAYOUT_SAWTOOTH &&
                spec->endianness != CANCESTRY_CODEC_ENDIANNESS_BIG) {
                return codec_load_fail(error, CANCESTRY_CODEC_ERR_PARSE, node->line,
                                       node->column,
                                       "sawtooth layout is only valid for big-endian signals (signal '%s')",
                                       spec->name);
            }
        }
    }

    if (codec_ast_find_pair(node, "scale", &index)) {
        if (!codec_field_double(node->pairs[index].value, &spec->scale, error, "scale")) {
            return false;
        }
    }
    if (codec_ast_find_pair(node, "offset", &index)) {
        if (!codec_field_double(node->pairs[index].value, &spec->offset, error, "offset")) {
            return false;
        }
    }
    if (spec->type == CANCESTRY_CODEC_SIGNAL_TYPE_UINT ||
        spec->type == CANCESTRY_CODEC_SIGNAL_TYPE_INT) {
        if (spec->scale == 0.0) {
            return codec_load_fail(error, CANCESTRY_CODEC_ERR_PARSE, node->line, node->column,
                                   "signal '%s' has scale 0, which cannot be encoded", spec->name);
        }
    }
    if (codec_ast_find_pair(node, "unit", &index)) {
        if (!codec_field_string(node->pairs[index].value, arena, &spec->unit, NULL) ||
            spec->unit[0] == '\0') {
            return codec_load_fail(error, CANCESTRY_CODEC_ERR_PARSE, node->pairs[index].line,
                                   node->pairs[index].column, "signal 'unit' must be a string");
        }
        if (strlen(spec->unit) > CANCESTRY_CODEC_UNIT_MAX) {
            return codec_load_fail(error, CANCESTRY_CODEC_ERR_PARSE, node->pairs[index].line,
                                   node->pairs[index].column, "signal unit too long (max %u)",
                                   (unsigned)CANCESTRY_CODEC_UNIT_MAX);
        }
    }
    if (codec_ast_find_pair(node, "values", &index)) {
        size_t i;
        const codec_ast_node_t *values = node->pairs[index].value;
        if (values->kind != CODEC_AST_MAP) {
            return codec_load_fail(error, CANCESTRY_CODEC_ERR_PARSE, values->line,
                                   values->column, "signal 'values' must be a mapping");
        }
        spec->mapping_count = values->pair_count;
        spec->mappings = (codec_value_spec_t *)codec_arena_alloc(arena, values->pair_count *
                                                                             sizeof(*spec->mappings));
        if (spec->mappings == NULL && values->pair_count > 0u) {
            return codec_load_fail(error, CANCESTRY_CODEC_ERR_NO_MEMORY, values->line,
                                   values->column, "out of memory");
        }
        for (i = 0u; i < values->pair_count; ++i) {
            const codec_ast_pair_t *pair = &values->pairs[i];
            size_t k;
            if (pair->key_length == 0u) {
                return codec_load_fail(error, CANCESTRY_CODEC_ERR_PARSE, pair->line,
                                       pair->column,
                                       "signal 'values' keys must be non-negative integers");
            }
            for (k = 0u; k < pair->key_length; ++k) {
                if (pair->key[k] < '0' || pair->key[k] > '9') {
                    return codec_load_fail(error, CANCESTRY_CODEC_ERR_PARSE, pair->line,
                                           pair->column,
                                           "signal 'values' keys must be non-negative integers");
                }
            }
            if (!codec_parse_uint64(pair->key, pair->key_length, &spec->mappings[i].raw)) {
                return codec_load_fail(error, CANCESTRY_CODEC_ERR_PARSE, pair->line,
                                       pair->column, "signal 'values' key is out of range");
            }
            if (spec->bit_length < 64u &&
                spec->mappings[i].raw >= (UINT64_C(1) << spec->bit_length)) {
                return codec_load_fail(error, CANCESTRY_CODEC_ERR_PARSE, pair->line,
                                       pair->column,
                                       "signal 'values' key %llu does not fit in %u bits",
                                       (unsigned long long)spec->mappings[i].raw,
                                       (unsigned)spec->bit_length);
            }
            if (!codec_field_string(pair->value, arena, &spec->mappings[i].label,
                                    &spec->mappings[i].label_length) ||
                spec->mappings[i].label_length == 0u) {
                return codec_load_fail(error, CANCESTRY_CODEC_ERR_PARSE, pair->line,
                                       pair->column, "signal 'values' labels must be strings");
            }
            if (spec->mappings[i].label_length > CANCESTRY_CODEC_LABEL_MAX) {
                return codec_load_fail(error, CANCESTRY_CODEC_ERR_PARSE, pair->line,
                                       pair->column, "signal value label too long (max %u)",
                                       (unsigned)CANCESTRY_CODEC_LABEL_MAX);
            }
        }
    }
    if (codec_ast_find_pair(node, "min", &index)) {
        if (!codec_field_double(node->pairs[index].value, &spec->min, error, "min")) {
            return false;
        }
        spec->has_min = true;
    }
    if (codec_ast_find_pair(node, "max", &index)) {
        if (!codec_field_double(node->pairs[index].value, &spec->max, error, "max")) {
            return false;
        }
        spec->has_max = true;
    }
    if (spec->has_min && spec->has_max && spec->min > spec->max) {
        return codec_load_fail(error, CANCESTRY_CODEC_ERR_PARSE, node->line, node->column,
                               "signal '%s' declares min above max", spec->name);
    }
    if (codec_ast_find_pair(node, "strict", &index)) {
        if (!codec_field_bool(node->pairs[index].value, &spec->strict, error, "strict")) {
            return false;
        }
    }

    /* Schema conditional rules. */
    if (spec->type == CANCESTRY_CODEC_SIGNAL_TYPE_BOOLEAN && spec->bit_length != 1u) {
        return codec_load_fail(error, CANCESTRY_CODEC_ERR_PARSE, node->line, node->column,
                               "boolean signal '%s' must be exactly 1 bit wide", spec->name);
    }
    if (spec->type == CANCESTRY_CODEC_SIGNAL_TYPE_ENUM && spec->mappings == NULL) {
        return codec_load_fail(error, CANCESTRY_CODEC_ERR_PARSE, node->line, node->column,
                               "enum signal '%s' requires a 'values' mapping", spec->name);
    }

    /* Bit-model sanity (codec-map-spec.md sections 4-5, 5.1). */
    if (spec->layout == CANCESTRY_CODEC_LAYOUT_SAWTOOTH) {
        /* Sawtooth: validate that the sawtooth bit set fits in 64 bits. */
        uint32_t idx = (spec->start_bit >> 3u) * 8u + (7u - (spec->start_bit & 7u));
        if (idx + spec->bit_length > CANCESTRY_CODEC_PAYLOAD_MAX_BITS) {
            return codec_load_fail(error, CANCESTRY_CODEC_ERR_PARSE, node->line, node->column,
                                   "sawtooth signal '%s' exceeds the 64-bit payload",
                                   spec->name);
        }
    } else if (spec->endianness == CANCESTRY_CODEC_ENDIANNESS_LITTLE) {
        if ((uint64_t)spec->start_bit + (uint64_t)spec->bit_length >
            CANCESTRY_CODEC_PAYLOAD_MAX_BITS) {
            return codec_load_fail(error, CANCESTRY_CODEC_ERR_PARSE, node->line, node->column,
                                   "little-endian signal '%s' exceeds the 64-bit payload",
                                   spec->name);
        }
    } else {
        if (spec->start_bit < spec->bit_length - 1u) {
            return codec_load_fail(error, CANCESTRY_CODEC_ERR_PARSE, node->line, node->column,
                                   "big-endian signal '%s' extends below payload bit 0",
                                   spec->name);
        }
    }
    return true;
}

static bool codec_parse_message(const codec_ast_node_t *node,
                                codec_arena_t *arena,
                                codec_message_spec_t *spec,
                                cancestry_codec_load_error_t *error)
{
    size_t index = 0u;
    size_t i;

    memset(spec, 0, sizeof(*spec));
    spec->node = node;

    if (node->kind != CODEC_AST_MAP) {
        return codec_load_fail(error, CANCESTRY_CODEC_ERR_PARSE, node->line, node->column,
                               "message must be a mapping");
    }
    if (!codec_check_unknown_keys(node, CODEC_MESSAGE_KEYS,
                                  sizeof(CODEC_MESSAGE_KEYS) / sizeof(CODEC_MESSAGE_KEYS[0]),
                                  "message", error)) {
        return false;
    }
    if (!codec_ast_find_pair(node, "id", &index)) {
        return codec_load_fail(error, CANCESTRY_CODEC_ERR_PARSE, node->line, node->column,
                               "message is missing required field 'id'");
    }
    {
        int64_t value = 0;
        if (!codec_field_int(node->pairs[index].value, false, 0, CODEC_LOAD_CAN_ID_MAX, &value,
                             error, "id")) {
            return false;
        }
        spec->id = (uint32_t)value;
    }
    if (!codec_ast_find_pair(node, "name", &index)) {
        return codec_load_fail(error, CANCESTRY_CODEC_ERR_PARSE, node->line, node->column,
                               "message is missing required field 'name'");
    }
    if (!codec_field_string(node->pairs[index].value, arena, &spec->name, &spec->name_length) ||
        spec->name_length == 0u) {
        return codec_load_fail(error, CANCESTRY_CODEC_ERR_PARSE, node->pairs[index].line,
                               node->pairs[index].column, "message 'name' must be a string");
    }
    if (spec->name_length > CANCESTRY_CODEC_NAME_MAX) {
        return codec_load_fail(error, CANCESTRY_CODEC_ERR_PARSE, node->pairs[index].line,
                               node->pairs[index].column, "message name too long (max %u)",
                               (unsigned)CANCESTRY_CODEC_NAME_MAX);
    }
    if (!codec_ast_find_pair(node, "dlc", &index)) {
        return codec_load_fail(error, CANCESTRY_CODEC_ERR_PARSE, node->line, node->column,
                               "message '%s' is missing required field 'dlc'", spec->name);
    }
    {
        int64_t value = 0;
        if (!codec_field_int(node->pairs[index].value, false, 0, 8, &value, error, "dlc")) {
            return false;
        }
        spec->dlc = (uint8_t)value;
    }
    if (codec_ast_find_pair(node, "period_ms", &index)) {
        int64_t value = 0;
        if (!codec_field_int(node->pairs[index].value, false, 0, INT64_C(0xFFFFFFFF), &value,
                             error, "period_ms")) {
            return false;
        }
        spec->period_ms = (uint32_t)value;
    }
    if (codec_ast_find_pair(node, "description", &index)) {
        if (!codec_field_string(node->pairs[index].value, arena, &spec->description, NULL)) {
            return codec_load_fail(error, CANCESTRY_CODEC_ERR_PARSE, node->pairs[index].line,
                                   node->pairs[index].column,
                                   "message 'description' must be a string");
        }
        if (strlen(spec->description) > CODEC_LOAD_DESCRIPTION_MAX) {
            return codec_load_fail(error, CANCESTRY_CODEC_ERR_PARSE, node->pairs[index].line,
                                   node->pairs[index].column, "message description too long");
        }
    }
    if (!codec_ast_find_pair(node, "signals", &index)) {
        return codec_load_fail(error, CANCESTRY_CODEC_ERR_PARSE, node->line, node->column,
                               "message '%s' is missing required field 'signals'", spec->name);
    }
    if (node->pairs[index].value->kind != CODEC_AST_SEQ ||
        node->pairs[index].value->item_count == 0u) {
        return codec_load_fail(error, CANCESTRY_CODEC_ERR_PARSE, node->pairs[index].line,
                               node->pairs[index].column,
                               "message 'signals' must be a non-empty sequence");
    }
    spec->signal_count = node->pairs[index].value->item_count;
    spec->signals =
        (codec_signal_spec_t *)codec_arena_alloc(arena, spec->signal_count * sizeof(*spec->signals));
    if (spec->signals == NULL) {
        return codec_load_fail(error, CANCESTRY_CODEC_ERR_NO_MEMORY, node->line, node->column,
                               "out of memory");
    }
    for (i = 0u; i < spec->signal_count; ++i) {
        if (!codec_parse_signal(node->pairs[index].value->items[i], arena, spec->id,
                                &spec->signals[i], error)) {
            return false;
        }
    }
    return true;
}

static bool codec_validate_map(const codec_ast_node_t *root,
                               codec_arena_t *arena,
                               codec_map_spec_t *spec,
                               cancestry_codec_load_error_t *error)
{
    size_t index = 0u;
    const codec_ast_node_t *map_node;
    size_t i;
    size_t m;

    memset(spec, 0, sizeof(*spec));
    if (root->kind != CODEC_AST_MAP) {
        return codec_load_fail(error, CANCESTRY_CODEC_ERR_PARSE, root->line, root->column,
                               "the document root must be a mapping");
    }
    if (!codec_check_unknown_keys(root, CODEC_TOP_KEYS,
                                  sizeof(CODEC_TOP_KEYS) / sizeof(CODEC_TOP_KEYS[0]),
                                  "document root", error)) {
        return false;
    }
    if (!codec_ast_find_pair(root, "schema_version", &index)) {
        return codec_load_fail(error, CANCESTRY_CODEC_ERR_PARSE, root->line, root->column,
                               "missing required field 'schema_version'");
    }
    {
        const codec_ast_node_t *version = root->pairs[index].value;
        bool ok = false;
        if (version->kind == CODEC_AST_SCALAR && version->text_length == 5u) {
            if (memcmp(version->text, "0.2.0", 5u) == 0 || memcmp(version->text, "0.3.0", 5u) == 0) {
                ok = true;
            }
        }
        if (!ok) {
            return codec_load_fail(error, CANCESTRY_CODEC_ERR_PARSE, version->line,
                                   version->column, "'schema_version' must be \"0.2.0\" or \"0.3.0\"");
        }
    }
    if (!codec_ast_find_pair(root, "codec_map", &index)) {
        return codec_load_fail(error, CANCESTRY_CODEC_ERR_PARSE, root->line, root->column,
                               "missing required field 'codec_map'");
    }
    map_node = root->pairs[index].value;
    if (map_node->kind != CODEC_AST_MAP) {
        return codec_load_fail(error, CANCESTRY_CODEC_ERR_PARSE, map_node->line,
                               map_node->column, "'codec_map' must be a mapping");
    }
    if (!codec_check_unknown_keys(map_node, CODEC_MAP_KEYS,
                                  sizeof(CODEC_MAP_KEYS) / sizeof(CODEC_MAP_KEYS[0]),
                                  "codec_map", error)) {
        return false;
    }

    if (!codec_ast_find_pair(map_node, "name", &index)) {
        return codec_load_fail(error, CANCESTRY_CODEC_ERR_PARSE, map_node->line,
                               map_node->column, "codec_map is missing required field 'name'");
    }
    if (!codec_field_string(map_node->pairs[index].value, arena, &spec->name,
                            &spec->name_length) ||
        spec->name_length == 0u) {
        return codec_load_fail(error, CANCESTRY_CODEC_ERR_PARSE, map_node->pairs[index].line,
                               map_node->pairs[index].column, "codec_map 'name' must be a string");
    }
    if (spec->name_length > CANCESTRY_CODEC_NAME_MAX) {
        return codec_load_fail(error, CANCESTRY_CODEC_ERR_PARSE, map_node->pairs[index].line,
                               map_node->pairs[index].column, "codec map name too long (max %u)",
                               (unsigned)CANCESTRY_CODEC_NAME_MAX);
    }
    {
        size_t k;
        for (k = 0u; k < spec->name_length; ++k) {
            if (spec->name[k] == '.') {
                return codec_load_fail(error, CANCESTRY_CODEC_ERR_PARSE,
                                       map_node->pairs[index].line, map_node->pairs[index].column,
                                       "codec map names must not contain '.'");
            }
        }
    }
    if (!codec_ast_find_pair(map_node, "version", &index)) {
        return codec_load_fail(error, CANCESTRY_CODEC_ERR_PARSE, map_node->line,
                               map_node->column, "codec_map is missing required field 'version'");
    }
    if (!codec_field_string(map_node->pairs[index].value, arena, &spec->version, NULL) ||
        !codec_version_pattern_ok(spec->version, strlen(spec->version))) {
        return codec_load_fail(error, CANCESTRY_CODEC_ERR_PARSE, map_node->pairs[index].line,
                               map_node->pairs[index].column,
                               "codec_map 'version' must match \"<digits>.<digits>.<digits>\"");
    }
    if (strlen(spec->version) > CODEC_LOAD_VERSION_MAX) {
        return codec_load_fail(error, CANCESTRY_CODEC_ERR_PARSE, map_node->pairs[index].line,
                               map_node->pairs[index].column, "codec map version too long");
    }
    if (codec_ast_find_pair(map_node, "description", &index)) {
        if (!codec_field_string(map_node->pairs[index].value, arena, &spec->description, NULL)) {
            return codec_load_fail(error, CANCESTRY_CODEC_ERR_PARSE, map_node->pairs[index].line,
                                   map_node->pairs[index].column,
                                   "codec_map 'description' must be a string");
        }
        if (strlen(spec->description) > CODEC_LOAD_DESCRIPTION_MAX) {
            return codec_load_fail(error, CANCESTRY_CODEC_ERR_PARSE, map_node->pairs[index].line,
                                   map_node->pairs[index].column, "codec map description too long");
        }
    }
    if (!codec_ast_find_pair(map_node, "messages", &index)) {
        return codec_load_fail(error, CANCESTRY_CODEC_ERR_PARSE, map_node->line,
                               map_node->column, "codec_map is missing required field 'messages'");
    }
    {
        const codec_ast_node_t *messages = map_node->pairs[index].value;
        if (messages->kind != CODEC_AST_SEQ || messages->item_count == 0u) {
            return codec_load_fail(error, CANCESTRY_CODEC_ERR_PARSE, messages->line,
                                   messages->column, "'messages' must be a non-empty sequence");
        }
        if (messages->item_count > UINT16_MAX) {
            return codec_load_fail(error, CANCESTRY_CODEC_ERR_PARSE, messages->line,
                                   messages->column, "too many messages (max %u)",
                                   (unsigned)UINT16_MAX);
        }
        spec->message_count = messages->item_count;
        spec->messages =
            (codec_message_spec_t *)codec_arena_alloc(arena, spec->message_count *
                                                                 sizeof(*spec->messages));
        if (spec->messages == NULL) {
            return codec_load_fail(error, CANCESTRY_CODEC_ERR_NO_MEMORY, messages->line,
                                   messages->column, "out of memory");
        }
        for (i = 0u; i < spec->message_count; ++i) {
            if (!codec_parse_message(messages->items[i], arena, &spec->messages[i], error)) {
                return false;
            }
        }
    }

    /* Duplicate message ids are a definition conflict (SW-FR-CODEC-007). */
    for (m = 0u; m < spec->message_count; ++m) {
        size_t n;
        for (n = m + 1u; n < spec->message_count; ++n) {
            if (spec->messages[m].id == spec->messages[n].id) {
                return codec_load_fail(error, CANCESTRY_CODEC_ERR_CONFLICT,
                                       spec->messages[n].node->line,
                                       spec->messages[n].node->column,
                                       "duplicate message id %u (messages '%s' and '%s')",
                                       (unsigned)spec->messages[m].id, spec->messages[m].name,
                                       spec->messages[n].name);
            }
        }
    }
    /* Duplicate short signal names within the map are a conflict too. */
    for (m = 0u; m < spec->message_count; ++m) {
        size_t s;
        for (s = 0u; s < spec->messages[m].signal_count; ++s) {
            size_t mm;
            size_t ss;
            for (mm = m; mm < spec->message_count; ++mm) {
                size_t first = (mm == m) ? s + 1u : 0u;
                for (ss = first; ss < spec->messages[mm].signal_count; ++ss) {
                    if (strcmp(spec->messages[m].signals[s].name,
                               spec->messages[mm].signals[ss].name) == 0) {
                        return codec_load_fail(
                            error, CANCESTRY_CODEC_ERR_CONFLICT,
                            spec->messages[mm].signals[ss].node->line,
                            spec->messages[mm].signals[ss].node->column,
                            "duplicate signal name '%s' in codec map '%s'",
                            spec->messages[m].signals[s].name, spec->name);
                    }
                }
            }
        }
    }

    for (m = 0u; m < spec->message_count; ++m) {
        spec->total_signals += spec->messages[m].signal_count;
        for (i = 0u; i < spec->messages[m].signal_count; ++i) {
            spec->total_mappings += spec->messages[m].signals[i].mapping_count;
        }
    }
    if (spec->total_signals > UINT16_MAX) {
        return codec_load_fail(error, CANCESTRY_CODEC_ERR_PARSE, root->line, root->column,
                               "too many signals in one codec map (max %u)",
                               (unsigned)UINT16_MAX);
    }
    return true;
}

/* ------------------------------------------------------------------------- */
/* Final block build                                                         */
/* ------------------------------------------------------------------------- */

static size_t codec_align8(size_t value)
{
    return (value + 7u) & ~((size_t)7u);
}

/** Reserve @p size bytes at an 8-byte-aligned offset. */
static void *codec_blob_reserve(char *blob, size_t *cursor, size_t size)
{
    char *result = blob + codec_align8(*cursor);

    *cursor = codec_align8(*cursor) + size;
    return result;
}

static char *codec_blob_string(char *blob, size_t *cursor, const char *text)
{
    size_t length = strlen(text);
    char *result = blob + *cursor;

    memcpy(result, text, length + 1u);
    *cursor += length + 1u;
    return result;
}

static cancestry_codec_map_t *codec_build_map(const codec_map_spec_t *spec,
                                              cancestry_codec_load_error_t *error)
{
    size_t cursor = 0u;
    size_t size;
    cancestry_codec_map_t *map;
    cancestry_codec_message_t *messages;
    cancestry_codec_signal_t *signals;
    cancestry_codec_value_mapping_t *mappings;
    size_t m;
    size_t i;

    size = codec_align8(sizeof(*map));
    size += codec_align8(spec->message_count * sizeof(*messages));
    size += codec_align8(spec->total_signals * sizeof(*signals));
    size += codec_align8(spec->total_mappings * sizeof(*mappings));
    if (spec->name != NULL) {
        size += strlen(spec->name) + 1u;
    }
    if (spec->version != NULL) {
        size += strlen(spec->version) + 1u;
    }
    if (spec->description != NULL) {
        size += strlen(spec->description) + 1u;
    }
    for (m = 0u; m < spec->message_count; ++m) {
        size += strlen(spec->messages[m].name) + 1u;
        if (spec->messages[m].description != NULL) {
            size += strlen(spec->messages[m].description) + 1u;
        }
        for (i = 0u; i < spec->messages[m].signal_count; ++i) {
            const codec_signal_spec_t *s = &spec->messages[m].signals[i];
            size_t v;
            size += strlen(s->name) + 1u;
            if (s->unit != NULL) {
                size += strlen(s->unit) + 1u;
            }
            for (v = 0u; v < s->mapping_count; ++v) {
                size += strlen(s->mappings[v].label) + 1u;
            }
        }
    }

    map = (cancestry_codec_map_t *)malloc(size);
    if (map == NULL) {
        codec_load_fail(error, CANCESTRY_CODEC_ERR_NO_MEMORY, 0u, 0u, "out of memory");
        return NULL;
    }
    memset(map, 0, size);

    cursor = codec_align8(sizeof(*map));
    messages = (cancestry_codec_message_t *)codec_blob_reserve(
        (char *)map, &cursor, spec->message_count * sizeof(*messages));
    signals = (cancestry_codec_signal_t *)codec_blob_reserve(
        (char *)map, &cursor, spec->total_signals * sizeof(*signals));
    mappings = (cancestry_codec_value_mapping_t *)codec_blob_reserve(
        (char *)map, &cursor, spec->total_mappings * sizeof(*mappings));

    map->name = codec_blob_string((char *)map, &cursor, spec->name);
    map->version = codec_blob_string((char *)map, &cursor, spec->version);
    map->description = spec->description != NULL
                           ? codec_blob_string((char *)map, &cursor, spec->description)
                           : NULL;
    map->messages = messages;
    map->message_count = (uint16_t)spec->message_count;

    {
        size_t signal_cursor = 0u;
        size_t mapping_cursor = 0u;
        for (m = 0u; m < spec->message_count; ++m) {
            const codec_message_spec_t *ms = &spec->messages[m];
            cancestry_codec_message_t *message = &messages[m];

            message->id = ms->id;
            message->name = codec_blob_string((char *)map, &cursor, ms->name);
            message->dlc = ms->dlc;
            message->period_ms = ms->period_ms;
            message->description = ms->description != NULL
                                       ? codec_blob_string((char *)map, &cursor, ms->description)
                                       : NULL;
            message->signals = &signals[signal_cursor];
            message->signal_count = (uint16_t)ms->signal_count;
            for (i = 0u; i < ms->signal_count; ++i) {
                const codec_signal_spec_t *ss = &ms->signals[i];
                cancestry_codec_signal_t *signal = &signals[signal_cursor + i];
                size_t v;

                signal->name = codec_blob_string((char *)map, &cursor, ss->name);
                signal->start_bit = ss->start_bit;
                signal->bit_length = ss->bit_length;
                signal->type = ss->type;
                signal->endianness = ss->endianness;
                signal->layout = ss->layout;
                signal->scale = ss->scale;
                signal->offset = ss->offset;
                signal->unit =
                    ss->unit != NULL ? codec_blob_string((char *)map, &cursor, ss->unit) : NULL;
                if (ss->mapping_count > 0u) {
                    signal->values = &mappings[mapping_cursor];
                    signal->value_count = (uint16_t)ss->mapping_count;
                    for (v = 0u; v < ss->mapping_count; ++v) {
                        mappings[mapping_cursor + v].raw = ss->mappings[v].raw;
                        mappings[mapping_cursor + v].name =
                            codec_blob_string((char *)map, &cursor, ss->mappings[v].label);
                    }
                    mapping_cursor += ss->mapping_count;
                } else {
                    signal->values = NULL;
                    signal->value_count = 0u;
                }
                signal->has_min = ss->has_min;
                signal->has_max = ss->has_max;
                signal->min = ss->min;
                signal->max = ss->max;
                signal->strict = ss->strict;
                if (ss->layout == CANCESTRY_CODEC_LAYOUT_SAWTOOTH) {
                    /* Compute min/max of the sawtooth set for frame-size checks. */
                    uint32_t idx = (ss->start_bit >> 3u) * 8u + (7u - (ss->start_bit & 7u));
                    uint32_t min_p = UINT32_MAX;
                    uint32_t max_p = 0u;
                    uint32_t k;
                    for (k = 0u; k < ss->bit_length; ++k) {
                        uint32_t p = ((idx + k) >> 3u) * 8u + (7u - ((idx + k) & 7u));
                        if (p < min_p) {
                            min_p = p;
                        }
                        if (p > max_p) {
                            max_p = p;
                        }
                    }
                    signal->first_bit = min_p;
                    signal->last_bit = max_p;
                } else if (ss->endianness == CANCESTRY_CODEC_ENDIANNESS_LITTLE) {
                    signal->first_bit = ss->start_bit;
                    signal->last_bit = ss->start_bit + ss->bit_length - 1u;
                } else {
                    signal->first_bit = ss->start_bit - (ss->bit_length - 1u);
                    signal->last_bit = ss->start_bit;
                }
                signal->message_id = ss->message_id;
            }
            signal_cursor += ms->signal_count;
        }
    }

    return map;
}

/* ------------------------------------------------------------------------- */
/* Public API                                                                */
/* ------------------------------------------------------------------------- */

cancestry_codec_map_t *cancestry_codec_map_load(const char *text,
                                                size_t length,
                                                cancestry_codec_load_error_t *error)
{
    codec_load_context_t context;
    codec_ast_node_t *root = NULL;
    codec_map_spec_t spec;
    cancestry_codec_map_t *map;

    if (text == NULL) {
        codec_load_fail(error, CANCESTRY_CODEC_ERR_NULL, 0u, 0u, "text is NULL");
        return NULL;
    }
    if (length == 0u) {
        codec_load_fail(error, CANCESTRY_CODEC_ERR_PARSE, 1u, 1u, "document is empty");
        return NULL;
    }
    if (memchr(text, '\0', length) != NULL) {
        codec_load_fail(error, CANCESTRY_CODEC_ERR_PARSE, 1u, 1u,
                        "NUL bytes are not allowed in a codec map document");
        return NULL;
    }

    memset(&context, 0, sizeof(context));
    context.text = text;
    context.length = length;
    context.pos = 0u;
    context.line = 1u;
    context.column = 1u;

    if (!codec_load_parse_document(&context, &root, error)) {
        codec_arena_free(&context.arena);
        return NULL;
    }
    if (!codec_validate_map(root, &context.arena, &spec, error)) {
        codec_arena_free(&context.arena);
        return NULL;
    }
    map = codec_build_map(&spec, error);
    codec_arena_free(&context.arena);
    if (map != NULL && error != NULL) {
        memset(error, 0, sizeof(*error));
        error->status = CANCESTRY_CODEC_OK;
    }
    return map;
}

void cancestry_codec_map_free(cancestry_codec_map_t *map)
{
    free(map);
}
