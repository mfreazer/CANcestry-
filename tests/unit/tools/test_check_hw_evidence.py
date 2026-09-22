"""Fixture unit tests for ci/check_hw_evidence.py (H-03, issue #36).

The gate enforces ``HwAGENTS.md`` rule 13: every committed visual is a view of
a hashed plot-data file, which itself pins a hashed evidence artifact. These
tests cover the required matrix T1-T28 from issue #36 section 5.1, plus the
fail-closed branches the checker can take on malformed input (every ambiguity
must resolve to an error, never to a silent pass).

Fixture layout: ``tests/unit/tools/fixtures/hw-evidence/clean/`` is the single
committed clean tree (plot-data, source evidence, rendered SVG).
``stub-renderers/`` holds the two stub renderers that honour the documented
CLI contract. Each test materializes the clean tree into a temporary
repository, injects the real plot-data schema and a manifest, and then applies
one named mutation, so a failure names the contract it broke.

Requirements traced: HW-SF-002, HW-FR-004, HW-FR-009; HwAGENTS.md rules 4, 5
and 13. Test ids: HW-EVIDENCE-GATE-001..028, HW-EVIDENCE-GATE-DOC-001,
HW-EVIDENCE-GATE-DECL-001, HW-EVIDENCE-GATE-CLI-001.

The tests are hermetic: ``FixtureRepo.run`` pins ``RENDERER_DRIFT_ALLOWED``
rather than inheriting it, because the workflow exports the drift allowance
when a PR declares one. Inheriting it made the undeclared-drift test
(HW-EVIDENCE-GATE-024) pass in the developer's shell and fail in CI -- the
failure is now impossible in both directions.
"""

from __future__ import annotations

import contextlib
import hashlib
import importlib.util
import io
import json
import os
import re
import shutil
import subprocess
import sys
from collections import namedtuple
from pathlib import Path

import pytest

from conftest import REPO_ROOT

CHECKER = REPO_ROOT / "ci" / "check_hw_evidence.py"
SCHEMA = REPO_ROOT / "schemas" / "hw" / "hw-plot-data-0.1.0.schema.json"
PLAN = REPO_ROOT / "docs" / "hw" / "visual-evidence-plan.md"
FIXTURES = Path(__file__).resolve().parent / "fixtures" / "hw-evidence"
CLEAN = FIXTURES / "clean"
STUBS = FIXTURES / "stub-renderers"

_spec = importlib.util.spec_from_file_location("ci_check_hw_evidence", CHECKER)
check_hw_evidence = importlib.util.module_from_spec(_spec)
sys.modules["ci_check_hw_evidence"] = check_hw_evidence
_spec.loader.exec_module(check_hw_evidence)

PLOT_ID = "holdup_decay_001"
PLOT_REL = "hw/tests/evidence/%s.plot.json" % PLOT_ID
SVG_REL = "hw/tests/evidence/renders/%s.svg" % PLOT_ID
MANIFEST_REL = "hw/tests/evidence/renders/manifest.json"
EVIDENCE_REL = "hw/tests/evidence/holdup_001.json"
SIDECAR_REL = "hw/tests/evidence/renders/%s.svg.provenance.json" % PLOT_ID
RENDERER_NAME = "cancestry-render-fixture"
RENDERER_REL = "ci/renderers/%s.py" % RENDERER_NAME
HW_FAST = REPO_ROOT / ".github" / "workflows" / "hw-fast.yml"
DRIFT_ALLOWED = "RENDERER_DRIFT_ALLOWED"

GateResult = namedtuple("GateResult", "code out")

# ASCII quote characters, used to unwrap the shell's '"'"' idiom.
# (Written with chr() so the fixtures cannot mangle them.)
QUOTE_CHARS = chr(34) + chr(39)
BACKSLASH = chr(92)
BLANK_CLASS = "[ " + BACKSLASH + "t]"
# grep matches one line at a time, so in Python the negated class must not be
# allowed to run past the newline (issue: a bare "renderer-drift:" would match).
NOT_BLANK_CLASS = "[^ " + BACKSLASH + "t" + BACKSLASH + "n]"
SPACE_CLASS = "[[:space:]]"
NOT_SPACE_CLASS = "[^[:space:]]"


def sha256_bytes(data: bytes) -> str:
    return "sha256:" + hashlib.sha256(data).hexdigest()


def sha256_path(path: Path) -> str:
    return sha256_bytes(path.read_bytes())


