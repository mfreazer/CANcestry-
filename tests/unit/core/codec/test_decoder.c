/*
 * Unit tests for the codec decoder.
 *
 * Verifies:
 *   SW-FR-CODEC-001  The software shall decode CAN frames into named signals.
 *   SW-FR-CODEC-003  Little- and big-endian signals, canonical LSB0 bit model.
 *   SW-FR-CODEC-004  Scaling and offset.
 *   SW-FR-CODEC-005  Signed and unsigned integer signals.
 *   SW-FR-CODEC-006  Boolean and enum value mappings.
 *   SYS-NF-001       Determinism: same input, bit-identical output.
 *   QA-H02           Bit-level semantics correctness.
 *
 * Traceability: CODEC-DECODE-001, CODEC-BITPACK-001, CODEC-BITPACK-002,
 *               CODEC-SCALING-001, CODEC-SIGNED-001, CODEC-BOOL-ENUM-001,
 *               CODEC-DETERMINISM-001
 *
 * Normative source: docs/packages/codec-map-spec.md sections 3-8.
 */

#include "cancestry/codec/decoder.h"

#include "cancestry_codec_test.h"

#include <string.h>

static cancestry_codec_map_t *test_map = NULL;
static cancestry_codec_map_t *bit_map = NULL;
static cancestry_codec_warnings_t test_warnings;

static const cancestry_codec_signal_t *signal_of(const cancestry_codec_map_t *map,
                                                 const char *name)
{
    return cancestry_codec_map_find_signal(map, name);
}

static void test_little_endian_basics(void)
{
    const cancestry_codec_signal_t *signal = signal_of(test_map, "EngineSpeed");
    const uint8_t frame[8] = {0x34u, 0x12u, 0u, 0u, 0u, 0u, 0u, 0u};
    cancestry_decoded_signal_t out;

    CANCESSTRY_TEST_CHECK(signal != NULL);
    CANCESSTRY_TEST_CHECK(signal->first_bit == 0u);
    CANCESSTRY_TEST_CHECK(signal->last_bit == 15u);

    CANCESSTRY_TEST_CHECK(cancestry_codec_decode_signal(signal, frame, sizeof(frame), &out) ==
                          CANCESTRY_CODEC_OK);
    CANCESSTRY_TEST_CHECK_U64(out.raw, 0x1234u);
    CANCESSTRY_TEST_CHECK(out.value.kind == CANCESTRY_VALUE_KIND_REAL);
    CANCESSTRY_TEST_CHECK_DOUBLE(out.value.value.real, 1165.0, 1e-6);
    CANCESSTRY_TEST_CHECK(out.label == NULL);
}

static void test_big_endian_within_byte(void)
{
    /* A big-endian signal with MSB at bit 23 occupies payload bits 16..23. */
    const cancestry_codec_signal_t *signal = signal_of(test_map, "CoolantTemp");
    const uint8_t frame[8] = {0u, 0u, 0x80u, 0u, 0u, 0u, 0u, 0u};
    cancestry_decoded_signal_t out;

    CANCESSTRY_TEST_CHECK(signal != NULL);
    CANCESSTRY_TEST_CHECK(signal->first_bit == 16u);
    CANCESSTRY_TEST_CHECK(signal->last_bit == 23u);

    CANCESSTRY_TEST_CHECK(cancestry_codec_decode_signal(signal, frame, sizeof(frame), &out) ==
                          CANCESTRY_CODEC_OK);
    /* 0x80 as int8 is -128; physical = raw + offset = -128 + -40 = -168. */
    CANCESSTRY_TEST_CHECK_U64(out.raw, 0x80u);
    CANCESSTRY_TEST_CHECK(out.value.kind == CANCESTRY_VALUE_KIND_REAL);
    CANCESSTRY_TEST_CHECK_DOUBLE(out.value.value.real, -168.0, 1e-6);
}

