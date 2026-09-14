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
bit g lives in byte g//8 at bit position g%8 (bit 0 = least significant).

Two layouts are supported (codec-map-spec.md secs. 4-5.1):

    little-endian  start_bit = LSB position;  value bit i -> start_bit + i
                   (layout contiguous, the only valid layout for little)
    big-endian contiguous  start_bit = MSB position;
                           value bit i -> start_bit - (n-1) + i
    big-endian sawtooth    start_bit = MSB position in Motorola sawtooth
                           numbering; payload bits are BE_BITS[be_idx(start)..
                           be_idx(start)+n-1] with the first element carrying
                           the MSB of the raw value, mirroring opendbc's
                           get_raw_value (spec sec. 5.1).

DBC uses two conventions that map onto this model as follows:

    Intel   (@1, little endian)   start_bit = LSB position; always
                                  representable; mapped 1:1 with layout
                                  contiguous.
    Motorola (@0, big endian)     start_bit = MSB position in sawtooth
                                  numbering. Single-byte Motorola signals are
                                  representable with layout contiguous (the
                                  sawtooth set equals the contiguous run), but
                                  multi-byte Motorola signals require layout
                                  sawtooth and are now emitted with
                                  layout: sawtooth instead of being skipped.

Multiplexed signals (SGM_ multiplexer or multiplexed entries) are out of scope
for v0.2.0/v0.3.0 (see the parent issue) and are skipped with a warning.

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

__version__ = "0.2.0"

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
_UNIT_MAX = 32
_LABEL_MAX = 64
_PAYLOAD_MAX_BITS = 64
_DLC_MAX = 8

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
    representable (see module docstring).

    Motorola multi-byte signals are now representable with layout sawtooth
    (codec-map-spec.md sec. 5.1); only the multiplexed and overlong-name cases
    remain unrepresentable. Signals that would be rejected by the CANcestry
    loader (sawtooth exceeding the 64-bit payload, unit/label length, etc.)
    are also treated as unrepresentable so the generated artifact always
    loads.
    """
    values = values or {}

    if len(sig.name) > _NAME_MAX:
        return None

    # Pre-validate loader limits that are not expressed in the JSON Schema.
    # Sawtooth exceeding the 64-bit global payload is a load error.
    # Compute the would-be layout to decide.
    is_saw = not sig.is_little_endian and (sig.msb // 8) != (sig.lsb // 8)
    # DLC-based fit: signal bits must fit within the message's payload size.
    # This prevents the decoder from reporting FRAME_TOO_SHORT and the CLI
    # from skipping the entire frame (see tests/integration/opendbc-parity/codec_cli.c).
    dlc_bits = msg.size * 8
    if is_saw:
        be_idx = (sig.msb >> 3) * 8 + (7 - (sig.msb & 7))
        if be_idx + sig.size > _PAYLOAD_MAX_BITS:
            return None
        # Check that all sawtooth payload bits are within DLC
        # (equivalent to max_p < dlc_bits)
        # Compute max_p via range logic; faster to check via be mapping
        max_p = 0
        min_p = 64
        for k in range(sig.size):
            p = BE_BITS[be_idx + k]
            if p < min_p:
                min_p = p
            if p > max_p:
                max_p = p
        if max_p >= dlc_bits:
            return None
    elif sig.is_little_endian:
        if sig.lsb + sig.size > _PAYLOAD_MAX_BITS:
            return None
        if sig.lsb + sig.size > dlc_bits:
            return None
    else:
        if sig.msb < sig.size - 1:
            return None
        # Contiguous big: bits are [msb - size +1, msb]
        first = sig.msb - sig.size + 1
        if sig.msb >= dlc_bits or first >= dlc_bits:
            # If msb itself is outside DLC, signal exceeds
            return None

    # Unit/label length limits are loader errors (max 32 / 64).
    if sig.unit and len(sig.unit) > _UNIT_MAX:
        # Drop the unit rather than the whole signal; the physical value is
        # still representable.
        sig_unit = None
    else:
        sig_unit = sig.unit if sig.unit else None

    if values:
        for label in values.values():
            if len(label) > _LABEL_MAX:
                return None
        # Also check that enum keys fit in bit_length (loader checks)
        if sig.size < 64:
            limit = 1 << sig.size
            for k in values.keys():
                if k >= limit:
                    return None

    out: dict[str, Any] = {"name": sig.name}

    sawtooth = is_saw

    if sig.is_little_endian:
        out["start_bit"] = sig.lsb
        out["endianness"] = "little"
    else:
        # Motorola: CANcestry big-endian start_bit is the LSB0 MSB position,
        # which is the raw DBC start bit (opendbc msb). The layout field
        # distinguishes the contiguous vs sawtooth interpretation.
        out["start_bit"] = sig.msb
        out["endianness"] = "big"
        if sawtooth:
            out["layout"] = "sawtooth"

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

    if sig_unit:
        out["unit"] = sig_unit

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
        if msg.size < 0 or msg.size > _DLC_MAX:
            warnings.append(f"message {msg.name!r}: DLC {msg.size} out of range (0..{_DLC_MAX}); skipped")
            continue
        # VECTOR__INDEPENDENT_SIG_MSG is a DBC placeholder for unassigned
        # signals (size 0, signals all sharing start bit 0). It has no CAN
        # payload and is not representable as a codec-map message.
        if msg.size == 0:
            warnings.append(f"message {msg.name!r}: DLC 0 (placeholder); skipped")
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
            # Preserve unit truncation info for warning
            pre_unit = sig.unit
            mapped = map_signal(msg, sig, values)
            if mapped is None:
                if len(sig.name) > _NAME_MAX:
                    reason = "signal name longer than 64 bytes"
                elif sig.unit and len(sig.unit) > _UNIT_MAX:
                    # map_signal drops the unit but still returns a signal;
                    # this branch is for other unrepresentable cases
                    reason = "unknown"
                elif values and any(len(v) > _LABEL_MAX for v in values.values()):
                    reason = "value label longer than 64 bytes"
                elif not sig.is_little_endian and (sig.msb // 8) != (sig.lsb // 8):
                    be_idx = (sig.msb >> 3) * 8 + (7 - (sig.msb & 7))
                    if be_idx + sig.size > _PAYLOAD_MAX_BITS:
                        reason = "sawtooth exceeds 64-bit payload"
                    else:
                        reason = "unknown"
                elif sig.is_little_endian:
                    if sig.lsb + sig.size > _PAYLOAD_MAX_BITS:
                        reason = "little-endian exceeds 64-bit payload"
                    else:
                        reason = "unknown"
                else:
                    if sig.msb < sig.size - 1:
                        reason = "big-endian extends below payload bit 0"
                    else:
                        reason = "unknown"
                warnings.append(f"message {msg.name!r}: signal {sig.name!r} skipped: {reason}")
                continue
            # Emit a warning when the unit was dropped due to length
            if pre_unit and len(pre_unit) > _UNIT_MAX and "unit" not in mapped:
                warnings.append(f"message {msg.name!r}: signal {sig.name!r}: unit truncated (dropped) as it exceeds {_UNIT_MAX} bytes")
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
