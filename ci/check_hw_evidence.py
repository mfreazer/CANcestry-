#!/usr/bin/env python3
"""Fail-closed visual-evidence hash-chain gate (H-03, issue #36).

``HwAGENTS.md`` rule 13: a committed visual is a *view* of a hashed plot-data
file that itself references a hashed evidence artifact. The picture is never
the proof; the hash chain is. This checker enforces the chain end to end:

    hw/tests/evidence/<plot_id>.plot.json      (schema hw-plot-data-0.1.0)
      -> source_evidence_path + source_evidence_hash
      -> hw/tests/evidence/renders/<plot_id>.svg  (provenance comment + footer)
      -> hw/tests/evidence/renders/manifest.json  (plot-data/SVG/renderer hashes)

Fast path (``hw-fast``): verifies the committed bytes against the manifest and
the plot-data contract. It never re-renders - the fast path relies on the
manifest's plot-data/SVG hash coupling. Nightly path
(``--rerender --strict``): re-runs each renderer from the pinned plot-data and
requires byte-identical output, which is the only place renderer drift can be
detected. ``docs/hw/visual-evidence-plan.md`` is the prose contract; its footer
template and the ``FOOTER_LINES`` constant below must agree (the document wins
if they ever diverge).

Usage:
    python3 ci/check_hw_evidence.py [--root .] [--renderers-dir ci/renderers]
                                    [--rerender] [--strict] [--json]

Exit codes:
    0  all checks passed (warnings permitted)
    1  one or more errors
    2  setup error (missing schema, missing manifest, ``jsonschema`` not
       installed, ``--strict --rerender`` with a missing renderer)

Requirements traced: HW-SF-002, HW-FR-004, HW-FR-009 (evidence provenance and
hash-chain integrity); HwAGENTS.md rules 4, 5 and 13.
Test ids: HW-EVIDENCE-GATE-001..028 (tests/unit/tools/test_check_hw_evidence.py).
"""

from __future__ import annotations

import argparse
import hashlib
import html
import json
import os
import re
import subprocess
import sys
from pathlib import Path

try:
    import jsonschema
except ImportError:  # pragma: no cover - the T27 test injects None instead
    jsonschema = None

EVIDENCE_RELATIVE = os.path.join("hw", "tests", "evidence")
RENDERS_RELATIVE = os.path.join("hw", "tests", "evidence", "renders")
MANIFEST_RELATIVE = os.path.join("hw", "tests", "evidence", "renders",
                                 "manifest.json")
PLOT_DATA_SCHEMA_RELATIVE = os.path.join("schemas", "hw",
                                         "hw-plot-data-0.1.0.schema.json")
PLOT_DATA_SUFFIX = ".plot.json"
SIDECAR_SUFFIX = ".svg.provenance.json"
DEFAULT_RENDERERS_DIR = os.path.join("ci", "renderers")
MANIFEST_SCHEMA_VERSION = "0.1.0"

COMMENT_MARKER = "cancestry-provenance"

# The embedded provenance comment contract (HwAGENTS.md rule 13,
# visual-evidence-plan section 4.1). Emitted by renderers, parsed here.
COMMENT_KEYS = (
    "plot_id",
    "plot_version",
    "plot_data_hash",
    "status",
    "method",
    "credibility_level",
    "provisional",
    "source_evidence_path",
    "source_evidence_hash",
    "oracle_id",
    "renderer_tool",
    "renderer_tool_version",
    "renderer_tool_digest",
    "generated_at",
)
COMMENT_OPTIONAL_KEYS = ("generated_at",)

# The visual footer contract (visual-evidence-plan section 4.2). These five
# lines are required, in this order, and must agree with the comment. The
# template is copied byte-for-byte from the plan document.
FOOTER_LINES = (
    "EVIDENCE   {plot_id}@{plot_version}",
    "STATUS     {status}({method},{credibility_level}[,provisional])",
    "PLOT-DATA  {plot_data_hash}",
    "ORACLE     {oracle_id}",
    "RENDERER   {renderer_tool} {renderer_tool_version}",
)
# Optional lines: allowed, and checked for agreement with the comment when
# present, but not required by this revision of the contract.
OPTIONAL_FOOTER_LINES = {
    "SOURCE": ("source_evidence_path", "source_evidence_hash"),
    "GENERATED": ("generated_at",),
}

