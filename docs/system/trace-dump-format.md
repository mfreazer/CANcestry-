# CANcestry Trace Dump Format Specification

Version: 1.0.0
Format magic: `CTRC` (CANcestry TRace Capture)

Implements: SW-FR-TOOL-005, SW-FR-TOOL-009, SW-FR-TOOL-010.

This document is the normative reference for the binary trace ring-buffer
dump consumed by `tools/trace_viewer/` (`trace_format.py`,
`trace_viewer.py`). The dump captures the `core/fsm` trace ring
(`cancestry_fsm_trace_record_t`, `cancestry_fsm_trace_ring_t`) and
`core/hal` fault and frame-flow reports on the target into a fixed-size
binary file, so a captured run can be reconstructed deterministically on the
host (SYS-NF-001).

## 1. Byte Order and Encoding

All integers are little-endian, regardless of the target architecture, so
captures are comparable across targets. Text is UTF-8. The format contains
no floating-point values.

## 2. File Header

The file starts with a fixed 32-byte header:

| Offset | Size | Field          | Value                                              |
|-------:|-----:|----------------|----------------------------------------------------|
| 0      | 4    | `magic`        | `CTRC` (bytes `43 54 52 43`)                        |
| 4      | 2    | `version`      | Format version, currently `1`                       |
| 6      | 2    | `header_size`  | `32`                                                |
| 8      | 4    | `record_size`  | Size of one record in bytes (at least 29)           |
| 12     | 4    | `record_count` | Number of records that follow the header            |
| 16     | 4    | `capacity`     | Ring capacity the capture was taken with            |
| 20     | 4    | `dropped`      | Records dropped from the ring before the capture    |
| 24     | 8    | `reserved`     | Zero; readers shall not interpret                   |

A reader shall refuse the file when:

- the file is shorter than `header_size`;
- `magic` differs from `CTRC`;
- `version` is not a version the reader supports;
- `header_size` differs from the size the reader implements (32);
- `record_size` is below 28 + 1 bytes (a record carries at least the
  terminating NUL of an empty detail);
- `record_count` exceeds `capacity`.

## 3. Record Layout

`record_count` records follow the header back to back, each exactly
`record_size` bytes:

| Offset | Size        | Field         | Meaning                                        |
|-------:|------------:|---------------|------------------------------------------------|
| 0      | 8           | `sequence`    | Unsigned, strictly increasing in chronological order |
| 8      | 8           | `timestamp_us`| Signed monotonic microseconds (SYS-NF-001 time base) |
| 16     | 4           | `instance_id` | FSM instance identifier, `0` for non-instance sources |
| 20     | 4           | `kind`        | Signed record kind (section 4)                 |
| 24     | 4           | `code`        | Signed kind-specific code, `0` when unused     |
| 28     | `record_size` - 28 | `detail` | UTF-8 text, NUL terminated; text ends at the first NUL |

The `detail` field is sized by the dumper (`record_size`); a reader shall
accept any `record_size >= 29` and shall truncate the decoded text at the
first NUL byte. The engine-side trace record holds up to
`CANCESTRY_FSM_TRACE_DETAIL_MAX` (48) characters, so a dump taken from the
reference engine has `record_size == 76`.

The `sequence` numbers are assigned by the dumping tool, not by the engine:
`cancestry_fsm_trace_record_t` carries no sequence field. The dumper walks
the ring slot by slot and numbers the records in chronological order, so
sequence `n + 1` is the chronological successor of sequence `n`.

## 4. Record Kinds

Values 0-11 are the canonical values of `cancestry_fsm_trace_kind_t`
(`core/fsm/include/cancestry/fsm/engine.h`):

