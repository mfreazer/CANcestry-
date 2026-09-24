#!/usr/bin/env python3
"""FMEDA calculator: ISO 26262-5:2018 hardware architectural metrics (H-09).

Tool id ``fmeda`` in ``docs/hw/tool-qualification.md`` (TCL2, ISO 26262-8
section 13). Requirements traced: HW-SF-005 (FMEDA, oracle OR-004),
HW-NF-004 (FIT provenance); HwAGENTS.md rules 1, 2, 4, 5.

Inputs
    hw/fmeda/fmeda-analysis.csv   one row per (component, failure mode);
                                  row contract schemas/hw/hw-fmeda-0.1.0.schema.json
    hw/bom/fit-database.json      curated base failure rates (FIT-nnn)
    hw/bom/datasheets/extract-iso26262-5-2018-targets.json
                                  ASIL target values (Tables 4/5/6) and the
                                  project PMHF target of issue #56

Classification algebra (ISO 26262-5:2018 Annex B flow, Annex C equations)
    lambda            = fit_total x fmd_fraction                    (per row)
    safety_related=N  -> excluded from every sum
    violates_sg=N, mpf_relevant=N -> lambda_S      = lambda
    violates_sg=N, mpf_relevant=Y -> lambda_MPF,L  = lambda x (1 - dc_latent)
                                     lambda_MPF,DP = lambda x dc_latent
    violates_sg=Y, no SM          -> lambda_SPF    = lambda
    violates_sg=Y, SM (dc_spf)    -> lambda_RF     = lambda x (1 - dc_spf)     (C.3)
                                     covered part lambda x dc_spf becomes MPF:
                                     lambda_MPF,L  = covered x (1 - dc_latent) (C.5)
                                     lambda_MPF,DP = covered x dc_latent
Metrics (safety-related rows only)
    SPFM = 1 - sum(lambda_SPF + lambda_RF) / sum(lambda_SR)                  (C.7)
    LFM  = 1 - sum(lambda_MPF,L) / sum(lambda_SR - lambda_SPF - lambda_RF)    (C.8)
    PMHF (single-point/residual)   = sum(lambda_SPF + lambda_RF)
    PMHF (dual-point term)         = sum(lambda_MPF,DP) x sum(lambda_MPF,L) x T_lifetime
                                     (ISO 26262-10 simplified form; lambda in
                                     1/h, T_lifetime an explicit parameter,
                                     default 1e5 h)
    PMHF                           = single-point/residual + dual-point term
    PMHF (source simplification)   = sum(lambda_SPF + lambda_RF) + sum(lambda_MPF,L)
                                     (the conservative form printed by the TI
                                     SLYP685 teaching example; reported for
                                     the qualification fixture only)
    MTBF (reliability)             = 1e9 h / sum(lambda of every part, FIT)
    mean time to SG violation      = 1e9 h / PMHF(FIT)   (issue #56 '1/PMHF')

Determinism (HwAGENTS.md rule 5): every quantity is computed with exact
rational arithmetic (``fractions.Fraction``) and rendered with fixed half-up
decimal quantisation; the report carries no timestamp, seed, path or
environment data, so its bytes depend only on the inputs.

Usage
    python3 tools/fmeda-calculator.py --root . [--format json|md] [--out FILE]
    python3 tools/fmeda-calculator.py --root . --check      # CSV/doc consistency gate
    python3 tools/fmeda-calculator.py --root . --write-doc  # regenerate docs/hw/fmeda.md blocks
    python3 tools/fmeda-calculator.py --csv other.csv --fit-database db.json --lifetime-hours 1e5

Exit status 0 on success; 1 on any contract violation (fail closed).
"""
from __future__ import annotations

import argparse
import csv
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

TOOL_ID = "fmeda"
TOOL_VERSION = "0.1.0"

FMEDA_CSV = "hw/fmeda/fmeda-analysis.csv"
FIT_DATABASE = "hw/bom/fit-database.json"
TARGETS_EXTRACT = "hw/bom/datasheets/extract-iso26262-5-2018-targets.json"
ROW_SCHEMA = "schemas/hw/hw-fmeda-0.1.0.schema.json"
FIT_SCHEMA = "schemas/hw/hw-fit-database-0.1.0.schema.json"
FMEDA_DOC = "docs/hw/fmeda.md"

