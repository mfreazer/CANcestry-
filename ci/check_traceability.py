#!/usr/bin/env python3
"""Check that the CANcestry traceability record is complete and resolvable.

``docs/qa/test-strategy.md`` section 10 requires traceability completeness to be
checked in CI. This dependency-free, read-only script enforces the six-column
record, controlled vocabularies, requirement coverage, passing-test resolution,
and the embedded deferred ledger.

Rules enforced
--------------
1. ``docs/trace/traceability.csv`` has the documented six-column header,
   including explicit ``artifact_type``, and no empty or duplicated row.
2. ``artifact_type``, ``verification_method`` and ``status`` use the documented
   vocabularies.
3. Requirement ids are shaped like ``SW-FR-*``/``SYS-*``/``QA-*``.
4. Every ``SW-FR-*``/``SYS-*`` id used in the CSV is actually defined in
   ``docs/software/SwRS.md`` or ``docs/SyRS.md``.
5. No requirement is silently absent: every requirement defined in the SwRS or
   SyRS appears in the CSV, or is named in the deferred ledger of
   ``docs/trace/traceability.md``.
6. Every ``passing`` row resolves: its ``test_id`` appears literally in a source
   artifact under ``tests/``, ``examples/``, ``tools/`` or ``ci/``.
7. Every test source (``*.c`` under ``tests/``) cites at least one requirement
   id or QA review item.
8. Every requirement whose rows are all ``planned``/``failed`` is named in the
   deferred ledger. ``QA-*`` review items are exempt.

The deferred ledger is the section of ``docs/trace/traceability.md`` whose
heading contains "deferred ledger". It may name ids literally or as inclusive
ranges; only the first table column is normative.
"""

import csv
import os
import re
import sys

CSV_RELATIVE_PATH = os.path.join("docs", "trace", "traceability.csv")
RECORD_RELATIVE_PATH = os.path.join("docs", "trace", "traceability.md")
REQUIREMENT_SOURCES = [
    os.path.join("docs", "software", "SwRS.md"),
    os.path.join("docs", "SyRS.md"),
]

CSV_HEADER = [
    "requirement_id",
    "artifact_type",
    "artifact_id",
    "verification_method",
    "test_id",
    "status",
]
ARTIFACT_TYPES = (
    "source",
    "component",
    "test",
    "document",
    "schema",
    "tool",
    "ci",
    "release",
    "requirement",
)
METHODS = ("test", "inspection", "demonstration")
STATUSES = ("passing", "planned", "failed")

# Where a passing row's test id must appear: executable/reviewable artifacts,
# not the prose in docs/ itself.
ARTIFACT_DIRECTORIES = ("tests", "examples", "tools", "ci")
ARTIFACT_SUFFIXES = (".c", ".h", ".py", ".txt", ".json", ".yml", ".yaml", ".cmake")

REQUIREMENT_ID = re.compile(r"^(?:SW-FR-[A-Z]+-\d{3}|SYS-[A-Z]+-\d{3}|QA-[A-Za-z0-9.\-]+)$")
DEFINED_SW_FR = re.compile(r"\bSW-FR-[A-Z]+-\d{3}\b")
DEFINED_SYS = re.compile(r"\bSYS-[A-Z]+-\d{3}\b")
RANGE = re.compile(r"\b((?:SW-FR-[A-Z]+|SYS-[A-Z]+)-)(\d{3})\.\.(\d{3})\b")
ANY_ID = re.compile(r"\b(?:SW-FR-[A-Z]+-\d{3}|SYS-[A-Z]+-\d{3})\b")


class Report(object):
    """Collect failures grouped by rule."""

    def __init__(self):
        self.failures = []

    def fail(self, rule, message):
        self.failures.append("rule %d: %s" % (rule, message))


def read_text(path):
    with open(path, "r", encoding="utf-8", errors="replace") as handle:
        return handle.read()


