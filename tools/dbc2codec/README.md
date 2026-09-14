# dbc2codec

Host-side compiler that translates a Vector `.dbc` file into a CANcestry
v0.2.0 codec map (YAML). It implements Phase 2.5 (issue: "Phase 2.5 —
`opendbc` DBC-to-Codec Compiler & Parity Harness").

## Usage

```console
$ python tools/dbc2codec/dbc2codec.py input.dbc -o output.yaml \
    --source-url https://github.com/commaai/opendbc \
    --commit a1b2c3d4e5f6 \
    --license "MIT (Copyright (c) comma.ai)"
```

`--commit`, `--source-url` and `--license` populate the mandatory provenance
header (`docs/packages/codec-map-spec.md` §9). The generated document is
validated against `schemas/codec-map-0.2.0.schema.json` before it is written;
validation failure aborts with a non-zero exit status.

Dependencies: `PyYAML` and `jsonschema` (see `requirements.txt`).

## What it does

For each DBC message (`BO_`) it emits a codec-map message; for each signal
(`SG_`) it emits a codec-map signal, mapping:

| DBC construct | Codec-map field |
|---|---|
| Intel (`@1`, little-endian) start bit | `start_bit` (LSB position), `endianness: little` |
| Motorola (`@0`, big-endian) start bit | `start_bit` (MSB position), `endianness: big` |
| `SG_ ... -` (signed) | `type: int` (two's complement) |
| `(factor, offset)` | `scale`, `offset` |
| `VAL_` value table | `type: enum` / `type: boolean` + `values` |
| 1-bit unsigned flag | `type: boolean` |
| `[min\|max]` | (not emitted — see below) |

The DBC parser and the Motorola `lsb`/`msb` computation mirror comma.ai
`opendbc`'s `opendbc/can/dbc.py` exactly, so the generated bit positions are
bit-exact with the reference parser used by the parity harness.

## Documented limitations (signals that are skipped, with a warning)

1. **Multiplexed signals** (`SG_ name M : ...` and `SG_ name mux : ...`) —
   out of scope for v0.2.0 (see the parent issue).
2. **Multi-byte Motorola signals** — a Motorola signal that spans more than
   one byte uses a "sawtooth" bit layout that a contiguous linear LSB0 run
   cannot express (the v0.2.0 codec model stores a signal as a single
   contiguous bit run; see `core/codec/README.md` § "Bit model"). Single-byte
   Motorola signals are representable and are emitted.
3. **Duplicate signal names** — the v0.2.0 codec map requires short signal
   names to be unique within a map (`SW-FR-CODEC-007`); the first occurrence is
   kept and later ones are skipped with a warning.
4. **Message frame ids > 29 bits** — skipped (the schema caps `id` at
   2^29-1).

Signals are emitted in ascending `(start_bit, name)` order and messages in
ascending frame-id order, so output is deterministic for a fixed input
(`SYS-NF-001`).
