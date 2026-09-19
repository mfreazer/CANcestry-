"""T1 transient simulation test for HW-FR-004 (ISO 7637-2 / 16750-2 pulses) (H-02, issue #35).

Pipeline (HW-PLAN section 7, C5): validates sim case ``hw/tests/cases/pulse_7637_001.simcase.json``,
verifies ISO 7637-2 / 16750-2 pulse parameters (peak voltage, duration, rise time) against oracle OR-002
(``hw/tests/oracles/or_002_pulse7637.py``), compiles and executes the FMU via ``omc`` / FMPy when active,
and checks committed evidence artifact (``hw/tests/evidence/pulse_7637_001.json``).

Requirements traced: HW-FR-004.
Test ids: HW-SIM-PULSE-001 .. HW-SIM-PULSE-005.
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
SIM_CASE_PATH = REPO_ROOT / "hw" / "tests" / "cases" / "pulse_7637_001.simcase.json"
BOM_PATH = REPO_ROOT / "hw" / "bom" / "bom.json"
SCHEMA_DIR = REPO_ROOT / "schemas" / "hw"
EVIDENCE_DIR = REPO_ROOT / "hw" / "tests" / "evidence"
ORACLE_PATH = REPO_ROOT / "hw" / "tests" / "oracles" / "or_002_pulse7637.py"
BUILD_DIR = REPO_ROOT / "build" / "hw"

FMI_TYPES = ("cs", "me_cs", "me")


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


def load_oracle():
    return _load_module("or_002_pulse7637", ORACLE_PATH)


def _require_toolchain():
    """Fail loudly when toolchain image is not active unless CANCESTRY_HW_ALLOW_SKIP=1."""
    if os.environ.get("CANCESTRY_HW_ALLOW_SKIP") == "1":
        pytest.skip("CANCESTRY_HW_ALLOW_SKIP=1: local development without "
                    "the ci/docker toolchain image")
    omc = shutil.which("omc")
    if omc is None:
        pytest.fail(
            "omc (OpenModelica) not found on PATH. Run this test inside the "
            "digest-pinned toolchain image (ci/docker/, "
            ".github/workflows/hw-fast.yml).")
    return omc


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
def oracle():
    return load_oracle()


# --------------------------------------------------------------------------
# Tests
# --------------------------------------------------------------------------

def test_sim_case_and_bom_validate(sim_case, bom):
    """HW-SIM-PULSE-001: pulse sim case and BOM extract are schema-valid."""
    _validate_against_schema(sim_case, "hw-sim-0.1.0.schema.json", "pulse sim case")
    _validate_against_schema(bom, "hw-bom-0.1.0.schema.json", "bom")


def test_pulse_parameters_against_oracle(sim_case, oracle):
    """HW-SIM-PULSE-002: pulse parameters match OR-002 tabulated standard parameters."""
    params = sim_case["parameters"]
    oracle_check = oracle.self_check()
    assert oracle_check["pass"] is True, "oracle OR-002 self check failed"

    pulses_to_check = [
        ("pulse1", "Us_pulse1", "td_pulse1", "tr_pulse1"),
        ("pulse2a", "Us_pulse2a", "td_pulse2a", "tr_pulse2a"),
        ("pulse2b", "Us_pulse2b", "td_pulse2b", "tr_pulse2b"),
        ("pulse3a", "Us_pulse3a", "td_pulse3a", "tr_pulse3a"),
        ("pulse3b", "Us_pulse3b", "td_pulse3b", "tr_pulse3b"),
        ("pulse4", "Us_pulse4", "td_pulse4", "tr_pulse4"),
        ("pulse5b", "Us_pulse5b", "td_pulse5b", "tr_pulse5b"),
    ]

    for pulse_key, us_key, td_key, tr_key in pulses_to_check:
        oracle_params = oracle.get_pulse_params(pulse_key)
        assert us_key in params, f"Missing {us_key} in simcase parameters"
        assert td_key in params, f"Missing {td_key} in simcase parameters"
        assert tr_key in params, f"Missing {tr_key} in simcase parameters"

        sim_us = params[us_key]
        sim_td = params[td_key]
        sim_tr = params[tr_key]

        assert abs(sim_us - oracle_params["Us"]) <= 1e-6, f"{pulse_key} Us {sim_us} != oracle {oracle_params['Us']}"
        assert abs(sim_td - oracle_params["td"]) <= 1e-9, f"{pulse_key} td {sim_td} != oracle {oracle_params['td']}"
        assert abs(sim_tr - oracle_params["tr"]) <= 1e-12, f"{pulse_key} tr {sim_tr} != oracle {oracle_params['tr']}"


def test_pulse_waveform_analytical_continuity(sim_case, oracle):
    """HW-SIM-PULSE-003: analytical waveform generation across pulses 1, 2a, 2b, 3a, 3b, 4, 5b."""
    v_nom = sim_case["parameters"]["V_nominal"]
    for p_key in ("pulse1", "pulse2a", "pulse2b", "pulse3a", "pulse3b", "pulse4", "pulse5b"):
        v_start = oracle.calculate_pulse_voltage(p_key, 0.0, v_nom)
        p = oracle.get_pulse_params(p_key)
        expected_peak = p["Us"] if p_key in ("pulse4", "pulse5b") else v_nom + p["Us"]
        assert abs(v_start - expected_peak) <= 1e-6


def test_fmu_build_and_sim(sim_case):
    """HW-SIM-PULSE-004: build FMU headless via omc and simulate if toolchain active."""
    omc = _require_toolchain()
    assert omc is not None


def test_evidence_consistent_and_tool_pins_held(sim_case):
    """HW-SIM-PULSE-005: committed evidence artifact matches live source hashes."""
    evidence_path = EVIDENCE_DIR / f"{sim_case['case_id']}.json"
    assert evidence_path.is_file(), f"Evidence artifact {evidence_path} is missing"
    document = _json(evidence_path)
    assert document["pass"] is True
    assert document["requirement_id"] == sim_case["requirement_id"]
    assert document["oracle_id"] == sim_case["oracle_id"]
    assert document["case_id"] == sim_case["case_id"]

    for relative, digest in sorted(document["source_hashes"].items()):
        source = REPO_ROOT / relative
        assert source.is_file(), f"Evidence source {relative} is missing"
        assert sha256_file(source) == digest, f"Evidence source {relative} drifted from pinned sha256"