def load_rows(root, report):
    """Rule 1: CSV structure. Return parsed records."""
    path = os.path.join(root, CSV_RELATIVE_PATH)
    if not os.path.isfile(path):
        report.fail(1, "%s is missing" % CSV_RELATIVE_PATH)
        return []
    with open(path, "r", encoding="utf-8", newline="") as handle:
        rows = list(csv.reader(handle))
    if not rows:
        report.fail(1, "%s is empty" % CSV_RELATIVE_PATH)
        return []
    if rows[0] != CSV_HEADER:
        report.fail(1, "header is %r, expected %r" % (rows[0], CSV_HEADER))

    records = []
    seen = set()
    for number, row in enumerate(rows[1:], start=2):
        if len(row) != len(CSV_HEADER):
            report.fail(1, "line %d has %d fields, expected %d" %
                        (number, len(row), len(CSV_HEADER)))
            continue
        if any(field.strip() == "" for field in row):
            report.fail(1, "line %d has an empty field: %r" % (number, row))
            continue
        record = dict(zip(CSV_HEADER, (field.strip() for field in row)))
        key = (record["requirement_id"], record["artifact_type"],
               record["artifact_id"], record["test_id"])
        if key in seen:
            report.fail(1, "duplicate row for %s / %s / %s / %s" % key)
        seen.add(key)
        records.append(record)
    return records


def check_vocabulary(records, report):
    """Rule 2 and 3: controlled values and id shape."""
    for record in records:
        if record["artifact_type"] not in ARTIFACT_TYPES:
            report.fail(2, "%s: artifact_type %r is not one of %s" %
                        (record["requirement_id"], record["artifact_type"],
                         ", ".join(ARTIFACT_TYPES)))
        if record["verification_method"] not in METHODS:
            report.fail(2, "%s: verification_method %r is not one of %s" %
                        (record["requirement_id"], record["verification_method"],
                         ", ".join(METHODS)))
        if record["status"] not in STATUSES:
            report.fail(2, "%s: status %r is not one of %s" %
                        (record["requirement_id"], record["status"],
                         ", ".join(STATUSES)))
        if not REQUIREMENT_ID.match(record["requirement_id"]):
            report.fail(3, "%s is not a recognised requirement id" %
                        record["requirement_id"])


def read_defined_ids(root, report):
    """Return every requirement id defined in the SwRS and SyRS."""
    defined = set()
    for relative in REQUIREMENT_SOURCES:
        path = os.path.join(root, relative)
        if not os.path.isfile(path):
            report.fail(5, "%s is missing" % relative)
            continue
        text = read_text(path)
        defined.update(DEFINED_SW_FR.findall(text))
        defined.update(DEFINED_SYS.findall(text))
    return defined


def read_deferred_ids(root, report):
    """Return ids named in the first column of the embedded ledger."""
    path = os.path.join(root, RECORD_RELATIVE_PATH)
    if not os.path.isfile(path):
        report.fail(5, "%s is missing" % RECORD_RELATIVE_PATH)
        return set()
    lines = read_text(path).splitlines()
    start = None
    for index, line in enumerate(lines):
        if line.startswith("#") and "deferred ledger" in line.lower():
            start = index
            break
    if start is None:
        report.fail(5, "%s has no 'Deferred ledger' heading" % RECORD_RELATIVE_PATH)
        return set()

    deferred = set()
    for line in lines[start + 1:]:
        if line.startswith("#"):
            break
        if not line.startswith("|"):
            continue
        cells = line.split("|")
        if len(cells) < 3:
            continue
        cell = cells[1]
        if set(cell.strip()) <= set("-: "):
            continue
        deferred.update(ANY_ID.findall(cell))
        for prefix, first, last in RANGE.findall(cell):
            for value in range(int(first), int(last) + 1):
                deferred.add("%s%03d" % (prefix, value))
    if not deferred:
        report.fail(5, "the deferred ledger names no requirement ids")
    return deferred


