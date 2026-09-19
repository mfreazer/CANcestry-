"""Oracle OR-001: RC hold-up / energy-balance closed form.

Registry: ``hw/tests/oracles/registry.csv`` (class (a), serves HW-SF-002,
HW-SF-004, HW-FR-009 per ``docs/hw/virtual-bench-plan.md`` section 4).

Model (mirrors ``CancestryLib.Power.Holdup``): the retention (VBAT) domain
is a capacitor ``C`` with series ``ESR`` feeding a constant load current
``I = I_mcu + I_leak`` (worst-case bounds from the HwRS HW-FR-009
derivation, cited via ``hw/bom/``). While the main rail is below the node
voltage the charge path is off and the domain discharges into the load.

Closed form (class (a), HW-PLAN section 10.2): with the node voltage
``v(0) = v0`` and constant load,

    vC(t) = vC(0) - I*t/C,   v(t) = vC(t) - ESR*I,   vC(0) = v0 + ESR*I

so the ESR drop is carried by the initial capacitor state and the node
trajectory is the pure energy-balance ramp

    v(t) = v0 - (I/C) * t,

i.e. ``C * dv = I * dt`` integrated in closed form. The hold-up margin is
the time to the floor voltage,

    t_floor(v_floor) = C * (v0 - v_floor) / I,

which is the energy-balance form ``C * dv = I * t`` solved for ``t``.

The oracle is deterministic (no wall-clock time, no randomness) and has no
third-party dependencies; ``self_check()`` regresses the closed form
against an independent fixed-step RK4 integration of the same ODE so the
analytic solution is witnessed by an independent method (oracle class (a)
plus a cross-check, per HwAGENTS.md rule 3).
"""

import json
import sys

__all__ = [
    "total_load_current",
    "vbat",
    "time_to_floor",
    "closed_form_trace",
    "self_check",
]


def total_load_current(i_mcu, i_leak):
    """Constant retention-domain load current [A]."""
    return i_mcu + i_leak


def vbat(t, v0, i_mcu, i_leak, c):
    """Node voltage [V] at time ``t`` [s] (closed form, see module doc)."""
    return v0 - total_load_current(i_mcu, i_leak) * t / c


def time_to_floor(v0, v_floor, i_mcu, i_leak, c):
    """Time [s] until the node reaches ``v_floor``; 0.0 when already below."""
    dv = v0 - v_floor
    if dv <= 0.0:
        return 0.0
    return c * dv / total_load_current(i_mcu, i_leak)


def closed_form_trace(times, v0, i_mcu, i_leak, c):
    """Closed-form node voltage at each sample of ``times`` (list of float)."""
    return [vbat(t, v0, i_mcu, i_leak, c) for t in times]


def _rk4_endpoint(v0, slope, stop, steps):
    """Fixed-step RK4 integration of ``dv/dt = slope`` (constant)."""
    h = stop / steps
    v = v0
    for _ in range(steps):
        k1 = slope
        k2 = slope
        k3 = slope
        k4 = slope
        v += (h / 6.0) * (k1 + 2.0 * k2 + 2.0 * k3 + k4)
    return v


def self_check():
    """Independent cross-check of the closed form.

    Regresses ``vbat`` against a fine fixed-step RK4 integration of
    ``dv/dt = -I/C`` and verifies the floor-time consistency
    ``vbat(t_floor) == v_floor``. Returns a deterministic result dict;
    exits non-zero (via ``main``) on any violation.
    """
    v0 = 3.3
    i_mcu = 12e-6
    i_leak = 5e-6
    c = 10e-6
    v_floor = 1.65
    stop = 0.15
    steps = 100000

    rk4 = _rk4_endpoint(v0, -total_load_current(i_mcu, i_leak) / c, stop,
                        steps)
    analytic = vbat(stop, v0, i_mcu, i_leak, c)
    max_delta = max(
        abs(vbat(stop * k / steps, v0, i_mcu, i_leak, c)
            - (v0 - total_load_current(i_mcu, i_leak) * (stop * k / steps) / c))
        for k in range(steps + 1))

    t_floor = time_to_floor(v0, v_floor, i_mcu, i_leak, c)
    floor_consistency = abs(vbat(t_floor, v0, i_mcu, i_leak, c) - v_floor)

    result = {
        "max_analytic_trace_delta_v": max_delta,
        "rk4_endpoint_v": rk4,
        "analytic_endpoint_v": analytic,
        "endpoint_delta_v": abs(rk4 - analytic),
        "time_to_floor_s": t_floor,
        "floor_consistency_delta_v": floor_consistency,
        "pass": (max_delta == 0.0
                 and abs(rk4 - analytic) <= 1e-9
                 and floor_consistency <= 1e-9),
    }
    return result


def main(argv):
    """Standalone self-check; prints the deterministic result as JSON."""
    del argv
    result = self_check()
    print(json.dumps(result, sort_keys=True))
    return 0 if result["pass"] else 1


if __name__ == "__main__":  # pragma: no cover
    sys.exit(main(sys.argv))