class FixtureRepo(object):
    """A materialized hardware repository with one clean plot registered."""

    def __init__(self, root: Path):
        self.root = root

    # -- paths and IO ------------------------------------------------------
    def path(self, relative: str) -> Path:
        return self.root / relative

    def read(self, relative: str) -> str:
        return self.path(relative).read_text(encoding="utf-8")

    def read_bytes(self, relative: str) -> bytes:
        return self.path(relative).read_bytes()

    def write(self, relative: str, text: str) -> None:
        target = self.path(relative)
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(text, encoding="utf-8")

    def write_bytes(self, relative: str, data: bytes) -> None:
        target = self.path(relative)
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(data)

    def json(self, relative: str):
        return json.loads(self.read(relative))

    def write_json(self, relative: str, document) -> None:
        self.write(relative, json.dumps(document, indent=2, sort_keys=True)
                   + "\n")

    # -- fixture editing ---------------------------------------------------
    def plot_data(self) -> dict:
        return self.json(PLOT_REL)

    def mutate_plot_data_bytes(self, mutate) -> dict:
        """Rewrite the plot-data without re-rendering (deliberately broken
        states: a plot-data file that no honest renderer could draw)."""
        document = self.plot_data()
        mutate(document)
        self.write_json(PLOT_REL, document)
        self.recompute_manifest()
        return document

    def commit_plot_data_change(self, mutate) -> dict:
        """Apply a *consistent* plot-data edit: rebind the view and manifest.

        The rendered SVG pins the plot-data hash in its comment and footer, so
        any edit that is meant to stay consistent must be re-bound exactly as a
        real render would be. Tests that intend drift edit the files directly.
        """
        document = self.plot_data()
        mutate(document)
        self.write_json(PLOT_REL, document)
        self.render_artifact()
        self.recompute_manifest()
        return document

    def render_artifact(self, stub: str = "stub_pass.py") -> None:
        """Render the committed view with the fixture stub renderer.

        The clean fixture SVG is exactly what ``stub_pass.py`` produces from
        the clean plot-data; materializing a consistent tree means running the
        renderer, never hand-editing the view (HwAGENTS.md rule 13).
        """
        result = subprocess.run(
            [sys.executable, str(STUBS / stub), "--plot-data",
             str(self.path(PLOT_REL)), "--version",
             self.plot_data()["renderer_tool_version"], "--output", "-"],
            stdout=subprocess.PIPE, check=True)
        self.write_bytes(SVG_REL, result.stdout)

    def manifest(self) -> dict:
        return self.json(MANIFEST_REL)

    def rewrite_manifest(self, mutate) -> dict:
        document = self.manifest()
        mutate(document)
        self.write_json(MANIFEST_REL, document)
        return document

    def recompute_manifest(self) -> dict:
        """Rebuild the manifest from the bytes currently on disk."""
        document = self.plot_data()
        entry = {
            "plot_id": PLOT_ID,
            "plot_version": document["plot_version"],
            "plot_data_sha256": sha256_path(self.path(PLOT_REL)),
            "expected_renderer_tool": document["renderer_tool"],
            "expected_renderer_tool_version": document["renderer_tool_version"],
            "expected_renderer_tool_digest": document["renderer_tool_digest"],
            "expected_svg_sha256": sha256_path(self.path(SVG_REL)),
            "last_updated": "2026-09-20T00:00:00Z",
        }
        self.write_json(MANIFEST_REL, {"schema_version": "0.1.0",
                                       "entries": [entry]})
        return entry

    def install_renderer(self, stub: str) -> str:
        """Install a stub renderer and bind its digest into the plot-data."""
        target = self.path(RENDERER_REL)
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy(STUBS / stub, target)
        digest = sha256_path(self.path(RENDERER_REL))
        document = self.plot_data()
        document["renderer_tool_digest"] = digest
        self.write_json(PLOT_REL, document)
        return digest

    # -- running the gate --------------------------------------------------
    def run(self, *args, env=None) -> GateResult:
        """Run the gate in-process and return its exit code and stdout.

        ``env`` is the complete drift environment, never an addition to the
        ambient one: the variable is removed first and restored afterwards, so
        an exported allowance cannot silently satisfy a test that expects the
        undeclared case to be fatal.
        """
        previous = os.environ.pop(DRIFT_ALLOWED, None)
        try:
            for key, value in (env or {}).items():
                os.environ[key] = value
            buffer = io.StringIO()
            with contextlib.redirect_stdout(buffer):
                code = check_hw_evidence.main([str(CHECKER), "--root",
                                               str(self.root)] + list(args))
        finally:
            if previous is None:
                os.environ.pop(DRIFT_ALLOWED, None)
            else:
                os.environ[DRIFT_ALLOWED] = previous
        return GateResult(code, buffer.getvalue())


def build(tmp_path: Path, stub: str | None = "stub_pass.py", *,
          with_schema: bool = True, manifest: bool = True,
          orphan_svg: str | None = None) -> FixtureRepo:
    """Materialize the clean fixture into ``tmp_path`` and register it."""
    shutil.copytree(CLEAN, tmp_path, dirs_exist_ok=True)
    repo = FixtureRepo(tmp_path)
    if with_schema:
        target = repo.path("schemas/hw/hw-plot-data-0.1.0.schema.json")
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy(SCHEMA, target)
    if stub:
        repo.install_renderer(stub)
    repo.render_artifact()
    repo.recompute_manifest()
    if not manifest:
        repo.path(MANIFEST_REL).unlink()
    if orphan_svg:
        repo.write("hw/tests/evidence/renders/%s.svg" % orphan_svg,
                   '<?xml version="1.0"?>\n'
                   '<svg xmlns="http://www.w3.org/2000/svg"/>\n')
    return repo


# --------------------------------------------------------------------------
# T1-T28: the required matrix from issue #36 section 5.1
# --------------------------------------------------------------------------

def test_t01_clean_tree_passes(tmp_path):
    """T1: a consistent tree is green, including the stub renderer digest."""
    result = build(tmp_path).run()
    assert result.code == 0
    assert "PASS" in result.out


def test_t02_missing_schema_is_a_setup_error(tmp_path):
    """T2: no plot-data schema -> exit 2, fail closed."""
    result = build(tmp_path, with_schema=False).run()
    assert result.code == 2
    assert "plot-data-schema-missing" in result.out


