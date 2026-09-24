#!/usr/bin/env python3
"""WCCA / derating gate for the hardware BOM baseline (issue #56, H-09).

Requirements traced: HW-NF-002 (junction temperature <= 80 % of Tj,max),
HW-NF-003 (continuous electrical stress <= 70 % of the rated maximum for
voltage and current, capacitors <= 80 % of the rated voltage; exceptions
require a QA-recorded waiver in docs/hw/wcca-derating.md), HW-FR-009 and
HW-SF-002 (charge-path / hold-up worst case). HwAGENTS.md rules 1, 2, 4, 15.

What the gate enforces on ``hw/wcca/wcca-analysis.csv``:

1. every row validates against ``schemas/hw/hw-wcca-0.1.0.schema.json``
   (exact header, typed columns, unique ``wcca_id``);
2. ``derating_ratio`` equals ``worst_case / rated_max`` rounded to six
   decimals - the ratio is recomputed, never trusted;
3. ``threshold`` is the HW-NF-003 / HW-NF-002 threshold implied by the
   (stress, category) pair: 0.80 for capacitor voltage/current and for every
   temperature row, 0.70 for the voltage/current of every other category;
4. ``verdict`` is PASS iff ``derating_ratio <= threshold`` and FAIL otherwise;
   a FAIL row must name a ``waiver_id`` that is registered on its own line of
   the waiver register in ``docs/hw/wcca-derating.md`` (rule 15,
   line-anchored) and covers that row; an unregistered, malformed, rejected
   or unreferenced waiver fails the gate. A waiver never turns FAIL into PASS
   and a ``proposed`` waiver is reported as pending QA on every run;
5. every cited ``source_entry`` exists in the cited schema-validated
   ``source_extract`` under ``hw/bom/datasheets/`` (rule 2: no number without
   an extract) and ``rated_max`` equals one of the cited entries;
6. every ``hw/bom/bom.json`` parameter with ``citation_type: wcca_analysis``
   has a matching, line-anchored ``WCCA-R-nnn`` closed-parameter row in the
   document with the same value and unit (the WCCA closes the value, the BOM
   carries it);
7. the derating table rendered inside ``docs/hw/wcca-derating.md`` between
   the ``BEGIN WCCA ROWS`` / ``END WCCA ROWS`` markers matches the CSV
   (generated view; ``--export`` prints the table, ``--write`` rewrites the
   block after a successful validation).

Exit status 0 only when every rule holds; any missing, unreadable or
malformed input fails closed. No wall clock, no randomness: the output is a
deterministic function of the repository state.

Usage:
    python3 ci/check_hw_wcca.py [--root .] [--export] [--write] [--json]
"""
from __future__ import annotations

import argparse
import csv
import io
import json
import os
import re
import sys
from decimal import Decimal, ROUND_HALF_UP

try:
    import jsonschema
except ImportError:  # pragma: no cover - CI installs jsonschema
    jsonschema = None

WCCA_CSV = "hw/wcca/wcca-analysis.csv"
WCCA_DOC = "docs/hw/wcca-derating.md"
BOM_JSON = "hw/bom/bom.json"
ROW_SCHEMA = "schemas/hw/hw-wcca-0.1.0.schema.json"
EXTRACT_SCHEMA = "schemas/hw/hw-datasheet-extract-0.1.0.schema.json"

CSV_HEADER = (
    "wcca_id", "part_id", "component", "category", "stress", "unit",
    "rated_max", "rated_basis", "worst_case", "condition", "derating_ratio",
    "threshold", "verdict", "waiver_id", "requirement_ids", "source_extract",
    "source_entry", "note",
)
NUMERIC_COLUMNS = ("rated_max", "worst_case", "derating_ratio", "threshold")
RATIO_PLACES = 6

# HW-NF-003 / HW-NF-002 thresholds (issue #56 SE memo 2026-09-22: 80 % for
# capacitors, 70 % for everything else; temperature 80 % of Tj,max).
CAPACITOR_THRESHOLD = 0.8
OTHER_THRESHOLD = 0.7
TEMPERATURE_THRESHOLD = 0.8

