/*
 * CANcestry - signal encoder implementation.
 *
 * Implementation notes:
 *   - Bit insertion mirrors extraction (codec-map-spec.md sections 4-5.1).
 *     Contiguous signals write raw bit i to payload bit first_bit + i.
 *     Sawtooth signals write via the Motorola sawtooth ordering: raw MSB
 *     goes to the first sawtooth payload position, raw LSB to the last,
 *     which is the exact inverse of decode for any endianness and width.
 *   - Encoding converts physical to raw with raw = round((physical -
 *     offset) / scale), rounding to nearest with ties away from zero
 *     (codec-map-spec.md section 7). The rounding helper uses truncation
 *     towards zero instead of libm floor/ceil: trunc(q + 0.5) for q >= 0 and
 *     trunc(q - 0.5) for q < 0 are exactly floor(q + 0.5) and ceil(q - 0.5),
 *     which rounds halves away from zero. This keeps the core free of libm.
 *   - All double-to-integer conversions are range-guarded so no conversion
 *     can invoke undefined behavior (C99 6.3.1.4).
 *   - No allocation, no global state. Same input -> same output (SYS-NF-001).
 */

#include "cancestry/codec/encoder.h"

#include "codec_bits.h"

#include <stddef.h>
#include <stdint.h>

/** 2^63 as a double; the guard bound for int64 conversion. */
#define CODEC_INT64_LIMIT_D (9223372036854775808.0)

/**
 * Round to nearest, ties away from zero, without libm.
 *
 * Returns @p q unchanged when it is outside the exact int64 range; the
 * caller's range checks reject such values.
 */
static double codec_round_half_away(double q)
{
    if (q >= CODEC_INT64_LIMIT_D) {
        return q;
    }
    if (q < -CODEC_INT64_LIMIT_D) {
        return q;
    }
    if (q >= 0.0) {
        return (double)(int64_t)(q + 0.5);
    }
    return (double)(int64_t)(q - 0.5);
}

/**
 * Convert a finite physical value to raw for uint/int signals, without
 * range-checking the result yet. Returns false when @p q is not finite.
 */
static bool codec_physical_to_raw(const cancestry_codec_signal_t *signal,
                                  double physical,
                                  double *raw_out)
{
    double q;

    if (physical != physical) { /* NaN */
        return false;
    }
    if (signal->scale == 1.0 && signal->offset == 0.0) {
        q = physical;
    } else {
        q = (physical - signal->offset) / signal->scale;
    }
    *raw_out = codec_round_half_away(q);
    return true;
}

/**
 * Apply the declared physical min/max policy. Returns OK when the value is
 * accepted, WARN_VALUE_CLAMPED when a non-strict clamp was applied,
 * ERR_RANGE when a strict signal receives an out-of-range value.
 */
static cancestry_codec_status_t codec_apply_limits(const cancestry_codec_signal_t *signal,
                                                   double *physical)
{
    double clamped = *physical;

    if (signal->has_min && clamped < signal->min) {
        if (signal->strict) {
            return CANCESTRY_CODEC_ERR_RANGE;
        }
        clamped = signal->min;
    }
    if (signal->has_max && clamped > signal->max) {
        if (signal->strict) {
            return CANCESTRY_CODEC_ERR_RANGE;
        }
        clamped = signal->max;
    }
    if (clamped != *physical) {
        *physical = clamped;
        return CANCESTRY_CODEC_WARN_VALUE_CLAMPED;
    }
    return CANCESTRY_CODEC_OK;
}

/** Encode a boolean: accepted kinds are BOOL and INT/UINT equal to 0 or 1. */
static cancestry_codec_status_t codec_encode_boolean(const cancestry_value_t *value,
                                                     uint64_t *raw_out)
{
    uint64_t bit;

    switch (value->kind) {
    case CANCESTRY_VALUE_KIND_BOOL:
        bit = value->value.boolean ? 1u : 0u;
        break;
    case CANCESTRY_VALUE_KIND_INT:
        bit = (uint64_t)value->value.integer;
        break;
    case CANCESTRY_VALUE_KIND_UINT:
        bit = value->value.unsigned_integer;
        break;
    default:
        return CANCESTRY_CODEC_ERR_ARGUMENT;
    }
    if (bit > 1u) {
        return CANCESTRY_CODEC_ERR_RANGE;
    }
    *raw_out = bit;
    return CANCESTRY_CODEC_OK;
}

/** Encode an enum: accepted kinds are INT and UINT holding the raw value. */
static cancestry_codec_status_t codec_encode_enum(const cancestry_codec_signal_t *signal,
                                                  const cancestry_value_t *value,
                                                  uint64_t *raw_out)
{
    uint64_t raw;
    uint64_t mask = codec_bits_mask(signal->bit_length);

    switch (value->kind) {
    case CANCESTRY_VALUE_KIND_INT:
        if (value->value.integer < 0) {
            return CANCESTRY_CODEC_ERR_RANGE;
        }
        raw = (uint64_t)value->value.integer;
        break;
    case CANCESTRY_VALUE_KIND_UINT:
        raw = value->value.unsigned_integer;
        break;
    default:
        return CANCESTRY_CODEC_ERR_ARGUMENT;
    }
    if (raw > mask) {
        return CANCESTRY_CODEC_ERR_RANGE;
    }
    *raw_out = raw;
    return CANCESTRY_CODEC_OK;
}