def test_t03_plot_data_schema_violation(tmp_path):
    """T3: schema violations are reported as plot-data-schema."""
    repo = build(tmp_path)
    result = repo.run()  # keep the clean result for contrast
    assert result.code == 0
    repo.commit_plot_data_change(lambda document: document.update(method="t2"))
    result = repo.run()
    assert result.code == 1
    assert "plot-data-schema" in result.out


def test_t04_ragged_series_is_an_error(tmp_path):
    """T4: x/y length mismatch is an error, not a warning."""
    repo = build(tmp_path)
    repo.mutate_plot_data_bytes(
        lambda document: document["series"][0]["y"].pop())
    result = repo.run()
    assert result.code == 1
    assert "plot-data-semantics" in result.out
    assert "samples" in result.out


def test_t05_non_monotonic_x_warns_but_passes(tmp_path):
    """T5: a non-increasing x axis is a warning (exit 0 still allowed)."""
    repo = build(tmp_path)
    repo.commit_plot_data_change(
        lambda document: document["series"][0]["x"].__setitem__(3, 0.0))
    result = repo.run()
    assert result.code == 0
    assert "WARN: plot-data-semantics" in result.out


def test_t06_inverted_tolerance_band(tmp_path):
    """T6: lower above upper is an error."""
    def invert(document):
        lower, upper = document["series"][2], document["series"][3]
        lower["y"], upper["y"] = upper["y"], lower["y"]
    repo = build(tmp_path)
    repo.mutate_plot_data_bytes(invert)
    result = repo.run()
    assert result.code == 1
    assert "tolerance band is inverted" in result.out


def test_t07_tolerance_band_without_its_series(tmp_path):
    """T7: a band that names a missing series is an error."""
    repo = build(tmp_path)
    repo.mutate_plot_data_bytes(
        lambda document: document["tolerance_band"].update(upper_series="nope"))
    result = repo.run()
    assert result.code == 1
    assert "does not name a series" in result.out


def test_t08_source_hash_mismatch(tmp_path):
    """T8: a stale source_evidence_hash breaks the chain."""
    repo = build(tmp_path)
    repo.commit_plot_data_change(
        lambda document: document.update(source_evidence_hash="sha256:" + "0" * 64))
    result = repo.run()
    assert result.code == 1
    assert "hash-chain" in result.out


def test_t09_source_path_missing(tmp_path):
    """T9: a missing source artifact breaks the chain."""
    repo = build(tmp_path)
    repo.commit_plot_data_change(
        lambda document: document.update(
            source_evidence_path="hw/tests/evidence/nope.json"))
    result = repo.run()
    assert result.code == 1
    assert "does not exist" in result.out


def test_t10_status_mismatch_with_evidence(tmp_path):
    """T10: the view may not promote the evidence disposition."""
    repo = build(tmp_path)
    evidence = repo.json(EVIDENCE_REL)
    evidence["pass"] = False
    repo.write_json(EVIDENCE_REL, evidence)
    repo.commit_plot_data_change(
        lambda document: document.update(
            source_evidence_hash=sha256_path(repo.path(EVIDENCE_REL))))
    result = repo.run()
    assert result.code == 1
    assert "status-consistency" in result.out
    assert "pass=true" in result.out


def test_t11_provisional_mismatch_with_evidence(tmp_path):
    """T11: provisional must mirror the source evidence."""
    repo = build(tmp_path)
    evidence = repo.json(EVIDENCE_REL)
    evidence["provisional"] = False
    repo.write_json(EVIDENCE_REL, evidence)
    repo.commit_plot_data_change(
        lambda document: document.update(
            source_evidence_hash=sha256_path(repo.path(EVIDENCE_REL))))
    result = repo.run()
    assert result.code == 1
    assert "status-consistency" in result.out
    assert "provisional" in result.out


def test_t12_svg_without_a_manifest_entry(tmp_path):
    """T12: an unregistered visual is a manifest error."""
    repo = build(tmp_path, orphan_svg="orphan_999")
    result = repo.run()
    assert result.code == 1
    assert "rendered artifact has no manifest entry" in result.out


def test_t13_tampered_svg(tmp_path):
    """T13: SVG bytes that disagree with the manifest are drift."""
    repo = build(tmp_path)
    repo.write_bytes(SVG_REL, repo.read_bytes(SVG_REL) + b"<!-- x -->\n")
    result = repo.run()
    assert result.code == 1
    assert "manifest-drift" in result.out


def test_t14_sidecar_out_of_sync(tmp_path):
    """T14: a sidecar that disagrees with the comment is a manifest error."""
    repo = build(tmp_path)
    repo.write_json(SIDECAR_REL, {"plot_id": PLOT_ID, "oracle_id": "OR-999"})
    result = repo.run()
    assert result.code == 1
    assert "FAIL: manifest" in result.out
    assert "sidecar" in result.out


def test_t15_tampered_plot_data(tmp_path):
    """T15: plot-data bytes that disagree with the manifest are drift."""
    repo = build(tmp_path)
    repo.write(PLOT_REL, repo.read(PLOT_REL) + "\n")
    result = repo.run()
    assert result.code == 1
    assert "manifest-drift" in result.out


def test_t16_renderer_digest_bump(tmp_path):
    """T16: a renderer whose digest moved is manifest drift."""
    repo = build(tmp_path)
    stale = "sha256:" + "1" * 64
    repo.commit_plot_data_change(
        lambda document: document.update(renderer_tool_digest=stale))
    repo.rewrite_manifest(
        lambda document: document["entries"][0].update(
            expected_renderer_tool_digest=stale))
    repo.write(RENDERER_REL, repo.read(RENDERER_REL) + "\n# bump\n")
    result = repo.run()
    assert result.code == 1
    assert "manifest-drift" in result.out
    assert "renderer" in result.out


