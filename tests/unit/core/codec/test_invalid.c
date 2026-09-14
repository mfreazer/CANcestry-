/*
 * Unit tests for codec invalid-frame and argument handling.
 *
 * Verifies:
 *   SW-FR-CODEC-008  The software shall drop frames too short for declared
 *                    signals and raise a codec warning.
 *   SYS-NF-001       Deterministic error behavior.
 *
 * Traceability: CODEC-INVALID-FRAME-001
 *
 * Normative source: docs/packages/codec-map-spec.md section 8.
 */

#include "cancestry/codec/decoder.h"
#include "cancestry/codec/encoder.h"

#include "cancestry_codec_test.h"

#include <string.h>

static cancestry_codec_map_t *test_map = NULL;

static void test_frame_too_short_is_dropped(void)
{
    cancestry_decoded_signal_t signals[CANCESTRY_CODEC_DEMO_SIGNAL_COUNT];
    size_t count = 99u; /* sentinel: must not be touched on a drop */
    cancestry_codec_warnings_t warnings;
    const uint8_t frame[1] = {0u};
    size_t i;

    for (i = 0u; i < sizeof(signals) / sizeof(signals[0]); ++i) {
        signals[i].signal = NULL;
        signals[i].raw = 0xBADu;
    }
    memset(&warnings, 0, sizeof(warnings));

    /* A 1-byte frame cannot cover EngineSpeed (bits 0..15); the whole
     * message must be dropped with a warning and no signal updated. */
    CANCESSTRY_TEST_CHECK(cancestry_codec_decode_frame(
                              test_map, CANCESTRY_CODEC_DEMO_MESSAGE_ID, frame, sizeof(frame),
                              signals, sizeof(signals) / sizeof(signals[0]), &count,
                              &warnings) == CANCESTRY_CODEC_WARN_FRAME_TOO_SHORT);
    CANCESSTRY_TEST_CHECK_U64(count, 99u);
    CANCESSTRY_TEST_CHECK_U64(warnings.frame_too_short, 1u);
    for (i = 0u; i < sizeof(signals) / sizeof(signals[0]); ++i) {
        CANCESSTRY_TEST_CHECK(signals[i].signal == NULL);
        CANCESSTRY_TEST_CHECK_U64(signals[i].raw, 0xBADu);
    }

    /* A 4-byte frame fits the first signals but not BigCounter (bits
     * 32..63): still dropped. */
    {
        const uint8_t frame4[4] = {0u, 0u, 0u, 0u};
        CANCESSTRY_TEST_CHECK(cancestry_codec_decode_frame(
                                  test_map, CANCESTRY_CODEC_DEMO_MESSAGE_ID, frame4,
                                  sizeof(frame4), signals,
                                  sizeof(signals) / sizeof(signals[0]), &count,
                                  &warnings) == CANCESTRY_CODEC_WARN_FRAME_TOO_SHORT);
        CANCESSTRY_TEST_CHECK_U64(warnings.frame_too_short, 2u);
        CANCESSTRY_TEST_CHECK_U64(count, 99u);
    }

    /* Warnings may be NULL. */
    CANCESSTRY_TEST_CHECK(cancestry_codec_decode_frame(
                              test_map, CANCESTRY_CODEC_DEMO_MESSAGE_ID, frame, sizeof(frame),
                              signals, sizeof(signals) / sizeof(signals[0]), &count, NULL) ==
                          CANCESTRY_CODEC_WARN_FRAME_TOO_SHORT);
}

static void test_unknown_can_id(void)
{
    cancestry_decoded_signal_t signals[CANCESTRY_CODEC_DEMO_SIGNAL_COUNT];
    size_t count = 0u;
    cancestry_codec_warnings_t warnings;
    const uint8_t frame[8] = {0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u};

    memset(&warnings, 0, sizeof(warnings));
    CANCESSTRY_TEST_CHECK(cancestry_codec_decode_frame(
                              test_map, 0x777u, frame, sizeof(frame), signals,
                              sizeof(signals) / sizeof(signals[0]), &count, &warnings) ==
                          CANCESTRY_CODEC_ERR_NOT_FOUND);
    CANCESSTRY_TEST_CHECK_U64(warnings.frame_too_short, 0u);
    CANCESSTRY_TEST_CHECK_U64(warnings.enum_unknown, 0u);
    CANCESSTRY_TEST_CHECK_U64(count, 0u);
}