ENTRY_KEYS = (
    "plot_id",
    "plot_version",
    "plot_data_sha256",
    "expected_renderer_tool",
    "expected_renderer_tool_version",
    "expected_renderer_tool_digest",
    "expected_svg_sha256",
    "last_updated",
)
HASH_PATTERN = re.compile(r"^sha256:[0-9a-f]{64}$")
SEMVER_PATTERN = re.compile(
    r"^(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)"
    r"(?:-[0-9A-Za-z.-]+)?(?:\+[0-9A-Za-z.-]+)?$")


def unescape_xml(text):
    """Decode XML/HTML character references a renderer may have emitted.

    Covers the five predefined XML entities plus numeric character
    references, so a renderer that escapes a footer value (for example
    ``&#95;`` for ``_``) still matches the comment after decoding.
    """
    return html.unescape(text)

SETUP_CODES = ("plot-data-schema-missing", "manifest-missing",
               "jsonschema-missing")


class Report(object):
    """Collect findings with a deterministic order and severity."""

    def __init__(self):
        self.findings = []

    def add(self, severity, code, message, fatal=False):
        self.findings.append({"severity": severity, "code": code,
                              "message": message, "fatal": bool(fatal)})

    def error(self, code, message, fatal=False):
        self.add("error", code, message, fatal=fatal)

    def warning(self, code, message):
        self.add("warning", code, message)

    @property
    def errors(self):
        return [finding for finding in self.findings
                if finding["severity"] == "error"]

    @property
    def warnings(self):
        return [finding for finding in self.findings
                if finding["severity"] == "warning"]

    def exit_code(self):
        if any(finding["fatal"] or finding["code"] in SETUP_CODES
               for finding in self.findings):
            return 2
        return 1 if self.errors else 0


# --------------------------------------------------------------------------
# Small deterministic helpers
# --------------------------------------------------------------------------

def sha256_bytes(data):
    return "sha256:%s" % hashlib.sha256(data).hexdigest()


def sha256_file(path):
    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        for chunk in iter(lambda: handle.read(65536), b""):
            digest.update(chunk)
    return "sha256:%s" % digest.hexdigest()


def read_text(path):
    with open(path, "r", encoding="utf-8", errors="replace") as handle:
        return handle.read()


def strip_tags(text):
    """Reduce an SVG to its text content (tags removed, entities decoded)."""
    return unescape_xml(re.sub(r"<[^>]*>", "", text))


def repo_path(root, relative, report, code, label):
    """Resolve a repository-relative artifact path, fail-closed."""
    if not isinstance(relative, str) or not relative:
        report.error(code, "%s: %r is not a repository-relative path"
                     % (label, relative))
        return None
    candidate = Path(relative)
    if candidate.is_absolute() or ".." in candidate.parts:
        report.error(code, "%s: %s is not a repository-relative path"
                     % (label, relative))
        return None
    resolved = (Path(root) / candidate).resolve()
    if not str(resolved).startswith(str(Path(root).resolve()) + os.sep):
        report.error(code, "%s: %s escapes the repository root"
                     % (label, relative))
        return None
    return resolved


def json_document(path, report, code, label):
    try:
        document = json.loads(read_text(path))
    except ValueError as error:
        report.error(code, "%s: %s is not valid JSON: %s"
                     % (label, path.name, error))
        return None
    if not isinstance(document, dict):
        report.error(code, "%s: %s must be a JSON object"
                     % (label, path.name))
        return None
    return document


# --------------------------------------------------------------------------
# Contract parsing
# --------------------------------------------------------------------------

def parse_provenance(svg_text):
    """Return {key: value} for the embedded cancestry-provenance comment."""
    start = svg_text.find("<!--")
    while start != -1:
        end = svg_text.find("-->", start)
        if end == -1:
            return None
        body = svg_text[start + 4:end].strip()
        lines = [line.strip() for line in body.splitlines() if line.strip()]
        if lines and lines[0] == COMMENT_MARKER:
            fields = {}
            for line in lines[1:]:
                if "=" not in line:
                    return None
                key, value = line.split("=", 1)
                fields[key.strip()] = unescape_xml(value.strip())
            return fields
        start = svg_text.find("<!--", end)
    return None


def footer_lines(svg_text):
    """Visible footer lines of an SVG, in document order."""
    text = strip_tags(svg_text)
    return [line.strip() for line in text.splitlines() if line.strip()]


