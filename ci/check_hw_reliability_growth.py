#!/usr/bin/env python3
"""Reliability-growth gate: the FMEDA MTBF must not silently regress (H-09, issue #56).

Requirements traced: HW-NF-004 (FIT provenance of safety-related parts),
HW-SF-005 (FMEDA). HwAGENTS.md rules 1, 4, 5.

What it does
    1. recomputes the FMEDA with ``tools/fmeda-calculator.py`` (the qualified
       calculator, tool id ``fmeda``): reliability MTBF = 1e9 h / sum(FIT of every
       part of hw/fmeda/fmeda-analysis.csv), plus PMHF, SPFM and LFM;
    2. reads ``hw/fmeda/mtbf-history.csv`` (row contract
       schemas/hw/hw-mtbf-history-0.1.0.schema.json, append-only, keyed by the
       sha256 of the FMEDA CSV; no wall-clock timestamps);
    3. compares the new MTBF with the last accepted row: a drop of more than
       10 percent fails with
       ``MTBF regression detected: {old_mtbf} → {new_mtbf}. Review component additions.``
       (exit 1) and the history is left untouched;
    4. otherwise: if the FMEDA CSV hash already equals the last row, nothing
       changes (pass); if it differs, a new row is appended (pass) unless
       ``--no-append`` is given. Run it locally and commit the appended row in
       the PR that changes the FMEDA; the nightly job re-runs it and reports the
       row it would append.

Fail closed: a missing, empty, malformed, out-of-order or schema-violating
history, a history whose recorded metrics do not match the calculator for the
current CSV hash, or any calculator error exits 1. A drop of 10 percent or
less is accepted with a warning so that the reviewer sees it.

Usage
    python3 ci/check_hw_reliability_growth.py [--root .] [--no-append] [--json]
    python3 ci/check_hw_reliability_growth.py --root . --init "#56 initial baseline"
        (writes the first row when no history exists; refuses to overwrite)
"""
from __future__ import annotations

import argparse
import csv
import hashlib
import importlib.util
import io
import json
import os
import sys
from decimal import Decimal, ROUND_HALF_UP
from fractions import Fraction

try:
    import jsonschema
except ImportError:  # pragma: no cover - CI installs jsonschema
    jsonschema = None

HISTORY_CSV = "hw/fmeda/mtbf-history.csv"
HISTORY_SCHEMA = "schemas/hw/hw-mtbf-history-0.1.0.schema.json"
CALCULATOR = os.path.join("tools", "fmeda-calculator.py")
HEADER = ("sequence", "fmeda_csv_sha256", "total_fit", "mtbf_hours", "pmhf_fit",
          "mean_time_to_sg_violation_hours", "spfm_percent", "lfm_percent", "issue_ref")
NUMERIC = ("total_fit", "mtbf_hours", "pmhf_fit", "mean_time_to_sg_violation_hours",
           "spfm_percent", "lfm_percent")
MAX_DROP = Fraction(1, 10)
REGRESSION_MESSAGE = "MTBF regression detected: {old_mtbf} \u2192 {new_mtbf}. Review component additions."


class GrowthError(ValueError):
    """Fail-closed condition."""


def load_calculator(root):
    path = os.path.join(root, CALCULATOR)
    spec = importlib.util.spec_from_file_location("fmeda_calculator", path)
    if spec is None or spec.loader is None:
        raise GrowthError("%s is missing" % CALCULATOR)
    module = importlib.util.module_from_spec(spec)
    try:
        spec.loader.exec_module(module)
    except (OSError, SyntaxError) as error:
        raise GrowthError("%s cannot be loaded: %s" % (CALCULATOR, error))
    return module


def sha256_of(path):
    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        digest.update(handle.read())
    return "sha256:" + digest.hexdigest()


def quantize(value, places):
    exact = Decimal(value.numerator) / Decimal(value.denominator)
    return str(exact.quantize(Decimal(1).scaleb(-places), rounding=ROUND_HALF_UP))