DEFAULT_LIFETIME_HOURS = Fraction(100000)
FIT_PER_HOUR = Fraction(10) ** 9

CSV_HEADER = (
    "fmeda_id", "part_id", "component", "fit_id", "fit_total", "failure_mode",
    "fmd_fraction", "fmd_basis", "fit_mode", "safety_related", "violates_sg",
    "mpf_relevant", "sm_spf", "dc_spf", "sm_latent", "dc_latent",
    "classification", "fit_safe", "fit_spf_rf", "fit_mpf_detected",
    "fit_mpf_latent", "requirement_ids", "note",
)
INPUT_NUMERIC = ("fit_total", "fmd_fraction", "dc_spf", "dc_latent")
DERIVED_NUMERIC = ("fit_mode", "fit_safe", "fit_spf_rf", "fit_mpf_detected", "fit_mpf_latent")
DERIVED_PLACES = 4
FIT_PLACES = 4
PERCENT_PLACES = 2

METRICS_BEGIN = "<!-- BEGIN FMEDA METRICS -->"
METRICS_END = "<!-- END FMEDA METRICS -->"
ROWS_BEGIN = "<!-- BEGIN FMEDA ROWS -->"
ROWS_END = "<!-- END FMEDA ROWS -->"


class FmedaError(ValueError):
    """Contract violation: the calculator refuses to produce a result."""


# ---------------------------------------------------------------------------
# Exact arithmetic helpers
# ---------------------------------------------------------------------------

def frac(value):
    """Exact Fraction from a decimal string / int / float (via repr)."""
    if isinstance(value, Fraction):
        return value
    if isinstance(value, bool):
        raise FmedaError("boolean is not a number: %r" % value)
    if isinstance(value, int):
        return Fraction(value)
    if isinstance(value, float):
        return Fraction(repr(value))
    if isinstance(value, str):
        try:
            return Fraction(value.strip())
        except (ValueError, ZeroDivisionError):
            raise FmedaError("not a number: %r" % value)
    raise FmedaError("unsupported numeric type %r" % type(value).__name__)


def quantize(value, places):
    """Half-up decimal string of an exact Fraction with ``places`` decimals."""
    if value is None:
        return None
    numerator, denominator = value.numerator, value.denominator
    exact = Decimal(numerator) / Decimal(denominator)
    return str(exact.quantize(Decimal(1).scaleb(-places), rounding=ROUND_HALF_UP))


def fit_text(value):
    return quantize(value, FIT_PLACES)


def percent_text(ratio):
    return None if ratio is None else quantize(ratio * 100, PERCENT_PLACES)


def exact_text(value):
    return None if value is None else "%d/%d" % (value.numerator, value.denominator)


# ---------------------------------------------------------------------------
# Row classification
# ---------------------------------------------------------------------------

def classify_row(row):
    """Return the exact per-row split (Annex B/C) as a dict of Fractions."""
    fit_total = frac(row["fit_total"])
    fraction = frac(row["fmd_fraction"])
    dc_spf = frac(row["dc_spf"])
    dc_latent = frac(row["dc_latent"])
    for name, value in (("fit_total", fit_total), ("fmd_fraction", fraction),
                        ("dc_spf", dc_spf), ("dc_latent", dc_latent)):
        if value < 0:
            raise FmedaError("%s: %s must be >= 0" % (row.get("fmeda_id"), name))
    for name, value in (("fmd_fraction", fraction), ("dc_spf", dc_spf), ("dc_latent", dc_latent)):
        if value > 1:
            raise FmedaError("%s: %s must be <= 1" % (row.get("fmeda_id"), name))
    lam = fit_total * fraction
    zero = Fraction(0)
    split = {"fit_mode": lam, "fit_safe": zero, "fit_spf_rf": zero,
             "fit_mpf_detected": zero, "fit_mpf_latent": zero}
    if row["safety_related"] == "N":
        split["classification"] = "excluded"
        return split
    has_sm = row["sm_spf"].strip() not in ("", "-") and dc_spf > 0
    if row["violates_sg"] == "Y":
        if has_sm:
            residual = lam * (1 - dc_spf)
            covered = lam * dc_spf
            split["fit_spf_rf"] = residual
            split["fit_mpf_latent"] = covered * (1 - dc_latent)
            split["fit_mpf_detected"] = covered * dc_latent
            split["classification"] = "RF"
        else:
            if row["sm_spf"].strip() not in ("", "-") or dc_spf != 0:
                raise FmedaError("%s: a single-point fault must carry sm_spf '-' and dc_spf 0"
                                 % row.get("fmeda_id"))
            split["fit_spf_rf"] = lam
            split["classification"] = "SPF"
    else:
        if has_sm or dc_spf != 0:
            raise FmedaError("%s: violates_sg=N rows must not claim a single-point safety mechanism"
                             % row.get("fmeda_id"))
        if row["mpf_relevant"] == "Y":
            split["fit_mpf_latent"] = lam * (1 - dc_latent)
            split["fit_mpf_detected"] = lam * dc_latent
            split["classification"] = "MPF"
        else:
            if dc_latent != 0 or row["sm_latent"].strip() not in ("", "-"):
                raise FmedaError("%s: safe faults carry no latent coverage" % row.get("fmeda_id"))
            split["fit_safe"] = lam
            split["classification"] = "S"
    return split


