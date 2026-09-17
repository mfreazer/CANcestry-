/*
 * Shared helpers for the CANcestry codec unit tests.
 *
 * Loads codec maps from inline YAML through the real loader, so decoder and
 * encoder tests exercise the loader as well.
 */

#ifndef CANCESTRY_CODEC_TEST_H
#define CANCESTRY_CODEC_TEST_H

#include "cancestry/codec/loader.h"
#include "cancestry/codec/types.h"

#include "cancestry_test.h"

#include <string.h>

/** Load a codec map from inline YAML; prints and counts a failure on error. */
static inline cancestry_codec_map_t *cancestry_test_load_map(const char *yaml)
{
    cancestry_codec_load_error_t error;
    cancestry_codec_map_t *map = cancestry_codec_map_load(yaml, strlen(yaml), &error);

    if (map == NULL) {
        printf("    FAIL to load test codec map: %s (line %u, column %u)\n", error.message,
               (unsigned)error.line, (unsigned)error.column);
        fflush(stdout);
        cancestry_test_failures++;
    }
    return map;
}

/**
 * A CAN FD codec map (issue #20): one 64-byte message with a signal in the
 * last payload byte. Loaded with CAN FD capabilities declared, so callers that
 * need the classic-only load path must expect CANCESTRY_CODEC_ERR_UNSUPPORTED.
 */
static const char *const cancestry_test_fd_codec_yaml =
    "schema_version: \"0.3.0\"\n"
    "codec_map:\n"
    "  name: fdemo\n"
    "  version: 1.0.0\n"
    "  can_fd: true\n"
    "  messages:\n"
    "    - id: 0x100\n"
    "      name: StatusMsg\n"
    "      dlc: 8\n"
    "      signals:\n"
    "        - name: VehicleSpeed\n"
    "          start_bit: 0\n"
    "          bit_length: 16\n"
    "          type: uint\n"
    "          endianness: little\n"
    "    - id: 0x400\n"
    "      name: FdMsg\n"
    "      dlc: 64\n"
    "      signals:\n"
    "        - name: FdTail\n"
    "          start_bit: 504\n"
    "          bit_length: 8\n"
    "          type: uint\n"
    "          endianness: little\n";

#define CANCESTRY_TEST_FD_MESSAGE_ID ((uint32_t)0x400u)

/** Load cancestry_test_fd_codec_yaml with CAN FD capabilities declared. */
static inline cancestry_codec_map_t *cancestry_test_load_fd_map(void)
{
    cancestry_codec_platform_caps_t caps;
    cancestry_codec_load_error_t error;
    cancestry_codec_map_t *map;

    caps.can_fd = true;
    map = cancestry_codec_map_load_checked(cancestry_test_fd_codec_yaml,
                                           strlen(cancestry_test_fd_codec_yaml), &caps, &error);
    if (map == NULL) {
        printf("    FAIL to load FD test codec map: %s (line %u, column %u)\n", error.message,
               (unsigned)error.line, (unsigned)error.column);
        fflush(stdout);
        cancestry_test_failures++;
    }
    return map;
}

/*
 * Demo codec map used by the decoder, encoder and invalid-frame tests.
 *
 * Message 0x1A0 (8 bytes):
 *   EngineSpeed   LE uint 16 @ 0        scale 0.25          (bits 0..15)
 *   CoolantTemp   BE int  8  @ 23       offset -40          (bits 16..23)
 *   BrakePressed  LE bool 1  @ 24       labels RELEASED/PRESSED
 *   Gear          LE enum 3  @ 25       labels PARK..DRIVE  (bits 25..27)
 *   BigCounter    BE uint 32 @ 63                            (bits 32..63)
 */
#define CANCESTRY_CODEC_DEMO_MESSAGE_ID ((uint32_t)0x1A0u)
#define CANCESTRY_CODEC_DEMO_SIGNAL_COUNT ((size_t)5u)

static const char *const cancestry_test_demo_yaml =
    "schema_version: \"0.2.0\"\n"
    "codec_map:\n"
    "  name: demo\n"
    "  version: 1.0.0\n"
    "  messages:\n"
    "    - id: 0x1A0\n"
    "      name: Engine\n"
    "      dlc: 8\n"
    "      period_ms: 10\n"
    "      signals:\n"
    "        - name: EngineSpeed\n"
    "          start_bit: 0\n"
    "          bit_length: 16\n"
    "          type: uint\n"
    "          endianness: little\n"
    "          scale: 0.25\n"
    "          offset: 0\n"
    "          unit: rpm\n"
    "          min: 0\n"
    "          max: 16000\n"
    "        - name: CoolantTemp\n"
    "          start_bit: 23\n"
    "          bit_length: 8\n"
    "          type: int\n"
    "          endianness: big\n"
    "          scale: 1\n"
    "          offset: -40\n"
    "          unit: degC\n"
    "        - name: BrakePressed\n"
    "          start_bit: 24\n"
    "          bit_length: 1\n"
    "          type: boolean\n"
    "          endianness: little\n"
    "          values:\n"
    "            \"0\": RELEASED\n"
    "            \"1\": PRESSED\n"
    "        - name: Gear\n"
    "          start_bit: 25\n"
    "          bit_length: 3\n"
    "          type: enum\n"
    "          endianness: little\n"
    "          values:\n"
    "            \"0\": PARK\n"
    "            \"1\": REVERSE\n"
    "            \"2\": NEUTRAL\n"
    "            \"3\": DRIVE\n"
    "        - name: BigCounter\n"
    "          start_bit: 63\n"
    "          bit_length: 32\n"
    "          type: uint\n"
    "          endianness: big\n";

/* Bitpacking codec map: pure integer signals with tricky spans. */
#define CANCESTRY_CODEC_BIT_MESSAGE_ID ((uint32_t)0x200u)

static const char *const cancestry_test_bit_yaml =
    "schema_version: \"0.2.0\"\n"
    "codec_map:\n"
    "  name: bits\n"
    "  version: 2.0.0\n"
    "  messages:\n"
    "    - id: 0x200\n"
    "      name: BitSoup\n"
    "      dlc: 8\n"
    "      signals:\n"
    "        - name: Bit0\n"
    "          start_bit: 0\n"
    "          bit_length: 1\n"
    "          type: uint\n"
    "          endianness: little\n"
    "        - name: Bit7\n"
    "          start_bit: 7\n"
    "          bit_length: 1\n"
    "          type: uint\n"
    "          endianness: little\n"
    "        - name: Le12\n"
    "          start_bit: 5\n"
    "          bit_length: 12\n"
    "          type: uint\n"
    "          endianness: little\n"
    "        - name: Be12\n"
    "          start_bit: 15\n"
    "          bit_length: 12\n"
    "          type: uint\n"
    "          endianness: big\n"
    "        - name: Int12Le\n"
    "          start_bit: 20\n"
    "          bit_length: 12\n"
    "          type: int\n"
    "          endianness: little\n"
    "        - name: Int12Be\n"
    "          start_bit: 31\n"
    "          bit_length: 12\n"
    "          type: int\n"
    "          endianness: big\n"
    "        - name: Le64\n"
    "          start_bit: 0\n"
    "          bit_length: 64\n"
    "          type: uint\n"
    "          endianness: little\n"
    "        - name: Be64\n"
    "          start_bit: 63\n"
    "          bit_length: 64\n"
    "          type: uint\n"
    "          endianness: big\n";

#endif /* CANCESTRY_CODEC_TEST_H */
