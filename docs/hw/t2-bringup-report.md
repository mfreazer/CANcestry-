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
