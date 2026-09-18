/*
 * Shared fixture for the CANcestry UDS conformance suite (Phase 10,
 * issue #26).
 *
 * The suite verifies the behavioural contract of docs/software/SwRS.md
 * section 14.2 (SW-FR-UDS-001..008) against the shipping stack: the UDS
 * configuration is loaded through cancestry_uds_config_load() (which
 * validates it against schemas/uds-0.1.0.schema.json), DID signal mappings
 * resolve through a real codec namespace, values mirror into the shared
 * signal value store the recipe engine uses, and every write/routine side
 * effect passes through a governor stub that mirrors the FSM capability
 * semantics (a signal write allowlist). A test that passes through this
 * fixture has exercised the loader, the schema validation, the service
 * dispatch, the governor checkpoint and the signal integration at once.
 *
 * Everything is static inline: the suite has no build-time dependency beyond
 * the core libraries, and each test stays an independent executable (the
 * convention of tests/unit/support/cancestry_test.h).
 */

#ifndef CANCESTRY_UDS_CONFORMANCE_SUPPORT_H
#define CANCESTRY_UDS_CONFORMANCE_SUPPORT_H

#include "cancestry/codec/loader.h"
#include "cancestry/codec/namespace.h"
#include "cancestry/recipe/engine.h"
#include "cancestry/uds/loader.h"
#include "cancestry/uds/services.h"

#include "cancestry_test.h"

#include <string.h>

/* ------------------------------------------------------------------------- */
/* Sizes                                                                     */
/* ------------------------------------------------------------------------- */

#define UDS_TEST_DID_DATA_MAX ((size_t)128u)
#define UDS_TEST_DID_IDS_MAX ((size_t)8u)
#define UDS_TEST_SIGNAL_SLOTS_MAX ((size_t)4u)
#define UDS_TEST_RESPONSE_MAX ((size_t)64u)

/* ------------------------------------------------------------------------- */
/* Demo configuration                                                        */
/* ------------------------------------------------------------------------- */

/** Codec map with the signals the demo DIDs map to (SW-FR-UDS-008). */
static inline const char *uds_test_codec_yaml(void)
{
    return "schema_version: \"0.2.0\"\n"
           "codec_map:\n"
           "  name: diag\n"
           "  version: 1.0.0\n"
           "  messages:\n"
           "    - id: 0x500\n"
           "      name: DiagMsg\n"
           "      dlc: 8\n"
           "      signals:\n"
           "        - name: RpmTarget\n"
           "          start_bit: 0\n"
           "          bit_length: 16\n"
           "          type: uint\n"
           "          endianness: little\n"
           "        - name: RpmActual\n"
           "          start_bit: 16\n"
           "          bit_length: 16\n"
           "          type: uint\n"
           "          endianness: little\n";
}

/**
 * Demo UDS configuration (schema: schemas/uds-0.1.0.schema.json).
 *
 *   DID 0xF190  VIN, 17 bytes, read-only, static default (governor denial).
 *   DID 0x0204  2 bytes, mapped to signal diag.RpmTarget (live values).
 *   DID 0x0205  2 bytes, mapped to signal diag.RpmActual (store-capacity case).
 *   DID 0x1234  4 bytes, writable, static default (plain WDBI/RDBI).
 *   DID 0xD000  2 bytes, read-only, static default (single-frame WDBI denial
 *               over the transport loop; the VIN is too long for one frame).
 *   DID 0x1235  6 bytes, writable, static default (a WDBI for it is 9 bytes,
 *               i.e. a genuine multi-frame ISO-TP request).
 *   Routine 0x0203  start (record 00), stop (no record), results (record 01 02).
 *   Routine 0x0300  start declared but disallowed (NRC 0x12).
 *   Routine 0x0301  only results declared (start/stop -> NRC 0x12).
 */
