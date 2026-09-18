/*
 * CANcestry UDS conformance: services 0x22 / 0x2E / 0x31 (Phase 10, issue #26).
 *
 * Contract under test (docs/software/SwRS.md section 14.2):
 *
 *   - SW-FR-UDS-001: request bytes in, response bytes out; the server is a
 *     pure function of (server state, request).
 *   - SW-FR-UDS-002: ReadDataByIdentifier returns 62 DID data; NRC 0x31 for
 *     an unknown DID, 0x13 for a malformed request, 0x22 when the mapped
 *     signal value cannot be encoded in the DID length.
 *   - SW-FR-UDS-003: WriteDataByIdentifier returns 6E DID after storing the
 *     declared length; NRC 0x31 unknown DID, 0x13 wrong length, 0x72 when
 *     the shared signal store cannot accept the mirrored value.
 *   - SW-FR-UDS-004: RoutineControl start/stop/results with the configured
 *     record; NRC 0x31 unknown routine, 0x12 unsupported or disallowed
 *     sub-function, 0x13 malformed request.
 *   - SW-FR-UDS-005: unsupported services answer 7F SID 11.
 *   - SW-FR-UDS-006: the configuration is validated against
 *     schemas/uds-0.1.0.schema.json at load time; duplicates, unknown
 *     fields, bad versions and out-of-range values are refused.
 *   - SW-FR-UDS-008: a signal-mapped DID encodes the current shared signal
 *     value little-endian and falls back to its stored bytes.
 *
 * Test ids: UDS-SVC-001 .. UDS-SVC-007.
 */

#include "uds_conformance_support.h"

/* UDS-SVC-001: ReadDataByIdentifier on static DIDs (SW-FR-UDS-002). */
static void uds_svc_read_static(void)
{
    uds_test_env_t env;
    uint8_t request[3];
    uint8_t response[UDS_TEST_RESPONSE_MAX];
    size_t length = 0u;
    cancestry_uds_status_t status;

    uds_test_env_init(&env, NULL, UDS_TEST_SIGNAL_SLOTS_MAX, true);

    /* Known DID: 62 F1 90 + the 17 default bytes. */
    request[0] = 0x22u;
    request[1] = 0xF1u;
    request[2] = 0x90u;
    status = uds_test_process(&env, request, 3u, response, &length);
    CANCESSTRY_TEST_CHECK(status == CANCESTRY_UDS_OK);
    CANCESSTRY_TEST_CHECK(length == 20u);
    CANCESSTRY_TEST_CHECK(response[0] == 0x62u);
    CANCESSTRY_TEST_CHECK(response[1] == 0xF1u && response[2] == 0x90u);
    CANCESSTRY_TEST_CHECK(response[3] == 0x57u && response[4] == 0x41u && response[5] == 0x53u);
    CANCESSTRY_TEST_CHECK(env.server.counters.did_reads == 1u);
    CANCESSTRY_TEST_CHECK(env.server.counters.positive_responses == 1u);

    /* Unknown DID: 7F 22 31. */
    request[1] = 0x99u;
    request[2] = 0x99u;
    status = uds_test_process(&env, request, 3u, response, &length);
    CANCESSTRY_TEST_CHECK(status == CANCESTRY_UDS_ERR_NOT_FOUND);
    CANCESSTRY_TEST_CHECK(uds_test_is_nrc(response, length, 0x22u, 0x31u));
    CANCESSTRY_TEST_CHECK(env.server.counters.nrc_request_out_of_range == 1u);

    /* Malformed: two DIDs and no DID are both 0x13 (exactly one DID per
     * request at this level). */
    {
        uint8_t two[5] = {0x22u, 0xF1u, 0x90u, 0x02u, 0x04u};

        status = uds_test_process(&env, two, 5u, response, &length);
        CANCESSTRY_TEST_CHECK(status == CANCESTRY_UDS_ERR_LENGTH);
        CANCESSTRY_TEST_CHECK(uds_test_is_nrc(response, length, 0x22u, 0x13u));
    }
    {
        uint8_t one[1] = {0x22u};

        status = uds_test_process(&env, one, 1u, response, &length);
        CANCESSTRY_TEST_CHECK(status == CANCESTRY_UDS_ERR_LENGTH);
        CANCESSTRY_TEST_CHECK(uds_test_is_nrc(response, length, 0x22u, 0x13u));
    }
    CANCESSTRY_TEST_CHECK(env.server.counters.negative_responses == 3u);
    CANCESSTRY_TEST_CHECK(env.server.counters.requests_processed == 4u);

    uds_test_env_destroy(&env);
}

