/*
 * CANcestry - UDS server: request processing and the governor checkpoint.
 *
 * Normative references:
 *   ISO 14229-1                       services 0x22, 0x2E, 0x31 (subset)
 *   docs/software/SwRS.md             SW-FR-UDS-001 .. SW-FR-UDS-008 (Phase 10)
 *   docs/system/governor.md           (v0.2.1 stub level) fail-closed approval
 *                                     flow; the UDS governor mirrors the
 *                                     recipe/FSM stubs (SW-FR-GOV-005/006)
 *   docs/system/SyRS.md               SYS-NF-001 (determinism), SYS-NF-002 (bounded resources)
 *
 * Design notes:
 *   - Pure request/response: cancestry_uds_server_process() copies the
 *     request, produces the response into a caller buffer and returns a
 *     status that mirrors the negative response code. It never blocks,
 *     allocates or touches hardware (SW-FR-UDS-001).
 *   - Governor (SW-FR-UDS-007): every WriteDataByIdentifier and
 *     RoutineControl side effect passes through the configured governor
 *     function synchronously before any effect happens. A NULL governor is
 *     the fail-closed default and denies everything (SW-FR-GOV-006); a denial
 *     produces no partial effect, increments the violation counter
 *     (SW-FR-GOV-005) and is answered with NRC 0x22.
 *   - Signal mapping (SW-FR-UDS-008): a DID may map to a shared signal. Reads
 *     encode the current value little-endian; approved writes mirror the
 *     written bytes into the shared signal value store
 *     (cancestry_recipe_signal_store_t, the same store the recipe engine and
 *     the decode pipeline use) as an unsigned integer.
 *   - The server does not consult the governor for ReadDataByIdentifier:
 *     reading is not an observable side effect (governor.md section 4).
 */

#ifndef CANCESTRY_UDS_SERVICES_H
#define CANCESTRY_UDS_SERVICES_H

#include "cancestry/codec/namespace.h"
#include "cancestry/recipe/engine.h"
#include "cancestry/uds/types.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------------- */
/* Governor stub (SW-FR-UDS-007, docs/system/governor.md)                    */
/* ------------------------------------------------------------------------- */

/** Governor decision. Anything but APPROVE is a denial; there is no "maybe". */
typedef enum cancestry_uds_governor_decision {
    CANCESTRY_UDS_GOVERNOR_APPROVE = 0,
    CANCESTRY_UDS_GOVERNOR_DENY = 1
} cancestry_uds_governor_decision_t;

/** Kind of side effect requesting approval. */
typedef enum cancestry_uds_governor_request_kind {
    /** WriteDataByIdentifier (0x2E). */
    CANCESTRY_UDS_GOVERNOR_WRITE_DID = 0,
    /** RoutineControl (0x31), any sub-function. */
    CANCESTRY_UDS_GOVERNOR_RUN_ROUTINE = 1
} cancestry_uds_governor_request_kind_t;

/**
 * One side-effect approval request, built on the stack and passed
 * synchronously. All pointers are valid only for the duration of the
 * callback; a governor that needs the data afterwards must copy it.
 *
 * Fields are meaningful per @c kind: WRITE_DID fills did/did_entry/data/
 * data_length and, when the DID maps to a signal, has_signal/signal_name/
 * signal_id/signal_value; RUN_ROUTINE fills routine_id/routine_entry/
 * subfunction. The did_entry's read_only flag is the static policy input the
 * reference governor uses to deny writes to read-only signals.
 */
