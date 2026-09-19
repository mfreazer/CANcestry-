/*
 * CANcestry - BMS thermal and torque governor.
 *
 * Implements: SW-FR-BMS-001 (stateless evaluation),
 *             SW-FR-BMS-002 (MaxDischargeCurrent limit),
 *             SW-FR-BMS-003 (fixed-point deterministic arithmetic),
 *             SW-FR-BMS-004 (fail-closed BMS fault handling),
 *             SW-FR-BMS-005 (intervention fault output),
 *             SW-FR-BMS-006 (bounded, allocation-free execution).
 *
 * The module intentionally has no mutable storage.  In particular, it does
 * not remember a previous limit, accumulate tokens, or silently log.  The
 * result is the complete evidence record that the integration layer can put
 * into its fault/event sink before it emits the returned actuator value.
 */

#include "cancestry/governor/bms_governor.h"

#include <stddef.h>
#include <stdint.h>

#define BMS_POWER_DENOMINATOR ((uint64_t)1000000000u)

static void bms_result_init(cancestry_bms_governor_result_t *result)
{
    result->decision = CANCESTRY_BMS_GOVERNOR_BLOCK;
    result->requested_current_ma = 0u;
    result->allowed_current_ma = 0u;
    result->maximum_power_w = 0u;
    result->allowed_power_w = 0u;
    result->allowed_torque_mnm = 0u;
    result->intervention = true;
    result->fault_code = CANCESTRY_BMS_GOVERNOR_FAULT_INVALID_INPUT;
}

static uint32_t bms_saturate_u32(uint64_t value)
{
    if (value > (uint64_t)UINT32_MAX) {
        return UINT32_MAX;
    }
    return (uint32_t)value;
}

/**
 * Compute ceil(power * 1e9 / (voltage * efficiency)).
 *
 * The result is a current in mA.  Ceil, rather than round-to-nearest, is
 * required: underestimating current could authorize a request above
 * MaxDischargeCurrent.  The largest possible numerator for the public input
 * types fits in uint64_t; the explicit saturation keeps the contract safe if
 * the types are widened in a later release.
 */
static uint32_t bms_requested_current_ma(uint32_t power_w,
                                         uint32_t voltage_mv,
                                         uint16_t efficiency_permille)
{
    uint64_t numerator = (uint64_t)power_w * BMS_POWER_DENOMINATOR;
    uint64_t denominator = (uint64_t)voltage_mv * (uint64_t)efficiency_permille;
    uint64_t quotient;

    if (denominator == 0u) {
        return UINT32_MAX;
    }
    quotient = numerator / denominator;
    if ((numerator % denominator) != 0u) {
        quotient++;
    }
    return bms_saturate_u32(quotient);
}

/**
 * Compute floor(current * voltage * efficiency / 1e9) without a 128-bit
 * extension.  The current and voltage operands are each uint32_t, so their
 * product is representable in uint64_t.  Division is performed before the
 * efficiency multiplication; the remainder is added separately to preserve
 * the exact floor result.
 */
static uint32_t bms_maximum_power_w(uint32_t current_ma,
                                    uint32_t voltage_mv,
                                    uint16_t efficiency_permille)
{
    uint64_t product;
    uint64_t whole;
    uint64_t remainder;
    uint64_t scaled;

    if (voltage_mv != 0u &&
        (uint64_t)current_ma > UINT64_MAX / (uint64_t)voltage_mv) {
        return UINT32_MAX;
    }
    product = (uint64_t)current_ma * (uint64_t)voltage_mv;
    whole = product / BMS_POWER_DENOMINATOR;
    remainder = product % BMS_POWER_DENOMINATOR;
    scaled = (whole * (uint64_t)efficiency_permille) +
             ((remainder * (uint64_t)efficiency_permille) / BMS_POWER_DENOMINATOR);
    return bms_saturate_u32(scaled);
}

static uint32_t bms_scale_torque(uint32_t torque_mnm,
                                 uint32_t allowed_power_w,
                                 uint32_t requested_power_w)
{
    uint64_t scaled;

    if (requested_power_w == 0u) {
        /* An inconsistent torque-only request is not safe to infer from zero
         * electrical demand; fail closed rather than inventing a ratio. */
        return 0u;
    }
    scaled = ((uint64_t)torque_mnm * (uint64_t)allowed_power_w) /
             (uint64_t)requested_power_w;
    return bms_saturate_u32(scaled);
}

