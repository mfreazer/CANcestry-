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
| [`docs/hw/mbse-plan.md`](hw/mbse-plan.md) | 0.4.1 | H-04 N1 — remove stereotype-string fallback; Boolean markers and real safety traces only |
| [`docs/hw/virtual-bench-plan.md`](hw/virtual-bench-plan.md) | 0.4.1 | H-04 review — corrected OR-002 source; pulse qualification pending #41 |
| [`docs/hw/tool-qualification.md`](hw/tool-qualification.md) | 0.2.3 | H-04 N2 — point to enforced pulse state rejection, test the boundary and distinguish stateful OR-001 use; F1 remains normative |
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
| [`docs/hw/h04-enforcement.md`](hw/h04-enforcement.md) | 0.1.3 | H-04 N1/N2 — Boolean-only safety markers, bounded FMU-state rejection, continued review readiness and retained evidence-discipline observations |

| `hw/model/CancestryLib/Power/PulseISO7637_2.mo`, `hw/tests/oracles/or_002_pulse7637.py` | 0.2.0 | H-04 — existing pulse leading-edge correction; OR-002 normative ISO 16750-2:2012 source; 35 V suppressed level; incomplete waveform pending #41 |
| `hw/tests/cases/pulse_7637_001.simcase.json`, `schemas/hw/hw-sim-0.1.0.schema.json` | 0.1.0 | H-04 — invariant tolerances and deterministic feature-grid contract |
| `hw/tests/{test_power_sim,test_pulse_sim,test_evidence_determinism}.py` | H-04 | Real FMU invariants, failure fixtures, canonical manifests for both cases |
| `hw/tests/oracles/or_001_holdup.py` | H-04 | Registry path correction only; analytical physics unchanged |
| `ci/check_hw_traceability.py`, `schemas/hw/hw-traceability-0.1.0.schema.json` | 0.1.0 | H-04 — virtual_bench method, tool inheritance and full-output witness contract |
| `hw/tests/traceability.csv`, `hw/tests/evidence/{holdup_001,pulse_7637_001}.json` | H-04 | Hold-up remains provisional CL2; pulse qualification withdrawn to pending/CL0; matching source hashes and separate regression verdicts |

| `schemas/hw/hw-pulse-evidence-0.1.0.schema.json` | 0.1.0 | H-04 F3 — two-state qualification-manifest scope justified; not a run-result schema; partial promotion still fails closed |
| `hw/bom/datasheets/extract-iso16750-2-2012.json` | 0.1.0 | HW-FR-004 Table 6 numeric source transcription, URL fallback (no invented PDF hash) |
| [`docs/hw/pulse-coverage.md`](hw/pulse-coverage.md) | 0.1.1 | Exact Pulse 4/Test B pending scope; F3 qualification-versus-run-status distinction |

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
