"""H-09 WCCA / derating gate contracts: HW-NF-002, HW-NF-003, HW-FR-009 (issue #56).

Every rule of ci/check_hw_wcca.py is exercised negatively on a scratch copy
of the repository inputs: a drifted ratio, a wrong threshold, a verdict that
disagrees with the arithmetic, an unregistered / rejected / unreferenced
waiver, a missing extract entry, a BOM parameter that the WCCA does not
close, a drifted generated table and every missing-input case fail closed.
"""
import csv
import io
import json
import shutil

import pytest

from conftest import REPO_ROOT
from ci import check_hw_wcca as gate

WAIVER_HEADER = "| waiver_id | rows | rationale | status | qa_record |"


@pytest.fixture
def repo(tmp_path):
    for directory in ("schemas/hw", "hw/bom", "hw/wcca"):
        shutil.copytree(REPO_ROOT / directory, tmp_path / directory)
    (tmp_path / "docs/hw").mkdir(parents=True)
    shutil.copy(REPO_ROOT / gate.WCCA_DOC, tmp_path / gate.WCCA_DOC)
    return tmp_path


def run(root, *args):
    report, rows = gate.run(str(root))
    return report, rows


def read_rows(root):
    with open(root / gate.WCCA_CSV, newline="", encoding="utf-8") as handle:
        return list(csv.reader(handle))


def write_rows(root, table):
    buffer = io.StringIO()
    csv.writer(buffer, lineterminator="\n").writerows(table)
    (root / gate.WCCA_CSV).write_text(buffer.getvalue(), encoding="utf-8")


def edit_csv(root, wcca_id, **changes):
    table = read_rows(root)
    header = table[0]
    for record in table[1:]:
        if record[0] == wcca_id:
            for column, value in changes.items():
                record[header.index(column)] = value
    write_rows(root, table)


def edit_doc(root, old, new, count=1):
    path = root / gate.WCCA_DOC
    text = path.read_text(encoding="utf-8")
    assert text.count(old) >= 1, old
    path.write_text(text.replace(old, new, count), encoding="utf-8")


def edit_bom(root, change):
    path = root / gate.BOM_JSON
    document = json.loads(path.read_text(encoding="utf-8"))
    change(document)
    path.write_text(json.dumps(document, indent=2), encoding="utf-8")


def failures(report, rule):
    return [message for message in report.failures if message.startswith("rule %d:" % rule)]


# --- repository state -------------------------------------------------------

def test_repository_state_passes_and_is_pending_qa(repo, capsys):
    assert gate.main(["--root", str(repo)]) == 0
    out = capsys.readouterr().out
    assert "check_hw_wcca: PASS (6 rows: 4 PASS, 2 FAIL under registered waiver, 2 warnings)" in out
    assert out.count("WARNING: ") == 2 and "WCCA-W-001 is proposed" in out


def test_json_export_and_write_are_deterministic(repo, capsys):
    assert gate.main(["--root", str(repo), "--json"]) == 0
    summary = json.loads(capsys.readouterr().out)
    assert summary["result"] == "PASS" and summary["rows"] == 6
    assert summary["pass"] == 4 and summary["fail_waived"] == 2 and summary["failures"] == []
    assert gate.main(["--root", str(repo), "--export"]) == 0
    exported = capsys.readouterr().out
    assert exported.startswith("| wcca_id | part | component |") and "| WCCA-006 |" in exported
    before = (repo / gate.WCCA_DOC).read_bytes()
    assert gate.main(["--root", str(repo), "--write"]) == 0
    assert (repo / gate.WCCA_DOC).read_bytes() == before
    assert (REPO_ROOT / gate.WCCA_DOC).read_bytes() == before


@pytest.mark.parametrize("stress,category,threshold", [
    ("voltage", "capacitor", 0.8), ("current", "capacitor", 0.8),
    ("temperature", "capacitor", 0.8), ("temperature", "mcu", 0.8),
    ("voltage", "mcu", 0.7), ("current", "conductor", 0.7), ("voltage", "diode", 0.7)])
def test_thresholds_follow_hw_nf_003_and_hw_nf_002(stress, category, threshold):
    assert gate.expected_threshold(stress, category) == threshold


