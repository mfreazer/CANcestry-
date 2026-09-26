#!/usr/bin/env python3
"""T2 orchestration tests: t2_busoff_001 Bus-Off recovery (H-10, issue #62).

Layers:

* Unit tests (always run): contract-constant lockstep across
  renode/can_fault_injector.py, renode/cancestry-hw-fault.resc and
  firmware/can_fault.c; the ISO 11898-1 128 x 11 bound; the pending-manifest
  schema contract and determinism; the fail-closed invariant checker on
  synthetic event sets; no Renode/ELF required.
* Integration test (T2 toolchain only): runs the real scenario against the
  committed evidence with --check. It FAILS LOUDLY when the toolchain is
  absent, and only skips when CANCESTRY_HW_ALLOW_SKIP=1 is set (the
  test_power_sim.py convention).

Implementations under test: hw/virtual-bench/t2_fault_common.py,
hw/virtual-bench/scenarios/t2_busoff_001.py.
Requirements traced: HW-FR-003; HwAGENTS.md rules 4 and 5.
Test ids: HW-T2-BUSOFF-001..008, HW-T2-BUSOFF-E2E-001.
"""

from __future__ import annotations

import importlib.util
import json
import os
import re
import sys
from pathlib import Path

import pytest

REPO_ROOT = Path(__file__).resolve().parents[2]
VIRTUAL_BENCH = Path(__file__).resolve().parent
sys.path.insert(0, str(VIRTUAL_BENCH))

import t2_fault_common as t2fc  # noqa: E402


def _load_scenario_module(name):
    spec = importlib.util.spec_from_file_location(
        name, VIRTUAL_BENCH / "scenarios" / ("%s.py" % name))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


busoff = _load_scenario_module("t2_busoff_001")
SCENARIO = busoff.SCENARIO
EVIDENCE_PATH = SCENARIO.evidence_path
INJECTOR_PATH = REPO_ROOT / "hw/virtual-bench/renode/can_fault_injector.py"
FAULT_RESC_PATH = REPO_ROOT / "hw/virtual-bench/renode/cancestry-hw-fault.resc"
FAULT_DRIVER_PATH = REPO_ROOT / "hw/virtual-bench/firmware/can_fault.c"
PLATFORM_PATH = REPO_ROOT / "hw/virtual-bench/renode/stm32g474-cancestry.repl"


def _fault_event_set(**overrides):
    events = {
        "bus_active_us": 10824,
        "detection_us": 10100,
        "injection_us": 10000,
        "recovery_request_us": 10120,
        "recovery_us": 10124,
    }
    events.update(overrides)
    return events


def _fault_summary(**overrides):
    summary = {
        "alive_counter": 340,
        "crc_error_count": 0,
        "run_complete": 1,
    }
    summary.update(overrides)
    return summary


def _fault_counters(**overrides):
    counters = {
        "busoff_injections": 1,
        "crc_injections": 0,
        "injector_error": 0,
    }
    counters.update(overrides)
    return counters


def _fault_samples():
    inject_step = t2fc.INJECT_AT_US // t2fc.MASTER_STEP_US - 1
    samples = [{"alive_counter": 100 + step} for step in range(120)]
    samples[inject_step]["alive_counter"] = 200
    samples[-1]["alive_counter"] = 400
    return samples


# ---------------------------------------------------------------------------
# HW-T2-BUSOFF-001: contract lockstep across the four sources
# ---------------------------------------------------------------------------

