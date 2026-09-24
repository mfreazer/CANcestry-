# T2 Virtual-Bench Bring-Up Report — H-08 documented outcome (issue #55)

**Status: STOPPED under Lead SE stop criterion 1 — the 20-dispatch
cycle budget (memo 2026-09-22) is exhausted (dispatch 20 of 20,
2026-09-23). NO live co-simulation run was completed: the bring-up
progressed to platform construction + quantum/seed configuration
(`step4`) but never executed the firmware ELF, the FMU coupling, the
invariant, or determinism. The ledger row
`HW-SF-002,virtual_bench` remains `sim-pending` / CL0 with empty
evidence cells (grep-checkable in `hw/tests/traceability.csv`).**

**This is the honest "documented-outcome" report required by the stop
criteria — not the success-path report of deliverable 3. It is
engineering bring-up evidence only: not a requirement-satisfaction
claim, not a safety claim, and it promotes nothing (HwAGENTS.md rules
11/13/14).**

- Continuation issue (next step, per the memo): **#60**
- Bring-up PR (state at stop): **#59** (`arena/01a0cbe2-cancestry` →
  main, head `6662659`, MERGEABLE, all PR checks green at stop)
- Continuity record: `docs/hw/t2-bringup-handover.md`

---

## 1. Outcome (one paragraph)

