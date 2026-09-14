/*
 * Unit tests for the codec map loader.
 *
 * Verifies:
 *   SW-FR-CODEC-007  Signal definition conflicts (duplicate ids, duplicate
 *                    signal names) are detected.
 *   SYS-NF-001       Deterministic parse/validation.
 *   SYS-NF-008       Every schema constraint of
 *                    schemas/codec-map-0.2.0.schema.json is enforced.
 *
 * Traceability: CODEC-CONFLICT-001, CODEC-DETERMINISM-001
 *
 * Normative source: schemas/codec-map-0.2.0.schema.json,
 *                   docs/packages/codec-map-spec.md.
 */

#include "cancestry/codec/loader.h"

#include "cancestry_codec_test.h"

#include <stdio.h>
#include <string.h>

static const char *const minimal_yaml =
    "schema_version: \"0.2.0\"\n"
    "codec_map:\n"
    "  name: minimal\n"
    "  version: 1.0.0\n"
    "  messages:\n"
    "    - id: 100\n"
    "      name: M\n"
    "      dlc: 8\n"
    "      signals:\n"
    "        - name: S\n"
    "          start_bit: 0\n"
    "          bit_length: 8\n"
    "          type: uint\n"
    "          endianness: little\n";

static const char *const full_yaml =
    "---\n"
    "# a leading comment\n"
    "schema_version: \"0.2.0\"  # trailing comment\n"
    "codec_map:\n"
    "  name: 'full'\n"
    "  version: 2.1.0\n"
    "  description: \"A \\\"quoted\\\" caf\\u00e9\"\n"
    "  messages:\n"
    "    - id: 0x1A0\n"
    "      name: Msg\n"
    "      dlc: 0o10\n"
    "      period_ms: 100\n"
    "      description: 'it''s a message'\n"
    "      signals:\n"
    "        - name: Speed\n"
    "          start_bit: 0\n"
    "          bit_length: 16\n"
    "          type: uint\n"
    "          endianness: little\n"
    "          scale: 0.1\n"
    "          offset: 1\n"
    "          unit: \"km/h\"\n"
    "          min: 0.0\n"
    "          max: 6500\n"
    "          strict: false\n"
    "        - name: Mode\n"
    "          start_bit: 16\n"
    "          bit_length: 2\n"
    "          type: enum\n"
    "          endianness: little\n"
    "          values:\n"
    "            \"0\": \"OFF\"\n"
    "            \"1\": \"ON\"\n"
    "...\n";

/* Build a document with one configurable signal block. */
static const char *signal_doc(const char *signal_block)
{
    static char buffer[1024];
    (void)snprintf(buffer, sizeof(buffer),
                   "schema_version: \"0.2.0\"\n"
                   "codec_map:\n"
                   "  name: demo\n"
                   "  version: 1.0.0\n"
                   "  messages:\n"
                   "    - id: 100\n"
                   "      name: M\n"
                   "      dlc: 8\n"
                   "      signals:\n"
                   "%s",
                   signal_block);
    return buffer;
}

static void check_load_fails(const char *yaml, cancestry_codec_status_t expected)
{
    cancestry_codec_load_error_t error;
    cancestry_codec_map_t *map = cancestry_codec_map_load(yaml, strlen(yaml), &error);

    CANCESSTRY_TEST_CHECK(map == NULL);
    CANCESSTRY_TEST_CHECK(error.status == expected);
    CANCESSTRY_TEST_CHECK(error.line >= 1u);
    CANCESSTRY_TEST_CHECK(error.message[0] != '\0');
}

