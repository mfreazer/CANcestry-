/*
 * CANcestry - UDS configuration loader.
 *
 * Implementation notes:
 *   - The loader speaks the same strict, documented YAML subset as the codec,
 *     recipe and FSM loaders: block mappings and block sequences only, no
 *     flow collections, anchors, aliases, tags, directives or multi-line
 *     scalars. Anything outside the subset is rejected with a line/column
 *     error instead of being guessed at.
 *   - Validation implements every constraint of
 *     schemas/uds-0.1.0.schema.json in C (required fields, field types,
 *     ranges, additionalProperties, the conditional DID rules), so no
 *     external JSON Schema validator runs at load time (SW-FR-UDS-006,
 *     agents.md "Schema is law"). On top of the schema it enforces the
 *     structural rules the runtime depends on: duplicate DID/routine ids,
 *     signal-mapped DID lengths of at most 8 bytes, and default arrays whose
 *     length matches the DID length.
 *   - Memory: the parse tree and the compiled definitions share one bump
 *     arena that the returned config owns. The config header itself is one
 *     separate block. cancestry_uds_config_free() releases all of it in one
 *     call. Loading may allocate; the runtime never does (SYS-NF-002), which
 *     is why the loader is its own static library.
 */

#include "cancestry/uds/loader.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------------- */
/* Arena                                                                     */
/* ------------------------------------------------------------------------- */

/* Chunked bump allocator; chunks never move, so every pointer handed out
 * stays valid until the config is freed. */
typedef struct uds_arena_chunk {
    struct uds_arena_chunk *next;
    size_t capacity;
} uds_arena_chunk_t;

typedef struct uds_arena {
    uds_arena_chunk_t *head;
    char *cursor;
    size_t remaining;
} uds_arena_t;

