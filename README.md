# CANcestry-
CAN bus  open CAN codec maps, recipes, and emulation state machines.describe the bus, teach the machine, emulate the module

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
| `CANCESTRY_ENABLE_ASAN` | `ON` in Debug | Build with AddressSanitizer and UndefinedBehaviorSanitizer. |
| `CANCESTRY_STRICT_WARNINGS` | `ON` | Build with `-Wall -Wextra -Werror -Wpedantic` and the embedded coding-rule warnings. |

The core libraries build with the same warning set that CI enforces, so a clean
local build is a clean CI build.

## Repository map

| Path | Contents |
|---|---|
| `docs/` | System, software and package specifications, plus traceability. |
| `schemas/` | JSON Schemas for package manifests, codec maps, recipes and FSMs. |
| `core/event/` | Portable event model: types, clock and bounded queue. See [`core/event/README.md`](core/event/README.md). |
| `core/codec/` | Codec engine: decode/encode against codec-map schemas. See [`core/codec/README.md`](core/codec/README.md). |
| `core/recipe/` | Recipe engine: event-triggered transformations, routing and filtering between the event bus and the codec engine. See [`core/recipe/README.md`](core/recipe/README.md). |
| `tests/unit/` | Host unit tests, one executable per area. |
| `ci/` | Repository checks that run as CTest tests. |

## Documentation

- [`docs/system/event-ordering.md`](docs/system/event-ordering.md) - normative event ordering and queue overflow policies.
- [`docs/software/SwRS.md`](docs/software/SwRS.md) - software requirements.
- [`docs/trace/traceability.csv`](docs/trace/traceability.csv) - requirement-to-test mapping.
