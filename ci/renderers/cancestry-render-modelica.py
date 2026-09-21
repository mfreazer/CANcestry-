#!/usr/bin/env python3
"""Draw a hardware oracle-overlay SVG from a hashed plot-data file.

This is the reference renderer for the hardware visual-evidence pipeline
(H-03 contract, H-05 first plots):

    plot-data (hw-plot-data-0.1.0)  ->  this renderer  ->  SVG on stdout

It is deliberately dependency-light and *pure*: the only third-party import is
``jsonschema`` (already pinned in ``ci/docker/Dockerfile``), and nothing else is
read besides the plot-data file, the schema and the evidence artifact the
plot-data pins by hash. No fonts, no images, no wall clock, no randomness: the
same plot-data always produces the same bytes, on any machine and any Python
>= 3.10, which is what makes the nightly ``--rerender --strict`` check
meaningful. ``docs/hw/visual-evidence-plan.md`` is the prose contract.

Drawing is greyscale-safe by construction. Series are separated by line style
and marker *shape* (drawn as paths, not font glyphs), the tolerance band is
filled with a hatch pattern, and every series is named in the legend, so no
claim is encoded by colour alone.

Usage:
    cancestry-render-modelica.py --plot-data <path> --version <semver>
                                 [--root <repo>] [--output -|<path>]

Exit codes:
    0  SVG produced (stdout or --output path)
    2  usage or contract error: missing plot-data/schema, schema violation,
       source-evidence hash mismatch, renderer version mismatch, unknown
       marker or hatch value. Nothing is written on failure.

Requirements traced: HW-SF-002, HW-FR-004; HwAGENTS.md rules 4, 5 and 13.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
import sys
from pathlib import Path

try:
    import jsonschema
except ImportError:  # pragma: no cover - the toolchain image pins jsonschema
    jsonschema = None

SCHEMA_RELATIVE = os.path.join("schemas", "hw",
                               "hw-plot-data-0.1.0.schema.json")
PLOT_DATA_SUFFIX = ".plot.json"
RENDERS_NAME = "renders"

# Canvas geometry (all fixed; the drawing scales with the viewBox, not with
# the data, so the byte stream never depends on content length).
WIDTH, HEIGHT = 960, 640
MARGIN_LEFT, MARGIN_RIGHT = 96, 32
MARGIN_TOP, MARGIN_BOTTOM = 72, 248
PLOT_WIDTH = WIDTH - MARGIN_LEFT - MARGIN_RIGHT
PLOT_HEIGHT = HEIGHT - MARGIN_TOP - MARGIN_BOTTOM

DASH_PATTERNS = {
    "solid": "",
    "dashed": "10 6",
    "dotted": "2 5",
    "dashdot": "12 5 2 5",
}

# Greyscale-safe hatches: each entry is a list of line segments inside a 8x8
# tile, in (x1, y1, x2, y2) tile coordinates.
HATCH_PATTERNS = {
    "///": ((0, 8, 8, 0), (8, 0, 0, 8), (-1, 1, 1, -1), (7, 9, 9, 7)),
    "---": ((0, 4, 8, 4),),
    "|||": ((4, 0, 4, 8),),
    "+++": ((0, 4, 8, 4), (4, 0, 4, 8)),
    "xxx": ((0, 0, 8, 8), (8, 0, 0, 8)),
    "...": (),
}
HATCH_DOTS = ("...",)

STROKE = "#101010"
SOFT = "#606060"


class ContractError(Exception):
    """A fail-closed contract violation: nothing is drawn."""


# --------------------------------------------------------------------------
# Input handling
# --------------------------------------------------------------------------

def repository_root(plot_path):
    """Find the repository root above a plot-data file.

    The plot-data layout is fixed by the H-03 contract
    (``hw/tests/evidence/<plot_id>.plot.json``), so the evidence artifact and
    the schema are addressed relative to the repository, never to the cwd.
    """
    path = Path(plot_path).resolve()
    for candidate in [path.parent] + list(path.parents):
        if (candidate / SCHEMA_RELATIVE).is_file():
            return candidate
    raise ContractError(
        "cannot locate %s above %s: the plot-data file must live inside the "
        "repository that carries the hardware schemas" % (SCHEMA_RELATIVE,
                                                          plot_path))


def load_json(path, what):
    try:
        with open(path, "r", encoding="utf-8") as handle:
            return json.load(handle)
    except OSError as error:
        raise ContractError("%s is unreadable: %s" % (what, error))
    except ValueError as error:
        raise ContractError("%s is not valid JSON: %s" % (what, error))


def sha256_file(path):
    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        for chunk in iter(lambda: handle.read(65536), b""):
            digest.update(chunk)
    return "sha256:%s" % digest.hexdigest()


def validate(root, document, plot_path):
    schema_path = root / SCHEMA_RELATIVE
    if jsonschema is None:
        raise ContractError("jsonschema is not installed; refusing to render "
                            "unvalidated data (HwAGENTS.md rule 13)")
    schema = load_json(schema_path, "plot-data schema")
    validator = jsonschema.Draft202012Validator(
        schema, format_checker=jsonschema.FormatChecker())
    violations = sorted(error.message for error in
                        validator.iter_errors(document))
    if violations:
        raise ContractError("plot-data %s violates the schema: %s"
                            % (plot_path, "; ".join(violations[:6])))


def check_source_evidence(root, document):
    """The picture is a view of a hashed artifact: verify the artifact."""
    relative = document["source_evidence_path"]
    path = (root / relative).resolve()
    if not str(path).startswith(str(root) + os.sep):
        raise ContractError("source evidence %s escapes the repository root"
                            % relative)
    if not path.is_file():
        raise ContractError("source evidence %s does not exist" % relative)
    actual = sha256_file(path)
    if actual != document["source_evidence_hash"]:
        raise ContractError(
            "source evidence %s hashes to %s, the plot-data pins %s "
            "(re-issue the plot-data)" % (relative, actual,
                                          document["source_evidence_hash"]))
    return relative


# --------------------------------------------------------------------------
# Deterministic geometry helpers
# --------------------------------------------------------------------------

def nice_ticks(low, high, target=6):
    """Human-readable tick values with a deterministic step."""
    if not math.isfinite(low) or not math.isfinite(high) or high <= low:
        raise ContractError("cannot lay out an axis over [%r, %r]"
                            % (low, high))
    raw = (high - low) / float(target)
    magnitude = 10 ** math.floor(math.log10(raw))
    for factor in (1.0, 2.0, 2.5, 5.0, 10.0):
        step = factor * magnitude
        if step >= raw:
            break
    start = math.ceil(low / step) * step
    values = []
    value = start
    while value <= high + step * 1e-9:
        values.append(round(value, 12))
        value += step
    decimals = max(0, min(6, -int(math.floor(math.log10(step)))))
    return values, decimals


def escape(text):
    return (str(text).replace("&", "&amp;").replace("<", "&lt;")
            .replace(">", "&gt;").replace('"', "&quot;"))


def fmt(value, decimals):
    return ("%%.%df" % decimals) % value


def data_extent(document, key):
    low = min(min(series[key]) for series in document["series"])
    high = max(max(series[key]) for series in document["series"])
    if high == low:
        high = low + 1.0
    return low, high


class Canvas(object):
    """Maps data coordinates to canvas coordinates, once, deterministically."""

    def __init__(self, document):
        self.x_ticks, self.x_decimals = nice_ticks(*data_extent(document, "x"))
        y_low, y_high = data_extent(document, "y")
        pad = (y_high - y_low) * 0.08
        self.y_ticks, self.y_decimals = nice_ticks(y_low - pad, y_high + pad)
        self.x_min, self.x_max = self.x_ticks[0], self.x_ticks[-1]
        self.y_min, self.y_max = self.y_ticks[0], self.y_ticks[-1]

    def x(self, value):
        span = self.x_max - self.x_min or 1.0
        return MARGIN_LEFT + (value - self.x_min) / span * PLOT_WIDTH

    def y(self, value):
        span = self.y_max - self.y_min or 1.0
        return MARGIN_TOP + PLOT_HEIGHT - (value - self.y_min) / span \
            * PLOT_HEIGHT

    def points(self, series):
        return " ".join("%.3f,%.3f" % (self.x(x), self.y(y))
                        for x, y in zip(series["x"], series["y"]))


# --------------------------------------------------------------------------
# Drawing
# --------------------------------------------------------------------------

def hatch_definitions(hatches):
    lines = []
    for index, hatch in enumerate(sorted(hatches)):
        if hatch not in HATCH_PATTERNS:
            raise ContractError("unknown hatch %r (plan section 7 enumerates "
                                "the greyscale-safe hatches)" % hatch)
        identifier = "hatch-%d" % index
        body = []
        if hatch in HATCH_DOTS:
            for row in range(2):
                for column in range(2):
                    body.append('<circle cx="%s" cy="%s" r="1.1" '
                                'fill="%s"/>'
                                % (2 + 4 * column, 2 + 4 * row, SOFT))
        for x1, y1, x2, y2 in HATCH_PATTERNS[hatch]:
            body.append('<line x1="%s" y1="%s" x2="%s" y2="%s" '
                        'stroke="%s" stroke-width="1"/>'
                        % (x1, y1, x2, y2, SOFT))
        lines.append('<pattern id="%s" width="8" height="8" '
                     'patternUnits="userSpaceOnUse">%s</pattern>'
                     % (identifier, "".join(body)))
    return lines, {hatch: "hatch-%d" % index
                   for index, hatch in enumerate(sorted(hatches))}


def marker_path(shape, cx, cy):
    """Hand-drawn marker shapes: no fonts, no colour, always the same id."""
    size = 4.0
    if shape == "none":
        return None
    if shape == "circle":
        return '<circle cx="%.3f" cy="%.3f" r="3.2" fill="#ffffff" ' \
               'stroke="%s" stroke-width="1.4"/>' % (cx, cy, STROKE)
    if shape == "square":
        return '<rect x="%.3f" y="%.3f" width="6.4" height="6.4" ' \
               'fill="#ffffff" stroke="%s" stroke-width="1.4"/>' \
               % (cx - size * 0.8, cy - size * 0.8, STROKE)
    if shape == "diamond":
        return '<polygon points="%.3f,%.3f %.3f,%.3f %.3f,%.3f %.3f,%.3f" ' \
               'fill="#ffffff" stroke="%s" stroke-width="1.4"/>' % (
                   cx, cy - size, cx + size, cy, cx, cy + size, cx - size, cy,
                   STROKE)
    if shape == "triangle":
        return '<polygon points="%.3f,%.3f %.3f,%.3f %.3f,%.3f" ' \
               'fill="#ffffff" stroke="%s" stroke-width="1.4"/>' % (
                   cx, cy - size, cx + size * 0.9, cy + size * 0.8,
                   cx - size * 0.9, cy + size * 0.8, STROKE)
    if shape in ("cross", "x"):
        return ('<path d="M%.3f,%.3f L%.3f,%.3f" stroke="%s" '
                'stroke-width="1.6"/>' % (cx - size, cy - size, cx + size,
                                          cy + size, STROKE) if shape == "x"
                else '<path d="M%.3f,%.3f L%.3f,%.3f M%.3f,%.3f L%.3f,%.3f" '
                     'stroke="%s" stroke-width="1.6"/>'
                     % (cx - size, cy, cx + size, cy, cx, cy - size, cx,
                        cy + size, STROKE))
    raise ContractError("unknown marker %r (schema enumerates the marker "
                        "shapes)" % shape)


def draw_axes(canvas, document):
    x_label = document["series"][0]["x_units"]
    y_label = document["series"][0]["y_units"]
    parts = [
        '<rect x="%d" y="%d" width="%d" height="%d" fill="#ffffff" '
        'stroke="#d0d0d0"/>' % (MARGIN_LEFT, MARGIN_TOP, PLOT_WIDTH,
                                PLOT_HEIGHT),
    ]
    for value in canvas.x_ticks:
        x = canvas.x(value)
        parts.append('<line x1="%.3f" y1="%d" x2="%.3f" y2="%d" '
                     'stroke="#e0e0e0"/>'
                     % (x, MARGIN_TOP, x, MARGIN_TOP + PLOT_HEIGHT))
        parts.append('<line x1="%.3f" y1="%d" x2="%.3f" y2="%d" '
                     'stroke="%s"/>' % (x, MARGIN_TOP + PLOT_HEIGHT, x,
                                        MARGIN_TOP + PLOT_HEIGHT + 6, STROKE))
        parts.append('<text x="%.3f" y="%d" text-anchor="middle" '
                     'font-family="DejaVu Sans, sans-serif" font-size="11">'
                     '%s</text>' % (x, MARGIN_TOP + PLOT_HEIGHT + 22,
                                    fmt(value, canvas.x_decimals)))
    for value in canvas.y_ticks:
        y = canvas.y(value)
        parts.append('<line x1="%d" y1="%.3f" x2="%d" y2="%.3f" '
                     'stroke="#e0e0e0"/>'
                     % (MARGIN_LEFT, y, MARGIN_LEFT + PLOT_WIDTH, y))
        parts.append('<line x1="%d" y1="%.3f" x2="%d" y2="%.3f" '
                     'stroke="%s"/>' % (MARGIN_LEFT - 6, y, MARGIN_LEFT, y,
                                        STROKE))
        parts.append('<text x="%d" y="%.3f" text-anchor="end" '
                     'font-family="DejaVu Sans, sans-serif" font-size="11">'
                     '%s</text>' % (MARGIN_LEFT - 10, y + 4,
                                    fmt(value, canvas.y_decimals)))
    parts.append('<line x1="%d" y1="%d" x2="%d" y2="%d" stroke="%s"/>'
                 % (MARGIN_LEFT, MARGIN_TOP + PLOT_HEIGHT,
                    MARGIN_LEFT + PLOT_WIDTH, MARGIN_TOP + PLOT_HEIGHT,
                    STROKE))
    parts.append('<line x1="%d" y1="%d" x2="%d" y2="%d" stroke="%s"/>'
                 % (MARGIN_LEFT, MARGIN_TOP, MARGIN_LEFT,
                    MARGIN_TOP + PLOT_HEIGHT, STROKE))
    parts.append('<text x="%d" y="%d" text-anchor="middle" '
                 'font-family="DejaVu Sans, sans-serif" font-size="12">%s'
                 '</text>'
                 % (MARGIN_LEFT + PLOT_WIDTH // 2, MARGIN_TOP + PLOT_HEIGHT
                    + 44, escape(axis_label(x_label, "time"))))
    parts.append('<text x="24" y="%d" text-anchor="middle" '
                 'font-family="DejaVu Sans, sans-serif" font-size="12" '
                 'transform="rotate(-90 24 %d)">%s</text>'
                 % (MARGIN_TOP + PLOT_HEIGHT // 2, MARGIN_TOP
                    + PLOT_HEIGHT // 2, escape(axis_label(y_label,
                                                           "voltage"))))
    return parts


QUANTITIES = {"V": "voltage", "mV": "voltage", "A": "current",
              "mA": "current", "s": "time", "ms": "time"}


def axis_label(units, fallback):
    quantity = QUANTITIES.get(units, fallback)
    return "%s [%s]" % (quantity, units)


def draw_band(canvas, document, identifiers):
    band = document.get("tolerance_band")
    if not band:
        return [], []
    lookup = {series["name"]: series for series in document["series"]}
    lower = lookup[band["lower_series"]]
    upper = lookup[band["upper_series"]]
    if len(lower["x"]) == len(upper["x"]):
        forward = " ".join("%.3f,%.3f" % (canvas.x(x), canvas.y(y))
                           for x, y in zip(upper["x"], upper["y"]))
        backward = " ".join("%.3f,%.3f" % (canvas.x(x), canvas.y(y))
                            for x, y in zip(reversed(lower["x"]),
                                            reversed(lower["y"])))
        hatch = band.get("hatch", "///")
        identifier = identifiers[hatch]
        polygon = ('<polygon points="%s %s" fill="url(#%s)" '
                   'stroke="%s" stroke-width="1" stroke-dasharray="3 3"/>'
                   % (forward, backward, identifier, SOFT))
    else:
        polygon = ""
    legend = ('<rect x="0" y="-6" width="34" height="12" '
              'fill="url(#%s)" stroke="%s" stroke-dasharray="3 3"/>'
              % (identifiers[band.get("hatch", "///")], SOFT),
              None, escape(band["description"]))
    return ([polygon] if polygon else []), [legend]


def draw_series(canvas, document):
    parts, legend = [], []
    for series in document["series"]:
        if series["role"] in ("tolerance_lower", "tolerance_upper"):
            continue
        encoding = series["non_color_encoding"]
        style = DASH_PATTERNS.get(encoding["line_style"])
        if style is None:
            raise ContractError("unknown line_style %r"
                                % encoding["line_style"])
        dash = ' stroke-dasharray="%s"' % style if style else ""
        parts.append('<polyline points="%s" fill="none" stroke="%s" '
                     'stroke-width="2"%s/>'
                     % (canvas.points(series), STROKE, dash))
        for position, (x, y) in enumerate(zip(series["x"], series["y"])):
            if position % 4:
                continue  # keep the marker cadence deterministic and sparse
            glyph = marker_path(encoding["marker"], canvas.x(x), canvas.y(y))
            if glyph:
                parts.append(glyph)
        legend.append(('<line x1="0" y1="0" x2="34" y2="0" stroke="%s" '
                       'stroke-width="2"%s/>'
                       % (STROKE, dash),
                       marker_path(encoding["marker"], 17, 0),
                       "%s (%s, %s)" % (escape(series["name"]),
                                        encoding["line_style"],
                                        encoding["marker"])))
    return parts, legend


def draw_legend(entries):
    parts = []
    top = MARGIN_TOP + PLOT_HEIGHT + 44
    for index, entry in enumerate(entries):
        y = top + index * 18
        parts.append('<g transform="translate(%d,%d)">' % (40, y))
        parts += [entry[0]]
        if len(entry) == 3 and entry[1]:
            parts.append(entry[1])
        parts.append('<text x="44" y="4" font-family="DejaVu Sans, '
                     'sans-serif" font-size="11">%s</text>' % entry[-1])
        parts.append("</g>")
    return parts


def footer_values(document, root, plot_hash, evidence_relative):
    provisional = ",provisional" if document.get("provisional") else ""
    values = {
        "plot_id": document["plot_id"],
        "plot_version": document["plot_version"],
        "plot_data_hash": plot_hash,
        "status": document["status"],
        "method": document["method"],
        "credibility_level": document["credibility_level"],
        "provisional": "true" if document.get("provisional") else "false",
        "source_evidence_path": evidence_relative,
        "source_evidence_hash": document["source_evidence_hash"],
        "oracle_id": document["oracle_id"],
        "renderer_tool": document["renderer_tool"],
        "renderer_tool_version": document["renderer_tool_version"],
        "renderer_tool_digest": document["renderer_tool_digest"],
        "generated_at": document.get("generated_at"),
    }
    footer = [
        ("EVIDENCE", "%s@%s" % (values["plot_id"], values["plot_version"])),
        ("STATUS", "%s(%s,%s%s)" % (values["status"], values["method"],
                                    values["credibility_level"],
                                    provisional)),
        ("PLOT-DATA", values["plot_data_hash"]),
        ("ORACLE", values["oracle_id"]),
        ("RENDERER", "%s %s" % (values["renderer_tool"],
                                values["renderer_tool_version"])),
    ]
    return values, footer


def render(root, plot_path, document, expected_version):
    if document["renderer_tool_version"] != expected_version:
        raise ContractError(
            "--version %s does not match the plot-data renderer_tool_version "
            "%s" % (expected_version, document["renderer_tool_version"]))
    if document["renderer_tool"] != "cancestry-render-modelica":
        raise ContractError("plot-data names renderer_tool %r; this renderer "
                            "is cancestry-render-modelica"
                            % document["renderer_tool"])
    evidence_relative = check_source_evidence(root, document)
    plot_hash = sha256_file(plot_path)
    canvas = Canvas(document)
    hatches = set()
    band = document.get("tolerance_band")
    if band:
        hatches.add(band.get("hatch", "///"))
    definitions, identifiers = hatch_definitions(hatches)

    values, footer = footer_values(document, root, plot_hash,
                                   evidence_relative)
    body = [
        '<?xml version="1.0" encoding="UTF-8"?>',
        '<svg xmlns="http://www.w3.org/2000/svg" width="%d" height="%d" '
        'viewBox="0 0 %d %d" role="img" aria-labelledby="%s-title %s-desc">'
        % (WIDTH, HEIGHT, WIDTH, HEIGHT, values["plot_id"],
           values["plot_id"]),
        "<title id=\"%s-title\">%s</title>"
        % (values["plot_id"], escape(document.get("plot_title")
                                     or values["plot_id"])),
        "<desc id=\"%s-desc\">%s</desc>"
        % (values["plot_id"], escape(
            document.get("plot_description")
            or "Model series versus oracle series with a hatched tolerance "
               "band; series are distinguished by line style and marker "
               "shape as well as by name.")),
        "<!--",
        "cancestry-provenance",
    ]
    body += ["%s=%s" % (key, escape(value)) for key, value in values.items()
             if value is not None]
    body += ["-->", '<defs>%s</defs>' % "".join(definitions)]
    body += draw_axes(canvas, document)
    band_parts, band_legend = draw_band(canvas, document, identifiers)
    body += band_parts
    series_parts, series_legend = draw_series(canvas, document)
    body += series_parts
    body += ['<text x="%d" y="%d" font-family="DejaVu Sans, sans-serif" '
             'font-size="13">%s</text>'
             % (MARGIN_LEFT, 34, escape(document.get("plot_title")
                                        or values["plot_id"]))]
    body += draw_legend(band_legend + series_legend)
    top = HEIGHT - 14 - 15 * 7  # five required lines + SOURCE + GENERATED
    for index, (label, text) in enumerate(footer):
        body.append('<text x="24" y="%d" font-family="DejaVu Sans Mono, '
                    'monospace" font-size="11">%s  %s</text>'
                    % (top + index * 15, label, escape(text)))
    body.append('<text x="24" y="%d" font-family="DejaVu Sans Mono, '
                'monospace" font-size="11">SOURCE  %s %s</text>'
                % (top + 5 * 15, escape(values["source_evidence_path"]),
                   values["source_evidence_hash"]))
    body.append('<text x="24" y="%d" font-family="DejaVu Sans Mono, '
                'monospace" font-size="11">GENERATED  %s</text>'
                % (top + 6 * 15, escape(values["generated_at"] or "none")))
    body.append("</svg>")
    return "\n".join(body) + "\n"


def main(argv=None):
    parser = argparse.ArgumentParser(
        description="Render a hardware plot-data file to SVG (H-03 contract).")
    parser.add_argument("--plot-data", required=True,
                       help="path to the <plot_id>.plot.json file")
    parser.add_argument("--version", required=True,
                       help="renderer version; must match the plot-data")
    parser.add_argument("--output", required=True,
                       help="'-' for stdout, otherwise a file path")
    parser.add_argument("--root", default=None,
                       help="repository root (default: found above the "
                            "plot-data file)")
    args = parser.parse_args(argv)

    try:
        root = Path(args.root).resolve() if args.root \
            else repository_root(args.plot_data)
        plot_path = Path(args.plot_data).resolve()
        document = load_json(plot_path, "plot-data")
        if plot_path.name.endswith(PLOT_DATA_SUFFIX):
            stem = plot_path.name[:-len(PLOT_DATA_SUFFIX)]
            if stem != document.get("plot_id"):
                raise ContractError("file name does not match plot_id %r"
                                    % document.get("plot_id"))
        validate(root, document, plot_path)
        svg = render(root, plot_path, document, args.version)
    except ContractError as error:
        sys.stderr.write("render error: %s\n" % error)
        return 2

    if args.output == "-":
        sys.stdout.write(svg)
    else:
        Path(args.output).write_text(svg, encoding="utf-8")
    return 0


if __name__ == "__main__":
    sys.exit(main())
