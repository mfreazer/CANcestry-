# T2 Virtual Bench Bring-Up — Postmortem / Retrospective (H-08, issue #55/#60)

**Scope:** dispatches 9–27 of `hw-nightly` on
`arena/01a0cbe2-cancestry` (stop at 20/20 of the original budget,
re-budget 21–30, success criterion met at 25, fully green at 27),
fixes F-2…F-37, PR #59. Companion to the chronological
`t2-bringup-report.md` and the operational `t2-bringup-handover.md`;
methodology lives in `t2-bringup-lessons-learned.md`.

**Outcome:** SUCCESS. Dispatch 27 (run `35937190878` @ `51bd4c3`,
2026-09-24) — first `success` conclusion of the workflow — proved in
one run: live Renode 1.16.1 + FMI co-simulation of the v1.0.0
firmware ELF, the OR-001 invariant
`retention_write < safe_latch < iwdg_fire`, RTC_BKP0R preservation
across the scripted IWDG reset, plant within ±1 mV, RUN-2
byte-identical evidence, in-container suite 46/46 (e2e as a third
determinism probe), and all three CI artifacts persisted.

---

## 1. Timeline in one screen

| Phase | Dispatches | What happened |
|---|---|---|
| Original budget | 9–18 | Foundation → first platform loads → honest stop called at 18/18 with an overclaim corrected in writing |
| Stop tail | 19–20 | Complete include (step0–8), pydev regs, quantum/seed; LoadELF fault → **STOP, report, issue #60** |
| Re-budget, cycles 1–3 | 21–23 | First complete include re-proof → EOF wall (F-33 diagnostics) → stale-race burn (22) → **root cause: `self.Machine`** (23, F-34) |
| Cycle 4 | 24 | **First complete live scenario: invariant HELD** → died in never-run pass path (`KeyError`, F-35) |
| Cycle 5 | 25 | **Success criterion MET**: RUN 1 wrote evidence, RUN 2 byte-identical, artifacts finally landed (chmod fix); 2 never-run fixtures red (F-36) |
| Cycle 6 | 26 | Zero-execution stop: F-36 comment apostrophe truncated the single-quoted payload (self-inflicted; F-37 `bash -n` gates) |
| Cycle 7 | 27 | **FULL GREEN** — every stage, every artifact, suite 46/46 |

Budget honesty: 25 cycles used of 30 (18 at stop + 21–27); burns
disclosed = stale race (22), stray `35854449839` (chargeability with
Lead SE), self-inflicted syntax stop (26); remaining 28–30 optional.

## 2. What the failures had in common

Of the six re-budget fixes (F-32…F-37), the decisive three (F-34,
F-35, F-36) were the **same meta-fault: code or fixtures that had
never executed** because CI had never reached that branch —
a model attribute used only on first real bus access, an evidence
assembly step reachable only after a green scenario, fixtures
assuming the pending on-disk form after the first passing write. The
fourth (F-37) was a **validation-coverage gap**: the gate
(`yaml.safe_load`) structurally could not see a bash-level fault.

Corollary shipped as a standing gate: every workflow `run:` block is
now `bash -n`-checked (F-37 tests), and the e2e probe runs on every
green CI pass (F-36 env), so the success path re-tests itself
forever.

## 3. What worked (keep all of it)

- **Fail-closed design.** Every guard fired as intended and never
  once lied: H-07's e2e refusal to downgrade, the schema's
  pending/passing pairing, ordering checks raising before evidence
  bytes existed, `--check` refusing non-identical evidence, the
  stop-criteria that ended the original budget cleanly.
- **Diagnostics as a product.** The escalation ladder — console
  tail (F-20) → monitor transcript (F-22/23) → T2-DIAG one-liner
  (F-25) → exception-to-file/line + `proc=`/`console-crash=`
  (F-33) — compressed root-cause time from *several dispatches*
  (19–22) to *one* (23). The `/tmp` artifact split (F-33) outlived
  the zip failures it was built to survive.
