# T2 Virtual Bench Bring-Up — Handover (H-08, issue #55)

**Status: RE-BUDGETED 21–30 — dispatches 21–24 executed (all failed
closed), F-33/F-34/F-35 applied, **live co-sim + OR-001 invariant
PROVEN (dispatch 24)**, ready for dispatch 25.** Dispatch 21 (run `35893814425` @ `2306a36`) delivered the
**first complete include** — step0…step8 green, F-32 runtime-verified
— then hit the bare monitor EOF. Dispatch 22 (run `35924809178`)
raced the F-33 push and duplicated that failure on stale `2306a36`
(zero new information; cycle consumed). **Dispatch 23 (run
`35928186369` @ `fc3fc93`, dispatcher-confirmed SHA) determined the
root cause through all three F-33 channels:**
`err=Renode monitor closed the connection while awaiting response to
'emulation RunFor "0.000100"'` (the first CPU execution) +
`console-crash=Fatal error: Python runtime error: 'PythonPeripheral'
object has no attribute 'Machine'` + `proc=alive-at-failure (killed
by runner)` — plus the full 151-line transcript showing banner, `-e`
echo, preflight magic ✓, all slot reads answered, then no RunFor
reply. Chain: a scripted model's `self.Machine` raised
MissingMemberException on the first real bus access →
`PythonEngine.Execute` wrapped it as `RecoverableException("Python
runtime error: …")` → unhandled on the CPU bus path
(`ReadDoubleWordFromBusWrapper` → `ExceptionKeeper` → `TlibExecute`)
→ CrashHandler → process death → FIN → bridge EOF. **F-34** replaces
all 10 `self.Machine` sites (iwdg 5 / rcc 3 / rtc_backup 2) with
`monitor.Machine` — the binding injected by `PythonEngine.InnerInit`
(CLI Monitor surrogate), runtime-proven by the resc step5/step7
fallback and official `segger-rtt.py`; `IMachine` :97 `RequestReset`
/ :173 `ElapsedVirtualTime` verified. Named regression test
`test_peripheral_models_reach_machine_via_monitor_scope`; class check
passed (platform models only — firmware/FMU/schema/oracle/`.resc`/
runner/bridge untouched); evidence re-issued canonically (status stays
`pending`); gates green. **Dispatch 24 (run `35932082726` @ `a91ff75`,
2026-09-23 23:08:40Z) = the milestone:** the full live co-simulation
completed — `run_scenario` returned, which per its control flow
requires all three hooks fired, `iwdgrstf` observed on post-reset
readback, `ordering_violation is None` (**`retention_write <
safe_latch < iwdg_fire` HELD vs OR-001**), retention preserved
in-run, and plant within ±1 mV — and only then the never-executed
pass path crashed: `KeyError: 'bridge_sha256'` at
`passing_document` because `main` did `run_body, _ = run_scenario(...)`
and discarded the second return (`runlog` holds the hash keys). RUN 1
wrote no evidence; RUN 2 never ran → the byte-identical half of the
criterion is still unproven. **F-35** merges both returns via
`scenario_passing_body` (with a disjointness guard) + two named tests;
the same commit also `sudo chmod`s the container-root-owned evidence
trees before artifact uploads (the mechanism behind the persistent
`build/hw/` zip failures — F-33's `/tmp` split landed its first
artifact at this run: `t2-ci-logs-a91ff75…`, 8044 B) and adds
`hw/tests/evidence/` to the artifact paths. Class check passed
(runner + workflow only — firmware/FMU/schema/oracle/`.resc`/models/
bridge untouched); evidence re-issued canonically (JSON `cf2d3867…`,
PLOT `75ab2d69…`, SVG `c460d076…`, status `pending`); gates green.
Stop criteria carry forward verbatim.

**GitHub outage note (resolved):** `GH_TOKEN` went invalid mid-session
(401s on every `gh api`); after reconnecting GitHub in Arena the
connection works again. During the outage, reads still worked via
`fetch_page`/`curl` on `github.com` HTML (blob pages embed `rawLines`
— recipe in §3); keep that as the fallback. `gh workflow dispatch`
remains 403 — dispatches stay human-triggered from the UI (§5.1).

