/*
 * CANcestry - internal LSB0 bit access helpers shared by decoder and encoder.
 *
 * The canonical payload bit array is linear LSB0 (codec-map-spec.md section
 * 3): payload bit g lives in frame[g / 8] at bit position g % 8, where bit
 * position 0 is the least significant bit of the byte.
 *
 * These helpers are private to core/codec/src and must stay allocation-free.
 *
 * Sawtooth helpers (codec-map-spec.md section 5.1) implement the DBC Motorola
 * big-endian sawtooth layout via the BE_BITS ordering:
 *   be_bits[idx] = (idx>>3)*8 + (7 - (idx&7))
 *   be_idx(p)    = (p>>3)*8 + (7 - (p&7))
 * A signal's sawtooth payload bits are be_bits[be_idx(start_bit) .. be_idx(start_bit)+count-1],
 * with the first element carrying the most significant raw bit.
 */

#ifndef CANCESTRY_CODEC_BITS_H
#define CANCESTRY_CODEC_BITS_H

#include "cancestry/codec/types.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/**
 * Read @p count payload bits starting at global payload bit @p first_bit,
 * least significant bit first, zero-extended to 64 bits.
 *
 * @p count may be 64. The caller guarantees every accessed bit lies within
 * @p length bytes.
 */
static inline uint64_t codec_bits_load(const uint8_t *frame,
                                       size_t length,
                                       uint32_t first_bit,
                                       uint32_t count)
{
    uint64_t raw = 0u;
    uint32_t i;

    (void)length;
    for (i = 0u; i < count; ++i) {
        uint32_t global = first_bit + i;
        uint8_t bit = (uint8_t)((frame[global >> 3u] >> (global & 7u)) & 1u);
        raw |= (uint64_t)bit << i;
    }
    return raw;
}

/**
 * Write the low @p count bits of @p value to the @p count payload bits
 * starting at global payload bit @p first_bit.
 *
 * @p count may be 64. The caller guarantees every accessed bit lies within
 * @p length bytes.
 */
static inline void codec_bits_store(uint8_t *frame,
                                    size_t length,
                                    uint32_t first_bit,
                                    uint32_t count,
                                    uint64_t value)
{
    uint32_t i;

    (void)length;
    for (i = 0u; i < count; ++i) {
        uint32_t global = first_bit + i;
        uint8_t mask = (uint8_t)(1u << (global & 7u));
        if (((value >> i) & 1u) != 0u) {
            frame[global >> 3u] |= mask;
        } else {
            frame[global >> 3u] &= (uint8_t)~mask;
        }
    }
}

/** Convert a payload bit number to its sawtooth table index. */
static inline uint32_t codec_bits_be_idx(uint32_t p)
{
    return (p >> 3u) * 8u + (7u - (p & 7u));
}

/** Convert a sawtooth table index to the corresponding payload bit. */
static inline uint32_t codec_bits_be_bit(uint32_t idx)
{
    return (idx >> 3u) * 8u + (7u - (idx & 7u));
}

/**
 * Read @p count bits in DBC Motorola sawtooth order starting at @p start_bit
 * (MSB payload position). The first sawtooth element carries the most
 * significant raw bit. Mirrors opendbc's get_raw_value for big-endian signals.
 */
static inline uint64_t codec_bits_load_saw(const uint8_t *frame,
                                           size_t length,
                                           uint32_t start_bit,
                                           uint32_t count)
{
    uint64_t raw = 0u;
    uint32_t idx = codec_bits_be_idx(start_bit);
    uint32_t k;

    (void)length;
    for (k = 0u; k < count; ++k) {
        uint32_t p = codec_bits_be_bit(idx + k);
        uint8_t bit = (uint8_t)((frame[p >> 3u] >> (p & 7u)) & 1u);
        raw |= (uint64_t)bit << (count - 1u - k);
    }
    return raw;
}

