"""Fixture unit tests for ci/check_hw_traceability.py (H-01, issue #33).

The HwRS parser must fail closed when the requirement tables change format
(HW-PLAN section 10.8), and the gate must enforce the honest ledger before
any hardware requirement can read passing: status vocabulary, oracle rule
(HW-PLAN section 10.2), credibility rule (HW-PLAN section 10.3) and
evidence integrity (HwAGENTS.md rule 4).

Requirements traced: HW-SF-002 (seed row), HW-PLAN 10.2/10.3/10.8,
HwAGENTS.md rule 4. Test ids: HW-TRACE-PARSER-001..004,
HW-TRACE-GATE-001..014.
"""

from __future__ import annotations

import contextlib
import csv
import hashlib
import importlib.util
import io
import json
import subprocess
import sys

import pytest

from conftest import REPO_ROOT

CHECKER = REPO_ROOT / "ci" / "check_hw_traceability.py"
ROW_SCHEMA = REPO_ROOT / "schemas" / "hw" / "hw-traceability-0.1.0.schema.json"

_spec = importlib.util.spec_from_file_location("ci_check_hw_traceability",
                                               CHECKER)
check_hw = importlib.util.module_from_spec(_spec)
sys.modules["ci_check_hw_traceability"] = check_hw
_spec.loader.exec_module(check_hw)

HWRS_VALID = """# CANcestry HwRS (fixture)

## 2. Safety requirements

| ID | Requirement (shall) | Derivation | Method / Tier | Req. CL | Oracle class | Initial status |
|---|---|---|---|---|---|---|
| HW-SF-001 | Safe state without firmware. | SYS-SF-004 | T0 netlist analysis | CL3 | (d) netlist query | `analysis-pending` |
| HW-SF-002 | Retention across brownout and removal. | SW-FR-LOG-004 | T1 hold-up sim; T2; T4 | CL3 | (a) OR-001 | `sim-pending` |

## 3. Functional requirements

| ID | Requirement (shall) | Derivation | Method / Tier | Req. CL | Oracle class | Initial status |
|---|---|---|---|---|---|---|
| HW-FR-005 | Sleep current budget. | Budget | T1 power model; T4 | CL1 | (b) tables | `sim-pending` |
| HW-FR-009 | Hold-up capacitor on VBAT. | HW-SF-002 | T1 hold-up sim; T4 | CL3 | (a) OR-001 | `sim-pending` |
"""

REGISTRY_VALID = (
    "oracle_id,oracle,class,serves,validation_gap\n"
    "OR-001,RC hold-up / energy-balance closed form,(a),HW-SF-002,"
    "Idealized ODE gap; T2 SPICE or bench correlation\n"
    "OR-002,ISO 7637-2 pulse parameters,(b),HW-FR-004,Fixture pulse gap\n"
)

TRACE_HEADER = (
    "requirement_id,method,capella_element_id,credibility_level,oracle_id,"
    "required_cl,status,evidence,evidence_sha256,inherited_validation_gap\n"
)

EVIDENCE_REL = "hw/tests/evidence/holdup_001.json"
ORACLE_SOURCE = "hw/tests/oracles/or_001_holdup.py"


def sha256_bytes(text):
    return "sha256:%s" % hashlib.sha256(text.encode("utf-8")).hexdigest()


def write(path, text):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")
    return path


def make_repo(root, hwrs=HWRS_VALID, registry=REGISTRY_VALID, trace=None,
              with_schema=True, sources=None):
    """Compose a minimal hardware repository under ``root``.

    ``trace``: ledger text (written only when not None). ``sources``:
    {relative path: text} files (e.g. the oracle source pinned by evidence).
    """
    write(root / "docs" / "hw" / "HwRS.md", hwrs)
    write(root / "hw" / "tests" / "oracles" / "registry.csv", registry)
    # Compose the authoritative JSON too; malformed legacy CSV still exercises
    # the native compatibility checks before the JSON/exact-export guard.
    records = []
    classes = {"(a)": "analytical", "(b)": "standard", "(c)": "golden_measurement", "(d)": "independent_model"}
    for record in csv.DictReader(io.StringIO(registry)):
        if not all(record.get(key) is not None for key in
                   ("oracle_id", "oracle", "class", "serves", "validation_gap")):
            continue
        records.append({"oracle_id": record["oracle_id"], "description": record["oracle"],
                        "class": classes.get(record["class"], record["class"]),
                        "serves": record["serves"].split(";"),
                        "validation_gap": record["validation_gap"],
                        "source_citation": "Fixture 2026"})
    write(root / "hw/tests/oracles/registry.json", json.dumps({"schema_version": "0.1.0", "oracles": records}))
    write(root / "schemas/hw/hw-oracle-registry-0.1.0.schema.json",
          (REPO_ROOT / "schemas/hw/hw-oracle-registry-0.1.0.schema.json").read_text())
    write(root / check_hw.QUALIFICATION_PATH, (REPO_ROOT / check_hw.QUALIFICATION_PATH).read_text())
    if trace is not None:
        write(root / "hw" / "tests" / "traceability.csv", trace)
    if with_schema:
        write(root / ROW_SCHEMA.relative_to(REPO_ROOT),
              ROW_SCHEMA.read_text(encoding="utf-8"))
    if sources:
        for relative, text in sorted(sources.items()):
            write(root / relative, text)
    return root


def make_evidence(sources=None, requirement="HW-SF-002", oracle="OR-001",
                  passed=True):
    return {
        "case_id": "holdup_001",
        "oracle_id": oracle,
        "pass": passed,
        "requirement_id": requirement,
        "tool_pins": {"capellambse": "0.6.17"},
        "source_hashes": {
            relative: sha256_bytes(text)
            for relative, text in sorted((sources or {}).items())
        },
    }


def row(requirement, method, status, credibility="CL2", oracle="OR-001",
        required_cl="CL3", evidence="", digest="", capella="CAP_PENDING", gap=""):
    """One ledger row, using CSV escaping for status and inherited gap text."""
    stream = io.StringIO(newline="")
    csv.writer(stream, lineterminator="\n").writerow((
        requirement, method, capella, credibility, oracle, required_cl,
        status, evidence, digest, gap))
    return stream.getvalue()


