/*
 * CANcestry - FSM file loader.
 *
 * Implementation notes:
 *   - The loader speaks the same strict, documented YAML subset as the codec and
 *     recipe loaders: block mappings and block sequences only, no flow
 *     collections, anchors, aliases, tags, directives or multi-line scalars.
 *     Anything outside the subset is rejected with a line/column error instead
 *     of being guessed at.
 *   - Validation implements every constraint of schemas/fsm-0.3.0.schema.json in
 *     C (required fields, field types, ranges, enums, additionalProperties, the
 *     oneOf action shape, the conditional transition fields), so no external JSON
 *     Schema validator runs at load time (SW-FR-FSM-003, agents.md "Schema is
 *     law"). The 0.3.0 schema finalizes the 0.2.0 declaration structure
 *     unchanged (issue #13), so the loader accepts schema_version "0.2.0" and
 *     "0.3.0" alike - the codec loader's precedent for the 0.2.0/0.3.0 codec
 *     map schemas - and rejects every other value. Fields outside the schema,
 *     including `layout` and `priority`, are refused at load time by the
 *     additionalProperties checks (issue #11 Flag 1). On top of that it performs the reference checks
 *     docs/software/SwAD.md section 10 lists as definition errors: unknown
 *     initial state, unknown transition target, unknown machine, timer, or
 *     variable name, and an expression whose grammar is invalid. Those extra
 *     checks are what makes "compiled before execution" (SW-FR-FSM-002) real:
 *     after loading, the runtime resolves states and timers by index only.
 *   - Memory: the parse tree and the compiled definitions share one bump arena
 *     that the returned set owns, because every string a definition needs is
 *     already a NUL-terminated scalar in that arena. The set header itself is one
 *     separate block. cancestry_fsm_set_free() releases all of it in one call.
 *     Loading may allocate; the runtime never does (SYS-NF-002), which is why the
 *     loader is its own static library.
 *   - Floating-point scalars are converted with strtod, which follows the active
 *     C locale's decimal separator. Hosts and targets shall run with
 *     LC_NUMERIC=C; see core/fsm/README.md.
 */

#include "cancestry/fsm/loader.h"

#include "fsm_expression.h"

#include <errno.h>
#include <float.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/** Largest number token accepted in a literal, keeping the scan buffer small. */
#define FSM_SCAN_TOKEN_MAX ((size_t)48u)

/** @return true when @p value is neither NaN nor infinite. */
static bool fsm_number_finite(double value)
{
    return value >= -DBL_MAX && value <= DBL_MAX;
}

/* ------------------------------------------------------------------------- */
/* Arena                                                                     */
/* ------------------------------------------------------------------------- */

/*
 * Chunked bump allocator. A chunk is never moved or shrunk, so every pointer the
 * loader hands out stays valid until the set is freed.
 */
typedef struct fsm_arena_chunk {
    struct fsm_arena_chunk *next;
    size_t capacity;
    /* storage follows the header */
} fsm_arena_chunk_t;

typedef struct fsm_arena {
    fsm_arena_chunk_t *head;
    char *cursor;
    size_t remaining;
} fsm_arena_t;