/* UDS-SVC-002: ReadDataByIdentifier on a signal-mapped DID: the current
 * shared signal value is encoded little-endian; without a value the stored
 * bytes are served; an unencodable value is 0x22 (SW-FR-UDS-002/008). */
static void uds_svc_read_signal_mapped(void)
{
    uds_test_env_t env;
    uint8_t request[3];
    uint8_t response[UDS_TEST_RESPONSE_MAX];
    size_t length = 0u;
    cancestry_value_t value;

    uds_test_env_init(&env, NULL, UDS_TEST_SIGNAL_SLOTS_MAX, true);
    request[0] = 0x22u;
    request[1] = 0x02u;
    request[2] = 0x04u;

    /* No signal value yet: the seeded default bytes are served. */
    CANCESSTRY_TEST_CHECK(uds_test_process(&env, request, 3u, response, &length) ==
                          CANCESTRY_UDS_OK);
    CANCESSTRY_TEST_CHECK(length == 5u);
    CANCESSTRY_TEST_CHECK(response[3] == 0x00u && response[4] == 0x00u);

    /* A live value encodes little-endian: 0x1234 -> 34 12. */
    value.kind = CANCESTRY_VALUE_KIND_UINT;
    value.value.unsigned_integer = 0x1234u;
    CANCESSTRY_TEST_CHECK(cancestry_recipe_signal_store_set(
                              &env.signals, uds_test_signal_id(&env, "diag.RpmTarget"),
                              &value) == CANCESTRY_RECIPE_OK);
    CANCESSTRY_TEST_CHECK(uds_test_process(&env, request, 3u, response, &length) ==
                          CANCESTRY_UDS_OK);
    CANCESSTRY_TEST_CHECK(length == 5u);
    CANCESSTRY_TEST_CHECK(response[3] == 0x34u && response[4] == 0x12u);

    /* A value too large for the declared DID length is 0x22, not a guess. */
    value.value.unsigned_integer = 0x10000u; /* 2-byte DID */
    CANCESSTRY_TEST_CHECK(cancestry_recipe_signal_store_set(
                              &env.signals, uds_test_signal_id(&env, "diag.RpmTarget"),
                              &value) == CANCESTRY_RECIPE_OK);
    CANCESSTRY_TEST_CHECK(uds_test_process(&env, request, 3u, response, &length) ==
                          CANCESTRY_UDS_ERR_DENIED);
    CANCESSTRY_TEST_CHECK(uds_test_is_nrc(response, length, 0x22u, 0x22u));

    /* Reals have no deterministic DID layout at this level: 0x22. */
    value.kind = CANCESTRY_VALUE_KIND_REAL;
    value.value.real = 1.5;
    CANCESSTRY_TEST_CHECK(cancestry_recipe_signal_store_set(
                              &env.signals, uds_test_signal_id(&env, "diag.RpmTarget"),
                              &value) == CANCESTRY_RECIPE_OK);
    CANCESSTRY_TEST_CHECK(uds_test_process(&env, request, 3u, response, &length) ==
                          CANCESTRY_UDS_ERR_DENIED);
    CANCESSTRY_TEST_CHECK(uds_test_is_nrc(response, length, 0x22u, 0x22u));

    uds_test_env_destroy(&env);
}

/* UDS-SVC-003: WriteDataByIdentifier with an approving governor: bytes are
 * stored, the mapped signal is mirrored, unknown DIDs and wrong lengths are
 * refused, and a full signal store reports 0x72 (SW-FR-UDS-003/008). */
