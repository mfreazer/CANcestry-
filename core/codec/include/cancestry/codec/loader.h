/*
 * CANcestry - codec map loader.
 *
 * Normative references:
 *   docs/packages/codec-map-spec.md        (v0.2.1)
 *   schemas/codec-map-0.2.0.schema.json    codec map v0.2.0 schema
 *   docs/software/SwRS.md                  SW-FR-CODEC-007, SW-FR-CODEC-008
 *
 * The loader parses the YAML subset documented in core/codec/README.md and
 * validates every constraint of the codec-map-0.2.0 JSON Schema in C, so no
 * external JSON Schema validator is needed at runtime.
 *
 * Allocation strategy (SYS-NF-002):
 *   - Loading allocates: one arena for the parse tree (freed before the
 *     loader returns) and one block that holds the finished
 *     cancestry_codec_map_t and everything it points to.
 *   - The runtime library (cancestry_codec: decode/encode/namespace) never
 *     allocates. This file is deliberately kept in a separate static library
 *     (cancestry_codec_loader) so CI can symbol-scan the runtime library for
 *     allocator references.
 */

#ifndef CANCESTRY_CODEC_LOADER_H
#define CANCESTRY_CODEC_LOADER_H

#include "cancestry/codec/types.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Capacity of cancestry_codec_load_error_t::message. */
#define CANCESTRY_CODEC_LOAD_ERROR_MESSAGE_MAX ((size_t)160u)

/**
 * Loader failure report.
 *
 * On failure @c status is negative, @c line and @c column are 1-based
 * positions into the source text (0 when not applicable), and @c message is a
 * NUL-terminated human-readable description.
 */
typedef struct cancestry_codec_load_error {
    cancestry_codec_status_t status;
    size_t line;
    size_t column;
    char message[CANCESTRY_CODEC_LOAD_ERROR_MESSAGE_MAX];
} cancestry_codec_load_error_t;

/**
 * Parse and validate a codec map YAML document and build the runtime
 * representation.
 *
 * The document must conform to the YAML subset and the codec-map-0.2.0 schema
 * (see core/codec/README.md for both). Validation failures return
 * CANCESTRY_CODEC_ERR_PARSE; duplicate message ids or duplicate signal names
 * return CANCESTRY_CODEC_ERR_CONFLICT.
 *
 * The returned map owns one heap block; release it with
 * cancestry_codec_map_free(). The input text is not referenced after this
 * call returns.
 *
 * @param text    NUL-free YAML document bytes.
 * @param length  Number of bytes in @p text.
 * @param error   Receives a failure report, or NULL.
 * @return The loaded map, or NULL on any error (with @p error filled in when
 *         provided).
 */
cancestry_codec_map_t *cancestry_codec_map_load(const char *text,
                                                size_t length,
                                                cancestry_codec_load_error_t *error);

/** Release a map returned by cancestry_codec_map_load(). NULL is a no-op. */
void cancestry_codec_map_free(cancestry_codec_map_t *map);

#ifdef __cplusplus
}
#endif

#endif /* CANCESTRY_CODEC_LOADER_H */
