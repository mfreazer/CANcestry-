"""Tests for the Phase 8 upgrade of ci/check_schemas_valid.py (issue #22).

Acceptance criterion: the checker must reject a schema with a logical
Draft-2020-12 violation (e.g. an invalid regex pattern) while keeping the
existing structural checks green for every committed schema.

Requirements traced: SW-FR-TOOL-008. Test ids: SCHEMA-LOGIC-001..004.
"""

from __future__ import annotations

import importlib.util
import json
import subprocess
import sys

import pytest

from conftest import CHECK_SCHEMAS, SCHEMA_DIR

# The checker lives outside a package; load it under a fixed module name so
# the tests can call its helpers directly.
_spec = importlib.util.spec_from_file_location("ci_check_schemas",
                                               CHECK_SCHEMAS)
ci_check_schemas = importlib.util.module_from_spec(_spec)
sys.modules["ci_check_schemas"] = ci_check_schemas
_spec.loader.exec_module(ci_check_schemas)


def run_checker(directory):
    return subprocess.run(
        [sys.executable, str(CHECK_SCHEMAS), str(directory)],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        universal_newlines=True)


def test_all_committed_schemas_pass():
    """SCHEMA-LOGIC-001: the repository schemas are logically valid."""
    pytest.importorskip("jsonschema")
    result = run_checker(SCHEMA_DIR)
    assert result.returncode == 0, result.stdout
    assert "Draft 2020-12 metaschema" in result.stdout
    assert "all schemas are valid" in result.stdout


def test_rejects_invalid_regex_pattern(tmp_path):
    """SCHEMA-LOGIC-002: the issue #22 acceptance case, an invalid regex."""
    pytest.importorskip("jsonschema")
    bad = {
        "$schema": "https://json-schema.org/draft/2020-12/schema",
        "$id": "https://cancestry.dev/schemas/bad-0.1.0.schema.json",
        "type": "object",
        "properties": {"x": {"type": "string", "pattern": "[unclosed"}},
    }
    path = tmp_path / "bad-0.1.0.schema.json"
    path.write_text(json.dumps(bad), encoding="utf-8")
    result = run_checker(tmp_path)
    assert result.returncode == 1
    assert "is not a valid Draft 2020-12 schema" in result.stdout
    assert "regex" in result.stdout


def test_rejects_required_not_an_array(tmp_path):
    """SCHEMA-LOGIC-003: a metaschema type violation."""
    pytest.importorskip("jsonschema")
    bad = {
        "$schema": "https://json-schema.org/draft/2020-12/schema",
        "$id": "https://cancestry.dev/schemas/bad-0.1.0.schema.json",
        "required": "name",
    }
    path = tmp_path / "bad-0.1.0.schema.json"
    path.write_text(json.dumps(bad), encoding="utf-8")
    result = run_checker(tmp_path)
    assert result.returncode == 1
    assert "not of type 'array'" in result.stdout


def test_structural_checks_still_apply(tmp_path):
    """The dialect and $id-version checks survive the upgrade."""
    bad = {"$schema": "http://json-schema.org/draft-04/schema#"}
    path = tmp_path / "old-0.1.0.schema.json"
    path.write_text(json.dumps(bad), encoding="utf-8")
    result = run_checker(tmp_path)
    assert result.returncode == 1
    assert "2020-12 dialect" in result.stdout

    mismatch = {
        "$schema": "https://json-schema.org/draft/2020-12/schema",
        "$id": "https://cancestry.dev/schemas/other-9.9.9.schema.json",
    }
    path = tmp_path / "fsm-0.3.0.schema.json"
    path.write_text(json.dumps(mismatch), encoding="utf-8")
    result = run_checker(tmp_path)
    assert result.returncode == 1
    assert "$id version" in result.stdout


def test_unit_level_helpers(tmp_path):
    pytest.importorskip("jsonschema")
    bad = {"type": "object", "properties": {"x": {"pattern": "([a)+"}}}
    problems = ci_check_schemas.check_schema_logically(bad)
    assert problems and "regex" in problems[0]
    good = json.loads((SCHEMA_DIR / "fsm-0.3.0.schema.json")
                      .read_text(encoding="utf-8"))
    assert ci_check_schemas.check_schema_logically(good) == []
    assert ci_check_schemas.jsonschema_available() is True
    # A missing file yields a problem list, not an exception.
    assert ci_check_schemas.check_file(tmp_path / "missing.json") != []