def write_evidence(root, document):
    path = write(root / EVIDENCE_REL, json.dumps(document, sort_keys=True))
    return sha256_bytes(path.read_text(encoding="utf-8"))


def evidence_row(root, sources, status="passing(sim,CL2,provisional)",
                 requirement="HW-SF-002", oracle="OR-001", credibility="CL2",
                 method="sim", passed=True, required_cl="CL3"):
    """Write a passing row whose evidence artifact really matches."""
    document = make_evidence(sources=sources, requirement=requirement,
                             oracle=oracle, passed=passed)
    digest = write_evidence(root, document)
    return TRACE_HEADER + row(requirement, method, status, credibility,
                              oracle, required_cl=required_cl,
                              evidence=EVIDENCE_REL, digest=digest)


class _Result:
    """Minimal CompletedProcess stand-in (returncode + stdout)."""

    def __init__(self, returncode, stdout):
        self.returncode = returncode
        self.stdout = stdout


def run_checker(root, use_subprocess=False, explain=False):
    """Run the gate in-process (coverage-visible) or as a real CLI call."""
    args = [str(root)] + (["--explain"] if explain else [])
    if use_subprocess:
        return subprocess.run(
            [sys.executable, str(CHECKER)] + args,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            universal_newlines=True)
    buffer = io.StringIO()
    with contextlib.redirect_stdout(buffer):
        returncode = check_hw.main(["checker"] + args)
    return _Result(returncode, buffer.getvalue())


def clean_row():
    return {"requirement_id": "HW-SF-002", "method": "sim",
            "capella_element_id": "CAP_PENDING", "credibility_level": "CL2",
            "oracle_id": "OR-001", "required_cl": "CL3",
            "status": "sim-pending", "evidence": "", "evidence_sha256": "",
            "inherited_validation_gap": ""}


# ---------------------------------------------------------------------------
# HwRS parser (fail-closed) - HW-TRACE-PARSER-001..004
# ---------------------------------------------------------------------------

def test_parser_reads_required_cl(tmp_path):
    """HW-TRACE-PARSER-001: valid tables parse to {id: required CL}."""
    make_repo(tmp_path)
    table = check_hw.parse_hwrs(HWRS_VALID)
    assert table == {
        "HW-SF-001": "CL3",
        "HW-SF-002": "CL3",
        "HW-FR-005": "CL1",
        "HW-FR-009": "CL3",
    }


def test_parser_fails_closed_on_short_row(tmp_path):
    """HW-TRACE-PARSER-002: a 6-field row is a format change -> fail closed."""
    broken = HWRS_VALID.replace(
        "| HW-SF-001 | Safe state without firmware. | SYS-SF-004 | "
        "T0 netlist analysis | CL3 | (d) netlist query | `analysis-pending` |",
        "| HW-SF-001 | Safe state. | SYS-SF-004 | T0 | CL3 | `x` |")
    with pytest.raises(check_hw.HwrsFormatError) as excinfo:
        check_hw.parse_hwrs(broken)
    assert any("6 fields" in issue for issue in excinfo.value.issues)

    make_repo(tmp_path, hwrs=broken)
    result = run_checker(tmp_path)
    assert result.returncode == 1
    assert "fields" in result.stdout and "format change" in result.stdout


def test_parser_explain_flag_reports_table_contract(tmp_path):
    """HW-TRACE-PARSER-005: --explain gives a repairable parse diagnostic."""
    broken = "# HwRS with a changed table\n| ID | Requirement |\n|---|---|\n"
    make_repo(tmp_path, hwrs=broken,
              trace=TRACE_HEADER + row("HW-SF-002", "sim", "sim-pending",
                                        credibility="CL0", oracle=""))
    result = run_checker(tmp_path, explain=True)
    assert result.returncode == 2
    assert "EXPECTED HwRS Markdown table format" in result.stdout
    assert "exactly seven fields" in result.stdout
    assert "HW-(?:SF|FR|NF)-" in result.stdout


def test_parser_fails_closed_on_bad_cl_and_duplicates(tmp_path):
    """HW-TRACE-PARSER-003: non-CL cell and duplicate id -> fail closed."""
    bad_cl = HWRS_VALID.replace("T0 netlist analysis | CL3",
                                "T0 netlist analysis | CL9")
    with pytest.raises(check_hw.HwrsFormatError) as excinfo:
        check_hw.parse_hwrs(bad_cl)
    assert any("CL9" in issue for issue in excinfo.value.issues)

    duplicate = HWRS_VALID.replace(
        "HW-FR-009 | Hold-up capacitor on VBAT.",
        "HW-SF-002 | Hold-up capacitor on VBAT.")
    with pytest.raises(check_hw.HwrsFormatError) as excinfo:
        check_hw.parse_hwrs(duplicate)
    assert any("duplicate" in issue for issue in excinfo.value.issues)

    # The composed (valid) repo still passes, with a plain pending row.
    trace = (TRACE_HEADER
             + row("HW-SF-002", "sim", "sim-pending", credibility="CL0",
                   oracle=""))
    make_repo(tmp_path, trace=trace)
    result = run_checker(tmp_path)
    assert result.returncode == 0, result.stdout


def test_parser_fails_closed_when_no_rows_found(tmp_path):
    """HW-TRACE-PARSER-004: zero requirement rows -> fail closed."""
    empty_hwrs = "# empty HwRS\nno tables here\n"
    with pytest.raises(check_hw.HwrsFormatError) as excinfo:
        check_hw.parse_hwrs(empty_hwrs)
    assert any("no HW-* requirement rows" in issue
               for issue in excinfo.value.issues)

    make_repo(tmp_path, hwrs=empty_hwrs)
    result = run_checker(tmp_path)
    assert result.returncode == 1
    assert "no HW-* requirement rows" in result.stdout


