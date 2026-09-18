# CANcestry yaml2c

A host-side code generator that turns canonical CANcestry YAML documents
into pure static C headers, so a target image can drop the runtime YAML
loader entirely ("static codegen", Phase 8, issue #22).

Implements: SW-FR-TOOL-001..004, SW-FR-TOOL-010.

- **FSM documents** (`schemas/fsm-0.3.0.schema.json`) become headers
  initializing `cancestry_fsm_set_t` — the exact struct the runtime loader
  in `core/fsm/src/loader.c` builds — with `const static` arrays, so the
  generated data is a 1:1, zero-allocation drop-in (SW-FR-TOOL-001/003).
- **Codec-map documents** (`schemas/codec-map-0.3.0.schema.json`) become
  headers initializing `cancestry_codec_map_t`, with every derived field
  (`first_bit`/`last_bit`, sawtooth bounds, value mappings) computed exactly
  as `core/codec/src/loader.c` computes it (SW-FR-TOOL-002/003).

"Schema is law" (SW-FR-TOOL-004): every input is validated against the
canonical Draft-2020-12 JSON Schema before a single line of C is emitted,
and the generator additionally enforces the C loaders' structural rules
(duplicate keys, YAML 1.1 scalar handling, number grammar, name/length
limits, expression grammar) with loader-identical verdicts. A document the
runtime loader would refuse never reaches code generation.

## Usage

```
python3 tools/yaml2c/yaml2c.py [--kind fsm|codec] [--schema-dir DIR]
                               [--set-symbol PREFIX] [--guard GUARD]
                               [--stdout | -o OUT.h] input.yaml
```

The document kind is auto-detected from the top-level key
(`state_machines` vs `codec_map`); `--kind` overrides. The default symbol
prefix derives from the input file name; `--set-symbol` overrides it.
`--no-schema-validate` skips only the JSON Schema pass (the loader-parity
checks always run); it exists for validating documents against older schema
generations, not for bypassing validation in CI.

Exit codes: `0` ok, `1` refused document (schema violation, loader-parity
refusal, YAML error), `2` usage or environment error.

### Example

```
python3 tools/yaml2c/yaml2c.py \
    --schema-dir schemas --kind fsm \
    --set-symbol gateway --guard GATEWAY_FSM_H \
    -o gateway_fsm.h examples/gateway_real/gateway_fsm.yaml
```

`gateway_fsm.h` now defines `gateway` as a
`static const cancestry_fsm_set_t`; link it instead of calling
`cancestry_fsm_set_load()`. A worked example, including a codec map in the
same binary, lives in `examples/gateway_real/`
(`cancestry_gateway_real_static` CMake target).

## Determinism and output guarantees

- Identical input bytes produce byte-identical headers (SW-FR-TOOL-009).
- All storage is `static const`; no initializers execute at runtime and the
  linked binary passes `ci/check_no_alloc.py` (SW-FR-TOOL-003).
- Strings are emitted as octal-escaped C literals (no trigraphs, no hex
  escape run-ons); doubles round-trip bit-exactly through `repr`.

## Dependencies

`PyYAML` and `jsonschema` (see `requirements.txt`) plus the standard
library. Host-side only: no new C dependencies in `core/` or `platform/`
(SW-FR-TOOL-010).

## Tests

`tests/unit/tools/` — generator output compiled with strict flags and
linked against the real runtime, loader-parity refusal matrices, expression
grammar acceptance matrix, CLI contract, schema-gate behaviour, and the
100% line-coverage requirement of issue #22.