Across 20 dispatched `hw-nightly` cycles the T2 virtual bench went
from an unprovisioned toolchain to a platform that constructs
**end-to-end in pinned Renode 1.16.1**: machine, NVIC, Cortex-M4F,
memory map, the four scripted PythonPeripherals registered on the
sysbus (`step3c`), and the deterministic quantum/seed configured
(`step4`). Every fault along the way was root-caused against the
authoritative Renode v1.16.1 sources (submodule pin `add012af`) and
fixed in the platform script or runner — F-15…F-31 are closed and
**runtime-proven where applicable** (F-29 at dispatch 19, F-30/F-31 at
dispatch 20). Dispatch 20, the final budgeted cycle, stopped at the
next boundary: `sysbus LoadELF $elf`, a runner-side variable-form bug
(**F-32**, pre-diagnosed to the exact line in #60). The core question
of H-08 — *does the live Renode↔FMU co-simulation actually work?* —
therefore remains **unanswered**, and per the Lead SE's note on #55
that answer ("it has not yet been proven to work, and here is exactly
the next line to fix") is a **successful outcome of this issue,
reported honestly, not a failure to hide**.

## 2. Stop criterion triggered

Lead SE memo (2026-09-22), any one of: (1) 20 total dispatch cycles;
(2) any fault outside the platform script (`.repl`/`.resc`) — firmware
crash, FMU load/step, bridge sync; (3) determinism failure.

**Criterion 1 fired at dispatch 20** (20/20 used). Note for the
record: the dispatch-20 fault itself was *inside* the script-driven
bring-up path (runner↔`.resc` variable form — F-32 in #60), i.e. of
the fixable class; the budget was exhausted first, so criterion 1
governs the stop. Criterion 2 and 3 were never reached: the firmware
never executed, the FMU never loaded, determinism never ran.

## 3. Dispatch history (hw-nightly, workflow_dispatch; 9–20 of the budget)

| # | run | commit | outcome |
|---|---|---|---|
| 9–11 | 35726064092 / 35727339896 / 35730854679 | pre-F-20 | toolchain/preflight failures (F-15…F-20) |
| 12 | 35733480131 | 3f5d0f8 (F-23) | preflight magic 0 — `.resc` not loaded (CWD) |
| 13 | 35773043927 | 22cf6eb (F-23b) | clean-shutdown diagnostic |
| 14 | 35774011082 | 8702ed3 (F-23c) | T2-DIAG annotation line works → F-24 `$ORIGIN` |
| 15 | 35777946390 | 0803c84 (F-24) | first real `.repl` load; **E25** on cpu line (misdiagnosed then) |
| 16 | 35778907099 | 598a360 (F-25) | same E25 — true cause found later: camelCase `performanceInMips` (F-27) |
| 17 | 35805300102 | 4beacd2 (F-27) | E25 gone (CPU constructs); `iwdg` "Could not find source file" (CWD `File.Exists`) → F-28 |
| 18 | 35806027349 | 8269dca (F-28) | `$ORIGIN` PyDevFromFile in `.resc`; **E39** `t2_trace` size not 0x400-aligned → F-29 |
| 19 | 35807326391 | c185cee (F-29) | **`step3: platform loaded`** (F-29 proven); `PyDevFromFile … Parameters did not match the signature` → F-30 (bare name LiteralToken rejected for `string` param); F-31 pre-diagnosed |
| 20 | 35851511484 | 6662659 (F-30/F-31) | **`step3c: 4 pydev registered`; `step4: quantum and seed set`** (F-30/F-31 proven); `sysbus LoadELF $elf … Parameters did not match` → **F-32 → STOP (criterion 1)** |

All outcomes read via the check-run annotations API (run-log fetch is
blocked from the sandbox); the T2-DIAG final-line annotation carries
the console step markers, first transcript error, and runner error.

## 4. Proven at stop (each runtime-observed or CI-enforced)

1. **T2 toolchain provisioning for CI** — `ci/docker/Dockerfile.t2`
   builds without `CANCESTRY_HW_ALLOW_SKIP=1`; every dispatched run
   executed in the provisioned image (identity pins in §6).
2. **Off-tree v1.0.0 ELF build** — dispatches 9+ found the built ELF
   at `/work/build/hw/t2_firmware_v1.0.elf` (step0 marker); sha256
   recorded at run time, never committed.
3. **Diagnostics** — the T2-DIAG final-line annotation (F-22/F-23c)
   is what made every root cause above diagnosable with logs blocked.
4. **Platform description loads end-to-end** — dispatch 19/20
   `step3: platform loaded`: NVIC, Cortex-M4F (`PerformanceInMips`
   property form, F-27), flash/sram/ccmram, `t2_trace` at
   0x60000000 size 0x400 (F-29), no E25/E39.
5. **Four scripted PythonPeripherals register** — dispatch 20
   `step3c: 4 pydev registered, cwd=/opt/renode`: `$ORIGIN`
   PyDevFromFile path (F-28/F-30) with the monitor's token-type
   binding rules satisfied (quoted names).
6. **Deterministic quantum + seed configured** — dispatch 20
   `step4: quantum and seed set`: `SetGlobalQuantum "0.0001"` and
   `SetSeed 123456789` (F-31: unquoted DecimalIntegerToken for
   `SetSeed(int)`).
7. **Full gate battery at CI parity** — at stop, on head `6662659`:
   schemas/contracts/capella/traceability/evidence checks all PASS;
   321 unit/tool/T2 tests + 24 bridge tests pass (the single local
   failure is the designed fail-closed T2-toolchain guard); committed
   T2 evidence is byte-identical to canonical regeneration.
8. **Bring-up findings F-1 and F-2** (referenced from code):
   - **F-1** — `cpu AddSymbolHook` does not exist in Renode 1.15.x;
     first shipped in v1.16.0
     (`src/Renode/Plugins/OsSymbolHook.cs`, extension on
     `ICpuSupportingGdb`). Hence the hard pin to **1.16.1**
     (`ci/docker/renode-1.16.1.pin`).
   - **F-2** — the plant is an **FMI 2.0** CoSimulation FMU: the
     plan's "FMI 3.0" wording is a planning detail, not a
     requirement; pinned OpenModelica 1.24.0 cannot export FMI 3.0
     (`FMI.mo checkFMIVersion` accepts only 1.0/2.0), and the same
     toolchain produced the FMI 2.0 CS FMU behind the passing T1
     `holdup_001` evidence.

## 5. Unproven at stop (the actual H-08 question, unanswered)

- **ELF execution** — `step5` vector-SP readback (0x20018000), any
  instruction execution, the three symbol hooks, magic write
  (`0x54324353`) and `step7`/`step8`.
- **FMU co-simulation** — Holdup FMU instantiation (fmpy 0.3.24),
  the 100 µs master / 1 µs `RunFor` coupling, the FMI bridge
  read/write protocol.
- **The invariant** — `retention_write < safe_latch < iwdg_fire`
  timestamps vs OR-001 bounds; RTC_BKP0R / RCC-CSR shadow
  persistence across the scripted IWDG reset.
- **Determinism** — RUN-2 byte-identical evidence (criterion 3 never
  exercised).

Caveat recorded honestly: dispatch 18's handover table had listed the
four PyPeripherals as "✅ construct (F-28)" — that was
source-verification, not runtime proof (the E39 aborted the include
before those lines first ran). Runtime proof arrived at dispatch 20
(`step3c`); the handover was corrected.

## 6. Toolchain identity (as pinned and executed)

| component | pin | where |
|---|---|---|
| Renode | v1.16.1 portable .NET 8, asset id 356830383, 77304986 bytes; **measured sha256 recorded per run** in `build/hw/t2_retention_001.runlog.json` | `ci/docker/renode-1.16.1.pin` + Dockerfile.t2 build log |
| Renode sources (all F-series diagnoses) | tag v1.16.1 → `src/Infrastructure` → commit `add012af003a0f620d3da52828262676f374d121` | handover §3, verified via `gh api repos/renode/renode/contents/src/Infrastructure?ref=v1.16.1` |
| OpenModelica base | `openmodelica/openmodelica:v1.24.0-minimal@sha256:ab60ed68b4ba3aafa3cddb6319b31ead0346416fb0f2b62bdd7a9e19b486227b` | `ci/docker/Dockerfile.t2`, `ci/docker/base-image.digest` |
| FMPy / NumPy | `fmpy==0.3.24` (TCL1 pin), `numpy==2.1.3` | Dockerfile.t2, CI gates |
| cross toolchain | `gcc-arm-none-eabi` + `libnewlib-arm-none-eabi` (Ubuntu jammy names, F-fix) | Dockerfile.t2 |
| firmware ELF | off-tree build of tag v1.0.0 sources + driver-only `hw/virtual-bench/firmware/`; **sha256 at run time, never committed** | preflight (`CANCESTRY_T2_ELF`) |

**No live-run artifact hashes are claimed** — the evidence chain
(`hw/tests/evidence/t2_retention_001.*`) stays in its pending,
byte-identical-to-canonical state (status `pending`, pass=false, CL0).

## 7. Exact follow-up needed (issue #60)

1. **F-32 (pre-diagnosed one-liner, not applied — stop was called
   first):** `run_t2_retention.py:408` passes
   `-e '$elf="@%s"' …`. The quoted form stores a monitor StringToken
   whose value retains the literal `@`; `sysbus LoadELF $elf`
   (`cancestry-hw.resc:116`) converts it to `ReadFilePath`, whose
   validator runs `File.Exists("@/work/…")` → false → swallowed
   `RecoverableException` → `ParametersMismatchException`. Fix: drop
   the `@` inside the quotes (`'$elf="%s"'`) **or** use the unquoted
   PathToken form the `.resc` header documents (`$elf=@…`), plus a
   runner regression test and a comment correction in the `.resc`
   header (it claims `set $elf @…`, which the runner does not send).
2. **Re-budget:** explicit Lead SE authorization for dispatches 21+
   (criterion 1 is spent; a fresh budget/extension memo is required
   before any further `hw-nightly` dispatch of this bring-up).
3. Everything after `LoadELF` was source-audited against `add012af`
   in one pass (hook existence/scope, multiline token forms,
   `TimeInterval` conversions, bridge command forms — see #60) but is
   runtime-unproven; expect the same fail-closed T2-DIAG discipline
   for whatever it surfaces next.
4. When the live run lands: rewrite this report success-path per #55
   deliverables 3–4 (invariant + determinism + toolchain digests),
   keep the pending evidence replaced only by executed, hashed runs.

## 8. Ledger, promotion, non-claims

- **Ledger:** `hw/tests/traceability.csv` row
  `HW-SF-002,virtual_bench,la-comp-retentiondomain,CL0,OR-001,CL3,sim-pending,,,`
  — **unchanged by this PR** (grep-checkable; the row deliberately
  carries no `evidence_sha256`).
- **Promotion path (unchanged):** a separate safety-relevant PR,
  gated on HS-01 (F1 determinism acceptance) + HS-02 (F2 ISO 16750-2
  parameter verification) closure **and** human safety reviewer
  sign-off, may move the row to
  `passing(virtual_bench, CL2, provisional)`. Not started.
- **Scope guardrails held:** firmware read-only (no
  `platform/cortex_m/` change), no BOR/power-supervisor physics
  (brownout stays `not_simulated`, human-safety-reviewer gated), no
  T4, no Bus-Off/CRC/multi-channel, no test-double fallback, no gate
  weakening (rule 11) — the integration was never made to "pass" by
  retreating from the live path.
- **SE note (issue #55), quoted for the record:** *"The point of H-08
  is to find out whether the live co-simulation actually works — so if
  it doesn't, that is a successful outcome of the issue, reported
  honestly, not a failure to hide."*

---

## Postscript — Lead SE memo: stop endorsed, merge authorized, re-budget granted (21–30)

Subsequent to this report's stop record, the Lead SE issued the memo
*"RE: T2 Bring-Up Stop at Dispatch 20 — PR #59 Merge Authorized,
Issue #60 Re-Budget Granted (21–30)"*. For the record:

1. **Stop endorsed** as correct execution of the budget directive
   (including the dispatch-18 pydev overclaim correction).
2. **PR #59 merge authorized** (Release Manager action) at the stop
   commit `a7b0455`.
3. **Re-budget: GRANTED, bounded — dispatches 21–30.** Dispatch 30 is
   the new hard stop with the same mandatory report-or-stop.
4. **F-32 fix: AUTHORIZED with a fault-class condition** — pre-
   authorized only for platform-script class; any fault touching the
   firmware, FMU internals, evidence schema, or oracle must be
   surfaced to the Lead SE *before* applying.
   **Condition check performed before applying:** the fix's touch set
   is `run_t2_retention.py` (the `$elf` `-e` form, line ~408), a
   `.resc` *header comment* correction, one named regression test in
   `test_t2_retention.py`, and the canonical evidence re-issue (the
   runner is a pinned source; status stays `pending`). Verified
   untouched: `platform/cortex_m/`, `hw/virtual-bench/firmware/`,
   `hw/virtual-bench/fmi2_smoke_slave.c` / Holdup model,
   `schemas/hw/hw-t2-evidence-0.1.0.schema.json`, and
   `hw/tests/oracles/`. → **class check PASSED → applied.**
5. **Stop criteria carried forward verbatim** into dispatches 21–30:
   (1) dispatch 30 hard stop; (2) any fault outside the platform
   script → stop and document, do not spend the budget debugging it;
   (3) non-identical two-run evidence hashes → stop and flag; no
   test-double fallback, no gate weakening.
6. Success definition unchanged: live co-sim with the invariant
   checked against OR-001 and byte-identical two-run hashes, **or**
   the next honest stop-report naming the next blocker.

This report remains the honest stop-record for the 20/20 budget. It
will be replaced by the success-path variant (issue #55 deliverables
3–4) if a re-budgeted run lands live evidence, or superseded by a
second stop-report if the re-budget exhausts.

## Re-budget log — dispatches 21–27 (cycles 1–7 of 21–30); F-33 … F-37 — **FULLY GREEN AT 27**

**Dispatch 21** — `hw-nightly` #26, run `35893814425`, manual
dispatch, 2026-09-23 17:10, head `2306a36` (F-32), job exit 2 after
1 m 29 s (run total 1 m 37 s).

*Milestone — first complete include.* The step markers ran green
through the entire platform script for the first time in the bring-up:

`step0 … step2: machine created | step2b: repl isfile=True | step3:
platform loaded | step3b: sysbus=NO machine=NO monitor.Machine=yes |
step3c: 4 pydev registered | step4: quantum and seed set | step5:
vector SP=0x20018000 (ELF loaded) | step6: symbol hooks installed |
step7: t2_trace readback=0x54324353 | step8: include complete,
machine left paused`

F-32 is therefore runtime-verified (step5 would have aborted at
`sysbus LoadELF` otherwise), and steps 5–8 join the proven list.

*Failure.* `err=Renode monitor closed the connection` — the bare EOF
(pre-F-33 wording) from `fmi_bridge._read_prompt`'s `recv() == b""`
branch, with **no** `first-transcript-error=` segment (the RX stream
carried no error keyword). The connection was accepted (banner read
succeeded), our own `quit` is only ever sent in `close()` after the
error, so the FIN arrived from Renode's side before teardown.

*Evidence situation (why the root cause is still open).* The T2
artifact zip failed for the third consecutive run ("error while
creating the zip file"; mechanism candidate: `Dockerfile.t2` has no
`USER` directive → `build/hw/` is root-owned on the runner), and job
log blobs are egress-blocked from the sandbox. The only surviving
failure record is the 2400-char annotation — which, by construction
pre-F-33, could not say which command was in flight, whether Renode
had exited, or what its console carried. **No root cause is claimed.**

*Source audit (pinned refs; see handover §3 for the recipe and full
facts).* Symbol-hook bodies run with `machine` in scope and their
Python errors are logged, not fatal (`BlockPythonEngine.cs`);
`ExecuteAsMainThread` blocks indefinitely, so a clean self-exit must be
a posted action — wired as `shell.Quitted → Emulator.Exit` after
`term.Run` ends (`Program.cs`/`CommandLineInterface.cs`/AntShell
`Shell.cs`); a clean "exit after the `-e` queue drains" is refuted by
dispatch 20's post-include preflight response; `SocketServerProvider`
never closes a healthy single client. Open vectors: an unhandled
crash (CrashHandler prints `Fatal error:` to the merged stderr —
exactly what F-33 now surfaces) and a Python-peripheral runtime throw
(`PeripheralPythonEngine.Execute` has no per-call error callback).

**Dispatch 22** — `hw-nightly` #27, run `35924809178`, manual
dispatch at 2026-09-23 21:49:45Z, head **`2306a36` (stale)**, failure
after 1 m 23 s. The trigger raced the F-33 push by ~6 minutes (the
recovery push landed ~21:55 while the run had already been queued at
the then-current origin tip), so the cycle executed pre-F-33 code and
produced a **byte-identical** T2-DIAG (same console markers, same bare
`err=Renode monitor closed the connection`) and the same zip error —
zero new information; cycle consumed; F-33 first applies from
dispatch 23 (head `3d37f86`). Disclosure: an untracked stray dispatch
(run `35854449839` @ `a7b0455`, 11:25Z, during the stop period before
the re-budget existed) re-ran the known F-32 fault and likewise carries
no new information — flagged for the Lead SE to rule whether it is
chargeable against cycles 21–30 (handover §4 footnote).

**Dispatch 23** — `hw-nightly`, run `35928186369` @ `fc3fc93`
(trigger time confirmed by the dispatcher before the run), failure.
This run **determined the EOF root cause** and proved F-33 in
production — all three new channels fired in one annotation:

```
console-crash=Fatal error: Python runtime error: 'PythonPeripheral' object has no attribute 'Machine'
proc=alive-at-failure (killed by runner)
err=Renode monitor closed the connection while awaiting response to 'emulation RunFor "0.000100"'
```

The full 151-line monitor transcript (captured in the failure output)
shows the complete bridge protocol working: banner + `-e` echo during
include, machine prompt, preflight magic `0x54324353` ✓, all four
slot reads answered, `emulation RunFor "0.000100"` sent — and no
response, then our teardown `quit`. Root-cause chain, source-pinned:
the first bus access of real firmware execution hit a scripted
PythonPeripheral whose body used `self.Machine`; the PythonPeripheral
scope carries only `request`/`self`/`size` (+ base vars — no
`machine`); IronPython raised `MissingMemberException`, which
`PythonEngine.Execute` wraps as `RecoverableException("Python runtime
error: …")`; thrown from the CPU bus path (`ReadDoubleWordFromBusWrapper`
→ `ExceptionKeeper.ThrowExceptions` → `TlibExecute`) it is unhandled →
CrashHandler `Fatal error:` → process death → monitor FIN → bridge EOF
on the in-flight RunFor. **Fault class: the virtual-bench platform
models — platform-script class → in-lane.** Firmware executed correctly
up to its first peripheral access (no HardFault); FMU instantiated
cleanly; no schema/oracle involvement.

**F-34 applied** (the `self.Machine` → `monitor.Machine` fix; condition
check per memo §3): touch set = `iwdg_model.py` (5 sites),
`rcc_model.py` (3), `rtc_backup_model.py` (2) + named regression test
`test_peripheral_models_reach_machine_via_monitor_scope` + canonical
evidence re-issue. The replacement binding is the officially endorsed
one: `monitor` is injected into every peripheral scope by
`PythonEngine.InnerInit` (CLI registers the Monitor surrogate for the
whole emulation lifetime), the `.resc` step5/step7 fallback
`monitor.Machine['sysbus']` is runtime-proven on this platform, official
`scripts/single-node/segger-rtt.py` uses `monitor.Machine.SystemBus`,
and `IMachine` (:97 `RequestReset`, :173 `ElapsedVirtualTime`) is the
same interface the resc hooks already target. Verified untouched:
`platform/cortex_m/`, `hw/virtual-bench/firmware/`, `fmi2_smoke_slave.c`
/ `Holdup.mo`, `schemas/`, `hw/tests/oracles/`, the `.resc`, the runner,
and the bridge. → **class check PASSED → applied.**

*Fault-class verdict (now determined at dispatch 23, stated for Lead SE
override):* the EOF was a **symptom** — the cause is a scripted
PythonPeripheral model raising on first execution (`self.Machine`).
Models, resc, runner, and bridge together are the **platform script**
→ in-lane for dispatches 21–30. Not a memo stop-class: no firmware
HardFault (the ELF ran correctly up to its first peripheral access),
no FMU load/step failure, no bridge sync-contract violation, no
schema/oracle issue. F-34 fixes the determined cause; if dispatch 24
surfaces a *different* fault past the RunFor, re-classify then.

**F-33 applied** (diagnostics enrichment; condition check per the
memo's §3 discipline): touch set = `fmi_bridge.py` EOF raise (banner
vs `_last_command` context, mirroring F-20) + `run_t2_retention.py`
(pre-reap `proc=` state file; `console-crash=` scan for CrashHandler
signatures; budget-safe summary assembly so `err=` is never
head-truncated) + `hw-nightly.yml` (separate `/tmp` CI-logs artifact
uploaded *before* the `build/hw/` zip) + four named tests
(`test_monitor_endpoint_eof_during_banner_is_named`,
`test_monitor_endpoint_eof_names_inflight_command`,
`test_diagnostic_summary_carries_exit_state_and_crash_tail`,
`test_diagnostic_summary_budget_never_truncates_err`) + canonical
evidence re-issue (runner/bridge/tests are pinned; status stays
`pending`). Verified untouched: `platform/cortex_m/`,
`hw/virtual-bench/firmware/`, `fmi2_smoke_slave.c` / `Holdup.mo`,
`schemas/hw/hw-t2-evidence-0.1.0.schema.json`, `hw/tests/oracles/`,
`.resc` execution logic. → **class check PASSED → applied.**

**Dispatch 24** — run `35932082726`, head `a91ff75` (F-34), manual
dispatch from the GitHub UI, created 2026-09-23 23:08:40 UTC (T2 job
23:09:22–23:09:44, exit 1). The ELF rebuilt on the pinned toolchain
(sha256 `a03ed7d891b2edc0be9828d6c9294fe4bacbabfae286b84108c173cbdd50e8d2`;
the two pre-existing build warnings are unchanged). **MILESTONE: the
full live co-simulation completed and passed every in-run assertion.**
`run_scenario` returning normally proves, in order: `bridge.run()`
executed the whole 150 ms scenario with all three symbol hooks firing,
the post-reset readback observed `iwdgrstf`,
`ordering_violation is None` — i.e. **`retention_write < safe_latch <
iwdg_fire` HELD against OR-001** — and the plant check finished within
its ±1 mV tolerance. The success criterion's invariant half is now
runtime-proven (the first time in this bring-up). The runner then
crashed assembling the *passing* evidence document — code that had
never executed before:

```
== RUN 1: full T2 pipeline (live Renode + FMU, writes evidence)
Traceback (most recent call last):
  File "hw/virtual-bench/run_t2_retention.py", line 874, in main
    document = passing_document(run_body)
               ^^^^^^^^^^^^^^^^^^^^^^^^^^^
  File "hw/virtual-bench/run_t2_retention.py", line 765, in passing_document
    "bridge": run_body["bridge_sha256"],
              ~~~~~~~~^^^^^^^^^^^^^^^^^
KeyError: 'bridge_sha256'
```

RUN 1 exited non-zero before `output.write_text`, so the `&&` chain
stopped: no evidence was persisted and RUN 2 (`--check`) never ran —
the byte-identical two-run half of the criterion remains unproven.
Artifacts: the F-33 `/tmp` split worked for the first time
(`t2-ci-logs-a91ff75…`, 8044 B, containing this traceback), while the
`build/hw/` portion of the combined zip still failed — consistent with
the root-owned-files mechanism the chmod below fixes.

**F-35 applied** (root cause source-pinned before any edit; condition
check per memo §3): `main` did `run_body, _ = run_scenario(...)`, i.e.
it took the first return (the `passed` dict: `events`/`ordering`/
`plant`/`retention`) and discarded the second (the `runlog`, which
holds `bridge_sha256`/`elf_sha256`/`fmu_sha256`/`trace_sha256`/
`measured_tools`); `passing_document` needs keys from both, so the
first access into a runlog key raised. Fix = new helper
`scenario_passing_body(passed, runlog)` that merges both dicts (with a
disjointness guard: overlap between the two key sets is an
AssertionError, so a future contract change cannot merge silently) +
main now unpacks and merges both. Touch set: `run_t2_retention.py`
(helper + main unpack/call) + two named regression tests
(`test_scenario_passing_body_merges_real_run_shapes` — pins both real
dict shapes, reproduces the exact dispatch-24 `KeyError`, then asserts
the merged document is schema-valid, `status == "passing"`, free of
`pending_reason`, hash-complete, and round-trips through
`render_evidence_bytes`; `test_scenario_passing_body_rejects_key_overlap`)
+ `hw-nightly.yml` (`sudo chmod -R a+rX build/hw hw/tests/evidence`
before the artifact uploads — the runner user cannot chmod
container-root-owned files, and `hw/tests/evidence/` added to the
`build/hw` artifact paths so a *passing* JSON persists) + canonical
evidence re-issue (new JSON `sha256:cf2d3867…9c7f4`, PLOT
`75ab2d69…1496`, SVG `c460d076…3d4d`, status stays `pending`).
Verified untouched: `platform/cortex_m/`, `hw/virtual-bench/firmware/`,
`fmi2_smoke_slave.c` / `Holdup.mo`, `schemas/`, `hw/tests/oracles/`,
the `.resc`, the `.repl`, all three scripted models, and `fmi_bridge.py`.
→ **class check PASSED (platform script/runner → in-lane) → applied.**

*Gate-integrity note:* the sandbox `venv3` had a corrupted `rpds`
native module, which made `jsonschema` unimportable and caused every
schema-dependent test to be silently skipped via `importorskip`. The
venv was rebuilt from the exact CI pins before this cycle's gates; with
the suite actually running, the new F-35 test immediately caught a
fixture hex-character bug and the evidence-manifest re-issue caught a
silent `plot_id` match miss (patched against the real key). Final
gates: 5/5 checkers PASS; 327 passed, 1 deselected (handover §5.3
battery) and 585 passed across all of `tests/unit/tools` + the
retention file; `hw/tests` 43 passed + 9 pre-existing omc errors;
ledger row `HW-SF-002,…,sim-pending` exact; evidence `pending`.

**Dispatch 25** — run `35934538207`, head `a314803` (F-35), manual
dispatch from the GitHub UI, created 2026-09-23 23:37:47 UTC.
**SUCCESS CRITERION MET (memo §4).** The job's own words:

```
== RUN 1: full T2 pipeline (live Renode + FMU, writes evidence)
T2 evidence written: /work/hw/tests/evidence/t2_retention_001.json
== RUN 2: independent re-run; --check requires byte-identical evidence
T2 CHECK PASSED: byte-identical evidence reproduced
```

RUN 1 re-executed the whole scenario with every in-run assertion
(hooks, `iwdgrstf`, ordering vs OR-001, retention, plant tolerance)
and wrote the PASSING artifact; RUN 2 independently reproduced it
**byte-identical**. The ELF sha256 is again `a03ed7d8…0e8d2` (stable
across dispatches 24/25). Third confirmation: the in-container suite
then validated the *passing* on-disk form itself —
`test_committed_t2_evidence_is_schema_valid` and
`test_committed_t2_evidence_matches_canonical_regeneration` (passing
branch: canonical render + `_assert_passing_t2_invariants`) both
passed against RUN 1's bytes. Artifacts: **all three landed for the
first time**, including `t2-virtual-bench-a314803…` at **1,920,932 B**
— the F-35 chmod ended the `build/hw/` zip failure streak (runs
19/20/21/24) and the artifact now carries the ELF, FMU, runlog, trace
and the passing evidence JSON; `t2-ci-logs-a314803…` 9253 B; renderer
artifact 5979 B.

The workflow conclusion was still `failure` because the final
in-container pytest step exited non-zero on 2 of 46 tests — both
same-class (never-executed-until-now) fixtures, not platform faults:

1. `test_t2_retention_end_to_end` fail-closed with "T2 toolchain not
   found": the workflow passes `CANCESTRY_T2_ELF` inline to RUN 1 and
   RUN 2 but **not to the pytest line**, and H-07 forbids the e2e test
   from silently downgrading — the suite's first successful execution
   hit the guard as designed.
2. `test_schema_rejects_passing_artifact_with_pending_reason`
   assumed the on-disk artifact is PENDING (the only form carrying
   `pending_reason`); after RUN 1 the on-disk form is PASSING, so
   forcing `status: "passing"` produced nothing to reject and
   `assert violations` failed.

**F-36 applied** (condition check per memo §3). Touch set:
`hw-nightly.yml` (pytest line receives the same inline
`CANCESTRY_T2_ELF` as RUN 1/2 — same env, same contract) and
`test_t2_retention.py` (the fixture now **injects** `pending_reason`
explicitly and asserts the rejection *names* it, making the test
independent of which form is on disk — it exercises the schema's
`if status=passing → not required [pending_reason]` rule in both
states). The e2e test itself is unchanged: with the env present it
re-runs `--check` as a third determinism probe on every green run.
Verified untouched: `platform/cortex_m/`, firmware,
`fmi2_smoke_slave.c` / `Holdup.mo`, `schemas/`, `hw/tests/oracles/`,
the `.resc`, the `.repl`, models, `fmi_bridge.py`, **and the runner**
(`run_t2_retention.py`). → **class check PASSED (workflow + test
fixture → runner class, in-lane) → applied.** Because
`test_t2_retention.py` is in `PINNED_SOURCES`, canonical evidence was
re-issued (new JSON `sha256:0e27899a…a7242`, PLOT
`cc66ef4b…acb9`, SVG `d5f82c86…7635`, status stays `pending`);
the runner itself did not change.

Gates after F-36: 5/5 checkers PASS; 327 passed, 1 deselected
(handover §5.3 battery); `hw/tests` 43 passed + 9 pre-existing omc
errors; ledger row exact; evidence `pending`; workflow YAML valid.

**Dispatch 26** — run `35935628647`, head `699bf31` (F-36), manual
dispatch from the GitHub UI, created 2026-09-23 23:51:14 UTC.
**Zero-execution stop: the step script failed to PARSE, before any
command ran.** The job output is the whole story:

```
…/86fd9aed….sh: line 55: syntax error near unexpected token `('
…/86fd9aed….sh: line 55: `  # F-25 (issue #55): the runner's T2-DIAG line (the failure'
Error: Process completed with exit code 2.
```

Root cause (self-inflicted, source-pinned before any edit): the F-36
comment added inside the T2 step's **single-quoted** `bash -lc '…'`
payload contained an apostrophe (`suite's`, line 198 of the
workflow). Bash terminated the single-quoted string there; the stray
closing quote on the `' 2>&1` line then *re-opened* a string that
swallowed the host section up to the F-25 comment, whose own
`runner's` apostrophe closed it — leaving `s T2-DIAG line (the
failure` to be parsed as a command, i.e. a raw `(` at script line 55.
Because bash parses the whole docker pipeline before executing
anything, **no container ran, no builds ran, no tests ran** — the
cycle produced zero new information (same class of waste as the
dispatch-22 stale race, this time self-inflicted) and is disclosed as
such. The YAML validator passed the file throughout: this is a
bash-level fault invisible to `yaml.safe_load`, which exposed the gap
in the local gate battery.

**F-37 applied** (condition check per memo §3 — workflow + new test
only). Touch set: `hw-nightly.yml` (the F-36 comment rewritten
apostrophe-free, plus an F-37 warning line) and a **new** test file
`tests/unit/tools/test_check_hw_workflow_bash_syntax.py` with two
named guards: (1) `test_workflow_run_block_is_bash_parseable` runs
`bash -n` over every `run:` block of every workflow (GH `${{ }}`
expressions stubbed first) — proven to FAIL on the dispatch-26 block
before the comment fix and to pass after; a reconstruction of the
broken block reproduces the exact `syntax error near unexpected token
'('` reported by CI; (2)
`test_single_quoted_bash_lc_payloads_have_no_raw_apostrophes` flags
raw apostrophes inside `bash -lc '…'` payloads after stripping the
valid `'"'"'` embedding idiom (the form `hw-fast.yml` legitimately
uses). Verified untouched: **all of `PINNED_SOURCES`** (empty diff —
runner, tests for retention, bridge, models, `.resc`/`.repl`,
firmware, FMU, schemas, oracles, Dockerfiles) → **no evidence
re-issue required**. → **class check PASSED (workflow + tool test →
runner class, in-lane) → applied.**

Environment note (reset occurrence 6, Appendix A): between turns the
workspace rewound the local branch to `8857bfa` again and partially
wiped `venv3` (native `rpds`, `coverage/`, and
`pip/_internal/operations/build` deleted — the same corruption
signature seen earlier). Recovered per the Appendix recipe
(`reset --hard 699bf31` with the new untracked test file surviving,
venv rebuilt from the exact pins).

Gates after F-37: 5/5 checkers PASS; **348 passed, 1 deselected**
(handover §5.3 battery — 327 + the 21 new workflow-syntax tests);
606 passed across all of `tests/unit/tools` + the retention file;
`hw/tests` 43 passed + 9 pre-existing omc errors; ledger row exact;
evidence `pending` (unchanged — nothing pinned moved); workflow YAML
valid.

**Dispatch 27 — FULL GREEN (run `35937190878`, head `51bd4c3`,
created 2026-09-24T00:10:30Z). The first `hw-nightly` run in the
entire bring-up to conclude `success`.** Both jobs green: the T2
live-co-simulation job (00:10:34 → 00:11:22, 48 s) and the renderer
`--rerender --strict` determinism job. Because the T2 job's step is a
`set -e` chain, job success is GitHub's own attestation that, in
order: RUN 1 executed the full scenario with every in-run assertion
and wrote the passing evidence; RUN 2 `--check` reproduced it
byte-identical; **and the in-container suite passed 46/46** — which
now includes the e2e test running as a *third* determinism probe
(F-36 restored its `CANCESTRY_T2_ELF`) and the form-independent
rejection fixture (F-36) validating against the passing on-disk form.
Artifacts all landed: `t2-virtual-bench-51bd4c3…` **2,111,481 B**
(grown by the e2e re-run outputs), `t2-ci-logs-51bd4c3…` 8109 B,
renderer `hw-nightly-51bd4c3…` 5994 B. The F-37 `bash -n` guards ran
green in the PR battery at this same head (7/7 checks).

Success criterion (memo §4): **met at dispatch 25, re-proven whole
and fully green at dispatch 27.** Proven cumulative: live co-sim with
`retention_write < safe_latch < iwdg_fire` vs OR-001 (24, re-proven
25/27), two-run byte-identical evidence (25, re-proven 27), persisted
passing artifact in CI storage (25/27), the passing artifact's schema/
regeneration/invariant validation in-container (25/27), e2e third
probe (27), and workflow bash-parseability as a standing gate (F-37).
Remaining out-of-scope: T4/CL3 (bench correlation, human-gated).

Cycle accounting: **25 used (18 at stop + re-budget 21–27); remaining
28–30 = 3; hard stop 30.** Per memo §4 the second success path is
complete — whether to exercise the remaining cycles or call the stop
is the Lead SE's decision; PR #59 stands MERGEABLE with 7/7 checks at
`51bd4c3` awaiting the Release Manager.

## Appendix A — Workspace-reset recovery (provenance note for auditors)

Three (four counting the post-commit re-clone below) times during this
bring-up the sandbox workspace reset rewound the **local** git state of
`arena/01a0cbe2-cancestry` while leaving the worktree content in place
(the documented hazard in `docs/hw/t2-bringup-handover.md` §6;
occurrences: before committing the stop-report at `a7b0455`, after the
re-budget memo before applying F-32, and after dispatch 21 before
applying F-33 — the third rewind targeted the merge base again and was
recovered to `2306a36`). A fourth occurrence, after F-33 was committed
as `7b549fc`, was harsher: a fresh re-clone to `8857bfa` destroyed that
commit object outright while the worktree files survived untracked;
recovery used `git reset --mixed FETCH_HEAD` (= `2306a36`) so the
index re-acquired the tracked files, verified the worktree diff was
byte-identical to the lost commit (11 files, 492+/70−), recommitted as
`0aa0b60`, and pushed. A fifth occurrence, during the F-35 turn after
dispatch 24, rewound the local branch pointer to the merge base
`8857bfa` again while leaving the worktree content in place; the seven
in-flight files (runner, tests, workflow, four evidence artifacts) were
copied aside, `git reset --hard` restored `a91ff75` (= `FETCH_HEAD` =
origin), and the copies were restored byte-for-byte — after which a
hash comparison exposed that one file's patch (the renders manifest)
had silently matched no entry on the first attempt and was re-run
against the correct `plot_id` key. A sixth occurrence, during the
F-37 turn after dispatch 26, again rewound the pointer to `8857bfa`
**and** partially wiped `venv3` (native `rpds`, `coverage/` and
`pip/_internal/operations/build` deleted — the same corruption
signature as the first venv breakage); the one in-flight untracked
file (the new workflow-syntax test) survived `reset --hard 699bf31`
and the venv was rebuilt from the exact CI pins. Recovery was
mechanical, not editorial, every time:

1. `origin` is the source of truth: fetched
   `refs/heads/arena/01a0cbe2-cancestry` (first `6662659`, then
   `a7b0455`, then `2306a36`).
2. **Every key worktree file was hash-verified** against the origin
   blob (`git hash-object <f>` vs `git rev-parse origin/<branch>:<f>`)
   — the platform script, `.repl`, runner, tests, evidence artifacts,
   this report, and the handover all `MATCH`ed before any reset (12/12
   on the third occurrence).
3. Only then: `git reset --hard origin/<branch>`; when uncommitted
   edits existed (first occurrence) the in-flight files were copied
   aside beforehand and restored byte-for-byte afterwards.

No committed content was hand-edited during recovery; each recovery
is visible in the push history as a fast-forward of verified state.
The tree this report describes is provably reconstructed from origin,
not re-authored.
