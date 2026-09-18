"""trace_viewer tests (Phase 8, issue #22).

The wrap-around chronological reconstruction test was written FIRST, per the
issue #22 system-engineer note ("write the test for the wrap-around
chronological reconstruction first, as that is the highest risk area for
non-deterministic parsing").

Requirements traced: SW-FR-TOOL-005, SW-FR-TOOL-006, SW-FR-TOOL-007,
SW-FR-TOOL-010.
Test ids: TRACE-VIEW-WRAP-001, TRACE-VIEW-TIMELINE-001, TRACE-VIEW-DOT-001,
TRACE-VIEW-FORMAT-001.
"""

from __future__ import annotations

import json

import pytest

from tools.trace_viewer import trace_format as tf
from tools.trace_viewer import trace_viewer as tv

from conftest import run_trace_viewer


def record(sequence, timestamp_us, instance=1, kind=1, code=0, detail=""):
    return (sequence, timestamp_us, instance, kind, code, detail)


# ---------------------------------------------------------------------------
# TRACE-VIEW-WRAP-001: chronological reconstruction across ring wrap.
# ---------------------------------------------------------------------------

class TestWrapAroundReconstruction:
    RECORDS = [record(i, i * 100, detail="e%d" % i) for i in range(10)]

    def test_no_wrap_file_order_is_chronological(self):
        dump = tf.parse_dump(tf.build_dump(self.RECORDS, capacity=16))
        assert [r.sequence for r in dump.records] == list(range(10))
        assert dump.wrapped is False

    def test_wrap_reconstructs_chronological_order(self):
        """The highest-risk case: capacity 6, 10 records, slot-ordered copy.

        Physical slots 0..5 hold sequences [6, 7, 8, 9, 4, 5]; the viewer
        must reorder to [4, 5, 6, 7, 8, 9] from the sequence numbers alone.
        """
        dump = tf.parse_dump(tf.build_dump(self.RECORDS, capacity=6))
        assert [r.sequence for r in dump.records] == [4, 5, 6, 7, 8, 9]
        assert dump.wrapped is True
        # Deterministic: parsing twice yields the identical order.
        again = tf.parse_dump(tf.build_dump(self.RECORDS, capacity=6))
        assert [r.sequence for r in again.records] == \
            [r.sequence for r in dump.records]

    def test_full_but_never_wrapped_stays_in_order(self):
        dump = tf.parse_dump(tf.build_dump(self.RECORDS[:6], capacity=6))
        assert [r.sequence for r in dump.records] == [0, 1, 2, 3, 4, 5]
        assert dump.wrapped is False

    def test_wrap_with_gaps_from_dropped_records(self):
        """Sequence numbers carry the reconstruction even with dropped gaps."""
        sparse = [record(0, 0), record(1, 100), record(40, 400),
                  record(41, 500), record(42, 600)]
        dump = tf.parse_dump(tf.build_dump(sparse, capacity=3))
        assert [r.sequence for r in dump.records] == [40, 41, 42]
        assert dump.wrapped is True

    def test_single_record(self):
        dump = tf.parse_dump(tf.build_dump([record(7, 0)], capacity=4))
        assert len(dump.records) == 1 and dump.records[0].sequence == 7

    def test_empty_dump(self):
        dump = tf.parse_dump(tf.build_dump([], capacity=4))
        assert dump.records == [] and dump.wrapped is False

    def test_two_descents_are_refused(self):
        blob = tf.build_dump(self.RECORDS, capacity=6)
        body = bytearray(blob[tf.HEADER_SIZE:])
        record_size = 76
        first, second = body[:record_size], body[record_size:2 * record_size]
        corrupt = blob[:tf.HEADER_SIZE] + bytes(second + first +
                                                body[2 * record_size:])
        with pytest.raises(tf.TraceFormatError) as info:
            tf.parse_dump(corrupt)
        assert "descents" in str(info.value)

    def test_non_monotonic_same_sequence_is_refused(self):
        blob = tf.build_dump([record(5, 0), record(5, 1)], capacity=8)
        with pytest.raises(tf.TraceFormatError):
            tf.parse_dump(blob)

    def test_rotation_point_at_physical_end(self):
        """A wrapped ring whose copy happens to start at the rotation point
        (oldest record physically first) is chronological as stored."""
        blob = tf.build_dump(self.RECORDS, capacity=6)
        dump = tf.parse_dump(blob)
        assert dump.records[0].sequence == 4


