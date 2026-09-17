/*
 * CANcestry codec conformance: CAN FD codec-map rules ("schema is law").
 *
 * The codec loader is the last cheap place a transport mismatch can be
 * caught, so these rules are load-time rules: a document that asks for CAN FD
 * on a platform that does not have it is refused before any runtime sees it,
 * and a classic document keeps exactly the meaning it had before Phase 7.
 *
 * Verifies (SW-FR-CANFD-001, SW-FR-CANFD-004, CODEC-CANFD-SCHEMA-001):
 *   1. `can_fd: true` loads when the declared platform capabilities include
 *      CAN FD, and is refused with CANCESTRY_CODEC_ERR_UNSUPPORTED when they
 *      do not - including the legacy entry point
 *      cancestry_codec_map_load() and a NULL caps argument, both of which
 *      mean "classic only" (fail closed).
 *   2. A classic map is unchanged: no can_fd key, dlc capped at 8, signals
 *      capped at the 64-bit payload.
 *   3. An FD map accepts exactly the payload lengths CAN FD can put on the
 *      wire (0-8, 12, 16, 20, 24, 32, 48, 64) and rejects 9, 10, 11, 13 and
 *      65.
 *   4. An FD map may address payload bits up to 511 and no further, for both
 *      the contiguous and the sawtooth layout.
 *   5. A non-boolean `can_fd` and an unknown key (`max_dlc`) are parse
 *      errors, mirroring `additionalProperties: false` in
 *      schemas/codec-map-0.3.0.schema.json.
 *
 * Implements: SW-FR-CANFD-001, SW-FR-CANFD-004
 * Test ids:    CODEC-CANFD-SCHEMA-001
 */

#include "cancestry/codec/loader.h"
#include "cancestry/codec/types.h"
#include "cancestry_test.h"

#include <string.h>

#define MAP_HEADER(can_fd_line)                                                \
    "schema_version: \"0.3.0\"\n"                                              \
    "codec_map:\n"                                                             \
    "  name: fd_rules\n"                                                       \
    "  version: 1.0.0\n" can_fd_line "  messages:\n"                           \
    "    - id: 0x1F0\n"                                                        \
    "      name: RadarCluster\n"

#define MAP_FOOTER(signal_lines)                                               \
    "      signals:\n" signal_lines

#define SIGNAL(name, bit, len, endian)                                         \
    "        - name: " name "\n"                                               \
    "          start_bit: " bit "\n"                                           \
    "          bit_length: " len "\n"                                          \
    "          type: uint\n"                                                   \
    "          endianness: " endian "\n"

#define SAWTOOTH_SIGNAL(name, bit, len)                                        \
    "        - name: " name "\n"                                               \
    "          start_bit: " bit "\n"                                           \
    "          bit_length: " len "\n"                                          \
    "          type: uint\n"                                                   \
    "          endianness: big\n"                                              \
    "          layout: sawtooth\n"

/** Load @p yaml with @p can_fd capability and require success. */
static cancestry_codec_map_t *load_ok(bool platform_can_fd, const char *yaml)
{
    cancestry_codec_platform_caps_t caps;
    cancestry_codec_load_error_t error;
    cancestry_codec_map_t *map;

    caps.can_fd = platform_can_fd;
    memset(&error, 0, sizeof(error));
    map = cancestry_codec_map_load_checked(yaml, strlen(yaml), &caps, &error);
    if (map == NULL) {
        printf("    FAIL expected load to succeed: %s (line %u, column %u)\n", error.message,
               (unsigned)error.line, (unsigned)error.column);
        fflush(stdout);
        cancestry_test_failures++;
    }
    return map;
}

/** Require @p yaml to fail with @p expected through the capability-checked loader. */
static void check_fails(bool platform_can_fd,
                        const char *yaml,
                        cancestry_codec_status_t expected)
{
    cancestry_codec_platform_caps_t caps;
    cancestry_codec_load_error_t error;
    cancestry_codec_map_t *map;

    caps.can_fd = platform_can_fd;
    memset(&error, 0, sizeof(error));
    map = cancestry_codec_map_load_checked(yaml, strlen(yaml), &caps, &error);
    if (map != NULL) {
        printf("    FAIL expected load to fail with %s but it succeeded\n",
               cancestry_codec_status_name(expected));
        fflush(stdout);
        cancestry_test_failures++;
        cancestry_codec_map_free(map);
        return;
    }
    CANCESSTRY_TEST_CHECK_I64((int64_t)error.status, (int64_t)expected);
}