# ---------------------------------------------------------------------------
# Aggregation and metrics
# ---------------------------------------------------------------------------

def metrics_from_sums(total_sr, spf_rf, mpf_detected, mpf_latent, lifetime_hours=DEFAULT_LIFETIME_HOURS):
    """ISO 26262-5 Annex C metrics from the safety-related sums (FIT).

    ``total_sr`` = sum of every safety-related lambda (safe faults included);
    the LFM denominator ``total_sr - spf_rf`` therefore includes safe faults
    exactly as Equation C.8 prescribes. Returns exact Fractions (None when a
    metric is undefined).
    """
    total_sr, spf_rf = frac(total_sr), frac(spf_rf)
    mpf_detected, mpf_latent = frac(mpf_detected), frac(mpf_latent)
    lifetime = frac(lifetime_hours)
    if lifetime <= 0:
        raise FmedaError("lifetime_hours must be positive")
    if total_sr < 0 or spf_rf < 0 or mpf_detected < 0 or mpf_latent < 0:
        raise FmedaError("failure-rate sums must be non-negative")
    if spf_rf > total_sr or mpf_detected + mpf_latent > total_sr - spf_rf:
        raise FmedaError("failure-rate sums are inconsistent (parts exceed the total)")
    spfm = None if total_sr == 0 else 1 - spf_rf / total_sr
    lfm_denominator = total_sr - spf_rf
    if mpf_detected + mpf_latent == 0 or lfm_denominator == 0:
        lfm = None  # no multiple-point faults: the metric is not applicable
    else:
        lfm = 1 - mpf_latent / lfm_denominator
    dual_point = mpf_detected * mpf_latent * lifetime / FIT_PER_HOUR
    return {
        "spfm": spfm,
        "lfm": lfm,
        "pmhf_single_point_fit": spf_rf,
        "pmhf_dual_point_fit": dual_point,
        "pmhf_fit": spf_rf + dual_point,
        "pmhf_source_simplified_fit": spf_rf + mpf_latent,
        "lifetime_hours": lifetime,
    }


def aggregate(rows, lifetime_hours=DEFAULT_LIFETIME_HOURS):
    """Classify every row and compute the metrics; exact Fractions throughout."""
    splits = []
    sums = {key: Fraction(0) for key in ("total_all", "total_sr", "safe", "spf_rf",
                                          "mpf_detected", "mpf_latent", "excluded")}
    per_part = {}
    for row in rows:
        split = classify_row(row)
        splits.append((row, split))
        lam = split["fit_mode"]
        sums["total_all"] += lam
        part = per_part.setdefault(row["part_id"], {"fraction": Fraction(0), "fit_total": frac(row["fit_total"]),
                                                    "lambda": Fraction(0)})
        part["fraction"] += frac(row["fmd_fraction"])
        part["lambda"] += lam
        if frac(row["fit_total"]) != part["fit_total"]:
            raise FmedaError("%s: fit_total differs between rows of %s" % (row["fmeda_id"], row["part_id"]))
        if split["classification"] == "excluded":
            sums["excluded"] += lam
            continue
        sums["total_sr"] += lam
        sums["safe"] += split["fit_safe"]
        sums["spf_rf"] += split["fit_spf_rf"]
        sums["mpf_detected"] += split["fit_mpf_detected"]
        sums["mpf_latent"] += split["fit_mpf_latent"]
    for part_id, part in sorted(per_part.items()):
        if part["fraction"] != 1:
            raise FmedaError("failure-mode fractions of %s sum to %s, not 1" % (part_id, part["fraction"]))
    metrics = metrics_from_sums(sums["total_sr"], sums["spf_rf"], sums["mpf_detected"],
                                sums["mpf_latent"], lifetime_hours)
    mtbf = None if sums["total_all"] == 0 else FIT_PER_HOUR / sums["total_all"]
    pmhf = metrics["pmhf_fit"]
    metrics["mtbf_hours"] = mtbf
    metrics["mean_time_to_sg_violation_hours"] = None if pmhf == 0 else FIT_PER_HOUR / pmhf
    return {"rows": splits, "sums": sums, "metrics": metrics, "per_part": per_part}


