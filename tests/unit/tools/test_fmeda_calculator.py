"""Tool qualification of tools/fmeda-calculator.py (tool id ``fmeda``, TCL2).

ISO 26262-8:2018 section 13 qualification by validation against reference
computations (issue #56, H-09; HW-SF-005 / OR-004, HW-NF-004):

* an exact-arithmetic oracle implementing ISO 26262-5:2018 Annex C
  equations C.1..C.8 independently of the tool, compared as Fractions;
* the public TI SLYP685 teaching example, iterations n = 1..4, taken from the
  schema-validated extract hw/bom/datasheets/extract-ti-slyp685-fmeda-example.json
  (every FIT-level value reproduced exactly, percentages to the published
  one-decimal precision where the publication is correctly rounded, and the
  two publication defects asserted as such);
* the Chalmers 2023 aggregate example (extract-chalmers-2023-fmeda-example.json)
  to two decimals as published;
* the repository FMEDA (hw/fmeda/fmeda-analysis.csv), byte-identical output
  across runs, and every fail-closed path of the CLI.

The declared gap (ISO 26262-5:2018 Annex E Table E.1 is not reproduced
because its values are not publicly available) is recorded in
docs/hw/tool-qualification.md section 4.4.
"""
import importlib.util
import json
import shutil
from decimal import Decimal
from fractions import Fraction

import pytest

from conftest import REPO_ROOT

SPEC = importlib.util.spec_from_file_location("fmeda_calculator", REPO_ROOT / "tools" / "fmeda-calculator.py")
calc = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(calc)

TI_EXTRACT = "hw/bom/datasheets/extract-ti-slyp685-fmeda-example.json"
CHALMERS_EXTRACT = "hw/bom/datasheets/extract-chalmers-2023-fmeda-example.json"


def extract_values(relative):
    document = json.loads((REPO_ROOT / relative).read_text(encoding="utf-8"))
    return {entry["name"]: Fraction(repr(entry["value"])) for entry in document["entries"]}


def row(fmeda_id, part_id, fit_id, fit_total, fraction, safety_related="Y", violates="N", mpf="N",
        sm_spf="-", dc_spf="0", sm_latent="-", dc_latent="0", component="c", failure_mode="m"):
    return {
        "fmeda_id": fmeda_id, "part_id": part_id, "component": component, "fit_id": fit_id,
        "fit_total": str(fit_total), "failure_mode": failure_mode, "fmd_fraction": str(fraction),
        "fmd_basis": "assumption", "fit_mode": "0", "safety_related": safety_related,
        "violates_sg": violates, "mpf_relevant": mpf, "sm_spf": sm_spf, "dc_spf": str(dc_spf),
        "sm_latent": sm_latent, "dc_latent": str(dc_latent), "classification": "S", "fit_safe": "0",
        "fit_spf_rf": "0", "fit_mpf_detected": "0", "fit_mpf_latent": "0",
        "requirement_ids": "HW-SF-005", "note": "fixture",
    }


def oracle(rows, lifetime=Fraction(100000)):
    """Independent Annex C computation: no shared code with the tool."""
    total = spf_rf = mpf_l = mpf_dp = Fraction(0)
    for r in rows:
        if r["safety_related"] != "Y":
            continue
        lam = Fraction(r["fit_total"]) * Fraction(r["fmd_fraction"])
        total += lam
        dc, dcl = Fraction(r["dc_spf"]), Fraction(r["dc_latent"])
        if r["violates_sg"] == "Y":
            spf_rf += lam * (1 - dc)
            mpf_l += lam * dc * (1 - dcl)
            mpf_dp += lam * dc * dcl
        elif r["mpf_relevant"] == "Y":
            mpf_l += lam * (1 - dcl)
            mpf_dp += lam * dcl
    spfm = 1 - spf_rf / total
    lfm = None if mpf_l + mpf_dp == 0 else 1 - mpf_l / (total - spf_rf)
    pmhf = spf_rf + mpf_dp * mpf_l * lifetime / Fraction(10 ** 9)
    return {"total": total, "spf_rf": spf_rf, "mpf_l": mpf_l, "mpf_dp": mpf_dp, "spfm": spfm, "lfm": lfm, "pmhf": pmhf}


