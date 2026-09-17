/*
 * CANcestry codec conformance: CAN FD payload bounds.
 *
 * Proves that the codec engine packs and unpacks 64-byte CAN FD payloads
 * correctly and that it cannot overflow an 8-byte classic buffer.
 *
 * Verifies (SW-FR-CANFD-001, SW-FR-CANFD-005, CODEC-CANFD-BOUNDS-001):
 *   1. A CAN FD codec map (`can_fd: true`, dlc 64) loads and reports
 *      can_fd/dlc on the runtime structs; signals may address payload bits
 *      0..511.
 *   2. Signals placed in the first byte, the middle and the last byte of a
 *      64-byte payload encode and decode bit-exactly, both little- and
 *      big-endian, and every byte outside the declared signals stays
 *      untouched (read-modify-write).
 *   3. A full 64-byte round trip (encode -> decode) reproduces the original
 *      payload byte for byte, with zero warnings.
 *   4. The classic 8-byte buffer is never overrun: encoding or decoding a
 *      signal that lives beyond byte 8 against a heap-allocated 8-byte
 *      buffer fails with FRAME_TOO_SHORT and leaves every byte unchanged.
 *      Both buffers in this test are heap allocations of exactly the size
 *      under test, so AddressSanitizer turns any one-byte overrun into a
 *      hard failure.
 *   5. Payload lengths the wire format cannot express (9, 10, 11, 65) stay
 *      an argument error; 64 is accepted.
 *
 * Implements: SW-FR-CANFD-001, SW-FR-CANFD-002, SW-FR-CANFD-005
 * Test ids:    CODEC-CANFD-BOUNDS-001
 */

#include "cancestry/codec/decoder.h"
#include "cancestry/codec/encoder.h"
#include "cancestry/codec/loader.h"
#include "cancestry/codec/types.h"
#include "cancestry_test.h"

#include <stdlib.h>
#include <string.h>

#define FD_MESSAGE_ID ((uint32_t)0x1F0u)
#define FD_SIGNAL_COUNT ((size_t)4u)
#define FD_PAYLOAD ((size_t)64u)
#define CLASSIC_PAYLOAD ((size_t)8u)

/*
 * CAN FD codec map: one 64-byte message whose signals sit at the very start,
 * the middle and the very end of the payload.
 *
 *   HeadByte   LE uint  8 @ 0      bits   0..7   (byte 0)
 *   BeMid      BE uint 16 @ 127    bits 112..127 (bytes 14-15, big-endian)
 *   Wide64     LE uint 64 @ 256    bits 256..319 (bytes 32-39)
 *   TailByte   LE uint  8 @ 504    bits 504..511 (byte 63)
 */
static const char *const fd_map_yaml =
    "schema_version: \"0.3.0\"\n"
    "codec_map:\n"
    "  name: fd_demo\n"
    "  version: 1.0.0\n"
    "  description: CAN FD payload bounds fixture\n"
    "  can_fd: true\n"
    "  messages:\n"
    "    - id: 0x1F0\n"
    "      name: RadarCluster\n"
    "      dlc: 64\n"
    "      period_ms: 20\n"
    "      signals:\n"
    "        - name: HeadByte\n"
    "          start_bit: 0\n"
    "          bit_length: 8\n"
    "          type: uint\n"
    "          endianness: little\n"
    "        - name: BeMid\n"
    "          start_bit: 127\n"
    "          bit_length: 16\n"
    "          type: uint\n"
    "          endianness: big\n"
    "        - name: Wide64\n"
    "          start_bit: 256\n"
    "          bit_length: 64\n"
    "          type: uint\n"
    "          endianness: little\n"
    "        - name: TailByte\n"
    "          start_bit: 504\n"
    "          bit_length: 8\n"
    "          type: uint\n"
    "          endianness: little\n";

/** Load the FD map with CAN FD capabilities declared. */
static cancestry_codec_map_t *load_fd_map(void)
{
    cancestry_codec_platform_caps_t caps;
    cancestry_codec_load_error_t error;
    cancestry_codec_map_t *map;

    caps.can_fd = true;
    map = cancestry_codec_map_load_checked(fd_map_yaml, strlen(fd_map_yaml), &caps, &error);
    if (map == NULL) {
        printf("    FAIL to load FD codec map: %s (line %u, column %u)\n", error.message,
               (unsigned)error.line, (unsigned)error.column);
        fflush(stdout);
        cancestry_test_failures++;
    }
    return map;
}

static cancestry_value_t u64_value(uint64_t raw)
{
    cancestry_value_t value;

    memset(&value, 0, sizeof(value));
    value.kind = CANCESTRY_VALUE_KIND_UINT;
    value.value.unsigned_integer = raw;
    return value;
}

