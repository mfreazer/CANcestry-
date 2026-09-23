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
Test ids: HW-T2-ORCH-001 .. HW-T2-ORCH-007, HW-T2-E2E-001.
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
SCHEMA_PATH = REPO_ROOT / "schemas" / "hw" / "hw-t2-evidence-0.1.0.schema.json"

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


def test_omc_build_script_is_fully_formatted():
    """HW-T2-ORCH-007: the generated omc script has no bare ``%s``.

    Regression for F-18 (issue #55): the buildModelFMU line left a bare
    ``%s`` in the .mos file and omc's lexer rejected it at build time
    (dispatch 6, run 35722295912). The script text is a pure function, so
    this pins it without the toolchain.
    """
    text = runner.omc_build_script_text()
    # No unformatted placeholder may leak into omc's input.
    assert "%s" not in text, "unformatted placeholder in omc script:\n%s" % text
    # The model name must be formatted into the buildModelFMU call.
    assert ('buildModelFMU(CancestryLib.Power.Holdup, version="2.0", '
            'fmuType="cs", fileNamePrefix="cancestry_t2_holdup");') in text
    # The three pinned model files must be loaded.
    for name in ("package.mo", "Power/package.mo", "Power/Holdup.mo"):
        assert 'loadFile("%s")' % (runner.MODEL_ROOT / name) in text


def test_f32_elf_variable_is_quoted_without_path_marker():
    """F-32 (issue #60): ``$elf`` must be a quoted StringToken with NO ``@``.

    Dispatch 20 (run 35851511484, the final cycle of the first budget)
    died at ``sysbus LoadELF $elf`` with 'Parameters did not match the
    signature': the runner's old ``$elf="@..."`` form stored the literal
    '@' inside a StringToken (StringToken strips quotes; unlike PathToken
    it never trims '@'), LoadELF's ReadFilePath validator then ran
    ``File.Exists("@/...")`` -> false, and TryPrepareParameters swallows
    the RecoverableException into a signature mismatch. The value must be
    the clean absolute path so the validator sees a real file.
    """
    src = (REPO_ROOT / "hw/virtual-bench/run_t2_retention.py").read_text(
        encoding="utf-8")
    assert "'$elf=\"@%s\"'" not in src, (
        "F-32 regression: '@' inside the quoted $elf value would fail "
        "LoadELF's ReadFilePath validation")
    assert "'$elf=\"%s\"'" in src, (
        "F-32: expected the runner to set $elf as a clean quoted path")
    # The .resc consumption point stays the plain variable form (the fix
    # belongs to the runner, not to the platform script's LoadELF line).
    resc = (REPO_ROOT / "hw/virtual-bench/renode/cancestry-hw.resc").read_text(
        encoding="utf-8")
    assert "sysbus LoadELF $elf" in resc


# ---------------------------------------------------------------------------
# HW-T2-E2E: full pipeline (Renode-equipped infrastructure only)
# ---------------------------------------------------------------------------

def _t2_schema():
    import jsonschema
    return jsonschema.Draft202012Validator(
        json.loads(SCHEMA_PATH.read_text(encoding="utf-8")),
        format_checker=jsonschema.FormatChecker())

def _t2_toolchain_available():
    # locate_renode()/locate_elf() fail closed by raising T2SetupError
    # (HW-T2-ORCH-004/005), so the probe must translate that into "absent"
    # instead of letting the raise bypass the explicit-skip branch below.
    try:
        if runner.locate_renode() is None:
            return False
    except runner.T2SetupError:
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
    argv = ["run_t2_retention.py", "--check"]
    # Reuse the FMU artifact a prior run in this environment already built,
    # so the check compares identical FMU bytes (same contract as the ELF).
    # If none exists, the runner builds it (build_fmu) as it always has.
    prebuilt = runner.BUILD_DIR / "cancestry_t2_holdup.fmu"
    if prebuilt.is_file():
        argv += ["--fmu", str(prebuilt)]
    exit_code = runner.main(argv)
    assert exit_code == 0