static void test_capability_gate(void)
{
    static const char *const fd_yaml =
        MAP_HEADER("  can_fd: true\n")
        "      dlc: 64\n" MAP_FOOTER(SIGNAL("Tail", "504", "8", "little"));
    static const char *const classic_yaml =
        MAP_HEADER("")
        "      dlc: 8\n" MAP_FOOTER(SIGNAL("Head", "0", "8", "little"));
    cancestry_codec_map_t *map;
    cancestry_codec_load_error_t error;

    /* 1. FD map + FD platform loads. */
    map = load_ok(true, fd_yaml);
    CANCESSTRY_TEST_CHECK(map != NULL);
    if (map != NULL) {
        CANCESSTRY_TEST_CHECK(map->can_fd);
        CANCESSTRY_TEST_CHECK_U64(map->messages[0].dlc, 64u);
        cancestry_codec_map_free(map);
    }

    /* 1. FD map + classic platform is refused at load time. */
    check_fails(false, fd_yaml, CANCESTRY_CODEC_ERR_UNSUPPORTED);

    /* 1. The legacy entry point means "classic only". */
    memset(&error, 0, sizeof(error));
    map = cancestry_codec_map_load(fd_yaml, strlen(fd_yaml), &error);
    CANCESSTRY_TEST_CHECK(map == NULL);
    CANCESSTRY_TEST_CHECK_I64((int64_t)error.status,
                              (int64_t)CANCESTRY_CODEC_ERR_UNSUPPORTED);
    CANCESSTRY_TEST_CHECK(strstr(error.message, "can_fd") != NULL);

    /* 1. A NULL caps argument is "capabilities unknown" -> classic only. */
    memset(&error, 0, sizeof(error));
    map = cancestry_codec_map_load_checked(fd_yaml, strlen(fd_yaml), NULL, &error);
    CANCESSTRY_TEST_CHECK(map == NULL);
    CANCESSTRY_TEST_CHECK_I64((int64_t)error.status,
                              (int64_t)CANCESTRY_CODEC_ERR_UNSUPPORTED);

    /* 2. A classic map still loads through both entry points. */
    map = cancestry_codec_map_load(classic_yaml, strlen(classic_yaml), &error);
    CANCESSTRY_TEST_CHECK(map != NULL);
    if (map != NULL) {
        CANCESSTRY_TEST_CHECK(!map->can_fd);
        cancestry_codec_map_free(map);
    }
    map = load_ok(true, classic_yaml);
    CANCESSTRY_TEST_CHECK(map != NULL);
    if (map != NULL) {
        CANCESSTRY_TEST_CHECK(!map->can_fd);
        cancestry_codec_map_free(map);
    }

}

static void test_dlc_vocabulary(void)
{
    static const char *const ok_lengths[] = {"0",  "1",  "8",  "12", "16",
                                             "20", "24", "32", "48", "64"};
    static const char *const bad_lengths[] = {"9", "10", "11", "13", "17", "65"};
    size_t i;

    for (i = 0u; i < sizeof(ok_lengths) / sizeof(ok_lengths[0]); ++i) {
        char yaml[512];
        cancestry_codec_map_t *map;

        (void)snprintf(yaml, sizeof(yaml),
                       MAP_HEADER("  can_fd: true\n")
                       "      dlc: %s\n" MAP_FOOTER(SIGNAL("Head", "0", "8", "little")),
                       ok_lengths[i]);
        map = load_ok(true, yaml);
        CANCESSTRY_TEST_CHECK(map != NULL);
        if (map != NULL) {
            cancestry_codec_map_free(map);
        }
    }

    for (i = 0u; i < sizeof(bad_lengths) / sizeof(bad_lengths[0]); ++i) {
        char yaml[512];

        (void)snprintf(yaml, sizeof(yaml),
                       MAP_HEADER("  can_fd: true\n")
                       "      dlc: %s\n" MAP_FOOTER(SIGNAL("Head", "0", "8", "little")),
                       bad_lengths[i]);
        check_fails(true, yaml, CANCESTRY_CODEC_ERR_PARSE);
    }

    /* A classic map keeps the 0..8 rule: 12 is a legal CAN FD length but not a
     * legal classic one (backwards compatibility, SW-FR-CANFD-001). */
    {
        static const char *const yaml =
            MAP_HEADER("")
            "      dlc: 12\n" MAP_FOOTER(SIGNAL("Head", "0", "8", "little"));
        check_fails(false, yaml, CANCESTRY_CODEC_ERR_PARSE);
        check_fails(true, yaml, CANCESTRY_CODEC_ERR_PARSE);
    }
}

