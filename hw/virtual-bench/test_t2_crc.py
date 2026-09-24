#!/usr/bin/env python3
"""T2 orchestration tests: t2_crc_001 CRC error handling (H-10, issue #62).

Layers:

* Unit tests (always run): pending-manifest schema contract and determinism;
  the fail-closed invariant checker on synthetic event sets; the acknowledgment
  chain (detect -> raise -> acknowledge -> medium clears) on the contract
  constants; no Renode/ELF required.
* Integration test (T2 toolchain only): runs the real scenario against the
  committed evidence with --check. FAILS LOUDLY without the toolchain; skips
  only with CANCESTRY_HW_ALLOW_SKIP=1.

Implementations under test: hw/virtual-bench/t2_fault_common.py,
hw/virtual-bench/scenarios/t2_crc_001.py.
Requirements traced: HW-FR-003; HwAGENTS.md rules 4 and 5.
Test ids: HW-T2-CRC-001..006, HW-T2-CRC-E2E-001.
"""

from __future__ import annotations

import importlib.util
import json
import os
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


crc = _load_scenario_module("t2_crc_001")
SCENARIO = crc.SCENARIO
EVIDENCE_PATH = SCENARIO.evidence_path
FAULT_DRIVER_PATH = REPO_ROOT / "hw/virtual-bench/firmware/can_fault.c"


def _crc_event_set(**overrides):
    events = {
        "crc_ack_us": 10110,
        "crc_cleared_us": 10110,
        "detection_us": 10100,
        "injection_us": 10000,
    }
    events.update(overrides)
    return events


def _crc_summary(**overrides):
    summary = {
        "alive_counter": 340,
        "crc_error_count": 1,
        "run_complete": 1,
    }
    summary.update(overrides)
    return summary


def _crc_counters(**overrides):
    counters = {
        "busoff_injections": 0,
        "crc_injections": 1,
        "injector_error": 0,
    }
    counters.update(overrides)
    return counters


def _crc_samples():
    inject_step = t2fc.INJECT_AT_US // t2fc.MASTER_STEP_US - 1
    samples = [{"alive_counter": 100 + step} for step in range(120)]
    samples[inject_step]["alive_counter"] = 200
    samples[-1]["alive_counter"] = 400
    return samples


# ---------------------------------------------------------------------------
# HW-T2-CRC-001: scenario wiring on the contract
# ---------------------------------------------------------------------------

def test_crc_scenario_contract():
    """HW-T2-CRC-001: watch slots, fault code, trace columns are contracted."""
    assert SCENARIO.case_id == "t2_crc_001"
    assert SCENARIO.fault == "crc_error"
    assert SCENARIO.fault_command == t2fc.CMD_INJECT_CRC == 0x43
    assert SCENARIO.watch_slots == {
        "crc_ack_us": t2fc.SLOT_CRC_ACK_US,
        "crc_cleared_us": t2fc.SLOT_CRC_CLEARED_US,
        "detection_us": t2fc.SLOT_CRC_DETECT_US,
        "injection_us": t2fc.SLOT_CRC_INJECT_US,
    }
    assert SCENARIO.trace_columns == "time_us,bus_state,fdcan1_psr,crc_error_count"
    # The firmware maps a corrupted wire frame to the v1.0.0 normative code,
    # never to an invented "CRC error" code.
    driver = FAULT_DRIVER_PATH.read_text(encoding="utf-8")
    assert "CANCESTRY_HAL_FAULT_MALFORMED_FRAME" in driver
    assert "CANCESTRY_HAL_FAULT_CRC" not in driver


def test_crc_pending_manifest_is_schema_valid_and_deterministic():
    """HW-T2-CRC-002: pending renders twice identically and validates."""
    import jsonschema
    schema = jsonschema.Draft202012Validator(
        json.loads(crc.SCHEMA_PATH.read_text(encoding="utf-8")),
        format_checker=jsonschema.FormatChecker())
    first = t2fc.render_evidence_bytes(
        crc.expected_pending_manifest(SCENARIO))
    second = t2fc.render_evidence_bytes(
        crc.expected_pending_manifest(SCENARIO))
    assert first == second
    document = json.loads(first)
    schema.validate(document)
    assert document["case_id"] == "t2_crc_001"
    assert document["oracle_id"] == "none"
    assert document["credibility_level"] == "CL0"
    assert document["scenario"]["fault"] == "crc_error"
    assert document["scenario"]["inject_at_us"] == 10_000
    assert "issues/62" in document["pending_reason"]


