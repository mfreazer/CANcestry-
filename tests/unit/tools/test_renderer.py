#!/usr/bin/env python3
"""Tests for ci/renderers/cancestry-render-modelica.py (H-05, issue #44).

The renderer is the tool that turns a hashed plot-data file into the committed
SVG. It must be *deterministic* (byte-identical output for byte-identical
input, which is what the nightly ``--rerender --strict`` gate checks), it must
fail closed on a broken hash chain or an invalid plot-data file, and its output
must satisfy the H-03 provenance contract (comment and footer agree) without
encoding anything by colour alone.

The fixtures are built per test from small, explicit arrays: these tests are
about the renderer, not about any particular evidence artifact, so they never
read a real evidence file.
"""

from __future__ import annotations

import hashlib
import importlib.util
import json
import re
import shutil
import subprocess
import sys
from pathlib import Path

import pytest

from conftest import REPO_ROOT

RENDERER = REPO_ROOT / "ci" / "renderers" / "cancestry-render-modelica.py"
SCHEMA = REPO_ROOT / "schemas" / "hw" / "hw-plot-data-0.1.0.schema.json"
CHECKER = REPO_ROOT / "ci" / "check_hw_evidence.py"

_spec = importlib.util.spec_from_file_location("ci_check_hw_evidence", CHECKER)
check_hw_evidence = importlib.util.module_from_spec(_spec)
sys.modules["ci_check_hw_evidence"] = check_hw_evidence
_spec.loader.exec_module(check_hw_evidence)

PLOT_ID = "renderer_probe_001"
PLOT_REL = "hw/tests/evidence/%s.plot.json" % PLOT_ID
STORED_SVG = "hw/tests/evidence/renders/%s.svg" % PLOT_ID
TIMES = [index * 0.005 for index in range(11)]


def sha256_file(path: Path) -> str:
    return "sha256:" + hashlib.sha256(path.read_bytes()).hexdigest()


def build_repository(tmp_path: Path, **plot_overrides) -> Path:
    """A minimal repository carrying the schema, an evidence file and plot."""
    schema = tmp_path / "schemas" / "hw" / SCHEMA.name
    schema.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy(SCHEMA, schema)
    renderer = tmp_path / "ci" / "renderers" / RENDERER.name
    renderer.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy(RENDERER, renderer)

    evidence = tmp_path / "hw/tests/evidence/probe_001.json"
    evidence.parent.mkdir(parents=True, exist_ok=True)
    evidence.write_text(json.dumps({
        "case_id": "probe_001",
        "credibility_level": "CL2",
        "evidence_of": "Renderer unit-test fixture; not an evidence claim.",
        "oracle_id": "OR-001",
        "pass": True,
        "provisional": True,
        "requirement_id": "HW-SF-002",
    }, indent=2) + "\n", encoding="utf-8")

    def series(name, role, values, line_style, marker):
        return {"name": name, "role": role, "x": TIMES, "y": values,
                "x_units": "s", "y_units": "V",
                "non_color_encoding": {"line_style": line_style,
                                       "marker": marker}}

    oracle = [3.3 - 0.0017 * index * 5 for index in range(len(TIMES))]
    document = {
        "schema_version": "0.1.0",
        "plot_id": PLOT_ID,
        "plot_version": "0.1.0",
        "plot_title": "Renderer unit-test plot",
        "source_evidence_path": "hw/tests/evidence/probe_001.json",
        "source_evidence_hash": sha256_file(evidence),
        "source_requirement": "HW-SF-002",
        "method": "sim",
        "credibility_level": "CL2",
        "status": "passing",
        "provisional": True,
        "oracle_id": "OR-001",
        "generated_at": "2026-09-20T00:00:00Z",
        "renderer_tool": "cancestry-render-modelica",
        "renderer_tool_version": "0.1.0",
        "renderer_tool_digest": sha256_file(renderer),
        "tolerance_band": {"lower_series": "band_lower",
                           "upper_series": "band_upper",
                           "description": "\u00b11 mV per sample, per OR-001",
                           "hatch": "///", "units": "V"},
        "series": [
            series("model", "model", [value + 0.0002 for value in oracle],
                   "solid", "circle"),
            series("oracle", "oracle", oracle, "dashed", "square"),
            series("band_lower", "tolerance_lower",
                   [value - 0.001 for value in oracle], "dotted", "none"),
            series("band_upper", "tolerance_upper",
                   [value + 0.001 for value in oracle], "dotted", "none"),
        ],
    }
    document.update(plot_overrides)
    plot = tmp_path / PLOT_REL
    plot.write_text(json.dumps(document, indent=2) + "\n", encoding="utf-8")
    return tmp_path