static void test_map_shape(void)
{
    cancestry_codec_map_t *map = load_fd_map();
    const cancestry_codec_message_t *message;

    CANCESSTRY_TEST_CHECK(map != NULL);
    if (map == NULL) {
        return;
    }
    CANCESSTRY_TEST_CHECK(map->can_fd);
    CANCESSTRY_TEST_CHECK_U64(map->message_count, 1u);
    message = cancestry_codec_map_find_message(map, FD_MESSAGE_ID);
    CANCESSTRY_TEST_CHECK(message != NULL);
    if (message != NULL) {
        CANCESSTRY_TEST_CHECK_U64(message->dlc, FD_PAYLOAD);
        CANCESSTRY_TEST_CHECK_U64(message->signal_count, FD_SIGNAL_COUNT);
    }
    /* The last signal reaches the final bit of a 64-byte payload. */
    {
        const cancestry_codec_signal_t *tail = cancestry_codec_map_find_signal(map, "TailByte");
        CANCESSTRY_TEST_CHECK(tail != NULL);
        if (tail != NULL) {
            CANCESSTRY_TEST_CHECK_U64(tail->first_bit, 504u);
            CANCESSTRY_TEST_CHECK_U64(tail->last_bit, 511u);
        }
    }
    cancestry_codec_map_free(map);
}

static void test_encode_decode_all_signals(void)
{
    cancestry_codec_map_t *map = load_fd_map();
    uint8_t *frame = NULL;
    cancestry_decoded_signal_t decoded[FD_SIGNAL_COUNT];
    cancestry_codec_warnings_t warnings;
    size_t count = 0u;
    size_t i;

    CANCESSTRY_TEST_CHECK(map != NULL);
    if (map == NULL) {
        return;
    }

    /* Exactly 64 heap bytes: ASan fails the test on any overrun. */
    frame = (uint8_t *)malloc(FD_PAYLOAD);
    CANCESSTRY_TEST_CHECK(frame != NULL);
    if (frame == NULL) {
        cancestry_codec_map_free(map);
        return;
    }
    memset(frame, 0, FD_PAYLOAD);

    {
        const cancestry_codec_signal_t *signal;
        cancestry_codec_status_t status;
        cancestry_value_t value;

        signal = cancestry_codec_map_find_signal(map, "HeadByte");
        CANCESSTRY_TEST_CHECK(signal != NULL);
        value = u64_value(0xA5u);
        status = cancestry_codec_encode_signal(signal, &value, frame, FD_PAYLOAD, NULL);
        CANCESSTRY_TEST_CHECK_I64((int64_t)status, (int64_t)CANCESTRY_CODEC_OK);

        signal = cancestry_codec_map_find_signal(map, "BeMid");
        CANCESSTRY_TEST_CHECK(signal != NULL);
        value = u64_value(0xBEEFu);
        status = cancestry_codec_encode_signal(signal, &value, frame, FD_PAYLOAD, NULL);
        CANCESSTRY_TEST_CHECK_I64((int64_t)status, (int64_t)CANCESTRY_CODEC_OK);

        signal = cancestry_codec_map_find_signal(map, "Wide64");
        CANCESSTRY_TEST_CHECK(signal != NULL);
        value = u64_value(UINT64_C(0x0123456789ABCDEF));
        status = cancestry_codec_encode_signal(signal, &value, frame, FD_PAYLOAD, NULL);
        CANCESSTRY_TEST_CHECK_I64((int64_t)status, (int64_t)CANCESTRY_CODEC_OK);

        signal = cancestry_codec_map_find_signal(map, "TailByte");
        CANCESSTRY_TEST_CHECK(signal != NULL);
        value = u64_value(0x5Au);
        status = cancestry_codec_encode_signal(signal, &value, frame, FD_PAYLOAD, NULL);
        CANCESSTRY_TEST_CHECK_I64((int64_t)status, (int64_t)CANCESTRY_CODEC_OK);
    }

    /* Bytes the map does not declare stay zero: encode is read-modify-write. */
    CANCESSTRY_TEST_CHECK_U64(frame[0], 0xA5u);
    CANCESSTRY_TEST_CHECK_U64(frame[1], 0u);
    /* Contiguous big-endian (codec-map-spec.md section 5): signal bit i maps
     * to payload bit start_bit - (bit_length - 1) + i, so the LSB sits in the
     * lower byte of the run and the MSB (start_bit 127) in byte 15. */
    CANCESSTRY_TEST_CHECK_U64(frame[14], 0xEFu);
    CANCESSTRY_TEST_CHECK_U64(frame[15], 0xBEu);
    CANCESSTRY_TEST_CHECK_U64(frame[16], 0u);
    CANCESSTRY_TEST_CHECK_U64(frame[32], 0xEFu); /* little-endian: LSB first */
    CANCESSTRY_TEST_CHECK_U64(frame[39], 0x01u);
    CANCESSTRY_TEST_CHECK_U64(frame[40], 0u);
    CANCESSTRY_TEST_CHECK_U64(frame[62], 0u);
    CANCESSTRY_TEST_CHECK_U64(frame[63], 0x5Au);

    /* Decode the same buffer back. */
    memset(&warnings, 0, sizeof(warnings));
    {
        cancestry_codec_status_t status = cancestry_codec_decode_frame(
            map, FD_MESSAGE_ID, frame, FD_PAYLOAD, decoded, FD_SIGNAL_COUNT, &count, &warnings);
        CANCESSTRY_TEST_CHECK_I64((int64_t)status, (int64_t)CANCESTRY_CODEC_OK);
    }
    CANCESSTRY_TEST_CHECK_U64(count, FD_SIGNAL_COUNT);
    CANCESSTRY_TEST_CHECK_U64(warnings.frame_too_short, 0u);
    CANCESSTRY_TEST_CHECK_U64(warnings.enum_unknown, 0u);
    CANCESSTRY_TEST_CHECK_U64(warnings.value_clamped, 0u);
    for (i = 0u; i < count && i < FD_SIGNAL_COUNT; ++i) {
        CANCESSTRY_TEST_CHECK(decoded[i].signal != NULL);
    }
    {
        const cancestry_codec_signal_t *head = cancestry_codec_map_find_signal(map, "HeadByte");
        const cancestry_codec_signal_t *bemid = cancestry_codec_map_find_signal(map, "BeMid");
        const cancestry_codec_signal_t *wide = cancestry_codec_map_find_signal(map, "Wide64");
        const cancestry_codec_signal_t *tail = cancestry_codec_map_find_signal(map, "TailByte");
        CANCESSTRY_TEST_CHECK(decoded[0].signal == head);
        CANCESSTRY_TEST_CHECK_U64(decoded[0].raw, 0xA5u);
        CANCESSTRY_TEST_CHECK(decoded[1].signal == bemid);
        CANCESSTRY_TEST_CHECK_U64(decoded[1].raw, 0xBEEFu);
        CANCESSTRY_TEST_CHECK(decoded[2].signal == wide);
        CANCESSTRY_TEST_CHECK_U64(decoded[2].raw, UINT64_C(0x0123456789ABCDEF));
        CANCESSTRY_TEST_CHECK(decoded[3].signal == tail);
        CANCESSTRY_TEST_CHECK_U64(decoded[3].raw, 0x5Au);
    }

    /* Re-encoding the decoded values reproduces the payload byte for byte. */
    {
        uint8_t *again = (uint8_t *)malloc(FD_PAYLOAD);
        CANCESSTRY_TEST_CHECK(again != NULL);
        if (again != NULL) {
            memset(again, 0, FD_PAYLOAD);
            for (i = 0u; i < count && i < FD_SIGNAL_COUNT; ++i) {
                CANCESSTRY_TEST_CHECK(
                    cancestry_codec_encode_signal(decoded[i].signal, &decoded[i].value, again,
                                                  FD_PAYLOAD, NULL) >= CANCESTRY_CODEC_OK);
            }
            CANCESSTRY_TEST_CHECK(memcmp(frame, again, FD_PAYLOAD) == 0);
            free(again);
        }
    }

    free(frame);
    cancestry_codec_map_free(map);
}

