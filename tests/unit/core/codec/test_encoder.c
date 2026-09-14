/*
 * Unit tests for the codec encoder.
 *
 * Verifies:
 *   SW-FR-CODEC-002  The software shall encode named signals into CAN frames.
 *   SW-FR-CODEC-003  Little- and big-endian signals, canonical LSB0 bit model.
 *   SW-FR-CODEC-004  Scaling and offset, round-to-nearest ties-away rounding.
 *   SW-FR-CODEC-005  Signed and unsigned integer signals.
 *   SW-FR-CODEC-006  Boolean and enum value mappings.
 *   SYS-NF-001       Determinism: same input, bit-identical output.
 *   QA-H02           Encode is the exact inverse of decode.
 *
 * Traceability: CODEC-ENCODE-001, CODEC-BITPACK-002, CODEC-SCALING-001,
 *               CODEC-SIGNED-001, CODEC-BOOL-ENUM-001, CODEC-DETERMINISM-001
 *
 * Normative source: docs/packages/codec-map-spec.md sections 3-7.
 */

#include "cancestry/codec/decoder.h"
#include "cancestry/codec/encoder.h"

#include "cancestry_codec_test.h"

#include <string.h>

static cancestry_codec_map_t *test_map = NULL;
static cancestry_codec_map_t *bit_map = NULL;
static cancestry_codec_map_t *rounding_map = NULL;

static const char *const rounding_yaml =
    "schema_version: \"0.2.0\"\n"
    "codec_map:\n"
    "  name: rounding\n"
    "  version: 1.0.0\n"
    "  messages:\n"
    "    - id: 0x300\n"
    "      name: Rounding\n"
    "      dlc: 8\n"
    "      signals:\n"
    "        - name: Halves\n"
    "          start_bit: 0\n"
    "          bit_length: 8\n"
    "          type: uint\n"
    "          endianness: little\n"
    "          scale: 0.5\n"
    "        - name: SignedHalves\n"
    "          start_bit: 8\n"
    "          bit_length: 8\n"
    "          type: int\n"
    "          endianness: little\n"
    "          scale: 0.5\n"
    "        - name: Clamped\n"
    "          start_bit: 16\n"
    "          bit_length: 8\n"
    "          type: uint\n"
    "          endianness: little\n"
    "          scale: 0.5\n"
    "          min: 0\n"
    "          max: 100\n"
    "          strict: false\n"
    "        - name: StrictRange\n"
    "          start_bit: 24\n"
    "          bit_length: 8\n"
    "          type: int\n"
    "          endianness: little\n"
    "          min: -50\n"
    "          max: 50\n"
    "        - name: U8\n"
    "          start_bit: 32\n"
    "          bit_length: 8\n"
    "          type: uint\n"
    "          endianness: little\n"
    "        - name: I8\n"
    "          start_bit: 40\n"
    "          bit_length: 8\n"
    "          type: int\n"
    "          endianness: little\n"
    "        - name: U64\n"
    "          start_bit: 0\n"
    "          bit_length: 64\n"
    "          type: uint\n"
    "          endianness: little\n"
    "        - name: I64\n"
    "          start_bit: 0\n"
    "          bit_length: 64\n"
    "          type: int\n"
    "          endianness: little\n";

static const cancestry_value_t *real_value(double value)
{
    static cancestry_value_t out;
    out.kind = CANCESTRY_VALUE_KIND_REAL;
    out.value.real = value;
    return &out;
}

static const cancestry_value_t *int_value(int64_t value)
{
    static cancestry_value_t out;
    out.kind = CANCESTRY_VALUE_KIND_INT;
    out.value.integer = value;
    return &out;
}

static const cancestry_value_t *uint_value(uint64_t value)
{
    static cancestry_value_t out;
    out.kind = CANCESTRY_VALUE_KIND_UINT;
    out.value.unsigned_integer = value;
    return &out;
}

static const cancestry_value_t *bool_value(bool value)
{
    static cancestry_value_t out;
    out.kind = CANCESTRY_VALUE_KIND_BOOL;
    out.value.boolean = value;
    return &out;
}

