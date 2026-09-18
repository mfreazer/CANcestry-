# CANcestry Traceability

| Field | Value |
|---|---|
| Document | CANcestry Traceability Record |
| Version | 1.0.0 final v1 report + Phase 12 rows |
| Status | v1.0.0 software evidence finalized; target hardware limitations are explicit in the final report and HIL report |
| Owner | QA |
| Last Review | 2026-09-18 |
| Checked in CI | `cancestry_traceability_consistent` (`ci/check_traceability.py`) |

This document accompanies [`traceability.csv`](traceability.csv). The CSV maps
requirements to verification artifacts; this record defines the release scope so
"coverage" is auditable, states the numbers that back the claim, and lists every
requirement that is **not** verified in the v1.0.0 software scope with the v1.1.0 owner that accepts it.
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

## 2. Historical v0.3.0-rc.1 release scope

The v0.3.0 Release Candidate delivered the **portable core runtime** and proved
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

## 3. Coverage at Phase 12 / v1.0.0 ([issue #30](https://github.com/mfreazer/CANcestry-/issues/30))

`docs/software/SwRS.md` and `docs/SyRS.md` define **220** requirement ids. Of
those, **164 are in the implemented v1.0.0 scope and all 164 have at least one
`passing` row in the CSV**. The remaining **56 are named in the v1.1.0 deferred
ledger** (section 5). The CSV contains **275 rows**: 251 passing, 24 planned and
zero failed; the planned rows are explicit forward-looking verification paths.

| Area | `passing` | `planned` | rows |
|---|---:|---:|---:|
| `SW-FR-EVENT` | 8 | 0 | 8 |
| `SW-FR-CODEC` | 8 | 0 | 8 |
| `SW-FR-RECIPE` | 6 | 0 | 6 |
| `SW-FR-FSM` (all 55 requirements) | 59 | 0 | 59 |
| `SW-FR-GOV` (stub level, 005/006) | 5 | 0 | 5 |
| `SW-FR-HAL` (Phase 6) | 11 | 1 | 12 |
| `SW-FR-CANFD` (Phase 7) | 12 | 1 | 13 |
| `SW-FR-TOOL` (Phases 5-7) | 24 | 0 | 24 |
| `SW-FR-TP` (Phase 10) | 22 | 0 | 22 |
| `SW-FR-UDS` (Phase 10) | 17 | 0 | 17 |
| `SW-FR-BM` (Phase 11) | 15 | 0 | 15 |
| `SW-FR-BMS` (Phase 12) | 8 | 0 | 8 |
| `SW-FR-HIL` (Phase 12) | 8 | 0 | 8 |
| `SW-FR-SAFETY` (Phase 12) | 5 | 0 | 5 |
| `SYS-FR` | 14 | 9 | 23 |
| `SYS-NF` | 18 | 0 | 18 |
| `SYS-IR` | 1 | 0 | 1 |
| `SYS-SF` | 2 | 4 | 6 |
| `SYS-SEC` | 0 | 2 | 2 |
| `QA-*` review items | 8 | 7 | 15 |
| **Total** | **251** | **24** | **275** |

By method: 261 `test`, 11 `inspection`, 3 `demonstration`.

The distinct implemented v1.0.0 requirement count is 164/164 (100%); the row
count is larger because independent verification paths are retained.

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

## 4.3 Phase 10 UDS & ISO-TP test ids ([issue #26](https://github.com/mfreazer/CANcestry-/issues/26))

The Phase 10 suites run the shipping transport engine and UDS server through
their public headers only, under AddressSanitizer/UndefinedBehaviorSanitizer,
with a virtual 1 ms clock (the `core/event` clock abstraction) so every
timeout is a deterministic tick count.

