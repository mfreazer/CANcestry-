"""T1 hold-up simulation test for HW-SF-002 (H-01, issue #33).

Test-first per the H-01 constraint: this file, the sim case
``hw/tests/cases/holdup_001.simcase.json`` and the oracle
``hw/tests/oracles/or_001_holdup.py`` (OR-001) were committed *before*
the model ``hw/model/CancestryLib/Power/Holdup.mo`` (red), and go green
only against that model (green) in the digest-pinned toolchain image
(``ci/docker/``, ``.github/workflows/hw-fast.yml``).

Pipeline (HW-PLAN section 7, C5): build the FMU headless via ``omc``,
execute it with FMPy, assert VBAT(t) against OR-001 within the
sim-case-declared tolerance, and verify the committed evidence artifact
(``hw/tests/evidence/holdup_001.json``) - tool digests, pinned sources,
trace hash. Per H-01: if the pipeline cannot run in this environment the
test FAILS LOUDLY (or skips only when ``CANCESTRY_HW_ALLOW_SKIP=1`` is
explicitly set for local development); it never downgrades to a
compile-only check.

Requirements traced: HW-SF-002, HW-FR-009.
Test ids: HW-SIM-HOLDUP-001 .. HW-SIM-HOLDUP-005.
"""

from __future__ import annotations

import hashlib
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
MODEL_ROOT = REPO_ROOT / "hw" / "model" / "CancestryLib"
SIM_CASE_PATH = REPO_ROOT / "hw" / "tests" / "cases" / "holdup_001.simcase.json"
BOM_PATH = REPO_ROOT / "hw" / "bom" / "bom.json"
SCHEMA_DIR = REPO_ROOT / "schemas" / "hw"
EVIDENCE_DIR = REPO_ROOT / "hw" / "tests" / "evidence"
ORACLE_PATH = REPO_ROOT / "hw" / "tests" / "oracles" / "or_001_holdup.py"
BUILD_DIR = REPO_ROOT / "build" / "hw"

FMI_TYPES = ("cs", "me_cs", "me")


# --------------------------------------------------------------------------
# Loading and validation helpers
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


def sha256_file(path):
    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        for chunk in iter(lambda: handle.read(65536), b""):
            digest.update(chunk)
    return "sha256:%s" % digest.hexdigest()


def _validate_against_schema(document, schema_name, what):
    import jsonschema

    schema = _json(SCHEMA_DIR / schema_name)
    errors = sorted(
        jsonschema.Draft202012Validator(schema).iter_errors(document),
        key=lambda error: error.message)
    problems = ["%s: %s" % (what, error.message) for error in errors]
    if problems:
        raise AssertionError("; ".join(problems))


def load_sim_case():
    return _json(SIM_CASE_PATH)


def load_bom():
    return _json(BOM_PATH)


# Explicit link between the model/sim-case parameter names (Holdup.mo)
# and the cited BOM entries (hw/bom/bom.json). HwAGENTS.md rule 2: no
# simulated number without a cited extract - this mapping, asserted in
# test_sim_case_parameters_match_bom, is the CL2 provenance link.
MODEL_PARAM_TO_BOM = {
    "C": "C_nominal",
    "ESR": "ESR_budget",
    "I_mcu": "I_VBAT_bound",
    "I_leak": "I_leak_max",
    "R_path": "R_path",
    "V0": "V_main_nominal",
    "VIN_brownout": "V_BOR3",
    "V_floor": "V_VBAT_min",
}


def bom_parameter_map(bom):
    """{parameter name: (value, part_id)} across all parts (names unique)."""
    mapping = {}
    for part in bom["parts"]:
        for entry in part["parameters"]:
            name = entry["name"]
            if name in mapping:
                raise AssertionError(
                    "parameter %s is cited by more than one part" % name)
            mapping[name] = (entry["value"], part["part_id"])
    return mapping


def load_oracle():
    return _load_module("or_001_holdup", ORACLE_PATH)


# --------------------------------------------------------------------------
# Toolchain (digest-pinned image: OpenModelica + FMPy + Python)
# --------------------------------------------------------------------------