static void uds_svc_write(void)
{
    uds_test_env_t env;
    uint8_t request[16];
    uint8_t response[UDS_TEST_RESPONSE_MAX];
    size_t length = 0u;
    cancestry_uds_status_t status;
    cancestry_value_t value;

    /* Approve-everything governor, one signal slot: the first mirrored write
     * fills the shared store. */
    uds_test_env_init(&env, NULL, 1u, true);
    env.policy.approve_all = true;

    /* Static writable DID 0x1234: 6E 12 34 and the slot is updated. */
    request[0] = 0x2Eu;
    request[1] = 0x12u;
    request[2] = 0x34u;
    request[3] = 0x11u;
    request[4] = 0x22u;
    request[5] = 0x33u;
    request[6] = 0x44u;
    status = uds_test_process(&env, request, 7u, response, &length);
    CANCESSTRY_TEST_CHECK(status == CANCESTRY_UDS_OK);
    CANCESSTRY_TEST_CHECK(length == 3u);
    CANCESSTRY_TEST_CHECK(response[0] == 0x6Eu && response[1] == 0x12u && response[2] == 0x34u);
    CANCESSTRY_TEST_CHECK(env.did_data[env.config->dids[3].data_offset + 0u] == 0x11u);
    CANCESSTRY_TEST_CHECK(env.did_data[env.config->dids[3].data_offset + 3u] == 0x44u);
    CANCESSTRY_TEST_CHECK(env.server.counters.did_writes == 1u);

    /* Signal-mapped DID 0x0204: the mirrored value appears in the store. */
    request[1] = 0x02u;
    request[2] = 0x04u;
    request[3] = 0x78u;
    request[4] = 0x56u;
    status = uds_test_process(&env, request, 5u, response, &length);
    CANCESSTRY_TEST_CHECK(status == CANCESTRY_UDS_OK);
    CANCESSTRY_TEST_CHECK(env.policy.last_kind == CANCESTRY_UDS_GOVERNOR_WRITE_DID);
    CANCESSTRY_TEST_CHECK(env.policy.last_has_signal);
    CANCESSTRY_TEST_CHECK(env.policy.last_signal_value == 0x5678u);
    CANCESSTRY_TEST_CHECK(strcmp(env.policy.last_signal_name, "diag.RpmTarget") == 0);
    CANCESSTRY_TEST_CHECK(cancestry_recipe_signal_store_get(
                              &env.signals, uds_test_signal_id(&env, "diag.RpmTarget"),
                              &value) == CANCESTRY_RECIPE_OK);
    CANCESSTRY_TEST_CHECK(value.kind == CANCESTRY_VALUE_KIND_UINT);
    CANCESSTRY_TEST_CHECK(value.value.unsigned_integer == 0x5678u);

    /* The stored bytes changed too, and RDBI serves them back. */
    {
        uint8_t read[3] = {0x22u, 0x02u, 0x04u};

        CANCESSTRY_TEST_CHECK(uds_test_process(&env, read, 3u, response, &length) ==
                              CANCESTRY_UDS_OK);
        CANCESSTRY_TEST_CHECK(length == 5u);
        CANCESSTRY_TEST_CHECK(response[3] == 0x78u && response[4] == 0x56u);
    }

    /* The store has one slot and it is taken: the second mapped DID mirrors
     * into a full store and reports 0x72, leaving no partial effect. */
    request[1] = 0x02u;
    request[2] = 0x05u;
    request[3] = 0x99u;
    request[4] = 0x88u;
    status = uds_test_process(&env, request, 5u, response, &length);
    CANCESSTRY_TEST_CHECK(status == CANCESTRY_UDS_ERR_PROGRAMMING);
    CANCESSTRY_TEST_CHECK(uds_test_is_nrc(response, length, 0x2Eu, 0x72u));
    CANCESSTRY_TEST_CHECK(env.did_data[env.config->dids[2].data_offset + 0u] == 0x00u);
    CANCESSTRY_TEST_CHECK(env.server.counters.nrc_general_programming_failure == 1u);

    /* Unknown DID: 0x31. */
    request[1] = 0x99u;
    request[2] = 0x99u;
    status = uds_test_process(&env, request, 5u, response, &length);
    CANCESSTRY_TEST_CHECK(status == CANCESTRY_UDS_ERR_NOT_FOUND);
    CANCESSTRY_TEST_CHECK(uds_test_is_nrc(response, length, 0x2Eu, 0x31u));

    /* Wrong data length (3 bytes for a 2-byte DID): 0x13. */
    request[1] = 0x02u;
    request[2] = 0x04u;
    status = uds_test_process(&env, request, 6u, response, &length);
    CANCESSTRY_TEST_CHECK(status == CANCESTRY_UDS_ERR_LENGTH);
    CANCESSTRY_TEST_CHECK(uds_test_is_nrc(response, length, 0x2Eu, 0x13u));

    uds_test_env_destroy(&env);
}

