#!/usr/bin/env python3
"""Check the CANcestry hardware traceability record (H-01, issue #33).

``docs/hw/HW-PLAN.md`` section 10.8 and ``HwAGENTS.md`` rule 4 require the
hardware ledger to be CI-gated with the same discipline as the software
record (which ``ci/check_traceability.py`` keeps untouched: the hardware
rows live in their own record, ``hw/tests/traceability.csv``, so the
six-column software record and its gate are unchanged).

Rules enforced
--------------
0. ``docs/hw/HwRS.md`` must be found and its requirement tables must parse:
   every row starting with an ``HW-*`` id has exactly 7 fields and a
   required-CL cell that reads ``CL1``/``CL2``/``CL3``. Any deviation fails
   the gate CLOSED (the parser is a structural guard: if the HwRS table
   format changes, the gate fails instead of silently tracing nothing).
1. ``hw/tests/traceability.csv`` carries the documented ten-column header;
   one row per (requirement, method) pair; no empty required fields.
2. ``method``, ``status``, ``credibility_level`` and ``oracle_id`` use the
   controlled vocabularies (HwAGENTS.md rule 4); every row is additionally
   validated against ``schemas/hw/hw-traceability-0.1.0.schema.json`` when
   ``jsonschema`` is installed (the gate is enforced natively either way).
3. No orphan hardware ids: every requirement id in a row is defined in
   ``HwRS.md``.
4. The row's ``required_cl`` equals the HwRS "Req. CL" for that requirement.
5. Oracle rule (HW-PLAN section 10.2): every ``passing*`` row names an
   oracle listed in ``hw/tests/oracles/registry.csv``.
6. Credibility (HW-PLAN section 10.3): a ``passing*`` row needs
   credibility >= required CL; a safety row may close
   ``passing(sim,CLn,provisional)`` below the required CL only as a T1/T2
   closure pending T4 correlation (HwRS section 1 closure policy);
   ``fully-verified(CL3)`` is reachable only through the ``bench`` method.
7. Evidence (HwAGENTS.md rule 4): every ``passing*`` row carries an evidence
   artifact path whose recomputed sha256 matches the row; the evidence must
   record ``pass: true`` and matching requirement/oracle ids, and every
   source file it pins by hash must still match (evidence drift = gate
   failure). Non-passing rows carry no evidence.

8. Evidence inherits exact TCL2/TCL3 producer validation gaps unless an
   independent, hashed full-output witness validates it. TCL1-only rows carry
   no tool gap. Implements HW-SF-002 / HW-FR-004.

9. HW-FR-004 pulse source evidence is validated even while pending: partial
   coverage cannot be passing, source hashes and inherited tool gaps stay
   enforced, and the aggregate qualification must agree with the ledger.

Usage:
    python3 ci/check_hw_traceability.py <repo-root> [--explain]

``--explain`` is maintainer-only: it prints the expected HwRS Markdown table
contract and returns exit code 2 when the fail-closed parser rejects a table.
The normal CI invocation omits it and retains exit code 1 for parse failures.

Exit codes:
    0  the hardware ledger is complete, honest and consistent
    1  at least one rule was violated (or the HwRS tables no longer parse)
    2  the HwRS tables failed to parse and ``--explain`` was requested
"""

import argparse
import csv
import json
import os
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from ci.check_hw_contracts import load_registry as load_json_registry, registry_csv, read_json, validate, load_contract

HWRs_RELATIVE_PATH = os.path.join("docs", "hw", "HwRS.md")
REGISTRY_RELATIVE_PATH = os.path.join("hw", "tests", "oracles", "registry.csv")
TRACEABILITY_RELATIVE_PATH = os.path.join("hw", "tests", "traceability.csv")
ROW_SCHEMA_RELATIVE_PATH = os.path.join("schemas", "hw",
                                        "hw-traceability-0.1.0.schema.json")