def current_state(root, calc):
    """Exact metrics of the committed FMEDA, rendered as history cells."""
    try:
        report, result = calc.compute(root)
    except calc.FmedaError as error:
        raise GrowthError("FMEDA calculator failed: %s" % error)
    metrics, sums = result["metrics"], result["sums"]
    if metrics["mtbf_hours"] is None or metrics["lfm"] is None or metrics["spfm"] is None:
        raise GrowthError("FMEDA yields no MTBF/SPFM/LFM (empty or degenerate analysis)")
    mean_time = metrics["mean_time_to_sg_violation_hours"]
    if mean_time is None:
        raise GrowthError("FMEDA yields no PMHF (mean time to safety-goal violation undefined)")
    return {
        "fmeda_csv_sha256": sha256_of(os.path.join(root, calc.FMEDA_CSV)),
        "total_fit": quantize(sums["total_all"], 4),
        "mtbf_hours": quantize(metrics["mtbf_hours"], 1),
        "pmhf_fit": quantize(metrics["pmhf_fit"], 4),
        "mean_time_to_sg_violation_hours": quantize(mean_time, 1),
        "spfm_percent": quantize(metrics["spfm"] * 100, 2),
        "lfm_percent": quantize(metrics["lfm"] * 100, 2),
    }


def load_history(root):
    path = os.path.join(root, HISTORY_CSV)
    try:
        with open(path, "r", encoding="utf-8", newline="") as handle:
            raw = handle.read()
    except OSError as error:
        raise GrowthError("%s is missing or unreadable (%s); a reliability history is mandatory "
                          "(create it with --init once, then commit it)" % (HISTORY_CSV, error))
    reader = csv.reader(io.StringIO(raw))
    try:
        header = tuple(next(reader))
    except StopIteration:
        raise GrowthError("%s is empty" % HISTORY_CSV)
    if header != HEADER:
        raise GrowthError("%s header must be exactly %s" % (HISTORY_CSV, ",".join(HEADER)))
    schema = None
    if jsonschema is not None:
        try:
            with open(os.path.join(root, HISTORY_SCHEMA), "r", encoding="utf-8") as handle:
                schema = json.load(handle)
            jsonschema.Draft202012Validator.check_schema(schema)
        except (OSError, ValueError, jsonschema.SchemaError) as error:
            raise GrowthError("%s unusable: %s" % (HISTORY_SCHEMA, error))
    rows = []
    for number, record in enumerate(reader, start=2):
        if len(record) != len(HEADER):
            raise GrowthError("%s line %d: expected %d fields, got %d" % (HISTORY_CSV, number, len(HEADER), len(record)))
        row = dict(zip(HEADER, record))
        typed = dict(row)
        try:
            typed["sequence"] = int(row["sequence"])
            for column in NUMERIC:
                typed[column] = float(Fraction(row[column]))
        except (ValueError, ZeroDivisionError):
            raise GrowthError("%s line %d: non-numeric cell" % (HISTORY_CSV, number))
        if schema is not None:
            errors = sorted(jsonschema.Draft202012Validator(
                schema, format_checker=jsonschema.FormatChecker()).iter_errors(typed),
                key=lambda e: (str(list(e.absolute_path)), e.message))
            if errors:
                raise GrowthError("%s line %d: %s: %s" % (HISTORY_CSV, number, list(errors[0].absolute_path), errors[0].message))
        if typed["sequence"] != len(rows) + 1:
            raise GrowthError("%s line %d: sequence %d out of order (expected %d)"
                              % (HISTORY_CSV, number, typed["sequence"], len(rows) + 1))
        rows.append(row)
    if not rows:
        raise GrowthError("%s carries no rows (corrupted or truncated history)" % HISTORY_CSV)
    return rows


def render_row(sequence, state, issue_ref):
    buffer = io.StringIO()
    csv.writer(buffer, lineterminator="\n").writerow([
        str(sequence), state["fmeda_csv_sha256"], state["total_fit"], state["mtbf_hours"], state["pmhf_fit"],
        state["mean_time_to_sg_violation_hours"], state["spfm_percent"], state["lfm_percent"], issue_ref])
    return buffer.getvalue()


