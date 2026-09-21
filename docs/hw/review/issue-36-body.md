# H-03 (#36) review record — pull request #45

Reviewer-facing record for the merged H-03 change. Kept in the repository
because the automation token cannot comment on issues (HTTP 403), so the PR
body and this file are the review trail of record.

## 1. What merged

| # | Commit | Contents |
|---|---|---|
| 1 | `9e3ddc0` | `schemas/hw/hw-plot-data-0.1.0.schema.json`; `protocol` in the method enum of the traceability schema **and** `ci/check_hw_traceability.py`; evidence/ledger hash re-issue |
| 2 | `177e3eb` | `HwAGENTS.md` rule 13 (verbatim) + `docs/hw/visual-evidence-plan.md` |
| 3 | `b3344b5` | empty `hw/tests/evidence/renders/manifest.json` |
| 4 | `ab2acdc` | `ci/check_hw_evidence.py` + `hw-fast` gate 6 |
| 5 | `3dbc400` | `tests/unit/tools/test_check_hw_evidence.py` (72 tests) + fixtures + coverage wiring |
| 6 | `d162af0` | `.github/workflows/hw-nightly.yml` (`--rerender --strict`) |

Every commit was re-verified green on its own tree (schema, contracts, ledger,
visual-evidence gate, unit tests).

## 2. Finding H-03-F1: the drift allowance was fail-open (caught in CI)

**What the first CI run did.** `hw-fast` failed on
`test_t24_undeclared_rerender_drift_is_an_error`, which passed locally. The
gate itself was correct; the *wiring* was not:

```yaml
RENDERER_DRIFT_ALLOWED: ${{ contains(github.event.pull_request.body, 'renderer-drift:') && '1' || '0' }}
```

`contains()` is a whole-body substring match, and the phrase appears in normal
prose — including the body of the PR that introduced the rule. So the very PR
that documented "a drift must be declared" silently enabled the allowance and
downgraded a renderer-drift error to a warning. **A contributor quoting the
rule could have disabled the check it describes.**

**Why it was caught.** Because `hw-fast` ran against the PR that defines the
contract: the test asserting the *fatal* case executed with the allowance
enabled by the PR body itself. A green local run could not show this; the
failure mode only exists where the PR body does.

**Fix.**

- The allowance is derived inside the container from a line-anchored pattern
  (`DRIFT_DECL` in `.github/workflows/hw-fast.yml`): leading list markers are
  permitted, the phrase must start a line, and a *reason* is required
  (a bare `renderer-drift:` declares nothing). Push events carry no PR body and
  never allow drift.
- `tests/unit/tools/test_check_hw_evidence.py::test_decl001_drift_declaration_is_line_anchored`
  extracts that pattern from the workflow and pins it against six accepted
  declarations and five non-declarations (empty body, prose mention, code-span
  mention, mid-sentence, bare prefix). The test also asserts that the old
  substring form does not reappear.
- The tests no longer inherit the ambient environment: `FixtureRepo.run` sets
  the complete drift environment (removing the variable first, restoring it
  afterwards), and any value other than the literal `1` fails closed.
- `docs/hw/visual-evidence-plan.md` §6 now states the declaration form
  normatively, and states that a mention is not a declaration.
- The checker's error message tells the author the required form.

**Generalisation for the review trail.** An allowance parsed from free text
must fail closed on the *absence of an explicit, line-anchored declaration with
a required reason*, and the phrase that names the allowance must not itself be
able to grant it. `RENDERER_DRIFT_ALLOWED` now satisfies both.

## 3. N1 disposition (dead stereotype branch)

**Deleted, and recorded.** `docs/hw/h04-enforcement.md` §"N1/N2 pre-merge
disposition (2026-09-20)" states that `ci/check_capella_model.py::
is_safety_mechanism` no longer tokenizes `stereotype` or `stereotypes` — the
branch was removed, not broadened. The function (line 150) accepts only:

- the explicit `safety_mechanism="true"` attribute, or
- a typed `BooleanPropertyValue` named `safety_mechanism` whose value is true.

Negative tests at `tests/unit/tools/test_check_capella_model.py:361-385` remove
the Boolean marker and try bare, bracketed, comma-separated and prose
stereotype strings; none may grant safety coverage. Typed positives still pass.
Nothing to track: the disposition is in the review record and the code no
longer carries the branch.

## 4. Ledger row referenced by the H-03 schema change

```
HW-SF-002,sim,la-comp-retentiondomain,CL2,OR-001,CL3,"passing(sim,CL2,provisional)",hw/tests/evidence/holdup_001.json,sha256:290daa59…,openmodelica: Compiler semantics outside independently validated output remain unqualified; OR-001/OR-002 regressions do not cover all translation and solver behavior.
```

`la-comp-retentiondomain` is a real element id in
`hw/model/capella/cancestry.capella`, not a placeholder. See H-05's record
(`issue-44-body.md` §1) for the rule 7 analysis and the latent placeholder
finding.
