/*
 * CANcestry - FSM file loader.
 *
 * Normative references:
 *   docs/packages/fsm-spec.md          (v0.2.1)
 *   schemas/fsm-0.2.0.schema.json      FSM file v0.2.0 schema (the schema is law)
 *   docs/software/SwRS.md              SW-FR-FSM-001 (load), -002 (compile),
 *                                      -003 (reject invalid)
 *   docs/software/SwAD.md              section 10 (definition errors reject the file)
 *
 * The loader parses the YAML subset documented in core/fsm/README.md (the same
 * subset the codec and recipe loaders accept) and validates every constraint of
 * the fsm-0.2.0 JSON Schema in C, so no external JSON Schema validator runs at
 * load time (SW-FR-FSM-003).
 *
 * Compiling (SW-FR-FSM-002) happens in the same pass: every machine reference,
 * initial state, transition target, timer name, signal name and variable name is
 * resolved to an index or validated against the declaration set, and every guard
 * and expression operand is grammar-checked. Anything unresolved is a load-time
 * error; the runtime never looks up a name to find a state.
 *
 * Allocation strategy (SYS-NF-002):
 *   - Loading allocates: one arena for the parse tree (freed before the loader
 *     returns) and one block that holds the finished cancestry_fsm_set_t and
 *     everything it points to.
 *   - The runtime library (cancestry_fsm) never allocates. The loader is a
 *     separate static library (cancestry_fsm_loader) so CI can symbol-scan the
 *     runtime archive for allocator references.
 */

#ifndef CANCESTRY_FSM_LOADER_H
#define CANCESTRY_FSM_LOADER_H

#include "cancestry/fsm/types.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Capacity of cancestry_fsm_load_error_t::message. */
#define CANCESTRY_FSM_LOAD_ERROR_MESSAGE_MAX ((size_t)192u)

/**
 * Loader failure report.
 *
 * On failure @c status is negative, @c line and @c column are 1-based positions
 * into the source text (0 when not applicable), and @c message is a NUL-
 * terminated human-readable description.
 */
typedef struct cancestry_fsm_load_error {
    cancestry_fsm_status_t status;
    size_t line;
    size_t column;
    char message[CANCESTRY_FSM_LOAD_ERROR_MESSAGE_MAX];
} cancestry_fsm_load_error_t;

/**
 * Parse, validate and compile one FSM file.
 *
 * The document must conform to the YAML subset and the fsm-0.2.0 schema.
 * Validation failures return CANCESTRY_FSM_ERR_PARSE, including:
 *   - a wrong or missing schema_version,
 *   - unknown keys anywhere (the schema forbids additionalProperties),
 *   - a state name used twice inside one machine (SW-FR-FSM-006),
 *   - an initial state or transition target naming an undeclared state
 *     (SW-FR-FSM-005),
 *   - a transition without its conditional field (signal for signal_changed,
 *     timer for timer_expired),
 *   - a machine-level name (state, timer, variable) used twice,
 *   - a guard or operand expression whose grammar is invalid.
 * Two instances with the same id, or two machines with the same name, return
 * CANCESTRY_FSM_ERR_CONFLICT.
 *
 * @param text    YAML document bytes, without NUL characters.
 * @param length  Number of bytes in @p text.
 * @param error   Receives a failure report; may be NULL.
 * @return The loaded set, or NULL on any error. The returned set owns one heap
 *         block; release it with cancestry_fsm_set_free(). The input text is
 *         not referenced after this call returns.
 */
cancestry_fsm_set_t *cancestry_fsm_set_load(const char *text,
                                           size_t length,
                                           cancestry_fsm_load_error_t *error);

/** Release a set returned by cancestry_fsm_set_load(). NULL is a no-op. */
void cancestry_fsm_set_free(cancestry_fsm_set_t *set);

/*
 * Multiple FSM files are loaded as multiple sets, and the engine is
 * initialized over the array of sets in load order. There is deliberately no
 * "append to a loaded set" call: a loaded set is one immutable heap block, and
 * growing it would need a realloc that invalidates every pointer inside it.
 * Instance ids must be unique across the array (the engine rejects
 * duplicates), and so must machine names, because instances reference machines
 * by name.
 */

#ifdef __cplusplus
}
#endif

#endif /* CANCESTRY_FSM_LOADER_H */