/* UDS-SVC-004: RoutineControl start/stop/results with the configured
 * records and the negative codes for unknown routines, unsupported
 * sub-functions and malformed requests (SW-FR-UDS-004). */
static void uds_svc_routine_control(void)
{
    uds_test_env_t env;
    uint8_t request[4];
    uint8_t response[UDS_TEST_RESPONSE_MAX];
    size_t length = 0u;

    uds_test_env_init(&env, NULL, UDS_TEST_SIGNAL_SLOTS_MAX, true);
    request[0] = 0x31u;

    /* start 0x0203: 71 01 02 03 00. */
    request[1] = 0x01u;
    request[2] = 0x02u;
    request[3] = 0x03u;
    CANCESSTRY_TEST_CHECK(uds_test_process(&env, request, 4u, response, &length) ==
                          CANCESTRY_UDS_OK);
    CANCESSTRY_TEST_CHECK(length == 5u);
    CANCESSTRY_TEST_CHECK(response[0] == 0x71u);
    CANCESSTRY_TEST_CHECK(response[1] == 0x01u);
    CANCESSTRY_TEST_CHECK(response[2] == 0x02u && response[3] == 0x03u);
    CANCESSTRY_TEST_CHECK(response[4] == 0x00u);

    /* stop 0x0203: no record configured: 71 02 02 03. */
    request[1] = 0x02u;
    CANCESSTRY_TEST_CHECK(uds_test_process(&env, request, 4u, response, &length) ==
                          CANCESTRY_UDS_OK);
    CANCESSTRY_TEST_CHECK(length == 4u);
    CANCESSTRY_TEST_CHECK(response[0] == 0x71u && response[1] == 0x02u);

    /* requestResults 0x0203: 71 03 02 03 01 02. */
    request[1] = 0x03u;
    CANCESSTRY_TEST_CHECK(uds_test_process(&env, request, 4u, response, &length) ==
                          CANCESTRY_UDS_OK);
    CANCESSTRY_TEST_CHECK(length == 6u);
    CANCESSTRY_TEST_CHECK(response[4] == 0x01u && response[5] == 0x02u);

    /* Unknown routine: 0x31. */
    request[2] = 0x99u;
    request[3] = 0x99u;
    CANCESSTRY_TEST_CHECK(uds_test_process(&env, request, 4u, response, &length) ==
                          CANCESTRY_UDS_ERR_NOT_FOUND);
    CANCESSTRY_TEST_CHECK(uds_test_is_nrc(response, length, 0x31u, 0x31u));

    /* Unsupported sub-function 0x04 (and the suppressed-response bit is not
     * interpreted at this level): 0x12. */
    request[2] = 0x02u;
    request[3] = 0x03u;
    request[1] = 0x04u;
    CANCESSTRY_TEST_CHECK(uds_test_process(&env, request, 4u, response, &length) ==
                          CANCESTRY_UDS_ERR_UNSUPPORTED);
    CANCESSTRY_TEST_CHECK(uds_test_is_nrc(response, length, 0x31u, 0x12u));
    request[1] = 0x81u;
    CANCESSTRY_TEST_CHECK(uds_test_process(&env, request, 4u, response, &length) ==
                          CANCESTRY_UDS_ERR_UNSUPPORTED);
    CANCESSTRY_TEST_CHECK(uds_test_is_nrc(response, length, 0x31u, 0x12u));

    /* Disallowed sub-function (routine 0x0300 declares start allowed false):
     * 0x12. */
    request[1] = 0x01u;
    request[2] = 0x03u;
    request[3] = 0x00u;
    CANCESSTRY_TEST_CHECK(uds_test_process(&env, request, 4u, response, &length) ==
                          CANCESTRY_UDS_ERR_UNSUPPORTED);
    CANCESSTRY_TEST_CHECK(uds_test_is_nrc(response, length, 0x31u, 0x12u));

    /* Undeclared sub-function (routine 0x0301 declares only results): 0x12
     * for start and stop, results works. */
    request[1] = 0x01u;
    request[2] = 0x03u;
    request[3] = 0x01u;
    CANCESSTRY_TEST_CHECK(uds_test_process(&env, request, 4u, response, &length) ==
                          CANCESTRY_UDS_ERR_UNSUPPORTED);
    request[1] = 0x02u;
    CANCESSTRY_TEST_CHECK(uds_test_process(&env, request, 4u, response, &length) ==
                          CANCESTRY_UDS_ERR_UNSUPPORTED);
    request[1] = 0x03u;
    CANCESSTRY_TEST_CHECK(uds_test_process(&env, request, 4u, response, &length) ==
                          CANCESTRY_UDS_OK);
    CANCESSTRY_TEST_CHECK(length == 5u);
    CANCESSTRY_TEST_CHECK(response[4] == 0x7Fu);

    /* Malformed lengths: 3 bytes and 5 bytes are both 0x13 (no routine
     * request data at this level). */
    request[1] = 0x01u;
    request[2] = 0x02u;
    request[3] = 0x03u;
    {
        uint8_t short_request[3] = {0x31u, 0x01u, 0x02u};

        CANCESSTRY_TEST_CHECK(uds_test_process(&env, short_request, 3u, response, &length) ==
                              CANCESTRY_UDS_ERR_LENGTH);
        CANCESSTRY_TEST_CHECK(uds_test_is_nrc(response, length, 0x31u, 0x13u));
    }
    {
        uint8_t long_request[5] = {0x31u, 0x01u, 0x02u, 0x03u, 0x00u};

        CANCESSTRY_TEST_CHECK(uds_test_process(&env, long_request, 5u, response, &length) ==
                              CANCESTRY_UDS_ERR_LENGTH);
        CANCESSTRY_TEST_CHECK(uds_test_is_nrc(response, length, 0x31u, 0x13u));
    }
    CANCESSTRY_TEST_CHECK(env.server.counters.routine_controls == 4u);

    uds_test_env_destroy(&env);
}

