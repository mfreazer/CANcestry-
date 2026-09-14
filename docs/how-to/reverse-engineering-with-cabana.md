# Reverse-engineering a CAN bus with Cabana and dbc2codec

This how-to walks through ingesting a third-party vehicle's CAN traffic into a
CANcestry package: capture raw frames, build a DBC with
[Cabana](https://comma.ai/cabana), and compile the DBC into a validated
CANcestry codec map with `tools/dbc2codec`.

Requirements traced: `SYS-FR-018` (third-party extensibility via imported
DBCs), `QA-v0.2-R01` (schema validation enforced on generated artifacts).

## 1. Capture

Record raw CAN frames on the target vehicle with a comma.ai panda (or any
CAN logger that emits `.dbc`-compatible traces), on the relevant buses.

```console
$ python tools/cabana_replay.py --recording /path/to/rlog
```

Keep the capture long enough to cover every message of interest across all
vehicle states (ignition off/on, driving, accessory), so that Cabana has
enough samples to infer signal boundaries.

## 2. Cabana — build the DBC

1. Open the recording in Cabana.
2. Identify each message (`BO_`) by address and mark its DLC.
3. Use the plot/bit-dither views to locate signal boundaries and endianness
   (Intel / little-endian vs. Motorola / big-endian), sign, and `(factor,
   offset)` scaling.
4. Assign names, units, and `VAL_` value tables where the meaning is known.
5. Export the `.dbc` file.

> **Note on multiplexing.** CANcestry v0.2.0 does not model multiplexed
> signals; `dbc2codec` skips them with a warning. If a message is multiplexed,
> split its groups into separate messages (or drop the mux group) before
> export.

## 3. dbc2codec — compile to a codec map

Compile the DBC into a validated CANcestry v0.2.0 codec map, recording the
provenance header (`docs/packages/codec-map-spec.md` §9):

```console
$ pip install -r tools/dbc2codec/requirements.txt
$ python tools/dbc2codec/dbc2codec.py vehicle.dbc \
    -o maps/vehicle.yaml \
    --name vehicle \
    --source-url https://github.com/commaai/opendbc \
    --commit <git-sha> \
    --license "MIT (Copyright (c) comma.ai)"
```

The tool validates the output against `schemas/codec-map-0.2.0.schema.json`
and refuses to write invalid output. Review the warnings on stderr for
skipped signals (multiplexed, multi-byte Motorola, duplicate names); see
`tools/dbc2codec/README.md`.

## 4. Assemble the CANcestry package

Place the generated map under `maps/` and add a manifest
(`docs/packages/package-spec.md`):

```toml
# cancestry.toml
[package]
name = "vehicle"
version = "0.1.0"
type = "codec"

[runtime]
min_cancestry_version = "0.2.0"
```

```text
vehicle-package/
  cancestry.toml
  maps/
    vehicle.yaml
```

## 5. Verify

Run the opendbc parity harness to confirm the generated map decodes bit-exact
against an independent reference parser (`tests/integration/opendbc-parity`),
then add signal-level recipes or FSM states against the imported signals.
