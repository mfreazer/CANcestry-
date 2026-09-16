# CANcestry Traceability

| Field | Value |
|---|---|
| Document | CANcestry Traceability Record |
| Version | 0.3.0 |
| Status | Finalized for the v0.3.0 Release Candidate (issue #13) |
| Owner | QA |
| Last Review | 2026-09-16 |

This document accompanies [`traceability.csv`](traceability.csv). The CSV maps
requirements to verification artifacts; this record defines the release scope
so "coverage" is auditable, and it names the verification artifact families.

## CSV format

```csv
requirement_id,artifact_id,verification_method,test_id,status
```

- `requirement_id`: a requirement from `docs/SyRS.md` (SYS-*), `docs/software/SwRS.md`
  (SW-FR-*), or a QA review item (QA-*).
- `artifact_id`: the code artifact, schema, or document that realizes the
  requirement.
- `verification_method`: `test`, `inspection`, or `demonstration`.
- `test_id`: the named test case (or planned test id) that verifies the row.
- `status`: `passing` (executed and green in CI), `planned` (designed, not yet
  realized in v0.3.0), or `failed`.

A requirement may appear in several rows; each row is one independent
verification path. Requirement ids also appear as `artifact_id`s where a
system-level requirement is realized by a software requirement (for example
`SYS-FR-003` realized by `SW-FR-CODEC-001`).

## v0.3.0-rc.1 release scope

The v0.3.0 Release Candidate delivers the **portable core runtime** and proves
it end to end in the gateway integration harness
([issue #13](https://github.com/mfreazer/CANcestry-/issues/13)):

| Area | Requirements | Verification |
|---|---|---|
| Event bus | SW-FR-EVENT-001..006 | `tests/unit/core/event/` |
| Codec engine | SW-FR-CODEC-001..008 | `tests/unit/core/codec/`, opendbc parity |
| Recipe engine | SW-FR-RECIPE-001..003, 005..007 | `tests/unit/core/recipe/` |
| Governor (stub level) | SW-FR-GOV-005, SW-FR-GOV-006 | recipe and FSM governor tests |
| FSM runtime | SW-FR-FSM-001..055 | `tests/unit/core/fsm/`, `tests/conformance/fsm/` |
| Determinism | SYS-NF-001, SYS-FR-014 | golden-output and double-run tests |
| Bounded resources / zero runtime heap | SYS-NF-002 | tripwire tests + `ci/check_no_alloc.py` |
| Observability | SYS-NF-005 | engine/queue/governor counters, gateway report |
| Maintainability | SYS-NF-006 | modular host-only build, no hardware in CI |
| Portability | SYS-NF-007 | pure C99 core, host build of every module |
| Testability | SYS-NF-008 | this record plus one test per high-priority requirement |
| System integration | SYS-FR-003..006, 009..010, 014, 019; SYS-SF-002 | `examples/gateway/` (`GATEWAY-*` tests) |
| FSM schema finalization | SW-FR-FSM-001..003 | `schemas/fsm-0.3.0.schema.json`, `FSM-LOAD-*` |

### Gateway test ids (issue #13)

| Test id | Executable / artifact | Proves |
|---|---|---|
| GATEWAY-LOOP-001 | `cancestry_gateway_loop` | full ingress-decode-queue-recipe/fsm-encode-egress cycle |
| GATEWAY-DETERMINISM-001 | `cancestry_gateway_loop` | two independent runs byte-identical |
| GATEWAY-NO-ALLOC-001 | `cancestry_gateway_loop` + `cancestry_gateway_no_alloc_symbols` | zero heap allocation in the processing loop |
| GATEWAY-FAILCLOSED-001 | `cancestry_gateway_loop` | governor denial and expression fault are recorded, the offending instance is suspended, the loop continues |
| GATEWAY-COUNTERS-001 | `cancestry_gateway_loop` | frame/event/drop/violation counters exposed and exact |
| GATEWAY-TRACE-001 | `cancestry_gateway_loop` | transition/event trace rendered deterministically (golden output) |
| GATEWAY-HOST-001 | host build of `examples/gateway` | every module builds and runs without hardware |

### Out of scope for v0.3.0-rc.1 (deferred, with rationale)

These requirements stay `planned` in the CSV and are owned by later phases;
none of them is a portable-core requirement:

| Requirements | Phase that owns them |
|---|---|
| SW-FR-PKG-001..007 | package loader |
| SW-FR-CAN-001..006 | CAN integration (physical interfaces) |
| SW-FR-LOG-001..005 | logger and replay |
| SW-FR-SIM-001..004 | host simulator |
| SW-FR-GOV-001..004 | full safety governor (token buckets, SAFE mode); stub-level fail-closed behavior is verified via SW-FR-GOV-005/006 and SW-FR-FSM-023/039..042 |
| SW-FR-RECIPE-004 | message timeout watches; documented as deferred in `core/recipe/README.md` (the stub engine has no runtime clock, timeout triggers refuse to match - fail closed) |
| SYS-NF-003 | latency measurement needs performance tooling (later phase) |
| SYS-NF-004 | bus-off recovery needs the CAN integration phase |

## Coverage statement

For the v0.3.0 scope defined above: **every** `SW-FR-*` requirement
(EVENT, CODEC, RECIPE except 004, GOV-005/006, FSM-001..055) and **every**
in-scope `SYS-NF-*` requirement (001, 002, 005, 006, 007, 008) has at least
one `passing` row in [`traceability.csv`](traceability.csv). The deferred
requirements listed above remain `planned` by design, not by omission.
