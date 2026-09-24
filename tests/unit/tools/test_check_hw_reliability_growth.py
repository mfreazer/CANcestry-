"""H-09 reliability-growth gate: HW-NF-004 / HW-SF-005 (issue #56, commit 4).

ci/check_hw_reliability_growth.py recomputes the FMEDA MTBF with the
qualified calculator and compares it with hw/fmeda/mtbf-history.csv: a drop
above 10 percent fails with the issue #56 message, a smaller drop warns and
appends, an unchanged FMEDA is a no-op, and every missing / corrupted history
condition fails closed.
"""
import csv
import io
import json
import shutil

import pytest

from conftest import REPO_ROOT
from ci import check_hw_reliability_growth as gate

calc = gate.load_calculator(str(REPO_ROOT))
REGRESSION = "MTBF regression detected: 6578947.4 \u2192 3311258.3. Review component additions."


@pytest.fixture
def repo(tmp_path):
    for directory in ("schemas/hw", "hw/bom", "hw/fmeda"):
        shutil.copytree(REPO_ROOT / directory, tmp_path / directory)
    (tmp_path / "tools").mkdir()
    shutil.copy(REPO_ROOT / gate.CALCULATOR, tmp_path / gate.CALCULATOR)
    return tmp_path


def history(root):
    return (root / gate.HISTORY_CSV).read_text(encoding="utf-8")


def rewrite_fmeda(root, mutate):
    """Apply ``mutate(rows)`` and regenerate the derived columns exactly."""
    path = root / calc.FMEDA_CSV
    with open(path, newline="", encoding="utf-8") as handle:
        table = list(csv.DictReader(handle))
    mutate(table)
    for row in table:
        split = calc.classify_row(row)
        row["classification"] = split["classification"]
        for column in calc.DERIVED_NUMERIC:
            row[column] = calc.quantize(split[column], 4)
    buffer = io.StringIO()
    writer = csv.DictWriter(buffer, fieldnames=list(calc.CSV_HEADER), lineterminator="\n")
    writer.writeheader()
    writer.writerows(table)
    path.write_text(buffer.getvalue(), encoding="utf-8")


def set_fit(root, part_id, fit_id, rate):
    def mutate(rows):
        for row in rows:
            if row["part_id"] == part_id:
                row["fit_total"] = str(rate)
    rewrite_fmeda(root, mutate)
    db_path = root / gate.os.path.join("hw", "bom", "fit-database.json")
    document = json.loads(db_path.read_text(encoding="utf-8"))
    for entry in document["entries"]:
        if entry["fit_id"] == fit_id:
            entry["fit_rate_per_1e9_hours"] = rate
    db_path.write_text(json.dumps(document, indent=2), encoding="utf-8")


def touch_note(root):
    rewrite_fmeda(root, lambda rows: rows[-1].update(note=rows[-1]["note"] + " (revised)"))


# --- repository state -------------------------------------------------------

def test_repository_history_matches_the_committed_fmeda(repo, capsys):
    assert gate.main(["--root", str(repo)]) == 0
    assert "PASS (FMEDA unchanged since history row 1; MTBF 6578947.4 h)" in capsys.readouterr().out
    assert history(repo) == history(REPO_ROOT)
    assert gate.main(["--root", str(repo), "--json"]) == 0
    summary = json.loads(capsys.readouterr().out)
    assert summary["result"] == "unchanged" and summary["change_percent"] == "0.00"
    assert summary["appended_row"] is None and summary["fmeda_csv_sha256"] == history(repo).splitlines()[1].split(",")[1]


def test_changed_fmeda_with_stable_mtbf_appends_a_row(repo, capsys):
    touch_note(repo)
    assert gate.main(["--root", str(repo), "--no-append"]) == 0
    out = capsys.readouterr().out
    assert "--no-append: row not written" in out and "pending row: 2,sha256:" in out
    assert history(repo) == history(REPO_ROOT)
    assert gate.main(["--root", str(repo), "--issue-ref", "#57 note revision"]) == 0
    out = capsys.readouterr().out
    assert "row 2 appended to hw/fmeda/mtbf-history.csv - commit it" in out
    rows = history(repo).splitlines()
    assert len(rows) == 3 and rows[2].startswith("2,sha256:") and rows[2].endswith(",#57 note revision")
    assert rows[2].split(",")[3] == "6578947.4"
    assert gate.main(["--root", str(repo)]) == 0
    assert "unchanged since history row 2" in capsys.readouterr().out


def test_regression_above_ten_percent_fails_with_the_issue_message(repo, capsys):
    set_fit(repo, "PRT-001", "FIT-001", 300)
    before = history(repo)
    assert gate.main(["--root", str(repo)]) == 1
    out = capsys.readouterr().out
    assert "FAIL: " + REGRESSION in out and out.rstrip().endswith("check_hw_reliability_growth: FAIL")
    assert history(repo) == before
    assert gate.main(["--root", str(repo), "--json"]) == 1
    assert json.loads(capsys.readouterr().out) == {"result": "FAIL", "error": REGRESSION}


def test_drop_within_allowance_warns_and_appends(repo, capsys):
    set_fit(repo, "PRT-002", "FIT-002", 12)
    assert gate.main(["--root", str(repo)]) == 0
    out = capsys.readouterr().out
    assert "WARNING: MTBF dropped -6.17 % (within the 10 % allowance): 6578947.4 -> 6172839.5 h" in out
    assert "PASS (MTBF 6578947.4 -> 6172839.5 h, -6.17 %; row 2 appended" in out
    assert history(repo).splitlines()[2].split(",")[2] == "162.0000"


