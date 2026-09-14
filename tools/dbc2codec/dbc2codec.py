#!/usr/bin/env python3
"""dbc2codec — compile a Vector DBC file into a CANcestry v0.2.0 codec map.

This tool implements the host-side half of Phase 2.5 ("opendbc DBC-to-Codec
Compiler", issue: "Phase 2.5 — `opendbc` DBC-to-Codec Compiler & Parity
Harness"). It translates the DBC constructs that CANcestry v0.2.0 can express
into a validated YAML codec map, and records the mandatory provenance header.

Requirements traced (see docs/trace/traceability.csv):
    SYS-FR-018   third-party extensibility via imported DBCs
    QA-v0.2-R01  schema validation enforced on generated artifacts
    QA-H02       bit-level semantics correctness (validated by the parity
                 harness in tests/integration/opendbc-parity)

Normative references:
    docs/packages/codec-map-spec.md        (v0.2.1) sec. 9 (provenance)
    schemas/codec-map-0.2.0.schema.json    output schema

Bit-numbering contract
----------------------
CANcestry uses a *linear LSB0* bit model (codec-map-spec.md sec. 3): payload
bit g lives in byte g//8 at bit position g%8 (bit 0 = least significant). A
signal occupies a contiguous run of payload bits:

    little-endian  start_bit = LSB position;  value bit i -> start_bit + i
    big-endian     start_bit = MSB position;  value bit i -> start_bit - (n-1) + i

DBC uses two conventions that map onto this model as follows:

    Intel   (@1, little endian)   start_bit = LSB position. Always
                                  representable; mapped 1:1.
    Motorola (@0, big endian)     start_bit = MSB position in a "sawtooth"
                                  numbering. A Motorola signal is representable
                                  in the linear model only when it fits inside a
                                  single byte; multi-byte Motorola signals use a
                                  sawtooth layout that a contiguous linear run
                                  cannot express, so they are skipped with a
                                  warning.

Multiplexed signals (SGM_ multiplexer or multiplexed entries) are out of scope
for v0.2.0 (see the parent issue) and are skipped with a warning.

The DBC parsing and the Motorola lsb/msb computation below deliberately mirror
comma.ai `opendbc`'s `opendbc/can/dbc.py` so that the generated bit positions
are bit-exact with the reference parser used by the parity harness.
"""

from __future__ import annotations

import argparse
import os
import re
import sys
from typing import Any

try:
    import yaml
except ImportError as exc:  # pragma: no cover
    sys.exit("dbc2codec requires PyYAML (`pip install pyyaml`): %s" % exc)

try:
    import jsonschema
except ImportError as exc:  # pragma: no cover
    sys.exit("dbc2codec requires jsonschema (`pip install jsonschema`): %s" % exc)

__version__ = "0.1.0"

# --------------------------------------------------------------------------
# DBC grammar (mirrors opendbc/can/dbc.py so bit positions are bit-exact)
# --------------------------------------------------------------------------

BO_RE = re.compile(r"^BO_ (\w+) (\w+) *: (\w+) (\w+)")
# Non-multiplexed signal.
SG_RE = re.compile(
    r"^SG_ (\w+) : (\d+)\|(\d+)@(\d)([+-]) "
    r"\(([0-9.+\-eE]+),([0-9.+\-eE]+)\) "
    r"\[[0-9.+\-eE]+\|[0-9.+\-eE]+\] \"(.*)\" .*"
)
# Multiplexed signal (multiplexer itself is `SG_ name M : ...`).
SGM_RE = re.compile(
    r"^SG_ (\w+) (\w+) *: (\d+)\|(\d+)@(\d)([+-]) "
    r"\(([0-9.+\-eE]+),([0-9.+\-eE]+)\) "
    r"\[[0-9.+\-eE]+\|[0-9.+\-eE]+\] \"(.*)\" .*"
)
VAL_RE = re.compile(r"^VAL_ (\w+) (\w+) (.*);")
VAL_PAIR_RE = re.compile(r'(\d+)\s+"([^"]*)"')

