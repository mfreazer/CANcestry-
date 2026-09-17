# CANcestry Codec Map Specification

Version: 0.3.0

## 1. Purpose

A codec map defines how CAN frames map to named signals.

## 2. Canonical Signal Names

The canonical signal name is:

    <codec_map_name>.<signal_name>

Short names are allowed only when unambiguous.

If two codec maps define the same short signal name, the short name becomes ambiguous and shall be rejected unless fully qualified.

## 3. Byte and Bit Numbering

The canonical bit array is linear LSB0:

    data[0] bit 0 = global bit 0
    data[0] bit 7 = global bit 7
    data[1] bit 0 = global bit 8
    data[1] bit 7 = global bit 15

## 4. Little-Endian Signals

For endianness little, layout must be `contiguous` (the default):

- start_bit is the position of the signal LSB.
- Signal bit i maps to payload bit start_bit + i.

Layout `sawtooth` is invalid for little-endian signals and shall be rejected.

## 5. Big-Endian Signals

For endianness big with layout `contiguous` (the default):

- start_bit is the position of the signal MSB.
- Signal bit i, where i = 0 is LSB, maps to payload bit:

      start_bit - (bit_length - 1) + i

This is the historic v0.2.0 model: the signal occupies a single contiguous
run `[first_bit, last_bit]` where `first_bit = start_bit - (bit_length - 1)`.

## 5.1 Big-Endian Sawtooth Signals

For endianness big with layout `sawtooth`:

- start_bit is the DBC Motorola MSB position (the same integer that appears
  after `SG_` in a DBC file and that `opendbc` calls `msb`).
- The signal's payload bits are the Motorola sawtooth ordering. Let

      be_idx(p) = (p >> 3) * 8 + (7 - (p & 7))
      be_bit(k) = (k >> 3) * 8 + (7 - (k & 7))

  Then payload bit for sawtooth position k (0-indexed) is

      payload_k = be_bit(be_idx(start_bit) + k)

  Raw bit `bit_length - 1 - k` (i.e. the most significant bits first) maps to
  `payload_k`. This is bit-exact with `opendbc`'s `get_raw_value` for
  Motorola signals (see `tools/dbc2codec/dbc2codec.py` `BE_BITS`).

- In particular, a single-byte sawtooth signal occupies the same set as a
  contiguous signal with the same start_bit and bit_length; both decodings are
  bit-identical. Multi-byte sawtooth signals span bytes with the sawtooth
  pattern, e.g. start 7 length 16 -> payload bits
  `[7,6,5,4,3,2,1,0,15,14,13,12,11,10,9,8]` giving raw `data[0]<<8|data[1]`.

The layout field defaults to `contiguous` when omitted, so every v0.2.0 codec
map remains valid. New maps that need multi-byte Motorola signals should set
`layout: sawtooth` on those signals. `bit_layout` is accepted as an alias for
`layout`.

Schema: `schemas/codec-map-0.2.0.schema.json` (extended) and
`schemas/codec-map-0.3.0.schema.json` both allow the optional `layout` /
`bit_layout` enum `["contiguous","sawtooth"]`. Loaders shall accept
`schema_version` `"0.2.0"` and `"0.3.0"`; other versions are rejected.

## 6. Signed Signals

Signed integer signals use two's complement.

For bit_length N, if bit N-1 is set, the raw integer is sign-extended to 64 bits.

## 7. Scaling

Physical value:

    physical = raw * scale + offset

Encoding:

    raw = round((physical - offset) / scale)

Rounding is round-to-nearest, ties away from zero.

## 8. Invalid Frames

If a received frame is too short for a declared signal:

- the message is dropped,
- a codec warning is raised,
- no signal is updated.

For contiguous signals the needed payload bits are `[first_bit, last_bit]`;
for sawtooth signals the needed bits are the sawtooth set described in
section 5.1. Extra bytes are ignored.

## 8.1 CAN FD Payloads (Phase 7, issue #20)

A codec map may declare itself a CAN FD map with the optional `codec_map`
flag:

```yaml
codec_map:
  name: radar
  version: 1.0.0
  can_fd: true
  messages:
    - id: 0x1F0
      name: RadarCluster
      dlc: 64
      signals:
        - name: RadarTail
          start_bit: 504
          bit_length: 8
          type: uint
          endianness: little
```

The flag switches two vocabularies, and nothing else:

| | classic map (`can_fd` absent or false) | CAN FD map (`can_fd: true`) |
|---|---|---|
| `dlc` | 0..8 | 0-8, 12, 16, 20, 24, 32, 48, 64 (the ISO 11898-1 DLC codes) |
| `start_bit` | 0..63 | 0..511 |
| signal width | at most 64 bits | at most 64 bits (unchanged) |
| bit model | section 3 LSB0 | section 3 LSB0 (unchanged) |

Rules:

- `can_fd` is a `codec_map` field, not a message field; a message-level
  `can_fd` is an unknown key and is rejected.
- A classic map is validated exactly as it was before Phase 7, so existing
  documents are unaffected (SW-FR-CANFD-001).
- Payload lengths 9, 10 and 11 bytes are not representable on either bus, so
  the codec rejects them as an argument error rather than accepting a frame no
  controller can produce (SW-FR-CANFD-002).
- A map that declares `can_fd: true` is refused at load time, with
  `CANCESTRY_CODEC_ERR_UNSUPPORTED`, when the platform capabilities passed to
  the loader do not include CAN FD. The mismatch is a load-time error, never a
  runtime surprise (SW-FR-CANFD-004, "schema is law").
- The normative JSON Schema for both variants is
  [`schemas/codec-map-0.3.0.schema.json`](../../schemas/codec-map-0.3.0.schema.json)
  (`classic_message` / `fd_message`).

## 9. Provenance and Attribution (Phase 2.5+)

Codec maps generated from external sources (e.g., comma.ai `opendbc`) MUST include a machine-readable provenance header in the YAML file comments or metadata. 

The header must contain:
1. The source repository URL.
2. The specific Git commit hash used for generation.
3. The original license attribution (e.g., MIT License).

Example:
```yaml
# GENERATED BY: dbc2codec v0.2.0
# SOURCE: https://github.com/commaai/opendbc
# COMMIT: a1b2c3d4e5f6...
# LICENSE: MIT (Copyright (c) comma.ai)
schema_version: "0.2.0"
codec_map:
  name: opendbc_honda_civic_2020
  ...
```
