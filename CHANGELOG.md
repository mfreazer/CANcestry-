# Changelog

All notable changes to CANcestry are documented here. The format is
Keep a Changelog style; requirement IDs refer to `docs/software/SwRS.md` and
`docs/SyRS.md`, and the requirement-to-artifact mapping lives in
`docs/trace/traceability.csv`.

## [0.3.0-rc.1] - 2026-09-16

Phase 5 release candidate: the four portable core modules now run together in
one deterministic, zero-heap gateway loop, and the v0.3.0 schemas are final.

### Added

- **System integration gateway loop** (`examples/gateway/main.c`, issue #13):
  raw CAN frame ingress -> `core/codec` decode -> `core/event` queue ->
  `core/recipe` + `core/fsm` state transitions -> `core/codec` encode ->
  egress sink. The harness proves, in one program (SYS-FR-014, SYS-NF-001/002):
  - zero heap allocations during the processing loop (allocator tripwire plus
    `ci/check_no_alloc.py` symbol scans on the core archives and on the
    example object),
  - bit-exact reproducibility: two independent runs and repeated invocations
    produce byte-identical output and egress frames,
  - fail-closed behavior: a governor denial and an expression fault are
    recorded, the offending FSM instance is suspended (SW-FR-FSM-024), and the
    loop continues draining the queue,
  - the finalized FSM schema loaded and validated at load time
    (SW-FR-FSM-001/003), invalid documents rejected before any runtime sees
    them.
  Registered as CTest tests `cancestry_gateway_loop` and
  `cancestry_gateway_no_alloc_symbols`; the target additionally compiles with
  `-Wconversion`.
- **`schemas/fsm-0.3.0.schema.json`**: finalizes the v0.2.0 declaration
  structure unchanged for the v0.3.0 release candidate. The Phase 4 runtime
  needs no new file-level fields (chain depth, deferred transitions,
  missed-tick handling and queue policy are runtime behavior), and fields
  outside the FSM schema - including `layout` (a codec-map signal field) and
  `priority` (a runtime event class) - are rejected at load time by
  `additionalProperties: false` (issue #11 Flag 1).
- **FSM loader targets v0.3.0** (`core/fsm/loader.c`): validates against the
  finalized schema and accepts `schema_version` `"0.2.0"` and `"0.3.0"` alike
  (the two schemas are structurally identical, mirroring the codec loader's
  0.2.0/0.3.0 handling); every other version is rejected.
- `CANCESTRY_BUILD_EXAMPLES` CMake option (default `ON`, host builds only).
- `docs/trace/traceability.md`: the v0.3.0 scope definition and coverage
  summary backing the traceability CSV (Test strategy section 10).

### Changed

- Project version is now `0.3.0` (`CMakeLists.txt`).

### Highlights of the v0.3.0 series (phases 1-4)

- **Deterministic event model** (`core/event`): common event structure with
  seven normative event types, injectable monotonic clocks, and a bounded
  caller-owned queue implementing the normative selection order (timestamp,
  priority class, sequence) with drop-newest overflow that never drops faults
  (SW-FR-EVENT-001..006, SYS-NF-001/002).
- **Codec engine** (`core/codec`): allocation-free decode/encode over the
  v0.2.0/v0.3.0 codec-map schemas with the canonical LSB0 bit model,
  little/big endian, sawtooth layout, scaling/offset, signed/unsigned,
  boolean/enum mappings and a bounded shared signal namespace
  (SW-FR-CODEC-001..008, SYS-FR-003/004/010).
- **opendbc parity** (`tests/integration/opendbc-parity`, `tools/dbc2codec`):
  third-party DBC material converts to codec maps with provenance headers and
  decodes bit-identically to opendbc (SYS-NF-008, SYS-FR-018).
- **Recipe engine** (`core/recipe`): event-triggered, condition-gated
  transformations with the full action set, directional filtering, interface
  bindings, transition-action rejection and a fail-closed governor stub
  (SW-FR-RECIPE-001..003/005..007, SW-FR-GOV-005/006).
- **FSM runtime** (`core/fsm`): allocation-free, deterministic state machine
  execution with compiled definitions, guards and expressions, timers,
  capability enforcement, bounded queues and budgets, golden-output tracing
  and the seven-suite conformance suite (SW-FR-FSM-001..055).

## [0.2.0] - 2026-09-13

v0.2.0 corrective baseline: schemas, traceability skeleton and the test
strategy recorded in `Test strategy.md` v0.2.0; portable event core, codec
engine and recipe engine implemented with host unit tests.
