# CANcestry-
CAN bus  open CAN codec maps, recipes, and emulation state machines.describe the bus, teach the machine, emulate the module

**Latest release: v1.0.0** (software safety-case baseline) — the portable cores,
stateless BMS governor and hardware-first HIL evidence are covered by the
release gate. See the [changelog](CHANGELOG.md), the
[Safety Manual](docs/safety/SafetyManual.md), the
[HIL report](docs/qa/hil-fault-injection-report.md) and the
[final traceability report](docs/trace/final_v1_report.md).

The v1.0.0 software evidence is an ASIL-B alignment target, not an ISO 26262
certification; target-board HIL measurements remain an integration prerequisite.

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
| `schemas/` | JSON Schemas for package manifests, codec maps, recipes and FSMs (FSM finalized at v0.3.0). |
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
| `ci/` | Repository checks run as CTest tests (`check_no_alloc.py`, `check_schemas_valid.py`, `check_traceability.py`). |
| `formal/` | Optional Phase 9 verification drivers for Frama-C/WP, KLEE and MISRA C:2012 cppcheck analysis. |
| `docs/qa/` | QA records, including the Phase 12 HIL report and target procedure. |
| `docs/releases/` | Release notes and community announcements. |

## Documentation

- [`docs/releases/v0.3.0-rc.1.md`](docs/releases/v0.3.0-rc.1.md) - release notes and announcement for the current release candidate.
- [`docs/system/event-ordering.md`](docs/system/event-ordering.md) - normative event ordering and queue overflow policies.
- [`docs/software/SwRS.md`](docs/software/SwRS.md) - software requirements.
- [`docs/trace/traceability.md`](docs/trace/traceability.md) - scope, coverage numbers, deferred ledger and how to audit them.
- [`docs/trace/final_v1_report.md`](docs/trace/final_v1_report.md) - v1.0.0 coverage result and v1.1.0 deferred ledger.
- [`docs/trace/traceability.csv`](docs/trace/traceability.csv) - requirement-to-test mapping (checked in CI).
- [`docs/qa/hil-fault-injection-report.md`](docs/qa/hil-fault-injection-report.md) - Phase 12 fault-injection evidence and target procedure.
- [`docs/qa/smoke-test-v0.3.0-rc.1.md`](docs/qa/smoke-test-v0.3.0-rc.1.md) - historical v0.3.0 verification evidence.
- [`docs/safety/SafetyManual.md`](docs/safety/SafetyManual.md) - v1.0.0 architecture, FMEA and ASIL-B-aligned safety evidence.
- [`docs/safety/MISRA_Deviations.md`](docs/safety/MISRA_Deviations.md) - MISRA C:2012 deviation record and static-analysis procedure.
- [`Test strategy.md`](Test%20strategy.md) - the QA strategy the checks above implement.