def test_t17_duplicate_manifest_entry(tmp_path):
    """T17: two entries for one plot id are a manifest error."""
    repo = build(tmp_path)
    repo.rewrite_manifest(
        lambda document: document["entries"].append(
            dict(document["entries"][0])))
    result = repo.run()
    assert result.code == 1
    assert "duplicate manifest entry" in result.out


def test_t18_provenance_comment_absent(tmp_path):
    """T18: a visual without provenance is an assertion."""
    repo = build(tmp_path)
    svg = repo.read(SVG_REL)
    start, end = svg.index("<!--"), svg.index("-->") + len("-->")
    repo.write(SVG_REL, svg[:start] + svg[end:])
    repo.recompute_manifest()
    result = repo.run()
    assert result.code == 1
    assert "no embedded cancestry-provenance comment" in result.out


def test_t19_provenance_comment_mismatched(tmp_path):
    """T19: comment fields must match the plot-data file."""
    repo = build(tmp_path)
    repo.write(SVG_REL, repo.read(SVG_REL).replace("oracle_id=OR-001",
                                                   "oracle_id=OR-099"))
    repo.recompute_manifest()
    result = repo.run()
    assert result.code == 1
    assert "provenance comment oracle_id" in result.out


def test_t20_footer_status_line_missing(tmp_path):
    """T20: the required STATUS footer line is mandatory."""
    repo = build(tmp_path)
    kept = [line for line in repo.read(SVG_REL).splitlines()
            if "STATUS     " not in line]
    repo.write(SVG_REL, "\n".join(kept) + "\n")
    repo.recompute_manifest()
    result = repo.run()
    assert result.code == 1
    assert "footer line STATUS is missing" in result.out


def test_t21_footer_and_comment_disagree(tmp_path):
    """T21: the visible footer must agree with the embedded comment."""
    repo = build(tmp_path)
    repo.write(SVG_REL, repo.read(SVG_REL).replace(
        "STATUS     passing(sim,CL2,provisional)",
        "STATUS     pending(sim,CL2,provisional)"))
    repo.recompute_manifest()
    result = repo.run()
    assert result.code == 1
    assert "footer line STATUS reads" in result.out


def test_t22_xml_entities_in_the_footer_pass(tmp_path):
    """T22: XML character references are decoded before comparison."""
    repo = build(tmp_path)
    repo.write(SVG_REL, repo.read(SVG_REL).replace(
        "EVIDENCE   holdup_decay_001@0.1.0",
        "EVIDENCE   holdup&#95;decay&#95;001@0.1.0"))
    repo.recompute_manifest()
    result = repo.run()
    assert result.code == 0, result.out


def test_t23_rerender_hash_match(tmp_path):
    """T23: --rerender reproduces the committed bytes."""
    result = build(tmp_path, stub="stub_pass.py").run("--rerender")
    assert result.code == 0, result.out


def test_t24_rerender_drift_without_declaration(tmp_path):
    """T24: undeclared renderer drift is an error."""
    result = build(tmp_path, stub="stub_drift.py").run("--rerender")
    assert result.code == 1
    assert "renderer-drift" in result.out
    assert "declare it on a line of the PR body" in result.out


def test_t25_rerender_drift_allowed_is_a_warning(tmp_path):
    """T25: a declared renderer drift is reported, not fatal."""
    result = build(tmp_path, stub="stub_drift.py").run(
        "--rerender", env={DRIFT_ALLOWED: "1"})
    assert result.code == 0, result.out
    assert "WARN: renderer-drift" in result.out


@pytest.mark.parametrize("value", ["0", "true", "yes", ""])
def test_t24_only_the_literal_one_allows_drift(tmp_path, value):
    """T24b: any other value of the allowance fails closed."""
    result = build(tmp_path, stub="stub_drift.py").run(
        "--rerender", env={DRIFT_ALLOWED: value})
    assert result.code == 1
    assert "renderer-drift" in result.out


def test_decl001_drift_declaration_is_line_anchored():
    """HW-EVIDENCE-GATE-DECL-001: the workflow accepts a declaration only.

    The flag is derived from the PR body by ``.github/workflows/hw-fast.yml``.
    A whole-body substring match is a fail-open hole: the check's own
    documentation mentions the phrase, so a PR that merely quotes the rule
    would turn a renderer-drift error into a warning. The pattern is extracted
    from the workflow and pinned here against both cases.
    """
    workflow = HW_FAST.read_text(encoding="utf-8")
    assert "contains(github.event.pull_request.body, 'renderer-drift:')" \
        not in workflow, "the drift allowance must not be a substring match"
    match = re.search(r"^\s*DRIFT_DECL=(?P<quoted>.+)$", workflow, re.M)
    assert match, "the workflow must define DRIFT_DECL"
    # The shell wraps the pattern in single quotes via the '"'"' idiom; the
    # pattern itself contains no quotes, so stripping them is exact.
    literal = match.group("quoted").strip()
    for character in QUOTE_CHARS:  # unwrap the shell quoting, not substrings
        literal = literal.replace(character, "")
    assert literal.startswith("^") and "renderer-drift:" in literal, literal
    # POSIX character classes are the shell's; Python spells them out.
    pattern = re.compile(
        literal.replace(NOT_SPACE_CLASS, NOT_BLANK_CLASS)
               .replace(SPACE_CLASS, BLANK_CLASS), re.M)

    declarations = [
        "renderer-drift: matplotlib 3.9 changed the hatch pattern.\n",
        "Summary.\n\nrenderer-drift: font upgrade changed glyph metrics\n",
        "- renderer-drift: libcairo 1.18 rasterises differently\n",
        "1. renderer-drift: renderer pinned to a new patch release\n",
        "**renderer-drift:** the pinned renderer was replaced\n",
        "> renderer-drift: upstream tool release\n",
    ]
    for body in declarations:
        assert pattern.search(body), body

    mentions = [
        "",
        "We did not declare renderer-drift: anywhere in this body.\n",
        "Errors include `renderer-drift:` and `rerender`.\n",
        "See the renderer-drift: policy above.\n",
        "renderer-drift:\n",  # a bare prefix states no reason
    ]
    for body in mentions:
        assert not pattern.search(body), body