def _require_toolchain():
    """Fail loudly when the toolchain image is not active (H-01 rule)."""
    if os.environ.get("CANCESTRY_HW_ALLOW_SKIP") == "1":
        pytest.skip("CANCESTRY_HW_ALLOW_SKIP=1: local development without "
                    "the ci/docker toolchain image")
    omc = shutil.which("omc")
    if omc is None:
        pytest.fail(
            "omc (OpenModelica) not found on PATH. Run this test inside the "
            "digest-pinned toolchain image (ci/docker/, "
            ".github/workflows/hw-fast.yml). Per H-01: if the FMPy/omc "
            "pipeline cannot run within hosted-runner limits, stop and flag "
            "it - do not downgrade the oracle test to a compile-only check.")
    return omc


def _run_omc(omc, script, cwd):
    command = [omc, "-q", 'runScript("%s")' % script]
    result = subprocess.run(
        command, cwd=str(cwd), stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT, universal_newlines=True, timeout=600)
    return result


def _omc_version(omc):
    result = subprocess.run(
        [omc, "--version"], stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        universal_newlines=True)
    return result.stdout.strip().splitlines()[0] if result.stdout.strip() \
        else "unknown"


def build_fmu(sim_case, build_dir):
    """Build the FMU headless via omc; return the FMU path."""
    build_dir.mkdir(parents=True, exist_ok=True)
    model = sim_case["model"]
    package_dir = MODEL_ROOT
    power_dir = MODEL_ROOT / "Power"
    script = build_dir / "omc_build.mos"
    template = (
        "loadModel(Modelica);\n"
        'loadFile("%s");\n'
        'loadFile("%s");\n'
        'loadFile("%s");\n'
        'buildModelFMU(%s, version="2.0", fmuType="__FMU_TYPE__", '
        'fileNamePrefix="cancestry_holdup");\n' % (
            package_dir / "package.mo",
            power_dir / "package.mo",
            power_dir / "Holdup.mo",
            model,
        ))
    omc = _require_toolchain()
    for fmi_type in FMI_TYPES:
        script.write_text(template.replace("__FMU_TYPE__", fmi_type),
                          encoding="utf-8")
        result = _run_omc(omc, str(script), build_dir)
        match = re.findall(r'([^\s"\'()]+\.fmu)', result.stdout)
        if result.returncode == 0 and match:
            fmu = Path(match[-1])
            if not fmu.is_absolute():
                fmu = build_dir / fmu
            if fmu.is_file():
                return fmu
        if "fmuType" not in result.stdout:
            break  # a real error, not an unsupported target: stop early
    raise AssertionError(
        "omc buildModelFMU failed (last output):\n%s" % result.stdout[-4000:])


def simulate_fmu(fmu, sim_case):
    """Execute the FMU with FMPy; return (times, voltages) as float lists."""
    import fmpy

    solver = sim_case["solver"]
    result = fmpy.simulate_fmu(
        str(fmu),
        start_time=0.0,
        stop_time=solver["stop_s"],
        step_size=solver["step_s"],
        output=["v"],
        logger=None)
    times = [float(value) for value in result[0]]
    voltages = [float(value) for value in result[1]["v"]]
    assert len(times) == len(voltages) and len(times) > 1, \
        "FMU trace is empty"
    assert abs(times[0]) <= 1e-12, "trace must start at t=0"
    assert abs(times[-1] - solver["stop_s"]) <= 2 * solver["step_s"], \
        "trace does not reach the declared stop time"
    return times, voltages


def write_trace_artifacts(case_id, times, voltages, numerics, tool):
    """Per-run artifacts (gitignored build/): hashed trace + run log."""
    build_dir = REPO_ROOT / "build" / "hw"
    build_dir.mkdir(parents=True, exist_ok=True)
    trace_path = build_dir / ("%s.trace.csv" % case_id)
    with open(trace_path, "w", encoding="utf-8", newline="") as handle:
        handle.write("time_s,v_v\n")
        for t, v in zip(times, voltages):
            handle.write("%s,%s\n" % (repr(t), repr(v)))
    run_log = {
        "case_id": case_id,
        "pass": True,
        "result": numerics,
        "tool_measured": tool,
        "trace": {
            "file": "build/hw/%s.trace.csv" % case_id,
            "sha256": sha256_file(trace_path),
        },
    }
    run_log_path = build_dir / ("%s.runlog.json" % case_id)
    run_log_path.write_text(
        json.dumps(run_log, sort_keys=True, indent=2) + "\n", encoding="utf-8")
    return run_log