static void test_classic_buffer_is_never_overrun(void)
{
    cancestry_codec_map_t *map = load_fd_map();
    uint8_t *classic = NULL;
    const cancestry_codec_signal_t *tail;
    cancestry_value_t value;
    cancestry_decoded_signal_t out;
    cancestry_decoded_signal_t decoded[FD_SIGNAL_COUNT];
    cancestry_codec_warnings_t warnings;
    size_t count = 0u;
    size_t i;

    CANCESSTRY_TEST_CHECK(map != NULL);
    if (map == NULL) {
        return;
    }
    tail = cancestry_codec_map_find_signal(map, "TailByte");
    CANCESSTRY_TEST_CHECK(tail != NULL);
    if (tail == NULL) {
        cancestry_codec_map_free(map);
        return;
    }

    /* Exactly 8 heap bytes, pre-filled with a canary pattern. Any write past
     * the end is a heap-buffer-overflow under ASan; any write inside is
     * caught by the memcmp below. */
    classic = (uint8_t *)malloc(CLASSIC_PAYLOAD);
    CANCESSTRY_TEST_CHECK(classic != NULL);
    if (classic == NULL) {
        cancestry_codec_map_free(map);
        return;
    }
    memset(classic, 0xA5, CLASSIC_PAYLOAD);

    value = u64_value(0x5Au);
    CANCESSTRY_TEST_CHECK_I64(
        (int64_t)cancestry_codec_encode_signal(tail, &value, classic, CLASSIC_PAYLOAD, NULL),
        (int64_t)CANCESTRY_CODEC_ERR_FRAME_TOO_SHORT);
    for (i = 0u; i < CLASSIC_PAYLOAD; ++i) {
        CANCESSTRY_TEST_CHECK_U64(classic[i], 0xA5u);
    }

    /* Decoding an FD message out of an 8-byte buffer drops the message
     * (SW-FR-CODEC-008) and writes nothing. */
    memset(&warnings, 0, sizeof(warnings));
    memset(decoded, 0, sizeof(decoded));
    CANCESSTRY_TEST_CHECK_I64(
        (int64_t)cancestry_codec_decode_frame(map, FD_MESSAGE_ID, classic, CLASSIC_PAYLOAD,
                                              decoded, FD_SIGNAL_COUNT, &count, &warnings),
        (int64_t)CANCESTRY_CODEC_WARN_FRAME_TOO_SHORT);
    CANCESSTRY_TEST_CHECK_U64(warnings.frame_too_short, 1u);
    for (i = 0u; i < FD_SIGNAL_COUNT; ++i) {
        CANCESSTRY_TEST_CHECK(decoded[i].signal == NULL);
        CANCESSTRY_TEST_CHECK_U64(decoded[i].raw, 0u);
    }
    CANCESSTRY_TEST_CHECK_I64(
        (int64_t)cancestry_codec_decode_signal(tail, classic, CLASSIC_PAYLOAD, &out),
        (int64_t)CANCESTRY_CODEC_ERR_FRAME_TOO_SHORT);

    free(classic);
    cancestry_codec_map_free(map);
}