def one_decimal(ratio):
    return calc.quantize(ratio * 100, 1)


def pub(value):
    """Published decimal text of an extract value (Fractions print as p/q)."""
    return str(Decimal(value.numerator) / Decimal(value.denominator))


def truncated_one_decimal(ratio):
    exact = Decimal(ratio.numerator) / Decimal(ratio.denominator) * 100
    return str(exact.quantize(Decimal("0.1"), rounding="ROUND_DOWN"))


# --- TI SLYP685 teaching example (published fixture) ---------------------------

def ti_rows(iteration):
    v = extract_values(TI_EXTRACT)
    sensor, mcu, fan, wd, light = v["sensor_fit"], v["mcu_fit"], v["fan_fit"], v["wd_fit"], v["warning_light_fit"]
    dc = {1: "0", 2: pub(v["dc_wd_n2"]), 3: pub(v["dc_wd_n2"]), 4: pub(v["dc_wd_n4"])}[iteration]
    dc_latent = "0" if iteration < 3 else pub(v["dc_latent_mcu_n3"])
    rows = [row("FMEDA-001", "PRT-001", "FIT-001", sensor, "1", violates="Y")]
    if iteration == 1:
        rows.append(row("FMEDA-002", "PRT-002", "FIT-002", mcu, "0.5", violates="Y"))
    else:
        rows.append(row("FMEDA-002", "PRT-002", "FIT-002", mcu, "0.5", violates="Y", sm_spf="SM1 watchdog",
                        dc_spf=dc, sm_latent="-" if iteration < 3 else "warning light", dc_latent=dc_latent))
    rows.append(row("FMEDA-003", "PRT-002", "FIT-002", mcu, "0.5"))
    if iteration < 4:
        rows.append(row("FMEDA-004", "PRT-003", "FIT-003", fan, "0.5", violates="Y"))
        rows.append(row("FMEDA-005", "PRT-003", "FIT-003", fan, "0.5"))
    else:
        for index, part in ((4, "PRT-003"), (6, "PRT-005")):
            rows.append(row("FMEDA-%03d" % index, part, "FIT-003", fan, "0.5", mpf="Y"))
            rows.append(row("FMEDA-%03d" % (index + 1), part, "FIT-003", fan, "0.5"))
    if iteration >= 2:
        rows.append(row("FMEDA-008", "PRT-004", "FIT-004", wd, "1", mpf="Y"))
    if iteration >= 3:
        rows.append(row("FMEDA-009", "PRT-006", "FIT-005", light, "1", safety_related="N"))
    return rows, v


def test_ti_slyp685_iteration_1_no_safety_mechanism():
    rows, v = ti_rows(1)
    result = calc.aggregate(rows)
    sums, metrics = result["sums"], result["metrics"]
    assert sums["total_sr"] == v["n1_total_fit"] == 111
    assert sums["spf_rf"] == v["n1_spf_rf_fit"] == 56
    assert sums["mpf_latent"] == 0 and sums["mpf_detected"] == 0
    assert one_decimal(metrics["spfm"]) == pub(v["n1_spfm_percent"]) == "49.5"
    assert metrics["lfm"] is None  # slide: LFM 'irrelevant'
    assert metrics["pmhf_fit"] == metrics["pmhf_source_simplified_fit"] == 56
    assert [split["classification"] for _, split in result["rows"]] == ["SPF", "SPF", "S", "SPF", "S"]


def test_ti_slyp685_iteration_2_watchdog_dc_90():
    rows, v = ti_rows(2)
    result = calc.aggregate(rows)
    sums, metrics = result["sums"], result["metrics"]
    assert sums["total_sr"] == v["n2_total_fit"] == 121
    assert sums["spf_rf"] == v["n2_spf_rf_fit"] == 11
    assert sums["mpf_latent"] == v["n2_mpf_latent_fit"] == 55
    assert one_decimal(metrics["spfm"]) == pub(v["n2_spfm_percent"]) == "90.9"
    assert one_decimal(metrics["lfm"]) == calc.quantize(v["n2_lfm_percent"], 1) == "50.0"
    assert metrics["pmhf_source_simplified_fit"] == v["n2_pmhf_source_fit"] == 66
    assert metrics["pmhf_fit"] == 11  # no detected multiple-point faults: dual-point term is zero
    assert dict(result["rows"][1][1], classification=None)["fit_spf_rf"] == 5