static void test_minimal_defaults(void)
{
    cancestry_codec_map_t *map = cancestry_test_load_map(minimal_yaml);

    CANCESSTRY_TEST_CHECK(map != NULL);
    CANCESSTRY_TEST_CHECK_STRING(map->name, "minimal");
    CANCESSTRY_TEST_CHECK_STRING(map->version, "1.0.0");
    CANCESSTRY_TEST_CHECK(map->description == NULL);
    CANCESSTRY_TEST_CHECK_U64(map->message_count, 1u);

    CANCESSTRY_TEST_CHECK_U64(map->messages[0].id, 100u);
    CANCESSTRY_TEST_CHECK_STRING(map->messages[0].name, "M");
    CANCESSTRY_TEST_CHECK_U64(map->messages[0].dlc, 8u);
    CANCESSTRY_TEST_CHECK_U64(map->messages[0].period_ms, 0u);
    CANCESSTRY_TEST_CHECK(map->messages[0].description == NULL);
    CANCESSTRY_TEST_CHECK_U64(map->messages[0].signal_count, 1u);

    CANCESSTRY_TEST_CHECK_STRING(map->messages[0].signals[0].name, "S");
    CANCESSTRY_TEST_CHECK_U64(map->messages[0].signals[0].start_bit, 0u);
    CANCESSTRY_TEST_CHECK_U64(map->messages[0].signals[0].bit_length, 8u);
    CANCESSTRY_TEST_CHECK(map->messages[0].signals[0].type ==
                          CANCESTRY_CODEC_SIGNAL_TYPE_UINT);
    CANCESSTRY_TEST_CHECK(map->messages[0].signals[0].endianness ==
                          CANCESTRY_CODEC_ENDIANNESS_LITTLE);
    CANCESSTRY_TEST_CHECK_DOUBLE(map->messages[0].signals[0].scale, 1.0, 0.0);
    CANCESSTRY_TEST_CHECK_DOUBLE(map->messages[0].signals[0].offset, 0.0, 0.0);
    CANCESSTRY_TEST_CHECK(map->messages[0].signals[0].unit == NULL);
    CANCESSTRY_TEST_CHECK(map->messages[0].signals[0].values == NULL);
    CANCESSTRY_TEST_CHECK_U64(map->messages[0].signals[0].value_count, 0u);
    CANCESSTRY_TEST_CHECK(map->messages[0].signals[0].strict == true);
    CANCESSTRY_TEST_CHECK(map->messages[0].signals[0].has_min == false);
    CANCESSTRY_TEST_CHECK(map->messages[0].signals[0].has_max == false);
    CANCESSTRY_TEST_CHECK_U64(map->messages[0].signals[0].first_bit, 0u);
    CANCESSTRY_TEST_CHECK_U64(map->messages[0].signals[0].last_bit, 7u);
    CANCESSTRY_TEST_CHECK_U64(map->messages[0].signals[0].message_id, 100u);

    cancestry_codec_map_free(map);
}

static void test_full_featured(void)
{
    cancestry_codec_map_t *map = cancestry_test_load_map(full_yaml);
    const cancestry_codec_message_t *message;
    const cancestry_codec_signal_t *speed;
    const cancestry_codec_signal_t *mode;

    CANCESSTRY_TEST_CHECK(map != NULL);
    CANCESSTRY_TEST_CHECK_STRING(map->name, "full");
    CANCESSTRY_TEST_CHECK_STRING(map->version, "2.1.0");
    /* Escapes: \" and \u00e9 decode. */
    CANCESSTRY_TEST_CHECK_STRING(map->description, "A \"quoted\" caf\xC3\xA9");

    message = &map->messages[0];
    CANCESSTRY_TEST_CHECK_U64(message->id, 0x1A0u);
    CANCESSTRY_TEST_CHECK_U64(message->dlc, 8u); /* 0o10 */
    CANCESSTRY_TEST_CHECK_U64(message->period_ms, 100u);
    CANCESSTRY_TEST_CHECK_STRING(message->description, "it's a message");
    CANCESSTRY_TEST_CHECK_U64(message->signal_count, 2u);

    speed = &message->signals[0];
    CANCESSTRY_TEST_CHECK_STRING(speed->name, "Speed");
    CANCESSTRY_TEST_CHECK_DOUBLE(speed->scale, 0.1, 0.0);
    CANCESSTRY_TEST_CHECK_DOUBLE(speed->offset, 1.0, 0.0);
    CANCESSTRY_TEST_CHECK_STRING(speed->unit, "km/h");
    CANCESSTRY_TEST_CHECK(speed->has_min);
    CANCESSTRY_TEST_CHECK(speed->has_max);
    CANCESSTRY_TEST_CHECK_DOUBLE(speed->min, 0.0, 0.0);
    CANCESSTRY_TEST_CHECK_DOUBLE(speed->max, 6500.0, 0.0);
    CANCESSTRY_TEST_CHECK(speed->strict == false);
    CANCESSTRY_TEST_CHECK_U64(speed->first_bit, 0u);
    CANCESSTRY_TEST_CHECK_U64(speed->last_bit, 15u);

    mode = &message->signals[1];
    CANCESSTRY_TEST_CHECK(mode->type == CANCESTRY_CODEC_SIGNAL_TYPE_ENUM);
    CANCESSTRY_TEST_CHECK_U64(mode->value_count, 2u);
    CANCESSTRY_TEST_CHECK_U64(mode->values[0].raw, 0u);
    CANCESSTRY_TEST_CHECK_STRING(mode->values[0].name, "OFF");
    CANCESSTRY_TEST_CHECK_U64(mode->values[1].raw, 1u);
    CANCESSTRY_TEST_CHECK_STRING(mode->values[1].name, "ON");

    cancestry_codec_map_free(map);
}