def expected_footer(comment):
    """Render the required footer lines from the embedded comment.

    The plan document's STATUS template carries the literal marker
    ``[,provisional]``: it expands to ``,provisional`` when the plot-data
    declares provisional=true and to nothing otherwise. Nothing else in the
    template is conditional.
    """
    values = {
        "plot_id": comment.get("plot_id", ""),
        "plot_version": comment.get("plot_version", ""),
        "plot_data_hash": comment.get("plot_data_hash", ""),
        "oracle_id": comment.get("oracle_id", ""),
        "renderer_tool": comment.get("renderer_tool", ""),
        "renderer_tool_version": comment.get("renderer_tool_version", ""),
        "method": comment.get("method", ""),
        "credibility_level": comment.get("credibility_level", ""),
        "status": comment.get("status", ""),
    }
    provisional = comment.get("provisional") == "true"
    lines = []
    for template in FOOTER_LINES:
        line = template.format(**values)
        line = line.replace("[,provisional]",
                            ",provisional" if provisional else "")
        lines.append(line)
    return lines


def check_footer(svg_text, comment, relative, report):
    """Both halves of the provenance contract must agree (rule 13)."""
    lines = footer_lines(svg_text)
    expected = expected_footer(comment)
    cursor = 0
    for index, template in enumerate(FOOTER_LINES):
        label = template.split()[0]
        expected_tokens = expected[index].split()
        found = None
        for position in range(cursor, len(lines)):
            tokens = lines[position].split()
            if tokens and tokens[0] == label:
                found = (position, tokens)
                break
        if found is None:
            report.error("footer", "%s: footer line %s is missing"
                         % (relative, label))
            continue
        position, tokens = found
        cursor = position + 1
        if tokens != expected_tokens:
            report.error(
                "footer", "%s: footer line %s reads %r but the embedded "
                "comment requires %r" % (relative, label,
                                         " ".join(tokens),
                                         " ".join(expected_tokens)))
    for label in sorted(OPTIONAL_FOOTER_LINES):
        keys = OPTIONAL_FOOTER_LINES[label]
        for line in lines:
            tokens = line.split()
            if tokens and tokens[0] == label:
                if tokens[1:] != [comment.get(key, "") for key in keys]:
                    report.error(
                        "footer", "%s: optional footer line %s disagrees with "
                        "the embedded comment" % (relative, label))
                break


def check_comment(comment, expected, relative, report):
    """Comment fields must match the plot-data file, manifest and sidecar."""
    if comment is None:
        report.error("provenance", "%s: no embedded cancestry-provenance "
                     "comment (HwAGENTS.md rule 13: a visual without "
                     "provenance is an assertion)" % relative)
        return
    for key in COMMENT_KEYS:
        if key in COMMENT_OPTIONAL_KEYS:
            continue
        if key not in comment:
            report.error("provenance", "%s: provenance comment omits %s"
                         % (relative, key))
            continue
        if comment[key] != expected[key]:
            report.error("provenance", "%s: provenance comment %s=%r does not "
                         "match %r" % (relative, key, comment[key],
                                       expected[key]))
    for key in COMMENT_OPTIONAL_KEYS:
        if key in comment and comment[key] != expected.get(key):
            report.error("provenance", "%s: provenance comment %s=%r does not "
                         "match %r" % (relative, key, comment[key],
                                       expected.get(key)))


# --------------------------------------------------------------------------
# Plot-data checks
# --------------------------------------------------------------------------

def schema_violations(document, schema, label):
    validator = jsonschema.Draft202012Validator(
        schema, format_checker=jsonschema.FormatChecker())
    return sorted("%s: %s" % (label, error.message)
                  for error in validator.iter_errors(document))


def series_by_name(document, report, relative):
    """Unique series by name; duplicate names fail closed."""
    names = {}
    for series in document.get("series", []):
        name = series.get("name")
        if name in names:
            report.error("plot-data-semantics",
                         "%s: duplicate series name %r" % (relative, name))
            continue
        names[name] = series
    return names


def check_series_semantics(series, relative, report):
    name = series.get("name")
    x, y = series.get("x", []), series.get("y", [])
    if len(x) != len(y):
        report.error("plot-data-semantics",
                     "%s: series %s has %d x samples and %d y samples"
                     % (relative, name, len(x), len(y)))
        return
    if any(b <= a for a, b in zip(x, x[1:])):
        report.warning("plot-data-semantics",
                       "%s: series %s x is not strictly increasing"
                       % (relative, name))


