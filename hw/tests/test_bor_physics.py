"""T1 brown-out-reset (BOR) model tests - H-11, issue #64.

Two layers, mirroring ``hw/tests/test_holdup_physics.py`` and
``hw/tests/test_power_sim.py``:

* Toolchain-free (always run): the BOR model equation set
  (``hw/model/CancestryLib/Power/BOR.mo``) is executed by a pure-Python
  reference implementation of exactly those equations. The reference is
  cross-checked against the model SOURCE (parameter defaults are parsed out of
  the ``.mo`` file, and the parameter set is pinned to the schema-validated BOM
  entries and to ``hw/tests/cases/bor_brownout_001.simcase.json``), so the two
  cannot drift. The retention-domain branch is additionally regressed against
  oracle OR-001 - the registered closed form (``v = V0 - I*ESR - I*t/C``) that
  serves HW-SF-002 / HW-SF-004.
* Toolchain-gated (``omc`` + FMPy): ``omc`` builds the
  ``CancestryLib.Power.BOR`` FMI 2.0 ModelExchange FMU and FMPy executes it
  through the guarded ZERO-STATE ModelExchange evaluator - the same execution
  path this repository qualifies for stateless sources
  (``hw/tests/test_pulse_sim.py`` / OR-002, ``docs/hw/h04-enforcement.md``:
  "OpenModelica 1.24 CS cannot step the zero-state source"). The same four
  behaviours are asserted from the FMU outputs. Like ``test_power_sim.py``
  this layer FAILS LOUDLY when the toolchain is absent and only skips when
  ``CANCESTRY_HW_ALLOW_SKIP=1`` is explicitly set for local development.

Honest posture (issue #64): the BOR threshold behaviour, the hysteresis release
gate and the reset timing have NO registered oracle. Nothing in this file
closes a requirement: the ledger row stays ``sim-pending`` / CL0 and the
brownout T2 artifact stays a pending manifest. OR-001 witnesses the
retention-domain closed form only.

Requirements traced: HW-SF-002 (sub-events (ii) and (iii)), HW-SF-004,
HW-FR-009; HwAGENTS.md rules 1, 2, 4 and 5.
Test ids: HW-PHYS-BOR-001 .. HW-PHYS-BOR-008, HW-SIM-BOR-001.
"""

from __future__ import annotations

import importlib.util
import json
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import Mock

import pytest

REPO_ROOT = Path(__file__).resolve().parents[2]
MODEL_PATH = REPO_ROOT / "hw" / "model" / "CancestryLib" / "Power" / "BOR.mo"
# The loaded package chain root: hw/model/CancestryLib (MODEL_PATH is
# its Power/BOR.mo).
MODEL_DIR = REPO_ROOT / "hw" / "model" / "CancestryLib"
SIM_CASE_PATH = REPO_ROOT / "hw" / "tests" / "cases" / "bor_brownout_001.simcase.json"
BOM_PATH = REPO_ROOT / "hw" / "bom" / "bom.json"
BOM_EXTRACT_PATHS = (
    REPO_ROOT / "hw" / "bom" / "datasheets" / "extract-holdup-cap.json",
    REPO_ROOT / "hw" / "bom" / "datasheets" / "extract-mcu-vbat.json",
)
ORACLE_PATH = REPO_ROOT / "hw" / "tests" / "oracles" / "or_001_holdup.py"
SCHEMA_DIR = REPO_ROOT / "schemas" / "hw"
BUILD_DIR = REPO_ROOT / "build" / "hw"

MODEL_NAME = "CancestryLib.Power.BOR"
CASE_ID = "bor_brownout_001"

# HwAGENTS.md rule 2: every simulated number cites a BOM/extract entry. This
# explicit map is the provenance link asserted by
# test_parameters_match_the_bom; it names the bom.json parameter entry each
# BOR.mo default is bound to.
MODEL_PARAM_TO_BOM = {
    "V_nominal": "V_main_nominal",
    "V_bor": "V_BOR3",
    "V0_vbat": "V_main_nominal",
    "I_mcu": "I_VBAT_bound",
    "I_leak": "I_leak_max",
    "ESR_vbat": "ESR_budget",
    "C_vbat": "C_nominal",
    "V_vbat_min": "V_VBAT_min",
    "R_path": "R_path",
}

# The two fixture parameters: deliberately NOT backed by a vendor source
# (documented in the model docstring and in the sim case's not_covered list).
FIXTURE_PARAMETERS = ("V_bor_hyst", "t_boot")
# Scenario constants fixed by issue #64 (not physical claims).
ISSUE_PARAMETERS = {"t_brownout": 0.01, "t_collapse": 100.0e-6}


# --------------------------------------------------------------------------
# Source and contract loading
# --------------------------------------------------------------------------

def _load_module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


def _json(path):
    with open(path, "r", encoding="utf-8") as handle:
        return json.load(handle)


def load_oracle():
    return _load_module("or_001_holdup", ORACLE_PATH)


def model_source():
    return MODEL_PATH.read_text(encoding="utf-8")


def model_parameters():
    """Parse the BOR.mo parameter declarations into {name: (value, unit)}.

    The defaults in the source are the single source of truth for the model's
    parameter values (HwAGENTS.md rule 2: they must equal the cited BOM
    entries); the sim case is asserted against them.
    """
    text = model_source()
    pattern = re.compile(
        r'parameter\s+Real\s+([A-Za-z_]\w*)\s*\(\s*unit\s*=\s*"([^"]*)"\s*\)'
        r'\s*=\s*([0-9eE.+-]+)\s*"([^"]*)"', re.S)
    parameters = {}
    for match in pattern.finditer(text):
        name, unit, value, comment = match.groups()
        assert name not in parameters, "duplicate parameter %s" % name
        parameters[name] = {"value": float(value), "unit": unit,
                            "comment": " ".join(comment.split())}
    assert parameters, "no parameter declarations parsed from %s" % MODEL_PATH
    return parameters


