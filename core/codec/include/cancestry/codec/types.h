/*
 * CANcestry - portable codec engine: shared types.
 *
 * Normative references:
 *   docs/packages/codec-map-spec.md        (v0.2.1) sections 2-8
 *   docs/software/SwRS.md                  SW-FR-CODEC-001 .. SW-FR-CODEC-008
 *   docs/system/SyRS.md                    SYS-NF-001 (determinism), SYS-NF-002 (bounded resources)
 *   schemas/codec-map-0.2.0.schema.json    codec map v0.2.0 schema
 *
 * Design notes:
 *   - A loaded codec map is a single, self-contained allocation. Every array
 *     and string pointer inside it refers into that allocation, so the runtime
 *     path (decode, encode, namespace) never allocates: it only reads.
 *   - Strings are borrowed pointers with the lifetime of the codec map.
 *   - Bit semantics follow the canonical LSB0 model of codec-map-spec.md
 *     section 3: data[0] bit 0 is global bit 0, data[1] bit 0 is global bit 8.
 *   - decode/encode are pure functions of their inputs (SYS-NF-001); see
 *     core/codec/README.md for the documented floating-point rules.
 */

#ifndef CANCESTRY_CODEC_TYPES_H
#define CANCESTRY_CODEC_TYPES_H

#include "cancestry/event/types.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Classic CAN payload width in bits; the codec never sees frames wider than 8 bytes. */
#define CANCESTRY_CODEC_PAYLOAD_MAX_BITS ((uint32_t)64u)

/** Maximum length of codec map, message and signal names accepted by the loader. */
#define CANCESTRY_CODEC_NAME_MAX ((size_t)64u)

/** Maximum length of a unit string accepted by the loader. */
#define CANCESTRY_CODEC_UNIT_MAX ((size_t)32u)

/** Maximum length of a value-mapping label accepted by the loader. */
#define CANCESTRY_CODEC_LABEL_MAX ((size_t)64u)

/**
 * Operation results. Values >= 0 mean the operation succeeded; positive
 * values carry a warning (see cancestry_codec_status_is_warning()).
 */
typedef enum cancestry_codec_status {
    /** Success. */
    CANCESTRY_CODEC_OK = 0,
    /** Decode: frame too short for a declared signal; message dropped, nothing written. */
    CANCESTRY_CODEC_WARN_FRAME_TOO_SHORT = 1,
    /** Encode: value was outside declared min/max and the signal is non-strict; clamped. */
    CANCESTRY_CODEC_WARN_VALUE_CLAMPED = 2,
    /** Decode: enum raw value has no label in the value mapping. */
    CANCESTRY_CODEC_WARN_ENUM_UNKNOWN = 3,
    /** A required pointer argument was NULL. */
    CANCESTRY_CODEC_ERR_NULL = -1,
    /** An argument is malformed or has the wrong value kind. */
    CANCESTRY_CODEC_ERR_ARGUMENT = -2,
    /** No message, signal, codec map, or label matches the request. */
    CANCESTRY_CODEC_ERR_NOT_FOUND = -3,
    /** A short signal name resolves to signals from more than one codec map. */
    CANCESTRY_CODEC_ERR_AMBIGUOUS = -4,
    /** Two definitions conflict: duplicate map name, message id, or signal name. */
    CANCESTRY_CODEC_ERR_CONFLICT = -5,
    /** The value cannot be represented in the signal's declared range. */
    CANCESTRY_CODEC_ERR_RANGE = -6,
    /** Encode: destination frame is too short for the signal's bit span. */
    CANCESTRY_CODEC_ERR_FRAME_TOO_SHORT = -7,
    /** Loader: YAML syntax error or schema violation. */
    CANCESTRY_CODEC_ERR_PARSE = -8,
    /** Loader: out of memory. */
    CANCESTRY_CODEC_ERR_NO_MEMORY = -9,
    /** Namespace: no free registration slots. */
    CANCESTRY_CODEC_ERR_CAPACITY = -10
} cancestry_codec_status_t;

