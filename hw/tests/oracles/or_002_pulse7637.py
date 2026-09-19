"""Oracle OR-002: Tabulated ISO 7637-2 / ISO 16750-2 supply transient parameters.

Governed by HwAGENTS.md rule 3, HW-PLAN section 10.2, and virtual-bench-plan section 4.
Class (b) oracle: tabulated reference parameters from international standards
ISO 7637-2:2011 and ISO 16750-2:2012 for 12 V DC automotive electrical systems.

Provides authoritative pulse parameter definitions and expected peak voltage/duration
bounds for pulses 1, 2a, 2b, 3a, 3b, 4, and 5b.
"""

# Tabulated standard parameters for 12V DC system transients (OR-002)
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
        "name": "ISO 16750-2 Pulse 4 (Starter motor cranking voltage drop)",
        "Us": 6.0,         # V
        "td": 0.02,        # s (20 ms)
        "tr": 0.001,       # s (1 ms)
        "Ri": 0.0,         # Ohm
        "standard": "ISO 16750-2:2012 Section 4.6.4",
    },
    "pulse5b": {
        "name": "ISO 16750-2 Pulse 5b (Suppressed load dump transient)",
        "Us": 35.0,        # V clamped
        "td": 0.2,         # s (200 ms)
        "tr": 0.005,       # s (5 ms)
        "Ri": 0.5,         # Ohm
        "standard": "ISO 16750-2:2012 Section 4.6.2",
    },
}


def get_pulse_params(pulse_name):
    """Return dict of standard parameters for given pulse key (e.g. 'pulse1')."""
    if pulse_name not in PULSE_PARAMETERS:
        raise KeyError(f"Unknown pulse {pulse_name!r}; valid keys: {list(PULSE_PARAMETERS.keys())}")
    return PULSE_PARAMETERS[pulse_name].copy()


def calculate_pulse_voltage(pulse_name, t, v_nominal=13.5):
    """Calculate analytical voltage v(t) for a given pulse type and time t."""
    p = get_pulse_params(pulse_name)
    if pulse_name in ("pulse1", "pulse2a", "pulse3a", "pulse3b"):
        if 0.0 <= t < p["td"]:
            return v_nominal + p["Us"] * (1.0 if p["td"] == 0 else 1.0)
        return v_nominal
    elif pulse_name == "pulse2b":
        if 0.0 <= t < p["td"]:
            return v_nominal + p["Us"]
        return v_nominal
    elif pulse_name == "pulse4":
        if 0.0 <= t < p["td"]:
            return p["Us"]
        return v_nominal
    elif pulse_name == "pulse5b":
        if 0.0 <= t < p["td"]:
            return p["Us"]
        return v_nominal
    return v_nominal


def self_check():
    """Oracle self-test verifying parameter integrity."""
    for key, params in PULSE_PARAMETERS.items():
        assert "Us" in params and "td" in params and "tr" in params and "standard" in params
    return {"pass": True, "count": len(PULSE_PARAMETERS)}
