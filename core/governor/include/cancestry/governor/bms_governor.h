/*
 * CANcestry - stateless BMS thermal and torque governor.
 *
 * Normative references:
 *   docs/software/SwRS.md section 16
 *   docs/system/governor.md section 5
 *
 * Implements: SW-FR-BMS-001..006.
 *
 * This interface deliberately has no mutable context.  A caller supplies a
 * complete snapshot of the VCU request and the BMS limits and receives a
 * complete decision.  The result's fault_code is the event a caller must
 * record; the governor itself does not write a log or retain history.
 */

#ifndef CANCESTRY_GOVERNOR_BMS_GOVERNOR_H
#define CANCESTRY_GOVERNOR_BMS_GOVERNOR_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Fixed-point efficiency scale. 950 means 95.0 percent. */
#define CANCESTRY_BMS_EFFICIENCY_SCALE ((uint16_t)1000u)

/** Fault code emitted when a request is reduced or blocked by a BMS limit. */
#define CANCESTRY_BMS_GOVERNOR_FAULT_GOVERNOR_INTERVENTION ((uint32_t)0x42534D01u)

/** Fault code emitted when a BMS snapshot cannot be used safely. */
#define CANCESTRY_BMS_GOVERNOR_FAULT_INVALID_INPUT ((uint32_t)0x42534D02u)

/** Fault code emitted when the BMS reports a state in which output is unsafe. */
#define CANCESTRY_BMS_GOVERNOR_FAULT_BMS_UNAVAILABLE ((uint32_t)0x42534D03u)

/** BMS operating state included in every evaluation snapshot. */
typedef enum cancestry_bms_state {
    /** BMS is healthy; the configured current limit still applies. */
    CANCESTRY_BMS_STATE_NORMAL = 0,
    /** BMS is thermally derated; MaxDischargeCurrent is the hard ceiling. */
    CANCESTRY_BMS_STATE_THERMAL_DERATING = 1,
    /** BMS data is not trustworthy; no torque/power is permitted. */
    CANCESTRY_BMS_STATE_FAULT = 2
} cancestry_bms_state_t;

/** Governor decision for an otherwise valid request. */
typedef enum cancestry_bms_governor_decision {
    /** Request is within the BMS current budget. */
    CANCESTRY_BMS_GOVERNOR_ALLOW = 0,
    /** Request is accepted only at the returned derated value. */
    CANCESTRY_BMS_GOVERNOR_DERATE = 1,
    /** No power or torque may be transmitted. */
    CANCESTRY_BMS_GOVERNOR_BLOCK = 2
} cancestry_bms_governor_decision_t;

/** Return status of the pure evaluation operation. */
typedef enum cancestry_bms_governor_status {
    CANCESTRY_BMS_GOVERNOR_OK = 0,
    CANCESTRY_BMS_GOVERNOR_ERR_NULL = -1,
    CANCESTRY_BMS_GOVERNOR_ERR_INVALID_INPUT = -2,
    CANCESTRY_BMS_GOVERNOR_ERR_BMS_FAULT = -3
} cancestry_bms_governor_status_t;

/**
 * Immutable input snapshot from the VCU and BMS.
 *
 * Power is the canonical electrical equivalent of the VCU torque request:
 * the VCU supplies it in watts so that this safety boundary does not need a
 * motor-specific torque constant or a floating-point conversion.  The
 * optional torque fields are scaled in milli-newton-metres and are reduced
 * in the same proportion for a human-readable actuator limit.
 *
 * All current and voltage values use milli-units.  For example, 400 V is
 * 400000 mV and 250 A is 250000 mA.  This makes the calculation exact and
 * avoids floating-point behavior in the safety decision.
 */
typedef struct cancestry_bms_governor_request {
    /** VCU electrical power demand, in watts. */
    uint32_t requested_power_w;
    /** Optional VCU torque demand, in milli-newton-metres. */
    uint32_t requested_torque_mnm;
    /** DC bus voltage reported by the BMS, in millivolts. */
    uint32_t bus_voltage_mv;
    /** BMS MaxDischargeCurrent, in milliamperes. */
    uint32_t max_discharge_current_ma;
    /** Drivetrain efficiency, in per-mille, from 1 through 1000. */
    uint16_t efficiency_permille;
    /** BMS validity/thermal state. */
    cancestry_bms_state_t bms_state;
} cancestry_bms_governor_request_t;

/**
 * Complete immutable decision output.
 *
 * A caller must use allowed_power_w/allowed_torque_mnm for the physical CAN
 * response and must record fault_code when intervention is true.  A DERATE
 * result is not permission to send requested_power_w.
 */
typedef struct cancestry_bms_governor_result {
    cancestry_bms_governor_decision_t decision;
    /** Requested current, rounded up so the limit cannot be exceeded. */
    uint32_t requested_current_ma;
    /** Current budget copied from the BMS snapshot. */
    uint32_t allowed_current_ma;
    /** Maximum electrical power representable by the current budget. */
    uint32_t maximum_power_w;
    /** Power value that may be passed to a UDS/CAN response. */
    uint32_t allowed_power_w;
    /** Proportionally derated optional torque value. */
    uint32_t allowed_torque_mnm;
    /** True when a request was reduced or blocked. */
    bool intervention;
    /** Zero when no fault needs recording. */
    uint32_t fault_code;
} cancestry_bms_governor_result_t;

/**
 * Evaluate one request without changing state or performing I/O.
 *
 * The function is total for every non-NULL input: invalid or unsafe BMS data
 * produces a BLOCK result and a non-zero fault code.  The return status tells
 * the caller whether the snapshot itself was usable; it must not be used to
 * bypass the BLOCK decision.
 */
cancestry_bms_governor_status_t cancestry_bms_governor_evaluate(
    const cancestry_bms_governor_request_t *request,
    cancestry_bms_governor_result_t *result);

/** @return true only when the result permits a response at allowed_power_w. */
bool cancestry_bms_governor_may_transmit(
    const cancestry_bms_governor_result_t *result);

/**
 * Apply the governor as the UDS/CAN response gate.
 *
 * On success, @p safe_power_w and @p safe_torque_mnm contain the only values
 * the integration may emit. A DERATE result returns true and writes the
 * reduced values; a BMS fault, invalid snapshot, NULL output or BLOCK result
 * writes zero and returns false. @p fault_code always receives the explicit
 * diagnostic payload, so a caller can raise `GOVERNOR_INTERVENTION` before
 * sending a derated response without making this function stateful.
 */
bool cancestry_bms_governor_gate_response(
    const cancestry_bms_governor_request_t *request,
    uint32_t *safe_power_w,
    uint32_t *safe_torque_mnm,
    uint32_t *fault_code);

/** @return Stable name for a governor fault code; never NULL. */
const char *cancestry_bms_governor_fault_name(uint32_t fault_code);

/** @return Stable name for a decision; never NULL. */
const char *cancestry_bms_governor_decision_name(
    cancestry_bms_governor_decision_t decision);

#ifdef __cplusplus
}
#endif

#endif /* CANCESTRY_GOVERNOR_BMS_GOVERNOR_H */
