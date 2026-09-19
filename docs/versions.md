# CANcestry version and release policy

| Field | Value |
|---|---|
| Current release | `1.0.0` |
| Source of truth | [`VERSION`](../VERSION) and root [`CMakeLists.txt`](../CMakeLists.txt) |
| Release date | 2026-09-19 |
| Release gate | QA-EV-01 formally closed; see [`docs/qa/closed-findings.md`](qa/closed-findings.md) |
| Final release | Authorized; `v1.0.0` tag on the release commit |

## Version history

| Version | State | Evidence / decision |
|---|---|---|
| `0.3.0-rc.1` | Previous candidate | Portable runtime, FSM, HAL, transport and UDS baseline. |
| `1.0.0-rc.1` | Superseded candidate | Phase 12 BMS, HIL, safety and traceability evidence present; QA-EV-01 corrected in `core/event/` pending formal QA closure. |
| `1.0.0` | Current release | QA-EV-01 formally closed (merge `ad81ca79d17da6ea6e97a036f9f10ac8555fe87e`, closure record in `docs/qa/closed-findings.md`); final version bump and `v1.0.0` tag executed. |

## Hardware baseline (H-Phase 1)

Hardware documents are versioned independently of the software release
(HW-PLAN §2). The H-Phase 1 baseline committed under issue #33:

| Document | Version | Status |
|---|---|---|
| [`docs/hw/HW-PLAN.md`](hw/HW-PLAN.md) | 1.0.0 | Approved — H-Phase 1 plan of record |
| [`docs/hw/HwRS.md`](hw/HwRS.md) | 0.2.0 | Approved — H-Phase 1 baseline (QA review of requirement text in flight; not modified by H-01) |
| [`docs/hw/mbse-plan.md`](hw/mbse-plan.md) | 0.2.0 | Approved — Capella model plan (H-03 execution) |
| [`docs/hw/virtual-bench-plan.md`](hw/virtual-bench-plan.md) | 0.2.0 | Approved — oracle registry and virtual-bench policy |
| [`HwAGENTS.md`](../HwAGENTS.md) | 1.1.0 | Approved — load-bearing hardware agent policy |

These are document baselines, not release candidates: H-Phase 1 has no
software-release semantics, and the `VERSION`/`CMakeLists.txt` pair above
remains the sole source of truth for software releases.

## Release-candidate rules

1. `VERSION`, CMake project metadata, the SwRS, safety artifacts and traceability
   record must identify the current candidate as `1.0.0-rc.1`.
2. The final release marker must not be inferred from passing host tests alone.
   QA must review the reserved fault-slot behavior, all-fault saturation path,
   HAL safe-state action and IWDG escalation evidence.
3. A final release requires a clean strict build, sanitizer validation, the
   traceability gate, safety/HIL review, and a written QA closure for QA-EV-01.
4. Only after that closure may the maintainer perform the final version bump,
   create the release tag, and close the v1.0.0 milestone.

This file is a release-control record, not an authorization to claim final
1.0.0.