typedef struct cancestry_uds_governor_request {
    cancestry_uds_governor_request_kind_t kind;
    /* WriteDataByIdentifier */
    uint16_t did;
    const cancestry_uds_did_t *did_entry;
    /** The bytes the client wants to write; @c data_length long. */
    const uint8_t *data;
    size_t data_length;
    /* Signal mapping of the DID, when declared. */
    bool has_signal;
    const char *signal_name;
    cancestry_signal_id_t signal_id;
    /** The little-endian value the approved write mirrors into the signal. */
    cancestry_value_t signal_value;
    /* RoutineControl */
    uint16_t routine_id;
    const cancestry_uds_routine_t *routine_entry;
    /** 0x01 start, 0x02 stop, 0x03 request-results. */
    uint8_t subfunction;
} cancestry_uds_governor_request_t;

/**
 * Governor stub: a function pointer the platform (or the test suite)
 * provides. Returning DENY blocks the side effect, increments the server's
 * violation counter and produces NRC 0x22 (SW-FR-UDS-007). NULL denies
 * everything (fail closed, SW-FR-GOV-006).
 */
typedef cancestry_uds_governor_decision_t (*cancestry_uds_governor_fn)(
    void *user_data, const cancestry_uds_governor_request_t *request);

/* ------------------------------------------------------------------------- */
/* Counters                                                                  */
/* ------------------------------------------------------------------------- */

/** Monotonic server counters; never reset by the server. */
typedef struct cancestry_uds_server_counters {
    /** Requests processed (any response). */
    uint32_t requests_processed;
    /** Positive responses emitted. */
    uint32_t positive_responses;
    /** Negative responses emitted. */
    uint32_t negative_responses;
    /* Service mix. */
    uint32_t did_reads;
    uint32_t did_writes;
    uint32_t routine_controls;
    /* Negative response code mix. */
    uint32_t nrc_service_not_supported;
    uint32_t nrc_sub_function_not_supported;
    uint32_t nrc_incorrect_message_length;
    uint32_t nrc_conditions_not_correct;
    uint32_t nrc_request_out_of_range;
    uint32_t nrc_general_programming_failure;
    /** Governor denials; the violation counter (SW-FR-GOV-005). */
    uint32_t governor_denials;
} cancestry_uds_server_counters_t;

/* ------------------------------------------------------------------------- */
/* Server                                                                    */
/* ------------------------------------------------------------------------- */

/** Borrowed server configuration, copied into the server at init time. */
typedef struct cancestry_uds_server_config {
    /** Loaded DID/routine tables; required. */
    const cancestry_uds_config_t *config;
    /**
     * Caller-owned DID data store, config->did_data_length bytes. Seeded from
     * the DID defaults during init. Required when the config declares DIDs.
     */
    uint8_t *did_data;
    /**
     * Caller-owned DID signal id array, config->did_count slots. Filled by
     * init: every signal-mapped DID is resolved through @p namespace and a
     * resolution failure fails init (fail-closed). Required when the config
     * declares DIDs.
     */
    cancestry_signal_id_t *did_signal_ids;
    /**
     * Codec namespace for DID signal resolution; required when any DID maps
     * a signal, optional otherwise.
     */
    const cancestry_codec_namespace_t *namespace;
    /**
     * Shared signal value store for the DID/signal mirror; optional. Without
     * it, signal-mapped DIDs serve their stored bytes and writes do not
     * mirror (the governor still runs).
     */
    cancestry_recipe_signal_store_t *signals;
    /** Governor stub; NULL denies every write and routine (fail-closed). */
    cancestry_uds_governor_fn governor;
    void *governor_user_data;
} cancestry_uds_server_config_t;

/** UDS server over borrowed configuration. Never allocates. */
typedef struct cancestry_uds_server {
    const cancestry_uds_config_t *config;
    uint8_t *did_data;
    cancestry_signal_id_t *did_signal_ids;
    const cancestry_codec_namespace_t *namespace;
    cancestry_recipe_signal_store_t *signals;
    cancestry_uds_governor_fn governor;
    void *governor_user_data;
    cancestry_uds_server_counters_t counters;
    bool initialized;
} cancestry_uds_server_t;

