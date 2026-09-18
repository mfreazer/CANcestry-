/*
 * CANcestry - UDS server: request processing (ISO 14229-1 subset).
 *
 * Normative references:
 *   ISO 14229-1             services 0x22 / 0x2E / 0x31 and the negative
 *                           response codes used here (0x11, 0x12, 0x13,
 *                           0x22, 0x31, 0x72)
 *   docs/software/SwRS.md   SW-FR-UDS-001 .. SW-FR-UDS-008 (Phase 10, issue #26)
 *   docs/system/governor.md fail-closed approval flow (stub level)
 *   docs/system/SyRS.md     SYS-NF-001 (determinism), SYS-NF-002 (bounded resources)
 *
 * Behavioural contract (the conformance suites pin every clause):
 *   - Pure function of (server state, request bytes): the same request always
 *     produces the same response and the same counter deltas (SYS-NF-001).
 *   - Every WriteDataByIdentifier and RoutineControl side effect goes through
 *     the governor synchronously, before any effect happens; a NULL governor
 *     denies everything (SW-FR-GOV-006), a denial leaves no partial effect
 *     and is answered with NRC 0x22 (SW-FR-UDS-007).
 *   - A signal-mapped DID encodes its current shared signal value
 *     little-endian on reads and mirrors written bytes into the signal as an
 *     unsigned integer on approved writes (SW-FR-UDS-008).
 *
 * This file is allocation-free by construction (SYS-NF-002); CI symbol-scans
 * the cancestry_uds archive.
 */

#include "cancestry/uds/services.h"

#include <string.h>

/* ------------------------------------------------------------------------- */
/* Internal helpers                                                          */
/* ------------------------------------------------------------------------- */

/** Build a negative response 7F SID NRC and return the mirrored status. */
static cancestry_uds_status_t uds_negative(cancestry_uds_server_t *server,
                                           uint8_t sid,
                                           uint8_t nrc,
                                           cancestry_uds_status_t status,
                                           uint8_t *response,
                                           size_t response_capacity,
                                           size_t *response_length)
{
    if (response_capacity < 3u) {
        return CANCESTRY_UDS_ERR_CAPACITY;
    }
    response[0] = CANCESTRY_UDS_SID_NEGATIVE;
    response[1] = sid;
    response[2] = nrc;
    *response_length = 3u;

    server->counters.negative_responses++;
    switch (nrc) {
    case CANCESTRY_UDS_NRC_SERVICE_NOT_SUPPORTED:
        server->counters.nrc_service_not_supported++;
        break;
    case CANCESTRY_UDS_NRC_SUB_FUNCTION_NOT_SUPPORTED:
        server->counters.nrc_sub_function_not_supported++;
        break;
    case CANCESTRY_UDS_NRC_INCORRECT_MESSAGE_LENGTH:
        server->counters.nrc_incorrect_message_length++;
        break;
    case CANCESTRY_UDS_NRC_CONDITIONS_NOT_CORRECT:
        server->counters.nrc_conditions_not_correct++;
        break;
    case CANCESTRY_UDS_NRC_REQUEST_OUT_OF_RANGE:
        server->counters.nrc_request_out_of_range++;
        break;
    case CANCESTRY_UDS_NRC_GENERAL_PROGRAMMING_FAILURE:
        server->counters.nrc_general_programming_failure++;
        break;
    default:
        break;
    }
    return status;
}

/** Decode little-endian bytes (at most 8) into an unsigned integer. */
static uint64_t uds_decode_le(const uint8_t *data, size_t length)
{
    uint64_t value = 0u;
    size_t i = length;

    while (i > 0u) {
        i--;
        value = (value << 8) | (uint64_t)data[i];
    }
    return value;
}

/**
 * Encode a signal value little-endian into @p length bytes
 * (SW-FR-UDS-008).
 *
 * @return true when the value is representable; false means the value cannot
 *         be encoded in the declared DID length (or is a real), which the
 *         caller reports as NRC 0x22.
 */
