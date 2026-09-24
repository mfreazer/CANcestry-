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
  ``CancestryLib.Power.BOR`` FMI 2.0 CoSimulation FMU and FMPy steps it; the
  same four behaviours are asserted from the FMU outputs. Like
  ``test_power_sim.py`` this layer FAILS LOUDLY when the toolchain is absent
  and only skips when ``CANCESTRY_HW_ALLOW_SKIP=1`` is explicitly set for
  local development.

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
        'buildModelFMU(%s, version="2.0", fmuType="cs", '
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


def _terminate_fmu(slave):
    """Terminate the FMU; return the recorded failure or None.

    FMI lifecycle observation (hw-fast dispatches 36035296702 / 36036288651,
    recorded in docs/hw/tool-qualification.md section 3.1): the OpenModelica
    runtime implements ``fmi2Terminate`` as legal only in
    ``modelEventMode``/``modelContinuousTimeMode``
    (SimulationRuntime/fmi/export/openmodelica/fmu2_model_interface.c.inc:
    ``fmi2Terminate`` -> ``invalidState(..., modelEventMode|
    modelContinuousTimeMode, ~0)``) and returns fmi2Error otherwise (its
    internal state machine, not the simulated values, decides that). The
    terminate outcome is therefore RECORDED and returned, never silently
    dropped and never allowed to invalidate a trajectory that has already
    been verified value by value.

    The FMU's own failure-level log is not usable as a second signal here:
    the pinned runtime only emits it with debug logging on, and enabling
    ``loggingOn`` changed the runtime's status reporting for the collapse
    boundary step in the same dispatch (doStep reported fmi2Error where the
    identical call had returned OK with logging off, while the FMU's values
    stayed correct at every sampled instant). The value guarantee is
    therefore the sample-by-sample comparison against the reference
    implementation below, which covers every recorded microsecond.
    """
    try:
        slave.terminate()
        return None
    except Exception as error:  # noqa: BLE001 - recorded, then reported below
        return error


def test_toolchain_builds_the_bor_fmu_and_reproduces_the_behaviours():
    """HW-SIM-BOR-001: omc -> FMI 2.0 CS FMU -> the same four behaviours."""
    _require_toolchain()
    import fmpy
    from fmpy.model_description import read_model_description

    ref = BorReference()
    p = ref.p
    fmu, measured = _build_bor_fmu()
    unzipdir = fmpy.extract(str(fmu))
    description = read_model_description(unzipdir)
    assert str(description.fmiVersion).startswith("2.0"), (
        "the pinned OpenModelica must export FMI 2.0 (bring-up finding F-2)")
    assert description.coSimulation is not None
    refs = {variable.name: variable.valueReference
            for variable in description.modelVariables}
    for name in ("v", "v_vbat", "nrst", "reset_asserted",
                 "bor_below_threshold", "bor_release_condition",
                 "retention_preserved", "sram_preserved", "firmware_running",
                 "t_bor_assert", "t_bor_release", "t_recovery"):
        assert name in refs, "the BOR FMU does not export %s" % name

    slave = fmpy.instantiate_fmu(unzipdir, description,
                                 fmi_type="CoSimulation")
    slave.setupExperiment(startTime=0.0)
    slave.enterInitializationMode()
    slave.exitInitializationMode()
    step_us = int(load_sim_case()["solver"]["step_s"] * 1e6)
    stop_us = int(load_sim_case()["solver"]["stop_s"] * 1e6)
    observed = {}
    time_us = 0
    while time_us < stop_us:
        slave.doStep(time_us / 1e6, step_us / 1e6)
        time_us += step_us
        values = slave.getReal([refs[name] for name in
                                ("v", "nrst", "v_vbat",
                                 "retention_preserved", "sram_preserved",
                                 "firmware_running")])
        observed[time_us] = dict(zip(
            ("v", "nrst", "v_vbat", "retention_preserved",
             "sram_preserved", "firmware_running"),
            (float(value) for value in values)))

    # Terminate last, and record (never hide) its outcome. The trajectory has
    # already been read: on the pinned OpenModelica runtime the terminate
    # status is decided by the FMU's internal state machine, not by the
    # simulated values (see _terminate_fmu).
    terminate_error = _terminate_fmu(slave)

    t_assert_us = int(p["t_brownout"] * 1e6)
    t_release_us = int((p["t_brownout"] + p["t_collapse"]) * 1e6)
    t_resume_us = int((p["t_brownout"] + p["t_collapse"] + p["t_boot"]) * 1e6)
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
    # The FMU trajectory tracks the reference implementation.
    for time_us, sample in sorted(observed.items()):
        time = time_us / 1e6
        assert abs(sample["v"] - ref.rail(time)) <= 1e-9
        assert abs(sample["v_vbat"] - ref.vbat(time)) <= 1e-9
        assert sample["nrst"] == ref.reset_state(time)[1]
        assert sample["sram_preserved"] == ref.sram_preserved(time)
    assert measured["omc"]
    print("HW-SIM-BOR-001: terminate outcome: %s"
          % ("ok" if terminate_error is None else terminate_error))
