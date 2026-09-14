/*
 * CANcestry - signal encoder.
 *
 * Normative references:
 *   docs/packages/codec-map-spec.md  sections 3-7
 *   docs/software/SwRS.md            SW-FR-CODEC-002 .. SW-FR-CODEC-006
 *
 * Encode is allocation-free, has no global state, and is a pure function of
 * its inputs. Rounding is round-to-nearest, ties away from zero
 * (codec-map-spec.md section 7).
 */

#ifndef CANCESTRY_CODEC_ENCODER_H
#define CANCESTRY_CODEC_ENCODER_H

#include "cancestry/codec/types.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Encode one signal's physical value into a payload buffer.
 *
 * The signal's bit span is written read-modify-write; bits outside the span
 * are preserved, so encoding every signal of a decoded frame reproduces the
 * original frame.
 *
 * Accepted value kinds per signal type (documented in core/codec/README.md):
 *   uint    unscaled: UINT;  scaled: REAL or UINT.
 *   int     unscaled: INT;   scaled: REAL or INT.
 *   boolean BOOL, or INT/UINT equal to 0 or 1.
 *   enum    INT or UINT (the raw value).
 *
 * Values outside the signal's bit range fail with CANCESTRY_CODEC_ERR_RANGE.
 * On signals with a declared min/max, out-of-range physical values fail with
 * CANCESTRY_CODEC_ERR_RANGE when strict (the default) or are clamped with a
 * value_clamped warning otherwise.
 *
 * @param signal       Signal definition.
 * @param value        Physical value to encode.
 * @param frame        Destination buffer; existing bits outside the signal
 *                     span are preserved.
 * @param frame_length Number of bytes in @p frame, in [1, 8]; must cover the
 *                     signal's bit span.
 * @param warnings     Warning counters, or NULL.
 * @return CANCESTRY_CODEC_OK, CANCESTRY_CODEC_WARN_VALUE_CLAMPED when a
 *         non-strict clamp was applied, or a negative status.
 */
cancestry_codec_status_t cancestry_codec_encode_signal(const cancestry_codec_signal_t *signal,
                                                       const cancestry_value_t *value,
                                                       uint8_t *frame,
                                                       size_t frame_length,
                                                       cancestry_codec_warnings_t *warnings);

/**
 * Look up the raw value behind a value-mapping label.
 *
 * @param signal  Signal with a value mapping (boolean or enum).
 * @param label   Label to find.
 * @param raw_out Receives the mapped raw value.
 * @return true when the label exists, false otherwise.
 */
bool cancestry_codec_label_to_raw(const cancestry_codec_signal_t *signal,
                                  const char *label,
                                  uint64_t *raw_out);

#ifdef __cplusplus
}
#endif

#endif /* CANCESTRY_CODEC_ENCODER_H */