**Next action:** trigger `hw-nightly` from the UI **on
`arena/01a0cbe2-cancestry`** for **dispatch 25** (cycle 5 of the
re-budget) — origin tip carries F-35 (verify the run page shows that
SHA before waiting on results; dispatch 22 taught us the race).
Expectation: RUN 1 completes the whole pipeline (scenario + evidence
write), RUN 2 `--check` reports byte-identical evidence, the
in-container pytest suite is green, and BOTH artifacts land (chmod
fix) — together the full success criterion. Possible next faults
still in platform-script/runner class: evidence-write or render
mismatch; a two-run hash mismatch is a **STOP-and-flag** event per
memo §4 (no test doubles, no gate weakening). If anything points at
firmware/FMU internals, **stop and surface to the Lead SE**
(memo §3/§4); remaining cycles 25–30, hard stop 30.

This document is internal continuity documentation for the bring-up. It is
not a safety claim and promotes nothing (HwAGENTS.md rules 13/14).

---

## 1. Mission, constraints, and the stop criteria

Issue #55 (H-08): first **live** Renode 1.16.1 (pinned) + FMI2
(`CancestryLib.Power.Holdup` FMU) co-simulation of the real v1.0.0 ELF on
the simulated STM32G474, in the `hw-nightly` CI job without any
`CANCESTRY_HW_ALLOW_SKIP=1`; assert `retention_write < safe_latch <
iwdg_fire` (real symbol-hook timestamps vs OR-001 bounds); prove two
independent runs give byte-identical evidence; bring-up report
`docs/hw/t2-bringup-report.md` (written when the live run lands — or as the
honest "documented-outcome" report if the stop criteria are met).

Hard constraints (issue + HwAGENTS.md): firmware read-only (nothing under
`platform/cortex_m/` changes); **no ledger promotion** (the
`HW-SF-002, virtual_bench` row stays `sim-pending`/CL0 — grep-checkable);
no BOR/power-supervisor physics (brownout reset stays `not_simulated`,
human-safety-reviewer gated); virtual bench only (no T4); no
Bus-Off/CRC/multi-channel scope; **stop-and-flag, never fake** (rule 11) —
no test-double fallback, no gate weakening.

Lead SE decision memo (2026-09-22) — stop criteria (any one of):
1. **20 total dispatch cycles** (18 used as of this handover);
2. **any fault outside the platform script** (`.repl`/`.resc`): firmware
   crash (HardFault/MemManage/BusFault), FMU fails to load or step, FMI
   bridge fails to synchronize, or determinism failure → stop and write the
   documented-outcome report (ledger row unchanged; next step becomes a
   separate issue);
3. **determinism failure** (two runs, different hashes) → stop and flag.

SE also decided (2026-09-22): the T2 ELF call graph was verified to never
touch DWT/DEMCR (the driver bypasses `cancestry_watchdog_init` /
`hal_stm32_clock_init`), so **no DWT extension of the platform is
required**; and absolute/`$ORIGIN`-path fixes for the PythonPeripheral
`filename:` resolution are pre-authorized.

## 2. Current state (as of `8269dca`, dispatch 18)

Platform load progress (each step source-verified before dispatch):

| stage | state at dispatch 18 |
|---|---|
| `mach create`, monitor, T2-DIAG markers | ✅ |
| `nvic` (`systickFrequency`, `IRQ -> cpu@0`) | ✅ constructs |
| `cpu` (`CPU.CortexM`, `cpuType: "cortex-m4f"`, `PerformanceInMips: 100`) | ✅ constructs (E25 resolved, F-27) |
| `flash`/`sram`/`ccmram` | ✅ |
| `iwdg`/`rtc_backup`/`gpioa`/`rcc` (via `machine PyDevFromFile $ORIGIN/...` in the .resc) | ✅ construct (F-28) |
| `t2_trace` (`MappedMemory @ 0x60000000, size 0x200`) | ❌ **E39** — size not aligned to guest page size 0x400 |
| ELF load, symbol hooks, FMU co-simulation, invariant, determinism | not reached |

Dispatch 18 T2-DIAG (run 35806027349):
`first-transcript-error= ... Error E39: Exception was thrown during
registration of 't2_trace' in 'sysbus': Could not register memory at
offset 0x60000000 and size 0x200 - the size has to be aligned to guest
page size 0x400. At .../stm32g474-cancestry.repl:161:33`.

