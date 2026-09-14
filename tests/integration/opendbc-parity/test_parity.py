"""Bit-exact parity between the CANcestry C decoder and comma.ai `opendbc`.

For every representable signal in the selected vehicle DBCs, this test feeds
identical random CAN frames to both the CANcestry C decoder (via `codec_cli`)
and opendbc's reference parser (`opendbc.can.parser.get_raw_value` +
`opendbc.can.dbc.DBC`) and asserts the physical values agree within a defined
tolerance (1e-6 absolute, plus a small relative term for large magnitudes).

Requirements traced:
    SYS-FR-018  third-party extensibility via imported DBCs
    SYS-NF-008  testability via an independent reference parser
    QA-H02      bit-level semantics correctness validated against opendbc
"""

from __future__ import annotations

import math
import random
import subprocess
from pathlib import Path

import yaml
from opendbc.can.dbc import DBC, Signal
from opendbc.can.parser import get_raw_value

# Deterministic frame generation (SYS-NF-001: same input -> same output).
SEED = 0xC0DEC0DE
FRAMES_PER_MESSAGE = 8
# Defined float tolerance from the parent issue (QA-H02).
ABS_TOL = 1e-6
REL_TOL = 1e-9


def opendbc_physical(data: bytes, sig: Signal) -> float:
    """Physical value computed exactly as opendbc's CANParser does."""
    raw = get_raw_value(data, sig)
    if sig.is_signed:
        raw -= ((raw >> (sig.size - 1)) & 0x1) * (1 << sig.size)
    return raw * sig.factor + sig.offset


def decode_with_c(codec_cli: Path, yaml_path: Path, frames: list[tuple[int, bytes]]):
    """Decode a batch of frames through the CANcestry CLI.

    Returns {can_id: [ {signal_name: float_value}, ... ]} preserving order.
    """
    stdin = "".join("%d %s\n" % (cid, data.hex()) for cid, data in frames)
    proc = subprocess.run(
        [str(codec_cli), str(yaml_path)], input=stdin, capture_output=True, text=True
    )
    assert proc.returncode == 0, proc.stderr
    out: dict[int, list[dict[str, float]]] = {}
    for line in proc.stdout.splitlines():
        parts = line.split()
        can_id = int(parts[0])
        vals = {}
        for kv in parts[1:]:
            name, _, value = kv.partition("=")
            vals[name] = float(value)
        out.setdefault(can_id, []).append(vals)
    return out


def test_decode_parity(dbc_paths, run_dbc2codec, codec_cli: Path):
    checked = 0
    for name, dbc_path in dbc_paths:
        yaml_path, _warnings = run_dbc2codec(dbc_path, name)
        with open(yaml_path, encoding="utf-8") as fh:
            doc = yaml.safe_load(fh)
        codec_map = doc["codec_map"]

        opendbc_db = DBC(name)
        # Map the codec-map (29-bit, flag-stripped) id back to opendbc's raw
        # frame id so we can look the message up in the reference database.
        opendbc_by_id = {m.address & 0x1FFFFFFF: m for m in opendbc_db.msgs.values()}

        rng = random.Random(SEED)
        frames: list[tuple[int, bytes]] = []
        expected: dict[int, list[dict[str, float]]] = {}

        for msg in codec_map["messages"]:
            can_id = msg["id"]
            dlc = msg["dlc"]
            opendbc_msg = opendbc_by_id[can_id]
            sig_names = [s["name"] for s in msg["signals"]]
            for _ in range(FRAMES_PER_MESSAGE):
                data = bytes(rng.getrandbits(8) for _ in range(dlc))
                frames.append((can_id, data))
                row = {}
                for sig_name in sig_names:
                    sig = opendbc_msg.sigs[sig_name]
                    row[sig_name] = opendbc_physical(data, sig)
                expected.setdefault(can_id, []).append(row)

        actual = decode_with_c(codec_cli, yaml_path, frames)

        for can_id, rows in expected.items():
            assert can_id in actual, "missing decoded frame for id %d in %s" % (can_id, name)
            got_rows = actual[can_id]
            assert len(got_rows) == len(rows)
            for exp_row, got_row in zip(rows, got_rows):
                for sig_name, exp_val in exp_row.items():
                    got_val = got_row[sig_name]
                    assert math.isclose(got_val, exp_val, rel_tol=REL_TOL, abs_tol=ABS_TOL), (
                        "%s: signal %r in message 0x%x mismatch: got %r, expected %r"
                        % (name, sig_name, can_id, got_val, exp_val)
                    )
                    checked += 1

    assert checked > 0, "no signals were compared; is the DBC selection correct?"