static void test_decode_encode_round_trip(void)
{
    /* decode a frame, re-encode every signal over a copy, expect identity. */
    const uint8_t original[8] = {0x34u, 0x12u, 0x80u, 0x06u, 0xDEu, 0xADu, 0xBEu, 0xEFu};
    cancestry_decoded_signal_t decoded[CANCESTRY_CODEC_DEMO_SIGNAL_COUNT];
    size_t count = 0u;
    uint8_t rebuilt[8];
    size_t i;

    memcpy(rebuilt, original, sizeof(rebuilt));
    CANCESSTRY_TEST_CHECK(cancestry_codec_decode_frame(
                              test_map, CANCESTRY_CODEC_DEMO_MESSAGE_ID, original,
                              sizeof(original), decoded, sizeof(decoded) / sizeof(decoded[0]),
                              &count, NULL) == CANCESTRY_CODEC_OK);
    CANCESSTRY_TEST_CHECK_U64(count, CANCESTRY_CODEC_DEMO_SIGNAL_COUNT);

    for (i = 0u; i < count; ++i) {
        CANCESSTRY_TEST_CHECK(cancestry_codec_encode_signal(decoded[i].signal,
                                                            &decoded[i].value, rebuilt,
                                                            sizeof(rebuilt), NULL) ==
                              CANCESTRY_CODEC_OK);
    }
    CANCESSTRY_TEST_CHECK(memcmp(rebuilt, original, sizeof(rebuilt)) == 0);
}

static void test_bit_map_round_trip(void)
{
    const uint8_t original[8] = {0xA1u, 0x0Bu, 0xF0u, 0x0Fu, 0x00u, 0x00u, 0x00u, 0x00u};
    cancestry_decoded_signal_t decoded[8];
    size_t count = 0u;
    uint8_t rebuilt[8];
    size_t i;

    memcpy(rebuilt, original, sizeof(rebuilt));
    CANCESSTRY_TEST_CHECK(cancestry_codec_decode_frame(
                              bit_map, CANCESTRY_CODEC_BIT_MESSAGE_ID, original, sizeof(original),
                              decoded, sizeof(decoded) / sizeof(decoded[0]), &count, NULL) ==
                          CANCESTRY_CODEC_OK);
    CANCESSTRY_TEST_CHECK_U64(count, 8u);

    for (i = 0u; i < count; ++i) {
        CANCESSTRY_TEST_CHECK(cancestry_codec_encode_signal(decoded[i].signal,
                                                            &decoded[i].value, rebuilt,
                                                            sizeof(rebuilt), NULL) ==
                              CANCESTRY_CODEC_OK);
    }
    CANCESSTRY_TEST_CHECK(memcmp(rebuilt, original, sizeof(rebuilt)) == 0);
}

static void test_rounding_ties_away_from_zero(void)
{
    /* Scale 0.5 is chosen deliberately: it is exact in IEEE double, so the
     * x.5 ties below are exact halfway points. */
    const cancestry_codec_signal_t *halves = cancestry_codec_map_find_signal(rounding_map, "Halves");
    const cancestry_codec_signal_t *signed_halves =
        cancestry_codec_map_find_signal(rounding_map, "SignedHalves");
    uint8_t frame[8];
    size_t i;

    CANCESSTRY_TEST_CHECK(halves != NULL);
    CANCESSTRY_TEST_CHECK(signed_halves != NULL);

    for (i = 0u; i < sizeof(frame); ++i) {
        frame[i] = 0u;
    }
    CANCESSTRY_TEST_CHECK(cancestry_codec_encode_signal(halves, real_value(1.25), frame,
                                                        sizeof(frame), NULL) ==
                          CANCESTRY_CODEC_OK);
    CANCESSTRY_TEST_CHECK_U64(frame[0], 3u); /* 2.5 rounds to 3 (ties away) */
    CANCESSTRY_TEST_CHECK(cancestry_codec_encode_signal(halves, real_value(1.75), frame,
                                                        sizeof(frame), NULL) ==
                          CANCESTRY_CODEC_OK);
    CANCESSTRY_TEST_CHECK_U64(frame[0], 4u); /* 3.5 rounds to 4 */
    CANCESSTRY_TEST_CHECK(cancestry_codec_encode_signal(halves, real_value(1.24), frame,
                                                        sizeof(frame), NULL) ==
                          CANCESTRY_CODEC_OK);
    CANCESSTRY_TEST_CHECK_U64(frame[0], 2u); /* 2.48 rounds to 2 */
    CANCESSTRY_TEST_CHECK(cancestry_codec_encode_signal(halves, real_value(0.25), frame,
                                                        sizeof(frame), NULL) ==
                          CANCESTRY_CODEC_OK);
    CANCESSTRY_TEST_CHECK_U64(frame[0], 1u); /* 0.5 rounds to 1 */
    CANCESSTRY_TEST_CHECK(cancestry_codec_encode_signal(halves, real_value(0.24), frame,
                                                        sizeof(frame), NULL) ==
                          CANCESTRY_CODEC_OK);
    CANCESSTRY_TEST_CHECK_U64(frame[0], 0u); /* 0.48 rounds to 0 */

    CANCESSTRY_TEST_CHECK(cancestry_codec_encode_signal(signed_halves, real_value(-1.25),
                                                        frame, sizeof(frame), NULL) ==
                          CANCESTRY_CODEC_OK);
    CANCESSTRY_TEST_CHECK_U64(frame[1], 0xFDu); /* -2.5 rounds to -3 (ties away) */
    CANCESSTRY_TEST_CHECK(cancestry_codec_encode_signal(signed_halves, real_value(-1.75),
                                                        frame, sizeof(frame), NULL) ==
                          CANCESTRY_CODEC_OK);
    CANCESSTRY_TEST_CHECK_U64(frame[1], 0xFCu); /* -3.5 rounds to -4 */
    CANCESSTRY_TEST_CHECK(cancestry_codec_encode_signal(signed_halves, real_value(-1.24),
                                                        frame, sizeof(frame), NULL) ==
                          CANCESTRY_CODEC_OK);
    CANCESSTRY_TEST_CHECK_U64(frame[1], 0xFEu); /* -2.48 rounds to -2 */
}