def test_t26_strict_rerender_without_a_renderer(tmp_path):
    """T26: --strict --rerender with no renderer is a setup failure."""
    result = build(tmp_path, stub=None).run("--rerender", "--strict")
    assert result.code == 2
    assert "rerender" in result.out


def test_t27_jsonschema_missing_fails_closed(tmp_path, monkeypatch):
    """T27: without jsonschema the gate cannot pass."""
    repo = build(tmp_path)
    monkeypatch.setattr(check_hw_evidence, "jsonschema", None)
    result = repo.run()
    assert result.code == 2
    assert "jsonschema-missing" in result.out


def test_t28_json_output_is_a_parseable_array(tmp_path):
    """T28: --json emits machine-readable findings."""
    repo = build(tmp_path)
    repo.commit_plot_data_change(lambda document: document.update(method="t2"))
    result = repo.run("--json")
    assert result.code == 1
    findings = json.loads(result.out)
    assert isinstance(findings, list) and findings
    assert {"severity", "code", "message", "fatal"} <= set(findings[0])


# --------------------------------------------------------------------------
# Contract and integration guards
# --------------------------------------------------------------------------

def test_doc_footer_template_matches_the_checker():
    """HW-EVIDENCE-GATE-DOC-001: if the two diverge, the document wins."""
    text = PLAN.read_text(encoding="utf-8")
    blocks = text.split("```")
    candidates = [block.strip("\n") for block in blocks
                  if block.lstrip("\n").startswith("EVIDENCE   ")]
    assert len(candidates) == 1, "the plan document footer template moved"
    documented = tuple(line for line in candidates[0].splitlines()
                       if line.strip())
    assert documented == check_hw_evidence.FOOTER_LINES


def test_cli_entry_point_exit_codes(tmp_path):
    """HW-EVIDENCE-GATE-CLI-001: the real CLI returns the documented codes."""
    repo = build(tmp_path)
    result = subprocess.run([sys.executable, str(CHECKER), "--root",
                             str(repo.root)], stdout=subprocess.PIPE,
                            stderr=subprocess.PIPE, universal_newlines=True)
    assert result.returncode == 0, result.stdout
    missing = subprocess.run([sys.executable, str(CHECKER), "--root",
                              str(tmp_path / "nowhere")],
                             stdout=subprocess.PIPE,
                             stderr=subprocess.PIPE, universal_newlines=True)
    assert missing.returncode == 2
    assert "not a directory" in missing.stdout


def test_cli_defaults_to_the_current_directory(tmp_path, monkeypatch, capsys):
    """The documented default root (`.`) and argv default are exercised."""
    repo = build(tmp_path)
    monkeypatch.chdir(repo.root)
    monkeypatch.setattr(sys, "argv", [str(CHECKER)])
    assert check_hw_evidence.main() == 0
    assert "PASS" in capsys.readouterr().out


def test_repository_manifest_registers_only_pending_placeholder_views():
    """H-06 (#49)/H-07 (#53): every committed view is a pending placeholder.

    Supersedes the #36 commit-3 snapshot ("the committed manifest is empty and
    green") and the H-06 single-view snapshot: H-07 registers the pending
    t2_retention_001 placeholder next to the pending pulse_5a_001 one. No
    passing plot may appear, every entry must be structurally complete, and
    the manifest hashes must match the committed bytes.
    """
    manifest = json.loads((REPO_ROOT / MANIFEST_REL).read_text(encoding="utf-8"))
    assert manifest["schema_version"] == "0.1.0"
    assert [entry["plot_id"] for entry in manifest["entries"]] == [
        "pulse_5a_001", "t2_retention_001"]
    for entry in manifest["entries"]:
        assert sorted(entry) == sorted(check_hw_evidence.ENTRY_KEYS)
    pulse_rel = "hw/tests/evidence/pulse_5a_001.plot.json"
    pulse_svg_rel = "hw/tests/evidence/renders/pulse_5a_001.svg"
    t2_rel = "hw/tests/evidence/t2_retention_001.plot.json"
    t2_svg_rel = "hw/tests/evidence/renders/t2_retention_001.svg"
    pulse = json.loads((REPO_ROOT / pulse_rel).read_text(encoding="utf-8"))
    assert pulse["status"] == "pending", "HW-FR-004 is pending; no passing plot"
    assert pulse["provisional"] is True and pulse["credibility_level"] == "CL0"
    t2 = json.loads((REPO_ROOT / t2_rel).read_text(encoding="utf-8"))
    assert t2["status"] == "pending", "HW-SF-002 T2 is pending; no passing plot"
    assert t2["provisional"] is True and t2["credibility_level"] == "CL0"
    by_id = {entry["plot_id"]: entry for entry in manifest["entries"]}
    assert by_id["pulse_5a_001"]["plot_data_sha256"] == sha256_path(REPO_ROOT / pulse_rel)
    assert by_id["pulse_5a_001"]["expected_svg_sha256"] == sha256_path(REPO_ROOT / pulse_svg_rel)
    assert by_id["t2_retention_001"]["plot_data_sha256"] == sha256_path(REPO_ROOT / t2_rel)
    assert by_id["t2_retention_001"]["expected_svg_sha256"] == sha256_path(REPO_ROOT / t2_svg_rel)