def test_ratio_is_rounded_half_up_to_six_places():
    assert gate.derating_ratio(3.399, 4.0) == 0.84975
    assert gate.derating_ratio(0.0000005, 1.0) == 0.000001
    assert gate.derating_ratio(85.0, 125.0) == 0.68


# --- rules 2-4: arithmetic, threshold and verdict -----------------------------

def test_ratio_drift_fails(repo):
    edit_csv(repo, "WCCA-004", derating_ratio="0.539000")
    report, _ = run(repo)
    assert failures(report, 2) and "WCCA-004" in failures(report, 2)[0]


def test_capacitor_threshold_cannot_be_relaxed_or_tightened(repo):
    edit_csv(repo, "WCCA-004", threshold="0.7")
    edit_csv(repo, "WCCA-006", threshold="0.8")
    report, _ = run(repo)
    assert len(failures(report, 3)) == 2
    assert "must be 0.8 for voltage/capacitor" in failures(report, 3)[0]
    assert "must be 0.7 for current/conductor" in failures(report, 3)[1]


def test_temperature_threshold_is_80_percent(repo):
    edit_csv(repo, "WCCA-003", threshold="0.7")
    report, _ = run(repo)
    assert "must be 0.8 for temperature/mcu" in failures(report, 3)[0]


def test_verdict_must_follow_the_arithmetic(repo):
    edit_csv(repo, "WCCA-001", verdict="PASS", waiver_id="-")
    report, _ = run(repo)
    messages = failures(report, 4)
    assert any("verdict PASS but ratio 0.84975 vs threshold 0.7 requires FAIL" in m for m in messages)


def test_ratio_above_threshold_without_waiver_fails(repo):
    edit_csv(repo, "WCCA-004", worst_case="5.2", derating_ratio="0.825397", verdict="FAIL")
    report, _ = run(repo)
    assert any("FAIL row without a WCCA-W-nnn waiver" in m for m in failures(report, 4))


def test_pass_row_must_not_carry_a_waiver(repo):
    edit_csv(repo, "WCCA-005", waiver_id="WCCA-W-001")
    edit_doc(repo, "| WCCA-W-001 | WCCA-001;WCCA-002 |", "| WCCA-W-001 | WCCA-001;WCCA-002;WCCA-005 |")
    report, _ = run(repo)
    assert any("PASS row must not carry a waiver" in m for m in failures(report, 4))


# --- rule 4: waiver register --------------------------------------------------

def test_unregistered_waiver_fails(repo):
    edit_csv(repo, "WCCA-001", waiver_id="WCCA-W-002")
    report, _ = run(repo)
    assert any("waiver WCCA-W-002 is not registered" in m for m in failures(report, 4))


def test_waiver_must_list_the_row(repo):
    edit_doc(repo, "| WCCA-W-001 | WCCA-001;WCCA-002 |", "| WCCA-W-001 | WCCA-001 |")
    report, _ = run(repo)
    assert any("WCCA-002: waiver WCCA-W-001 does not list this row" in m for m in failures(report, 4))


def test_rejected_waiver_fails_and_approved_waiver_is_silent(repo):
    edit_doc(repo, "| proposed |", "| rejected-qa |")
    report, _ = run(repo)
    assert sum("was rejected by QA" in m for m in failures(report, 4)) == 2
    edit_doc(repo, "| rejected-qa |", "| approved-qa |")
    report, _ = run(repo)
    assert report.ok and report.warnings == []


def test_registered_but_unreferenced_waiver_fails(repo):
    edit_doc(repo, WAIVER_HEADER + "\n|---|---|---|---|---|\n",
             WAIVER_HEADER + "\n|---|---|---|---|---|\n| WCCA-W-009 | WCCA-006 | spare | approved-qa | QA-1 |\n")
    report, _ = run(repo)
    assert any("waiver WCCA-W-009 is registered but no FAIL row references it" in m for m in failures(report, 4))


def test_waiver_listing_unknown_row_fails(repo):
    edit_doc(repo, "| WCCA-W-001 | WCCA-001;WCCA-002 |", "| WCCA-W-001 | WCCA-001;WCCA-002;WCCA-099 |")
    report, _ = run(repo)
    assert any("lists unknown row WCCA-099" in m for m in failures(report, 4))