/* UDS-SVC-005: unsupported services answer 7F SID 11, including a
 * zero-length request with a placeholder SID (SW-FR-UDS-005). */
static void uds_svc_unsupported_service(void)
{
    uds_test_env_t env;
    uint8_t response[UDS_TEST_RESPONSE_MAX];
    size_t length = 0u;

    uds_test_env_init(&env, NULL, UDS_TEST_SIGNAL_SLOTS_MAX, true);
    {
        uint8_t request[2] = {0x10u, 0x03u}; /* DiagnosticSessionControl */

        CANCESSTRY_TEST_CHECK(uds_test_process(&env, request, 2u, response, &length) ==
                              CANCESTRY_UDS_ERR_UNSUPPORTED);
        CANCESSTRY_TEST_CHECK(uds_test_is_nrc(response, length, 0x10u, 0x11u));
    }
    {
        uint8_t request[2] = {0x27u, 0x01u}; /* SecurityAccess */

        CANCESSTRY_TEST_CHECK(uds_test_process(&env, request, 2u, response, &length) ==
                              CANCESTRY_UDS_ERR_UNSUPPORTED);
        CANCESSTRY_TEST_CHECK(uds_test_is_nrc(response, length, 0x27u, 0x11u));
    }
    {
        uint8_t request[1] = {0x3Eu}; /* TesterPresent */

        CANCESSTRY_TEST_CHECK(uds_test_process(&env, request, 1u, response, &length) ==
                              CANCESTRY_UDS_ERR_UNSUPPORTED);
        CANCESSTRY_TEST_CHECK(uds_test_is_nrc(response, length, 0x3Eu, 0x11u));
    }
    {
        uint8_t empty[1] = {0x00u};

        /* A zero-length request is an unsupported service with SID 0. */
        CANCESSTRY_TEST_CHECK(uds_test_process(&env, empty, 0u, response, &length) ==
                              CANCESTRY_UDS_ERR_UNSUPPORTED);
    }
    CANCESSTRY_TEST_CHECK(length == 3u);
    CANCESSTRY_TEST_CHECK(response[0] == 0x7Fu && response[1] == 0x00u &&
                          response[2] == 0x11u);
    CANCESSTRY_TEST_CHECK(env.server.counters.nrc_service_not_supported == 4u);
    CANCESSTRY_TEST_CHECK(env.server.counters.requests_processed == 4u);

    uds_test_env_destroy(&env);
}

