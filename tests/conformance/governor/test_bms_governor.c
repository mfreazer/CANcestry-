/*
 * CANcestry Phase 12 BMS governor conformance.
 *
 * Implements: SW-FR-BMS-001..006.
 * Test ids: BMS-GOV-001 .. BMS-GOV-008.
 *
 * The vectors are deliberately integer-only.  They prove that a thermal BMS
 * limit cannot be bypassed by a 100 kW VCU request, that the returned value is
 * the one an integration may transmit, and that malformed or unavailable BMS
 * data fails closed.
 */

#include "cancestry/governor/bms_governor.h"

#include "cancestry_test.h"

#include <stdint.h>

static cancestry_bms_governor_request_t nominal_request(void)
{
    cancestry_bms_governor_request_t request;

    request.requested_power_w = 100000u;
    request.requested_torque_mnm = 1000000u;
    request.bus_voltage_mv = 400000u;
    request.max_discharge_current_ma = 250000u;
    request.efficiency_permille = 950u;
    request.bms_state = CANCESTRY_BMS_STATE_THERMAL_DERATING;
    return request;
}

static void test_thermal_intervention(void)
{
    cancestry_bms_governor_request_t request = nominal_request();
    cancestry_bms_governor_result_t result;
    cancestry_bms_governor_status_t status;

    CANCESSTRY_TEST_CASE("BMS-GOV-001: thermal derating blocks the original 100 kW value");
    status = cancestry_bms_governor_evaluate(&request, &result);

    CANCESSTRY_TEST_CHECK(status == CANCESTRY_BMS_GOVERNOR_OK);
    CANCESSTRY_TEST_CHECK(result.decision == CANCESTRY_BMS_GOVERNOR_DERATE);
    CANCESSTRY_TEST_CHECK_U64(result.maximum_power_w, 95000u);
    CANCESSTRY_TEST_CHECK_U64(result.allowed_power_w, 95000u);
    CANCESSTRY_TEST_CHECK_U64(result.allowed_torque_mnm, 950000u);
    CANCESSTRY_TEST_CHECK(result.intervention);
    CANCESSTRY_TEST_CHECK_U64(result.fault_code,
                              CANCESTRY_BMS_GOVERNOR_FAULT_GOVERNOR_INTERVENTION);
    CANCESSTRY_TEST_CHECK(cancestry_bms_governor_may_transmit(&result));

    {
        uint32_t safe_power_w = 0u;
        uint32_t safe_torque_mnm = 0u;
        uint32_t fault_code = 0u;

        CANCESSTRY_TEST_CHECK(cancestry_bms_governor_gate_response(
            &request, &safe_power_w, &safe_torque_mnm, &fault_code));
        CANCESSTRY_TEST_CHECK_U64(safe_power_w, 95000u);
        CANCESSTRY_TEST_CHECK_U64(safe_torque_mnm, 950000u);
        CANCESSTRY_TEST_CHECK_U64(fault_code,
                                  CANCESTRY_BMS_GOVERNOR_FAULT_GOVERNOR_INTERVENTION);
    }
}

static void test_normal_request_within_limit(void)
{
    cancestry_bms_governor_request_t request = nominal_request();
    cancestry_bms_governor_result_t result;

    CANCESSTRY_TEST_CASE("BMS-GOV-002: an in-budget normal request is allowed unchanged");
    request.requested_power_w = 90000u;
    request.requested_torque_mnm = 900000u;
    request.bms_state = CANCESTRY_BMS_STATE_NORMAL;

    CANCESSTRY_TEST_CHECK(cancestry_bms_governor_evaluate(&request, &result) ==
                          CANCESTRY_BMS_GOVERNOR_OK);
    CANCESSTRY_TEST_CHECK(result.decision == CANCESTRY_BMS_GOVERNOR_ALLOW);
    CANCESSTRY_TEST_CHECK_U64(result.allowed_power_w, 90000u);
    CANCESSTRY_TEST_CHECK_U64(result.allowed_torque_mnm, 900000u);
    CANCESSTRY_TEST_CHECK(!result.intervention);
    CANCESSTRY_TEST_CHECK_U64(result.fault_code, 0u);
}