/** Encode a uint: UINT always; REAL only when the signal is scaled. */
static cancestry_codec_status_t codec_encode_uint(const cancestry_codec_signal_t *signal,
                                                  const cancestry_value_t *value,
                                                  uint64_t *raw_out)
{
    double physical;
    double raw_d;
    bool scaled = (signal->scale != 1.0 || signal->offset != 0.0);
    cancestry_codec_status_t status;
    uint64_t mask = codec_bits_mask(signal->bit_length);

    if (value->kind == CANCESTRY_VALUE_KIND_UINT && !scaled) {
        /*
         * Exact path: unscaled unsigned values are encoded as integers so
         * that the full 64-bit range round-trips (double cannot represent
         * values above 2^53 exactly). Only the optional min/max check goes
         * through double.
         */
        uint64_t raw = value->value.unsigned_integer;
        physical = (double)raw;
        status = codec_apply_limits(signal, &physical);
        if (status == CANCESTRY_CODEC_ERR_RANGE) {
            return status;
        }
        if (status == CANCESTRY_CODEC_WARN_VALUE_CLAMPED) {
            if (physical < 0.0) {
                return CANCESTRY_CODEC_ERR_RANGE;
            }
            if (signal->bit_length < 64u && physical > (double)mask) {
                return CANCESTRY_CODEC_ERR_RANGE;
            }
            if (signal->bit_length == 64u && physical >= 18446744073709551616.0) {
                return CANCESTRY_CODEC_ERR_RANGE;
            }
            *raw_out = (uint64_t)physical;
            return status;
        }
        if (signal->bit_length < 64u && raw > mask) {
            return CANCESTRY_CODEC_ERR_RANGE;
        }
        *raw_out = raw;
        return CANCESTRY_CODEC_OK;
    }

    if (value->kind == CANCESTRY_VALUE_KIND_UINT) {
        physical = (double)value->value.unsigned_integer;
    } else if (value->kind == CANCESTRY_VALUE_KIND_REAL && scaled) {
        physical = value->value.real;
    } else {
        return CANCESTRY_CODEC_ERR_ARGUMENT;
    }

    status = codec_apply_limits(signal, &physical);
    if (status == CANCESTRY_CODEC_ERR_RANGE) {
        return status;
    }
    if (!codec_physical_to_raw(signal, physical, &raw_d)) {
        return CANCESTRY_CODEC_ERR_ARGUMENT;
    }
    if (raw_d < 0.0) {
        return CANCESTRY_CODEC_ERR_RANGE;
    }
    if (signal->bit_length < 64u) {
        if (raw_d > (double)mask) {
            return CANCESTRY_CODEC_ERR_RANGE;
        }
    } else {
        /* 2^64 as a double; values at or above it cannot fit in uint64_t. */
        if (raw_d >= 18446744073709551616.0) {
            return CANCESTRY_CODEC_ERR_RANGE;
        }
    }
    *raw_out = (uint64_t)raw_d;
    return status;
}