static bool uds_encode_value_le(const cancestry_value_t *value, uint8_t *out, uint16_t length)
{
    uint64_t raw;
    uint16_t i;

    switch (value->kind) {
    case CANCESTRY_VALUE_KIND_BOOL:
        raw = value->value.boolean ? 1u : 0u;
        if (length < 8u && raw > ((uint64_t)1u << (8u * length)) - 1u) {
            return false;
        }
        break;
    case CANCESTRY_VALUE_KIND_UINT:
        raw = value->value.unsigned_integer;
        if (length < 8u && raw > (((uint64_t)1u << (8u * length)) - 1u)) {
            return false;
        }
        break;
    case CANCESTRY_VALUE_KIND_INT:
        if (length == 8u) {
            raw = (uint64_t)value->value.integer; /* modulo 2^64: well-defined */
        } else {
            int64_t v = value->value.integer;
            int64_t bound = ((int64_t)1) << ((int)(8u * length) - 1);

            if (v < -bound || v > bound - 1) {
                return false;
            }
            raw = (uint64_t)v;
        }
        break;
    case CANCESTRY_VALUE_KIND_REAL:
    case CANCESTRY_VALUE_KIND_UNSET:
    case CANCESTRY_VALUE_KIND_COUNT:
    default:
        /* A deterministic DID byte layout for reals is not defined at this
         * level; the read is refused instead of guessed (fail-closed). */
        return false;
    }
    for (i = 0u; i < length; ++i) {
        out[i] = (uint8_t)((raw >> (8u * (uint32_t)i)) & 0xFFu);
    }
    return true;
}

/** @return true when the DID maps to a signal the server can mirror. */
static bool uds_did_signal_index(const cancestry_uds_server_t *server,
                                 uint16_t index,
                                 cancestry_signal_id_t *id_out)
{
    if (server->config->dids[index].signal == NULL) {
        return false;
    }
    *id_out = server->did_signal_ids[index];
    return true;
}

/** Consult the governor; the fail-closed default denies (SW-FR-UDS-007). */
static bool uds_governor_approves(cancestry_uds_server_t *server,
                                  const cancestry_uds_governor_request_t *request)
{
    cancestry_uds_governor_decision_t decision;

    if (server->governor == NULL) {
        /* No governor configured: deny every side effect (SW-FR-GOV-006). */
        server->counters.governor_denials++;
        return false;
    }
    decision = server->governor(server->governor_user_data, request);
    if (decision != CANCESTRY_UDS_GOVERNOR_APPROVE) {
        server->counters.governor_denials++;
        return false;
    }
    return true;
}

/* ------------------------------------------------------------------------- */
/* Config lookups                                                            */
/* ------------------------------------------------------------------------- */

const cancestry_uds_did_t *cancestry_uds_config_find_did(const cancestry_uds_config_t *config,
                                                         uint16_t did,
                                                         uint16_t *index_out)
{
    uint16_t i;

    if (config == NULL) {
        return NULL;
    }
    for (i = 0u; i < config->did_count; ++i) {
        if (config->dids[i].did == did) {
            if (index_out != NULL) {
                *index_out = i;
            }
            return &config->dids[i];
        }
    }
    return NULL;
}

const cancestry_uds_routine_t *cancestry_uds_config_find_routine(
    const cancestry_uds_config_t *config,
    uint16_t routine,
    uint16_t *index_out)
{
    uint16_t i;

    if (config == NULL) {
        return NULL;
    }
    for (i = 0u; i < config->routine_count; ++i) {
        if (config->routines[i].routine == routine) {
            if (index_out != NULL) {
                *index_out = i;
            }
            return &config->routines[i];
        }
    }
    return NULL;
}

/* ------------------------------------------------------------------------- */
/* Lifecycle                                                                 */
/* ------------------------------------------------------------------------- */