| Test id | Executable / artifact | Proves |
|---|---|---|
| `TP-RX-001` | `cancestry_conformance_tp_iso_tp_reassembly` | Single Frame (classic PCI) reassembles and is delivered as a borrowed pointer |
| `TP-RX-002` | `cancestry_conformance_tp_iso_tp_reassembly` | FF + CF burst reassembles in order; the engine answers the FF with FC CTS |
| `TP-RX-003` | `cancestry_conformance_tp_iso_tp_reassembly` | A CF sequence-number skip drops the session, clears the buffer and raises `TRANSPORT_PROTOCOL_FAULT`; the next frame starts fresh |
| `TP-RX-004` | `cancestry_conformance_tp_iso_tp_reassembly` | N_Cr expiry at an exact tick count aborts with `TRANSPORT_TIMEOUT` and no partial delivery |
| `TP-RX-005` | `cancestry_conformance_tp_iso_tp_reassembly` | An FF announcing more than the static buffer aborts before storing a byte, answers FC OVFLW and raises `TRANSPORT_BUFFER_OVERFLOW` |
| `TP-RX-006` | `cancestry_conformance_tp_iso_tp_reassembly` | Invalid PCI nibbles, frame lengths that contradict the PCI, SF/FF mid-session and CF without a session are all refused fail-closed, no crash, no hang |
| `TP-RX-007` | `cancestry_conformance_tp_iso_tp_reassembly` | Faults land in the queue as `FAULT_RAISED` in the FAULT priority class with the `(source_id << 16) \| fault` code and resolve through the name table |
| `TP-RX-008` | `cancestry_conformance_tp_iso_tp_reassembly` | Bounds and helpers: SN wrap-around at 0x0, 120-byte message reassembly, contract edges |
| `TP-TX-001` | `cancestry_conformance_tp_iso_tp_segmentation` | A payload of at most 7 bytes goes out as one Single Frame |
| `TP-TX-002` | `cancestry_conformance_tp_iso_tp_segmentation` | FF + CF segmentation with the rolling SN from 1, bit-exact payload, up to 4095 bytes |
| `TP-TX-003` | `cancestry_conformance_tp_iso_tp_segmentation` | Block Size: exactly BS CFs per Flow Control, BS = 0 unlimited |
| `TP-TX-004` | `cancestry_conformance_tp_iso_tp_segmentation` | STmin pacing in the ms and 100-900 us encodings at 1 ms tick granularity; STmin 0 bursts in one tick |
| `TP-TX-005` | `cancestry_conformance_tp_iso_tp_segmentation` | Flow Control statuses: CTS proceeds, WT is tolerated up to the configured maximum then aborts, OVFLW aborts the sender |
| `TP-TX-006` | `cancestry_conformance_tp_iso_tp_segmentation` | N_Bs expiry (no FC after the FF) aborts with `TRANSPORT_TIMEOUT` at an exact tick |
| `TP-TX-007` | `cancestry_conformance_tp_iso_tp_segmentation` | A sink-declined frame is retried on later ticks and never dropped; permanent decline hits the N_As bound and aborts |
| `TP-TX-008` | `cancestry_conformance_tp_iso_tp_segmentation` | An unexpected FC (no TX session) is a protocol violation, not a crash |
| `TP-TX-009` | `cancestry_conformance_tp_iso_tp_segmentation` | `send()` argument validation: NULL, zero, over-length, over-capacity, busy |
| `UDS-SVC-001` | `cancestry_conformance_uds_services` | RDBI on a static DID returns `62 + DID + data`; malformed requests are 0x13, unknown DIDs 0x31 |
| `UDS-SVC-002` | `cancestry_conformance_uds_services` | RDBI on a signal-mapped DID encodes the live signal value little-endian; no value falls back to stored bytes; unencodable values are 0x22 |
| `UDS-SVC-003` | `cancestry_conformance_uds_services` | WDBI stores exactly the declared length, mirrors into the mapped signal, refuses wrong lengths (0x13) and unknown DIDs (0x31), and reports 0x72 when the signal store is full with no partial effect |
| `UDS-SVC-004` | `cancestry_conformance_uds_services` | RoutineControl start/stop/results with configured records; unknown routine 0x31, unsupported/disallowed sub-function 0x12, malformed 0x13 |
| `UDS-SVC-005` | `cancestry_conformance_uds_services` | Unsupported services (including a zero-length request) answer `7F SID 11` |
| `UDS-SVC-006` | `cancestry_conformance_uds_services` | The UDS loader validates against `schemas/uds-0.1.0.schema.json`: unknown fields, bad versions, duplicates, out-of-range bytes and over-long signal DIDs are refused with located errors |
| `UDS-SVC-007` | `cancestry_conformance_uds_services` | Resource bounds: server init fail-closes on an unresolvable signal mapping and NULL dependencies |
| `UDS-GOV-001` | `cancestry_conformance_uds_governor_integration` | A WDBI for a read-only DID is denied by the governor: NRC 0x22, violation counter, no partial effect |
| `UDS-GOV-002` | `cancestry_conformance_uds_governor_integration` | The FSM-style signal-write allowlist: outside denied with 0x22, inside approved and mirrored into the shared store |
| `UDS-GOV-003` | `cancestry_conformance_uds_governor_integration` | A NULL governor denies every write and routine execution (fail-closed default) |
| `UDS-GOV-004` | `cancestry_conformance_uds_governor_integration` | A routine execution denied by the governor reports NRC 0x22 without running it |
| `UDS-GOV-005` | `cancestry_conformance_uds_governor_integration` | The full stack: a Single Frame WDBI to a read-only DID reassembles, is denied, and the 7F response is segmented back; a multi-frame WDBI (FF + CF) is approved and answered `6E DID` |
| `cancestry_transport_no_malloc_symbols` | `ci/check_no_alloc.py` over `libcancestry_transport.a` | The transport runtime path contains no heap allocation symbol |
| `cancestry_uds_no_malloc_symbols` | `ci/check_no_alloc.py` over `libcancestry_uds.a` | The UDS runtime path contains no heap allocation symbol (the loader is a separate, load-time-only library) |