static void test_big_endian_derived_bits(void)
{
    /* Derived first_bit/last_bit for a big-endian signal. */
    cancestry_codec_map_t *map = cancestry_test_load_map(signal_doc(
        "        - name: Be12\n"
        "          start_bit: 15\n"
        "          bit_length: 12\n"
        "          type: uint\n"
        "          endianness: big\n"));
    CANCESSTRY_TEST_CHECK(map != NULL);
    CANCESSTRY_TEST_CHECK_U64(map->messages[0].signals[0].first_bit, 4u);
    CANCESSTRY_TEST_CHECK_U64(map->messages[0].signals[0].last_bit, 15u);
    cancestry_codec_map_free(map);
}

static void test_signals_may_exceed_dlc(void)
{
    /* A signal beyond the declared dlc is loadable; the runtime drops
     * too-short frames at decode time (spec section 8). */
    cancestry_codec_map_t *map = cancestry_test_load_map(signal_doc(
        "        - name: Wide\n"
        "          start_bit: 40\n"
        "          bit_length: 8\n"
        "          type: uint\n"
        "          endianness: little\n"));
    CANCESSTRY_TEST_CHECK(map != NULL);
    CANCESSTRY_TEST_CHECK_U64(map->messages[0].signals[0].last_bit, 47u);
    cancestry_codec_map_free(map);
}