# ---------------------------------------------------------------------------
# Loading and validation
# ---------------------------------------------------------------------------

def _read_text(path):
    with open(path, "r", encoding="utf-8", newline="") as handle:
        return handle.read()


def _read_json(path):
    def unique(pairs):
        result = {}
        for key, value in pairs:
            if key in result:
                raise FmedaError("duplicate JSON key %r in %s" % (key, path))
            result[key] = value
        return result
    try:
        return json.loads(_read_text(path), object_pairs_hook=unique)
    except (OSError, ValueError) as error:
        raise FmedaError("%s: %s" % (path, error))


def _validate(document, schema_path, label):
    if jsonschema is None:
        return
    schema = _read_json(schema_path)
    try:
        jsonschema.Draft202012Validator.check_schema(schema)
    except jsonschema.SchemaError as error:
        raise FmedaError("%s is not a valid schema: %s" % (schema_path, error.message))
    errors = sorted(jsonschema.Draft202012Validator(
        schema, format_checker=jsonschema.FormatChecker()).iter_errors(document),
        key=lambda e: (str(list(e.absolute_path)), e.message))
    if errors:
        raise FmedaError("; ".join("%s %s: %s" % (label, list(e.absolute_path), e.message) for e in errors))


def load_fit_database(path, schema_path=None):
    document = _read_json(path)
    if schema_path:
        _validate(document, schema_path, os.path.basename(path))
    standards = {item["standard_id"] for item in document["standards"]}
    entries = {}
    for entry in document["entries"]:
        if entry["fit_id"] in entries:
            raise FmedaError("%s: duplicate fit_id %s" % (path, entry["fit_id"]))
        if entry["standard"] not in standards:
            raise FmedaError("%s: %s cites undeclared standard %r" % (path, entry["fit_id"], entry["standard"]))
        entries[entry["fit_id"]] = entry
    return document, entries


def load_rows(csv_path, schema_path=None, fit_entries=None):
    """Parse the FMEDA CSV; every row is typed and schema-validated."""
    try:
        raw = _read_text(csv_path)
    except OSError as error:
        raise FmedaError("%s: %s" % (csv_path, error))
    reader = csv.reader(io.StringIO(raw))
    try:
        header = tuple(next(reader))
    except StopIteration:
        raise FmedaError("%s is empty" % csv_path)
    if header != CSV_HEADER:
        raise FmedaError("%s header must be exactly %s" % (csv_path, ",".join(CSV_HEADER)))
    schema = None
    if schema_path and jsonschema is not None:
        schema = _read_json(schema_path)
    rows, seen = [], set()
    for number, record in enumerate(reader, start=2):
        if len(record) != len(CSV_HEADER):
            raise FmedaError("%s line %d: expected %d fields, got %d" % (csv_path, number, len(CSV_HEADER), len(record)))
        row = dict(zip(CSV_HEADER, record))
        typed = dict(row)
        for column in INPUT_NUMERIC + DERIVED_NUMERIC:
            try:
                typed[column] = float(Fraction(row[column]))
            except (ValueError, ZeroDivisionError):
                raise FmedaError("%s line %d: %s is not numeric: %r" % (csv_path, number, column, row[column]))
        if schema is not None:
            errors = sorted(jsonschema.Draft202012Validator(
                schema, format_checker=jsonschema.FormatChecker()).iter_errors(typed),
                key=lambda e: (str(list(e.absolute_path)), e.message))
            if errors:
                raise FmedaError("%s line %d: %s: %s" % (csv_path, number, list(errors[0].absolute_path), errors[0].message))
        if row["fmeda_id"] in seen:
            raise FmedaError("%s line %d: duplicate fmeda_id %s" % (csv_path, number, row["fmeda_id"]))
        seen.add(row["fmeda_id"])
        if fit_entries is not None:
            entry = fit_entries.get(row["fit_id"])
            if entry is None:
                raise FmedaError("%s line %d: fit_id %s is not in the FIT database" % (csv_path, number, row["fit_id"]))
            if entry["part_id"] != row["part_id"]:
                raise FmedaError("%s line %d: %s belongs to %s, row cites %s"
                                 % (csv_path, number, row["fit_id"], entry["part_id"], row["part_id"]))
            if frac(entry["fit_rate_per_1e9_hours"]) != frac(row["fit_total"]):
                raise FmedaError("%s line %d: fit_total %s != FIT database rate %s for %s"
                                 % (csv_path, number, row["fit_total"], entry["fit_rate_per_1e9_hours"], row["fit_id"]))
        row["_line"] = number
        rows.append(row)
    if not rows:
        raise FmedaError("%s carries no rows" % csv_path)
    return rows


