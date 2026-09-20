"""OR-002 reference / regression data for HW-FR-004; qualification is pending.

Normative suppressed load dump (legacy Pulse 5b): ISO 16750-2:2012,
fourth edition, §4.6.4.2.3, Figure 9 / Table 6, printed pp. 12–13.
For the 12 V system Us* = 35 V. The old 40 V value and attribution to
ISO 7637-2:2011 §5.6.2 Table 11 were wrong, not an acceptable model limit.
ISO 7637-2:2011's Foreword explicitly removes pulses 4/5a/5b.

Source transcription: hw/bom/datasheets/extract-iso16750-2-2012.json.
Source/edition disposition and exact Pulse 4 scope: docs/hw/pulse-coverage.md.
Full qualification is tracked by https://github.com/mfreazer/CANcestry-/issues/41.

The 5b Us key below is the LEGACY name for suppressed level Us*, not the
unclamped generator Us (79..101 V). Ri=0.5 ohm and td=350 ms are within
Table 6 ranges, but the present td/3 source lacks the suppressed plateau,
the standard edge/duration measurement definitions and loaded topology.
Correcting the citation/peak does NOT qualify this shape. Pulse 4 is an
engineering dip fixture (1 ms / 20 ms / 1 ms), not a standard starting profile.
Do not promote either case, or the aggregate evidence, from pending.
"""

# Reference/engineering fixture values for the reduced 12 V source; not a qualification verdict.
PULSE_PARAMETERS = {
    "pulse1": {
        "name": "ISO 7637-2 Pulse 1 (Supply disconnect from inductive loads)",
        "Us": -100.0,      # V
        "td": 0.002,       # s (2 ms)
        "tr": 1.0e-6,      # s (1 us)
        "Ri": 10.0,        # Ohm
        "standard": "ISO 7637-2:2011 Table 1",
    },
    "pulse2a": {
        "name": "ISO 7637-2 Pulse 2a (Current interruption in parallel harness)",
        "Us": 37.0,        # V
        "td": 5.0e-5,      # s (50 us)
        "tr": 1.0e-6,      # s (1 us)
        "Ri": 2.0,         # Ohm
        "standard": "ISO 7637-2:2011 Table 2",
    },
    "pulse2b": {
        "name": "ISO 7637-2 Pulse 2b (DC motor spin-down generator action)",
        "Us": 10.0,        # V above DC
        "td": 1.0,         # s
        "tr": 0.001,       # s (1 ms)
        "Ri": 0.0,         # Ohm
        "standard": "ISO 7637-2:2011 Table 3",
    },
    "pulse3a": {
        "name": "ISO 7637-2 Pulse 3a (Negative fast switching burst)",
        "Us": -150.0,      # V
        "td": 1.0e-7,      # s (100 ns)
        "tr": 5.0e-9,      # s (5 ns)
        "Ri": 50.0,        # Ohm
        "standard": "ISO 7637-2:2011 Table 4",
    },
    "pulse3b": {
        "name": "ISO 7637-2 Pulse 3b (Positive fast switching burst)",
        "Us": 100.0,       # V
        "td": 1.0e-7,      # s (100 ns)
        "tr": 5.0e-9,      # s (5 ns)
        "Ri": 50.0,        # Ohm
        "standard": "ISO 7637-2:2011 Table 4",
    },
    "pulse4": {
        "name": "Reduced engineering dip fixture (Pulse 4 incomplete)",
        "Us": 6.0,         # V
        "td": 0.02,        # s (20 ms)
        "tr": 0.001,       # s (1 ms)
        "Ri": 0.0,         # Ohm
        "standard": "Target: ISO 16750-2:2012 §4.6.3.2, Figure 7 / Table 3; current fixture is not that profile",
    },
    "pulse5b": {
        "name": "ISO 16750-2 Pulse 5b (Suppressed load dump transient)",
        "Us": 35.0,        # V; legacy key means Us*, from ISO 16750-2:2012 Table 6
        "td": 0.35,        # s; selected within 40..400 ms, shape still unqualified
        "tr": 0.005,       # s (5 ms)
        "Ri": 0.5,         # Ohm
        "standard": "ISO 16750-2:2012 §4.6.4.2.3, Figure 9 / Table 6, pp. 12–13; shape qualification pending #41",
    },
}


def get_pulse_params(pulse_name):
    """Return reference/fixture parameters for given pulse key (e.g. 'pulse1')."""
    if pulse_name not in PULSE_PARAMETERS:
        raise KeyError(f"Unknown pulse {pulse_name!r}; valid keys: {list(PULSE_PARAMETERS.keys())}")
    return PULSE_PARAMETERS[pulse_name].copy()


def expected_invariants(pulse_name, v_nominal=13.5):
    """Scalar regression expectations, NOT a standards-qualification verdict.

    The H-02 source convention treats Us as an excursion for pulses 1/2/3
    and an absolute level for 4/5b. td/3 is the existing reduced exponential
    approximation (95% decay over td), not an assertion that the standard
    specifies an exact exponential time constant. Both are validation gaps.
    """
    p = get_pulse_params(pulse_name)
    absolute = pulse_name in ("pulse4", "pulse5b")
    peak = p["Us"] if absolute else v_nominal + p["Us"]
    return {"peak_v": peak, "peak_reference_v": p["Us"],
            "absolute_peak": absolute, "time_to_peak_s": p["tr"],
            "decay_tau_s": p["td"] / 3 if pulse_name in
                           ("pulse1", "pulse2a", "pulse5b") else None,
            "duration_s": p["td"]}


def self_check():
    """Fixture integrity only; not independent qualification of the pulse shape."""
    assert set(PULSE_PARAMETERS) == {"pulse1", "pulse2a", "pulse2b", "pulse3a",
                                     "pulse3b", "pulse4", "pulse5b"}
    for key, params in PULSE_PARAMETERS.items():
        assert params["Us"] != 0 and params["td"] > params["tr"] > 0
        assert params["Ri"] >= 0 and params["standard"]
    # ISO 16750-2:2012 §4.6.4.2.3 Table 6: Us* corrected to 35 V; shape stays pending.
    assert (PULSE_PARAMETERS["pulse5b"]["Us"], PULSE_PARAMETERS["pulse5b"]["Ri"],
            PULSE_PARAMETERS["pulse5b"]["td"]) == (35.0, 0.5, 0.35)
    return {"pass": True, "count": len(PULSE_PARAMETERS)}