def test_committed_clean_fixture_is_self_consistent():
    """The committed fixture's SVG pins the committed plot-data hash."""
    plot_hash = sha256_path(CLEAN / PLOT_REL)
    comment = check_hw_evidence.parse_provenance(
        (CLEAN / SVG_REL).read_text(encoding="utf-8"))
    assert comment["plot_data_hash"] == plot_hash


# --------------------------------------------------------------------------
# Fail-closed branches (beyond the required matrix)
# --------------------------------------------------------------------------

def test_missing_manifest_is_a_setup_error(tmp_path):
    result = build(tmp_path, manifest=False).run()
    assert result.code == 2
    assert "manifest-missing" in result.out


def test_manifest_not_an_object(tmp_path):
    repo = build(tmp_path)
    repo.write(MANIFEST_REL, "[]\n")
    result = repo.run()
    assert result.code == 1
    assert "must be a JSON object" in result.out


def test_manifest_entries_not_an_array(tmp_path):
    repo = build(tmp_path)
    repo.write_json(MANIFEST_REL, {"schema_version": "0.1.0",
                                   "entries": {"plot": PLOT_ID}})
    result = repo.run()
    assert result.code == 1
    assert "entries must be an array" in result.out


def test_manifest_schema_version_and_entry_shape(tmp_path):
    repo = build(tmp_path)
    repo.write_json(MANIFEST_REL, {
        "schema_version": "9.9.9",
        "entries": ["not-an-object",
                    {"plot_id": PLOT_ID},
                    {"plot_id": "x", "plot_version": "v1",
                     "plot_data_sha256": "nope",
                     "expected_renderer_tool": "t",
                     "expected_renderer_tool_version": "0.1.0",
                     "expected_renderer_tool_digest": "nope",
                     "expected_svg_sha256": "nope", "last_updated": "now"}]})
    result = repo.run()
    assert result.code == 1
    for expected in ("schema_version", "entry must be an object",
                     "missing expected_renderer_tool", "is not sha256:<64 hex>",
                     "is not semver"):
        assert expected in result.out, expected


def test_manifest_entry_without_a_plot_data_file(tmp_path):
    repo = build(tmp_path)
    repo.path(PLOT_REL).unlink()
    result = repo.run()
    assert result.code == 1
    assert "has no committed plot-data file" in result.out


def test_plot_data_file_name_must_match_plot_id(tmp_path):
    repo = build(tmp_path)
    repo.write("hw/tests/evidence/renamed.plot.json", repo.read(PLOT_REL))
    repo.path(PLOT_REL).unlink()
    result = repo.run()
    assert result.code == 1
    assert "file name does not match plot_id" in result.out


def test_duplicate_plot_ids_across_files(tmp_path):
    repo = build(tmp_path)
    repo.write("hw/tests/evidence/copy.plot.json", repo.read(PLOT_REL))
    result = repo.run()
    assert result.code == 1
    assert "more than one plot-data file declares plot_id" in result.out


def test_plot_data_payloads_that_are_not_objects(tmp_path):
    repo = build(tmp_path)
    repo.write(PLOT_REL, "[1, 2, 3]\n")
    result = repo.run()
    assert result.code == 1
    assert "must be a JSON object" in result.out
    repo.write(PLOT_REL, "{not json\n")
    result = repo.run()
    assert result.code == 1
    assert "is not valid JSON" in result.out


def test_plot_data_without_plot_id(tmp_path):
    repo = build(tmp_path)
    document = repo.plot_data()
    del document["plot_id"]
    repo.write_json(PLOT_REL, document)
    result = repo.run()
    assert result.code == 1
    assert "plot_id is missing" in result.out


def test_plot_version_must_match_the_manifest(tmp_path):
    repo = build(tmp_path)
    repo.commit_plot_data_change(
        lambda document: document.update(plot_version="0.2.0"))
    repo.rewrite_manifest(
        lambda document: document["entries"][0].update(plot_version="0.1.0"))
    result = repo.run()
    assert result.code == 1
    assert "plot_version" in result.out


def test_renderer_fields_must_match_the_manifest(tmp_path):
    repo = build(tmp_path)
    repo.rewrite_manifest(
        lambda document: document["entries"][0].update(
            expected_renderer_tool_version="9.9.9"))
    result = repo.run()
    assert result.code == 1
    assert "expected_renderer_tool_version" in result.out


def test_source_path_must_stay_inside_the_repository(tmp_path):
    repo = build(tmp_path)
    for bad_path in ("/etc/passwd", "../outside.json"):
        repo.commit_plot_data_change(
            lambda document, value=bad_path: document.update(
                source_evidence_path=value))
        result = repo.run()
        assert result.code == 1
        assert "hash-chain" in result.out