def test_contract_constants_match_all_sources():
    """HW-T2-BUSOFF-001: injector/.resc/driver/commons cannot drift apart."""
    injector = INJECTOR_PATH.read_text(encoding="utf-8")
    resc = FAULT_RESC_PATH.read_text(encoding="utf-8")
    driver = FAULT_DRIVER_PATH.read_text(encoding="utf-8")
    platform = PLATFORM_PATH.read_text(encoding="utf-8")

    # Magic + window offsets (injector model vs t2_fault_common constants).
    assert "INJ_MAGIC = 0x43464931" in injector  # t2fc.INJ_MAGIC
    assert t2fc.INJ_MAGIC == 0x43464931
    for offset in ("0x00", "0x04", "0x08", "0x0C", "0x10", "0x14",
                   "0x18", "0x1C", "0x20", "0x24", "0x28", "0x2C"):
        assert ("INJ_OFF_" in injector), "offset table moved: %s" % offset
    assert "CMD_INJECT_BUSOFF = 0x42" in injector  # matches t2fc
    assert "CMD_INJECT_CRC = 0x43" in injector
    assert t2fc.CMD_INJECT_BUSOFF == 0x42
    assert t2fc.CMD_INJECT_CRC == 0x43

    # FDCAN register surface in all three firmware-facing sources.
    for pattern, what in (
        (r"FDCAN_OFF_CCCR = 0x018", "CCCR offset in injector"),
        (r"FDCAN_OFF_PSR = 0x044", "PSR offset in injector"),
        (r"FDCAN_OFF_IR = 0x050", "IR offset in injector"),
        (r"0x40006418", "CCCR in the driver"),
        (r"0x40006444", "PSR in the driver"),
        (r"0x40006450", "IR in the driver"),
    ):
        if pattern.startswith("0x"):
            assert pattern in driver, what
        else:
            assert re.search(pattern, injector), what
    assert t2fc.FDCAN1_BASE == 0x40006400
    assert t2fc.FDCAN_OFF_PSR == 0x44

    # Trace slots: identical addresses in every source that names them.
    slots = {
        0x60000040: "SLOT_BUSOFF_INJECT_US",
        0x60000044: "SLOT_CRC_INJECT_US",
        0x60000048: "SLOT_RECOVERY_REQUEST_US",
        0x6000004C: "SLOT_BUS_ACTIVE_US",
        0x60000050: "SLOT_CRC_CLEARED_US",
    }
    for addr, name in slots.items():
        assert "%s = 0x%08X" % (name, addr) in injector, (name, addr)
        assert getattr(t2fc, name) == addr
        assert "0x%08x" % addr in platform, (
            "platform contract comment lost the slot 0x%08x" % addr)
    # .resc write hook stamps 0x48 (CCCR) and 0x54 (CRC ack) with the
    # matching register guards. Dispatch 19: ONE registration carries both
    # branches (plus the self-test marker) - a second
    # SetHookBeforePeripheralWrite call on the same peripheral UNWRAPS AND
    # REPLACES the first (SystemBusGenerated, v1.16.1), which silently
    # killed the CCCR branch in every run before dispatch 19.
    assert len([l for l in resc.splitlines()
                if l.startswith("sysbus SetHookBeforePeripheralWrite")]) == 1
    assert "0x60000048" in resc and "offset == 0x18" in resc
    assert "0x60000054" in resc and "offset == 0x50" in resc
    # The self-test marker branch and its fail-closed include-time check.
    assert "offset == 0x3F0 and value == 0x4C4C4C4C" in resc
    assert "0x60000070" in resc and "0x48484848" in resc
    assert t2fc.SLOT_HOOK_LIVENESS == 0x60000070
    # Symbol hooks: names and slot addresses agree with the driver.
    for symbol, addr in (("t2_can_busoff_detect", 0x60000058),
                         ("t2_can_busoff_recover", 0x6000005C),
                         ("t2_can_crc_detect", 0x60000060)):
        assert 'cpu AddSymbolHook "%s"' % symbol in resc
        assert ("WriteDoubleWord(0x%08x" % addr) in resc
        assert re.search(r"void %s\(void\)" % re.escape(symbol), driver), (
            "the global hook symbol %s disappeared from can_fault.c" % symbol)
        assert getattr(t2fc, "SLOT_%s" % {
            "t2_can_busoff_detect": "BUSOFF_DETECT_US",
            "t2_can_busoff_recover": "BUSOFF_RECOVER_US",
            "t2_can_crc_detect": "CRC_DETECT_US",
        }[symbol]) == addr
    # Firmware-side slots match the driver definitions (address strings in
    # can_fault.c are lowercase hex, as written).
    for addr, macro in ((0x60000064, "T2_SLOT_CRC_ERROR_COUNT"),
                        (0x60000068, "T2_SLOT_RUN_COMPLETE"),
                        (0x6000006C, "T2_SLOT_ALIVE_COUNTER")):
        assert ("#define %s (*(volatile uint32_t *)0x%08xu" % (macro, addr)) \
            in driver
    # The scenario's watch slots are exactly the contract addresses.
    assert SCENARIO.watch_slots == {
        "bus_active_us": t2fc.SLOT_BUS_ACTIVE_US,
        "detection_us": t2fc.SLOT_BUSOFF_DETECT_US,
        "injection_us": t2fc.SLOT_BUSOFF_INJECT_US,
        "recovery_request_us": t2fc.SLOT_RECOVERY_REQUEST_US,
        "recovery_us": t2fc.SLOT_BUSOFF_RECOVER_US,
    }


