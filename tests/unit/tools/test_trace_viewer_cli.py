"""In-process CLI tests for trace_viewer (issue #22).

Running ``main()`` directly pins the exit-code contract (0 ok, 1 corrupt
dump, 2 usage/IO) and covers the CLI paths for the 100% coverage
requirement. The subprocess-level behaviour is covered in
test_trace_viewer.py.

Requirements traced: SW-FR-TOOL-005..007, SW-FR-TOOL-010.
Test ids: TRACE-VIEW-CLI-001..004.
"""

from __future__ import annotations

import json
import struct

import pytest

from tools.trace_viewer import trace_format as tf
from tools.trace_viewer import trace_viewer as tv


def record(sequence, timestamp_us, instance=1, kind=1, code=0, detail=""):
    return (sequence, timestamp_us, instance, kind, code, detail)


SAMPLE = [
    record(0, 1000, 1, 9, 2, "i -> running"),
    record(1, 2000, 1, 1, 1, "(initial)->OFF"),
    record(2, 3000, 1, 1, 1, "OFF->ON"),
    record(3, 4000, 1, 11, 3, "CLUSTER_TIMEOUT/warning"),
    record(4, 5000, 2, tf.HAL_KIND_FAULT, 1, "vcan0"),
    record(5, 6000, 1, 1, 0, "ON->OFF"),
]


@pytest.fixture()
def dump_file(tmp_path):
    path = tmp_path / "capture.bin"
    path.write_bytes(tf.build_dump(SAMPLE, capacity=8, dropped=2))
    return path


def test_text_timeline_by_default(dump_file, capsys):
    assert tv.main([str(dump_file)]) == 0
    out = capsys.readouterr().out
    assert "6 record(s)" in out and "2 dropped before capture" in out
    assert "+0.000000" in out and "transition" in out


def test_json_and_absolute_and_filters(dump_file, capsys):
    assert tv.main([str(dump_file), "--json"]) == 0
    document = json.loads(capsys.readouterr().out)
    assert document["header"]["dropped"] == 2

    assert tv.main([str(dump_file), "--absolute"]) == 0
    assert "+0.001000" in capsys.readouterr().out

    assert tv.main([str(dump_file), "--kind", "transition",
                    "--kind", "fault"]) == 0
    out = capsys.readouterr().out
    assert "OFF->ON" in out and "lifecycle" not in out

    assert tv.main([str(dump_file), "--instance", "2"]) == 0
    assert "hal_fault" in capsys.readouterr().out

    both = tv.main([str(dump_file), "--instance", "2", "--json"])
    assert both == 0
    assert len(json.loads(capsys.readouterr().out)["records"]) == 1


def test_dot_output(dump_file, tmp_path, capsys):
    target = tmp_path / "trace.dot"
    assert tv.main([str(dump_file), "--dot", str(target)]) == 0
    assert "trace_viewer: wrote %s" % target in capsys.readouterr().err
    assert "digraph cancestry_trace" in target.read_text(encoding="utf-8")
    # --json --dot combine: both outputs are produced.
    target2 = tmp_path / "trace2.dot"
    assert tv.main([str(dump_file), "--json", "--dot", str(target2)]) == 0
    assert "digraph" in target2.read_text(encoding="utf-8")


def test_corrupt_dump_exits_one(tmp_path, capsys):
    bad = tmp_path / "bad.bin"
    bad.write_bytes(b"XXXX" + b"\x00" * 28)
    assert tv.main([str(bad)]) == 1
    assert "trace_viewer" in capsys.readouterr().err


def test_missing_file_exits_two(tmp_path, capsys):
    assert tv.main([str(tmp_path / "ghost.bin")]) == 2
    assert "No such file" in capsys.readouterr().err


def test_usage_error_exits_two():
    with pytest.raises(SystemExit) as info:
        tv.main(["--bogus"])
    assert info.value.code == 2


def test_wrap_around_timeline_via_cli(tmp_path, capsys):
    """A wrapped dump parsed end to end prints chronological times."""
    records = [record(i, i * 100, 1, 2, 0, "s%d->s%d" % (i, i + 1))
               for i in range(10)]
    path = tmp_path / "wrap.bin"
    path.write_bytes(tf.build_dump(records, capacity=6))
    assert tv.main([str(path)]) == 0
    out = capsys.readouterr().out
    assert "ring wrapped" in out
    # Chronological: the first printed event is sequence 4 (time 400us).
    assert "+0.000400" in out


def test_dot_write_error_exits_two(dump_file, tmp_path, capsys):
    blocker = tmp_path / "blocker.txt"
    blocker.write_text("a file, not a directory", encoding="utf-8")
    target = blocker / "trace.dot"
    assert tv.main([str(dump_file), "--dot", str(target)]) == 2
    assert "trace_viewer: error" in capsys.readouterr().err


def test_parse_header_rejects_bad_framing(tmp_path):
    good = tf.build_dump(SAMPLE, capacity=8)
    small_header = bytearray(good)
    struct.pack_into("<H", small_header, 6, 16)  # header_size
    with pytest.raises(tf.TraceFormatError) as info:
        tf.parse_header(bytes(small_header))
    assert "header size" in str(info.value)

    small_record = bytearray(good)
    struct.pack_into("<I", small_record, 8, 20)  # record_size
    with pytest.raises(tf.TraceFormatError) as info:
        tf.parse_header(bytes(small_record))
    assert "record size" in str(info.value)


def test_kind_name_fallback_for_unknown_hal_code():
    assert tf.kind_name(tf.HAL_KIND_FAULT, 99) == "hal_fault:code_99"