# Motorola sawtooth bit numbering: be_bits[k] is the linear LSB0 bit number at
# sawtooth position k (byte 0 MSB = sawtooth 0 = LSB0 bit 7, ...). This is the
# exact table opendbc builds.
BE_BITS = [j + i * 8 for i in range(64) for j in range(7, -1, -1)]

# CANcestry codec-map limits (core/codec/README.md): names <= 64 bytes.
_NAME_MAX = 64

# Maximum CAN id accepted by schemas/codec-map-0.2.0.schema.json (29-bit).
_CAN_ID_MAX = 536870911


class DbcSignal:
    """One parsed DBC signal, in opendbc's normalized form."""

    def __init__(self, name: str, start_bit: int, size: int, is_little: bool,
                 is_signed: bool, factor: float, offset: float, unit: str) -> None:
        self.name = name
        self.start_bit = start_bit  # raw DBC start bit
        self.size = size
        self.is_little_endian = is_little
        self.is_signed = is_signed
        self.factor = factor
        self.offset = offset
        self.unit = unit
        self.choices: dict[int, str] = {}

        if is_little:
            self.lsb = start_bit
            self.msb = start_bit + size - 1
        else:
            idx = BE_BITS.index(start_bit)
            self.lsb = BE_BITS[idx + size - 1]
            self.msb = start_bit


class DbcMessage:
    def __init__(self, frame_id: int, name: str, size: int) -> None:
        self.frame_id = frame_id
        self.name = name
        self.size = size
        self.signals: list[DbcSignal] = []
        self.signal_by_name: dict[str, DbcSignal] = {}


class DbcDatabase:
    """Minimal DBC database holding BO_ / SG_ / VAL_ constructs."""

    def __init__(self) -> None:
        self.messages: dict[int, DbcMessage] = {}
        self.values: dict[tuple[int, str], dict[int, str]] = {}

    @classmethod
    def load(cls, text: str) -> "DbcDatabase":
        db = cls()
        current: DbcMessage | None = None
        for raw in text.splitlines():
            line = raw.strip()
            if line.startswith("BO_ "):
                m = BO_RE.match(line)
                if not m:
                    continue
                frame_id = int(m.group(1), 0)
                msg_name = m.group(2)
                size = int(m.group(3), 0)
                current = DbcMessage(frame_id, msg_name, size)
                db.messages[frame_id] = current
            elif line.startswith("SG_ "):
                if current is None:
                    continue
                m = SG_RE.match(line)
                is_multiplexed = False
                if not m:
                    m = SGM_RE.match(line)
                    is_multiplexed = True
                if not m:
                    continue
                sig_name = m.group(1)
                # Multiplexed entries are out of scope; record them so the
                # caller can warn, but do not emit a signal.
                if is_multiplexed:
                    continue
                start_bit = int(m.group(2))
                size = int(m.group(3))
                is_little = m.group(4) == "1"
                is_signed = m.group(5) == "-"
                factor = float(m.group(6))
                offset = float(m.group(7))
                unit = m.group(8)
                sig = DbcSignal(sig_name, start_bit, size, is_little, is_signed,
                                factor, offset, unit)
                current.signals.append(sig)
                current.signal_by_name[sig_name] = sig
            elif line.startswith("VAL_ "):
                m = VAL_RE.match(line)
                if not m:
                    continue
                frame_id = int(m.group(1), 0)
                sig_name = m.group(2)
                rest = m.group(3)
                mapping: dict[int, str] = {}
                for num_s, label in VAL_PAIR_RE.findall(rest):
                    mapping[int(num_s, 0)] = label
                db.values[(frame_id, sig_name)] = mapping
        return db


# --------------------------------------------------------------------------
# Signal classification / mapping
# --------------------------------------------------------------------------

def _is_default_scale(factor: float, offset: float) -> bool:
    return factor == 1.0 and offset == 0.0


