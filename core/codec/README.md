# CANcestry codec engine

Portable decode/encode engine for the v0.2.0 codec map schema, implementing
`docs/packages/codec-map-spec.md` (v0.2.1) bit semantics exactly.

## Libraries

| Target | Contents | Allocates? |
|---|---|---|
| `cancestry_codec` | types, decoder, encoder, namespace | never |
| `cancestry_codec_loader` | YAML subset parser + schema validation | load time only |

A loaded `cancestry_codec_map_t` is one heap block that owns the map struct,
all message/signal arrays, and all strings. The runtime library only reads
borrowed pointers into that block, which is why it can be allocation-free
(verified by `ci/check_no_alloc.py` on the built archive in CI).

## Bit model (normative)

Canonical LSB0 linear bit numbering: `data[0] bit 0` is global bit 0,
`data[1] bit 0` is global bit 8, and so on (spec section 3).

- **little-endian**: `start_bit` is the LSB position; signal bit `i` maps to
  payload bit `start_bit + i`.
- **big-endian**: `start_bit` is the MSB position; signal bit `i` maps to
  payload bit `start_bit - (bit_length - 1) + i`.

Both endiannesses therefore occupy a contiguous payload bit run
`[first_bit, last_bit]`, and extraction is identical for both. Signed signals
are two's complement and sign-extend to 64 bits (spec section 6). Scaling is
`physical = raw * scale + offset`; encoding rounds to nearest, ties away from
zero, and is the exact inverse of decoding.

## Decoded value kinds

| Signal type | Unscaled | Scaled (`scale != 1` or `offset != 0`) |
|---|---|---|
| uint | `UINT` (raw) | `REAL` (physical) |
| int | `INT` (sign-extended raw) | `REAL` (physical) |
| boolean | `BOOL`, with label from `values` when mapped | n/a (scale/offset ignored) |
| enum | `INT` (raw), with label from `values` when mapped | n/a (scale/offset ignored) |

`cancestry_decoded_signal_t::raw` always holds the zero-extended bit pattern,
so decode → encode round-trips every bit exactly.

## Encode value kinds

- uint: `UINT` always, `REAL` when scaled.
- int: `INT` always, `REAL` when scaled.
- boolean: `BOOL`, or `INT`/`UINT` equal to 0 or 1.
- enum: `INT` or `UINT` holding the raw value.

Unscaled uint/int encoding takes an exact integer path so the full 64-bit
range round-trips (double cannot represent integers above 2^53).

## Interpretations and decisions

The v0.2.0 schema and spec leave some corners undefined; the choices made
here are normative for this implementation:

- **`strict`** defaults to `true`. On encode, a physical value outside the
  declared `min`/`max` fails with `CANCESTRY_CODEC_ERR_RANGE`; with
  `strict: false` it is clamped to the range and counted as a
  `value_clamped` warning. `min`/`max` apply to uint/int signals only.
- **`scale`/`offset` on boolean and enum signals** are accepted by the loader
  (the schema permits them) but ignored, because the value mapping keys are
  raw values. `scale: 0` on uint/int is rejected: it makes encoding undefined.
- **Value mapping keys** must be decimal integers fitting the signal's bit
  width. Enum decoding of an unmapped raw value succeeds with a NULL label
  and counts an `enum_unknown` warning.
- **Duplicate definitions are conflicts** (`CANCESTRY_CODEC_ERR_CONFLICT`,
  SW-FR-CODEC-007): two messages with the same id in one map, or one short
  signal name defined twice in one map. Two *different* maps may share short
  names; resolving such a short name then fails with
  `CANCESTRY_CODEC_ERR_AMBIGUOUS` and the canonical
  `<codec_map_name>.<signal_name>` must be used (spec section 2).
- **Codec map names must not contain '.'**, or canonical names would be
  ambiguous to parse. Signal names may contain '.'; canonical resolution
  splits at the first dot.
- **Signals may exceed the message `dlc`**: the loader accepts them, and
  decoding a frame too short for any declared signal drops the whole message
  with a `frame_too_short` warning and no signal updates (spec section 8).
  Extra bytes beyond a signal's span are ignored.
- **Signal ids** are assigned by the namespace in registration order (1-based)
  and are stable for a fixed registration order (SYS-NF-001). The namespace
  is a bounded registry over caller-owned storage and never allocates.
- **Float tolerance** for tests is `1e-6` (absolute), the value named by the
  issue. `strtod` follows the C locale's decimal separator: hosts/targets
  shall run with `LC_NUMERIC=C`, and codec maps shall use ASCII digits with
  '.' as the decimal separator. NaN/infinity scalars cannot be written in the
  YAML subset and are rejected by validation.
- **String fields require YAML strings**: an unquoted scalar that parses as
  an integer, float or boolean is rejected for every string-typed schema
  field (name, unit, `values` labels, ...) per the schema's `type: string`.
  Quoted scalars are always strings, whatever they look like.
- **Bounds**: codec map/message/signal names ≤ 64 bytes, units ≤ 32, value
  labels ≤ 64, descriptions ≤ 512, version strings ≤ 32; messages/signals per
  map ≤ 65535. These bound the load-time allocation.

### Documented discrepancy

`docs/trace/traceability.csv` mapped SYS-FR-010 (shared signal namespace) to
`SW-FR-CODEC-010`, but `docs/software/SwRS.md` v0.2.1 defines only
SW-FR-CODEC-001..008 and the namespace requirement is SW-FR-CODEC-007. The
CSV row now points at SW-FR-CODEC-007 (the intended id, recorded in the PR
description). Neither document was edited; this README is the record.

## YAML subset

The loader parses the strict subset below; anything else fails with a
line/column error rather than being guessed at:

- block mappings and block sequences only (no flow `[]`/`{}`, no anchors,
  aliases, tags, directives, no multi-line scalars);
- optional `---` document start and `...` document end;
- comments (`#` to end of line, but only after whitespace in plain scalars);
- plain, single-quoted (`''` escape) and double-quoted scalars (C-style
  escapes incl. `\xXX`, `\uXXXX`, `\UXXXXXXXX` encoded as UTF-8);
- integer scalars in decimal, `0x` hex and `0o` octal; float scalars with
  '.' or exponent; booleans `true`/`false`;
- duplicate mapping keys are rejected; tabs are not allowed in indentation.

Validation then enforces every constraint of
`schemas/codec-map-0.2.0.schema.json` (required fields, types, ranges,
`additionalProperties: false`, the boolean/`bit_length: 1` and enum/`values`
conditionals) in C, so no external JSON Schema validator runs at load time.

## Example

```yaml
schema_version: "0.2.0"
codec_map:
  name: demo
  version: 1.0.0
  messages:
    - id: 0x1A0
      name: EngineData
      dlc: 4
      period_ms: 10
      signals:
        - name: EngineSpeed
          start_bit: 0
          bit_length: 16
          type: uint
          endianness: little
          scale: 0.25
          unit: rpm
          min: 0
          max: 16000
        - name: BrakePressed
          start_bit: 16
          bit_length: 1
          type: boolean
          endianness: little
          values:
            "0": RELEASED
            "1": PRESSED
```