| Value | Name           | Typical `detail` rendering                        |
|------:|----------------|---------------------------------------------------|
| 0     | `event`        | `event <name> seq=<n>`                            |
| 1     | `transition`   | `<from>-><to>`; the initial entry is `(initial)-><to>` |
| 2     | `action`       | `<action> ok`                                     |
| 3     | `action_error` | action execution failure text                     |
| 4     | `guard_true`   | guard expression text                             |
| 5     | `guard_false`  | guard expression text                             |
| 6     | `guard_error`  | guard evaluation failure text                     |
| 7     | `denied`       | event/transition refusal text                     |
| 8     | `timer`        | `<timer> expired`                                 |
| 9     | `lifecycle`    | lifecycle step text (instance start, ready, run)  |
| 10    | `warning`      | warning text                                      |
| 11    | `fault`        | `<FAULT_NAME>/<severity>`                         |

Values 1000-1002 identify records of HAL origin (section 4 of
`docs/system/event-ordering.md` for the fault event path). The `code` field
of kind 1000 carries `cancestry_hal_fault_code_t`
(`core/hal/include/cancestry/hal/types.h`):

| Value | Kind            | `code` meaning                                       |
|------:|-----------------|------------------------------------------------------|
| 1000  | `hal_fault`     | 0 `none`, 1 `rx_ring_overflow`, 2 `tx_ring_overflow`, 3 `bus_backpressure`, 4 `interface_down`, 5 `bus_error_passive`, 6 `bus_off`, 7 `timestamp_non_monotonic`, 8 `malformed_frame`, 9 `init_failed`, 10 `protocol_unsupported` |
| 1001  | `hal_rx_frame`  | interface index of the received frame                |
| 1002  | `hal_tx_frame`  | interface index of the transmitted frame             |

A record of kind 11 or 1000 is a fault record. Unknown kind values render as
`kind_<n>`; readers shall not refuse them (forward compatibility).

## 5. Ring Semantics and Chronological Reconstruction

The target trace ring is a fixed-size circular buffer: when it is full, a
new record replaces the oldest one and `dropped` counts every replaced
record. The physical order of the records in the file is therefore the ring
slot order, which equals the chronological order only while the ring has
never wrapped.

A reader shall reconstruct the chronological order deterministically from
the `sequence` numbers alone:

1. A *descent point* is a physical index `i` where
   `sequence[i + 1] < sequence[i]`.
2. When `record_count == capacity > 0` the physical end of the file wraps
   around to the ring's oldest record, so the pair (last, first) participates:
   if `sequence[0] < sequence[last]`, index `last` is a descent point.
3. Zero descent points: the ring never wrapped; file order is chronological
   and the dump is not marked wrapped.
4. Exactly one descent point at index `k`: the chronological order is
   `records[k+1:] + records[:k+1]` and the dump is marked wrapped
   (unless `k` is the last index, which only happens in the step-2 scan).
5. More than one descent point: the dump is corrupt. The reader shall refuse
   it and never guess an order (fail closed).
6. A full ring whose records are monotonic as stored was captured without
   wrapping; it must not be rotated (the step-2 gate on
   `record_count == capacity` exists exactly for this case).
7. After reordering, the sequence numbers must be strictly increasing
   (which also implies uniqueness); any violation is refused.

Sequence gaps are legal: `dropped` records before the capture window leave
holes, and reconstruction uses the order, not the arithmetic progression.

## 6. Determinism Requirements

- Parsing the same dump bytes twice shall produce the identical timeline.
- Reconstruction depends only on the file content; wall-clock time,
  locale and environment shall not influence the output.
- Timelines render relative timestamps against the first chronological
  record by default; absolute rendering shall show the raw `timestamp_us`.

## 7. Relationship to Other Documents

- `core/fsm/include/cancestry/fsm/engine.h` defines the traced kinds and the
  ring structure that feed this format.
- `docs/system/event-ordering.md` defines the monotonic microsecond time
  base and the event priority classes referenced by event records.
- `docs/software/SwRS.md` section 13 (SW-FR-TOOL-005..010) states the tool
  requirements this format satisfies.