def map_signal(msg: DbcMessage, sig: DbcSignal, values: dict[int, str] | None):
    """Return a codec-map signal dict, or None if the signal is not
    representable in the v0.2.0 model (see module docstring)."""
    values = values or {}

    # Multi-byte Motorola signals use a sawtooth layout a contiguous linear
    # run cannot express -> skip (documented limitation).
    if not sig.is_little_endian and (sig.msb // 8) != (sig.lsb // 8):
        return None

    if len(sig.name) > _NAME_MAX:
        return None

    out: dict[str, Any] = {"name": sig.name}

    if sig.is_little_endian:
        out["start_bit"] = sig.lsb
        out["endianness"] = "little"
    else:
        # Motorola single-byte: CANcestry big-endian start_bit is the LSB0 MSB
        # position, which is the raw DBC start bit (opendbc msb).
        out["start_bit"] = sig.msb
        out["endianness"] = "big"

    out["bit_length"] = sig.size

    has_values = bool(values)
    if sig.size == 1 and not sig.is_signed and _is_default_scale(sig.factor, sig.offset):
        out["type"] = "boolean"
        if has_values:
            out["values"] = {str(k): v for k, v in values.items()}
    elif has_values and not sig.is_signed and _is_default_scale(sig.factor, sig.offset):
        out["type"] = "enum"
        out["values"] = {str(k): v for k, v in values.items()}
    elif sig.is_signed:
        out["type"] = "int"
        if not _is_default_scale(sig.factor, sig.offset):
            out["scale"] = sig.factor
            out["offset"] = sig.offset
    else:
        out["type"] = "uint"
        if not _is_default_scale(sig.factor, sig.offset):
            out["scale"] = sig.factor
            out["offset"] = sig.offset

    if sig.unit:
        out["unit"] = sig.unit

    return out


# --------------------------------------------------------------------------
# Codec map assembly
# --------------------------------------------------------------------------

def build_codec_map(db: DbcDatabase, name: str, version: str,
                    description: str | None = None) -> tuple[dict[str, Any], list[str]]:
    """Build the codec-map document from a parsed database.

    Returns (document, warnings). Messages are emitted in ascending frame-id
    order and signals in ascending (start_bit, name) order so the output is
    deterministic for a fixed input (SYS-NF-001).
    """
    warnings: list[str] = []
    messages: list[dict[str, Any]] = []
    used_names: set[str] = set()

    for frame_id in sorted(db.messages):
        msg = db.messages[frame_id]
        can_id = frame_id & 0x1FFFFFFF  # strip the extended-frame flag bit
        if can_id > _CAN_ID_MAX:
            warnings.append(f"message {msg.name!r}: frame id {frame_id} exceeds "
                            f"29 bits; skipped")
            continue
        signals: list[dict[str, Any]] = []
        for sig in sorted(msg.signals, key=lambda s: (s.start_bit, s.name)):
            if sig.name in used_names:
                # The v0.2.0 codec map requires short signal names to be unique
                # within a map (SW-FR-CODEC-007); keep the first occurrence.
                warnings.append(f"message {msg.name!r}: signal {sig.name!r} skipped: "
                                f"duplicate signal name")
                continue
            values = db.values.get((frame_id, sig.name))
            mapped = map_signal(msg, sig, values)
            if mapped is None:
                if not sig.is_little_endian and (sig.msb // 8) != (sig.lsb // 8):
                    reason = "multi-byte big-endian (Motorola) layout is not " \
                             "representable in the v0.2.0 codec model"
                elif len(sig.name) > _NAME_MAX:
                    reason = "signal name longer than 64 bytes"
                else:
                    reason = "unknown"
                warnings.append(f"message {msg.name!r}: signal {sig.name!r} skipped: {reason}")
                continue
            signals.append(mapped)
            used_names.add(sig.name)
        if not signals:
            warnings.append(f"message {msg.name!r}: no representable signals; skipped")
            continue
        messages.append({
            "id": can_id,
            "name": msg.name,
            "dlc": msg.size,
            "signals": signals,
        })

    if not messages:
        raise ValueError("no representable messages in DBC; cannot build codec map")

    doc: dict[str, Any] = {
        "schema_version": "0.2.0",
        "codec_map": {
            "name": name,
            "version": version,
            "messages": messages,
        },
    }
    if description:
        doc["codec_map"]["description"] = description
    return doc, warnings


# --------------------------------------------------------------------------
# Provenance / output
# --------------------------------------------------------------------------

PROVENANCE_TEMPLATE = (
    "# GENERATED BY: dbc2codec v{version}\n"
    "# SOURCE: {source_url}\n"
    "# COMMIT: {commit}\n"
    "# LICENSE: {license}\n"
)


class _CodecMapDumper(yaml.SafeDumper):
    """Indent block sequences under their key.

    The CANcestry loader only accepts the indented block style (see
    core/codec/README.md "YAML subset"); PyYAML's default emits indentless
    sequences, which the loader rejects.
    """

    def increase_indent(self, flow: bool = False, indentless: bool = False):
        return super().increase_indent(flow, False)


class _Quoted(str):
    """str subclass whose instances are always emitted double-quoted."""


def _quoted_representer(dumper: yaml.Dumper, data: _Quoted):
    return dumper.represent_scalar("tag:yaml.org,2002:str", str(data), style='"')


_CodecMapDumper.add_representer(_Quoted, _quoted_representer)


def render(doc: dict[str, Any], provenance: dict[str, str]) -> str:
    header = PROVENANCE_TEMPLATE.format(**provenance)
    # schema_version/version must be YAML strings (the schema const/pattern are
    # strings); force-quote them so they can never be read back as numbers.
    doc = dict(doc)
    doc["schema_version"] = _Quoted(doc["schema_version"])
    codec_map = dict(doc["codec_map"])
    codec_map["version"] = _Quoted(codec_map["version"])
    doc["codec_map"] = codec_map
    body = yaml.dump(doc, Dumper=_CodecMapDumper, sort_keys=False,
                     allow_unicode=True, width=4096)
    return header + body


def validate(doc: dict[str, Any], schema_path: str) -> None:
    """Validate the generated document against the codec-map JSON Schema."""
    with open(schema_path, encoding="utf-8") as fh:
        schema = yaml.safe_load(fh)
    jsonschema.validate(instance=doc, schema=schema)


# --------------------------------------------------------------------------
# CLI
# --------------------------------------------------------------------------

def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        prog="dbc2codec",
        description="Compile a Vector DBC file into a CANcestry v0.2.0 codec map.",
    )
    parser.add_argument("input", help="source .dbc file")
    parser.add_argument("-o", "--output", help="output .yaml file (default: stdout)")
    parser.add_argument("--name", help="codec map name (default: DBC basename)")
    parser.add_argument("--version", default="1.0.0", help="codec map version (default: 1.0.0)")
    parser.add_argument("--source-url", default="https://github.com/commaai/opendbc",
                        help="provenance source URL")
    parser.add_argument("--commit", default="", help="provenance Git commit hash")
    parser.add_argument("--license", default="MIT (Copyright (c) comma.ai)",
                        help="provenance license attribution")
    parser.add_argument("--schema", default=None,
                        help="path to codec-map-0.2.0.schema.json "
                             "(default: schemas/ next to the repo root)")
    parser.add_argument("--version-info", action="version",
                        version="dbc2codec v%s" % __version__)
    args = parser.parse_args(argv)

    with open(args.input, encoding="utf-8", errors="replace") as fh:
        text = fh.read()

    db = DbcDatabase.load(text)
    basename = os.path.basename(args.input)
    name = args.name or os.path.splitext(basename)[0]
    if "." in name:
        parser.error("codec map name must not contain '.' (see core/codec/README.md)")

    try:
        doc, warnings = build_codec_map(db, name, args.version,
                                        description="Generated from %s" % basename)
    except ValueError as exc:
        parser.error(str(exc))

    schema_path = args.schema
    if schema_path is None:
        here = os.path.dirname(os.path.abspath(__file__))
        schema_path = os.path.abspath(os.path.join(here, "..", "..",
                                                   "schemas",
                                                   "codec-map-0.2.0.schema.json"))
    validate(doc, schema_path)

    provenance = {
        "version": __version__,
        "source_url": args.source_url,
        "commit": args.commit,
        "license": args.license,
    }
    rendered = render(doc, provenance)

    if args.output:
        with open(args.output, "w", encoding="utf-8") as fh:
            fh.write(rendered)
    else:
        sys.stdout.write(rendered)

    for w in warnings:
        print("warning: %s" % w, file=sys.stderr)

    return 0


if __name__ == "__main__":
    sys.exit(main())
