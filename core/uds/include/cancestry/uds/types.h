/*
 * CANcestry - UDS (ISO 14229-1) server: shared types.
 *
 * Normative references:
 *   ISO 14229-1                       unified diagnostic services, part 1:
 *                                     specification and requirements (subset:
 *                                     0x22, 0x2E, 0x31 at this level)
 *   schemas/uds-0.1.0.schema.json     UDS configuration v0.1.0 schema
 *   docs/software/SwRS.md             SW-FR-UDS-001 .. SW-FR-UDS-008 (Phase 10)
 *   docs/system/SyRS.md               SYS-NF-001 (determinism), SYS-NF-002 (bounded resources)
 *
 * Design notes:
 *   - The server is transport-agnostic: one request buffer in, one response
 *     buffer out (SW-FR-UDS-001). The platform wires the ISO-TP engine's
 *     completed-message callback to the request entry point and segments the
 *     response through the ISO-TP send path.
 *   - UDS sessions, security access, functional addressing, and services other
 *     than ReadDataByIdentifier / WriteDataByIdentifier / RoutineControl are
 *     deliberately absent: they belong to later phases.
 *   - The DID and routine tables are loaded from a schema-validated YAML
 *     document (SW-FR-UDS-006); the loaded config owns one allocation and the
 *     runtime only borrows it, mirroring the codec/recipe/FSM loaders.
 *   - DID data lives in a caller-owned byte store ("DID data store") whose
 *     layout the loader computes (one fixed-length slot per DID); the runtime
 *     never allocates (SYS-NF-002).
 */

#ifndef CANCESTRY_UDS_TYPES_H
#define CANCESTRY_UDS_TYPES_H

#include "cancestry/event/types.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------------- */
/* Bounds (bounded resources: SYS-NF-002)                                    */
/* ------------------------------------------------------------------------- */

/** Maximum number of DID entries a configuration may declare (schema bound). */
#define CANCESTRY_UDS_MAX_DIDS ((uint16_t)64u)

/** Maximum number of routine entries a configuration may declare (schema bound). */
#define CANCESTRY_UDS_MAX_ROUTINES ((uint16_t)64u)

/** Largest DID length in bytes (the ISO-TP 12-bit FF length limit). */
#define CANCESTRY_UDS_DID_LENGTH_MAX ((uint16_t)4095u)

/** Largest DID length that may be mirrored into a 64-bit signal value. */
#define CANCESTRY_UDS_DID_SIGNAL_LENGTH_MAX ((uint16_t)8u)

/** Maximum length of a DID/routine name-related error text (loader). */
#define CANCESTRY_UDS_NAME_MAX ((size_t)129u)

/* ------------------------------------------------------------------------- */
/* Service and sub-function identifiers (ISO 14229-1, Table 425 subset)      */
/* ------------------------------------------------------------------------- */

#define CANCESTRY_UDS_SID_RDBI ((uint8_t)0x22u)
#define CANCESTRY_UDS_SID_WDBI ((uint8_t)0x2Eu)
#define CANCESTRY_UDS_SID_ROUTINE_CONTROL ((uint8_t)0x31u)
/** Positive response SID = request SID + 0x40. */
#define CANCESTRY_UDS_SID_POSITIVE_OFFSET ((uint8_t)0x40u)
/** Negative response SID. */
#define CANCESTRY_UDS_SID_NEGATIVE ((uint8_t)0x7Fu)

/** RoutineControl sub-functions. */
#define CANCESTRY_UDS_RC_START ((uint8_t)0x01u)
#define CANCESTRY_UDS_RC_STOP ((uint8_t)0x02u)
#define CANCESTRY_UDS_RC_RESULTS ((uint8_t)0x03u)

/* ------------------------------------------------------------------------- */
/* Negative response codes (ISO 14229-1, Table 52 subset)                    */
/* ------------------------------------------------------------------------- */

/** The server does not support the requested service. */
#define CANCESTRY_UDS_NRC_SERVICE_NOT_SUPPORTED ((uint8_t)0x11u)
/** The server does not support the requested sub-function. */
#define CANCESTRY_UDS_NRC_SUB_FUNCTION_NOT_SUPPORTED ((uint8_t)0x12u)
/** The request length or format is incorrect. */
#define CANCESTRY_UDS_NRC_INCORRECT_MESSAGE_LENGTH ((uint8_t)0x13u)
/** Conditions for the operation are not met (governor denial, unencodable value). */
#define CANCESTRY_UDS_NRC_CONDITIONS_NOT_CORRECT ((uint8_t)0x22u)
/** The DID or routine identifier is not known to the server. */
#define CANCESTRY_UDS_NRC_REQUEST_OUT_OF_RANGE ((uint8_t)0x31u)
/** Writing the mapped signal failed in the shared signal store. */
#define CANCESTRY_UDS_NRC_GENERAL_PROGRAMMING_FAILURE ((uint8_t)0x72u)