def collect_artifacts(root):
    """Concatenate text of source artifacts in the resolution directories."""
    chunks = []
    for directory in ARTIFACT_DIRECTORIES:
        base = os.path.join(root, directory)
        if not os.path.isdir(base):
            continue
        for current, subdirectories, files in os.walk(base):
            subdirectories[:] = [name for name in subdirectories
                                 if not name.startswith("build")
                                 and name != "CMakeFiles"
                                 and name != "__pycache__"]
            for name in files:
                if not name.endswith(ARTIFACT_SUFFIXES):
                    continue
                try:
                    chunks.append(read_text(os.path.join(current, name)))
                except OSError:
                    continue
    return "\n".join(chunks)


def check_rows(records, defined, deferred, artifacts, report):
    """Rules 4, 6 and 8."""
    by_requirement = {}
    for record in records:
        by_requirement.setdefault(record["requirement_id"], []).append(record)
        if record["status"] == "passing" and record["test_id"] not in artifacts:
            report.fail(6, "%s: passing row %s (%s) names a test id that no source "
                        "artifact under %s mentions" %
                        (record["requirement_id"], record["artifact_id"],
                         record["test_id"], ", ".join(ARTIFACT_DIRECTORIES) + "/"))

    for requirement_id, rows in sorted(by_requirement.items()):
        if requirement_id.startswith("QA-"):
            continue
        if requirement_id not in defined:
            report.fail(4, "%s appears in the CSV but is not defined in the SwRS or "
                        "the SyRS" % requirement_id)
        if all(row["status"] != "passing" for row in rows) and requirement_id not in deferred:
            report.fail(8, "%s has no passing row and is not named in the deferred "
                        "ledger" % requirement_id)


def check_coverage(defined, csv_ids, deferred, report):
    """Rule 5: no defined requirement is silently absent."""
    for requirement_id in sorted(defined):
        if requirement_id not in csv_ids and requirement_id not in deferred:
            report.fail(5, "%s is defined in the SwRS or SyRS but appears neither in "
                        "the CSV nor in the deferred ledger" % requirement_id)


def check_tests_cite_requirements(root, report):
    """Rule 7: every C test source cites a requirement or QA item."""
    citation = re.compile(r"\b(?:SW-FR-[A-Z]+-\d{3}|SYS-[A-Z]+-\d{3}|QA-[A-Za-z0-9.\-]+)\b")
    checked = 0
    base = os.path.join(root, "tests")
    for current, subdirectories, files in os.walk(base):
        subdirectories[:] = [name for name in subdirectories
                             if not name.startswith("build") and name != "CMakeFiles"]
        for name in sorted(files):
            if not name.endswith(".c"):
                continue
            checked += 1
            path = os.path.join(current, name)
            if not citation.search(read_text(path)):
                report.fail(7, "%s cites no requirement id" %
                            os.path.relpath(path, root))
    return checked


def main(argv):
    if len(argv) != 2:
        print("usage: check_traceability.py <repo-root>")
        return 1
    root = argv[1]
    if not os.path.isdir(root):
        print("FAIL: %s is not a directory" % root)
        return 1

    report = Report()
    records = load_rows(root, report)
    check_vocabulary(records, report)
    defined = read_defined_ids(root, report)
    deferred = read_deferred_ids(root, report)
    artifacts = collect_artifacts(root)
    check_rows(records, defined, deferred, artifacts, report)
    check_coverage(defined, {record["requirement_id"] for record in records}, deferred,
                   report)
    checked_tests = check_tests_cite_requirements(root, report)

    passing = sum(1 for record in records if record["status"] == "passing")
    planned = sum(1 for record in records if record["status"] == "planned")
    failed = sum(1 for record in records if record["status"] == "failed")
    traced = {record["requirement_id"] for record in records
              if not record["requirement_id"].startswith("QA-")}
    print("rows: %d (passing %d, planned %d, failed %d)" %
          (len(records), passing, planned, failed))
    print("requirements defined: %d | traced in the CSV: %d | deferred: %d" %
          (len(defined), len(traced), len(deferred)))
    print("test sources citing a requirement: %d" % checked_tests)

    if report.failures:
        print("FAIL: %d traceability problem(s)" % len(report.failures))
        for failure in report.failures:
            print("      %s" % failure)
        return 1
    print("PASS: the six-column traceability record is complete and every passing row resolves")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