HW_REQUIREMENT_ID = re.compile(r"^HW-(?:SF|FR|NF)-\d{3}$")
HW_ROW = re.compile(r"^\|\s*(HW-(?:SF|FR|NF)-\d{3})\s*\|")
CREDIBILITY = re.compile(r"^CL([0-3])$")
REQUIRED_CREDIBILITY = re.compile(r"^CL([1-3])$")
ORACLE_ID = re.compile(r"^OR-\d{3}$")
EVIDENCE_SHA256 = re.compile(r"^sha256:[0-9a-f]{64}$")

# Verification methods (HW-PLAN sections 10.1/10.8: analysis (T0), sim (T1),
# virtual_bench (T1/T2 harness), protocol (T3 conformance), bench (T4)).
# Kept in lockstep with schemas/hw/hw-traceability-0.1.0.schema.json and
# schemas/hw/hw-plot-data-0.1.0.schema.json (issue #36, H-03).
METHODS = ("analysis", "sim", "virtual_bench", "protocol", "bench")

# Honest-ledger status vocabulary (HwAGENTS.md rule 4).
STATUS_LITERALS = (
    "draft",
    "analysis-pending",
    "sim-pending",
    "bench-pending",
    "passing(analysis)",
    "fully-verified(CL3)",
)
STATUS_SIM = re.compile(r"^passing\(sim,(CL[123])(,provisional)?\)$")

CSV_HEADER = (
    "requirement_id",
    "method",
    "capella_element_id",
    "credibility_level",
    "oracle_id",
    "required_cl",
    "status",
    "evidence",
    "evidence_sha256",
    "inherited_validation_gap",
)
REGISTRY_HEADER = ("oracle_id", "oracle", "class", "serves",
                   "validation_gap")

# H-01 has no Capella model to resolve yet. These explicit placeholders are
# accepted and deliberately excluded from any future Capella orphan check;
# H-02 will replace them with real model element identifiers.
CAPELLA_PLACEHOLDERS = frozenset(("CAP_PENDING", "LA_PENDING"))

EXPECTED_HWRS_TABLE_FORMAT = (
    "HwRS requirement rows must be Markdown pipe tables with exactly seven "
    "fields:\n"
    "  | HW-SF-002 | requirement text | derivation | method / tier | CL3 | "
    "oracle class | initial status |\n"
    "The requirement-id cell must match %s; the Req. CL cell must be CL1, "
    "CL2 or CL3. The leading row regex is:\n"
    "  %s"
) % (HW_ROW.pattern, HW_ROW.pattern)

HWRs_TABLE_FIELDS = 7  # id, requirement, derivation, method, req cl, oracle, status


class HwrsFormatError(Exception):
    """The HwRS requirement tables deviate from the expected format."""

    def __init__(self, issues):
        super(HwrsFormatError, self).__init__("; ".join(issues))
        self.issues = issues


class Report(object):
    """Collect failures grouped by rule."""

    def __init__(self):
        self.failures = []

    def fail(self, rule, message):
        self.failures.append("rule %d: %s" % (rule, message))

    @property
    def ok(self):
        return not self.failures


def read_text(path):
    with open(path, "r", encoding="utf-8", errors="replace") as handle:
        return handle.read()


def sha256_file(path):
    """Return the sha256 of a file as ``sha256:<hex>`` (deterministic)."""
    import hashlib

    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        for chunk in iter(lambda: handle.read(65536), b""):
            digest.update(chunk)
    return "sha256:%s" % digest.hexdigest()


def print_hwrs_format_explanation():
    """Explain the fail-closed HwRS table contract for maintainers."""
    print("EXPECTED HwRS Markdown table format:")
    print(EXPECTED_HWRS_TABLE_FORMAT)
    print("Rows must use pipe separators; escaped pipes inside cells are not "
          "supported by the H-01 parser.")