@pytest.mark.parametrize("row,fragment", [
    ("| WCCA-W-001 | WCCA-001;WCCA-002 | ok | proposed |", "malformed waiver row"),
    ("| W-1 | WCCA-001;WCCA-002 | ok | proposed | QA |", "invalid or duplicated"),
    ("| WCCA-W-001 | WCCA-001;WCCA-002 | ok | maybe | QA |", "status 'maybe' not in"),
    ("| WCCA-W-001 | WCCA-001;WCCA-002 |  | proposed | QA |", "needs a rationale and a QA record"),
])
def test_malformed_waiver_rows_fail(repo, row, fragment):
    text = (repo / gate.WCCA_DOC).read_text(encoding="utf-8")
    start = text.index(gate.WAIVER_BEGIN) + len(gate.WAIVER_BEGIN)
    stop = text.index(gate.WAIVER_END)
    body = "\n" + WAIVER_HEADER + "\n|---|---|---|---|---|\n" + row + "\n"
    (repo / gate.WCCA_DOC).write_text(text[:start] + body + text[stop:], encoding="utf-8")
    report, _ = run(repo)
    assert any(fragment in m for m in failures(report, 4))


def test_duplicate_waiver_id_fails(repo):
    line = "| WCCA-W-001 | WCCA-001;WCCA-002 | dup | approved-qa | QA |"
    edit_doc(repo, gate.WAIVER_END, line + "\n" + gate.WAIVER_END)
    report, _ = run(repo)
    assert any("invalid or duplicated" in m for m in failures(report, 4))


def test_waiver_header_and_markers_are_required(repo):
    edit_doc(repo, WAIVER_HEADER, "| id | rows | why | status | qa |")
    report, _ = run(repo)
    assert any("waiver register header must be" in m for m in failures(report, 4))
    edit_doc(repo, gate.WAIVER_END, "")
    report, _ = run(repo)
    assert any("waiver register block markers" in m for m in failures(report, 4))


# --- rule 5: extracts ---------------------------------------------------------

def test_missing_extract_entry_fails(repo):
    edit_csv(repo, "WCCA-003", source_entry="Tj_max_suffix_9")
    report, _ = run(repo)
    assert any("entries ['Tj_max_suffix_9'] not found" in m for m in failures(report, 5))


def test_rated_max_must_be_a_cited_extract_value(repo):
    edit_csv(repo, "WCCA-003", rated_max="130.0", derating_ratio="0.746154")
    report, _ = run(repo)
    assert any("rated_max 130.0 is not one of the cited entry values" in m for m in failures(report, 5))


def test_missing_or_invalid_extract_fails_closed(repo):
    (repo / "hw/bom/datasheets/extract-copper-ipc2221.json").unlink()
    report, _ = run(repo)
    assert any("extract-copper-ipc2221.json: extract unusable" in m for m in failures(report, 5))
    (repo / "hw/bom/datasheets/extract-copper-ipc2221.json").write_text('{"kind": "datasheet_extract"}', encoding="utf-8")
    report, _ = run(repo)
    assert any("extract unusable" in m for m in failures(report, 5))


# --- rule 6: BOM closure ------------------------------------------------------

def test_bom_wcca_parameter_without_closed_row_fails(repo):
    def change(document):
        for part in document["parts"]:
            for parameter in part["parameters"]:
                if parameter["name"] == "R_path":
                    parameter["value"] = 0.06
    edit_bom(repo, change)
    report, _ = run(repo)
    assert any("R_path = 0.06 ohm has no matching WCCA-R-nnn" in m for m in failures(report, 6))


def test_bom_citation_must_name_the_result_and_row_must_carry_the_part(repo):
    def change(document):
        for part in document["parts"]:
            for parameter in part["parameters"]:
                if parameter["name"] == "R_path":
                    parameter["citation"] = "docs/hw/wcca-derating.md"
    edit_bom(repo, change)
    edit_doc(repo, "| hw/bom/bom.json PRT-002;", "| hw/bom/bom.json;")
    report, _ = run(repo)
    assert any("citation must name WCCA-R-001" in m for m in failures(report, 6))
    assert any("WCCA-R-001 carried_by must name PRT-002" in m for m in failures(report, 6))