def measured_tool(omc):
    import importlib.metadata

    def version(name):
        try:
            return importlib.metadata.version(name)
        except importlib.metadata.PackageNotFoundError:
            return "unknown"

    return {
        "openmodelica": _omc_version(omc),
        "fmpy": version("fmpy"),
        "numpy": version("numpy"),
        "python": "%d.%d.%d" % sys.version_info[:3],
    }


# --------------------------------------------------------------------------
# Fixtures
# --------------------------------------------------------------------------

@pytest.fixture(scope="session")
def sim_case():
    return load_sim_case()


@pytest.fixture(scope="session")
def bom():
    return load_bom()


@pytest.fixture(scope="session")
def pipeline(sim_case, bom):
    """Build the FMU once per session and run the oracle-verified sim."""
    build_dir = BUILD_DIR / sim_case["case_id"]
    fmu = build_fmu(sim_case, build_dir)
    omc = _require_toolchain()
    times, voltages = simulate_fmu(fmu, sim_case)

    oracle = load_oracle()
    params = sim_case["parameters"]
    v0 = params["V0"]
    i_mcu = params["I_mcu"]
    i_leak = params["I_leak"]
    c = params["C"]
    tolerance = sim_case["tolerances"]["vb_max_abs_delta_v"]
    v_floor = params["V_floor"]

    deltas = [abs(v - oracle.vbat(t, v0, i_mcu, i_leak, c))
              for t, v in zip(times, voltages)]
    max_delta = max(deltas)
    if max_delta > tolerance:
        pytest.fail(
            "VBAT(t) deviates from oracle OR-001 by %.3e V (tolerance %.3e "
            "V declared in %s)" %
            (max_delta, tolerance, SIM_CASE_PATH.relative_to(REPO_ROOT)))

    v_end = voltages[-1]
    if v_end < v_floor:
        pytest.fail(
            "VBAT falls to %.6f V below the %.6f V retention floor before "
            "the end of the HW-SF-002 event" % (v_end, v_floor))

    numerics = {
        "max_abs_delta_v": max_delta,
        "min_v_v": min(voltages),
        "n_samples": len(times),
        "time_to_floor_s": oracle.time_to_floor(v0, v_floor, i_mcu, i_leak,
                                                c),
        "tolerance_v": tolerance,
        "v_end_v": v_end,
        "v_floor_v": v_floor,
    }
    tool = measured_tool(omc)
    write_trace_artifacts(sim_case["case_id"], times, voltages, numerics,
                          tool)
    return {
        "fmu": fmu,
        "times": times,
        "voltages": voltages,
        "numerics": numerics,
        "tool": tool,
    }


# --------------------------------------------------------------------------
# Tests
# --------------------------------------------------------------------------

def test_sim_case_and_bom_validate(sim_case, bom):
    """HW-SIM-HOLDUP-001: the case and the BOM extract are schema-valid."""
    _validate_against_schema(sim_case, "hw-sim-0.1.0.schema.json",
                             "sim case")
    _validate_against_schema(bom, "hw-bom-0.1.0.schema.json", "bom")
    for part in bom["parts"]:
        extract = part["datasheet_ref"].get("extract")
        if extract:
            document = _json(REPO_ROOT / extract)
            _validate_against_schema(document, "hw-bom-0.1.0.schema.json",
                                     "extract %s" % extract)