def check_derived_columns(result):
    """The CSV's derived columns must equal the exact split at 4 decimals."""
    problems = []
    for row, split in result["rows"]:
        for column in DERIVED_NUMERIC:
            expected = quantize(split[column], DERIVED_PLACES)
            actual = quantize(frac(row[column]), DERIVED_PLACES)
            if expected != actual:
                problems.append("%s: %s is %s, exact split gives %s" % (row["fmeda_id"], column, row[column], expected))
        if row["classification"] != split["classification"]:
            problems.append("%s: classification %s, exact split gives %s"
                            % (row["fmeda_id"], row["classification"], split["classification"]))
    if problems:
        raise FmedaError("derived-column drift: " + "; ".join(problems))


def load_targets(path):
    """ASIL targets from the schema-validated extract (rule 2)."""
    document = _read_json(path)
    values = {entry["name"]: frac(entry["value"]) for entry in document["entries"]}
    required = ("spfm_target_asil_b", "lfm_target_asil_b", "pmhf_target_asil_b_per_hour",
                "pmhf_project_target_per_hour", "lifetime_hours_default")
    missing = [name for name in required if name not in values]
    if missing:
        raise FmedaError("%s lacks target entries %s" % (path, missing))
    return values


# ---------------------------------------------------------------------------
# Report
# ---------------------------------------------------------------------------

def build_report(result, targets=None, asil="B"):
    metrics = result["metrics"]
    sums = result["sums"]
    report = {
        "tool": {"id": TOOL_ID, "version": TOOL_VERSION,
                 "method": "ISO 26262-5:2018 Annex C (C.1-C.8), exact rational arithmetic"},
        "inputs": {
            "rows": len(result["rows"]),
            "parts": sorted(result["per_part"]),
            "lifetime_hours": quantize(metrics["lifetime_hours"], 0),
        },
        "sums_fit": {
            "total_all_parts": fit_text(sums["total_all"]),
            "excluded_not_safety_related": fit_text(sums["excluded"]),
            "total_safety_related": fit_text(sums["total_sr"]),
            "safe": fit_text(sums["safe"]),
            "single_point_plus_residual": fit_text(sums["spf_rf"]),
            "multiple_point_detected_perceived": fit_text(sums["mpf_detected"]),
            "multiple_point_latent": fit_text(sums["mpf_latent"]),
        },
        "metrics": {
            "spfm_percent": percent_text(metrics["spfm"]),
            "lfm_percent": percent_text(metrics["lfm"]),
            "lfm_note": None if metrics["lfm"] is not None else "no multiple-point faults: LFM not applicable",
            "pmhf_single_point_fit": fit_text(metrics["pmhf_single_point_fit"]),
            "pmhf_dual_point_fit": fit_text(metrics["pmhf_dual_point_fit"]),
            "pmhf_fit": fit_text(metrics["pmhf_fit"]),
            "pmhf_per_hour": _sci(metrics["pmhf_fit"] / FIT_PER_HOUR),
            "pmhf_source_simplified_fit": fit_text(metrics["pmhf_source_simplified_fit"]),
            "mtbf_hours": quantize(metrics["mtbf_hours"], 1),
            "mean_time_to_sg_violation_hours": quantize(metrics["mean_time_to_sg_violation_hours"], 1),
        },
        "exact": {
            "spfm": exact_text(metrics["spfm"]),
            "lfm": exact_text(metrics["lfm"]),
            "pmhf_fit": exact_text(metrics["pmhf_fit"]),
            "mtbf_hours": exact_text(metrics["mtbf_hours"]),
        },
        "rows": [
            {
                "fmeda_id": row["fmeda_id"],
                "part_id": row["part_id"],
                "failure_mode": row["failure_mode"],
                "classification": split["classification"],
                "fit_mode": fit_text(split["fit_mode"]),
                "fit_safe": fit_text(split["fit_safe"]),
                "fit_spf_rf": fit_text(split["fit_spf_rf"]),
                "fit_mpf_detected": fit_text(split["fit_mpf_detected"]),
                "fit_mpf_latent": fit_text(split["fit_mpf_latent"]),
            }
            for row, split in result["rows"]
        ],
    }
    if targets is not None:
        report["targets"] = compare_targets(metrics, targets, asil)
    return report


