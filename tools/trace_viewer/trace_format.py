"""Binary trace ring-buffer dump format: parsing and reconstruction.

Implements: SW-FR-TOOL-005, SW-FR-TOOL-010. Normative reference:
docs/system/trace-dump-format.md (introduced with Phase 8, issue #22).

The core/fsm trace ring (``cancestry_fsm_trace_record_t``) and core/hal fault
reports are captured on the target into a fixed-size binary file. Because the
target ring wraps, the physical order of the records in the file is *not*
the chronological order once ``record_count`` reached ``capacity``. The
viewer reconstructs the chronological order deterministically from the
monotonically increasing per-record ``sequence`` numbers (SYS-NF-001):

  * scan the records for "descent points" (where the sequence number of the
    next physical record is smaller than the current one),
  * zero descents: the ring never wrapped, file order is chronological,
  * exactly one descent at index k: chronological order is
    ``records[k+1:] + records[:k+1]`` (the oldest record follows the newest),
  * more than one descent: the dump is corrupt and refused, never guessed.

All integers are little-endian (the format fixes a byte order so captures
are comparable across targets); see docs/system/trace-dump-format.md.

Requirements traced: SW-FR-TOOL-005, SW-FR-TOOL-010. Test ids:
TRACE-VIEW-WRAP-001, TRACE-VIEW-TIMELINE-001, TRACE-VIEW-DOT-001.
"""

from __future__ import annotations

import struct
from dataclasses import dataclass, field
from typing import List, Optional, Tuple

#: File magic: "CTRC" (CANcestry TRace Capture).
MAGIC = b"CTRC"

#: Current format version written by dumpers.
FORMAT_VERSION = 1

#: Fixed header size in bytes.
HEADER_SIZE = 32

#: Bytes of a record before the variable-length detail text.
RECORD_FIXED_SIZE = 28

_HEADER = struct.Struct("<4sHHIIIIQ")

# FSM trace kinds: values are the canonical values of
# cancestry_fsm_trace_kind_t (core/fsm/include/cancestry/fsm/engine.h).
FSM_KIND_NAMES = {
    0: "event",
    1: "transition",
    2: "action",
    3: "action_error",
    4: "guard_true",
    5: "guard_false",
    6: "guard_error",
    7: "denied",
    8: "timer",
    9: "lifecycle",
    10: "warning",
    11: "fault",
}

# HAL-origin records (docs/system/trace-dump-format.md section 4): the dump
# helper maps a HAL fault report onto kind 1000 with the HAL fault code in
# `code`; frame flow records use 1001/1002.
HAL_KIND_FAULT = 1000
HAL_KIND_RX_FRAME = 1001
HAL_KIND_TX_FRAME = 1002

HAL_KIND_NAMES = {
    HAL_KIND_FAULT: "hal_fault",
    HAL_KIND_RX_FRAME: "hal_rx_frame",
    HAL_KIND_TX_FRAME: "hal_tx_frame",
}

# cancestry_hal_fault_code_t (core/hal/include/cancestry/hal/types.h).
HAL_FAULT_NAMES = {
    0: "none",
    1: "rx_ring_overflow",
    2: "tx_ring_overflow",
    3: "bus_backpressure",
    4: "interface_down",
    5: "bus_error_passive",
    6: "bus_off",
    7: "timestamp_non_monotonic",
    8: "malformed_frame",
    9: "init_failed",
    10: "protocol_unsupported",
}


class TraceFormatError(Exception):
    """The dump file does not conform to the documented format."""


@dataclass
class TraceHeader:
    """Parsed dump header (docs/system/trace-dump-format.md section 2)."""

    format_version: int
    record_size: int
    record_count: int
    capacity: int
    dropped: int
    reserved: int


@dataclass
class TraceRecord:
    """One decoded trace record."""

    sequence: int
    timestamp_us: int
    instance_id: int
    kind: int
    code: int
    detail: str
    #: 1-based physical position in the file (diagnostics only).
    physical_index: int = 0

    def kind_name(self) -> str:
        return FSM_KIND_NAMES.get(self.kind, HAL_KIND_NAMES.get(
            self.kind, "kind_%d" % self.kind))

    def is_fault(self) -> bool:
        return self.kind == 11 or self.kind == HAL_KIND_FAULT


@dataclass
class TraceDump:
    """A parsed dump: header plus records in chronological order."""

    header: TraceHeader
    records: List[TraceRecord] = field(default_factory=list)
    wrapped: bool = False


def kind_name(kind: int, code: int = 0) -> str:
    """Stable human-readable name of a record kind."""
    if kind == HAL_KIND_FAULT:
        return "hal_fault:%s" % HAL_FAULT_NAMES.get(code, "code_%d" % code)
    return FSM_KIND_NAMES.get(kind, HAL_KIND_NAMES.get(
        kind, "kind_%d" % kind))


def parse_header(blob: bytes) -> TraceHeader:
    """Parse and validate the 32-byte dump header."""
    if len(blob) < HEADER_SIZE:
        raise TraceFormatError("file is smaller than the %d-byte header"
                               % HEADER_SIZE)
    magic, version, header_size, record_size, record_count, capacity, \
        dropped, reserved = _HEADER.unpack_from(blob, 0)
    if magic != MAGIC:
        raise TraceFormatError("bad magic %r (expected %r)" % (magic, MAGIC))
    if version != FORMAT_VERSION:
        raise TraceFormatError("unsupported format version %d (expected %d)"
                               % (version, FORMAT_VERSION))
    if header_size != HEADER_SIZE:
        raise TraceFormatError("unsupported header size %d" % header_size)
    if record_size < RECORD_FIXED_SIZE + 1:
        raise TraceFormatError("record size %d is below the %d-byte minimum"
                               % (record_size, RECORD_FIXED_SIZE + 1))
    if record_count > capacity:
        raise TraceFormatError("record count %d exceeds ring capacity %d"
                               % (record_count, capacity))
    return TraceHeader(format_version=version, record_size=record_size,
                       record_count=record_count, capacity=capacity,
                       dropped=dropped, reserved=reserved)