def test_source_evidence_must_be_a_json_object(tmp_path):
    repo = build(tmp_path)
    repo.write(EVIDENCE_REL, "[]\n")
    repo.commit_plot_data_change(
        lambda document: document.update(
            source_evidence_hash=sha256_path(repo.path(EVIDENCE_REL))))
    result = repo.run()
    assert result.code == 1
    assert "must be a JSON object" in result.out


def test_missing_svg_render(tmp_path):
    repo = build(tmp_path)
    repo.path(SVG_REL).unlink()
    result = repo.run()
    assert result.code == 1
    assert "rendered artifact for plot" in result.out


def test_unregistered_plot_data_file(tmp_path):
    repo = build(tmp_path)
    extra = repo.read(PLOT_REL).replace(PLOT_ID, "unregistered_002")
    repo.write("hw/tests/evidence/unregistered_002.plot.json", extra)
    result = repo.run()
    assert result.code == 1
    assert "is not registered in manifest.json" in result.out


def test_duplicate_series_names(tmp_path):
    repo = build(tmp_path)
    repo.mutate_plot_data_bytes(
        lambda document: document["series"][3].update(name="model"))
    result = repo.run()
    assert result.code == 1
    assert "duplicate series name" in result.out


def test_band_series_role_mismatch(tmp_path):
    repo = build(tmp_path)
    repo.mutate_plot_data_bytes(
        lambda document: document["tolerance_band"].update(lower_series="model"))
    result = repo.run()
    assert result.code == 1
    assert "expected 'tolerance_lower'" in result.out


def test_tolerance_band_series_must_share_an_x_axis(tmp_path):
    repo = build(tmp_path)
    repo.mutate_plot_data_bytes(
        lambda document: document["series"][3].update(
            x=[value + 1 for value in document["series"][3]["x"]]))
    result = repo.run()
    assert result.code == 1
    assert "do not share an x axis" in result.out


def test_provenance_comment_contract_edges(tmp_path):
    """Malformed comments fail closed; optional keys are checked when present."""
    repo = build(tmp_path)
    svg = repo.read(SVG_REL)
    repo.write(SVG_REL, svg.replace("-->", "", 1))       # unterminated
    result = repo.run()
    assert result.code == 1
    assert "no embedded cancestry-provenance comment" in result.out
    repo.write(SVG_REL, "<!-- note -->\n" + svg)          # foreign comment first
    repo.recompute_manifest()
    assert repo.run().code == 0
    repo.write(SVG_REL, svg.replace("oracle_id=OR-001",  # '=' missing
                                    "oracle id OR-001"))
    result = repo.run()
    assert result.code == 1
    assert "no embedded cancestry-provenance comment" in result.out
    repo.write(SVG_REL, svg.replace("oracle_id=OR-001\n", ""))
    result = repo.run()
    assert result.code == 1
    assert "provenance comment omits oracle_id" in result.out
    repo.write(SVG_REL, svg.replace("renderer_tool=",
                                    "generated_at=2026-09-20T00:00:00Z\n"
                                    "renderer_tool=", 1))
    result = repo.run()
    assert result.code == 1
    assert "generated_at" in result.out


def test_optional_footer_lines_must_agree_when_present(tmp_path):
    repo = build(tmp_path)
    repo.write(SVG_REL, repo.read(SVG_REL).replace(
        "SOURCE     hw/tests/evidence/holdup_001.json",
        "SOURCE     hw/tests/evidence/other.json"))
    repo.recompute_manifest()
    result = repo.run()
    assert result.code == 1
    assert "optional footer line SOURCE disagrees" in result.out


def test_sidecar_matches_the_comment(tmp_path):
    repo = build(tmp_path)
    comment = check_hw_evidence.parse_provenance(repo.read(SVG_REL))
    repo.write_json(SIDECAR_REL, comment)
    assert repo.run().code == 0


def test_sidecar_not_an_object(tmp_path):
    repo = build(tmp_path)
    repo.write(SIDECAR_REL, "[]\n")
    result = repo.run()
    assert result.code == 1
    assert "must be a JSON object" in result.out


def test_renderer_absent_is_a_warning_on_the_fast_path(tmp_path):
    result = build(tmp_path, stub=None).run()
    assert result.code == 0, result.out
    assert "WARN: renderer-unavailable" in result.out


def test_renderer_absent_without_strict_is_an_error(tmp_path):
    result = build(tmp_path, stub=None).run("--rerender")
    assert result.code == 1
    assert "is not available" in result.out


def test_renderer_failure_is_reported(tmp_path):
    repo = build(tmp_path)
    repo.write(RENDERER_REL, "import sys\nsys.exit('boom')\n")
    repo.commit_plot_data_change(
        lambda document: document.update(
            renderer_tool_digest=sha256_path(repo.path(RENDERER_REL))))
    result = repo.run("--rerender")
    assert result.code == 1
    assert "failed with exit" in result.out
    assert "boom" in result.out


def test_checker_helper_edges():
    """Unit-level guards for the checker's small helpers."""
    assert check_hw_evidence.unescape_xml("a&amp;b&#95;c") == "a&b_c"
    assert check_hw_evidence.strip_tags("<text>ok</text>") == "ok"
    assert check_hw_evidence.parse_provenance("<svg/>") is None
    assert check_hw_evidence.sha256_bytes(b"") == (
        "sha256:e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855")
    report = check_hw_evidence.Report()
    report.warning("y", "hmm")
    assert report.exit_code() == 0
    assert len(report.warnings) == 1 and not report.errors
    report.error("x", "boom")
    assert report.exit_code() == 1
    report.add("error", "z", "setup", fatal=True)
    assert report.exit_code() == 2

