within CancestryLib.Power;
model PulseISO7637_2
  "ISO 7637-2 / ISO 16750-2 supply transient generator model.

   Generates supply transient test pulses for 12 V automotive electrical
   systems per ISO 7637-2 (pulses 1, 2a, 2b, 3a, 3b) and ISO 16750-2
   (cranking pulse 4, suppressed load dump pulse 5b, unsuppressed load
   dump pulse 5a per Test A).

   Witnessed by oracle OR-002 (ISO 7637-2 / 16750-2 tabulated pulse parameters,
   hw/tests/oracles/); provides regression stimuli for HwRS HW-FR-004; aggregate qualification is pending.

   Parameters:
     V_nominal: nominal supply voltage (13.5 V DC)
     Us_pulse1..5b: peak transient voltage offsets or levels
     td_pulse1..5b: pulse durations
     tr_pulse1..5b: finite leading-edge time to peak
     Us/td/tr/Ri_pulse5a: pulse 5a (Test A) unclamped-level parameters (H-06)

   Coverage table (HW-FR-004; H-04 #38; H-06 #49):
     Pulse 1: covered (negative transient)
     Pulse 2a: covered (inductive load switching)
     Pulse 2b: covered (inductive load switching, slower)
     Pulse 3a: covered (fast negative transient)
     Pulse 3b: covered (fast positive transient)
     Pulse 4: PENDING. Engineering fixture: 13.5 V to 6 V in 1 ms,
              dwell 20 ms, recover in 1 ms (nominal at 22 ms).
              No temperature dependence, battery ESR shift, alternator recovery
              or full multi-stage starting profile; qualification CL0 (#41)
     Pulse 5a: PENDING. H-06 reduced fixture for ISO 16750-2:2012 section
               4.6.4.2.2 (Test A, without centralized load dump suppression),
               Figure 8 / Table 5: unclamped level Us=79 V (Table 5 lower
               bound with the footnote a lower-Ri pairing), 5 ms linear edge,
               td/3 decay over 350 ms. No Figure 8 0.9/0.1 edge/duration
               measurement definitions, no unclamped-generator topology, Ri
               unexercised, no 10-pulse repetition, no loaded DUT response;
               qualification CL0, shape qualification deferred to #41
     Pulse 5b: PENDING. Legacy Us parameter means suppressed Us*=35 V,
               per ISO 16750-2:2012 section 4.6.4.2.3, Figure 9 / Table 6.
               Clamp/decay topology and standard timing definitions remain
               incorrect/incomplete for qualification; tracked in #41

   Covered means unloaded source invariants only, not DUT immunity. Ri is
   unexercised source metadata. The exponential td/3 decay and linear edge
   are reduced-model approximations, not full generator conformance."

  parameter Real V_nominal(unit = "V") = 13.5
    "Nominal 12V DC system operating voltage";

  // Pulse 1: Transient caused by supply disconnection from inductive loads
  parameter Real Us_pulse1(unit = "V") = -100.0 "Pulse 1 peak voltage";
  parameter Real td_pulse1(unit = "s") = 0.002 "Pulse 1 duration (2 ms)";
  parameter Real tr_pulse1(unit = "s") = 1.0e-6 "Pulse 1 rise time (1 us)";
  parameter Real Ri_pulse1(unit = "Ohm") = 10.0 "Pulse 1 source resistance";

  // Pulse 2a: Transient due to current interruption in parallel harness
  parameter Real Us_pulse2a(unit = "V") = 37.0 "Pulse 2a peak voltage";
  parameter Real td_pulse2a(unit = "s") = 5.0e-5 "Pulse 2a duration (50 us)";
  parameter Real tr_pulse2a(unit = "s") = 1.0e-6 "Pulse 2a rise time (1 us)";
  parameter Real Ri_pulse2a(unit = "Ohm") = 2.0 "Pulse 2a source resistance";

  // Pulse 2b: Transient from DC motor acting as generator during spin-down
  parameter Real Us_pulse2b(unit = "V") = 10.0 "Pulse 2b peak voltage above DC";
  parameter Real td_pulse2b(unit = "s") = 1.0 "Pulse 2b duration (1 s)";
  parameter Real tr_pulse2b(unit = "s") = 0.001 "Pulse 2b rise/fall time (1 ms)";

  // Pulse 3a: Negative fast transient bursts
  parameter Real Us_pulse3a(unit = "V") = -150.0 "Pulse 3a peak voltage";
  parameter Real td_pulse3a(unit = "s") = 1.0e-7 "Pulse 3a duration (100 ns)";
  parameter Real tr_pulse3a(unit = "s") = 5.0e-9 "Pulse 3a rise time (5 ns)";

  // Pulse 3b: Positive fast transient bursts
  parameter Real Us_pulse3b(unit = "V") = 100.0 "Pulse 3b peak voltage";
  parameter Real td_pulse3b(unit = "s") = 1.0e-7 "Pulse 3b duration (100 ns)";
  parameter Real tr_pulse3b(unit = "s") = 5.0e-9 "Pulse 3b rise time (5 ns)";

  // Pulse 4: Starter motor engagement cranking voltage drop
  parameter Real Us_pulse4(unit = "V") = 6.0 "Pulse 4 cranking voltage level";
  parameter Real td_pulse4(unit = "s") = 0.02 "Pulse 4 cranking duration (20 ms)";
  parameter Real tr_pulse4(unit = "s") = 0.001 "Pulse 4 drop fall time (1 ms)";

  // Pulse 5b: Suppressed load dump transient
  parameter Real Us_pulse5b(unit = "V") = 35.0 "Legacy name for Us*: suppressed level per ISO 16750-2:2012 Table 6; source shape pending #41";
  parameter Real td_pulse5b(unit = "s") = 0.35 "Pulse 5b duration (350 ms)";
  parameter Real tr_pulse5b(unit = "s") = 0.005 "Pulse 5b rise time (5 ms)";

  parameter Real Ri_pulse5b(unit = "Ohm") = 0.5 "Source resistance metadata; unloaded model does not exercise Ri";

  // Pulse 5a: Unsuppressed load dump transient (H-06, ISO 16750-2:2012 Test A)
  parameter Real Us_pulse5a(unit = "V") = 79.0 "Unclamped generator level per ISO 16750-2:2012 section 4.6.4.2.2 Table 5 lower bound (footnote a pairs it with the lower Ri); fixture bound via sim case; shape pending #41";
  parameter Real td_pulse5a(unit = "s") = 0.35 "Pulse 5a duration (350 ms), selected within the Table 5 40..400 ms range";
  parameter Real tr_pulse5a(unit = "s") = 0.005 "Pulse 5a rise time (5 ms), within the tabulated 10 ms -5/+0 rising slope";

  parameter Real Ri_pulse5a(unit = "Ohm") = 0.5 "Source resistance metadata; unloaded model does not exercise Ri";

  // Selector: 0=nominal, 1=pulse 1, 2=pulse 2a, 3=pulse 2b, 4=pulse 3a, 5=pulse 3b, 6=pulse 4, 7=pulse 5b, 8=pulse 5a
  // (H-06: pulse 5a is appended as selector 8 so the existing 1..7 mapping stays stable.)
  parameter Integer pulse_selector = 1 "Active pulse type";

  Real v_out(unit = "V") "Generated transient output voltage";
  Real v_pulse1(unit = "V") "Pulse 1 waveform";
  Real v_pulse2a(unit = "V") "Pulse 2a waveform";
  Real v_pulse2b(unit = "V") "Pulse 2b waveform";
  Real v_pulse3a(unit = "V") "Pulse 3a waveform";
  Real v_pulse3b(unit = "V") "Pulse 3b waveform";
  Real v_pulse4(unit = "V") "Pulse 4 waveform";
  Real v_pulse5b(unit = "V") "Pulse 5b waveform";
  Real v_pulse5a(unit = "V") "Pulse 5a waveform (H-06)";

equation
  // HW-FR-004 contract correction: exercise the already-declared tr values.
  // Finite linear leading edges; td is measured from the peak. No new plant.
  // Exponential td/3 is the H-02 reduced-shape convention, not a new standard claim.
  v_pulse1 = V_nominal + Us_pulse1 * (if time < 0 then 0
    else if time < tr_pulse1 then time / tr_pulse1
    else if time < tr_pulse1 + td_pulse1 then exp(-(time - tr_pulse1) / (td_pulse1 / 3.0)) else 0);
  v_pulse2a = V_nominal + Us_pulse2a * (if time < 0 then 0
    else if time < tr_pulse2a then time / tr_pulse2a
    else if time < tr_pulse2a + td_pulse2a then exp(-(time - tr_pulse2a) / (td_pulse2a / 3.0)) else 0);
  v_pulse2b = V_nominal + Us_pulse2b * (if time < 0 then 0
    else if time < tr_pulse2b then time / tr_pulse2b
    else if time < tr_pulse2b + td_pulse2b then 1.0
    else if time < 2*tr_pulse2b + td_pulse2b then 1 - (time - tr_pulse2b - td_pulse2b) / tr_pulse2b else 0);
  v_pulse3a = V_nominal + Us_pulse3a * (if time < 0 then 0
    else if time < tr_pulse3a then time / tr_pulse3a
    else if time < tr_pulse3a + td_pulse3a then 1.0
    else if time < 2*tr_pulse3a + td_pulse3a then 1 - (time - tr_pulse3a - td_pulse3a) / tr_pulse3a else 0);
  v_pulse3b = V_nominal + Us_pulse3b * (if time < 0 then 0
    else if time < tr_pulse3b then time / tr_pulse3b
    else if time < tr_pulse3b + td_pulse3b then 1.0
    else if time < 2*tr_pulse3b + td_pulse3b then 1 - (time - tr_pulse3b - td_pulse3b) / tr_pulse3b else 0);
  v_pulse4 = V_nominal + (Us_pulse4 - V_nominal) * (if time < 0 then 0
    else if time < tr_pulse4 then time / tr_pulse4
    else if time < tr_pulse4 + td_pulse4 then 1.0
    else if time < 2*tr_pulse4 + td_pulse4 then 1 - (time - tr_pulse4 - td_pulse4) / tr_pulse4 else 0);
  v_pulse5b = V_nominal + (Us_pulse5b - V_nominal) * (if time < 0 then 0
    else if time < tr_pulse5b then time / tr_pulse5b
    else if time < tr_pulse5b + td_pulse5b then exp(-(time - tr_pulse5b) / (td_pulse5b / 3.0)) else 0);
  // H-06 pulse 5a branch: the same H-02 reduced-shape convention (absolute
  // level, linear edge, td/3 exponential decay, return to nominal after td),
  // bound to the unclamped Test A level. This is a regression fixture, not a
  // qualified ISO 16750-2:2012 Figure 8 waveform; shape qualification is #41.
  v_pulse5a = V_nominal + (Us_pulse5a - V_nominal) * (if time < 0 then 0
    else if time < tr_pulse5a then time / tr_pulse5a
    else if time < tr_pulse5a + td_pulse5a then exp(-(time - tr_pulse5a) / (td_pulse5a / 3.0)) else 0);

  // Muxed output according to pulse_selector
  v_out = if pulse_selector == 1 then v_pulse1
          else if pulse_selector == 2 then v_pulse2a
          else if pulse_selector == 3 then v_pulse2b
          else if pulse_selector == 4 then v_pulse3a
          else if pulse_selector == 5 then v_pulse3b
          else if pulse_selector == 6 then v_pulse4
          else if pulse_selector == 7 then v_pulse5b
          else if pulse_selector == 8 then v_pulse5a
          else V_nominal;

annotation (
  Documentation(info = "<html>
<h4>ISO 7637-2 / ISO 16750-2 Pulse Generator</h4>
<p>Reduced unloaded source for HW-FR-004, checked against OR-002 scalar invariants. See the coverage table and explicit validation gaps in the model docstring. This does not demonstrate DUT immunity or full standard waveform conformance. Pulse 4 and 5b qualification remain pending in issue #41; pulse 5a (Test A, H-06 #49) lands as a reduced unclamped-level fixture whose shape qualification is likewise deferred to issue #41 (no promotion).</p>
</html>"
));
end PulseISO7637_2;