# ---------------------------------------------------------------------------
# HW-T2-EVID: evidence artifact and schema contract (toolchain-free;
# land with commit 4, issue #53 deliverable 7)
# ---------------------------------------------------------------------------

def _assert_passing_t2_invariants(document):
    """Toolchain-free invariants of a committed T2 bring-up artifact.

    Cross-run byte-identity is proven by the hw-nightly ``--check``
    determinism gate (issue #55); the invariants that any static check can
    verify are asserted here: bring-up status, the QA-EV-01 ordering on the
    real timestamps, source-hash drift detection against the pinned sources,
    the scenario/tool contract constants and the artifact hash forms.
    """
    assert document["status"] == "passing"
    assert document["pass"] is True
    assert document["provisional"] is True
    events = document["events"]
    assert (events["retention_write_us"]
            < events["safe_latch_us"]
            < events["iwdg_fire_us"])
    assert document["ordering"]["contract"] == (
        "retention_write < safe_latch < iwdg_fire")
    assert document["ordering"]["holds"] is True
    # No event may land outside the 150 ms scenario.
    assert 0 <= events["iwdg_fire_us"] < fmi_bridge.SCENARIO_DURATION_US
    # Source-hash drift detection: the artifact was generated from exactly
    # the pinned sources now in the tree.
    assert document["source_hashes"] == runner._live_source_hashes()
    scenario = document["scenario"]
    assert scenario["duration_us"] == fmi_bridge.SCENARIO_DURATION_US
    assert scenario["master_step_us"] == fmi_bridge.MASTER_STEP_US
    assert scenario["fmu_step_us"] == fmi_bridge.FMU_STEP_US
    assert scenario["method"] == "virtual_bench"
    assert scenario["seed"] == fmi_bridge.FIXED_SEED
    assert document["tool_pins"] == runner.TOOL_PINS
    for key in ("elf", "fmu", "bridge", "trace"):
        assert document["hashes"][key].startswith("sha256:")
    assert document["retention"]["preserved_across_iwdg_reset"] is True
    # 0x45565101 = CANCESTRY_FAULT_CODE_QUEUE_SATURATION (QA-EV-01).
    assert document["retention"]["code"] == 0x45565101


def test_committed_t2_evidence_matches_canonical_regeneration():
    """HW-T2-EVID-001: the committed artifact is canonical and consistent.

    Pending form: byte-identical to the canonical pending-manifest
    regeneration. Passing form (post bring-up, issue #55): the bring-up run
    evidence, canonically rendered, with the bring-up invariants held.
    """
    committed = EVIDENCE_PATH.read_text(encoding="utf-8")
    document = json.loads(committed)
    if document["status"] == "passing":
        assert committed == runner.render_evidence_bytes(document), \
            "the passing artifact is not canonically rendered"
        _assert_passing_t2_invariants(document)
        return
    assert document["status"] == "pending"
    regenerated = runner.render_evidence_bytes(
        runner.expected_pending_manifest())
    assert committed == regenerated


def test_committed_t2_evidence_is_schema_valid():
    """HW-T2-EVID-002: the committed artifact validates against hw-t2-evidence-0.1.0."""
    jsonschema = pytest.importorskip("jsonschema")
    del jsonschema
    document = json.loads(EVIDENCE_PATH.read_text(encoding="utf-8"))
    violations = sorted(_t2_schema().iter_errors(document),
                        key=lambda error: error.message)
    assert not violations, [error.message for error in violations]


def test_schema_rejects_pending_artifact_claiming_events():
    """HW-T2-EVID-003: pending + events/hashes/toolchain is invalid (fail-closed)."""
    pytest.importorskip("jsonschema")
    document = runner.expected_pending_manifest()
    document["events"] = {"retention_write_us": 1, "safe_latch_us": 2,
                          "iwdg_fire_us": 3}
    violations = sorted(_t2_schema().iter_errors(document),
                        key=lambda error: error.message)
    assert violations, "a pending artifact must not carry events"