static void test_limits_strict_and_clamp(void)
{
    const cancestry_codec_signal_t *clamped = cancestry_codec_map_find_signal(rounding_map, "Clamped");
    const cancestry_codec_signal_t *strict_range =
        cancestry_codec_map_find_signal(rounding_map, "StrictRange");
    const cancestry_codec_signal_t *engine_speed =
        cancestry_codec_map_find_signal(test_map, "EngineSpeed");
    uint8_t frame[8];
    cancestry_codec_warnings_t warnings;
    size_t i;

    for (i = 0u; i < sizeof(frame); ++i) {
        frame[i] = 0u;
    }
    memset(&warnings, 0, sizeof(warnings));

    /* Declared min/max with default strict=true rejects out-of-range values. */
    CANCESSTRY_TEST_CHECK(cancestry_codec_encode_signal(strict_range, int_value(60), frame,
                                                        sizeof(frame), NULL) ==
                          CANCESTRY_CODEC_ERR_RANGE);
    CANCESSTRY_TEST_CHECK(cancestry_codec_encode_signal(strict_range, int_value(-60), frame,
                                                        sizeof(frame), NULL) ==
                          CANCESTRY_CODEC_ERR_RANGE);
    CANCESSTRY_TEST_CHECK(cancestry_codec_encode_signal(strict_range, int_value(50), frame,
                                                        sizeof(frame), NULL) ==
                          CANCESTRY_CODEC_OK);
    CANCESSTRY_TEST_CHECK_U64(frame[3], 50u);

    CANCESSTRY_TEST_CHECK(cancestry_codec_encode_signal(engine_speed, real_value(20000.0),
                                                        frame, sizeof(frame), NULL) ==
                          CANCESTRY_CODEC_ERR_RANGE);
    CANCESSTRY_TEST_CHECK(cancestry_codec_encode_signal(engine_speed, real_value(16000.0),
                                                        frame, sizeof(frame), NULL) ==
                          CANCESTRY_CODEC_OK);
    CANCESSTRY_TEST_CHECK_U64(frame[0], 0u);
    CANCESSTRY_TEST_CHECK_U64(frame[1], 0xFAu); /* 16000 / 0.25 = 64000 = 0xFA00 */

    /* Non-strict clamps and counts a warning. */
    CANCESSTRY_TEST_CHECK(cancestry_codec_encode_signal(clamped, real_value(120.0), frame,
                                                        sizeof(frame), &warnings) ==
                          CANCESTRY_CODEC_WARN_VALUE_CLAMPED);
    CANCESSTRY_TEST_CHECK_U64(warnings.value_clamped, 1u);
    CANCESSTRY_TEST_CHECK_U64(frame[2], 200u); /* 100 / 0.5 */
    CANCESSTRY_TEST_CHECK(cancestry_codec_encode_signal(clamped, real_value(-10.0), frame,
                                                        sizeof(frame), &warnings) ==
                          CANCESTRY_CODEC_WARN_VALUE_CLAMPED);
    CANCESSTRY_TEST_CHECK_U64(warnings.value_clamped, 2u);
    CANCESSTRY_TEST_CHECK_U64(frame[2], 0u);
    CANCESSTRY_TEST_CHECK(cancestry_codec_encode_signal(clamped, real_value(50.0), frame,
                                                        sizeof(frame), &warnings) ==
                          CANCESTRY_CODEC_OK);
    CANCESSTRY_TEST_CHECK_U64(warnings.value_clamped, 2u);
    CANCESSTRY_TEST_CHECK_U64(frame[2], 100u);
}

