/*
 * CANcestry - signal namespace implementation.
 *
 * Implementation notes:
 *   - The namespace is a bounded array of borrowed codec map pointers over
 *     caller-owned storage. No allocation, no global state.
 *   - Signal ids are the registration-order index: the first signal of the
 *     first registered map is 1, and so on. Resolution by id recomputes the
 *     order deterministically, so nothing extra needs to be stored
 *     (SYS-NF-001).
 *   - Canonical names are "<codec_map_name>.<signal_name>", split at the
 *     first dot. Short names resolve only while unambiguous across all
 *     registered maps (codec-map-spec.md section 2).
 *   - Registration rejects a duplicate codec map name and duplicate short
 *     signal names within one map (SW-FR-CODEC-007). Short names shared
 *     across different maps are allowed; resolving such a short name then
 *     fails with CANCESTRY_CODEC_ERR_AMBIGUOUS.
 */

#include "cancestry/codec/namespace.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

bool cancestry_codec_namespace_init(cancestry_codec_namespace_t *namespace,
                                    const cancestry_codec_map_t **slots,
                                    uint16_t capacity)
{
    if (namespace == NULL) {
        return false;
    }
    if (slots == NULL || capacity == 0u || capacity > CANCESTRY_CODEC_NAMESPACE_MAX_CAPACITY) {
        namespace->slots = NULL;
        namespace->capacity = 0u;
        namespace->count = 0u;
        return false;
    }
    namespace->slots = slots;
    namespace->capacity = capacity;
    namespace->count = 0u;
    return true;
}

bool cancestry_codec_namespace_is_valid(const cancestry_codec_namespace_t *namespace)
{
    return (namespace != NULL) && (namespace->slots != NULL) &&
           (namespace->capacity > 0u) &&
           (namespace->capacity <= CANCESTRY_CODEC_NAMESPACE_MAX_CAPACITY) &&
           (namespace->count <= namespace->capacity);
}

uint16_t cancestry_codec_namespace_size(const cancestry_codec_namespace_t *namespace)
{
    if (!cancestry_codec_namespace_is_valid(namespace)) {
        return 0u;
    }
    return namespace->count;
}

uint32_t cancestry_codec_namespace_signal_count(const cancestry_codec_namespace_t *namespace)
{
    uint32_t total = 0u;
    uint16_t i;

    if (!cancestry_codec_namespace_is_valid(namespace)) {
        return 0u;
    }
    for (i = 0u; i < namespace->count; ++i) {
        uint16_t m;
        for (m = 0u; m < namespace->slots[i]->message_count; ++m) {
            total += (uint32_t)namespace->slots[i]->messages[m].signal_count;
        }
    }
    return total;
}

static bool codec_string_equal(const char *lhs, const char *rhs)
{
    size_t i;

    if (lhs == NULL || rhs == NULL) {
        return false;
    }
    for (i = 0u; lhs[i] != '\0' && rhs[i] != '\0'; ++i) {
        if (lhs[i] != rhs[i]) {
            return false;
        }
    }
    return lhs[i] == '\0' && rhs[i] == '\0';
}

static bool map_has_duplicate_signal_names(const cancestry_codec_map_t *map)
{
    uint16_t m;

    for (m = 0u; m < map->message_count; ++m) {
        const cancestry_codec_message_t *message = &map->messages[m];
        uint16_t a;
        for (a = 0u; a < message->signal_count; ++a) {
            const char *name = message->signals[a].name;
            uint16_t mm;
            uint16_t b;

            /* Compare against every signal that follows in map order, across
             * message boundaries too: canonical names need map-wide
             * uniqueness. */
            for (mm = m; mm < map->message_count; ++mm) {
                const cancestry_codec_message_t *other = &map->messages[mm];
                uint16_t first = (mm == m) ? (uint16_t)(a + 1u) : 0u;
                for (b = first; b < other->signal_count; ++b) {
                    if (codec_string_equal(name, other->signals[b].name)) {
                        return true;
                    }
                }
            }
        }
    }
    return false;
}

cancestry_codec_status_t cancestry_codec_namespace_register(cancestry_codec_namespace_t *namespace,
                                                            const cancestry_codec_map_t *map)
{
    uint16_t i;

    if (namespace == NULL || map == NULL) {
        return CANCESTRY_CODEC_ERR_NULL;
    }
    if (!cancestry_codec_namespace_is_valid(namespace)) {
        return CANCESTRY_CODEC_ERR_NULL;
    }
    if (map->name == NULL || map->message_count == 0u || map->messages == NULL) {
        return CANCESTRY_CODEC_ERR_ARGUMENT;
    }
    for (i = 0u; i < namespace->count; ++i) {
        if (codec_string_equal(namespace->slots[i]->name, map->name)) {
            return CANCESTRY_CODEC_ERR_CONFLICT;
        }
    }
    if (map_has_duplicate_signal_names(map)) {
        return CANCESTRY_CODEC_ERR_CONFLICT;
    }
    if (namespace->count >= namespace->capacity) {
        return CANCESTRY_CODEC_ERR_CAPACITY;
    }
    namespace->slots[namespace->count] = map;
    namespace->count = (uint16_t)(namespace->count + 1u);
    return CANCESTRY_CODEC_OK;
}

/**
 * Find the signal that carries registration id @p target_id and fill @p out.
 * Returns true when found.
 */
