# opendbc parity harness

Integration tests that prove the CANcestry C decoder produces bit-exact
results compared to comma.ai `opendbc`'s reference `CANParser` for signals
imported from real vehicle DBCs. This is the Phase 2.5 parity harness
(`QA-H02`).

## What it verifies

`test_parity.py`:

1. Compiles each selected vehicle DBC into a CANcestry codec map with
   `tools/dbc2codec`.
2. Builds the CANcestry C decoder into a small CLI (`codec_cli.c`).
3. Generates identical, deterministic random CAN frames (fixed seed) and feeds
   them to both:
   - opendbc's reference parser (`opendbc.can.parser.get_raw_value` +
     `opendbc.can.dbc.DBC`), and
   - the CANcestry C decoder.
4. Asserts every decoded physical value agrees within `1e-6` absolute / `1e-9`
   relative tolerance.

`test_dbc2codec.py` covers the compiler itself: provenance header, schema
validation, deterministic output, and the documented skip rules (multiplexed,
multi-byte Motorola, duplicate names).

## Selected DBCs

| DBC | Coverage |
|---|---|
| `tesla_model3_vehicle` | all little-endian (Intel), enums/booleans, scaling |
| `hyundai_i30_2014` | all little-endian, heavy scaling |
| `toyota_prius_2010_pt` | all big-endian (Motorola), single- and multi-byte |

Signals the v0.2.0 codec model cannot express (multi-byte Motorola,
multiplexed) are skipped by `dbc2codec` and are therefore outside the
comparison; see `tools/dbc2codec/README.md`.

## Pinned reference

The reference `opendbc` checkout is pinned in `requirements.txt` to commit
`057aee25b5eee7530f0b95b5b508c8c3247b0cd7` (the known-good commit these
assertions were validated against).

## Running

```console
$ python -m venv .venv && . .venv/bin/activate
$ pip install -r tests/integration/opendbc-parity/requirements.txt
$ pytest tests/integration/opendbc-parity -q
```

A C compiler (`gcc`/`clang`) must be on `PATH`; the harness compiles the
CANcestry codec CLI once per session.
