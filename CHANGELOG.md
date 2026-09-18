# Changelog

All notable changes to CANcestry are documented here. The format is
Keep a Changelog style; requirement IDs refer to `docs/software/SwRS.md` and
`docs/SyRS.md`, and the requirement-to-artifact mapping lives in
`docs/trace/traceability.csv`.

## [Unreleased] - Phase 10: UDS & ISO-TP Transport ([issue #26](https://github.com/mfreazer/CANcestry-/issues/26))

Phase 10 adds the diagnostic stack below the gateway: a zero-allocation,
deterministic ISO 15765-2 (ISO-TP) transport engine over classic CAN, and a
UDS (ISO 14229-1) server for ReadDataByIdentifier (0x22),
WriteDataByIdentifier (0x2E) and RoutineControl (0x31), with every write and
routine side effect passing through the fail-closed governor stub. New
requirements: `SW-FR-TP-001..010` and `SW-FR-UDS-001..008`
(`docs/software/SwRS.md` section 14), all traced in
`docs/trace/traceability.csv`.

### Added

- **ISO-TP transport engine** (`core/transport`, SW-FR-TP-001..010):
  caller-owned static RX/TX buffers, SF/FF/CF/FC handling with the 12-bit FF
  length and rolling 4-bit sequence number, fail-closed aborts (protocol
  violation, sequence skip, buffer overflow with FC OVFLW) that clear the
  session and raise `TRANSPORT_*` faults into the `core/event` queue, and
  N_As/N_Bs/N_Cr timers driven exclusively by the deterministic 1 ms tick.
  TX honours the receiver's Flow Control (Block Size, STmin in the ms and
  100-900 us encodings, Wait-frame limit, OVFLW) and retries sink-declined
  frames until N_As expires.
- **UDS server** (`core/uds`, SW-FR-UDS-001..008): request in, response
  bytes out; RDBI/WDBI/RoutineControl with the negative response codes
  0x11/0x12/0x13/0x22/0x31/0x72; signal-mapped DIDs mirror through the codec
  namespace and the shared signal store; the governor stub denies everything
  when NULL and every denial leaves no partial effect.
- **UDS configuration loader** (`core/uds/loader`, SW-FR-UDS-006): a
  YAML-subset loader validating against `schemas/uds-0.1.0.schema.json`
  (schema is law), with located 1-based line/column errors. Heap use is
  load-time only and lives in the separate `cancestry_uds_loader` library.
- **Conformance suites** (`tests/conformance/transport`,
  `tests/conformance/uds`): `TP-RX-001..008`, `TP-TX-001..009`,
  `UDS-SVC-001..007` and `UDS-GOV-001..005`, including the full
  transport-to-UDS loop, all under ASan/UBSan with a virtual 1 ms clock;
  plus the `cancestry_transport_no_malloc_symbols` and
  `cancestry_uds_no_malloc_symbols` archive gates.
- **Documentation** (`core/transport/README.md`, `core/uds/README.md`):
  frame contract, tick order, timer semantics, fault codes, NRC table and
  worked examples for both halves of the diagnostic stack.

## [Unreleased] - Phase 9: Formal Verification & ASIL-B Alignment ([issue #24](https://github.com/mfreazer/CANcestry-/issues/24))

Phase 9 adds non-intrusive ACSL contracts for the event queue and codec bit
paths, KLEE harnesses for bounded expression evaluation and adversarial event
ordering, a strict cppcheck/MISRA C:2012 driver, and the initial software
Safety Manual and deviation record. Formal tools remain optional host-side
verification dependencies and are never linked into the runtime.

### Added

- **Frama-C/WP contracts** (`core/event`, `core/codec`): queue push/pop,
  codec encode/decode and contiguous/Motorola bit helpers now state storage,
  bit-width and legal-payload preconditions for RTE and functional review.
- **KLEE harnesses** (`formal/klee`): symbolic expression depth, resolver
  arithmetic fault paths and timestamp/priority/sequence comparator properties.
- **MISRA and safety evidence** (`formal/misra`, `docs/safety`): strict static
  analysis driver, reviewed deviation record and ASIL-B alignment manual draft.