# ---------------------------------------------------------------------------
# Gate rules - HW-TRACE-GATE-001..014
# ---------------------------------------------------------------------------

def test_gate_passes_on_seed_ledger(tmp_path):
    """HW-TRACE-GATE-001: the H-01 seed rows (sim + bench) pass."""
    sources = {ORACLE_SOURCE: "oracle source\n"}
    make_repo(tmp_path, sources=sources)
    trace = evidence_row(tmp_path, sources)
    trace += row("HW-SF-002", "bench", "bench-pending", credibility="CL0",
                 oracle="")
    write(tmp_path / "hw" / "tests" / "traceability.csv", trace)
    result = run_checker(tmp_path)
    assert result.returncode == 0, result.stdout
    assert "passing 1, pending 1" in result.stdout


def test_gate_rejects_orphan_requirement(tmp_path):
    """HW-TRACE-GATE-002: an HW id not in HwRS.md fails the gate."""
    make_repo(tmp_path)
    trace = TRACE_HEADER + row("HW-SF-999", "sim", "sim-pending",
                               credibility="CL0", oracle="")
    make_repo(tmp_path, trace=trace)
    result = run_checker(tmp_path)
    assert result.returncode == 1
    assert "orphan hardware id" in result.stdout


def test_gate_rejects_bad_status_and_vocabulary(tmp_path):
    """HW-TRACE-GATE-003: status/method vocabulary is enforced."""
    make_repo(tmp_path)
    trace = (TRACE_HEADER
             + row("HW-SF-001", "flying", "draft", credibility="CL0",
                   oracle="")
             + row("HW-FR-005", "sim", "passing(flying,CL9)",
                   credibility="CL0", oracle="", required_cl="CL1"))
    make_repo(tmp_path, trace=trace)
    result = run_checker(tmp_path)
    assert result.returncode == 1
    assert ("not one of analysis, sim, virtual_bench, protocol, bench"
            in result.stdout)
    assert "not in the honest-ledger vocabulary" in result.stdout


def test_gate_rejects_required_cl_mismatch(tmp_path):
    """HW-TRACE-GATE-004: required_cl must equal the HwRS Req. CL."""
    make_repo(tmp_path)
    trace = TRACE_HEADER + row("HW-FR-005", "sim", "sim-pending",
                               credibility="CL0", oracle="",
                               required_cl="CL3")
    make_repo(tmp_path, trace=trace)
    result = run_checker(tmp_path)
    assert result.returncode == 1
    assert "required_cl CL3 != HwRS Req. CL CL1" in result.stdout


def test_gate_requires_oracle_for_passing(tmp_path):
    """HW-TRACE-GATE-005: no oracle id (or unregistered one) -> no pass."""
    make_repo(tmp_path)
    trace = (TRACE_HEADER
             + row("HW-SF-002", "sim", "passing(sim,CL3)", oracle="",
                   evidence=EVIDENCE_REL, digest="sha256:" + "0" * 64)
             + row("HW-SF-001", "analysis", "passing(analysis)",
                   oracle="OR-999",
                   evidence=EVIDENCE_REL, digest="sha256:" + "0" * 64))
    make_repo(tmp_path, trace=trace)
    result = run_checker(tmp_path)
    assert result.returncode == 1
    assert "without an oracle_id" in result.stdout
    assert "OR-999" in result.stdout and "registry.csv" in result.stdout


def test_gate_credibility_below_required_fails(tmp_path):
    """HW-TRACE-GATE-006: passing below required CL without provisional."""
    sources = {ORACLE_SOURCE: "oracle source\n"}
    make_repo(tmp_path, sources=sources)
    trace = evidence_row(tmp_path, sources, status="passing(sim,CL2)",
                         requirement="HW-FR-009")
    write(tmp_path / "hw" / "tests" / "traceability.csv", trace)
    result = run_checker(tmp_path)
    assert result.returncode == 1
    assert "below the required CL3" in result.stdout


def test_gate_provisional_allowed_for_safety_rows_only(tmp_path):
    """HW-TRACE-GATE-007: provisional closure is a safety-row mechanism."""
    sources = {ORACLE_SOURCE: "oracle source\n"}
    # Safety row HW-SF-002: provisional at CL2 < required CL3 is allowed
    # (T1 closure pending T4, HwRS closure policy).
    make_repo(tmp_path, sources=sources)
    trace = evidence_row(tmp_path, sources, requirement="HW-SF-002")
    write(tmp_path / "hw" / "tests" / "traceability.csv", trace)
    assert run_checker(tmp_path).returncode == 0

    # Functional row HW-FR-009: the same provisional status must fail.
    trace = evidence_row(tmp_path, sources, requirement="HW-FR-009")
    write(tmp_path / "hw" / "tests" / "traceability.csv", trace)
    result = run_checker(tmp_path)
    assert result.returncode == 1
    assert "not a provisional safety closure" in result.stdout


def test_gate_status_cl_must_match_credibility_level(tmp_path):
    """HW-TRACE-GATE-008: passing(sim,CLn) CL must equal credibility_level."""
    sources = {ORACLE_SOURCE: "oracle source\n"}
    make_repo(tmp_path, sources=sources)
    # HW-SF-001 requires CL3 and the row carries CL3 in the status, so the
    # credibility rule holds; the mismatch under test is between the status
    # CL (3) and the credibility_level field (CL2).
    trace = evidence_row(tmp_path, sources, status="passing(sim,CL3)",
                         credibility="CL2", requirement="HW-SF-001")
    write(tmp_path / "hw" / "tests" / "traceability.csv", trace)
    result = run_checker(tmp_path)
    assert result.returncode == 1
    assert "status declares CL3 but credibility_level is CL2" in result.stdout