# ---------------------------------------------------------------------------
# TRACE-VIEW-FORMAT-001: header and framing.
# ---------------------------------------------------------------------------

class TestFormat:
    def test_bad_magic(self):
        blob = bytearray(tf.build_dump([record(1, 0)]))
        blob[0:4] = b"XXXX"
        with pytest.raises(tf.TraceFormatError) as info:
            tf.parse_dump(bytes(blob))
        assert "magic" in str(info.value)

    def test_bad_version(self):
        blob = tf.build_dump([record(1, 0)], version=99)
        with pytest.raises(tf.TraceFormatError) as info:
            tf.parse_dump(blob)
        assert "version" in str(info.value)

    def test_count_exceeds_capacity(self):
        """A header claiming more records than the ring holds is corrupt."""
        import struct

        blob = bytearray(tf.build_dump([record(i, i) for i in range(5)],
                                       capacity=8))
        struct.pack_into("<I", blob, 12, 9)  # record_count > capacity
        with pytest.raises(tf.TraceFormatError) as info:
            tf.parse_dump(bytes(blob))
        assert "capacity" in str(info.value)

    def test_truncated_file(self):
        blob = tf.build_dump([record(i, i) for i in range(3)], capacity=8)
        with pytest.raises(tf.TraceFormatError):
            tf.parse_dump(blob[:-10])

    def test_header_too_small(self):
        with pytest.raises(tf.TraceFormatError):
            tf.parse_dump(b"\x00" * 8)

    def test_detail_is_truncated_and_nul_terminated(self):
        blob = tf.build_dump([record(1, 0, detail="x" * 200)], capacity=2,
                             detail_size=48)
        dump = tf.parse_dump(blob)
        assert len(dump.records[0].detail) == 47

    def test_kind_names_are_stable(self):
        assert tf.kind_name(1) == "transition"
        assert tf.kind_name(11) == "fault"
        assert tf.kind_name(tf.HAL_KIND_FAULT, 6) == "hal_fault:bus_off"
        assert tf.kind_name(tf.HAL_KIND_RX_FRAME) == "hal_rx_frame"
        assert tf.kind_name(999) == "kind_999"
        assert tf.FSM_KIND_NAMES[0] == "event"
        rec = tf.TraceRecord(sequence=1, timestamp_us=0, instance_id=1,
                             kind=11, code=0, detail="", physical_index=1)
        assert rec.is_fault() is True
        rec.kind = 2
        assert rec.is_fault() is False


# ---------------------------------------------------------------------------
# TRACE-VIEW-TIMELINE-001: human-readable timeline, JSON, dot.
# ---------------------------------------------------------------------------

SAMPLE = [
    record(0, 1000, 1, 9, 2, "i -> running"),
    record(1, 2000, 1, 1, 1, "(initial)->OFF"),
    record(2, 3000, 1, 1, 1, "OFF->ON"),
    record(3, 4000, 1, 11, 3, "CLUSTER_TIMEOUT/warning"),
    record(4, 5000, 2, tf.HAL_KIND_FAULT, 1, "vcan0"),
    record(5, 6000, 1, 1, 0, "ON->OFF"),
]


def test_timeline_text_rendering():
    dump = tf.parse_dump(tf.build_dump(SAMPLE, capacity=8))
    text = tv.render_timeline(dump)
    assert "ring capacity 8" in text
    assert "transition" in text
    assert "CLUSTER_TIMEOUT/warning" in text
    assert "+0.000000" in text and "+0.005000" in text
    # Absolute mode keeps the raw microsecond value.
    absolute = tv.render_timeline(dump, relative=False)
    assert "+0.001000" in absolute