static void test_decode_arguments(void)
{
    cancestry_decoded_signal_t signals[CANCESTRY_CODEC_DEMO_SIGNAL_COUNT];
    size_t count = 0u;
    const uint8_t frame[8] = {0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u};

    CANCESSTRY_TEST_CHECK(cancestry_codec_decode_frame(
                              NULL, CANCESTRY_CODEC_DEMO_MESSAGE_ID, frame, sizeof(frame),
                              signals, sizeof(signals) / sizeof(signals[0]), &count, NULL) ==
                          CANCESTRY_CODEC_ERR_NULL);
    CANCESSTRY_TEST_CHECK(cancestry_codec_decode_frame(
                              test_map, CANCESTRY_CODEC_DEMO_MESSAGE_ID, NULL, sizeof(frame),
                              signals, sizeof(signals) / sizeof(signals[0]), &count, NULL) ==
                          CANCESTRY_CODEC_ERR_NULL);
    CANCESSTRY_TEST_CHECK(cancestry_codec_decode_frame(
                              test_map, CANCESTRY_CODEC_DEMO_MESSAGE_ID, frame, sizeof(frame),
                              signals, sizeof(signals) / sizeof(signals[0]), NULL, NULL) ==
                          CANCESTRY_CODEC_ERR_NULL);
    /* frame_length 0 and 9 are malformed. */
    CANCESSTRY_TEST_CHECK(cancestry_codec_decode_frame(
                              test_map, CANCESTRY_CODEC_DEMO_MESSAGE_ID, frame, 0u, signals,
                              sizeof(signals) / sizeof(signals[0]), &count, NULL) ==
                          CANCESTRY_CODEC_ERR_ARGUMENT);
    CANCESSTRY_TEST_CHECK(cancestry_codec_decode_frame(
                              test_map, CANCESTRY_CODEC_DEMO_MESSAGE_ID, frame, 9u, signals,
                              sizeof(signals) / sizeof(signals[0]), &count, NULL) ==
                          CANCESTRY_CODEC_ERR_ARGUMENT);
    /* NULL output storage or zero capacity. */
    CANCESSTRY_TEST_CHECK(cancestry_codec_decode_frame(
                              test_map, CANCESTRY_CODEC_DEMO_MESSAGE_ID, frame, sizeof(frame),
                              NULL, sizeof(signals) / sizeof(signals[0]), &count, NULL) ==
                          CANCESTRY_CODEC_ERR_ARGUMENT);
    CANCESSTRY_TEST_CHECK(cancestry_codec_decode_frame(
                              test_map, CANCESTRY_CODEC_DEMO_MESSAGE_ID, frame, sizeof(frame),
                              signals, 0u, &count, NULL) == CANCESTRY_CODEC_ERR_ARGUMENT);
    /* Capacity below the message's signal count. */
    CANCESSTRY_TEST_CHECK(cancestry_codec_decode_frame(
                              test_map, CANCESTRY_CODEC_DEMO_MESSAGE_ID, frame, sizeof(frame),
                              signals, CANCESTRY_CODEC_DEMO_SIGNAL_COUNT - 1u, &count, NULL) ==
                          CANCESTRY_CODEC_ERR_ARGUMENT);
}