def test_gate_fully_verified_requires_bench(tmp_path):
    """HW-TRACE-GATE-009: fully-verified(CL3) is bench (T4) only."""
    sources = {ORACLE_SOURCE: "oracle source\n"}
    make_repo(tmp_path, sources=sources)
    trace = evidence_row(tmp_path, sources, method="sim",
                         status="fully-verified(CL3)", credibility="CL3",
                         requirement="HW-SF-001")
    write(tmp_path / "hw" / "tests" / "traceability.csv", trace)
    result = run_checker(tmp_path)
    assert result.returncode == 1
    assert "reachable only through the bench method" in result.stdout

    trace = evidence_row(tmp_path, sources, method="bench",
                         status="fully-verified(CL3)", credibility="CL3",
                         requirement="HW-SF-001")
    write(tmp_path / "hw" / "tests" / "traceability.csv", trace)
    assert run_checker(tmp_path).returncode == 0


def test_gate_evidence_integrity(tmp_path):
    """HW-TRACE-GATE-010..012: missing/mismatched/drifted evidence fails."""
    sources = {ORACLE_SOURCE: "oracle source\n"}
    base_status = "passing(sim,CL2,provisional)"

    # (a) passing row without any evidence.
    make_repo(tmp_path)
    trace = TRACE_HEADER + row("HW-SF-002", "sim", base_status)
    make_repo(tmp_path, trace=trace)
    result = run_checker(tmp_path)
    assert result.returncode == 1
    assert "without evidence path and hash" in result.stdout

    # (b) evidence file missing.
    make_repo(tmp_path)
    trace = (TRACE_HEADER
             + row("HW-SF-002", "sim", base_status, evidence=EVIDENCE_REL,
                   digest="sha256:" + "0" * 64))
    make_repo(tmp_path, trace=trace)
    result = run_checker(tmp_path)
    assert result.returncode == 1
    assert "evidence artifact" in result.stdout and "is missing" in result.stdout

    # (c) recorded hash does not match the artifact.
    make_repo(tmp_path, sources=sources)
    document = make_evidence(sources=sources)
    write_evidence(tmp_path, document)
    trace = (TRACE_HEADER
             + row("HW-SF-002", "sim", base_status, evidence=EVIDENCE_REL,
                   digest="sha256:" + "1" * 64))
    write(tmp_path / "hw" / "tests" / "traceability.csv", trace)
    result = run_checker(tmp_path)
    assert result.returncode == 1
    assert "does not match the recorded sha256" in result.stdout

    # (d) evidence does not record pass=true / wrong requirement / oracle.
    for tamper in ({"pass": False}, {"requirement_id": "HW-SF-001"},
                   {"oracle_id": "OR-002"}):
        make_repo(tmp_path, sources=sources)
        document = make_evidence(sources=sources)
        document.update(tamper)
        digest = write_evidence(tmp_path, document)
        trace = (TRACE_HEADER
                 + row("HW-SF-002", "sim", base_status, evidence=EVIDENCE_REL,
                       digest=digest))
        write(tmp_path / "hw" / "tests" / "traceability.csv", trace)
        result = run_checker(tmp_path)
        assert result.returncode == 1, tamper

    # (e) a source pinned by the evidence drifted after issuance.
    make_repo(tmp_path, sources=sources)
    document = make_evidence(sources=sources)
    digest = write_evidence(tmp_path, document)
    write(tmp_path / ORACLE_SOURCE, "oracle source TAMPERED\n")
    trace = (TRACE_HEADER
             + row("HW-SF-002", "sim", base_status, evidence=EVIDENCE_REL,
                   digest=digest))
    write(tmp_path / "hw" / "tests" / "traceability.csv", trace)
    result = run_checker(tmp_path)
    assert result.returncode == 1, result.stdout
    assert "drifted from the pinned sha256" in result.stdout


def test_gate_non_passing_row_must_not_carry_evidence(tmp_path):
    """HW-TRACE-GATE-013: pending rows carry no evidence (honest ledger)."""
    make_repo(tmp_path)
    trace = (TRACE_HEADER
             + row("HW-SF-002", "bench", "bench-pending", credibility="CL0",
                   oracle="", evidence=EVIDENCE_REL,
                   digest="sha256:" + "0" * 64))
    make_repo(tmp_path, trace=trace)
    result = run_checker(tmp_path)
    assert result.returncode == 1
    assert "non-passing row carries evidence" in result.stdout


def test_gate_rejects_duplicate_pairs_and_bad_registry(tmp_path):
    """HW-TRACE-GATE-014: unique (requirement, method); registry is law."""
    make_repo(tmp_path)
    trace = (TRACE_HEADER
             + row("HW-SF-002", "sim", "sim-pending", credibility="CL0",
                   oracle="")
             + row("HW-SF-002", "sim", "bench-pending", credibility="CL0",
                   oracle=""))
    make_repo(tmp_path, trace=trace)
    result = run_checker(tmp_path)
    assert result.returncode == 1
    assert "duplicate row" in result.stdout

    bad_registry = "oracle_id,oracle,class,serves,validation_gap\nBAD-x,oracle,(a),x,\n"
    make_repo(tmp_path, registry=bad_registry)
    trace = TRACE_HEADER + row("HW-SF-002", "sim", "sim-pending",
                               credibility="CL0", oracle="")
    make_repo(tmp_path, trace=trace, registry=bad_registry)
    result = run_checker(tmp_path)
    assert result.returncode == 1
    assert "OR-xxx" in result.stdout


def test_gate_missing_inputs_and_usage(tmp_path):
    """Usage errors and missing HwRS/registry/ledger fail the gate."""
    assert check_hw.main(["checker"]) == 1
    assert check_hw.main(["checker", "/nowhere/at/all"]) == 1

    empty = tmp_path / "empty"
    empty.mkdir()
    result = run_checker(empty)
    assert result.returncode == 1
    assert "HwRS.md is missing" in result.stdout
    assert "oracle registry" in result.stdout
    assert "traceability.csv is missing" in result.stdout

    # Empty registry and ledger files are malformed (missing headers).
    make_repo(tmp_path)
    write(tmp_path / "hw" / "tests" / "oracles" / "registry.csv", "")
    write(tmp_path / "hw" / "tests" / "traceability.csv", "")
    result = run_checker(tmp_path)
    assert result.returncode == 1
    assert "expected" in result.stdout