/* UDS-SVC-006: the loader validates against schemas/uds-0.1.0.schema.json:
 * the demo document loads with exact offsets and defaults; unknown fields,
 * bad versions, duplicates, out-of-range values, flow syntax, mismatched
 * defaults and signal-mapped lengths over 8 bytes are refused with a
 * located message (SW-FR-UDS-006). */
static void uds_svc_loader_validation(void)
{
    cancestry_uds_load_error_t error;
    cancestry_uds_config_t *config;

    /* The demo document loads: 4 DIDs, 3 routines, exact store layout. */
    config = cancestry_uds_config_load(uds_test_config_yaml(),
                                       strlen(uds_test_config_yaml()), &error);
    CANCESSTRY_TEST_CHECK(config != NULL);
    CANCESSTRY_TEST_CHECK(config->did_count == 6u);
    CANCESSTRY_TEST_CHECK(config->routine_count == 3u);
    CANCESSTRY_TEST_CHECK(config->did_data_length == 17u + 2u + 2u + 4u + 2u + 6u);
    CANCESSTRY_TEST_CHECK(config->dids[0].data_offset == 0u);
    CANCESSTRY_TEST_CHECK(config->dids[1].data_offset == 17u);
    CANCESSTRY_TEST_CHECK(config->dids[3].data_offset == 21u);
    CANCESSTRY_TEST_CHECK(config->dids[4].data_offset == 25u);
    CANCESSTRY_TEST_CHECK(config->dids[4].read_only);
    CANCESSTRY_TEST_CHECK(config->dids[4].signal == NULL);
    CANCESSTRY_TEST_CHECK(config->dids[5].data_offset == 27u);
    CANCESSTRY_TEST_CHECK(!config->dids[5].read_only);
    CANCESSTRY_TEST_CHECK(config->dids[5].signal == NULL);
    CANCESSTRY_TEST_CHECK(config->dids[0].read_only);
    CANCESSTRY_TEST_CHECK(!config->dids[3].read_only);
    CANCESSTRY_TEST_CHECK(config->dids[1].signal != NULL);
    CANCESSTRY_TEST_CHECK(config->dids[3].signal == NULL);
    CANCESSTRY_TEST_CHECK(config->routines[0].start.declared &&
                          config->routines[0].start.allowed);
    CANCESSTRY_TEST_CHECK(config->routines[0].start.response_length == 1u);
    CANCESSTRY_TEST_CHECK(config->routines[0].stop.declared);
    CANCESSTRY_TEST_CHECK(config->routines[0].stop.response_length == 0u);
    CANCESSTRY_TEST_CHECK(!config->routines[2].start.declared);
    CANCESSTRY_TEST_CHECK(config->routines[1].start.declared &&
                          !config->routines[1].start.allowed);
    cancestry_uds_config_free(config);

    /* Unknown field (additionalProperties: false). */
    {
        const char *doc = "schema_version: \"0.1.0\"\n"
                          "uds:\n"
                          "  dids:\n"
                          "    - did: 0x0001\n"
                          "      length: 1\n"
                          "      writable: true\n";

        config = cancestry_uds_config_load(doc, strlen(doc), &error);
        CANCESSTRY_TEST_CHECK(config == NULL);
        CANCESSTRY_TEST_CHECK(error.status == CANCESTRY_UDS_ERR_PARSE);
        CANCESSTRY_TEST_CHECK(error.line != 0u);
    }

    /* Wrong schema version. */
    {
        const char *doc = "schema_version: \"0.2.0\"\n"
                          "uds:\n"
                          "  dids:\n"
                          "    - did: 0x0001\n"
                          "      length: 1\n";

        config = cancestry_uds_config_load(doc, strlen(doc), &error);
        CANCESSTRY_TEST_CHECK(config == NULL);
        CANCESSTRY_TEST_CHECK(error.status == CANCESTRY_UDS_ERR_PARSE);
    }

    /* Duplicate DID. */
    {
        const char *doc = "schema_version: \"0.1.0\"\n"
                          "uds:\n"
                          "  dids:\n"
                          "    - did: 0x0001\n"
                          "      length: 1\n"
                          "    - did: 0x0001\n"
                          "      length: 2\n";

        config = cancestry_uds_config_load(doc, strlen(doc), &error);
        CANCESSTRY_TEST_CHECK(config == NULL);
        CANCESSTRY_TEST_CHECK(error.status == CANCESTRY_UDS_ERR_CONFLICT);
    }

    /* Duplicate routine. */
    {
        const char *doc = "schema_version: \"0.1.0\"\n"
                          "uds:\n"
                          "  routines:\n"
                          "    - routine: 0x0203\n"
                          "    - routine: 0x0203\n";

        config = cancestry_uds_config_load(doc, strlen(doc), &error);
        CANCESSTRY_TEST_CHECK(config == NULL);
        CANCESSTRY_TEST_CHECK(error.status == CANCESTRY_UDS_ERR_CONFLICT);
    }

    /* Out-of-range values: DID id, length, byte value. */
    {
        const char *doc = "schema_version: \"0.1.0\"\n"
                          "uds:\n"
                          "  dids:\n"
                          "    - did: 0x10000\n"
                          "      length: 1\n";

        config = cancestry_uds_config_load(doc, strlen(doc), &error);
        CANCESSTRY_TEST_CHECK(config == NULL);
        CANCESSTRY_TEST_CHECK(error.status == CANCESTRY_UDS_ERR_PARSE);
    }
    {
        const char *doc = "schema_version: \"0.1.0\"\n"
                          "uds:\n"
                          "  dids:\n"
                          "    - did: 0x0001\n"
                          "      length: 0\n";

        config = cancestry_uds_config_load(doc, strlen(doc), &error);
        CANCESSTRY_TEST_CHECK(config == NULL);
    }
    {
        const char *doc = "schema_version: \"0.1.0\"\n"
                          "uds:\n"
                          "  dids:\n"
                          "    - did: 0x0001\n"
                          "      length: 1\n"
                          "      default:\n"
                          "        - 0x100\n";

        config = cancestry_uds_config_load(doc, strlen(doc), &error);
        CANCESSTRY_TEST_CHECK(config == NULL);
    }

    /* Default array length must match the DID length. */
    {
        const char *doc = "schema_version: \"0.1.0\"\n"
                          "uds:\n"
                          "  dids:\n"
                          "    - did: 0x0001\n"
                          "      length: 2\n"
                          "      default:\n"
                          "        - 0x01\n";

        config = cancestry_uds_config_load(doc, strlen(doc), &error);
        CANCESSTRY_TEST_CHECK(config == NULL);
    }

    /* A signal-mapped DID longer than 8 bytes cannot be mirrored: refused. */
    {
        const char *doc = "schema_version: \"0.1.0\"\n"
                          "uds:\n"
                          "  dids:\n"
                          "    - did: 0x0001\n"
                          "      length: 9\n"
                          "      signal: diag.RpmTarget\n";

        config = cancestry_uds_config_load(doc, strlen(doc), &error);
        CANCESSTRY_TEST_CHECK(config == NULL);
    }

    /* Flow collections are outside the YAML subset. */
    {
        const char *doc = "schema_version: \"0.1.0\"\n"
                          "uds:\n"
                          "  dids:\n"
                          "    - did: 0x0001\n"
                          "      length: 2\n"
                          "      default: [0x01, 0x02]\n";

        config = cancestry_uds_config_load(doc, strlen(doc), &error);
        CANCESSTRY_TEST_CHECK(config == NULL);
    }

    /* A malformed signal name is refused (schema pattern). */
    {
        const char *doc = "schema_version: \"0.1.0\"\n"
                          "uds:\n"
                          "  dids:\n"
                          "    - did: 0x0001\n"
                          "      length: 2\n"
                          "      signal: 1bad.name\n";

        config = cancestry_uds_config_load(doc, strlen(doc), &error);
        CANCESSTRY_TEST_CHECK(config == NULL);
    }
}