def test_json_rendering():
    dump = tf.parse_dump(tf.build_dump(SAMPLE, capacity=8))
    document = json.loads(tv.render_json(dump))
    assert document["header"]["record_count"] == 6
    assert document["header"]["wrapped"] is False
    assert document["records"][0]["kind_name"] == "lifecycle"
    assert document["records"][2]["detail"] == "OFF->ON"


def test_dot_output_marks_faulted_edges_red():
    """The OFF->ON edge is followed by a fault for instance 1 before that
    instance's next transition: it must be red; ON->OFF must not."""
    dump = tf.parse_dump(tf.build_dump(SAMPLE, capacity=8))
    dot = tv.render_dot(dump)
    assert "digraph cancestry_trace" in dot
    assert '"(initial)" -> "OFF"' in dot
    assert '"ON" -> "OFF" [label=1];' in dot
    faulted = [line for line in dot.splitlines()
               if '"OFF" -> "ON"' in line]
    assert len(faulted) == 1 and "color=red" in faulted[0]
    # The HAL fault belongs to instance 2 which took no transitions: the
    # ON->OFF edge stays unmarked because of it.
    clean = [line for line in dot.splitlines() if '"ON" -> "OFF"' in line]
    assert "color=red" not in clean[0]
    assert sum(1 for line in dot.splitlines() if "color=red" in line) == 1


def test_dot_quote_escaping():
    dump = tf.parse_dump(tf.build_dump(
        [record(0, 0, 1, 1, 0, 'a"b->c\\d')], capacity=2))
    dot = tv.render_dot(dump)
    assert '\\"' in dot


def test_state_edges_ignore_untagged_faults():
    dump = tf.parse_dump(tf.build_dump(
        [record(0, 0, 5, 11, 1, "lonely fault")], capacity=2))
    edges, faulted, _ = tv._state_edges(dump)
    assert edges == {} and faulted == {}


# ---------------------------------------------------------------------------
# CLI behaviour.
# ---------------------------------------------------------------------------

def test_cli_text_json_dot_and_filters(tmp_path):
    dump_file = tmp_path / "capture.bin"
    dump_file.write_bytes(tf.build_dump(SAMPLE, capacity=8, dropped=2))

    text = run_trace_viewer(str(dump_file))
    assert text.returncode == 0
    assert "2 dropped before capture" in text.stdout

    json_out = run_trace_viewer(str(dump_file), "--json")
    assert json_out.returncode == 0
    assert json.loads(json_out.stdout)["header"]["dropped"] == 2

    dot_file = tmp_path / "trace.dot"
    dot_out = run_trace_viewer(str(dump_file), "--dot", str(dot_file))
    assert dot_out.returncode == 0
    assert dot_file.exists() and "digraph" in dot_file.read_text()

    filtered = run_trace_viewer(str(dump_file), "--kind", "transition")
    assert filtered.returncode == 0
    assert len(filtered.stdout.strip().splitlines()) == 3 + 2  # header rows

    by_instance = run_trace_viewer(str(dump_file), "--instance", "2")
    assert by_instance.returncode == 0
    assert "hal_fault" in by_instance.stdout
    assert "OFF->ON" not in by_instance.stdout

    absolute = run_trace_viewer(str(dump_file), "--absolute")
    assert "+0.001000" in absolute.stdout


def test_cli_rejects_corrupt_dump(tmp_path):
    bad = tmp_path / "bad.bin"
    bad.write_bytes(b"not a trace file at all")
    result = run_trace_viewer(str(bad))
    assert result.returncode == 1
    assert "trace_viewer" in result.stderr


def test_cli_rejects_missing_file(tmp_path):
    result = run_trace_viewer(str(tmp_path / "ghost.bin"))
    assert result.returncode == 2