### Changed

- **PR diagnostics** (`.github/workflows/pr-fast.yml`): removed the temporary
  diagnostic commit/push path and restored normal workflow-run artifact uploads.

## [Unreleased] - Phase 8: Toolchain Enhancements, Static Codegen & Trace Visualizer ([issue #22](https://github.com/mfreazer/CANcestry-/issues/22))

The host toolchain can now eliminate the runtime YAML loaders on the target
and post-mortem a captured run: `tools/yaml2c` compiles canonical FSM and
codec-map YAML into pure `const static` C headers, `tools/trace_viewer`
reconstructs a chronological timeline (and a state graph) from a binary
trace ring dump, and `ci/check_schemas_valid.py` validates the schema set
with full Draft-2020-12 logical validation. New requirements:
`SW-FR-TOOL-001..010` (`docs/software/SwRS.md` section 13), all traced in
`docs/trace/traceability.csv`.

### Added

- **Static code generator** (`tools/yaml2c`, SW-FR-TOOL-001..004):
  converts a schema-valid `fsm-0.3.0` or `codec-map-0.3.0` YAML document
  into a compilable C99 header of `const static` data that initializes the
  runtime definition types 1:1 (`cancestry_fsm_set_t`,
  `cancestry_codec_map_t`), derived fields included
  (payload bit windows, sawtooth bounds). Inputs are validated against the
  canonical Draft-2020-12 schemas before emission, and the loaders'
  structural rules (duplicate keys, YAML 1.1 scalars, number grammar,
  expression grammar, limits) are enforced with loader-identical verdicts.
  A binary linking the generated headers passes `ci/check_no_alloc.py`.
- **Gateway static example** (`examples/gateway_real`, SW-FR-TOOL-001/003):
  `main_static.c` plus `gateway_fsm.yaml`/`gateway_codec.yaml` and a
  `cancestry_gateway_real_static` CMake target demonstrate a full FSM+codec
  binary with both runtime loaders replaced by generated headers
  (`cancestry_gateway_real_static_loop`,
  `cancestry_gateway_real_static_no_alloc_symbols` tests).
- **Trace dump format** (`docs/system/trace-dump-format.md`,
  SW-FR-TOOL-005): normative specification of the `CTRC` binary dump of the
  FSM trace ring and HAL fault/frame records, including the
  sequence-number-based chronological reconstruction rule for wrapped rings
  (fail-closed on ambiguous dumps).
- **Trace viewer** (`tools/trace_viewer`, SW-FR-TOOL-005..007/009/010):
  dependency-free CLI that parses `CTRC` dumps into a chronological
  timeline (text or JSON, relative or absolute times, kind/instance
  filters) and optionally emits a Graphviz `.dot` graph of the transitions
  actually taken, with faulted edges drawn red.
- **Schema logical validation** (`ci/check_schemas_valid.py`,
  SW-FR-TOOL-008): beyond the structural checks, every schema in
  `schemas/` is now validated against the Draft-2020-12 metaschema with
  format checkers (catching, e.g., invalid regex `pattern`s); without
  `jsonschema` the check degrades to an explicit SKIP, never a silent pass.
- **Toolchain requirements and traceability** (`docs/software/SwRS.md`
  section 13, `docs/trace/traceability.csv`, SW-FR-TOOL-001..010).