static void test_parse_errors(void)
{
    /* Document-level errors. */
    check_load_fails("", CANCESTRY_CODEC_ERR_PARSE);
    check_load_fails("   \n# only comments\n", CANCESTRY_CODEC_ERR_PARSE);
    check_load_fails("schema_version: \"0.2.0\"\n", CANCESTRY_CODEC_ERR_PARSE);
    check_load_fails(
        "schema_version: \"0.2.1\"\n"
        "codec_map:\n"
        "  name: a\n"
        "  version: 1.0.0\n"
        "  messages:\n"
        "    - id: 1\n"
        "      name: M\n"
        "      dlc: 1\n"
        "      signals:\n"
        "        - name: S\n"
        "          start_bit: 0\n"
        "          bit_length: 1\n"
        "          type: uint\n"
        "          endianness: little\n",
        CANCESTRY_CODEC_ERR_PARSE);
    check_load_fails(
        "schema_version: \"0.2.0\"\n"
        "extra: 1\n"
        "codec_map:\n"
        "  name: a\n"
        "  version: 1.0.0\n"
        "  messages:\n"
        "    - id: 1\n"
        "      name: M\n"
        "      dlc: 1\n"
        "      signals:\n"
        "        - name: S\n"
        "          start_bit: 0\n"
        "          bit_length: 1\n"
        "          type: uint\n"
        "          endianness: little\n",
        CANCESTRY_CODEC_ERR_PARSE);
    /* codec_map must be a mapping, not a scalar. */
    check_load_fails("schema_version: \"0.2.0\"\ncodec_map: nope\n", CANCESTRY_CODEC_ERR_PARSE);
    /* YAML syntax: a line without a colon. */
    check_load_fails("schema_version \"0.2.0\"\ncodec_map:\n  name: a\n",
                     CANCESTRY_CODEC_ERR_PARSE);
    /* Unexpected indentation at the root mapping. */
    check_load_fails(
        "schema_version: \"0.2.0\"\n"
        "  codec_map:\n"
        "    name: a\n"
        "    version: 1.0.0\n"
        "    messages:\n"
        "      - id: 1\n"
        "        name: M\n"
        "        dlc: 1\n"
        "        signals:\n"
        "          - name: S\n"
        "            start_bit: 0\n"
        "            bit_length: 1\n"
        "            type: uint\n"
        "            endianness: little\n",
        CANCESTRY_CODEC_ERR_PARSE);
    /* Tabs are not allowed in indentation. */
    check_load_fails(
        "schema_version: \"0.2.0\"\n"
        "codec_map:\n"
        "\tname: a\n"
        "\tversion: 1.0.0\n"
        "\tmessages:\n"
        "\t  - id: 1\n"
        "\t    name: M\n"
        "\t    dlc: 1\n"
        "\t    signals:\n"
        "\t      - name: S\n"
        "\t        start_bit: 0\n"
        "\t        bit_length: 1\n"
        "\t        type: uint\n"
        "\t        endianness: little\n",
        CANCESTRY_CODEC_ERR_PARSE);
    /* Unterminated quoted string. */
    check_load_fails(
        "schema_version: \"0.2.0\"\n"
        "codec_map:\n"
        "  name: a\n"
        "  version: 1.0.0\n"
        "  description: \"unterminated\n"
        "  messages:\n"
        "    - id: 1\n"
        "      name: M\n"
        "      dlc: 1\n"
        "      signals:\n"
        "        - name: S\n"
        "          start_bit: 0\n"
        "          bit_length: 1\n"
        "          type: uint\n"
        "          endianness: little\n",
        CANCESTRY_CODEC_ERR_PARSE);
    /* Duplicate mapping key. */
    check_load_fails(
        "schema_version: \"0.2.0\"\n"
        "codec_map:\n"
        "  name: a\n"
        "  name: b\n"
        "  version: 1.0.0\n"
        "  messages:\n"
        "    - id: 1\n"
        "      name: M\n"
        "      dlc: 1\n"
        "      signals:\n"
        "        - name: S\n"
        "          start_bit: 0\n"
        "          bit_length: 1\n"
        "          type: uint\n"
        "          endianness: little\n",
        CANCESTRY_CODEC_ERR_PARSE);
}