def test_ti_slyp685_iteration_3_warning_light_perceives_latent_faults():
    rows, v = ti_rows(3)
    result = calc.aggregate(rows)
    sums, metrics = result["sums"], result["metrics"]
    assert sums["excluded"] == v["warning_light_fit"] == 1
    assert sums["total_sr"] == 121 and sums["spf_rf"] == 11
    assert sums["mpf_latent"] == v["n3_mpf_latent_fit"] == 10
    assert sums["mpf_detected"] == 45
    assert metrics["pmhf_source_simplified_fit"] == v["n3_pmhf_source_fit"] == 21
    # Publication defect: the slide prints 90.1 % for 1 - 10/(121 - 11); the fraction is 90.9 %.
    assert one_decimal(metrics["lfm"]) == "90.9" != pub(v["n3_lfm_printed_percent"])
    assert metrics["lfm"] == 1 - Fraction(10, 110)


def test_ti_slyp685_iteration_4_dc_99_and_redundant_fans():
    rows, v = ti_rows(4)
    result = calc.aggregate(rows)
    sums, metrics = result["sums"], result["metrics"]
    assert sums["total_sr"] == v["n4_total_fit"] == 131
    assert sums["spf_rf"] == v["n4_spf_rf_fit"] == Fraction(3, 2)
    assert sums["mpf_latent"] == v["n4_mpf_latent_fit"] == 20
    assert metrics["pmhf_source_simplified_fit"] == v["n4_pmhf_source_fit"] == Fraction(43, 2)
    # Publication defect: the slide truncates instead of rounding.
    assert truncated_one_decimal(metrics["spfm"]) == pub(v["n4_spfm_printed_percent"]) == "98.8"
    assert truncated_one_decimal(metrics["lfm"]) == pub(v["n4_lfm_printed_percent"]) == "84.5"
    assert one_decimal(metrics["spfm"]) == "98.9" and one_decimal(metrics["lfm"]) == "84.6"
    assert metrics["spfm"] == 1 - Fraction(3, 2) / 131
    assert metrics["lfm"] == 1 - Fraction(20) / (131 - Fraction(3, 2))


@pytest.mark.parametrize("iteration", [1, 2, 3, 4])
def test_ti_iterations_agree_with_the_independent_oracle(iteration):
    rows, _ = ti_rows(iteration)
    result = calc.aggregate(rows)
    expected = oracle(rows)
    assert result["sums"]["total_sr"] == expected["total"]
    assert result["sums"]["spf_rf"] == expected["spf_rf"]
    assert result["sums"]["mpf_latent"] == expected["mpf_l"]
    assert result["sums"]["mpf_detected"] == expected["mpf_dp"]
    assert result["metrics"]["spfm"] == expected["spfm"]
    assert result["metrics"]["lfm"] == expected["lfm"]
    assert result["metrics"]["pmhf_fit"] == expected["pmhf"]


# --- Chalmers 2023 aggregate example -----------------------------------------------

def test_chalmers_2023_aggregate_example_to_published_precision():
    v = extract_values(CHALMERS_EXTRACT)
    metrics = calc.metrics_from_sums(v["safety_related_fit"], v["spf_rf_fit"], v["mpf_detected_fit"],
                                     v["mpf_latent_fit"], v["lifetime_hours"])
    assert calc.percent_text(metrics["spfm"]) == pub(v["spfm_percent"]) == "94.56"
    assert calc.percent_text(metrics["lfm"]) == pub(v["lfm_percent"]) == "93.84"
    assert calc.quantize(metrics["pmhf_fit"], 3) == "6.962" == pub(v["pmhf_fit"])
    assert metrics["pmhf_dual_point_fit"] == Fraction("76.372") * Fraction("7.4") * Fraction(100000, 10 ** 9)
    assert v["total_fit"] - v["non_safety_related_fit"] == v["safety_related_fit"]