def test_exact_ten_percent_boundary(repo):
    # 152 FIT -> 168.888... FIT would be exactly -10 %; 168 FIT (-9.52 %) passes, 170 FIT (-10.59 %) fails.
    set_fit(repo, "PRT-002", "FIT-002", 18)
    assert gate.main(["--root", str(repo), "--no-append"]) == 0
    set_fit(repo, "PRT-002", "FIT-002", 20)
    assert gate.main(["--root", str(repo), "--no-append"]) == 1


def test_improvement_appends_without_warning(repo, capsys):
    set_fit(repo, "PRT-002", "FIT-002", 1)
    assert gate.main(["--root", str(repo)]) == 0
    out = capsys.readouterr().out
    assert "WARNING" not in out and "6622516.6 h, 0.66 %" in out


def test_history_without_trailing_newline_is_repaired_on_append(repo):
    path = repo / gate.HISTORY_CSV
    path.write_text(history(repo).rstrip("\n"), encoding="utf-8")
    touch_note(repo)
    assert gate.main(["--root", str(repo)]) == 0
    assert len(history(repo).splitlines()) == 3


# --- init -------------------------------------------------------------------

def test_init_creates_the_history_once(repo, capsys):
    (repo / gate.HISTORY_CSV).unlink()
    assert gate.main(["--root", str(repo)]) == 1
    assert "a reliability history is mandatory" in capsys.readouterr().out
    assert gate.main(["--root", str(repo), "--init", "#56 H-09 initial FMEDA baseline (T0, two-part BOM)"]) == 0
    assert "initialised hw/fmeda/mtbf-history.csv" in capsys.readouterr().out
    assert history(repo) == history(REPO_ROOT)
    assert gate.main(["--root", str(repo), "--init", "#56 again"]) == 1
    assert "never overwrites" in capsys.readouterr().out


# --- fail-closed history contract ------------------------------------------------

@pytest.mark.parametrize("contents,fragment", [
    ("", "is empty"),
    ("sequence,mtbf_hours\n1,2\n", "header must be exactly"),
    (",".join(gate.HEADER) + "\n", "carries no rows"),
    (",".join(gate.HEADER) + "\n1,sha256:00\n", "expected 9 fields, got 2"),
    (",".join(gate.HEADER) + "\none,sha256:%s,152,6578947.4,36.5345,27371372.0,76.02,91.99,#56\n" % ("0" * 64), "non-numeric cell"),
    (",".join(gate.HEADER) + "\n1,sha256:%s,152,lots,36.5345,27371372.0,76.02,91.99,#56\n" % ("0" * 64), "non-numeric cell"),
    (",".join(gate.HEADER) + "\n0,sha256:%s,152,6578947.4,36.5345,27371372.0,76.02,91.99,#56\n" % ("0" * 64), "less than the minimum of 1"),
    (",".join(gate.HEADER) + "\n1,sha256:%s,152,6578947.4,36.5345,27371372.0,76.02,91.99,issue 56\n" % ("0" * 64), "does not match"),
    (",".join(gate.HEADER) + "\n2,sha256:%s,152,6578947.4,36.5345,27371372.0,76.02,91.99,#56\n" % ("0" * 64), "sequence 2 out of order (expected 1)"),
])
def test_malformed_history_fails_closed(repo, capsys, contents, fragment):
    (repo / gate.HISTORY_CSV).write_text(contents, encoding="utf-8")
    assert gate.main(["--root", str(repo)]) == 1
    assert fragment in capsys.readouterr().out


def test_hand_edited_metrics_for_the_same_hash_fail(repo, capsys):
    text = history(repo).replace(",76.02,", ",96.02,")
    (repo / gate.HISTORY_CSV).write_text(text, encoding="utf-8")
    assert gate.main(["--root", str(repo)]) == 1
    assert "records spfm_percent that differ from the calculator for the same FMEDA hash" in capsys.readouterr().out


def test_unusable_schema_fails_closed(repo, capsys):
    (repo / gate.HISTORY_SCHEMA).write_text("{", encoding="utf-8")
    assert gate.main(["--root", str(repo)]) == 1
    assert "hw-mtbf-history-0.1.0.schema.json unusable" in capsys.readouterr().out


def test_calculator_failures_fail_closed(repo, capsys):
    (repo / calc.FMEDA_CSV).write_text("broken", encoding="utf-8")
    assert gate.main(["--root", str(repo)]) == 1
    assert "FMEDA calculator failed" in capsys.readouterr().out
    (repo / gate.CALCULATOR).write_text("def (", encoding="utf-8")
    assert gate.main(["--root", str(repo)]) == 1
    assert "cannot be loaded" in capsys.readouterr().out
    (repo / gate.CALCULATOR).unlink()
    assert gate.main(["--root", str(repo)]) == 1
    assert "cannot be loaded" in capsys.readouterr().out or "is missing" in capsys.readouterr().out


def test_degenerate_fmeda_fails_closed(repo, capsys):
    rewrite_fmeda(repo, lambda rows: [row.update(safety_related="N") for row in rows])
    assert gate.main(["--root", str(repo)]) == 1
    assert "no MTBF/SPFM/LFM" in capsys.readouterr().out

    def no_violations(rows):
        for row in rows:
            row.update(safety_related="Y", violates_sg="N", mpf_relevant="Y", sm_spf="-", dc_spf="0",
                       sm_latent="boot check", dc_latent="1")
    rewrite_fmeda(repo, no_violations)
    assert gate.main(["--root", str(repo)]) == 1
    assert "no PMHF" in capsys.readouterr().out


def test_gate_runs_without_jsonschema(repo, monkeypatch):
    monkeypatch.setattr(gate, "jsonschema", None)
    assert gate.main(["--root", str(repo)]) == 0
