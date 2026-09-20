#!/usr/bin/env python3
"""Fixture stub renderer that drifts (H-03 test suite, T24/T25).

Identical to stub_pass.py except for one extra byte before the closing tag, so
re-rendering the same plot-data produces different bytes than the committed
artifact. The gate must report that as renderer drift: an undeclared drift is
an error, and a drift declared with ``renderer-drift:`` in the PR body plus
``RENDERER_DRIFT_ALLOWED=1`` is a warning instead.

Keep everything else in this file in sync with stub_pass.py: the test suite
proves stub_pass reproduces the committed clean artifact exactly, and that
this stub does not.
"""

import argparse
import hashlib
import json
import pathlib
import sys


def load_plot_data(path):
    return json.loads(pathlib.Path(path).read_text(encoding="utf-8"))


def provenance_values(document, plot_hash):
    return {
        "plot_id": document["plot_id"],
        "plot_version": document["plot_version"],
        "plot_data_hash": plot_hash,
        "status": document["status"],
        "method": document["method"],
        "credibility_level": document["credibility_level"],
        "provisional": "true" if document["provisional"] else "false",
        "source_evidence_path": document["source_evidence_path"],
        "source_evidence_hash": document["source_evidence_hash"],
        "oracle_id": document["oracle_id"],
        "renderer_tool": document["renderer_tool"],
        "renderer_tool_version": document["renderer_tool_version"],
        "renderer_tool_digest": document["renderer_tool_digest"],
    }


def comment(values):
    order = ("plot_id", "plot_version", "plot_data_hash", "status", "method",
             "credibility_level", "provisional", "source_evidence_path",
             "source_evidence_hash", "oracle_id", "renderer_tool",
             "renderer_tool_version", "renderer_tool_digest")
    lines = ["<!-- cancestry-provenance"]
    lines.extend("%s=%s" % (key, values[key]) for key in order)
    lines.append("-->")
    return lines


def footer(values):
    provisional = ",provisional" if values["provisional"] == "true" else ""
    return [
        "EVIDENCE   %s@%s" % (values["plot_id"], values["plot_version"]),
        "STATUS     %s(%s,%s%s)" % (values["status"], values["method"],
                                    values["credibility_level"], provisional),
        "SOURCE     %s %s" % (values["source_evidence_path"],
                              values["source_evidence_hash"]),
        "PLOT-DATA  %s" % values["plot_data_hash"],
        "ORACLE     %s" % values["oracle_id"],
        "RENDERER   %s %s" % (values["renderer_tool"],
                              values["renderer_tool_version"]),
    ]


def escape(text):
    return (text.replace("&", "&amp;").replace("<", "&lt;")
            .replace(">", "&gt;"))


def polyline(series, y_min, y_max):
    xs = series["x"]
    ys = series["y"]
    span = (y_max - y_min) or 1.0
    points = []
    for x, y in zip(xs, ys):
        px = 60 + 520 * (x - xs[0]) / ((xs[-1] - xs[0]) or 1.0)
        py = 420 - 340 * (y - y_min) / span
        points.append("%.3f,%.3f" % (px, py))
    return " ".join(points)


def band_polygon(lower, upper, y_min, y_max):
    span = (y_max - y_min) or 1.0
    top = polyline(upper, y_min, y_max).split()
    bottom = polyline(lower, y_min, y_max).split()[::-1]
    return " ".join(top + bottom)


def render(document, plot_hash):
    values = provenance_values(document, plot_hash)
    series = {entry["name"]: entry for entry in document["series"]}
    ys = [value for entry in document["series"] for value in entry["y"]]
    y_min, y_max = min(ys), max(ys)
    payload = []
    payload.extend(comment(values))
    payload.append('<title>%s</title>' % escape(document.get("plot_title", values["plot_id"])))
    payload.append('<desc>Rendered from %s by %s %s.</desc>' % (
        document["source_evidence_path"], values["renderer_tool"],
        values["renderer_tool_version"]))
    payload.append('<rect x="0" y="0" width="640" height="480" fill="#ffffff"/>')
    payload.append('<defs><pattern id="hatch" width="6" height="6" '
                   'patternUnits="userSpaceOnUse"><path d="M0,0 L6,6" '
                   'stroke="#000000" stroke-width="0.4"/></pattern></defs>')
    band = document.get("tolerance_band")
    lines = []
    if band:
        polygon = band_polygon(series[band["lower_series"]],
                               series[band["upper_series"]], y_min, y_max)
        lines.append('<polygon points="%s" fill="url(#hatch)" '
                     'stroke="#000000" stroke-width="0.5"/>' % polygon)
    styles = {"solid": "", "dashed": ' stroke-dasharray="6 3"',
              "dotted": ' stroke-dasharray="1 3"',
              "dashdot": ' stroke-dasharray="8 3 1 3"'}
    for entry in document["series"]:
        if entry["role"] in ("tolerance_lower", "tolerance_upper"):
            continue
        lines.append('<polyline points="%s" fill="none" stroke="#000000" '
                     'stroke-width="1.5"%s/>' % (
                         polyline(entry, y_min, y_max),
                         styles[entry["non_color_encoding"]["line_style"]]))
    payload.append("".join(lines))
    for row, text in enumerate([document.get("plot_title", values["plot_id"]),
                                ""] + footer(values)):
        payload.append('<text x="12" y="%d" font-family="monospace" '
                       'font-size="11" fill="#000000">%s</text>' % (
                           464 + 14 * row, escape(text)))
    return ("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
            "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"640\" "
            "height=\"480\" viewBox=\"0 0 640 480\" role=\"img\">\n"
            + "\n".join(payload) + "\n<!-- drift -->\n</svg>\n"
            ).encode("utf-8")


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--plot-data", required=True)
    parser.add_argument("--version", required=True)
    parser.add_argument("--output", required=True)
    args = parser.parse_args(argv)
    path = pathlib.Path(args.plot_data)
    document = load_plot_data(path)
    plot_hash = "sha256:" + hashlib.sha256(path.read_bytes()).hexdigest()
    artifact = render(document, plot_hash)
    if args.output == "-":
        sys.stdout.buffer.write(artifact)
    else:
        pathlib.Path(args.output).write_bytes(artifact)
    return 0


if __name__ == "__main__":
    sys.exit(main())
