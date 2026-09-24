# FMEDA — Hardware Architectural Metrics (T0 analysis)

| Field | Value |
|---|---|
| Document | `docs/hw/fmeda.md` |
| Version | 0.1.0 |
| Status | Draft — T0 analysis on the H-01 candidate BOM baseline (`hw/bom/bom.json`) |
| Issue | #56 (H-09) commit 2 |
| Requirements | HW-SF-005 (FMEDA, oracle OR-004), HW-NF-004 (FIT provenance), HW-SF-001, HW-SF-002, HW-SF-003, HW-FR-009 |
| Inputs | `hw/fmeda/fmeda-analysis.csv` (row contract `schemas/hw/hw-fmeda-0.1.0.schema.json`), `hw/bom/fit-database.json` (`schemas/hw/hw-fit-database-0.1.0.schema.json`), `hw/bom/datasheets/extract-iso26262-5-2018-targets.json` |
| Tool | `tools/fmeda-calculator.py` v0.1.0 — tool id `fmeda`, TCL2, `docs/hw/tool-qualification.md` §4.4 |
| Generated views | The two blocks marked `BEGIN/END FMEDA METRICS` and `BEGIN/END FMEDA ROWS` are written by `python3 tools/fmeda-calculator.py --root . --write-doc` and checked byte-for-byte by `--check` in hw-fast gate 1.7. Do not edit them by hand. |

## 1. Scope and honesty statement

This is the first FMEDA of the CANcestry gateway hardware and it is a
**T0 paper analysis**: no vendor part is selected (H-02), no schematic or
netlist exists (H-03), and every failure-mode distribution is an analyst
assumption marked `fmd_basis = assumption` in the CSV. The FIT rates are
SN 29500 base-failure-rate classes curated in `hw/bom/fit-database.json`
(secondary tabulation via TI SLYP685; primary table rows and vendor data
pending H-02). The result is therefore **an ASIL-B alignment check of the
architecture as written in HwRS v0.3.0, not an ASIL-B claim**. Where the
architecture does not reach a target the document says so and records the
gap as a finding; nothing is tuned to pass.

Scope follows the safety goal and safety mechanisms already in the HwRS:

- Safety goal (SafetyManual §1): no unintended torque / contactor closure;
  safe state = 0 torque, contactors open, canonical frame `0x100` (HW-SF-004).
- Analysed elements: the two BOM parts of the H-01 baseline — PRT-001 (MCU,
  150 FIT) and PRT-002 (hold-up capacitor, 2 FIT). The supervisor, external
  watchdog, transceivers and contactor driver named by HW-NF-004 are not yet
  BOM parts and are **not** in the sums; adding them is expected to lower the
  SPFM further until the read-back mechanism of finding FMEDA-F-001 exists.
- Safety mechanisms credited: external window watchdog + NRST safe state
  (HW-SF-003/HW-FR-010/HW-SF-001), retention-record CRC/version check
  (HW-SF-002 → SW-FR-LOG-004), ISO 11898-1 protocol checks (HW-FR-003),
  boot-time retention-record validation.

## 2. Method

ISO 26262-5:2018 Annex B fault classification and Annex C equations, applied
per (component, failure mode) row and summed over safety-related rows only:

| Step | Rule |
|---|---|
| λ per row | `fit_total × fmd_fraction` |
| Not safety related | excluded from every sum |
| Cannot violate the SG, no MPF role | safe fault λ_S |
| Cannot violate the SG, MPF role (safety-mechanism or energy-store fault) | λ_MPF,L = λ × (1 − DC_latent); λ_MPF,DP = λ × DC_latent |
| Can violate the SG, no safety mechanism | single-point fault λ_SPF = λ |
| Can violate the SG, safety mechanism with DC | residual λ_RF = λ × (1 − DC) (C.3); covered part λ × DC becomes multiple-point, latent share (1 − DC_latent) (C.5) |
| SPFM | 1 − Σ(λ_SPF + λ_RF) / Σλ_SR (C.7) |
| LFM | 1 − Σλ_MPF,L / Σ(λ_SR − λ_SPF − λ_RF) (C.8) |
| PMHF | Σ(λ_SPF + λ_RF) + Σλ_MPF,DP × Σλ_MPF,L × T_lifetime, T_lifetime = 1e5 h (ISO 26262-10 simplified dual-point term; explicit parameter, `--lifetime-hours`) |
| MTBF | 1e9 h / Σλ over all parts (reliability figure for `docs/hw/rams-summary.md`); the issue's "1/PMHF" is reported separately as mean time to safety-goal violation |

Diagnostic-coverage values are taken from the ISO 26262-5:2018 Annex D
classes (low 60 %, medium 90 %, high 99 %) as recorded in the targets
extract; no intermediate values are invented. All arithmetic is exact
rational arithmetic (`fractions.Fraction`) with half-up rendering, so the
generated blocks below are byte-identical on every run (HwAGENTS.md rule 5).

## 3. Results