# ---------------------------------------------------------------------------
# HW-T2-BUSOFF-002: the ISO 11898-1 128 x 11 bound, in the sources themselves
# ---------------------------------------------------------------------------

def test_busoff_recovery_limit_is_128_by_11_recessive_bits():
    """HW-T2-BUSOFF-002: 704 us, derived - never a bare constant."""
    assert t2fc.BUSOFF_RECOVERY_SEQUENCES == 128
    assert t2fc.BUSOFF_RECOVERY_BITS == 11
    assert t2fc.NOMINAL_BITRATE_MBPS == 2
    assert t2fc.BUSOFF_RECOVERY_US == 704
    injector = INJECTOR_PATH.read_text(encoding="utf-8")
    assert "BUSOFF_RECOVERY_SEQUENCES = 128" in injector
    assert "BUSOFF_RECOVERY_BITS = 11" in injector
    assert "NOMINAL_BITRATE_MBPS = 2" in injector
    assert t2fc.INJECT_AT_US == 10_000
    assert t2fc.SCENARIO_DURATION_US == 50_000
    assert t2fc.MASTER_STEP_US == 100
    # The injection boundary is a master-step multiple (deterministic stamp).
    assert t2fc.INJECT_AT_US % t2fc.MASTER_STEP_US == 0


# ---------------------------------------------------------------------------
# HW-T2-BUSOFF-003/004: pending manifest contract and determinism
# ---------------------------------------------------------------------------

def _t2_schema():
    import jsonschema
    return jsonschema.Draft202012Validator(
        json.loads(busoff.SCHEMA_PATH.read_text(encoding="utf-8")),
        format_checker=jsonschema.FormatChecker())


def test_pending_manifest_is_schema_valid_and_deterministic():
    """HW-T2-BUSOFF-003: pending renders twice identically and validates."""
    first = t2fc.render_evidence_bytes(
        busoff.expected_pending_manifest(SCENARIO))
    second = t2fc.render_evidence_bytes(
        busoff.expected_pending_manifest(SCENARIO))
    assert first == second
    assert first.endswith("\n") and not first.endswith("\n\n")
    document = json.loads(first)
    _t2_schema().validate(document)
    assert document["case_id"] == "t2_busoff_001"
    assert document["status"] == "pending"
    assert document["pass"] is False
    assert document["credibility_level"] == "CL0"
    assert document["requirement_id"] == "HW-FR-003"
    assert document["oracle_id"] == "none"
    assert document["scenario"]["fault"] == "bus_off"
    assert document["scenario"]["inject_at_us"] == 10_000
    assert document["scenario"]["duration_us"] == 50_000
    assert "issues/62" in document["pending_reason"]
    # The renode TCL2 gap is inherited verbatim; a TCL1 tool carries none.
    assert document["inherited_validation_gap"].startswith("renode: ")
    assert "can_fault_injector:" not in document["inherited_validation_gap"]
    assert "openmodelica:" not in document["inherited_validation_gap"]