def load_sim_case():
    return _json(SIM_CASE_PATH)


def bom_parameter_map(bom):
    mapping = {}
    for part in bom["parts"]:
        for entry in part["parameters"]:
            name = entry["name"]
            assert name not in mapping, "BOM parameter %s is not unique" % name
            mapping[name] = (entry["value"], entry["condition"], part["part_id"])
    return mapping


# --------------------------------------------------------------------------
# Reference implementation of the BOR model equations
# --------------------------------------------------------------------------

class BorReference(object):
    """Pure-Python evaluation of the BOR.mo equation set (exact).

    Every method mirrors one equation of the model; the parameters default to
    the values parsed from the model source, so a parameter edit propagates
    here automatically.
    """

    def __init__(self, parameters=None):
        self.p = dict(parameters or
                      {name: entry["value"]
                       for name, entry in model_parameters().items()})

    # -- equations ---------------------------------------------------------
    def rail(self, time):
        """v (supply stimulus): nominal -> 0 V for t_collapse -> nominal."""
        p = self.p
        if time < p["t_brownout"]:
            return p["V_nominal"]
        if time < p["t_brownout"] + p["t_collapse"]:
            return 0.0
        return p["V_nominal"]

    def below_threshold(self, rail):
        return 1.0 if rail < self.p["V_bor"] else 0.0

    def release_condition(self, rail):
        return 1.0 if rail > self.p["V_bor"] + self.p["V_bor_hyst"] else 0.0

    def reset_state(self, time, rail=None):
        """(reset_asserted, nrst) - the hysteresis-gated reset latch."""
        rail = self.rail(time) if rail is None else rail
        t_assert = self.p["t_brownout"]
        t_release = self.p["t_brownout"] + self.p["t_collapse"]
        asserted = (time >= t_assert) and not (
            time >= t_release and self.release_condition(rail) > 0.5)
        asserted = 1.0 if asserted else 0.0
        return asserted, (0.0 if asserted > 0.5 else 1.0)

    def vbat(self, time):
        """Retention-domain node voltage (OR-001 closed form over the collapse)."""
        p = self.p
        load = p["I_mcu"] + p["I_leak"]
        t_assert = p["t_brownout"]
        t_release = p["t_brownout"] + p["t_collapse"]
        charged = p["V0_vbat"] - p["ESR_vbat"] * load
        if time < t_assert:
            return charged
        if time < t_release:
            return charged - load * (time - t_assert) / p["C_vbat"]
        return charged

    def retention_preserved(self, time):
        return 1.0 if self.vbat(time) > self.p["V_vbat_min"] else 0.0

    def sram_preserved(self, time):
        return 1.0 if time < self.p["t_brownout"] else 0.0

    def firmware_running(self, time):
        asserted, _ = self.reset_state(time)
        t_release = self.p["t_brownout"] + self.p["t_collapse"]
        if asserted > 0.5:
            return 0.0
        if time >= self.p["t_brownout"] and time < t_release + self.p["t_boot"]:
            return 0.0
        return 1.0

    def trace(self, times):
        times = list(times)
        return {
            "time": times,
            "v": [self.rail(t) for t in times],
            "nrst": [self.reset_state(t)[1] for t in times],
            "reset_asserted": [self.reset_state(t)[0] for t in times],
            "v_vbat": [self.vbat(t) for t in times],
            "retention_preserved": [self.retention_preserved(t) for t in times],
            "sram_preserved": [self.sram_preserved(t) for t in times],
            "firmware_running": [self.firmware_running(t) for t in times],
        }


def brownout_window_times():
    """A fixed trace grid: before, during and after the collapse."""
    ref = BorReference()
    p = ref.p
    return [
        0.0,
        p["t_brownout"] - 2.0e-6,
        p["t_brownout"],
        p["t_brownout"] + p["t_collapse"] / 2.0,
        p["t_brownout"] + p["t_collapse"],
        p["t_brownout"] + p["t_collapse"] + p["t_boot"],
        p["t_brownout"] + 10.0e-6 + p["t_collapse"],
    ]


# --------------------------------------------------------------------------
# HW-PHYS-BOR-001: model/source/contract lockstep
# --------------------------------------------------------------------------

def test_parameters_match_the_bom():
    """HW-PHYS-BOR-001: every BOR.mo default is a cited BOM value (rule 2)."""
    parameters = model_parameters()
    bom = bom_parameter_map(_json(BOM_PATH))
    for model_name, bom_name in MODEL_PARAM_TO_BOM.items():
        assert model_name in parameters, "BOR.mo lost parameter %s" % model_name
        assert bom_name in bom, ("bom.json lost the cited entry %s "
                                 "(BOR.mo %s)" % (bom_name, model_name))
        assert parameters[model_name]["value"] == bom[bom_name][0], (
            "BOR.mo %s = %r is not the cited %s = %r from %s"
            % (model_name, parameters[model_name]["value"], bom_name,
               bom[bom_name][0], bom[bom_name][2]))
        assert parameters[model_name]["unit"], model_name


def test_fixture_and_issue_parameters_are_declared():
    """HW-PHYS-BOR-002: fixtures are named as fixtures; issue constants fixed."""
    parameters = model_parameters()
    for name in FIXTURE_PARAMETERS:
        assert name in parameters, "fixture parameter %s is missing" % name
        assert "FIXTURE" in parameters[name]["comment"].upper(), (
            "%s must be declared an unqualified engineering fixture in its "
            "docstring (rule 2: no uncited number passes as a value)" % name)
    for name, value in ISSUE_PARAMETERS.items():
        assert parameters[name]["value"] == value, (
            "%s is an issue #64 scenario constant: model has %r, issue fixes %r"
            % (name, parameters[name]["value"], value))
    # The release gate only makes sense when the recovered rail clears the
    # hysteresis band (documented model precondition).
    assert parameters["V_nominal"]["value"] > (
        parameters["V_bor"]["value"] + parameters["V_bor_hyst"]["value"])