static void test_integer_ranges(void)
{
    const cancestry_codec_signal_t *u8 = cancestry_codec_map_find_signal(rounding_map, "U8");
    const cancestry_codec_signal_t *i8 = cancestry_codec_map_find_signal(rounding_map, "I8");
    const cancestry_codec_signal_t *u64 = cancestry_codec_map_find_signal(rounding_map, "U64");
    const cancestry_codec_signal_t *i64 = cancestry_codec_map_find_signal(rounding_map, "I64");
    uint8_t frame[8];
    size_t i;

    for (i = 0u; i < sizeof(frame); ++i) {
        frame[i] = 0u;
    }
    CANCESSTRY_TEST_CHECK(cancestry_codec_encode_signal(u8, uint_value(255u), frame,
                                                        sizeof(frame), NULL) ==
                          CANCESTRY_CODEC_OK);
    CANCESSTRY_TEST_CHECK_U64(frame[4], 0xFFu);
    CANCESSTRY_TEST_CHECK(cancestry_codec_encode_signal(u8, uint_value(256u), frame,
                                                        sizeof(frame), NULL) ==
                          CANCESTRY_CODEC_ERR_RANGE);

    CANCESSTRY_TEST_CHECK(cancestry_codec_encode_signal(i8, int_value(-128), frame,
                                                        sizeof(frame), NULL) ==
                          CANCESTRY_CODEC_OK);
    CANCESSTRY_TEST_CHECK_U64(frame[5], 0x80u);
    CANCESSTRY_TEST_CHECK(cancestry_codec_encode_signal(i8, int_value(127), frame,
                                                        sizeof(frame), NULL) ==
                          CANCESTRY_CODEC_OK);
    CANCESSTRY_TEST_CHECK_U64(frame[5], 0x7Fu);
    CANCESSTRY_TEST_CHECK(cancestry_codec_encode_signal(i8, int_value(128), frame,
                                                        sizeof(frame), NULL) ==
                          CANCESTRY_CODEC_ERR_RANGE);
    CANCESSTRY_TEST_CHECK(cancestry_codec_encode_signal(i8, int_value(-129), frame,
                                                        sizeof(frame), NULL) ==
                          CANCESTRY_CODEC_ERR_RANGE);

    /* Full-width 64-bit integers round-trip exactly. */
    CANCESSTRY_TEST_CHECK(cancestry_codec_encode_signal(u64, uint_value(UINT64_MAX), frame,
                                                        sizeof(frame), NULL) ==
                          CANCESTRY_CODEC_OK);
    for (i = 0u; i < 8u; ++i) {
        CANCESSTRY_TEST_CHECK_U64(frame[i], 0xFFu);
    }
    CANCESSTRY_TEST_CHECK(cancestry_codec_encode_signal(i64, int_value(INT64_MAX), frame,
                                                        sizeof(frame), NULL) ==
                          CANCESTRY_CODEC_OK);
    CANCESSTRY_TEST_CHECK_U64(frame[7], 0x7Fu);
    CANCESSTRY_TEST_CHECK_U64(frame[0], 0xFFu);
    CANCESSTRY_TEST_CHECK(cancestry_codec_encode_signal(i64, int_value(INT64_MIN), frame,
                                                        sizeof(frame), NULL) ==
                          CANCESTRY_CODEC_OK);
    CANCESSTRY_TEST_CHECK_U64(frame[7], 0x80u);
}