def check_tolerance_band(document, series, relative, report):
    band = document.get("tolerance_band")
    if band is None:
        return
    resolved = {}
    for key, role in (("lower_series", "tolerance_lower"),
                      ("upper_series", "tolerance_upper")):
        name = band.get(key)
        series_entry = series.get(name)
        if series_entry is None:
            report.error("plot-data-semantics",
                         "%s: tolerance_band %s %r does not name a series"
                         % (relative, key, name))
            continue
        if series_entry.get("role") != role:
            report.error("plot-data-semantics",
                         "%s: tolerance_band %s %r has role %r, expected %r"
                         % (relative, key, name, series_entry.get("role"),
                            role))
            continue
        resolved[key] = series_entry
    lower, upper = resolved.get("lower_series"), resolved.get("upper_series")
    if lower is None or upper is None:
        return
    if lower.get("x") != upper.get("x"):
        report.error("plot-data-semantics",
                     "%s: tolerance band series do not share an x axis"
                     % relative)
        return
    for position, (low, high) in enumerate(zip(lower.get("y", []),
                                               upper.get("y", []))):
        if low > high:
            report.error("plot-data-semantics",
                         "%s: tolerance band is inverted at sample %d "
                         "(lower %r > upper %r)" % (relative, position, low,
                                                    high))
            return


def check_plot_data_semantics(document, relative, report):
    series = series_by_name(document, report, relative)
    for name in sorted(series, key=lambda value: str(value)):
        check_series_semantics(series[name], relative, report)
    check_tolerance_band(document, series, relative, report)


def check_source_chain(document, relative, root, report):
    """source_evidence_path exists and its sha256 equals the pinned hash."""
    path = document.get("source_evidence_path")
    digest = document.get("source_evidence_hash")
    resolved = repo_path(root, path, report, "hash-chain", relative)
    if resolved is None:
        return None
    if not resolved.is_file():
        report.error("hash-chain", "%s: source evidence %s does not exist"
                     % (relative, path))
        return None
    actual = sha256_file(resolved)
    if actual != digest:
        report.error("hash-chain", "%s: source evidence %s hashes to %s, the "
                     "plot-data file pins %s (re-issue the plot-data)"
                     % (relative, path, actual, digest))
        return None
    return resolved


def check_status_consistency(document, relative, evidence, report):
    """The view inherits the evidence disposition; it never improves it."""
    status = document.get("status")
    passing = evidence.get("pass") is True
    if passing and status != "passing":
        report.error("status-consistency",
                     "%s: source evidence records pass=true but the plot-data "
                     "status is %r" % (relative, status))
    if not passing and status == "passing":
        report.error("status-consistency",
                     "%s: source evidence does not record pass=true but the "
                     "plot-data status is 'passing'" % relative)
    if bool(document.get("provisional")) != bool(evidence.get("provisional")):
        report.error("status-consistency",
                     "%s: provisional=%r does not match the source evidence "
                     "(%r)" % (relative, document.get("provisional"),
                               evidence.get("provisional")))
    if document.get("credibility_level") != evidence.get("credibility_level"):
        report.error("status-consistency",
                     "%s: credibility_level=%r does not match the source "
                     "evidence (%r)" % (relative, document.get("credibility_level"),
                                        evidence.get("credibility_level")))
    if document.get("oracle_id") != evidence.get("oracle_id"):
        report.error("status-consistency",
                     "%s: oracle_id=%r does not match the source evidence (%r)"
                     % (relative, document.get("oracle_id"),
                        evidence.get("oracle_id")))
    requirement = document.get("source_requirement")
    if requirement is not None and requirement != evidence.get("requirement_id"):
        report.error("status-consistency",
                     "%s: source_requirement=%r does not match the source "
                     "evidence requirement %r"
                     % (relative, requirement, evidence.get("requirement_id")))


# --------------------------------------------------------------------------
# Manifest and artifact checks
# --------------------------------------------------------------------------

