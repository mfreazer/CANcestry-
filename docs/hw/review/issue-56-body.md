# H-09 (#56) — WCCA + Derating + FMEDA + RAMS Summary: PR record

Reviewer-facing record for the single PR that closes issue #56 (T0 analysis
only; four ordered commits). Kept in the repository next to the H-05 record
(`issue-44-body.md`) so the PR description and the review trail survive the
issue thread.

## 1. Required statements (issue #56 acceptance)

- BOM authority: `bom.json` updated per QA review 2026-09-22.
- Derating thresholds aligned with HW-NF-003: 80% for capacitors, 70% for others.
- FIT database created with curated rates from SN 29500 (base-failure-rate
  classes as tabulated in TI SLYP685; IEC TR 62380 and vendor AEC-Q data are
  declared in `hw/bom/fit-database.json` as pending sources with no entry yet).
- Hash cascade re-issued for tool-qualification v0.2.8. Evidence determinism verified.
- FMEDA calculator qualified per ISO 26262-8 §13 — **with one deviation from
  the issue wording, stated here rather than papered over:** the issue asks for
  "Annex D Table D.1..D.3 reproduced to exact published precision". In ISO
  26262-5:2018 Annex D is the diagnostic-coverage evaluation (Table D.1 = the
  60/90/99 % classes, which the calculator uses as its DC vocabulary) and the
  SPFM/LFM worked example is Annex E Table E.1, whose rows are not publicly
  available and were not obtained. The calculator is therefore qualified
  against the Annex C equations with an exact-arithmetic independent oracle
  and two public fixtures reproduced to exact published precision (TI SLYP685
  n = 1..4 at FIT level and one-decimal percentages; Chalmers 2023 aggregate
  at two decimals), and the Table E.1 gap is declared in
  `docs/hw/tool-qualification.md` §4.4 and OR-004. No statement in this PR
  claims Table E.1 was reproduced.

## 2. Commits (ordered)

| # | Commit | Content |
|---|---|---|
| 1 | WCCA + derating | `docs/hw/wcca-derating.md` v0.1.0 (voltage/current/temperature derating, formal WCCA of the hold-up charge path closing `R_path` = 0.05 Ω bound against 0.044229 Ω worst case), `hw/wcca/wcca-analysis.csv` (6 rows), `schemas/hw/hw-wcca-0.1.0.schema.json`, `ci/check_hw_wcca.py` (+ 41 unit tests, 99 % branch coverage), hw-fast gate 1.6; `hw/bom/bom.json` `R_path` `citation_type: budget → wcca_analysis` (enum added to `hw-bom-0.1.0.schema.json`); new extracts `extract-mcu-ratings.json`, `extract-copper-ipc2221.json`, `extract-wcca-allowances.json`; HW-NF-003 `analysis-pending` ledger row; hash cascade for the bom/schema/extract changes |
| 2 | FMEDA | `hw/bom/fit-database.json` + schema; `bom.json` `fit_source` curated (schema enum `curated`, cross-checked by `check_hw_contracts.py`); `hw/fmeda/fmeda-analysis.csv` + schema; `tools/fmeda-calculator.py` (exact `Fraction` arithmetic, byte-identical output, `--check`/`--write-doc`); `docs/hw/fmeda.md`; `tool-qualification.md` v0.2.8 (`fmeda` TCL2, §4.4); OR-004 re-scoped; fixtures `extract-ti-slyp685-fmeda-example.json`, `extract-chalmers-2023-fmeda-example.json`, `extract-iso26262-5-2018-targets.json`; 44 calculator tests; hw-fast gate 1.7; HW-SF-005 `analysis-pending` ledger row; hash cascade re-issued to fixpoint |
| 3 | RAMS | `docs/hw/rams-summary.md` v0.1.0 and the README "RAMS" section |
| 4 | Reliability growth | `ci/check_hw_reliability_growth.py` (+ 22 tests), `hw/fmeda/mtbf-history.csv` + schema, hw-nightly wiring (`--no-append` on the runner; the PR author commits appended rows) |

## 3. Results (honest ledger, HwAGENTS.md rule 4)

