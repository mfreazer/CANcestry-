/*
 * CANcestry - signal decoder implementation.
 *
 * Implementation notes:
 *   - Bit extraction follows the canonical LSB0 model (codec-map-spec.md
 *     sections 3-5). Contiguous signals (layout=contiguous, the default)
 *     occupy a contiguous run [first_bit, last_bit]: little-endian
 *     first_bit = start_bit, big-endian first_bit = start_bit-(n-1). Sawtooth
 *     signals (layout=sawtooth, spec section 5.1) use the DBC Motorola
 *     sawtooth ordering: payload bits are BE_BITS[be_idx(start_bit) ..
 *     be_idx(start_bit)+n-1] with the first element carrying the most
 *     significant raw bit, mirroring opendbc's get_raw_value. Both layouts
 *     share the same scaling/sign/value-mapping path.
 *   - Signed signals are two's complement, sign-extended to 64 bits
 *     (codec-map-spec.md section 6).
 *   - No allocation, no global state, no recursion. Decoding the same frame
 *     twice produces bit-identical output (SYS-NF-001).
 */

#include "cancestry/codec/decoder.h"

#include "codec_bits.h"

#include <stddef.h>
#include <stdint.h>

const cancestry_codec_message_t *cancestry_codec_map_find_message(const cancestry_codec_map_t *map,
                                                                  uint32_t can_id)
{
    uint16_t i;

    if (map == NULL) {
        return NULL;
    }
    for (i = 0u; i < map->message_count; ++i) {
        if (map->messages[i].id == can_id) {
            return &map->messages[i];
        }
    }
    return NULL;
}

const cancestry_codec_signal_t *cancestry_codec_map_find_signal(const cancestry_codec_map_t *map,
                                                                const char *name)
{
    uint16_t m;
    size_t n;

    if (map == NULL || name == NULL) {
        return NULL;
    }
    for (m = 0u; m < map->message_count; ++m) {
        const cancestry_codec_message_t *message = &map->messages[m];
        for (n = 0u; n < message->signal_count; ++n) {
            size_t k;
            const cancestry_codec_signal_t *signal = &message->signals[n];
            for (k = 0u; name[k] != '\0' && signal->name[k] != '\0'; ++k) {
                if (name[k] != signal->name[k]) {
                    break;
                }
            }
            if (name[k] == '\0' && signal->name[k] == '\0') {
                return signal;
            }
        }
    }
    return NULL;
}

/* ------------------------------------------------------------------------- */
/* Per-signal primitive                                                      */
/* ------------------------------------------------------------------------- */

static cancestry_codec_status_t decode_raw_into(const cancestry_codec_signal_t *signal,
                                                uint64_t raw,
                                                cancestry_decoded_signal_t *out)
{
    out->signal = signal;
    out->raw = raw;
    out->label = NULL;

    switch (signal->type) {
    case CANCESTRY_CODEC_SIGNAL_TYPE_UINT:
        if (signal->scale == 1.0 && signal->offset == 0.0) {
            out->value.kind = CANCESTRY_VALUE_KIND_UINT;
            out->value.value.unsigned_integer = raw;
        } else {
            out->value.kind = CANCESTRY_VALUE_KIND_REAL;
            out->value.value.real = ((double)raw * signal->scale) + signal->offset;
        }
        break;
    case CANCESTRY_CODEC_SIGNAL_TYPE_INT: {
        int64_t signed_raw = codec_bits_sign_extend(raw, signal->bit_length);
        if (signal->scale == 1.0 && signal->offset == 0.0) {
            out->value.kind = CANCESTRY_VALUE_KIND_INT;
            out->value.value.integer = signed_raw;
        } else {
            out->value.kind = CANCESTRY_VALUE_KIND_REAL;
            out->value.value.real = ((double)signed_raw * signal->scale) + signal->offset;
        }
        break;
    }
    case CANCESTRY_CODEC_SIGNAL_TYPE_BOOLEAN:
        out->value.kind = CANCESTRY_VALUE_KIND_BOOL;
        out->value.value.boolean = (raw & 1u) != 0u;
        break;
    case CANCESTRY_CODEC_SIGNAL_TYPE_ENUM:
        out->value.kind = CANCESTRY_VALUE_KIND_INT;
        out->value.value.integer = (int64_t)raw;
        break;
    default:
        /* The loader only produces the four schema types. */
        out->value.kind = CANCESTRY_VALUE_KIND_UNSET;
        return CANCESTRY_CODEC_ERR_ARGUMENT;
    }

    if (signal->values != NULL) {
        uint16_t i;
        for (i = 0u; i < signal->value_count; ++i) {
            if (signal->values[i].raw == raw) {
                out->label = signal->values[i].name;
                break;
            }
        }
    }
    return CANCESTRY_CODEC_OK;
}