WAIVER_ID = re.compile(r"^WCCA-W-\d{3}$")
WAIVER_STATUSES = ("proposed", "approved-qa", "rejected-qa")
CLOSED_ID = re.compile(r"^WCCA-R-\d{3}$")
ROWS_BEGIN = "<!-- BEGIN WCCA ROWS -->"
ROWS_END = "<!-- END WCCA ROWS -->"
WAIVER_BEGIN = "<!-- BEGIN WCCA WAIVERS -->"
WAIVER_END = "<!-- END WCCA WAIVERS -->"
CLOSED_BEGIN = "<!-- BEGIN WCCA CLOSED PARAMETERS -->"
CLOSED_END = "<!-- END WCCA CLOSED PARAMETERS -->"


class Report:
    def __init__(self):
        self.failures = []
        self.warnings = []

    def fail(self, rule, message):
        self.failures.append("rule %d: %s" % (rule, message))

    def warn(self, message):
        self.warnings.append(message)

    @property
    def ok(self):
        return not self.failures


def expected_threshold(stress, category):
    if stress == "temperature":
        return TEMPERATURE_THRESHOLD
    return CAPACITOR_THRESHOLD if category == "capacitor" else OTHER_THRESHOLD


def derating_ratio(worst_case, rated_max):
    """worst_case / rated_max rounded half-up to RATIO_PLACES decimals."""
    quotient = Decimal(repr(worst_case)) / Decimal(repr(rated_max))
    return float(quotient.quantize(Decimal(1).scaleb(-RATIO_PLACES),
                                   rounding=ROUND_HALF_UP))


def _read_text(root, relative):
    path = os.path.join(root, relative)
    with open(path, "r", encoding="utf-8", newline="") as handle:
        return handle.read()


def _read_json(root, relative):
    def unique(pairs):
        result = {}
        for key, value in pairs:
            if key in result:
                raise ValueError("duplicate JSON key %r" % key)
            result[key] = value
        return result
    return json.loads(_read_text(root, relative), object_pairs_hook=unique)


def _coerce(raw):
    row = dict(raw)
    for column in NUMERIC_COLUMNS:
        text = row.get(column, "")
        try:
            row[column] = float(text)
        except (TypeError, ValueError):
            raise ValueError("column %s is not numeric: %r" % (column, text))
    return row


def load_rows(root, report):
    """Parse and schema-validate the CSV; None when it cannot be trusted."""
    try:
        raw = _read_text(root, WCCA_CSV)
    except OSError as error:
        report.fail(1, "%s is missing or unreadable: %s" % (WCCA_CSV, error))
        return None
    reader = csv.reader(io.StringIO(raw))
    try:
        header = tuple(next(reader))
    except StopIteration:
        report.fail(1, "%s is empty" % WCCA_CSV)
        return None
    if header != CSV_HEADER:
        report.fail(1, "%s header must be exactly %s" % (WCCA_CSV, ",".join(CSV_HEADER)))
        return None
    schema = None
    if jsonschema is not None:
        try:
            schema = _read_json(root, ROW_SCHEMA)
            jsonschema.Draft202012Validator.check_schema(schema)
        except (OSError, ValueError, jsonschema.SchemaError) as error:
            report.fail(1, "%s is not a valid schema: %s" % (ROW_SCHEMA, error))
            return None
    rows = []
    seen = set()
    for number, record in enumerate(reader, start=2):
        if not record:
            report.fail(1, "line %d: blank line" % number)
            continue
        if len(record) != len(CSV_HEADER):
            report.fail(1, "line %d: expected %d fields, got %d" % (number, len(CSV_HEADER), len(record)))
            continue
        try:
            row = _coerce(dict(zip(CSV_HEADER, record)))
        except ValueError as error:
            report.fail(1, "line %d: %s" % (number, error))
            continue
        if schema is not None:
            errors = sorted(jsonschema.Draft202012Validator(
                schema, format_checker=jsonschema.FormatChecker()).iter_errors(row),
                key=lambda e: (str(list(e.absolute_path)), e.message))
            for error in errors:
                report.fail(1, "line %d: %s: %s" % (number, list(error.absolute_path), error.message))
            if errors:
                continue
        if row["wcca_id"] in seen:
            report.fail(1, "line %d: duplicate wcca_id %s" % (number, row["wcca_id"]))
            continue
        seen.add(row["wcca_id"])
        rows.append((number, row))
    if not rows and report.ok:
        report.fail(1, "%s carries no rows" % WCCA_CSV)
        return None
    return rows


