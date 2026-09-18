#!/usr/bin/env python3
"""trace_viewer - render CANcestry trace ring dumps as a timeline.

Phase 8, issue #22. The tool parses the binary trace ring dump documented in
docs/system/trace-dump-format.md (FSM trace records from core/fsm and HAL
fault/frame reports from core/hal), reconstructs the chronological order
across ring wrap-around from the monotonic sequence numbers, and prints a
human-readable timeline (SW-FR-TOOL-005, SW-FR-TOOL-006).

Requirements traced: SW-FR-TOOL-005, SW-FR-TOOL-006, SW-FR-TOOL-007,
SW-FR-TOOL-010. Test ids: TRACE-VIEW-WRAP-001, TRACE-VIEW-TIMELINE-001,
TRACE-VIEW-DOT-001.

Usage:
    python3 tools/trace_viewer/trace_viewer.py dump.bin
    python3 tools/trace_viewer/trace_viewer.py dump.bin --json
    python3 tools/trace_viewer/trace_viewer.py dump.bin --dot trace.dot
    python3 tools/trace_viewer/trace_viewer.py dump.bin --kind transition --instance 1

The optional Graphviz output (.dot) draws the state transitions actually
taken during the run; every edge that led into a fault is drawn in red
(SW-FR-TOOL-007). An edge is "faulted" when a FAULT or HAL-fault record for
the same instance appears after the transition and before that instance's
next transition - a deterministic, local rule a reviewer can re-check by eye
in the timeline.

Exit codes: 0 success, 1 the dump was refused (bad header, corrupt ring
order), 2 usage error.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import sys
from typing import Dict, List, Optional, Tuple

if __package__ in (None, ""):  # pragma: no cover - script mode only
    _REPO_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    if _REPO_ROOT not in sys.path:
        sys.path.insert(0, _REPO_ROOT)
    from tools.trace_viewer import trace_format as tf  # type: ignore  # noqa: E402
else:
    from . import trace_format as tf

_TRANSITION_DETAIL = re.compile(r"^(?P<from>.+?)->(?P<to>.+)$")

_KIND_COLUMN = 14


# ---------------------------------------------------------------------------
# Rendering.
# ---------------------------------------------------------------------------

def format_timestamp(timestamp_us: int, base_us: Optional[int] = None) -> str:
    """Render a monotonic microsecond timestamp; relative when a base is given."""
    value = float(timestamp_us - base_us) / 1e6 if base_us is not None \
        else float(timestamp_us) / 1e6
    return "%+.6f" % value


def render_timeline(dump: tf.TraceDump, relative: bool = True) -> str:
    """Human-readable chronological timeline, one record per line."""
    lines: List[str] = []
    header = dump.header
    base = dump.records[0].timestamp_us if (dump.records and relative) else None
    lines.append("CANcestry trace dump: %d record(s), ring capacity %d, "
                 "%d dropped before capture%s"
                 % (len(dump.records), header.capacity, header.dropped,
                    ", ring wrapped" if dump.wrapped else ""))
    lines.append("%-8s %-13s %-5s %-*s %8s %s"
                 % ("seq", "time[s]", "inst", _KIND_COLUMN, "kind", "code",
                    "detail"))
    for record in dump.records:
        lines.append("%-8d %-13s %-5d %-*s %8d %s"
                     % (record.sequence,
                        format_timestamp(record.timestamp_us, base),
                        record.instance_id,
                        _KIND_COLUMN,
                        tf.kind_name(record.kind, record.code),
                        record.code,
                        record.detail))
    return "\n".join(lines)


def record_to_dict(record: tf.TraceRecord, base: Optional[int]) -> dict:
    return {
        "sequence": record.sequence,
        "time_s": round((record.timestamp_us -
                         (base if base is not None else 0)) / 1e6, 9),
        "timestamp_us": record.timestamp_us,
        "instance_id": record.instance_id,
        "kind": record.kind,
        "kind_name": tf.kind_name(record.kind, record.code),
        "code": record.code,
        "detail": record.detail,
    }


def render_json(dump: tf.TraceDump, relative: bool = True) -> str:
    base = dump.records[0].timestamp_us if (dump.records and relative) else None
    return json.dumps({
        "header": {
            "format_version": dump.header.format_version,
            "record_size": dump.header.record_size,
            "record_count": dump.header.record_count,
            "capacity": dump.header.capacity,
            "dropped": dump.header.dropped,
            "wrapped": dump.wrapped,
        },
        "records": [record_to_dict(record, base) for record in dump.records],
    }, indent=2)


# ---------------------------------------------------------------------------
# Graphviz export (SW-FR-TOOL-007).
# ---------------------------------------------------------------------------

def _state_edges(dump: tf.TraceDump) -> Tuple[Dict[Tuple[str, str], int],
                                              Dict[Tuple[str, str], int],
                                              int]:
    """Collect transition edges and the faulted subset.

    An edge (from, to) is faulted when a fault record for the same instance
    follows the transition before that instance's next transition. The
    initial pseudo-transition renders as "(initial)" -> state.
    """
    edges: Dict[Tuple[str, str], int] = {}
    faulted: Dict[Tuple[str, str], int] = {}
    last_edge_of_instance: Dict[int, Tuple[str, str]] = {}
    for record in dump.records:
        if record.kind == 1:  # CANCESTRY_FSM_TRACE_TRANSITION
            match = _TRANSITION_DETAIL.match(record.detail)
            if match:
                edge = (match.group("from"), match.group("to"))
                edges[edge] = edges.get(edge, 0) + 1
                last_edge_of_instance[record.instance_id] = edge
        elif record.is_fault():
            edge = last_edge_of_instance.get(record.instance_id)
            if edge is not None:
                faulted[edge] = faulted.get(edge, 0) + 1
    return edges, faulted, len(last_edge_of_instance)


def _dot_quote(name: str) -> str:
    return '"%s"' % name.replace("\\", "\\\\").replace('"', '\\"')


def render_dot(dump: tf.TraceDump) -> str:
    """Graphviz .dot of the transitions taken; faulted edges in red."""
    edges, faulted, _ = _state_edges(dump)
    lines = [
        "/* Generated by CANcestry trace_viewer: transitions actually taken.",
        "   Faulted edges (a fault followed before the instance's next",
        "   transition) are red (SW-FR-TOOL-007). */",
        "digraph cancestry_trace {",
        "    rankdir=LR;",
        '    node [shape=box fontname="monospace"];',
        '    edge [fontname="monospace" fontsize=10];',
    ]
    for (source, target), count in sorted(edges.items()):
        attributes = ["label=%d" % count]
        if (source, target) in faulted:
            attributes.append("color=red")
            attributes.append("fontcolor=red")
            attributes.append("penwidth=2")
        lines.append("    %s -> %s [%s];" % (_dot_quote(source),
                                             _dot_quote(target),
                                             ", ".join(attributes)))
    lines.append("}")
    return "\n".join(lines) + "\n"


# ---------------------------------------------------------------------------
# CLI.
# ---------------------------------------------------------------------------

def build_arg_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="trace_viewer",
        description="Parse a CANcestry binary trace ring dump and print a "
                    "chronological timeline (wrap-around aware).")
    parser.add_argument("dump", help="binary trace dump file (CTRC format)")
    parser.add_argument("--json", action="store_true",
                        help="emit the timeline as JSON instead of text")
    parser.add_argument("--dot", metavar="FILE",
                        help="also write a Graphviz .dot of the transitions "
                             "taken (faulted edges in red)")
    parser.add_argument("--kind", action="append", default=None,
                        metavar="NAME",
                        help="show only records of this kind; repeatable")
    parser.add_argument("--instance", type=int, default=None, metavar="ID",
                        help="show only records of this instance id")
    parser.add_argument("--absolute", action="store_true",
                        help="print absolute instead of relative timestamps")
    return parser


def main(argv: Optional[List[str]] = None) -> int:
    parser = build_arg_parser()
    args = parser.parse_args(argv)
    try:
        with open(args.dump, "rb") as handle:
            blob = handle.read()
    except OSError as error:
        print("trace_viewer: error: %s" % error, file=sys.stderr)
        return 2
    try:
        dump = tf.parse_dump(blob)
    except tf.TraceFormatError as error:
        print("trace_viewer: %s: %s" % (args.dump, error), file=sys.stderr)
        return 1

    records = dump.records
    if args.instance is not None:
        records = [r for r in records if r.instance_id == args.instance]
    if args.kind:
        wanted = set(args.kind)
        records = [r for r in records
                   if tf.kind_name(r.kind, r.code) in wanted
                   or r.kind_name() in wanted]
    filtered = tf.TraceDump(header=dump.header, records=records,
                            wrapped=dump.wrapped)

    if args.json:
        sys.stdout.write(render_json(filtered) + "\n")
    else:
        sys.stdout.write(render_timeline(filtered,
                                         relative=not args.absolute) + "\n")
    if args.dot:
        try:
            with open(args.dot, "w", encoding="utf-8") as handle:
                handle.write(render_dot(dump))
        except OSError as error:
            print("trace_viewer: error: %s" % error, file=sys.stderr)
            return 2
        print("trace_viewer: wrote %s" % args.dot, file=sys.stderr)
    return 0


if __name__ == "__main__":  # pragma: no cover  # pragma: no cover
    sys.exit(main())