def test_sim_case_matches_the_model_and_its_schema():
    """HW-PHYS-BOR-003: the case is schema-valid and pins the model exactly."""
    jsonschema = pytest.importorskip("jsonschema")
    case = load_sim_case()
    schema = _json(SCHEMA_DIR / "hw-sim-0.1.0.schema.json")
    errors = sorted(jsonschema.Draft202012Validator(schema).iter_errors(case),
                    key=lambda error: error.message)
    assert not errors, "; ".join(error.message for error in errors)
    assert case["case_id"] == CASE_ID
    assert case["model"] == MODEL_NAME
    parameters = model_parameters()
    assert set(case["parameters"]) == set(parameters), (
        "the sim case and the model declare different parameter sets")
    for name, value in case["parameters"].items():
        assert value == parameters[name]["value"], (
            "sim case %s = %r but BOR.mo declares %r"
            % (name, value, parameters[name]["value"]))
    # The model really exists in the CancestryLib inventory (file-per-class).
    assert MODEL_PATH.is_file()
    assert "within CancestryLib.Power;" in model_source()
    assert re.search(r"\bmodel\s+BOR\b", model_source())
    assert re.search(r"\bend\s+BOR\s*;\s*$", model_source())


# --------------------------------------------------------------------------
# HW-PHYS-BOR-004: reset asserts at the BOR threshold
# --------------------------------------------------------------------------

def test_reset_asserts_when_the_rail_falls_below_the_threshold():
    """HW-PHYS-BOR-004: NRST is pulled low at the BOR level 3 crossing."""
    ref = BorReference()
    p = ref.p
    timestamps = brownout_window_times()
    trace = ref.trace(timestamps)
    # Above the threshold before the collapse: reset released, comparator 0.
    for index in (0, 1):
        assert trace["v"][index] == p["V_nominal"]
        assert ref.below_threshold(trace["v"][index]) == 0.0
        assert trace["nrst"][index] == 1.0
        assert trace["reset_asserted"][index] == 0.0
    # Once the rail is collapsed (collapse interval [t_assert, t_release))
    # the comparator is 1 and NRST is asserted.
    assert ref.below_threshold(0.0) == 1.0
    for index in (2, 3):
        assert trace["v"][index] == 0.0
        assert trace["nrst"][index] == 0.0
        assert trace["reset_asserted"][index] == 1.0
    # At the end of the collapse the rail is back at nominal: the comparator
    # clears, the release condition holds and NRST is released (the reset
    # release instant of HW-PHYS-BOR-005).
    assert trace["v"][4] == p["V_nominal"]
    assert ref.below_threshold(trace["v"][4]) == 0.0
    assert ref.release_condition(trace["v"][4]) == 1.0
    assert trace["nrst"][4] == 1.0
    # Crossing check: the modelled assertion instant is the instant the
    # stimulus crosses the threshold (the step collapse at t_brownout), and
    # the threshold value itself is the cited BOR level 3 value.
    assert p["V_bor"] == 2.8


# --------------------------------------------------------------------------
# HW-PHYS-BOR-005: release gate (threshold + hysteresis)
# --------------------------------------------------------------------------

def test_reset_releases_only_above_threshold_plus_hysteresis():
    """HW-PHYS-BOR-005: hysteresis is a release GATE, not decoration."""
    ref = BorReference()
    p = ref.p
    # Counterfactual: a rail that recovered INTO the hysteresis band
    # [V_bor, V_bor + V_bor_hyst] does not satisfy the release condition, so
    # the reset stays asserted past the collapse interval.
    band_voltage = p["V_bor"] + p["V_bor_hyst"] / 2.0
    assert ref.release_condition(band_voltage) == 0.0
    after_collapse = p["t_brownout"] + p["t_collapse"] * 2.0
    asserted, nrst = ref.reset_state(after_collapse, rail=band_voltage)
    assert asserted == 1.0 and nrst == 0.0
    # At the threshold itself release is still withheld (strictly greater).
    assert ref.release_condition(p["V_bor"]) == 0.0
    # Modelled stimulus: the rail returns to nominal, the release condition
    # reads true and the reset is released exactly at the end of the collapse.
    assert ref.release_condition(p["V_nominal"]) == 1.0
    assert ref.reset_state(p["t_brownout"] + p["t_collapse"] - 1.0e-9)[0] == 1.0
    assert ref.reset_state(p["t_brownout"] + p["t_collapse"]) == (0.0, 1.0)


# --------------------------------------------------------------------------
# HW-PHYS-BOR-006: retention domain preserved (OR-001 witnessed)
# --------------------------------------------------------------------------

def test_retention_domain_is_preserved_across_the_brownout():
    """HW-PHYS-BOR-006: VBAT stays above the floor; OR-001 closed form holds."""
    ref = BorReference()
    oracle = load_oracle()
    p = ref.p
    case = load_sim_case()
    tolerance = case["tolerances"]["vbat_max_abs_delta_v"]
    load = p["I_mcu"] + p["I_leak"]
    t_assert = p["t_brownout"]
    t_release = p["t_brownout"] + p["t_collapse"]
    # Dense grid across the collapse interval [t_assert, t_release): the
    # model discharges the node there and returns it to the charged value
    # when the rail recovers (the recharge transient R_path*C = 0.5 us is
    # below the 1 us plant step and is not resolved - documented reduction).
    samples = 1001
    worst = 0.0
    for step in range(samples):
        time = t_assert + (t_release - t_assert) * step / samples
        vbat = ref.vbat(time)
        # OR-001: the independent closed form, evaluated from the collapse
        # onset (the charge path is off for the whole interval).
        expected = oracle.vbat(time - t_assert, p["V0_vbat"], p["I_mcu"],
                               p["I_leak"], p["C_vbat"], p["ESR_vbat"])
        worst = max(worst, abs(vbat - expected))
        assert vbat > p["V_vbat_min"], (
            "retention node dropped to %r V at t=%r s (floor %r V)"
            % (vbat, time, p["V_vbat_min"]))
        assert ref.retention_preserved(time) == 1.0
    assert worst <= tolerance, ("retention branch differs from OR-001 by %r V "
                               "(tolerance %r)" % (worst, tolerance))
    # The node is returned to its charged value on rail recovery (the
    # reduced-model boundary) and the whole collapse costs millivolts.
    assert ref.vbat(t_release) == p["V0_vbat"] - p["ESR_vbat"] * load
    assert ref.vbat(t_release) > ref.vbat(t_release - 1.0e-9)
    # Margin: the modelled brownout removes millivolts, not volts.
    drop = p["V0_vbat"] - ref.vbat(t_release)
    assert p["V_vbat_min"] < ref.vbat(t_release)
    assert drop < 1.0e-3, "modelled retention drop %r V is unexpectedly large" % drop
    assert load == 1.7e-05