static inline const char *uds_test_config_yaml(void)
{
    return "schema_version: \"0.1.0\"\n"
           "uds:\n"
           "  dids:\n"
           "    - did: 0xF190\n"
           "      length: 17\n"
           "      read_only: true\n"
           "      default:\n"
           "        - 0x57\n"
           "        - 0x41\n"
           "        - 0x53\n"
           "        - 0x00\n"
           "        - 0x00\n"
           "        - 0x00\n"
           "        - 0x00\n"
           "        - 0x00\n"
           "        - 0x00\n"
           "        - 0x00\n"
           "        - 0x00\n"
           "        - 0x00\n"
           "        - 0x00\n"
           "        - 0x00\n"
           "        - 0x00\n"
           "        - 0x00\n"
           "        - 0x00\n"
           "    - did: 0x0204\n"
           "      length: 2\n"
           "      signal: diag.RpmTarget\n"
           "      default:\n"
           "        - 0x00\n"
           "        - 0x00\n"
           "    - did: 0x0205\n"
           "      length: 2\n"
           "      signal: diag.RpmActual\n"
           "      default:\n"
           "        - 0x00\n"
           "        - 0x00\n"
           "    - did: 0x1234\n"
           "      length: 4\n"
           "      default:\n"
           "        - 0xDE\n"
           "        - 0xAD\n"
           "        - 0xBE\n"
           "        - 0xEF\n"
           "    - did: 0xD000\n"
           "      length: 2\n"
           "      read_only: true\n"
           "      default:\n"
           "        - 0x00\n"
           "        - 0x00\n"
           "    - did: 0x1235\n"
           "      length: 6\n"
           "      default:\n"
           "        - 0x00\n"
           "        - 0x00\n"
           "        - 0x00\n"
           "        - 0x00\n"
           "        - 0x00\n"
           "        - 0x00\n"
           "  routines:\n"
           "    - routine: 0x0203\n"
           "      start:\n"
           "        response:\n"
           "          - 0x00\n"
           "      stop:\n"
           "        allowed: true\n"
           "      results:\n"
           "        response:\n"
           "          - 0x01\n"
           "          - 0x02\n"
           "    - routine: 0x0300\n"
           "      start:\n"
           "        allowed: false\n"
           "    - routine: 0x0301\n"
           "      results:\n"
           "        response:\n"
           "          - 0x7F\n";
}

/* ------------------------------------------------------------------------- */
/* Governor policy: the FSM capability semantics as a stub                    */
/* ------------------------------------------------------------------------- */

/**
 * Governor policy under test: deny writes to read-only DIDs and to signals
 * outside the write allowlist (the semantics of
 * cancestry_fsm_capabilities_t::signal_write), optionally deny routines.
 */
typedef struct uds_test_policy {
    /** Approve everything (policy bypass, for positive paths). */
    bool approve_all;
    /** Deny every routine execution. */
    bool deny_routines;
    /** Write allowlist; NULL or count 0 means "no signal may be written". */
    const char *const *signal_write;
    size_t signal_write_count;
    /* Diagnostics of the last request (scalars and stable pointers only). */
    size_t calls;
    cancestry_uds_governor_request_kind_t last_kind;
    uint16_t last_did;
    uint16_t last_routine;
    uint8_t last_subfunction;
    bool last_read_only;
    bool last_has_signal;
    const char *last_signal_name;
    cancestry_signal_id_t last_signal_id;
    uint64_t last_signal_value;
    size_t last_data_length;
} uds_test_policy_t;

static inline cancestry_uds_governor_decision_t uds_test_governor(
    void *user_data,
    const cancestry_uds_governor_request_t *request)
{
    uds_test_policy_t *policy = (uds_test_policy_t *)user_data;

    policy->calls++;
    policy->last_kind = request->kind;
    policy->last_did = request->did;
    policy->last_routine = request->routine_id;
    policy->last_subfunction = request->subfunction;
    policy->last_data_length = request->data_length;
    policy->last_has_signal = request->has_signal;
    policy->last_signal_name = request->signal_name;
    policy->last_signal_id = request->signal_id;
    policy->last_signal_value =
        (request->has_signal) ? request->signal_value.value.unsigned_integer : 0u;
    if (request->did_entry != NULL) {
        policy->last_read_only = request->did_entry->read_only;
    }

    if (policy->approve_all) {
        return CANCESTRY_UDS_GOVERNOR_APPROVE;
    }
    if (request->kind == CANCESTRY_UDS_GOVERNOR_RUN_ROUTINE) {
        return policy->deny_routines ? CANCESTRY_UDS_GOVERNOR_DENY
                                     : CANCESTRY_UDS_GOVERNOR_APPROVE;
    }
    if (request->did_entry != NULL && request->did_entry->read_only) {
        return CANCESTRY_UDS_GOVERNOR_DENY;
    }
    if (request->has_signal) {
        size_t i;

        for (i = 0u; i < policy->signal_write_count; ++i) {
            if (strcmp(policy->signal_write[i], request->signal_name) == 0) {
                return CANCESTRY_UDS_GOVERNOR_APPROVE;
            }
        }
        return CANCESTRY_UDS_GOVERNOR_DENY;
    }
    return CANCESTRY_UDS_GOVERNOR_APPROVE;
}

/* ------------------------------------------------------------------------- */
/* Environment                                                               */
/* ------------------------------------------------------------------------- */