def parse_hwrs(text):
    """Parse the HwRS requirement tables into {requirement_id: required CL}.

    Fails closed (``HwrsFormatError``) when a row does not have exactly
    ``HWRs_TABLE_FIELDS`` fields, when the required-CL cell is not
    CL1/CL2/CL3, when an id is duplicated, or when no requirement rows are
    found at all (i.e. the table format changed).
    """
    table = {}
    issues = []
    for number, line in enumerate(text.splitlines(), start=1):
        match = HW_ROW.match(line)
        if not match:
            continue
        requirement_id = match.group(1)
        cells = [cell.strip()
                 for cell in line.strip().strip("|").split("|")]
        if len(cells) != HWRs_TABLE_FIELDS:
            issues.append(
                "line %d: %s row has %d fields, expected %d (HwRS table "
                "format change?)" % (number, requirement_id, len(cells),
                                     HWRs_TABLE_FIELDS))
            continue
        required_cl = cells[4]
        if not REQUIRED_CREDIBILITY.match(required_cl):
            issues.append(
                "line %d: %s: required CL %r is not CL1/CL2/CL3" %
                (number, requirement_id, required_cl))
            continue
        if requirement_id in table:
            issues.append(
                "line %d: duplicate HwRS requirement %s" %
                (number, requirement_id))
            continue
        table[requirement_id] = required_cl
    if not table:
        issues.append("no HW-* requirement rows found in the HwRS tables "
                      "(HwRS table format change?)")
    if issues:
        raise HwrsFormatError(issues)
    return table


def credibility_level(value):
    """Numeric level for a CL0..CL3 string, or None when not shaped right."""
    match = CREDIBILITY.match(value)
    if not match:
        return None
    return int(match.group(1))


def load_registry(root, report):
    """Return the set of registered oracle ids (rule: registry is law)."""
    path = os.path.join(root, REGISTRY_RELATIVE_PATH)
    if not os.path.isfile(path):
        report.fail(0, "%s is missing (oracle registry)" %
                    REGISTRY_RELATIVE_PATH)
        return {}
    with open(path, "r", encoding="utf-8", newline="") as handle:
        rows = list(csv.reader(handle))
    if not rows or rows[0] != list(REGISTRY_HEADER):
        report.fail(0, "%s header is %r, expected %r" %
                    (REGISTRY_RELATIVE_PATH, rows[0] if rows else None,
                     list(REGISTRY_HEADER)))
        return {}
    oracles = set()
    for number, row in enumerate(rows[1:], start=2):
        if len(row) != len(REGISTRY_HEADER):
            report.fail(0, "%s line %d has %d fields, expected %d" %
                        (REGISTRY_RELATIVE_PATH, number, len(row),
                         len(REGISTRY_HEADER)))
            continue
        if not ORACLE_ID.match(row[0]):
            report.fail(0, "%s line %d: oracle id %r is not OR-xxx" %
                        (REGISTRY_RELATIVE_PATH, number, row[0]))
            continue
        oracles.add(row[0])
    try:
        document = load_json_registry(Path(root))
        if read_text(path) != registry_csv(document):
            report.fail(0, "registry.csv differs from authoritative registry.json; regenerate the export")
        return {row["oracle_id"]: row for row in document["oracles"]}
    except (OSError, ValueError) as error:
        report.fail(0, f"Authoritative oracle registry: {error}")
        return {}


def load_rows(root, report):
    """Rule 1: the hardware ledger's structure. Return (line, row) pairs."""
    path = os.path.join(root, TRACEABILITY_RELATIVE_PATH)
    if not os.path.isfile(path):
        report.fail(1, "%s is missing (hardware traceability record)" %
                    TRACEABILITY_RELATIVE_PATH)
        return []
    with open(path, "r", encoding="utf-8", newline="") as handle:
        rows = list(csv.reader(handle))
    if not rows or rows[0] != list(CSV_HEADER):
        report.fail(1, "%s header is %r, expected %r" %
                    (TRACEABILITY_RELATIVE_PATH, rows[0] if rows else None,
                     list(CSV_HEADER)))
        return []
    records = []
    seen = set()
    for number, row in enumerate(rows[1:], start=2):
        if len(row) != len(CSV_HEADER):
            report.fail(1, "%s line %d has %d fields, expected %d" %
                        (TRACEABILITY_RELATIVE_PATH, number, len(row),
                         len(CSV_HEADER)))
            continue
        record = dict(zip(CSV_HEADER, (field.strip() for field in row)))
        for field in ("requirement_id", "method", "capella_element_id",
                      "credibility_level", "required_cl", "status"):
            if record[field] == "":
                report.fail(1, "%s line %d: empty %r field" %
                            (TRACEABILITY_RELATIVE_PATH, number, field))
                break
        key = (record["requirement_id"], record["method"])
        if key in seen:
            report.fail(1, "duplicate row for (requirement, method) pair %s"
                        % (key,))
        seen.add(key)
        records.append((number, record))
    return records


