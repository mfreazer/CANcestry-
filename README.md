# CANcestry-
CAN bus  open CAN codec maps, recipes, and emulation state machines.describe the bus, teach the machine, emulate the module

**Latest release: v0.3.0-rc.1** (release candidate) — the four portable cores run
together in one deterministic, zero-heap gateway loop. See the
[release notes](docs/releases/v0.3.0-rc.1.md), the
[smoke test record](docs/qa/smoke-test-v0.3.0-rc.1.md) and the
[traceability record](docs/trace/traceability.md).

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
| `examples/gateway/` | Top-level integration harness: the full decode → event → recipe/FSM → encode gateway loop. See [`examples/gateway/README.md`](examples/gateway/README.md). |
| `tests/unit/` | Host unit tests, one executable per area. |
| `tests/conformance/` | The FSM conformance suites (seven contracts, run against the shipping loader and engine). |
| `tests/integration/` | Cross-implementation checks, including the opendbc parity harness. |
| `ci/` | Repository checks that run as CTest tests (`check_no_alloc.py`, `check_schemas_valid.py`, `check_traceability.py`). |
| `docs/qa/` | QA records: the executed v0.3.0-rc.1 smoke test and its on-target procedure. |
| `docs/releases/` | Release notes and community announcements. |

## Documentation

- [`docs/releases/v0.3.0-rc.1.md`](docs/releases/v0.3.0-rc.1.md) - release notes and announcement for the current release candidate.
- [`docs/system/event-ordering.md`](docs/system/event-ordering.md) - normative event ordering and queue overflow policies.
- [`docs/software/SwRS.md`](docs/software/SwRS.md) - software requirements.
- [`docs/trace/traceability.md`](docs/trace/traceability.md) - scope, coverage numbers, deferred ledger and how to audit them.
- [`docs/trace/traceability.csv`](docs/trace/traceability.csv) - requirement-to-test mapping (checked in CI).
- [`docs/qa/smoke-test-v0.3.0-rc.1.md`](docs/qa/smoke-test-v0.3.0-rc.1.md) - executed verification evidence and the on-target procedure.
- [`Test strategy.md`](Test%20strategy.md) - the QA strategy the checks above implement.