static void test_schema_violations(void)
{
    /* Codec map name constraints. */
    check_load_fails(
        "schema_version: \"0.2.0\"\n"
        "codec_map:\n"
        "  name: a.b\n"
        "  version: 1.0.0\n"
        "  messages:\n"
        "    - id: 1\n"
        "      name: M\n"
        "      dlc: 1\n"
        "      signals:\n"
        "        - name: S\n"
        "          start_bit: 0\n"
        "          bit_length: 1\n"
        "          type: uint\n"
        "          endianness: little\n",
        CANCESTRY_CODEC_ERR_PARSE);
    check_load_fails(
        "schema_version: \"0.2.0\"\n"
        "codec_map:\n"
        "  name: \"\"\n"
        "  version: 1.0.0\n"
        "  messages:\n"
        "    - id: 1\n"
        "      name: M\n"
        "      dlc: 1\n"
        "      signals:\n"
        "        - name: S\n"
        "          start_bit: 0\n"
        "          bit_length: 1\n"
        "          type: uint\n"
        "          endianness: little\n",
        CANCESTRY_CODEC_ERR_PARSE);
    /* Version pattern ^\d+\.\d+\.\d+$. */
    check_load_fails(
        "schema_version: \"0.2.0\"\n"
        "codec_map:\n"
        "  name: a\n"
        "  version: 1.0\n"
        "  messages:\n"
        "    - id: 1\n"
        "      name: M\n"
        "      dlc: 1\n"
        "      signals:\n"
        "        - name: S\n"
        "          start_bit: 0\n"
        "          bit_length: 1\n"
        "          type: uint\n"
        "          endianness: little\n",
        CANCESTRY_CODEC_ERR_PARSE);
    check_load_fails(
        "schema_version: \"0.2.0\"\n"
        "codec_map:\n"
        "  name: a\n"
        "  version: v1.0.0\n"
        "  messages:\n"
        "    - id: 1\n"
        "      name: M\n"
        "      dlc: 1\n"
        "      signals:\n"
        "        - name: S\n"
        "          start_bit: 0\n"
        "          bit_length: 1\n"
        "          type: uint\n"
        "          endianness: little\n",
        CANCESTRY_CODEC_ERR_PARSE);
    /* messages must be a non-empty sequence of mappings. */
    check_load_fails(
        "schema_version: \"0.2.0\"\n"
        "codec_map:\n"
        "  name: a\n"
        "  version: 1.0.0\n"
        "  messages: []\n",
        CANCESTRY_CODEC_ERR_PARSE);
    check_load_fails(
        "schema_version: \"0.2.0\"\n"
        "codec_map:\n"
        "  name: a\n"
        "  version: 1.0.0\n"
        "  messages:\n"
        "    - 5\n",
        CANCESTRY_CODEC_ERR_PARSE);
    /* Message fields. */
    check_load_fails(
        "schema_version: \"0.2.0\"\n"
        "codec_map:\n"
        "  name: a\n"
        "  version: 1.0.0\n"
        "  messages:\n"
        "    - id: -1\n"
        "      name: M\n"
        "      dlc: 1\n"
        "      signals:\n"
        "        - name: S\n"
        "          start_bit: 0\n"
        "          bit_length: 1\n"
        "          type: uint\n"
        "          endianness: little\n",
        CANCESTRY_CODEC_ERR_PARSE);
    check_load_fails(
        "schema_version: \"0.2.0\"\n"
        "codec_map:\n"
        "  name: a\n"
        "  version: 1.0.0\n"
        "  messages:\n"
        "    - id: 536870912\n"
        "      name: M\n"
        "      dlc: 1\n"
        "      signals:\n"
        "        - name: S\n"
        "          start_bit: 0\n"
        "          bit_length: 1\n"
        "          type: uint\n"
        "          endianness: little\n",
        CANCESTRY_CODEC_ERR_PARSE);
    check_load_fails(
        "schema_version: \"0.2.0\"\n"
        "codec_map:\n"
        "  name: a\n"
        "  version: 1.0.0\n"
        "  messages:\n"
        "    - id: 1.5\n"
        "      name: M\n"
        "      dlc: 1\n"
        "      signals:\n"
        "        - name: S\n"
        "          start_bit: 0\n"
        "          bit_length: 1\n"
        "          type: uint\n"
        "          endianness: little\n",
        CANCESTRY_CODEC_ERR_PARSE);
    check_load_fails(
        "schema_version: \"0.2.0\"\n"
        "codec_map:\n"
        "  name: a\n"
        "  version: 1.0.0\n"
        "  messages:\n"
        "    - id: 1\n"
        "      name: M\n"
        "      dlc: 9\n"
        "      signals:\n"
        "        - name: S\n"
        "          start_bit: 0\n"
        "          bit_length: 1\n"
        "          type: uint\n"
        "          endianness: little\n",
        CANCESTRY_CODEC_ERR_PARSE);
    check_load_fails(
        "schema_version: \"0.2.0\"\n"
        "codec_map:\n"
        "  name: a\n"
        "  version: 1.0.0\n"
        "  messages:\n"
        "    - id: 1\n"
        "      name: M\n"
        "      dlc: 1\n"
        "      period_ms: -1\n"
        "      signals:\n"
        "        - name: S\n"
        "          start_bit: 0\n"
        "          bit_length: 1\n"
        "          type: uint\n"
        "          endianness: little\n",
        CANCESTRY_CODEC_ERR_PARSE);
    check_load_fails(
        "schema_version: \"0.2.0\"\n"
        "codec_map:\n"
        "  name: a\n"
        "  version: 1.0.0\n"
        "  messages:\n"
        "    - id: 1\n"
        "      name: M\n"
        "      dlc: 1\n"
        "      signals: []\n",
        CANCESTRY_CODEC_ERR_PARSE);
}