# --- exact-arithmetic oracle over a parameter sweep ----------------------------------

@pytest.mark.parametrize("dc_spf,dc_latent,fraction", [
    ("0", "0", "1"), ("0.6", "0.6", "0.25"), ("0.9", "0.99", "0.4"), ("0.99", "0.9", "0.7"),
    ("0.999", "0", "0.5"), ("1", "1", "0.3")])
def test_row_split_is_exact_for_every_dc_class(dc_spf, dc_latent, fraction):
    fit_total = "137.5"
    covered = row("FMEDA-001", "PRT-001", "FIT-001", fit_total, fraction, violates="Y",
                  sm_spf="-" if dc_spf == "0" else "SM", dc_spf=dc_spf, dc_latent=dc_latent if dc_spf != "0" else "0")
    remainder = row("FMEDA-002", "PRT-001", "FIT-001", fit_total, str(1 - Fraction(fraction)), mpf="Y",
                    sm_latent="check", dc_latent=dc_latent)
    if Fraction(fraction) == 1:
        remainder = None
    rows = [covered] + ([remainder] if remainder else [])
    result = calc.aggregate(rows)
    expected = oracle(rows)
    lam = Fraction(fit_total) * Fraction(fraction)
    split = result["rows"][0][1]
    assert split["fit_spf_rf"] == lam * (1 - Fraction(dc_spf))
    if dc_spf != "0":
        assert split["fit_mpf_latent"] == lam * Fraction(dc_spf) * (1 - Fraction(dc_latent))
        assert split["fit_mpf_detected"] == lam * Fraction(dc_spf) * Fraction(dc_latent)
    assert result["metrics"]["spfm"] == expected["spfm"]
    assert result["metrics"]["lfm"] == expected["lfm"]
    assert result["metrics"]["pmhf_fit"] == expected["pmhf"]
    assert result["metrics"]["mtbf_hours"] == Fraction(10 ** 9) / Fraction(fit_total)


def test_metrics_from_sums_edge_cases():
    empty = calc.metrics_from_sums(0, 0, 0, 0)
    assert empty["spfm"] is None and empty["lfm"] is None and empty["pmhf_fit"] == 0
    only_safe = calc.metrics_from_sums(10, 0, 0, 0)
    assert only_safe["spfm"] == 1 and only_safe["lfm"] is None
    with_lifetime = calc.metrics_from_sums(100, 10, 50, 20, lifetime_hours="2e5")
    assert with_lifetime["pmhf_dual_point_fit"] == Fraction(50 * 20 * 200000, 10 ** 9)
    for bad in ((100, 110, 0, 0), (100, 10, 80, 20), (-1, 0, 0, 0)):
        with pytest.raises(calc.FmedaError):
            calc.metrics_from_sums(*bad)
    with pytest.raises(calc.FmedaError, match="lifetime_hours must be positive"):
        calc.metrics_from_sums(1, 0, 0, 0, lifetime_hours=0)


def test_number_coercion_is_exact_and_strict():
    assert calc.frac("0.1") + calc.frac("0.2") == Fraction(3, 10)
    assert calc.frac(0.1) == Fraction("0.1") and calc.frac(3) == 3 and calc.frac(Fraction(1, 3)) == Fraction(1, 3)
    for bad in ("abc", True, None, "1/0"):
        with pytest.raises(calc.FmedaError):
            calc.frac(bad)
    assert calc.quantize(Fraction(1, 8), 2) == "0.13" and calc.quantize(None, 2) is None
    assert calc.percent_text(None) is None and calc.exact_text(None) is None
    assert calc._sci(Fraction(0)) == "0.000e+00" and calc._sci(None) is None
    assert calc._sci(Fraction(365345, 10 ** 13)) == "3.653e-08"


# --- classification guards ------------------------------------------------------------