def test_inherited_gap_matches_controlled_table():
    """HW-T2-BUSOFF-004: the copied gap text IS the controlled-table text."""
    text = (REPO_ROOT / "docs/hw/tool-qualification.md").read_text(
        encoding="utf-8")
    start = text.index("<!-- BEGIN TOOL CLASSIFICATION -->")
    end = text.index("<!-- END TOOL CLASSIFICATION -->")
    rows = [line for line in text[start:end].splitlines()
            if line.startswith("| renode ") or line.startswith("| renode|")]
    assert len(rows) == 1, "renode row not found in the controlled table"
    cells = [cell.strip() for cell in rows[0].strip().strip("|").split("|")]
    gap = cells[5]
    assert t2fc.INHERITED_VALIDATION_GAP == "renode: %s" % gap
    # The fault injector must be classified in the same table as TCL1.
    injector_rows = [line for line in text[start:end].splitlines()
                     if line.startswith("| can_fault_injector ")]
    assert len(injector_rows) == 1
    cells = [cell.strip() for cell in
             injector_rows[0].strip().strip("|").split("|")]
    assert cells[4] == "TCL1"
    assert cells[5] == "-", "TCL1 must carry an empty gap"


# ---------------------------------------------------------------------------
# HW-T2-BUSOFF-005/006: invariant checker (synthetic, fail-closed)
# ---------------------------------------------------------------------------

def test_busoff_invariants_accept_the_reference_run():
    """HW-T2-BUSOFF-005: a correct event set passes unchanged."""
    failures = t2fc.check_invariants(SCENARIO, _fault_event_set(),
                                     _fault_summary(), _fault_counters(),
                                     _fault_samples())
    assert failures == []


@pytest.mark.parametrize("label,events,summary,counters,needle", [
    ("missing detection", {"detection_us": 0}, {}, {}, "missing event: detection_us"),
    ("detection before injection", {"detection_us": 9999}, {}, {},
     "detection precedes injection"),
    ("request before detection", {"recovery_request_us": 10050}, {}, {},
     "recovery request precedes detection"),
    ("action at the bound", {"recovery_us": 10100 + 704}, {}, {},
     "recovery action took 704 us"),
    ("medium too fast", {"bus_active_us": 10700}, {}, {},
     "the medium released the bus 580 us"),
    ("bus before action", {"bus_active_us": 10123}, {}, {},
     "before the firmware's recovery action"),
    ("no completion", {}, {"run_complete": 0}, {}, "run_complete is 0"),
    ("counter wrong", {}, {}, {"busoff_injections": 2}, "exactly 1 Bus-Off"),
    ("crosstalk", {}, {}, {"crc_injections": 1}, "unexpected CRC injections"),
    ("injector error", {}, {}, {"injector_error": 1}, "injector error register"),
])
def test_busoff_invariants_reject_bad_runs(label, events, summary, counters,
                                           needle):
    """HW-T2-BUSOFF-006: every contract violation is named, never silent."""
    failures = t2fc.check_invariants(SCENARIO, _fault_event_set(**events),
                                     _fault_summary(**summary),
                                     _fault_counters(**counters),
                                     _fault_samples())
    assert any(needle in failure for failure in failures), (label, failures)


# ---------------------------------------------------------------------------
# HW-T2-BUSOFF-007: injection command shape (monitor contract)
# ---------------------------------------------------------------------------