def _sci(value):
    """Deterministic scientific notation with 4 significant digits."""
    if value is None:
        return None
    if value == 0:
        return "0.000e+00"
    exact = Decimal(value.numerator) / Decimal(value.denominator)
    return "%.3e" % exact


def compare_targets(metrics, targets, asil="B"):
    key = asil.lower()
    spfm_target = targets["spfm_target_asil_%s" % key]
    lfm_target = targets["lfm_target_asil_%s" % key]
    pmhf_iso = targets["pmhf_target_asil_%s_per_hour" % key] * FIT_PER_HOUR
    pmhf_project = targets["pmhf_project_target_per_hour"] * FIT_PER_HOUR

    def verdict(value, target, higher_is_better):
        if value is None:
            return "n/a"
        ok = value >= target if higher_is_better else value < target
        return "PASS" if ok else "FAIL"

    spfm = None if metrics["spfm"] is None else metrics["spfm"] * 100
    lfm = None if metrics["lfm"] is None else metrics["lfm"] * 100
    return {
        "asil": asil,
        "spfm": {"target_percent": quantize(spfm_target, 2), "value_percent": percent_text(metrics["spfm"]),
                 "verdict": verdict(spfm, spfm_target, True)},
        "lfm": {"target_percent": quantize(lfm_target, 2), "value_percent": percent_text(metrics["lfm"]),
                "verdict": verdict(lfm, lfm_target, True)},
        "pmhf_iso_table6": {"target_fit": fit_text(pmhf_iso), "value_fit": fit_text(metrics["pmhf_fit"]),
                            "verdict": verdict(metrics["pmhf_fit"], pmhf_iso, False)},
        "pmhf_project_issue56": {"target_fit": fit_text(pmhf_project), "value_fit": fit_text(metrics["pmhf_fit"]),
                                 "verdict": verdict(metrics["pmhf_fit"], pmhf_project, False)},
    }


def render_json(report):
    return json.dumps(report, indent=2, sort_keys=True) + "\n"