static void test_payload_bit_limit(void)
{
    /* FD map: the last bit of a 64-byte payload is addressable... */
    {
        static const char *const yaml =
            MAP_HEADER("  can_fd: true\n")
            "      dlc: 64\n" MAP_FOOTER(SIGNAL("Tail", "504", "8", "little"));
        cancestry_codec_map_t *map = load_ok(true, yaml);
        CANCESSTRY_TEST_CHECK(map != NULL);
        if (map != NULL) {
            CANCESSTRY_TEST_CHECK_U64(map->messages[0].signals[0].last_bit, 511u);
            cancestry_codec_map_free(map);
        }
    }
    /* ...and one bit further is not. */
    {
        static const char *const yaml =
            MAP_HEADER("  can_fd: true\n")
            "      dlc: 64\n" MAP_FOOTER(SIGNAL("Tail", "505", "8", "little"));
        check_fails(true, yaml, CANCESTRY_CODEC_ERR_PARSE);
    }
    /* The sawtooth layout obeys the same 512-bit limit. */
    {
        static const char *const yaml =
            MAP_HEADER("  can_fd: true\n")
            "      dlc: 64\n" MAP_FOOTER(SAWTOOTH_SIGNAL("Tail", "504", "16"));
        check_fails(true, yaml, CANCESTRY_CODEC_ERR_PARSE);
    }
    /* A classic map keeps the 64-bit limit: bit 100 is out of reach. */
    {
        static const char *const yaml =
            MAP_HEADER("")
            "      dlc: 8\n" MAP_FOOTER(SIGNAL("Wide", "100", "8", "little"));
        check_fails(false, yaml, CANCESTRY_CODEC_ERR_PARSE);
        check_fails(true, yaml, CANCESTRY_CODEC_ERR_PARSE);
    }
}

static void test_field_validation(void)
{
    /* can_fd must be a boolean. */
    {
        static const char *const yaml =
            MAP_HEADER("  can_fd: yes\n")
            "      dlc: 64\n" MAP_FOOTER(SIGNAL("Head", "0", "8", "little"));
        check_fails(true, yaml, CANCESTRY_CODEC_ERR_PARSE);
    }
    /* Unknown keys are rejected, exactly like the JSON Schema's
     * additionalProperties: false. */
    {
        static const char *const yaml =
            MAP_HEADER("  can_fd: true\n  max_dlc: 64\n")
            "      dlc: 64\n" MAP_FOOTER(SIGNAL("Head", "0", "8", "little"));
        check_fails(true, yaml, CANCESTRY_CODEC_ERR_PARSE);
    }
    {
        static const char *const yaml =
            MAP_HEADER("")
            "      dlc: 8\n      can_fd: true\n" MAP_FOOTER(SIGNAL("Head", "0", "8", "little"));
        check_fails(true, yaml, CANCESTRY_CODEC_ERR_PARSE);
    }
}

int main(void)
{
    CANCESSTRY_TEST_SUITE_BEGIN("codec CAN FD schema rules");

    CANCESSTRY_TEST_CASE("can_fd is gated on declared platform capabilities");
    test_capability_gate();

    CANCESSTRY_TEST_CASE("dlc uses the classic or the CAN FD vocabulary");
    test_dlc_vocabulary();

    CANCESSTRY_TEST_CASE("the payload bit limit follows the can_fd flag");
    test_payload_bit_limit();

    CANCESSTRY_TEST_CASE("can_fd field validation matches the JSON Schema");
    test_field_validation();

    return CANCESSTRY_TEST_SUITE_END();
}