**Expected dispatch 19 result** (after F-29): `step3 → step3c (4 pydev
registered, cwd=...) → step5 (vector SP=0x20018000) → step7 (t2_trace
readback=0x54324353) → step8` → preflight passes → first live scenario:
FMU instantiation (fmpy 0.3.24), 100 µs master / 1 µs FMU `RunFor`
quanta, the three symbol hooks stamping the trace area, the 51 ms armed /
2 ms post-escalation IWDG window, invariant check vs OR-001, then RUN 2
(determinism). **If dispatch 19/20 faults outside the platform script,
apply SE stop criterion 2/3 — stop and document; do not keep patching.**

*Superseded by events: dispatch 19/20 ran — F-29/F-30/F-31 all
landed as predicted through `step4`, then dispatch 20 stopped at
`LoadELF` (F-32) with criterion 1 (20/20) triggering the stop; see
section 4 and `docs/hw/t2-bringup-report.md`. **Dispatch 21 (re-budget
cycle 1) then ran the include to completion (step5–step8 proven) and
failed at the monitor EOF — see section 4's row 21 and the report's
re-budget log.** The table above is therefore stale below `step4`;
trust the dispatch table, not this snapshot.*

## 3. Source-verified facts about Renode 1.16.1 (do not re-derive)

Authoritative pin: **renode tag v1.16.1 → submodule `src/Infrastructure`
→ commit `add012af003a0f620d3da52828262676f374d121`** (check via
`gh api repos/renode/renode/contents/src/Infrastructure?ref=v1.16.1`).
F-26 originally grepped a date-guessed commit (`1117a7b419`) in the
standalone infra repo — that produced a wrong "no such property"
conclusion; always verify the gitlink.

- **DLR `.repl` attribute rule**: `ConstructorOrPropertyAttribute.
  IsPropertyAttribute == char.IsUpper(Name[0])` — an attribute is a
  property assignment IFF its name starts with an UPPERCASE letter;
  anything else is a constructor argument that must name-match a
  constructor parameter (case-sensitive exact match, `NameOrAliasMatches`)
  or the constructor is rejected → **E25 "Could not find suitable
  constructor … unused attributes"**. This was the true root cause of the
  dispatch 15/16 E25 (camelCase `performanceInMips`).
- `PerformanceInMips` is a property on **`BaseCPU`**
  (`src/Emulator/Peripherals/Peripherals/CPU/BaseCPU.cs`, default **100**
  set in the constructor); the official `platforms/cpus/stm32f746.repl`
  declares `PerformanceInMips: 462` (PascalCase = property form).
- `CortexM` ctor (1.16.1): `(string cpuType, IMachine machine, NVIC nvic,
  [NameAlias("id")] uint cpuId = 0, Endianess endianness = LittleEndian,
  uint? fpuInterruptNumber = null, uint? numberOfMPURegions = null,
  bool enableTrustZone = false, uint? numberOfSAURegions = null,
  uint? numberOfIDAURegions = null)`; the `machine` parameter is filled
  automatically from `@ sysbus` (`TryGetValueOfOurDefaultParameter`).
- `NVIC` ctor: `(IMachine, ulong systickFrequency = 50*0x800000,
  byte priorityMask = 0xFF, bool haltSystickOnDeepSleep = true)`; the
  official form includes `IRQ -> cpu@0` on the NVIC.