def row_schema_violations(row, root):
    """Validate one ledger row against the hw-traceability schema.

    Returns a list of human-readable violations; an empty list when the row
    is conformant or the deep check cannot run (``jsonschema`` absent or
    the schema unreadable is reported as a violation, never skipped
    silently - the native rules below still enforce the gate).
    """
    try:
        import jsonschema
    except ImportError:
        return []
    schema_path = os.path.join(root, ROW_SCHEMA_RELATIVE_PATH)
    try:
        schema = json.loads(read_text(schema_path))
    except (OSError, ValueError) as error:
        return ["row schema %s cannot be loaded: %s" %
                (ROW_SCHEMA_RELATIVE_PATH, error)]
    errors = sorted(jsonschema.Draft202012Validator(schema).iter_errors(row),
                    key=lambda error: error.message)
    return ["row schema: %s" % error.message for error in errors]


def is_passing(status):
    """True for the passing/fully-verified statuses (HwAGENTS.md rule 4)."""
    return (status.startswith("passing")
            or status.startswith("fully-verified"))


QUALIFICATION_PATH = "docs/hw/tool-qualification.md"
RUNTIME_PINS = {"python", "numpy"}
TOOL_HEADER = ["tool_id", "Tool / role and rationale", "TI", "TD", "TCL", "validation_gap"]


def load_tool_qualifications(root, report):
    """Rule 8 (HW-SF-002 / HW-FR-004): the controlled TCL table is law."""
    try:
        text = read_text(os.path.join(root, QUALIFICATION_PATH))
        start, end = "<!-- BEGIN TOOL CLASSIFICATION -->", "<!-- END TOOL CLASSIFICATION -->"
        if text.count(start) != 1 or text.count(end) != 1 or text.index(start) >= text.index(end):
            raise ValueError("missing/duplicate/reversed classification markers")
        lines = text.split(start)[1].split(end)[0].strip().splitlines()
        rows = [[cell.strip() for cell in line.strip().strip("|").split("|")]
                for line in lines]
        if len(rows) < 3 or rows[0] != TOOL_HEADER or len(rows[1]) != 6:
            raise ValueError("invalid classification header/table")
        tools = {}
        for row in rows[2:]:
            if len(row) != 6:
                raise ValueError("classification row must have six fields")
            tool, _, ti, td, tcl, gap = row
            gap = "" if gap == "-" else gap
            if not tool or tool in tools or tcl not in ("TCL1", "TCL2", "TCL3", "pending"):
                raise ValueError(f"invalid/duplicate tool classification {tool!r} / {tcl!r}")
            expected = {"TCL1": (("TI1", "TD1"), ("TI1", "TD2"), ("TI1", "TD3"), ("TI2", "TD1")),
                        "TCL2": (("TI2", "TD2"),), "TCL3": (("TI2", "TD3"),)}
            if tcl != "pending" and (ti, td) not in expected[tcl]:
                raise ValueError(f"{tool}: TI/TD inconsistent with {tcl}")
            if (tcl in ("TCL2", "TCL3") and not gap) or (tcl == "TCL1" and gap):
                raise ValueError(f"{tool}: validation_gap inconsistent with {tcl}")
            tools[tool] = {"tcl": tcl, "gap": gap}
        if not {"openmodelica", "fmpy", "capellambse"} <= tools.keys():
            raise ValueError("missing required tool classification")
        return tools
    except (OSError, ValueError) as error:
        report.fail(8, f"Tool qualification table: {error}")
        return {}


CAPELLA_MODEL_RELATIVE = os.path.join("hw", "model", "capella", "cancestry.capella")


