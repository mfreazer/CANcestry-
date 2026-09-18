# CANcestry trace viewer

Parses a binary trace ring-buffer dump (the `CTRC` format, normatively
specified in [`docs/system/trace-dump-format.md`](../../docs/system/trace-dump-format.md))
and renders a human-readable chronological timeline of the events, state
transitions, actions, guard results, timers and faults a run produced —
including HAL-origin fault and frame-flow records.

Implements: SW-FR-TOOL-005, SW-FR-TOOL-006, SW-FR-TOOL-007, SW-FR-TOOL-009,
SW-FR-TOOL-010 (Phase 8, issue #22).

## Why reconstruction is needed

The target trace ring (`core/fsm`, `cancestry_fsm_trace_ring_t`) is a
circular buffer: once it is full, new records overwrite the oldest ones, so
the physical record order in a dump is *not* chronological. Each record
carries a monotonically increasing `sequence` number assigned at dump time;
the viewer scans for descent points and rotates the record list back into
chronological order (deterministically — an ambiguous dump with more than
one descent is refused, never guessed). See section 5 of the format
specification.

## Usage

```
python3 tools/trace_viewer/trace_viewer.py <dump.bin> [options]
```

| Option | Effect |
|---|---|
| `--json` | Print the timeline as JSON instead of text |
| `--dot FILE` | Additionally write a Graphviz `.dot` state graph |
| `--kind NAME` | Only records of this kind (repeatable, e.g. `transition`, `fault`, `hal_fault:bus_off`) |
| `--instance ID` | Only records of this FSM instance |
| `--absolute` | Show raw `timestamp_us` instead of times relative to the first record |

Exit codes: `0` ok, `1` the dump violates the format, `2` usage or I/O
error.

### Example

```
$ python3 tools/trace_viewer/trace_viewer.py capture.bin
CANcestry trace dump: 6 record(s), ring capacity 8, 2 dropped before capture
    +0.000000  inst 1  lifecycle   ready
    +0.001000  inst 1  transition  (initial)->OFF
    +0.002000  inst 1  transition  OFF->ON
    +0.003000  inst 1  fault       CLUSTER_TIMEOUT/warning
    +0.004000  inst 2  hal_fault:vcan0
    +0.005000  inst 1  transition  ON->OFF

$ python3 tools/trace_viewer/trace_viewer.py capture.bin --dot trace.dot
trace_viewer: wrote trace.dot
```

The `.dot` output contains one edge per transition actually taken; an edge
followed by a fault of the same instance before that instance's next
transition is drawn red (`color=red fontcolor=red penwidth=2`),
so a state graph review shows the faulted paths at a glance
(SW-FR-TOOL-007).

## Dependencies

Python 3 standard library only (`argparse`, `json`, `struct`). There are no
third-party dependencies and no C dependencies are added to `core/` or
`platform/` (SW-FR-TOOL-010).

## Tests

`tests/unit/tools/test_trace_viewer.py` (parsing, wrap-around
reconstruction, renderers) and `tests/unit/tools/test_trace_viewer_cli.py`
(CLI contract) cover every line of both modules, including the
wrap-around reconstruction test that was written first as the
highest-risk area (TRACE-VIEW-WRAP-001).