- `PythonPeripheral` ctor: `(int size, bool initable = false,
  string script = null, string filename = null)`; it calls
  **`File.Exists(filename)` in the constructor — CWD-relative**. The
  headless CI process's effective CWD is NOT the script directory
  (dispatch 14 run 35773043927 and dispatch 17 run 35805300102), despite
  the runner's `subprocess` cwd. Use **`machine PyDevFromFile
  $ORIGIN/<file> <addr> <size> [initable] [name]`** (public `Machine`
  extension, `PythonPeripheralExtensions`, v1.16.1) from the `.resc`,
  where `$ORIGIN` expands to the script's directory (proven since F-24).
- `MappedMemory` ctor: `(IMachine, long size, int? segmentSize = null,
  ...)`; **registration requires the size aligned to guest page size
  0x400** (E39, dispatch 18) — sizes like 0x200 are rejected.
- `sysbus: init: Tag <range> "name"` **is valid** in 1.16.1 (official
  `platforms/cpus/stm32f412.repl` uses it); the F-25 belief that it was
  invalid was a misattribution of the E25 (corrected in-tree in F-27).
- `cpuType: "cortex-m4f"` (FPU on) is the official M4F idiom (official
  `stm32f412.repl` re-declares `cpu:` with `cpuType: "cortex-m4f"`).
- The T2 ELF call graph (`hw/virtual-bench/firmware/main.c` + the v1.0.0
  `cancestry_watchdog_*` / `cancestry_event_hard_fault_escalate` paths it
  invokes) never touches DWT/DEMCR — DWT stays `NOT_SIMULATED` in the
  platform (Lead SE decision, 2026-09-22).

Post-dispatch-21 source audit (all from pinned refs; fetched via
`gh api …/contents?ref=… | base64 -d` while auth worked, then via
`github.com` blob HTML `rawLines` after the API token died — see §6):

- **`BlockPythonEngine.cs` (infra `add012af`,
  `src/Emulator/Extensions/Hooks/`)**: `InnerInit` does
  `Scope.SetVariable(Core.Machine.MachineKeyword, Machine)` +
  `cpu` + `self` ⇒ **the resc's symbol-hook bodies DO have `machine`**
  (the step3b monitor-python `machine=NO` does not apply inside hooks);
  every hook body runs through `Execute(code, error => CPU.Log(Error,
  "Python runtime error: …"))` ⇒ **a hook exception is logged, never
  fatal** — the "hook NameError kills Renode" hypothesis is dead.
- **`Program.cs` (v1.16.1, `src/Renode/`)**: worker thread runs
  `CommandLineInterface.Run` then `Emulator.FinishExecutionAsMainThread()`;
  main thread blocks in `ExecuteAsMainThread` (`Emulator.cs`:
  `while (actionsOnMainThread.TryTake(out action, -1))`) ⇒ **the process
  does not self-exit from loop drain**; exit requires a posted
  action — the wiring is in `CommandLineInterface.PrepareShell`:
  `shell.Quitted += Emulator.Exit` (and `monitor.Quitted += shell.Stop`).
- **AntShell `Shell.Start` → `term.Run(stopOnError=true)`**: the run
  loop ends only on `Stop()`/cancel (input `null`) or a null read ⇒
  Quitted ⇒ `Emulator.Exit`. A clean "exit-after-the-`-e`-queue-drains"
  would have killed dispatch 20's post-include preflight read — it
  returned data — so that theory is **refuted**; the EOF needs a
  different mechanism (or a crash).
- **`CrashHandler.cs`**: unhandled .NET exceptions print
  `Fatal error:` + stack to **stderr** (merged into the runner's console
  log) before the runtime kills the process — F-33's `console-crash=`
  scan targets exactly those signatures.
- **`SocketServerProvider.cs`**: single accepted client (backlog 1),
  writer/reader threads Join before the next Accept; no server-side
  close of a healthy connection — a FIN before our `quit` therefore
  means process death or a stream-level error, not a server replace.
- **`PeripheralPythonEngine.cs`**: peripheral call bodies execute via
  bare `Execute(code)` (no per-call error callback visible) — a Python
  peripheral runtime throw may propagate (unlike hooks); keep as an
  open vector until `console-crash=`/`proc=` speak.

## 4. Dispatch history (hw-nightly, workflow_dispatch, this branch)

Run IDs verified against `gh run list`; older entries per the session
record. Cycles are the SE budget unit (22 used: 18 at stop + re-budget
dispatches 21–24; remaining 25–30 = 6 cycles, hard stop at 30).
One stop-period **stray** dispatch (run `35854449839` @ `a7b0455`,
2026-09-23 11:25Z) sits outside the re-budget numbering — see the
footnote below the table; flagged for Lead SE whether it is chargeable.

| # | run | commit | outcome |
|---|---|---|---|
| 9–11 | 35726064092 / 35727339896 / 35730854679 | pre-F-20 era | toolchain/preflight failures (F-15…F-20) |
| 12 | 35733480131 | 3f5d0f8 (F-23) | preflight magic 0 — .resc not loaded (CWD) |
| 13 | 35773043927 | 22cf6eb (F-23b) | clean-shutdown diagnostic |
| 14 | 35774011082 | 8702ed3 (F-23c) | T2-DIAG line works → F-24 `$ORIGIN` fix |
| 15 | 35777946390 | 0803c84 (F-24) | first real .repl load; **E25** on cpu line (misattributed to the Tag block by truncated diagnostics) |
| 16 | 35778907099 | 598a360 (F-25) | same E25 — true cause: camelCase `performanceInMips` (F-27 diagnosis) |
| 17 | 35805300102 | 4beacd2 (F-27) | **E25 gone** (CPU constructs); new fault: `iwdg` PythonPeripheral "Could not find source file" (CWD-relative `File.Exists`) |
| 18 | 35806027349 | 8269dca (F-28) | all 4 PythonPeripherals construct; **E39** on `t2_trace` (size 0x200 not 0x400-aligned) → F-29 diagnosed |
| 19 | 35807326391 | c185cee (F-29) | **F-29 PROVEN** — `step3: platform loaded` (full `.repl` incl. `t2_trace`); new fault: `machine PyDevFromFile … Parameters did not match the signature` at the first name arg → **F-30** (bare LiteralToken rejected for `string` param); F-31 (quoted seed → `SetSeed(int)`) pre-diagnosed in the same audit → both fixed pre-dispatch-20 |
| 20 | 35851511484 | 6662659 (F-30/F-31) | **F-30/F-31 PROVEN** — `step3c: 4 pydev registered, cwd=/opt/renode`; `step4: quantum and seed set`; fault at `sysbus LoadELF $elf` (**F-32**: runner stores `$elf` as quoted StringToken with literal `@`; `ReadFilePath` validation fails) → **STOP, criterion 1 (20/20)**; report written; follow-up = issue #60 |
| 21 | 35893814425 | 2306a36 (F-32) | **FIRST COMPLETE INCLUDE** — step0…step8 green (step5 ELF load + SP readback, step6 hooks, step7 magic `0x54324353`, step8 paused) ⇒ F-32 runtime-verified; then fail-closed `err=Renode monitor closed the connection` (bare EOF, no transcript-error keyword); artifact zip failed 3/3; **F-33 applied** (EOF context + `proc=`/`console-crash=` + separate CI-logs artifact) → was meant for dispatch 22. Cycle 1 of the re-budget. |
| 22 | 35924809178 | 2306a36 (**stale**) | hw-nightly #27, manually triggered 21:49:45Z — **raced the F-33 push (~21:55) by ~6 min**, so the run executed pre-F-33 code: byte-identical T2-DIAG (bare EOF), same zip failure, no `t2-ci-logs` artifact step. Zero new information; cycle consumed. Cycle 2 of the re-budget. |
| 23 | 35928186369 | fc3fc93 (F-33) | **ROOT CAUSE DETERMINED** — first RunFor killed by `'PythonPeripheral' object has no attribute 'Machine'` (Fatal error on the CPU bus path; `self.Machine` in the scripted models); `err=` named the in-flight `emulation RunFor`; `proc=alive-at-failure`; full 151-line transcript captured (preflight magic ✓, slot reads ✓). **F-34** applied: 10× `self.Machine`→`monitor.Machine` + named test. Cycle 3 of the re-budget (remaining: 24–30). |
| 24 | 35932082726 | a91ff75 (F-34) | **LIVE CO-SIM + OR-001 INVARIANT HELD** — `run_scenario` returned (all hooks fired, `iwdgrstf` observed, `ordering_violation` None, retention preserved in-run, plant ±1 mV) ⇒ first complete scenario execution; then never-executed pass path: `KeyError: 'bridge_sha256'` at `passing_document` (`main` discarded the `runlog` return). RUN 1 wrote no evidence; RUN 2 never ran. `t2-ci-logs-a91ff75…` landed (8044 B, F-33 split works); `build/hw` zip still failed (root-owned → F-35 chmod). **F-35 applied**: `scenario_passing_body` merge + 2 named tests + workflow chmod/artifact paths. Cycle 4 of the re-budget (remaining: 25–30). |

Note on dispatch 18's pydev row: the `✅ construct (F-28)` entries in
section 2 were **source-verified, not runtime-proven** — the E39 aborted
the include before the `.resc` pydev lines ever ran (dispatch 18's
console markers stop at `step2b`). Runtime proof of the pydev
registrations arrives with dispatch 20 (step3c).

Stray footnote (disclosure): run `35854449839` @ `a7b0455`
(workflow_dispatch, 2026-09-23 11:25:29Z) fired **during the stop
period**, ~30 min after dispatch 20's failure and before the re-budget
terms existed; it was never recorded in this table. Its T2-DIAG shows
the known F-32-era fault verbatim (`elf=@/work/…`, `sysbus LoadELF
$elf` signature error, magic `0x00000000`) — zero new information. It
is disclosed here rather than absorbed silently: the Lead SE decides
whether it is chargeable against the 21–30 budget (if charged,
remaining cycles become 23–29 with hard stop 30).

## 5. Procedures that work in this environment

### 5.1 Triggering dispatches
`gh workflow dispatch` returns 403 from the sandbox — **the human
(coordinator/SE) triggers `hw-nightly` from the UI** on the branch. Note a
fresh run may not appear in `gh run list` for a few seconds.

**Also 403 this session (2026-09-23): issue API writes.** `gh issue
comment`/`gh issue edit` → `Resource not accessible by integration`
(PR comments still work). The post-memo F-32/re-budget update that could
not go on issue #60 lives in the **PR #59 comment** (`5797344789`), the
report postscript/Appendix A, and this handover — do not retry issue
comments expecting a different result until the token scope changes.

### 5.2 Reading results (log fetch is blocked)
`gh run view <id> --log` / the logs API are blocked (results-receiver
egress). Use the **annotations API** — the T2-DIAG line is a stored
annotation:
```
CR=$(gh api repos/mfreazer/CANcestry-/commits/<sha>/check-runs \
     --jq '.check_runs[] | select(.conclusion=="failure") | .id' | head -1)