bool cancestry_uds_server_init(cancestry_uds_server_t *server,
                               const cancestry_uds_server_config_t *config)
{
    uint16_t i;

    if (server == NULL || config == NULL || config->config == NULL) {
        return false;
    }
    if (config->config->did_count > 0u &&
        (config->did_data == NULL || config->did_signal_ids == NULL)) {
        return false;
    }

    memset(server, 0, sizeof(*server));
    server->config = config->config;
    server->did_data = config->did_data;
    server->did_signal_ids = config->did_signal_ids;
    server->namespace = config->namespace;
    server->signals = config->signals;
    server->governor = config->governor;
    server->governor_user_data = config->governor_user_data;

    /* Resolve every signal-mapped DID through the codec namespace and fail
     * closed when a name does not resolve (SW-FR-UDS-008). */
    for (i = 0u; i < server->config->did_count; ++i) {
        const cancestry_uds_did_t *entry = &server->config->dids[i];

        server->did_signal_ids[i] = CANCESTRY_ID_NONE;
        if (entry->signal != NULL) {
            cancestry_codec_resolution_t resolution;

            if (server->namespace == NULL) {
                memset(server, 0, sizeof(*server));
                return false;
            }
            if (cancestry_codec_namespace_resolve(server->namespace, entry->signal,
                                                  &resolution) != CANCESTRY_CODEC_OK) {
                memset(server, 0, sizeof(*server));
                return false;
            }
            server->did_signal_ids[i] = resolution.signal_id;
        }
    }

    /* Seed the DID data store: zero first, then the configured defaults. */
    if (server->did_data != NULL && server->config->did_data_length > 0u) {
        memset(server->did_data, 0, server->config->did_data_length);
        for (i = 0u; i < server->config->did_count; ++i) {
            const cancestry_uds_did_t *entry = &server->config->dids[i];

            if (entry->default_data != NULL) {
                memcpy(&server->did_data[entry->data_offset], entry->default_data,
                       (size_t)entry->length);
            }
        }
    }

    server->initialized = true;
    return true;
}

bool cancestry_uds_server_is_valid(const cancestry_uds_server_t *server)
{
    return server != NULL && server->initialized;
}

const cancestry_uds_server_counters_t *cancestry_uds_server_counters(
    const cancestry_uds_server_t *server)
{
    if (server == NULL || !server->initialized) {
        return NULL;
    }
    return &server->counters;
}

/* ------------------------------------------------------------------------- */
/* Service 0x22: ReadDataByIdentifier (SW-FR-UDS-002)                        */
/* ------------------------------------------------------------------------- */

cancestry_uds_status_t cancestry_uds_read_data_by_identifier(cancestry_uds_server_t *server,
                                                             const uint8_t *request,
                                                             size_t request_length,
                                                             uint8_t *response,
                                                             size_t response_capacity,
                                                             size_t *response_length)
{
    const cancestry_uds_did_t *entry;
    uint16_t did;
    uint16_t index = 0u;
    const uint8_t *source;
    bool encoded = false;

    if (server == NULL || !server->initialized) {
        return CANCESTRY_UDS_ERR_NULL;
    }
    if (request == NULL || response == NULL || response_length == NULL) {
        return CANCESTRY_UDS_ERR_NULL;
    }
    /* Exactly one DID per request at this level (SW-FR-UDS-002). */
    if (request_length != 3u) {
        return uds_negative(server, CANCESTRY_UDS_SID_RDBI,
                            CANCESTRY_UDS_NRC_INCORRECT_MESSAGE_LENGTH,
                            CANCESTRY_UDS_ERR_LENGTH, response, response_capacity,
                            response_length);
    }
    did = (uint16_t)(((uint16_t)request[1] << 8) | (uint16_t)request[2]);
    entry = cancestry_uds_config_find_did(server->config, did, &index);
    if (entry == NULL) {
        return uds_negative(server, CANCESTRY_UDS_SID_RDBI,
                            CANCESTRY_UDS_NRC_REQUEST_OUT_OF_RANGE,
                            CANCESTRY_UDS_ERR_NOT_FOUND, response, response_capacity,
                            response_length);
    }
    if (response_capacity < (size_t)entry->length + 3u) {
        return CANCESTRY_UDS_ERR_CAPACITY;
    }

    /* Data source: a signal-mapped DID encodes the current shared signal
     * value when one exists, falling back to the stored bytes
     * (SW-FR-UDS-008). */
    source = &server->did_data[entry->data_offset];
    if (entry->signal != NULL) {
        if (entry->length > CANCESTRY_UDS_DID_SIGNAL_LENGTH_MAX) {
            /* Defensive: the loader refuses this shape; a hand-built config
             * gets a deterministic refusal instead of a guess. */
            return uds_negative(server, CANCESTRY_UDS_SID_RDBI,
                                CANCESTRY_UDS_NRC_CONDITIONS_NOT_CORRECT,
                                CANCESTRY_UDS_ERR_DENIED, response, response_capacity,
                                response_length);
        }
        if (server->signals != NULL) {
            cancestry_value_t value;

            if (cancestry_recipe_signal_store_get(server->signals,
                                                  server->did_signal_ids[index],
                                                  &value) == CANCESTRY_RECIPE_OK) {
                if (!uds_encode_value_le(&value, &response[3], entry->length)) {
                    return uds_negative(server, CANCESTRY_UDS_SID_RDBI,
                                        CANCESTRY_UDS_NRC_CONDITIONS_NOT_CORRECT,
                                        CANCESTRY_UDS_ERR_DENIED, response,
                                        response_capacity, response_length);
                }
                encoded = true;
            }
        }
    }

    response[0] = (uint8_t)(CANCESTRY_UDS_SID_RDBI + CANCESTRY_UDS_SID_POSITIVE_OFFSET);
    response[1] = (uint8_t)(did >> 8);
    response[2] = (uint8_t)(did & 0xFFu);
    if (!encoded) {
        memcpy(&response[3], source, (size_t)entry->length);
    }
    *response_length = (size_t)entry->length + 3u;

    server->counters.did_reads++;
    server->counters.positive_responses++;
    return CANCESTRY_UDS_OK;
}