def load_manifest(root, report):
    path = Path(root) / MANIFEST_RELATIVE
    if not path.is_file():
        report.error("manifest-missing",
                     "%s is missing; every visual artifact must be registered "
                     "(HwAGENTS.md rule 13)" % MANIFEST_RELATIVE, fatal=True)
        return None
    document = json_document(path, report, "manifest", "manifest")
    if document is None:
        return None
    if document.get("schema_version") != MANIFEST_SCHEMA_VERSION:
        report.error("manifest", "manifest.json: schema_version %r is not %r"
                     % (document.get("schema_version"),
                        MANIFEST_SCHEMA_VERSION))
    entries = document.get("entries")
    if not isinstance(entries, list):
        report.error("manifest", "manifest.json: entries must be an array")
        return None
    seen = set()
    valid = []
    for position, entry in enumerate(entries):
        label = "manifest.json entries[%d]" % position
        if not isinstance(entry, dict):
            report.error("manifest", "%s: entry must be an object" % label)
            continue
        missing = [key for key in ENTRY_KEYS if key not in entry]
        if missing:
            report.error("manifest", "%s: missing %s"
                         % (label, ", ".join(sorted(missing))))
            continue
        for key in ("plot_data_sha256", "expected_renderer_tool_digest",
                    "expected_svg_sha256"):
            if not HASH_PATTERN.match(str(entry[key])):
                report.error("manifest", "%s: %s is not sha256:<64 hex>"
                             % (label, key))
        if not SEMVER_PATTERN.match(str(entry["plot_version"])):
            report.error("manifest", "%s: plot_version %r is not semver"
                         % (label, entry["plot_version"]))
        if entry["plot_id"] in seen:
            report.error("manifest", "%s: duplicate manifest entry for plot "
                         "%s" % (label, entry["plot_id"]))
            continue
        seen.add(entry["plot_id"])
        valid.append(entry)
    return valid


def plot_data_index(root, report):
    """{plot_id: Path} for every committed plot-data file."""
    base = Path(root) / EVIDENCE_RELATIVE
    index = {}
    for path in sorted(base.rglob("*%s" % PLOT_DATA_SUFFIX)):
        document = json_document(path, report, "plot-data-schema", "plot-data")
        if document is None:
            continue
        plot_id = document.get("plot_id")
        if not isinstance(plot_id, str) or not plot_id:
            report.error("plot-data-schema",
                         "%s: plot_id is missing" % path.name)
            continue
        relative = path.relative_to(root).as_posix()
        if plot_id in index:
            report.error("manifest",
                         "%s: more than one plot-data file declares plot_id %r"
                         % (relative, plot_id))
            continue
        index[plot_id] = (path, document)
    return index


def check_plot(root, entry, plot_path, document, schema, report, options):
    plot_id = entry["plot_id"]
    relative = plot_path.relative_to(root).as_posix()
    stem = plot_path.name[:-len(PLOT_DATA_SUFFIX)]
    if stem != plot_id:
        report.error("manifest", "%s: file name does not match plot_id %r "
                     "(expected %s%s)" % (relative, plot_id, plot_id,
                                          PLOT_DATA_SUFFIX))
    violations = schema_violations(document, schema, relative)
    for violation in violations:
        report.error("plot-data-schema", violation)
    if violations:
        return
    check_plot_data_semantics(document, relative, report)
    if document.get("plot_version") != entry["plot_version"]:
        report.error("manifest", "%s: plot_version %r does not match the "
                     "manifest entry %r" % (relative, document.get(
                         "plot_version"), entry["plot_version"]))

    plot_data_hash = sha256_file(plot_path)
    if plot_data_hash != entry["plot_data_sha256"]:
        report.error("manifest-drift",
                     "%s: hashes to %s, the manifest records %s"
                     % (relative, plot_data_hash, entry["plot_data_sha256"]))
    for key, entry_key in (("renderer_tool", "expected_renderer_tool"),
                           ("renderer_tool_version",
                            "expected_renderer_tool_version"),
                           ("renderer_tool_digest",
                            "expected_renderer_tool_digest")):
        if document.get(key) != entry[entry_key]:
            report.error("manifest-drift",
                         "%s: %s=%r does not match the manifest %s=%r"
                         % (relative, key, document.get(key), entry_key,
                            entry[entry_key]))

    evidence_document = None
    evidence_path = check_source_chain(document, relative, root, report)
    if evidence_path is not None:
        evidence_document = json_document(evidence_path, report, "hash-chain",
                                          relative)
        if evidence_document is not None:
            check_status_consistency(document, relative, evidence_document,
                                     report)

    svg_relative = "%s/%s.svg" % (RENDERS_RELATIVE, plot_id)
    svg_path = Path(root) / svg_relative
    if not svg_path.is_file():
        report.error("manifest", "%s: rendered artifact for plot %s is "
                     "missing" % (svg_relative, plot_id))
        return
    svg_text = read_text(svg_path)
    expected = {
        "plot_id": plot_id,
        "plot_version": document.get("plot_version"),
        "plot_data_hash": plot_data_hash,
        "status": document.get("status"),
        "method": document.get("method"),
        "credibility_level": document.get("credibility_level"),
        "provisional": "true" if document.get("provisional") else "false",
        "source_evidence_path": document.get("source_evidence_path"),
        "source_evidence_hash": document.get("source_evidence_hash"),
        "oracle_id": document.get("oracle_id"),
        "renderer_tool": document.get("renderer_tool"),
        "renderer_tool_version": document.get("renderer_tool_version"),
        "renderer_tool_digest": document.get("renderer_tool_digest"),
        "generated_at": document.get("generated_at"),
    }
    comment = parse_provenance(svg_text)
    check_comment(comment, expected, svg_relative, report)
    if comment is not None:
        check_footer(svg_text, comment, svg_relative, report)
    svg_hash = sha256_file(svg_path)
    if svg_hash != entry["expected_svg_sha256"]:
        report.error("manifest-drift",
                     "%s: hashes to %s, the manifest records %s"
                     % (svg_relative, svg_hash, entry["expected_svg_sha256"]))
    check_sidecar(root, plot_id, comment, expected, report)
    check_renderer_availability(root, entry, expected, report, options)
    if options["rerender"]:
        rerender(root, entry, plot_path, svg_relative, svg_hash, report,
                 options)