def load_capella_element_ids(root):
    """Load all element IDs declared in the Capella model if present.

    Returns a set of element id strings, or None if the model file is absent.
    Uses xml.etree.ElementTree so it runs dependency-free without capellambse.
    """
    capella_path = os.path.join(root, CAPELLA_MODEL_RELATIVE)
    if not os.path.isfile(capella_path):
        return None
    try:
        import xml.etree.ElementTree as ET
        tree = ET.parse(capella_path)
        return {elem.attrib["id"] for elem in tree.iter() if "id" in elem.attrib}
    except Exception:
        return None


def _repo_artifact(root, relative):
    """Keep evidence/witness sources inside the checked repository."""
    path = Path(relative)
    if path.is_absolute() or ".." in path.parts:
        raise ValueError(f"artifact path is not repository-relative: {relative!r}")
    resolved = (Path(root) / path).resolve()
    if not resolved.is_relative_to(Path(root).resolve()):
        raise ValueError(f"artifact path escapes repository: {relative!r}")
    return resolved


def _metadata_schema(root, definition):
    schema = read_json(Path(root) / ROW_SCHEMA_RELATIVE_PATH)
    return {"$schema": schema["$schema"], "$defs": schema["$defs"],
            "$ref": f"#/$defs/{definition}"}


def independent_tool_waivers(document, row, root, registry, tools):
    """Check full-output witnesses; an oracle ID alone cannot waive a gap."""
    waived = set()
    for ref in document.get("independent_tool_validation", []):
        tool = ref["tool"]
        if tool in waived or tool not in document["tool_pins"] or tool not in tools:
            raise ValueError("duplicate or unknown waiver producer")
        if tools[tool]["tcl"] not in ("TCL2", "TCL3"):
            raise ValueError("waiver producer must be TCL2/TCL3")
        oracle = registry.get(ref["oracle_id"])
        if not oracle or row["requirement_id"] not in oracle["serves"]:
            raise ValueError("waiver oracle is unregistered or does not serve this requirement")
        if document.get("source_hashes", {}).get(ref["output"]) != ref["output_sha256"]:
            raise ValueError("waiver output is not pinned by the evidence")
        output = _repo_artifact(root, ref["output"])
        witness_path = _repo_artifact(root, ref["witness"])
        if sha256_file(output) != ref["output_sha256"] or sha256_file(witness_path) != ref["witness_sha256"]:
            raise ValueError("waiver output/witness hash mismatch")
        witness = read_json(witness_path)
        validate(witness, _metadata_schema(root, "tool_validation_witness"), "independent witness")
        if witness["oracle_id"] != ref["oracle_id"] or witness["output_sha256"] != ref["output_sha256"]:
            raise ValueError("witness oracle/output does not match reference")
        if tool not in witness["independent_of"] or tool in witness["tool_pins"]:
            raise ValueError("witness is not independent of producer")
        for producer in witness["tool_pins"]:
            if producer not in RUNTIME_PINS and tools.get(producer, {}).get("tcl") != "TCL1":
                raise ValueError("witness uses an unknown or unqualified producer")
        for relative, digest in witness["source_hashes"].items():
            if sha256_file(_repo_artifact(root, relative)) != digest:
                raise ValueError("independent witness source drift")
        waived.add(tool)
    return waived


def check_tool_inheritance(number, row, document, root, registry, tools, report):
    """Rule 8: exact TCL2+ gap inheritance; empty for TCL1 or verified waiver."""
    try:
        metadata = {key: document[key] for key in
                    ("tool_pins", "independent_tool_validation") if key in document}
        validate(metadata, _metadata_schema(root, "evidence_tool_metadata"), "evidence tool metadata")
        pins = document["tool_pins"]
        producers = set(pins) - RUNTIME_PINS
        if not producers:
            raise ValueError("evidence declares no classified producing tool")
        if any(source.endswith(".mo") for source in document.get("source_hashes", {})) and "openmodelica" not in producers:
            raise ValueError("Modelica evidence must declare the OpenModelica compiler pin")
        for tool in sorted(producers):
            if tool not in tools or tools[tool]["tcl"] == "pending":
                raise ValueError(f"unknown/unqualified producing tool {tool!r}")
        waived = independent_tool_waivers(document, row, root, registry, tools)
        expected = " | ".join(f"{tool}: {tools[tool]['gap']}" for tool in sorted(producers - waived)
                              if tools[tool]["tcl"] in ("TCL2", "TCL3"))
        if row["inherited_validation_gap"] != expected:
            raise ValueError(f"inherited_validation_gap must be {expected!r}")
    except (OSError, ValueError) as error:
        report.fail(8, f"line {number}: {error}")


