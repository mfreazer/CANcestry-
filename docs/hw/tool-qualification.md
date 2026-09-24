# CANcestry Tool Qualification Plan and Evidence (ISO 26262-8 §13)

| Field | Value |
|---|---|
| **Document** | CANcestry Tool Qualification Plan and Evidence |
| **Version** | 0.2.10 |
| **Status** | Draft — H-11, pending QA and Human Reviewer approval |
| **Owner** | System Engineer |
| **Approver** | QA Lead |
| **Last Review** | 2026-09-24 |
| **Repository location** | `docs/hw/tool-qualification.md` |
| **Governing documents** | ISO 26262-8:2018 §13; hardware policy |

## 1. Purpose and scope

Implements verification discipline for HW-SF-001..005, HW-FR-004 and
HW-FR-009. Tool confidence is not model credibility or hardware qualification.
Independent regressions bound the behavior exercised; they do not qualify all
compiler semantics or establish physical CL3 correlation.

## 2. Classification scheme

- **TI1**: confidence that tool malfunction cannot introduce or fail to detect
  errors in a safety-related item. **TI2**: all other cases.
- **TD1 / TD2 / TD3**: high / medium / low confidence in detecting malfunction
  and erroneous output.
- **TCL1**: TI1, or TI2 with TD1. **TCL2**: TI2 with TD2.
  **TCL3**: TI2 with TD3. TCL2/TCL3 require qualification evidence.

## 3. Controlled classification table

This table is machine-read by `ci/check_hw_traceability.py`. Tool IDs match
keys in an evidence artifact's `tool_pins`. `python` and `numpy` are recorded
runtime dependencies, not independently classified evidence-producing tools.
An unknown producer, pending classification, missing table or empty TCL2+
gap fails closed. `-` represents an empty tool gap for TCL1 only.

