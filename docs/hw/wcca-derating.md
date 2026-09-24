# CANcestry WCCA and Derating Analysis (T0)

| Field | Value |
|---|---|
| **Document** | CANcestry Worst-Case Circuit Analysis and Derating Analysis |
| **Version** | 0.1.0 |
| **Status** | Draft — H-09 (issue #56); waiver register pending QA record |
| **Owner** | System Engineer |
| **Approver** | QA Lead (waivers per HW-NF-003) |
| **Last Review** | 2026-09-24 |
| **Repository location** | `docs/hw/wcca-derating.md` |
| **Governing documents** | `docs/hw/HwRS.md` v0.3.0 (HW-NF-001, HW-NF-002, HW-NF-003, HW-NF-004, HW-FR-009, HW-SF-002); `docs/hw/HW-PLAN.md` §6.4/§9; `HwAGENTS.md` rules 1, 2, 4, 15 |
| **Machine-read companions** | `hw/wcca/wcca-analysis.csv` (schema `schemas/hw/hw-wcca-0.1.0.schema.json`), `ci/check_hw_wcca.py` (PR gate), `hw/bom/datasheets/extract-wcca-allowances.json`, `extract-mcu-ratings.json`, `extract-copper-ipc2221.json` |

## 1. Purpose and scope

This is the T0 worst-case circuit analysis (WCCA) and derating analysis of the
hardware baseline recorded in `hw/bom/bom.json`. It proves, by closed-form
calculation against cited extracts, whether every baselined component is
operated inside the HW-NF-003 electrical derating limits and the HW-NF-002
junction-temperature limit at the HW-NF-001 worst-case ambient, and it closes
the last engineering *budget* of the hold-up model — the charge-path series
resistance `R_path` — by a formal analysis (§5, WCCA-R-001).

Scope, honestly stated:

- **Covered:** PRT-001 (STM32G474 candidate MCU: VDD, VBAT, junction
  temperature), PRT-002 (retention hold-up capacitor: DC voltage, body
  temperature, effective capacitance) and the VBAT_RTC charge-path conductor
  (trace/via resistance and continuous current). These are the only parts in
  the authoritative BOM (`hw/bom/bom.json`; there is no `bom.csv`, QA review
  2026-09-22).
- **Not covered (no part exists yet):** buck and LDO regulators, voltage
  supervisor, safe-state network, external watchdog, CAN transceivers,
  contactor driver and the charge-path diode itself. Each receives a
  selection constraint in §6 that the H-02 vendor selection must satisfy; the
  derating rows for those parts are added to `hw/wcca/wcca-analysis.csv` when
  their datasheet extracts exist (HwAGENTS.md rule 2: no number without an
  extract). Their absence keeps HW-NF-003 `analysis-pending` in the ledger.
- **Not a thermal simulation:** the junction temperature of §4 is the
  HW-NF-002 steady-state closed form (`Tj = Ta + theta_JA x P`), a T0 bound.
  HW-NF-002 itself stays `sim-pending` until the T1 thermal RC model exists.

Every value in this document comes from a schema-validated extract under
`hw/bom/datasheets/` or is a line-anchored allowance of §3 (rule 15). The
derating table of §4 is a generated view of the CSV and CI rejects drift.

## 2. Derating rules applied

| Rule | Source | Threshold | Applies to |
|---|---|---|---|
| D1 | HW-NF-003 | continuous voltage and current ≤ **70 %** of the rated maximum | semiconductors, ICs, conductors, every category except capacitors |
| D2 | HW-NF-003 | DC voltage ≤ **80 %** of the rated voltage | capacitors |
| D3 | HW-NF-002 | junction temperature ≤ **80 %** of rated `Tj,max` (for passives: body temperature ≤ 80 % of the upper category temperature) | every component at 85 °C ambient (HW-NF-001) and worst-case bus load |
| D4 | HW-NF-003 | exceptions require a **QA-recorded waiver** in this document (§7) | any FAIL row |

`ci/check_hw_wcca.py` derives the threshold from the (stress, category) pair
of each CSV row, recomputes `derating_ratio = worst_case / rated_max`, and
fails the PR on any ratio above threshold that is not covered by a registered
waiver. A waiver never turns a FAIL into a PASS: the ratio stays on record and
the gate prints the pending-QA state on every run.

## 3. Worst-case conditions and allowance register

Allowances are the envelope the design is verified against. They are not
measurements. Each is line-anchored (rule 15), recorded in
`hw/bom/datasheets/extract-wcca-allowances.json`, and is re-pinned by a vendor
datasheet or by the netlist/layout query (OR-005) at H-02.

| allowance_id | Quantity | Value | Pinned by | Rationale |
|---|---|---|---|---|
| WCCA-A-001 | VDD_3V3 total regulation window | ±3 % (3.201 V … 3.399 V) | LDO datasheet (H-02; no HwRS ID yet, HAD §6 note) | Initial accuracy, line, load and temperature of an automotive LDO; the requirement set has no rail-tolerance ID, so the envelope is declared here. |
| WCCA-A-002 | MCU junction-to-ambient thermal resistance `theta_JA` | ≤ 60 K/W | DS12787 package thermal table once the package is chosen | Upper bound covering the LQFP candidate packages on a 4-layer board. |
| WCCA-A-003 | MCU dissipation in gateway mode `P_mcu` | ≤ 0.20 W (≈ 60 mA at 3.3 V incl. I/O) | HW-NF-005 rail-by-rail power sum (T1 power model, sim-pending) | Bounds self-heating for the HW-NF-002 closed form. |
| WCCA-A-004 | Charge-path diode forward drop `V_F` at 17 µA, 85 °C | ≤ 0.40 V | Diode datasheet `V_F` curve (H-02) | Covers Schottky (typ. 0.05–0.15 V) and silicon (typ. 0.35–0.40 V) candidates; the model idealizes the diode, this allowance bounds the error. |
| WCCA-A-005 | Charge-path conductor geometry | length ≤ 20 mm, width ≥ 0.3 mm, copper ≥ 35 µm (1 oz) | KiCad layout query (OR-005) | Sets the ohmic bound WCCA-R-001 and the ampacity of WCCA-R-004. |
| WCCA-A-006 | Vias in the charge path | ≤ 2 vias, ≤ 1.5 mΩ each at 85 °C | KiCad layout query (OR-005) | 0.3 mm drill, 25 µm plating, 1.6 mm board: 1.2 mΩ at 20 °C. |
| WCCA-A-007 | Hold-up capacitor effective capacitance at 3.4 V bias, 85 °C, end of life | `C_eff ≥ 0.5 x C_nominal` | Capacitor datasheet DC-bias and ageing curves (H-02) | Combined tolerance (−20 %), DC-bias and ageing loss of a class-2 ceramic; a polymer/tantalum part is far inside the bound. |

Fixed inputs (requirement bounds, source `hwers`): ambient 85 °C (HW-NF-001);
`V_main_nominal` 3.3 V, `V_VBAT_min` 1.65 V, `I_VBAT_bound` 12 µA
(`extract-mcu-vbat.json`); `C_nominal` 10 µF, `I_leak_max` 5 µA,
`ESR_budget` 0.02 Ω (`extract-holdup-cap.json`); HW-SF-002 event duration
(ii)+(iii) = 50 ms + 100 ms = 150 ms.

## 4. Derating analysis (generated view of `hw/wcca/wcca-analysis.csv`)

<!-- BEGIN WCCA ROWS -->
| wcca_id | part | component | stress | rated max | basis | worst case | ratio | threshold | verdict | waiver |
|---|---|---|---|---|---|---|---|---|---|---|
| WCCA-001 | PRT-001 | STM32G474 VDD supply pin | voltage | 4 V | absolute-maximum | 3.399 V | 0.849750 | 0.70 | FAIL | WCCA-W-001 |
| WCCA-002 | PRT-001 | STM32G474 VBAT backup-domain pin | voltage | 4 V | absolute-maximum | 3.399 V | 0.849750 | 0.70 | FAIL | WCCA-W-001 |
| WCCA-003 | PRT-001 | STM32G474 junction temperature | temperature | 125 degC | recommended-operating-maximum | 97 degC | 0.776000 | 0.80 | PASS | - |
| WCCA-004 | PRT-002 | Retention hold-up capacitor DC voltage | voltage | 6.3 V | selection-constraint | 3.399 V | 0.539524 | 0.80 | PASS | - |
| WCCA-005 | PRT-002 | Retention hold-up capacitor body temperature | temperature | 125 degC | recommended-operating-maximum | 85 degC | 0.680000 | 0.80 | PASS | - |
| WCCA-006 | NET-VBAT_RTC | VBAT_RTC charge-path conductor (trace and vias) | current | 0.999 A | computed-ampacity | 0.000017 A | 0.000017 | 0.70 | PASS | - |
<!-- END WCCA ROWS -->

Per-row derivation (all arithmetic is reproduced by `ci/check_hw_wcca.py`):

- **WCCA-001 / WCCA-002 — MCU supply pins.** Worst case
  `V_main_max = 3.3 V x 1.03 = 3.399 V` (WCCA-R-002); the VBAT node is
  charged from VDD_3V3 through the unidirectional charge path and can never
  exceed it. Rated maximum is the destructive-stress rating
  `VDD_abs_max = VBAT_abs_max = 4.0 V`. Ratio `3.399 / 4.0 = 0.849750 > 0.70`:
  **FAIL against the blanket 70 % rule**, dispositioned by waiver WCCA-W-001
  (§7). The physics is not marginal — the worst case sits inside the
  recommended operating range (`3.399 / 3.6 = 0.944`) — the rule simply does
  not fit CMOS supply pins whose absolute maximum is only 21 % above nominal.
- **WCCA-003 — MCU junction temperature.**
  `Tj = 85 °C + 60 K/W x 0.20 W = 97 °C` (WCCA-R-003); rated
  `Tj,max = 125 °C` for ordering suffix 7. Ratio `0.776 ≤ 0.80` **PASS** with
  3 K of margin. A suffix-6 part (`Tj,max = 105 °C`) is at
  `85 / 105 = 0.81` before any dissipation and is therefore excluded
  (finding F-001, constraint WCCA-C-003).
- **WCCA-004 — hold-up capacitor DC voltage.** `3.399 V` against the
  selection constraint `V_rated ≥ 6.3 V` (WCCA-C-001: the next standard
  rating above `3.399 / 0.80 = 4.249 V`). Ratio `0.539524 ≤ 0.80` **PASS**.
- **WCCA-005 — hold-up capacitor body temperature.** 85 °C ambient; the
  17 µA continuous current dissipates `I^2 x ESR ≈ 6e-12 W`, so self-heating
  is nil. Rated upper category temperature 125 °C (AEC-Q200 Grade 1, required
  by `bom.json`). Ratio `0.68 ≤ 0.80` **PASS**; Grade 2/3 parts are excluded.
- **WCCA-006 — charge-path conductor current.** Continuous current
  `I_VBAT_bound + I_leak_max = 17 µA`; IPC-2221 external ampacity of the
  WCCA-A-005 minimum trace at 10 K rise `= 0.048 x 10^0.44 x 16.275^0.725 =
  0.999 A` (WCCA-R-004). Ratio `1.7e-5 ≤ 0.70` **PASS**. The charging inrush is
  a microsecond-class transient (`Q = C x V = 34 µC`) bounded by the
  current-limited charger of the HAD §3 topology, not a continuous stress.

Result: 4 of 6 rows PASS; 2 rows (the MCU supply pins) FAIL the blanket rule
and are covered by the proposed waiver WCCA-W-001. **The DoD statement "all
components PASS derating checks" is therefore met only under the waiver, and
the waiver is not QA-approved yet.** This is recorded, not hidden.

## 5. Formal WCCA of the hold-up charge path

The hold-up model (`hw/model/CancestryLib/Power/Holdup.mo`) feeds the VBAT
node from the 3V3 rail through an **ideal diode in series with `R_path`**,
with `ESR` in series with the capacitor and a constant retention load
`I_mcu + I_leak`. Three physical effects hide behind that idealization and are
bounded here: the ohmic resistance of the path (R-001), the diode forward
drop (A-004, R-005) and the capacitance the part really delivers (A-007,
R-005). ESR stays the separate `ESR_budget` parameter (its drop at 17 µA is
0.34 µV, three orders below the sim-case tolerance, see
`extract-holdup-cap.json`).

### 5.1 WCCA-R-001 — ohmic charge-path resistance (closes `R_path`)

`R_path` is the ohmic series resistance of the conducting path: copper trace
plus vias. With the IEC 60028 copper constants
(`rho_20 = 1.7241e-8 Ω·m`, `alpha_20 = 0.00393 /K`,
`extract-copper-ipc2221.json`) and the WCCA-A-005/A-006 layout allowances:

```
R_trace(20 °C) = rho_20 x L / (w x t)
               = 1.7241e-8 x 0.020 / (0.3e-3 x 35e-6)   = 0.032840 Ω
R_trace(85 °C) = R_trace(20 °C) x (1 + alpha_20 x 65 K)  = 0.041229 Ω
R_vias         = 2 x 1.5 mΩ                              = 0.003000 Ω
R_path,wc      = R_trace(85 °C) + R_vias                 = 0.044229 Ω
R_path bound   = 0.05 Ω  (margin 1 − 0.044229 / 0.05 = 11.5 %, i.e. the
                 worst case uses 88.5 % of the bound)
```

The 0.05 Ω value carried by `hw/bom/bom.json` (PRT-002 `R_path`),
`hw/bom/datasheets/extract-holdup-cap.json` and
`hw/tests/cases/holdup_001.simcase.json` is therefore **retained as a WCCA
upper bound** — its citation type changes from `budget` to `wcca_analysis`;
the number does not move, so no simulation artifact changes physically (the
parameter is unexercised in `holdup_001`: the diode is off for the whole
event). The bound holds for any layout meeting WCCA-A-005/A-006, which the
OR-005 layout query verifies at H-02.

What `R_path` does **not** contain: the diode's own conduction. At the
retention current the incremental diode resistance
`r_d = n x V_T / I ≈ 30.9 mV / 17 µA ≈ 1.8 kΩ` (85 °C) dwarfs the copper, so
the recharge tail after an event is governed by the diode
(`tau ≈ r_d x C ≈ 18 ms` toward `V_main − V_F`), not by `R_path`
(`R_path x C = 0.5 µs`). The planned `holdup_charge_001` case
(virtual-bench-plan §8.1) must therefore model the diode characteristic
explicitly; the lumped `R_path` bound is valid only for the ohmic part, which
is exactly how the Modelica block uses it.

### 5.2 WCCA-R-002 — rail window

`V_main_max = 3.3 x 1.03 = 3.399 V`, `V_main_min = 3.3 x 0.97 = 3.201 V`
(WCCA-A-001). `V_main_max` is the voltage stress on the MCU pins and on the
hold-up capacitor (WCCA-001/002/004); `V_main_min` is the starting point of
the hold-up margin (R-005).

### 5.3 WCCA-R-003 — MCU junction temperature and temperature grade

`Tj,wc = T_ambient_max + theta_JA x P_mcu = 85 + 60 x 0.20 = 97 °C`.
HW-NF-002 requires `Tj ≤ 0.8 x Tj,max`, i.e. `Tj,max ≥ 121.25 °C`: the
candidate MCU **must be ordered in temperature range 7 (Tj,max 125 °C) or 3
(130 °C)**; range 6 (105 °C) cannot meet HW-NF-002 at 85 °C ambient even at
zero dissipation. Verified at T1 by the HW-NF-002 thermal RC model.

### 5.4 WCCA-R-004 — charge-path conductor current

Continuous current `I = I_VBAT_bound + I_leak_max = 12 + 5 = 17 µA`.
IPC-2221 external ampacity of the minimum WCCA-A-005 cross-section
(`0.3 mm x 35 µm = 16.275 mil²`) at 10 K rise: `0.999 A`. The conductor
rule (70 %) leaves five orders of magnitude of margin; the inrush at
power-up is a `Q = 34 µC` transient limited by the current-limited charger.

### 5.5 WCCA-R-005 — hold-up margin with a real diode and derated capacitance

The retention requirement HW-SF-002 (ii)+(iii) needs VBAT ≥ 1.65 V for
150 ms at the worst-case load of 17 µA. With the initial voltage lowered by
the rail tolerance and the diode drop:

```
V0,wc   = V_main_min − V_F,max = 3.201 − 0.40 = 2.801 V
dV      = V0,wc − V_VBAT_min   = 2.801 − 1.65  = 1.151 V
t_floor = C_eff x dV / I
```

| C_eff / C_nominal | t_floor | margin vs 150 ms |
|---|---|---|
| 1.00 (model, `holdup_001`, V0 = 3.3 V) | 0.971 s | 6.5x |
| 1.00 (rail −3 %, V_F 0.40 V) | 0.677 s | 4.5x |
| 0.70 | 0.474 s | 3.2x |
| **0.50 (allowance WCCA-A-007)** | **0.339 s** | **2.26x** |

Conclusion: the ideal-diode / nominal-capacitance assumptions of
`holdup_001` do not hide a violation — the retention floor is held with
≥ 2.26x margin even at half the nominal capacitance and a silicon-class
diode drop. The voltage decay during the event is `I x t / C = 0.255 V`
(nominal C), so VBAT stays above the 2.8 V brownout level of sub-event (ii)
and the diode remains off, which is the OR-001 validity condition
(`iCh = 0`). Note that with `V0,wc = 2.801 V` the VBAT node starts essentially
*at* the brownout level: the brownout sub-event contributes no recharge, and
the analysis above already assumes none.

### 5.6 Closed parameters carried by the BOM

<!-- BEGIN WCCA CLOSED PARAMETERS -->
| result_id | parameter | value | unit | carried_by | derivation |
|---|---|---|---|---|---|
| WCCA-R-001 | R_path | 0.05 | ohm | hw/bom/bom.json PRT-002; hw/bom/datasheets/extract-holdup-cap.json; hw/tests/cases/holdup_001.simcase.json | §5.1: R_trace(85 °C) 0.041229 Ω + 2 vias 0.003 Ω = 0.044229 Ω ≤ 0.05 Ω bound |
<!-- END WCCA CLOSED PARAMETERS -->

`ci/check_hw_wcca.py` requires every `citation_type: wcca_analysis`
parameter of `hw/bom/bom.json` to match a row of this table by name, value
and unit, and the BOM citation to name the `WCCA-R-nnn` identifier.

## 6. Selection constraints for H-02 (outputs of this WCCA)

| constraint_id | Part | Constraint | Derived from |
|---|---|---|---|
| WCCA-C-001 | PRT-002 hold-up capacitor | rated DC voltage ≥ 6.3 V; AEC-Q200 Grade 1 (125 °C); `C_eff(3.4 V, 85 °C, EOL) ≥ 5 µF` | D2 on `V_main_max`; D3; WCCA-A-007 / R-005 |
| WCCA-C-002 | Charge-path diode (not in BOM) | `V_F ≤ 0.40 V` at 17 µA and 85 °C; reverse rating ≥ 10 V (D1 on `V_main_max`: ≥ 4.86 V, next standard value); non-repetitive surge rating ≥ charger current limit / 0.70; AEC-Q101 | WCCA-A-004; D1; HW-NF-004 |
| WCCA-C-003 | PRT-001 MCU | ordering temperature range 7 or 3 (`Tj,max ≥ 125 °C`); package `theta_JA ≤ 60 K/W` on the chosen stack-up; `P_mcu ≤ 0.20 W` confirmed by the HW-NF-005 power sum | R-003; WCCA-A-002/A-003 |
| WCCA-C-004 | Layout (VBAT_RTC net) | charge-path trace ≤ 20 mm, ≥ 0.3 mm wide, 1 oz copper, ≤ 2 vias | WCCA-A-005/A-006; R-001 |

## 7. Waiver register (HW-NF-003 exceptions)

Each waiver is one line, referenced by the FAIL rows it covers. `status` is
`proposed` until the QA Lead records the decision (`approved-qa` or
`rejected-qa`) with the review reference in `qa_record`. `ci/check_hw_wcca.py`
fails on an unregistered or rejected waiver and prints proposed waivers as
pending on every run.

<!-- BEGIN WCCA WAIVERS -->
| waiver_id | rows | rationale | status | qa_record |
|---|---|---|---|---|
| WCCA-W-001 | WCCA-001;WCCA-002 | The 70 % rule of HW-NF-003 is a stress-derating rule for discrete semiconductors and conductors; applied to a CMOS supply pin whose absolute maximum (4.0 V) is 21 % above the nominal 3.3 V it is unsatisfiable by construction (limit 2.8 V, below the 1.71 V…3.6 V operating range's own nominal). Disposition applied: supply pins are verified against the manufacturer's recommended operating maximum with the WCCA-A-001 rail window (3.399 V ≤ 3.6 V, 0.944 of VDD_op_max, 0.850 of the absolute maximum) — the treatment of microcircuit supply voltage in established derating practice (e.g. ECSS-Q-ST-30-11C, microcircuits: supply voltage limited to the recommended operating conditions; derating applied to junction temperature, power and output current). Junction temperature (WCCA-003) and output/I/O currents (rows added with the netlist) stay under the full HW-NF-002/HW-NF-003 rules. | proposed | pending QA Lead record (issue #56 review); HwRS HW-NF-003 wording to be refined to "absolute maximum ratings of discrete semiconductors and conductors; IC supply pins per recommended operating conditions" in the next HwRS revision |
<!-- END WCCA WAIVERS -->

## 8. Findings and follow-ups

| id | Finding | Consequence | Owner / where |
|---|---|---|---|
| WCCA-F-001 | An STM32G474 in temperature range 6 (Tj,max 105 °C) cannot meet HW-NF-002 at 85 °C ambient. | Order range 7 or 3 (WCCA-C-003). | H-02 BOM selection |
| WCCA-F-002 | Class-2 ceramic hold-up capacitors lose 20–50 % of nominal capacitance at 3.4 V bias / end of life; HW-FR-009 states the nominal value only. | Selection constraint WCCA-C-001 (`C_eff ≥ 5 µF`); margin still ≥ 2.26x (R-005). Propose adding "effective at bias/EOL" to HW-FR-009 in the next HwRS revision. | H-02; HwRS owner |
| WCCA-F-003 | The charge-path diode is not a BOM part although the model and HAD assume one; `R_path` cannot bound its conduction. | Constraint WCCA-C-002; `holdup_charge_001` must model the diode explicitly (virtual-bench-plan §8.1). | H-02; future T1 case |
| WCCA-F-004 | No HwRS requirement fixes the 3V3 rail tolerance (HAD §6 note); the WCCA had to declare WCCA-A-001. | Add an LDO tolerance requirement (HW-FR-0xx) with the H-02 regulator selection. | HwRS owner |
| WCCA-F-005 | The blanket 70 % voltage rule is unsatisfiable for IC supply pins (waiver WCCA-W-001). | QA decision on the waiver; HW-NF-003 wording refinement. | QA Lead |

## 9. Verification hooks and ledger posture

- PR gate: `python3 ci/check_hw_wcca.py --root .` (wired into
  `.github/workflows/hw-fast.yml`); unit tests
  `tests/unit/tools/test_check_hw_wcca.py`.
- Ledger: `hw/tests/traceability.csv` carries
  `HW-NF-003 / analysis / analysis-pending` — no passing claim, because the
  BOM is incomplete (§1) and WCCA-W-001 awaits QA. HW-NF-002 stays
  `sim-pending` (T1 thermal model not built).
- The `R_path` closure changes `hw/bom/bom.json`, `extract-holdup-cap.json`
  and the BOM/extract schemas, all of which are pinned by
  `hw/tests/evidence/holdup_001.json`; the evidence hash cascade is re-issued
  in the same PR (see `CHANGELOG.md`, H-09).

## 10. Change log

| Version | Date | Change |
|---|---|---|
| 0.1.0 | 2026-09-24 | H-09 (issue #56) commit 1: initial T0 WCCA and derating analysis of the BOM baseline; `R_path` closed (WCCA-R-001, `budget` → `wcca_analysis`); allowance register, selection constraints, waiver WCCA-W-001 (proposed), findings F-001..F-005; CI gate `ci/check_hw_wcca.py`. |
