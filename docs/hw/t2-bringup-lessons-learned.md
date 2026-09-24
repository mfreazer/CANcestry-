# Skill: Debugging Fail-Closed CI Bring-Ups (T2 Virtual Bench, H-08)

**What this is:** the reusable debugging methodology forged during the
T2 bring-up (issue #55/#60, dispatches 9–27, fixes F-2…F-37). It is
written for the next safety-relevant, fail-closed pipeline that has to
go from "never ran" to "green on CI" inside a fixed dispatch budget.
Every rule below cites the dispatch or fix that taught it.

Companions: `t2-bringup-report.md` (chronological facts),
`t2-bringup-handover.md` (operational recipes), and
`t2-bringup-postmortem.md` (retrospective).

---

## 1. The loop

One cycle = one dispatch, and every cycle runs the same shape:

```
human dispatches → verify run-page SHA (≠ the SHA you waited on?)
→ read the failure through every diagnostic channel
→ SOURCE-PIN the root cause (quote the line; name the mechanism)
→ CLASS CHECK (in-lane vs stop-and-flag) BEFORE editing
→ minimal fix + named regression test that FAILS on the old state
→ evidence re-issue iff a PINNED_SOURCE moved (canonical recipe only)
→ full gates (checkers, battery WITH skip counts, ledger, yaml AND bash)
→ docs (report entry, handover status/next/cycles/table row)
→ stage EXPLICIT files → commit → push → PR comment → watch 7 checks
→ ask the human for the next dispatch + "verify the SHA" line
```

**Non-negotiable ordering:** source-pin before edit; class check
before edit; prove the new test fails before it passes. A fix whose
regression test never saw the broken state is a hypothesis, not a fix
(F-37 reconstructed the dispatch-26 block specifically to prove
`bash -n` catches it).

## 2. Fault taxonomy — classify first, type second

| Class | Definition | Action | Examples |
|---|---|---|---|
| **Platform-script / runner** | models, `.resc`/`.repl`, runner, tests, workflow, docs, evidence plumbing | Fix in-lane (pre-authorization), class check documented | F-32…F-37: every fix of 21–27 |
| **Stop-class (Lead SE)** | firmware HardFault, FMU load/step failure, bridge FMI sync failure, schema/oracle defect | STOP, document, surface *before* touching anything | Never hit in 21–27 — the check passed every time |
| **Never-executed path** | code or fixtures that only run on a branch CI has never reached | Fix + write the test that runs that branch with *real* shapes | F-34, F-35, F-36 (see §3) |
| **Validation-coverage gap** | the gate cannot see the fault (yaml ≠ bash; skips ≠ passes) | Fix the *gate*, then the fault | F-37 (yaml passed a bash syntax error); venv `rpds` incident (§6) |
| **Process burn** | zero new information consumed | Disclose, never debug; fix the process | dispatch 22 (stale race), dispatch 26 (self-inflicted parse death) |

The class check is a *pre-authorization condition*, not a formality:
list the touch set, `git diff` the pinned sources (empty diff = no
evidence re-issue required), and if anything smells of firmware/FMU/
schema/oracle → stop and surface. The moment you rationalize "it's
probably fine" is the moment you've broken memo §3.

## 3. The signature pattern: the success path is the least-tested path

Three of the six re-budget fixes were the *same* meta-fault:

- **F-34** — `self.Machine` in scripted models: plausible, mirrored
  resc idioms, **never executed until dispatch 23** killed the process
  on first real bus access. Resc/monitor idioms do not transfer
  between scopes; attributes need source proof or runtime proof.
- **F-35** — `main` unpacked `run_scenario()` wrong and passed only
  the `passed` dict; `passing_document` needs `runlog` too. The whole
  block ran for the first time *after the success criteria passed*
  (dispatch 24) and raised `KeyError: 'bridge_sha256'`.
- **F-36** — two fixtures assumed the *pending* on-disk form; they
  only broke once RUN 1 started writing the *passing* form
  (dispatch 25). The H-07 e2e guard also fired as designed because
  the workflow forgot its env var.

**Rules this earns:**

1. Every green-path step is untested until it runs green once. Treat
   the first green run as a test of more than your fix.
2. Test the *exact* real shapes (return tuples, dict key sets,
   on-disk forms), not paraphrases — F-35's test pins both dicts as
   `run_scenario` actually builds them and reproduces the original
   `KeyError` first.
3. Prefer fixtures that are *state-independent*: inject the field
   under test (`pending_reason`) instead of depending on repo state.
4. Guard the whole class, not just the instance: F-37 added
   `bash -n` over **every** workflow `run:` block, plus a payload
   apostrophe check — because the instance (one comment) was cheap to
   miss and expensive to burn a dispatch on.

## 4. Forensics: what to read, in what order

1. **`gh run list` first — always.** Confirm run id, `headSha`,
   conclusion. Dispatch 22 raced a push and burned a cycle on stale
   code; the SHA check turns that 6-minute waste into a no-op. Tell
   the human: *"verify the run page shows `<sha>` before waiting."*
2. **The one-line state.** `T2-DIAG …` (F-25) compresses console +
   transcript + exit state into ≤1000 chars that survive GitHub's
   annotation truncation. If a pipeline lacks one, build it first —
   its absence cost dispatches 19–21.
3. **Annotations and the step-summary tail** (job logs may be
   egress-blocked; annotations API and job-page HTML are not).
4. **Artifacts, in designed order:** `t2-ci-logs-<sha>` first (F-33
   split it out *because* the main zip died), then the bench bundle.
   The artifact names/sizes alone are evidence: zip failed 19–24 →
   chmod fix (F-35) → 1.9 MB at 25 → 2.1 MB at 27.
5. **Source-pin:** traceback line → read that function *and* its
   caller/callee contracts (unpack order, return tuples, first
   access into a dict) → ask "**when did this line first execute?**"
   That single question resolves most never-run-path faults.
6. **When egress blocks everything:** `github.com` HTML blob pages
   embed `rawLines`; `gh api .../actions/runs/<id>/artifacts` works;
   Renode sources via `gh api search/code` (pin the path first —
   guessed paths 404).

## 5. Cycle (budget) economics

- Dispatches are a SE budget unit. Reconcile against `gh run list`
  before claiming counts; disclose strays (session `35854449839`)
  and burns (22, 26) in the report — the accounting *is* the trust.
- A fault outside your class: stop and document immediately (memo
  §4). Debugging it eats the remaining cycles and still needs Lead SE
  consent.
- A two-run hash mismatch: stop and flag. No test doubles, no gate
  weakening, no "byte-identical modulo timestamps." Rule 11.
- Hard stop at dispatch 30 with a mandatory report, success or not.
- One push = one fully gated commit. The session's pushes are the
  audit trail; never force, never rewrite.

## 6. Gate integrity: a green battery can be a lying battery

- **`importorskip` is a silent failure mode.** A corrupted `rpds`
  native module made `jsonschema` unimportable; every schema test
  *skipped* and the battery still looked green. After ANY venv event,
  run with `-rs` and read the skip list; rebuild the venv from the
  exact CI pins (`capellambse==0.6.17 fmpy==0.3.24 numpy==2.1.3
  jsonschema==4.23.0 pytest==8.3.2 pytest-cov==5.0.0 pyyaml`).
- **YAML-valid ≠ shell-valid.** `yaml.safe_load` passed the
  dispatch-26 breakage all day. Syntax-check what the runner actually
  executes (`bash -n`, F-37).
- **Hash-patching must be keyed and asserted.** The renders-manifest
  patch matched no entry on the first attempt (wrong key `id` vs
  `plot_id`) and silently wrote identical bytes; caught only because
  `git diff` stayed empty when it should have moved. Assert
  `matched == 1`; never trust a quiet script.
- **Hex is `[0-9a-f]`, not "digits and letters."** The F-35 fixture
  used `"t"*64` and schema validation (running for the first time)
  rejected it. The gate caught the test — good — but know your charset.
- **Grep checks must be exact-line** (`grep -c '^…$'`), and evidence
  status greps count occurrences, not vibes.

## 7. Honesty rules that made the record trusted

1. **Stop-and-flag never fake (rule 11):** no test doubles, no gate
   weakening, no hand-edited hashes. Evidence is re-issued only by
   the canonical §5.4 recipe.
2. **Correct overclaims loudly.** Dispatch 18's claimed proof was
   retracted in writing when later artifacts disproved it; the Lead
   SE called that "the culture working." A corrected record ages
   better than a defended one.
3. **Publish every burn** (races, strays, self-inflicted stops) in
   the report with the same detail as the successes — dispatch 26's
   apostrophe is in the log with a forensic walkthrough for a reason.
4. **Ledger/evidence state is runner-owned.** `sim-pending` and
   `status: pending` change only through the pipeline's own paths;
   promotion stays gated on HS-01/HS-02 + human safety reviewer in a
   separate safety PR.
5. **Never claim what only CI can prove.** Local gates prove
   structure; the dispatch proves runtime. Keep the two vocabularies
   separate in the report.

## 8. Environment hazards (see handover §6 for recipes)

- **Workspace resets** rewind the local branch (often to the merge
  base) and may wipe venvs, while the worktree survives. Before
  trusting any `git diff --stat`, check `git log -1` and
  `git rev-parse origin/<branch>`. Recovery: fetch → hash-verify key
  files → copy in-flight files aside → `reset --hard FETCH_HEAD` →
  restore → rebuild venv. Untracked files survive `reset --hard`.
  Six occurrences happened; every one is in report Appendix A.
- **Egress map:** `api.github.com` via `gh` works while the Arena
  GitHub connection is valid; raw.githubusercontent, release CDNs,
  and artifact blob downloads are blocked; `github.com` HTML keeps
  working unauthenticated (`rawLines`).
- **`gh workflow dispatch` is 403** — dispatches are human-triggered
  from the UI. The session's job ends at "push + ask"; never fake a
  trigger or a result.

## 9. Pre-push checklist (run this every fix cycle)

- [ ] Root cause source-pinned: line quoted, mechanism named, "first
      execution" question answered
- [ ] Class check: touch set listed; `git diff` vs `PINNED_SOURCES`
      (empty unless intended); stop-class smell → surface, don't apply
- [ ] Named regression test: **demonstrated FAILING on the broken
      state**, passing after; uses the real shapes/branches
- [ ] If any pinned source moved: canonical §5.4 evidence re-issue,
      manifest patched by `plot_id` with `matched == 1`, status stays
      `pending`
- [ ] Gates: 5 checkers · battery (count **and** `-rs` skips) ·
      hw/tests (43 + 9 pre-existing omc errors) · ledger exact line ·
      evidence status · `yaml.safe_load` · `bash -n` (F-37 tests)
- [ ] Docs: report entry (verbatim diagnostics) + handover status /
      next / cycles / table row
- [ ] Stage explicit paths only (never `git add -A`; `venv3/` stays
      untracked)
- [ ] Push → PR comment (class check, hashes, cycle accounting) →
      watch all 7 checks → MERGEABLE
- [ ] Ask for the next dispatch with the **verify-SHA** line

## 10. One paragraph to keep

Fail-closed bring-ups are won by diagnostics, classification, and
honesty — not by typing faster. Make every failure print its own
postmortem (T2-DIAG), make every fix prove it sees the broken state,
make every gate check the thing that actually executes, and make
every cycle's accounting auditable. The budget is finite; the record
is not — the record is what lets the next session (or the Lead SE)
trust the green.