static void *uds_arena_alloc(uds_arena_t *arena, size_t size)
{
    size_t aligned = (size + 7u) & ~((size_t)7u);

    if (aligned > arena->remaining) {
        size_t chunk_bytes = (aligned > 4096u) ? aligned + sizeof(uds_arena_chunk_t)
                                               : 8192u + sizeof(uds_arena_chunk_t);
        uds_arena_chunk_t *chunk = (uds_arena_chunk_t *)malloc(chunk_bytes);

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

static void *uds_arena_calloc(uds_arena_t *arena, size_t count, size_t size)
{
    void *result;

    if (count == 0u) {
        return NULL;
    }
    result = uds_arena_alloc(arena, count * size);
    if (result != NULL) {
        memset(result, 0, count * size);
    }
    return result;
}

static void uds_arena_release(uds_arena_t *arena)
{
    uds_arena_chunk_t *chunk = arena->head;

    while (chunk != NULL) {
        uds_arena_chunk_t *next = chunk->next;

        free(chunk);
        chunk = next;
    }
    memset(arena, 0, sizeof(*arena));
}

static char *uds_arena_strdup(uds_arena_t *arena, const char *text, size_t length)
{
    char *copy = (char *)uds_arena_alloc(arena, length + 1u);

    if (copy == NULL) {
        return NULL;
    }
    memcpy(copy, text, length);
    copy[length] = '\0';
    return copy;
}

/* ------------------------------------------------------------------------- */
/* Growable vector over the arena                                            */
/* ------------------------------------------------------------------------- */

typedef struct uds_vec {
    void *items;
    size_t count;
    size_t capacity;
    size_t element_size;
} uds_vec_t;

static void uds_vec_init(uds_vec_t *vec, size_t element_size)
{
    vec->items = NULL;
    vec->count = 0u;
    vec->capacity = 0u;
    vec->element_size = element_size;
}

/* Growth moves the array, so nothing may hold a pointer into it across a
 * push; the builder fills a complete vector before publishing its base. */
static bool uds_vec_push(uds_arena_t *arena, uds_vec_t *vec, const void *item)
{
    if (vec->count == vec->capacity) {
        size_t next_capacity = (vec->capacity == 0u) ? 4u : vec->capacity * 2u;
        void *next = uds_arena_calloc(arena, next_capacity, vec->element_size);
        size_t bytes = vec->count * vec->element_size;

        if (next == NULL) {
            return false;
        }
        if (bytes != 0u && vec->items != NULL) {
            memcpy(next, vec->items, bytes);
        }
        vec->items = next;
        vec->capacity = next_capacity;
    }
    memcpy((char *)vec->items + (vec->count * vec->element_size), item, vec->element_size);
    vec->count++;
    return true;
}

static void *uds_vec_seal(uds_vec_t *vec)
{
    return vec->items;
}

/* ------------------------------------------------------------------------- */
/* YAML subset parser                                                        */
/* ------------------------------------------------------------------------- */

typedef enum uds_node_kind {
    UDS_NODE_NULL = 0,
    UDS_NODE_SCALAR = 1,
    UDS_NODE_MAP = 2,
    UDS_NODE_SEQ = 3
} uds_node_kind_t;

typedef struct uds_node {
    uds_node_kind_t kind;
    const char *text;
    size_t text_length;
    bool quoted;
    size_t line;
    size_t column;
    struct uds_pair *pairs;
    size_t pair_count;
    struct uds_node **items;
    size_t item_count;
} uds_node_t;

typedef struct uds_pair {
    const char *key;
    size_t key_length;
    size_t line;
    size_t column;
    uds_node_t *value;
} uds_pair_t;

typedef struct uds_line {
    const char *start;
    size_t length;
    size_t indent;
    size_t number;
} uds_line_t;

typedef struct uds_parse {
    const char *text;
    size_t length;
    uds_line_t *lines;
    size_t line_count;
    size_t index;
    size_t offset;
    size_t offset_column;
    uds_arena_t *arena;
    cancestry_uds_load_error_t *error;
    bool failed;
} uds_parse_t;

static void uds_fail(uds_parse_t *parse,
                     cancestry_uds_status_t status,
                     size_t line,
                     size_t column,
                     const char *format,
                     ...)
{
    va_list args;

    if (parse != NULL && parse->failed) {
        return; /* keep the first diagnostic */
    }
    if (parse != NULL) {
        parse->failed = true;
    }
    if (parse == NULL || parse->error == NULL) {
        return;
    }
    memset(parse->error, 0, sizeof(*parse->error));
    parse->error->status = status;
    parse->error->line = line;
    parse->error->column = column;
    va_start(args, format);
    (void)vsnprintf(parse->error->message, sizeof(parse->error->message), format, args);
    va_end(args);
}

static bool uds_fail_at(uds_parse_t *parse, size_t line, size_t column, const char *format, ...)
{
    va_list args;

    if (parse->failed) {
        return false;
    }
    parse->failed = true;
    if (parse->error != NULL) {
        memset(parse->error, 0, sizeof(*parse->error));
        parse->error->status = CANCESTRY_UDS_ERR_PARSE;
        parse->error->line = line;
        parse->error->column = column;
        va_start(args, format);
        (void)vsnprintf(parse->error->message, sizeof(parse->error->message), format, args);
        va_end(args);
    }
    return false;
}

static bool uds_line_is_blank(const char *text, size_t length)
{
    size_t i;

    for (i = 0u; i < length; ++i) {
        if (text[i] != ' ' && text[i] != '\t' && text[i] != '\r') {
            return false;
        }
    }
    return true;
}

/* Strip a trailing comment and trailing blanks from a scalar or mapping tail. */
static void uds_trim_scalar(const char **text, size_t *length)
{
    size_t i;

    for (i = 0u; i < *length; ++i) {
        if ((*text)[i] == '#' && i > 0u && ((*text)[i - 1u] == ' ' || (*text)[i - 1u] == '\t')) {
            *length = i;
            break;
        }
    }
    while (*length > 0u &&
           ((*text)[*length - 1u] == ' ' || (*text)[*length - 1u] == '\t' ||
            (*text)[*length - 1u] == '\r')) {
        (*length)--;
    }
}

/* Split the document into content lines, rejecting tabs in indentation. */
static bool uds_split_lines(uds_parse_t *parse)
{
    size_t position = 0u;
    size_t number = 1u;
    size_t count = 0u;
    uds_line_t *lines;

    while (position <= parse->length) {
        size_t end = position;
        size_t indent;

        while (end < parse->length && parse->text[end] != '\n') {
            end++;
        }
        indent = 0u;
        while (position + indent < end && parse->text[position + indent] == ' ') {
            indent++;
        }
        if (position + indent < end && parse->text[position + indent] == '\t') {
            return uds_fail_at(parse, number, indent + 1u,
                               "tabs are not allowed in indentation (use spaces)");
        }
        if (!uds_line_is_blank(parse->text + position, end - position) &&
            parse->text[position + indent] != '#') {
            if (indent % 2u != 0u) {
                return uds_fail_at(parse, number, indent + 1u,
                                   "indentation must be a multiple of two spaces");
            }
            count++;
        }
        position = (end < parse->length) ? end + 1u : parse->length + 1u;
        if (end >= parse->length) {
            break;
        }
        number++;
    }

    lines = (uds_line_t *)uds_arena_calloc(parse->arena, (count == 0u) ? 1u : count,
                                           sizeof(*lines));
    if (lines == NULL) {
        (void)uds_fail(parse, CANCESTRY_UDS_ERR_NO_MEMORY, 0u, 0u, "out of memory");
        return false;
    }
    {
        size_t index = 0u;

        position = 0u;
        number = 1u;
        while (position <= parse->length) {
            size_t end = position;
            size_t indent = 0u;

            while (end < parse->length && parse->text[end] != '\n') {
                end++;
            }
            while (position + indent < end && parse->text[position + indent] == ' ') {
                indent++;
            }
            if (!uds_line_is_blank(parse->text + position, end - position) &&
                parse->text[position + indent] != '#') {
                lines[index].start = parse->text + position + indent;
                lines[index].length = end - position - indent;
                lines[index].indent = indent;
                lines[index].number = number;
                index++;
            }
            if (end >= parse->length) {
                break;
            }
            position = end + 1u;
            number++;
        }
    }
    parse->lines = lines;
    parse->line_count = count;
    return true;
}

static bool uds_parse_at_end(const uds_parse_t *parse)
{
    return parse->index >= parse->line_count;
}

static size_t uds_parse_column(const uds_parse_t *parse)
{
    if (parse->offset != 0u) {
        return parse->offset_column;
    }
    return parse->lines[parse->index].indent;
}

static const char *uds_parse_line_text(const uds_parse_t *parse, size_t *length)
{
    const uds_line_t *line = &parse->lines[parse->index];

    *length = line->length - parse->offset;
    return line->start + parse->offset;
}

static size_t uds_parse_line_number(const uds_parse_t *parse)
{
    if (parse->index >= parse->line_count) {
        return parse->line_count;
    }
    return parse->lines[parse->index].number;
}

static void uds_parse_advance_line(uds_parse_t *parse)
{
    parse->index++;
    parse->offset = 0u;
    parse->offset_column = 0u;
}

/*
 * Read one scalar or the value tail of a mapping entry. Handles quoted
 * scalars and rejects flow collections, anchors, aliases, tags and block
 * scalars explicitly instead of misreading them.
 */
static uds_node_t *uds_parse_scalar(uds_parse_t *parse, const char *text, size_t length,
                                    size_t line, size_t column)
{
    uds_node_t *node;
    const char *body = text;
    size_t body_length = length;

    (void)uds_trim_scalar(&body, &body_length);
    if (body_length == 0u) {
        return NULL; /* an empty value is YAML null */
    }
    if (body[0] == '{' || body[0] == '[') {
        (void)uds_fail_at(parse, line, column,
                          "flow collections are outside the supported YAML subset");
        return NULL;
    }
    if (body[0] == '|' || body[0] == '>' || body[0] == '&' || body[0] == '*' || body[0] == '!') {
        (void)uds_fail_at(parse, line, column,
                          "block scalars, anchors, aliases and tags are outside the supported "
                          "YAML subset");
        return NULL;
    }
    node = (uds_node_t *)uds_arena_calloc(parse->arena, 1u, sizeof(*node));
    if (node == NULL) {
        (void)uds_fail(parse, CANCESTRY_UDS_ERR_NO_MEMORY, line, column, "out of memory");
        return NULL;
    }
    node->kind = UDS_NODE_SCALAR;
    node->line = line;
    node->column = column;
    if ((body[0] == '"' || body[0] == '\'') && body_length >= 2u &&
        body[body_length - 1u] == body[0]) {
        char quote = body[0];
        size_t i;
        size_t out = 0u;
        char *buffer = uds_arena_alloc(parse->arena, body_length); /* upper bound */

        if (buffer == NULL) {
            (void)uds_fail(parse, CANCESTRY_UDS_ERR_NO_MEMORY, line, column, "out of memory");
            return NULL;
        }
        for (i = 1u; i + 1u < body_length; ++i) {
            char c = body[i];

            if (quote == '"' && c == '\\') {
                char escaped = body[i + 1u];

                switch (escaped) {
                case 'n':
                    buffer[out++] = '\n';
                    break;
                case 't':
                    buffer[out++] = '\t';
                    break;
                case 'r':
                    buffer[out++] = '\r';
                    break;
                case '"':
                    buffer[out++] = '"';
                    break;
                case '\\':
                    buffer[out++] = '\\';
                    break;
                default:
                    (void)uds_fail_at(parse, line, column, "unsupported escape sequence in a "
                                                            "double-quoted scalar");
                    return NULL;
                }
                i++;
                continue;
            }
            if (quote == '\'' && c == '\'' && i + 1u < body_length - 1u &&
                body[i + 1u] == '\'') {
                buffer[out++] = '\'';
                i++;
                continue;
            }
            buffer[out++] = c;
        }
        node->quoted = true;
        node->text = uds_arena_strdup(parse->arena, buffer, out);
        node->text_length = out;
        if (node->text == NULL) {
            (void)uds_fail(parse, CANCESTRY_UDS_ERR_NO_MEMORY, line, column, "out of memory");
            return NULL;
        }
        return node;
    }
    node->text = uds_arena_strdup(parse->arena, body, body_length);
    node->text_length = body_length;
    if (node->text == NULL) {
        (void)uds_fail(parse, CANCESTRY_UDS_ERR_NO_MEMORY, line, column, "out of memory");
        return NULL;
    }
    return node;
}

static uds_node_t *uds_parse_node(uds_parse_t *parse, size_t min_indent);

static uds_node_t *uds_parse_document_node(uds_parse_t *parse, size_t indent)
{
    return uds_parse_node(parse, indent);
}

/* @return true when the inline remainder of a "- " starts a mapping entry. */
static bool uds_inline_starts_mapping(const char *text, size_t length)
{
    size_t i;
    char quote = '\0';

    for (i = 0u; i < length; ++i) {
        char c = text[i];

        if (quote != '\0') {
            if (c == quote) {
                quote = '\0';
            }
            continue;
        }
        if (c == '"' || c == '\'') {
            quote = c;
            continue;
        }
        if (c == '#' && i > 0u && text[i - 1u] == ' ') {
            return false;
        }
        if (c == ':' && (i + 1u >= length || text[i + 1u] == ' ' || text[i + 1u] == '\t')) {
            return true;
        }
    }
    return false;
}

/* Parse a block sequence whose items start at @p indent. */
static uds_node_t *uds_parse_sequence(uds_parse_t *parse, size_t indent)
{
    uds_vec_t items;
    uds_node_t *node;

    uds_vec_init(&items, sizeof(uds_node_t *));
    while (!uds_parse_at_end(parse) && uds_parse_column(parse) == indent) {
        size_t length;
        const char *text = uds_parse_line_text(parse, &length);
        size_t consumed = 1u; /* the '-' */
        size_t line = uds_parse_line_number(parse);
        size_t column = indent;

        if (length < 1u || text[0] != '-') {
            break;
        }
        while (consumed < length && text[consumed] == ' ') {
            consumed++;
        }
        if (consumed >= length) {
            /* "-" alone: the item is the block below it. */
            uds_node_t *child;

            uds_parse_advance_line(parse);
            child = uds_parse_document_node(parse, indent + 1u);
            if (parse->failed) {
                return NULL;
            }
            if (child == NULL) {
                (void)uds_fail_at(parse, line, column, "sequence item has no value");
                return NULL;
            }
            if (!uds_vec_push(parse->arena, &items, &child)) {
                (void)uds_fail(parse, CANCESTRY_UDS_ERR_NO_MEMORY, line, column,
                               "out of memory");
                return NULL;
            }
            continue;
        }
        {
            size_t child_indent = indent + consumed;
            uds_node_t *child;

            parse->offset += consumed;
            parse->offset_column = child_indent;
            if (uds_inline_starts_mapping(text + consumed, length - consumed)) {
                child = uds_parse_document_node(parse, child_indent);
            } else {
                child = uds_parse_scalar(parse, text + consumed, length - consumed, line,
                                         child_indent + 1u);
            }
            if (parse->failed) {
                return NULL;
            }
            if (child == NULL) {
                (void)uds_fail_at(parse, line, child_indent + 1u, "sequence item has no value");
                return NULL;
            }
            if (parse->offset != 0u) {
                uds_parse_advance_line(parse);
            }
            if (!uds_vec_push(parse->arena, &items, &child)) {
                (void)uds_fail(parse, CANCESTRY_UDS_ERR_NO_MEMORY, line, column,
                               "out of memory");
                return NULL;
            }
        }
    }
    if (items.count == 0u) {
        (void)uds_fail_at(parse, uds_parse_line_number(parse), indent + 1u,
                          "expected a sequence with at least one item");
        return NULL;
    }
    node = (uds_node_t *)uds_arena_calloc(parse->arena, 1u, sizeof(*node));
    if (node == NULL) {
        (void)uds_fail(parse, CANCESTRY_UDS_ERR_NO_MEMORY, 0u, 0u, "out of memory");
        return NULL;
    }
    node->kind = UDS_NODE_SEQ;
    node->line = (items.count > 0u) ? ((const uds_node_t *const *)items.items)[0]->line : 0u;
    node->column = indent;
    node->items = (uds_node_t **)uds_vec_seal(&items);
    node->item_count = items.count;
    return node;
}

/* Parse a block mapping whose keys start at @p indent. */
static uds_node_t *uds_parse_mapping(uds_parse_t *parse, size_t indent)
{
    uds_vec_t pairs;
    uds_node_t *node;

    uds_vec_init(&pairs, sizeof(uds_pair_t));
    while (!uds_parse_at_end(parse) && uds_parse_column(parse) == indent) {
        size_t length;
        const char *text = uds_parse_line_text(parse, &length);
        size_t line = uds_parse_line_number(parse);
        size_t key_end = length;
        size_t value_start = 0u;
        const char *key;
        size_t key_length;
        size_t i;
        uds_node_t *value = NULL;
        uds_pair_t pair;

        if (length >= 2u && text[0] == '-' && (text[1] == ' ' || text[1] == '\t')) {
            break; /* a sequence item, not a mapping key */
        }
        /* Find the key separator: ": " or a trailing ':' at end of line. */
        for (i = 0u; i < length; ++i) {
            if (text[i] == ':') {
                if (i + 1u >= length || text[i + 1u] == ' ' || text[i + 1u] == '\t') {
                    key_end = i;
                    value_start = i + 1u;
                    break;
                }
            }
            if (text[i] == '#' && i > 0u && text[i - 1u] == ' ') {
                break;
            }
        }
        if (key_end == length) {
            (void)uds_fail_at(parse, line, indent + 1u,
                             "expected \"key: value\" or \"key:\" at this indentation");
            return NULL;
        }
        key = text;
        key_length = key_end;
        while (key_length > 0u && (key[key_length - 1u] == ' ' || key[key_length - 1u] == '\t')) {
            key_length--;
        }
        if (key_length == 0u) {
            (void)uds_fail_at(parse, line, indent + 1u, "empty mapping key");
            return NULL;
        }
        if (key[0] == '"' || key[0] == '\'') {
            (void)uds_fail_at(parse, line, indent + 1u, "quoted keys are not used by CANcestry "
                                                        "schema field names");
            return NULL;
        }
        for (i = 0u; i < key_length; ++i) {
            char c = key[i];
            bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                      (c >= '0' && c <= '9') || c == '_' || c == '-';

            if (!ok) {
                (void)uds_fail_at(parse, line, indent + i + 1u,
                                  "invalid character in mapping key");
                return NULL;
            }
        }
        for (i = 0u; i < pairs.count; ++i) {
            const uds_pair_t *existing = &((const uds_pair_t *)pairs.items)[i];

            if (existing->key_length == key_length &&
                memcmp(existing->key, key, key_length) == 0) {
                (void)uds_fail_at(parse, line, indent + 1u, "duplicate mapping key");
                return NULL;
            }
        }
        while (value_start < length && (text[value_start] == ' ' || text[value_start] == '\t')) {
            value_start++;
        }
        if (value_start < length) {
            size_t tail_length = length - value_start;
            const char *tail = text + value_start;

            parse->offset += value_start;
            parse->offset_column = indent + value_start;
            value = uds_parse_scalar(parse, tail, tail_length, line, indent + value_start + 1u);
            if (parse->failed) {
                return NULL;
            }
            if (parse->offset != 0u) {
                uds_parse_advance_line(parse);
            }
        } else {
            size_t child_indent;

            uds_parse_advance_line(parse);
            if (uds_parse_at_end(parse) || uds_parse_column(parse) <= indent) {
                value = NULL; /* key with an empty body: YAML null */
            } else {
                child_indent = uds_parse_column(parse);
                value = uds_parse_document_node(parse, child_indent);
                if (parse->failed) {
                    return NULL;
                }
            }
        }
        pair.key = uds_arena_strdup(parse->arena, key, key_length);
        pair.key_length = key_length;
        pair.line = line;
        pair.column = indent + 1u;
        pair.value = value;
        if (pair.key == NULL) {
            (void)uds_fail(parse, CANCESTRY_UDS_ERR_NO_MEMORY, line, indent + 1u,
                           "out of memory");
            return NULL;
        }
        if (!uds_vec_push(parse->arena, &pairs, &pair)) {
            (void)uds_fail(parse, CANCESTRY_UDS_ERR_NO_MEMORY, line, indent + 1u,
                           "out of memory");
            return NULL;
        }
    }
    if (pairs.count == 0u) {
        (void)uds_fail_at(parse, uds_parse_line_number(parse), indent + 1u,
                          "expected a mapping with at least one key");
        return NULL;
    }
    node = (uds_node_t *)uds_arena_calloc(parse->arena, 1u, sizeof(*node));
    if (node == NULL) {
        (void)uds_fail(parse, CANCESTRY_UDS_ERR_NO_MEMORY, 0u, 0u, "out of memory");
        return NULL;
    }
    node->kind = UDS_NODE_MAP;
    node->line = ((const uds_pair_t *)pairs.items)[0].line;
    node->column = indent;
    node->pairs = (uds_pair_t *)uds_vec_seal(&pairs);
    node->pair_count = pairs.count;
    return node;
}

static uds_node_t *uds_parse_node(uds_parse_t *parse, size_t min_indent)
{
    size_t indent;
    size_t length;
    const char *text;

    if (uds_parse_at_end(parse)) {
        return NULL;
    }
    indent = uds_parse_column(parse);
    if (indent < min_indent) {
        return NULL;
    }
    text = uds_parse_line_text(parse, &length);
    if (length >= 1u && text[0] == '-' &&
        (length == 1u || text[1] == ' ' || text[1] == '\t')) {
        return uds_parse_sequence(parse, indent);
    }
    return uds_parse_mapping(parse, indent);
}

/* ------------------------------------------------------------------------- */
/* Node access helpers                                                       */
/* ------------------------------------------------------------------------- */

static const uds_pair_t *uds_map_find(const uds_node_t *node, const char *key)
{
    size_t i;

    if (node == NULL || node->kind != UDS_NODE_MAP || key == NULL) {
        return NULL;
    }
    for (i = 0u; i < node->pair_count; ++i) {
        if (node->pairs[i].key_length == strlen(key) &&
            memcmp(node->pairs[i].key, key, node->pairs[i].key_length) == 0) {
            return &node->pairs[i];
        }
    }
    return NULL;
}

static const uds_node_t *uds_map_value(const uds_node_t *node, const char *key)
{
    const uds_pair_t *pair = uds_map_find(node, key);

    return (pair != NULL) ? pair->value : NULL;
}

/** Plain scalar integer, accepting the decimal, 0x and 0o forms. */
static bool uds_scalar_uint(uds_parse_t *parse, const uds_node_t *node, const char *what,
                            uint64_t *out)
{
    const char *text;
    size_t length;
    unsigned base = 10u;
    size_t i;
    uint64_t value = 0u;

    if (node == NULL || node->kind != UDS_NODE_SCALAR || node->quoted) {
        (void)uds_fail_at(parse, (node != NULL) ? node->line : 0u,
                          (node != NULL) ? node->column : 1u, "%s must be an integer", what);
        return false;
    }
    text = node->text;
    length = node->text_length;
    if (length >= 2u && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
        base = 16u;
        i = 2u;
    } else if (length >= 2u && text[0] == '0' && (text[1] == 'o' || text[1] == 'O')) {
        base = 8u;
        i = 2u;
    } else {
        i = 0u;
        if (length > 1u && text[0] == '0') {
            (void)uds_fail_at(parse, node->line, node->column,
                              "%s: a leading zero means an octal literal; use 0o", what);
            return false;
        }
    }
    if (i >= length) {
        (void)uds_fail_at(parse, node->line, node->column, "%s must be an integer", what);
        return false;
    }
    for (; i < length; ++i) {
        unsigned digit;

        if (text[i] >= '0' && text[i] <= '9') {
            digit = (unsigned)(text[i] - '0');
        } else if (base == 16u && text[i] >= 'a' && text[i] <= 'f') {
            digit = (unsigned)(text[i] - 'a') + 10u;
        } else if (base == 16u && text[i] >= 'A' && text[i] <= 'F') {
            digit = (unsigned)(text[i] - 'A') + 10u;
        } else {
            (void)uds_fail_at(parse, node->line, node->column, "%s must be an integer", what);
            return false;
        }
        if (digit >= base) {
            (void)uds_fail_at(parse, node->line, node->column, "%s must be an integer", what);
            return false;
        }
        if (value > (UINT64_MAX - digit) / base) {
            (void)uds_fail_at(parse, node->line, node->column, "%s is out of range", what);
            return false;
        }
        value = (value * base) + digit;
    }
    *out = value;
    return true;
}

static bool uds_scalar_bool(uds_parse_t *parse, const uds_node_t *node, const char *what,
                            bool *out)
{
    if (node == NULL || node->kind != UDS_NODE_SCALAR || node->quoted) {
        (void)uds_fail_at(parse, (node != NULL) ? node->line : 0u,
                          (node != NULL) ? node->column : 1u, "%s must be true or false", what);
        return false;
    }
    if (node->text_length == 4u && memcmp(node->text, "true", 4u) == 0) {
        *out = true;
        return true;
    }
    if (node->text_length == 5u && memcmp(node->text, "false", 5u) == 0) {
        *out = false;
        return true;
    }
    (void)uds_fail_at(parse, node->line, node->column, "%s must be true or false", what);
    return false;
}

/* ------------------------------------------------------------------------- */
/* Builder                                                                   */
/* ------------------------------------------------------------------------- */

/** Signal-name grammar: ^[a-zA-Z_][a-zA-Z0-9_]*(\.[a-zA-Z_][a-zA-Z0-9_]*)?$ */
static bool uds_signal_name_valid(const char *text, size_t length)
{
    size_t i;
    bool at_segment_start = true;

    if (length == 0u || length > CANCESTRY_UDS_NAME_MAX) {
        return false;
    }
    for (i = 0u; i < length; ++i) {
        char c = text[i];
        bool alpha = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';

        if (at_segment_start) {
            if (!alpha) {
                return false;
            }
            at_segment_start = false;
        } else if (c == '.') {
            if (i + 1u == length) {
                return false; /* trailing dot */
            }
            at_segment_start = true;
        } else if (!alpha && !(c >= '0' && c <= '9')) {
            return false;
        }
    }
    return !at_segment_start;
}

/**
 * Parse a byte array (block sequence of integers 0..255) into arena storage.
 *
 * @param max_items  Schema bound on the item count.
 * @param what       Field name for diagnostics.
 */
static bool uds_build_byte_array(uds_parse_t *parse, const uds_node_t *node, size_t max_items,
                                 const char *what, const uint8_t **out, uint16_t *out_length)
{
    uds_vec_t bytes;
    size_t i;

    if (node == NULL || node->kind != UDS_NODE_SEQ) {
        (void)uds_fail_at(parse, (node != NULL) ? node->line : 0u,
                          (node != NULL) ? node->column : 1u, "%s must be a byte sequence",
                          what);
        return false;
    }
    if (node->item_count > max_items) {
        (void)uds_fail_at(parse, node->line, node->column, "%s accepts at most %u items", what,
                          (unsigned)max_items);
        return false;
    }
    uds_vec_init(&bytes, sizeof(uint8_t));
    for (i = 0u; i < node->item_count; ++i) {
        uint64_t value;
        uint8_t byte;

        if (!uds_scalar_uint(parse, node->items[i], what, &value)) {
            return false;
        }
        if (value > 255u) {
            (void)uds_fail_at(parse, node->items[i]->line, node->items[i]->column,
                              "%s bytes must be in 0..255", what);
            return false;
        }
        byte = (uint8_t)value;
        if (!uds_vec_push(parse->arena, &bytes, &byte)) {
            (void)uds_fail(parse, CANCESTRY_UDS_ERR_NO_MEMORY, node->line, node->column,
                           "out of memory");
            return false;
        }
    }
    *out = (const uint8_t *)uds_vec_seal(&bytes);
    *out_length = (uint16_t)bytes.count;
    return true;
}

static const char *const UDS_DID_KEYS[] = {"did", "length", "read_only", "signal", "default"};
static const char *const UDS_ROUTINE_KEYS[] = {"routine", "start", "stop", "results"};
static const char *const UDS_ROUTINE_OP_KEYS[] = {"allowed", "response"};
static const char *const UDS_ROOT_KEYS[] = {"schema_version", "uds"};
static const char *const UDS_UDS_KEYS[] = {"dids", "routines"};

/** Verify every map key is declared by the schema; report the offender. */
static bool uds_check_keys(uds_parse_t *parse, const uds_node_t *node,
                           const char *const *allowed, size_t allowed_count, const char *what)
{
    size_t i;
    size_t j;

    if (node == NULL || node->kind != UDS_NODE_MAP) {
        return true;
    }
    for (i = 0u; i < node->pair_count; ++i) {
        bool known = false;

        for (j = 0u; j < allowed_count; ++j) {
            if (node->pairs[i].key_length == strlen(allowed[j]) &&
                memcmp(node->pairs[i].key, allowed[j], node->pairs[i].key_length) == 0) {
                known = true;
                break;
            }
        }
        if (!known) {
            (void)uds_fail_at(parse, node->pairs[i].line, node->pairs[i].column,
                              "unknown field \"%.*s\" in %s (additionalProperties: false)",
                              (int)node->pairs[i].key_length, node->pairs[i].key, what);
            return false;
        }
    }
    return true;
}

static bool uds_build_routine_op(uds_parse_t *parse, const uds_node_t *node,
                                 cancestry_uds_routine_op_t *op)
{
    bool allowed = true;

    op->declared = true;
    if (!uds_check_keys(parse, node, UDS_ROUTINE_OP_KEYS, 2u, "routine operation")) {
        return false;
    }
    {
        const uds_node_t *allowed_node = uds_map_value(node, "allowed");

        if (allowed_node != NULL) {
            if (!uds_scalar_bool(parse, allowed_node, "allowed", &allowed)) {
                return false;
            }
        }
    }
    op->allowed = allowed;
    {
        const uds_node_t *response_node = uds_map_value(node, "response");

        if (response_node != NULL) {
            if (!uds_build_byte_array(parse, response_node, CANCESTRY_UDS_DID_LENGTH_MAX,
                                      "response", &op->response, &op->response_length)) {
                return false;
            }
        } else {
            op->response = NULL;
            op->response_length = 0u;
        }
    }
    return true;
}

static bool uds_build_did(uds_parse_t *parse, const uds_node_t *node, cancestry_uds_did_t *did)
{
    uint64_t value;
    const uds_node_t *field;

    if (!uds_check_keys(parse, node, UDS_DID_KEYS, 5u, "dids entry")) {
        return false;
    }
    /* did: required, 0..65535. */
    field = uds_map_value(node, "did");
    if (field == NULL) {
        (void)uds_fail_at(parse, node->line, node->column, "dids entry requires \"did\"");
        return false;
    }
    if (!uds_scalar_uint(parse, field, "did", &value) || value > 65535u) {
        if (!parse->failed) {
            (void)uds_fail_at(parse, field->line, field->column, "did must be in 0..65535");
        }
        return false;
    }
    did->did = (uint16_t)value;

    /* length: required, 1..4095. */
    field = uds_map_value(node, "length");
    if (field == NULL) {
        (void)uds_fail_at(parse, node->line, node->column, "dids entry requires \"length\"");
        return false;
    }
    if (!uds_scalar_uint(parse, field, "length", &value) || value < 1u ||
        value > CANCESTRY_UDS_DID_LENGTH_MAX) {
        if (!parse->failed) {
            (void)uds_fail_at(parse, field->line, field->column,
                              "length must be in 1..4095");
        }
        return false;
    }
    did->length = (uint16_t)value;

    /* read_only: optional boolean, default false. */
    field = uds_map_value(node, "read_only");
    if (field != NULL) {
        if (!uds_scalar_bool(parse, field, "read_only", &did->read_only)) {
            return false;
        }
    } else {
        did->read_only = false;
    }

    /* signal: optional name (short or canonical). */
    field = uds_map_value(node, "signal");
    if (field != NULL) {
        if (field->kind != UDS_NODE_SCALAR || field->text_length == 0u) {
            (void)uds_fail_at(parse, field->line, field->column, "signal must be a name string");
            return false;
        }
        if (!uds_signal_name_valid(field->text, field->text_length)) {
            (void)uds_fail_at(parse, field->line, field->column,
                              "signal must match a codec signal name "
                              "(short or <codec_map>.<signal>)");
            return false;
        }
        did->signal = uds_arena_strdup(parse->arena, field->text, field->text_length);
        if (did->signal == NULL) {
            (void)uds_fail(parse, CANCESTRY_UDS_ERR_NO_MEMORY, field->line, field->column,
                           "out of memory");
            return false;
        }
        /* The signal mirror decodes a little-endian unsigned integer into a
         * 64-bit signal value, so a mapped DID is bounded to 8 bytes. */
        if (did->length > CANCESTRY_UDS_DID_SIGNAL_LENGTH_MAX) {
            (void)uds_fail_at(parse, field->line, field->column,
                              "a signal-mapped DID must have length 1..8");
            return false;
        }
    } else {
        did->signal = NULL;
    }

    /* default: optional byte array, exactly `length` bytes. */
    field = uds_map_value(node, "default");
    if (field != NULL) {
        uint16_t default_length = 0u;

        if (!uds_build_byte_array(parse, field, CANCESTRY_UDS_DID_LENGTH_MAX, "default",
                                  &did->default_data, &default_length)) {
            return false;
        }
        if ((uint32_t)default_length != (uint32_t)did->length) {
            (void)uds_fail_at(parse, field->line, field->column,
                              "default must carry exactly `length` (%u) bytes",
                              (unsigned)did->length);
            return false;
        }
    } else {
        did->default_data = NULL;
    }
    return true;
}

static bool uds_build_routine(uds_parse_t *parse, const uds_node_t *node,
                              cancestry_uds_routine_t *routine)
{
    uint64_t value;
    const uds_node_t *field;

    if (!uds_check_keys(parse, node, UDS_ROUTINE_KEYS, 4u, "routines entry")) {
        return false;
    }
    field = uds_map_value(node, "routine");
    if (field == NULL) {
        (void)uds_fail_at(parse, node->line, node->column, "routines entry requires \"routine\"");
        return false;
    }
    if (!uds_scalar_uint(parse, field, "routine", &value) || value > 65535u) {
        if (!parse->failed) {
            (void)uds_fail_at(parse, field->line, field->column,
                              "routine must be in 0..65535");
        }
        return false;
    }
    routine->routine = (uint16_t)value;

    memset(&routine->start, 0, sizeof(routine->start));
    memset(&routine->stop, 0, sizeof(routine->stop));
    memset(&routine->results, 0, sizeof(routine->results));

    field = uds_map_value(node, "start");
    if (field != NULL) {
        if (field->kind != UDS_NODE_MAP ||
            !uds_build_routine_op(parse, field, &routine->start)) {
            if (!parse->failed) {
                (void)uds_fail_at(parse, field->line, field->column, "start must be a mapping");
            }
            return false;
        }
    }
    field = uds_map_value(node, "stop");
    if (field != NULL) {
        if (field->kind != UDS_NODE_MAP ||
            !uds_build_routine_op(parse, field, &routine->stop)) {
            if (!parse->failed) {
                (void)uds_fail_at(parse, field->line, field->column, "stop must be a mapping");
            }
            return false;
        }
    }
    field = uds_map_value(node, "results");
    if (field != NULL) {
        if (field->kind != UDS_NODE_MAP ||
            !uds_build_routine_op(parse, field, &routine->results)) {
            if (!parse->failed) {
                (void)uds_fail_at(parse, field->line, field->column,
                                  "results must be a mapping");
            }
            return false;
        }
    }
    return true;
}

static bool uds_build_config(uds_parse_t *parse, const uds_node_t *root,
                             cancestry_uds_config_t *config)
{
    const uds_node_t *schema_version;
    const uds_node_t *uds_node;
    const uds_node_t *dids_node;
    const uds_node_t *routines_node;
    uds_vec_t dids;
    uds_vec_t routines;
    size_t i;
    size_t data_offset = 0u;

    memset(config, 0, sizeof(*config));

    if (root == NULL || root->kind != UDS_NODE_MAP) {
        (void)uds_fail_at(parse, 1u, 1u, "the document must be a mapping");
        return false;
    }
    if (!uds_check_keys(parse, root, UDS_ROOT_KEYS, 2u, "the document")) {
        return false;
    }
    schema_version = uds_map_value(root, "schema_version");
    if (schema_version == NULL || schema_version->kind != UDS_NODE_SCALAR ||
        schema_version->text_length != 5u ||
        memcmp(schema_version->text, "0.1.0", 5u) != 0) {
        (void)uds_fail_at(parse, (schema_version != NULL) ? schema_version->line : 1u,
                          (schema_version != NULL) ? schema_version->column : 1u,
                          "schema_version must be \"0.1.0\" (schemas/uds-0.1.0.schema.json)");
        return false;
    }
    uds_node = uds_map_value(root, "uds");
    if (uds_node == NULL || uds_node->kind != UDS_NODE_MAP) {
        const uds_pair_t *pair = uds_map_find(root, "uds");

        (void)uds_fail_at(parse, (pair != NULL) ? pair->line : 1u,
                          (pair != NULL) ? pair->column : 1u, "uds must be a mapping");
        return false;
    }
    if (!uds_check_keys(parse, uds_node, UDS_UDS_KEYS, 2u, "uds")) {
        return false;
    }
    dids_node = uds_map_value(uds_node, "dids");
    routines_node = uds_map_value(uds_node, "routines");
    if (dids_node == NULL && routines_node == NULL) {
        (void)uds_fail_at(parse, uds_node->line, uds_node->column,
                          "uds requires \"dids\" or \"routines\"");
        return false;
    }

    uds_vec_init(&dids, sizeof(cancestry_uds_did_t));
    if (dids_node != NULL) {
        if (dids_node->kind != UDS_NODE_SEQ) {
            (void)uds_fail_at(parse, dids_node->line, dids_node->column,
                              "dids must be a sequence");
            return false;
        }
        if (dids_node->item_count > (size_t)CANCESTRY_UDS_MAX_DIDS) {
            (void)uds_fail_at(parse, dids_node->line, dids_node->column,
                              "dids accepts at most %u entries", (unsigned)CANCESTRY_UDS_MAX_DIDS);
            return false;
        }
        for (i = 0u; i < dids_node->item_count; ++i) {
            cancestry_uds_did_t did;
            size_t j;

            memset(&did, 0, sizeof(did));
            if (dids_node->items[i] == NULL || dids_node->items[i]->kind != UDS_NODE_MAP) {
                (void)uds_fail_at(parse, dids_node->line, dids_node->column,
                                  "each dids entry must be a mapping");
                return false;
            }
            if (!uds_build_did(parse, dids_node->items[i], &did)) {
                return false;
            }
            for (j = 0u; j < dids.count; ++j) {
                const cancestry_uds_did_t *existing =
                    &((const cancestry_uds_did_t *)dids.items)[j];

                if (existing->did == did.did) {
                    (void)uds_fail(parse, CANCESTRY_UDS_ERR_CONFLICT, dids_node->items[i]->line,
                                   dids_node->items[i]->column,
                                   "duplicate did 0x%04X in dids", (unsigned)did.did);
                    return false;
                }
            }
            did.data_offset = data_offset;
            data_offset += (size_t)did.length;
            if (!uds_vec_push(parse->arena, &dids, &did)) {
                (void)uds_fail(parse, CANCESTRY_UDS_ERR_NO_MEMORY, dids_node->line,
                               dids_node->column, "out of memory");
                return false;
            }
        }
    }

    uds_vec_init(&routines, sizeof(cancestry_uds_routine_t));
    if (routines_node != NULL) {
        if (routines_node->kind != UDS_NODE_SEQ) {
            (void)uds_fail_at(parse, routines_node->line, routines_node->column,
                              "routines must be a sequence");
            return false;
        }
        if (routines_node->item_count > (size_t)CANCESTRY_UDS_MAX_ROUTINES) {
            (void)uds_fail_at(parse, routines_node->line, routines_node->column,
                              "routines accepts at most %u entries",
                              (unsigned)CANCESTRY_UDS_MAX_ROUTINES);
            return false;
        }
        for (i = 0u; i < routines_node->item_count; ++i) {
            cancestry_uds_routine_t routine;
            size_t j;

            memset(&routine, 0, sizeof(routine));
            if (routines_node->items[i] == NULL ||
                routines_node->items[i]->kind != UDS_NODE_MAP) {
                (void)uds_fail_at(parse, routines_node->line, routines_node->column,
                                  "each routines entry must be a mapping");
                return false;
            }
            if (!uds_build_routine(parse, routines_node->items[i], &routine)) {
                return false;
            }
            for (j = 0u; j < routines.count; ++j) {
                const cancestry_uds_routine_t *existing =
                    &((const cancestry_uds_routine_t *)routines.items)[j];

                if (existing->routine == routine.routine) {
                    (void)uds_fail(parse, CANCESTRY_UDS_ERR_CONFLICT,
                                   routines_node->items[i]->line, routines_node->items[i]->column,
                                   "duplicate routine 0x%04X in routines",
                                   (unsigned)routine.routine);
                    return false;
                }
            }
            if (!uds_vec_push(parse->arena, &routines, &routine)) {
                (void)uds_fail(parse, CANCESTRY_UDS_ERR_NO_MEMORY, routines_node->line,
                               routines_node->column, "out of memory");
                return false;
            }
        }
    }

    config->dids = (dids.count > 0u) ? (cancestry_uds_did_t *)uds_vec_seal(&dids) : NULL;
    config->did_count = (uint16_t)dids.count;
    config->routines =
        (routines.count > 0u) ? (cancestry_uds_routine_t *)uds_vec_seal(&routines) : NULL;
    config->routine_count = (uint16_t)routines.count;
    config->did_data_length = data_offset;
    return true;
}

/* ------------------------------------------------------------------------- */
/* Public API                                                                */
/* ------------------------------------------------------------------------- */

/** The config plus its arena, allocated as one block (fsm_set_block pattern). */
typedef struct uds_config_block {
    cancestry_uds_config_t config;
    uds_arena_t arena;
} uds_config_block_t;

cancestry_uds_config_t *cancestry_uds_config_load(const char *text,
                                                  size_t length,
                                                  cancestry_uds_load_error_t *error)
{
    uds_config_block_t *block;
    uds_parse_t parse;
    uds_node_t *root;

    if (error != NULL) {
        memset(error, 0, sizeof(*error));
    }
    if (text == NULL) {
        if (error != NULL) {
            error->status = CANCESTRY_UDS_ERR_NULL;
            (void)snprintf(error->message, sizeof(error->message), "text is NULL");
        }
        return NULL;
    }

    block = (uds_config_block_t *)calloc(1u, sizeof(*block));
    if (block == NULL) {
        if (error != NULL) {
            error->status = CANCESTRY_UDS_ERR_NO_MEMORY;
            (void)snprintf(error->message, sizeof(error->message), "out of memory");
        }
        return NULL;
    }

    memset(&parse, 0, sizeof(parse));
    parse.text = text;
    parse.length = length;
    parse.arena = &block->arena;
    parse.error = error;

    if (!uds_split_lines(&parse)) {
        uds_arena_release(&block->arena);
        free(block);
        return NULL;
    }
    root = uds_parse_node(&parse, 0u);
    if (parse.failed) {
        uds_arena_release(&block->arena);
        free(block);
        return NULL;
    }
    if (!uds_build_config(&parse, root, &block->config)) {
        uds_arena_release(&block->arena);
        free(block);
        return NULL;
    }
    return &block->config;
}

void cancestry_uds_config_free(cancestry_uds_config_t *config)
{
    uds_config_block_t *block;

    if (config == NULL) {
        return;
    }
    /* The config is the first member of its block; the definitions point into
     * the block's arena, so releasing the arena and the block is the complete
     * teardown. */
    block = (uds_config_block_t *)(void *)config;
    uds_arena_release(&block->arena);
    free(block);
}
