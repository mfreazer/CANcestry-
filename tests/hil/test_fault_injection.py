"""Host verification of the Phase 12 software HIL model.

Implements: SW-FR-HIL-001..006.
Test ids: HIL-BUSOFF-001, HIL-CRC-001, HIL-BROWNOUT-001.
"""

from pathlib import Path
import sys

# Permit both ``pytest tests/hil`` and direct execution from the repository.
sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from tests.hil.hil_fault_injection import (  # noqa: E402
    FaultKind,
    GatewayState,
    HilFaultInjectionRunner,
)


def test_bus_off_recovery_is_hardware_first():
    result = HilFaultInjectionRunner().run("bus_off")[0]
    assert result.test_id == "HIL-BUSOFF-001"
    assert result.fault == FaultKind.BUS_OFF.value
    assert result.passed
    assert result.metrics["safe_state_transitions"] == 1
    assert result.metrics["recovery_count"] == 1
    assert result.metrics["tx_authorized_after_recovery"]

    events = [entry["event"] for entry in result.trace]
    assert events.index("BUS_OFF_ASSERTED") < events.index("FSM_SAFE_STATE")
    assert events.index("BUS_OFF_RECOVERED") < events.index("FSM_RECOVERY_COMPLETE")


def test_crc_error_is_rejected_before_software_queue():
    result = HilFaultInjectionRunner().run("crc_error")[0]
    assert result.test_id == "HIL-CRC-001"
    assert result.fault == FaultKind.CRC_ERROR.value
    assert result.passed
    assert result.metrics["crc_rejections"] == 1
    assert result.metrics["software_queue_depth"] == 0
    assert result.metrics["processed_frames"] == 0
    assert result.trace[-1]["event"] == "CAN_CRC_REJECTED"


def test_brownout_latches_safe_outputs_before_power_down():
    result = HilFaultInjectionRunner().run("brownout")[0]
    assert result.test_id == "HIL-BROWNOUT-001"
    assert result.fault == FaultKind.BROWNOUT.value
    assert result.passed
    assert result.metrics["bor_triggered"]
    assert result.metrics["torque_nm"] == 0
    assert not result.metrics["contactors_closed"]
    assert not result.metrics["mcu_running"]
    assert result.trace[-1]["event"] == "FSM_SAFE_STATE"


def test_all_scenarios_are_stable_and_pass():
    results = HilFaultInjectionRunner().run("all")
    assert [result.test_id for result in results] == [
        "HIL-BUSOFF-001",
        "HIL-CRC-001",
        "HIL-BROWNOUT-001",
    ]
    assert all(result.passed for result in results)
    assert all(result.fault in {kind.value for kind in FaultKind} for result in results)
    assert GatewayState.ACTIVE.value == "ACTIVE"