## 4.4 Phase 11 Bare-Metal & Hard Real-Time HAL test ids ([issue #28](https://github.com/mfreazer/CANcestry-/issues/28))

The Phase 11 bare-metal conformance suite runs against the Cortex-M HAL and watchdog
implementations, verifying hard real-time latency bounds (< 50µs), bounded ISR execution,
independent watchdog (IWDG) timeout reset, safe-state GPIO/transceiver recovery, and
linker-level zero-allocation enforcement.

| Test id | Executable / artifact | Proves |
|---|---|---|
| `BM-LAT-001` | `cancestry_conformance_bm_latency` | Sub-50µs single-frame latency from hardware CAN RX interrupt arrival to FSM event processing |
| `BM-LAT-002` | `cancestry_conformance_bm_latency` | Sustained sub-50µs latency per frame during 20-frame bursts with zero dropped frames |
| `BM-LAT-003` | `cancestry_conformance_bm_latency` | Hardware cycle-counter timestamp monotonicity and seamless 32-bit rollover handling without time glitches |
| `BM-LAT-004` | `cancestry_conformance_bm_latency` | Bounded O(1) ISR execution time with non-blocking fail-closed overflow handling |
| `BM-SAFE-001` | `cancestry_conformance_bm_watchdog` | Regular watchdog feeding during main loop ticks keeps the system alive and operational |
| `BM-SAFE-002` | `cancestry_conformance_bm_watchdog` | Artificially hanging the main loop triggers an IWDG reset and immediate fail-safe state |
| `BM-SAFE-003` | `cancestry_conformance_bm_watchdog` | Safe-state CAN transmission (0 Torque / Contactor Open) emitted upon reset recovery |
| `BM-SAFE-004` | `cancestry_conformance_bm_watchdog` | FSM explicit authorization gate prevents unauthorized CAN transmission after reset |
| `BM-ALLOC-001` | `cancestry_conformance_bm_zero_alloc` | Linker script `cancestry_baremetal.ld` strips allocator functions and rejects dynamic allocation calls with undefined reference errors |
| `cancestry_platform_cortex_m_no_malloc_symbols` | `ci/check_no_alloc.py` over `libcancestry_platform_cortex_m.a` | Bare-metal Cortex-M platform archive contains zero heap allocation symbols |

## 4.5 Phase 12 BMS, HIL & final safety-case test ids ([issue #30](https://github.com/mfreazer/CANcestry-/issues/30))