static void test_bit_soup(void)
{
    cancestry_decoded_signal_t signals[8];
    size_t count = 0u;
    size_t i;
    const uint8_t frame[8] = {0xA1u, 0x0Bu, 0xF0u, 0x0Fu, 0x00u, 0x00u, 0x00u, 0x00u};

    CANCESSTRY_TEST_CHECK(cancestry_codec_decode_frame(bit_map, CANCESTRY_CODEC_BIT_MESSAGE_ID,
                                                       frame, sizeof(frame), signals,
                                                       sizeof(signals) / sizeof(signals[0]),
                                                       &count, &test_warnings) ==
                          CANCESTRY_CODEC_OK);
    CANCESSTRY_TEST_CHECK_U64(count, 8u);
    for (i = 0u; i < count; ++i) {
        const cancestry_decoded_signal_t *decoded = &signals[i];

        if (strcmp(decoded->signal->name, "Bit0") == 0) {
            /* data[0] = 0xA1: bit 0 is set. */
            CANCESSTRY_TEST_CHECK(decoded->value.kind == CANCESTRY_VALUE_KIND_UINT);
            CANCESSTRY_TEST_CHECK_U64(decoded->raw, 1u);
        } else if (strcmp(decoded->signal->name, "Bit7") == 0) {
            /* data[0] = 0xA1: bit 7 is set. */
            CANCESSTRY_TEST_CHECK(decoded->value.kind == CANCESTRY_VALUE_KIND_UINT);
            CANCESSTRY_TEST_CHECK_U64(decoded->raw, 1u);
        } else if (strcmp(decoded->signal->name, "Le12") == 0) {
            /* bits 5..16: data[0] bit5/bit7 set (0xA1), data[1] = 0x0B
             * -> raw = 1 | 4 | 8 | 16 | 64 = 93. */
            CANCESSTRY_TEST_CHECK(decoded->value.kind == CANCESTRY_VALUE_KIND_UINT);
            CANCESSTRY_TEST_CHECK_U64(decoded->raw, 93u);
        } else if (strcmp(decoded->signal->name, "Be12") == 0) {
            /* bits 4..15: data[0] = 0xA1 bits 4..7 = 0101, data[1] = 0x0B
             * bits 8..15 -> raw = 2 + 8 + 16 + 32 + 128 = 186. */
            CANCESSTRY_TEST_CHECK(decoded->value.kind == CANCESTRY_VALUE_KIND_UINT);
            CANCESSTRY_TEST_CHECK_U64(decoded->raw, 186u);
        } else if (strcmp(decoded->signal->name, "Int12Le") == 0) {
            /* Bits 20..31: data[2] = 0xF0 -> raw bits 0..3 = 1111,
             * data[3] = 0x0F -> raw bits 4..7 = 1111, bits 8..11 = 0000.
             * raw = 0xFF, sign bit clear -> +255. */
            CANCESSTRY_TEST_CHECK(decoded->value.kind == CANCESTRY_VALUE_KIND_INT);
            CANCESSTRY_TEST_CHECK_U64(decoded->raw, 0xFFu);
            CANCESSTRY_TEST_CHECK_I64(decoded->value.value.integer, 255);
        } else if (strcmp(decoded->signal->name, "Int12Be") == 0) {
            /* Same span 20..31 -> raw = 0xFF, sign bit clear -> +255. */
            CANCESSTRY_TEST_CHECK(decoded->value.kind == CANCESTRY_VALUE_KIND_INT);
            CANCESSTRY_TEST_CHECK_U64(decoded->raw, 0xFFu);
            CANCESSTRY_TEST_CHECK_I64(decoded->value.value.integer, 255);
        } else if (strcmp(decoded->signal->name, "Le64") == 0) {
            /* Whole frame, little-endian. */
            CANCESSTRY_TEST_CHECK(decoded->value.kind == CANCESTRY_VALUE_KIND_UINT);
            CANCESSTRY_TEST_CHECK_U64(decoded->raw, UINT64_C(0x000000000FF00BA1));
        } else if (strcmp(decoded->signal->name, "Be64") == 0) {
            /* start 63, len 64: first_bit = 0, identical to Le64. */
            CANCESSTRY_TEST_CHECK(decoded->value.kind == CANCESTRY_VALUE_KIND_UINT);
            CANCESSTRY_TEST_CHECK_U64(decoded->raw, UINT64_C(0x000000000FF00BA1));
        } else {
            CANCESSTRY_TEST_CHECK(false);
        }
    }
}

static void test_be12_explicit_vector(void)
{
    /* Big-endian 12-bit at start_bit 15 occupies payload bits 4..15: the
     * high nibble of byte 0 carries the signal's low 4 bits. */
    const cancestry_codec_signal_t *signal = signal_of(bit_map, "Be12");
    const uint8_t frame[8] = {0xA0u, 0x0Bu, 0u, 0u, 0u, 0u, 0u, 0u};
    cancestry_decoded_signal_t out;

    CANCESSTRY_TEST_CHECK(cancestry_codec_decode_signal(signal, frame, sizeof(frame), &out) ==
                          CANCESTRY_CODEC_OK);
    CANCESSTRY_TEST_CHECK_U64(out.raw, 0xBAu);

    /* Signal bit i maps to payload bit start_bit - (bit_length - 1) + i. */
    CANCESSTRY_TEST_CHECK(signal->first_bit == 4u);
    CANCESSTRY_TEST_CHECK(signal->last_bit == 15u);
}