typedef struct uds_test_env {
    /* Definitions. */
    cancestry_codec_map_t *codec_map;
    const cancestry_codec_map_t *namespace_slots[1];
    cancestry_codec_namespace_t namespace;
    cancestry_uds_config_t *config;

    /* Caller-owned runtime storage. */
    uint8_t did_data[UDS_TEST_DID_DATA_MAX];
    cancestry_signal_id_t did_signal_ids[UDS_TEST_DID_IDS_MAX];
    cancestry_recipe_signal_store_t signals;
    cancestry_recipe_signal_slot_t signal_slots[UDS_TEST_SIGNAL_SLOTS_MAX];

    /* Server and governor. */
    cancestry_uds_server_t server;
    uds_test_policy_t policy;
    bool with_governor;
} uds_test_env_t;

/**
 * Bring up the demo environment.
 *
 * @param config_yaml  The UDS configuration document (default:
 *                     uds_test_config_yaml()).
 * @param signal_slots Signal store capacity (small values exercise the
 *                     bounded-store paths; 0 disables the store entirely).
 * @param with_governor true binds uds_test_governor(); false leaves the
 *                     governor NULL (the fail-closed default).
 */
static inline void uds_test_env_init(uds_test_env_t *env, const char *config_yaml,
                              size_t signal_slots, bool with_governor)
{
    cancestry_codec_load_error_t codec_error;
    cancestry_uds_load_error_t uds_error;
    cancestry_uds_server_config_t server_config;

    memset(env, 0, sizeof(*env));
    env->with_governor = with_governor;

    env->codec_map = cancestry_codec_map_load(uds_test_codec_yaml(),
                                              strlen(uds_test_codec_yaml()), &codec_error);
    CANCESSTRY_TEST_CHECK(env->codec_map != NULL);

    env->namespace_slots[0] = env->codec_map;
    CANCESSTRY_TEST_CHECK(cancestry_codec_namespace_init(&env->namespace, env->namespace_slots,
                                                         1u));
    CANCESSTRY_TEST_CHECK(cancestry_codec_namespace_register(&env->namespace, env->codec_map) ==
                          CANCESTRY_CODEC_OK);

    if (config_yaml == NULL) {
        config_yaml = uds_test_config_yaml();
    }
    env->config = cancestry_uds_config_load(config_yaml, strlen(config_yaml), &uds_error);
    CANCESSTRY_TEST_CHECK(env->config != NULL);
    CANCESSTRY_TEST_CHECK(env->config->did_data_length <= UDS_TEST_DID_DATA_MAX);
    CANCESSTRY_TEST_CHECK((size_t)env->config->did_count <= UDS_TEST_DID_IDS_MAX);

    if (signal_slots > 0u) {
        CANCESSTRY_TEST_CHECK(cancestry_recipe_signal_store_init(
            &env->signals, env->signal_slots, signal_slots));
    }

    server_config.config = env->config;
    server_config.did_data = env->did_data;
    server_config.did_signal_ids = env->did_signal_ids;
    server_config.namespace = &env->namespace;
    server_config.signals = (signal_slots > 0u) ? &env->signals : NULL;
    server_config.governor = with_governor ? uds_test_governor : NULL;
    server_config.governor_user_data = &env->policy;
    CANCESSTRY_TEST_CHECK(cancestry_uds_server_init(&env->server, &server_config));
}

/**
 * Tear the demo environment down again. The loader owns heap allocations by
 * design (load-time only, never on the runtime path), so a conformance case
 * must release them before it exits - the rest of the suite runs under
 * AddressSanitizer/LeakSanitizer.
 */
static inline void uds_test_env_destroy(uds_test_env_t *env)
{
    if (env->config != NULL) {
        cancestry_uds_config_free(env->config);
        env->config = NULL;
    }
    if (env->codec_map != NULL) {
        cancestry_codec_map_free(env->codec_map);
        env->codec_map = NULL;
    }
}

/** Process one request through the demo server (SW-FR-UDS-001). */
static inline cancestry_uds_status_t uds_test_process(uds_test_env_t *env,
                                               const uint8_t *request,
                                               size_t request_length,
                                               uint8_t *response,
                                               size_t *response_length)
{
    return cancestry_uds_server_process(&env->server, request, request_length, response,
                                        UDS_TEST_RESPONSE_MAX, response_length);
}

/** @return true when @p response starts with 7F @p sid @p nrc. */
static inline bool uds_test_is_nrc(const uint8_t *response, size_t length, uint8_t sid, uint8_t nrc)
{
    return length == 3u && response[0] == 0x7Fu && response[1] == sid && response[2] == nrc;
}

/** Resolve a demo signal name to its stable id through the demo namespace. */
static inline cancestry_signal_id_t uds_test_signal_id(uds_test_env_t *env, const char *name)
{
    cancestry_codec_resolution_t resolution;

    CANCESSTRY_TEST_CHECK(cancestry_codec_namespace_resolve(&env->namespace, name,
                                                            &resolution) == CANCESTRY_CODEC_OK);
    return resolution.signal_id;
}

#endif /* CANCESTRY_UDS_CONFORMANCE_SUPPORT_H */