/** Signal value types; mirrors the codec map schema "type" field. */
typedef enum cancestry_codec_signal_type {
    CANCESTRY_CODEC_SIGNAL_TYPE_UINT = 0,
    CANCESTRY_CODEC_SIGNAL_TYPE_INT = 1,
    CANCESTRY_CODEC_SIGNAL_TYPE_BOOLEAN = 2,
    CANCESTRY_CODEC_SIGNAL_TYPE_ENUM = 3,
    CANCESTRY_CODEC_SIGNAL_TYPE_COUNT
} cancestry_codec_signal_type_t;

/** Signal endianness; mirrors the codec map schema "endianness" field. */
typedef enum cancestry_codec_endianness {
    CANCESTRY_CODEC_ENDIANNESS_LITTLE = 0,
    CANCESTRY_CODEC_ENDIANNESS_BIG = 1
} cancestry_codec_endianness_t;

/** Signal bit layout; mirrors the codec map schema "layout" field. */
typedef enum cancestry_codec_layout {
    /** Contiguous linear LSB0 run (codec-map-spec.md sections 4-5). */
    CANCESTRY_CODEC_LAYOUT_CONTIGUOUS = 0,
    /** Sawtooth Motorola layout for multi-byte big-endian signals (spec section 5.1). */
    CANCESTRY_CODEC_LAYOUT_SAWTOOTH = 1,
    CANCESTRY_CODEC_LAYOUT_COUNT
} cancestry_codec_layout_t;

/** One raw-value to label mapping (codec map "values" object entry). */
typedef struct cancestry_codec_value_mapping {
    /** Raw value the label is attached to. */
    uint64_t raw;
    /** Label text; borrowed from the codec map allocation. Never NULL. */
    const char *name;
} cancestry_codec_value_mapping_t;

/**
 * A decoded signal definition inside a codec map.
 *
 * Instances are owned by their codec map and remain valid until the map is
 * freed. All pointers are borrowed from the map allocation.
 */
typedef struct cancestry_codec_signal {
    /** Short signal name, unique within its codec map. Never NULL. */
    const char *name;
    /**
     * Start bit in the canonical LSB0 model. For little-endian signals this is
     * the LSB position; for big-endian signals this is the MSB position
     * (codec-map-spec.md sections 4 and 5).
     */
    uint32_t start_bit;
    /** Width in bits, in [1, 64]. */
    uint32_t bit_length;
    cancestry_codec_signal_type_t type;
    cancestry_codec_endianness_t endianness;
    /** Scaling factor; 1.0 when not declared. Ignored for boolean/enum. */
    double scale;
    /** Offset; 0.0 when not declared. Ignored for boolean/enum. */
    double offset;
    /** Physical unit, or NULL when not declared. */
    const char *unit;
    /** Value mapping entries, or NULL for uint/int signals without one. */
    const cancestry_codec_value_mapping_t *values;
    /** Number of entries in @c values. */
    uint16_t value_count;
    bool has_min;
    bool has_max;
    /** Declared physical range; meaningful only when the matching has_* is true. */
    double min;
    double max;
    /**
     * Range policy. Defaults to true. When true, encoding a physical value
     * outside declared min/max fails with CANCESTRY_CODEC_ERR_RANGE; when
     * false the value is clamped and a warning is counted.
     */
    bool strict;
    /** Derived: lowest payload bit the signal occupies (LSB position). */
    uint32_t first_bit;
    /** Derived: highest payload bit the signal occupies. */
    uint32_t last_bit;
    /** Derived: id of the message this signal belongs to. */
    uint32_t message_id;
    /** Bit layout. Defaults to contiguous. Sawtooth is the Motorola sawtooth layout. */
    cancestry_codec_layout_t layout;
} cancestry_codec_signal_t;