static void test_le12_explicit_vector(void)
{
    /* Little-endian 12-bit at start_bit 5 occupies payload bits 5..16. */
    const cancestry_codec_signal_t *signal = signal_of(bit_map, "Le12");
    const uint8_t frame[8] = {0x20u, 0x01u, 0u, 0u, 0u, 0u, 0u, 0u};
    cancestry_decoded_signal_t out;

    CANCESSTRY_TEST_CHECK(cancestry_codec_decode_signal(signal, frame, sizeof(frame), &out) ==
                          CANCESTRY_CODEC_OK);
    CANCESSTRY_TEST_CHECK_U64(out.raw, 9u);
    CANCESSTRY_TEST_CHECK(signal->first_bit == 5u);
    CANCESSTRY_TEST_CHECK(signal->last_bit == 16u);
}

static void test_full_frame_decode(void)
{
    cancestry_decoded_signal_t signals[CANCESTRY_CODEC_DEMO_SIGNAL_COUNT];
    size_t count = 0u;
    size_t i;
    const uint8_t frame[8] = {0x34u, 0x12u, 0x80u, 0x06u, 0xDEu, 0xADu, 0xBEu, 0xEFu};

    CANCESSTRY_TEST_CHECK(cancestry_codec_decode_frame(
                              test_map, CANCESTRY_CODEC_DEMO_MESSAGE_ID, frame, sizeof(frame),
                              signals, sizeof(signals) / sizeof(signals[0]), &count,
                              &test_warnings) == CANCESTRY_CODEC_OK);
    CANCESSTRY_TEST_CHECK_U64(count, CANCESTRY_CODEC_DEMO_SIGNAL_COUNT);

    for (i = 0u; i < count; ++i) {
        const cancestry_decoded_signal_t *decoded = &signals[i];

        if (strcmp(decoded->signal->name, "EngineSpeed") == 0) {
            CANCESSTRY_TEST_CHECK_U64(decoded->raw, 0x1234u);
            CANCESSTRY_TEST_CHECK(decoded->value.kind == CANCESTRY_VALUE_KIND_REAL);
            CANCESSTRY_TEST_CHECK_DOUBLE(decoded->value.value.real, 1165.0, 1e-6);
        } else if (strcmp(decoded->signal->name, "CoolantTemp") == 0) {
            CANCESSTRY_TEST_CHECK_U64(decoded->raw, 0x80u);
            CANCESSTRY_TEST_CHECK_DOUBLE(decoded->value.value.real, -168.0, 1e-6);
        } else if (strcmp(decoded->signal->name, "BrakePressed") == 0) {
            /* data[3] = 0x06: bit 0 clear -> RELEASED. */
            CANCESSTRY_TEST_CHECK(decoded->value.kind == CANCESTRY_VALUE_KIND_BOOL);
            CANCESSTRY_TEST_CHECK(decoded->value.value.boolean == false);
            CANCESSTRY_TEST_CHECK_STRING(decoded->label, "RELEASED");
        } else if (strcmp(decoded->signal->name, "Gear") == 0) {
            /* data[3] = 0x06: bits 1..3 = 0b011 -> DRIVE. */
            CANCESSTRY_TEST_CHECK_U64(decoded->raw, 3u);
            CANCESSTRY_TEST_CHECK(decoded->value.kind == CANCESTRY_VALUE_KIND_INT);
            CANCESSTRY_TEST_CHECK_I64(decoded->value.value.integer, 3);
            CANCESSTRY_TEST_CHECK_STRING(decoded->label, "DRIVE");
        } else if (strcmp(decoded->signal->name, "BigCounter") == 0) {
            /* BE uint32 with MSB at payload bit 63: per the spec formula the
             * signal LSB sits at payload bit 32 (byte 4 bit 0), so the raw
             * value reads bytes 4..7 least-significant-first:
             * 0xDE + 0xAD<<8 + 0xBE<<16 + 0xEF<<24 = 0xEFBEADDE. */
            CANCESSTRY_TEST_CHECK_U64(decoded->raw, UINT64_C(0xEFBEADDE));
            CANCESSTRY_TEST_CHECK(decoded->value.kind == CANCESTRY_VALUE_KIND_UINT);
            CANCESSTRY_TEST_CHECK_U64(decoded->value.value.unsigned_integer,
                                      UINT64_C(0xEFBEADDE));
        } else {
            CANCESSTRY_TEST_CHECK(false);
        }
    }
}