- **Class-check discipline.** Memo §3's pre-authorization condition
  was run and documented for every fix; all six landed touches were
  provably runner/platform-script class (pinned-source diffs empty
  or intended). The stop-class branch of the tree was never entered —
  because it was checked, not because it was assumed.
- **Honesty compounding.** Retracting dispatch 18's overclaim,
  disclosing every burned cycle and stray dispatch, quoting
  diagnostics verbatim — each made the next report cheaper to trust
  and the Lead SE's re-budget possible.
- **Human-in-the-loop cadence.** Human-triggered dispatches +
  verify-SHA ritual (born from the dispatch-22 race) kept the session
  from burning cycles on stale heads; Release Manager merges stayed
  outside the agent's hands.

## 4. What hurt (fixes already landed)

| Cost | Incident | Now prevented by |
|---|---|---|
| ~3 cycles | Diagnostics too thin to see inside Renode (19–22) | F-20…F-33 ladder + `t2-ci-logs` artifact split |
| 1 cycle | Dispatch 22 raced the F-33 push (stale `2306a36`) | Verify-SHA line in every ask |
| unknown | Silent schema-test skips from broken `rpds` (venv) | Rebuild-at-pins recipe + `-rs` skip audits (lessons §6) |
| 1 cycle | Dispatch 26: raw apostrophe in a single-quoted payload | F-37 `bash -n` + payload guard; apostrophe-free comments |
| near-miss | Renders-manifest patch silently matched no entry | `plot_id` keying + `matched == 1` assertion |
| near-miss | Fixture hex charset (`"t"*64`) | Schema validation runs in the battery — keep it running |

## 5. Metrics worth remembering

- **First green:** dispatch 27 after 27 documented runs; workflow
  conclusion histogram before it: 19 fail, 20 fail, 21 fail,
  22 fail, 23 fail, 24 fail, 25 fail (pytest tail), 26 fail (parse), 27 **success**.
- **Artifact evolution:** zip failed on runs 19/20/21/24 → `/tmp`
  split first landed at 24 (8 KB) → chmod (F-35) landed the full
  bundle at 25 (1,920,932 B) → 27 (2,111,481 B) with evidence JSON.
- **Fix velocity:** root-cause-to-green = 1 dispatch for the
  `self.Machine` fault (diagnosis at 23, proof at 24) once
  diagnostics existed; the pass-path and fixture faults each cost
  exactly one dispatch to surface and one push to fix.
- **Gates at green:** 5/5 checkers; 348 battery tests + 1 e2e
  (runs on CI); hw/tests 43 passed + 9 pre-existing omc errors;
  ledger `sim-pending` exact; evidence `pending` in-repo (passing
  bytes live in the CI artifacts, written by the runner only).

## 6. Open threads (deliberately not closed here)

1. **T4 / CL3:** bench correlation and promotion remain human-gated
   (HwRS policy, HwAGENTS rule 4). This bring-up claims CL2-class
   virtual-bench evidence only.
2. **Optional cycles 28–30:** extra re-provenance or early stop —
   Lead SE's call; hard stop 30.
3. **Stray `35854449839`:** chargeability still with Lead SE.
4. **PR #59:** MERGEABLE, 7/7 at `de27523`-era heads — Release
   Manager merges (never this session).
5. **BOR/power-supervisor, QA-EV-01 stimulus, CAN/thermal:** out of
   H-07 scope per the evidence artifact's `not_covered` — unchanged.

## 7. The one-line retrospective

We got green by treating *diagnostics, classification, and honesty*
as the product: every failure taught the next fix, every fix carried
a test that had seen the broken state, every gate checked what the
machine actually executes, and every cycle's accounting stayed
auditable — so when the success path finally ran, nothing in it was
meeting for the first time except the assertions themselves.