def test_row_schema_validation_paths(tmp_path):
    """Row schema violations are reported when jsonschema is available."""
    pytest.importorskip("jsonschema")
    make_repo(tmp_path)
    assert check_hw.row_schema_violations(clean_row(), str(tmp_path)) == []

    bad = dict(clean_row(), method="flying", evidence_sha256="nothex")
    violations = check_hw.row_schema_violations(bad, str(tmp_path))
    assert violations and all(v.startswith("row schema: ")
                              for v in violations)

    # An unreadable schema is a violation, never a silent skip.
    make_repo(tmp_path / "noschema", with_schema=False)
    violations = check_hw.row_schema_violations(clean_row(),
                                                str(tmp_path / "noschema"))
    assert violations and "cannot be loaded" in violations[0]


def test_row_schema_skips_deep_check_without_jsonschema(tmp_path, monkeypatch):
    """Without jsonschema the deep check degrades (native rules stay on)."""
    monkeypatch.setitem(sys.modules, "jsonschema", None)
    make_repo(tmp_path)
    assert check_hw.row_schema_violations(clean_row(), str(tmp_path)) == []


def test_gate_remaining_error_paths(tmp_path):
    """HW-TRACE-GATE-015: field-count, empty-field and digest error paths."""
    base_status = "passing(sim,CL2,provisional)"
    # (a) registry row with the wrong field count.
    make_repo(tmp_path, registry="oracle_id,oracle,class,serves,validation_gap\nOR-001,two\n")
    result = run_checker(tmp_path)
    assert result.returncode == 1
    assert "has 2 fields, expected 5" in result.stdout

    # (b) ledger row with the wrong field count.
    make_repo(tmp_path, trace=TRACE_HEADER + "HW-SF-002,sim\n")
    result = run_checker(tmp_path)
    assert result.returncode == 1
    assert "has 2 fields, expected 10" in result.stdout

    # (c) empty required field in a ledger row.
    make_repo(tmp_path, trace=TRACE_HEADER + "HW-SF-002,,,CL0,,CL3,draft,,,\n")
    result = run_checker(tmp_path)
    assert result.returncode == 1
    assert "empty 'method' field" in result.stdout

    # (d) evidence digest that is not sha256:<64 hex>.
    make_repo(tmp_path)
    trace = (TRACE_HEADER
             + row("HW-SF-002", "sim", base_status, evidence=EVIDENCE_REL,
                   digest="sha256:xyz"))
    make_repo(tmp_path, trace=trace)
    result = run_checker(tmp_path)
    assert result.returncode == 1
    assert "not sha256:<64 hex>" in result.stdout

    # (e) evidence artifact that is not valid JSON.
    make_repo(tmp_path)
    write(tmp_path / EVIDENCE_REL, "not json")
    digest = sha256_bytes("not json")
    trace = (TRACE_HEADER
             + row("HW-SF-002", "sim", base_status, evidence=EVIDENCE_REL,
                   digest=digest))
    write(tmp_path / "hw" / "tests" / "traceability.csv", trace)
    result = run_checker(tmp_path)
    assert result.returncode == 1
    assert "is not valid JSON" in result.stdout

    # (f) a source pinned by the evidence does not exist.
    make_repo(tmp_path)
    document = make_evidence(sources={"hw/gone/missing.py": "x"})
    digest = write_evidence(tmp_path, document)
    trace = (TRACE_HEADER
             + row("HW-SF-002", "sim", base_status, evidence=EVIDENCE_REL,
                   digest=digest))
    write(tmp_path / "hw" / "tests" / "traceability.csv", trace)
    result = run_checker(tmp_path)
    assert result.returncode == 1
    assert "evidence source hw/gone/missing.py is missing" in result.stdout

    # (g) credibility_level outside CL0..CL3.
    make_repo(tmp_path)
    trace = TRACE_HEADER + row("HW-SF-002", "sim", "sim-pending",
                               credibility="CLX", oracle="")
    make_repo(tmp_path, trace=trace)
    result = run_checker(tmp_path)
    assert result.returncode == 1
    assert "not CL0..CL3" in result.stdout

    # (h) oracle_id shaped wrong (pending row may pre-declare an oracle).
    make_repo(tmp_path)
    trace = TRACE_HEADER + row("HW-SF-002", "sim", "sim-pending",
                               credibility="CL0", oracle="OR-x")
    make_repo(tmp_path, trace=trace)
    result = run_checker(tmp_path)
    assert result.returncode == 1
    assert "OR-xxx" in result.stdout

    # (i) fully-verified(CL3) without CL3 credibility.
    sources = {ORACLE_SOURCE: "oracle source\n"}
    make_repo(tmp_path, sources=sources)
    trace = evidence_row(tmp_path, sources, method="bench",
                         status="fully-verified(CL3)", credibility="CL2",
                         requirement="HW-SF-001")
    write(tmp_path / "hw" / "tests" / "traceability.csv", trace)
    result = run_checker(tmp_path)
    assert result.returncode == 1
    assert "requires credibility_level CL3, row has CL2" in result.stdout


def test_helper_semantics():
    assert check_hw.credibility_level("CL0") == 0
    assert check_hw.credibility_level("CL3") == 3
    assert check_hw.credibility_level("CL9") is None
    assert check_hw.credibility_level("x") is None
    assert check_hw.is_passing("passing(sim,CL2)")
    assert check_hw.is_passing("passing(analysis)")
    assert check_hw.is_passing("fully-verified(CL3)")
    assert not check_hw.is_passing("sim-pending")
    assert not check_hw.is_passing("draft")


def test_cli_on_repository_tree(monkeypatch):
    """The real H-01 ledger passes end-to-end as an actual CLI process."""
    result = run_checker(REPO_ROOT, use_subprocess=True)
    assert result.returncode == 0, result.stdout
    assert "PASS: the hardware ledger is consistent" in result.stdout

    # Also cover the normal ``main()`` entry point, which reads sys.argv.
    monkeypatch.setattr(sys, "argv", ["check_hw_traceability.py",
                                       str(REPO_ROOT)])
    assert check_hw.main() == 0


