within CancestryLib.Power;
model PulseISO7637_2
  "ISO 7637-2 / ISO 16750-2 supply transient generator model.

   Generates supply transient test pulses for 12 V automotive electrical
   systems per ISO 7637-2 (pulses 1, 2a, 2b, 3a, 3b) and ISO 16750-2
   (cranking pulse 4, suppressed load dump pulse 5b).

   Witnessed by oracle OR-002 (ISO 7637-2 / 16750-2 tabulated pulse parameters,
   hw/tests/oracles/); satisfies HwRS HW-FR-004.

   Parameters:
     V_nominal: nominal supply voltage (13.5 V DC)
     Us_pulse1..5b: peak transient voltage offsets or levels
     td_pulse1..5b: pulse durations
     tr_pulse1..5b: rise/fall time constants"

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
  parameter Real Us_pulse5b(unit = "V") = 35.0 "Pulse 5b clamped peak voltage";
  parameter Real td_pulse5b(unit = "s") = 0.2 "Pulse 5b duration (200 ms)";
  parameter Real tr_pulse5b(unit = "s") = 0.005 "Pulse 5b rise time (5 ms)";

  // Selector: 0=nominal, 1=pulse 1, 2=pulse 2a, 3=pulse 2b, 4=pulse 3a, 5=pulse 3b, 6=pulse 4, 7=pulse 5b
  parameter Integer pulse_selector = 1 "Active pulse type";

  Real v_out(unit = "V") "Generated transient output voltage";
  Real v_pulse1(unit = "V") "Pulse 1 waveform";
  Real v_pulse2a(unit = "V") "Pulse 2a waveform";
  Real v_pulse2b(unit = "V") "Pulse 2b waveform";
  Real v_pulse3a(unit = "V") "Pulse 3a waveform";
  Real v_pulse3b(unit = "V") "Pulse 3b waveform";
  Real v_pulse4(unit = "V") "Pulse 4 waveform";
  Real v_pulse5b(unit = "V") "Pulse 5b waveform";

equation
  // Waveform individual pulse models
  v_pulse1 = V_nominal + (if time >= 0.0 and time < td_pulse1 then Us_pulse1 * exp(-time / (td_pulse1 / 3.0)) else 0.0);
  v_pulse2a = V_nominal + (if time >= 0.0 and time < td_pulse2a then Us_pulse2a * exp(-time / (td_pulse2a / 3.0)) else 0.0);
  v_pulse2b = V_nominal + (if time >= 0.0 and time < td_pulse2b then Us_pulse2b else 0.0);
  v_pulse3a = V_nominal + (if time >= 0.0 and time < td_pulse3a then Us_pulse3a else 0.0);
  v_pulse3b = V_nominal + (if time >= 0.0 and time < td_pulse3b then Us_pulse3b else 0.0);
  v_pulse4 = if time >= 0.0 and time < td_pulse4 then Us_pulse4 else V_nominal;
  v_pulse5b = V_nominal + (if time >= 0.0 and time < td_pulse5b then (Us_pulse5b - V_nominal) * exp(-time / (td_pulse5b / 3.0)) else 0.0);

  // Muxed output according to pulse_selector
  v_out = if pulse_selector == 1 then v_pulse1
          else if pulse_selector == 2 then v_pulse2a
          else if pulse_selector == 3 then v_pulse2b
          else if pulse_selector == 4 then v_pulse3a
          else if pulse_selector == 5 then v_pulse3b
          else if pulse_selector == 6 then v_pulse4
          else if pulse_selector == 7 then v_pulse5b
          else V_nominal;

annotation (
  Documentation(info = "<html>
<h4>ISO 7637-2 / ISO 16750-2 Pulse Generator</h4>
<p>Generates standardized automotive transient pulses for supply line immunity testing. Verified against tabulated parameters in oracle OR-002.</p>
</html>"
));
end PulseISO7637_2;