# --------------------------------------------------------------------------
# HW-PHYS-BOR-007: main SRAM lost, execution suspended, recovery time
# --------------------------------------------------------------------------

def test_main_sram_is_lost_and_firmware_resumes_after_the_recovery_time():
    """HW-PHYS-BOR-007: SRAM discarded, firmware suspended, t_boot resumption."""
    ref = BorReference()
    p = ref.p
    t_assert = p["t_brownout"]
    t_release = p["t_brownout"] + p["t_collapse"]
    # Main SRAM survives until the reset asserts and never returns.
    assert ref.sram_preserved(t_assert - 1.0e-9) == 1.0
    for offset in (0.0, 1.0e-6, p["t_collapse"], 1.0e-3):
        assert ref.sram_preserved(t_assert + offset) == 0.0
    # The retention domain is the ONLY surviving store at that instant.
    assert ref.retention_preserved(t_assert + 1.0e-6) == 1.0
    # Firmware execution: running before the reset, suspended while the reset
    # is asserted and while rebooting, running after the recovery time.
    assert ref.firmware_running(t_assert - 1.0e-9) == 1.0
    assert ref.firmware_running(t_assert) == 0.0
    assert ref.firmware_running(t_release) == 0.0
    assert ref.firmware_running(t_release + p["t_boot"] - 1.0e-9) == 0.0
    assert ref.firmware_running(t_release + p["t_boot"]) == 1.0
    # The modelled recovery time is the reset-deassertion -> resumption
    # latency of the model.
    assert p["t_boot"] == 100.0e-6


def test_model_is_algebraic_and_deterministic():
    """HW-PHYS-BOR-008: no states, no seeds, no wall clock (rule 5)."""
    source = model_source()
    assert "der(" not in source, (
        "the BOR fixture is deliberately algebraic (no states to integrate) - "
        "a der() would need a solver contract")
    assert "initial equation" not in source
    case = load_sim_case()
    assert case["seeds"] == [], "the BOR fixture must declare no seeds"
    assert case["solver"]["stop_s"] == 0.02
    assert case["solver"]["step_s"] == 1.0e-6
    for needle in ("time", "V_bor", "V_bor_hyst", "nrst"):
        assert needle in source


# --------------------------------------------------------------------------
# HW-SIM-BOR-001: the pinned OpenModelica toolchain compiles and runs it
# --------------------------------------------------------------------------

def _require_toolchain():
    if os.environ.get("CANCESTRY_HW_ALLOW_SKIP") == "1":
        pytest.skip("CANCESTRY_HW_ALLOW_SKIP=1: T1 FMU toolchain not required")
    if shutil.which("omc") is None:
        pytest.fail("omc (OpenModelica) is not on PATH: run inside the pinned "
                    "ci/docker toolchain image (or set "
                    "CANCESTRY_HW_ALLOW_SKIP=1 for local development)")
    try:
        import fmpy  # noqa: F401
    except ImportError as error:
        pytest.fail("FMPy is required for the BOR FMU check: %s" % error)


def bor_build_script_text():
    """Text of the omc script that builds the BOR FMU (pure; offline-testable).

    Shape kept in lockstep with the proven retention build (F-18): the package
    chain is loaded explicitly from its real paths, the model name is
    formatted into the buildModelFMU call and ``fileNamePrefix`` names the
    archive. The paths are asserted to exist WITHOUT omc by
    test_build_script_loads_existing_files - the first hw-fast dispatch of
    this file (run 36034555221) failed with "Failed to load package
    CancestryLib ... Class CancestryLib.Power.BOR not found in scope" because
    the chain pointed one directory too high; the offline guard makes that
    failure mode local instead of a red CI dispatch.

    ``fmuType="me"`` (not ``"cs"``): BOR.mo is a ZERO-STATE algebraic source
    (HW-PHYS-BOR-008 forbids ``der(``), and the pinned OpenModelica 1.24
    CoSimulation runtime cannot step a zero-state source - the finding
    recorded for the OR-002 pulse fixture in docs/hw/h04-enforcement.md and
    reproduced for this model by hw-fast dispatches 36035296703 (fmi2Error
    from ``fmi2Terminate``), 36036288651 and 36036948275 (fmi2Error from
    ``fmi2DoStep`` at the modelled collapse boundary t = 10 ms). The
    ModelExchange archive is executed by the guarded zero-state evaluator
    below, exactly like the pulse fixture; version stays "2.0" (bring-up
    finding F-2: the pinned OpenModelica cannot export FMI 3.0).
    """
    return (
        "loadModel(Modelica);\n"
        "getErrorString();\n"
        'loadFile("%s");\n' % (MODEL_DIR / "package.mo") +
        "getErrorString();\n"
        'loadFile("%s");\n' % (MODEL_DIR / "Power" / "package.mo") +
        "getErrorString();\n"
        'loadFile("%s");\n' % MODEL_PATH +
        "getErrorString();\n"
        'buildModelFMU(%s, version="2.0", fmuType="me", '
        'fileNamePrefix="cancestry_bor_brownout_001");\n' % MODEL_NAME +
        "getErrorString();\n")