static bool codec_namespace_signal_at(const cancestry_codec_namespace_t *namespace,
                                      cancestry_signal_id_t target_id,
                                      cancestry_codec_resolution_t *out)
{
    cancestry_signal_id_t current = 1u;
    uint16_t i;

    for (i = 0u; i < namespace->count; ++i) {
        const cancestry_codec_map_t *map = namespace->slots[i];
        uint16_t m;
        for (m = 0u; m < map->message_count; ++m) {
            const cancestry_codec_message_t *message = &map->messages[m];
            uint16_t s;
            for (s = 0u; s < message->signal_count; ++s) {
                if (current == target_id) {
                    out->signal_id = target_id;
                    out->map = map;
                    out->signal = &message->signals[s];
                    return true;
                }
                current++;
            }
        }
    }
    return false;
}

/**
 * Recompute the registration id of a signal known to be registered.
 * Returns 0 (CANCESTRY_ID_NONE) when the signal is not found.
 */
static cancestry_signal_id_t codec_namespace_id_of(const cancestry_codec_namespace_t *namespace,
                                                   const cancestry_codec_map_t *map,
                                                   const cancestry_codec_signal_t *signal)
{
    cancestry_signal_id_t current = 1u;
    uint16_t i;

    for (i = 0u; i < namespace->count; ++i) {
        const cancestry_codec_map_t *registered = namespace->slots[i];
        uint16_t m;
        if (registered != map) {
            /* Count the whole map's signals and move on. */
            for (m = 0u; m < registered->message_count; ++m) {
                current += (cancestry_signal_id_t)registered->messages[m].signal_count;
            }
            continue;
        }
        for (m = 0u; m < map->message_count; ++m) {
            const cancestry_codec_message_t *message = &map->messages[m];
            uint16_t s;
            for (s = 0u; s < message->signal_count; ++s) {
                if (&message->signals[s] == signal) {
                    return current;
                }
                current++;
            }
        }
    }
    return CANCESTRY_ID_NONE;
}

cancestry_codec_status_t cancestry_codec_namespace_resolve(const cancestry_codec_namespace_t *namespace,
                                                           const char *name,
                                                           cancestry_codec_resolution_t *out)
{
    size_t i;
    size_t dot = SIZE_MAX;
    uint16_t m;

    if (namespace == NULL || name == NULL || out == NULL) {
        return CANCESTRY_CODEC_ERR_NULL;
    }
    if (!cancestry_codec_namespace_is_valid(namespace) || name[0] == '\0') {
        return CANCESTRY_CODEC_ERR_ARGUMENT;
    }

    for (i = 0u; name[i] != '\0'; ++i) {
        if (name[i] == '.') {
            dot = i;
            break;
        }
    }

    if (dot != SIZE_MAX) {
        /* Canonical name: "<codec_map_name>.<signal_name>". */
        const cancestry_codec_map_t *map = NULL;
        const cancestry_codec_signal_t *signal;
        const char *signal_name = &name[dot + 1u];

        if (dot == 0u || signal_name[0] == '\0') {
            return CANCESTRY_CODEC_ERR_ARGUMENT;
        }
        for (m = 0u; m < namespace->count; ++m) {
            const cancestry_codec_map_t *candidate = namespace->slots[m];
            size_t k;
            for (k = 0u; candidate->name[k] != '\0' && k < dot; ++k) {
                if (candidate->name[k] != name[k]) {
                    break;
                }
            }
            if (candidate->name[k] == '\0' && k == dot) {
                map = candidate;
                break;
            }
        }
        if (map == NULL) {
            return CANCESTRY_CODEC_ERR_NOT_FOUND;
        }
        signal = cancestry_codec_map_find_signal(map, signal_name);
        if (signal == NULL) {
            return CANCESTRY_CODEC_ERR_NOT_FOUND;
        }
        out->map = map;
        out->signal = signal;
        out->signal_id = codec_namespace_id_of(namespace, map, signal);
        return CANCESTRY_CODEC_OK;
    }

    /* Short name: allowed only while unambiguous. */
    {
        const cancestry_codec_map_t *found_map = NULL;
        const cancestry_codec_signal_t *found_signal = NULL;
        uint32_t matches = 0u;

        for (m = 0u; m < namespace->count; ++m) {
            const cancestry_codec_map_t *map = namespace->slots[m];
            uint16_t mm;
            for (mm = 0u; mm < map->message_count; ++mm) {
                const cancestry_codec_message_t *message = &map->messages[mm];
                uint16_t s;
                for (s = 0u; s < message->signal_count; ++s) {
                    if (codec_string_equal(message->signals[s].name, name)) {
                        matches++;
                        found_map = map;
                        found_signal = &message->signals[s];
                    }
                }
            }
        }
        if (matches == 0u) {
            return CANCESTRY_CODEC_ERR_NOT_FOUND;
        }
        if (matches > 1u) {
            return CANCESTRY_CODEC_ERR_AMBIGUOUS;
        }
        out->map = found_map;
        out->signal = found_signal;
        out->signal_id = codec_namespace_id_of(namespace, found_map, found_signal);
        return CANCESTRY_CODEC_OK;
    }
}

cancestry_codec_status_t cancestry_codec_namespace_resolve_id(
    const cancestry_codec_namespace_t *namespace,
    cancestry_signal_id_t signal_id,
    cancestry_codec_resolution_t *out)
{
    if (namespace == NULL || out == NULL) {
        return CANCESTRY_CODEC_ERR_NULL;
    }
    if (!cancestry_codec_namespace_is_valid(namespace) || signal_id == CANCESTRY_ID_NONE) {
        return CANCESTRY_CODEC_ERR_ARGUMENT;
    }
    if (!codec_namespace_signal_at(namespace, signal_id, out)) {
        return CANCESTRY_CODEC_ERR_NOT_FOUND;
    }
    return CANCESTRY_CODEC_OK;
}