def parse_records(blob: bytes, header: TraceHeader) -> List[TraceRecord]:
    """Decode the physical record sequence (not yet re-ordered)."""
    detail_size = header.record_size - RECORD_FIXED_SIZE
    record_struct = struct.Struct("<QqIii%ds" % detail_size)
    expected = header.record_count
    available = len(blob) - HEADER_SIZE
    if available < expected * record_struct.size:
        raise TraceFormatError(
            "file holds %d bytes of records, %d needed for %d records of "
            "size %d" % (max(available, 0), expected * record_struct.size,
                         expected, record_struct.size))
    records: List[TraceRecord] = []
    offset = HEADER_SIZE
    for index in range(expected):
        sequence, timestamp_us, instance_id, kind, code, raw_detail = \
            record_struct.unpack_from(blob, offset)
        detail = raw_detail.split(b"\x00", 1)[0].decode("utf-8",
                                                        errors="replace")
        records.append(TraceRecord(sequence=sequence,
                                   timestamp_us=timestamp_us,
                                   instance_id=instance_id, kind=kind,
                                   code=code, detail=detail,
                                   physical_index=index + 1))
        offset += record_struct.size
    return records


def chronological_order(records: List[TraceRecord],
                        ring_full: bool = False
                        ) -> Tuple[List[TraceRecord], bool]:
    """Reconstruct chronological order from the sequence numbers.

    ``ring_full`` tells whether ``record_count == capacity``: only then are
    the last and first physical records neighbours in the ring, so only then
    may the pair (last, first) take part in the descent scan. A dump from a
    ring that never wrapped must be monotonic as stored. Refuses
    (TraceFormatError) when the dump is internally inconsistent instead of
    guessing an order (fail closed).
    """
    if not records:
        return [], False

    if len(records) == 1:
        return list(records), False

    last = len(records) - 1
    descents = [i for i in range(last)
                if records[i + 1].sequence < records[i].sequence]
    if ring_full and records[0].sequence < records[last].sequence:
        # The physical end of the file wraps around to the ring's oldest
        # record; a smaller sequence there marks the rotation point.
        descents.append(last)
    if len(descents) > 1:
        raise TraceFormatError(
            "%d sequence descents in the dump; the ring order is corrupt and "
            "cannot be reconstructed" % len(descents))

    if not descents:
        ordered = list(records)
        wrapped = False
    else:
        k = descents[0]
        ordered = records[k + 1:] + records[:k + 1]
        wrapped = k != last

    # The reconstructed order must be strictly increasing and unique; any
    # violation means the capture is corrupt (deterministic refusal).
    for previous, current in zip(ordered, ordered[1:]):
        if current.sequence <= previous.sequence:
            raise TraceFormatError(
                "sequence %d does not increase after sequence %d; the dump "
                "is inconsistent" % (current.sequence, previous.sequence))
    # Note: strict increase above already implies sequence uniqueness.
    return ordered, wrapped


def parse_dump(blob: bytes) -> TraceDump:
    """Parse a complete dump file image into a TraceDump."""
    header = parse_header(blob)
    records = parse_records(blob, header)
    ordered, wrapped = chronological_order(records,
                                           ring_full=header.record_count ==
                                           header.capacity and
                                           header.capacity > 0)
    return TraceDump(header=header, records=ordered, wrapped=wrapped)


def build_dump(records: List[tuple], capacity: Optional[int] = None,
               dropped: int = 0, detail_size: int = 48,
               version: int = FORMAT_VERSION) -> bytes:
    """Serialize records (chronological) into a dump image.

    ``records`` is a list of ``(sequence, timestamp_us, instance_id, kind,
    code, detail)`` tuples in chronological order. When ``capacity`` is
    smaller than the number of records the *newest* ``capacity`` records are
    kept, and they are stored in raw slot order (newest record last, oldest
    record in the middle), exactly like a target ring copied slot by slot
    after wrapping: this is what the mock captures for the wrap-around tests
    use.
    """
    if capacity is None:
        capacity = max(len(records), 1)
    if len(records) > capacity:
        retained = records[-capacity:]
        # After the wrap the oldest retained record sits at the write
        # position (len(records) % capacity); a slot-ordered copy therefore
        # starts with the record written after it.
        rotation = capacity - (len(records) % capacity)
        stored = retained[rotation:] + retained[:rotation]
    else:
        stored = list(records)
    detail_struct = struct.Struct("<%ds" % detail_size)
    record_size = RECORD_FIXED_SIZE + detail_size
    blob = bytearray()
    blob += _HEADER.pack(MAGIC, version, HEADER_SIZE, record_size,
                         len(stored), capacity, dropped, 0)
    for record in stored:
        sequence, timestamp_us, instance_id, kind, code, detail = record
        raw_detail = detail.encode("utf-8")[:detail_size - 1]
        blob += struct.pack("<QqIii", sequence, timestamp_us, instance_id,
                            kind, code)
        blob += detail_struct.pack(raw_detail)
    return bytes(blob)