gh api repos/mfreazer/CANcestry-/check-runs/$CR/annotations \
   --jq '.[] | .message' | grep -a T2-DIAG
```
T2-DIAG format (final line of the failed step): `T2-DIAG console=<70-char
capped step markers> | first-transcript-error=<1400-char context> |
console-crash=<230-char unhandled-crash signature, F-33> |
proc=<Renode pre-reap exit state, F-33> | err=<200-char runner
error>`, total ≤ 2400 — under pressure the console/transcript parts
shrink and `err=` is never truncated (F-33; pre-F-33 the head-slice
ate the error tail). F-33 EOF wording: `… during the startup banner
read` vs `… while awaiting response to '<command>'`.

### 5.3 Gates (local CI parity)
A venv with the exact CI pins: `capellambse==0.6.17`, `fmpy==0.3.24`,
`numpy==2.1.3`, plus `jsonschema`, `pyyaml`, `pytest`
(`python3 -m venv venv3 && venv3/bin/pip install ...`). Then:
```
venv3/bin/python ci/check_schemas_valid.py schemas
venv3/bin/python ci/check_hw_contracts.py .
venv3/bin/python ci/check_capella_model.py .
venv3/bin/python ci/check_hw_traceability.py .
venv3/bin/python ci/check_hw_evidence.py --root .
venv3/bin/python -m pytest tests/unit/tools/test_check_hw_*.py \
    tests/unit/tools/test_check_capella_model.py \
    hw/virtual-bench/test_t2_retention.py -q      # ~327 passed