static void test_boolean_encoding(void)
{
    const cancestry_codec_signal_t *brake = cancestry_codec_map_find_signal(test_map, "BrakePressed");
    uint8_t frame[8];
    size_t i;

    for (i = 0u; i < sizeof(frame); ++i) {
        frame[i] = 0u;
    }
    CANCESSTRY_TEST_CHECK(cancestry_codec_encode_signal(brake, bool_value(true), frame,
                                                        sizeof(frame), NULL) ==
                          CANCESTRY_CODEC_OK);
    CANCESSTRY_TEST_CHECK_U64(frame[3], 1u); /* payload bit 24 */
    CANCESSTRY_TEST_CHECK(cancestry_codec_encode_signal(brake, bool_value(false), frame,
                                                        sizeof(frame), NULL) ==
                          CANCESTRY_CODEC_OK);
    CANCESSTRY_TEST_CHECK_U64(frame[3], 0u);
    CANCESSTRY_TEST_CHECK(cancestry_codec_encode_signal(brake, int_value(1), frame,
                                                        sizeof(frame), NULL) ==
                          CANCESTRY_CODEC_OK);
    CANCESSTRY_TEST_CHECK_U64(frame[3], 1u);
    CANCESSTRY_TEST_CHECK(cancestry_codec_encode_signal(brake, uint_value(0u), frame,
                                                        sizeof(frame), NULL) ==
                          CANCESTRY_CODEC_OK);
    CANCESSTRY_TEST_CHECK_U64(frame[3], 0u);
    CANCESSTRY_TEST_CHECK(cancestry_codec_encode_signal(brake, int_value(2), frame,
                                                        sizeof(frame), NULL) ==
                          CANCESTRY_CODEC_ERR_RANGE);
}

static void test_enum_encoding_and_labels(void)
{
    const cancestry_codec_signal_t *gear = cancestry_codec_map_find_signal(test_map, "Gear");
    uint8_t frame[8];
    uint64_t raw = 0u;
    size_t i;

    for (i = 0u; i < sizeof(frame); ++i) {
        frame[i] = 0u;
    }
    CANCESSTRY_TEST_CHECK(cancestry_codec_encode_signal(gear, int_value(3), frame,
                                                        sizeof(frame), NULL) ==
                          CANCESTRY_CODEC_OK);
    CANCESSTRY_TEST_CHECK_U64(frame[3], 0x06u); /* bits 25..27 = 0b011 */
    CANCESSTRY_TEST_CHECK(cancestry_codec_encode_signal(gear, uint_value(2u), frame,
                                                        sizeof(frame), NULL) ==
                          CANCESTRY_CODEC_OK);
    CANCESSTRY_TEST_CHECK_U64(frame[3], 0x04u);
    /* An unmapped raw value fits the bit width and is accepted. */
    CANCESSTRY_TEST_CHECK(cancestry_codec_encode_signal(gear, int_value(7), frame,
                                                        sizeof(frame), NULL) ==
                          CANCESTRY_CODEC_OK);
    CANCESSTRY_TEST_CHECK_U64(frame[3], 0x0Eu);
    CANCESSTRY_TEST_CHECK(cancestry_codec_encode_signal(gear, int_value(8), frame,
                                                        sizeof(frame), NULL) ==
                          CANCESTRY_CODEC_ERR_RANGE);

    CANCESSTRY_TEST_CHECK(cancestry_codec_label_to_raw(gear, "DRIVE", &raw));
    CANCESSTRY_TEST_CHECK_U64(raw, 3u);
    CANCESSTRY_TEST_CHECK(cancestry_codec_label_to_raw(gear, "PARK", &raw));
    CANCESSTRY_TEST_CHECK_U64(raw, 0u);
    CANCESSTRY_TEST_CHECK(!cancestry_codec_label_to_raw(gear, "SPORT", &raw));
    CANCESSTRY_TEST_CHECK(!cancestry_codec_label_to_raw(gear, NULL, &raw));
    CANCESSTRY_TEST_CHECK(!cancestry_codec_label_to_raw(NULL, "DRIVE", &raw));
    CANCESSTRY_TEST_CHECK(!cancestry_codec_label_to_raw(gear, "DRIVE", NULL));
}