static void test_enum_unknown_label(void)
{
    cancestry_decoded_signal_t signals[CANCESTRY_CODEC_DEMO_SIGNAL_COUNT];
    size_t count = 0u;
    cancestry_codec_warnings_t warnings = {0u, 0u, 0u};
    const uint8_t frame[8] = {0u, 0u, 0u, 0x0Eu, 0u, 0u, 0u, 0u};

    /* data[3] = 0x0E: gear bits = 0b111 = 7, which has no label. */
    CANCESSTRY_TEST_CHECK(cancestry_codec_decode_frame(
                              test_map, CANCESTRY_CODEC_DEMO_MESSAGE_ID, frame, sizeof(frame),
                              signals, sizeof(signals) / sizeof(signals[0]), &count,
                              &warnings) == CANCESTRY_CODEC_OK);
    CANCESSTRY_TEST_CHECK_U64(count, CANCESTRY_CODEC_DEMO_SIGNAL_COUNT);
    CANCESSTRY_TEST_CHECK_U64(warnings.enum_unknown, 1u);
    CANCESSTRY_TEST_CHECK(signals[3].label == NULL);
    CANCESSTRY_TEST_CHECK_I64(signals[3].value.value.integer, 7);
}

static void test_extra_bytes_ignored(void)
{
    /* The demo message declares dlc 8; a longer-than-declared frame is fine
     * because extra bytes are ignored (spec section 8). */
    cancestry_decoded_signal_t signals[CANCESTRY_CODEC_DEMO_SIGNAL_COUNT];
    size_t count = 0u;
    const uint8_t frame[8] = {0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u};

    CANCESSTRY_TEST_CHECK(cancestry_codec_decode_frame(
                              test_map, CANCESTRY_CODEC_DEMO_MESSAGE_ID, frame, 8u, signals,
                              sizeof(signals) / sizeof(signals[0]), &count, NULL) ==
                          CANCESTRY_CODEC_OK);
    CANCESSTRY_TEST_CHECK_U64(count, CANCESTRY_CODEC_DEMO_SIGNAL_COUNT);
}

static void test_determinism(void)
{
    cancestry_decoded_signal_t first[CANCESTRY_CODEC_DEMO_SIGNAL_COUNT];
    cancestry_decoded_signal_t second[CANCESTRY_CODEC_DEMO_SIGNAL_COUNT];
    size_t first_count = 0u;
    size_t second_count = 0u;
    size_t i;
    const uint8_t frame[8] = {0x34u, 0x12u, 0x80u, 0x06u, 0xDEu, 0xADu, 0xBEu, 0xEFu};

    CANCESSTRY_TEST_CHECK(cancestry_codec_decode_frame(
                              test_map, CANCESTRY_CODEC_DEMO_MESSAGE_ID, frame, sizeof(frame),
                              first, sizeof(first) / sizeof(first[0]), &first_count, NULL) ==
                          CANCESTRY_CODEC_OK);
    CANCESSTRY_TEST_CHECK(cancestry_codec_decode_frame(
                              test_map, CANCESTRY_CODEC_DEMO_MESSAGE_ID, frame, sizeof(frame),
                              second, sizeof(second) / sizeof(second[0]), &second_count, NULL) ==
                          CANCESTRY_CODEC_OK);
    CANCESSTRY_TEST_CHECK(first_count == second_count);
    for (i = 0u; i < first_count; ++i) {
        CANCESSTRY_TEST_CHECK(first[i].signal == second[i].signal);
        CANCESSTRY_TEST_CHECK_U64(first[i].raw, second[i].raw);
        CANCESSTRY_TEST_CHECK(first[i].value.kind == second[i].value.kind);
        switch (first[i].value.kind) {
        case CANCESTRY_VALUE_KIND_BOOL:
            CANCESSTRY_TEST_CHECK(first[i].value.value.boolean ==
                                  second[i].value.value.boolean);
            break;
        case CANCESTRY_VALUE_KIND_INT:
            CANCESSTRY_TEST_CHECK_I64(first[i].value.value.integer,
                                      second[i].value.value.integer);
            break;
        case CANCESTRY_VALUE_KIND_UINT:
            CANCESSTRY_TEST_CHECK_U64(first[i].value.value.unsigned_integer,
                                      second[i].value.value.unsigned_integer);
            break;
        case CANCESTRY_VALUE_KIND_REAL:
            CANCESSTRY_TEST_CHECK_DOUBLE(first[i].value.value.real, second[i].value.value.real,
                                         0.0);
            break;
        default:
            CANCESSTRY_TEST_CHECK(false);
            break;
        }
        if (first[i].label != NULL) {
            CANCESSTRY_TEST_CHECK(second[i].label != NULL);
            CANCESSTRY_TEST_CHECK_STRING(first[i].label, second[i].label);
        } else {
            CANCESSTRY_TEST_CHECK(second[i].label == NULL);
        }
    }
}