# ---------------------------------------------------------------------------
# HW-T2-CRC-003/004: invariant checker (synthetic, fail-closed)
# ---------------------------------------------------------------------------

def test_crc_invariants_accept_the_reference_run():
    """HW-T2-CRC-003: a correct event set passes unchanged."""
    failures = t2fc.check_invariants(SCENARIO, _crc_event_set(),
                                     _crc_summary(), _crc_counters(),
                                     _crc_samples())
    assert failures == []


@pytest.mark.parametrize("label,events,summary,counters,needle", [
    ("missing detection", {"detection_us": 0}, {}, {}, "missing event: detection_us"),
    ("missing ack", {"crc_ack_us": 0}, {}, {}, "missing event: crc_ack_us"),
    ("ack before detection", {"crc_ack_us": 10050}, {}, {},
     "before detecting the error"),
    ("medium cleared late", {"crc_cleared_us": 10400}, {}, {},
     "CRC condition cleared at 10400 us"),
    ("counter not incremented", {}, {"crc_error_count": 0}, {},
     "the firmware CRC error counter is 0"),
    ("crash", {}, {"run_complete": 0}, {}, "run_complete is 0"),
    ("counter wrong", {}, {}, {"crc_injections": 2}, "exactly 1 CRC injection"),
    ("crosstalk", {}, {}, {"busoff_injections": 1},
     "unexpected Bus-Off injections"),
])
def test_crc_invariants_reject_bad_runs(label, events, summary, counters,
                                        needle):
    """HW-T2-CRC-004: every contract violation is named, never silent."""
    failures = t2fc.check_invariants(SCENARIO, _crc_event_set(**events),
                                     _crc_summary(**summary),
                                     _crc_counters(**counters),
                                     _crc_samples())
    assert any(needle in failure for failure in failures), (label, failures)


# ---------------------------------------------------------------------------
# HW-T2-CRC-005: committed artifact == canonical pending regeneration
# ---------------------------------------------------------------------------

def test_committed_crc_evidence_matches_canonical_regeneration():
    """HW-T2-CRC-005: no hand edits to the committed pending artifact."""
    committed = EVIDENCE_PATH.read_text(encoding="utf-8")
    document = json.loads(committed)
    if document["status"] == "pending":
        regenerated = t2fc.render_evidence_bytes(
            crc.expected_pending_manifest(SCENARIO))
        assert committed == regenerated
    else:
        assert committed == t2fc.render_evidence_bytes(document)


# ---------------------------------------------------------------------------
# HW-T2-CRC-E2E-001: full scenario run (T2 toolchain only)
# ---------------------------------------------------------------------------

def _toolchain_available():
    try:
        if t2fc.locate_renode() is None:
            return False
    except t2fc.T2SetupError:
        return False
    return bool(os.environ.get("CANCESTRY_T2_FAULT_ELF")
                or os.environ.get("CANCESTRY_T2_ELF"))


def test_t2_crc_end_to_end():
    """HW-T2-CRC-E2E-001: run + --check against committed evidence."""
    if not _toolchain_available():
        if os.environ.get("CANCESTRY_HW_ALLOW_SKIP") == "1":
            pytest.skip("CANCESTRY_HW_ALLOW_SKIP=1: T2 fault toolchain "
                        "(renode, CANCESTRY_T2_FAULT_ELF) not available")
        pytest.fail(
            "T2 fault toolchain not found (renode, CANCESTRY_T2_FAULT_ELF). "
            "Run on Renode-equipped infrastructure (HW-PLAN C5) with the "
            "can_fault ELF (CANCESTRY_T2_DRIVER=can_fault); per the H-07 "
            "convention this must not silently downgrade.")
    exit_code = crc.main(["t2_crc_001.py", "--check"])
    assert exit_code == 0