cancestry_codec_status_t cancestry_codec_decode_signal(const cancestry_codec_signal_t *signal,
                                                       const uint8_t *frame,
                                                       size_t frame_length,
                                                       cancestry_decoded_signal_t *out)
{
    uint64_t raw;
    cancestry_codec_status_t status;

    if (signal == NULL || frame == NULL || out == NULL) {
        return CANCESTRY_CODEC_ERR_NULL;
    }
    if (frame_length == 0u || frame_length > CANCESTRY_CAN_FRAME_MAX_LENGTH) {
        return CANCESTRY_CODEC_ERR_ARGUMENT;
    }
    if (signal->layout == CANCESTRY_CODEC_LAYOUT_SAWTOOTH) {
        if (!codec_bits_saw_fits(frame_length, signal->start_bit, signal->bit_length)) {
            return CANCESTRY_CODEC_ERR_FRAME_TOO_SHORT;
        }
        raw = codec_bits_load_saw(frame, frame_length, signal->start_bit, signal->bit_length);
    } else {
        if (signal->last_bit >= frame_length * 8u) {
            return CANCESTRY_CODEC_ERR_FRAME_TOO_SHORT;
        }
        raw = codec_bits_load(frame, frame_length, signal->first_bit, signal->bit_length);
    }
    status = decode_raw_into(signal, raw, out);
    return status;
}

/* ------------------------------------------------------------------------- */
/* Frame-level decode                                                        */
/* ------------------------------------------------------------------------- */

cancestry_codec_status_t cancestry_codec_decode_frame(const cancestry_codec_map_t *map,
                                                      uint32_t can_id,
                                                      const uint8_t *frame,
                                                      size_t frame_length,
                                                      cancestry_decoded_signal_t *signals,
                                                      size_t capacity,
                                                      size_t *count,
                                                      cancestry_codec_warnings_t *warnings)
{
    const cancestry_codec_message_t *message;
    uint16_t i;

    if (map == NULL || frame == NULL || count == NULL) {
        return CANCESTRY_CODEC_ERR_NULL;
    }
    if (signals == NULL || capacity == 0u) {
        return CANCESTRY_CODEC_ERR_ARGUMENT;
    }
    if (frame_length == 0u || frame_length > CANCESTRY_CAN_FRAME_MAX_LENGTH) {
        return CANCESTRY_CODEC_ERR_ARGUMENT;
    }

    message = cancestry_codec_map_find_message(map, can_id);
    if (message == NULL) {
        return CANCESTRY_CODEC_ERR_NOT_FOUND;
    }
    if ((size_t)message->signal_count > capacity) {
        return CANCESTRY_CODEC_ERR_ARGUMENT;
    }

    /*
     * SW-FR-CODEC-008 / codec-map-spec.md section 8: when the frame is too
     * short for any declared signal the whole message is dropped and nothing
     * is updated.
     */
    for (i = 0u; i < message->signal_count; ++i) {
        const cancestry_codec_signal_t *signal = &message->signals[i];
        bool too_short = false;
        if (signal->layout == CANCESTRY_CODEC_LAYOUT_SAWTOOTH) {
            too_short = !codec_bits_saw_fits(frame_length, signal->start_bit, signal->bit_length);
        } else {
            too_short = signal->last_bit >= frame_length * 8u;
        }
        if (too_short) {
            if (warnings != NULL) {
                warnings->frame_too_short++;
            }
            return CANCESTRY_CODEC_WARN_FRAME_TOO_SHORT;
        }
    }

    for (i = 0u; i < message->signal_count; ++i) {
        const cancestry_codec_signal_t *signal = &message->signals[i];
        uint64_t raw;
        if (signal->layout == CANCESTRY_CODEC_LAYOUT_SAWTOOTH) {
            raw = codec_bits_load_saw(frame, frame_length, signal->start_bit, signal->bit_length);
        } else {
            raw = codec_bits_load(frame, frame_length, signal->first_bit, signal->bit_length);
        }

        (void)decode_raw_into(signal, raw, &signals[i]);
        if (signal->type == CANCESTRY_CODEC_SIGNAL_TYPE_ENUM && signals[i].label == NULL &&
            warnings != NULL) {
            warnings->enum_unknown++;
        }
    }
    *count = (size_t)message->signal_count;
    return CANCESTRY_CODEC_OK;
}
