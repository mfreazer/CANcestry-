#!/usr/bin/env python3
"""T2 orchestration tests: HW-SF-002 retention verification (H-07, issue #53).

Layers:

* Unit tests (always run): deterministic-regeneration of the pending
  manifest, toolchain gating (fail-closed), evidence rendering discipline,
  and the scenario contract constants. No Renode/omc/FMPy required.
* Integration test (Renode-equipped infrastructure only): the full pipeline
  per test_power_sim.py's convention - it FAILS LOUDLY when the toolchain is
  absent, and only skips when ``CANCESTRY_HW_ALLOW_SKIP=1`` is explicitly set
  for local development.

Implementations under test: hw/virtual-bench/run_t2_retention.py,
hw/virtual-bench/fmi_bridge.py.
Requirements traced: HW-SF-002, HW-SF-004; HwAGENTS.md rules 4 and 5.
Test ids: HW-T2-ORCH-001 .. HW-T2-ORCH-006, HW-T2-E2E-001.
"""

from __future__ import annotations

import importlib.util
import json
import os
import shutil
import sys
from pathlib import Path

import pytest

REPO_ROOT = Path(__file__).resolve().parents[2]
VIRTUAL_BENCH = Path(__file__).resolve().parent
EVIDENCE_PATH = REPO_ROOT / "hw" / "tests" / "evidence" / "t2_retention_001.json"

sys.path.insert(0, str(VIRTUAL_BENCH))

import fmi_bridge  # noqa: E402
import run_t2_retention as runner  # noqa: E402


# ---------------------------------------------------------------------------
# HW-T2-ORCH: pending manifest, rendering and gating (toolchain-free)
# ---------------------------------------------------------------------------

def test_pending_manifest_regeneration_is_deterministic():
    """HW-T2-ORCH-001: rendering twice yields identical bytes."""
    first = runner.render_evidence_bytes(runner.expected_pending_manifest())
    second = runner.render_evidence_bytes(runner.expected_pending_manifest())
    assert first == second
    assert first.endswith("\n") and not first.endswith("\n\n")
    # Canonical formatting: sorted keys, 2-space indent (json.dumps contract).
    document = json.loads(first)
    assert document["case_id"] == "t2_retention_001"
    assert document["status"] == "pending"
    assert document["pass"] is False
    assert document["credibility_level"] == "CL0"
    assert document["requirement_id"] == "HW-SF-002"
    assert document["oracle_id"] == "OR-001"
    assert document["provisional"] is True
    assert "#41" not in first  # the deferral names THIS issue's scope


def test_pending_manifest_has_no_time_or_absolute_path_metadata():
    """HW-T2-ORCH-002: metadata stays portable and wall-clock-free."""
    raw = runner.render_evidence_bytes(runner.expected_pending_manifest())
    import re
    assert not re.search(r'"(?:timestamp|generated_at|created_at|'
                         r'updated_at)"\s*:', raw)
    document = json.loads(raw)

    def strings(value):
        if isinstance(value, dict):
            for child in value.values():
                yield from strings(child)
        elif isinstance(value, list):
            for child in value:
                yield from strings(child)
        elif isinstance(value, str):
            yield value

    for value in strings(document):
        assert not Path(value).is_absolute(), value


def test_pending_manifest_scenario_contract_matches_bridge():
    """HW-T2-ORCH-003: the manifest and the bridge agree on the timing."""
    document = runner.expected_pending_manifest()
    scenario = document["scenario"]
    assert scenario["master_step_us"] == fmi_bridge.MASTER_STEP_US == 100
    assert scenario["fmu_step_us"] == fmi_bridge.FMU_STEP_US == 1
    assert scenario["duration_us"] == fmi_bridge.SCENARIO_DURATION_US
    assert scenario["seed"] == fmi_bridge.FIXED_SEED
    # The seed is pinned in the platform script as well (lockstep test in
    # fmi_bridge_test.py covers the .resc side).


def test_locate_elf_fails_closed(monkeypatch):
    """HW-T2-ORCH-004: a missing ELF aborts before anything runs."""
    monkeypatch.delenv("CANCESTRY_T2_ELF", raising=False)
    with pytest.raises(runner.T2SetupError, match="CANCESTRY_T2_ELF"):
        runner.locate_elf()
    with pytest.raises(runner.T2SetupError, match="not found"):
        runner.locate_elf("/nonexistent/firmware.elf")


def test_locate_renode_fails_closed(monkeypatch):
    """HW-T2-ORCH-005: a missing Renode aborts before anything runs."""
    monkeypatch.delenv("CANCESTRY_RENODE", raising=False)
    monkeypatch.setattr(shutil, "which", lambda name: None)
    with pytest.raises(runner.T2SetupError, match="renode not found"):
        runner.locate_renode()


def test_pinned_sources_exist():
    """HW-T2-ORCH-006: every pinned source resolves (no dangling pins)."""
    for relative in runner.PINNED_SOURCES:
        assert (REPO_ROOT / relative).is_file(), relative


# ---------------------------------------------------------------------------
# HW-T2-E2E: full pipeline (Renode-equipped infrastructure only)
# ---------------------------------------------------------------------------

def _t2_toolchain_available():
    if runner.locate_renode() is None:
        return False
    if shutil.which("omc") is None:
        return False
    return bool(os.environ.get("CANCESTRY_T2_ELF"))


def test_t2_retention_end_to_end():
    """HW-T2-E2E-001: the full pipeline runs and reproduces the evidence.

    Fails loudly without the T2 toolchain (Renode + omc + the v1.0.0 ELF);
    set CANCESTRY_HW_ALLOW_SKIP=1 to skip explicitly for local development
    (the test_power_sim.py convention). A successful run must reproduce the
    committed evidence byte-for-byte (issue #53 determinism constraint).
    """
    if not _t2_toolchain_available():
        if os.environ.get("CANCESTRY_HW_ALLOW_SKIP") == "1":
            pytest.skip("CANCESTRY_HW_ALLOW_SKIP=1: T2 toolchain (renode, "
                        "omc, CANCESTRY_T2_ELF) not available locally")
        pytest.fail(
            "T2 toolchain not found (renode/omc/CANCESTRY_T2_ELF). Run this "
            "test on Renode-equipped infrastructure (HW-PLAN C5) with the "
            "v1.0.0 firmware ELF; per H-07 it must not silently downgrade to "
            "a partial check.")
    exit_code = runner.main(["run_t2_retention.py", "--check"])
    assert exit_code == 0
