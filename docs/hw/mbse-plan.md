# CANcestry MBSE Plan (Capella / Arcadia)

| Field | Value |
|---|---|
| **Document** | CANcestry MBSE Plan |
| **Version** | 0.4.1 |
| **Status** | Draft — H-04, pending Human Reviewer / QA approval |
| **Owner** | System Engineer |
| **Approver** | QA Lead, Release Manager |
| **Last Review** | 2026-09-20 |
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
- PA realization not resolving to a non-root LA component.
- HW-SF-* requirement without a directed trace path to a logical component
  carrying a `BooleanPropertyValue` named `safety_mechanism` with value true,
  or the exact `safety_mechanism="true"` attribute. Raw `stereotype` /
  `stereotypes` strings do **not** qualify (N1); they are not resolved profile
  applications. The current seed uses the typed-Boolean alternative.
  Requirement existence/name alone is not linkage; cycles and container
  membership do not confer coverage, and broken references fail closed.
- Regulatory authority represented as a functional actor instead of an OA
  Constraint linked to HW-FR-003/004, HW-SF-005 and HW-NF-004.
- SA Mode mappings that drift from the normative firmware FSM.

The SA modes are architectural aliases, not additional firmware states:
`idle → LISTEN_ONLY`, `active → ACTIVE`, `diagnosing → CONFIG`,
`safe-latch → SAFE`. Each Mode carries a `firmware_fsm` link to
`docs/system/mode-fault-state-machine.md#1-system-modes` and a
`firmware_mode` property checked against the normative state list. Diagnostic
access in SAFE and BOOT/OFF transients remain governed by the firmware FSM;
no new firmware transitions or passive-latch recovery path are introduced.

## 3. Safety analysis path

FHA/FMEA/FTA in Capella safety viewpoints (ATICA optional, with documented fallback per HW-PLAN C4). Quantitative SPFM/LFM/PMHF by the Python FMEDA calculator fed by `hw/bom/bom.csv` + `fit-database.csv`; `docs/hw/fmeda.md` is a rendered view only (HW-PLAN §6.4). The calculator's oracle gate is the ISO 26262-5 Annex D worked example (OR-004), executed in CI.

## 4. Bridge to Modelica

`hw/model/bridge.json` is the schema-validated source of truth. CI obtains LA
component names from capellambse (excluding the root container) and top-level
Modelica `model` / `block` declarations from `hw/model/CancestryLib/`.
Every LA component appears exactly once; each existing Modelica block is
referenced exactly once. Unknown names, duplicates and omissions fail closed.
The supported Modelica inventory is file-per-class with an explicit matching
`within` clause; unsupported nested model declarations fail rather than being
silently omitted. Comments, strings and package declarations are not models.

`not_simulated` has a null target and one of `not-yet-modeled`,
`out-of-scope-for-h02`, `emulated-by-other-means`, `not-simulatable`.
A simulated row has rationale `n/a`. Every row retains a QA-readable
`coverage_note`; mapping a stimulus does not claim that the supervisor's
threshold/reset physics are simulated. Implements HW-SF-001..005,
HW-FR-002, HW-FR-004, HW-FR-008 and HW-FR-009.

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
| 0.4.1 | 2026-09-20 | N1: remove the unqualified stereotype-string fallback; retain explicit/typed Boolean markers and real downstream trace checks. |
| 0.4.0 | 2026-09-19 | H-04 #38: JSON bridge and structured trades; generated views, live 1:1 validation, safety-mechanism reachability, authority constraint and firmware-aligned SA modes. |
| 0.3.0 | 2026-09-19 | Capella seed & structural gate baseline (issue #35): recorded Trades T-01..T-04 in Appendix A; documented Capella model seed structure under `hw/model/capella/` and bridge specification `hw/model/bridge.json`. |

---

## Appendix A. Architectural Trade Studies (T-01..T-04)

`hw/model/trades.json` is authoritative; this appendix is a generated view.
Run `python3 ci/check_hw_contracts.py . --export` to regenerate it and the
oracle views. CI rejects manual drift. Trades remain recorded, not decided:
null selection, rejection rationale or weight means pending review, not an
approved choice or equal weighting. Motivating HwRS IDs are in each record.

<!-- BEGIN TRADES -->
| Trade | Motivation | Owner / target | Options / rejection rationale | Selected option | Criteria / weight | Decision rationale |
| --- | --- | --- | --- | --- | --- | --- |
| T-01: Third CAN Controller | HW-FR-001; HW-FR-002 | System Engineer / H-Phase 2 | On-chip third CAN instance (S32K-class): not rejected; pending review<br>SPI CAN-FD companion IC: not rejected; pending review | Pending (recorded, not decided) | MCU/HAL compatibility: pending<br>HAL driver complexity: pending<br>SPI bandwidth: pending<br>BOM cost: pending | Decision and criterion weights remain pending SE/QA review in H-Phase 2; this record is not a component selection. |
| T-02: Watchdog Topology | HW-SF-003; HW-SF-005; HW-FR-010 | System Engineer / QA Lead / H-Phase 2 | Internal IWDG only: Cannot satisfy HW-SF-003 / HW-FR-010: external watchdog is mandatory.<br>Internal IWDG plus external window supervisor IC: not rejected; pending review | Pending (recorded, not decided) | Single-fault coverage: pending<br>FIT rate budget: pending<br>Timebase tolerance: pending | Decision and criterion weights remain pending SE/QA review in H-Phase 2; this record is not a component selection. |
| T-03: Retention Store | HW-SF-002; HW-FR-009 | System Engineer / H-Phase 2 | Populate supercap: not rejected; pending review<br>DNP supercap footprint with ceramic capacitor bank: not rejected; pending review | Pending (recorded, not decided) | VBAT retention floor at worst-case supply loss: pending | Decision and criterion weights remain pending SE/QA review in H-Phase 2; this record is not a component selection. |
| T-04: Termination Default | HW-FR-002; HW-FR-003 | System Engineer / H-Phase 2 | Termination enabled by default: not rejected; pending review<br>Termination disabled by default: not rejected; pending review | Pending (recorded, not decided) | Bus topology compliance: pending<br>Stub reflections: pending<br>Unpowered bus loading: pending | Decision and criterion weights remain pending SE/QA review in H-Phase 2; this record is not a component selection. |
<!-- END TRADES -->