# H-04: HW-SF-002 / HW-FR-004 inherited tool gaps and independent witnesses.
def tool_gap(root, tool='openmodelica'):
    report = check_hw.Report()
    tools = check_hw.load_tool_qualifications(root, report)
    assert report.ok, report.failures
    return f"{tool}: {tools[tool]['gap']}"


def install_tool_evidence(root, document, gap=''):
    digest = write_evidence(root, document)
    write(root / 'hw/tests/traceability.csv', TRACE_HEADER + row(
        'HW-SF-002', 'virtual_bench', 'passing(sim,CL2,provisional)',
        evidence=EVIDENCE_REL, digest=digest, gap=gap))


@pytest.mark.parametrize('tcl,gap_value,passes', [
    ('TCL1', '', True), ('TCL1', 'spurious gap', False),
    ('TCL2', '', False), ('TCL2', 'wrong gap', False), ('TCL2', 'exact', True),
    ('TCL3', '', False), ('TCL3', 'exact', True),
])
def test_tcl_gap_inheritance(tmp_path, tcl, gap_value, passes):
    make_repo(tmp_path)
    if tcl == 'TCL3':
        path = tmp_path / check_hw.QUALIFICATION_PATH
        path.write_text(path.read_text().replace('| TI2 | TD2 | TCL2 |', '| TI2 | TD3 | TCL3 |'))
    document = make_evidence()
    document['tool_pins'] = {'capellambse': '0.6.17'} if tcl == 'TCL1' else {
        'openmodelica': '1.24', 'fmpy': '0.3.24', 'python': '>=3.10', 'numpy': '2.1.3'}
    gap = tool_gap(tmp_path) if gap_value == 'exact' else gap_value
    install_tool_evidence(tmp_path, document, gap)
    result = run_checker(tmp_path)
    assert result.returncode == (0 if passes else 1), result.stdout
    if not passes:
        assert 'inherited_validation_gap' in result.stdout


def test_multiple_tool_gaps_are_sorted(tmp_path):
    make_repo(tmp_path)
    path = tmp_path / check_hw.QUALIFICATION_PATH
    path.write_text(path.read_text().replace('<!-- END TOOL CLASSIFICATION -->',
        '| auxiliary | Fixture compiler | TI2 | TD3 | TCL3 | Auxiliary gap. |\n<!-- END TOOL CLASSIFICATION -->'))
    document = make_evidence()
    document['tool_pins'] = {'openmodelica': '1.24', 'auxiliary': '1'}
    install_tool_evidence(tmp_path, document, 'auxiliary: Auxiliary gap. | ' + tool_gap(tmp_path))
    assert run_checker(tmp_path).returncode == 0


@pytest.mark.parametrize('pins', [None, {}, {'python': '3.11'},
                                 {'unknown': '1'}, {'renode': 'pending'},
                                 {'openmodelica': ''}])
def test_unqualified_or_missing_tool_pins_fail(tmp_path, pins):
    make_repo(tmp_path)
    document = make_evidence()
    if pins is None:
        document.pop('tool_pins')
    else:
        document['tool_pins'] = pins
    install_tool_evidence(tmp_path, document)
    result = run_checker(tmp_path)
    assert result.returncode == 1
    assert 'rule 8:' in result.stdout


def test_modelica_artifact_cannot_hide_compiler(tmp_path):
    source = {'hw/model/example.mo': 'model Example end Example;'}
    make_repo(tmp_path, sources=source)
    document = make_evidence(sources=source)
    install_tool_evidence(tmp_path, document)
    result = run_checker(tmp_path)
    assert result.returncode == 1 and 'must declare the OpenModelica' in result.stdout


def test_pending_row_has_no_inherited_artifact_gap(tmp_path):
    make_repo(tmp_path, trace=TRACE_HEADER + row(
        'HW-SF-002', 'bench', 'bench-pending', oracle='', gap='not an artifact'))
    result = run_checker(tmp_path)
    assert result.returncode == 1 and 'pending row' in result.stdout


def test_legacy_method_and_nonexistent_cl_rejected(tmp_path):
    make_repo(tmp_path, trace=TRACE_HEADER + row('HW-SF-002', 't2', 'sim-pending', credibility='CL4'))
    result = run_checker(tmp_path)
    assert result.returncode == 1 and 'method' in result.stdout and 'CL4' in result.stdout


@pytest.mark.parametrize('change', [
    lambda s: '',
    lambda s: s.replace('tool_id | Tool / role and rationale', 'old_header | Tool'),
    lambda s: s.replace('<!-- END TOOL CLASSIFICATION -->', '<!-- BEGIN TOOL CLASSIFICATION -->'),
    lambda s: s.replace('| TI2 | TD2 | TCL2 |', '| TI2 | TD2 | TCL9 |'),
    lambda s: s.replace('| TI2 | TD2 | TCL2 |', '| TI2 | TD1 | TCL2 |'),
    lambda s: s.replace('openmodelica |', 'fmpy |'),
    lambda s: s.replace('| openmodelica |', '| other_compiler |'),
    lambda s: s.replace('| TI2 | TD2 | TCL2 | Compiler semantics outside independently validated output remain unqualified; OR-001/OR-002 regressions do not cover all translation and solver behavior. |', '| TI2 | TD2 | TCL2 | - |'),
    lambda s: s.replace('| TI2 | TD1 | TCL1 | - |', '| TI2 | TD1 | TCL1 | not empty |'),
    lambda s: s.replace('| TI2 | TD2 | TCL2 |', '| TI2 | TCL2 |'),
])
def test_qualification_table_fails_closed(tmp_path, change):
    make_repo(tmp_path)
    path = tmp_path / check_hw.QUALIFICATION_PATH
    path.write_text(change(path.read_text()))
    report = check_hw.Report()
    assert check_hw.load_tool_qualifications(tmp_path, report) == {}
    assert not report.ok