def check_sidecar(root, plot_id, comment, expected, report):
    """Optional machine-readable mirror of the embedded comment."""
    path = (Path(root) / RENDERS_RELATIVE
            / ("%s%s" % (plot_id, SIDECAR_SUFFIX)))
    if not path.is_file():
        return
    relative = path.relative_to(root).as_posix()
    document = json_document(path, report, "manifest", relative)
    if document is None:
        return
    if document != {key: expected[key] for key in COMMENT_KEYS
                    if expected.get(key) is not None}:
        report.error("manifest", "%s: sidecar does not match the embedded "
                     "provenance comment" % relative)
    if comment is not None and any(document.get(key) != comment.get(key)
                                   for key in COMMENT_KEYS
                                   if expected.get(key) is not None):
        report.error("manifest", "%s: sidecar does not match the embedded "
                     "provenance comment" % relative)


def renderer_path(root, entry, options):
    return (Path(root) / options["renderers_dir"]
            / ("%s.py" % entry["expected_renderer_tool"]))


def check_renderer_availability(root, entry, expected, report, options):
    """Renderer digest drift is checked whenever the renderer is present."""
    path = renderer_path(root, entry, options)
    if not path.is_file():
        if options["rerender"]:
            report.error("rerender", "%s: renderer %s is not available"
                         % (entry["plot_id"],
                            path.relative_to(root).as_posix()),
                         fatal=options["strict"])
        else:
            report.warning("renderer-unavailable",
                           "%s: renderer %s is not present; its digest cannot "
                           "be verified here (the nightly --rerender --strict "
                           "job is the authority)" % (
                               entry["plot_id"],
                               path.relative_to(root).as_posix()))
        return
    digest = sha256_file(path)
    if digest != expected["renderer_tool_digest"]:
        report.error("manifest-drift",
                     "%s: renderer %s hashes to %s, the manifest and plot-data "
                     "pin %s" % (entry["plot_id"],
                                 path.relative_to(root).as_posix(), digest,
                                 expected["renderer_tool_digest"]))


def rerender(root, entry, plot_path, svg_relative, svg_hash, report, options):
    """Re-run the pinned renderer and require byte-identical output."""
    path = renderer_path(root, entry, options)
    if not path.is_file():
        return  # already reported by check_renderer_availability
    command = [sys.executable, str(path),
               "--plot-data", str(plot_path),
               "--version", str(entry["expected_renderer_tool_version"]),
               "--output", "-"]
    result = subprocess.run(command, stdout=subprocess.PIPE,
                            stderr=subprocess.PIPE, cwd=str(root))
    if result.returncode != 0:
        report.error("rerender", "%s: renderer %s failed with exit %d: %s"
                     % (entry["plot_id"],
                        path.relative_to(root).as_posix(), result.returncode,
                        result.stderr.decode("utf-8", "replace").strip()[-400:]))
        return
    produced = sha256_bytes(result.stdout)
    if produced == svg_hash:
        return
    message = ("%s: re-rendering the pinned plot-data produced %s, the "
               "committed artifact is %s (renderer drift is a non-evidence "
               "change: declare it on a line of the PR body that begins "
               "\"renderer-drift:\" followed by the reason, or restore the "
               "committed bytes)" % (svg_relative, produced, svg_hash))
    if os.environ.get("RENDERER_DRIFT_ALLOWED") == "1":
        report.warning("renderer-drift", message)
    else:
        report.error("renderer-drift", message)