static void test_bms_fault_blocks(void)
{
    cancestry_bms_governor_request_t request = nominal_request();
    cancestry_bms_governor_result_t result;

    CANCESSTRY_TEST_CASE("BMS-GOV-003: unavailable BMS data produces a zero-output block");
    request.bms_state = CANCESTRY_BMS_STATE_FAULT;

    CANCESSTRY_TEST_CHECK(cancestry_bms_governor_evaluate(&request, &result) ==
                          CANCESTRY_BMS_GOVERNOR_ERR_BMS_FAULT);
    CANCESSTRY_TEST_CHECK(result.decision == CANCESTRY_BMS_GOVERNOR_BLOCK);
    CANCESSTRY_TEST_CHECK_U64(result.allowed_power_w, 0u);
    CANCESSTRY_TEST_CHECK_U64(result.allowed_torque_mnm, 0u);
    CANCESSTRY_TEST_CHECK_U64(result.fault_code,
                              CANCESTRY_BMS_GOVERNOR_FAULT_BMS_UNAVAILABLE);
    CANCESSTRY_TEST_CHECK(!cancestry_bms_governor_may_transmit(&result));

    {
        uint32_t safe_power_w = UINT32_MAX;
        uint32_t safe_torque_mnm = UINT32_MAX;
        uint32_t fault_code = 0u;

        CANCESSTRY_TEST_CHECK(!cancestry_bms_governor_gate_response(
            &request, &safe_power_w, &safe_torque_mnm, &fault_code));
        CANCESSTRY_TEST_CHECK_U64(safe_power_w, 0u);
        CANCESSTRY_TEST_CHECK_U64(safe_torque_mnm, 0u);
        CANCESSTRY_TEST_CHECK_U64(fault_code,
                                  CANCESTRY_BMS_GOVERNOR_FAULT_BMS_UNAVAILABLE);
    }
}

static void test_invalid_snapshot_blocks(void)
{
    cancestry_bms_governor_request_t request = nominal_request();
    cancestry_bms_governor_result_t result;

    CANCESSTRY_TEST_CASE("BMS-GOV-004: zero voltage and invalid efficiency fail closed");
    request.bus_voltage_mv = 0u;

    CANCESSTRY_TEST_CHECK(cancestry_bms_governor_evaluate(&request, &result) ==
                          CANCESTRY_BMS_GOVERNOR_ERR_INVALID_INPUT);
    CANCESSTRY_TEST_CHECK(result.decision == CANCESTRY_BMS_GOVERNOR_BLOCK);
    CANCESSTRY_TEST_CHECK_U64(result.allowed_power_w, 0u);
    CANCESSTRY_TEST_CHECK_U64(result.fault_code,
                              CANCESTRY_BMS_GOVERNOR_FAULT_INVALID_INPUT);
    request = nominal_request();
    request.efficiency_permille = (uint16_t)(CANCESTRY_BMS_EFFICIENCY_SCALE + 1u);
    CANCESSTRY_TEST_CHECK(cancestry_bms_governor_evaluate(&request, &result) ==
                          CANCESTRY_BMS_GOVERNOR_ERR_INVALID_INPUT);
    CANCESSTRY_TEST_CHECK(result.decision == CANCESTRY_BMS_GOVERNOR_BLOCK);
}

static void test_rounding_and_exact_boundary(void)
{
    cancestry_bms_governor_request_t request = nominal_request();
    cancestry_bms_governor_result_t result;

    CANCESSTRY_TEST_CASE("BMS-GOV-005: current rounding is conservative at the boundary");
    request.requested_power_w = 95000u;
    request.bms_state = CANCESTRY_BMS_STATE_NORMAL;

    CANCESSTRY_TEST_CHECK(cancestry_bms_governor_evaluate(&request, &result) ==
                          CANCESTRY_BMS_GOVERNOR_OK);
    CANCESSTRY_TEST_CHECK(result.decision == CANCESTRY_BMS_GOVERNOR_ALLOW);
    CANCESSTRY_TEST_CHECK_U64(result.requested_current_ma, 250000u);
    CANCESSTRY_TEST_CHECK_U64(result.allowed_power_w, 95000u);

    request.requested_power_w = 95001u;
    CANCESSTRY_TEST_CHECK(cancestry_bms_governor_evaluate(&request, &result) ==
                          CANCESTRY_BMS_GOVERNOR_OK);
    CANCESSTRY_TEST_CHECK(result.decision == CANCESTRY_BMS_GOVERNOR_DERATE);
    CANCESSTRY_TEST_CHECK_U64(result.allowed_power_w, 95000u);
}