def make_independent_witness(root):
    sources = {ORACLE_SOURCE: 'independent analytical reference fixture\n',
               'hw/tests/evidence/output.csv': 'time,v\n0,3.3\n1,3.2\n'}
    make_repo(root, sources=sources)
    output_hash = sha256_bytes(sources['hw/tests/evidence/output.csv'])
    document = make_evidence(sources=sources)
    document['tool_pins'] = {'openmodelica': '1.24', 'fmpy': '0.3.24'}
    witness = {'oracle_id': 'OR-001', 'pass': True, 'scope': 'full_output',
               'output_sha256': output_hash, 'independent_of': ['openmodelica'],
               'source_hashes': {ORACLE_SOURCE: sha256_bytes(sources[ORACLE_SOURCE])},
               'tool_pins': {'python': '3.11'}}
    ref = {'tool': 'openmodelica', 'oracle_id': 'OR-001',
           'output': 'hw/tests/evidence/output.csv', 'output_sha256': output_hash,
           'witness': 'hw/tests/evidence/independent.json', 'witness_sha256': ''}
    document['independent_tool_validation'] = [ref]
    return document, ref, witness


def publish_witness(root, document, ref, witness):
    path = write(root / ref['witness'], json.dumps(witness, sort_keys=True))
    ref['witness_sha256'] = sha256_bytes(path.read_text())
    install_tool_evidence(root, document)


def test_hashed_independent_full_output_witness_can_waive_tool_gap(tmp_path):
    document, ref, witness = make_independent_witness(tmp_path)
    publish_witness(tmp_path, document, ref, witness)
    result = run_checker(tmp_path)
    assert result.returncode == 0, result.stdout


@pytest.mark.parametrize('change', [
    lambda d, r, w: d['independent_tool_validation'].append(dict(r)),
    lambda d, r, w: r.update(tool='unknown'),
    lambda d, r, w: r.update(tool='fmpy'),
    lambda d, r, w: r.update(oracle_id='OR-999'),
    lambda d, r, w: r.update(oracle_id='OR-002'),  # wrong serves requirement
    lambda d, r, w: d['source_hashes'].pop(r['output']),
    lambda d, r, w: r.update(output_sha256='sha256:'+'f'*64),
    lambda d, r, w: w.update(scope='partial'),
    lambda d, r, w: w.update(**{'pass': False}),
    lambda d, r, w: w.update(oracle_id='OR-002'),
    lambda d, r, w: w.update(output_sha256='sha256:'+'f'*64),
    lambda d, r, w: w.update(independent_of=['fmpy']),
    lambda d, r, w: w.update(tool_pins={'openmodelica': '1.24'}),
    lambda d, r, w: w.update(tool_pins={'unknown': '1'}),
    lambda d, r, w: w.update(source_hashes={}),
    lambda d, r, w: w['source_hashes'].update({ORACLE_SOURCE: 'sha256:'+'f'*64}),
    lambda d, r, w: w['source_hashes'].update({'missing.py': 'sha256:'+'f'*64}),
    lambda d, r, w: w.update(extra='not schema valid'),
])
def test_invalid_waivers_fail_closed(tmp_path, change):
    document, ref, witness = make_independent_witness(tmp_path)
    change(document, ref, witness)
    publish_witness(tmp_path, document, ref, witness)
    result = run_checker(tmp_path)
    assert result.returncode == 1, result.stdout
    assert 'rule 8:' in result.stdout


def test_waiver_artifacts_must_match_hashes(tmp_path):
    document, ref, witness = make_independent_witness(tmp_path)
    publish_witness(tmp_path, document, ref, witness)
    (tmp_path / ref['output']).write_text('tampered')
    assert 'waiver output/witness hash mismatch' in run_checker(tmp_path).stdout
    document, ref, witness = make_independent_witness(tmp_path)
    publish_witness(tmp_path, document, ref, witness)
    (tmp_path / ref['witness']).write_text('{}')
    assert 'waiver output/witness hash mismatch' in run_checker(tmp_path).stdout


@pytest.mark.parametrize('path', ['/tmp/outside', '../outside'])
def test_artifact_path_escape_rejected(tmp_path, path):
    with pytest.raises(ValueError, match='repository-relative'):
        check_hw._repo_artifact(tmp_path, path)


def test_symlink_escape_rejected(tmp_path):
    (tmp_path / 'link').symlink_to(tmp_path.parent)
    with pytest.raises(ValueError, match='escapes repository'):
        check_hw._repo_artifact(tmp_path, 'link/outside')


def test_authoritative_registry_and_export_drift(tmp_path):
    make_repo(tmp_path, trace=TRACE_HEADER)
    registry = tmp_path / 'hw/tests/oracles/registry.json'
    document = json.loads(registry.read_text())
    document['oracles'][0]['description'] = 'Changed registry text'
    registry.write_text(json.dumps(document))
    result = run_checker(tmp_path)
    assert result.returncode == 1 and 'differs from authoritative' in result.stdout
    registry.unlink()
    result = run_checker(tmp_path)
    assert result.returncode == 1 and 'Authoritative oracle registry' in result.stdout


@pytest.mark.parametrize('document', [[], {'source_hashes': []}])
def test_invalid_evidence_shape_rejected(tmp_path, document):
    make_repo(tmp_path)
    install_tool_evidence(tmp_path, document)
    result = run_checker(tmp_path)
    assert result.returncode == 1 and 'must be' in result.stdout


@pytest.fixture
def pulse_repo(tmp_path):
    """Real HW-FR-004 source/metadata chain, isolated for promotion negatives."""
    import shutil
    for directory in ('hw', 'ci/docker', 'schemas/hw', 'docs/hw'):
        shutil.copytree(REPO_ROOT / directory, tmp_path / directory,
                        ignore=shutil.ignore_patterns('__pycache__', '*.pyc'))
    return tmp_path