venv3/bin/python -m pytest hw/tests -q             # omc-dependent tests error
                                                    # (no OpenModelica in sandbox;
                                                    # pre-existing, identical on
                                                    # clean base)
```
`hw/virtual-bench/test_t2_retention.py::test_t2_retention_end_to_end`
fail-closes outside CI by design ("T2 toolchain not found") — expected
locally.

### 5.4 Evidence re-issue (canonical — do it this way, not by hand-editing)
The T2 case manifest must be **byte-identical to the canonical
regeneration** (`test_committed_t2_evidence_matches_canonical_regeneration`
enforces this). After any change to a pinned source (`.repl`, `.resc`,
runner, models, …):
```
cd repo && venv3/bin/python - <<'EOF'
import sys, hashlib, json
sys.path.insert(0, 'hw/virtual-bench')
import run_t2_retention as runner
open('hw/tests/evidence/t2_retention_001.json','w').write(
    runner.render_evidence_bytes(runner.expected_pending_manifest()))
def h(p): return 'sha256:' + hashlib.sha256(open(p,'rb').read()).hexdigest()
H_e = h('hw/tests/evidence/t2_retention_001.json')
p = 'hw/tests/evidence/t2_retention_001.plot.json'
s = open(p).read(); s = s.replace(json.loads(s)['source_evidence_hash'], H_e)
open(p,'w').write(s)
print(H_e, h(p))
EOF
venv3/bin/python ci/renderers/cancestry-render-modelica.py \
  --plot-data hw/tests/evidence/t2_retention_001.plot.json --version 0.1.0 \
  --output hw/tests/evidence/renders/t2_retention_001.svg --root .