| Test id | Executable / artifact | Proves |
|---|---|---|
| `BMS-GOV-001` | `cancestry_conformance_bms_governor` | 100 kW thermal request is reduced to the 95 kW current/voltage ceiling and exposes `GOVERNOR_INTERVENTION` |
| `BMS-GOV-002` | `cancestry_conformance_bms_governor` | In-budget normal request is returned unchanged without an intervention |
| `BMS-GOV-003` | `cancestry_conformance_bms_governor` | Faulted BMS snapshot blocks with zero power/torque |
| `BMS-GOV-004` | `cancestry_conformance_bms_governor` | Invalid voltage/efficiency inputs fail closed |
| `BMS-GOV-005` | `cancestry_conformance_bms_governor` | Conservative current rounding and exact current boundary |
| `BMS-GOV-006` | `cancestry_conformance_bms_governor` | Zero demand, saturation and bounded integer arithmetic |
| `BMS-GOV-007` | `cancestry_conformance_bms_governor` | NULL API behavior and stable diagnostic names |
| `BMS-GOV-008` | `cancestry_conformance_bms_governor` | Unknown BMS state fails closed |
| `cancestry_governor_no_malloc_symbols` | `ci/check_no_alloc.py` over `libcancestry_governor.a` | The BMS governor runtime archive contains no allocator symbol |
| `HIL-BUSOFF-001` | `tests/hil/hil_fault_injection.py` | Hardware Bus-Off precedes FSM SAFE_STATE, TX is revoked, and bounded recovery is observed |
| `HIL-CRC-001` | `tests/hil/hil_fault_injection.py` | Hardware CRC rejects a corrupted frame before software queue/FSM processing |
| `HIL-BROWNOUT-001` | `tests/hil/hil_fault_injection.py` | BOR safe latch forces zero torque/open contactors before simulated power-down |
| `HIL-REPORT-001` | `tests/hil/test_phase12_artifacts.py` | HIL backend, limitations, scenario ids and reproduction evidence are documented |
| `SAFETY-MANUAL-001` | `tests/hil/test_phase12_artifacts.py` | Final manual contains architecture/data flow and external-assessor scope |
| `SAFETY-FMEA-001` | `tests/hil/test_phase12_artifacts.py` | Manual contains the FMEA summary |
| `SAFETY-TSR-001` | `tests/hil/test_phase12_artifacts.py` | Manual maps implemented features to ASIL-B-aligned TSRs |
| `TRACE-V1-001` | `tests/hil/test_phase12_artifacts.py` | Final report has 100% in-scope coverage and v1.1.0 deferrals |
| `SAFETY-RELEASE-001` | `tests/hil/test_phase12_artifacts.py` | VERSION, CHANGELOG and matrix have consistent release markers and no failed row |

## 5. Deferred ledger: v1.1.0 requirements with no complete verified artifact yet

Every requirement below is either absent from the CSV or present only with
`planned` rows. None of them is a portable-core requirement, and each is owned
by a later phase. The complete v1.1.0 expansion and rationale are also
reproduced in [`final_v1_report.md`](final_v1_report.md). `ci/check_traceability.py` reads the **first column** of this
table (the rationale column is prose and never counts): ids and inclusive ranges
written there are what the coverage rules treat as deliberately deferred, so a
requirement can only be "not verified" on purpose.