def render_metrics_markdown(report):
    metrics = report["metrics"]
    sums = report["sums_fit"]
    lines = [
        "| Quantity | Value | Note |",
        "|---|---|---|",
        "| Parts / rows analysed | %d / %d | %s |" % (len(report["inputs"]["parts"]), report["inputs"]["rows"],
                                                      ", ".join(report["inputs"]["parts"])),
        "| Total failure rate, all parts | %s FIT | reliability basis (MTBF) |" % sums["total_all_parts"],
        "| Safety-related failure rate | %s FIT | excluded (not safety related): %s FIT |"
        % (sums["total_safety_related"], sums["excluded_not_safety_related"]),
        "| Safe faults | %s FIT | |" % sums["safe"],
        "| Single-point + residual faults | %s FIT | numerator of SPFM |" % sums["single_point_plus_residual"],
        "| Multiple-point, detected/perceived | %s FIT | |" % sums["multiple_point_detected_perceived"],
        "| Multiple-point, latent | %s FIT | numerator of LFM |" % sums["multiple_point_latent"],
        "| **SPFM** | **%s %%** | 1 - (SPF+RF)/SR |" % metrics["spfm_percent"],
        "| **LFM** | **%s %%** | 1 - MPF,L/(SR - SPF - RF)%s |"
        % (metrics["lfm_percent"] if metrics["lfm_percent"] is not None else "n/a",
           "" if metrics["lfm_note"] is None else "; " + metrics["lfm_note"]),
        "| **PMHF** | **%s FIT** (%s /h) | SPF+RF %s FIT + dual-point term %s FIT (T_lifetime %s h) |"
        % (metrics["pmhf_fit"], metrics["pmhf_per_hour"], metrics["pmhf_single_point_fit"],
           metrics["pmhf_dual_point_fit"], report["inputs"]["lifetime_hours"]),
        "| MTBF (reliability, 1e9 h / total FIT) | %s h | all parts, constant failure rate |" % metrics["mtbf_hours"],
        "| Mean time to safety-goal violation (1e9 h / PMHF) | %s h | issue #56 '1/PMHF' figure; not the reliability MTBF |"
        % metrics["mean_time_to_sg_violation_hours"],
    ]
    if "targets" in report:
        targets = report["targets"]
        lines += [
            "",
            "| Target (ASIL %s alignment) | Required | Achieved | Verdict |" % targets["asil"],
            "|---|---|---|---|",
            "| SPFM (ISO 26262-5:2018 Table 4) | >= %s %% | %s %% | %s |"
            % (targets["spfm"]["target_percent"], targets["spfm"]["value_percent"], targets["spfm"]["verdict"]),
            "| LFM (Table 5) | >= %s %% | %s %% | %s |"
            % (targets["lfm"]["target_percent"], targets["lfm"]["value_percent"] or "n/a", targets["lfm"]["verdict"]),
            "| PMHF (Table 6, ASIL %s) | < %s FIT | %s FIT | %s |"
            % (targets["asil"], targets["pmhf_iso_table6"]["target_fit"], targets["pmhf_iso_table6"]["value_fit"],
               targets["pmhf_iso_table6"]["verdict"]),
            "| PMHF (issue #56 project target) | < %s FIT | %s FIT | %s |"
            % (targets["pmhf_project_issue56"]["target_fit"], targets["pmhf_project_issue56"]["value_fit"],
               targets["pmhf_project_issue56"]["verdict"]),
        ]
    return "\n".join(lines) + "\n"


def render_rows_markdown(result):
    lines = ["| fmeda_id | part | failure mode | lambda (FIT) | class | safe | SPF+RF | MPF detected | MPF latent | SM (SPF) / DC | latent detection / DC |",
             "|---|---|---|---|---|---|---|---|---|---|---|"]
    for row, split in result["rows"]:
        lines.append("| %s | %s | %s | %s | %s | %s | %s | %s | %s | %s / %s | %s / %s |" % (
            row["fmeda_id"], row["part_id"], row["failure_mode"], fit_text(split["fit_mode"]),
            split["classification"], fit_text(split["fit_safe"]), fit_text(split["fit_spf_rf"]),
            fit_text(split["fit_mpf_detected"]), fit_text(split["fit_mpf_latent"]),
            row["sm_spf"], quantize(frac(row["dc_spf"]) * 100, 0) + " %",
            row["sm_latent"], quantize(frac(row["dc_latent"]) * 100, 0) + " %"))
    return "\n".join(lines) + "\n"


def render_markdown(report, result):
    return ("## FMEDA metrics\n\n" + render_metrics_markdown(report) + "\n## Rows\n\n" + render_rows_markdown(result))


# ---------------------------------------------------------------------------
# Document blocks (docs/hw/fmeda.md generated views)
# ---------------------------------------------------------------------------

def _replace_block(text, begin, end, body):
    start = text.find(begin)
    stop = text.find(end)
    if start < 0 or stop < 0 or stop < start:
        raise FmedaError("%s: block markers %s / %s missing or out of order" % (FMEDA_DOC, begin, end))
    return text[:start + len(begin)] + "\n" + body + text[stop:]


def _block_body(text, begin, end):
    start = text.find(begin)
    stop = text.find(end)
    if start < 0 or stop < 0 or stop < start:
        raise FmedaError("%s: block markers %s / %s missing or out of order" % (FMEDA_DOC, begin, end))
    return text[start + len(begin):stop].strip("\n")