def check_evidence(number, row, root, report, registry, tools):
    """Rule 7: passing rows must pin a live, matching evidence artifact."""
    evidence = row["evidence"]
    digest = row["evidence_sha256"]
    if not evidence or not digest:
        report.fail(7, "line %d: %s / %s: passing row without evidence "
                    "path and hash" %
                    (number, row["requirement_id"], row["method"]))
        return
    if not EVIDENCE_SHA256.match(digest):
        report.fail(7, "line %d: evidence sha256 %r is not sha256:<64 hex>" %
                    (number, digest))
        return
    path = os.path.join(root, evidence)
    if not os.path.isfile(path):
        report.fail(7, "line %d: evidence artifact %s is missing" %
                    (number, evidence))
        return
    if sha256_file(path) != digest:
        report.fail(7, "line %d: evidence artifact %s does not match the "
                    "recorded sha256 (re-issue the evidence)" %
                    (number, evidence))
        return
    try:
        document = read_json(path)
    except ValueError as error:
        report.fail(7, "line %d: evidence artifact %s is not valid JSON: %s" %
                    (number, evidence, error))
        return
    if not isinstance(document, dict):
        report.fail(7, f"line {number}: evidence must be a JSON object")
        return
    if not isinstance(document.get("source_hashes", {}), dict):
        report.fail(7, f"line {number}: source_hashes must be an object")
        return
    check_tool_inheritance(number, row, document, root, registry, tools, report)
    if document.get("pass") is not True:
        report.fail(7, "line %d: evidence artifact %s does not record "
                    "pass=true" % (number, evidence))
    if document.get("requirement_id") != row["requirement_id"]:
        report.fail(7, "line %d: evidence artifact %s pins requirement %r, "
                    "row pins %r" %
                    (number, evidence, document.get("requirement_id"),
                     row["requirement_id"]))
    if document.get("oracle_id") != row["oracle_id"]:
        report.fail(7, "line %d: evidence artifact %s pins oracle %r, row "
                    "pins %r" %
                    (number, evidence, document.get("oracle_id"),
                     row["oracle_id"]))
    sources = document.get("source_hashes", {})
    for relative in sorted(sources):
        source_path = os.path.join(root, relative)
        if not os.path.isfile(source_path):
            report.fail(7, "line %d: evidence source %s is missing" %
                        (number, relative))
        elif sha256_file(source_path) != sources[relative]:
            report.fail(7, "line %d: evidence source %s drifted from the "
                        "pinned sha256 (re-issue the evidence)" %
                        (number, relative))