def change_pulse_manifest(root, change):
    path = root / check_hw.PULSE_EVIDENCE_RELATIVE_PATH
    document = json.loads(path.read_text())
    change(document)
    path.write_text(json.dumps(document))


def test_pending_pulse_manifest_is_still_checked(pulse_repo):
    result = run_checker(pulse_repo)
    assert result.returncode == 0, result.stdout
    assert 'passing 1, pending 3' in result.stdout


@pytest.mark.parametrize('pulse', ['pulse4', 'pulse5b'])
def test_partial_pulse_cannot_be_passing(pulse_repo, pulse):
    change_pulse_manifest(pulse_repo, lambda d: d['pulses'][pulse].update(status='passing'))
    result = run_checker(pulse_repo)
    assert result.returncode == 1 and 'Pulse qualification' in result.stdout


def test_partial_aggregate_cannot_be_passing(pulse_repo):
    change_pulse_manifest(pulse_repo, lambda d: d.update(status='passing', **{'pass':True}, credibility_level='CL2'))
    result = run_checker(pulse_repo)
    assert result.returncode == 1 and 'Pulse qualification' in result.stdout


@pytest.mark.parametrize('change', [
    lambda d: d.update(**{'pass':True}),
    lambda d: d.update(provisional=False),
    lambda d: d.update(credibility_level='CL2'),
    lambda d: d['pulses']['pulse4'].update(provisional=False),
    lambda d: d['pulses'].pop('pulse5b'),
    lambda d: d['pulses']['pulse5b'].update(extra='undeclared'),
    lambda d: d.update(inherited_validation_gap='invented gap'),
])
def test_pulse_pending_contract_and_inherited_gap(pulse_repo, change):
    change_pulse_manifest(pulse_repo, change)
    assert run_checker(pulse_repo).returncode == 1


def test_pending_pulse_sources_are_hash_checked(pulse_repo):
    path = pulse_repo / 'hw/model/CancestryLib/Power/PulseISO7637_2.mo'
    path.write_text(path.read_text()+'\n// source drift\n')
    result = run_checker(pulse_repo)
    assert result.returncode == 1 and 'pulse evidence source' in result.stdout


def test_pending_pulse_requires_manifest(pulse_repo):
    (pulse_repo / check_hw.PULSE_EVIDENCE_RELATIVE_PATH).unlink()
    result = run_checker(pulse_repo)
    assert result.returncode == 1 and 'Pulse qualification' in result.stdout


@pytest.mark.parametrize('tamper', ['promote', 'remove', 'duplicate', 'credibility'])
def test_pulse_ledger_must_agree_with_pending_manifest(pulse_repo, tamper):
    path = pulse_repo / 'hw/tests/traceability.csv'
    rows = list(csv.DictReader(io.StringIO(path.read_text())))
    pulse = next(r for r in rows if r['oracle_id'] == 'OR-002')
    if tamper == 'promote':
        pulse['status'] = 'passing(sim,CL2,provisional)'
        pulse['credibility_level'] = 'CL2'
    elif tamper == 'remove':
        rows.remove(pulse)
    elif tamper == 'duplicate':
        rows.append(dict(pulse))
    else:
        pulse['credibility_level'] = 'CL2'
    with path.open('w', newline='') as handle:
        writer = csv.DictWriter(handle, fieldnames=check_hw.CSV_HEADER)
        writer.writeheader()
        writer.writerows(rows)
    result = run_checker(pulse_repo)
    assert result.returncode == 1 and 'Pulse qualification' in result.stdout


def test_complete_manifest_requires_matching_passing_ledger(pulse_repo):
    # Synthetic metadata fixture, not a promotion of repository evidence.
    def complete(document):
        document.update(status='passing', credibility_level='CL2', **{'pass':True})
        for pulse in document['pulses'].values():
            pulse.update(coverage='complete', status='passing', oracle_credibility='CL2')
    change_pulse_manifest(pulse_repo, complete)
    result = run_checker(pulse_repo)
    assert result.returncode == 1
    assert 'passing pulse manifest and ledger' in result.stdout


def test_complete_fixture_with_matching_ledger(pulse_repo):
    # Exercise the future legitimate path without inventing real qualification.
    def complete(document):
        document.update(status='passing', credibility_level='CL2', **{'pass':True})
        for pulse in document['pulses'].values():
            pulse.update(coverage='complete', status='passing', oracle_credibility='CL2')
    change_pulse_manifest(pulse_repo, complete)
    evidence_path = pulse_repo / check_hw.PULSE_EVIDENCE_RELATIVE_PATH
    document = json.loads(evidence_path.read_text())
    path = pulse_repo / 'hw/tests/traceability.csv'
    rows = list(csv.DictReader(io.StringIO(path.read_text())))
    pulse = next(r for r in rows if r['oracle_id'] == 'OR-002')
    pulse.update(status='passing(sim,CL2,provisional)', credibility_level='CL2',
                 evidence=check_hw.PULSE_EVIDENCE_RELATIVE_PATH,
                 evidence_sha256=check_hw.sha256_file(evidence_path),
                 inherited_validation_gap=document['inherited_validation_gap'])
    with path.open('w', newline='') as handle:
        writer = csv.DictWriter(handle, fieldnames=check_hw.CSV_HEADER)
        writer.writeheader()
        writer.writerows(rows)
    result = run_checker(pulse_repo)
    assert result.returncode == 0, result.stdout


@pytest.mark.parametrize('target', ['aggregate', 'pulse4', 'pulse5b'])
@pytest.mark.parametrize('run_status', ['failing', 'failed'])
def test_pulse_qualification_manifest_rejects_run_result_status(pulse_repo, target, run_status):
    """HW-FR-004 / F3: failed runs cannot masquerade as qualification manifests."""
    def change(document):
        record = document if target == 'aggregate' else document['pulses'][target]
        record['status'] = run_status
    change_pulse_manifest(pulse_repo, change)
    result = run_checker(pulse_repo)
    assert result.returncode == 1
    assert 'Pulse qualification' in result.stdout and 'is not one of' in result.stdout