<!-- BEGIN FMEDA METRICS -->
| Quantity | Value | Note |
|---|---|---|
| Parts / rows analysed | 2 / 8 | PRT-001, PRT-002 |
| Total failure rate, all parts | 152.0000 FIT | reliability basis (MTBF) |
| Safety-related failure rate | 152.0000 FIT | excluded (not safety related): 0.0000 FIT |
| Safe faults | 15.0000 FIT | |
| Single-point + residual faults | 36.4500 FIT | numerator of SPFM |
| Multiple-point, detected/perceived | 91.2915 FIT | |
| Multiple-point, latent | 9.2585 FIT | numerator of LFM |
| **SPFM** | **76.02 %** | 1 - (SPF+RF)/SR |
| **LFM** | **91.99 %** | 1 - MPF,L/(SR - SPF - RF) |
| **PMHF** | **36.5345 FIT** (3.653e-08 /h) | SPF+RF 36.4500 FIT + dual-point term 0.0845 FIT (T_lifetime 100000 h) |
| MTBF (reliability, 1e9 h / total FIT) | 6578947.4 h | all parts, constant failure rate |
| Mean time to safety-goal violation (1e9 h / PMHF) | 27371372.0 h | issue #56 '1/PMHF' figure; not the reliability MTBF |

| Target (ASIL B alignment) | Required | Achieved | Verdict |
|---|---|---|---|
| SPFM (ISO 26262-5:2018 Table 4) | >= 90.00 % | 76.02 % | FAIL |
| LFM (Table 5) | >= 60.00 % | 91.99 % | PASS |
| PMHF (Table 6, ASIL B) | < 100.0000 FIT | 36.5345 FIT | PASS |
| PMHF (issue #56 project target) | < 10.0000 FIT | 36.5345 FIT | FAIL |
<!-- END FMEDA METRICS -->

### 3.1 Comparison with the ASIL-B targets

- **SPFM 76.02 % < 90 %: FAIL.** The single-point fault FMEDA-003 (safe-state /
  contactor-enable output stuck active, 30 FIT, no read-back) alone consumes
  19.7 % of the safety-related failure rate. Even with every other residual
  removed the metric cannot reach 90 % until that fault is covered.
- **LFM 91.99 % ≥ 60 %: PASS.** The latent share is dominated by the watchdog-
  covered core faults (5.4 FIT) and the undetected capacitor drift (0.6 FIT).
- **PMHF 36.53 FIT (3.65e-8 /h)**: below the ISO 26262-5:2018 Table 6 ASIL-B
  value of 100 FIT (PASS) but **above the project target of 10 FIT (1e-8 /h)
  stated in issue #56 (FAIL)**. The project target is the Table 6 ASIL-D value
  and is kept as the design goal; it is not reachable while FMEDA-003 is an
  uncovered single-point fault (30 FIT on its own).
- The dual-point PMHF term (0.08 FIT) is negligible against the single-point
  term; the PMHF is a single-point-fault problem, not a latent-fault problem.

## 4. Single-point and residual faults (SPFM numerator, 36.45 FIT)

| Row | Fault | λ_SPF + λ_RF (FIT) | Why it is not covered |
|---|---|---|---|
| FMEDA-003 | Safe-state / contactor-enable output stuck active | 30.0000 | HwRS v0.3.0 has no output read-back or second independent path while the MCU is running; HW-SF-001 only guarantees the passive safe state when the MCU is unpowered or in reset |
| FMEDA-001 | Core/flash/SRAM program-flow corruption, residual of the watchdog | 6.0000 | Watchdog DC 90 % (Annex D medium class): flow corruptions that keep servicing the window |
| FMEDA-004 | FDCAN frame wrong in content but well-formed | 0.3000 | Protocol CRC/format checks (DC 99 %) do not detect a semantically wrong frame |
| FMEDA-002 | Retention record corrupted yet CRC-consistent | 0.1500 | Residual of the CRC/version check (DC 99 %) |

## 5. Latent faults (LFM numerator, 9.2585 FIT)

| Row | Fault | λ_MPF,L (FIT) | Latent because |
|---|---|---|---|
| FMEDA-001 | Watchdog-covered core faults not reported at boot | 5.4000 | Latent DC 90 %: reset-cause/record report at next boot |
| FMEDA-004 | CAN faults covered by protocol checks but not surfaced | 2.9700 | Latent DC 90 %: error counters / bus-off diagnostics |
| FMEDA-008 | Hold-up capacitor parametric drift | 0.6000 | No detection; exposed only by a long supply drop |
| FMEDA-002 | Retention-domain faults covered but not reported | 0.1485 | Latent DC 99 % |
| FMEDA-006 | Hold-up capacitor open | 0.0800 | Latent DC 90 %: record missing at next boot |
| FMEDA-007 | Hold-up capacitor short | 0.0600 | Latent DC 90 %: backup-domain reset flag |

## 6. Row table (generated)

<!-- BEGIN FMEDA ROWS -->
| fmeda_id | part | failure mode | lambda (FIT) | class | safe | SPF+RF | MPF detected | MPF latent | SM (SPF) / DC | latent detection / DC |
|---|---|---|---|---|---|---|---|---|---|---|
| FMEDA-001 | PRT-001 | Core/flash/SRAM fault: corrupted program flow or state (wrong torque/contactor command or loss of the safe-state reaction) | 60.0000 | RF | 0.0000 | 6.0000 | 48.6000 | 5.4000 | External windowed watchdog + NRST-driven safe state (SSN-owned) / 90 % | Reset-cause and retention-record check at boot; safe-state frame 0x100 consumed by SSN (SafetyManual sec. 4.1) / 90 % |
| FMEDA-002 | PRT-001 | VBAT/RTC retention domain fault: backup registers corrupted or backup-domain switch failure (stale or invalid retention record) | 15.0000 | RF | 0.0000 | 0.1500 | 14.7015 | 0.1485 | Retention-record CRC and version check (SW-FR-LOG-004); invalid record -> safe state / 99 % | Boot-time record validation reports the invalid record (diagnostic frame) / 99 % |
| FMEDA-003 | PRT-001 | GPIO/output-stage fault: safe-state or contactor-enable output stuck in the active (non-safe) state | 30.0000 | SPF | 0.0000 | 30.0000 | 0.0000 | 0.0000 | - / 0 % | - / 0 % |
| FMEDA-004 | PRT-001 | FDCAN peripheral fault: corrupted or mistimed frame content (wrong torque/safe-state message on the bus) | 30.0000 | RF | 0.0000 | 0.3000 | 26.7300 | 2.9700 | ISO 11898-1 CRC/format checks and protocol error counters; bus-off -> receiver safe state / 99 % | Error counters / bus-off reported in diagnostics; receiver time-out reaction / 90 % |
| FMEDA-005 | PRT-001 | Faults of peripherals not used by the safety function (ADC, timers, USB, unused ports) | 15.0000 | S | 15.0000 | 0.0000 | 0.0000 | 0.0000 | - / 0 % | - / 0 % |
| FMEDA-006 | PRT-002 | Open circuit / loss of capacitance (no hold-up energy during a supply drop) | 0.8000 | MPF | 0.0000 | 0.0000 | 0.7200 | 0.0800 | - / 0 % | Retention record missing/invalid after a supply event is detected by the boot-time record check / 90 % |
| FMEDA-007 | PRT-002 | Short circuit (VBAT domain pulled down; retention domain lost) | 0.6000 | MPF | 0.0000 | 0.0000 | 0.5400 | 0.0600 | - / 0 % | Backup-domain reset flag / invalid record at boot / 90 % |
| FMEDA-008 | PRT-002 | Parametric drift (capacitance or leakage out of the C_eff / I_leak budget) | 0.6000 | MPF | 0.0000 | 0.0000 | 0.0000 | 0.6000 | - / 0 % | - / 0 % |
<!-- END FMEDA ROWS -->

## 7. Findings (line-anchored; each is an H-02/H-03 input, not a claim)

- **FMEDA-F-001 (SPFM, PMHF):** an output read-back (or a second, independently
  driven enable in series with the contactor path) for the safe-state outputs is
  required before any ASIL-B metric claim; with DC 99 % on FMEDA-003 the
  single-point sum drops from 36.45 FIT to 6.75 FIT (SPFM 95.6 %, PMHF ≈ 6.9 FIT
  < 10 FIT). This is a HwRS change request (new HW-SF row), not a BOM change.
- **FMEDA-F-002 (LFM):** a periodic hold-up self-test (measured VBAT decay
  during a controlled main-rail drop) would move FMEDA-008 from latent to
  detected; low priority (0.6 FIT) but it is the only undetected capacitor mode.
- **FMEDA-F-003 (inputs):** every `fmd_fraction` is an assumption; the vendor
  FMEDA of the selected MCU (H-02) replaces rows FMEDA-001..005 and may change
  the verdicts in either direction. The FIT database entries carry
  `confidence: secondary-tabulation` for the same reason.
- **FMEDA-F-004 (scope):** supervisor, external watchdog IC, CAN transceivers
  and the contactor driver are missing from the BOM and therefore from the
  sums; the FTA cut-set analysis demanded by HW-SF-005 is not part of this
  issue (T0 analysis only).

## 8. Determinism and tool qualification

- `python3 tools/fmeda-calculator.py --root .` prints the JSON report; `--format
  md` prints the two generated blocks; `--check` fails closed if the CSV's
  derived columns or this document's generated blocks drift from the exact
  computation; `--write-doc` regenerates the blocks.
- The calculator is qualified at TCL2 by `tests/unit/tools/test_fmeda_calculator.py`
  (exact-fraction oracle, TI SLYP685 n = 1..4 teaching example, Chalmers 2023
  aggregate example, error paths) — see `docs/hw/tool-qualification.md` §4.4
  including the declared gap on ISO 26262-5:2018 Annex E Table E.1.
- `ci/check_hw_reliability_growth.py` (commit 4) reuses `aggregate()` to
  track the MTBF of this CSV against `hw/fmeda/mtbf-history.csv`.
