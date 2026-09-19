"""Local (toolchain-free) regression of the Holdup physics.

Executes the exact ODE of ``CancestryLib.Power.Holdup`` (see
``hw/model/CancestryLib/Power/Holdup.mo``) with a fixed-step RK4 integrator
in pure Python and regresses it against oracle OR-001. This is a unit test
of the *model equations* so the physics is witnessed locally, without
OpenModelica; it is NOT the FMU evidence - the oracle-verified FMU run
(``hw/tests/test_power_sim.py`` in the ci/docker toolchain image) is the
evidence for the ledger row.

Requirements traced: HW-SF-002, HW-FR-009.
Test ids: HW-PHYS-HOLDUP-001, HW-PHYS-HOLDUP-002, HW-PHYS-ORACLE-001.
"""

from __future__ import annotations

import importlib.util
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ORACLE_PATH = HERE / "oracles" / "or_001_holdup.py"

_spec = importlib.util.spec_from_file_location("or_001_holdup", ORACLE_PATH)
or_001 = importlib.util.module_from_spec(_spec)
sys.modules["or_001_holdup"] = or_001
_spec.loader.exec_module(or_001)

# holdup_001 parameter set (identical to the sim case and BOM extract).
P = {
    "C": 1.0e-05,
    "ESR": 0.02,
    "I_mcu": 1.2e-05,
    "I_leak": 5.0e-06,
    "V0": 3.3,
    "VIN_brownout": 2.8,
    "V_floor": 1.65,
    "R_path": 0.05,
    "t_brownout": 0.05,
    "t_remove": 0.1,
}

I_LOAD = P["I_mcu"] + P["I_leak"]


def rail_voltage(t):
    """Main-rail timeline of the Holdup model (brownout, then removal)."""
    return P["VIN_brownout"] if t < P["t_brownout"] else 0.0


def diode_current(t, v_c):
    """Charge-path branch current at capacitor state v_c.

    Solves the ideal diode with series R_path against the node equation
    v = vC - ESR*(I_load - iCh); the diode is off whenever the rail is at
    or below the node voltage (the whole holdup_001 event).
    """
    vin = rail_voltage(t)
    i_on = (vin - v_c + P["ESR"] * I_LOAD) / (P["R_path"] + P["ESR"])
    if i_on > 0.0 and vin > (v_c - P["ESR"] * (I_LOAD - i_on)):
        return i_on
    return 0.0


def rhs(t, v_c, i_ch):
    """Holdup.mo state equation: der(vC) = (iCh - iLoad)/C."""
    return (i_ch - I_LOAD) / P["C"]


def node_voltage(v_c, i_ch):
    """Holdup.mo node equation: v = vC - ESR*(iLoad - iCh)."""
    return v_c - P["ESR"] * (I_LOAD - i_ch)


def rk4_run(stop, steps):
    """Fixed-step RK4 over the model DAE; returns (times, node voltages)."""
    # Holdup.mo initializes vC(0)=V0. The node starts after the ESR drop,
    # which is the OR-001 effective initial voltage.
    v_c = P["V0"]
    h = stop / steps
    times, voltages = [0.0], [node_voltage(v_c, 0.0)]
    t = 0.0
    for _ in range(steps):
        i1 = diode_current(t, v_c)
        k1 = rhs(t, v_c, i1)
        i2 = diode_current(t + 0.5 * h, v_c + 0.5 * h * k1)
        k2 = rhs(t + 0.5 * h, v_c + 0.5 * h * k1, i2)
        i3 = diode_current(t + 0.5 * h, v_c + 0.5 * h * k2)
        k3 = rhs(t + 0.5 * h, v_c + 0.5 * h * k2, i3)
        i4 = diode_current(t + h, v_c + h * k3)
        k4 = rhs(t + h, v_c + h * k3, i4)
        v_c += (h / 6.0) * (k1 + 2.0 * k2 + 2.0 * k3 + k4)
        t += h
        i_ch = diode_current(t, v_c)
        times.append(t)
        voltages.append(node_voltage(v_c, i_ch))
    return times, voltages


def test_model_ode_matches_oracle():
    """HW-PHYS-HOLDUP-001: RK4(model ODE) tracks OR-001 to machine noise."""
    stop = P["t_brownout"] + P["t_remove"]
    times, voltages = rk4_run(stop, 15000)
    max_delta = max(
        abs(v - or_001.vbat(t, P["V0"], P["I_mcu"], P["I_leak"], P["C"],
                             P["ESR"]))
        for t, v in zip(times, voltages))
    assert max_delta <= 1e-9, "max |RK4 - OR-001| = %r V" % max_delta


def test_retention_margin():
    """HW-PHYS-HOLDUP-002: floor held with a large closed-form margin."""
    stop = P["t_brownout"] + P["t_remove"]
    times, voltages = rk4_run(stop, 15000)
    assert voltages[-1] >= P["V_floor"]
    # Closed-form hold-up margin (C*dv = I*dt solved for t): the event ends
    # at 0.15 s while the closed-form floor time is ~0.97 s (10x margin).
    t_floor = or_001.time_to_floor(P["V0"], P["V_floor"], P["I_mcu"],
                                   P["I_leak"], P["C"], P["ESR"])
    assert t_floor > 5 * stop, "insufficient hold-up margin: %r s" % t_floor
    # The charge path is off for the whole event (rail <= 2.8 V < node).
    assert all(diode_current(t, v_c) == 0.0
               for t, v_c in zip(times, [v + P["ESR"] * I_LOAD
                                         for v in voltages]))


def test_oracle_self_check():
    """HW-PHYS-ORACLE-001: OR-001 self-check (closed form vs RK4)."""
    result = or_001.self_check()
    assert result["pass"], result