def check_arithmetic(rows, report):
    """Rules 2-4: ratio, threshold and verdict are recomputed, not trusted."""
    for number, row in rows:
        ratio = derating_ratio(row["worst_case"], row["rated_max"])
        if abs(row["derating_ratio"] - ratio) > 0.5 * 10 ** -RATIO_PLACES:
            report.fail(2, "line %d: %s: derating_ratio %r != worst_case/rated_max = %r"
                        % (number, row["wcca_id"], row["derating_ratio"], ratio))
        threshold = expected_threshold(row["stress"], row["category"])
        if abs(row["threshold"] - threshold) > 1e-12:
            report.fail(3, "line %d: %s: threshold %r must be %r for %s/%s (HW-NF-003/HW-NF-002)"
                        % (number, row["wcca_id"], row["threshold"], threshold, row["stress"], row["category"]))
        verdict = "PASS" if ratio <= threshold + 1e-12 else "FAIL"
        if row["verdict"] != verdict:
            report.fail(4, "line %d: %s: verdict %s but ratio %r vs threshold %r requires %s"
                        % (number, row["wcca_id"], row["verdict"], ratio, threshold, verdict))
        if verdict == "PASS" and row["waiver_id"] != "-":
            report.fail(4, "line %d: %s: PASS row must not carry a waiver" % (number, row["wcca_id"]))
        if verdict == "FAIL" and not WAIVER_ID.match(row["waiver_id"]):
            report.fail(4, "line %d: %s: FAIL row without a WCCA-W-nnn waiver (HW-NF-003 requires a "
                           "QA-recorded waiver in %s)" % (number, row["wcca_id"], WCCA_DOC))


def _block(text, begin, end, label, report):
    start = text.find(begin)
    stop = text.find(end)
    if start < 0 or stop < 0 or stop < start:
        report.fail(4, "%s: %s block markers %s / %s missing or out of order" % (WCCA_DOC, label, begin, end))
        return None
    return text[start + len(begin):stop]


def _table_rows(block):
    """Pipe-table body rows of a block as lists of stripped cells."""
    rows = []
    for line in block.splitlines():
        stripped = line.strip()
        if not stripped.startswith("|"):
            continue
        cells = [cell.strip() for cell in stripped.strip("|").split("|")]
        if all(re.fullmatch(r":?-{3,}:?", cell) for cell in cells):
            continue
        rows.append(cells)
    return rows


def load_waivers(text, report):
    """Waiver register: | waiver_id | rows | rationale | status | qa_record |."""
    block = _block(text, WAIVER_BEGIN, WAIVER_END, "waiver register", report)
    if block is None:
        return {}
    waivers = {}
    rows = _table_rows(block)
    if not rows or rows[0] != ["waiver_id", "rows", "rationale", "status", "qa_record"]:
        report.fail(4, "%s: waiver register header must be | waiver_id | rows | rationale | status | qa_record |" % WCCA_DOC)
        return {}
    for cells in rows[1:]:
        if len(cells) != 5:
            report.fail(4, "%s: malformed waiver row %r" % (WCCA_DOC, cells))
            continue
        waiver_id, covered, rationale, status, qa_record = cells
        if not WAIVER_ID.match(waiver_id) or waiver_id in waivers:
            report.fail(4, "%s: waiver id %r invalid or duplicated" % (WCCA_DOC, waiver_id))
            continue
        if status not in WAIVER_STATUSES:
            report.fail(4, "%s: waiver %s status %r not in %s" % (WCCA_DOC, waiver_id, status, WAIVER_STATUSES))
            continue
        if not rationale or not qa_record:
            report.fail(4, "%s: waiver %s needs a rationale and a QA record cell" % (WCCA_DOC, waiver_id))
            continue
        waivers[waiver_id] = {
            "rows": tuple(item.strip() for item in covered.split(";") if item.strip()),
            "status": status,
            "qa_record": qa_record,
        }
    return waivers


