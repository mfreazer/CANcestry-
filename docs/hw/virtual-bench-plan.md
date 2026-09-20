# CANcestry Virtual Bench Plan

| Field | Value |
|---|---|
| **Document** | CANcestry Virtual Bench Plan |
| **Version** | 0.4.0 |
| **Status** | Draft — H-04, pending QA approval |
| **Owner** | System Engineer |
| **Co-author** | QA Lead (oracle rule, credibility scheme) |
| **Approver** | Release Manager |
| **Last Review** | 2026-09-19 |
| **Repository location** | `docs/hw/virtual-bench-plan.md` |
| **Governing documents** | `docs/hw/HW-PLAN.md` v1.0.0, `docs/qa/hw-validation-matrix.md` v0.1.0 |

## 1. Architecture

The virtual bench couples two simulators over FMI 3.0:

    Renode (virtual MCU target)             OpenModelica (CancestryLib plant)
    - executes v1.0.0 ARM ELF (hash-pinned) - Power: ISO 7637-2/16750-2, hold-up
    - FDCAN, IWDG, DWT, RTC_BKP, GPIO       - Bus: ISO 11898-2 lumped phy
    - fault stimulus injection              - Thermal: RC network
                                            - Safety: supervisor
                 \                                   /
                  \---- FMI 3.0 co-simulation ------/
                                |
                    pytest orchestrator (CI)
                    oracles: OR-xxx registry
                    evidence: hashed traces

The virtual bench executes the **real firmware binary**; it never reimplements firmware logic (HW-PLAN §8.3).

## 2. Firmware provenance

The ELF is built from tag `v1.0.0` with the pinned toolchain image (`ci/docker/`, digest-pinned). Its sha256 is recorded in every evidence artifact.

**Rebuild and retention policy (QA finding VB-F1):** the toolchain image digest is preserved in CI artifacts for the lifetime of the hardware program. Any firmware change — including security patches, toolchain updates, or re-compilation — invalidates all existing T2 rows and requires re-issuing T2 evidence against the new ELF hash. Old rows are marked `superseded` in the traceability CSV, not deleted. Any T2 row whose provenance cannot be reproduced (missing image digest, missing ELF) is `invalidated` and cannot be cited as evidence.

## 3. Model scopes and `not_simulated` rule

- **Renode platform file** models exactly: FDCAN/bxCAN registers used by `hal_stm32.c`, IWDG, RTC backup registers, DWT CYCCNT, reset/GPIO default states. Everything else is `not_simulated` with rationale in the platform file header.
- **CancestryLib** packages: `Power`, `Bus`, `Thermal`, `Safety`. Protocol logic is **out of scope** (stays in C conformance suites).
- CI check: every safety-relevant Capella component maps to a Modelica element or carries `not_simulated` + rationale (HW-PLAN §6.3).

## 4. Oracle registry

`hw/tests/oracles/registry.json` is the normative source of truth, validated by
`schemas/hw/hw-oracle-registry-0.1.0.schema.json`. `registry.csv` is a generated
backward-compatible export, not an editable record. The table below is also
generated; CI rejects drift. OR-010 replaces the former provisional OR-005b
identifier to satisfy the strict `OR-NNN` contract; no measurement or oracle
closure is claimed by this renumbering.