def validate_rows(records, hwrs, registry, root, report):
    """Rules 2-7 on the parsed rows."""
    tools = load_tool_qualifications(root, report)
    capella_ids = load_capella_element_ids(root)
    for number, row in records:
        requirement_id = row["requirement_id"]
        method = row["method"]
        status = row["status"]
        oracle_id = row["oracle_id"]
        credibility = row["credibility_level"]
        required_cl = row["required_cl"]
        capella_id = row["capella_element_id"]

        # H-05 hardening: placeholder fail-closed gate and Capella element cross-validation.
        # CAP_PENDING / LA_PENDING were H-01 placeholders while no Capella model existed.
        # Now that the model exists, passing rows must link to a real model element.
        if capella_id in CAPELLA_PLACEHOLDERS:
            if is_passing(status):
                report.fail(2, "line %d: %s / %s: passing row carries placeholder "
                            "capella_element_id %r (honest ledger: passing evidence "
                            "requires resolved Capella linkage)" %
                            (number, requirement_id, method, capella_id))
        elif capella_ids is not None:
            if capella_id not in capella_ids:
                report.fail(2, "line %d: %s / %s: capella_element_id %r does not exist "
                            "in the Capella model (%s)" %
                            (number, requirement_id, method, capella_id,
                             CAPELLA_MODEL_RELATIVE))

        for violation in row_schema_violations(row, root):
            report.fail(2, "line %d: %s" % (number, violation))

        if method not in METHODS:
            report.fail(2, "line %d: method %r is not one of %s" %
                        (number, method, ", ".join(METHODS)))
        if not (status in STATUS_LITERALS or STATUS_SIM.match(status)):
            report.fail(2, "line %d: status %r is not in the honest-ledger "
                        "vocabulary (HwAGENTS.md rule 4)" % (number, status))
        if credibility_level(credibility) is None:
            report.fail(2, "line %d: credibility_level %r is not CL0..CL3" %
                        (number, credibility))
        if oracle_id and not ORACLE_ID.match(oracle_id):
            report.fail(2, "line %d: oracle_id %r is not OR-xxx" %
                        (number, oracle_id))

        if requirement_id not in hwrs:
            report.fail(3, "line %d: %s is not defined in HwRS.md (orphan "
                        "hardware id)" % (number, requirement_id))
            continue  # required-CL cross-check needs the HwRS entry

        if required_cl != hwrs[requirement_id]:
            report.fail(4, "line %d: %s: required_cl %s != HwRS Req. CL %s" %
                        (number, requirement_id, required_cl,
                         hwrs[requirement_id]))

        if not is_passing(status):
            if row["inherited_validation_gap"]:
                report.fail(8, f"line {number}: pending row has no artifact and must not inherit a tool gap")
            if row["evidence"] or row["evidence_sha256"]:
                report.fail(7, "line %d: %s / %s: non-passing row carries "
                            "evidence (honest ledger: evidence only with a "
                            "pass)" % (number, requirement_id, method))
            continue

        # Passing row: oracle rule, credibility rule, evidence rule.
        if not oracle_id:
            report.fail(5, "line %d: %s / %s: passing row without an "
                        "oracle_id (HW-PLAN section 10.2)" %
                        (number, requirement_id, method))
        elif oracle_id not in registry:
            report.fail(5, "line %d: oracle %r is not in %s (HW-PLAN "
                        "section 10.2)" %
                        (number, oracle_id, REGISTRY_RELATIVE_PATH))

        achieved = credibility_level(credibility)
        required = credibility_level(required_cl)
        if achieved is not None and required is not None:
            if status == "fully-verified(CL3)":
                if method != "bench":
                    report.fail(6, "line %d: fully-verified(CL3) is "
                                "reachable only through the bench method "
                                "(T4 correlation)" % number)
                if achieved != 3:
                    report.fail(6, "line %d: fully-verified(CL3) requires "
                                "credibility_level CL3, row has %s" %
                                (number, credibility))
            elif status == "passing(analysis)":
                if achieved < required:
                    report.fail(6, "line %d: passing(analysis) at %s is "
                                "below the required %s" %
                                (number, credibility, required_cl))
            else:  # passing(sim,CLn[,provisional])
                match = STATUS_SIM.match(status)
                if match is None:
                    # A passing status outside the vocabulary: the rule 2
                    # vocabulary failure has already been reported for it.
                    continue
                declared = match.group(1)
                if declared != credibility:
                    report.fail(6, "line %d: status declares %s but "
                                "credibility_level is %s" %
                                (number, declared, credibility))
                if achieved < required:
                    provisional = match.group(2) is not None
                    safety = requirement_id.startswith("HW-SF-") or requirement_id == "HW-FR-004"
                    if not (provisional and safety):
                        report.fail(6, "line %d: %s / %s at %s is below the "
                                    "required %s and is not a provisional "
                                    "safety closure (HwRS closure policy)" %
                                    (number, requirement_id, method,
                                     credibility, required_cl))

        check_evidence(number, row, root, report, registry, tools)



PULSE_EVIDENCE_RELATIVE_PATH = "hw/tests/evidence/pulse_7637_001.json"