/** Encode an int: INT always; REAL only when the signal is scaled. */
static cancestry_codec_status_t codec_encode_int(const cancestry_codec_signal_t *signal,
                                                 const cancestry_value_t *value,
                                                 uint64_t *raw_out)
{
    double physical;
    double raw_d;
    bool scaled = (signal->scale != 1.0 || signal->offset != 0.0);
    int64_t raw_signed;
    cancestry_codec_status_t status;
    uint64_t mask = codec_bits_mask(signal->bit_length);
    double min_d;
    double max_d;

    if (signal->bit_length == 64u) {
        min_d = -CODEC_INT64_LIMIT_D;
        max_d = CODEC_INT64_LIMIT_D - 1.0; /* = 2^63 - 1, inexact but only a guard */
    } else {
        uint32_t width = signal->bit_length;
        min_d = -((double)(UINT64_C(1) << (width - 1u)));
        max_d = (double)((UINT64_C(1) << (width - 1u)) - UINT64_C(1));
    }

    if (value->kind == CANCESTRY_VALUE_KIND_INT && !scaled) {
        /*
         * Exact path: unscaled signed values are encoded as integers so that
         * the full 64-bit range round-trips. Only the optional min/max check
         * goes through double.
         */
        int64_t raw = value->value.integer;
        physical = (double)raw;
        status = codec_apply_limits(signal, &physical);
        if (status == CANCESTRY_CODEC_ERR_RANGE) {
            return status;
        }
        if (status == CANCESTRY_CODEC_WARN_VALUE_CLAMPED) {
            if (physical < min_d || physical >= (signal->bit_length == 64u ? CODEC_INT64_LIMIT_D
                                                                           : max_d + 1.0)) {
                return CANCESTRY_CODEC_ERR_RANGE;
            }
            *raw_out = (uint64_t)(int64_t)physical & mask;
            return status;
        }
        if (signal->bit_length != 64u &&
            (raw < (int64_t)min_d || raw > (int64_t)max_d)) {
            return CANCESTRY_CODEC_ERR_RANGE;
        }
        *raw_out = (uint64_t)raw & mask;
        return CANCESTRY_CODEC_OK;
    }

    if (value->kind == CANCESTRY_VALUE_KIND_INT) {
        physical = (double)value->value.integer;
    } else if (value->kind == CANCESTRY_VALUE_KIND_REAL && scaled) {
        physical = value->value.real;
    } else {
        return CANCESTRY_CODEC_ERR_ARGUMENT;
    }

    status = codec_apply_limits(signal, &physical);
    if (status == CANCESTRY_CODEC_ERR_RANGE) {
        return status;
    }
    if (!codec_physical_to_raw(signal, physical, &raw_d)) {
        return CANCESTRY_CODEC_ERR_ARGUMENT;
    }

    if (signal->bit_length == 64u) {
        if (raw_d < min_d || raw_d >= CODEC_INT64_LIMIT_D) {
            return CANCESTRY_CODEC_ERR_RANGE;
        }
        raw_signed = (int64_t)raw_d;
    } else {
        if (raw_d < min_d || raw_d > max_d) {
            return CANCESTRY_CODEC_ERR_RANGE;
        }
        raw_signed = (int64_t)raw_d;
    }
    *raw_out = (uint64_t)raw_signed & mask;
    return status;
}

bool cancestry_codec_label_to_raw(const cancestry_codec_signal_t *signal,
                                  const char *label,
                                  uint64_t *raw_out)
{
    uint16_t i;
    size_t k;

    if (signal == NULL || label == NULL || raw_out == NULL || signal->values == NULL) {
        return false;
    }
    for (i = 0u; i < signal->value_count; ++i) {
        const char *candidate = signal->values[i].name;
        for (k = 0u; label[k] != '\0' && candidate[k] != '\0'; ++k) {
            if (label[k] != candidate[k]) {
                break;
            }
        }
        if (label[k] == '\0' && candidate[k] == '\0') {
            *raw_out = signal->values[i].raw;
            return true;
        }
    }
    return false;
}

cancestry_codec_status_t cancestry_codec_encode_signal(const cancestry_codec_signal_t *signal,
                                                       const cancestry_value_t *value,
                                                       uint8_t *frame,
                                                       size_t frame_length,
                                                       cancestry_codec_warnings_t *warnings)
{
    cancestry_codec_status_t status = CANCESTRY_CODEC_OK;
    uint64_t raw = 0u;

    if (signal == NULL || value == NULL || frame == NULL) {
        return CANCESTRY_CODEC_ERR_NULL;
    }
    if (!codec_frame_length_ok(frame_length)) {
        return CANCESTRY_CODEC_ERR_ARGUMENT;
    }
    if (signal->layout == CANCESTRY_CODEC_LAYOUT_SAWTOOTH) {
        if (!codec_bits_saw_fits(frame_length, signal->start_bit, signal->bit_length)) {
            return CANCESTRY_CODEC_ERR_FRAME_TOO_SHORT;
        }
    } else {
        if (signal->last_bit >= frame_length * 8u) {
            return CANCESTRY_CODEC_ERR_FRAME_TOO_SHORT;
        }
    }

    switch (signal->type) {
    case CANCESTRY_CODEC_SIGNAL_TYPE_BOOLEAN:
        status = codec_encode_boolean(value, &raw);
        break;
    case CANCESTRY_CODEC_SIGNAL_TYPE_ENUM:
        status = codec_encode_enum(signal, value, &raw);
        break;
    case CANCESTRY_CODEC_SIGNAL_TYPE_UINT:
        status = codec_encode_uint(signal, value, &raw);
        break;
    case CANCESTRY_CODEC_SIGNAL_TYPE_INT:
        status = codec_encode_int(signal, value, &raw);
        break;
    default:
        return CANCESTRY_CODEC_ERR_ARGUMENT;
    }
    if (status < 0) {
        return status;
    }

    if (signal->layout == CANCESTRY_CODEC_LAYOUT_SAWTOOTH) {
        codec_bits_store_saw(frame, frame_length, signal->start_bit, signal->bit_length, raw);
    } else {
        codec_bits_store(frame, frame_length, signal->first_bit, signal->bit_length, raw);
    }
    if (status == CANCESTRY_CODEC_WARN_VALUE_CLAMPED && warnings != NULL) {
        warnings->value_clamped++;
    }
    return status;
}
