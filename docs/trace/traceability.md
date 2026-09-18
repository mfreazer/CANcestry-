# CANcestry Traceability

| Field | Value |
|---|---|
| Document | CANcestry Traceability Record |
| Version | 0.3.0-rc.1 + Phase 6/7 rows |
| Status | Finalized for the v0.3.0 Release Candidate ([issue #13](https://github.com/mfreazer/CANcestry-/issues/13)); Phase 6 ([issue #17](https://github.com/mfreazer/CANcestry-/issues/17)) and Phase 7 ([issue #20](https://github.com/mfreazer/CANcestry-/issues/20)) rows added on the way to v0.4.0 |
| Owner | QA |
| Last Review | 2026-09-17 |
| Checked in CI | `cancestry_traceability_consistent` (`ci/check_traceability.py`) |

This document accompanies [`traceability.csv`](traceability.csv). The CSV maps
requirements to verification artifacts; this record defines the release scope so
"coverage" is auditable, states the numbers that back the claim, and lists every
requirement that is **not** verified at v0.3.0-rc.1 with the phase that owns it.
Both statements are machine-checked (`ci/check_traceability.py`, section 6).

## 1. CSV format

```csv
requirement_id,artifact_id,verification_method,test_id,status
```

| Field | Meaning |
|---|---|
| `requirement_id` | A requirement from `docs/software/SwRS.md` (`SW-FR-*`), `docs/SyRS.md` (`SYS-*`), or a QA review item (`QA-*`). |
| `artifact_id` | The code artifact, schema, document or logical component that realizes the requirement — e.g. `GATEWAY-LOOP`, `schemas/fsm-0.3.0.schema.json`, `tests/integration/opendbc-parity`. |
| `verification_method` | `test`, `inspection` or `demonstration`. |
| `test_id` | The named test case that verifies the row. For a `passing` row this id must appear **literally** in a source artifact under `tests/`, `examples/`, `tools/` or `ci/`, so a reviewer can open the thing that verifies the claim (enforced, section 6). Ids on `planned` rows are reserved names for work not yet realized. |
| `status` | `passing` (executed and green in CI), `planned` (designed, not yet realized), or `failed`. |

A requirement may appear in several rows; each row is one independent
verification path (26 requirements have more than one). Requirement ids also
appear as `artifact_id`s where a system-level requirement is realized by a
software requirement (for example `SYS-FR-003` realized by `SW-FR-CODEC-001`).

> **Format note.** `Test strategy.md` section 10 sketches a six-column form with
> a separate `artifact_type`. The shipped record keeps the five columns above
> and expresses the artifact kind inside `artifact_id` (a logical name such as
> `EVENT-QUEUE-COUNTERS`, or a path). The CI check treats the header above as
> normative; if the strategy is reworded for v0.4.0, the two should be
> reconciled in one change.

## 2. v0.3.0-rc.1 release scope

The v0.3.0 Release Candidate delivers the **portable core runtime** and proves
it end to end in the gateway integration harness ([issue
#13](https://github.com/mfreazer/CANcestry-/issues/13)):

| Area | Requirements | Verification |
|---|---|---|
| Event bus | `SW-FR-EVENT-001..006` | `tests/unit/core/event/` |
| Codec engine | `SW-FR-CODEC-001..008` | `tests/unit/core/codec/`, opendbc parity |
| Recipe engine | `SW-FR-RECIPE-001..003, 005..007` | `tests/unit/core/recipe/` |
| Governor (stub level) | `SW-FR-GOV-005`, `SW-FR-GOV-006` | recipe and FSM governor tests |
| FSM runtime | `SW-FR-FSM-001..055` | `tests/unit/core/fsm/`, `tests/conformance/fsm/`, [`core/fsm/README.md`](../../core/fsm/README.md) |
| Determinism | `SYS-NF-001`, `SYS-FR-014` | golden-output and double-run tests |
| Bounded resources / zero runtime heap | `SYS-NF-002` | tripwire tests + `ci/check_no_alloc.py` |
| Observability | `SYS-NF-005` | engine/queue/governor counters, gateway report |
| Maintainability | `SYS-NF-006` | modular host-only build, no hardware in CI |
| Portability | `SYS-NF-007` | pure C99 core, host build of every module, harness cross-link procedure ([smoke test record](../qa/smoke-test-v0.3.0-rc.1.md) section 5) |
| Testability | `SYS-NF-008` | this record, the CI check in section 6, and one test per high-priority requirement |
| System integration | `SYS-FR-003..006, 009..010, 014, 019`; `SYS-SF-002` | `examples/gateway/` (`GATEWAY-*` tests) |
| FSM schema finalization | `SW-FR-FSM-001..003` | `schemas/fsm-0.3.0.schema.json`, `FSM-LOAD-*` |

## 3. Coverage at v0.3.0-rc.1

`docs/software/SwRS.md` and `docs/SyRS.md` define **167** requirement ids. Of
those, **125 are traced in the CSV** and **56 are named in the deferred ledger**
(section 5). Of the 125 traced requirements, 111 have at least one `passing` row
and 14 have only `planned` rows; every one of those 14 is also listed in the
ledger.

| Area | `passing` | `planned` | rows |
|---|---:|---:|---:|
| `SW-FR-EVENT` | 8 | 0 | 8 |
| `SW-FR-CODEC` | 8 | 0 | 8 |
| `SW-FR-RECIPE` | 6 | 0 | 6 |
| `SW-FR-FSM` (all 55 requirements) | 59 | 0 | 59 |
| `SW-FR-GOV` (stub level, 005/006) | 5 | 0 | 5 |
| `SW-FR-HAL` (Phase 6) | 11 | 1 | 12 |
| `SW-FR-CANFD` (Phase 7) | 12 | 1 | 13 |
| `SYS-FR` | 14 | 9 | 23 |
| `SYS-NF` | 18 | 0 | 18 |
| `SYS-IR` | 1 | 0 | 1 |
| `SYS-SF` | 2 | 4 | 6 |
| `SYS-SEC` | 0 | 2 | 2 |
| `QA-*` review items | 8 | 7 | 15 |
| **Total** | **152** | **24** | **176** |

By method: 162 `test`, 11 `inspection`, 3 `demonstration`.

There are **no `failed` rows**: a failing row would mean a released claim is not
met, and the release gate (section 6) refuses that state.

## 4. Gateway test ids ([issue #13](https://github.com/mfreazer/CANcestry-/issues/13))

| Test id | Executable / artifact | Proves |
|---|---|---|
| `GATEWAY-LOOP-001` | `cancestry_gateway_loop` | full ingress-decode-queue-recipe/fsm-encode-egress cycle |
| `GATEWAY-DETERMINISM-001` | `cancestry_gateway_loop` | two independent runs byte-identical |
| `GATEWAY-NO-ALLOC-001` | `cancestry_gateway_loop` + `cancestry_gateway_no_alloc_symbols` | zero heap allocation in the processing loop |
| `GATEWAY-FAILCLOSED-001` | `cancestry_gateway_loop` | governor denial and expression fault are recorded, the offending instance is suspended, the loop continues |
| `GATEWAY-COUNTERS-001` | `cancestry_gateway_loop` | frame/event/drop/violation counters exposed and exact |
| `GATEWAY-TRACE-001` | `cancestry_gateway_loop` | transition/event trace rendered deterministically (golden output) |
| `GATEWAY-HOST-001` | host build of `examples/gateway` | every module builds and runs without hardware |

The executed evidence for these ids — commands, exit statuses, golden-output
hash and the on-target procedure — is recorded in the
[v0.3.0-rc.1 smoke test record](../qa/smoke-test-v0.3.0-rc.1.md).

## 4.1 Phase 7 CAN FD test ids ([issue #20](https://github.com/mfreazer/CANcestry-/issues/20))

| Test id | Executable / artifact | Proves |
|---|---|---|
| `CODEC-CANFD-BOUNDS-001` | `cancestry_conformance_codec_can_fd_payload_bounds` | 64-byte payloads encode/decode bit-exactly; an 8-byte classic buffer is never overrun (ASan); only wire-representable lengths are accepted |
| `CODEC-CANFD-SCHEMA-001` | `cancestry_conformance_codec_can_fd_schema` | `can_fd: true` is refused at load time on a classic platform; classic maps keep the v0.2.0 `dlc`/`start_bit` limits; the FD `dlc` vocabulary is exactly the CAN FD one |
| `HAL-CANFD-FALLBACK-001` | `cancestry_conformance_hal_can_fd_fallback` | A CAN FD frame on a classic-only interface raises `PROTOCOL_UNSUPPORTED`, is dropped and counted, and is ordered ahead of that poll's `CAN_RX` events; an FD-capable interface delivers all 64 bytes |
| `FSM-CANFD-EGRESS-001` | `cancestry_conformance_fsm_can_fd_egress` | The declarative FSM send path refuses an FD message (action error) instead of truncating it, while classic sends on the same map still work |
| `RECIPE-CANFD-EGRESS-001` | `cancestry_test_recipe_engine` | The same refusal on the recipe engine's send path |
| `HAL-REAL-CANFD-001` | `examples/gateway_real` | SocketCAN `CAN_RAW_FD_FRAMES` negotiation, `CANFD_MTU` ingress/egress and the 64-byte round trip on a real interface (`planned`: needs `vcan` with `fd on`, unavailable in the CI sandbox - the example prints a SKIP and exits 0) |

The zero-allocation half of `SW-FR-CANFD-005` reuses the existing archive scans
(`cancestry_hal_no_malloc_symbols`, `cancestry_platform_linux_no_malloc_symbols`,
`cancestry_event_no_malloc_symbols`), which now cover the 64-byte frame and
`CANFD_MTU` wire buffers.

## 4.2 Phase 9 formal verification ids ([issue #24](https://github.com/mfreazer/CANcestry-/issues/24))

The formal artifacts are kept outside the default target build and are invoked
explicitly in the safety verification environment. Their source contracts and
harnesses cite the existing requirement ids; a missing tool is an environment
failure, never a passing result.

| Verification id | Artifact | Proves |
|---|---|---|
| `FRAMA-WP-EVENT-CODEC-001` | `formal/frama-c/verify_wp.sh` and ACSL contracts in `core/event/src/queue.c`, `core/codec/src/{encoder,decoder}.c` | Queue writes remain within caller storage; encode/decode frame and bit-span preconditions prevent runtime errors; short-frame decode is atomic |
| `KLEE-EXPR-001` | `formal/klee/expression_harness.c` | Bounded expression text and resolver values cannot cause unbounded parser depth, undefined arithmetic, division by zero or signed division overflow |
| `KLEE-EVENT-ORDER-001` | `formal/klee/event_order_harness.c` | Symbolic adversarial timestamps preserve antisymmetry and transitivity of timestamp/priority/sequence ordering |
| `MISRA-C2012-001` | `formal/misra/run_cppcheck.sh`, `docs/safety/MISRA_Deviations.md` | cppcheck MISRA C:2012 analysis is run against `core/` and `platform/`; every accepted exception has a bounded technical rationale |
| `SAFETY-MANUAL-001` | `docs/safety/SafetyManual.md` | Architecture, fail-closed behavior, fault saturation, assumptions and the independent verification strategy are documented |

These ids are verification-record labels rather than new software
requirements. The existing rows for `SW-FR-EVENT-004..006`,
`SW-FR-CODEC-001..008`, `SW-FR-FSM-035..038`, `SW-FR-FSM-045..046` and
`SYS-NF-001..002` remain the normative requirement mappings.

## 5. Deferred ledger: requirements with no verified artifact yet

Every requirement below is either absent from the CSV or present only with
`planned` rows. None of them is a portable-core requirement, and each is owned
by a later phase. `ci/check_traceability.py` reads the **first column** of this
table (the rationale column is prose and never counts): ids and inclusive ranges
written there are what the coverage rules treat as deliberately deferred, so a
requirement can only be "not verified" on purpose.

| Deferred requirements | Phase that owns them | Why they are not verified at v0.3.0-rc.1 |
|---|---|---|
| `SW-FR-PKG-001..007`, `SYS-FR-001`, `SYS-FR-002`, `SYS-FR-017`, `SYS-SF-006`, `SYS-SEC-001`, `SYS-SEC-002`, `SYS-IR-004` | Package loader, integrity and versioning | v0.3.0-rc.1 loads codec maps, recipes and FSMs as individual declarative files with per-file schema validation. The package manifest, semantic-version compatibility, hash/signature verification, debug-interface policy and the stable-format guarantee for the combined package belong to the package layer. |
| `SW-FR-CAN-001..006`, `SYS-FR-008`, `SYS-IR-001`, `SYS-IR-002`, `SYS-IR-003`, `SYS-IR-006`, `SYS-NF-004`, `SYS-SF-005`, `SYS-SF-007` | CAN integration and platform layer | These need hardware access the core deliberately does not have: bit rates up to 1 Mbps, named physical interfaces, host transport, bus-off recovery, watchdog, error counters and persistent storage. The gateway harness is a logic harness with no bus backend ([smoke test record](../qa/smoke-test-v0.3.0-rc.1.md) section 5.1). |
| `SW-FR-LOG-001..005`, `SYS-FR-011`, `SYS-FR-020`, `SYS-SEC-003` | Logger, log lifecycle and replay | Log formats, retrieval, local clearing, privacy expectations and replay of logged traffic. |
| `SW-FR-SIM-001..004`, `SYS-FR-012`, `SYS-FR-013` | Host simulator and host interface | Package execution against virtual CAN inputs, plus the status/package-management/mode-control interface. `SYS-FR-014` determinism is already claimed by the gateway harness, but its simulator row stays `planned` until the simulator exists. |
| `SW-FR-GOV-001..004`, `SYS-SF-001`, `SYS-SF-003` | Full safety governor | Token buckets, ID-level rate limits and an enforced non-transmitting boot state. v0.3.0-rc.1 ships the fail-closed stub, verified at stub level by `SW-FR-GOV-005`/`SW-FR-GOV-006` and `SW-FR-FSM-023/039..042`. |
| `SYS-FR-015`, `SYS-FR-016`, `SYS-SF-004` | Mode and fault state machine (SAFE mode) | Boot-into-safe-state, the complete fault-class table and SAFE-mode transitions. The fail-closed half of `SYS-FR-016` is demonstrated today (`GATEWAY-FAILCLOSED-001`); bus-off recovery and storage failure are not. |
| `SW-FR-RECIPE-004` | Recipe engine (timeout watches) | The stub engine has no runtime clock, so a timeout trigger refuses to match (fail closed); documented in [`core/recipe/README.md`](../../core/recipe/README.md). |
| `SYS-FR-007`, `SYS-SF-008`, `SYS-NF-003` | Emulation profile tests, static analysis, performance tooling | The v0.3.0 profile test for `SYS-FR-007`, the "no user code execution" inspection for `SYS-SF-008`, and event-to-action latency measurement for `SYS-NF-003` need later-phase tooling. |
| `SW-FR-HAL-010` | Phase 6 hardware integration | The Linux SocketCAN backend is compiled and linked, but verification against a live `vcan0`/`vcan1` pair requires elevated privileges and kernel modules not present in the CI sandbox; the conformance suite covers behaviour against the mock HAL, and the source is reviewable. See `examples/gateway_real/README.md`. |

## 6. Reproduce and audit

```sh
# Build the portable core and run every registered test (44 tests at Phase 7).
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel
ctest --test-dir build --output-on-failure

# The traceability gate (registered as cancestry_traceability_consistent).
python3 ci/check_traceability.py .
```

The gate enforces, and reports the numbers for:

1. the documented CSV header, no empty or duplicated rows;
2. the `verification_method` and `status` vocabularies;
3. requirement-id shape (`SW-FR-*`, `SYS-*`, `QA-*`);
4. no invented ids: every `SW-FR-*`/`SYS-*` id used in the CSV is defined in the SwRS or the SyRS;
5. no silently absent requirement: every defined requirement is in the CSV or in the deferred ledger (section 5);
6. every `passing` row resolves to a literally named test id in `tests/`, `examples/`, `tools/` or `ci/`;
7. every test source (`tests/**/*.c`) cites at least one requirement id or QA review item;
8. every requirement whose rows are all `planned` is named in the deferred ledger.

To trace a single requirement by hand:

```sh
grep '^SYS-NF-002,' docs/trace/traceability.csv        # its rows
grep -rn 'GATEWAY-NO-ALLOC-001' tests/ examples/ ci/   # the artifact that verifies it
```

To add a requirement or change a claim: add or edit the row, make sure the test
id appears in the artifact that verifies it, then run the gate. To defer a
requirement on purpose, name it in the ledger in section 5 with its owning
phase — the gate fails otherwise, which is the point.

Release gate: **no `failed` row and a green gate** are prerequisites for tagging
a release candidate; the executed evidence for the v0.3.0-rc.1 candidate is in
the [smoke test record](../qa/smoke-test-v0.3.0-rc.1.md).

## 7. Change history

| Version | Date | Change |
|---|---|---|
| 0.6.0-wip | 2026-09-18 | Phase 9 (issue #24): formal-verification artifact ids, ACSL/WP contracts, KLEE harnesses, MISRA driver and safety-manual evidence map. |
| 0.4.0-wip | 2026-09-17 | Phase 7 (issue #20): CAN FD requirement rows `SW-FR-CANFD-001..006`, the Phase 7 test-id table in section 4.1, and coverage numbers refreshed to the post-Phase-6/7 state (167 defined, 125 traced, 176 rows). |
| 0.3.0-rc.1 | 2026-09-16 | Phase 5: gateway integration ids (`GATEWAY-*`), FSM schema finalization, coverage numbers, the deferred ledger in section 5, test-id resolution labels in the artifacts, and the CI gate. |
| 0.3.0 | 2026-09-16 | Phase 4/5 scope: FSM runtime rows (`SW-FR-FSM-001..055`) and the conformance suites. |
| 0.2.0 | 2026-09-13 | Skeleton record: event, codec and recipe rows, QA review items from the v0.2.0 acceptance review. |