| Quantity | Value | Verdict |
|---|---|---|
| WCCA rows | 6 (4 PASS; WCCA-001/002 FAIL under waiver WCCA-W-001, status `proposed`) | gate PASS with 2 pending-QA warnings |
| `R_path` worst case | 0.044229 Ω vs 0.05 Ω bound (11.5 % margin) | closed, WCCA-R-001 |
| Hold-up floor time at C_eff = 0.5 C, 17 µA | 0.3385 s vs 150 ms need | 2.26× |
| SPFM | 76.02 % vs ≥ 90 % (ASIL B) | **FAIL** — FMEDA-003 (output stuck active, 30 FIT, no read-back) |
| LFM | 91.99 % vs ≥ 60 % | PASS |
| PMHF | 36.53 FIT = 3.65e-8 /h | PASS vs ISO Table 6 ASIL-B (1e-7 /h); **FAIL** vs issue target 1e-8 /h |
| MTBF (reliability) | 6,578,947 h (152 FIT, two-part baseline) | history row 1 |
| Field failures at 10,000 units/yr | 13.3 first year; 39.9 per 36-month cohort | issue arithmetic corrected (150,000 h MTBF would be 584/yr, not 0.6) |

No ASIL-B claim is made. `hw/tests/traceability.csv` rows HW-NF-003 and
HW-SF-005 are `analysis-pending` (waiver pending QA; metrics not met; FTA
cut-set analysis not done).

## 4. Deviations from the issue body (per the SE memo of 2026-09-22 and findings during implementation)

1. `bom.json` is authoritative; there is no `bom.csv` (memo). Stale `bom.csv` /
   `fit-database.csv` mentions in `HW-PLAN.md` (tree listing) and
   `mbse-plan.md` §3 were corrected; the HwRS HW-NF-004 wording still says
   `hw/bom/bom.csv` — HwRS is frozen text, flagged for the next QA HwRS PR.
2. Capacitor derating 80 % (memo), others 70 %, Tj 80 % — enforced by
   `check_hw_wcca.py` rule 3 from the (stress, category) pair; the CSV cannot
   relax a threshold.
3. The blanket 70 % rule is unsatisfiable for a 3.3 V CMOS supply pin whose
   absolute maximum is 4.0 V (limit would be 2.8 V). Rows WCCA-001/002 are
   FAIL under the proposed waiver WCCA-W-001 (verified against the recommended
   operating maximum instead); a waiver never turns FAIL into PASS in the CSV
   and the gate warns until QA records the decision. HwRS wording refinement
   proposed in the waiver text.
4. The FIT database is a **secondary tabulation** of SN 29500 classes (TI
   SLYP685); the primary SN 29500 table rows and vendor FMEDA data are H-02
   inputs, recorded as `confidence: secondary-tabulation`.
5. Annex E Table E.1 not reproduced (see §1). The issue's "1/PMHF" MTBF is
   reported as "mean time to safety-goal violation", separately from the
   reliability MTBF, so the two are never conflated
   (`schemas/hw/hw-mtbf-history-0.1.0.schema.json`).
6. `ci/check_hw_reliability_growth.py` runs with `--no-append` in hw-nightly:
   the runner never rewrites tracked files; it prints the pending row.
   Locally the default appends and the row is committed with the FMEDA change.

## 5. Gates run locally (all green except the toolchain-bound ones)

`check_schemas_valid.py schemas`, `check_hw_contracts.py .` (+ `--export`
idempotent), `check_hw_wcca.py --root .`, `fmeda-calculator.py --root .
--check`, `check_hw_traceability.py .` (7 rows), `check_hw_evidence.py --root
.` and `--rerender --strict`, `check_hw_reliability_growth.py --root .
--no-append`, `hw/tests/test_evidence_determinism.py` (5 passed),
`tests/unit/tools` (624 passed, 1 skipped), hw-fast gate-2 coverage run over
the six included files (99 % branch). `test_power_sim.py`/`test_pulse_sim.py`
and the Capella gate need the pinned container (omc/fmpy/capellambse) and run
in hw-fast.

## 6. Reviewer checklist

- [ ] QA: record WCCA-W-001 (`approved-qa` or `rejected-qa`) in
      `docs/hw/wcca-derating.md`; rejection fails `check_hw_wcca.py`.
- [ ] QA: confirm the FMEDA disposition of FMEDA-003 as a single-point fault
      and open the HwRS change request for finding FMEDA-F-001.
- [ ] Human Reviewer: `tool-qualification.md` §4.4 declared gap (Annex E
      Table E.1) — accept as TCL2 qualification scope or require the licensed
      fixture before merge.
- [ ] Human Reviewer: `docs/hw/rams-summary.md` availability policy statement
      (fail-safe immobilization, no redundancy) — confirm it matches the item
      definition.