<!-- BEGIN ORACLE REGISTRY -->
| ID | Oracle | Class | Serves | Validation Gap | Source citation |
| --- | --- | --- | --- | --- | --- |
| OR-001 | RC hold-up / energy-balance closed form | analytical | HW-SF-002, HW-SF-004, HW-FR-009 | Idealized ODE; ESR temperature dependence, leakage nonlinearity, and real brownout shapes not modeled. T2 action: correlate with vendor SPICE or bench measurement. | HwRS.md v0.2.0 (2026), HW-SF-002 / HW-FR-009: C dV/dt = -I; or_001_holdup.py independent closed form and RK4 self-check. |
| OR-002 | ISO 7637-2 / 16750-2 tabulated pulse parameters | standard | HW-FR-004 | Idealized unloaded pulse source only; source impedance coupling, full cranking shape, TVS response, parasitics, bursts and T4 correlation remain unvalidated. | ISO 7637-2:2011; ISO 16750-2:2012; H-04 issue #38 specifies Pulse 5b Us=40 V, Ri=0.5 ohm, td=350 ms. |
| OR-003 | ISO 11898-2 bit-timing and level tables | standard | HW-FR-003, HW-FR-007 | Bus model and physical ISO 11898-2 conformance measurements pending. | ISO 11898-2:2016 and HwRS.md HW-FR-003 / HW-FR-007. |
| OR-004 | ISO 26262-5 Annex D worked example (FMEDA tool gate) | standard | HW-SF-005 | FMEDA calculator and independent Annex D regression not implemented. | ISO 26262-5:2018 Annex D worked example; calculator qualification pending. |
| OR-005 | Independent netlist query (KiCad) vs schematic author intent | independent_model | HW-SF-001, HW-FR-008 | Netlist query and independent schematic-intent comparison pending ECAD. | HwRS.md v0.2.0 (2026), HW-SF-001 / HW-FR-008; independent KiCad netlist query planned, no netlist yet. |
| OR-006 | Datasheet sleep/VBAT current tables | standard | HW-FR-005, HW-SF-002, HW-FR-009 | Vendor table/page re-verification and board-current measurement pending. | STMicroelectronics DS12787, STM32G474 VBAT current tables; hw/bom/datasheets/extract-mcu-vbat.json. |
| OR-007 | STM32G4 LSI tolerance datasheet | standard | HW-SF-003 | Vendor table/page re-verification pending; LSI does not qualify the primary watchdog window. | STMicroelectronics DS12787, STM32G474 LSI tolerance table (exact table/page verification pending). |
| OR-008 | STM32G4 BOR level table | standard | HW-SF-004 | BOR threshold spread and physical brownout correlation pending. | STMicroelectronics DS12787, STM32G474 BOR level table; HW-SF-004. |
| OR-009 | External watchdog IC datasheet (window + timebase tolerance) | standard | HW-FR-010, HW-SF-003 | External watchdog vendor, datasheet and timebase/window correlation pending. | HwRS.md v0.2.0 (2026), HW-FR-010 / HW-SF-003; vendor IC selection and datasheet pending. |
| OR-010 | Golden measurement from first fabricated board (post-bench) | golden_measurement | HW-SF-001 | No fabricated-board measurement exists; cannot support CL3 closure. | HwRS.md v0.2.0 (2026), HW-SF-001; planned golden-board measurement, not acquired. Renumbered OR-010 from OR-005b in H-04. |
<!-- END ORACLE REGISTRY -->

A T1/T2 row cannot become `passing` without a registry link (CI-enforced via `oracle_id` column).

**Oracle upgrade path (QA ruling VB-Q2):** when the first board is fabricated and measured, class (d) oracles for physical-layer requirements are supplemented by a class (c) golden-measurement oracle. The class (c) oracle is added to the registry (OR-010, formerly OR-005b) and the traceability row is updated to require the class (c) link for CL3 closure.

## 5. Credibility scheme (CL0–CL3)

- **CL0** informational.
- **CL1** model unit-verified against an (a)/(b) oracle for the claimed behavior class.
- **CL2** CL1 + parameters validated against datasheet/standard tables + CI regression.
- **CL3** CL2 + correlation to physical measurement or an independently authored model.

Assignment rule: requirement's required CL (HwRS `Req. CL`) ≤ model's achieved CL for closure. CL3 is reachable only after bench data exists; until then, CL3-required rows stay `passing (CL2, provisional)` and are listed in `bench-required.md`.

## 6. Determinism and evidence format

Fixed seeds; digest-pinned Docker images; co-simulation master step fixed at 100 µs, FMU internal step ≤ 1 µs.

**Sub-step event injection (QA ruling VB-Q1):** retention-write and fail-safe-latch assertions required by HW-SF-004 are injected into the FMU as **discrete timed events** at their actual occurrence times, not sampled at the 100 µs master step. If discrete event injection cannot be guaranteed at sub-100 µs precision, the master step is reduced to 10 µs **for the HW-SF-004 scenario only**. The energy-balance calculation itself is closed-form (OR-001) and does not depend on the integration step.

Evidence artifact per run: tool digests, ELF sha256, trace CSV sha256, oracle deltas, pass/fail — same layout as `docs/qa/hil-fault-injection-report.md`.

## 7. CI layout

