/*
 * CANcestry - UDS server: type helpers and name tables.
 *
 * Normative references:
 *   ISO 14229-1             negative response codes, Table 52 subset
 *   docs/software/SwRS.md   SW-FR-UDS-002 .. SW-FR-UDS-005 (Phase 10)
 *   docs/system/SyRS.md     SYS-NF-001 (determinism)
 *
 * Static storage only: no dynamic initialization, no allocation (SYS-NF-002).
 */

#include "cancestry/uds/types.h"

bool cancestry_uds_status_is_ok(cancestry_uds_status_t status)
{
    return status >= CANCESTRY_UDS_OK;
}

const char *cancestry_uds_status_name(cancestry_uds_status_t status)
{
    switch (status) {
    case CANCESTRY_UDS_OK:
        return "OK";
    case CANCESTRY_UDS_ERR_NULL:
        return "ERR_NULL";
    case CANCESTRY_UDS_ERR_ARGUMENT:
        return "ERR_ARGUMENT";
    case CANCESTRY_UDS_ERR_CAPACITY:
        return "ERR_CAPACITY";
    case CANCESTRY_UDS_ERR_NOT_FOUND:
        return "ERR_NOT_FOUND";
    case CANCESTRY_UDS_ERR_LENGTH:
        return "ERR_LENGTH";
    case CANCESTRY_UDS_ERR_DENIED:
        return "ERR_DENIED";
    case CANCESTRY_UDS_ERR_UNSUPPORTED:
        return "ERR_UNSUPPORTED";
    case CANCESTRY_UDS_ERR_PROGRAMMING:
        return "ERR_PROGRAMMING";
    case CANCESTRY_UDS_ERR_PARSE:
        return "ERR_PARSE";
    case CANCESTRY_UDS_ERR_CONFLICT:
        return "ERR_CONFLICT";
    case CANCESTRY_UDS_ERR_NO_MEMORY:
        return "ERR_NO_MEMORY";
    default:
        return "INVALID";
    }
}

const char *cancestry_uds_nrc_name(uint8_t nrc)
{
    switch (nrc) {
    case CANCESTRY_UDS_NRC_SERVICE_NOT_SUPPORTED:
        return "SERVICE_NOT_SUPPORTED";
    case CANCESTRY_UDS_NRC_SUB_FUNCTION_NOT_SUPPORTED:
        return "SUB_FUNCTION_NOT_SUPPORTED";
    case CANCESTRY_UDS_NRC_INCORRECT_MESSAGE_LENGTH:
        return "INCORRECT_MESSAGE_LENGTH_OR_INVALID_FORMAT";
    case CANCESTRY_UDS_NRC_CONDITIONS_NOT_CORRECT:
        return "CONDITIONS_NOT_CORRECT";
    case CANCESTRY_UDS_NRC_REQUEST_OUT_OF_RANGE:
        return "REQUEST_OUT_OF_RANGE";
    case CANCESTRY_UDS_NRC_GENERAL_PROGRAMMING_FAILURE:
        return "GENERAL_PROGRAMMING_FAILURE";
    default:
        return "INVALID";
    }
}
