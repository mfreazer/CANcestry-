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
(HW-PLAN §2). The H-Phase 1 baseline committed under issues #33 and #35:

| Document / Artifact | Version | Status |
|---|---|---|
| [`docs/hw/HW-PLAN.md`](hw/HW-PLAN.md) | 1.0.0 | Approved — H-Phase 1 plan of record |
| [`docs/hw/HwRS.md`](hw/HwRS.md) | 0.2.0 | Approved — H-Phase 1 baseline (requirement text frozen; requirement changes via QA PRs) |
| [`docs/hw/mbse-plan.md`](hw/mbse-plan.md) | 0.3.0 | Approved — Capella model seed & Appendix A Trades T-01..T-04 baseline |
| [`docs/hw/virtual-bench-plan.md`](hw/virtual-bench-plan.md) | 0.3.0 | Approved — oracle registry source of truth (§4) and Model fidelity roadmap (§8.1) |
| [`docs/hw/tool-qualification.md`](hw/tool-qualification.md) | 0.1.0 | Approved — ISO 26262-8 §13 TCL classification and OR-001/OR-002 evidence |
| [`HwAGENTS.md`](../HwAGENTS.md) | 1.1.0 | Approved — load-bearing hardware agent policy |
| [`hw/model/capella/`](../hw/model/capella/) | 0.1.0 | Approved — Arcadia seed model (OA/SA/LA/PA + Capella requirements) |
| [`hw/model/bridge.csv`](../hw/model/bridge.csv) | 0.1.0 | Approved — Capella LA → CancestryLib Modelica bridge mapping |
| [`schemas/hw/hw-datasheet-extract-0.1.0.schema.json`](../schemas/hw/hw-datasheet-extract-0.1.0.schema.json) | 0.1.0 | Approved — datasheet parameter extract schema |

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