<!-- BEGIN TOOL CLASSIFICATION -->
| tool_id | Tool / role and rationale | TI | TD | TCL | validation_gap |
|---|---|---|---|---|---|
| openmodelica | OpenModelica compiles Modelica into FMUs. A compiler bug can silently change simulation semantics, introducing errors into safety-related artifacts. OR-001/OR-002 regressions cover only exercised semantics. | TI2 | TD2 | TCL2 | Compiler semantics outside independently validated output remain unqualified; OR-001/OR-002 regressions do not cover all translation and solver behavior. |
| fmpy | Bounded, independently oracle-checked FMU execution/readout only: OR-001 uses explicit CoSimulation `doStep` scheduling and OR-002 uses the guarded zero-state ModelExchange evaluator. Subject to §3.1; TD1 is conditional on detection coverage for the exact claimed outputs, not a claim that FMPy cannot introduce errors. | TI2 | TD1 | TCL1 | - |
| capellambse | capellambse is a model reader, not a safety-case producer. Live structural checks and negative fixtures detect missed linkage/parse errors. | TI2 | TD1 | TCL1 | - |
| cancestry-render-modelica | Pure-Python SVG renderer (H-05): it draws only what a schema-validated plot-data file declares, verifies the pinned source-evidence hash before drawing, and refuses to run on a broken hash chain or an unknown vocabulary. A defect can at worst fail to display a validated claim; it cannot introduce or alter a numerical result. | TI1 | TD1 | TCL1 | - |
| fmi_bridge | Deterministic FMI 3.0 co-simulation master (H-07, issue #53): integer-microsecond fixed-step scheduling (100 µs master / 1 µs FMU internal), no wall clock and no randomness (source-lint-enforced), marshals plant/MCU values, captures events from the platform's hook-stamped trace slots and asserts the strict QA-EV-01 ordering against the OR-001-anchored plant trajectory. Every exchanged value is recorded in the hashed trace and the event times come from Renode-side registers, not from bridge computation: a bridge defect can at worst fail to detect a mismatch or halt the run; it cannot introduce or alter a numerical result into the item. Bounded contract tests HW-T2-BRIDGE-001..015 support the detection argument for this exact configuration. | TI1 | TD1 | TCL1 | - |
| fmeda | `tools/fmeda-calculator.py` (H-09, issue #56): ISO 26262-5:2018 Annex C metric calculator (SPFM, LFM, PMHF, MTBF) over `hw/fmeda/fmeda-analysis.csv` and `hw/bom/fit-database.json`. A defect can silently misstate a hardware architectural metric in `docs/hw/fmeda.md` and the reliability figures of `docs/hw/rams-summary.md`, so it can introduce an error into a safety-related work product (TI2). Detection (§4.4): exact rational arithmetic with fixed half-up rendering, an independent Annex C oracle, the public TI SLYP685 n = 1..4 teaching example and the Chalmers 2023 aggregate example reproduced to publication precision, byte-identical output across runs, and the `--check` drift gate on the CSV's derived columns and the generated document blocks (TD2, not TD1: the ISO 26262-5:2018 Annex E worked example is not publicly available and is not reproduced). | TI2 | TD2 | TCL2 | ISO 26262-5:2018 Annex E Table E.1 worked example not reproduced (values not publicly available); qualification rests on the Annex C equations, the exact-arithmetic oracle and two public fixtures (TI SLYP685, Chalmers 2023); the T0 inputs (assumed failure-mode distributions, secondary-tabulated SN 29500 rates) are outside the tool's qualification scope and no evidence artifact may claim ASIL-B metrics from them. |
| renode | Renode executes the real v1.0.0 firmware ELF on the T2 virtual bench (H-07, issue #53). A platform-model defect (scripted IWDG/RTC_BKP/GPIO/RCC peripheral models, DWT accuracy) can silently alter firmware-visible register or timing semantics, introducing errors into T2 evidence — the ELF cannot be re-derived independently, so detection rests on golden-trace/vendor-reference correlation which does not exist yet. | TI2 | TD2 | TCL2 | Renode peripheral models (IWDG, GPIO, RTC_BKP, DWT) are scripted emulations, not vendor-validated silicon models: platform-model bugs can alter firmware-visible timing or register semantics, and this gap is inherited by every T2 evidence artifact; golden-trace correlation against vendor reference behavior and T4 bench correlation are required before any promotion. |
| can_fault_injector | CAN bus fault injector peripheral (H-10, issue #62): `hw/virtual-bench/renode/can_fault_injector.py`, a scripted `Python.PythonPeripheral` that projects the Bus-Off / CRC fault CONDITION of a shared CAN medium into the FDCAN register scratch and evaluates the ISO 11898-1 128 × 11 recovery sequence as a pure function of emulation virtual time. It cannot inject a passing result into the firmware under test: a defect can at worst mis-project the condition, which either fails to produce the scenario events (the runners abort fail-closed before evidence) or is caught by the per-step readback, the injector error register, the injection counters and the HW-T2-BUSOFF-001 contract-lockstep tests — it never fabricates a detection/recovery that the firmware did not perform, so the tool only fails to detect, and misfires are detected (§4.5). | TI1 | TD1 | TCL1 | - |
| bor_reset_injector | BOR reset injector peripheral (H-11, issue #64): `hw/virtual-bench/renode/bor_reset_injector.py`, a scripted `Python.PythonPeripheral` that models the ACTIVE-LOW NRST reset line (asserted when the brownout is injected, released exactly 100 us later), discards the T2 main-SRAM marker word, sets RCC_CSR.BORRSTF through the bus so the scripted RCC model keeps the reset cause in its retention shadow, and requests the BOR machine reset so the REAL firmware re-enters its reset handler. It cannot inject a passing result into the firmware under test: it never writes the firmware's own slots (main-SRAM observation, recovered code, run-complete, alive counter) and it never fabricates a detection or a recovery. A defect can at worst fail to produce the scenario events (the runners abort fail-closed before evidence) or be caught by the per-access readback, the injector error register, the exactly-one-injection rule and the HW-T2-BROWNOUT-001..011 contract-lockstep tests - it only fails to detect, and misfires are detected (section 4.6). | TI1 | TD1 | TCL1 | - |
<!-- END TOOL CLASSIFICATION -->

### 3.1 FMPy reclassification precondition (F1, normative)

Implements HW-SF-002 / HW-FR-004. **TCL1 is conditional on the bounded use
above, not an intrinsic property of FMPy.** FMPy is TI2: an executor, master
algorithm or reader can introduce errors as well as fail to detect them.
The present TD1 argument is limited to outputs actually checked by independent
oracles and negative fixtures. OR-001 checks the declared hold-up trajectory;
OR-002 currently supports numerical regression only, not a qualified passing
pulse claim. A version pin or a green regression alone is not a TD1 argument
for other behavior or a broader safety claim.

**Before extending that use, reassess and, where TD1 is no longer established,
reclassify FMPy to TCL2 (TI2/TD2), or TCL3 if only TD3 is justified.** This
precondition applies before accepting any new safety-related evidence that
relies on FMPy numerical integration, state/event handling, interpolation or
resampling, co-simulation/master coupling, or trace/result transformation
outside the independently checked output scope. A new FMU/model, execution
mode, solver/configuration or tool version also requires review of that scope;
a nominally unchanged reader role must not silently inherit the old argument.

The maintainer/QA review must, **before promotion or consumption as passing
evidence**:

1. Identify the changed producing role, relevant failure modes and all output
   semantics the safety claim relies on; justify TI/TD with independent
   oracle coverage and positive/negative tests for that exact configuration.
2. Record the disposition in this section and the controlled table. If TD1
   cannot be demonstrated, TCL1 must not remain by default: apply TCL2/TCL3
   with an explicit validation gap (or leave classification/evidence pending
   until the assessment is complete).
3. Pin the configuration, reissue affected source/evidence hashes and apply
   §5's enforced TCL2/TCL3 gap inheritance. No passing claim may omit that gap
   merely because the earlier, narrower FMPy role was TCL1.

This is a precondition on reuse, not retrospective tool qualification or a
promotion of the currently pending pulse evidence. OpenModelica remains TCL2.

**H-11 disposition (issue #64, v0.2.10): brownout/BOR configuration.** The
brownout scenario steps a SECOND FMU (the BOR plant, `CancestryLib.Power.BOR`)
with the same pinned FMPy 0.3.24 CoSimulation path, and its outputs are **not**
oracle-checked: no registered oracle covers the BOR threshold behaviour, the
hysteresis release gate or the reset timing (issue #64: oracle none), and the
retention-domain branch is the only part OR-001 witnesses (analytically, at
T1). Per this section's precondition the bounded TD1 argument therefore does
**not** extend to the BOR configuration: BOR FMU output may be consumed only
inside the pending / CL0 disposition, and a future passing claim built on it
requires (i) a registered oracle validating that output, or an explicit
FMPy TCL2/TCL3 reclassification for this configuration, (ii) this section and
the controlled table updated, and (iii) re-issued hashes and gaps per item 3
below. The scenario's own invariants are ordering, liveness and register-state
checks over emulation virtual time (the injector stamps and the platform
hooks): none of them is an FMPy output claim.

**FMI lifecycle observation (H-11, v0.2.10).** The OpenModelica FMU runtime
accepts `fmi2Terminate` only in `modelEventMode`/`modelContinuousTimeMode`
(`SimulationRuntime/fmi/export/openmodelica/fmu2_model_interface.c.inc`:
`fmi2Terminate` -> `invalidState(..., modelEventMode|modelContinuousTimeMode,
~0)`) and returns `fmi2Error` otherwise: the terminate status is decided by
the runtime's internal state machine, not by the simulated values. The T1
brownout check (`hw/tests/test_bor_physics.py`) and the brownout runner
therefore treat it as **non-value-carrying**: the FMU's outputs are read and
asserted first (every recorded microsecond is compared against the reference
implementation) and the terminate outcome is recorded and printed rather than
allowed to invalidate a verified trajectory. The pinned runtime emits its
failure-level log only when debug logging is switched on, and in dispatch
36036288651 enabling `loggingOn` changed the runtime's **status reporting**
for the collapse-boundary step (the identical `doStep` call reported
`fmi2Error` with logging on and `OK` with logging off, while the FMU's values
stayed correct at every sampled instant). The value guarantee is therefore the
sample-by-sample comparison, not the log; this is a documented tool behaviour,
not a modelling deviation, and it is inherited by any future FMU that reuses
this lifecycle code. Re-verify both observations if the OpenModelica pin or
the FMU build shape changes.

**H-06 disposition (issue #49, v0.2.6): pulse 5a configuration.** The H-06
selector-8 branch recompiles the `PulseISO7637_2` FMU (new equation branch and
four new parameters), so this precondition was applied before the
configuration was consumed. Review outcome: the producing role is unchanged —
the same pinned OpenModelica 1.24 build and FMPy 0.3.24 ModelExchange path,
the same zero-state/no-discrete guard, the same deterministic feature grids
and the same bounded OR-002 numerical-regression outputs (peak, time-to-peak,
td/3 decay, duration) now including the `pulse5a` invariants with their own
positive and negative fixtures. No safety claim relies on new FMPy output
semantics (no integration, event, coupling, interpolation or transformation
scope is added: the source stays stateless and algebraic), and the pulse 5a
evidence is consumed strictly as pending/CL0 with a `pending_reason` deferring
to #41 — never as passing evidence. FMPy therefore remains TCL1 for this
configuration under the same bounded argument, OpenModelica remains TCL2, and
the 5a case manifest inherits the OpenModelica gap verbatim. The controlled
table above is unchanged; this paragraph is the §3.1-required disposition
record.

**H-07 disposition (issue #53, v0.2.7): T2 virtual-bench configuration.**
The T2 foundation re-uses the same bounded FMPy role for the Holdup plant
(pinned OpenModelica 1.24 build, FMPy 0.3.24, explicit CoSimulation
`doStep` scheduling — the §4.1 argument carries over), but the execution
context is new: the FMU is now stepped by `fmi_bridge` inside a coupled
co-simulation with Renode executing the real v1.0.0 firmware ELF. Three
dispositions apply before any T2 evidence is consumed:

1. `fmi_bridge` is classified TI1/TD1/TCL1 for the bounded role above
   (integer fixed-step master, hashed trace of every exchanged value,
   event timestamps read from Renode-side registers, contract tests
   HW-T2-BRIDGE-001..015). Any extension of the bridge role — resampling,
   interpolation, master-side computation entering a claim, event synthesis —
   voids the TI1 argument and triggers this precondition again.
2. `renode` is classified TI2/TD2/TCL2: the scripted peripheral models can
   introduce errors that the ELF cannot reveal. Its validation gap is
   inherited verbatim by every T2 evidence artifact via the §5 inheritance
   rule. Golden-trace/vendor-reference correlation is a prerequisite for any
   promotion; T4 bench correlation remains required for CL3.
3. The Holdup FMU for T2 is built with `version="3.0"` (FMI 3.0,
   virtual-bench-plan §1) — a new FMU configuration in the §3.1 sense. The
   producing role (stateful CoSimulation plant, explicit `doStep`, readout
   of `v`) is unchanged and independently OR-001-checked inside every T2
   run, so FMPy remains TCL1 for this configuration under the same bounded
   argument. Review before consuming any passing T2 claim.

No executed T2 run exists yet (issue #53 delivers the foundation; the
pending manifest `hw/tests/evidence/t2_retention_001.json` is CL0); this
paragraph is the §3.1-required disposition record, not a promotion.

**H-10 disposition (issue #62, v0.2.9): T2 CAN fault scenarios (Bus-Off +
CRC).** The H-10 scenarios add a new execution configuration to the T2
bench, so this precondition was applied before their evidence is consumed:

1. **The fault scenarios do not use FMPy or an FMU.** The CAN bus medium is
   the `can_fault_injector` scripted peripheral; no plant model is built or
   stepped. The §3.1 FMPy reclassification precondition is therefore **not
   triggered** for these scenarios: nothing relies on FMPy numerical
   integration, state/event handling, interpolation or coupling, and the
   OpenModelica compiler contributes nothing to the artifacts (its gap is
   not inherited by `t2_busoff_001.json` / `t2_crc_001.json`). The
   retention scenario and its FMPy/OpenModelica dispositions are unchanged.
2. **`renode` stays TI2/TD2/TCL2**, and its gap is inherited verbatim by
   every H-10 fault-scenario artifact (pending and executed forms), exactly
   as for the retention evidence — the fault scenarios depend on the same
   CPU model, the same virtual-time base and an added scripted peripheral.
3. **`can_fault_injector` is classified TI1/TD1/TCL1** for the bounded role
   above (shared-medium fault-condition projection + deterministic lazy
   evaluation; one-writer-per-slot trace contract; no frame delivery and no
   protocol engine). Any extension of that role — frame synthesis, a
   protocol engine, master-side computation entering a claim — voids the
   TI1 argument and re-triggers this precondition.
4. **Consumption stays pending-only / CL0.** No executed T2 fault-scenario
   run exists yet; `t2_busoff_001.json` and `t2_crc_001.json` are committed
   as pending manifests (pass=false). No registered oracle covers Bus-Off
   timing or CRC handling (issue #62), so `oracle_id` is the literal
   `none`, the traceability rows `(HW-FR-003, virtual_bench)` stay
   `sim-pending` / CL0 and this paragraph grants no promotion.

This paragraph is the §3.1-required disposition record for the new T2
scenarios; it is a precondition on consumption, not tool qualification.

## 4. Qualification regression records

### 4.1 OpenModelica + FMPy against OR-001

- Requirement: HW-SF-002; supports HW-FR-009.
- Pins: OpenModelica 1.24, FMPy 0.3.24, NumPy 2.1.3.
- Case: `hw/tests/cases/holdup_001.simcase.json`.
- Harness: `hw/tests/test_power_sim.py`, actual headless FMU compilation and
  FMPy simulation against the independently derived closed form.
- Acceptance: VBAT trace error ≤ 0.001 V; retention floor held for the declared
  150 ms event. The tolerance and budget parameters remain provisional.
- Manifest: `hw/tests/evidence/holdup_001.json`. Per-run trace/hash and measured
  tool versions: `build/hw/holdup_001.runlog.json` (CI artifact, not Git data).
- Gap: charge branch/R_path, nonlinear leakage, temperature-dependent ESR,
  reset-domain behavior and physical brownout correlation are not validated.

#### 4.1.1 N2 execution-mode audit and conditional TD1 disposition

Implements HW-SF-002 / HW-FR-009. The qualified hold-up configuration is now
explicit rather than inferred from FMPy's default selection: the
[`pipeline()`](../../hw/tests/test_power_sim.py) builds only OpenModelica
`fmuType="cs"`; [`simulate_fmu()`](../../hw/tests/test_power_sim.py) reads the
FMU description, requires its `coSimulation` interface, and calls FMPy with
`fmi_type="CoSimulation"`. The real-FMU test asserts that interface and its
actual retention-voltage evolution. The [hold-up model](../../hw/model/CancestryLib/Power/Holdup.mo)
declares `der(vC)`, so the plant is stateful even though its state is advanced
inside the compiled CoSimulation FMU. A metadata fixture proves that a missing
CoSimulation interface cannot reach `fmpy.simulate_fmu()`; another fixture
proves that a stateful FMU is passed to FMPy with the explicit mode. Those
fixtures are execution-mode unit tests, not additional FMU simulations.

The pinned [FMPy 0.3.24 source](https://github.com/CATIA-Systems/FMPy/blob/ea96560c1b4f17f7360406c534991d72f00fa7bf/src/fmpy/simulation.py#L700-L789)
dispatches that explicit mode to `simulateCS()`, not `simulateME()`; its
[CoSimulation loop](https://github.com/CATIA-Systems/FMPy/blob/ea96560c1b4f17f7360406c534991d72f00fa7bf/src/fmpy/simulation.py#L1196-L1325)
schedules `fmu.doStep()` and records output. Consequently, the hold-up plant is
stateful, but FMPy is not selected as the ModelExchange/CVode numerical solver
for this configuration: integration of that state is inside the compiled
CoSimulation FMU. FMPy can still introduce scheduling or readout errors, so
this is not a "stateless reader" claim and it does not remove the
OpenModelica compiler gap.

`test_trace_matches_oracle()` compares every returned OR-001 `(time, v)` sample
with the independently derived closed form and rejects a maximum absolute
trajectory error greater than the sim-case-declared 0.001 V; the oracle also
self-checks its closed form against independent fixed-step RK4. Together with
the explicit mode/metadata checks, this supports the documented **conditional
TD1/TCL1** detection argument only for this pinned 1.24/0.3.24,
CoSimulation, one-FMU, no-coupling hold-up output and its stated tolerance. It
does not detect hidden compiler defects, establish an independent-validation
waiver, qualify an unobserved output, or extend to ModelExchange integration,
events, coupling, interpolation/resampling, transformations, a different
FMU/configuration/version, or a future use. Those uses remain subject to the
§3.1 reclassification precondition.

### 4.2 OpenModelica + FMPy against OR-002: regression only, qualification pending

- Requirement: HW-FR-004. Same tool pins; case
  `hw/tests/cases/pulse_7637_001.simcase.json`.
- `hw/tests/test_pulse_sim.py` executes the compiled stateless source via
  FMI ModelExchange/FMPy, with deterministic feature grids and bounded event
  iteration. **N2 code pointer:**
  [`execute_pulse()`](../../hw/tests/test_pulse_sim.py) reads the model
  description, then requires `modelExchange is not None`,
  `numberOfContinuousStates == 0`, and no model variable with
  `variability == 'discrete'`, **before** `fmpy.extract()` or `FMU2Model`
  instantiation. The rejection is of FMU-declared state/unsupported execution
  modes; it is not an independent proof against hidden compiler defects.
  Negative metadata fixtures assert that extraction/native construction are
  never reached for unsupported descriptions; a zero-state, continuous-valued
  algebraic output is accepted past the guard. These fixtures are not extra
  physical/FMU simulation evidence; real positive/faulted regressions also run.
- This rejection boundary is **pulse-specific**. The stateful hold-up path
  [`simulate_fmu()`](../../hw/tests/test_power_sim.py) is not covered by it;
  its bounded TCL1 argument relies on the independent OR-001 trajectory
  comparison in §4.1. Broader solver/master/output uses still trigger §3.1;
  neither this pointer nor green pulse tests grant FMPy unconditional TCL1.
- Invariants: fixture peak within 2%, time-to-peak within 5%, and applicable
  td/3 decay constant within 10%, with duration/recovery checks. These are
  regression tolerances, **not** a standard-qualified oracle or confidence
  intervals. Pulse 4's actual 1 ms / 20 ms / 1 ms engineering dip is also
  asserted at its region boundaries.
- Normative Test B / legacy 5b source: **ISO 16750-2:2012 §4.6.4.2.3,
  Figure 9 / Table 6, pp. 12–13**. Suppressed level corrected to **Us* = 35 V**
  from the validated source extract. The old 2011 Table 11 citation and 40 V
  level were errors, not acceptable qualification limits. Remaining source
  topology/timing correctness is tracked by [#41](https://github.com/mfreazer/CANcestry-/issues/41).
- H-06 (issue #49) adds the pulse 5a configuration to this record. Normative
  Test A / legacy 5a source: **ISO 16750-2:2012 §4.6.4.2.2, Figure 8 /
  Table 5, pp. 11–12** (unsuppressed load dump; the standard itself does not
  use the legacy 5a/5b names). Citation disposition: the H-06 issue body
  cited Table 6 for 5a; the normative source places the Test A parameters in
  Table 5, and the extract/oracle/sim-case chain transcribes Table 5. The
  fixture operating point is the footnote-a lower/lower pairing — unclamped
  **Us = 79 V** with **Ri = 0.5 Ω** (unused metadata), **td = 350 ms**,
  **tr = 5 ms** — bound only through
  `hw/bom/datasheets/extract-iso16750-2-2012.json` (`shape_qualification_pending:
  true`) and `hw/tests/cases/pulse_7637_001.simcase.json` (selector 8). The
  qualification evidence for this configuration is the OR-002 `pulse5a`
  numerical regression (peak 2%, time-to-peak 5%, td/3 decay 10%, duration
  2%) with positive and negative invariant fixtures — regression only: the
  Figure 8 waveform shape, its 0.9(Us−UA)+UA / 0.1(Us−UA)+UA edge/duration
  definitions, the unclamped-generator topology, exercised Ri and the
  ten-pulse repetition remain unqualified and are deferred to
  [#41](https://github.com/mfreazer/CANcestry-/issues/41). No promotion.
- Manifest: `hw/tests/evidence/pulse_7637_001.json`, **pending / pass=false /
  provisional=true / CL0**, schema-enforced for incomplete pulse coverage;
  since H-06 it records all eight pulses and pins the 5a artifacts by hash.
  The 5a case manifest `hw/tests/evidence/pulse_5a_001.json` carries the same
  disposition plus the schema-mandatory `pending_reason` referencing #41, and
  its pending placeholder view `hw/tests/evidence/pulse_5a_001.plot.json` →
  `renders/pulse_5a_001.svg` inherits it without improvement (rules 13–14).
  Per-pulse and aggregate run logs separately record `regression_pass`; they
  also carry pending qualification status, never a misleading passing verdict.
- Exact modeled/unmodeled scope, source links and confidence disposition:
  [pulse-coverage.md](pulse-coverage.md). Pulse 4 and 5b cannot be promoted
  from pending until independently qualified; no T4/DUT immunity claim exists.

### 4.3 Renode + FMI bridge against OR-001: T2 foundation, qualification pending

- Requirement: HW-SF-002 (ordering derivation HW-SF-004 / QA-EV-01). Oracle:
  OR-001 anchors the plant trajectory check executed inside every T2 run.
- Configuration (H-07, issue #53): Renode executes the real v1.0.0 firmware
  ELF on `hw/virtual-bench/renode/stm32g474-cancestry.repl` (scripted
  IWDG/RTC_BKP/GPIO/RCC peripherals; scope and `not_simulated` rationale in
  the platform header); `fmi_bridge` couples it to the Holdup FMU at a fixed
  100 µs master step / 1 µs FMU internal step with deterministic seeds and
  no wall clock; `run_t2_retention.py` orchestrates, asserts the strict
  retention < safe-latch < IWDG-fire ordering and writes
  `hw/tests/evidence/t2_retention_001.json` (schema
  `hw-t2-evidence-0.1.0.schema.json`).
- Status: **no executed T2 run exists.** The committed evidence artifact is
  the pending manifest (pass=false, CL0); the ledger row
  (HW-SF-002, virtual_bench) is `sim-pending`. The event-capture mechanism
  (CPU symbol hooks stamping exact emulation times) and the deterministic
  bridge are unit-tested (HW-T2-BRIDGE-001..015, HW-T2-ORCH-001..006), but
  unit tests are not T2 evidence.
- Acceptance for a future passing claim: a full pipeline run on
  Renode-equipped infrastructure (HW-PLAN C5) that (a) captures all three
  QA-EV-01 events in strict order within 150 ms, (b) matches OR-001 within
  the sim-case tolerance, (c) shows the retained fault code preserved across
  the scripted IWDG reset, and (d) reproduces the committed evidence
  byte-for-byte via `--check` — plus the §3.1 disposition review, golden
  Renode-trace correlation to bound the TCL2 gap, and Human Reviewer
  sign-off for the retention/fail-safe model per HwAGENTS.md rule 6.
- Not covered: BOR/supervisor reset (not_simulated, rule 6), DWT accuracy,
  CAN protocol simulation, stimulus injection (the QA-EV-01 escalation
  recipe reaching the firmware via CAN traffic is not yet defined), and all
  T4 physical correlation.

### 4.4 FMEDA calculator against public reference computations (H-09, issue #56)

- Requirement: HW-SF-005 (FMEDA; oracle OR-004 re-scoped to the FMEDA metric
  recomputation, `hw/tests/oracles/registry.json`) and HW-NF-004 (FIT
  provenance). Tool: `tools/fmeda-calculator.py` v0.1.0, tool id `fmeda`.
- Role: computes the ISO 26262-5:2018 Annex C metrics (C.1..C.8: SPFM, LFM,
  PMHF with the ISO 26262-10 simplified dual-point term and an explicit
  lifetime parameter, reliability MTBF) from `hw/fmeda/fmeda-analysis.csv`
  and `hw/bom/fit-database.json`, renders `docs/hw/fmeda.md`'s generated
  blocks and feeds `ci/check_hw_reliability_growth.py`. TI2: a wrong metric
  is a wrong safety-related claim. TD2: every quantity is exact
  (`fractions.Fraction`) and independently re-derivable, but the standard's
  own worked example is not available for a full-output comparison.
- Qualification evidence (`tests/unit/tools/test_fmeda_calculator.py`,
  hw-fast gate 2 with the 95 % branch-coverage bar, and gate 1.7 `--check`):
  (a) an independent Annex C oracle written without shared code, compared as
  exact fractions over a diagnostic-coverage sweep covering the Annex D
  classes 60 / 90 / 99 % and the 0 / 100 % corners; (b) the public TI SLYP685
  teaching example, iterations n = 1..4, specified by the schema-validated
  extract `hw/bom/datasheets/extract-ti-slyp685-fmeda-example.json`: every
  FIT-level sum (111/56, 121/11/55, 121/11/10, 131/1.5/20) and PMHF (56, 66,
  21, 21.5 FIT under the source's RF + MPF,L simplification) reproduced
  exactly, percentages reproduced to the published one-decimal precision
  (49.5 %, 90.9 %, 50.0 %) where the publication is correctly rounded, and
  the two publication defects (n = 3 "90.1 %" for 1 − 10/110 = 90.9 %; n = 4
  truncated 98.8 % / 84.5 %) asserted as defects rather than reproduced; (c)
  the Chalmers 2023 aggregate example (127 FIT safety-related, Σ(SPF+RF)
  6.9055, ΣMPF,L 7.4, ΣMPF,DP 76.372, T = 1e5 h) reproduced to the published
  two decimals (SPFM 94.56 %, LFM 93.84 %) and three decimals (PMHF 6.962
  FIT); (d) the repository FMEDA byte-identical across runs and against the
  committed document; (e) every fail-closed path (drifted derived column,
  fractions not summing to 1, FIT database mismatch, malformed CSV/JSON,
  missing inputs, inconsistent classification flags).
- Declared gap (normative): the issue #56 acceptance criterion names the
  "ISO 26262-5:2018 Annex D Tables D.1..D.3 worked example". In the 2018
  edition Annex D is the diagnostic-coverage evaluation (Table D.1 = the DC
  classes, used here as the DC vocabulary) and the SPFM/LFM worked example is
  Annex E, Table E.1. Neither the licensed text nor any public source quotes
  Table E.1's rows, so the calculator is **not** validated against it; the
  qualification claim is limited to the Annex C equations and the two public
  fixtures above. Obtaining the licensed standard and adding the Table E.1
  fixture is a QA action before any passing ASIL-B metric claim
  (`hw/tests/traceability.csv` HW-SF-005 stays `analysis-pending`).
- Not covered: the correctness of the inputs (failure-mode distributions are
  T0 assumptions; FIT rates are secondary-tabulated SN 29500 classes), the
  FTA cut-set analysis demanded by HW-SF-005, and any T2/T4 evidence.

### 4.5 Renode + CAN fault injector against no oracle: CAN fault scenarios, qualification pending

- Requirement: HW-FR-003 (ISO 11898-2 CAN interface conformance), exercised
  through the gateway node's Bus-Off recovery and CRC error handling. Oracle:
  **none** (issue #62; HwAGENTS.md rule 3 is satisfied by the honest
  sim-pending / CL0 disposition, never by an invented oracle id).
- Configuration (H-10, issue #62): Renode executes the real v1.0.0 firmware
  ELF (off-tree driver `hw/virtual-bench/firmware/can_fault.c`, built with
  `CANCESTRY_T2_DRIVER=can_fault`) on the UNCHANGED H-07 platform plus the
  `can_fault_injector` scripted peripheral at 0x40000000 (shared-medium
  fault-condition projection; ISO 11898-1 128 × 11 recovery sequence as a
  function of emulation virtual time); `cancestry-hw-fault.resc` installs
  the fault-event symbol hooks and the CCCR/IR write hooks;
  `t2_fault_common.py` orchestrates with the 100 µs master step of the H-07
  bridge contract and asserts the invariants in-run; the scenarios write
  `t2_busoff_001.json` / `t2_crc_001.json` (schema
  `hw-t2-fault-evidence-0.1.0.schema.json`).
- Documented deviations from the issue's wording, all recorded in the
  evidence schema description: (a) the evidence hash chain is ELF + bridge
  + injector + trace (no FMU — the medium is the injector, so no FMU hash
  exists); (b) the "CAN error interrupt handler" hook is realized as hooks
  on the firmware's error-detection functions (`t2_can_busoff_detect`,
  `t2_can_busoff_recover`, `t2_can_crc_detect`) because the v1.0.0 ELF ships
  no CAN error ISR (platform HAL is poll-driven, no device IRQs);
  (c) the CRC error is raised through the v1.0.0 normative code
  `CANCESTRY_HAL_FAULT_MALFORMED_FRAME` — the nearest fault code for a
  corrupted wire frame — and is recorded as such.
- Status: **no executed T2 fault-scenario run exists.** Both committed
  artifacts are pending manifests (pass=false, CL0); the ledger rows
  `(HW-FR-003, virtual_bench)` are `sim-pending`. The scenario machinery is
  host-tested offline (HW-T2-BUSOFF-001..008, HW-T2-CRC-001..006), which is
  not T2 evidence.
- Acceptance for a future executed run: the hw-nightly T2 job runs each
  scenario twice (`--check`) against the committed artifact, the Bus-Off
  run captures detection/recovery with `recovery_us - detection_us < 704 µs`
  (128 × 11 bit times at 2 Mbit/s) and the medium releasing exactly 704 µs
  after the recovery request, and the CRC run shows the firmware counter
  incremented with `run_complete = 1` — plus §3.1's disposition review. Even
  a green pair stays **sim-pending / CL0**: promotion is gated on a
  registered oracle and, per issue #62, on HS-01/HS-02 plus a human safety
  reviewer, which H-10 does not touch.
- Not covered: brownout/BOR physics (deferred to H-11, HS-01/HS-02,
  HwAGENTS.md rule 6), the FDCAN protocol engine / arbitration / physical
  layer (`not_simulated`), frame delivery and the no-corrupted-frame
  property of HIL-CRC-001 (software boundary conformance suite), DWT
  accuracy (deliberately unexercised), multi-channel / CRC-storm / thermal
  scenarios (issue #62: no scope creep), and all T4 physical correlation.

### 4.6 BOR plant + reset injector: brownout scenario, qualification pending

- Requirement: HW-SF-002 (sub-events (ii) main-rail brownout to BOR level 3
  and (iii) main-rail removal from BOR to 0 V - the reset side), HW-SF-004
  (retention write before the rail reaches the BOR level). Oracle: **none**
  (issue #64; HwAGENTS.md rule 3 is satisfied by the honest sim-pending / CL0
  disposition, never by an invented oracle id).
- Configuration (H-11, issue #64): Renode executes the real v1.0.0 firmware
  ELF (off-tree driver `hw/virtual-bench/firmware/bor_brownout.c`, built with
  `CANCESTRY_T2_DRIVER=bor_brownout`) on the UNCHANGED H-07 platform plus the
  `bor_reset_injector` scripted peripheral at the issue-pinned 0x40001000
  (NRST active-low with a 100 us pulse, main-SRAM marker discard,
  RCC_CSR.BORRSTF, BOR machine reset), brought up by
  `cancestry-hw-bor.resc`. The plant is the BOR FMU - FMI 2.0 CoSimulation,
  built headless by the pinned OpenModelica 1.24 from
  `hw/model/CancestryLib/Power/BOR.mo` under
  `hw/tests/cases/bor_brownout_001.simcase.json` - stepped by
  `t2_bor_common.py` over the H-07 100 us master / 1 us FMU contract; the
  MODELLED BOR assertion drives the injector command, so the injection is a
  plant-driven event and not a scenario constant that happens to be 10 ms.
- The BOR Modelica model as a TCL2-governed artifact (issue #64 deliverable
  13: "BOR Modelica model as TCL2"). The model is not a tool, so it is not a
  controlled-table row: its PRODUCER (`openmodelica`, TCL2) is, and the
  producer gap is inherited verbatim by every artifact that pins the BOR FMU
  (the pending manifest carries the exact sorted string of
  `ci/check_hw_traceability.py` rule 8). The model-level hazard the issue
  names - a physics-modelling bug producing a plausible brownout trajectory -
  is recorded as this section's validation gap: no independent oracle
  witnesses the BOR threshold behaviour, the hysteresis release gate or the
  reset timing, and the hysteresis value and the resumption latency are
  declared engineering fixtures. The model may therefore only ever be consumed
  pending / CL0; the T1 regressions (`hw/tests/test_bor_physics.py`
  HW-PHYS-BOR-001..008 plus the toolchain FMU check HW-SIM-BOR-001) bound the
  behaviour without qualifying it.
- Documented deviations from the issue's wording: (a) the evidence hash chain
  is FMU + ELF + bridge + injector + trace (the reset line is a platform
  extension, so its hash joins the chain); (b) the issue's "symbol hook on the
  reset handler" is realized on the firmware's post-reset BOR detection entry
  (`t2_bor_detect`) because the v1.0.0 ELF ships no BOR classification in its
  reset handler; (c) the production retention drain
  (`cancestry_watchdog_restore_retained_fault`) is deliberately NOT exercised,
  so the preservation claim is asserted from the RTC_BKP0R readback itself;
  (d) the NRST release stamp is the MODELLED instant (assertion + 100 us) that
  the lazy evaluation applies at the first access at or after it, never the
  detecting poll time.
- Status: **no executed T2 brownout run exists.** The committed artifact is a
  pending manifest (pass=false, CL0, `oracle_id` "none"); the ledger row
  `(HW-SF-002, virtual_bench)` stays `sim-pending`. The scenario machinery is
  host-tested offline (HW-T2-BROWNOUT-001..011), which is not T2 evidence.
- Acceptance for a future executed run: the hw-nightly T2 job runs the
  scenario twice (`--check`) against the committed artifact; a passing run
  captures `retention_write < bor_detect < recovery`, NRST asserted for
  exactly 100 us, the modelled assertion equal to the injection stamp, the
  retained code preserved with main SRAM discarded, RCC_CSR.BORRSTF set with
  IWDGRSTF clear, and `run_complete = 1` - plus the section 3.1 disposition
  above. Even a green pair stays **sim-pending / CL0**: promotion is gated on
  HS-01/HS-02 plus a human safety reviewer (HwAGENTS.md rule 6), which H-11
  does not touch.
- Not covered: the BOR cell behaviour on silicon (threshold spread over
  temperature, option-byte configuration, backup-domain switching), real
  brownout shapes / finite rail fall time / repeated brownouts, the
  retention-drain path, DWT accuracy (deliberately unexercised), the CAN and
  thermal scenarios (issue #64: no scope creep), and all T4 physical
  correlation.

## 5. Validation-gap inheritance (normative)

The hardware ledger `hw/tests/traceability.csv` adds
`inherited_validation_gap`. For each artifact, the checker derives producers
from `tool_pins` and computes the sorted, exact `tool_id: validation_gap` text
for every TCL2/TCL3 producer. Multiple gaps are joined with ` | `.
TCL1-only evidence and pending ledger rows must have an empty field. A
produced pending pulse artifact records its own `inherited_validation_gap`,
which the checker validates even without a closure-evidence ledger link.
The OR-002 row uses `virtual_bench` (CL0, sim-pending); this method names the automated
bench harness and does not turn T1 source-model evidence into T2 integration.
The software ledger and its checker are unchanged.

Naming an oracle is **not** an independent-validation waiver. A full-output
waiver requires a schema-validated `independent_tool_validation` reference
in the artifact: a registered oracle serving the same requirement, an actual
output pinned in `source_hashes`, and an independently hashed witness report
with `pass: true`, `scope: full_output`, matching output digest, nonempty
hashed source provenance, and independence from the producer. The witness
must not list that producer or another unqualified tool in its own pins.
QA still reviews the independence claim; hash matching is not an authorship
proof. An absent or partial witness retains the tool's gap; a malformed
waiver fails the gate. A valid full-output waiver removes only that tool's
gap, never the oracle's/model's physical validation gap.

The current OR-001/OR-002 regressions deliberately **do not claim a full-output
waiver**. The passing OR-001 ledger row and the pending OR-002 JSON artifact
retain the OpenModelica gap. The OR-002 ledger row itself has no closure
evidence and no inherited gap. OR-001 remains provisional pending T4 for CL3;
OR-002 has no qualified passing claim. Oracle/model gaps remain in
`hw/tests/oracles/registry.json` and each evidence manifest's `not_covered`.

## 6. Change log

| Version | Date | Change |
|---|---|---|
| 0.2.10 | 2026-09-24 | H-11 #64: `bor_reset_injector` classified TCL1 (TI1/TD1) for the bounded reset-line role; the BOR Modelica model recorded as a TCL2-governed artifact (openmodelica gap inherited verbatim, plus the model-level no-oracle gap in the new §4.6); §3.1 disposition for the new T2 brownout configuration (BOR FMU output is NOT inside the bounded FMPy TD1 argument - no passing claim may be consumed from it). Hash cascade re-issued for every evidence artifact and visual pinning this document. No qualification promotion; the brownout row stays sim-pending / CL0. |
| 0.2.9 | 2026-09-24 | H-10 #62: `can_fault_injector` classified TCL1 (TI1/TD1) for the bounded shared-medium fault-projection role; §3.1 disposition for the new T2 CAN fault scenarios (no FMPy/FMU — precondition not triggered; renode gap inherited verbatim; pending-only consumption at CL0); §4.5 qualification-pending record incl. the documented deviations (ELF+bridge+injector+trace hash chain, detection/recovery function hooks, MALFORMED_FRAME mapping). Hash cascade re-issued for every evidence artifact pinning this document. No qualification promotion; Bus-Off/CRC rows stay sim-pending / CL0. |
| 0.2.8 | 2026-09-24 | H-09 #56: fmeda classified TCL2 (TI2/TD2) for `tools/fmeda-calculator.py` with the declared Annex E Table E.1 gap; §4.4 qualification record (exact-arithmetic Annex C oracle, TI SLYP685 n = 1..4 and Chalmers 2023 public fixtures reproduced to publication precision, byte-identical output, `--check` drift gate). Hash cascade re-issued for every evidence artifact pinning this document. No qualification promotion of any other tool; no ASIL-B metric claim. |
| 0.2.7 | 2026-09-22 | H-07 #53: renode classified TCL2 (TI2/TD2) with the scripted-peripheral validation gap inherited by every T2 artifact; fmi_bridge added as TCL1 (TI1/TD1) for the bounded deterministic-master role; §3.1 disposition for the T2 configuration (FMI 3.0 Holdup build, coupled co-simulation) recorded — pending-only consumption in this PR; §4.3 T2 foundation record added. No executed T2 run exists; no qualification promotion. |
| 0.2.6 | 2026-09-21 | H-06 #49: record the pulse 5a (ISO 16750-2:2012 Test A, §4.6.4.2.2 Figure 8 / Table 5) configuration in §4.2 as OR-002 regression-only evidence with the Table 5-vs-Table 6 citation disposition; add the §3.1 disposition review for the recompiled selector-8 FMU (unchanged bounded FMPy TCL1 scope, OpenModelica stays TCL2, gap inherited verbatim by the new pulse_5a_001 case manifest and its pending placeholder view). Controlled §3 table untouched. No qualification promotion; shape qualification deferred to #41. |
| 0.2.5 | 2026-09-21 | N2 audit (PR #43): pin the hold-up build/execution to explicit CoSimulation, add the §4.1.1 execution-mode audit and the updated fmpy row, test the mode gate and real-FMU interface, and retain FMPy TCL1 only for the bounded OR-001 output-detection argument. Merged on top of 0.2.4. No FMPy gap waiver or pulse promotion. |
| 0.2.4 | 2026-09-20 | H-05 #44: add cancestry-render-modelica to the controlled table as TCL1 (pure renderer over validated data) with the rationale for the bounded role. |
| 0.2.3 | 2026-09-20 | N2: identify the actual pre-instantiation pulse FMU state checks and negative fixtures; distinguish the OR-001 stateful hold-up scope. No unconditional FMPy confidence or pulse-qualification promotion. |
| 0.2.2 | 2026-09-20 | Review F1: make the bounded FMPy TD1/TCL1 argument conditional; require role/configuration reassessment and TCL2/TCL3 reclassification before unvalidated producing uses enter passing evidence. |
| 0.2.1 | 2026-09-19 | H-04 review: correct normative Test B source and 35 V suppressed level; withdraw pulse qualification to pending/CL0; enforce partial-coverage/source/ledger agreement and artifact-level tool inheritance. |
| 0.2.0 | 2026-09-19 | H-04 #38: OpenModelica TCL2 (TI2/TD2); FMPy and capellambse bounded TCL1 roles confirmed; corrected TI definitions; checked gap inheritance and independent-witness contract; real pulse invariants and explicit qualification limits. |
| 0.1.0 | 2026-09-19 | H-02 classification and OR-001/OR-002 regression records. |
