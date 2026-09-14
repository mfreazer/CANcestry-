/*
 * CANcestry - recipe file loader.
 *
 * Normative references:
 *   docs/packages/recipe-spec.md       (v0.2.1)
 *   schemas/recipe-0.2.0.schema.json   recipe file v0.2.0 schema
 *   docs/software/SwRS.md              SW-FR-RECIPE-006 (transition rejection)
 *
 * The loader parses the YAML subset documented in core/recipe/README.md (the
 * same subset as the codec map loader) and validates every constraint of the
 * recipe-0.2.0 JSON Schema in C, so no external JSON Schema validator is
 * needed at runtime. Recipes containing the forbidden transition action are
 * rejected with a dedicated error (SW-FR-RECIPE-006).
 *
 * Allocation strategy (SYS-NF-002):
 *   - Loading allocates: one arena for the parse tree (freed before the
 *     loader returns) and one block that holds the finished
 *     cancestry_recipe_set_t and everything it points to.
 *   - The runtime library (cancestry_recipe: the engine) never allocates.
 *     The loader is deliberately kept in a separate static library
 *     (cancestry_recipe_loader) so CI can symbol-scan the runtime library
 *     for allocator references.
 */

#ifndef CANCESTRY_RECIPE_LOADER_H
#define CANCESTRY_RECIPE_LOADER_H

#include "cancestry/recipe/types.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Capacity of cancestry_recipe_load_error_t::message. */
#define CANCESTRY_RECIPE_LOAD_ERROR_MESSAGE_MAX ((size_t)192u)

/**
 * Loader failure report.
 *
 * On failure @c status is negative, @c line and @c column are 1-based
 * positions into the source text (0 when not applicable), and @c message is a
 * NUL-terminated human-readable description.
 */
typedef struct cancestry_recipe_load_error {
    cancestry_recipe_status_t status;
    size_t line;
    size_t column;
    char message[CANCESTRY_RECIPE_LOAD_ERROR_MESSAGE_MAX];
} cancestry_recipe_load_error_t;

/**
 * Parse and validate a recipe file YAML document and build the runtime
 * representation.
 *
 * The document must conform to the YAML subset and the recipe-0.2.0 schema
 * (see core/recipe/README.md for both). Validation failures return
 * CANCESTRY_RECIPE_ERR_PARSE, including:
 *   - recipes that use the forbidden transition action (SW-FR-RECIPE-006),
 *   - triggers that use the disallowed "type" field (recipe-spec.md section 2),
 *   - missing conditional fields (signal for signal_changed, timer for
 *     timer_expired, timeout_ms for timeout).
 * Duplicate recipe names within one file return
 * CANCESTRY_RECIPE_ERR_CONFLICT.
 *
 * The returned set owns one heap block; release it with
 * cancestry_recipe_set_free(). The input text is not referenced after this
 * call returns.
 *
 * @param text    NUL-free YAML document bytes.
 * @param length  Number of bytes in @p text.
 * @param error   Receives a failure report, or NULL.
 * @return The loaded set, or NULL on any error (with @p error filled in when
 *         provided).
 */
cancestry_recipe_set_t *cancestry_recipe_set_load(const char *text,
                                                  size_t length,
                                                  cancestry_recipe_load_error_t *error);

/** Release a set returned by cancestry_recipe_set_load(). NULL is a no-op. */
void cancestry_recipe_set_free(cancestry_recipe_set_t *set);

#ifdef __cplusplus
}
#endif

#endif /* CANCESTRY_RECIPE_LOADER_H */