static void test_bit_isolation(void)
{
    const cancestry_codec_signal_t *be12 = cancestry_codec_map_find_signal(bit_map, "Be12");
    const cancestry_codec_signal_t *le12 = cancestry_codec_map_find_signal(bit_map, "Le12");
    uint8_t frame[8];
    size_t i;

    frame[0] = 0xA1u;
    frame[1] = 0x0Bu;
    frame[2] = 0xF0u;
    frame[3] = 0x0Fu;
    for (i = 4u; i < sizeof(frame); ++i) {
        frame[i] = 0u;
    }
    /* Be12 raw 0x037: signal bits 0..3 go to payload bits 4..7 (high nibble
     * of byte 0), bits 4..11 go to payload bits 8..15 (byte 1). Only those
     * bits change: byte0 = (0xA1 & 0x0F) | 0x70 = 0x71. */
    CANCESSTRY_TEST_CHECK(cancestry_codec_encode_signal(be12, uint_value(0x037u), frame,
                                                        sizeof(frame), NULL) ==
                          CANCESTRY_CODEC_OK);
    CANCESSTRY_TEST_CHECK_U64(frame[0], 0x71u);
    CANCESSTRY_TEST_CHECK_U64(frame[1], 0x03u);
    CANCESSTRY_TEST_CHECK_U64(frame[2], 0xF0u);
    CANCESSTRY_TEST_CHECK_U64(frame[3], 0x0Fu);

    /* Le12 raw 0x5D writes payload bits 5..16: raw bits 0..2 = 0b101 go to
     * byte0 bits 5..7 (0x71 -> 0xB1), byte1 = raw bits 3..10 = 0x0B,
     * byte2 bit 0 (raw bit 11) stays 0. */
    CANCESSTRY_TEST_CHECK(cancestry_codec_encode_signal(le12, uint_value(0x5Du), frame,
                                                        sizeof(frame), NULL) ==
                          CANCESTRY_CODEC_OK);
    CANCESSTRY_TEST_CHECK_U64(frame[0], 0xB1u);
    CANCESSTRY_TEST_CHECK_U64(frame[1], 0x0Bu);
    CANCESSTRY_TEST_CHECK_U64(frame[2], 0xF0u);
    CANCESSTRY_TEST_CHECK_U64(frame[3], 0x0Fu);
    for (i = 4u; i < 8u; ++i) {
        CANCESSTRY_TEST_CHECK_U64(frame[i], 0u);
    }
}

static void test_encode_determinism(void)
{
    const cancestry_codec_signal_t *gear = cancestry_codec_map_find_signal(test_map, "Gear");
    uint8_t first[8];
    uint8_t second[8];
    size_t i;

    for (i = 0u; i < 8u; ++i) {
        first[i] = 0xA5u;
        second[i] = 0xA5u;
    }
    CANCESSTRY_TEST_CHECK(cancestry_codec_encode_signal(gear, int_value(3), first, 8u, NULL) ==
                          CANCESTRY_CODEC_OK);
    CANCESSTRY_TEST_CHECK(cancestry_codec_encode_signal(gear, int_value(3), second, 8u, NULL) ==
                          CANCESTRY_CODEC_OK);
    CANCESSTRY_TEST_CHECK(memcmp(first, second, sizeof(first)) == 0);
    CANCESSTRY_TEST_CHECK_U64(first[3], 0xA5u | 0x06u);
}

int main(void)
{
    CANCESSTRY_TEST_SUITE_BEGIN("codec encoder");

    test_map = cancestry_test_load_map(cancestry_test_demo_yaml);
    bit_map = cancestry_test_load_map(cancestry_test_bit_yaml);
    rounding_map = cancestry_test_load_map(rounding_yaml);

    CANCESSTRY_TEST_CASE("decode then encode reproduces the demo frame");
    test_decode_encode_round_trip();

    CANCESSTRY_TEST_CASE("decode then encode reproduces the bit-soup frame");
    test_bit_map_round_trip();

    CANCESSTRY_TEST_CASE("rounding is round-to-nearest, ties away from zero");
    test_rounding_ties_away_from_zero();

    CANCESSTRY_TEST_CASE("declared min/max: strict rejects, non-strict clamps");
    test_limits_strict_and_clamp();

    CANCESSTRY_TEST_CASE("integer ranges, including full 64-bit round trips");
    test_integer_ranges();

    CANCESSTRY_TEST_CASE("boolean encoding accepts BOOL, INT and UINT 0/1");
    test_boolean_encoding();

    CANCESSTRY_TEST_CASE("enum encoding and label lookup");
    test_enum_encoding_and_labels();

    CANCESSTRY_TEST_CASE("read-modify-write leaves other bits alone");
    test_bit_isolation();

    CANCESSTRY_TEST_CASE("encode is deterministic");
    test_encode_determinism();

    cancestry_codec_map_free(test_map);
    cancestry_codec_map_free(bit_map);
    cancestry_codec_map_free(rounding_map);
    return CANCESSTRY_TEST_SUITE_END();
}