cancestry_bms_governor_status_t cancestry_bms_governor_evaluate(
    const cancestry_bms_governor_request_t *request,
    cancestry_bms_governor_result_t *result)
{
    uint32_t maximum_power;
    uint32_t allowed_power;

    if (result == NULL) {
        return CANCESTRY_BMS_GOVERNOR_ERR_NULL;
    }
    bms_result_init(result);
    if (request == NULL) {
        return CANCESTRY_BMS_GOVERNOR_ERR_NULL;
    }

    result->requested_current_ma = bms_requested_current_ma(
        request->requested_power_w,
        request->bus_voltage_mv,
        request->efficiency_permille);

    if (request->bus_voltage_mv == 0u ||
        request->max_discharge_current_ma == 0u ||
        request->efficiency_permille == 0u ||
        request->efficiency_permille > CANCESTRY_BMS_EFFICIENCY_SCALE) {
        result->fault_code = CANCESTRY_BMS_GOVERNOR_FAULT_INVALID_INPUT;
        return CANCESTRY_BMS_GOVERNOR_ERR_INVALID_INPUT;
    }

    result->allowed_current_ma = request->max_discharge_current_ma;
    maximum_power = bms_maximum_power_w(request->max_discharge_current_ma,
                                        request->bus_voltage_mv,
                                        request->efficiency_permille);
    result->maximum_power_w = maximum_power;

    if (request->bms_state == CANCESTRY_BMS_STATE_FAULT) {
        result->decision = CANCESTRY_BMS_GOVERNOR_BLOCK;
        result->allowed_current_ma = 0u;
        result->maximum_power_w = 0u;
        result->allowed_power_w = 0u;
        result->allowed_torque_mnm = 0u;
        result->fault_code = CANCESTRY_BMS_GOVERNOR_FAULT_BMS_UNAVAILABLE;
        return CANCESTRY_BMS_GOVERNOR_ERR_BMS_FAULT;
    }
    if (request->bms_state != CANCESTRY_BMS_STATE_NORMAL &&
        request->bms_state != CANCESTRY_BMS_STATE_THERMAL_DERATING) {
        result->fault_code = CANCESTRY_BMS_GOVERNOR_FAULT_INVALID_INPUT;
        return CANCESTRY_BMS_GOVERNOR_ERR_INVALID_INPUT;
    }

    allowed_power = (request->requested_power_w < maximum_power)
                        ? request->requested_power_w
                        : maximum_power;
    result->allowed_power_w = allowed_power;
    result->allowed_torque_mnm = bms_scale_torque(request->requested_torque_mnm,
                                                  allowed_power,
                                                  request->requested_power_w);

    if (request->requested_power_w > maximum_power) {
        result->decision = CANCESTRY_BMS_GOVERNOR_DERATE;
        result->intervention = true;
        result->fault_code = CANCESTRY_BMS_GOVERNOR_FAULT_GOVERNOR_INTERVENTION;
    } else {
        result->decision = CANCESTRY_BMS_GOVERNOR_ALLOW;
        result->intervention = false;
        result->fault_code = 0u;
    }

    return CANCESTRY_BMS_GOVERNOR_OK;
}

bool cancestry_bms_governor_may_transmit(
    const cancestry_bms_governor_result_t *result)
{
    if (result == NULL) {
        return false;
    }
    if (result->decision == CANCESTRY_BMS_GOVERNOR_ALLOW) {
        return !result->intervention && result->fault_code == 0u;
    }
    if (result->decision == CANCESTRY_BMS_GOVERNOR_DERATE) {
        return result->intervention &&
               result->fault_code ==
                   CANCESTRY_BMS_GOVERNOR_FAULT_GOVERNOR_INTERVENTION;
    }
    return false;
}

bool cancestry_bms_governor_gate_response(
    const cancestry_bms_governor_request_t *request,
    uint32_t *safe_power_w,
    uint32_t *safe_torque_mnm,
    uint32_t *fault_code)
{
    cancestry_bms_governor_result_t result;
    cancestry_bms_governor_status_t status;

    if (safe_power_w == NULL || safe_torque_mnm == NULL || fault_code == NULL) {
        return false;
    }
    *safe_power_w = 0u;
    *safe_torque_mnm = 0u;
    *fault_code = CANCESTRY_BMS_GOVERNOR_FAULT_INVALID_INPUT;

    status = cancestry_bms_governor_evaluate(request, &result);
    *fault_code = result.fault_code;
    if (status != CANCESTRY_BMS_GOVERNOR_OK ||
        !cancestry_bms_governor_may_transmit(&result)) {
        return false;
    }
    *safe_power_w = result.allowed_power_w;
    *safe_torque_mnm = result.allowed_torque_mnm;
    return true;
}

const char *cancestry_bms_governor_fault_name(uint32_t fault_code)
{
    switch (fault_code) {
    case 0u:
        return "NONE";
    case CANCESTRY_BMS_GOVERNOR_FAULT_GOVERNOR_INTERVENTION:
        return "GOVERNOR_INTERVENTION";
    case CANCESTRY_BMS_GOVERNOR_FAULT_INVALID_INPUT:
        return "INVALID_INPUT";
    case CANCESTRY_BMS_GOVERNOR_FAULT_BMS_UNAVAILABLE:
        return "BMS_UNAVAILABLE";
    default:
        return "UNKNOWN";
    }
}

const char *cancestry_bms_governor_decision_name(
    cancestry_bms_governor_decision_t decision)
{
    switch (decision) {
    case CANCESTRY_BMS_GOVERNOR_ALLOW:
        return "ALLOW";
    case CANCESTRY_BMS_GOVERNOR_DERATE:
        return "DERATE";
    case CANCESTRY_BMS_GOVERNOR_BLOCK:
        return "BLOCK";
    default:
        return "INVALID";
    }
}