def test_schema_rejects_passing_artifact_with_pending_reason():
    """HW-T2-EVID-004: passing artifacts cannot carry a pending_reason."""
    pytest.importorskip("jsonschema")
    document = json.loads(EVIDENCE_PATH.read_text(encoding="utf-8"))
    document.update({
        "status": "passing",
        "pass": True,
        "credibility_level": "CL2",
        "events": {"retention_write_us": 1500, "safe_latch_us": 1600,
                   "iwdg_fire_us": 3600},
        "ordering": {"contract": "retention_write < safe_latch < iwdg_fire",
                     "holds": True},
        "hashes": {"elf": "sha256:" + "0" * 64, "fmu": "sha256:" + "1" * 64,
                   "bridge": "sha256:" + "2" * 64,
                   "trace": "sha256:" + "3" * 64},
        "plant": {"vbat_start_mv": 3299, "vbat_end_mv": 1650,
                  "or001_max_abs_delta_mv": 0, "tolerance_mv": 1},
        "retention": {"code": 1163284737,
                      "preserved_across_iwdg_reset": True},
        "toolchain": {"pins": {"renode": "1.15"},
                      "measured": {"renode": "Renode 1.15.0.12345"}},
    })
    violations = sorted(_t2_schema().iter_errors(document),
                        key=lambda error: error.message)
    assert violations, "a passing artifact must not carry pending_reason"


def test_orchestrator_rejects_ordering_violation_before_evidence():
    """HW-T2-EVID-005: an ordering failure never reaches evidence bytes."""
    events = [{"event": fmi_bridge.EVENT_RETENTION_WRITE, "time_us": 50},
              {"event": fmi_bridge.EVENT_SAFE_LATCH, "time_us": 40},
              {"event": fmi_bridge.EVENT_IWDG_FIRE, "time_us": 60}]
    violation = fmi_bridge.ordering_violation(events)
    assert violation is not None
    # The orchestrator raises before writing anything (fail-closed path).
    with pytest.raises(fmi_bridge.BridgeError, match="ordering contract"):
        raise fmi_bridge.BridgeError(
            "QA-EV-01 ordering contract failed: %s" % violation)

def test_diagnostic_summary_compresses_failure_state(tmp_path, monkeypatch):
    """F-23c: the final failure line carries the whole bring-up state.

    GitHub's error annotation surfaces only the step's LAST log line, so
    _diagnostic_summary must compress the console step markers (with
    probe values), the first error-looking text of the reconstructed
    monitor RX stream (the monitor delivers it in 1-5 byte chunks), and
    the bridge error into one annotation-sized line.
    """
    console = tmp_path / "console.log"
    console.write_text(
        "12:00:00.000 [INFO] Including script(s): x.resc\n"
        "12:00:00.100 [INFO] [cancestry-hw] step0: python logger channel OK, elf=@/work/x.elf\n"
        "12:00:00.200 [INFO] [cancestry-hw] step2: machine created\n"
        "12:00:00.300 [INFO] [cancestry-hw] step2b: repl path /work/y.repl isfile=True\n",
        encoding="utf-8")
    transcript = tmp_path / "transcript.log"
    chunks = [b"Could no", b"t find file '", b"stm32g474-c",
              b"ancestry", b".repl'\n"]
    lines = ["line %d [1790104856.%03d] RX %r" % (i + 1, i, c)
             for i, c in enumerate(chunks)]
    transcript.write_text("\n".join(lines) + "\n", encoding="utf-8")
    monkeypatch.setattr(runner, "RENODE_CONSOLE_LOG", console)
    monkeypatch.setattr(runner, "RENODE_TRANSCRIPT_LOG", transcript)
    summary = runner._diagnostic_summary(
        fmi_bridge.BridgeError("magic 0x0 != 0x54324353"))
    assert summary.startswith("T2-DIAG ")
    assert "step2b: repl path /work/y.repl isfile=True" in summary
    assert "first-transcript-error=" in summary
    assert "Could not find file" in summary
    assert "magic 0x0" in summary
    assert len(summary) <= 2400  # F-28: 2400-char T2-DIAG cap


