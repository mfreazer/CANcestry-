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
(HW-PLAN §2). The H-Phase 1 baseline committed under issues #33 and #35, with H-04 (#38) refinements pending QA/Human Reviewer approval:

| Document / Artifact | Version | Status |
|---|---|---|
| [`docs/hw/HW-PLAN.md`](hw/HW-PLAN.md) | 1.0.0 | Approved — H-Phase 1 plan of record |
| [`docs/hw/HwRS.md`](hw/HwRS.md) | 0.2.0 | Approved — H-Phase 1 baseline (requirement text frozen; requirement changes via QA PRs) |
| [`docs/hw/mbse-plan.md`](hw/mbse-plan.md) | 0.4.0 | H-04 draft — live bridge gate and generated structured-trade view |
| [`docs/hw/virtual-bench-plan.md`](hw/virtual-bench-plan.md) | 0.4.0 | H-04 draft — JSON oracle registry and fidelity roadmap |
| [`docs/hw/tool-qualification.md`](hw/tool-qualification.md) | 0.1.0 | Approved — ISO 26262-8 §13 TCL classification and OR-001/OR-002 evidence |
| [`HwAGENTS.md`](../HwAGENTS.md) | 1.1.1 | H-04 draft — oracle registry path correction; existing rules unchanged |
| [`hw/model/capella/`](../hw/model/capella/) | 0.2.0 | H-04 draft — safety traces/markers, OA authority constraint, SA firmware-mode aliases; Human Reviewer required |
| [`hw/model/bridge.json`](../hw/model/bridge.json) | 0.1.0 | H-04 draft — live 1:1 bridge and controlled rationale codes |
| [`schemas/hw/hw-datasheet-extract-0.1.0.schema.json`](../schemas/hw/hw-datasheet-extract-0.1.0.schema.json) | 0.1.0 | H-04 correction — PDF hash / URL fallback; URN migration blocked |

| [`hw/tests/oracles/registry.json`](../hw/tests/oracles/registry.json) / generated CSV | 0.1.0 | H-04 — strict IDs/classes/citations/gaps; OR-005b renamed OR-010 |
| [`hw/model/trades.json`](../hw/model/trades.json) | 0.1.0 | H-04 — T-01..T-04 recorded, not decided |
| `schemas/hw/hw-{oracle-registry,bridge,trade}-0.1.0.schema.json` | 0.1.0 | H-04 — closed-object contracts plus cross-record gates |
| `schemas/hw/hw-bom-0.1.0.schema.json`, `hw/bom/datasheets/*.json` | 0.1.0 | H-04 — corrected extract provenance contract, values unchanged |
| `ci/check_hw_contracts.py`, `ci/check_capella_model.py`, hardware checker unit tests | H-04 | Instance/relationship validation; generated-view drift and negative fixtures |
| `ci/docker/Dockerfile`, `.github/workflows/hw-fast.yml` | H-04 | Pinned capellambse 0.6.17; JSON-to-CSV export and contract gate |
| [`docs/hw/h04-enforcement.md`](hw/h04-enforcement.md) | 0.1.0 | H-04 implementation notes and explicit scope exception |

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