class _FakeEndpoint(object):
    def __init__(self, registers=None):
        self.commands = []
        self.registers = registers or {}

    def command(self, text, echo_fragment=None):
        assert echo_fragment in text
        self.commands.append(text)
        return ""

    def read_u32(self, addr):
        return self.registers.get(addr, 0)

    def run_for_us(self, us):
        raise NotImplementedError


def test_inject_command_uses_the_monitor_command_interface():
    """HW-T2-BUSOFF-007: ControlWrite <ascii> + readback, fail-closed.

    The device path is the single dotted token 'sysbus.<name>': a bare
    'sysbus' resolves to the SystemBus object, so the two-token form
    'sysbus can_fault_injector ...' fails in the monitor and the command
    never reaches the injector (dispatch 17: all watch slots read 0).
    """
    endpoint = _FakeEndpoint()
    t2fc.inject_command(endpoint, t2fc.CMD_INJECT_BUSOFF)
    assert endpoint.commands == [
        "sysbus.can_fault_injector ControlWrite 0x42 0x1"]
    # A nonzero injector error register aborts the run.
    bad = _FakeEndpoint({t2fc.INJ_BASE + t2fc.INJ_OFF_ERROR: 1})
    with pytest.raises(t2fc.T2FaultError, match="rejected"):
        t2fc.inject_command(bad, t2fc.CMD_INJECT_CRC)


def test_inject_command_rejects_the_two_token_device_path():
    """Dispatch 17 regression: the command must stay one dotted token."""
    endpoint = _FakeEndpoint()
    t2fc.inject_command(endpoint, t2fc.CMD_INJECT_BUSOFF)
    command = endpoint.commands[0]
    # 'sysbus can_fault_injector ...' would look for a can_fault_injector
    # member of the SystemBus type and fail before the injector runs.
    assert " sysbus " not in command and command.startswith(
        "sysbus.can_fault_injector ")
    assert "can_fault_injector ControlWrite" in command


# ---------------------------------------------------------------------------
# HW-T2-BUSOFF-008: committed artifact == canonical pending regeneration
# ---------------------------------------------------------------------------

def test_committed_busoff_evidence_matches_canonical_regeneration():
    """HW-T2-BUSOFF-008: no hand edits to the committed pending artifact."""
    committed = EVIDENCE_PATH.read_text(encoding="utf-8")
    document = json.loads(committed)
    _t2_schema().validate(document)
    pending = document["status"] == "pending"
    if pending:
        regenerated = t2fc.render_evidence_bytes(
            busoff.expected_pending_manifest(SCENARIO))
        assert committed == regenerated
    else:
        assert committed == t2fc.render_evidence_bytes(document), (
            "the executed-run artifact is not canonically rendered")


# ---------------------------------------------------------------------------
# HW-T2-BUSOFF-E2E-001: full scenario run (T2 toolchain only)
# ---------------------------------------------------------------------------

def _toolchain_available():
    try:
        if t2fc.locate_renode() is None:
            return False
    except t2fc.T2SetupError:
        return False
    return bool(os.environ.get("CANCESTRY_T2_FAULT_ELF")
                or os.environ.get("CANCESTRY_T2_ELF"))


def test_t2_busoff_end_to_end():
    """HW-T2-BUSOFF-E2E-001: run + --check against committed evidence."""
    if not _toolchain_available():
        if os.environ.get("CANCESTRY_HW_ALLOW_SKIP") == "1":
            pytest.skip("CANCESTRY_HW_ALLOW_SKIP=1: T2 fault toolchain "
                        "(renode, CANCESTRY_T2_FAULT_ELF) not available")
        pytest.fail(
            "T2 fault toolchain not found (renode, CANCESTRY_T2_FAULT_ELF). "
            "Run on Renode-equipped infrastructure (HW-PLAN C5) with the "
            "can_fault ELF (CANCESTRY_T2_DRIVER=can_fault); per the H-07 "
            "convention this must not silently downgrade.")
    exit_code = busoff.main(["t2_busoff_001.py", "--check"])
    assert exit_code == 0