static void *fsm_arena_alloc(fsm_arena_t *arena, size_t size)
{
    size_t aligned = (size + 7u) & ~((size_t)7u);

    if (aligned > arena->remaining) {
        size_t chunk_bytes = (aligned > 4096u) ? aligned + sizeof(fsm_arena_chunk_t)
                                               : 8192u + sizeof(fsm_arena_chunk_t);
        fsm_arena_chunk_t *chunk = (fsm_arena_chunk_t *)malloc(chunk_bytes);

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

static void *fsm_arena_calloc(fsm_arena_t *arena, size_t count, size_t size)
{
    void *result;

    if (count == 0u) {
        return NULL;
    }
    result = fsm_arena_alloc(arena, count * size);
    if (result != NULL) {
        memset(result, 0, count * size);
    }
    return result;
}

static void fsm_arena_release(fsm_arena_t *arena)
{
    fsm_arena_chunk_t *chunk = arena->head;

    while (chunk != NULL) {
        fsm_arena_chunk_t *next = chunk->next;

        free(chunk);
        chunk = next;
    }
    memset(arena, 0, sizeof(*arena));
}

static char *fsm_arena_strdup(fsm_arena_t *arena, const char *text, size_t length)
{
    char *copy = (char *)fsm_arena_alloc(arena, length + 1u);

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

typedef struct fsm_vec {
    void *items;
    size_t count;
    size_t capacity;
    size_t element_size;
} fsm_vec_t;

static void fsm_vec_init(fsm_vec_t *vec, size_t element_size)
{
    vec->items = NULL;
    vec->count = 0u;
    vec->capacity = 0u;
    vec->element_size = element_size;
}

/**
 * Append a copy of @p item.
 *
 * @note Growth moves the array, so nothing may hold a pointer into it across a
 *       push. The builder fills a complete vector before publishing its base
 *       pointer into a definition, which is what keeps that rule satisfied.
 */
static bool fsm_vec_push(fsm_arena_t *arena, fsm_vec_t *vec, const void *item)
{
    if (vec->count == vec->capacity) {
        size_t next_capacity = (vec->capacity == 0u) ? 4u : vec->capacity * 2u;
        void *next = fsm_arena_calloc(arena, next_capacity, vec->element_size);
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

static void *fsm_vec_seal(fsm_vec_t *vec)
{
    return vec->items;
}

/* ------------------------------------------------------------------------- */
/* YAML subset parser                                                        */
/* ------------------------------------------------------------------------- */

typedef enum fsm_node_kind {
    FSM_NODE_NULL = 0,
    FSM_NODE_SCALAR = 1,
    FSM_NODE_MAP = 2,
    FSM_NODE_SEQ = 3
} fsm_node_kind_t;

typedef struct fsm_node {
    fsm_node_kind_t kind;
    /* scalar */
    const char *text;
    size_t text_length;
    /** true when the scalar was quoted, which forces the string interpretation. */
    bool quoted;
    size_t line;
    size_t column;
    /* map */
    struct fsm_pair *pairs;
    size_t pair_count;
    /* seq */
    struct fsm_node **items;
    size_t item_count;
} fsm_node_t;

typedef struct fsm_pair {
    const char *key;
    size_t key_length;
    size_t line;
    size_t column;
    fsm_node_t *value;
} fsm_pair_t;

typedef struct fsm_line {
    const char *start;
    size_t length;
    size_t indent;
    size_t number;
} fsm_line_t;

typedef struct fsm_parse {
    const char *text;
    size_t length;
    fsm_line_t *lines;
    size_t line_count;
    size_t index;
    /** Bytes of the current line already consumed by an enclosing "- ". */
    size_t offset;
    size_t offset_column;
    fsm_arena_t *arena;
    cancestry_fsm_load_error_t *error;
    bool failed;
} fsm_parse_t;

static void fsm_fail(fsm_parse_t *parse,
                     cancestry_fsm_status_t status,
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

static bool fsm_fail_at(fsm_parse_t *parse, size_t line, size_t column, const char *format, ...)
{
    cancestry_fsm_status_t status = CANCESTRY_FSM_ERR_PARSE;
    va_list args;

    if (parse->failed) {
        return false;
    }
    parse->failed = true;
    if (parse->error != NULL) {
        memset(parse->error, 0, sizeof(*parse->error));
        parse->error->status = status;
        parse->error->line = line;
        parse->error->column = column;
        va_start(args, format);
        (void)vsnprintf(parse->error->message, sizeof(parse->error->message), format, args);
        va_end(args);
    }
    return false;
}

static bool fsm_line_is_blank(const char *text, size_t length)
{
    size_t i;

    for (i = 0u; i < length; ++i) {
        if (text[i] != ' ' && text[i] != '\t' && text[i] != '\r') {
            return false;
        }
    }
    return true;
}

/** Strip a trailing comment and trailing blanks from a scalar or mapping tail. */
static void fsm_trim_scalar(const char **text, size_t *length)
{
    size_t i;

    /* Quoted scalars keep everything inside the quotes; callers handle those
     * before reaching here. Find " #" outside quotes, then trim blanks. */
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

/** Split the document into content lines, rejecting tabs in indentation. */
static bool fsm_split_lines(fsm_parse_t *parse)
{
    size_t position = 0u;
    size_t number = 1u;
    size_t count = 0u;
    fsm_line_t *lines;

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
            return fsm_fail_at(parse, number, indent + 1u,
                               "tabs are not allowed in indentation (use spaces)");
        }
        if (!fsm_line_is_blank(parse->text + position, end - position) &&
            parse->text[position + indent] != '#') {
            if (indent % 2u != 0u) {
                return fsm_fail_at(parse, number, indent + 1u,
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

    lines = (fsm_line_t *)fsm_arena_calloc(parse->arena, (count == 0u) ? 1u : count,
                                           sizeof(*lines));
    if (lines == NULL) {
        (void)fsm_fail(parse, CANCESTRY_FSM_ERR_NO_MEMORY, 0u, 0u, "out of memory");
        return false;
    }
    {
        /* Second pass: fill the surviving lines. */
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
            if (!fsm_line_is_blank(parse->text + position, end - position) &&
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

static bool fsm_parse_at_end(const fsm_parse_t *parse)
{
    return parse->index >= parse->line_count;
}

static size_t fsm_parse_column(const fsm_parse_t *parse)
{
    if (parse->offset != 0u) {
        return parse->offset_column;
    }
    return parse->lines[parse->index].indent;
}

static const char *fsm_parse_line_text(const fsm_parse_t *parse, size_t *length)
{
    const fsm_line_t *line = &parse->lines[parse->index];

    *length = line->length - parse->offset;
    return line->start + parse->offset;
}

static size_t fsm_parse_line_number(const fsm_parse_t *parse)
{
    if (parse->index >= parse->line_count) {
        return parse->line_count;
    }
    return parse->lines[parse->index].number;
}

static void fsm_parse_advance_line(fsm_parse_t *parse)
{
    parse->index++;
    parse->offset = 0u;
    parse->offset_column = 0u;
}

/**
 * Read one scalar or the value tail of a mapping entry.
 *
 * Handles quoted scalars (with the YAML escapes the subset documents) and plain
 * scalars, and rejects flow collections, anchors, aliases, tags and block
 * scalars explicitly instead of misreading them.
 */
static fsm_node_t *fsm_parse_scalar(fsm_parse_t *parse, const char *text, size_t length,
                                    size_t line, size_t column)
{
    fsm_node_t *node;
    const char *body = text;
    size_t body_length = length;

    (void)fsm_trim_scalar(&body, &body_length);
    if (body_length == 0u) {
        return NULL; /* an empty value is YAML null */
    }
    if (body[0] == '{' || body[0] == '[') {
        (void)fsm_fail_at(parse, line, column,
                          "flow collections are outside the supported YAML subset");
        return NULL;
    }
    if (body[0] == '|' || body[0] == '>' || body[0] == '&' || body[0] == '*' || body[0] == '!') {
        (void)fsm_fail_at(parse, line, column,
                          "block scalars, anchors, aliases and tags are outside the supported "
                          "YAML subset");
        return NULL;
    }
    node = (fsm_node_t *)fsm_arena_calloc(parse->arena, 1u, sizeof(*node));
    if (node == NULL) {
        (void)fsm_fail(parse, CANCESTRY_FSM_ERR_NO_MEMORY, line, column, "out of memory");
        return NULL;
    }
    node->kind = FSM_NODE_SCALAR;
    node->line = line;
    node->column = column;
    if ((body[0] == '"' || body[0] == '\'') && body_length >= 2u &&
        body[body_length - 1u] == body[0]) {
        char quote = body[0];
        size_t i;
        size_t out = 0u;
        char *buffer = fsm_arena_alloc(parse->arena, body_length); /* upper bound */

        if (buffer == NULL) {
            (void)fsm_fail(parse, CANCESTRY_FSM_ERR_NO_MEMORY, line, column, "out of memory");
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
                    (void)fsm_fail_at(parse, line, column, "unsupported escape sequence in a "
                                                            "double-quoted scalar");
                    return NULL;
                }
                i++;
                continue;
            }
            if (quote == '\'' && c == '\'' && i + 1u < body_length - 1u && body[i + 1u] == '\'') {
                buffer[out++] = '\'';
                i++;
                continue;
            }
            buffer[out++] = c;
        }
        node->quoted = true;
        node->text = fsm_arena_strdup(parse->arena, buffer, out);
        node->text_length = out;
        if (node->text == NULL) {
            (void)fsm_fail(parse, CANCESTRY_FSM_ERR_NO_MEMORY, line, column, "out of memory");
            return NULL;
        }
        return node;
    }
    node->text = fsm_arena_strdup(parse->arena, body, body_length);
    node->text_length = body_length;
    if (node->text == NULL) {
        (void)fsm_fail(parse, CANCESTRY_FSM_ERR_NO_MEMORY, line, column, "out of memory");
        return NULL;
    }
    return node;
}

static fsm_node_t *fsm_parse_node(fsm_parse_t *parse, size_t min_indent);

static fsm_node_t *fsm_parse_document_node(fsm_parse_t *parse, size_t indent)
{
    /* A block at an exact indent: sequence or mapping. */
    size_t length;
    const char *text = fsm_parse_line_text(parse, &length);

    if (length >= 2u && text[0] == '-' && (text[1] == ' ' || text[1] == '\t')) {
        return fsm_parse_node(parse, indent);
    }
    if (length == 1u && text[0] == '-') {
        return fsm_parse_node(parse, indent);
    }
    return fsm_parse_node(parse, indent);
}

/** Report a conflicting definition (duplicate names the schema forbids). */
static bool fsm_fail_conflict(fsm_parse_t *parse, size_t line, size_t column, const char *format,
                              ...)
{
    cancestry_fsm_status_t status = CANCESTRY_FSM_ERR_CONFLICT;
    va_list args;

    if (parse->failed) {
        return false;
    }
    parse->failed = true;
    if (parse->error != NULL) {
        memset(parse->error, 0, sizeof(*parse->error));
        parse->error->status = status;
        parse->error->line = line;
        parse->error->column = column;
        va_start(args, format);
        (void)vsnprintf(parse->error->message, sizeof(parse->error->message), format, args);
        va_end(args);
    }
    return false;
}

/**
 * @return true when the inline remainder of a "- " starts a mapping entry
 *         ("key: value" or a bare "key:"), which is how a block sequence item
 *         with a map payload is written in the supported subset.
 */
static bool fsm_inline_starts_mapping(const char *text, size_t length)
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

/** Parse a block sequence whose items start at @p indent. */
static fsm_node_t *fsm_parse_sequence(fsm_parse_t *parse, size_t indent)
{
    fsm_vec_t items;
    fsm_node_t *node;

    fsm_vec_init(&items, sizeof(fsm_node_t *));
    while (!fsm_parse_at_end(parse) && fsm_parse_column(parse) == indent) {
        size_t length;
        const char *text = fsm_parse_line_text(parse, &length);
        size_t consumed = 1u; /* the '-' */
        size_t line = fsm_parse_line_number(parse);
        size_t column = indent;

        if (length < 1u || text[0] != '-') {
            break;
        }
        while (consumed < length && text[consumed] == ' ') {
            consumed++;
        }
        if (consumed >= length) {
            /* "-" alone: the item is the block below it. */
            fsm_node_t *child;

            fsm_parse_advance_line(parse);
            child = fsm_parse_document_node(parse, indent + 1u);
            if (parse->failed) {
                return NULL;
            }
            if (child == NULL) {
                (void)fsm_fail_at(parse, line, column, "sequence item has no value");
                return NULL;
            }
            if (!fsm_vec_push(parse->arena, &items, &child)) {
                (void)fsm_fail(parse, CANCESTRY_FSM_ERR_NO_MEMORY, line, column, "out of memory");
                return NULL;
            }
            continue;
        }
        {
            /* Inline item: either a scalar on the dash line, or the start of a
             * block whose first line is the remainder of this one. */
            size_t child_indent = indent + consumed;
            fsm_node_t *child;

            parse->offset += consumed;
            parse->offset_column = child_indent;
            if (fsm_inline_starts_mapping(text + consumed, length - consumed)) {
                child = fsm_parse_document_node(parse, child_indent);
            } else {
                child = fsm_parse_scalar(parse, text + consumed, length - consumed, line,
                                         child_indent + 1u);
            }
            if (parse->failed) {
                return NULL;
            }
            if (child == NULL) {
                (void)fsm_fail_at(parse, line, child_indent + 1u, "sequence item has no value");
                return NULL;
            }
            /* A scalar item leaves the line consumed; a block item consumes its
             * own lines. Either way the next item starts on a new line. */
            if (parse->offset != 0u) {
                fsm_parse_advance_line(parse);
            }
            if (!fsm_vec_push(parse->arena, &items, &child)) {
                (void)fsm_fail(parse, CANCESTRY_FSM_ERR_NO_MEMORY, line, column, "out of memory");
                return NULL;
            }
        }
    }
    node = (fsm_node_t *)fsm_arena_calloc(parse->arena, 1u, sizeof(*node));
    if (node == NULL) {
        (void)fsm_fail(parse, CANCESTRY_FSM_ERR_NO_MEMORY, 0u, 0u, "out of memory");
        return NULL;
    }
    node->kind = FSM_NODE_SEQ;
    node->items = (fsm_node_t **)fsm_vec_seal(&items);
    node->item_count = items.count;
    return node;
}

/** Parse a block mapping whose keys start at @p indent. */
static fsm_node_t *fsm_parse_mapping(fsm_parse_t *parse, size_t indent)
{
    fsm_vec_t pairs;
    fsm_node_t *node;

    fsm_vec_init(&pairs, sizeof(fsm_pair_t));
    while (!fsm_parse_at_end(parse) && fsm_parse_column(parse) == indent) {
        size_t length;
        const char *text = fsm_parse_line_text(parse, &length);
        size_t line = fsm_parse_line_number(parse);
        size_t key_end;
        size_t value_start;
        const char *key;
        size_t key_length;
        size_t i;
        fsm_node_t *value = NULL;
        fsm_pair_t pair;

        if (length >= 2u && text[0] == '-' && (text[1] == ' ' || text[1] == '\t')) {
            break; /* a sequence item, not a mapping key */
        }
        /* Find the key separator: ": " or a trailing ':' at end of line. */
        key_end = length;
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
            (void)fsm_fail_at(parse, line, indent + 1u,
                             "expected \"key: value\" or \"key:\" at this indentation");
            return NULL;
        }
        key = text;
        key_length = key_end;
        while (key_length > 0u && (key[key_length - 1u] == ' ' || key[key_length - 1u] == '\t')) {
            key_length--;
        }
        if (key_length == 0u) {
            (void)fsm_fail_at(parse, line, indent + 1u, "empty mapping key");
            return NULL;
        }
        if ((key[0] == '"' || key[0] == '\'')) {
            (void)fsm_fail_at(parse, line, indent + 1u, "quoted keys are not used by CANcestry "
                                                        "schema field names");
            return NULL;
        }
        {
            size_t s;

            for (s = 0u; s < key_length; ++s) {
                char c = key[s];
                bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                          (c >= '0' && c <= '9') || c == '_' || c == '-';

                if (!ok) {
                    (void)fsm_fail_at(parse, line, indent + s + 1u,
                                      "invalid character in mapping key");
                    return NULL;
                }
            }
        }
        for (i = 0u; i < pairs.count; ++i) {
            const fsm_pair_t *existing = &((const fsm_pair_t *)pairs.items)[i];

            if (existing->key_length == key_length && memcmp(existing->key, key, key_length) == 0) {
                (void)fsm_fail_at(parse, line, indent + 1u, "duplicate mapping key");
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
            value = fsm_parse_scalar(parse, tail, tail_length, line, indent + value_start + 1u);
            if (parse->failed) {
                return NULL;
            }
            if (parse->offset != 0u) {
                fsm_parse_advance_line(parse);
            }
        } else {
            size_t child_indent;

            fsm_parse_advance_line(parse);
            if (fsm_parse_at_end(parse) || fsm_parse_column(parse) <= indent) {
                value = NULL; /* key with an empty body: YAML null */
            } else {
                child_indent = fsm_parse_column(parse);
                if (child_indent <= indent) {
                    (void)fsm_fail_at(parse, fsm_parse_line_number(parse), child_indent + 1u,
                                      "nested block must be indented deeper than its key");
                    return NULL;
                }
                value = fsm_parse_document_node(parse, child_indent);
                if (parse->failed) {
                    return NULL;
                }
            }
        }
        pair.key = fsm_arena_strdup(parse->arena, key, key_length);
        pair.key_length = key_length;
        pair.line = line;
        pair.column = indent + 1u;
        pair.value = value;
        if (pair.key == NULL) {
            (void)fsm_fail(parse, CANCESTRY_FSM_ERR_NO_MEMORY, line, indent + 1u, "out of memory");
            return NULL;
        }
        if (!fsm_vec_push(parse->arena, &pairs, &pair)) {
            (void)fsm_fail(parse, CANCESTRY_FSM_ERR_NO_MEMORY, line, indent + 1u, "out of memory");
            return NULL;
        }
    }
    if (pairs.count == 0u) {
        (void)fsm_fail_at(parse, fsm_parse_line_number(parse), indent + 1u,
                          "expected a mapping with at least one key");
        return NULL;
    }
    node = (fsm_node_t *)fsm_arena_calloc(parse->arena, 1u, sizeof(*node));
    if (node == NULL) {
        (void)fsm_fail(parse, CANCESTRY_FSM_ERR_NO_MEMORY, 0u, 0u, "out of memory");
        return NULL;
    }
    node->kind = FSM_NODE_MAP;
    node->pairs = (fsm_pair_t *)fsm_vec_seal(&pairs);
    node->pair_count = pairs.count;
    return node;
}

static fsm_node_t *fsm_parse_node(fsm_parse_t *parse, size_t min_indent)
{
    size_t indent;
    size_t length;
    const char *text;

    if (fsm_parse_at_end(parse)) {
        return NULL;
    }
    indent = fsm_parse_column(parse);
    if (indent < min_indent) {
        return NULL;
    }
    text = fsm_parse_line_text(parse, &length);
    if (length >= 1u && text[0] == '-' &&
        (length == 1u || text[1] == ' ' || text[1] == '\t')) {
        return fsm_parse_sequence(parse, indent);
    }
    return fsm_parse_mapping(parse, indent);
}

/* ------------------------------------------------------------------------- */
/* Node access helpers                                                       */
/* ------------------------------------------------------------------------- */

static const fsm_pair_t *fsm_map_find(const fsm_node_t *node, const char *key)
{
    size_t i;

    if (node == NULL || node->kind != FSM_NODE_MAP || key == NULL) {
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

static const fsm_node_t *fsm_map_value(const fsm_node_t *node, const char *key)
{
    const fsm_pair_t *pair = fsm_map_find(node, key);

    return (pair != NULL) ? pair->value : NULL;
}

typedef struct fsm_ctx {
    fsm_parse_t *parse;
    fsm_arena_t *arena;
} fsm_ctx_t;

static bool fsm_key_allowed(fsm_ctx_t *ctx, const fsm_node_t *node, const char *const *allowed)
{
    size_t i;
    size_t j;

    if (node == NULL || node->kind != FSM_NODE_MAP) {
        return false;
    }
    for (i = 0u; i < node->pair_count; ++i) {
        bool found = false;

        for (j = 0u; allowed[j] != NULL; ++j) {
            if (strlen(allowed[j]) == node->pairs[i].key_length &&
                memcmp(allowed[j], node->pairs[i].key, node->pairs[i].key_length) == 0) {
                found = true;
                break;
            }
        }
        if (!found) {
            /* additionalProperties: false, reported with the offending key so a
             * package author can act on the message alone. */
            (void)fsm_fail_at(ctx->parse, node->pairs[i].line, node->pairs[i].column,
                              "\"%.*s\" is not a field of this object "
                              "(the schema forbids additional properties)",
                              (int)node->pairs[i].key_length, node->pairs[i].key);
            return false;
        }
    }
    return true;
}

static bool fsm_expect_map(fsm_ctx_t *ctx, const fsm_node_t *node, const char *what)
{
    if (node == NULL || node->kind != FSM_NODE_MAP) {
        (void)fsm_fail_at(ctx->parse, (node != NULL) ? node->line : 0u,
                          (node != NULL) ? node->column : 1u, "%s must be a mapping", what);
        return false;
    }
    return true;
}

static bool fsm_expect_seq(fsm_ctx_t *ctx, const fsm_node_t *node, const char *what,
                           size_t *count_out)
{
    if (node == NULL) {
        *count_out = 0u;
        return true;
    }
    if (node->kind != FSM_NODE_SEQ) {
        (void)fsm_fail_at(ctx->parse, node->line, node->column, "%s must be a sequence", what);
        return false;
    }
    *count_out = node->item_count;
    return true;
}

static bool fsm_scalar_text(fsm_ctx_t *ctx, const fsm_node_t *node, const char *what,
                            const char **out, size_t maximum, size_t *length_out)
{
    if (node == NULL || node->kind != FSM_NODE_SCALAR) {
        (void)fsm_fail_at(ctx->parse, (node != NULL) ? node->line : 0u,
                          (node != NULL) ? node->column : 1u, "%s must be a string", what);
        return false;
    }
    if (node->text_length > maximum) {
        (void)fsm_fail_at(ctx->parse, node->line, node->column,
                          "%s is longer than %u characters", what, (unsigned)maximum);
        return false;
    }
    *out = node->text;
    if (length_out != NULL) {
        *length_out = node->text_length;
    }
    return true;
}

static bool fsm_scalar_bool(fsm_ctx_t *ctx, const fsm_node_t *node, const char *what, bool *out)
{
    if (node == NULL || node->kind != FSM_NODE_SCALAR || node->quoted) {
        (void)fsm_fail_at(ctx->parse, (node != NULL) ? node->line : 0u,
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
    (void)fsm_fail_at(ctx->parse, node->line, node->column, "%s must be true or false", what);
    return false;
}

/** Plain scalar integer, accepting the decimal, 0x and 0o forms the other loaders accept. */
static bool fsm_scalar_uint(fsm_ctx_t *ctx, const fsm_node_t *node, const char *what,
                            uint64_t *out)
{
    const char *text;
    size_t length;
    unsigned base = 10u;
    size_t i;
    uint64_t value = 0u;

    if (node == NULL || node->kind != FSM_NODE_SCALAR || node->quoted) {
        (void)fsm_fail_at(ctx->parse, (node != NULL) ? node->line : 0u,
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
            (void)fsm_fail_at(ctx->parse, node->line, node->column,
                              "%s: a leading zero means an octal literal; use 0o", what);
            return false;
        }
    }
    if (i >= length) {
        (void)fsm_fail_at(ctx->parse, node->line, node->column, "%s must be an integer", what);
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
            (void)fsm_fail_at(ctx->parse, node->line, node->column, "%s must be an integer", what);
            return false;
        }
        if (digit >= base) {
            (void)fsm_fail_at(ctx->parse, node->line, node->column, "%s must be an integer", what);
            return false;
        }
        if (value > (UINT64_MAX - digit) / base) {
            (void)fsm_fail_at(ctx->parse, node->line, node->column, "%s is out of range", what);
            return false;
        }
        value = (value * base) + digit;
    }
    *out = value;
    return true;
}

/** Result of classifying a plain scalar as a YAML number. */
typedef struct fsm_number {
    /** false when the text is not a number at all (so it is an expression). */
    bool is_number;
    /** true for a real (a fraction or an exponent was present). */
    bool is_real;
    /** true when the literal carried a minus sign. */
    bool negative;
    uint64_t magnitude;
    double real;
    bool malformed;
} fsm_number_t;

static bool fsm_digit_value(char c, unsigned base, unsigned *digit_out)
{
    unsigned digit;

    if (c >= '0' && c <= '9') {
        digit = (unsigned)(c - '0');
    } else if (c >= 'a' && c <= 'f') {
        digit = (unsigned)(c - 'a') + 10u;
    } else if (c >= 'A' && c <= 'F') {
        digit = (unsigned)(c - 'A') + 10u;
    } else {
        return false;
    }
    if (digit >= base) {
        return false;
    }
    *digit_out = digit;
    return true;
}

/**
 * Classify a plain scalar as an integer or a real, following the same YAML subset
 * rules the codec and recipe loaders document: decimal, 0x hex, 0o octal and 0b
 * binary integers, reals with a fraction and an optional exponent, no leading-zero
 * decimals (an ambiguous octal spelling), and no underscores.
 */
static void fsm_scan_number(const char *text, size_t length, fsm_number_t *out)
{
    char token[FSM_SCAN_TOKEN_MAX];
    size_t token_length = 0u;
    size_t i = 0u;
    unsigned base = 10u;
    bool negative = false;
    bool seen_digit = false;
    bool is_real = false;
    bool overflow = false;
    uint64_t magnitude = 0u;

    memset(out, 0, sizeof(*out));
    if (length == 0u || length >= sizeof(token)) {
        return; /* not a number: too long, or empty */
    }

    if (text[0] == '-' || text[0] == '+') {
        negative = (text[0] == '-');
        token[token_length++] = text[0];
        i = 1u;
    }
    if (i + 1u < length && text[i] == '0' &&
        (text[i + 1u] == 'x' || text[i + 1u] == 'X')) {
        base = 16u;
        i += 2u;
    } else if (i + 1u < length && text[i] == '0' && (text[i + 1u] == 'o' || text[i + 1u] == 'O')) {
        base = 8u;
        i += 2u;
    } else if (i + 1u < length && text[i] == '0' && (text[i + 1u] == 'b' || text[i + 1u] == 'B')) {
        base = 2u;
        i += 2u;
    } else if (i + 1u < length && text[i] == '0' && text[i + 1u] >= '0' && text[i + 1u] <= '9') {
        /* 007: YAML 1.1 octal, ambiguous in a safety manifest. Refuse. */
        out->malformed = true;
        return;
    }
    while (i < length) {
        unsigned digit;

        if (!fsm_digit_value(text[i], base, &digit)) {
            break;
        }
        seen_digit = true;
        if (magnitude > (UINT64_MAX - digit) / base) {
            overflow = true;
        } else {
            magnitude = (magnitude * base) + digit;
        }
        token[token_length++] = text[i];
        i++;
    }
    if (!seen_digit) {
        return; /* not a number */
    }
    if (base != 10u) {
        if (i != length) {
            out->malformed = true;
            return;
        }
        out->is_number = true;
        out->negative = negative;
        out->magnitude = magnitude;
        if (overflow || magnitude > (uint64_t)INT64_MAX) {
            out->is_real = true;
            out->real = negative ? -(double)magnitude : (double)magnitude;
            return;
        }
        out->is_real = false;
        return;
    }
    /* Decimal: a fraction and an exponent make it a real. */
    if (i < length && text[i] == '.') {
        size_t j = i + 1u;

        if (j < length && text[j] >= '0' && text[j] <= '9') {
            is_real = true;
            token[token_length++] = '.';
            i = j;
            while (i < length && text[i] >= '0' && text[i] <= '9') {
                if (token_length + 1u >= sizeof(token)) {
                    return;
                }
                token[token_length++] = text[i];
                i++;
            }
        } else {
            /* "1." is not a number here: it would silently swallow a sentence. */
            return;
        }
    }
    if (i < length && (text[i] == 'e' || text[i] == 'E')) {
        size_t j = i + 1u;
        bool any = false;

        is_real = true;
        if (j < length && (text[j] == '+' || text[j] == '-')) {
            j++;
        }
        while (j < length && text[j] >= '0' && text[j] <= '9') {
            j++;
            any = true;
        }
        if (!any) {
            out->malformed = true;
            return;
        }
        while (i < j) {
            if (token_length + 1u >= sizeof(token)) {
                return;
            }
            token[token_length++] = text[i];
            i++;
        }
    }
    if (i != length) {
        return; /* trailing text: an expression, not a number */
    }
    token[token_length] = '\0';

    if (!is_real) {
        if (overflow) {
            /* A decimal beyond uint64 is still a number; keep it as a real. */
            char *end = NULL;
            double value = strtod(token, &end);

            out->is_number = true;
            out->is_real = true;
            out->negative = negative;
            if (end == NULL || *end != '\0' || !fsm_number_finite(value)) {
                out->malformed = true;
                return;
            }
            out->real = value;
            return;
        }
        out->is_number = true;
        out->is_real = false;
        out->negative = negative;
        out->magnitude = magnitude;
        return;
    }
    {
        char *end = NULL;
        double value;

        errno = 0;
        value = strtod(token, &end);
        out->is_number = true;
        out->is_real = true;
        out->negative = negative;
        if (end == NULL || *end != '\0' || errno == ERANGE || !fsm_number_finite(value)) {
            out->malformed = true;
            return;
        }
        out->real = value;
    }
}

/**
 * Interpret a scalar the way the schema's "expression_or_literal" does:
 * a quoted scalar is always a string, hence an expression; a plain scalar is a
 * number or boolean when it looks like one, and an expression otherwise.
 */
static bool fsm_operand(fsm_ctx_t *ctx, const fsm_node_t *node, const char *what,
                        cancestry_fsm_operand_t *out)
{
    fsm_number_t number;

    memset(out, 0, sizeof(*out));
    if (node == NULL || node->kind != FSM_NODE_SCALAR) {
        (void)fsm_fail_at(ctx->parse, (node != NULL) ? node->line : 0u,
                          (node != NULL) ? node->column : 1u,
                          "%s must be a number, a boolean or an expression string", what);
        return false;
    }
    if (node->text_length > CANCESTRY_FSM_EXPRESSION_MAX) {
        (void)fsm_fail_at(ctx->parse, node->line, node->column,
                          "%s is longer than %u characters", what,
                          (unsigned)CANCESTRY_FSM_EXPRESSION_MAX);
        return false;
    }
    if (!node->quoted) {
        if (node->text_length == 4u && memcmp(node->text, "true", 4u) == 0) {
            out->is_expression = false;
            out->literal.kind = CANCESTRY_VALUE_KIND_BOOL;
            out->literal.value.boolean = true;
            return true;
        }
        if (node->text_length == 5u && memcmp(node->text, "false", 5u) == 0) {
            out->is_expression = false;
            out->literal.kind = CANCESTRY_VALUE_KIND_BOOL;
            out->literal.value.boolean = false;
            return true;
        }
        fsm_scan_number(node->text, node->text_length, &number);
        if (number.malformed) {
            (void)fsm_fail_at(ctx->parse, node->line, node->column,
                              "%s is not a representable number", what);
            return false;
        }
        if (number.is_number) {
            out->is_expression = false;
            if (number.is_real) {
                out->literal.kind = CANCESTRY_VALUE_KIND_REAL;
                out->literal.value.real = number.real;
                return true;
            }
            if (number.magnitude > (uint64_t)INT64_MAX) {
                /* Beyond int64 the value degrades to a real instead of wrapping,
                 * matching the recipe loader. */
                out->literal.kind = CANCESTRY_VALUE_KIND_REAL;
                out->literal.value.real =
                    number.negative ? -(double)number.magnitude : (double)number.magnitude;
                return true;
            }
            out->literal.kind = CANCESTRY_VALUE_KIND_INT;
            /* The sign is applied as a subtraction, so -9223372036854775808 (a
             * magnitude of INT64_MAX + 1) is not rejected as "too large". */
            out->literal.value.integer =
                number.negative ? -(int64_t)(number.magnitude - 1u) - 1 : (int64_t)number.magnitude;
            return true;
        }
    }
    /* A string: an expression, evaluated when the action or guard runs. */
    out->is_expression = true;
    out->expression = node->text;
    return true;
}

static bool fsm_validate_expression(fsm_ctx_t *ctx, const fsm_node_t *node, const char *what)
{
    cancestry_fsm_expression_error_t error = CANCESTRY_FSM_EXPRESSION_OK;
    size_t offset = 0u;

    if (node == NULL || node->kind != FSM_NODE_SCALAR) {
        (void)fsm_fail_at(ctx->parse, (node != NULL) ? node->line : 0u,
                          (node != NULL) ? node->column : 1u, "%s must be a string", what);
        return false;
    }
    if (node->text_length > CANCESTRY_FSM_EXPRESSION_MAX) {
        (void)fsm_fail_at(ctx->parse, node->line, node->column,
                          "%s is longer than %u characters", what,
                          (unsigned)CANCESTRY_FSM_EXPRESSION_MAX);
        return false;
    }
    if (!cancestry_fsm_expression_validate(node->text, &error, &offset)) {
        (void)fsm_fail_at(ctx->parse, node->line, node->column + offset,
                          "%s is not a valid expression: %s", what,
                          cancestry_fsm_expression_error_name(error));
        return false;
    }
    return true;
}

static cancestry_event_type_t fsm_event_type_from_name(const char *name)
{
    if (name == NULL) {
        return CANCESTRY_EVENT_TYPE_INVALID;
    }
    if (strcmp(name, "can_rx") == 0) {
        return CANCESTRY_EVENT_TYPE_CAN_RX;
    }
    if (strcmp(name, "signal_changed") == 0) {
        return CANCESTRY_EVENT_TYPE_SIGNAL_CHANGED;
    }
    if (strcmp(name, "timer_expired") == 0) {
        return CANCESTRY_EVENT_TYPE_TIMER_EXPIRED;
    }
    if (strcmp(name, "state_entered") == 0) {
        return CANCESTRY_EVENT_TYPE_STATE_ENTERED;
    }
    if (strcmp(name, "state_exited") == 0) {
        return CANCESTRY_EVENT_TYPE_STATE_EXITED;
    }
    if (strcmp(name, "fault_raised") == 0) {
        return CANCESTRY_EVENT_TYPE_FAULT_RAISED;
    }
    if (strcmp(name, "power_mode_changed") == 0) {
        return CANCESTRY_EVENT_TYPE_POWER_MODE_CHANGED;
    }
    return CANCESTRY_EVENT_TYPE_INVALID;
}

/* ------------------------------------------------------------------------- */
/* Definition building                                                       */
/* ------------------------------------------------------------------------- */

/** Name sets of one machine, used to resolve references at load time. */
typedef struct fsm_names {
    const char **items;
    size_t count;
} fsm_names_t;

static int fsm_name_index(const fsm_names_t *names, const char *name)
{
    size_t i;

    if (names == NULL || name == NULL) {
        return -1;
    }
    for (i = 0u; i < names->count; ++i) {
        if (names->items[i] != NULL && strcmp(names->items[i], name) == 0) {
            return (int)i;
        }
    }
    return -1;
}

/** Collect the "name" scalar of every item of a sequence, rejecting duplicates. */
static bool fsm_collect_names(fsm_ctx_t *ctx,
                             const fsm_node_t *seq,
                             const char *what,
                             fsm_names_t *out)
{
    size_t count = (seq != NULL) ? seq->item_count : 0u;
    size_t i;

    out->items = NULL;
    out->count = 0u;
    if (count == 0u) {
        return true;
    }
    out->items = (const char **)fsm_arena_calloc(ctx->arena, count, sizeof(*out->items));
    if (out->items == NULL) {
        (void)fsm_fail(ctx->parse, CANCESTRY_FSM_ERR_NO_MEMORY, 0u, 0u, "out of memory");
        return false;
    }
    out->count = count;
    for (i = 0u; i < count; ++i) {
        const fsm_node_t *item = seq->items[i];
        const fsm_node_t *name = fsm_map_value(item, "name");
        const char *text = NULL;
        size_t j;

        if (!fsm_expect_map(ctx, item, what)) {
            return false;
        }
        if (!fsm_scalar_text(ctx, name, "name", &text, CANCESTRY_FSM_NAME_MAX, NULL)) {
            return false;
        }
        for (j = 0u; j < i; ++j) {
            if (strcmp(out->items[j], text) == 0) {
                (void)fsm_fail_at(ctx->parse, name->line, name->column,
                                  "duplicate %s name \"%s\" (names must be unique within a state "
                                  "machine)",
                                  what, text);
                return false;
            }
        }
        out->items[i] = text;
    }
    return true;
}

static const char *const fsm_action_keys[] = {"send_message", "set_signal", "set_variable",
                                              "start_timer",  "stop_timer", "reset_timer", "log",
                                              "raise_fault",  "transition", NULL};

static bool fsm_build_send_message(fsm_ctx_t *ctx,
                                   const fsm_node_t *body,
                                   cancestry_fsm_action_t *action)
{
    static const char *const allowed[] = {"interface", "message", "signals", NULL};
    cancestry_fsm_signal_value_t *values;
    const fsm_node_t *signals = fsm_map_value(body, "signals");
    const fsm_node_t *interface = fsm_map_value(body, "interface");
    const fsm_node_t *message = fsm_map_value(body, "message");
    size_t count = 0u;
    size_t i;
    const char *text;

    if (!fsm_expect_map(ctx, body, "send_message")) {
        return false;
    }
    if (!fsm_key_allowed(ctx, body, allowed)) {
        return false;
    }
    if (!fsm_scalar_text(ctx, interface, "send_message.interface", &text, CANCESTRY_FSM_NAME_MAX,
                        NULL)) {
        return false;
    }
    action->as.send_message.interface = text;
    if (!fsm_scalar_text(ctx, message, "send_message.message", &text, CANCESTRY_FSM_NAME_MAX,
                         NULL)) {
        return false;
    }
    action->as.send_message.message = text;
    if (signals == NULL || signals->kind != FSM_NODE_MAP || signals->pair_count == 0u) {
        (void)fsm_fail_at(ctx->parse, (signals != NULL) ? signals->line : body->line, 1u,
                          "send_message.signals must be a mapping with at least one entry");
        return false;
    }
    count = signals->pair_count;
    values = (cancestry_fsm_signal_value_t *)fsm_arena_calloc(ctx->arena, count, sizeof(*values));
    if (values == NULL) {
        (void)fsm_fail(ctx->parse, CANCESTRY_FSM_ERR_NO_MEMORY, signals->line, 1u, "out of memory");
        return false;
    }
    for (i = 0u; i < count; ++i) {
        values[i].name = signals->pairs[i].key; /* borrowed from the arena */
        if (!fsm_operand(ctx, signals->pairs[i].value, "send_message signal value",
                         &values[i].operand)) {
            return false;
        }
        if (values[i].operand.is_expression &&
            !fsm_validate_expression(ctx, signals->pairs[i].value, "send_message signal value")) {
            return false;
        }
    }
    action->as.send_message.values = values;
    action->as.send_message.value_count = (uint16_t)count;
    return true;
}

static bool fsm_build_action(fsm_ctx_t *ctx,
                             const fsm_node_t *node,
                             const fsm_names_t *timers,
                             const fsm_names_t *variables,
                             const fsm_names_t *states,
                             cancestry_fsm_action_t *action)
{
    size_t i;
    size_t matched = 0u;
    size_t matched_key = 0u;
    size_t matched_pair = 0u;
    const fsm_node_t *body = NULL;
    const char *text;

    memset(action, 0, sizeof(*action));
    if (!fsm_expect_map(ctx, node, "action")) {
        return false;
    }
    action->source_line = (uint16_t)node->line;
    /*
     * The schema spells an action as a one-key object (a "oneOf" over the nine
     * action keys). More than one key, or a key outside that set, is therefore a
     * schema violation and is rejected rather than guessed at.
     */
    for (i = 0u; i < node->pair_count; ++i) {
        size_t k;

        for (k = 0u; fsm_action_keys[k] != NULL; ++k) {
            if (strlen(fsm_action_keys[k]) == node->pairs[i].key_length &&
                memcmp(fsm_action_keys[k], node->pairs[i].key, node->pairs[i].key_length) == 0) {
                matched++;
                matched_key = k;
                matched_pair = i;
                break;
            }
        }
        if (fsm_action_keys[k] == NULL) {
            (void)fsm_fail_at(ctx->parse, node->pairs[i].line, node->pairs[i].column,
                              "unknown action \"%.*s\"", (int)node->pairs[i].key_length,
                              node->pairs[i].key);
            return false;
        }
    }
    if (matched != 1u) {
        (void)fsm_fail_at(ctx->parse, node->line, node->column,
                          "an action must contain exactly one action key (found %u)",
                          (unsigned)matched);
        return false;
    }
    body = node->pairs[matched_pair].value;
    if (body == NULL) {
        (void)fsm_fail_at(ctx->parse, node->line, node->column, "action body is missing");
        return false;
    }
    action->kind = (cancestry_fsm_action_kind_t)matched_key;

    switch (matched_key) {
    case CANCESTRY_FSM_ACTION_SEND_MESSAGE:
        return fsm_build_send_message(ctx, body, action);
    case CANCESTRY_FSM_ACTION_SET_SIGNAL: {
        static const char *const allowed[] = {"signal", "value", NULL};
        const fsm_node_t *signal = fsm_map_value(body, "signal");

        if (!fsm_expect_map(ctx, body, "set_signal")) {
            return false;
        }
        if (!fsm_key_allowed(ctx, body, allowed)) {
            return false;
        }
        if (!fsm_scalar_text(ctx, signal, "set_signal.signal", &text, CANCESTRY_FSM_NAME_MAX,
                            NULL)) {
            return false;
        }
        action->as.set_signal.signal = text;
        if (!fsm_operand(ctx, fsm_map_value(body, "value"), "set_signal.value",
                         &action->as.set_signal.operand)) {
            return false;
        }
        if (action->as.set_signal.operand.is_expression &&
            !fsm_validate_expression(ctx, fsm_map_value(body, "value"), "set_signal.value")) {
            return false;
        }
        return true;
    }
    case CANCESTRY_FSM_ACTION_SET_VARIABLE: {
        static const char *const allowed[] = {"variable", "value", NULL};
        const fsm_node_t *variable = fsm_map_value(body, "variable");

        if (!fsm_expect_map(ctx, body, "set_variable")) {
            return false;
        }
        if (!fsm_key_allowed(ctx, body, allowed)) {
            return false;
        }
        if (!fsm_scalar_text(ctx, variable, "set_variable.variable", &text, CANCESTRY_FSM_NAME_MAX,
                            NULL)) {
            return false;
        }
        if (fsm_name_index(variables, text) < 0) {
            (void)fsm_fail_at(ctx->parse, variable->line, variable->column,
                              "set_variable names \"%s\", which the state machine does not declare",
                              text);
            return false;
        }
        action->as.set_variable.variable = text;
        if (!fsm_operand(ctx, fsm_map_value(body, "value"), "set_variable.value",
                         &action->as.set_variable.operand)) {
            return false;
        }
        if (action->as.set_variable.operand.is_expression &&
            !fsm_validate_expression(ctx, fsm_map_value(body, "value"), "set_variable.value")) {
            return false;
        }
        return true;
    }
    case CANCESTRY_FSM_ACTION_START_TIMER: {
        static const char *const allowed[] = {"timer", "duration_ms", "repeat", NULL};
        const fsm_node_t *timer = fsm_map_value(body, "timer");
        const fsm_node_t *duration = fsm_map_value(body, "duration_ms");
        uint64_t value = 0u;

        if (!fsm_expect_map(ctx, body, "start_timer")) {
            return false;
        }
        if (!fsm_key_allowed(ctx, body, allowed)) {
            return false;
        }
        if (!fsm_scalar_text(ctx, timer, "start_timer.timer", &text, CANCESTRY_FSM_NAME_MAX, NULL)) {
            return false;
        }
        if (fsm_name_index(timers, text) < 0) {
            (void)fsm_fail_at(ctx->parse, timer->line, timer->column,
                              "start_timer names \"%s\", which the state machine does not declare",
                              text);
            return false;
        }
        action->as.start_timer.timer = text;
        if (duration != NULL) {
            if (!fsm_scalar_uint(ctx, duration, "start_timer.duration_ms", &value) ||
                value < 1u || value > 0xFFFFFFFFu) {
                if (!ctx->parse->failed) {
                    (void)fsm_fail_at(ctx->parse, duration->line, duration->column,
                                      "start_timer.duration_ms must be an integer of at least 1");
                }
                return false;
            }
            action->as.start_timer.duration_ms = (uint32_t)value;
            action->as.start_timer.has_duration_ms = true;
        }
        if (fsm_map_value(body, "repeat") != NULL) {
            if (!fsm_scalar_bool(ctx, fsm_map_value(body, "repeat"), "start_timer.repeat",
                                 &action->as.start_timer.repeat)) {
                return false;
            }
            action->as.start_timer.has_repeat = true;
        }
        return true;
    }
    case CANCESTRY_FSM_ACTION_STOP_TIMER:
    case CANCESTRY_FSM_ACTION_RESET_TIMER: {
        static const char *const allowed[] = {"timer", NULL};
        const fsm_node_t *timer = fsm_map_value(body, "timer");

        if (!fsm_expect_map(ctx, body, "timer action")) {
            return false;
        }
        if (!fsm_key_allowed(ctx, body, allowed)) {
            return false;
        }
        if (!fsm_scalar_text(ctx, timer, "timer", &text, CANCESTRY_FSM_NAME_MAX, NULL)) {
            return false;
        }
        if (fsm_name_index(timers, text) < 0) {
            (void)fsm_fail_at(ctx->parse, timer->line, timer->column,
                              "a timer action names \"%s\", which the state machine does not "
                              "declare",
                              text);
            return false;
        }
        action->as.timer.timer = text;
        return true;
    }
    case CANCESTRY_FSM_ACTION_LOG: {
        static const char *const allowed[] = {"level", "message", NULL};
        const fsm_node_t *level = fsm_map_value(body, "level");
        const fsm_node_t *message = fsm_map_value(body, "message");

        if (!fsm_expect_map(ctx, body, "log")) {
            return false;
        }
        if (!fsm_key_allowed(ctx, body, allowed)) {
            return false;
        }
        if (!fsm_scalar_text(ctx, level, "log.level", &text, 16u, NULL)) {
            return false;
        }
        if (strcmp(text, "info") == 0) {
            action->as.log.level = CANCESTRY_FSM_LOG_INFO;
        } else if (strcmp(text, "warning") == 0) {
            action->as.log.level = CANCESTRY_FSM_LOG_WARNING;
        } else if (strcmp(text, "error") == 0) {
            action->as.log.level = CANCESTRY_FSM_LOG_ERROR;
        } else {
            (void)fsm_fail_at(ctx->parse, level->line, level->column,
                              "log.level must be info, warning or error");
            return false;
        }
        if (!fsm_scalar_text(ctx, message, "log.message", &text, CANCESTRY_FSM_LOG_MESSAGE_MAX,
                            NULL)) {
            return false;
        }
        action->as.log.message = text;
        return true;
    }
    case CANCESTRY_FSM_ACTION_RAISE_FAULT: {
        static const char *const allowed[] = {"code", "severity", NULL};
        const fsm_node_t *code = fsm_map_value(body, "code");
        const fsm_node_t *severity = fsm_map_value(body, "severity");

        if (!fsm_expect_map(ctx, body, "raise_fault")) {
            return false;
        }
        if (!fsm_key_allowed(ctx, body, allowed)) {
            return false;
        }
        if (!fsm_scalar_text(ctx, code, "raise_fault.code", &text, CANCESTRY_FSM_FAULT_CODE_MAX,
                            NULL)) {
            return false;
        }
        action->as.raise_fault.code = text;
        if (!fsm_scalar_text(ctx, severity, "raise_fault.severity", &text, 16u, NULL)) {
            return false;
        }
        if (strcmp(text, "warning") == 0) {
            action->as.raise_fault.severity = CANCESTRY_FAULT_SEVERITY_WARNING;
        } else if (strcmp(text, "error") == 0) {
            action->as.raise_fault.severity = CANCESTRY_FAULT_SEVERITY_ERROR;
        } else if (strcmp(text, "critical") == 0) {
            action->as.raise_fault.severity = CANCESTRY_FAULT_SEVERITY_CRITICAL;
        } else {
            (void)fsm_fail_at(ctx->parse, severity->line, severity->column,
                              "raise_fault.severity must be warning, error or critical");
            return false;
        }
        return true;
    }
    case CANCESTRY_FSM_ACTION_TRANSITION: {
        static const char *const allowed[] = {"target", NULL};
        const fsm_node_t *target = fsm_map_value(body, "target");
        int index;

        if (!fsm_expect_map(ctx, body, "transition")) {
            return false;
        }
        if (!fsm_key_allowed(ctx, body, allowed)) {
            return false;
        }
        if (!fsm_scalar_text(ctx, target, "transition.target", &text, CANCESTRY_FSM_NAME_MAX,
                            NULL)) {
            return false;
        }
        index = fsm_name_index(states, text);
        if (index < 0) {
            (void)fsm_fail_at(ctx->parse, target->line, target->column,
                              "a transition action targets \"%s\", which is not a declared state",
                              text);
            return false;
        }
        action->as.transition.target = text;
        action->as.transition.target_index = (uint16_t)index;
        return true;
    }
    default:
        return false;
    }
}

static bool fsm_build_actions(fsm_ctx_t *ctx,
                              const fsm_node_t *seq,
                              const fsm_names_t *timers,
                              const fsm_names_t *variables,
                              const fsm_names_t *states,
                              cancestry_fsm_action_t **out,
                              uint16_t *count_out)
{
    size_t count = 0u;
    size_t i;
    cancestry_fsm_action_t *actions;

    *out = NULL;
    *count_out = 0u;
    if (!fsm_expect_seq(ctx, seq, "action list", &count)) {
        return false;
    }
    if (count == 0u) {
        return true;
    }
    actions = (cancestry_fsm_action_t *)fsm_arena_calloc(ctx->arena, count, sizeof(*actions));
    if (actions == NULL) {
        (void)fsm_fail(ctx->parse, CANCESTRY_FSM_ERR_NO_MEMORY, seq->line, 1u, "out of memory");
        return false;
    }
    for (i = 0u; i < count; ++i) {
        if (!fsm_build_action(ctx, seq->items[i], timers, variables, states, &actions[i])) {
            return false;
        }
    }
    *out = actions;
    *count_out = (uint16_t)count;
    return true;
}

static bool fsm_build_transitions(fsm_ctx_t *ctx,
                                  const fsm_node_t *seq,
                                  const fsm_names_t *timers,
                                  const fsm_names_t *variables,
                                  const fsm_names_t *states,
                                  cancestry_fsm_transition_t **out,
                                  uint16_t *count_out)
{
    size_t count = 0u;
    size_t i;
    cancestry_fsm_transition_t *transitions;

    *out = NULL;
    *count_out = 0u;
    if (!fsm_expect_seq(ctx, seq, "transitions", &count)) {
        return false;
    }
    if (count == 0u) {
        return true;
    }
    transitions =
        (cancestry_fsm_transition_t *)fsm_arena_calloc(ctx->arena, count, sizeof(*transitions));
    if (transitions == NULL) {
        (void)fsm_fail(ctx->parse, CANCESTRY_FSM_ERR_NO_MEMORY, seq->line, 1u, "out of memory");
        return false;
    }
    for (i = 0u; i < count; ++i) {
        static const char *const allowed[] = {"event", "interface", "message", "signal", "timer",
                                             "guard", "actions",     "target", NULL};
        cancestry_fsm_transition_t *transition = &transitions[i];
        const fsm_node_t *node = seq->items[i];
        const fsm_node_t *event = fsm_map_value(node, "event");
        const fsm_node_t *target = fsm_map_value(node, "target");
        const char *text = NULL;
        int index;

        if (!fsm_expect_map(ctx, node, "transition")) {
            return false;
        }
        if (!fsm_key_allowed(ctx, node, allowed)) {
            return false;
        }
        if (!fsm_scalar_text(ctx, event, "transition.event", &text, 32u, NULL)) {
            return false;
        }
        transition->event = fsm_event_type_from_name(text);
        if (transition->event == CANCESTRY_EVENT_TYPE_INVALID) {
            (void)fsm_fail_at(ctx->parse, event->line, event->column,
                              "transition.event \"%s\" is not one of the seven FSM events", text);
            return false;
        }
        if (!fsm_scalar_text(ctx, target, "transition.target", &text, CANCESTRY_FSM_NAME_MAX,
                            NULL)) {
            return false;
        }
        index = fsm_name_index(states, text);
        if (index < 0) {
            (void)fsm_fail_at(ctx->parse, target->line, target->column,
                              "transition targets \"%s\", which is not a declared state", text);
            return false;
        }
        transition->target = text;
        transition->target_index = (uint16_t)index;

        if (fsm_map_value(node, "interface") != NULL) {
            if (!fsm_scalar_text(ctx, fsm_map_value(node, "interface"), "transition.interface",
                                &text, CANCESTRY_FSM_NAME_MAX, NULL)) {
                return false;
            }
            transition->interface = text;
        }
        if (fsm_map_value(node, "message") != NULL) {
            if (!fsm_scalar_text(ctx, fsm_map_value(node, "message"), "transition.message", &text,
                                CANCESTRY_FSM_NAME_MAX, NULL)) {
                return false;
            }
            transition->message = text;
        }
        if (fsm_map_value(node, "signal") != NULL) {
            if (!fsm_scalar_text(ctx, fsm_map_value(node, "signal"), "transition.signal", &text,
                                 CANCESTRY_FSM_NAME_MAX, NULL)) {
                return false;
            }
            transition->signal = text;
        }
        if (fsm_map_value(node, "timer") != NULL) {
            if (!fsm_scalar_text(ctx, fsm_map_value(node, "timer"), "transition.timer", &text,
                                CANCESTRY_FSM_NAME_MAX, NULL)) {
                return false;
            }
            if (fsm_name_index(timers, text) < 0) {
                (void)fsm_fail_at(ctx->parse, fsm_map_value(node, "timer")->line, 1u,
                                  "transition.timer \"%s\" is not a declared timer", text);
                return false;
            }
            transition->timer = text;
        }
        /* The schema's conditionals: these two selectors need their filter field. */
        if (transition->event == CANCESTRY_EVENT_TYPE_TIMER_EXPIRED && transition->timer == NULL) {
            (void)fsm_fail_at(ctx->parse, node->line, node->column,
                              "a timer_expired transition requires a timer field");
            return false;
        }
        if (transition->event == CANCESTRY_EVENT_TYPE_SIGNAL_CHANGED && transition->signal == NULL) {
            (void)fsm_fail_at(ctx->parse, node->line, node->column,
                              "a signal_changed transition requires a signal field");
            return false;
        }
        if (transition->event == CANCESTRY_EVENT_TYPE_TIMER_EXPIRED &&
            fsm_name_index(timers, transition->timer) < 0) {
            (void)fsm_fail_at(ctx->parse, node->line, node->column,
                              "a timer_expired transition needs its timer declared");
            return false;
        }
        if (fsm_map_value(node, "guard") != NULL) {
            const fsm_node_t *guard = fsm_map_value(node, "guard");

            if (!fsm_validate_expression(ctx, guard, "transition.guard")) {
                return false;
            }
            transition->guard = guard->text;
        }
        {
            cancestry_fsm_action_t *actions = NULL;
            uint16_t action_count = 0u;

            if (!fsm_build_actions(ctx, fsm_map_value(node, "actions"), timers, variables, states,
                                   &actions, &action_count)) {
                return false;
            }
            transition->actions = actions;
            transition->action_count = action_count;
        }
    }
    *out = transitions;
    *count_out = (uint16_t)count;
    return true;
}

static bool fsm_build_machine(fsm_ctx_t *ctx,
                              const fsm_node_t *node,
                              cancestry_fsm_machine_t *machine)
{
    static const char *const allowed[] = {"name", "description", "initial", "variables", "timers",
                                         "states", NULL};
    fsm_names_t state_names;
    fsm_names_t timer_names;
    fsm_names_t variable_names;
    fsm_vec_t states;
    fsm_vec_t variables;
    fsm_vec_t timers;
    const fsm_node_t *state_seq = fsm_map_value(node, "states");
    const fsm_node_t *initial = fsm_map_value(node, "initial");
    size_t state_count = 0u;
    size_t i;
    const char *text;
    int index;

    memset(machine, 0, sizeof(*machine));
    if (!fsm_expect_map(ctx, node, "state machine")) {
        return false;
    }
    if (!fsm_key_allowed(ctx, node, allowed)) {
        return false;
    }
    if (!fsm_scalar_text(ctx, fsm_map_value(node, "name"), "state_machines[].name", &text,
                        CANCESTRY_FSM_NAME_MAX, NULL)) {
        return false;
    }
    machine->name = text;
    if (fsm_map_value(node, "description") != NULL) {
        if (!fsm_scalar_text(ctx, fsm_map_value(node, "description"), "description", &text,
                            CANCESTRY_FSM_DESCRIPTION_MAX, NULL)) {
            return false;
        }
        machine->description = text;
    }
    if (!fsm_scalar_text(ctx, initial, "initial", &text, CANCESTRY_FSM_NAME_MAX, NULL)) {
        return false;
    }
    if (!fsm_expect_seq(ctx, state_seq, "states", &state_count) || state_count == 0u) {
        if (!ctx->parse->failed) {
            (void)fsm_fail_at(ctx->parse, (state_seq != NULL) ? state_seq->line : node->line, 1u,
                              "a state machine needs at least one state");
        }
        return false;
    }
    /* Names first: uniqueness and reference resolution both need them. */
    if (!fsm_collect_names(ctx, state_seq, "state", &state_names)) {
        return false;
    }
    if (!fsm_collect_names(ctx, fsm_map_value(node, "timers"), "timer", &timer_names)) {
        return false;
    }
    if (!fsm_collect_names(ctx, fsm_map_value(node, "variables"), "variable", &variable_names)) {
        return false;
    }
    index = fsm_name_index(&state_names, text);
    if (index < 0) {
        (void)fsm_fail_at(ctx->parse, initial->line, initial->column,
                          "the initial state \"%s\" is not declared (SW-FR-FSM-005)", text);
        return false;
    }
    machine->initial = text;
    machine->initial_index = (uint16_t)index;

    /* Variables. */
    fsm_vec_init(&variables, sizeof(cancestry_fsm_variable_def_t));
    {
        const fsm_node_t *variable_seq = fsm_map_value(node, "variables");
        size_t count = 0u;

        if (!fsm_expect_seq(ctx, variable_seq, "variables", &count)) {
            return false;
        }
        for (i = 0u; i < count; ++i) {
            static const char *const variable_allowed[] = {"name", "type", "default", NULL};
            cancestry_fsm_variable_def_t variable;
            const fsm_node_t *item = variable_seq->items[i];

            memset(&variable, 0, sizeof(variable));
            if (!fsm_expect_map(ctx, item, "variable")) {
                return false;
            }
            if (!fsm_key_allowed(ctx, item, variable_allowed)) {
                return false;
            }
            if (!fsm_scalar_text(ctx, fsm_map_value(item, "name"), "variable.name", &text,
                                CANCESTRY_FSM_NAME_MAX, NULL)) {
                return false;
            }
            variable.name = text;
            if (!fsm_scalar_text(ctx, fsm_map_value(item, "type"), "variable.type", &text, 16u,
                                NULL)) {
                return false;
            }
            variable.type = cancestry_fsm_variable_type_from_name(text);
            if (variable.type == CANCESTRY_VALUE_KIND_UNSET) {
                (void)fsm_fail_at(ctx->parse, fsm_map_value(item, "type")->line, 1u,
                                  "variable.type must be boolean, integer or float");
                return false;
            }
            if (fsm_map_value(item, "default") != NULL) {
                if (!fsm_operand(ctx, fsm_map_value(item, "default"), "variable.default",
                                &variable.default_value)) {
                    return false;
                }
                if (variable.default_value.is_expression &&
                    !fsm_validate_expression(ctx, fsm_map_value(item, "default"),
                                            "variable.default")) {
                    return false;
                }
                variable.has_default = true;
            }
            if (!fsm_vec_push(ctx->arena, &variables, &variable)) {
                (void)fsm_fail(ctx->parse, CANCESTRY_FSM_ERR_NO_MEMORY, 0u, 0u, "out of memory");
                return false;
            }
        }
    }
    if (variables.count != 0u &&
        variables.count != (size_t)variable_names.count) {
        (void)fsm_fail_at(ctx->parse, node->line, node->column, "internal variable count mismatch");
        return false;
    }
    machine->variables = (const cancestry_fsm_variable_def_t *)fsm_vec_seal(&variables);
    machine->variable_count = (uint16_t)variables.count;

    /* Timers. */
    fsm_vec_init(&timers, sizeof(cancestry_fsm_timer_def_t));
    {
        const fsm_node_t *timer_seq = fsm_map_value(node, "timers");
        size_t count = 0u;

        if (!fsm_expect_seq(ctx, timer_seq, "timers", &count)) {
            return false;
        }
        for (i = 0u; i < count; ++i) {
            static const char *const timer_allowed[] = {"name", "duration_ms", "repeat",
                                                        "auto_start", NULL};
            cancestry_fsm_timer_def_t timer;
            const fsm_node_t *item = timer_seq->items[i];
            uint64_t duration = 0u;

            memset(&timer, 0, sizeof(timer));
            if (!fsm_expect_map(ctx, item, "timer")) {
                return false;
            }
            if (!fsm_key_allowed(ctx, item, timer_allowed)) {
                return false;
            }
            if (!fsm_scalar_text(ctx, fsm_map_value(item, "name"), "timer.name", &text,
                                CANCESTRY_FSM_NAME_MAX, NULL)) {
                return false;
            }
            timer.name = text;
            if (!fsm_scalar_uint(ctx, fsm_map_value(item, "duration_ms"), "timer.duration_ms",
                                &duration) ||
                duration < 1u || duration > 0xFFFFFFFFu) {
                if (!ctx->parse->failed) {
                    (void)fsm_fail_at(ctx->parse, item->line, item->column,
                                      "timer.duration_ms must be an integer of at least 1");
                }
                return false;
            }
            timer.duration_ms = (uint32_t)duration;
            if (!fsm_scalar_bool(ctx, fsm_map_value(item, "repeat"), "timer.repeat",
                                &timer.repeat)) {
                return false;
            }
            if (!fsm_scalar_bool(ctx, fsm_map_value(item, "auto_start"), "timer.auto_start",
                                &timer.auto_start)) {
                return false;
            }
            if (!fsm_vec_push(ctx->arena, &timers, &timer)) {
                (void)fsm_fail(ctx->parse, CANCESTRY_FSM_ERR_NO_MEMORY, 0u, 0u, "out of memory");
                return false;
            }
        }
    }
    machine->timers = (const cancestry_fsm_timer_def_t *)fsm_vec_seal(&timers);
    machine->timer_count = (uint16_t)timers.count;

    /* States, with their actions and transitions. */
    fsm_vec_init(&states, sizeof(cancestry_fsm_state_t));
    for (i = 0u; i < state_count; ++i) {
        static const char *const state_allowed[] = {"name", "entry", "exit", "transitions", NULL};
        const fsm_node_t *item = state_seq->items[i];
        cancestry_fsm_state_t state;

        memset(&state, 0, sizeof(state));
        if (!fsm_expect_map(ctx, item, "state")) {
            return false;
        }
        if (!fsm_key_allowed(ctx, item, state_allowed)) {
            return false;
        }
        if (!fsm_scalar_text(ctx, fsm_map_value(item, "name"), "state.name", &text,
                            CANCESTRY_FSM_NAME_MAX, NULL)) {
            return false;
        }
        state.name = text;
        {
            cancestry_fsm_action_t *entry = NULL;
            cancestry_fsm_action_t *exit_actions = NULL;
            cancestry_fsm_transition_t *transitions = NULL;
            uint16_t entry_count = 0u;
            uint16_t exit_count = 0u;
            uint16_t transition_count = 0u;

            if (!fsm_build_actions(ctx, fsm_map_value(item, "entry"), &timer_names,
                                   &variable_names, &state_names, &entry, &entry_count)) {
                return false;
            }
            if (!fsm_build_actions(ctx, fsm_map_value(item, "exit"), &timer_names,
                                   &variable_names, &state_names, &exit_actions, &exit_count)) {
                return false;
            }
            if (!fsm_build_transitions(ctx, fsm_map_value(item, "transitions"), &timer_names,
                                       &variable_names, &state_names, &transitions,
                                       &transition_count)) {
                return false;
            }
            state.entry = entry;
            state.entry_count = entry_count;
            state.exit = exit_actions;
            state.exit_count = exit_count;
            state.transitions = transitions;
            state.transition_count = transition_count;
        }
        if (!fsm_vec_push(ctx->arena, &states, &state)) {
            (void)fsm_fail(ctx->parse, CANCESTRY_FSM_ERR_NO_MEMORY, 0u, 0u, "out of memory");
            return false;
        }
    }
    machine->states = (const cancestry_fsm_state_t *)fsm_vec_seal(&states);
    machine->state_count = (uint16_t)states.count;
    return true;
}

static bool fsm_build_instance(fsm_ctx_t *ctx,
                               const fsm_node_t *node,
                               const cancestry_fsm_machine_t *machines,
                               uint16_t machine_count,
                               cancestry_fsm_instance_def_t *instance)
{
    static const char *const allowed[] = {"id",        "machine", "enabled", "bindings",
                                         "variables", "subscriptions", NULL};
    const fsm_node_t *subscriptions;
    const char *text;
    size_t i;
    int index;

    memset(instance, 0, sizeof(*instance));
    if (!fsm_expect_map(ctx, node, "instance")) {
        return false;
    }
    if (!fsm_key_allowed(ctx, node, allowed)) {
        return false;
    }
    if (!fsm_scalar_text(ctx, fsm_map_value(node, "id"), "instance.id", &text,
                        CANCESTRY_FSM_NAME_MAX, NULL)) {
        return false;
    }
    instance->id = text;
    if (!fsm_scalar_text(ctx, fsm_map_value(node, "machine"), "instance.machine", &text,
                        CANCESTRY_FSM_NAME_MAX, NULL)) {
        return false;
    }
    index = -1;
    for (i = 0u; i < machine_count; ++i) {
        if (strcmp(machines[i].name, text) == 0) {
            index = (int)i;
            break;
        }
    }
    if (index < 0) {
        (void)fsm_fail_at(ctx->parse, fsm_map_value(node, "machine")->line, 1u,
                          "instance \"%s\" references machine \"%s\", which is not declared",
                          instance->id, text);
        return false;
    }
    instance->machine = text;
    instance->machine_index = (uint16_t)index;
    if (!fsm_scalar_bool(ctx, fsm_map_value(node, "enabled"), "instance.enabled",
                        &instance->enabled)) {
        return false;
    }

    if (fsm_map_value(node, "bindings") != NULL) {
        const fsm_node_t *bindings = fsm_map_value(node, "bindings");
        fsm_vec_t vec;

        if (bindings->kind != FSM_NODE_MAP || bindings->pair_count == 0u) {
            (void)fsm_fail_at(ctx->parse, bindings->line, bindings->column,
                              "instance.bindings must be a non-empty mapping of alias to physical "
                              "interface");
            return false;
        }
        fsm_vec_init(&vec, sizeof(cancestry_fsm_binding_t));
        for (i = 0u; i < bindings->pair_count; ++i) {
            cancestry_fsm_binding_t binding;
            const fsm_node_t *value = bindings->pairs[i].value;

            if (bindings->pairs[i].key_length > CANCESTRY_FSM_NAME_MAX) {
                (void)fsm_fail_at(ctx->parse, bindings->pairs[i].line,
                                  bindings->pairs[i].column, "binding alias is too long");
                return false;
            }
            if (value == NULL || value->kind != FSM_NODE_SCALAR) {
                (void)fsm_fail_at(ctx->parse, bindings->line, 1u,
                                  "instance.bindings values must be physical interface names");
                return false;
            }
            binding.alias = bindings->pairs[i].key;
            binding.physical = value->text;
            if (!fsm_vec_push(ctx->arena, &vec, &binding)) {
                (void)fsm_fail(ctx->parse, CANCESTRY_FSM_ERR_NO_MEMORY, 0u, 0u, "out of memory");
                return false;
            }
        }
        instance->bindings = (const cancestry_fsm_binding_t *)fsm_vec_seal(&vec);
        instance->binding_count = (uint16_t)vec.count;
    }

    if (fsm_map_value(node, "variables") != NULL) {
        const fsm_node_t *overrides = fsm_map_value(node, "variables");
        const cancestry_fsm_machine_t *machine = &machines[instance->machine_index];
        fsm_vec_t vec;

        if (overrides->kind != FSM_NODE_MAP || overrides->pair_count == 0u) {
            (void)fsm_fail_at(ctx->parse, overrides->line, overrides->column,
                              "instance.variables must be a non-empty mapping");
            return false;
        }
        fsm_vec_init(&vec, sizeof(cancestry_fsm_variable_override_t));
        for (i = 0u; i < overrides->pair_count; ++i) {
            cancestry_fsm_variable_override_t override;
            size_t v;
            bool declared = false;

            memset(&override, 0, sizeof(override));
            if (overrides->pairs[i].key_length > CANCESTRY_FSM_NAME_MAX) {
                (void)fsm_fail_at(ctx->parse, overrides->pairs[i].line,
                                  overrides->pairs[i].column, "variable name is too long");
                return false;
            }
            for (v = 0u; v < machine->variable_count; ++v) {
                if (strcmp(machine->variables[v].name, overrides->pairs[i].key) == 0) {
                    declared = true;
                    break;
                }
            }
            if (!declared) {
                (void)fsm_fail_at(ctx->parse, overrides->pairs[i].line,
                                  overrides->pairs[i].column,
                                  "instance \"%s\" initialises variable \"%s\", which machine \"%s\" "
                                  "does not declare",
                                  instance->id, overrides->pairs[i].key, machine->name);
                return false;
            }
            override.name = overrides->pairs[i].key;
            if (!fsm_operand(ctx, overrides->pairs[i].value, "instance variable initializer",
                            &override.operand)) {
                return false;
            }
            if (override.operand.is_expression &&
                !fsm_validate_expression(ctx, overrides->pairs[i].value,
                                        "instance variable initializer")) {
                return false;
            }
            if (!fsm_vec_push(ctx->arena, &vec, &override)) {
                (void)fsm_fail(ctx->parse, CANCESTRY_FSM_ERR_NO_MEMORY, 0u, 0u, "out of memory");
                return false;
            }
        }
        instance->variables = (const cancestry_fsm_variable_override_t *)fsm_vec_seal(&vec);
        instance->variable_count = (uint16_t)vec.count;
    }

    subscriptions = fsm_map_value(node, "subscriptions");
    if (subscriptions != NULL) {
        static const char *const subscription_allowed[] = {"can_rx", "signals", "timers", "faults",
                                                           "power_mode", NULL};

        if (!fsm_expect_map(ctx, subscriptions, "subscriptions")) {
            return false;
        }
        if (!fsm_key_allowed(ctx, subscriptions, subscription_allowed)) {
            return false;
        }
        if (fsm_map_value(subscriptions, "timers") != NULL) {
            if (!fsm_scalar_bool(ctx, fsm_map_value(subscriptions, "timers"),
                                 "subscriptions.timers", &instance->subscriptions.timers)) {
                return false;
            }
            instance->subscriptions.has_timers = true;
        }
        if (fsm_map_value(subscriptions, "faults") != NULL) {
            if (!fsm_scalar_bool(ctx, fsm_map_value(subscriptions, "faults"),
                                 "subscriptions.faults", &instance->subscriptions.faults)) {
                return false;
            }
            instance->subscriptions.has_faults = true;
        }
        if (fsm_map_value(subscriptions, "power_mode") != NULL) {
            if (!fsm_scalar_bool(ctx, fsm_map_value(subscriptions, "power_mode"),
                                 "subscriptions.power_mode", &instance->subscriptions.power_mode)) {
                return false;
            }
            instance->subscriptions.has_power_mode = true;
        }
        if (fsm_map_value(subscriptions, "signals") != NULL) {
            const fsm_node_t *signals = fsm_map_value(subscriptions, "signals");
            size_t count = 0u;
            fsm_vec_t vec;

            if (!fsm_expect_seq(ctx, signals, "subscriptions.signals", &count)) {
                return false;
            }
            instance->subscriptions.has_signals = true;
            fsm_vec_init(&vec, sizeof(char *));
            for (i = 0u; i < count; ++i) {
                const char *signal_name = NULL;

                if (!fsm_scalar_text(ctx, signals->items[i], "subscriptions.signals entry",
                                     &signal_name, CANCESTRY_FSM_NAME_MAX, NULL)) {
                    return false;
                }
                if (!fsm_vec_push(ctx->arena, &vec, &signal_name)) {
                    (void)fsm_fail(ctx->parse, CANCESTRY_FSM_ERR_NO_MEMORY, 0u, 0u,
                                   "out of memory");
                    return false;
                }
            }
            instance->subscriptions.signals = (const char *const *)fsm_vec_seal(&vec);
            instance->subscriptions.signal_count = (uint16_t)vec.count;
        }
        if (fsm_map_value(subscriptions, "can_rx") != NULL) {
            const fsm_node_t *can_rx = fsm_map_value(subscriptions, "can_rx");
            size_t count = 0u;
            fsm_vec_t vec;

            if (!fsm_expect_seq(ctx, can_rx, "subscriptions.can_rx", &count)) {
                return false;
            }
            instance->subscriptions.has_can_rx = true;
            fsm_vec_init(&vec, sizeof(cancestry_fsm_can_rx_subscription_t));
            for (i = 0u; i < count; ++i) {
                static const char *const can_rx_allowed[] = {"interface", "id", "message", NULL};
                cancestry_fsm_can_rx_subscription_t subscription;
                const fsm_node_t *item = can_rx->items[i];
                uint64_t id = 0u;

                memset(&subscription, 0, sizeof(subscription));
                if (!fsm_expect_map(ctx, item, "subscriptions.can_rx entry")) {
                    return false;
                }
                if (!fsm_key_allowed(ctx, item, can_rx_allowed)) {
                    return false;
                }
                if (!fsm_scalar_text(ctx, fsm_map_value(item, "interface"),
                                     "subscriptions.can_rx.interface", &text,
                                     CANCESTRY_FSM_NAME_MAX, NULL)) {
                    return false;
                }
                subscription.interface = text;
                if (fsm_map_value(item, "id") != NULL) {
                    if (!fsm_scalar_uint(ctx, fsm_map_value(item, "id"), "subscriptions.can_rx.id",
                                        &id) ||
                        id > 536870911u) {
                        if (!ctx->parse->failed) {
                            (void)fsm_fail_at(ctx->parse, item->line, item->column,
                                              "subscriptions.can_rx.id must be a CAN id in "
                                              "[0, 0x1FFFFFFF]");
                        }
                        return false;
                    }
                    subscription.can_id = (uint32_t)id;
                    subscription.has_can_id = true;
                }
                if (fsm_map_value(item, "message") != NULL) {
                    if (!fsm_scalar_text(ctx, fsm_map_value(item, "message"),
                                         "subscriptions.can_rx.message", &text,
                                         CANCESTRY_FSM_NAME_MAX, NULL)) {
                        return false;
                    }
                    subscription.message = text;
                }
                if (!fsm_vec_push(ctx->arena, &vec, &subscription)) {
                    (void)fsm_fail(ctx->parse, CANCESTRY_FSM_ERR_NO_MEMORY, 0u, 0u,
                                   "out of memory");
                    return false;
                }
            }
            instance->subscriptions.can_rx =
                (const cancestry_fsm_can_rx_subscription_t *)fsm_vec_seal(&vec);
            instance->subscriptions.can_rx_count = (uint16_t)vec.count;
        }
    }
    return true;
}

/* ------------------------------------------------------------------------- */
/* Set assembly                                                              */
/* ------------------------------------------------------------------------- */

/** Set header: the public struct plus the arena that owns everything it points to. */
typedef struct fsm_set_block {
    cancestry_fsm_set_t set;
    fsm_arena_t arena;
} fsm_set_block_t;

static const fsm_node_t *fsm_root_node(fsm_ctx_t *ctx, const fsm_node_t *root)
{
    static const char *const allowed[] = {"schema_version", "state_machines", "instances", NULL};
    const fsm_node_t *version;

    if (!fsm_expect_map(ctx, root, "an FSM document")) {
        return NULL;
    }
    if (!fsm_key_allowed(ctx, root, allowed)) {
        return NULL;
    }
    version = fsm_map_value(root, "schema_version");
    if (version == NULL || version->kind != FSM_NODE_SCALAR ||
        version->text_length != 5u ||
        (memcmp(version->text, "0.2.0", 5u) != 0 && memcmp(version->text, "0.3.0", 5u) != 0)) {
        (void)fsm_fail_at(ctx->parse, (version != NULL) ? version->line : 1u, 1u,
                          "schema_version must be the string \"0.2.0\" or \"0.3.0\" "
                          "(schemas/fsm-0.3.0.schema.json)");
        return NULL;
    }
    if (fsm_map_value(root, "state_machines") == NULL ||
        fsm_map_value(root, "state_machines")->kind != FSM_NODE_SEQ ||
        fsm_map_value(root, "state_machines")->item_count == 0u) {
        (void)fsm_fail_at(ctx->parse, root->line, root->column,
                          "state_machines must be a non-empty sequence");
        return NULL;
    }
    if (fsm_map_value(root, "instances") == NULL ||
        fsm_map_value(root, "instances")->kind != FSM_NODE_SEQ ||
        fsm_map_value(root, "instances")->item_count == 0u) {
        (void)fsm_fail_at(ctx->parse, root->line, root->column,
                          "instances must be a non-empty sequence");
        return NULL;
    }
    return root;
}

static cancestry_fsm_set_t *fsm_load_block(const char *text,
                                          size_t length,
                                          cancestry_fsm_load_error_t *error,
                                          fsm_set_block_t *block)
{
    fsm_parse_t parse;
    fsm_ctx_t ctx;
    const fsm_node_t *root;
    fsm_vec_t machines;
    fsm_vec_t instances;
    const fsm_node_t *machine_seq;
    const fsm_node_t *instance_seq;
    size_t i;

    memset(&parse, 0, sizeof(parse));
    parse.text = text;
    parse.length = length;
    parse.arena = &block->arena;
    parse.error = error;
    ctx.parse = &parse;
    ctx.arena = &block->arena;

    if (!fsm_split_lines(&parse)) {
        return NULL;
    }
    if (parse.line_count == 0u) {
        (void)fsm_fail_at(&parse, 1u, 1u, "document is empty");
        return NULL;
    }
    root = fsm_parse_node(&parse, 0u);
    if (parse.failed || root == NULL) {
        if (!parse.failed) {
            (void)fsm_fail_at(&parse, 1u, 1u, "document is not a mapping");
        }
        return NULL;
    }
    if (!fsm_parse_at_end(&parse)) {
        (void)fsm_fail_at(&parse, fsm_parse_line_number(&parse), fsm_parse_column(&parse),
                          "unexpected content after the document root");
        return NULL;
    }
    if (fsm_root_node(&ctx, root) == NULL) {
        return NULL;
    }

    machine_seq = fsm_map_value(root, "state_machines");
    instance_seq = fsm_map_value(root, "instances");

    fsm_vec_init(&machines, sizeof(cancestry_fsm_machine_t));
    for (i = 0u; i < machine_seq->item_count; ++i) {
        cancestry_fsm_machine_t machine;

        if (!fsm_build_machine(&ctx, machine_seq->items[i], &machine)) {
            return NULL;
        }
        {
            size_t j;

            for (j = 0u; j < machines.count; ++j) {
                const cancestry_fsm_machine_t *existing =
                    &((const cancestry_fsm_machine_t *)machines.items)[j];

                if (strcmp(existing->name, machine.name) == 0) {
                    (void)fsm_fail_conflict(&parse, machine_seq->items[i]->line, 1u,
                                            "two state machines are named \"%s\"", machine.name);
                    return NULL;
                }
            }
        }
        if (!fsm_vec_push(&block->arena, &machines, &machine)) {
            (void)fsm_fail(&parse, CANCESTRY_FSM_ERR_NO_MEMORY, 0u, 0u, "out of memory");
            return NULL;
        }
    }

    fsm_vec_init(&instances, sizeof(cancestry_fsm_instance_def_t));
    for (i = 0u; i < instance_seq->item_count; ++i) {
        cancestry_fsm_instance_def_t instance;

        if (!fsm_build_instance(&ctx, instance_seq->items[i],
                                (const cancestry_fsm_machine_t *)fsm_vec_seal(&machines),
                                (uint16_t)machines.count, &instance)) {
            return NULL;
        }
        {
            size_t j;

            for (j = 0u; j < instances.count; ++j) {
                const cancestry_fsm_instance_def_t *existing =
                    &((const cancestry_fsm_instance_def_t *)instances.items)[j];

                if (strcmp(existing->id, instance.id) == 0) {
                    (void)fsm_fail_conflict(&parse, instance_seq->items[i]->line, 1u,
                                            "two instances are named \"%s\" (instance ids must be "
                                            "unique)",
                                            instance.id);
                    return NULL;
                }
            }
        }
        if (!fsm_vec_push(&block->arena, &instances, &instance)) {
            (void)fsm_fail(&parse, CANCESTRY_FSM_ERR_NO_MEMORY, 0u, 0u, "out of memory");
            return NULL;
        }
    }

    block->set.machines = (const cancestry_fsm_machine_t *)fsm_vec_seal(&machines);
    block->set.machine_count = (uint16_t)machines.count;
    block->set.instances = (const cancestry_fsm_instance_def_t *)fsm_vec_seal(&instances);
    block->set.instance_count = (uint16_t)instances.count;
    return &block->set;
}

/* ------------------------------------------------------------------------- */
/* Public API                                                                */
/* ------------------------------------------------------------------------- */

cancestry_fsm_set_t *cancestry_fsm_set_load(const char *text,
                                           size_t length,
                                           cancestry_fsm_load_error_t *error)
{
    fsm_set_block_t *block;
    cancestry_fsm_set_t *set;

    if (text == NULL) {
        if (error != NULL) {
            memset(error, 0, sizeof(*error));
            error->status = CANCESTRY_FSM_ERR_NULL;
            (void)snprintf(error->message, sizeof(error->message), "text is NULL");
        }
        return NULL;
    }
    if (length == 0u) {
        if (error != NULL) {
            memset(error, 0, sizeof(*error));
            error->status = CANCESTRY_FSM_ERR_PARSE;
            error->line = 1u;
            error->column = 1u;
            (void)snprintf(error->message, sizeof(error->message), "document is empty");
        }
        return NULL;
    }
    if (memchr(text, '\0', length) != NULL) {
        if (error != NULL) {
            memset(error, 0, sizeof(*error));
            error->status = CANCESTRY_FSM_ERR_PARSE;
            error->line = 1u;
            error->column = 1u;
            (void)snprintf(error->message, sizeof(error->message),
                           "NUL bytes are not allowed in an FSM document");
        }
        return NULL;
    }
    if (error != NULL) {
        memset(error, 0, sizeof(*error));
    }

    block = (fsm_set_block_t *)malloc(sizeof(*block));
    if (block == NULL) {
        if (error != NULL) {
            error->status = CANCESTRY_FSM_ERR_NO_MEMORY;
            (void)snprintf(error->message, sizeof(error->message), "out of memory");
        }
        return NULL;
    }
    memset(block, 0, sizeof(*block));
    set = fsm_load_block(text, length, error, block);
    if (set == NULL) {
        fsm_arena_release(&block->arena);
        free(block);
        if (error != NULL && error->status == CANCESTRY_FSM_OK) {
            error->status = CANCESTRY_FSM_ERR_PARSE;
            (void)snprintf(error->message, sizeof(error->message), "invalid FSM document");
        }
        return NULL;
    }
    if (error != NULL) {
        error->status = CANCESTRY_FSM_OK;
    }
    return set;
}

void cancestry_fsm_set_free(cancestry_fsm_set_t *set)
{
    fsm_set_block_t *block = (fsm_set_block_t *)(void *)set;

    if (block == NULL) {
        return;
    }
    /*
     * The definitions point into their own arena, so releasing the arena and the
     * header block is the complete teardown: nothing else owns anything.
     */
    fsm_arena_release(&block->arena);
    free(block);
}