def test_plot_data_without_a_tolerance_band_is_allowed(tmp_path):
    """A plain overlay without a band is valid: the band is conditional."""
    repo = build(tmp_path)
    repo.commit_plot_data_change(lambda document: (
        document.pop("tolerance_band"),
        document.update(series=[entry for entry in document["series"]
                                if entry["role"] not in ("tolerance_lower",
                                                         "tolerance_upper")])))
    result = repo.run()
    assert result.code == 0, result.out


def test_status_consistency_checks_credibility_oracle_and_requirement(tmp_path):
    """Every inherited disposition field is cross-checked against the source."""
    for field, value, expected in (("credibility_level", "CL1",
                                    "credibility_level"),
                                   ("oracle_id", "OR-777", "oracle_id"),
                                   ("requirement_id", "HW-FR-009",
                                    "source_requirement")):
        repo = build(tmp_path / field)
        evidence = repo.json(EVIDENCE_REL)
        evidence[field] = value
        repo.write_json(EVIDENCE_REL, evidence)
        repo.commit_plot_data_change(
            lambda document: document.update(
                source_evidence_hash=sha256_path(repo.path(EVIDENCE_REL))))
        result = repo.run()
        assert result.code == 1
        assert "status-consistency" in result.out
        assert expected in result.out


def test_source_path_escaping_the_repository_via_a_link(tmp_path):
    """A repository-relative path that resolves outside the root fails closed."""
    repo = build(tmp_path)
    link = repo.path("hw/tests/evidence/outside.json")
    link.symlink_to("/etc/hostname")
    repo.mutate_plot_data_bytes(
        lambda document: document.update(
            source_evidence_path="hw/tests/evidence/outside.json"))
    result = repo.run()
    assert result.code == 1
    assert "escapes the repository root" in result.out


def test_evidence_directory_without_plot_data(tmp_path):
    """No plot-data file anywhere: the index is empty and the entry fails."""
    repo = build(tmp_path)
    manifest = repo.read(MANIFEST_REL)
    shutil.rmtree(repo.path("hw/tests/evidence"))
    repo.write(MANIFEST_REL, manifest)
    result = repo.run()
    assert result.code == 1
    assert "has no committed plot-data file" in result.out


def test_renders_directory_without_svgs(tmp_path):
    """A renders directory with no SVG has nothing to orphan-check."""
    repo = build(tmp_path)
    repo.path(SVG_REL).unlink()
    result = repo.run()
    assert result.code == 1
    assert "rendered artifact for plot" in result.out


def test_repo_path_guard_rejects_empty_values(tmp_path):
    """Direct unit guard for the repository-relative path contract."""
    report = check_hw_evidence.Report()
    assert check_hw_evidence.repo_path(tmp_path, "", report, "hash-chain",
                                       "fixture") is None
    assert check_hw_evidence.repo_path(tmp_path, None, report, "hash-chain",
                                       "fixture") is None
    assert check_hw_evidence.repo_path(tmp_path, "ok.json", report,
                                       "hash-chain", "fixture") is not None
    assert report.exit_code() == 1


def test_root_that_is_not_a_directory(tmp_path):
    """The CLI reports a bad root as a setup failure."""
    code = check_hw_evidence.main([str(CHECKER), "--root",
                                   str(tmp_path / "nowhere")])
    assert code == 2

def test_pending_view_of_passing_evidence_is_an_error(tmp_path):
    """A view may lag the evidence, but not silently drop a pass."""
    repo = build(tmp_path)
    repo.commit_plot_data_change(lambda document: document.update(
        status="pending"))
    result = repo.run()
    assert result.code == 1
    assert "source evidence records pass=true" in result.out


def test_t29_tolerance_roles_without_tolerance_band(tmp_path):
    """Rule 14: series with tolerance roles require an explicit tolerance_band."""
    repo = build(tmp_path)
    def remove_band(document):
        document.pop("tolerance_band", None)
    repo.mutate_plot_data_bytes(remove_band)
    result = repo.run()
    assert result.code == 1
    assert "no tolerance_band is defined" in result.out or "tolerance_band" in result.out


def test_t30_svg_hatch_without_tolerance_band(tmp_path):
    """Rule 14: SVG must not contain a hatched band when tolerance_band is not declared."""
    repo = build(tmp_path)
    def remove_band(document):
        document.pop("tolerance_band", None)
        document["series"] = [s for s in document["series"]
                              if s["role"] not in ("tolerance_lower", "tolerance_upper")]
    repo.mutate_plot_data_bytes(remove_band)
    result = repo.run()
    assert result.code == 1
    assert "SVG contains a hatched tolerance band" in result.out


def test_t31_svg_missing_hatch_with_tolerance_band(tmp_path):
    """Rule 14: SVG must contain hatched band polygon when tolerance_band is declared."""
    repo = build(tmp_path)
    svg_text = repo.read(SVG_REL).replace('fill="url(#hatch)"', 'fill="none"')
    repo.write(SVG_REL, svg_text)
    repo.recompute_manifest()
    result = repo.run()
    assert result.code == 1
    assert "plot-data declares tolerance_band but SVG has no matching" in result.out