static void test_zero_power_and_saturated_current(void)
{
    cancestry_bms_governor_request_t request = nominal_request();
    cancestry_bms_governor_result_t result;

    CANCESSTRY_TEST_CASE("BMS-GOV-006: zero demand and large-current arithmetic stay bounded");
    request.requested_power_w = 0u;
    request.requested_torque_mnm = 123u;
    request.bms_state = CANCESTRY_BMS_STATE_NORMAL;
    CANCESSTRY_TEST_CHECK(cancestry_bms_governor_evaluate(&request, &result) ==
                          CANCESTRY_BMS_GOVERNOR_OK);
    CANCESSTRY_TEST_CHECK(result.decision == CANCESTRY_BMS_GOVERNOR_ALLOW);
    CANCESSTRY_TEST_CHECK_U64(result.requested_current_ma, 0u);
    CANCESSTRY_TEST_CHECK_U64(result.allowed_torque_mnm, 0u);

    request = nominal_request();
    request.requested_power_w = UINT32_MAX;
    request.bus_voltage_mv = 1u;
    request.max_discharge_current_ma = 1u;
    request.efficiency_permille = 1u;
    CANCESSTRY_TEST_CHECK(cancestry_bms_governor_evaluate(&request, &result) ==
                          CANCESTRY_BMS_GOVERNOR_OK);
    CANCESSTRY_TEST_CHECK_U64(result.requested_current_ma, UINT32_MAX);
    CANCESSTRY_TEST_CHECK_U64(result.maximum_power_w, 0u);
    CANCESSTRY_TEST_CHECK(result.decision == CANCESTRY_BMS_GOVERNOR_DERATE);
}

static void test_null_and_names(void)
{
    cancestry_bms_governor_result_t result;

    CANCESSTRY_TEST_CASE("BMS-GOV-007: NULL API use and diagnostic names are deterministic");
    CANCESSTRY_TEST_CHECK(cancestry_bms_governor_evaluate(NULL, &result) ==
                          CANCESTRY_BMS_GOVERNOR_ERR_NULL);
    CANCESSTRY_TEST_CHECK(cancestry_bms_governor_evaluate(NULL, NULL) ==
                          CANCESTRY_BMS_GOVERNOR_ERR_NULL);
    CANCESSTRY_TEST_CHECK(!cancestry_bms_governor_may_transmit(NULL));
    result.decision = (cancestry_bms_governor_decision_t)99;
    result.fault_code = 0u;
    CANCESSTRY_TEST_CHECK(!cancestry_bms_governor_may_transmit(&result));
    CANCESSTRY_TEST_CHECK_STRING(
        cancestry_bms_governor_fault_name(CANCESTRY_BMS_GOVERNOR_FAULT_GOVERNOR_INTERVENTION),
        "GOVERNOR_INTERVENTION");
    CANCESSTRY_TEST_CHECK_STRING(cancestry_bms_governor_fault_name(0xFFFFFFFFu), "UNKNOWN");
    CANCESSTRY_TEST_CHECK_STRING(
        cancestry_bms_governor_decision_name(CANCESTRY_BMS_GOVERNOR_DERATE),
        "DERATE");
    CANCESSTRY_TEST_CHECK_STRING(
        cancestry_bms_governor_decision_name((cancestry_bms_governor_decision_t)99),
        "INVALID");
}

static void test_unknown_bms_state_blocks(void)
{
    cancestry_bms_governor_request_t request = nominal_request();
    cancestry_bms_governor_result_t result;

    CANCESSTRY_TEST_CASE("BMS-GOV-008: unknown BMS state fails closed");
    request.bms_state = (cancestry_bms_state_t)99;
    CANCESSTRY_TEST_CHECK(cancestry_bms_governor_evaluate(&request, &result) ==
                          CANCESTRY_BMS_GOVERNOR_ERR_INVALID_INPUT);
    CANCESSTRY_TEST_CHECK(result.decision == CANCESTRY_BMS_GOVERNOR_BLOCK);
    CANCESSTRY_TEST_CHECK_U64(result.allowed_power_w, 0u);
    CANCESSTRY_TEST_CHECK_U64(result.fault_code,
                              CANCESTRY_BMS_GOVERNOR_FAULT_INVALID_INPUT);
}

int main(void)
{
    CANCESSTRY_TEST_SUITE_BEGIN("BMS Thermal and Torque Governor");

    test_thermal_intervention();
    test_normal_request_within_limit();
    test_bms_fault_blocks();
    test_invalid_snapshot_blocks();
    test_rounding_and_exact_boundary();
    test_zero_power_and_saturated_current();
    test_null_and_names();
    test_unknown_bms_state_blocks();

    return CANCESSTRY_TEST_SUITE_END();
}