static void test_frame_length_validation(void)
{
    cancestry_codec_map_t *map = load_fd_map();
    uint8_t *frame = NULL;
    cancestry_decoded_signal_t decoded[FD_SIGNAL_COUNT];
    size_t count = 0u;
    static const size_t rejected[] = {9u, 10u, 11u, 13u, 15u, 65u, 128u};
    static const size_t accepted[] = {1u, 8u, 12u, 16u, 20u, 24u, 32u, 48u, 64u};
    size_t i;

    CANCESSTRY_TEST_CHECK(map != NULL);
    if (map == NULL) {
        return;
    }
    frame = (uint8_t *)malloc(FD_PAYLOAD);
    CANCESSTRY_TEST_CHECK(frame != NULL);
    if (frame == NULL) {
        cancestry_codec_map_free(map);
        return;
    }
    memset(frame, 0, FD_PAYLOAD);

    for (i = 0u; i < sizeof(rejected) / sizeof(rejected[0]); ++i) {
        cancestry_codec_status_t status = cancestry_codec_decode_frame(
            map, FD_MESSAGE_ID, frame, rejected[i], decoded, FD_SIGNAL_COUNT, &count, NULL);
        CANCESSTRY_TEST_CHECK_I64((int64_t)status, (int64_t)CANCESTRY_CODEC_ERR_ARGUMENT);
    }
    for (i = 0u; i < sizeof(accepted) / sizeof(accepted[0]); ++i) {
        /* A short-but-legal frame is a dropped message, not an argument
         * error: the length itself is representable on the wire. */
        cancestry_codec_status_t status =
            cancestry_codec_decode_frame(map, FD_MESSAGE_ID, frame, accepted[i], decoded,
                                         FD_SIGNAL_COUNT, &count, NULL);
        CANCESSTRY_TEST_CHECK(status == CANCESTRY_CODEC_OK ||
                              status == CANCESTRY_CODEC_WARN_FRAME_TOO_SHORT);
    }
    /* Zero-length and above-64 are argument errors. */
    CANCESSTRY_TEST_CHECK_I64(
        (int64_t)cancestry_codec_decode_frame(map, FD_MESSAGE_ID, frame, 0u, decoded,
                                              FD_SIGNAL_COUNT, &count, NULL),
        (int64_t)CANCESTRY_CODEC_ERR_ARGUMENT);

    free(frame);
    cancestry_codec_map_free(map);
}

int main(void)
{
    CANCESSTRY_TEST_SUITE_BEGIN("codec CAN FD payload bounds");

    CANCESSTRY_TEST_CASE("an FD codec map exposes can_fd, dlc 64 and bits 0..511");
    test_map_shape();

    CANCESSTRY_TEST_CASE("64-byte payloads encode and decode bit-exactly");
    test_encode_decode_all_signals();

    CANCESSTRY_TEST_CASE("an 8-byte classic buffer is never overrun");
    test_classic_buffer_is_never_overrun();

    CANCESSTRY_TEST_CASE("only wire-representable payload lengths are accepted");
    test_frame_length_validation();

    return CANCESSTRY_TEST_SUITE_END();
}