- `hw-fast.yml` (PR): schema checks, Capella parse + orphan/unlinked checks, Modelica compile, FMPy smoke FMU, traceability gate.
- `hw-nightly.yml`: full transient sweep, thermal sweep, T2 scenario replay of the Phase 12 fault set (Bus-Off, CRC storm, brownout) on the virtual bench.

## 8. Exit criteria to physical bench

`docs/qa/hw-validation-matrix.md` §6 triggers (discretization gap, model boundary, oracle absence) are evaluated per requirement at H-Phase 1 exit; results published to `bench-required.md` (seed list in HwRS §5 is the starting set).

### 8.1 Model fidelity roadmap

This is a roadmap, **not** additional verification evidence. Only the two
existing model files are listed below. HW-SF-002 / HW-FR-009 motivate the
hold-up rows; HW-FR-004 motivates the pulse rows. Planned charge-path work
must provide an independently derived oracle before any ledger pass.

| model_file | fidelity_axis | current_oracle | target_oracle | tier | validation_gap |
|---|---|---|---|---|---|
| `hw/model/CancestryLib/Power/Holdup.mo` | Discharge, ESR vs temperature, nonlinear leakage | OR-001 (constant-current discharge only) | OR-001 plus vendor-SPICE / golden measurement | T1 → T4 | Constant ESR/leakage and ideal brownout shapes; model-boundary trigger. No claim of vendor or bench correlation. |
| `hw/model/CancestryLib/Power/Holdup.mo` | **Planned case `holdup_charge_001.simcase.json`: R_path and diode forward bias (iCh > 0)** | OR-001 excludes the conducting charge branch | **OR-001 extended with an independent piecewise charging solution** | **analysis (T0), then T1 regression** | R_path is a budget and unexercised in holdup_001. Charge turn-on/recharge analysis and case are future work, not implemented or passed by H-04. |
| `hw/model/CancestryLib/Power/Holdup.mo` | Supervisor threshold / real brownout and reset sequences | OR-001 discharge; OR-008 planned threshold reference | OR-008 plus golden brownout/reset traces | T2 → T4 | Supervisor threshold spread and firmware/reset-domain behavior are outside this plant; model-boundary and oracle-absence triggers. |
| `hw/model/CancestryLib/Power/PulseISO7637_2.mo` | Peak, time-to-peak, decay constant of unloaded source | OR-002 tabulated parameters | OR-002 plus independent waveform/generator measurement | T1 → T4 | Idealized source, no TVS or load coupling; sub-nanosecond sampling needed for fast pulses. Discretization and model-boundary triggers. |
| `hw/model/CancestryLib/Power/PulseISO7637_2.mo` | Full cranking profile; unsuppressed load-dump pulse 5a | OR-002 reduced cranking / suppressed 5b only | Extended OR-002 with qualified standard/golden waveform | T1 → T4 (H-06) | Multi-stage Ua/Us/t7/t8 cranking and unsuppressed 5a energy/clamping behavior are not closed by the present reduced source. H-06 owns the missing transient fidelity. |

Bus, thermal and supervisor-specific physics models do not yet exist. Their
future fidelity work remains in the bench-trigger planning; this table must
not make nonexistent model files appear implemented. H-03/H-04 no longer
serve as promised Renode or vendor-SPICE completion dates.

## 9. Open questions for QA

None outstanding as of v0.3.0.

## 10. Change Log

| Version | Date | Change |
|---|---|---|
| 0.1.0 | 2026-09-19 | Initial draft |
| 0.2.0 | 2026-09-19 | QA review applied: rebuild and retention policy added (VB-F1); sub-step discrete event injection clarified for HW-SF-004 (VB-Q1); OR-005b class (c) golden-measurement upgrade path added (VB-Q2); oracle registry expanded with OR-007, OR-008, OR-009 to serve HW-SF-003, HW-SF-004, HW-FR-010. |
| 0.4.0 | 2026-09-19 | H-04 #38: schema-validated JSON oracle registry, generated CSV/table, explicit pending-oracle gaps, OR-010 renumbering and tabulated fidelity/charge-path roadmap. |
| 0.3.0 | 2026-09-19 | Residual state reconciliation (issue #35): declared `hw/tests/oracles/registry.csv` source of truth for oracles and added `validation_gap` column to rendered view (§4); added Model fidelity roadmap (§8.1) defining T1 idealizations, trigger classes, and closure steps per `CancestryLib` model. |