/* ------------------------------------------------------------------------- */
/* Status                                                                    */
/* ------------------------------------------------------------------------- */

/**
 * Operation results. Values >= 0 mean the operation succeeded.
 *
 * For request processing, the negative status values mirror the negative
 * response code the server emitted, so the caller can log a reason without
 * re-parsing the response (SW-FR-UDS-002..005).
 */
typedef enum cancestry_uds_status {
    /** Success (the response is a positive response). */
    CANCESTRY_UDS_OK = 0,
    /** A required pointer argument was NULL or the server is not initialized. */
    CANCESTRY_UDS_ERR_NULL = -1,
    /** Malformed argument (zero-length request, capacity too small). */
    CANCESTRY_UDS_ERR_ARGUMENT = -2,
    /** The caller's response buffer is too small for the response. */
    CANCESTRY_UDS_ERR_CAPACITY = -3,
    /** Unknown DID or routine (negative response 0x31). */
    CANCESTRY_UDS_ERR_NOT_FOUND = -4,
    /** Malformed request (negative response 0x13). */
    CANCESTRY_UDS_ERR_LENGTH = -5,
    /** Governor denial or unencodable value (negative response 0x22). */
    CANCESTRY_UDS_ERR_DENIED = -6,
    /** Unsupported service or sub-function (negative response 0x11/0x12). */
    CANCESTRY_UDS_ERR_UNSUPPORTED = -7,
    /** The mapped signal store rejected the mirrored value (0x72). */
    CANCESTRY_UDS_ERR_PROGRAMMING = -8,
    /* Loader statuses. */
    /** Loader: YAML syntax or schema violation (SW-FR-UDS-006). */
    CANCESTRY_UDS_ERR_PARSE = -9,
    /** Loader: duplicate DID or routine identifier. */
    CANCESTRY_UDS_ERR_CONFLICT = -10,
    /** Loader: out of memory. */
    CANCESTRY_UDS_ERR_NO_MEMORY = -11
} cancestry_uds_status_t;

/* ------------------------------------------------------------------------- */
/* Configuration (loader output; borrowed by the runtime)                    */
/* ------------------------------------------------------------------------- */

/** One configured data identifier. */
typedef struct cancestry_uds_did {
    /** The two-byte data identifier. */
    uint16_t did;
    /** Fixed data length in bytes. */
    uint16_t length;
    /**
     * Advisory policy flag carried into every governor WRITE_DID request.
     * The server itself never bypasses the governor; the platform's governor
     * policy decides (SW-FR-UDS-007).
     */
    bool read_only;
    /** Optional codec signal name (short or canonical), or NULL. */
    const char *signal;
    /** Default bytes seeded into the DID data store at init, or NULL. */
    const uint8_t *default_data;
    /** Offset of this DID's slot inside the DID data store (loader-computed). */
    size_t data_offset;
} cancestry_uds_did_t;

/** One configured sub-function of a routine. */
typedef struct cancestry_uds_routine_op {
    /** true when the configuration declared this sub-function. */
    bool declared;
    /** Whether the sub-function is accepted (NRC 0x12 when false). */
    bool allowed;
    /** Static response record appended to the positive response; may be NULL. */
    const uint8_t *response;
    uint16_t response_length;
} cancestry_uds_routine_op_t;

/** One configured routine. */
typedef struct cancestry_uds_routine {
    /** The two-byte routine identifier. */
    uint16_t routine;
    /** Sub-function 0x01. */
    cancestry_uds_routine_op_t start;
    /** Sub-function 0x02. */
    cancestry_uds_routine_op_t stop;
    /** Sub-function 0x03. */
    cancestry_uds_routine_op_t results;
} cancestry_uds_routine_t;

/** A loaded UDS configuration. Owned by the loader allocation. */
typedef struct cancestry_uds_config {
    /** DID table, in declaration order. May be NULL when did_count is 0. */
    cancestry_uds_did_t *dids;
    uint16_t did_count;
    /** Routine table, in declaration order. May be NULL when routine_count is 0. */
    cancestry_uds_routine_t *routines;
    uint16_t routine_count;
    /** Total bytes of the DID data store (sum of all DID lengths). */
    size_t did_data_length;
} cancestry_uds_config_t;

/* ------------------------------------------------------------------------- */
/* Helpers                                                                   */
/* ------------------------------------------------------------------------- */

/** @return true when @p status indicates success (>= 0). */
bool cancestry_uds_status_is_ok(cancestry_uds_status_t status);

/** @return Stable, statically allocated name for @p status. Never NULL. */
const char *cancestry_uds_status_name(cancestry_uds_status_t status);

/**
 * @return Stable, statically allocated name for a negative response code,
 *         such as "REQUEST_OUT_OF_RANGE", or "INVALID". Never NULL.
 */
const char *cancestry_uds_nrc_name(uint8_t nrc);

#ifdef __cplusplus
}
#endif

#endif /* CANCESTRY_UDS_TYPES_H */