def check_doc(doc_path, report, result):
    text = _read_text(doc_path)
    drift = []
    if _block_body(text, METRICS_BEGIN, METRICS_END) != render_metrics_markdown(report).strip("\n"):
        drift.append("metrics block")
    if _block_body(text, ROWS_BEGIN, ROWS_END) != render_rows_markdown(result).strip("\n"):
        drift.append("rows block")
    if drift:
        raise FmedaError("%s: %s drifted from the calculator output (run: python3 tools/fmeda-calculator.py --write-doc)"
                         % (doc_path, " and ".join(drift)))


def write_doc(doc_path, report, result):
    text = _read_text(doc_path)
    text = _replace_block(text, METRICS_BEGIN, METRICS_END, render_metrics_markdown(report))
    text = _replace_block(text, ROWS_BEGIN, ROWS_END, render_rows_markdown(result))
    with open(doc_path, "w", encoding="utf-8", newline="") as handle:
        handle.write(text)


# ---------------------------------------------------------------------------
# Pipeline
# ---------------------------------------------------------------------------

def compute(root=".", csv_path=None, fit_database=None, targets_extract=None, lifetime_hours=None,
            asil="B", verify_derived=True):
    """Load, validate and compute; returns (report, result)."""
    root = os.path.abspath(root)
    csv_path = csv_path or os.path.join(root, FMEDA_CSV)
    fit_database = fit_database or os.path.join(root, FIT_DATABASE)
    targets_extract = targets_extract or os.path.join(root, TARGETS_EXTRACT)
    _, fit_entries = load_fit_database(fit_database, os.path.join(root, FIT_SCHEMA))
    rows = load_rows(csv_path, os.path.join(root, ROW_SCHEMA), fit_entries)
    targets = load_targets(targets_extract)
    if lifetime_hours is None:
        lifetime_hours = targets["lifetime_hours_default"]
    result = aggregate(rows, frac(lifetime_hours))
    if verify_derived:
        check_derived_columns(result)
    return build_report(result, targets, asil), result


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--root", default=".", help="repository root (default: .)")
    parser.add_argument("--csv", default=None, help="FMEDA CSV (default: <root>/%s)" % FMEDA_CSV)
    parser.add_argument("--fit-database", default=None, help="FIT database JSON (default: <root>/%s)" % FIT_DATABASE)
    parser.add_argument("--targets-extract", default=None, help="ASIL target extract (default: <root>/%s)" % TARGETS_EXTRACT)
    parser.add_argument("--lifetime-hours", default=None,
                        help="T_lifetime for the dual-point PMHF term (default: the extract's lifetime_hours_default, 1e5 h)")
    parser.add_argument("--asil", default="B", choices=("B", "C", "D"), help="ASIL whose Table 4/5/6 targets are compared")
    parser.add_argument("--format", default="json", choices=("json", "md"))
    parser.add_argument("--out", default=None, help="write the report to this file instead of stdout")
    parser.add_argument("--check", action="store_true",
                        help="consistency gate: derived CSV columns and the docs/hw/fmeda.md generated blocks must match")
    parser.add_argument("--write-doc", action="store_true", help="regenerate the docs/hw/fmeda.md generated blocks")
    args = parser.parse_args(argv)
    try:
        report, result = compute(args.root, args.csv, args.fit_database, args.targets_extract,
                                 args.lifetime_hours, args.asil)
        doc_path = os.path.join(os.path.abspath(args.root), FMEDA_DOC)
        if args.write_doc:
            write_doc(doc_path, report, result)
        if args.check:
            check_doc(doc_path, report, result)
            print("fmeda-calculator --check: PASS (%d rows; SPFM %s %%, LFM %s %%, PMHF %s FIT, MTBF %s h)"
                  % (report["inputs"]["rows"], report["metrics"]["spfm_percent"],
                     report["metrics"]["lfm_percent"], report["metrics"]["pmhf_fit"], report["metrics"]["mtbf_hours"]))
            return 0
    except FmedaError as error:
        print("fmeda-calculator: FAIL: %s" % error, file=sys.stderr)
        return 1
    output = render_json(report) if args.format == "json" else render_markdown(report, result)
    if args.out:
        with open(args.out, "w", encoding="utf-8", newline="") as handle:
            handle.write(output)
    else:
        sys.stdout.write(output)
    return 0


if __name__ == "__main__":
    sys.exit(main())