def check_manifest_entries(root, entries, schema, report, options):
    index = plot_data_index(root, report)
    registered = set()
    for entry in entries:
        plot_id = entry["plot_id"]
        registered.add(plot_id)
        indexed = index.get(plot_id)
        if indexed is None:
            report.error("manifest", "manifest.json: plot %s has no committed "
                         "plot-data file (%s%s)" % (plot_id, plot_id,
                                                    PLOT_DATA_SUFFIX))
            continue
        plot_path, document = indexed
        check_plot(root, entry, plot_path, document, schema, report, options)
    for plot_id in sorted(index):
        if plot_id not in registered:
            report.error("manifest", "%s%s: plot-data file is not registered "
                         "in manifest.json" % (plot_id, PLOT_DATA_SUFFIX))
    renders = Path(root) / RENDERS_RELATIVE
    for path in sorted(renders.glob("*.svg")):
        if path.stem not in registered:
            report.error("manifest", "%s: rendered artifact has no manifest "
                         "entry (every visual must be registered, "
                         "HwAGENTS.md rule 13)"
                         % path.relative_to(root).as_posix())


# --------------------------------------------------------------------------
# Entry point
# --------------------------------------------------------------------------

def load_schema(root, report):
    path = Path(root) / PLOT_DATA_SCHEMA_RELATIVE
    if jsonschema is None:
        report.warning("jsonschema-missing",
                       "jsonschema is not installed: the plot-data schema "
                       "cannot be validated; refusing to pass the gate")
        return None
    if not path.is_file():
        report.error("plot-data-schema-missing",
                     "%s is missing" % PLOT_DATA_SCHEMA_RELATIVE, fatal=True)
        return None
    return json_document(path, report, "plot-data-schema-missing",
                         "plot-data schema")


def argument_parser():
    parser = argparse.ArgumentParser(
        description="Check the CANcestry hardware visual-evidence hash chain.")
    parser.add_argument("--root", default=".",
                        help="repository root to check (default: .)")
    parser.add_argument("--renderers-dir", default=DEFAULT_RENDERERS_DIR,
                        help="directory holding <renderer_tool>.py "
                             "(default: %s)" % DEFAULT_RENDERERS_DIR)
    parser.add_argument("--rerender", action="store_true",
                        help="re-run each renderer and require byte-identical "
                             "output (nightly path)")
    parser.add_argument("--strict", action="store_true",
                        help="treat a missing renderer under --rerender as a "
                             "setup failure")
    parser.add_argument("--json", action="store_true",
                        help="emit the findings as a JSON array")
    return parser


def main(argv=None):
    if argv is None:
        argv = sys.argv
    args = argument_parser().parse_args(argv[1:])
    root = Path(args.root).resolve()
    if not root.is_dir():
        print("FAIL: %s is not a directory" % args.root)
        return 2
    options = {"renderers_dir": args.renderers_dir, "rerender": args.rerender,
               "strict": args.strict}
    report = Report()

    schema = load_schema(root, report)
    if schema is None:
        return finish(report, args.json, 0)
    entries = load_manifest(root, report)
    if entries is None:
        return finish(report, args.json, 0)
    check_manifest_entries(root, entries, schema, report, options)
    return finish(report, args.json, len(entries))


def finish(report, as_json, plots):
    if as_json:
        print(json.dumps(report.findings, sort_keys=True, indent=2))
    else:
        for finding in report.findings:
            print("%s: %s: %s"
                  % ("FAIL" if finding["severity"] == "error" else "WARN",
                     finding["code"], finding["message"]))
        print("visual evidence: %d plot(s) checked, %d error(s), %d warning(s)"
              % (plots, len(report.errors), len(report.warnings)))
        if report.exit_code() == 0:
            print("PASS: every committed visual is a view of a hashed, "
                  "consistent plot-data file")
    return report.exit_code()


if __name__ == "__main__":  # pragma: no cover
    sys.exit(main(sys.argv))