def test_fallback_when_jsonschema_missing(tmp_path, monkeypatch):
    """SCHEMA-LOGIC-004: without jsonschema the gate degrades to SKIP.

    The repository schemas still pass; the SKIP notice names the gap. The
    subprocess runs with a shadowing ``jsonschema.py`` that raises on
    import, which reproduces the minimal-environment situation.
    """
    monkeypatch.setitem(sys.modules, "jsonschema", None)
    assert ci_check_schemas.jsonschema_available() is False
    # The deep check silently reports no problems when it cannot run; the
    # caller prints the SKIP notice instead.
    assert ci_check_schemas.check_schema_logically({"type": "string"}) == []

    shadow = tmp_path / "shadow"
    shadow.mkdir()
    (shadow / "jsonschema.py").write_text(
        'raise ImportError("simulated missing jsonschema")\n',
        encoding="utf-8")
    env = {"PYTHONPATH": str(shadow)}
    env.update({k: v for k, v in __import__("os").environ.items()
                if k not in ("PYTHONPATH",)})
    result = subprocess.run(
        [sys.executable, str(CHECK_SCHEMAS), str(SCHEMA_DIR)],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        universal_newlines=True, env=env)
    assert result.returncode == 0
    assert "SKIP" in result.stdout


def test_main_usage_and_directory_checks(capsys):
    assert ci_check_schemas.main(["check_schemas_valid.py"]) == 1
    assert "usage:" in capsys.readouterr().out
    assert ci_check_schemas.main(["x", "/nowhere/at/all"]) == 1
    assert "is not a directory" in capsys.readouterr().out


def test_main_passes_on_the_repository_schemas(capsys):
    assert ci_check_schemas.main(["check_schemas_valid.py",
                                  str(SCHEMA_DIR)]) == 0
    out = capsys.readouterr().out
    assert out.count("PASS:") == 8 and "all schemas are valid" in out


def test_main_reports_every_problem(tmp_path, capsys):
    (tmp_path / "broken-0.1.0.schema.json").write_text(
        json.dumps({"$schema": "https://json-schema.org/draft/2020-12/schema",
                    "$id": "https://cancestry.dev/schemas/other-9.9.9.schema.json"}),
        encoding="utf-8")
    (tmp_path / "broken2-0.1.0.schema.json").write_text(
        "{not json", encoding="utf-8")
    (tmp_path / "broken3-0.1.0.schema.json").write_text(
        "[1, 2]", encoding="utf-8")
    (tmp_path / "not-a-schema.txt").write_text("ignored", encoding="utf-8")
    assert ci_check_schemas.main(["check_schemas_valid.py",
                                  str(tmp_path)]) == 1
    out = capsys.readouterr().out
    assert out.count("FAIL:") == 3
    assert "does not parse as JSON" in out
    assert "not an object" in out
    assert "$id version" in out
    assert "not-a-schema.txt" not in out


def test_check_file_missing_and_idless(tmp_path):
    problems = ci_check_schemas.check_file(tmp_path / "ghost.schema.json")
    assert problems and "cannot be read" in problems[0]
    (tmp_path / "x-0.1.0.schema.json").write_text(
        json.dumps({"$schema": "https://json-schema.org/draft/2020-12/schema"}),
        encoding="utf-8")
    problems = ci_check_schemas.check_file(tmp_path / "x-0.1.0.schema.json")
    assert any("missing $id" in problem for problem in problems)


def test_main_skip_notice_without_jsonschema(tmp_path, capsys, monkeypatch):
    monkeypatch.setattr(ci_check_schemas, "jsonschema_available",
                        lambda: False)
    assert ci_check_schemas.main(["check_schemas_valid.py",
                                  str(SCHEMA_DIR)]) == 0
    out = capsys.readouterr().out
    assert "SKIP: jsonschema not installed" in out
    assert "(parsed, structural)" in out