static void test_signal_schema_violations(void)
{
    check_load_fails(signal_doc(
        "        - name: S\n"
        "          bit_length: 8\n"
        "          type: uint\n"
        "          endianness: little\n"),
        CANCESTRY_CODEC_ERR_PARSE); /* missing start_bit */
    check_load_fails(signal_doc(
        "        - name: S\n"
        "          start_bit: -1\n"
        "          bit_length: 8\n"
        "          type: uint\n"
        "          endianness: little\n"),
        CANCESTRY_CODEC_ERR_PARSE);
    check_load_fails(signal_doc(
        "        - name: S\n"
        "          start_bit: 64\n"
        "          bit_length: 1\n"
        "          type: uint\n"
        "          endianness: little\n"),
        CANCESTRY_CODEC_ERR_PARSE);
    check_load_fails(signal_doc(
        "        - name: S\n"
        "          start_bit: 0\n"
        "          bit_length: 0\n"
        "          type: uint\n"
        "          endianness: little\n"),
        CANCESTRY_CODEC_ERR_PARSE);
    check_load_fails(signal_doc(
        "        - name: S\n"
        "          start_bit: 0\n"
        "          bit_length: 65\n"
        "          type: uint\n"
        "          endianness: little\n"),
        CANCESTRY_CODEC_ERR_PARSE);
    check_load_fails(signal_doc(
        "        - name: S\n"
        "          start_bit: 0\n"
        "          bit_length: 8\n"
        "          type: float\n"
        "          endianness: little\n"),
        CANCESTRY_CODEC_ERR_PARSE);
    check_load_fails(signal_doc(
        "        - name: S\n"
        "          start_bit: 0\n"
        "          bit_length: 8\n"
        "          type: uint\n"
        "          endianness: middle\n"),
        CANCESTRY_CODEC_ERR_PARSE);
    check_load_fails(signal_doc(
        "        - name: S\n"
        "          start_bit: 0\n"
        "          bit_length: 2\n"
        "          type: boolean\n"
        "          endianness: little\n"),
        CANCESTRY_CODEC_ERR_PARSE); /* boolean must be 1 bit */
    check_load_fails(signal_doc(
        "        - name: S\n"
        "          start_bit: 0\n"
        "          bit_length: 2\n"
        "          type: enum\n"
        "          endianness: little\n"),
        CANCESTRY_CODEC_ERR_PARSE); /* enum requires values */
    check_load_fails(signal_doc(
        "        - name: S\n"
        "          start_bit: 0\n"
        "          bit_length: 8\n"
        "          type: uint\n"
        "          endianness: little\n"
        "          extra: 1\n"),
        CANCESTRY_CODEC_ERR_PARSE); /* unknown signal field */
    check_load_fails(signal_doc(
        "        - name: S\n"
        "          start_bit: 0\n"
        "          bit_length: 8\n"
        "          type: uint\n"
        "          endianness: little\n"
        "          scale: fast\n"),
        CANCESTRY_CODEC_ERR_PARSE);
    check_load_fails(signal_doc(
        "        - name: S\n"
        "          start_bit: 0\n"
        "          bit_length: 8\n"
        "          type: uint\n"
        "          endianness: little\n"
        "          scale: 0\n"),
        CANCESTRY_CODEC_ERR_PARSE); /* scale 0 cannot be encoded */
    check_load_fails(signal_doc(
        "        - name: S\n"
        "          start_bit: 0\n"
        "          bit_length: 8\n"
        "          type: uint\n"
        "          endianness: little\n"
        "          scale: 1.5e999\n"),
        CANCESTRY_CODEC_ERR_PARSE); /* non-finite scale */
    check_load_fails(signal_doc(
        "        - name: S\n"
        "          start_bit: 0\n"
        "          bit_length: 8\n"
        "          type: uint\n"
        "          endianness: little\n"
        "          min: 10\n"
        "          max: 5\n"),
        CANCESTRY_CODEC_ERR_PARSE); /* min above max */
    /* Little-endian signal beyond the 64-bit payload. */
    check_load_fails(signal_doc(
        "        - name: S\n"
        "          start_bit: 60\n"
        "          bit_length: 8\n"
        "          type: uint\n"
        "          endianness: little\n"),
        CANCESTRY_CODEC_ERR_PARSE);
    /* Big-endian signal below payload bit 0. */
    check_load_fails(signal_doc(
        "        - name: S\n"
        "          start_bit: 2\n"
        "          bit_length: 8\n"
        "          type: uint\n"
        "          endianness: big\n"),
        CANCESTRY_CODEC_ERR_PARSE);
}