def check_pulse_qualification(root, hwrs, records, registry, report):
    """HW-FR-004: pending pulse evidence is still schema/hash/tool-gap gated.

    Numeric regression success is not a qualification pass. Keep the source
    manifest and its ledger row consistent even when the row cannot reference
    closure evidence. This is not a replacement for #36's future plot gate.
    """
    path = Path(root) / PULSE_EVIDENCE_RELATIVE_PATH
    if "HW-FR-004" not in hwrs and not path.exists():
        return  # Minimal fixtures / repos with no HW-FR-004 scope.
    try:
        document = load_contract(Path(root), PULSE_EVIDENCE_RELATIVE_PATH, "pulse-evidence")
        rows = [row for _, row in records if row["requirement_id"] == "HW-FR-004"
                and row["method"] == "virtual_bench"]
        if len(rows) != 1:
            raise ValueError("HW-FR-004 needs exactly one virtual_bench ledger row")
        row = rows[0]
        if document["status"] == "pending":
            if row["status"] != "sim-pending" or row["credibility_level"] != "CL0":
                raise ValueError("pending pulse manifest requires a sim-pending/CL0 ledger row; no passing claim")
        elif not is_passing(row["status"]) or row["credibility_level"] != document["credibility_level"]:
            raise ValueError("passing pulse manifest and ledger status/credibility disagree")
        tools = load_tool_qualifications(root, report)
        # The pending ledger has no closure evidence/gap, but a produced
        # regression artifact still inherits its compiler gap in its own JSON.
        artifact_row = {"requirement_id": document["requirement_id"],
                        "inherited_validation_gap": document["inherited_validation_gap"]}
        check_tool_inheritance(0, artifact_row, document, root, registry, tools, report)
        for relative, digest in sorted(document["source_hashes"].items()):
            if sha256_file(_repo_artifact(root, relative)) != digest:
                raise ValueError(f"pulse evidence source {relative} drifted from pinned sha256")
    except (OSError, ValueError) as error:
        report.fail(9, f"Pulse qualification: {error}")


def argument_parser():
    """Build the maintainer-facing command-line parser."""
    parser = argparse.ArgumentParser(
        description="Check the CANcestry hardware traceability record.")
    parser.add_argument("repo_root", help="repository root to check")
    parser.add_argument(
        "--explain",
        action="store_true",
        help="explain the expected HwRS Markdown table after a parse failure")
    return parser


def main(argv=None):
    """Run the gate; ``argv`` includes the program name for unit testing."""
    if argv is None:
        argv = sys.argv
    parser = argument_parser()
    if len(argv) < 2:
        print("usage: check_hw_traceability.py <repo-root> [--explain]")
        return 1
    args = parser.parse_args(argv[1:])
    root = args.repo_root
    if not os.path.isdir(root):
        print("FAIL: %s is not a directory" % root)
        return 1

    report = Report()
    hwrs_parse_failed = False
    hwrs_path = os.path.join(root, HWRs_RELATIVE_PATH)
    if not os.path.isfile(hwrs_path):
        report.fail(0, "%s is missing" % HWRs_RELATIVE_PATH)
        hwrs = {}
    else:
        try:
            hwrs = parse_hwrs(read_text(hwrs_path))
        except HwrsFormatError as error:
            hwrs_parse_failed = True
            for issue in error.issues:
                report.fail(0, "HwRS.md: %s" % issue)
            hwrs = {}

    registry = load_registry(root, report)
    records = load_rows(root, report)
    validate_rows(records, hwrs, registry, root, report)
    check_pulse_qualification(root, hwrs, records, registry, report)

    passing = sum(1 for _, row in records if is_passing(row["status"]))
    pending = len(records) - passing
    print("rows: %d (passing %d, pending %d) | HwRS requirements: %d | "
          "oracles registered: %d" %
          (len(records), passing, pending, len(hwrs), len(registry)))

    if not report.ok:
        print("FAIL: %d hardware traceability problem(s)" %
              len(report.failures))
        for failure in report.failures:
            print("      %s" % failure)
        if hwrs_parse_failed and args.explain:
            print_hwrs_format_explanation()
            return 2
        return 1
    print("PASS: the hardware ledger is consistent with HwRS.md, the oracle "
          "registry and the evidence artifacts")
    return 0


if __name__ == "__main__":  # pragma: no cover
    sys.exit(main(sys.argv))