/* UDS-SVC-007: local resource bounds. A response buffer that is too small
 * reports ERR_CAPACITY without writing; a DID signal that does not resolve
 * fails server init fail-closed (SW-FR-UDS-001/008). */
static void uds_svc_resource_bounds(void)
{
    uds_test_env_t env;
    uint8_t request[3];
    uint8_t response[2]; /* too small for every response */
    size_t length = 99u;
    cancestry_uds_server_t server;

    uds_test_env_init(&env, NULL, UDS_TEST_SIGNAL_SLOTS_MAX, true);
    request[0] = 0x22u;
    request[1] = 0xF1u;
    request[2] = 0x90u;

    CANCESSTRY_TEST_CHECK(cancestry_uds_server_process(&env.server, request, 3u, response,
                                                       2u, &length) ==
                          CANCESTRY_UDS_ERR_CAPACITY);
    CANCESSTRY_TEST_CHECK(length == 99u); /* untouched */

    /* A signal-mapped DID whose name does not resolve fails init. */
    {
        const char *doc = "schema_version: \"0.1.0\"\n"
                          "uds:\n"
                          "  dids:\n"
                          "    - did: 0x0001\n"
                          "      length: 2\n"
                          "      signal: diag.DoesNotExist\n";
        cancestry_uds_config_t *config =
            cancestry_uds_config_load(doc, strlen(doc), NULL);
        cancestry_uds_server_config_t server_config;
        uint8_t did_data[4];
        cancestry_signal_id_t ids[1];

        CANCESSTRY_TEST_CHECK(config != NULL);
        server_config.config = config;
        server_config.did_data = did_data;
        server_config.did_signal_ids = ids;
        server_config.namespace = &env.namespace;
        server_config.signals = &env.signals;
        server_config.governor = NULL;
        server_config.governor_user_data = NULL;
        CANCESSTRY_TEST_CHECK(!cancestry_uds_server_init(&server, &server_config));
        CANCESSTRY_TEST_CHECK(!cancestry_uds_server_is_valid(&server));
        cancestry_uds_config_free(config);
    }

    uds_test_env_destroy(&env);
}

int main(void)
{
    CANCESSTRY_TEST_SUITE_BEGIN("uds services conformance");
    CANCESSTRY_TEST_CASE("UDS-SVC-001 read static DIDs");
    uds_svc_read_static();
    CANCESSTRY_TEST_CASE("UDS-SVC-002 read signal-mapped DIDs");
    uds_svc_read_signal_mapped();
    CANCESSTRY_TEST_CASE("UDS-SVC-003 write DIDs");
    uds_svc_write();
    CANCESSTRY_TEST_CASE("UDS-SVC-004 routine control");
    uds_svc_routine_control();
    CANCESSTRY_TEST_CASE("UDS-SVC-005 unsupported service");
    uds_svc_unsupported_service();
    CANCESSTRY_TEST_CASE("UDS-SVC-006 loader schema validation");
    uds_svc_loader_validation();
    CANCESSTRY_TEST_CASE("UDS-SVC-007 resource bounds");
    uds_svc_resource_bounds();
    return CANCESSTRY_TEST_SUITE_END();
}