static void test_values_violations(void)
{
    check_load_fails(signal_doc(
        "        - name: S\n"
        "          start_bit: 0\n"
        "          bit_length: 2\n"
        "          type: enum\n"
        "          endianness: little\n"
        "          values: 5\n"),
        CANCESTRY_CODEC_ERR_PARSE); /* values must be a mapping */
    check_load_fails(signal_doc(
        "        - name: S\n"
        "          start_bit: 0\n"
        "          bit_length: 2\n"
        "          type: enum\n"
        "          endianness: little\n"
        "          values:\n"
        "            \"A\": \"X\"\n"),
        CANCESTRY_CODEC_ERR_PARSE); /* non-numeric key */
    check_load_fails(signal_doc(
        "        - name: S\n"
        "          start_bit: 0\n"
        "          bit_length: 2\n"
        "          type: enum\n"
        "          endianness: little\n"
        "          values:\n"
        "            \"-1\": \"X\"\n"),
        CANCESTRY_CODEC_ERR_PARSE); /* negative key */
    check_load_fails(signal_doc(
        "        - name: S\n"
        "          start_bit: 0\n"
        "          bit_length: 2\n"
        "          type: enum\n"
        "          endianness: little\n"
        "          values:\n"
        "            \"1\": 5\n"),
        CANCESTRY_CODEC_ERR_PARSE); /* label must be a string */
    check_load_fails(signal_doc(
        "        - name: S\n"
        "          start_bit: 0\n"
        "          bit_length: 2\n"
        "          type: enum\n"
        "          endianness: little\n"
        "          values:\n"
        "            \"4\": \"X\"\n"),
        CANCESTRY_CODEC_ERR_PARSE); /* key does not fit 2 bits */
}

static void test_conflicts(void)
{
    /* Duplicate message id. */
    check_load_fails(
        "schema_version: \"0.2.0\"\n"
        "codec_map:\n"
        "  name: a\n"
        "  version: 1.0.0\n"
        "  messages:\n"
        "    - id: 1\n"
        "      name: M1\n"
        "      dlc: 1\n"
        "      signals:\n"
        "        - name: A\n"
        "          start_bit: 0\n"
        "          bit_length: 1\n"
        "          type: uint\n"
        "          endianness: little\n"
        "    - id: 1\n"
        "      name: M2\n"
        "      dlc: 1\n"
        "      signals:\n"
        "        - name: B\n"
        "          start_bit: 0\n"
        "          bit_length: 1\n"
        "          type: uint\n"
        "          endianness: little\n",
        CANCESTRY_CODEC_ERR_CONFLICT);
    /* Duplicate signal name across messages. */
    check_load_fails(
        "schema_version: \"0.2.0\"\n"
        "codec_map:\n"
        "  name: a\n"
        "  version: 1.0.0\n"
        "  messages:\n"
        "    - id: 1\n"
        "      name: M1\n"
        "      dlc: 1\n"
        "      signals:\n"
        "        - name: A\n"
        "          start_bit: 0\n"
        "          bit_length: 1\n"
        "          type: uint\n"
        "          endianness: little\n"
        "    - id: 2\n"
        "      name: M2\n"
        "      dlc: 1\n"
        "      signals:\n"
        "        - name: A\n"
        "          start_bit: 0\n"
        "          bit_length: 1\n"
        "          type: uint\n"
        "          endianness: little\n",
        CANCESTRY_CODEC_ERR_CONFLICT);
}