/**
 * Initialize a server over borrowed configuration.
 *
 * Resolves every signal-mapped DID through @p config->namespace (failing
 * closed with false when a name does not resolve) and seeds the DID data
 * store from the configured defaults.
 *
 * @return true when the server is ready for use. On failure the server is
 *         left zeroed (safely unusable).
 */
bool cancestry_uds_server_init(cancestry_uds_server_t *server,
                               const cancestry_uds_server_config_t *config);

/** @return true when @p server is non-NULL and initialized. */
bool cancestry_uds_server_is_valid(const cancestry_uds_server_t *server);

/** @return Pointer to the live counters, or NULL when @p server is invalid. */
const cancestry_uds_server_counters_t *cancestry_uds_server_counters(
    const cancestry_uds_server_t *server);

/**
 * Process one reassembled UDS request and build the response
 * (SW-FR-UDS-001..005).
 *
 * @param server           Server.
 * @param request          Request bytes (service identifier first).
 * @param request_length   Request length in bytes, >= 1.
 * @param response         Caller-owned response buffer.
 * @param response_capacity Size of @p response in bytes.
 * @param response_length  Receives the response length; never written when
 *                         the status is negative.
 * @return CANCESTRY_UDS_OK for a positive response, a negative
 *         ::cancestry_uds_status_t mirroring the negative response code
 *         (the response buffer then holds 7F SID NRC), or
 *         CANCESTRY_UDS_ERR_CAPACITY when @p response_capacity is too small
 *         for the response (nothing written; the request is counted but no
 *         response is emitted).
 */
cancestry_uds_status_t cancestry_uds_server_process(cancestry_uds_server_t *server,
                                                    const uint8_t *request,
                                                    size_t request_length,
                                                    uint8_t *response,
                                                    size_t response_capacity,
                                                    size_t *response_length);

/* ------------------------------------------------------------------------- */
/* Service handlers (exposed for conformance testing; SW-FR-UDS-002..004)    */
/* ------------------------------------------------------------------------- */

/** ReadDataByIdentifier (0x22): 0x62 DID DID data... (SW-FR-UDS-002). */
cancestry_uds_status_t cancestry_uds_read_data_by_identifier(cancestry_uds_server_t *server,
                                                             const uint8_t *request,
                                                             size_t request_length,
                                                             uint8_t *response,
                                                             size_t response_capacity,
                                                             size_t *response_length);

/** WriteDataByIdentifier (0x2E): 0x6E DID DID (SW-FR-UDS-003). */
cancestry_uds_status_t cancestry_uds_write_data_by_identifier(cancestry_uds_server_t *server,
                                                              const uint8_t *request,
                                                              size_t request_length,
                                                              uint8_t *response,
                                                              size_t response_capacity,
                                                              size_t *response_length);

/** RoutineControl (0x31): 0x31 SF RID RID record... (SW-FR-UDS-004). */
cancestry_uds_status_t cancestry_uds_routine_control(cancestry_uds_server_t *server,
                                                     const uint8_t *request,
                                                     size_t request_length,
                                                     uint8_t *response,
                                                     size_t response_capacity,
                                                     size_t *response_length);

/**
 * Look up a DID in the configuration.
 *
 * @param index_out Optional; receives the table index of the entry.
 * @return The matching entry, or NULL when the DID is not configured.
 */
const cancestry_uds_did_t *cancestry_uds_config_find_did(const cancestry_uds_config_t *config,
                                                         uint16_t did,
                                                         uint16_t *index_out);

/**
 * Look up a routine in the configuration.
 *
 * @param index_out Optional; receives the table index of the entry.
 * @return The matching entry, or NULL when the routine is not configured.
 */
const cancestry_uds_routine_t *cancestry_uds_config_find_routine(
    const cancestry_uds_config_t *config,
    uint16_t routine,
    uint16_t *index_out);

#ifdef __cplusplus
}
#endif

#endif /* CANCESTRY_UDS_SERVICES_H */
