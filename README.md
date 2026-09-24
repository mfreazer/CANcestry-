# CANcestry-
CAN bus  open CAN codec maps, recipes, and emulation state machines.describe the bus, teach the machine, emulate the module

**Current release: v1.0.0** — the portable cores, stateless
BMS governor, hardware-first HIL evidence and QA-EV-01 closure are recorded.
See the [changelog](CHANGELOG.md), the
[Safety Manual](docs/safety/SafetyManual.md), the
[HIL report](docs/qa/hil-fault-injection-report.md), the
[version policy](docs/versions.md) and the [candidate traceability report](docs/trace/final_v1_report.md).

The candidate software evidence is an ASIL-B alignment target, not an ISO 26262
certification; target-board HIL measurements and formal QA-EV-01 closure remain
prerequisites for the final `1.0.0` bump, tag and milestone closure.

## Build and test

The repository is built with CMake. The only requirements are a C99 compiler
and CMake 3.16 or newer.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug      # Debug enables ASan + UBSan
cmake --build build
ctest --test-dir build --output-on-failure
```

Useful options:

| Option | Default | Purpose |
|---|---|---|
| `CANCESTRY_BUILD_TESTS` | `ON` | Build the host unit tests and register them with CTest. |
| `CANCESTRY_BUILD_EXAMPLES` | `ON` | Build the host examples (`examples/gateway`, the integration harness). Skipped when cross-compiling. |
| `CANCESTRY_ENABLE_ASAN` | `ON` in Debug | Build with AddressSanitizer and UndefinedBehaviorSanitizer. |
| `CANCESTRY_STRICT_WARNINGS` | `ON` | Build with `-Wall -Wextra -Werror -Wpedantic` and the embedded coding-rule warnings. |

The core libraries build with the same warning set that CI enforces, so a clean
local build is a clean CI build.

## Repository map

| Path | Contents |
|---|---|
| `docs/` | System, software and package specifications, plus traceability. |
| `docs/hw/` | Hardware plan of record (H-Phase 1): `HW-PLAN`, `HwRS`, `mbse-plan`, `virtual-bench-plan`. Governed by [`HwAGENTS.md`](HwAGENTS.md). |
| `hw/` | Hardware engineering tree (H-Phase 1): Modelica `CancestryLib` (Power hold-up), BOM + cited datasheet extracts, virtual-bench oracle registry, sim cases, evidence and the hardware traceability ledger. Gated by `hw-fast` CI. |
| `schemas/` | JSON Schemas for package manifests, codec maps, recipes and FSMs (FSM finalized at v0.3.0). `schemas/hw/` adds the hardware BOM/extract, sim-case and traceability-row schemas. |
| `core/event/` | Portable event model: types, clock and bounded queue. See [`core/event/README.md`](core/event/README.md). |
| `core/codec/` | Codec engine: decode/encode against codec-map schemas. See [`core/codec/README.md`](core/codec/README.md). |
| `core/recipe/` | Recipe engine: event-triggered transformations, routing and filtering between the event bus and the codec engine. See [`core/recipe/README.md`](core/recipe/README.md). |
| `core/fsm/` | FSM runtime: deterministic, allocation-free state machines with guards, timers and capability enforcement. See [`core/fsm/README.md`](core/fsm/README.md). |
| `core/governor/` | Stateless BMS thermal/torque governor. See [`core/governor/README.md`](core/governor/README.md). |
| `examples/gateway/` | Top-level integration harness: the full decode → event → recipe/FSM → encode gateway loop. See [`examples/gateway/README.md`](examples/gateway/README.md). |
| `tests/unit/` | Host unit tests, one executable per area. |
| `tests/conformance/` | C conformance suites, including the Phase 12 BMS governor tests. |
| `tests/hil/` | Deterministic hardware-first Bus-Off, CRC and brownout fault-injection simulation. |
| `tests/integration/` | Cross-implementation checks, including the opendbc parity harness. |
| `ci/` | Repository checks run as CTest tests (`check_no_alloc.py`, `check_schemas_valid.py`, `check_traceability.py`). `check_hw_traceability.py` is the hardware honest-ledger gate (run by `hw-fast` CI, not CTest); `ci/docker/` holds the digest-pinned hardware toolchain image. |
| `formal/` | Optional Phase 9 verification drivers for Frama-C/WP, KLEE and MISRA C:2012 cppcheck analysis. |
| `docs/qa/` | QA records, including the Phase 12 HIL report and target procedure. |
| `docs/releases/` | Release notes and community announcements. |

## Documentation

- [`docs/releases/v0.3.0-rc.1.md`](docs/releases/v0.3.0-rc.1.md) - release notes and announcement for the current release candidate.
- [`docs/system/event-ordering.md`](docs/system/event-ordering.md) - normative event ordering and queue overflow policies.
- [`docs/software/SwRS.md`](docs/software/SwRS.md) - software requirements.
- [`docs/trace/traceability.md`](docs/trace/traceability.md) - scope, coverage numbers, deferred ledger and how to audit them.
- [`docs/trace/final_v1_report.md`](docs/trace/final_v1_report.md) - v1.0.0-rc.1 candidate coverage result and v1.1.0 deferred ledger.
- [`docs/trace/traceability.csv`](docs/trace/traceability.csv) - requirement-to-test mapping (checked in CI).
- [`docs/qa/hil-fault-injection-report.md`](docs/qa/hil-fault-injection-report.md) - Phase 12 fault-injection evidence and target procedure.
- [`docs/qa/smoke-test-v0.3.0-rc.1.md`](docs/qa/smoke-test-v0.3.0-rc.1.md) - historical v0.3.0 verification evidence.
- [`docs/safety/SafetyManual.md`](docs/safety/SafetyManual.md) - v1.0.0-rc.1 architecture, FMEA and ASIL-B-aligned safety evidence.
- [`docs/safety/MISRA_Deviations.md`](docs/safety/MISRA_Deviations.md) - MISRA C:2012 deviation record and static-analysis procedure.
- [`docs/qa/test-strategy.md`](docs/qa/test-strategy.md) - the QA strategy the checks above implement.
- [`docs/qa/closed-findings.md`](docs/qa/closed-findings.md) - candidate QA closure record and open QA-EV-01 decision.
- [`docs/versions.md`](docs/versions.md) - release-candidate version and final-release gate policy, including the H-Phase 1 hardware document baseline.
- [`docs/hw/HW-PLAN.md`](docs/hw/HW-PLAN.md) - hardware plan of record (H-Phase 1): constraints, toolchain, virtual-bench and credibility policy.
- [`docs/hw/HwRS.md`](docs/hw/HwRS.md) - hardware requirements specification (HW-SF/HW-FR/HW-NF).
- [`docs/hw/virtual-bench-plan.md`](docs/hw/virtual-bench-plan.md) - virtual bench: oracle registry, simulation-first policy and ledger.
- [`HwAGENTS.md`](HwAGENTS.md) - hardware agent policy (load-bearing; `@cancestry-hw-agent` operates under it).

## RAMS

Reliability, Availability, Maintainability and Safety of the gateway hardware
are summarised in [`docs/hw/rams-summary.md`](docs/hw/rams-summary.md)
(T0 analysis, H-09). It draws on the FMEDA
([`docs/hw/fmeda.md`](docs/hw/fmeda.md), generated by
`tools/fmeda-calculator.py` from `hw/fmeda/fmeda-analysis.csv` and the curated
FIT database `hw/bom/fit-database.json`) and the worst-case circuit and
derating analysis ([`docs/hw/wcca-derating.md`](docs/hw/wcca-derating.md),
gated by `ci/check_hw_wcca.py`). The gateway is a single point of failure with
fail-safe immobilization (zero torque, contactors open) as its availability
policy, one field-replaceable unit diagnosed over UDS, and its ASIL-B
*alignment* is stated honestly: the current baseline does not meet the
SPFM target and no ASIL-B claim is made. MTBF regressions are tracked by
`ci/check_hw_reliability_growth.py` against `hw/fmeda/mtbf-history.csv` in
the nightly hardware pipeline.