def evaluate(root, calc, append=True, issue_ref="#56 FMEDA update"):
    """Return (summary dict, appended_row_or_None); raise GrowthError to fail."""
    state = current_state(root, calc)
    history = load_history(root)
    last = history[-1]
    # Compare at the recorded precision (one decimal hour) so that a re-issued
    # row with the same MTBF never reads as a drop.
    old_mtbf = Fraction(last["mtbf_hours"])
    new_mtbf = Fraction(state["mtbf_hours"])
    summary = {
        "history_rows": len(history),
        "last_sequence": int(last["sequence"]),
        "old_mtbf_hours": last["mtbf_hours"],
        "new_mtbf_hours": state["mtbf_hours"],
        "change_percent": quantize((new_mtbf - old_mtbf) / old_mtbf * 100, 2),
        "fmeda_csv_sha256": state["fmeda_csv_sha256"],
        "warnings": [],
    }
    if last["fmeda_csv_sha256"] == state["fmeda_csv_sha256"]:
        drift = [column for column in NUMERIC if Fraction(last[column]) != Fraction(state[column])]
        if drift:
            raise GrowthError("%s row %s records %s that differ from the calculator for the same FMEDA hash: "
                              "the history was edited by hand or the calculator changed; re-issue the row"
                              % (HISTORY_CSV, last["sequence"], ", ".join(drift)))
        summary["result"] = "unchanged"
        return summary, None
    if new_mtbf < old_mtbf * (1 - MAX_DROP):
        raise GrowthError(REGRESSION_MESSAGE.format(old_mtbf=last["mtbf_hours"], new_mtbf=state["mtbf_hours"]))
    if new_mtbf < old_mtbf:
        summary["warnings"].append("MTBF dropped %s %% (within the 10 %% allowance): %s -> %s h"
                                   % (summary["change_percent"], last["mtbf_hours"], state["mtbf_hours"]))
    row = render_row(int(last["sequence"]) + 1, state, issue_ref)
    if append:
        path = os.path.join(root, HISTORY_CSV)
        with open(path, "r", encoding="utf-8", newline="") as handle:
            existing = handle.read()
        with open(path, "w", encoding="utf-8", newline="") as handle:
            handle.write(existing if existing.endswith("\n") else existing + "\n")
            handle.write(row)
        summary["result"] = "appended"
    else:
        summary["result"] = "append-pending"
    return summary, row


def init_history(root, calc, issue_ref):
    path = os.path.join(root, HISTORY_CSV)
    if os.path.exists(path):
        raise GrowthError("%s already exists; --init never overwrites a history" % HISTORY_CSV)
    state = current_state(root, calc)
    row = render_row(1, state, issue_ref)
    with open(path, "w", encoding="utf-8", newline="") as handle:
        handle.write(",".join(HEADER) + "\n")
        handle.write(row)
    return row


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--root", default=".", help="repository root (default: .)")
    parser.add_argument("--no-append", action="store_true", help="never write the history; report only")
    parser.add_argument("--issue-ref", default="#56 FMEDA update", help="issue_ref cell of an appended row")
    parser.add_argument("--init", metavar="ISSUE_REF", default=None,
                        help="create the history with the current FMEDA as sequence 1 (refuses to overwrite)")
    parser.add_argument("--json", action="store_true", help="machine-readable summary")
    args = parser.parse_args(argv)
    root = os.path.abspath(args.root)
    try:
        calc = load_calculator(root)
        if args.init is not None:
            row = init_history(root, calc, args.init)
            print("check_hw_reliability_growth: initialised %s with %s" % (HISTORY_CSV, row.strip()))
            return 0
        summary, row = evaluate(root, calc, append=not args.no_append, issue_ref=args.issue_ref)
    except GrowthError as error:
        message = str(error)
        if args.json:
            print(json.dumps({"result": "FAIL", "error": message}, indent=2, sort_keys=True))
        else:
            print("FAIL: " + message)
            print("check_hw_reliability_growth: FAIL")
        return 1
    if args.json:
        print(json.dumps(dict(summary, appended_row=row), indent=2, sort_keys=True))
        return 0
    for warning in summary["warnings"]:
        print("WARNING: " + warning)
    if summary["result"] == "unchanged":
        print("check_hw_reliability_growth: PASS (FMEDA unchanged since history row %d; MTBF %s h)"
              % (summary["last_sequence"], summary["new_mtbf_hours"]))
    elif summary["result"] == "appended":
        print("check_hw_reliability_growth: PASS (MTBF %s -> %s h, %s %%; row %d appended to %s - commit it)"
              % (summary["old_mtbf_hours"], summary["new_mtbf_hours"], summary["change_percent"],
                 summary["last_sequence"] + 1, HISTORY_CSV))
    else:
        print("check_hw_reliability_growth: PASS (MTBF %s -> %s h, %s %%; --no-append: row not written)"
              % (summary["old_mtbf_hours"], summary["new_mtbf_hours"], summary["change_percent"]))
        print("pending row: " + row.strip())
    return 0


if __name__ == "__main__":
    sys.exit(main())
