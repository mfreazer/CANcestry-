/*
 * CANcestry - UDS configuration loader.
 *
 * Normative references:
 *   schemas/uds-0.1.0.schema.json     UDS configuration v0.1.0 schema
 *   docs/software/SwRS.md             SW-FR-UDS-006 (schema is law)
 *   docs/system/SyRS.md               SYS-NF-001 (determinism), SYS-NF-002 (bounded resources)
 *
 * The loader parses the YAML subset documented in core/uds/README.md (the
 * same subset as the codec, recipe and FSM loaders: block mappings and block
 * sequences only) and validates every constraint of the
 * uds-0.1.0 JSON Schema in C, so no external JSON Schema validator is needed
 * at load time. On top of the schema it enforces the structural rules the
 * runtime depends on: duplicate DIDs/routines, signal-mapped DID lengths of
 * at most 8 bytes, and default arrays whose length matches the DID length.
 *
 * Allocation strategy (SYS-NF-002):
 *   - Loading allocates: one arena for the parse tree (freed before the
 *     loader returns) and one block that holds the finished
 *     cancestry_uds_config_t and everything it points to.
 *   - The runtime library (cancestry_uds: services + types) never allocates.
 *     The loader is deliberately kept in a separate static library
 *     (cancestry_uds_loader) so CI can symbol-scan the runtime library for
 *     allocator references.
 */

#ifndef CANCESTRY_UDS_LOADER_H
#define CANCESTRY_UDS_LOADER_H

#include "cancestry/uds/types.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Capacity of cancestry_uds_load_error_t::message. */
#define CANCESTRY_UDS_LOAD_ERROR_MESSAGE_MAX ((size_t)192u)

/**
 * Loader failure report.
 *
 * On failure @c status is negative, @c line and @c column are 1-based
 * positions into the source text (0 when not applicable), and @c message is a
 * NUL-terminated human-readable description.
 */
typedef struct cancestry_uds_load_error {
    cancestry_uds_status_t status;
    size_t line;
    size_t column;
    char message[CANCESTRY_UDS_LOAD_ERROR_MESSAGE_MAX];
} cancestry_uds_load_error_t;

/**
 * Parse and validate a UDS configuration YAML document and build the runtime
 * representation (SW-FR-UDS-006).
 *
 * The document must conform to the YAML subset and the uds-0.1.0 schema (see
 * core/uds/README.md for both). Validation failures return
 * CANCESTRY_UDS_ERR_PARSE with a located message; duplicate identifiers
 * return CANCESTRY_UDS_ERR_CONFLICT.
 *
 * The returned config owns one heap block; release it with
 * cancestry_uds_config_free(). The input text is not referenced after this
 * call returns.
 *
 * @param text    NUL-free YAML document bytes.
 * @param length  Number of bytes in @p text.
 * @param error   Receives a failure report, or NULL.
 * @return The loaded config, or NULL on any error (with @p error filled in
 *         when provided).
 */
cancestry_uds_config_t *cancestry_uds_config_load(const char *text,
                                                  size_t length,
                                                  cancestry_uds_load_error_t *error);

/** Release a config returned by cancestry_uds_config_load(). NULL is a no-op. */
void cancestry_uds_config_free(cancestry_uds_config_t *config);

#ifdef __cplusplus
}
#endif

#endif /* CANCESTRY_UDS_LOADER_H */