static void test_lookup_helpers(void)
{
    const cancestry_codec_message_t *message =
        cancestry_codec_map_find_message(test_map, CANCESTRY_CODEC_DEMO_MESSAGE_ID);

    CANCESSTRY_TEST_CHECK(message != NULL);
    CANCESSTRY_TEST_CHECK_STRING(message->name, "Engine");
    CANCESSTRY_TEST_CHECK_U64(message->dlc, 8u);
    CANCESSTRY_TEST_CHECK_U64(message->period_ms, 10u);
    CANCESSTRY_TEST_CHECK_U64(message->signal_count, CANCESTRY_CODEC_DEMO_SIGNAL_COUNT);

    CANCESSTRY_TEST_CHECK(cancestry_codec_map_find_message(test_map, 0x777u) == NULL);
    CANCESSTRY_TEST_CHECK(cancestry_codec_map_find_message(NULL, 0u) == NULL);
    CANCESSTRY_TEST_CHECK(cancestry_codec_map_find_signal(test_map, "Gear") != NULL);
    CANCESSTRY_TEST_CHECK(cancestry_codec_map_find_signal(test_map, "Missing") == NULL);
    CANCESSTRY_TEST_CHECK(cancestry_codec_map_find_signal(test_map, NULL) == NULL);
    CANCESSTRY_TEST_CHECK(cancestry_codec_map_find_signal(NULL, "Gear") == NULL);
}

static void test_name_helpers(void)
{
    CANCESSTRY_TEST_CHECK_STRING(cancestry_codec_status_name(CANCESTRY_CODEC_OK), "OK");
    CANCESSTRY_TEST_CHECK_STRING(cancestry_codec_status_name(CANCESTRY_CODEC_ERR_RANGE),
                                 "ERR_RANGE");
    CANCESSTRY_TEST_CHECK_STRING(cancestry_codec_signal_type_name(
                                     CANCESTRY_CODEC_SIGNAL_TYPE_BOOLEAN),
                                 "boolean");
    CANCESSTRY_TEST_CHECK_STRING(
        cancestry_codec_endianness_name(CANCESTRY_CODEC_ENDIANNESS_BIG), "big");
    CANCESSTRY_TEST_CHECK(cancestry_codec_status_is_warning(CANCESTRY_CODEC_WARN_ENUM_UNKNOWN));
    CANCESSTRY_TEST_CHECK(!cancestry_codec_status_is_warning(CANCESTRY_CODEC_OK));
    CANCESSTRY_TEST_CHECK(!cancestry_codec_status_is_warning(CANCESTRY_CODEC_ERR_PARSE));
}

int main(void)
{
    CANCESSTRY_TEST_SUITE_BEGIN("codec decoder");

    test_map = cancestry_test_load_map(cancestry_test_demo_yaml);
    bit_map = cancestry_test_load_map(cancestry_test_bit_yaml);
    memset(&test_warnings, 0, sizeof(test_warnings));

    CANCESSTRY_TEST_CASE("little-endian 16-bit scaling decode");
    test_little_endian_basics();

    CANCESSTRY_TEST_CASE("big-endian int8 with offset");
    test_big_endian_within_byte();

    CANCESSTRY_TEST_CASE("bit soup: LE/BE spans, signed, 64-bit");
    test_bit_soup();

    CANCESSTRY_TEST_CASE("big-endian 12-bit explicit bit vector");
    test_be12_explicit_vector();

    CANCESSTRY_TEST_CASE("little-endian 12-bit explicit bit vector");
    test_le12_explicit_vector();

    CANCESSTRY_TEST_CASE("full frame decode with all value kinds");
    test_full_frame_decode();

    CANCESSTRY_TEST_CASE("unknown enum value warns and leaves the label NULL");
    test_enum_unknown_label();

    CANCESSTRY_TEST_CASE("extra bytes beyond the declared dlc are ignored");
    test_extra_bytes_ignored();

    CANCESSTRY_TEST_CASE("decode is deterministic");
    test_determinism();

    CANCESSTRY_TEST_CASE("map lookup helpers");
    test_lookup_helpers();

    CANCESSTRY_TEST_CASE("status and enum name helpers");
    test_name_helpers();

    cancestry_codec_map_free(test_map);
    cancestry_codec_map_free(bit_map);
    return CANCESSTRY_TEST_SUITE_END();
}