static void test_decode_signal_arguments(void)
{
    const cancestry_codec_signal_t *signal = cancestry_codec_map_find_signal(test_map, "Gear");
    const uint8_t frame[8] = {0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u};
    cancestry_decoded_signal_t out;

    CANCESSTRY_TEST_CHECK(cancestry_codec_decode_signal(NULL, frame, sizeof(frame), &out) ==
                          CANCESTRY_CODEC_ERR_NULL);
    CANCESSTRY_TEST_CHECK(cancestry_codec_decode_signal(signal, NULL, sizeof(frame), &out) ==
                          CANCESTRY_CODEC_ERR_NULL);
    CANCESSTRY_TEST_CHECK(cancestry_codec_decode_signal(signal, frame, sizeof(frame), NULL) ==
                          CANCESTRY_CODEC_ERR_NULL);
    CANCESSTRY_TEST_CHECK(cancestry_codec_decode_signal(signal, frame, 0u, &out) ==
                          CANCESTRY_CODEC_ERR_ARGUMENT);
    CANCESSTRY_TEST_CHECK(cancestry_codec_decode_signal(signal, frame, 9u, &out) ==
                          CANCESTRY_CODEC_ERR_ARGUMENT);
    /* Gear spans bits 25..27: 4 bytes are enough, 3 are not. */
    CANCESSTRY_TEST_CHECK(cancestry_codec_decode_signal(signal, frame, 4u, &out) ==
                          CANCESTRY_CODEC_OK);
    CANCESSTRY_TEST_CHECK(cancestry_codec_decode_signal(signal, frame, 3u, &out) ==
                          CANCESTRY_CODEC_ERR_FRAME_TOO_SHORT);
}

static void test_encode_arguments(void)
{
    const cancestry_codec_signal_t *speed = cancestry_codec_map_find_signal(test_map, "EngineSpeed");
    const cancestry_codec_signal_t *counter =
        cancestry_codec_map_find_signal(test_map, "BigCounter");
    uint8_t frame[8];
    cancestry_value_t value;
    size_t i;

    for (i = 0u; i < sizeof(frame); ++i) {
        frame[i] = 0u;
    }
    value.kind = CANCESTRY_VALUE_KIND_REAL;
    value.value.real = 100.0;

    CANCESSTRY_TEST_CHECK(cancestry_codec_encode_signal(NULL, &value, frame, sizeof(frame),
                                                        NULL) == CANCESTRY_CODEC_ERR_NULL);
    CANCESSTRY_TEST_CHECK(cancestry_codec_encode_signal(speed, NULL, frame, sizeof(frame),
                                                        NULL) == CANCESTRY_CODEC_ERR_NULL);
    CANCESSTRY_TEST_CHECK(cancestry_codec_encode_signal(speed, &value, NULL, sizeof(frame),
                                                        NULL) == CANCESTRY_CODEC_ERR_NULL);
    CANCESSTRY_TEST_CHECK(cancestry_codec_encode_signal(speed, &value, frame, 0u, NULL) ==
                          CANCESTRY_CODEC_ERR_ARGUMENT);
    CANCESSTRY_TEST_CHECK(cancestry_codec_encode_signal(speed, &value, frame, 9u, NULL) ==
                          CANCESTRY_CODEC_ERR_ARGUMENT);

    /* EngineSpeed spans bits 0..15: 2 bytes fit, 1 does not. */
    CANCESSTRY_TEST_CHECK(cancestry_codec_encode_signal(speed, &value, frame, 2u, NULL) ==
                          CANCESTRY_CODEC_OK);
    CANCESSTRY_TEST_CHECK(cancestry_codec_encode_signal(speed, &value, frame, 1u, NULL) ==
                          CANCESTRY_CODEC_ERR_FRAME_TOO_SHORT);
    /* BigCounter spans bits 32..63: needs the full 8 bytes. */
    CANCESSTRY_TEST_CHECK(cancestry_codec_encode_signal(counter, &value, frame, 7u, NULL) ==
                          CANCESTRY_CODEC_ERR_FRAME_TOO_SHORT);
}

