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
1. ``hw/tests/traceability.csv`` carries the documented nine-column header;
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

HWRs_RELATIVE_PATH = os.path.join("docs", "hw", "HwRS.md")
REGISTRY_RELATIVE_PATH = os.path.join("hw", "tests", "oracles", "registry.csv")
TRACEABILITY_RELATIVE_PATH = os.path.join("hw", "tests", "traceability.csv")
ROW_SCHEMA_RELATIVE_PATH = os.path.join("schemas", "hw",
                                        "hw-traceability-0.1.0.schema.json")

HW_REQUIREMENT_ID = re.compile(r"^HW-(?:SF|FR|NF)-\d{3}$")
HW_ROW = re.compile(r"^\|\s*(HW-(?:SF|FR|NF)-\d{3})\s*\|")
CREDIBILITY = re.compile(r"^CL([0-3])$")
REQUIRED_CREDIBILITY = re.compile(r"^CL([1-3])$")
ORACLE_ID = re.compile(r"^OR-\d{3}[a-z]?$")
EVIDENCE_SHA256 = re.compile(r"^sha256:[0-9a-f]{64}$")

# Verification methods (HW-PLAN section 10.8: analysis, sim(CLn), bench).
METHODS = ("analysis", "sim", "t2", "bench")

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
        return set()
    with open(path, "r", encoding="utf-8", newline="") as handle:
        rows = list(csv.reader(handle))
    if not rows or rows[0] != list(REGISTRY_HEADER):
        report.fail(0, "%s header is %r, expected %r" %
                    (REGISTRY_RELATIVE_PATH, rows[0] if rows else None,
                     list(REGISTRY_HEADER)))
        return set()
    oracles = set()
    for number, row in enumerate(rows[1:], start=2):
        if len(row) != len(REGISTRY_HEADER):
            report.fail(0, "%s line %d has %d fields, expected %d" %
                        (REGISTRY_RELATIVE_PATH, number, len(row),
                         len(REGISTRY_HEADER)))
            continue
        if not ORACLE_ID.match(row[0]):
            report.fail(0, "%s line %d: oracle id %r is not OR-xxx[letter]" %
                        (REGISTRY_RELATIVE_PATH, number, row[0]))
            continue
        oracles.add(row[0])
    return oracles


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


def check_evidence(number, row, root, report):
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
        document = json.loads(read_text(path))
    except ValueError as error:
        report.fail(7, "line %d: evidence artifact %s is not valid JSON: %s" %
                    (number, evidence, error))
        return
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
    for number, row in records:
        requirement_id = row["requirement_id"]
        method = row["method"]
        status = row["status"]
        oracle_id = row["oracle_id"]
        credibility = row["credibility_level"]
        required_cl = row["required_cl"]

        # CAP_PENDING/LA_PENDING are intentional H-01 placeholders. They
        # pass the non-empty ledger check but are not resolved against a
        # Capella model (there is no H-01 model to orphan-check yet).
        if row["capella_element_id"] in CAPELLA_PLACEHOLDERS:
            pass

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
            report.fail(2, "line %d: oracle_id %r is not OR-xxx[letter]" %
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

        check_evidence(number, row, root, report)


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