@pytest.mark.parametrize("kwargs,fragment", [
    (dict(violates="Y", sm_spf="SM", dc_spf="0"), "single-point fault must carry sm_spf '-'"),
    (dict(violates="Y", sm_spf="-", dc_spf="0.5"), "single-point fault must carry sm_spf '-'"),
    (dict(violates="N", sm_spf="SM", dc_spf="0.9"), "must not claim a single-point safety mechanism"),
    (dict(violates="N", mpf="N", dc_latent="0.9"), "safe faults carry no latent coverage"),
    (dict(violates="N", mpf="N", sm_latent="boot"), "safe faults carry no latent coverage"),
    (dict(fraction="1.5"), "fmd_fraction must be <= 1"),
    (dict(dc_spf="-0.1", violates="Y", sm_spf="SM"), "dc_spf must be >= 0"),
])
def test_inconsistent_rows_are_refused(kwargs, fragment):
    fraction = kwargs.pop("fraction", "1")
    with pytest.raises(calc.FmedaError, match=fragment):
        calc.classify_row(row("FMEDA-001", "PRT-001", "FIT-001", 10, fraction, **kwargs))


def test_aggregate_refuses_fraction_and_fit_total_inconsistencies():
    rows = [row("FMEDA-001", "PRT-001", "FIT-001", 10, "0.5"), row("FMEDA-002", "PRT-001", "FIT-001", 10, "0.4")]
    with pytest.raises(calc.FmedaError, match="fractions of PRT-001 sum to 9/10"):
        calc.aggregate(rows)
    rows[1]["fit_total"] = "12"
    rows[1]["fmd_fraction"] = "0.5"
    with pytest.raises(calc.FmedaError, match="fit_total differs between rows of PRT-001"):
        calc.aggregate(rows)


# --- repository FMEDA and the CLI -------------------------------------------------------

@pytest.fixture
def repo(tmp_path):
    for directory in ("schemas/hw", "hw/bom", "hw/fmeda"):
        shutil.copytree(REPO_ROOT / directory, tmp_path / directory)
    (tmp_path / "docs/hw").mkdir(parents=True)
    shutil.copy(REPO_ROOT / calc.FMEDA_DOC, tmp_path / calc.FMEDA_DOC)
    return tmp_path


def csv_lines(root):
    return (root / calc.FMEDA_CSV).read_text(encoding="utf-8").splitlines(keepends=True)


def test_repository_fmeda_matches_the_document_and_is_honest(repo, capsys):
    assert calc.main(["--root", str(repo), "--check"]) == 0
    out = capsys.readouterr().out
    assert out.startswith("fmeda-calculator --check: PASS (8 rows; SPFM 76.02 %, LFM 91.99 %, PMHF 36.5345 FIT")
    report, result = calc.compute(str(repo))
    assert report["targets"]["spfm"]["verdict"] == "FAIL"
    assert report["targets"]["lfm"]["verdict"] == "PASS"
    assert report["targets"]["pmhf_iso_table6"]["verdict"] == "PASS"
    assert report["targets"]["pmhf_project_issue56"]["verdict"] == "FAIL"
    assert report["exact"]["spfm"] == "2311/3040" and report["exact"]["mtbf_hours"] == "125000000/19"
    assert result["sums"]["total_all"] == 152 and result["sums"]["spf_rf"] == Fraction("36.45")
    assert report["metrics"]["mtbf_hours"] == "6578947.4"
    assert (repo / calc.FMEDA_DOC).read_bytes() == (REPO_ROOT / calc.FMEDA_DOC).read_bytes()