/* ------------------------------------------------------------------------- */
/* Service 0x2E: WriteDataByIdentifier (SW-FR-UDS-003)                       */
/* ------------------------------------------------------------------------- */

cancestry_uds_status_t cancestry_uds_write_data_by_identifier(cancestry_uds_server_t *server,
                                                              const uint8_t *request,
                                                              size_t request_length,
                                                              uint8_t *response,
                                                              size_t response_capacity,
                                                              size_t *response_length)
{
    const cancestry_uds_did_t *entry;
    uint16_t did;
    uint16_t index = 0u;
    cancestry_uds_governor_request_t gov;
    bool mirrored = false;

    if (server == NULL || !server->initialized) {
        return CANCESTRY_UDS_ERR_NULL;
    }
    if (request == NULL || response == NULL || response_length == NULL) {
        return CANCESTRY_UDS_ERR_NULL;
    }
    if (request_length < 3u) {
        return uds_negative(server, CANCESTRY_UDS_SID_WDBI,
                            CANCESTRY_UDS_NRC_INCORRECT_MESSAGE_LENGTH,
                            CANCESTRY_UDS_ERR_LENGTH, response, response_capacity,
                            response_length);
    }
    did = (uint16_t)(((uint16_t)request[1] << 8) | (uint16_t)request[2]);
    entry = cancestry_uds_config_find_did(server->config, did, &index);
    if (entry == NULL) {
        return uds_negative(server, CANCESTRY_UDS_SID_WDBI,
                            CANCESTRY_UDS_NRC_REQUEST_OUT_OF_RANGE,
                            CANCESTRY_UDS_ERR_NOT_FOUND, response, response_capacity,
                            response_length);
    }
    if (request_length != (size_t)entry->length + 3u) {
        return uds_negative(server, CANCESTRY_UDS_SID_WDBI,
                            CANCESTRY_UDS_NRC_INCORRECT_MESSAGE_LENGTH,
                            CANCESTRY_UDS_ERR_LENGTH, response, response_capacity,
                            response_length);
    }
    if (response_capacity < 3u) {
        return CANCESTRY_UDS_ERR_CAPACITY;
    }

    /* Build the governor request: the entry (with its read_only flag), the
     * bytes to write and, when mapped, the resolved signal and the mirrored
     * value (SW-FR-UDS-007/008). */
    memset(&gov, 0, sizeof(gov));
    gov.kind = CANCESTRY_UDS_GOVERNOR_WRITE_DID;
    gov.did = did;
    gov.did_entry = entry;
    gov.data = &request[3];
    gov.data_length = (size_t)entry->length;
    gov.has_signal = uds_did_signal_index(server, index, &gov.signal_id);
    if (gov.has_signal) {
        gov.signal_name = entry->signal;
        if (entry->length > CANCESTRY_UDS_DID_SIGNAL_LENGTH_MAX) {
            /* Defensive: the loader refuses this shape. */
            return uds_negative(server, CANCESTRY_UDS_SID_WDBI,
                                CANCESTRY_UDS_NRC_CONDITIONS_NOT_CORRECT,
                                CANCESTRY_UDS_ERR_DENIED, response, response_capacity,
                                response_length);
        }
        gov.signal_value.kind = CANCESTRY_VALUE_KIND_UINT;
        gov.signal_value.value.unsigned_integer =
            uds_decode_le(&request[3], (size_t)entry->length);
    }

    if (!uds_governor_approves(server, &gov)) {
        /* Denied: no partial effect, violation counter incremented, NRC 0x22
         * (SW-FR-UDS-007, SW-FR-GOV-005/006). */
        return uds_negative(server, CANCESTRY_UDS_SID_WDBI,
                            CANCESTRY_UDS_NRC_CONDITIONS_NOT_CORRECT,
                            CANCESTRY_UDS_ERR_DENIED, response, response_capacity,
                            response_length);
    }

    /* Approved: mirror into the signal store first (atomic: on failure the
     * DID slot is not written either), then store the bytes. */
    if (gov.has_signal && server->signals != NULL) {
        cancestry_value_t value;

        value.kind = CANCESTRY_VALUE_KIND_UINT;
        value.value.unsigned_integer = gov.signal_value.value.unsigned_integer;
        if (cancestry_recipe_signal_store_set(server->signals, gov.signal_id, &value) !=
            CANCESTRY_RECIPE_OK) {
            return uds_negative(server, CANCESTRY_UDS_SID_WDBI,
                                CANCESTRY_UDS_NRC_GENERAL_PROGRAMMING_FAILURE,
                                CANCESTRY_UDS_ERR_PROGRAMMING, response,
                                response_capacity, response_length);
        }
        mirrored = true;
    }
    memcpy(&server->did_data[entry->data_offset], &request[3], (size_t)entry->length);

    response[0] = (uint8_t)(CANCESTRY_UDS_SID_WDBI + CANCESTRY_UDS_SID_POSITIVE_OFFSET);
    response[1] = (uint8_t)(did >> 8);
    response[2] = (uint8_t)(did & 0xFFu);
    *response_length = 3u;

    server->counters.did_writes++;
    server->counters.positive_responses++;
    (void)mirrored;
    return CANCESTRY_UDS_OK;
}

