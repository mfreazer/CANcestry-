package CancestryLib
  "CANcestry hardware plant library (Layer 2 executable physics, HW-PLAN section 6.2).

   H-01 (issue #33): Power.Holdup - retention-domain (VBAT) hold-up, the
   first model closed by an independent oracle (OR-001, hw/tests/oracles/).
   Bus/, Thermal/ and Safety/ packages arrive with the later H-Phase 1
   issues (H-02 and later, per HW-PLAN G2).

   Parameter provenance (HwAGENTS.md rule 2): defaults are the H-01
   candidate values cited in hw/bom/bom.json and hw/bom/datasheets/;
   hw/tests/test_power_sim.py cross-checks the simulated parameter set
   against that extract."
  extends Modelica.Icons.Package;
end CancestryLib;
