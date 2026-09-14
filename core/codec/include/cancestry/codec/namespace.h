/*
 * CANcestry - signal namespace.
 *
 * Normative references:
 *   docs/packages/codec-map-spec.md  section 2 (canonical signal names)
 *   docs/software/SwRS.md            SW-FR-CODEC-007, SW-FR-PKG-007
 *   docs/system/SyRS.md              SYS-FR-010, SYS-NF-002 (bounded resources)
 *
 * Design notes:
 *   - The namespace is a bounded registry of codec maps over caller-owned
 *     storage; it never allocates and has no global state.
 *   - Canonical signal names are "<codec_map_name>.<signal_name>". Short
 *     names resolve only while unambiguous; as soon as two registered codec
 *     maps define the same short signal name, resolving that short name
 *     fails with CANCESTRY_CODEC_ERR_AMBIGUOUS and the canonical name must
 *     be used (codec-map-spec.md section 2).
 *   - Registration order is the identity order: signals receive stable ids
 *     1, 2, 3, ... in registration order. Registering the same set of maps in
 *     the same order always yields the same ids (SYS-NF-001).
 *   - Registration rejects conflicting definitions (duplicate codec map
 *     name, duplicate short signal name inside one map) with
 *     CANCESTRY_CODEC_ERR_CONFLICT.
 */

#ifndef CANCESTRY_CODEC_NAMESPACE_H
#define CANCESTRY_CODEC_NAMESPACE_H

#include "cancestry/codec/types.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Largest number of codec maps a namespace can hold (bounded resource usage). */
#define CANCESTRY_CODEC_NAMESPACE_MAX_CAPACITY ((uint16_t)256u)

/**
 * Bounded signal namespace over caller-owned storage.
 *
 * Instances may live in static storage, on the stack, or in a caller-managed
 * arena. The namespace never allocates.
 */
typedef struct cancestry_codec_namespace {
    /** Caller-owned array of @c capacity codec map pointers. */
    const cancestry_codec_map_t **slots;
    /** Number of codec map pointers the storage can hold. */
    uint16_t capacity;
    /** Number of registered codec maps. */
    uint16_t count;
} cancestry_codec_namespace_t;

/** Result of resolving a signal name or id. */
typedef struct cancestry_codec_resolution {
    /** Stable signal id assigned at registration time. */
    cancestry_signal_id_t signal_id;
    /** Owning codec map (registered; caller must keep it alive). */
    const cancestry_codec_map_t *map;
    /** Resolved signal definition (borrowed). */
    const cancestry_codec_signal_t *signal;
} cancestry_codec_resolution_t;

/**
 * Initialize a namespace over caller-owned storage.
 *
 * @param namespace  Namespace to initialize; ignored when NULL.
 * @param slots      Array of codec map pointers backing the namespace. It
 *                   shall outlive the namespace and shall not move.
 * @param capacity   Number of pointers in @p slots, in
 *                   [1, CANCESTRY_CODEC_NAMESPACE_MAX_CAPACITY].
 * @return true when the namespace is ready for use.
 */
bool cancestry_codec_namespace_init(cancestry_codec_namespace_t *namespace,
                                    const cancestry_codec_map_t **slots,
                                    uint16_t capacity);

/** @return true when @p namespace is non-NULL and initialized. */
bool cancestry_codec_namespace_is_valid(const cancestry_codec_namespace_t *namespace);

/** @return Number of registered codec maps, or 0 when invalid. */
uint16_t cancestry_codec_namespace_size(const cancestry_codec_namespace_t *namespace);

/** @return Total number of signals in all registered maps, or 0 when invalid. */
uint32_t cancestry_codec_namespace_signal_count(const cancestry_codec_namespace_t *namespace);

/**
 * Register a codec map.
 *
 * @param namespace  Namespace to register into.
 * @param map        Codec map to register. The namespace borrows it; the
 *                   caller keeps it alive and unmodified.
 * @return CANCESTRY_CODEC_OK, CANCESTRY_CODEC_ERR_NULL for NULL arguments,
 *         CANCESTRY_CODEC_ERR_CAPACITY when the namespace is full,
 *         CANCESTRY_CODEC_ERR_CONFLICT when the map name is already
 *         registered or the map defines a short signal name twice.
 */
cancestry_codec_status_t cancestry_codec_namespace_register(cancestry_codec_namespace_t *namespace,
                                                            const cancestry_codec_map_t *map);

/**
 * Resolve a signal name.
 *
 * Names containing '.' are canonical names ("<codec_map_name>.<signal_name>",
 * split at the first dot). Names without '.' are short names, allowed only
 * while unambiguous.
 *
 * @param namespace  Namespace to search.
 * @param name       Name to resolve.
 * @param out        Receives the resolution.
 * @return CANCESTRY_CODEC_OK, CANCESTRY_CODEC_ERR_NOT_FOUND,
 *         CANCESTRY_CODEC_ERR_AMBIGUOUS when a short name matches signals
 *         from more than one codec map, or a negative status for malformed
 *         arguments.
 */
cancestry_codec_status_t cancestry_codec_namespace_resolve(const cancestry_codec_namespace_t *namespace,
                                                           const char *name,
                                                           cancestry_codec_resolution_t *out);

/**
 * Resolve a stable signal id assigned at registration time.
 *
 * @param namespace  Namespace to search.
 * @param signal_id  Id to resolve; CANCESTRY_ID_NONE (0) is invalid.
 * @param out        Receives the resolution.
 * @return CANCESTRY_CODEC_OK, CANCESTRY_CODEC_ERR_NOT_FOUND, or a negative
 *         status for malformed arguments.
 */
cancestry_codec_status_t cancestry_codec_namespace_resolve_id(
    const cancestry_codec_namespace_t *namespace,
    cancestry_signal_id_t signal_id,
    cancestry_codec_resolution_t *out);

#ifdef __cplusplus
}
#endif

#endif /* CANCESTRY_CODEC_NAMESPACE_H */
