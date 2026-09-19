# CANcestry Safety Governor Specification

Version: 1.0.0-rc.1

> **Status**: release-candidate specification; final `1.0.0` bump, tag and
> milestone closure remain deferred while QA-EV-01 is open. The side-effect
> governor retains the fail-closed integration
> contract described below. Phase 12 adds the independent BMS thermal/torque
> governor in `core/governor`; this document describes how an integration must
> consume its result.

## 1. Role

The governor is the single approval point for observable side effects. No
recipe, FSM, UDS service or package may send a CAN frame or write a shared
signal without an approved request (SyAD: "No package may bypass the safety
governor").

The governor is not a normal event subscriber; it observes and approves
side-effect requests synchronously, at the point the effect is requested
(`docs/system/event-ordering.md` section on the governor).

## 2. Existing request interface (stub level)

A side-effect request is a `cancestry_recipe_governor_request_t`
(`cancestry/recipe/engine.h`): kind (`send_message`/`set_signal`), the
requesting recipe, the causing event, and the resolved effect (interface
name/id plus message name/id and encoded frame, or signal name/id and new
value). All pointers in a request are valid only for the duration of the
callback; a governor that needs the data afterwards must copy it.

The engine calls the configured governor function synchronously before
performing the effect. The decision is approve or deny.

## 3. Fail-closed rules (SW-FR-GOV-006)

- No governor configured (NULL function or NULL governor) denies **every**
  side effect. Denial is the default; approval is the exception that must be
  arranged.
- A denied request produces no partial effect: no frame is handed to the
  interface, no signal value changes.
- Denial increments the violation counter (`governor_denials`, visible in the
  engine counters per SW-FR-GOV-005) and is reported as an action error to the
  requesting recipe; with the default `on_error: stop` the recipe halts.

## 4. BMS thermal/torque governor (Phase 12)

`core/governor/bms_governor.c` implements `SW-FR-BMS-001..006` as a separate
static library. `cancestry_bms_governor_evaluate()` is pure and stateless. It
accepts a complete snapshot:

* VCU electrical equivalent power demand in watts, with optional torque in
  milli-newton-metres;
* BMS DC bus voltage in millivolts;
* BMS `MaxDischargeCurrent` in milliamperes;
* fixed-point drivetrain efficiency in per-mille; and
* BMS state (`NORMAL`, `THERMAL_DERATING` or `FAULT`).

The maximum safe electrical power is the floor of:

```
MaxDischargeCurrent_mA * bus_voltage_mV * efficiency_permille / 1,000,000,000
```

Requested current is rounded up. This ensures a request is not accepted due
to a rounding-down error. If the request exceeds the ceiling, the result is
`DERATE`, `allowed_power_w` is the ceiling, optional torque is reduced in the
same ratio, and `fault_code` is
`CANCESTRY_BMS_GOVERNOR_FAULT_GOVERNOR_INTERVENTION` (`GOVERNOR_INTERVENTION`).
The UDS/CAN integration shall record that fault and emit only
`allowed_power_w`; it shall never transmit the original request.
`cancestry_bms_governor_gate_response()` provides this caller-owned response
gate. A malformed or faulted BMS snapshot is `BLOCK` with zero output and the
gate returns false.

The C function does not write a log or retain a counter because that would
break the pure/stateless contract. `fault_code` is the deterministic logging
payload. The caller raises it as `FAULT_RAISED` and applies the safe response
through the ordinary side-effect governor. This preserves a single hardware
and software response gate while making the BMS calculation independently
reviewable.

Evidence: `BMS-GOV-001..008` in
`tests/conformance/governor/test_bms_governor.c` and the archive gate
`cancestry_governor_no_malloc_symbols`.

## 5. Deliberately out of scope at stub level

- TX ID allowlists (`SYS-SF-002` / `SW-FR-GOV-001`), token-bucket rate limits
  (`SYS-SF-003` / `SW-FR-GOV-002`), disabled-package blocking
  (`SW-FR-GOV-003`) and complete SAFE-mode escalation (`SYS-SF-004` /
  `SW-FR-GOV-004`) remain product policy work. The stub replaces these with a
  caller-provided decision function so the engine's integration surface is
  final while the policy is not.
- Recipe-local effects (`set_variable`, `log`, `raise_fault`, timer requests)
  are not governed at stub level.
- The BMS governor does not own a UDS response buffer or CAN driver. It returns
  the only value the integration is permitted to place in one; physical
  transceiver and reset behavior remain in the HAL/platform layer.
