#!/usr/bin/env python3
"""T2 scenario t2_busoff_001: Bus-Off recovery on the virtual bench (H-10).

Issue #62, commit 2: start the v1.0.0 firmware (off-tree CAN fault driver)
in Renode, inject a Bus-Off condition at t = 10 ms through the CAN bus fault
injector, capture the firmware's detection and recovery timestamps, and
assert the ISO 11898-1 timing invariant:

    recovery_us - detection_us < 128 * 11 bit_times (704 us at 2 Mbit/s)

Shared machinery: hw/virtual-bench/t2_fault_common.py (fail-closed
orchestration, pending/passing evidence forms, determinism gates).
Requirements traced: HW-FR-003; HwAGENTS.md rules 1, 4 and 5.
Oracle: none (issue #62; scenario stays sim-pending / CL0).

Usage:
    python3 hw/virtual-bench/scenarios/t2_busoff_001.py                 (run)
    python3 hw/virtual-bench/scenarios/t2_busoff_001.py --check         (gate)
    python3 hw/virtual-bench/scenarios/t2_busoff_001.py --emit-pending  (manifest)
"""

from __future__ import annotations

import sys
from pathlib import Path

BRIDGE_DIR = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(BRIDGE_DIR))

from t2_fault_common import (  # noqa: E402
    CMD_INJECT_BUSOFF,
    SCHEMA_PATH,  # noqa: F401  re-exported for the pytest wrapper
    SLOT_BUSOFF_INJECT_US,
    SLOT_BUSOFF_DETECT_US,
    SLOT_BUSOFF_RECOVER_US,
    SLOT_BUS_ACTIVE_US,
    SLOT_RECOVERY_REQUEST_US,
    ScenarioSpec,
    cli_main,
    expected_pending_manifest,  # noqa: F401  used by test_t2_busoff.py
)

SCENARIO = ScenarioSpec(
    case_id="t2_busoff_001",
    fault_command=CMD_INJECT_BUSOFF,
    fault="bus_off",
    trace_columns="time_us,bus_state,fdcan1_psr,alive_counter",
    watch_slots={
        "bus_active_us": SLOT_BUS_ACTIVE_US,
        "detection_us": SLOT_BUSOFF_DETECT_US,
        "injection_us": SLOT_BUSOFF_INJECT_US,
        "recovery_request_us": SLOT_RECOVERY_REQUEST_US,
        "recovery_us": SLOT_BUSOFF_RECOVER_US,
    },
    evidence_relative="hw/tests/evidence/t2_busoff_001.json",
    scenario_name="Bus-Off recovery (HIL-BUSOFF-001)",
    evidence_of=(
        "T2 virtual-bench Bus-Off recovery scenario for HW-FR-003 (H-10, "
        "issue #62): the CAN bus fault injector "
        "(hw/virtual-bench/renode/can_fault_injector.py) puts the shared "
        "medium into the Bus-Off condition at t = 10 ms; the real v1.0.0 "
        "firmware (off-tree driver hw/virtual-bench/firmware/can_fault.c) "
        "detects it, enters the real cancestry_hal_raise_fault path "
        "(CANCESTRY_HAL_FAULT_BUS_OFF -> CANCESTRY_HAL_IF_STATE_BUS_OFF plus "
        "the FAULT_RAISED event) and requests recovery by clearing "
        "CCCR.INIT; the medium releases the bus exactly 128 * 11 recessive "
        "bits later (704 us at the HW-FR-003 nominal 2 Mbit/s) and the "
        "recovery-action interval is asserted strictly below that bound. "
        "This manifest records the PENDING disposition (pass=false, CL0): "
        "no executing run of this scenario backs an evidence artifact yet."),
    pending_reason=(
        "The T2 fault-scenario machinery (CAN fault injector, scenario "
        "bring-up cancestry-hw-fault.resc, orchestration and the off-tree "
        "CAN fault driver) is delivered by https://github.com/mfreazer/"
        "CANcestry-/issues/62 (H-10), but no executed T2 run backs this "
        "artifact yet: like the H-07 retention foundation, the first "
        "executing run is produced by hw/virtual-bench/scenarios/"
        "t2_busoff_001.py on the T2 toolchain image (hw-nightly "
        "t2-virtual-bench job) and must reproduce this manifest's ledger "
        "chain deterministically - plus a second byte-identical run via "
        "--check - before any passing disposition may be claimed."),
    not_covered=[
        "No executed T2 run exists for this scenario: no event timestamps, "
        "no trace CSV and no ELF/bridge/injector artifact hashes are "
        "claimed by this manifest.",
        "No independent oracle exists for Bus-Off recovery timing (issue "
        "#62: oracle none): the 128 * 11 recessive-bit limit is the ISO "
        "11898-1 recovery sequence MODELLED by the fault injector itself. "
        "Credibility stays CL0 until a registered oracle or a golden bench "
        "capture exists; no oracle_id may be promoted into this row by "
        "rendering or prose.",
        "The FDCAN protocol engine, bit timing, arbitration, bit stuffing "
        "and the physical layer are not_simulated "
        "(stm32g474-cancestry.repl header): the medium projects the fault "
        "CONDITION (PSR.BO / CCCR.INIT) only; no frame-level claim is made.",
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
        "hw/virtual-bench/scenarios/t2_busoff_001.py",
        "hw/virtual-bench/t2_fault_common.py",
        "hw/virtual-bench/test_t2_busoff.py",
        "schemas/hw/hw-t2-fault-evidence-0.1.0.schema.json",
        "schemas/hw/hw-traceability-0.1.0.schema.json",
    ),
)


def main(argv=None):
    return cli_main(SCENARIO, argv if argv is not None else sys.argv)


if __name__ == "__main__":
    sys.exit(main(sys.argv))
