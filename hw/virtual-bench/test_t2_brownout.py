#!/usr/bin/env python3
"""T2 orchestration tests: t2_brownout_001 brownout / BOR reset (H-11, #64).

Layers:

* Unit tests (always run): contract-constant lockstep across
  renode/bor_reset_injector.py, renode/cancestry-hw-bor.resc,
  renode/stm32g474-cancestry.repl and firmware/bor_brownout.c; the issue #64
  brownout constants (10 ms onset, 100 us NRST pulse, the ordering chain); the
  pending-manifest schema contract and determinism; the fail-closed invariant
  checker on synthetic event sets; the injection command shape; no
  Renode/ELF/FMU required.
* Integration test (T2 toolchain only): runs the real scenario against the
  committed evidence with --check. It FAILS LOUDLY when the toolchain is
  absent, and only skips when CANCESTRY_HW_ALLOW_SKIP=1 is set (the
  test_power_sim.py convention).

Implementations under test: hw/virtual-bench/t2_bor_common.py,
hw/virtual-bench/scenarios/t2_brownout_001.py.
Requirements traced: HW-SF-002, HW-SF-004; HwAGENTS.md rules 4, 5 and 6.
Test ids: HW-T2-BROWNOUT-001..011, HW-T2-BROWNOUT-E2E-001.
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

import t2_bor_common as t2bc  # noqa: E402


def _load_scenario_module(name):
    spec = importlib.util.spec_from_file_location(
        name, VIRTUAL_BENCH / "scenarios" / ("%s.py" % name))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


brownout = _load_scenario_module("t2_brownout_001")
SCENARIO = brownout.SCENARIO
EVIDENCE_PATH = SCENARIO.evidence_path
INJECTOR_PATH = REPO_ROOT / "hw/virtual-bench/renode/bor_reset_injector.py"
RESC_PATH = REPO_ROOT / "hw/virtual-bench/renode/cancestry-hw-bor.resc"
DRIVER_PATH = REPO_ROOT / "hw/virtual-bench/firmware/bor_brownout.c"
PLATFORM_PATH = REPO_ROOT / "hw/virtual-bench/renode/stm32g474-cancestry.repl"
BUILD_SCRIPT_PATH = REPO_ROOT / "hw/virtual-bench/firmware/build_firmware.sh"
MODEL_PATH = REPO_ROOT / "hw/model/CancestryLib/Power/BOR.mo"


def _t2_schema():
    import jsonschema
    schema = json.loads(t2bc.SCHEMA_PATH.read_text(encoding="utf-8"))
    return jsonschema.Draft202012Validator(schema)


def _bor_event_set(**overrides):
    events = {
        "bor_detect_us": 10300,
        "bor_inject_us": 10000,
        "bor_recover_us": 10400,
        "model_assert_us": 10000,
        "model_release_us": 10100,
        "nrst_release_us": 10100,
        "retention_write_us": 500,
    }
    events.update(overrides)
    return events


def _bor_summary(**overrides):
    summary = {
        "alive_at_inject": 120,
        "alive_counter": 480,
        "gpioa_odr": 0x0000,
        "retention_code": t2bc.FAULT_CODE,
        "retention_code_at_detect": t2bc.FAULT_CODE,
        "rcc_csr": t2bc.RCC_CSR_BORRSTF,
        "run_complete": 1,
        "sram_canary": 0,
        "sram_magic_at_boot": 0,
    }
    summary.update(overrides)
    return summary


def _bor_counters(**overrides):
    counters = {"injections": 1, "injector_error": 0, "sram_lost": 1}
    counters.update(overrides)
    return counters


def _bor_samples():
    steps = t2bc.SCENARIO_DURATION_US // t2bc.MASTER_STEP_US
    return [{"time_us": (index + 1) * t2bc.MASTER_STEP_US,
             "rail_mv": 3300, "nrst": 1, "gpio_odr": 0,
             "alive_counter": 120 + index} for index in range(steps)]


# ---------------------------------------------------------------------------
# HW-T2-BROWNOUT-001: contract lockstep across the four sources
# ---------------------------------------------------------------------------

def test_contract_constants_match_all_sources():
    """HW-T2-BROWNOUT-001: injector/.resc/driver/commons cannot drift apart."""
    injector = INJECTOR_PATH.read_text(encoding="utf-8")
    resc = RESC_PATH.read_text(encoding="utf-8")
    driver = DRIVER_PATH.read_text(encoding="utf-8")
    platform = PLATFORM_PATH.read_text(encoding="utf-8")

    # Magic, window offsets and commands (injector model vs commons).
    assert "BOR_INJ_MAGIC = 0x424F5231" in injector
    assert t2bc.BOR_INJ_MAGIC == 0x424F5231
    assert "BOR_INJ_MAGIC_VALUE 0x424F5231u /* \"BOR1\" */" in driver
    for offset, name in (
            (0x00, "BOR_OFF_MAGIC"), (0x04, "BOR_OFF_COMMAND"),
            (0x08, "BOR_OFF_NRST_LEVEL"), (0x0C, "BOR_OFF_INJECT_COUNT"),
            (0x10, "BOR_OFF_INJECT_US"), (0x14, "BOR_OFF_RELEASE_US"),
            (0x18, "BOR_OFF_SRAM_LOST"), (0x1C, "BOR_OFF_ERROR"),
            (0x20, "BOR_OFF_TIME_US")):
        assert ("%s = 0x%02X" % (name, offset)) in injector, name
        assert getattr(t2bc, name) == offset
    assert t2bc.BOR_CMD_INJECT_BROWNOUT == 0x42
    assert t2bc.NRST_PULSE_US == 100
    assert "NRST_PULSE_US = 100" in injector
    assert "NRST_RELEASED = 0x1" in injector
    assert "NRST_ASSERTED = 0x0" in injector
    assert t2bc.NRST_RELEASED == 0x1 and t2bc.NRST_ASSERTED == 0x0
    # The command status is PERSISTENT (trace-area state word), so the
    # orchestrator can read it back over the window fail-closed whatever the
    # outcome of the command was: every status branch must store it.
    assert injector.count("WriteDoubleWord(BOR_SLOT_ERROR") >= 3
    assert "BOR_SLOT_ERROR = 0x6000010C" in injector

    # Trace-area slots (commons vs injector vs .resc vs the .repl contract).
    for injector_name, commons_name, address in (
            ("BOR_SLOT_INJECT_US", "SLOT_BOR_INJECT_US", 0x60000070),
            ("BOR_SLOT_NRST_RELEASE_US", "SLOT_NRST_RELEASE_US", 0x60000074),
            ("BOR_SLOT_NRST_LEVEL", "SLOT_NRST_LEVEL", 0x60000104),
            ("BOR_SLOT_SRAM_LOST", "SLOT_SRAM_CANARY_LOST", 0x60000108)):
        assert getattr(t2bc, commons_name) == address, commons_name
        assert ("%s = 0x%08X" % (injector_name, address)) in injector, \
            injector_name
    assert t2bc.SLOT_BOR_INJECTOR_ERROR == 0x6000010C
    for slot in ("0x60000070", "0x60000074", "0x60000078", "0x6000007C",
                 "0x60000080", "0x60000084", "0x60000088", "0x6000008C",
                 "0x60000104", "0x60000108", "0x6000010C"):
        assert slot in resc, "the .resc must initialize %s" % slot
        assert slot.lower() in platform.lower(), (
            "the platform contract must document %s" % slot)
    assert t2bc.SLOT_RETENTION_WRITE_US == 0x60000008
    assert "0x60000008" in resc

    # Firmware-visible registers and the SRAM marker word (driver vs commons).
    assert "0x40001000u" in driver and "0x40001008u" in driver
    assert "0x20017000u" in driver
    assert t2bc.SRAM_CANARY_ADDR == 0x20017000
    assert t2bc.SRAM_CANARY_MAGIC == 0x5AA5C0DE
    assert "0x5AA5C0DEu" in driver
    assert "0x40021094u" in driver
    assert t2bc.RCC_CSR_ADDR == 0x40021094
    assert "1u << 27u" in driver
    assert t2bc.RCC_CSR_BORRSTF == 1 << 27
    assert t2bc.RCC_CSR_IWDGRSTF == 1 << 29
    # The retained code comes from the v1.0.0 header, never a driver literal
    # (the value may only appear in prose, without the C suffix).
    assert "0x45565101u" not in driver
    assert "CANCESTRY_FAULT_CODE_QUEUE_SATURATION" in driver
    assert t2bc.FAULT_CODE == 0x45565101

    # The driver selection is registered in the build script and the bring-up
    # script registers the injector at the issue's address with the pinned
    # quantum/seed discipline.
    build = BUILD_SCRIPT_PATH.read_text(encoding="utf-8")
    assert "bor_brownout)" in build
    assert "$DRIVER_DIR/bor_brownout.c" in build
    assert "0x40001000" in resc
    assert 'machine PyDevFromFile $ORIGIN/bor_reset_injector.py 0x40001000 0x400 true "bor_reset_injector"' in resc
    assert 'emulation SetGlobalQuantum "0.0001"' in resc
    assert "include @cancestry-hw-bor.resc" not in resc  # nothing self-includes
    assert "assert variables['elf'] not in ('', None)" in resc


# ---------------------------------------------------------------------------
# HW-T2-BROWNOUT-002: issue #64 constants and the scenario shape
# ---------------------------------------------------------------------------

def test_issue_constants_and_scenario_shape():
    """HW-T2-BROWNOUT-002: 10 ms onset, 100 us pulse, 50 ms window."""
    assert t2bc.BROWNOUT_AT_US == 10_000
    assert t2bc.NRST_PULSE_US == 100
    assert t2bc.SCENARIO_DURATION_US == 50_000
    assert t2bc.MASTER_STEP_US == 100 and t2bc.FMU_STEP_US == 1
    assert t2bc.SCENARIO_DURATION_US % t2bc.MASTER_STEP_US == 0
    assert t2bc.MASTER_STEP_US % t2bc.FMU_STEP_US == 0
    assert t2bc.BROWNOUT_AT_US % t2bc.MASTER_STEP_US == 0
    # The modelled BOR fixture uses the same onset, collapse and seed.
    model = MODEL_PATH.read_text(encoding="utf-8")
    assert 'parameter Real t_brownout(unit = "s") = 0.01' in model
    assert 'parameter Real t_collapse(unit = "s") = 100.0e-6' in model
    assert t2bc.FIXED_SEED == "123456789"
    assert t2bc.TRACE_COLUMNS == "time_us,rail_mv,nrst,gpio_odr,alive_counter"
    assert SCENARIO.case_id == "t2_brownout_001"
    assert SCENARIO.watch_slots == {
        "bor_detect_us": t2bc.SLOT_BOR_DETECT_US,
        "bor_inject_us": t2bc.SLOT_BOR_INJECT_US,
        "bor_recover_us": t2bc.SLOT_BOR_RECOVER_US,
        "nrst_release_us": t2bc.SLOT_NRST_RELEASE_US,
        "retention_write_us": t2bc.SLOT_RETENTION_WRITE_US,
    }
    assert t2bc.REQUIREMENT_ID == "HW-SF-002"
    assert t2bc.ORACLE_ID == "none"


# ---------------------------------------------------------------------------
# HW-T2-BROWNOUT-003: tool qualification lockstep
# ---------------------------------------------------------------------------

def test_tool_qualification_lockstep():
    """HW-T2-BROWNOUT-003: both TCL2 gaps and the TCL1 injector row agree."""
    text = (REPO_ROOT / "docs/hw/tool-qualification.md").read_text(
        encoding="utf-8")
    start = "<!-- BEGIN TOOL CLASSIFICATION -->"
    end = "<!-- END TOOL CLASSIFICATION -->"
    assert text.count(start) == 1 and text.count(end) == 1
    table = text.split(start)[1].split(end)[0]
    rows = {line.split("|")[1].strip(): line
            for line in table.splitlines() if line.startswith("| ")}
    openmodelica = [cell.strip() for cell in
                    rows["openmodelica"].strip().strip("|").split("|")]
    renode = [cell.strip() for cell in
              rows["renode"].strip().strip("|").split("|")]
    assert openmodelica[4] == "TCL2" and renode[4] == "TCL2"
    assert t2bc.INHERITED_VALIDATION_GAP == (
        "openmodelica: %s | renode: %s"
        % (openmodelica[5], renode[5]))
    injector = [cell.strip() for cell in
                rows["bor_reset_injector"].strip().strip("|").split("|")]
    assert injector[4] == "TCL1", "the BOR injector must be TCL1"
    assert injector[5] == "-", "TCL1 must carry an empty gap"
    # TCL1 producers contribute no gap to the inherited string.
    assert "bor_reset_injector:" not in t2bc.INHERITED_VALIDATION_GAP
    assert "fmi_bridge:" not in t2bc.INHERITED_VALIDATION_GAP
    assert t2bc.TOOL_PINS["bor_reset_injector"] == "0.1.0"


# ---------------------------------------------------------------------------
# HW-T2-BROWNOUT-004/005: invariant checker (synthetic, fail-closed)
# ---------------------------------------------------------------------------

def test_invariants_accept_the_reference_run():
    """HW-T2-BROWNOUT-004: a correct event set passes unchanged."""
    failures = t2bc.check_invariants(_bor_event_set(), _bor_summary(),
                                     _bor_counters(), _bor_samples())
    assert failures == []


@pytest.mark.parametrize("label,events,summary,counters,needle", [
    ("missing detection", {"bor_detect_us": 0}, {}, {},
     "missing event: bor_detect_us"),
    ("not at 10 ms", {"bor_inject_us": 9900, "model_assert_us": 9900,
                      "nrst_release_us": 10000}, {},
     {}, "expected exactly 10000 us"),
    ("model disagrees", {"model_assert_us": 9999}, {},
     {}, "must be driven by the model"),
    ("pulse too long", {"nrst_release_us": 10200}, {},
     {}, "NRST was asserted for 200 us"),
    ("model release off", {"model_release_us": 10250}, {},
     {}, "the modelled release gate opened at 10250 us"),
    ("retention after brownout", {"retention_write_us": 10100}, {},
     {}, "not before the brownout"),
    ("detection before injection", {"bor_detect_us": 9999}, {},
     {}, "does not follow the reset injection"),
    ("recovery before detection", {"bor_recover_us": 10200}, {},
     {}, "before the fault was detected"),
    ("code lost in recovery", {}, {"retention_code_at_detect": 0}, {},
     "did not preserve it across the reset"),
    ("code lost at the end", {}, {"retention_code": 0}, {},
     "must not disturb the RTC backup domain"),
    ("sram survived at boot", {}, {"sram_magic_at_boot": t2bc.SRAM_CANARY_MAGIC},
     {}, "does not preserve main SRAM"),
    ("sram rewritten", {}, {"sram_canary": t2bc.SRAM_CANARY_MAGIC}, {},
     "main-SRAM marker still reads"),
    ("no BOR flag", {}, {"rcc_csr": 0}, {}, "RCC_CSR.BORRSTF is clear"),
    ("iwdg flag set", {}, {"rcc_csr": t2bc.RCC_CSR_BORRSTF
                           | t2bc.RCC_CSR_IWDGRSTF}, {},
     "no IWDG reset belongs to this scenario"),
    ("no completion", {}, {"run_complete": 0}, {}, "run_complete is 0"),
    ("hung firmware", {}, {"alive_counter": 119}, {},
     "alive counter did not advance"),
    ("safe pins driven", {}, {"gpioa_odr": 1 << 9}, {},
     "safe-latch pins are not both de-energised"),
    ("injector error", {}, {}, {"injector_error": 2},
     "injector error register is 2"),
    ("two injections", {}, {}, {"injections": 2},
     "expected exactly 1 brownout injection"),
    ("sram flag clear", {}, {}, {"sram_lost": 0},
     "injector SRAM-lost flag is 0"),
])
def test_invariants_reject_bad_runs(label, events, summary, counters, needle):
    """HW-T2-BROWNOUT-005: every contract violation is named, never silent."""
    failures = t2bc.check_invariants(_bor_event_set(**events),
                                     _bor_summary(**summary),
                                     _bor_counters(**counters),
                                     _bor_samples())
    assert any(needle in failure for failure in failures), (label, failures)


# ---------------------------------------------------------------------------
# HW-T2-BROWNOUT-006: injection command shape (monitor contract)
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
    """HW-T2-BROWNOUT-006: ControlWrite 0x42 + readback, fail-closed."""
    endpoint = _FakeEndpoint()
    t2bc.inject_brownout(endpoint)
    assert endpoint.commands == [
        "sysbus bor_reset_injector ControlWrite 0x42 0x1"]
    # A rejected injection (already-injected error code) aborts the run.
    bad = _FakeEndpoint({t2bc.BOR_BASE + t2bc.BOR_OFF_ERROR:
                         t2bc.BOR_ERR_ALREADY_INJECTED})
    with pytest.raises(t2bc.T2BrownoutError, match="rejected"):
        t2bc.inject_brownout(bad)


def test_preflight_reads_the_contracted_initial_state():
    """HW-T2-BROWNOUT-007: preflight fails closed on any deviation."""
    registers = {t2bc.SLOT_MAGIC_ADDR: t2bc.SLOT_MAGIC,
                 t2bc.BOR_BASE + t2bc.BOR_OFF_MAGIC: t2bc.BOR_INJ_MAGIC,
                 t2bc.BOR_BASE + t2bc.BOR_OFF_NRST_LEVEL: t2bc.NRST_RELEASED,
                 t2bc.SLOT_NRST_LEVEL: t2bc.NRST_RELEASED}
    t2bc.preflight(_FakeEndpoint(dict(registers)), SCENARIO)
    broken = dict(registers)
    broken[t2bc.BOR_BASE + t2bc.BOR_OFF_NRST_LEVEL] = t2bc.NRST_ASSERTED
    with pytest.raises(t2bc.T2BrownoutError, match="initial NRST level"):
        t2bc.preflight(_FakeEndpoint(broken), SCENARIO)
    missing_magic = dict(registers)
    missing_magic[t2bc.SLOT_MAGIC_ADDR] = 0
    with pytest.raises(t2bc.T2BrownoutError, match="t2 trace magic"):
        t2bc.preflight(_FakeEndpoint(missing_magic), SCENARIO)


# ---------------------------------------------------------------------------
# HW-T2-BROWNOUT-008: the FMI build script is the pinned configuration
# ---------------------------------------------------------------------------

def test_omc_build_script_targets_the_bor_model_as_fmi2_cs():
    """HW-T2-BROWNOUT-008: FMI 2.0 CoSimulation only (bring-up finding F-2)."""
    script = t2bc.omc_build_script_text()
    assert "CancestryLib.Power.BOR" in script
    assert 'version="2.0"' in script and 'fmuType="cs"' in script
    assert 'fileNamePrefix="cancestry_t2_bor"' in script, (
        "the prebuilt FMU path passed by the --check twin follows "
        "fileNamePrefix; it must stay pinned")
    assert "BOR.mo" in script
    # The proven retention build shape: package chain loaded explicitly, and
    # every loaded path must exist (the hw-fast dispatch of the sibling T1
    # check failed once on a wrong package path - assert it offline here).
    assert script.count("loadFile(") == 3
    assert "loadModel(Modelica)" in script
    for path in re.findall(r'loadFile\("([^"]+)"\)', script):
        assert Path(path).is_file(), path
    assert t2bc.MODEL_PATH.is_file()
    assert t2bc.SIM_CASE_PATH.is_file()
    case = json.loads(t2bc.SIM_CASE_PATH.read_text(encoding="utf-8"))
    assert case["model"] == "CancestryLib.Power.BOR"
    assert case["parameters"]["t_brownout"] == t2bc.BROWNOUT_AT_US / 1e6
    assert case["parameters"]["t_collapse"] == t2bc.NRST_PULSE_US / 1e6
    assert case["oracle_id"] == "OR-001"


# ---------------------------------------------------------------------------
# HW-T2-BROWNOUT-009: pending manifest schema + determinism
# ---------------------------------------------------------------------------

def test_pending_manifest_is_schema_valid_and_deterministic():
    """HW-T2-BROWNOUT-009: canonical regeneration, honest pending form."""
    validator = _t2_schema()
    document = brownout.expected_pending_manifest(SCENARIO)
    validator.validate(document)
    assert document["status"] == "pending"
    assert document["pass"] is False
    assert document["credibility_level"] == "CL0"
    assert document["oracle_id"] == "none"
    assert document["provisional"] is True
    assert "issues/64" in document["pending_reason"]
    assert document["sim_case"] == "hw/tests/cases/bor_brownout_001.simcase.json"
    assert document["inherited_validation_gap"] == t2bc.INHERITED_VALIDATION_GAP
    assert document["tool_pins"] == t2bc.TOOL_PINS
    assert "hashes" not in document and "events" not in document
    first = t2bc.render_evidence_bytes(document)
    second = t2bc.render_evidence_bytes(
        brownout.expected_pending_manifest(SCENARIO))
    assert first == second
    # Every pinned source exists and is pinned by a live hash.
    for relative, digest in document["source_hashes"].items():
        assert (REPO_ROOT / relative).is_file(), relative
        assert t2bc.sha256_file(REPO_ROOT / relative) == digest, relative
    assert "hw/model/CancestryLib/Power/BOR.mo" in document["source_hashes"]
    assert "hw/virtual-bench/renode/bor_reset_injector.py" in \
        document["source_hashes"]


def test_passing_form_carries_the_issue_hash_chain_and_invariants():
    """HW-T2-BROWNOUT-010: a passing run pins FMU+ELF+bridge+injector+trace."""
    validator = _t2_schema()
    runlog = {
        "bridge_sha256": "sha256:" + "a" * 64,
        "elf_sha256": "sha256:" + "b" * 64,
        "fmu_sha256": "sha256:" + "c" * 64,
        "injector_sha256": "sha256:" + "d" * 64,
        "trace_sha256": "sha256:" + "e" * 64,
        "measured_tools": {"renode": "renode v1.0.0.0\n  build: 1.16.1"},
    }
    document = t2bc.passing_document(SCENARIO, _bor_event_set(),
                                     _bor_summary(), runlog)
    validator.validate(document)
    assert document["status"] == "passing" and document["pass"] is True
    assert document["credibility_level"] == "CL0"  # issue #64: no oracle
    assert set(document["hashes"]) == {"bridge", "elf", "fmu", "injector",
                                       "trace"}
    assert document["invariants"]["retention_before_brownout"] is True
    assert document["invariants"]["detection_before_recovery"] is True
    assert document["invariants"]["nrst_pulse_us"] == t2bc.NRST_PULSE_US
    assert "pending_reason" not in document


# ---------------------------------------------------------------------------
# HW-T2-BROWNOUT-011: committed artifact == canonical regeneration
# ---------------------------------------------------------------------------

def test_committed_evidence_matches_canonical_regeneration():
    """HW-T2-BROWNOUT-011: no hand edits to the committed pending artifact."""
    committed = EVIDENCE_PATH.read_text(encoding="utf-8")
    document = json.loads(committed)
    _t2_schema().validate(document)
    if document["status"] == "pending":
        regenerated = t2bc.render_evidence_bytes(
            brownout.expected_pending_manifest(SCENARIO))
        assert committed == regenerated, (
            "the committed pending manifest is not the canonical "
            "regeneration; do not hand-edit evidence")
    else:
        assert committed == t2bc.render_evidence_bytes(document), (
            "the executed-run artifact is not canonically rendered")


# ---------------------------------------------------------------------------
# HW-T2-BROWNOUT-E2E-001: full scenario run (T2 toolchain only)
# ---------------------------------------------------------------------------

def _toolchain_available():
    try:
        if t2bc.locate_renode() is None:
            return False
    except t2bc.T2SetupError:
        return False
    return bool(os.environ.get("CANCESTRY_T2_BOR_ELF")
                or os.environ.get("CANCESTRY_T2_ELF"))


def test_t2_brownout_end_to_end():
    """HW-T2-BROWNOUT-E2E-001: run + --check against committed evidence."""
    if not _toolchain_available():
        if os.environ.get("CANCESTRY_HW_ALLOW_SKIP") == "1":
            pytest.skip("CANCESTRY_HW_ALLOW_SKIP=1: T2 brownout toolchain "
                        "(renode, CANCESTRY_T2_BOR_ELF) not available")
        pytest.fail(
            "T2 brownout toolchain not found (renode, CANCESTRY_T2_BOR_ELF). "
            "Run on Renode-equipped infrastructure (HW-PLAN C5) with the "
            "bor_brownout ELF (CANCESTRY_T2_DRIVER=bor_brownout); per the "
            "H-07 convention this must not silently downgrade.")
    exit_code = brownout.main(["t2_brownout_001.py", "--check"])
    assert exit_code == 0