def test_closed_parameter_table_is_validated(repo):
    edit_doc(repo, "| WCCA-R-001 | R_path | 0.05 |", "| WCCA-R-001 | R_path | five |")
    report, _ = run(repo)
    assert any("value 'five' is not numeric" in m for m in failures(report, 6))
    edit_doc(repo, "| WCCA-R-001 | R_path | five |", "| R-1 | R_path | 0.05 |")
    report, _ = run(repo)
    assert any("malformed closed-parameter row" in m for m in failures(report, 6))
    edit_doc(repo, "| result_id | parameter | value | unit | carried_by | derivation |", "| id | p | v | u | c | d |")
    report, _ = run(repo)
    assert any("closed-parameter header must be" in m for m in failures(report, 6))


def test_unreadable_bom_fails_closed(repo):
    (repo / gate.BOM_JSON).write_text("{", encoding="utf-8")
    report, _ = run(repo)
    assert any("hw/bom/bom.json unusable" in m for m in failures(report, 6))


# --- rule 7: generated table --------------------------------------------------

def test_doc_table_drift_fails_and_write_repairs(repo):
    edit_doc(repo, "| WCCA-006 |", "| WCCA-006 | stale |")
    assert gate.main(["--root", str(repo)]) == 1
    report, _ = run(repo)
    assert any("drifted from hw/wcca/wcca-analysis.csv" in m for m in failures(report, 7))
    assert gate.main(["--root", str(repo), "--write"]) == 0
    assert (repo / gate.WCCA_DOC).read_bytes() == (REPO_ROOT / gate.WCCA_DOC).read_bytes()


def test_write_never_rewrites_a_failing_document(repo):
    edit_csv(repo, "WCCA-004", derating_ratio="0.539000")
    edit_doc(repo, "| WCCA-006 |", "| WCCA-006 | stale |")
    before = (repo / gate.WCCA_DOC).read_bytes()
    assert gate.main(["--root", str(repo), "--write"]) == 1
    assert (repo / gate.WCCA_DOC).read_bytes() == before


def test_missing_document_or_markers_fail(repo):
    edit_doc(repo, gate.ROWS_END, "")
    report, _ = run(repo)
    assert any("derating rows block markers" in m for m in failures(report, 4))
    (repo / gate.WCCA_DOC).unlink()
    report, rows = run(repo)
    assert rows and any("wcca-derating.md is missing" in m for m in failures(report, 4))


# --- rule 1: CSV contract -----------------------------------------------------

def test_missing_empty_or_headerless_csv_fails_closed(repo, capsys):
    path = repo / gate.WCCA_CSV
    path.unlink()
    assert gate.main(["--root", str(repo)]) == 1
    assert "is missing or unreadable" in capsys.readouterr().out
    path.write_text("", encoding="utf-8")
    report, _ = run(repo)
    assert failures(report, 1)[0].endswith("wcca-analysis.csv is empty")
    path.write_text("wcca_id,part_id\n", encoding="utf-8")
    report, _ = run(repo)
    assert "header must be exactly" in failures(report, 1)[0]
    path.write_text(",".join(gate.CSV_HEADER) + "\n", encoding="utf-8")
    report, _ = run(repo)
    assert failures(report, 1)[0].endswith("carries no rows")
    assert gate.main(["--root", str(repo), "--export"]) == 1


def test_row_level_csv_defects_fail(repo):
    table = read_rows(repo)
    table.append([])
    table.append(table[1][:5])
    bad = list(table[1])
    bad[table[0].index("rated_max")] = "four"
    table.append(bad)
    dup = list(table[2])
    table.append(dup)
    invalid = list(table[3])
    invalid[0] = "WCCA-3"
    table.append(invalid)
    write_rows(repo, table)
    report, _ = run(repo)
    messages = failures(report, 1)
    assert any("blank line" in m for m in messages)
    assert any("expected 18 fields, got 5" in m for m in messages)
    assert any("column rated_max is not numeric" in m for m in messages)
    assert any("duplicate wcca_id WCCA-002" in m for m in messages)
    assert any("does not match" in m for m in messages)


def test_broken_row_schema_fails_closed(repo):
    (repo / gate.ROW_SCHEMA).write_text('{"type": "nonsense"}', encoding="utf-8")
    report, rows = run(repo)
    assert rows == [] and "is not a valid schema" in failures(report, 1)[0]


def test_gate_runs_without_jsonschema(repo, monkeypatch):
    monkeypatch.setattr(gate, "jsonschema", None)
    report, rows = run(repo)
    assert report.ok and len(rows) == 6
