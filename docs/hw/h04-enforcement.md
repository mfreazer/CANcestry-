# H-04 enforcement implementation notes

Version: 0.1.3 — draft, pending QA and Human Reviewer sign-off.
Issue: [#38](https://github.com/mfreazer/CANcestry-/issues/38).

## Requirement coverage

- HW-SF-001..005: safety-mechanism trace links, bridge completeness and honest
  evidence/qualification boundaries.
- HW-FR-001..003, HW-FR-008..010: structured architectural trades and live
  logical-to-Modelica mappings; no new component selections.
- HW-FR-004: transient-source oracle provenance, invariant assertions and
  pulse-coverage limits.
- HW-SF-002 / HW-FR-009: datasheet provenance and an explicit *planned*
  charge-path fidelity step; no new charge-path simulation case in H-04.

## Authorized scope exception: datasheet schema ID

H-04 requests `urn:cancestry:schema:hw:datasheet-extract-0.1.0`. The existing
shared `ci/check_schemas_valid.py` requires a `.schema.json` URL suffix.
The user explicitly chose to keep software gates untouched rather than
allow a compatibility change. Therefore the existing datasheet schema `$id`
URL is retained; the URN change is **blocked pending a separately approved
shared-validator change**. Tracked as [#40](https://github.com/mfreazer/CANcestry-/issues/40),
requested label `bug`, owner `mfreazer`, target milestone
**Hardware v0.1 (virtual bench proven)** ([milestone 1](https://github.com/mfreazer/CANcestry-/milestone/1)).
GitHub returned 403 when applying those issue metadata fields; they are not
yet attached. The reviewer's 2026-09-20 disposition accepts the deferral and
tracking and does not make this administrative limitation a merge blocker.
F2 separately requests the full migration inventory in #40's body; the
prepared body is in [review/issue-40-body.md](review/issue-40-body.md) because
the body-only edit was also denied. Applying it needs maintainer access.
The PDF hash/fallback and closed-object corrections are implemented
independently. Do not claim that the URN migration itself is implemented;
review readiness is not safety approval or permission for an agent merge.

## Repository reconciliation

The actual hardware ledger is `hw/tests/traceability.csv`; its evidence hash
column is `evidence_sha256`. `docs/trace/traceability.csv` is a six-column
software-only ledger with no OR-002 row. H-04 corrections apply to the hardware
ledger and do not alter the software gate or frozen HwRS text. The seed OR-002
row already used CL2, not the CL4 mentioned in the issue.

The strict `OR-NNN` schema requires renumbering planned OR-005b to OR-010.
This does not create a measurement: its validation gap explicitly records
that no first-board data exists. `registry.json` is authoritative;
`registry.csv` and the plan tables are deterministic, checked exports.

Trade selections and criterion weights remain null until SE/QA decide them.
The mandatory external-watchdog requirement is not reopened by the trade.
Datasheet PDF hashes are null where no PDF has been retained; URLs are
fallbacks, **not** invented PDF hashes or claims of manufacturer verification.

## Commands

```sh
python3 ci/check_hw_contracts.py .           # validate contracts and view drift
python3 ci/check_hw_contracts.py . --export  # regenerate CSV and Markdown views
python3 ci/check_capella_model.py .         # live inventories and structural gates
python3 ci/check_hw_traceability.py .       # honest ledger / evidence hashes
python3 -m pytest tests/unit/tools/test_check_hw_contracts.py tests/unit/tools/test_check_capella_model.py tests/unit/tools/test_check_hw_traceability.py
python3 -m pytest hw/tests                 # mandatory omc/FMPy in hw-fast image
```

`CANCESTRY_HW_ALLOW_SKIP=1` is only for explicit local development without
OpenModelica. It is never set by hardware CI and is not simulation evidence.
No new physics-model file, rendered visual, Capella component, software runtime
change or software CI-gate change is part of this issue.

## Existing pulse contract corrections and honest coverage

The H-02 FMU test only checked that `omc` existed. Its seven `tr` parameters
were unused by the Modelica equations, so genuine time-to-peak invariants
would necessarily fail. H-04 connects those already-declared leading-edge
parameters in the **existing** pulse model and executes actual FMI
ModelExchange evaluation of the stateless source. OpenModelica 1.24 CS
cannot step the zero-state source, so the evaluator explicitly requires zero
continuous and discrete state variables and fails closed if that changes. It does not add a plant model, DUT circuit or visuals.

The first review exposed a correctness error in the former issue-specified
5b citation/value. The normative source is now **ISO 16750-2:2012
§4.6.4.2.3 Figure 9/Table 6**, and the suppressed level is corrected from
40 V to **35 V** in the oracle, case and model. Citation correction alone
cannot qualify the existing decay topology: full source/clamp waveform,
timing definitions and loaded response remain pending in [#41](https://github.com/mfreazer/CANcestry-/issues/41).
See [pulse-coverage.md](pulse-coverage.md) for source evidence and exact scope.

Pulse 4 is concretely a **1 ms fall / 20 ms dwell / 1 ms rise** dip from
13.5 V to 6 V and back (recovered at 22 ms). No temperature dependence,
battery ESR shift, alternator recovery or standard multi-stage profile is
modeled. Its modeled portion has **CL0 qualification confidence**: fixture
regression is not an independently qualified oracle.

The aggregate pulse manifest and per-pulse states are **pending**, `pass: false`,
`provisional: true`, CL0, and the HW-FR-004 ledger row is `sim-pending` with no
closure artifact/hash. The new source-evidence schema and checker reject
partial-to-passing promotion even if all numerical regressions are green.
There are no plot-data files/schema on this branch (#36 is still open); future
plots must propagate this pending disposition, not the numerical-run verdict.

The tool-gap checker derives TCL2/TCL3 gaps from the controlled qualification
table and producing tool pins, not from the presence of an oracle ID.
The passing hold-up ledger row and the pending pulse artifact both retain
the OpenModelica gap in their appropriate records. Full-output waivers still
require independently hashed witnesses. Actual trace measurements and runtime
tool versions remain CI artifacts.

## Per-commit CI history (original reviewed sequence)

| Commit | Hosted hw-fast | Retrospective local ledger gate |
|---|---|---|
| `72756ceea1b3830a62cf419f880e012e1031cc5b` | Not run / no check run | Exit 1: source-hash drift after contract migration |
| `1bb1a1a4da2c23682ce67ad4632a5b7175e18701` | Not run / no check run | Exit 1: same source-hash drift |
| `53eb2f5452cc8a09055a10341f2d507a9f8007b9` | [Passed at the tip](https://github.com/mfreazer/CANcestry-/actions/runs/35481146781) | Passed before this review correction |

There was **no green-after-each-commit guarantee**. The first two commits are
not independently green: source manifests were reissued in commit 3. The
review correction is kept in commit 3 and its fresh CI result is reported in
PR #39; these original refs are preserved here for an honest audit trail.


## F5 — recorded commit-layering deviation (H04-D1)

**Scope:** HW-SF-001..005, HW-FR-004 / HW-FR-009 verification changes.
The intended layers were contracts (commit 1), structure (commit 2), and
qualification/evidence (commit 3). The actual third commit also contains
review-driven contract and structural corrections: the standards parameter
extract and BOM/extract provenance support, the pulse source-evidence schema,
and stricter stereotype handling with named bridge/safety negative fixtures.
Those would otherwise belong in layers 1 or 2. Evidence was reissued in layer
3 rather than in each earlier hash-affecting layer, leaving the first two
commits non-green as documented in the SHA/status table above.

**Reason and limitation:** review corrections were consolidated into the last
commit to retain the requested three-commit sequence and the already reviewed
earlier refs. The fixed Arena branch does **not** require this layering
violation and is not an excuse for stale intermediate hashes. This record
acknowledges the deviation; it does not retroactively make those commits green.

**Review consequence:** validate the integrated tip, not isolated/cherry-picked
commits 1 or 2. Their contents are not standalone qualification baselines.
Preserve the original SHA/results, reissue every affected manifest at the
current tip and report fresh CI after corrections. For subsequent work,
reissue hashes in the same commit as the source change and run that commit's
gates before declaring it independently reviewable. This deviation is being
recorded under the reviewer's F5 disposition, not silently waived by the agent.

## Review disposition and positive observations (2026-09-20)

The reviewer's disposition requires the F1 reclassification precondition in
qualification §3, the F2 inventory/body update for #40, an F3 enum decision or
schema-description justification, and this F5 layering record. F4, F6 and F7
remain follow-ups; no implementation or closure of those findings is claimed.
The URN deferral, withdrawn pulse qualification, green tip and honest
per-commit history are accepted as stated in that disposition. Human safety
review/sign-off is still required; no merge or hardware qualification follows
from these administrative dispositions.

The reviewer also explicitly asked the review record to recognize that the
agent:

- withdrew a passing claim rather than defending incomplete evidence;
- corrected a citation instead of rationalizing the discrepancy;
- disclosed the red intermediate-commit history rather than substituting
  the statement that the tip was green.

These are reviewer-supplied observations about evidence discipline, not an
agent's self-approval of the safety case. They remain part of the review
record alongside, rather than being erased by, the findings list.


## N1/N2 pre-merge disposition (2026-09-20)

**N1 — removed the stereotype-string branch, not broadened its grammar.**
`ci/check_capella_model.py::is_safety_mechanism` no longer tokenizes either
`stereotype` or `stereotypes`. They are unqualified strings, not resolved
Capella profile applications. A mechanism must have a typed
`BooleanPropertyValue` named `safety_mechanism` whose value is true, or the
exact `safety_mechanism="true"` attribute (the original H-04 attribute
alternative). The production seed already uses typed properties and is
unchanged. Tests retain the HW-SF-002 → RetentionDomain trace, remove its
Boolean marker and try bare, bracketed, comma-separated and prose stereotype
strings: none may grant coverage. Explicit/typed Boolean positives still pass.

**N2 — actual pulse rejection code and its limits.**
`hw/tests/test_pulse_sim.py::execute_pulse` checks the FMU description before
extraction or native construction: ModelExchange support, zero declared
continuous states, and no discrete-variability variables. Negative metadata
fixtures exercise each guard and prove no extraction/instantiation happens;
the positive metadata fixture distinguishes an algebraic continuous-valued
output from a continuous state. This is not a claim to detect hidden states
omitted from an FMU description, nor a claim that all FMPy use is stateless.
`test_power_sim.py::simulate_fmu` executes the separate stateful hold-up case,
whose independent OR-001 output comparison bounds its current TD1 argument.
Qualification §3.1 still requires reassessment/reclassification for expanded
producing roles. The requested rejection code exists; FMPy stays conditionally
TCL1 for the documented use, with no change to OpenModelica's TCL2 gap and no
promotion of pending pulse evidence.

**Readiness remains in effect.** Per the reviewer, N3/N4/N5, F2's unapplied
issue-body edit and the disclosed H04-D1 layering do not block human safety
review. N4/N5 remain follow-ups, not work claimed complete here. F2 remains
the agreed maintainer publishing action. Resolving N1 and answering N2 is
not human safety sign-off and does not authorize an agent merge.

The reviewer's evidence-culture observations above remain part of this review
trail: withdrawing an unsupported passing qualification, correcting the
standard citation, and disclosing non-green intermediate CI are recorded
alongside the gaps. They are not substitutes for the required safety review.