/** A CAN message definition inside a codec map. */
typedef struct cancestry_codec_message {
    /** 11- or 29-bit CAN id, in [0, 536870911]. */
    uint32_t id;
    /** Message name; borrowed from the map allocation. Never NULL. */
    const char *name;
    /** Declared data length, in [0, 8]. */
    uint8_t dlc;
    /** Declared period in milliseconds, or 0 when not declared. */
    uint32_t period_ms;
    /** Description, or NULL when not declared. */
    const char *description;
    /** Signal definitions, in map order. */
    const cancestry_codec_signal_t *signals;
    /** Number of entries in @c signals; at least 1. */
    uint16_t signal_count;
} cancestry_codec_message_t;

/**
 * A loaded codec map.
 *
 * Instances are produced by cancestry_codec_map_load() (which allocates one
 * block) and released by cancestry_codec_map_free(). The struct itself and
 * everything it points to live inside that single block.
 */
typedef struct cancestry_codec_map {
    /** Codec map name; unique within a signal namespace. Never NULL. */
    const char *name;
    /** Semantic version string, e.g. "1.2.3". Never NULL. */
    const char *version;
    /** Description, or NULL when not declared. */
    const char *description;
    /** Message definitions, in map order. */
    const cancestry_codec_message_t *messages;
    /** Number of entries in @c messages; at least 1. */
    uint16_t message_count;
} cancestry_codec_map_t;

/**
 * Codec warning counters.
 *
 * Counters are caller-owned, monotonic across calls, and never reset by the
 * codec. May be NULL for any operation.
 */
typedef struct cancestry_codec_warnings {
    /** Decodes dropped because the frame was too short (SW-FR-CODEC-008). */
    uint32_t frame_too_short;
    /** Encodes clamped to declared min/max on non-strict signals. */
    uint32_t value_clamped;
    /** Enum decodes whose raw value had no label. */
    uint32_t enum_unknown;
} cancestry_codec_warnings_t;

/**
 * One decoded signal.
 *
 * decode writes these into caller-owned storage; nothing here owns memory.
 */
typedef struct cancestry_decoded_signal {
    /** Signal definition; borrowed from the codec map. */
    const cancestry_codec_signal_t *signal;
    /**
     * Raw bit pattern zero-extended to 64 bits (for signed signals the signed
     * value is in @c value). Round-tripping through encode reproduces the
     * exact same bits.
     */
    uint64_t raw;
    /** Physical value. Kind rules are documented in core/codec/README.md. */
    cancestry_value_t value;
    /** Label from the value mapping for boolean/enum, or NULL when none exists. */
    const char *label;
} cancestry_decoded_signal_t;

/** @return true when @p status is a warning (positive), false otherwise. */
bool cancestry_codec_status_is_warning(cancestry_codec_status_t status);

/** @return Stable, statically allocated name for @p status. Never NULL. */
const char *cancestry_codec_status_name(cancestry_codec_status_t status);

/** @return Stable, statically allocated name for @p type. Never NULL. */
const char *cancestry_codec_signal_type_name(cancestry_codec_signal_type_t type);

/** @return Stable, statically allocated name for @p endianness. Never NULL. */
const char *cancestry_codec_endianness_name(cancestry_codec_endianness_t endianness);

/** @return Stable, statically allocated name for @p layout. Never NULL. */
const char *cancestry_codec_layout_name(cancestry_codec_layout_t layout);

/**
 * Locate the message with the given CAN id.
 *
 * Message ids are unique within a codec map (enforced by the loader), so the
 * result is deterministic. O(number of messages).
 *
 * @return The message, or NULL when @p map is NULL or no message matches.
 */
const cancestry_codec_message_t *cancestry_codec_map_find_message(const cancestry_codec_map_t *map,
                                                                  uint32_t can_id);

/**
 * Locate a signal by its short name within a codec map.
 *
 * Short signal names are unique within a codec map (enforced by the loader).
 *
 * @return The signal, or NULL when @p map or @p name is NULL or no signal matches.
 */
const cancestry_codec_signal_t *cancestry_codec_map_find_signal(const cancestry_codec_map_t *map,
                                                                const char *name);

#ifdef __cplusplus
}
#endif

#endif /* CANCESTRY_CODEC_TYPES_H */
