within CancestryLib.Power;
model Holdup
  "Retention-domain (VBAT) hold-up: capacitance, ESR, leakage and MCU VBAT load.

   Closes HW-SF-002 sub-events (ii) and (iii) at T1 (H-01, issue #33),
   witnessed by oracle OR-001 (RC hold-up / energy-balance closed form,
   hw/tests/oracles/or_001_holdup.py):

     (ii) main-rail brownout to BOR level 3 (2.8 V) for <= 50 ms
     (iii) main-rail removal from BOR to 0 V for <= 100 ms

   Sub-event (i) (IWDG reset) does not discharge the VBAT domain while the
   rail is present and is covered at T2 (virtual bench), not here.

   Topology (consistent with the STM32G4 VBAT domain, DS12787): the VBAT
   node is fed from the main rail through a charge path (ideal diode with
   series impedance R_path, budget) and stores energy in the hold-up
   capacitor (C with series ESR). The retention-domain load is the MCU
   VBAT-domain current (I_mcu) plus the capacitor leakage (I_leak), held at
   the worst-case bound of the HwRS HW-FR-009 derivation (V-independent
   within the retention window).

   While the main rail is at or below the node voltage (the whole
   holdup_001 event: rail 2.8 V or 0 V against an initial 3.3 V node) the
   charge path is off and the node discharges into the constant load:

     vC(0) = V0 and v(0) = V0 - ESR*(I_mcu + I_leak)
     v(t) = v(0) - (I_mcu + I_leak) * t / C  (OR-001 closed form)

   OR-001 uses that ESR-adjusted initial node voltage explicitly. I.e.
   C*dv = I*dt integrated in closed form; the hold-up margin is
   t_floor = C*(v(0) - V_floor)/(I_mcu + I_leak).

   Idealizations (documented, per HwAGENTS.md rule 2): ideal diode
   (forward drop negligible at uA-class load; H-02 WCCA), constant load at
   the worst-case bound, ESR in series with the ideal capacitor. Parameter
   defaults are the H-01 candidate values cited in hw/bom/bom.json and
   hw/bom/datasheets/; hw/tests/test_power_sim.py verifies the simulated
   parameter set against that extract (credibility CL2 per virtual-bench-
   plan section 5)."
  parameter Real C(unit = "F") = 1.0e-5
    "Hold-up capacitance (HW-FR-009: C >= 10 uF nominal 10 uF, PRT-002)";
  parameter Real ESR(unit = "Ohm") = 0.02
    "Capacitor ESR budget (PRT-002; pinned by the H-02 vendor datasheet)";
  parameter Real I_mcu(unit = "A") = 1.2e-5
    "MCU VBAT-domain load, worst-case bound = 10 x 1.2 uA typ (PRT-001, HwRS HW-FR-009)";
  parameter Real I_leak(unit = "A") = 5.0e-6
    "Capacitor leakage, worst case at 85 degC (PRT-002, HwRS HW-FR-009)";
  parameter Real V0(unit = "V") = 3.3
    "Initial VBAT node voltage = nominal 3V3 rail (PRT-001 V_main_nominal)";
  parameter Real VIN_brownout(unit = "V") = 2.8
    "Main-rail brownout floor = BOR level 3 (HW-SF-002; PRT-001 V_BOR3)";
  parameter Real V_floor(unit = "V") = 1.65
    "Retention floor at worst case (PRT-001 V_VBAT_min, HwRS HW-FR-009)";
  parameter Real R_path(unit = "Ohm") = 0.05
    "Charge-path impedance budget (idealized diode + trace; H-02 WCCA)";
  parameter Real t_brownout(unit = "s") = 0.05
    "Brownout duration at BOR level 3 (HW-SF-002 (ii), worst case 50 ms)";
  parameter Real t_remove(unit = "s") = 0.1
    "Removal duration from BOR to 0 V (HW-SF-002 (iii), worst case 100 ms)";

  Real v(fixed = true, start = V0, unit = "V")
    "VBAT node voltage (retention domain)";
  Real vC(start = V0, unit = "V")
    "Ideal capacitor voltage (internal to the ESR branch)";
  Real iLoad(unit = "A") "Retention-domain load current (MCU + leakage)";
  Real iCh(unit = "A") "Charge-path current from the main rail (>= 0)";
  Real vin(unit = "V") "Main-rail voltage at the charge path";

initial equation
  // Explicitly align the capacitor state with the declared initial VBAT
  // value. This prevents an underdetermined/inconsistent t=0 state when
  // the ESR branch algebraic equation is initialized.
  vC = V0;

equation
  // Timeline: [0, t_brownout) the rail sits at the BOR level 3 floor;
  // [t_brownout, t_brownout + t_remove] the rail is removed (0 V).
  vin = if time < t_brownout then VIN_brownout else 0.0;

  // Constant retention-domain load at the worst-case bound (HwRS
  // HW-FR-009 derivation).
  iLoad = I_mcu + I_leak;

  // Node equation: capacitor branch (ideal cap in series with ESR),
  // load sink, and the ideal-diode charge path.
  v = vC - ESR*(iLoad - iCh);
  der(vC) = (iCh - iLoad)/C;
  iCh = if vin > v then (vin - v)/R_path else 0.0;
annotation (
  Documentation(info = "<html>
<h4>Scenario</h4>
<p><code>holdup_001</code>: <code>vC(0) = V0 = 3.3 V</code>, so the
loaded node starts at <code>v(0) = V0 - ESR*(I_mcu + I_leak)</code>; the
rail is 2.8 V for 50 ms then 0 V for 100 ms. The charge path stays off
(2.8 V &lt; v(t) for the whole event), so the node follows OR-001's
ESR-adjusted closed form and ends at approximately 3.045 V, 1.395 V above
the 1.65 V retention floor.</p>
</html>"
));
end Holdup;