def test_cli_output_is_byte_identical_and_lifetime_is_a_parameter(repo, capsys, tmp_path):
    assert calc.main(["--root", str(repo)]) == 0
    first = capsys.readouterr().out
    assert calc.main(["--root", str(repo), "--out", str(tmp_path / "report.json")]) == 0
    assert (tmp_path / "report.json").read_text(encoding="utf-8") == first
    assert calc.main(["--root", str(repo), "--format", "md"]) == 0
    markdown = capsys.readouterr().out
    assert calc.main(["--root", str(repo), "--format", "md"]) == 0
    assert capsys.readouterr().out == markdown and "| **PMHF** |" in markdown
    assert calc.main(["--root", str(repo), "--lifetime-hours", "1e6", "--asil", "D"]) == 0
    longer = json.loads(capsys.readouterr().out)
    assert longer["inputs"]["lifetime_hours"] == "1000000"
    assert Fraction(longer["metrics"]["pmhf_dual_point_fit"]) == Fraction("0.8452")
    assert longer["targets"]["asil"] == "D" and longer["targets"]["pmhf_iso_table6"]["target_fit"] == "10.0000"
    assert calc.main(["--root", str(repo), "--asil", "C"]) == 0
    assert json.loads(capsys.readouterr().out)["targets"]["spfm"]["target_percent"] == "97.00"


def test_derived_column_drift_fails_closed(repo, capsys):
    lines = csv_lines(repo)
    lines[3] = lines[3].replace(",SPF,0.0000,30.0000,", ",SPF,0.0000,29.0000,")
    (repo / calc.FMEDA_CSV).write_text("".join(lines), encoding="utf-8")
    assert calc.main(["--root", str(repo), "--check"]) == 1
    assert "derived-column drift: FMEDA-003: fit_spf_rf is 29.0000" in capsys.readouterr().err
    lines[3] = lines[3].replace(",SPF,0.0000,29.0000,", ",RF,0.0000,30.0000,")
    (repo / calc.FMEDA_CSV).write_text("".join(lines), encoding="utf-8")
    assert calc.main(["--root", str(repo)]) == 1
    assert "classification RF, exact split gives SPF" in capsys.readouterr().err


def test_document_drift_fails_and_write_doc_repairs(repo, capsys):
    path = repo / calc.FMEDA_DOC
    path.write_text(path.read_text(encoding="utf-8").replace("| **SPFM** | **76.02 %** |", "| **SPFM** | **99.99 %** |"),
                    encoding="utf-8")
    assert calc.main(["--root", str(repo), "--check"]) == 1
    assert "metrics block drifted" in capsys.readouterr().err
    assert calc.main(["--root", str(repo), "--write-doc", "--check"]) == 0
    assert path.read_bytes() == (REPO_ROOT / calc.FMEDA_DOC).read_bytes()
    path.write_text(path.read_text(encoding="utf-8").replace("| FMEDA-008 | PRT-002 |", "| FMEDA-008 | PRT-009 |"),
                    encoding="utf-8")
    assert calc.main(["--root", str(repo), "--check"]) == 1
    assert "rows block drifted" in capsys.readouterr().err
    path.write_text(path.read_text(encoding="utf-8").replace(calc.ROWS_END, ""), encoding="utf-8")
    assert calc.main(["--root", str(repo), "--check"]) == 1
    assert "block markers" in capsys.readouterr().err
    assert calc.main(["--root", str(repo), "--write-doc"]) == 1


@pytest.mark.parametrize("mutate,fragment", [
    (lambda lines: lines[:1], "carries no rows"),
    (lambda lines: [], "is empty"),
    (lambda lines: ["a,b\n"] + lines[1:], "header must be exactly"),
    (lambda lines: lines + [lines[1]], "duplicate fmeda_id FMEDA-001"),
    (lambda lines: lines + ["FMEDA-009,PRT-001\n"], "expected 23 fields, got 2"),
    (lambda lines: lines[:1] + [lines[1].replace(",150,", ",lots,", 1)] + lines[2:], "fit_total is not numeric"),
    (lambda lines: lines[:1] + [lines[1].replace("FMEDA-001,", "FMEDA-1,", 1)] + lines[2:], "does not match"),
    (lambda lines: lines[:1] + [lines[1].replace(",FIT-001,", ",FIT-009,", 1)] + lines[2:], "FIT-009 is not in the FIT database"),
    (lambda lines: lines[:1] + [lines[1].replace(",FIT-001,150,", ",FIT-002,150,", 1)] + lines[2:], "FIT-002 belongs to PRT-002"),
    (lambda lines: lines[:1] + [lines[1].replace(",FIT-001,150,", ",FIT-001,151,", 1)] + lines[2:], "fit_total 151 != FIT database rate 150"),
])
def test_csv_contract_violations_fail_closed(repo, capsys, mutate, fragment):
    lines = csv_lines(repo)
    (repo / calc.FMEDA_CSV).write_text("".join(mutate(lines)), encoding="utf-8")
    assert calc.main(["--root", str(repo), "--check"]) == 1
    assert fragment in capsys.readouterr().err


