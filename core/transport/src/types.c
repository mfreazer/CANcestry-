/*
 * CANcestry - ISO-TP transport: type helpers and name tables.
 *
 * Normative references:
 *   docs/software/SwRS.md   SW-FR-TP-002, SW-FR-TP-008, SW-FR-TP-010 (Phase 10)
 *   docs/system/SyRS.md     SYS-NF-001 (determinism)
 *
 * Every name table is static storage with no dynamic initialization, so the
 * module stays allocation-free and the names are stable for the trace and
 * fault-manager consumers (SW-FR-TP-010).
 */

#include "cancestry/transport/types.h"

#include <string.h>

/* ------------------------------------------------------------------------- */
/* Status                                                                    */
/* ------------------------------------------------------------------------- */

bool cancestry_tp_status_is_ok(cancestry_tp_status_t status)
{
    return status >= CANCESTRY_TP_OK;
}

const char *cancestry_tp_status_name(cancestry_tp_status_t status)
{
    switch (status) {
    case CANCESTRY_TP_OK:
        return "OK";
    case CANCESTRY_TP_OK_MESSAGE:
        return "OK_MESSAGE";
    case CANCESTRY_TP_ERR_NULL:
        return "ERR_NULL";
    case CANCESTRY_TP_ERR_ARGUMENT:
        return "ERR_ARGUMENT";
    case CANCESTRY_TP_ERR_STATE:
        return "ERR_STATE";
    case CANCESTRY_TP_ERR_CAPACITY:
        return "ERR_CAPACITY";
    case CANCESTRY_TP_ERR_PROTOCOL:
        return "ERR_PROTOCOL";
    case CANCESTRY_TP_ERR_OVERFLOW:
        return "ERR_OVERFLOW";
    case CANCESTRY_TP_ERR_TIMEOUT:
        return "ERR_TIMEOUT";
    default:
        return "INVALID";
    }
}

/* ------------------------------------------------------------------------- */
/* State                                                                     */
/* ------------------------------------------------------------------------- */

const char *cancestry_tp_state_name(cancestry_tp_state_t state)
{
    switch (state) {
    case CANCESTRY_TP_STATE_IDLE:
        return "IDLE";
    case CANCESTRY_TP_STATE_FIRST_FRAME:
        return "FIRST_FRAME";
    case CANCESTRY_TP_STATE_CONSECUTIVE_FRAME:
        return "CONSECUTIVE_FRAME";
    case CANCESTRY_TP_STATE_FLOW_CONTROL:
        return "FLOW_CONTROL";
    default:
        return "INVALID";
    }
}

/* ------------------------------------------------------------------------- */
/* Faults                                                                    */
/* ------------------------------------------------------------------------- */

const char *cancestry_tp_fault_name(cancestry_tp_fault_code_t fault)
{
    switch (fault) {
    case CANCESTRY_TP_FAULT_NONE:
        return "TRANSPORT_NONE";
    case CANCESTRY_TP_FAULT_PROTOCOL:
        return "TRANSPORT_PROTOCOL_FAULT";
    case CANCESTRY_TP_FAULT_BUFFER_OVERFLOW:
        return "TRANSPORT_BUFFER_OVERFLOW";
    case CANCESTRY_TP_FAULT_TIMEOUT:
        return "TRANSPORT_TIMEOUT";
    default:
        return "INVALID";
    }
}

cancestry_fault_severity_t cancestry_tp_fault_severity(cancestry_tp_fault_code_t fault)
{
    /* All transport faults abort exactly one diagnostic session and leave the
     * gateway function untouched, so WARNING is the normative severity
     * (docs/system/mode-fault-state-machine.md section 5 "Fault Reaction
     * Table": session-level conditions drop, they do not suspend the node).
     * The fault manager may escalate on repetition. */
    switch (fault) {
    case CANCESTRY_TP_FAULT_PROTOCOL:
    case CANCESTRY_TP_FAULT_BUFFER_OVERFLOW:
    case CANCESTRY_TP_FAULT_TIMEOUT:
        return CANCESTRY_FAULT_SEVERITY_WARNING;
    case CANCESTRY_TP_FAULT_NONE:
    case CANCESTRY_TP_FAULT_COUNT:
    default:
        return CANCESTRY_FAULT_SEVERITY_INFO;
    }
}

/* ------------------------------------------------------------------------- */
/* STmin codec                                                               */
/* ------------------------------------------------------------------------- */

uint32_t cancestry_tp_stmin_decode_us(uint8_t stmin)
{
    if (stmin <= 0x7Fu) {
        /* 0x00..0x7F: milliseconds. */
        return (uint32_t)stmin * 1000u;
    }
    if (stmin >= 0xF1u && stmin <= 0xF9u) {
        /* 0xF1..0xF9: 100..900 microseconds. */
        return (uint32_t)(stmin - 0xF0u) * 100u;
    }
    /* Reserved encodings decode to 0 (SW-FR-TP-008): STmin only ever slows
     * the sender down, and 0 is a legal STmin, so the resulting behaviour
     * stays inside the protocol envelope. Counted nowhere: deterministic. */
    return 0u;
}

uint8_t cancestry_tp_stmin_encode_ms(uint32_t ms)
{
    if (ms > 127u) {
        return 0x7Fu;
    }
    return (uint8_t)ms;
}