| Deferred requirements | Phase that owns them | Why they are not verified in v1.0.0 |
|---|---|---|
| `SW-FR-PKG-001..007`, `SYS-FR-001`, `SYS-FR-002`, `SYS-FR-017`, `SYS-SF-006`, `SYS-SEC-001`, `SYS-SEC-002`, `SYS-IR-004` | Package loader, integrity and versioning | v1.0.0 loads codec maps, recipes and FSMs as individual declarative files with per-file schema validation. The package manifest, semantic-version compatibility, hash/signature verification, debug-interface policy and the stable-format guarantee for the combined package belong to the package layer. |
| `SW-FR-CAN-001..006`, `SYS-FR-008`, `SYS-IR-001`, `SYS-IR-002`, `SYS-IR-003`, `SYS-IR-006`, `SYS-NF-004`, `SYS-SF-005`, `SYS-SF-007` | CAN integration and platform layer | These need hardware access the core deliberately does not have: bit rates up to 1 Mbps, named physical interfaces, host transport, bus-off recovery, watchdog, error counters and persistent storage. The gateway harness is a logic harness with no bus backend ([smoke test record](../qa/smoke-test-v0.3.0-rc.1.md) section 5.1). |
| `SW-FR-LOG-001..005`, `SYS-FR-011`, `SYS-FR-020`, `SYS-SEC-003` | Logger, log lifecycle and replay | Log formats, retrieval, local clearing, privacy expectations and replay of logged traffic. |
| `SW-FR-SIM-001..004`, `SYS-FR-012`, `SYS-FR-013` | Host simulator and host interface | Package execution against virtual CAN inputs, plus the status/package-management/mode-control interface. `SYS-FR-014` determinism is already claimed by the gateway harness, but its simulator row stays `planned` until the simulator exists. |
| `SW-FR-GOV-001..004`, `SYS-SF-001`, `SYS-SF-003` | Full safety governor | Token buckets, ID-level rate limits and an enforced non-transmitting boot state. v1.0.0 ships the fail-closed stub, verified at stub level by `SW-FR-GOV-005`/`SW-FR-GOV-006` and `SW-FR-FSM-023/039..042`. |
| `SYS-FR-015`, `SYS-FR-016`, `SYS-SF-004` | Mode and fault state machine (SAFE mode) | Boot-into-safe-state, the complete fault-class table and SAFE-mode transitions. The fail-closed half of `SYS-FR-016` is demonstrated today (`GATEWAY-FAILCLOSED-001`); bus-off recovery and storage failure are not. |
| `SW-FR-RECIPE-004` | Recipe engine (timeout watches) | The stub engine has no runtime clock, so a timeout trigger refuses to match (fail closed); documented in [`core/recipe/README.md`](../../core/recipe/README.md). |
| `SYS-FR-007`, `SYS-SF-008`, `SYS-NF-003` | Emulation profile tests, static analysis, performance tooling | The v1.0.0 profile test for `SYS-FR-007`, the "no user code execution" inspection for `SYS-SF-008`, and event-to-action latency measurement for `SYS-NF-003` need later-phase tooling. |
| `SW-FR-HAL-010` | Phase 6 hardware integration | The Linux SocketCAN backend is compiled and linked, but verification against a live `vcan0`/`vcan1` pair requires elevated privileges and kernel modules not present in the CI sandbox; the conformance suite covers behaviour against the mock HAL, and the source is reviewable. See `examples/gateway_real/README.md`. |

## 6. Reproduce and audit

```sh
# Build the portable core and run every registered test, including Phase 12.
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

Release gate: **no `failed` row, 100% passing coverage of the implemented v1
scope, a green gate and accepted target limitations** are prerequisites for a
v1.0.0 release. The complete counts and v1.1.0 ledger are in
[`final_v1_report.md`](final_v1_report.md).

## 7. Change history

| Version | Date | Change |
|---|---|---|
| 1.0.0 | 2026-09-18 | Phase 12 (issue #30): stateless BMS governor, hardware-first HIL fault injection, completed Safety Manual, final v1 report and release artifacts. Coverage is 220 defined, 164 implemented in-scope, 56 explicitly deferred, 275 rows. |
| 0.8.0-wip | 2026-09-18 | Phase 11 (issue #28): Bare-metal ARM Cortex-M port and hard real-time HAL. |
| 0.7.0-wip | 2026-09-18 | Phase 10 (issue #26): ISO-TP transport rows `SW-FR-TP-001..010` and UDS rows `SW-FR-UDS-001..008`, the Phase 10 test-id table in section 4.3, and coverage numbers refreshed to the Phase 10 state (195 defined, 153 traced, 239 rows). |
| 0.6.0-wip | 2026-09-18 | Phase 9 (issue #24): formal-verification artifact ids, ACSL/WP contracts, KLEE harnesses, MISRA driver and safety-manual evidence map. |
| 0.4.0-wip | 2026-09-17 | Phase 7 (issue #20): CAN FD requirement rows `SW-FR-CANFD-001..006`, the Phase 7 test-id table in section 4.1, and coverage numbers refreshed to the post-Phase-6/7 state (167 defined, 125 traced, 176 rows). |
| 0.3.0-rc.1 | 2026-09-16 | Phase 5: gateway integration ids (`GATEWAY-*`), FSM schema finalization, coverage numbers, the deferred ledger in section 5, test-id resolution labels in the artifacts, and the CI gate. |
| 0.3.0 | 2026-09-16 | Phase 4/5 scope: FSM runtime rows (`SW-FR-FSM-001..055`) and the conformance suites. |
| 0.2.0 | 2026-09-13 | Skeleton record: event, codec and recipe rows, QA review items from the v0.2.0 acceptance review. |
