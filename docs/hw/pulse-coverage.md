# HW-FR-004 pulse coverage and normative-source disposition

Version: 0.2.0 — H-06 (#49) pulse 5a disposition and first pending render; H-04
review correction. Qualification is **pending**.
Owner: System Engineer / QA. Follow-up: [#41](https://github.com/mfreazer/CANcestry-/issues/41),
requested assignee `mfreazer`, label `bug`, target milestone
**Hardware v0.1 (virtual bench proven)**. The current integration returned
403 for issue metadata updates; the reviewer has explicitly accepted the
tracking/deferral and does not treat those administrative fields as a blocker.
No pulse qualification has been promoted.

A green waveform regression is not a passing requirement or standard-qualified
oracle. `pulse_7637_001.json` has `status: pending`, `pass: false`,
`provisional: true`, `credibility_level: CL0`; its per-pulse records are
pending, and the H-06 case manifest `pulse_5a_001.json` carries the same
disposition with a mandatory `pending_reason` deferring to #41.
The hardware ledger's HW-FR-004 virtual-bench row is `sim-pending` with no
closure evidence/hash; the 5a case manifest and its pending placeholder plot
are pinned transitively through the aggregate manifest's `source_hashes`.
The numerical tests still run and must pass.

## 1. Pulse 4: exact implemented fixture (not a standard starting profile)

The numbers below come from the existing case and Modelica equations, not a
claim that they are a selected ISO starting profile.

| Region | Time from onset | Voltage / behavior |
|---|---|---|
| Falling edge | 0–1 ms | Linear **13.5 V → 6 V**, duration **1 ms** |
| Dwell | 1–21 ms | **6 V**, duration **20 ms** |
| Rising / recovery edge | 21–22 ms | Linear **6 V → 13.5 V**, duration **1 ms** |
| Recovered source | ≥22 ms | Held at **13.5 V**; no dynamic alternator recovery |

**Not modeled:** temperature dependence, battery ESR variation, loaded source
impedance, alternator recovery dynamics, a multi-stage cranking profile, repeated
starts and DUT behavior. There are no temperature, battery-ESR or alternator
states/coefficients in this source.

**Oracle confidence on this modeled portion: CL0 for qualification.** No
independently qualified standard/measurement oracle establishes that this
1/20/1 ms engineering dip represents the required starting event. CI checks
its numerical behavior (2% peak = ±0.12 V at the 6 V floor; 5% time-to-floor =
±0.05 ms around 1 ms, plus recovery/duration checks). Those are regression
acceptance tolerances, not probability/confidence intervals and not a CL2 claim.

The correct starting-profile target is **ISO 16750-2:2012 §4.6.3.2,
Figure 7 / Table 3 (12 V)**, not §4.6.4. Profile selection and qualification are
outstanding in #41. [8](https://files.infocentre.io/files/docs_clients/126_2007734326_746656_16750-2%20ISO%20Environmental%20conditions%20and%20testing%20for%20electrical....pdf)

## 2. Pulse 5b: correctness finding, not an acceptable citation limitation

**Normative baseline selected:** ISO 16750-2:**2012**, fourth edition,
**§4.6.4.2.3, Figure 9 / Table 6, printed pp. 12–13**, Test B for a 12 V system.
This follows HwRS HW-FR-004's use of ISO 16750-2 for suppressed load dump; no
frozen requirement text is changed. [8](https://files.infocentre.io/files/docs_clients/126_2007734326_746656_16750-2%20ISO%20Environmental%20conditions%20and%20testing%20for%20electrical....pdf)

The prior `ISO 7637-2:2011 §5.6.2 Table 11` attribution was wrong. The 2011
Foreword explicitly says that pulses 4, 5a and 5b are no longer specified by
that edition. This is a verified edition/source error, not an unresolved
choice between equally valid citations. [5](https://webstore.ansi.org/preview-pages/ISO/preview_ISO+7637-2-2011.pdf)

Table 6 distinguishes **unclamped Us = 79–101 V** from **suppressed
Us* = 35 V** for the 12 V system. Ri spans 0.5–4 Ω and td spans 40–400 ms;
footnote a couples the voltage/resistance endpoint choices unless otherwise
agreed. It requires five pulses at one-minute intervals. Thus the prior 40 V
suppressed value was **not** the selected standard's 12 V Test B value.
[8](https://files.infocentre.io/files/docs_clients/126_2007734326_746656_16750-2%20ISO%20Environmental%20conditions%20and%20testing%20for%20electrical....pdf)

**Fixed in H-04 review:** corrected the oracle citation and changed the oracle,
sim case and existing model's suppressed level **40 V → 35 V**. The parameter
name `Us_pulse5b` is retained for compatibility but explicitly means **Us***.
The schema-validated numeric transcription is
`hw/bom/datasheets/extract-iso16750-2-2012.json`. The document reader supplied
text, not retained PDF bytes; `source_pdf_hash: null` with source URL is honest
provenance, not an invented checksum. No copyrighted PDF is committed.

**Not fixed / no pass:** the present source rises linearly over 5 ms, decays
from the suppressed level using tau = td/3 = 116.667 ms for td = 350 ms, then
returns to its baseline. That is not a qualified implementation of Figure 9's
separate unclamped-generator and suppressed waveform. It lacks the clamp
plateau/topology, verified edge/duration definitions, exercised Ri, repeated
pulses and loaded DUT response. Merely correcting 40 V to 35 V does not fix
those correctness/qualification gaps. #41 must close them before promotion.

This uses the reviewer's explicit fallback: **Pulse 5b is provisional and
pending, not passing**, and has a tracked resolution issue. The source/voltage
error is resolved; the full waveform/model correctness question remains open.

## 3. Pulse 5a: H-06 reduced unclamped fixture (not the Figure 8 waveform)

**Normative baseline selected:** ISO 16750-2:**2012**, fourth edition,
**§4.6.4.2.2, Figure 8 / Table 5, printed pp. 11–12**, Test A (without
centralized load dump suppression) for a 12 V system. The standard itself does
not use the legacy names "5a"/"5b"; this repository maps 5a ↔ Test A
(unsuppressed) and 5b ↔ Test B (suppressed, §2 above).
[8](https://files.infocentre.io/files/docs_clients/126_2007734326_746656_16750-2%20ISO%20Environmental%20conditions%20and%20testing%20for%20electrical....pdf)

Table 5 (12 V): unclamped **Us = 79–101 V**, **Ri = 0.5–4 Ω**,
**td = 40–400 ms**, **tr = 10 ms, −5/+0**; minimum test severity is **10
pulses at 1-minute intervals**. Footnote a pairs the upper voltage level with
the upper internal resistance — and the lower with the lower — unless
otherwise agreed. Figure 8 defines the rise time at U = 0.9(Us−UA)+UA and the
duration at U = 0.1(Us−UA)+UA, with UA the supply voltage of the generator in
operation (ISO 16750-1). The schema-validated transcription is the `pulse5a`
block of `hw/bom/datasheets/extract-iso16750-2-2012.json`
(`shape_qualification_pending: true`, issue #41).

**Citation disposition (H-06, #49):** the issue body cited Table 6 for pulse
5a; the normative source places the Test A parameters in **Table 5** (Table 6
is Test B / pulse 5b). The values above were transcribed from the standard,
not from the issue prose.

**Implemented by H-06:** the `PulseISO7637_2` selector-8 branch (same reduced
H-02 convention: absolute level, linear edge, td/3 exponential decay), the
OR-002 `pulse5a` tabulation and the sim-case parameters bound only through the
extract/sim-case chain at the Table 5 lower-bound operating point per footnote
a: **Us = 79 V, td = 350 ms, tr = 5 ms, Ri = 0.5 Ω (unused metadata)**. The
invariant tests in `hw/tests/test_pulse_sim.py` are regression checks against
that tabulation — green numerics do not qualify the waveform.

**Not fixed / no pass:** the fixture lacks the Figure 8 waveform shape and its
0.9/0.1 edge/duration measurement definitions, the unclamped-generator
topology, exercised Ri, the 10-pulse repetition, loaded DUT response and T4
correlation. Qualification remains **CL0 pending**: the `pulse_5a_001.json`
case manifest (`pass: false`, `pending_reason` → #41) and its pending
placeholder render `pulse_5a_001.plot.json` (zero-width hatched band, no data
series) record that disposition. No promotion is authorized; #41 must close
shape qualification first.

## 4. Enforced evidence disposition and visual evidence

`schemas/hw/hw-pulse-evidence-0.1.0.schema.json` closes every structured record.
Partial pulse coverage requires pending/provisional status; an aggregate pass
requires every pulse's coverage and status to permit a pass. Since H-06 the
schema also pairs each `case_id` with a fixed coverage scope: the aggregate
`pulse_7637_001` manifest must record all eight pulses (including `pulse5a`)
and must not carry a blanket `pending_reason`; the `pulse_5a_001` case
manifest must stay pending with a `pending_reason` deferring to #41 and record
exactly `pulse5a`. The traceability
gate validates the pulse manifest even while its ledger row is pending and
checks source hashes, inherited tool gaps and ledger/manifest agreement.
Negative fixtures attempt to promote Pulse 4, Pulse 5b, the aggregate and the
ledger; all must fail. The HW-FR-004 ledger rows stay without closure
evidence/hash (`sim-pending` / `bench-pending`): the honest-ledger gate
rejects evidence on a non-passing row, so the 5a artifacts are pinned
transitively by the aggregate manifest's `source_hashes` instead.

**Visual evidence (H-03 #36 infrastructure; first pending render by H-06).**
The plot-data contract (`schemas/hw/hw-plot-data-0.1.0.schema.json`), the
hash-chain gate (`ci/check_hw_evidence.py`) and the pinned renderer
(`ci/renderers/cancestry-render-modelica.py`) exist per
`docs/hw/visual-evidence-plan.md`. H-06 registers the first view:
`pulse_5a_001.plot.json` → `renders/pulse_5a_001.svg`, a pending placeholder
(no model/oracle data series; a zero-width hatched band over the intended
0–0.365 s window) whose footer and embedded provenance inherit the case
manifest's `pending(virtual_bench,CL0,provisional)` disposition. A plot must
propagate the source/per-pulse `pending` status; a successful `regression_pass`
run log is not authority to emit a `passing` plot. The numerical run log
explicitly carries pending qualification status as well.


Review F3: this source-evidence document is a reusable **qualification
manifest**, not an individual run verdict. Its two states are pending
(qualification absent) and passing (qualified coverage). The schema now
explicitly explains why `failing` is not a valid manifest status. Failed
regressions remain failed tests/CI and cannot authorize accepted passing
evidence; they must not be recoded as pending runs. A future persistent
run-result/plot-data contract must represent such failures explicitly, rather
than overloading this manifest. Negative fixtures reject run-result states
at both aggregate and per-pulse levels.
