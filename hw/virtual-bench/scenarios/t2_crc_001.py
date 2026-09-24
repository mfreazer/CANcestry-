#!/usr/bin/env python3
"""T2 scenario t2_crc_001: CRC error handling on the virtual bench (H-10).

Issue #62, commit 3: start the v1.0.0 firmware (off-tree CAN fault driver)
in Renode, inject a CRC error at t = 10 ms through the CAN bus fault
injector (one frame's CRC condition corrupted on the shared medium), capture
the firmware's detection timestamp, and assert that the firmware increments
its CRC error counter, acknowledges the error-logging interrupt and
completes the scenario WITHOUT crashing (run_complete = 1, the alive counter
keeps advancing).

Shared machinery: hw/virtual-bench/t2_fault_common.py (fail-closed
orchestration, pending/passing evidence forms, determinism gates).
Requirements traced: HW-FR-003; HwAGENTS.md rules 1, 4 and 5.
Oracle: none (issue #62; scenario stays sim-pending / CL0).

Usage:
    python3 hw/virtual-bench/scenarios/t2_crc_001.py                (run)
    python3 hw/virtual-bench/scenarios/t2_crc_001.py --check        (gate)
    python3 hw/virtual-bench/scenarios/t2_crc_001.py --emit-pending (manifest)
"""

from __future__ import annotations

import sys
from pathlib import Path

BRIDGE_DIR = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(BRIDGE_DIR))

from t2_fault_common import (  # noqa: E402
    CMD_INJECT_CRC,
    SCHEMA_PATH,  # noqa: F401  re-exported for the pytest wrapper
    SLOT_CRC_INJECT_US,
    SLOT_CRC_DETECT_US,
    SLOT_CRC_ACK_US,
    SLOT_CRC_CLEARED_US,
    ScenarioSpec,
    cli_main,
    expected_pending_manifest,  # noqa: F401  used by test_t2_crc.py
)

SCENARIO = ScenarioSpec(
    case_id="t2_crc_001",
    fault_command=CMD_INJECT_CRC,
    fault="crc_error",
    trace_columns="time_us,bus_state,fdcan1_psr,crc_error_count",
    watch_slots={
        "crc_ack_us": SLOT_CRC_ACK_US,
        "crc_cleared_us": SLOT_CRC_CLEARED_US,
        "detection_us": SLOT_CRC_DETECT_US,
        "injection_us": SLOT_CRC_INJECT_US,
    },
    evidence_relative="hw/tests/evidence/t2_crc_001.json",
    scenario_name="CRC error handling (HIL-CRC-001)",
    evidence_of=(
        "T2 virtual-bench CRC error handling scenario for HW-FR-003 (H-10, "
        "issue #62): the CAN bus fault injector "
        "(hw/virtual-bench/renode/can_fault_injector.py) corrupts the CRC "
        "condition of one frame on the shared medium at t = 10 ms "
        "(PSR.LEC = CRC error, ECR.REC/CEL advanced, IR.ELO raised); the "
        "real v1.0.0 firmware (off-tree driver "
        "hw/virtual-bench/firmware/can_fault.c) detects it, increments its "
        "firmware CRC error counter and raises the fault through the real "
        "cancestry_hal_raise_fault path "
        "(CANCESTRY_HAL_FAULT_MALFORMED_FRAME, the v1.0.0 normative code "
        "for a corrupted wire frame), acknowledges the error-logging "
        "interrupt and completes the scenario uncrashed. This manifest "
        "records the PENDING disposition (pass=false, CL0): no executing "
        "run of this scenario backs an evidence artifact yet."),
    pending_reason=(
        "The T2 fault-scenario machinery (CAN fault injector, scenario "
        "bring-up cancestry-hw-fault.resc, orchestration and the off-tree "
        "CAN fault driver) is delivered by https://github.com/mfreazer/"
        "CANcestry-/issues/62 (H-10), but no executed T2 run backs this "
        "artifact yet: like the H-07 retention foundation, the first "
        "executing run is produced by hw/virtual-bench/scenarios/"
        "t2_crc_001.py on the T2 toolchain image (hw-nightly "
        "t2-virtual-bench job) and must reproduce this manifest's ledger "
        "chain deterministically - plus a second byte-identical run via "
        "--check - before any passing disposition may be claimed."),
    not_covered=[
        "No executed T2 run exists for this scenario: no event timestamps, "
        "no trace CSV and no ELF/bridge/injector artifact hashes are "
        "claimed by this manifest.",
        "No independent oracle exists for CRC error handling (issue #62: "
        "oracle none): the asserted properties are the firmware's counter "
        "increment and its crash-free handling. Credibility stays CL0 "
        "until a registered oracle or a golden bench capture exists.",
        "The no-corrupted-frame-to-software-queue property of HIL-CRC-001 "
        "belongs to the software boundary conformance suite "
        "(docs/qa/hil-fault-injection-report.md section 4.2); this "
        "scenario injects no frame content and derives no RX-path claim, "
        "staying inside the issue #62 scope (no scope creep).",
        "The FDCAN protocol engine, bit timing, arbitration, bit stuffing "
        "and the physical layer are not_simulated "
        "(stm32g474-cancestry.repl header): the medium projects the fault "
        "CONDITION (PSR.LEC / ECR / IR.ELO) only.",
        "Brownout / BOR physics: deferred to H-11, gated on HS-01/HS-02 and "
        "a human safety reviewer (issue #62 constraint; HwAGENTS.md rule "
        "6).",
        "DWT CYCCNT accuracy is deliberately unexercised (bring-up finding "
        "F-27): events are captured from emulation virtual time and carry "
        "the renode TCL2 gap inherited above.",
        "T4 physical correlation: bench-pending (HwRS section 5); CL3 "
        "requires bench data and no ledger promotion is performed here.",
    ],
    pinned_sources=(
        "ci/docker/Dockerfile.t2",
        "ci/docker/renode-1.16.1.pin",
        "docs/hw/tool-qualification.md",
        "docs/hw/virtual-bench-plan.md",
        "hw/virtual-bench/firmware/build_firmware.sh",
        "hw/virtual-bench/firmware/can_fault.c",
        "hw/virtual-bench/firmware/startup.s",
        "hw/virtual-bench/fmi_bridge.py",
        "hw/virtual-bench/renode/can_fault_injector.py",
        "hw/virtual-bench/renode/cancestry-hw-fault.resc",
        "hw/virtual-bench/renode/stm32g474-cancestry.repl",
        "hw/virtual-bench/scenarios/t2_crc_001.py",
        "hw/virtual-bench/t2_fault_common.py",
        "hw/virtual-bench/test_t2_crc.py",
        "schemas/hw/hw-t2-fault-evidence-0.1.0.schema.json",
        "schemas/hw/hw-traceability-0.1.0.schema.json",
    ),
)


def main(argv=None):
    return cli_main(SCENARIO, argv if argv is not None else sys.argv)


if __name__ == "__main__":
    sys.exit(main(sys.argv))