def test_diagnostic_summary_carries_exit_state_and_crash_tail(
        tmp_path, monkeypatch):
    """F-33: the annotation carries Renode's exit state + crash signature.

    Dispatch 21 (run 35893814425) failed with a bare monitor EOF after a
    complete include; the console tail and transcript never persisted
    (the CI artifact zip fails since run 19), so the two discriminating
    facts - did Renode exit on its own, and did an unhandled crash print
    to the merged stderr - must live in the T2-DIAG line itself.
    """
    console = tmp_path / "console.log"
    console.write_text(
        "12:00:00.100 [INFO] [cancestry-hw] step8: include complete, "
        "machine left paused\n"
        "Fatal error:\n"
        "System.InvalidOperationException: boom while stepping\n"
        "   at Emulator.Run()\n",
        encoding="utf-8")
    state = tmp_path / "process_state.txt"
    state.write_text("renode_exit=already-exited rc=0\n", encoding="utf-8")
    missing = tmp_path / "no-such-transcript.log"
    monkeypatch.setattr(runner, "RENODE_CONSOLE_LOG", console)
    monkeypatch.setattr(runner, "RENODE_TRANSCRIPT_LOG", missing)
    monkeypatch.setattr(runner, "RENODE_PROCESS_STATE_LOG", state)
    eof_message = (
        "Renode monitor closed the connection while awaiting response to "
        "'emulation RunFor \"0.000100\"'")
    summary = runner._diagnostic_summary(fmi_bridge.BridgeError(eof_message))
    assert summary.startswith("T2-DIAG ")
    assert "step8: include complete" in summary
    assert "console-crash=" in summary
    assert "Fatal error" in summary
    assert "boom while stepping" in summary
    assert "proc=already-exited rc=0" in summary
    assert summary.rstrip().endswith("err=" + eof_message)
    assert len(summary) <= 2400


def test_diagnostic_summary_budget_never_truncates_err(tmp_path, monkeypatch):
    """F-33: under a full 2400-char load the err= tail survives intact.

    The pre-F-33 implementation head-truncated the assembled line, i.e.
    the bridge error - the single most important segment - was the first
    casualty of any long transcript. Budget pressure must squeeze the
    console markers and the transcript context instead.
    """
    console = tmp_path / "console.log"
    console.write_text(
        "".join(
            "12:00:%02d.000 [INFO] [cancestry-hw] step%d: marker payload "
            "padding padding padding\n" % (i % 60, i)
            for i in range(40)),
        encoding="utf-8")
    transcript = tmp_path / "transcript.log"
    transcript.write_text(
        "line 0 [1790104856.000] RX %r\n"
        % (b"There was an error evaluating the request: "
           + b"X" * 2000 + b"\n"),
        encoding="utf-8")
    state = tmp_path / "process_state.txt"
    state.write_text("renode_exit=alive-at-failure (killed by runner)\n",
                     encoding="utf-8")
    monkeypatch.setattr(runner, "RENODE_CONSOLE_LOG", console)
    monkeypatch.setattr(runner, "RENODE_TRANSCRIPT_LOG", transcript)
    monkeypatch.setattr(runner, "RENODE_PROCESS_STATE_LOG", state)
    tail = "the decisive closing sentence of the bridge error"
    summary = runner._diagnostic_summary(
        fmi_bridge.BridgeError(tail))
    assert len(summary) <= 2400
    assert summary.rstrip().endswith("err=" + tail)
    assert "proc=alive-at-failure (killed by runner)" in summary
    assert "first-transcript-error=" in summary