static void test_null_and_nul(void)
{
    cancestry_codec_load_error_t error;
    cancestry_codec_map_t *map;
    const char with_nul[] = {'s', 'c', 'h', 'e', 'm', 'a', '\0', '_', 'x', 'y'};

    map = cancestry_codec_map_load(NULL, 10u, &error);
    CANCESSTRY_TEST_CHECK(map == NULL);
    CANCESSTRY_TEST_CHECK(error.status == CANCESTRY_CODEC_ERR_NULL);

    map = cancestry_codec_map_load(with_nul, sizeof(with_nul), &error);
    CANCESSTRY_TEST_CHECK(map == NULL);
    CANCESSTRY_TEST_CHECK(error.status == CANCESTRY_CODEC_ERR_PARSE);

    /* Errors are reported even without an error sink. */
    map = cancestry_codec_map_load("", 0u, NULL);
    CANCESSTRY_TEST_CHECK(map == NULL);
    map = cancestry_codec_map_load(NULL, 0u, NULL);
    CANCESSTRY_TEST_CHECK(map == NULL);
}

static void test_independent_instances(void)
{
    cancestry_codec_map_t *first = cancestry_test_load_map(minimal_yaml);
    cancestry_codec_map_t *second = cancestry_test_load_map(minimal_yaml);

    CANCESSTRY_TEST_CHECK(first != NULL);
    CANCESSTRY_TEST_CHECK(second != NULL);
    CANCESSTRY_TEST_CHECK(first != second);
    CANCESSTRY_TEST_CHECK_STRING(first->name, second->name);
    CANCESSTRY_TEST_CHECK(first->name != second->name); /* separate storage */

    cancestry_codec_map_free(first);
    cancestry_codec_map_free(second);
    cancestry_codec_map_free(NULL); /* no-op */
}

static void test_status_names(void)
{
    CANCESSTRY_TEST_CHECK_STRING(cancestry_codec_status_name(CANCESTRY_CODEC_ERR_PARSE),
                                 "ERR_PARSE");
    CANCESSTRY_TEST_CHECK_STRING(cancestry_codec_status_name(CANCESTRY_CODEC_ERR_CONFLICT),
                                 "ERR_CONFLICT");
    CANCESSTRY_TEST_CHECK_STRING(cancestry_codec_status_name(CANCESTRY_CODEC_ERR_NO_MEMORY),
                                 "ERR_NO_MEMORY");
}

int main(void)
{
    CANCESSTRY_TEST_SUITE_BEGIN("codec loader");

    CANCESSTRY_TEST_CASE("minimal document loads with schema defaults");
    test_minimal_defaults();

    CANCESSTRY_TEST_CASE("full-featured document: quotes, escapes, hex/octal");
    test_full_featured();

    CANCESSTRY_TEST_CASE("big-endian signals derive first_bit/last_bit");
    test_big_endian_derived_bits();

    CANCESSTRY_TEST_CASE("signals may exceed the declared dlc");
    test_signals_may_exceed_dlc();

    CANCESSTRY_TEST_CASE("YAML syntax errors are rejected with positions");
    test_parse_errors();

    CANCESSTRY_TEST_CASE("schema violations are rejected");
    test_schema_violations();

    CANCESSTRY_TEST_CASE("signal schema violations are rejected");
    test_signal_schema_violations();

    CANCESSTRY_TEST_CASE("value mapping violations are rejected");
    test_values_violations();

    CANCESSTRY_TEST_CASE("duplicate ids and names are conflicts");
    test_conflicts();

    CANCESSTRY_TEST_CASE("NULL text and NUL bytes are rejected");
    test_null_and_nul();

    CANCESSTRY_TEST_CASE("each load yields an independent instance");
    test_independent_instances();

    CANCESSTRY_TEST_CASE("status names");
    test_status_names();

    return CANCESSTRY_TEST_SUITE_END();
}