def test_build_script_loads_existing_files():
    """HW-PHYS-BOR-009: every loadFile path of the FMU build exists."""
    script = bor_build_script_text()
    paths = re.findall(r'loadFile\("([^"]+)"\)', script)
    assert len(paths) == 3, paths
    for path in paths:
        assert Path(path).is_file(), (
            "%s is loaded by the omc build script but does not exist "
            "(the CancestryLib package chain must point at the real files)"
            % path)
    assert "buildModelFMU(CancestryLib.Power.BOR" in script
    assert 'fileNamePrefix="cancestry_bor_brownout_001"' in script
    # The execution path is part of the pinned build configuration: the
    # zero-state evaluator below requires a ModelExchange archive, and the
    # CoSimulation runtime of the pinned OpenModelica cannot step a
    # zero-state source (see bor_build_script_text).
    assert 'version="2.0"' in script and 'fmuType="me"' in script
    assert 'fmuType="cs"' not in script


def _build_bor_fmu():
    """Build the BOR FMU headless with omc; return (path, measured, build log)."""
    BUILD_DIR.mkdir(parents=True, exist_ok=True)
    script = BUILD_DIR / "omc_build_bor.mos"
    script.write_text(bor_build_script_text(), encoding="utf-8")
    result = subprocess.run(["omc", "--showErrorMessages", str(script)],
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            cwd=str(BUILD_DIR), timeout=900)
    output = result.stdout.decode("utf-8", "replace")
    assert result.returncode == 0, "omc failed:\n%s" % output
    matches = [name for name in re.findall(r'[\w.]+\.fmu', output)]
    assert matches, "omc reported no FMU name:\n%s" % output
    fmu = Path(matches[-1])
    if not fmu.is_absolute():
        fmu = BUILD_DIR / fmu
    assert fmu.is_file(), "omc did not produce %s" % fmu
    measured = {
        "omc": subprocess.run(["omc", "--version"], stdout=subprocess.PIPE,
                              timeout=120).stdout.decode("utf-8",
                                                         "replace").strip(),
    }
    return fmu, measured


BOR_FMU_OUTPUTS = (
    "v", "v_vbat", "nrst", "reset_asserted", "bor_below_threshold",
    "bor_release_condition", "retention_preserved", "sram_preserved",
    "firmware_running", "t_bor_assert", "t_bor_release", "t_recovery",
)
# The outputs sampled and compared against the reference implementation at
# every recorded microsecond of the sim-case grid.
BOR_SAMPLED_OUTPUTS = ("v", "nrst", "v_vbat", "retention_preserved",
                       "sram_preserved", "firmware_running")
# The bound the OR-002 pulse evaluator uses (hw/tests/test_pulse_sim.py):
# an event iteration that does not settle is a FAILURE, never a value.
MAX_EVENT_ITERATIONS = 100


def finish_event_iteration(slave):
    """Bound the FMI event iteration; a stalled/terminated FMU is never a pass.

    Same discipline as ``test_pulse_sim.finish_event_iteration``: the BOR
    source's relations are exact at the modelled boundaries (t_brownout,
    t_brownout + t_collapse, + t_boot), so a chattering relation must surface
    as a bounded-iteration failure instead of an arbitrary sampled value.
    """
    for _ in range(MAX_EVENT_ITERATIONS):
        needed, terminate, *_ = slave.newDiscreteStates()
        assert not terminate, "the BOR FMU requested premature termination"
        if not needed:
            return
    raise AssertionError(
        "the BOR FMU event iteration did not converge within %d iterations "
        "(chattering at a relation boundary is a fail-closed condition, not "
        "a value)" % MAX_EVENT_ITERATIONS)