def test_sim_case_parameters_match_bom(sim_case, bom):
    """HW-SIM-HOLDUP-002: every simulated parameter is cited (CL2 link)."""
    mapping = bom_parameter_map(bom)
    params = sim_case["parameters"]
    for name in ("t_brownout", "t_remove"):
        assert name in params, "scenario timing %s missing" % name
    # Worst-case durations of HW-SF-002 (ii) and (iii).
    assert 0.0 < params["t_brownout"] <= 0.05 + 1e-12
    assert 0.0 < params["t_remove"] <= 0.1 + 1e-12
    assert abs(sim_case["solver"]["stop_s"]
               - (params["t_brownout"] + params["t_remove"])) <= 1e-12
    # The mapping must be exact: every model parameter is cited and no
    # cited BOM parameter is left unlinked (drift fails the gate).
    assert set(MODEL_PARAM_TO_BOM.values()) == set(mapping), (
        "MODEL_PARAM_TO_BOM drifted from hw/bom/bom.json: model-linked=%s, "
        "BOM-cited=%s" % (sorted(MODEL_PARAM_TO_BOM.values()),
                          sorted(mapping)))
    for name, value in params.items():
        if name in ("t_brownout", "t_remove"):
            continue
        bom_name = MODEL_PARAM_TO_BOM.get(name)
        assert bom_name is not None, (
            "parameter %s has no cited BOM entry (HwAGENTS.md rule 2: no "
            "value without an extract)" % name)
        bom_value = mapping[bom_name][0]
        assert bom_value == value, (
            "parameter %s (cited as %s): sim case has %r, the BOM extract "
            "has %r" % (name, bom_name, value, bom_value))


def test_trace_matches_oracle(pipeline, sim_case):
    """HW-SIM-HOLDUP-003: VBAT(t) matches OR-001 within the declared tol."""
    numerics = pipeline["numerics"]
    tolerance = sim_case["tolerances"]["vb_max_abs_delta_v"]
    assert numerics["max_abs_delta_v"] <= tolerance
    assert numerics["n_samples"] > 1
    # The oracle's own self-check (closed form vs independent RK4) still holds.
    oracle = load_oracle()
    self_check = oracle.self_check()
    assert self_check["pass"], self_check


def test_vbat_stays_above_retention_floor(pipeline, sim_case):
    """HW-SIM-HOLDUP-004: acceptance - floor held across the event."""
    numerics = pipeline["numerics"]
    params = sim_case["parameters"]
    assert numerics["v_end_v"] >= params["V_floor"]
    assert numerics["min_v_v"] >= params["V_floor"]
    # The hold-up margin is declared in the evidence (10x margin rule of
    # HwRS HW-FR-009): the event ends far before the closed-form floor time.
    assert numerics["time_to_floor_s"] > sim_case["solver"]["stop_s"]


def test_evidence_consistent_and_tool_pins_held(pipeline, sim_case):
    """HW-SIM-HOLDUP-005: committed evidence matches live sources + pins."""
    evidence_path = EVIDENCE_DIR / ("%s.json" % sim_case["case_id"])
    assert evidence_path.is_file(), (
        "evidence artifact %s is missing; the traceability row cannot be "
        "passing without it (HwAGENTS.md rule 4)" %
        evidence_path.relative_to(REPO_ROOT))
    document = _json(evidence_path)
    assert document["pass"] is True
    assert document["requirement_id"] == sim_case["requirement_id"]
    assert document["oracle_id"] == sim_case["oracle_id"]
    assert document["case_id"] == sim_case["case_id"]
    for relative, digest in sorted(document["source_hashes"].items()):
        source = REPO_ROOT / relative
        assert source.is_file(), "evidence source %s is missing" % relative
        assert sha256_file(source) == digest, (
            "evidence source %s drifted from the pinned sha256" % relative)
    pins = document["tool_pins"]
    tool = pipeline["tool"]
    assert tool["fmpy"] == pins["fmpy"], (
        "fmpy %s != pinned %s (ci/docker pin drift)" %
        (tool["fmpy"], pins["fmpy"]))
    assert tool["numpy"] == pins["numpy"], (
        "numpy %s != pinned %s (ci/docker pin drift)" %
        (tool["numpy"], pins["numpy"]))
    assert pins["openmodelica"] in tool["openmodelica"], (
        "OpenModelica %s does not match the pinned %s" %
        (tool["openmodelica"], pins["openmodelica"]))
    major, minor = (int(part) for part in sys.version_info[:2])
    assert (major, minor) >= (3, 10), "Python pin (>= 3.10) not met"