def test_missing_inputs_fail_closed(repo, capsys):
    (repo / calc.FMEDA_CSV).unlink()
    assert calc.main(["--root", str(repo)]) == 1
    assert "fmeda-analysis.csv" in capsys.readouterr().err
    (repo / calc.FIT_DATABASE).write_text("{", encoding="utf-8")
    assert calc.main(["--root", str(repo)]) == 1
    assert "fit-database.json" in capsys.readouterr().err


def test_fit_database_contract(repo):
    path = repo / calc.FIT_DATABASE
    document = json.loads(path.read_text(encoding="utf-8"))
    document["entries"][1]["standard"] = "MIL-HDBK-217F"
    path.write_text(json.dumps(document), encoding="utf-8")
    with pytest.raises(calc.FmedaError, match="undeclared standard"):
        calc.compute(str(repo))
    document["entries"][1]["standard"] = "SN 29500"
    document["entries"][1]["fit_id"] = "FIT-001"
    path.write_text(json.dumps(document), encoding="utf-8")
    with pytest.raises(calc.FmedaError, match="duplicate fit_id"):
        calc.compute(str(repo))
    path.write_text('{"kind": "fit_database", "entries": [], "standards": []}', encoding="utf-8")
    with pytest.raises(calc.FmedaError, match="fit-database.json"):
        calc.compute(str(repo))
    (repo / calc.FIT_SCHEMA).write_text('{"type": "nonsense"}', encoding="utf-8")
    with pytest.raises(calc.FmedaError, match="not a valid schema"):
        calc.compute(str(repo))
    path.write_text('{"kind": "fit_database", "kind": "x"}', encoding="utf-8")
    with pytest.raises(calc.FmedaError, match="duplicate JSON key"):
        calc.compute(str(repo))


def test_targets_extract_must_carry_every_target(repo):
    path = repo / calc.TARGETS_EXTRACT
    document = json.loads(path.read_text(encoding="utf-8"))
    document["entries"] = [entry for entry in document["entries"] if entry["name"] != "lifetime_hours_default"]
    path.write_text(json.dumps(document), encoding="utf-8")
    with pytest.raises(calc.FmedaError, match="lacks target entries \\['lifetime_hours_default'\\]"):
        calc.compute(str(repo))


def test_calculator_runs_without_jsonschema(repo, monkeypatch):
    monkeypatch.setattr(calc, "jsonschema", None)
    report, _ = calc.compute(str(repo))
    assert report["metrics"]["spfm_percent"] == "76.02"


def test_report_without_targets_and_lfm_not_applicable():
    rows, _ = ti_rows(1)
    result = calc.aggregate(rows)
    report = calc.build_report(result)
    assert "targets" not in report and report["metrics"]["lfm_percent"] is None
    assert report["metrics"]["lfm_note"] == "no multiple-point faults: LFM not applicable"
    markdown = calc.render_metrics_markdown(report)
    assert "| **LFM** | **n/a %** | 1 - MPF,L/(SR - SPF - RF); no multiple-point faults" in markdown
    targets = calc.load_targets(str(REPO_ROOT / calc.TARGETS_EXTRACT))
    compared = calc.compare_targets(result["metrics"], targets)
    assert compared["lfm"]["verdict"] == "n/a" and compared["spfm"]["verdict"] == "FAIL"
    zero = calc.aggregate([row("FMEDA-001", "PRT-001", "FIT-001", 10, "1")])
    assert zero["metrics"]["mean_time_to_sg_violation_hours"] is None
    with_targets = calc.build_report(zero, targets)
    assert calc.render_metrics_markdown(with_targets).count("| n/a |") == 1