static void test_encode_value_kinds(void)
{
    const cancestry_codec_signal_t *speed = cancestry_codec_map_find_signal(test_map, "EngineSpeed");
    const cancestry_codec_signal_t *temp = cancestry_codec_map_find_signal(test_map, "CoolantTemp");
    const cancestry_codec_signal_t *gear = cancestry_codec_map_find_signal(test_map, "Gear");
    uint8_t frame[8];
    cancestry_value_t value;
    size_t i;

    for (i = 0u; i < sizeof(frame); ++i) {
        frame[i] = 0u;
    }

    /* REAL works for scaled uint/int signals. */
    value.kind = CANCESTRY_VALUE_KIND_REAL;
    value.value.real = 100.0;
    CANCESSTRY_TEST_CHECK(cancestry_codec_encode_signal(speed, &value, frame, sizeof(frame),
                                                        NULL) == CANCESTRY_CODEC_OK);
    CANCESSTRY_TEST_CHECK_U64(frame[0], 0x90u);
    CANCESSTRY_TEST_CHECK_U64(frame[1], 0x01u);

    /* NaN is rejected. */
    value.value.real = 0.0 / 0.0;
    CANCESSTRY_TEST_CHECK(cancestry_codec_encode_signal(speed, &value, frame, sizeof(frame),
                                                        NULL) == CANCESTRY_CODEC_ERR_ARGUMENT);
    /* Positive infinity cannot fit the bit range. */
    value.value.real = 1.0 / 0.0;
    CANCESSTRY_TEST_CHECK(cancestry_codec_encode_signal(speed, &value, frame, sizeof(frame),
                                                        NULL) == CANCESTRY_CODEC_ERR_RANGE);
    /* Negative infinity on a signed signal is out of range too. */
    value.value.real = -1.0 / 0.0;
    CANCESSTRY_TEST_CHECK(cancestry_codec_encode_signal(temp, &value, frame, sizeof(frame),
                                                        NULL) == CANCESTRY_CODEC_ERR_RANGE);

    /* Wrong kind for the signal type. */
    value.kind = CANCESTRY_VALUE_KIND_BOOL;
    value.value.boolean = true;
    CANCESSTRY_TEST_CHECK(cancestry_codec_encode_signal(speed, &value, frame, sizeof(frame),
                                                        NULL) == CANCESTRY_CODEC_ERR_ARGUMENT);
    CANCESSTRY_TEST_CHECK(cancestry_codec_encode_signal(gear, &value, frame, sizeof(frame),
                                                        NULL) == CANCESTRY_CODEC_ERR_ARGUMENT);

    value.kind = CANCESTRY_VALUE_KIND_INT;
    value.value.integer = 5;
    CANCESSTRY_TEST_CHECK(cancestry_codec_encode_signal(gear, &value, frame, sizeof(frame),
                                                        NULL) == CANCESTRY_CODEC_OK);
    value.value.integer = -1;
    CANCESSTRY_TEST_CHECK(cancestry_codec_encode_signal(gear, &value, frame, sizeof(frame),
                                                        NULL) == CANCESTRY_CODEC_ERR_RANGE);

    value.kind = CANCESTRY_VALUE_KIND_UNSET;
    CANCESSTRY_TEST_CHECK(cancestry_codec_encode_signal(speed, &value, frame, sizeof(frame),
                                                        NULL) == CANCESTRY_CODEC_ERR_ARGUMENT);
}

int main(void)
{
    CANCESSTRY_TEST_SUITE_BEGIN("codec invalid frames and arguments");

    test_map = cancestry_test_load_map(cancestry_test_demo_yaml);

    CANCESSTRY_TEST_CASE("too-short frames are dropped without touching outputs");
    test_frame_too_short_is_dropped();

    CANCESSTRY_TEST_CASE("unknown can id is NOT_FOUND with no warning");
    test_unknown_can_id();

    CANCESSTRY_TEST_CASE("decode argument validation");
    test_decode_arguments();

    CANCESSTRY_TEST_CASE("per-signal decode argument validation");
    test_decode_signal_arguments();

    CANCESSTRY_TEST_CASE("encode argument and frame length validation");
    test_encode_arguments();

    CANCESSTRY_TEST_CASE("encode value kind validation and non-finite values");
    test_encode_value_kinds();

    cancestry_codec_map_free(test_map);
    return CANCESSTRY_TEST_SUITE_END();
}