# then in hw/tests/evidence/renders/manifest.json, t2 entry:
#   plot_data_sha256 = sha256(new plot.json); expected_svg_sha256 = sha256(new svg)
```
The traceability T2 row (`HW-SF-002,virtual_bench,...,sim-pending,,`)
deliberately carries **no** `evidence_sha256` (pending row) — leave it.

## 6. Sandbox environment hazards (learned the hard way)

- **Workspace resets** (between/within turns): wipe `/home/user/scratch`
  and any venv, and may rewind the **local** branch pointer to the merge
  base (8857bfa) while leaving worktree content intact. Recovery:
  `git fetch -q origin arena/01a0c6d8-cancestry`; verify key worktree
  files hash-match FETCH_HEAD (`git hash-object f` vs
  `git rev-parse FETCH_HEAD:f`); save in-flight edits; `git reset --hard
  FETCH_HEAD`; re-apply. **Origin is the source of truth.**
- **Egress**: `raw.githubusercontent.com`, release-asset CDN, and
  results-receiver are TLS-blocked; `api.github.com` (via `gh`) and pypi
  work **only while the Arena GitHub connection is valid** — on
  2026-09-23 the session token expired: every `gh api` 401s and even
  header-less `curl api.github.com` gets 401 from this egress
  (reconnect GitHub in Arena to restore). `github.com` **HTML** keeps
  working unauthenticated: public blob pages embed the file text in a
  `rawLines` JSON array — fetch with `curl` + regex-extract (this is
  how the post-dispatch-21 source audit continued after the token
  died; recipe recorded in §3). Consequence: the Renode 1.16.1 binary
  **cannot be downloaded** — no local platform-load reproduction; CI
  is the only runtime.
- GitHub log fetches for run logs are blocked — use section 5.2.

## 7. Proven vs unproven

**Proven (runtime, by stop):** T2 toolchain provisioning (Dockerfile.t2:
Renode 1.16.1 identity pin `ci/docker/renode-1.16.1.pin`,
arm-none-eabi, fmpy 0.3.24); off-tree ELF build of the v1.0.0 sources;
T2-DIAG diagnostics; the full gate battery; **platform description
load end-to-end (dispatch 19/20 `step3`)**; **four PyDevFromFile
registrations (dispatch 20 `step3c`, F-30)**; **quantum + seed
(dispatch 20 `step4`, F-31)**; the `$ORIGIN` expansion mechanism
observed in executed-command echoes; **`sysbus LoadELF` + vector SP
readback `0x20018000` (dispatch 21 `step5`)**; **three `cpu
AddSymbolHook` installs (`step6`)**; **preflight magic `0x54324353`
written/read back on the real platform (`step7`)**; **the include
running to completion with the machine paused (`step8`)** — the
latter four first proven at dispatch 21. **Bridge-driven monitor
traffic beyond connect (dispatch 23): banner/`-e` echo pairing, the
preflight read returning `0x54324353`, all four trace-slot reads
answered, and `emulation RunFor` actually transmitted — full 151-line
transcript in the failure output.** Also proven at dispatch 23: the
F-33 channels in production (named-command `err=`, `console-crash=`,
`proc=`) and the first instructions of the real ELF executing (the
crash occurred *inside* `TlibExecute` during a bus read — firmware
code ran correctly up to that access). **Dispatch 24: the first
COMPLETE live scenario** — `emulation RunFor` sequences to scenario
end, all three symbol hooks fired, `iwdgrstf` observed on post-reset
readback, **the OR-001 ordering invariant held
(`ordering_violation is None`)**, RTC_BKP0R preservation asserted
in-run across the scripted IWDG reset, and the plant check within
±1 mV. The crash was afterwards, in evidence assembly (F-35), not in
the scenario.

**Unproven (the actual bring-up):** persisted passing evidence
(dispatch 24 crashed before `output.write_text`); RUN-2
byte-identical determinism (never reached); two-run reproduction of
the scenario/retention result (single-run only as of dispatch 24);
T4/CL3 and anything beyond OR-001. The invariant is proven (dispatch
24) but must re-prove on every subsequent green run. Note: hooks,
magic, and slot reads remain **platform bring-up proofs**; from
dispatch 24 the ELF additionally runs the whole scenario to
completion.

Watch items for the first live scenario: (a) the IWDG model's scripted
reset and whether the `machine` reset preserves `t2_trace` contents
(`MappedMemory` is not expected to be cleared by reset — verify from the
evidence, do not assume); (b) the `step3c` cwd probe output (record it in
the report — it documents the CWD behavior for tool-qualification);
(c) any FMU/bridge fault is a **stop-and-document** event per SE criterion
2, not a patch-and-retry one.

## 8. PR, ledger, promotion

- **PR**: #58 (`arena/01a0c6d8-cancestry`) was superseded mid-bring-up
  by **#59** (`arena/01a0cbe2-cancestry` → main, head `6662659` at
  stop, MERGEABLE, all PR checks green), which fast-forwards all of
  #58's commits plus F-29…F-31 and the handover/report updates.
  Bring-up only; no live run is claimed.
- **Ledger:** `hw/tests/traceability.csv` row `HW-SF-002,virtual_bench`
  stays `sim-pending` / CL0 with empty evidence cells — no promotion,
  grep-checkable.
- **Report:** `docs/hw/t2-bringup-report.md` **written at stop as the
  honest documented-outcome report** (criterion 1: 20/20 cycles), with
  the SE-mandated wording (issue #55 note: an honest broken/incomplete
  integration is a successful engineering outcome of H-08), the full
  dispatch history, the proven/unproven split, and the exact
  follow-up (issue **#60**: F-32 one-liner + re-budget for dispatches
  21+). If a re-budgeted run later lands live evidence, the report is
  rewritten success-path per #55 deliverables 3–4.
- **Promotion** (later, separate safety PR): gated on HS-01 (F1
  determinism acceptance) + HS-02 (F2 ISO 16750-2 parameter verification)
  closure **and** human safety reviewer sign-off →
  `passing(virtual_bench, CL2, provisional)`.

## 9. Key files

| path | what it is |
|---|---|
| `hw/virtual-bench/renode/stm32g474-cancestry.repl` | the platform model (F-29 applied: `t2_trace` size 0x400, guest-page aligned) |
| `hw/virtual-bench/renode/cancestry-hw.resc` | bring-up script: steps 0→8, `PyDevFromFile` registrations, symbol hooks, magic |
| `hw/virtual-bench/renode/{iwdg,rtc_backup,gpioa,rcc}_model.py` | scripted register models (bus-address based) |
| `hw/virtual-bench/run_t2_retention.py` | the T2 runner: preflight, launch, monitor, T2-DIAG, evidence rendering (`expected_pending_manifest`, `render_evidence_bytes`) |
| `hw/virtual-bench/fmi_bridge.py` | the FMI 2.0 co-simulation bridge (100 µs master / 1 µs FMU quanta) |
| `hw/virtual-bench/firmware/` | off-tree T2 driver (`main.c`) linked with the read-only v1.0.0 sources; never committed-ELF (built in CI, sha256 recorded at run time) |
| `hw/tests/evidence/t2_retention_001.json` (+ `.plot.json`, `renders/`) | the pending-disposition evidence chain (PENDING — no run claimed) |
| `hw/tests/traceability.csv` | the ledger (T2 row sim-pending/CL0) |
| `.github/workflows/hw-nightly.yml` | the T2 job (t2-virtual-bench) + failure annotation (T2-DIAG last line) |
| `ci/docker/Dockerfile.t2`, `ci/docker/renode-1.16.1.pin` | T2 toolchain image + Renode identity pin |
| `docs/hw/virtual-bench-plan.md`, `docs/hw/tool-qualification.md` | governing docs (model scopes, not_simulated rule, tool gaps) |
| `docs/hw/t2-bringup-report.md` | **the documented-outcome report written at stop (20/20)**: outcome, criterion, dispatch history, proven/unproven, toolchain pins, SE wording, follow-up |
| issue **#60** | SE-mandated follow-up: F-32 pre-diagnosis + re-budget request for dispatches 21+ |
