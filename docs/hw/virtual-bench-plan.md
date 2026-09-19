# CANcestry Virtual Bench Plan

| Field | Value |
|---|---|
| **Document** | CANcestry Virtual Bench Plan |
| **Version** | 0.3.0 |
| **Status** | Approved — H-Phase 1 baseline |
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

`hw/tests/oracles/registry.csv` is the normative source of truth for oracles (HW-PLAN §10.2, HwAGENTS.md Rule 3). The table below is a rendered view of that registry:

| ID | Oracle | Class | Serves | Validation Gap |
|---|---|---|---|---|
| OR-001 | RC hold-up / energy-balance closed form | (a) | HW-SF-002, HW-SF-004, HW-FR-009 | Idealized ODE; ESR temperature dependence, leakage nonlinearity, and real brownout shapes not modeled. T2 action: correlate with vendor SPICE or bench measurement. |
| OR-002 | ISO 7637-2 / 16750-2 tabulated pulse parameters | (b) | HW-FR-004 | — |
| OR-003 | ISO 11898-2 bit-timing and level tables | (b) | HW-FR-003, HW-FR-007 | — |
| OR-004 | ISO 26262-5 Annex D worked example (FMEDA tool gate) | (b) | HW-SF-005 | — |
| OR-005 | Independent netlist query (KiCad) vs schematic author intent | (d) | HW-SF-001, HW-FR-008 | — |
| OR-005b | Golden measurement from first fabricated board (post-bench) | (c) | HW-SF-001 | — |
| OR-006 | Datasheet sleep/VBAT current tables | (b) | HW-FR-005, HW-SF-002, HW-FR-009 | — |
| OR-007 | STM32G4 LSI tolerance datasheet | (b) | HW-SF-003 | — |
| OR-008 | STM32G4 BOR level table | (b) | HW-SF-004 | — |
| OR-009 | External watchdog IC datasheet (window + timebase tolerance) | (b) | HW-FR-010, HW-SF-003 | — |

A T1/T2 row cannot become `passing` without a registry link (CI-enforced via `oracle_id` column).

**Oracle upgrade path (QA ruling VB-Q2):** when the first board is fabricated and measured, class (d) oracles for physical-layer requirements are supplemented by a class (c) golden-measurement oracle. The class (c) oracle is added to the registry (e.g., OR-005b) and the traceability row is updated to require the class (c) link for CL3 closure.

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

To address QA's H-01 concern ("no stated path from T1 idealization to T2 fidelity"), each `CancestryLib` plant model defines its current T1 idealizations, the applicable T2/bench trigger class (HW-PLAN §10.4), and the fidelity step required to close the gap:

| Modelica Model | Current T1 Idealizations | Trigger Class (HW-PLAN §10.4) | Fidelity Step to Close |
|---|---|---|---|
| `Power.Holdup` | Constant-current discharge; linear leakage; constant ESR; ideal diode charge path. | Trigger 2 (Model boundary) | Add ESR temperature dependence ($T = -40^\circ\text{C}$ to $+85^\circ\text{C}$), non-linear capacitor leakage vs. voltage, WCCA derating, and correlate with vendor SPICE / bench measurement at H-03. |
| `Power.PulseISO7637_2` | Ideal voltage/current sources; lumped line impedance; ideal step/exponential edges. | Trigger 1 (Discretization gap) & Trigger 2 (Model boundary) | Add real TVS clamping curves, high-frequency parasitic inductance, and oscilloscope trace replay correlation at H-03/H-04. |
| `Bus.LumpedPhy` | Lumped RC/RL bus model; ideal transceiver switching thresholds; no common-mode choke saturation. | Trigger 2 (Model boundary) | Incorporate transceiver loop delay spread, differential-to-common-mode conversion, and physical cable harness measurement correlation at H-03/H-04. |
| `Thermal.RCNetwork` | Lumped 1D RC thermal nodes; constant ambient temperature; fixed thermal conductances. | Trigger 2 (Model boundary) | Include temperature-dependent $R_{\text{DS(on)}}$, PCB thermal copper pour FEM extract, and thermal camera bench calibration at H-03/H-04. |
| `Safety.Supervisor` | Fixed voltage threshold; ideal comparator response; constant propagation delay. | Trigger 1 (Discretization gap) & Trigger 2 (Model boundary) | Add threshold tolerance band across temperature, glitch filter dynamic response, and Renode T2 co-simulation replay at H-04. |

## 9. Open questions for QA

None outstanding as of v0.3.0.

## 10. Change Log

| Version | Date | Change |
|---|---|---|
| 0.1.0 | 2026-09-19 | Initial draft |
| 0.2.0 | 2026-09-19 | QA review applied: rebuild and retention policy added (VB-F1); sub-step discrete event injection clarified for HW-SF-004 (VB-Q1); OR-005b class (c) golden-measurement upgrade path added (VB-Q2); oracle registry expanded with OR-007, OR-008, OR-009 to serve HW-SF-003, HW-SF-004, HW-FR-010. |
| 0.3.0 | 2026-09-19 | Residual state reconciliation (issue #35): declared `hw/tests/oracles/registry.csv` source of truth for oracles and added `validation_gap` column to rendered view (§4); added Model fidelity roadmap (§8.1) defining T1 idealizations, trigger classes, and closure steps per `CancestryLib` model. |

