# T2 Virtual Bench Bring-Up — Handover (H-08, issue #55)

**Status: F-30 + F-31 APPLIED — awaiting human-triggered dispatch 20
(THE FINAL CYCLE of the 20-cycle budget; 19 used as of this revision).
Session branch `arena/01a0cbe2-cancestry` (continuation of
`arena/01a0c6d8-cancestry`, whose 21 commits were fast-forwarded in).
Dispatch 19 (run on `c185cee`) PROVED F-29: `step3 platform loaded` —
the full `.repl` now constructs including `t2_trace`; the new fault was
`PyDevFromFile … Parameters did not match the signature` (F-30, bare
name tokens rejected for `string` params by the monitor's token-type
table). F-31 (quoted `$t2_seed` → `SetSeed(int)` mismatch) was found in
the same source audit BEFORE it could burn cycle 20. Evidence
re-issued canonically; gates green.**

**Next action:** trigger `hw-nightly` (workflow_dispatch, from the UI —
`gh` dispatch is 403 from the sandbox) **on
`arena/01a0cbe2-cancestry`** for dispatch 20 — the LAST cycle. F-30 +
F-31 are already committed — do not re-apply. On failure, read the
T2-DIAG annotation (section 5.2); if the fault is outside the platform
script, or this cycle is exhausted, apply SE stop criterion 1/2/3
(section 1): stop, write the documented-outcome report, ledger
unchanged, separate follow-up issue. **There is no cycle 21 —
dispatch 20 must either land the live run or trigger the stop
report.**

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

## 4. Dispatch history (hw-nightly, workflow_dispatch, this branch)

Run IDs verified against `gh run list`; older entries per the session
record. Cycles are the SE budget unit (18 used).

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
| 19 | see check-runs on `c185cee` | c185cee (F-29) | **F-29 PROVEN** — `step3: platform loaded` (full `.repl` incl. `t2_trace`); new fault: `machine PyDevFromFile … Parameters did not match the signature` at the first name arg → **F-30** (bare LiteralToken rejected for `string` param); F-31 (quoted seed → `SetSeed(int)`) pre-diagnosed in the same audit → both fixed pre-dispatch-20 |

Note on dispatch 18's pydev row: the `✅ construct (F-28)` entries in
section 2 were **source-verified, not runtime-proven** — the E39 aborted
the include before the `.resc` pydev lines ever ran (dispatch 18's
console markers stop at `step2b`). Runtime proof of the pydev
registrations arrives with dispatch 20 (step3c).

## 5. Procedures that work in this environment

### 5.1 Triggering dispatches
`gh workflow dispatch` returns 403 from the sandbox — **the human
(coordinator/SE) triggers `hw-nightly` from the UI** on the branch. Note a
fresh run may not appear in `gh run list` for a few seconds.

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
err=<160-char runner error>`.

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
    hw/virtual-bench/test_t2_retention.py -q      # ~321 passed
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
  work. Consequence: the Renode 1.16.1 binary **cannot be downloaded** —
  no local platform-load reproduction; CI is the only runtime.
- GitHub log fetches for run logs are blocked — use section 5.2.

## 7. Proven vs unproven

**Proven:** T2 toolchain provisioning (Dockerfile.t2: Renode 1.16.1
identity pin `ci/docker/renode-1.16.1.pin`, arm-none-eabi, fmpy 0.3.24);
off-tree ELF build of the v1.0.0 sources; T2-DIAG diagnostics; the full
gate battery; **platform description load end-to-end (dispatch 19:
`step3: platform loaded`, all peripherals incl. the F-29 `t2_trace`)**;
the `$ORIGIN` expansion mechanism observed in the executed-command echo.

**Unproven (the actual bring-up):** the four `PyDevFromFile`
registrations at runtime (F-30 fix awaits dispatch 20, step3c); ELF
execution (vector SP 0x20018000 readback); preflight magic
0x54324353; `SetGlobalQuantum`/`SetSeed` (F-31); FMU load +
100 µs/1 µs co-simulation stepping; the three symbol hooks
(`cpu AddSymbolHook` exists in 1.16.1 — OsSymbolHook.cs, verified — but
has never executed); the invariant vs OR-001; RTC_BKP0R/RCC shadow
persistence across the scripted IWDG reset; RUN-2 byte-identical
determinism.

Watch items for the first live scenario: (a) the IWDG model's scripted
reset and whether the `machine` reset preserves `t2_trace` contents
(`MappedMemory` is not expected to be cleared by reset — verify from the
evidence, do not assume); (b) the `step3c` cwd probe output (record it in
the report — it documents the CWD behavior for tool-qualification);
(c) any FMU/bridge fault is a **stop-and-document** event per SE criterion
2, not a patch-and-retry one.

## 8. PR, ledger, promotion

- **PR**: superseded mid-bring-up — the original PR #58
  (`arena/01a0c6d8-cancestry` → main, opened 2026-09-23 at `8269dca` +
  this handover doc) was replaced by the continuation PR from
  `arena/01a0cbe2-cancestry`, which contains all of #58's commits
  fast-forwarded plus the F-29 fix. Bring-up only; no live run is
  claimed until dispatch 19/20 evidence lands.
- **Ledger:** `hw/tests/traceability.csv` row `HW-SF-002,virtual_bench`
  stays `sim-pending` / CL0 with empty evidence cells — no promotion,
  grep-checkable.
- **Report:** `docs/hw/t2-bringup-report.md` is written when the live run
  lands (invariant + determinism + hashes) **or** as the honest
  documented-outcome report if an SE stop criterion triggers — including
  the SE-mandated wording that an honest broken/incomplete integration is
  a successful engineering outcome, plus the exact follow-up needed.
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