/* ------------------------------------------------------------------------- */
/* Service 0x31: RoutineControl (SW-FR-UDS-004)                              */
/* ------------------------------------------------------------------------- */

cancestry_uds_status_t cancestry_uds_routine_control(cancestry_uds_server_t *server,
                                                     const uint8_t *request,
                                                     size_t request_length,
                                                     uint8_t *response,
                                                     size_t response_capacity,
                                                     size_t *response_length)
{
    const cancestry_uds_routine_t *entry;
    const cancestry_uds_routine_op_t *op;
    uint8_t subfunction;
    uint16_t routine;
    cancestry_uds_governor_request_t gov;

    if (server == NULL || !server->initialized) {
        return CANCESTRY_UDS_ERR_NULL;
    }
    if (request == NULL || response == NULL || response_length == NULL) {
        return CANCESTRY_UDS_ERR_NULL;
    }
    /* Routines at this level take no request data: SID + sub-function + id. */
    if (request_length != 4u) {
        return uds_negative(server, CANCESTRY_UDS_SID_ROUTINE_CONTROL,
                            CANCESTRY_UDS_NRC_INCORRECT_MESSAGE_LENGTH,
                            CANCESTRY_UDS_ERR_LENGTH, response, response_capacity,
                            response_length);
    }
    subfunction = request[1];
    routine = (uint16_t)(((uint16_t)request[2] << 8) | (uint16_t)request[3]);
    if (subfunction != CANCESTRY_UDS_RC_START && subfunction != CANCESTRY_UDS_RC_STOP &&
        subfunction != CANCESTRY_UDS_RC_RESULTS) {
        return uds_negative(server, CANCESTRY_UDS_SID_ROUTINE_CONTROL,
                            CANCESTRY_UDS_NRC_SUB_FUNCTION_NOT_SUPPORTED,
                            CANCESTRY_UDS_ERR_UNSUPPORTED, response, response_capacity,
                            response_length);
    }
    entry = cancestry_uds_config_find_routine(server->config, routine, NULL);
    if (entry == NULL) {
        return uds_negative(server, CANCESTRY_UDS_SID_ROUTINE_CONTROL,
                            CANCESTRY_UDS_NRC_REQUEST_OUT_OF_RANGE,
                            CANCESTRY_UDS_ERR_NOT_FOUND, response, response_capacity,
                            response_length);
    }
    op = (subfunction == CANCESTRY_UDS_RC_START)
             ? &entry->start
             : ((subfunction == CANCESTRY_UDS_RC_STOP) ? &entry->stop : &entry->results);
    if (!op->declared || !op->allowed) {
        return uds_negative(server, CANCESTRY_UDS_SID_ROUTINE_CONTROL,
                            CANCESTRY_UDS_NRC_SUB_FUNCTION_NOT_SUPPORTED,
                            CANCESTRY_UDS_ERR_UNSUPPORTED, response, response_capacity,
                            response_length);
    }
    if (response_capacity < (size_t)op->response_length + 4u) {
        return CANCESTRY_UDS_ERR_CAPACITY;
    }

    memset(&gov, 0, sizeof(gov));
    gov.kind = CANCESTRY_UDS_GOVERNOR_RUN_ROUTINE;
    gov.routine_id = routine;
    gov.routine_entry = entry;
    gov.subfunction = subfunction;
    if (!uds_governor_approves(server, &gov)) {
        return uds_negative(server, CANCESTRY_UDS_SID_ROUTINE_CONTROL,
                            CANCESTRY_UDS_NRC_CONDITIONS_NOT_CORRECT,
                            CANCESTRY_UDS_ERR_DENIED, response, response_capacity,
                            response_length);
    }

    response[0] =
        (uint8_t)(CANCESTRY_UDS_SID_ROUTINE_CONTROL + CANCESTRY_UDS_SID_POSITIVE_OFFSET);
    response[1] = subfunction;
    response[2] = (uint8_t)(routine >> 8);
    response[3] = (uint8_t)(routine & 0xFFu);
    if (op->response_length > 0u && op->response != NULL) {
        memcpy(&response[4], op->response, (size_t)op->response_length);
    }
    *response_length = (size_t)op->response_length + 4u;

    server->counters.routine_controls++;
    server->counters.positive_responses++;
    return CANCESTRY_UDS_OK;
}