def execute_bor(fmu, times_us, parameters=None):
    """Execute the stateless BOR FMU via FMI 2.0 ModelExchange/FMPy.

    Returns ``(samples, terminate_error)``: ``samples`` maps each requested
    integer microsecond to the dict of read outputs, and ``terminate_error``
    records the (non-value-carrying) ``fmi2Terminate`` outcome.

    Why ModelExchange and not CoSimulation ``doStep``
    -------------------------------------------------
    BOR.mo is a ZERO-STATE algebraic source (HW-PHYS-BOR-008 forbids
    ``der(``; the model is a fixture in the PulseISO7637_2 class). The pinned
    OpenModelica 1.24 CoSimulation runtime cannot step such a source - the
    finding this repository already recorded for the OR-002 pulse fixture
    (docs/hw/h04-enforcement.md: "OpenModelica 1.24 CS cannot step the
    zero-state source, so the evaluator explicitly requires zero continuous
    and discrete state variables and fails closed if that changes") and
    reproduced for this model by three hw-fast dispatches:

    * 36035296703 - the whole 1 us ``doStep`` loop returned values that
      matched the reference at every microsecond, then ``fmi2Terminate``
      returned fmi2Error (the runtime accepts terminate only in
      ``modelEventMode``/``modelContinuousTimeMode``:
      OMCompiler/SimulationRuntime/fmi/export/openmodelica/
      fmu2_model_interface.c, ``fmi2Terminate`` -> ``invalidState(...,
      modelEventMode|modelContinuousTimeMode, ~0)``);
    * 36036288651 - ``fmi2DoStep(0.01, 1e-6)`` returned fmi2Error at the
      modelled collapse boundary (that dispatch had debug logging on);
    * 36036948275 - the identical ``fmi2DoStep(0.01, 1e-6)`` returned
      fmi2Error with logging off, i.e. the CS status reporting at the
      boundary is not reproducible run to run.

    A status code that changes between identical dispatches cannot carry
    evidence (HwAGENTS.md rule 5), so this check does not consume one: the
    master sets the plant time explicitly and re-evaluates the event
    relations at each sample, exactly as the qualified pulse evaluator does.
    ModelExchange executes the SAME compiled equations - it is not a Python
    waveform substitute - and every recorded microsecond is still compared
    value by value against ``BorReference`` below.
    """
    import fmpy
    from fmpy.fmi2 import FMU2Model

    description = fmpy.read_model_description(str(fmu))
    # Fail closed on the interface BEFORE any extraction or native
    # instantiation (the N2 metadata discipline of test_pulse_sim.py).
    assert str(description.fmiVersion).startswith("2.0"), (
        "the pinned OpenModelica must export FMI 2.0 (bring-up finding F-2)")
    assert description.modelExchange is not None, (
        "the BOR FMU must support ModelExchange: the pinned OpenModelica CS "
        "runtime cannot step this zero-state source")
    assert description.numberOfContinuousStates == 0, (
        "the BOR evaluator requires a stateless FMU (HW-PHYS-BOR-008); a "
        "state would need a declared solver contract")
    assert not any(variable.variability == "discrete"
                   for variable in description.modelVariables), (
        "the BOR evaluator does not support discrete state variables")
    variables = {variable.name: variable.valueReference
                 for variable in description.modelVariables}
    for name in BOR_FMU_OUTPUTS:
        assert name in variables, "the BOR FMU does not export %s" % name
    params = dict(load_sim_case()["parameters"] if parameters is None
                  else parameters)
    assert set(params) <= set(variables), (
        "the FMU omitted declared sim-case parameters: %s"
        % sorted(set(params) - set(variables)))

    directory = fmpy.extract(str(fmu))
    slave = FMU2Model(guid=description.guid, unzipDirectory=directory,
                      modelIdentifier=description.modelExchange.modelIdentifier,
                      instanceName="bor_brownout_001")
    samples = {}
    terminate_error = None
    instantiated = False
    try:
        slave.instantiate()
        instantiated = True
        slave.setupExperiment(startTime=0.0,
                              stopTime=float(times_us[-1]) / 1e6)
        # Bind the executed FMU to the schema-validated sim-case parameter
        # set (the pulse evaluator's discipline): the defaults are asserted
        # equal to it offline, and setting them makes the executed
        # configuration explicit instead of implied.
        slave.setReal([variables[name] for name in sorted(params)],
                      [float(params[name]) for name in sorted(params)])
        slave.enterInitializationMode()
        slave.exitInitializationMode()
        finish_event_iteration(slave)
        slave.enterContinuousTimeMode()
        for time_us in times_us:
            slave.setTime(float(time_us) / 1e6)
            # Per-sample FMI 2.0 ModelExchange master order (the FMI spec's
            # ME step: the integrator completes, events settle, then the
            # master reads). In the pinned OpenModelica 1.24 runtime,
            # completedIntegratorStep re-evaluates the algebraic system at
            # the set time against the relations latched so far and stores
            # pre-values; the bounded fmi2NewDiscreteStates iteration then
            # settles the zero-crossing relations (and the derived chain,
            # which converges inside the iteration); the final getReal
            # re-evaluates from that settled state (the runtime's
            # need-update flag is still set). The model keeps every
            # boundary off the 1 us grid (BOR.mo: SUB-GRID BOUNDARY
            # PLACEMENT) because this runtime's zero-crossing hysteresis
            # holds the pre-crossing side at the exact crossing instant, so
            # no sample here is ever ambiguous; both CIS placements
            # (before or after the event block) were measured equivalent
            # on this stateless fixture.
            _, terminate = slave.completedIntegratorStep()
            assert not terminate, (
                "the BOR FMU requested premature termination at %d us"
                % time_us)
            slave.enterEventMode()
            finish_event_iteration(slave)
            slave.enterContinuousTimeMode()
            values = slave.getReal([variables[name]
                                    for name in BOR_SAMPLED_OUTPUTS])
            samples[time_us] = dict(zip(
                BOR_SAMPLED_OUTPUTS, (float(value) for value in values)))
        # Terminate LAST and record its outcome (never hide it). In the
        # ModelExchange path the FMU is in continuous-time mode here, which
        # is one of the two states the pinned runtime accepts terminate in;
        # the outcome is still non-value-carrying, because every claim below
        # comes from the sampled values.
        try:
            slave.terminate()
        except Exception as error:  # noqa: BLE001 - recorded, reported below
            terminate_error = error
        return samples, terminate_error
    finally:
        if instantiated:
            slave.freeInstance()
        shutil.rmtree(directory, ignore_errors=True)


