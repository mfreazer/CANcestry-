#!/usr/bin/env python3
"""T2 scenario t2_brownout_001: brownout / BOR reset on the virtual bench (H-11).

Issue #64, commit 3: start the v1.0.0 firmware (off-tree bor_brownout driver)
in Renode, step the CancestryLib.Power.BOR plant over the deterministic FMI
2.0 co-simulation, let the MODELLED BOR assertion drive the brownout injection
through the BOR reset injector (t = 10 ms, NRST asserted for 100 us), capture
the firmware's retention-write, BOR-detection and recovery timestamps, and
assert the issue's ordering chain:

    retention_write < BOR detection < recovery

plus the retention/SRAM split (the RTC backup domain preserved across the
reset, main SRAM lost), the BOR reset cause and the NRST pulse length.

Shared machinery: hw/virtual-bench/t2_bor_common.py (fail-closed orchestration,
pending/passing evidence forms, determinism gates).
Requirements traced: HW-SF-002 (sub-events (ii)/(iii)), HW-SF-004;
HwAGENTS.md rules 1, 4, 5 and 6.
Oracle: none (issue #64; the scenario stays sim-pending / CL0).

Usage:
    python3 hw/virtual-bench/scenarios/t2_brownout_001.py                 (run)
    python3 hw/virtual-bench/scenarios/t2_brownout_001.py --check         (gate)
    python3 hw/virtual-bench/scenarios/t2_brownout_001.py --emit-pending  (manifest)
"""

from __future__ import annotations

import sys
from pathlib import Path

BRIDGE_DIR = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(BRIDGE_DIR))

from t2_bor_common import (  # noqa: E402
    SCHEMA_PATH,  # noqa: F401  re-exported for the pytest wrapper
    SLOT_BOR_DETECT_US,
    SLOT_BOR_INJECT_US,
    SLOT_BOR_RECOVER_US,
    SLOT_NRST_RELEASE_US,
    SLOT_RETENTION_WRITE_US,
    ScenarioSpec,
    cli_main,
    expected_pending_manifest,  # noqa: F401  used by test_t2_brownout.py
)

