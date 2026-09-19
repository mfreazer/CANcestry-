# CANcestry MBSE Plan (Capella / Arcadia)

| Field | Value |
|---|---|
| **Document** | CANcestry MBSE Plan |
| **Version** | 0.3.0 |
| **Status** | Approved — H-Phase 1 baseline |
| **Owner** | System Engineer |
| **Approver** | QA Lead, Release Manager |
| **Last Review** | 2026-09-19 |
| **Repository location** | `docs/hw/mbse-plan.md` |
| **Governing documents** | `docs/hw/HW-PLAN.md` v1.0.0, `docs/qa/hw-validation-matrix.md` v0.1.0 |

## 1. Method and levels

Arcadia four levels per HW-PLAN §6.1:

- **OA** — vehicle/bench/diagnostic actors, operational modes.
- **SA** — system functions, CAN/power/safety interfaces.
- **LA** — power supervisor, CAN PHY, safety monitor, fail-safe latch, external watchdog as logical components.
- **PA** — STM32G474-class MCU, CAN transceivers, regulator, retention domain, external watchdog IC, connector.

## 2. Source-of-truth discipline

Requirements live **only** in `docs/hw/HwRS.md`. Capella requirement objects carry the HwRS ID as a property named **`hwrs_id`** (QA ruling MBSE-Q, confirmed) and link to logical/physical components. CI (`ci/check_capella_model.py` via `capellambse`) fails on:

- Orphan Capella blocks.
- Safety-relevant component without a linked `HW-SF-*` or `HW-FR-*` ID.
- `hwrs_id` property present in the model but absent from `HwRS.md`.
- HwRS ID present in `HwRS.md` but not linked from any Capella element (dangling requirement).
- PA element without an LA parent.

## 3. Safety analysis path

FHA/FMEA/FTA in Capella safety viewpoints (ATICA optional, with documented fallback per HW-PLAN C4). Quantitative SPFM/LFM/PMHF by the Python FMEDA calculator fed by `hw/bom/bom.csv` + `fit-database.csv`; `docs/hw/fmeda.md` is a rendered view only (HW-PLAN §6.4). The calculator's oracle gate is the ISO 26262-5 Annex D worked example (OR-004), executed in CI.

## 4. Bridge to Modelica

Mapping table (Capella LA component → CancestryLib block; port → connector; parametric constraint → parameter binding) maintained in `hw/model/bridge.csv`, schema-validated. CI check verifies every row resolves on both sides or is marked `not_simulated` with rationale. Fallback if automated generation proves impractical: manual mapping with the same CI check (HW-PLAN §6.3).

## 5. Model control

Capella project under `hw/model/capella/`. Git LFS applied if binary payloads exceed 10 MiB total or any single file exceeds 2 MiB (HW-PLAN C5). Model version recorded in `docs/versions.md`. Safety-relevant model changes (safety monitor chain, fail-safe topology, retention domain, watchdog wiring, power supervisor, external watchdog integration) require Human Reviewer sign-off; agents may not self-approve (HW-PLAN §5, §11.1).

**Concurrent edit policy (QA finding MBSE-F1):** Capella models do not merge cleanly at the diagram level. Safety-relevant diagrams shall be edited by one author at a time. The CI parser runs on every PR and rejects malformed models. Diagram conflicts are resolved by re-authoring under a single author, not by textual merge. This policy is mirrored in `HwAGENTS.md` once written, under the rule "one author per safety-relevant diagram".

## 6. Open questions for QA

None outstanding as of v0.3.0. The `hwrs_id` property name is confirmed (QA ruling MBSE-Q).

## 7. Change Log

| Version | Date | Change |
|---|---|---|
| 0.1.0 | 2026-09-19 | Initial draft |
| 0.2.0 | 2026-09-19 | QA review applied: concurrent edit policy for safety-relevant diagrams added (MBSE-F1); `hwrs_id` property name confirmed and CI check list expanded (MBSE-Q); external watchdog added to LA and PA level descriptions; Git LFS threshold quantified; FMEDA oracle gate referenced. |
| 0.3.0 | 2026-09-19 | Capella seed & structural gate baseline (issue #35): recorded Trades T-01..T-04 in Appendix A; documented Capella model seed structure under `hw/model/capella/` and bridge specification `hw/model/bridge.csv`. |

---

## Appendix A. Architectural Trade Studies (T-01..T-04)

Per HW-PLAN §6 and HAD §7, open architectural trades are recorded here with owner, decision criterion, and target resolution phase. Trades are recorded, not decided, in H-Phase 1.

| Trade ID | Description | Owner | Decision Criterion | Target Phase | Status |
|---|---|---|---|---|---|
| **T-01** | **Third CAN Controller**: On-chip 3rd instance (S32K-class) vs. SPI CAN-FD companion IC. | System Engineer | MCU selection, HAL driver complexity, SPI bandwidth, and BOM cost. | H-Phase 2 | Recorded |
| **T-02** | **Watchdog Topology**: Internal IWDG only vs. Internal + External window supervisor IC. | System Engineer / QA Lead | ASIL-B single-fault coverage (HW-SF-005), FIT rate budget, and timebase tolerance. | H-Phase 2 | Recorded |
| **T-03** | **Retention Store**: Supercap populate vs. DNP footprint / ceramic capacitor bank. | System Engineer | Holding retention voltage above $V_{\text{VBAT,min}}$ floor during worst-case loss-of-power event. | H-Phase 2 | Recorded |
| **T-04** | **Termination Default**: Per-channel termination default state during SSN activation. | System Engineer | Bus topology compliance, stub reflections, and bus loading when node is unpowered. | H-Phase 2 | Recorded |