def check_waivers(rows, waivers, report):
    referenced = set()
    for number, row in rows:
        waiver_id = row["waiver_id"]
        if waiver_id == "-":
            continue
        waiver = waivers.get(waiver_id)
        if waiver is None:
            report.fail(4, "line %d: %s: waiver %s is not registered in %s" % (number, row["wcca_id"], waiver_id, WCCA_DOC))
            continue
        referenced.add(waiver_id)
        if row["wcca_id"] not in waiver["rows"]:
            report.fail(4, "line %d: %s: waiver %s does not list this row" % (number, row["wcca_id"], waiver_id))
        if waiver["status"] == "rejected-qa":
            report.fail(4, "line %d: %s: waiver %s was rejected by QA" % (number, row["wcca_id"], waiver_id))
        elif waiver["status"] == "proposed":
            report.warn("%s: waiver %s is proposed, pending QA record (%s)"
                        % (row["wcca_id"], waiver_id, waiver["qa_record"]))
    for waiver_id, waiver in sorted(waivers.items()):
        if waiver_id not in referenced:
            report.fail(4, "%s: waiver %s is registered but no FAIL row references it" % (WCCA_DOC, waiver_id))
        known = {row["wcca_id"] for _, row in rows}
        for wcca_id in waiver["rows"]:
            if wcca_id not in known:
                report.fail(4, "%s: waiver %s lists unknown row %s" % (WCCA_DOC, waiver_id, wcca_id))


def load_extract(root, relative, cache, report):
    if relative in cache:
        return cache[relative]
    document = None
    try:
        document = _read_json(root, relative)
        if jsonschema is not None:
            schema = _read_json(root, EXTRACT_SCHEMA)
            errors = list(jsonschema.Draft202012Validator(
                schema, format_checker=jsonschema.FormatChecker()).iter_errors(document))
            if errors:
                raise ValueError(errors[0].message)
    except (OSError, ValueError) as error:
        report.fail(5, "%s: extract unusable: %s" % (relative, error))
        document = None
    cache[relative] = document
    return document


def check_extracts(rows, root, report):
    """Rule 5: every cited entry exists and rated_max is one of them."""
    cache = {}
    for number, row in rows:
        entries = {}
        for relative in row["source_extract"].split(";"):
            document = load_extract(root, relative, cache, report)
            if document is None:
                continue
            for entry in document["entries"]:
                entries[entry["name"]] = entry
        names = row["source_entry"].split(";")
        missing = [name for name in names if name not in entries]
        if missing:
            report.fail(5, "line %d: %s: entries %s not found in %s" % (number, row["wcca_id"], missing, row["source_extract"]))
            continue
        cited_values = [entries[name]["value"] for name in names]
        if not any(abs(value - row["rated_max"]) <= 1e-12 * max(1.0, abs(value)) for value in cited_values):
            report.fail(5, "line %d: %s: rated_max %r is not one of the cited entry values %r"
                        % (number, row["wcca_id"], row["rated_max"], cited_values))


def load_closed_parameters(text, report):
    block = _block(text, CLOSED_BEGIN, CLOSED_END, "closed parameters", report)
    if block is None:
        return {}
    rows = _table_rows(block)
    header = ["result_id", "parameter", "value", "unit", "carried_by", "derivation"]
    if not rows or rows[0] != header:
        report.fail(6, "%s: closed-parameter header must be | %s |" % (WCCA_DOC, " | ".join(header)))
        return {}
    closed = {}
    for cells in rows[1:]:
        if len(cells) != 6 or not CLOSED_ID.match(cells[0]):
            report.fail(6, "%s: malformed closed-parameter row %r" % (WCCA_DOC, cells))
            continue
        try:
            value = float(cells[2])
        except ValueError:
            report.fail(6, "%s: %s value %r is not numeric" % (WCCA_DOC, cells[0], cells[2]))
            continue
        closed.setdefault(cells[1], []).append({"result_id": cells[0], "value": value, "unit": cells[3], "carried_by": cells[4]})
    return closed


def check_bom_closure(root, closed, report):
    """Rule 6: bom.json wcca_analysis parameters are closed in the document."""
    try:
        bom = _read_json(root, BOM_JSON)
    except (OSError, ValueError) as error:
        report.fail(6, "%s unusable: %s" % (BOM_JSON, error))
        return
    for part in bom.get("parts", []):
        for parameter in part.get("parameters", []):
            if parameter.get("citation_type") != "wcca_analysis":
                continue
            name = parameter["name"]
            candidates = closed.get(name, [])
            match = [c for c in candidates
                     if abs(c["value"] - parameter["value"]) <= 1e-12 * max(1.0, abs(parameter["value"]))
                     and c["unit"] == parameter["unit"]]
            if not match:
                report.fail(6, "%s %s parameter %s = %r %s has no matching WCCA-R-nnn closed-parameter row in %s"
                            % (BOM_JSON, part["part_id"], name, parameter["value"], parameter["unit"], WCCA_DOC))
                continue
            if match[0]["result_id"] not in parameter.get("citation", ""):
                report.fail(6, "%s %s parameter %s citation must name %s" % (BOM_JSON, part["part_id"], name, match[0]["result_id"]))
            if part["part_id"] not in match[0]["carried_by"]:
                report.fail(6, "%s: %s carried_by must name %s" % (WCCA_DOC, match[0]["result_id"], part["part_id"]))