SCENARIO = ScenarioSpec(
    case_id="t2_brownout_001",
    evidence_relative="hw/tests/evidence/t2_brownout_001.json",
    scenario_name="Brownout / BOR reset (HW-SF-002 (ii)/(iii), HW-SF-004)",
    evidence_of=(
        "T2 virtual-bench brownout scenario for HW-SF-002 (H-11, issue #64): "
        "the H-11 plant CancestryLib.Power.BOR (hw/model/CancestryLib/Power/"
        "BOR.mo, compiled to an FMI 2.0 CoSimulation FMU by the pinned "
        "OpenModelica) collapses the 3V3 rail at t = 10 ms; its modelled BOR "
        "assertion drives the BOR reset injector "
        "(hw/virtual-bench/renode/bor_reset_injector.py), which pulls the "
        "active-low NRST line for 100 us, discards the main-SRAM marker word, "
        "sets RCC_CSR.BORRSTF and takes the BOR machine reset. The REAL "
        "v1.0.0 firmware (off-tree driver hw/virtual-bench/firmware/"
        "bor_brownout.c) then re-enters its reset handler, classifies the "
        "reset cause, recovers the retained terminal code through the real "
        "cancestry_watchdog_read_retention_register API and restores the safe "
        "state through the real cancestry_hardware_set_safe_state function. "
        "The artifact asserts the issue's ordering chain retention write < "
        "BOR detection < recovery, that the retained code survived the reset "
        "while main SRAM did not, and that the NRST pulse is exactly 100 us. "
        "This manifest records the PENDING disposition (pass=false, CL0): no "
        "executing run of this scenario backs an evidence artifact yet."),
    pending_reason=(
        "The T2 brownout machinery (BOR plant model, BOR reset injector, "
        "brownout bring-up cancestry-hw-bor.resc, orchestration and the "
        "off-tree brownout driver) is delivered by "
        "https://github.com/mfreazer/CANcestry-/issues/64 (H-11), but no "
        "executed T2 run backs this artifact yet: like the H-07 retention and "
        "H-10 CAN fault foundations, the first executing run is produced by "
        "hw/virtual-bench/scenarios/t2_brownout_001.py on the T2 toolchain "
        "image (hw-nightly t2-virtual-bench job) and must reproduce this "
        "manifest's ledger chain deterministically - plus a second "
        "byte-identical run via --check - before any passing disposition may "
        "be claimed. Promotion beyond sim-pending / CL0 additionally stays "
        "gated on HS-01/HS-02 plus a human safety reviewer (issue #64: the "
        "BOR physics are safety-relevant, HwAGENTS.md rule 6)."),
    not_covered=[
        "No executed T2 run exists for this scenario: no event timestamps, "
        "no trace CSV and no ELF/FMU/bridge/injector artifact hashes are "
        "claimed by this manifest.",
        "No independent oracle exists for the BOR threshold behaviour, the "
        "hysteresis release gate or the reset timing (issue #64: oracle "
        "none) - the modelled threshold and the reset line are the scenario's "
        "own stimulus. Credibility stays CL0 until a registered oracle or a "
        "golden bench capture exists; no oracle_id may be promoted into this "
        "row by rendering or prose. OR-001 witnesses only the retention-domain "
        "closed form of the T1 plant (hw/tests/test_bor_physics.py).",
        "The BOR hysteresis value (V_bor_hyst) and the post-reset resumption "
        "latency (t_boot) are declared ENGINEERING FIXTURES in BOR.mo: DS12787 "
        "publishes no BOR hysteresis and the v1.0.0 post-reset startup latency "
        "is not characterised. Bench correlation pending (HwRS section 5).",
        "The supply itself is not modelled beyond the BOR fixture's step "
        "collapse: real brownout shapes, the finite fall time of the rail, "
        "multiple/repeated brownouts and the vehicle harness impedance are out "
        "of H-11 scope (issue #64: no scope creep).",
        "The production retention drain path (cancestry_watchdog_"
        "restore_retained_fault, which clears RTC_BKP0R after admission) is "
        "deliberately NOT exercised: the scenario keeps the retained code "
        "readable across the whole window so the preservation claim can be "
        "asserted from the register readback itself (documented deviation, "
        "firmware/bor_brownout.c).",
        "The BOR cell behaviour on silicon (threshold spread over "
        "temperature, option-byte configuration, backup-domain switching) is "
        "not_simulated: the injector models the RESET LINE and its retention "
        "consequences, never the cell.",
        "DWT CYCCNT accuracy is deliberately unexercised (bring-up finding "
        "F-27): events are captured from emulation virtual time and carry the "
        "renode TCL2 gap inherited above.",
        "T4 physical correlation: bench-pending (HwRS section 5); CL3 "
        "requires bench data and no ledger promotion is performed here.",
    ],
    watch_slots={
        "bor_detect_us": SLOT_BOR_DETECT_US,
        "bor_inject_us": SLOT_BOR_INJECT_US,
        "bor_recover_us": SLOT_BOR_RECOVER_US,
        "nrst_release_us": SLOT_NRST_RELEASE_US,
        "retention_write_us": SLOT_RETENTION_WRITE_US,
    },
    pinned_sources=(
        "ci/docker/Dockerfile.t2",
        "ci/docker/renode-1.16.1.pin",
        "docs/hw/tool-qualification.md",
        "docs/hw/virtual-bench-plan.md",
        "hw/model/CancestryLib/Power/BOR.mo",
        "hw/model/CancestryLib/Power/package.mo",
        "hw/model/CancestryLib/package.mo",
        "hw/model/bridge.json",
        "hw/tests/cases/bor_brownout_001.simcase.json",
        "hw/tests/test_bor_physics.py",
        "hw/virtual-bench/firmware/bor_brownout.c",
        "hw/virtual-bench/firmware/build_firmware.sh",
        "hw/virtual-bench/firmware/startup.s",
        "hw/virtual-bench/fmi_bridge.py",
        "hw/virtual-bench/renode/bor_reset_injector.py",
        "hw/virtual-bench/renode/cancestry-hw-bor.resc",
        "hw/virtual-bench/renode/gpioa_model.py",
        "hw/virtual-bench/renode/iwdg_model.py",
        "hw/virtual-bench/renode/rcc_model.py",
        "hw/virtual-bench/renode/rtc_backup_model.py",
        "hw/virtual-bench/renode/stm32g474-cancestry.repl",
        "hw/virtual-bench/scenarios/t2_brownout_001.py",
        "hw/virtual-bench/t2_bor_common.py",
        "hw/virtual-bench/test_t2_brownout.py",
        "schemas/hw/hw-sim-0.1.0.schema.json",
        "schemas/hw/hw-t2-brownout-evidence-0.1.0.schema.json",
        "schemas/hw/hw-traceability-0.1.0.schema.json",
    ),
)


def main(argv=None):
    return cli_main(SCENARIO, argv if argv is not None else sys.argv)


if __name__ == "__main__":
    sys.exit(main(sys.argv))
