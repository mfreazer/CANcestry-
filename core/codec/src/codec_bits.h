/*
 * CANcestry - internal LSB0 bit access helpers shared by decoder and encoder.
 *
 * The canonical payload bit array is linear LSB0 (codec-map-spec.md section
 * 3): payload bit g lives in frame[g / 8] at bit position g % 8, where bit
 * position 0 is the least significant bit of the byte.
 *
 * These helpers are private to core/codec/src and must stay allocation-free.
 */

#ifndef CANCESTRY_CODEC_BITS_H
#define CANCESTRY_CODEC_BITS_H

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