/**
 * Write @p count bits in sawtooth order starting at @p start_bit.
 * Mirrors the inverse of codec_bits_load_saw: raw's MSB goes to the first
 * sawtooth payload position.
 */
static inline void codec_bits_store_saw(uint8_t *frame,
                                        size_t length,
                                        uint32_t start_bit,
                                        uint32_t count,
                                        uint64_t value)
{
    uint32_t idx = codec_bits_be_idx(start_bit);
    uint32_t k;

    (void)length;
    for (k = 0u; k < count; ++k) {
        uint32_t p = codec_bits_be_bit(idx + k);
        uint32_t raw_bit = count - 1u - k;
        uint8_t mask = (uint8_t)(1u << (p & 7u));
        if (((value >> raw_bit) & 1u) != 0u) {
            frame[p >> 3u] |= mask;
        } else {
            frame[p >> 3u] &= (uint8_t)~mask;
        }
    }
}

/**
 * @return true when all sawtooth payload bits for a signal lie within
 * @p length bytes.
 */
static inline bool codec_bits_saw_fits(size_t length,
                                       uint32_t start_bit,
                                       uint32_t count)
{
    uint32_t idx = codec_bits_be_idx(start_bit);
    uint32_t k;

    for (k = 0u; k < count; ++k) {
        uint32_t p = codec_bits_be_bit(idx + k);
        if (p >= length * 8u) {
            return false;
        }
    }
    return true;
}

/**
 * Compute the minimal and maximal payload bit used by a sawtooth signal.
 * Used for the loader's derived first_bit/last_bit and for quick frame-size
 * checks. Both are within [0,63] when the signal is valid.
 */
static inline void codec_bits_saw_range(uint32_t start_bit,
                                        uint32_t count,
                                        uint32_t *min_out,
                                        uint32_t *max_out)
{
    uint32_t idx = codec_bits_be_idx(start_bit);
    uint32_t min_p = UINT32_MAX;
    uint32_t max_p = 0u;
    uint32_t k;

    for (k = 0u; k < count; ++k) {
        uint32_t p = codec_bits_be_bit(idx + k);
        if (p < min_p) {
            min_p = p;
        }
        if (p > max_p) {
            max_p = p;
        }
    }
    *min_out = min_p;
    *max_out = max_p;
}

/**
 * @return true when @p frame_length is a payload length the codec accepts.
 *
 * Accepted lengths are exactly the ones a CAN bus can carry: 1..8 bytes for
 * classic CAN, and 12/16/20/24/32/48/64 for CAN FD (SW-FR-CANFD-001,
 * SW-FR-CANFD-002). 9, 10 and 11 bytes cannot exist on either bus, so they
 * stay an argument error exactly as they were before CAN FD support; the
 * codec never silently accepts a payload the wire format cannot express.
 */
static inline bool codec_frame_length_ok(size_t frame_length)
{
    if (frame_length == 0u || frame_length > CANCESTRY_CODEC_FRAME_MAX_LENGTH) {
        return false;
    }
    if (frame_length <= CANCESTRY_CODEC_CLASSIC_FRAME_MAX_LENGTH) {
        return true;
    }
    return cancestry_can_payload_length_is_valid(true, frame_length);
}

/**
 * Sign-extend a @p length-bit unsigned pattern to a 64-bit signed integer.
 *
 * @p length may be 64; the pattern is then reinterpreted as int64_t.
 */
static inline int64_t codec_bits_sign_extend(uint64_t raw, uint32_t length)
{
    if (length == 64u) {
        return (int64_t)raw;
    }
    return (int64_t)(raw << (64u - length)) >> (64u - length);
}

/** @return 2^@p length - 1, or UINT64_MAX when @p length is 64. */
static inline uint64_t codec_bits_mask(uint32_t length)
{
    if (length == 64u) {
        return UINT64_MAX;
    }
    return (UINT64_C(1) << length) - UINT64_C(1);
}

#endif /* CANCESTRY_CODEC_BITS_H */