def render(root: Path, *args, cwd: Path | None = None):
    """Run the renderer the way the nightly gate does (absolute input paths)."""
    command = [sys.executable, str(root / "ci/renderers" / RENDERER.name),
               "--plot-data", str(root / PLOT_REL), "--version", "0.1.0",
               "--output", "-"] + list(args)
    return subprocess.run(command, stdout=subprocess.PIPE,
                          stderr=subprocess.PIPE, cwd=str(cwd or root))


def write_view(root: Path) -> str:
    completed = render(root)
    assert completed.returncode == 0, completed.stderr.decode()
    path = root / STORED_SVG
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(completed.stdout)
    return completed.stdout.decode()


def test_renderer_is_deterministic(tmp_path):
    """Same plot-data, same bytes: from any cwd, in any order."""
    root = build_repository(tmp_path)
    first = render(root)
    second = render(root, cwd=Path(tmp_path).parent)
    assert first.returncode == second.returncode == 0
    assert first.stdout == second.stdout
    assert first.stdout == render(root).stdout


def test_renderer_produces_the_committed_view_byte_for_byte(tmp_path):
    """The stored artifact is exactly what the gate re-renders (H-03)."""
    root = build_repository(tmp_path)
    svg = write_view(root)
    assert render(root).stdout.decode() == svg


def test_output_matches_the_plot_data_contract(tmp_path):
    """The two halves of the provenance contract agree with the plot-data."""
    root = build_repository(tmp_path)
    svg = write_view(root)
    comment = check_hw_evidence.parse_provenance(svg)
    assert comment is not None, "no embedded cancestry-provenance comment"
    document = json.loads((root / PLOT_REL).read_text(encoding="utf-8"))
    assert comment["plot_data_hash"] == sha256_file(root / PLOT_REL)
    assert comment["plot_id"] == document["plot_id"]
    assert comment["status"] == document["status"]
    assert comment["provisional"] == "true"
    lines = check_hw_evidence.footer_lines(svg)
    cursor = 0
    for line in check_hw_evidence.expected_footer(comment):
        label = line.split()[0]
        for position in range(cursor, len(lines)):
            tokens = lines[position].split()
            if tokens and tokens[0] == label:
                assert tokens == line.split(), line
                cursor = position + 1
                break
        else:
            raise AssertionError("footer line %s is missing" % label)
    assert any(line.startswith("SOURCE") for line in lines)
    assert any(line.startswith("GENERATED") for line in lines)


