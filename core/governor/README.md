# BMS thermal and torque governor

The BMS governor is the Phase 12 safety boundary for the EV battery
replacement gateway (issue #30). It implements `SW-FR-BMS-001..006` from the
software requirements specification.

## Contract

`cancestry_bms_governor_evaluate()` is a pure function. It takes a complete
VCU/BMS snapshot and returns a complete decision; it has no static mutable
state, callbacks, timers, heap allocation, or hidden threshold.

The VCU's electrical equivalent power demand is supplied in watts. The BMS
supplies `MaxDischargeCurrent` in milliamperes and the measured DC bus voltage
in millivolts. The governor computes the safe electrical ceiling with integer
arithmetic:

```
maximum_power_w = floor(MaxDischargeCurrent_mA * bus_voltage_mV
                        * efficiency_permille / 1,000,000,000)
```

Requested current is rounded **up**, so a request is never accepted because
of a rounding-down error. A request above the ceiling returns `DERATE`, the
safe `allowed_power_w` (and proportional optional torque value), and the
fault code `GOVERNOR_INTERVENTION`. The integration layer must record this
fault before it emits a UDS/CAN response; it must never transmit the original
request after a `DERATE` result. A BMS fault or malformed snapshot returns
`BLOCK` and zero output.

The module does not log internally because logging would make the decision
stateful and would violate the pure-function contract. `fault_code` is the
explicit, deterministic log/event payload. The caller may map it to a
`FAULT_RAISED` event or UDS negative response without changing the decision.

## Example

At 400 V (`400000 mV`), 250 A (`250000 mA`) and 95% efficiency (`950`), the
maximum safe output is 95 kW. A 100 kW request is returned as:

```
decision       = DERATE
allowed_power  = 95000 W
fault_code     = GOVERNOR_INTERVENTION
```

The UDS/CAN integration uses `allowed_power_w`, not `requested_power_w`, and
records the intervention. `cancestry_bms_governor_gate_response()` is the
small caller-owned response gate: it writes only the safe power/torque values,
returns false with zero output for a BLOCK/invalid snapshot, and exposes the
fault code before the caller emits a response. The governor deliberately does
not own a UDS response buffer or a CAN driver, so it remains independently
verifiable.

## Build and verification

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --target cancestry_governor
ctest --test-dir build -R bms_governor --output-on-failure
python3 ci/check_no_alloc.py build/core/governor/libcancestry_governor.a
```
