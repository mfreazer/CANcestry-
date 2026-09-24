within CancestryLib.Power;
model BOR
  "Brown-out reset (BOR) supervision, reset behaviour and retention-domain hold-up.

   H-11 (issue #64): the item-level power-supervisor plant for the brownout
   fault scenario. The model closes the three BOR physics the bridge marked
   unmodelled and that the T2 platform deliberately left to a
   Human-Reviewer-approved change (stm32g474-cancestry.repl header,
   HwAGENTS.md rule 6):

     (1) supply stimulus: the 3V3 rail is nominal 3.3 V, collapses to 0 V at
         the brownout onset and returns to nominal (issue #64: brownout at
         t = 10 ms, main-rail collapse for 100 us);
     (2) BOR threshold + reset behaviour: the BOR asserts the (active-low)
         NRST reset signal when the rail falls below the BOR level 3
         threshold (2.8 V falling, HwRS HW-SF-002 (ii) / HW-SF-004, PRT-001
         V_BOR3, OR-008) and releases it only after the rail has returned
         above threshold + hysteresis (a release gate, not decoration);
     (3) retention domain: the RTC backup domain (VBAT node, charged from the
         rail through the charge path and held by the hold-up capacitance)
         keeps its charge across the collapse - the retained fault code
         survives - while the MAIN SRAM content is discarded by the BOR
         reset. The modelled reset-deassertion -> firmware-resumption
         latency closes the recovery time.

   Reduced-model posture (HwAGENTS.md rule 2; the H-02 pulse-fixture
   convention): this is a T1/T2 *fixture* for the brownout window, not a
   qualified BOR cell model. It is algebraic by construction (every output is
   a function of `time` and of extract-cited parameters), exactly like
   PulseISO7637_2, so it has no states to integrate and the 1 us plant step
   of the T2 bridge resolves it exactly. The retention-domain branch is the
   OR-001 closed form (v = V0 - I*ESR - I*t/C) evaluated over the collapse
   interval; the OR-001 plant trajectory itself (leakage nonlinearity, ESR
   temperature dependence, the whole 150 ms holdup_001 event) remains
   Holdup.mo's job and is not re-modelled here. The charge-path recharge
   transient is NOT resolved: R_path * C = 0.5 us is below the 1 us plant
   step, so the node is returned to its charged value when the rail recovers.

   Parameter provenance: V_nominal (PRT-001 V_main_nominal), V_bor (PRT-001
   V_BOR3, HwRS HW-SF-002 (ii) / HW-SF-004, OR-008 STM32G4 BOR level table),
   C_vbat / ESR_vbat / I_leak (PRT-002, hw/bom/datasheets/extract-holdup-cap.json),
   I_mcu (PRT-001 I_VBAT_bound, extract-mcu-vbat.json), V_vbat_min (PRT-001
   V_VBAT_min), R_path (PRT-002 / H-02 WCCA budget). hw/tests/test_bor_physics.py
   asserts every one of those links against hw/bom/bom.json on every run.

   ENGINEERING FIXTURES (explicitly unqualified, no vendor source; each one
   is recorded in the case's `not_covered`): V_bor_hyst (DS12787 publishes no
   BOR hysteresis - OR-008's validation gap 'BOR threshold spread and
   physical brownout correlation pending' covers this) and t_boot (the
   v1.0.0 post-reset startup latency is not characterised; bench
   correlation pending). They are scenario parameters with defaults, not
   claims about silicon.

   Evidence posture (issue #64): the BOR threshold, hysteresis and reset
   timing have NO registered oracle - no OR-XXX witnesses the BOR cell
   behaviour yet - so every artifact derived from this model stays CL0 /
   sim-pending and nothing here may be promoted. The threshold VALUE is the
   HwRS-bound BOR level 3 (OR-008 class (b)); the threshold BEHAVIOUR is
   unwitnessed.

   Requirements traced: HW-SF-002 (sub-events (ii) and (iii)), HW-SF-004
   (BOR level 3 threshold ordering), HW-FR-009; HwAGENTS.md rules 1, 2, 4, 5
   and 6."

  // ---------------------------------------------------------------------
  // Supply stimulus (issue #64 deliverable: nominal 3.3 V -> 0 V -> nominal)
  // ---------------------------------------------------------------------
  parameter Real V_nominal(unit = "V") = 3.3
    "Nominal 3V3 rail (PRT-001 V_main_nominal; HwRS HW-FR-009 derivation)";
  parameter Real t_brownout(unit = "s") = 0.01
    "Brownout onset on the modelled timeline (issue #64: t = 10 ms)";
  parameter Real t_collapse(unit = "s") = 100.0e-6
    "Main-rail collapse duration, NRST asserted interval (issue #64: 100 us)";

  // ---------------------------------------------------------------------
  // BOR threshold and reset behaviour
  // ---------------------------------------------------------------------
  parameter Real V_bor(unit = "V") = 2.8
    "BOR level 3 falling threshold (PRT-001 V_BOR3; HwRS HW-SF-002 (ii) and HW-SF-004; OR-008 class (b) table)";
  parameter Real V_bor_hyst(unit = "V") = 0.2
    "BOR release hysteresis - ENGINEERING FIXTURE, unqualified: DS12787 publishes no BOR hysteresis value (OR-008 gap)";
  parameter Real t_boot(unit = "s") = 100.0e-6
    "Reset deassertion to firmware resumption - ENGINEERING FIXTURE, unqualified: v1.0.0 post-reset startup latency not characterised";

  // ---------------------------------------------------------------------
  // Retention domain (VBAT node) - extract-cited parameters
  // ---------------------------------------------------------------------
  parameter Real C_vbat(unit = "F") = 1.0e-5
    "Hold-up capacitance on the retention node (HW-FR-009 C >= 10 uF; PRT-002)";
  parameter Real ESR_vbat(unit = "Ohm") = 0.02
    "Retention-node series resistance budget (PRT-002 ESR; same value OR-001 uses)";
  parameter Real V0_vbat(unit = "V") = 3.3
    "Initial retention-node voltage = charged 3V3 rail (PRT-001 V_main_nominal)";
  parameter Real I_mcu(unit = "A") = 1.2e-5
    "MCU VBAT-domain load bound = 10 x 1.2 uA typ (PRT-001 I_VBAT_bound)";
  parameter Real I_leak(unit = "A") = 5.0e-6
    "Capacitor leakage, worst case at 85 degC (PRT-002 I_leak_max)";
  parameter Real V_vbat_min(unit = "V") = 1.65
    "Retention floor at worst case (PRT-001 V_VBAT_min; HwRS HW-FR-009)";
  parameter Real R_path(unit = "Ohm") = 0.05
    "Charge-path impedance budget (PRT-002 / H-02 WCCA) - the recharge transient is not resolved by the 1 us plant step";

  // ---------------------------------------------------------------------
  // Outputs (the plant -> MCU / evidence surface)
  // ---------------------------------------------------------------------
  Real v(unit = "V")
    "Main 3V3 rail voltage: the BOR supervisor input";
  Real v_vbat(unit = "V")
    "Retention-domain (VBAT) node voltage across the brownout";
  Real nrst
    "Active-low NRST level: 1.0 = released (high), 0.0 = asserted (low, MCU held in reset)";
  Real reset_asserted
    "1.0 while the BOR holds the MCU in reset, 0.0 otherwise";
  Real bor_below_threshold
    "1.0 while the rail is below the falling BOR threshold (comparator 1)";
  Real bor_release_condition
    "1.0 while the rail is above threshold + hysteresis (comparator 2, the release gate)";
  Real retention_preserved
    "1.0 while the retention-domain (RTC backup) content is preserved: the VBAT node stays above the retention floor";
  Real sram_preserved
    "1.0 while main SRAM content survives: 0.0 from the BOR reset on (the reset discards it)";
  Real firmware_running
    "1.0 while the firmware executes: 0.0 while held in reset and while rebooting";
  Real t_bor_assert(unit = "s")
    "Modelled instant of the BOR assertion = brownout onset";
  Real t_bor_release(unit = "s")
    "Modelled instant the reset may be released = end of the rail collapse";
  Real t_recovery(unit = "s")
    "Modelled reset-deassertion to firmware-resumption time";

protected
  Real i_vbat_load(unit = "A")
    "Constant retention-domain load bound (MCU + capacitor leakage)";

equation
  // --- supply stimulus -------------------------------------------------
  // Idealized collapse (issue #64): the finite fall time of the real rail is
  // not modelled (the threshold crossing is the load-bearing instant).
  v = if time < t_brownout then V_nominal
      else if time < t_brownout + t_collapse then 0.0
      else V_nominal;

  // --- BOR comparators -------------------------------------------------
  bor_below_threshold = if v < V_bor then 1.0 else 0.0;
  bor_release_condition = if v > V_bor + V_bor_hyst then 1.0 else 0.0;

  // --- reset behaviour -------------------------------------------------
  // Asserted from the modelled falling crossing (the rail collapses through
  // the threshold at t_brownout) and released only when BOTH the collapse
  // interval has elapsed AND the release comparator reads true. The
  // hysteresis is therefore a release GATE: a rail that recovered only into
  // the band [V_bor, V_bor + V_bor_hyst] would keep the MCU in reset.
  t_bor_assert = t_brownout;
  t_bor_release = t_brownout + t_collapse;
  reset_asserted = if (time >= t_bor_assert) and not
    (time >= t_bor_release and bor_release_condition > 0.5) then 1.0 else 0.0;
  nrst = if reset_asserted > 0.5 then 0.0 else 1.0;

  // --- retention domain ------------------------------------------------
  // OR-001 closed form over the collapse interval: the charge path is off
  // while the rail is collapsed (0 V below the node), so the node discharges
  // into the constant retention load:
  //   v_vbat(t) = V0 - I_load*ESR - I_load*(t - t_bor_assert)/C
  // The battery-backed backup domain (RTC_BKP) is supplied from this node,
  // so its content is preserved while the node stays above V_vbat_min.
  i_vbat_load = I_mcu + I_leak;
  v_vbat = if time < t_bor_assert then V0_vbat - ESR_vbat * i_vbat_load
           else if time < t_bor_release then
             V0_vbat - ESR_vbat * i_vbat_load
             - i_vbat_load * (time - t_bor_assert) / C_vbat
           else V0_vbat - ESR_vbat * i_vbat_load;
  retention_preserved = if v_vbat > V_vbat_min then 1.0 else 0.0;

  // --- main SRAM -------------------------------------------------------
  // A BOR reset discards main SRAM content; the firmware re-derives its
  // state from the retention domain instead (no state is restored here).
  sram_preserved = if time < t_bor_assert then 1.0 else 0.0;

  // --- recovery time ---------------------------------------------------
  // Firmware execution is suspended from the reset assertion until the reset
  // is released (NRST high) plus the modelled resumption latency.
  t_recovery = t_boot;
  firmware_running = if reset_asserted > 0.5 then 0.0
                     else if (time >= t_bor_assert)
                             and (time < t_bor_release + t_recovery) then 0.0
                     else 1.0;

annotation (
  Documentation(info = "<html>
<h4>Brown-out reset supervision and retention-domain hold-up (H-11, issue #64)</h4>
<p>Reduced algebraic fixture for the brownout window: nominal 3.3 V rail,
collapse to 0 V at <code>t_brownout</code> (10 ms) for <code>t_collapse</code>
(100 us), BOR level 3 assertion at 2.8 V falling with a hysteresis release
gate, and the OR-001 retention-domain discharge over the collapse interval.
The main SRAM is discarded by the reset while the battery-backed retention
domain is preserved.</p>
<p>Posture: engineering fixture, CL0. No oracle witnesses the BOR threshold
behaviour, the hysteresis or the reset timing (issue #64: oracle none); the
threshold value is the HwRS-bound BOR level 3 (OR-008). The physical brownout
correlation of the cell remains a bench (T4) item - see
<code>docs/hw/virtual-bench-plan.md</code> and the T2 brownout scenario
<code>hw/virtual-bench/scenarios/t2_brownout_001.py</code>, which drives this
model's reset signal into the Renode BOR reset injector.</p>
</html>"
));
end BOR;