def test_renderer_passes_the_hardware_evidence_gate(tmp_path):
    """End to end: render, register the manifest, run the H-03 checker."""
    root = build_repository(tmp_path)
    svg_bytes = render(root).stdout
    view = root / STORED_SVG
    view.parent.mkdir(parents=True, exist_ok=True)
    view.write_bytes(svg_bytes)
    document = json.loads((root / PLOT_REL).read_text(encoding="utf-8"))
    (root / "hw/tests/evidence/renders/manifest.json").write_text(
        json.dumps({"schema_version": "0.1.0", "entries": [{
            "plot_id": PLOT_ID,
            "plot_version": document["plot_version"],
            "plot_data_sha256": sha256_file(root / PLOT_REL),
            "expected_renderer_tool": document["renderer_tool"],
            "expected_renderer_tool_version":
                document["renderer_tool_version"],
            "expected_renderer_tool_digest":
                document["renderer_tool_digest"],
            "expected_svg_sha256": sha256_file(view),
            "last_updated": "2026-09-20T00:00:00Z"}]}, indent=2) + "\n",
        encoding="utf-8")
    completed = subprocess.run(
        [sys.executable, str(CHECKER), "--root", str(root), "--rerender",
         "--strict"],
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    assert completed.returncode == 0, completed.stdout


def test_series_are_encoded_without_colour(tmp_path):
    """Line style and marker shape carry the series identity."""
    root = build_repository(tmp_path)
    svg = write_view(root)
    assert 'stroke-dasharray="10 6"' in svg      # oracle: dashed
    assert "<polyline" in svg
    assert "<circle" in svg                      # model: circle markers
    assert "<rect" in svg                        # oracle: square markers
    for name in ("model (solid, circle)", "oracle (dashed, square)"):
        assert name in svg, name
    document = json.loads((root / PLOT_REL).read_text(encoding="utf-8"))
    assert document["tolerance_band"]["description"] in svg


def test_tolerance_band_is_hatched(tmp_path):
    """The admissible band is a hatched polygon, not a colour wash."""
    root = build_repository(tmp_path)
    svg = write_view(root)
    assert "<pattern id=" in svg
    assert 'fill="url(#hatch-' in svg
    assert "<polygon" in svg


def test_renderer_embeds_no_wall_clock_timestamp(tmp_path):
    """Only the plot-data's own generated_at may appear in the output."""
    root = build_repository(tmp_path)
    svg = write_view(root)
    stamps = set(re.findall(r"\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}Z", svg))
    assert stamps == {"2026-09-20T00:00:00Z"}, stamps


def test_source_hash_mismatch_fails_closed(tmp_path):
    """A stale hash chain draws nothing at all."""
    root = build_repository(tmp_path)
    evidence = root / "hw/tests/evidence/probe_001.json"
    evidence.write_text(evidence.read_text(encoding="utf-8") + "\n",
                        encoding="utf-8")
    completed = render(root)
    assert completed.returncode == 2
    assert completed.stdout == b""
    assert b"re-issue the plot-data" in completed.stderr


def test_invalid_plot_data_fails_closed(tmp_path):
    root = build_repository(tmp_path, method="t2")
    completed = render(root)
    assert completed.returncode == 2
    assert b"violates the schema" in completed.stderr


def test_renderer_version_mismatch_fails_closed(tmp_path):
    root = build_repository(tmp_path)
    completed = render(root, "--version", "9.9.9")
    assert completed.returncode == 2
    assert b"does not match the plot-data renderer_tool_version" \
        in completed.stderr


def test_missing_schema_fails_closed(tmp_path):
    root = build_repository(tmp_path)
    (root / "schemas/hw" / SCHEMA.name).unlink()
    completed = render(root)
    assert completed.returncode == 2
    assert b"cannot locate" in completed.stderr


def test_unknown_line_style_and_marker_are_rejected(tmp_path):
    """Defence in depth: the schema enumerates both vocabularies."""
    root = build_repository(tmp_path)
    document = json.loads((root / PLOT_REL).read_text(encoding="utf-8"))
    document["series"][0]["non_color_encoding"]["marker"] = "star"
    (root / PLOT_REL).write_text(json.dumps(document, indent=2) + "\n",
                                 encoding="utf-8")
    completed = render(root)
    assert completed.returncode == 2
    assert b"star" in completed.stderr


def test_output_path_writes_the_same_bytes_as_stdout(tmp_path):
    root = build_repository(tmp_path)
    stdout = render(root).stdout
    target = tmp_path / "from-disk.svg"
    completed = render(root, "--output", str(target))
    assert completed.returncode == 0
    assert target.read_bytes() == stdout


def test_missing_arguments_are_a_usage_error(tmp_path):
    root = build_repository(tmp_path)
    completed = subprocess.run(
        [sys.executable, str(root / "ci/renderers" / RENDERER.name),
         "--version", "0.1.0"],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    assert completed.returncode == 2