def test_toolchain_builds_the_bor_fmu_and_reproduces_the_behaviours():
    """HW-SIM-BOR-001: omc -> FMI 2.0 ME FMU -> the same four behaviours."""
    _require_toolchain()

    ref = BorReference()
    p = ref.p
    fmu, measured = _build_bor_fmu()
    case = load_sim_case()
    step_us = int(case["solver"]["step_s"] * 1e6)
    stop_us = int(case["solver"]["stop_s"] * 1e6)
    # The full sim-case grid, inclusive of t = 0: every recorded microsecond
    # of the 20 ms window is compared against the reference implementation.
    times_us = list(range(0, stop_us + step_us, step_us))
    observed, terminate_error = execute_bor(fmu, times_us)
    assert sorted(observed) == times_us

    # round(): 0.01 + 0.0001 + 0.0001 is 0.010199999999999999 in binary64,
    # so a bare int(... * 1e6) would index the PREVIOUS sample (10199 us
    # instead of the resume instant 10200 us) and read the pre-resumption
    # value. The grid below is unchanged; this only names the correct sample.
    t_assert_us = int(round(p["t_brownout"] * 1e6))
    t_release_us = int(round((p["t_brownout"] + p["t_collapse"]) * 1e6))
    t_resume_us = int(round((p["t_brownout"] + p["t_collapse"] + p["t_boot"]) * 1e6))
    before = observed[t_assert_us - step_us]
    asserting = observed[t_assert_us]
    released = observed[t_release_us]
    resumed = observed[t_resume_us]
    # (a) threshold crossing: reset released before, asserted from the step in
    #     which the rail crossed the BOR level.
    assert before["v"] == p["V_nominal"] and before["nrst"] == 1.0
    assert asserting["v"] == 0.0
    assert asserting["nrst"] == 0.0
    # (b) release gate: released at the end of the collapse, and the retention
    #     node is still above the floor while it is asserted.
    assert released["nrst"] == 1.0
    assert asserting["v_vbat"] > p["V_vbat_min"]
    assert asserting["retention_preserved"] == 1.0
    # (c) main SRAM discarded by the reset; (d) resumption after t_boot.
    assert before["sram_preserved"] == 1.0
    assert asserting["sram_preserved"] == 0.0
    assert resumed["firmware_running"] == 1.0
    assert released["firmware_running"] == 0.0
    # The FMU trajectory tracks the reference implementation exactly, at
    # every sampled microsecond (tolerances from the schema-validated case).
    tolerance = case["tolerances"]["vbat_max_abs_delta_v"]
    for time_us in times_us:
        sample = observed[time_us]
        time = time_us / 1e6
        assert abs(sample["v"] - ref.rail(time)) <= 1e-9
        assert abs(sample["v_vbat"] - ref.vbat(time)) <= tolerance
        assert sample["nrst"] == ref.reset_state(time)[1]
        assert sample["sram_preserved"] == ref.sram_preserved(time)
        assert sample["retention_preserved"] == ref.retention_preserved(time)
        assert sample["firmware_running"] == ref.firmware_running(time)
    assert measured["omc"]
    print("HW-SIM-BOR-001: %d ModelExchange samples over %d us; terminate "
          "outcome: %s" % (len(observed), stop_us,
                           "ok" if terminate_error is None
                           else terminate_error))


# --------------------------------------------------------------------------
# Offline contract of the zero-state ModelExchange evaluator (no omc, no FMU)
# --------------------------------------------------------------------------
#
# These are metadata/sequencing fixtures in the N2 discipline of
# hw/tests/test_pulse_sim.py: they pin the evaluator's fail-closed guards and
# its FMI 2.0 call sequence. They are NOT FMU simulations and claim no
# physics - the physics is asserted by HW-PHYS-BOR-001..008 (reference
# implementation) and, on the pinned toolchain, by HW-SIM-BOR-001.

@pytest.mark.parametrize("needed,terminate,message", [
    (True, False, "did not converge"),
    (False, True, "premature termination"),
    (False, False, None),
])
def test_event_iteration_fails_closed(needed, terminate, message):
    """A stalled or self-terminating FMU is never a sampled value."""
    class FakeFmu(object):
        def newDiscreteStates(self):
            return needed, terminate, False, False, False, 0.0

    if message is None:
        finish_event_iteration(FakeFmu())
    else:
        with pytest.raises(AssertionError, match=message):
            finish_event_iteration(FakeFmu())


def _fake_description(supports_me=True, states=0, variability="continuous",
                      fmi_version="2.0", names=None, with_parameters=True):
    """A metadata fixture shaped like the real omc export.

    The declared outputs (``variability`` as parametrized) plus the sim-case
    parameters as ``fixed`` variables - the shape the evaluator's guards and
    its ``setReal`` parameter binding require.
    """
    names = list(names if names is not None else BOR_FMU_OUTPUTS)
    if with_parameters:
        names += sorted(load_sim_case()["parameters"])
    return SimpleNamespace(
        fmiVersion=fmi_version,
        guid="fixture-guid",
        modelExchange=(SimpleNamespace(modelIdentifier="fixture")
                       if supports_me else None),
        coSimulation=None,
        numberOfContinuousStates=states,
        modelVariables=[SimpleNamespace(name=name, valueReference=index,
                                        variability=variability)
                        for index, name in enumerate(names)])


@pytest.mark.parametrize("supports_me,states,variability,fmi_version,message", [
    (False, 0, "continuous", "2.0", "must support ModelExchange"),
    (True, 1, "continuous", "2.0", "requires a stateless FMU"),
    (True, -1, "continuous", "2.0", "requires a stateless FMU"),
    (True, None, "continuous", "2.0", "requires a stateless FMU"),
    (True, 0, "discrete", "2.0", "does not support discrete state variables"),
    (True, 0, "continuous", "3.0", "must export FMI 2.0"),
])
def test_bor_rejects_unsupported_metadata_before_native_execution(
        monkeypatch, supports_me, states, variability, fmi_version, message):
    """HW-SIM-BOR-002: the guards fire before extraction or native code."""
    fmpy = pytest.importorskip("fmpy")
    import fmpy.fmi2

    description = _fake_description(supports_me=supports_me, states=states,
                                    variability=variability,
                                    fmi_version=fmi_version)
    reader = Mock(return_value=description)
    extract = Mock(side_effect=AssertionError("extraction must not be reached"))
    native = Mock(side_effect=AssertionError("native execution must not be reached"))
    monkeypatch.setattr(fmpy, "read_model_description", reader)
    monkeypatch.setattr(fmpy, "extract", extract)
    monkeypatch.setattr(fmpy.fmi2, "FMU2Model", native)

    with pytest.raises(AssertionError, match=message):
        execute_bor("fixture.fmu", [0, 1])
    reader.assert_called_once_with("fixture.fmu")
    extract.assert_not_called()
    native.assert_not_called()


