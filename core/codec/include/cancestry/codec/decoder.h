/*
 * CANcestry - signal decoder.
 *
 * Normative references:
 *   docs/packages/codec-map-spec.md  sections 3-8
 *   docs/software/SwRS.md            SW-FR-CODEC-001, SW-FR-CODEC-003,
 *                                    SW-FR-CODEC-004, SW-FR-CODEC-005,
 *                                    SW-FR-CODEC-006, SW-FR-CODEC-008
 *   docs/system/SyRS.md              SYS-NF-001 (determinism)
 *
 * Decode is allocation-free, has no global state, and is a pure function of
 * its inputs: decoding the same frame twice produces bit-identical output
 * (SYS-NF-001).
 */

#ifndef CANCESTRY_CODEC_DECODER_H
#define CANCESTRY_CODEC_DECODER_H

#include "cancestry/codec/types.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Decode a classic CAN frame against a codec map.
 *
 * The frame is matched to a message by CAN id. When the frame is too short
 * for any declared signal, the whole message is dropped: nothing is written
 * to @p signals, @p *count is left untouched, the frame_too_short warning
 * counter is incremented (when @p warnings is non-NULL) and
 * CANCESTRY_CODEC_WARN_FRAME_TOO_SHORT is returned (SW-FR-CODEC-008).
 * Extra bytes beyond the declared dlc are ignored (codec-map-spec.md
 * section 8).
 *
 * @param map          Loaded codec map.
 * @param can_id       CAN id of the received frame.
 * @param frame        Payload bytes.
 * @param frame_length Number of bytes in @p frame, in [1, 8].
 * @param signals      Caller-owned output buffer of @p capacity entries.
 * @param capacity     Number of entries in @p signals; must be at least the
 *                     matched message's signal_count (query it with
 *                     cancestry_codec_map_find_message()).
 * @param count        Receives the number of decoded signals.
 * @param warnings     Warning counters, or NULL.
 * @return CANCESTRY_CODEC_OK on success, CANCESTRY_CODEC_WARN_FRAME_TOO_SHORT
 *         when the frame was dropped, CANCESTRY_CODEC_ERR_NOT_FOUND when no
 *         message matches @p can_id, or a negative status for malformed
 *         arguments.
 */
cancestry_codec_status_t cancestry_codec_decode_frame(const cancestry_codec_map_t *map,
                                                      uint32_t can_id,
                                                      const uint8_t *frame,
                                                      size_t frame_length,
                                                      cancestry_decoded_signal_t *signals,
                                                      size_t capacity,
                                                      size_t *count,
                                                      cancestry_codec_warnings_t *warnings);

/**
 * Decode a single signal from raw payload bytes.
 *
 * This is the per-signal primitive behind cancestry_codec_decode_frame();
 * it does not consult the codec map beyond @p signal and does not warn on
 * anything (warning counters belong to the frame-level API).
 *
 * @param signal       Signal definition.
 * @param frame        Payload bytes.
 * @param frame_length Number of bytes in @p frame; must be long enough for
 *                     the signal's bit span.
 * @param out          Receives raw, physical value and label.
 * @return CANCESTRY_CODEC_OK, CANCESTRY_CODEC_ERR_NULL for a NULL argument,
 *         CANCESTRY_CODEC_ERR_ARGUMENT when frame_length is 0 or above 8,
 *         or CANCESTRY_CODEC_ERR_FRAME_TOO_SHORT when the frame does not
 *         cover the signal's bit span.
 */
cancestry_codec_status_t cancestry_codec_decode_signal(const cancestry_codec_signal_t *signal,
                                                       const uint8_t *frame,
                                                       size_t frame_length,
                                                       cancestry_decoded_signal_t *out);

#ifdef __cplusplus
}
#endif

#endif /* CANCESTRY_CODEC_DECODER_H */