/* ------------------------------------------------------------------------- */
/* Dispatch (SW-FR-UDS-001, SW-FR-UDS-005)                                   */
/* ------------------------------------------------------------------------- */

cancestry_uds_status_t cancestry_uds_server_process(cancestry_uds_server_t *server,
                                                    const uint8_t *request,
                                                    size_t request_length,
                                                    uint8_t *response,
                                                    size_t response_capacity,
                                                    size_t *response_length)
{
    uint8_t sid;

    if (server == NULL || !server->initialized) {
        return CANCESTRY_UDS_ERR_NULL;
    }
    if (request == NULL || response == NULL || response_length == NULL) {
        return CANCESTRY_UDS_ERR_NULL;
    }
    server->counters.requests_processed++;

    if (request_length == 0u) {
        /* No service identifier at all: refuse with a placeholder SID
         * (documented; the wire cannot produce this through ISO-TP SF). */
        return uds_negative(server, 0x00u, CANCESTRY_UDS_NRC_SERVICE_NOT_SUPPORTED,
                            CANCESTRY_UDS_ERR_UNSUPPORTED, response, response_capacity,
                            response_length);
    }
    sid = request[0];
    switch (sid) {
    case CANCESTRY_UDS_SID_RDBI:
        return cancestry_uds_read_data_by_identifier(server, request, request_length,
                                                     response, response_capacity,
                                                     response_length);
    case CANCESTRY_UDS_SID_WDBI:
        return cancestry_uds_write_data_by_identifier(server, request, request_length,
                                                      response, response_capacity,
                                                      response_length);
    case CANCESTRY_UDS_SID_ROUTINE_CONTROL:
        return cancestry_uds_routine_control(server, request, request_length, response,
                                             response_capacity, response_length);
    default:
        return uds_negative(server, sid, CANCESTRY_UDS_NRC_SERVICE_NOT_SUPPORTED,
                            CANCESTRY_UDS_ERR_UNSUPPORTED, response, response_capacity,
                            response_length);
    }
}