- **Python unit tests** (`tests/unit/tools`, issue #22 acceptance):
  243 tests covering the new tools with 100% line coverage
  (`python3 -m coverage run -m pytest tests/unit/tools && python3 -m
  coverage report`), including loader-parity refusal matrices, expression
  acceptance matrices, wrap-around reconstruction and CLI contracts.

### Changed

- **CI** (`.github/workflows/pr-fast.yml`): a `toolchain-checks` job
  installs the Python tool dependencies and runs the `tests/unit/tools`
  suite and `ci/check_schemas_valid.py` on every PR.

## [Unreleased] - Phase 7: CAN FD & Extended Frame Support ([issue #20](https://github.com/mfreazer/CANcestry-/issues/20))

CAN FD is now a first-class citizen of the runtime path: 64-byte payloads flow
through the codec engine, the HAL and the Linux SocketCAN backend, and a
classic-only interface rejects an FD frame deterministically instead of
truncating it. New requirements: `SW-FR-CANFD-001..006`
(`docs/software/SwRS.md` section 12), all traced in
`docs/trace/traceability.csv`.

### Added

- **Codec engine, 64-byte payloads** (`core/codec`, SW-FR-CANFD-001/002):
  decode and encode accept up to `CANCESTRY_CODEC_FRAME_MAX_LENGTH` (64) bytes.
  The accepted lengths are exactly the ones a CAN bus can carry - 1..8 for
  classic CAN, 12/16/20/24/32/48/64 for CAN FD - so 9..11 bytes remain an
  argument error. A codec map opts in with `codec_map.can_fd: true`, which
  switches `dlc` to the CAN FD vocabulary and `start_bit` to 0..511; a map
  without the flag is validated exactly as before.
- **Load-time capability gate** (`core/codec/src/loader.c`, SW-FR-CANFD-004):
  new `cancestry_codec_map_load_checked(text, length, caps, error)` refuses a
  `can_fd: true` map with `CANCESTRY_CODEC_ERR_UNSUPPORTED` when the declared
  platform capabilities do not include CAN FD. `cancestry_codec_map_load()`
  and a `NULL` caps argument both mean "classic only", so the check always
  fails closed - the mismatch is a load-time error, never a runtime surprise.
- **HAL CAN FD contract** (`core/hal`, SW-FR-CANFD-002/003): the frame carries
  `is_fd` and a statically sized 64-byte payload; interfaces negotiate CAN FD
  once at open (new optional `caps` entry in the backend vtable, queried
  through `cancestry_hal_iface_can_fd()`). An FD frame on a classic-only
  interface is dropped, counted in the new `rx_protocol_rejected` status field,
  and reported with the new `CANCESTRY_HAL_FAULT_PROTOCOL_UNSUPPORTED` fault
  (ERROR severity) through the same deterministic path and priority class as
  every other HAL fault; FD egress on such an interface returns
  `CANCESTRY_HAL_ERR_UNSUPPORTED` and queues nothing. Truncation is prohibited
  in both directions.
- **SocketCAN CAN FD** (`platform/linux/socketcan.c`, SW-FR-CANFD-004):
  negotiates `CAN_RAW_FD_FRAMES` when `can_fd` is configured and falls back to
  a classic socket without failing the open when the interface refuses;
  classifies received messages by size (`CAN_MTU` vs `CANFD_MTU`), validates
  the payload length, and transmits FD frames with `CANFD_MTU` (requesting
  `CANFD_BRS` when a data bitrate is declared). A compile-time check pins the
  `CAN_MTU`/`CANFD_MTU` assumptions. The 64-byte path adds no allocation: the
  wire buffer is a fixed-size stack union and `setsockopt` takes a
  caller-owned int (`ci/check_no_alloc.py` scans the archive).
- **Mock HAL FD capability** (`platform/mock`): interfaces are classic-only by
  default and opt in with `cancestry_mock_hal_set_fd_support()`, which is what
  makes the fallback contract testable on the same code path the real backend
  uses.
- **Conformance suites**:
  - `tests/conformance/hal/test_can_fd_fallback.c` (`HAL-CANFD-FALLBACK-001`)
    pins the fallback contract: fault code and severity, drop-not-truncate,
    counters, fault-before-data ordering, egress refusal, and a 64-byte
    delivery on an FD-capable interface.
  - `tests/conformance/codec/test_can_fd_payload_bounds.c`
    (`CODEC-CANFD-BOUNDS-001`) proves 64-byte pack/unpack is bit-exact and that
    an 8-byte classic buffer is never overrun - both buffers are exact-size
    heap allocations, so ASan turns any one-byte overrun into a failure.
  - `tests/conformance/codec/test_can_fd_schema.c`
    (`CODEC-CANFD-SCHEMA-001`) proves the load-time rules: capability gate,
    FD vs classic `dlc` vocabulary, 512-bit payload limit, field validation.
  - `FSM-CANFD-EGRESS-001` and `RECIPE-CANFD-EGRESS-001` cover the
    classic-only declarative egress path.
- **`examples/gateway_real`**: a CAN FD demonstration
  (`HAL-REAL-CANFD-001`) that shows the load-time gate, reports the negotiated
  capability, and round-trips a 64-byte frame through HAL -> `core/event` ->
  codec, then transmits it with `CANFD_MTU`; it prints a SKIP with the enabling
  `ip link` command and exits 0 when the interface has no CAN FD.

### Changed

- **`schemas/codec-map-0.3.0.schema.json`**: optional `codec_map.can_fd`
  selects between `classic_message`/`classic_signal` (dlc 0..8, `start_bit`
  0..63) and `fd_message`/`fd_signal` (CAN FD dlc vocabulary, `start_bit`
  0..511). Documents without the flag validate exactly as before.
- **`schemas/hal-0.1.0.schema.json`**: optional per-interface `can_fd` and
  `data_bitrate` fields document the new `cancestry_hal_if_config_t` members.
- **`cancestry_can_rx_payload_t`** (`core/event`) now carries `is_fd` and a
  64-byte payload buffer, so an FD frame reaches the event bus intact instead
  of being truncated. Classic frames copy only their declared length, so
  classic traffic does no extra per-frame work (SW-FR-CANFD-005); the event
  struct is correspondingly larger.
- **`cancestry_hal_poll_rx()`'s `out_fault_count`** now counts every fault
  event the call enqueued, including the ones raised inline during frame
  delivery (timestamp monotonicity, protocol support), matching its documented
  meaning. `test_timestamp_monotonicity.c` now asserts the exact count.
- **Recipe/FSM `send_message`** refuses a message whose `dlc` exceeds the
  classic 8-byte payload with `ERR_UNSUPPORTED` instead of building a frame
  that would not fit, recorded as an action error (SW-FR-CANFD-006, fail
  closed).

### Documentation

- `docs/software/SwRS.md` section 12 (`SW-FR-CANFD-001..006`),
  `docs/trace/traceability.csv` + record (13 new rows, coverage numbers
  refreshed), `docs/packages/codec-map-spec.md` section 8.1,
  `core/codec/README.md`, `core/hal/README.md`, `examples/gateway_real/README.md`,
  and the CAN FD scope statements in `docs/SyRS.md`.

## [0.3.0-rc.1] - 2026-09-16

Phase 5 release candidate: the four portable core modules now run together in
one deterministic, zero-heap gateway loop, and the v0.3.0 schemas are final.
Release notes and announcement: [`docs/releases/v0.3.0-rc.1.md`](docs/releases/v0.3.0-rc.1.md).

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
- **Traceability completeness gate** (`ci/check_traceability.py`, registered as
  CTest test `cancestry_traceability_consistent`): validates the CSV header and
  vocabularies, rejects ids that the SwRS/SyRS do not define, requires every
  `passing` row's test id to appear in the artifact that verifies it, requires
  every test source to cite a requirement, and requires every requirement that
  is not verified to be named in the deferred ledger (Test strategy section 10:
  "Traceability completeness shall be checked in CI").
- **Smoke test record** (`docs/qa/smoke-test-v0.3.0-rc.1.md`): the executed
  evidence for the release candidate - 31/31 tests in Debug+ASan/UBSan and in
  Release, the allocator tripwire reporting zero allocation calls in the
  processing loop, byte-identical golden output across five runs and across
  build configurations - plus the step-by-step procedure and pass/fail criteria
  for a target-hardware run.
- **Release notes draft** (`docs/releases/v0.3.0-rc.1.md`): the community
  announcement highlighting the deterministic event model, opendbc parity, the
  recipe engine and the FSM runtime, with scope, upgrade notes and the open
  questions the maintainer is asked to rule on.
- Test-id labels in the artifacts that realize `passing` traceability rows
  (event unit tests, opendbc parity harness, FSM capability conformance, the
  gateway harness, the archive-scan registrations), so every `passing` row
  resolves to a file a reviewer can open.
- `core/fsm/README.md`: a requirement index for `SW-FR-FSM-001..055`, a public
  API map and an integration section tying the runtime to the gateway harness.

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