def _fmt(value):
    text = ("%.6f" % value).rstrip("0").rstrip(".")
    return text if text else "0"


def render_rows_table(rows):
    lines = ["| wcca_id | part | component | stress | rated max | basis | worst case | ratio | threshold | verdict | waiver |",
             "|---|---|---|---|---|---|---|---|---|---|---|"]
    for _, row in rows:
        lines.append("| %s | %s | %s | %s | %s %s | %s | %s %s | %.6f | %.2f | %s | %s |" % (
            row["wcca_id"], row["part_id"], row["component"], row["stress"],
            _fmt(row["rated_max"]), row["unit"], row["rated_basis"],
            _fmt(row["worst_case"]), row["unit"], row["derating_ratio"],
            row["threshold"], row["verdict"], row["waiver_id"]))
    return "\n".join(lines) + "\n"


def check_doc_table(text, rows, report):
    block = _block(text, ROWS_BEGIN, ROWS_END, "derating rows", report)
    if block is None:
        return
    if block.strip("\n") != render_rows_table(rows).strip("\n"):
        report.fail(7, "%s: table between %s and %s drifted from %s (run: python3 ci/check_hw_wcca.py --write)"
                    % (WCCA_DOC, ROWS_BEGIN, ROWS_END, WCCA_CSV))


def write_doc_table(root, text, rows):
    start = text.index(ROWS_BEGIN) + len(ROWS_BEGIN)
    stop = text.index(ROWS_END)
    updated = text[:start] + "\n" + render_rows_table(rows) + text[stop:]
    with open(os.path.join(root, WCCA_DOC), "w", encoding="utf-8", newline="") as handle:
        handle.write(updated)


def run(root, write=False):
    report = Report()
    rows = load_rows(root, report)
    if rows is None:
        return report, []
    check_arithmetic(rows, report)
    try:
        text = _read_text(root, WCCA_DOC)
    except OSError as error:
        report.fail(4, "%s is missing or unreadable: %s" % (WCCA_DOC, error))
        return report, rows
    waivers = load_waivers(text, report)
    check_waivers(rows, waivers, report)
    check_extracts(rows, root, report)
    check_bom_closure(root, load_closed_parameters(text, report), report)
    if write and report.ok:
        write_doc_table(root, text, rows)
        text = _read_text(root, WCCA_DOC)
    check_doc_table(text, rows, report)
    return report, rows


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--root", default=".", help="repository root (default: .)")
    parser.add_argument("--export", action="store_true", help="print the generated derating table and exit")
    parser.add_argument("--write", action="store_true", help="rewrite the generated table in the document after validation")
    parser.add_argument("--json", action="store_true", help="machine-readable summary")
    args = parser.parse_args(argv)
    root = os.path.abspath(args.root)
    report, rows = run(root, write=args.write)
    if args.export and rows:
        sys.stdout.write(render_rows_table(rows))
        return 0 if report.ok else 1
    summary = {
        "rows": len(rows),
        "pass": sum(1 for _, row in rows if row["verdict"] == "PASS"),
        "fail_waived": sum(1 for _, row in rows if row["verdict"] == "FAIL"),
        "warnings": report.warnings,
        "failures": report.failures,
        "result": "PASS" if report.ok else "FAIL",
    }
    if args.json:
        print(json.dumps(summary, indent=2, sort_keys=True))
    else:
        for _, row in rows:
            print("%s %-8s %-46s %-11s ratio %.6f thr %.2f %s%s" % (
                row["verdict"], row["wcca_id"], row["component"][:46], row["stress"],
                row["derating_ratio"], row["threshold"],
                "" if row["waiver_id"] == "-" else "waiver " + row["waiver_id"], ""))
        for warning in report.warnings:
            print("WARNING: " + warning)
        for failure in report.failures:
            print("FAIL: " + failure)
        print("check_hw_wcca: %s (%d rows: %d PASS, %d FAIL under registered waiver, %d warnings)"
              % (summary["result"], summary["rows"], summary["pass"], summary["fail_waived"], len(report.warnings)))
    return 0 if report.ok else 1


if __name__ == "__main__":
    sys.exit(main())