def test_missing_declared_output_is_rejected_before_native_execution(monkeypatch):
    """HW-SIM-BOR-003: an FMU that dropped a declared output fails closed."""
    fmpy = pytest.importorskip("fmpy")
    import fmpy.fmi2

    description = _fake_description(
        names=[name for name in BOR_FMU_OUTPUTS if name != "nrst"])
    extract = Mock(side_effect=AssertionError("extraction must not be reached"))
    native = Mock(side_effect=AssertionError("native execution must not be reached"))
    monkeypatch.setattr(fmpy, "read_model_description",
                        Mock(return_value=description))
    monkeypatch.setattr(fmpy, "extract", extract)
    monkeypatch.setattr(fmpy.fmi2, "FMU2Model", native)

    with pytest.raises(AssertionError, match="does not export nrst"):
        execute_bor("fixture.fmu", [0, 1])
    extract.assert_not_called()
    native.assert_not_called()


class _RecordingFmu(object):
    """A scripted FMI 2.0 ModelExchange slave: records calls, returns sentinels.

    Each declared output reads its own value reference as a sentinel, so the
    fixture can prove WHICH variable was read at WHICH instant without
    claiming any physics.
    """

    def __init__(self, **kwargs):
        self.calls = []
        self.kwargs = kwargs
        self.time = None
        self.reals = {}
        self.terminate_raises = False

    def _record(self, name, *args):
        self.calls.append((name,) + args)

    def instantiate(self):
        self._record("instantiate")

    def setupExperiment(self, **kwargs):
        self._record("setupExperiment", tuple(sorted(kwargs.items())))

    def setReal(self, references, values):
        self._record("setReal", tuple(references), tuple(values))
        self.reals.update(dict(zip(references, values)))

    def enterInitializationMode(self):
        self._record("enterInitializationMode")

    def exitInitializationMode(self):
        self._record("exitInitializationMode")

    def newDiscreteStates(self):
        self._record("newDiscreteStates")
        return False, False, False, False, False, 0.0

    def enterEventMode(self):
        self._record("enterEventMode")

    def enterContinuousTimeMode(self):
        self._record("enterContinuousTimeMode")

    def setTime(self, time):
        self.time = time
        self._record("setTime", time)

    def getReal(self, references):
        self._record("getReal", tuple(references))
        return [float(reference) for reference in references]

    def completedIntegratorStep(self):
        self._record("completedIntegratorStep")
        return False, False

    def terminate(self):
        self._record("terminate")
        if self.terminate_raises:
            raise RuntimeError("fmi2Terminate failed with status 3 (error)")

    def freeInstance(self):
        self._record("freeInstance")


def _patch_native(monkeypatch, slave):
    fmpy = pytest.importorskip("fmpy")
    import fmpy.fmi2

    monkeypatch.setattr(fmpy, "read_model_description",
                        Mock(return_value=_fake_description()))
    monkeypatch.setattr(fmpy, "extract", Mock(return_value="fixture-dir"))
    monkeypatch.setattr(fmpy.fmi2, "FMU2Model", lambda **kwargs: slave)
    monkeypatch.setattr(shutil, "rmtree", Mock())


def test_execute_bor_advances_time_and_re_evaluates_events_per_sample(monkeypatch):
    """HW-SIM-BOR-004: the evaluator's FMI sequence is the pinned contract."""
    slave = _RecordingFmu()
    _patch_native(monkeypatch, slave)

    times_us = [0, 9999, 10000, 10100, 20000]
    samples, terminate_error = execute_bor("fixture.fmu", times_us)

    assert terminate_error is None
    assert sorted(samples) == times_us
    # Every sample maps each declared output name to its own sentinel, i.e.
    # the read is by value reference and the mapping is not shuffled.
    for time_us in times_us:
        assert sorted(samples[time_us]) == sorted(BOR_SAMPLED_OUTPUTS)
    references = {variable.name: index
                  for index, variable in
                  enumerate(_fake_description().modelVariables)}
    assert samples[10000]["nrst"] == float(references["nrst"])

    names = [call[0] for call in slave.calls]
    # Setup: the sim-case parameters are bound BEFORE initialization.
    assert names[:5] == ["instantiate", "setupExperiment", "setReal",
                         "enterInitializationMode", "exitInitializationMode"]
    set_real = next(call for call in slave.calls if call[0] == "setReal")
    parameters = load_sim_case()["parameters"]
    assert len(set_real[1]) == len(parameters) == len(set_real[2])
    assert set(real for real in set_real[2]) == set(
        float(value) for value in parameters.values())
    # Initialization settles its events before the first sample is read.
    init_index = names.index("exitInitializationMode")
    assert names[init_index + 1:init_index + 3] == ["newDiscreteStates",
                                                    "enterContinuousTimeMode"]
    # Per sample: setTime -> completedIntegratorStep (the step's
    # zero-crossings are registered before the sample) -> event mode ->
    # bounded event iteration -> continuous-time mode -> getReal.
    per_sample = names[names.index("setTime"):]
    expected = []
    for _ in times_us:
        expected.extend(["setTime", "completedIntegratorStep", "enterEventMode",
                         "newDiscreteStates", "enterContinuousTimeMode",
                         "getReal"])
    assert per_sample[:len(expected)] == expected
    # The plant time is set from integer microseconds (determinism, rule 5).
    set_times = [call[1] for call in slave.calls if call[0] == "setTime"]
    assert set_times == [float(time_us) / 1e6 for time_us in times_us]
    assert names[-2:] == ["terminate", "freeInstance"]


def test_execute_bor_records_the_terminate_outcome_and_frees_the_instance(
        monkeypatch):
    """HW-SIM-BOR-005: terminate is non-value-carrying, never silently dropped."""
    slave = _RecordingFmu()
    slave.terminate_raises = True
    _patch_native(monkeypatch, slave)

    samples, terminate_error = execute_bor("fixture.fmu", [0, 10000])
    assert sorted(samples) == [0, 10000]
    assert terminate_error is not None and "status 3" in str(terminate_error)
    assert [call[0] for call in slave.calls][-2:] == ["terminate",
                                                      "freeInstance"]
