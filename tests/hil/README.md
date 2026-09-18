# CANcestry HIL fault-injection harness

This directory implements the Phase 12 HIL evidence path from issue #30:
`SW-FR-HIL-001..006` and `HIL-BUSOFF-001`, `HIL-CRC-001`,
`HIL-BROWNOUT-001`.

## Hardware-first contract

The runner models the physical order rather than implementing software
workarounds:

1. **Bus-Off:** the simulated CAN controller asserts Bus-Off. The gateway then
   enters `SAFE_STATE` and revokes transmission. The controller's bounded
   automatic recovery clears Bus-Off; the FSM requires two healthy main-loop
   observations before returning to `ACTIVE`.
2. **CRC/bit error:** the simulated CAN controller rejects a frame with an
   invalid CRC. It never enters the software queue, so the FSM cannot process
   malformed data.
3. **Brownout:** the simulated brown-out reset supervisor latches zero torque,
   opens the contactors and revokes TX before the MCU is marked powered down.
   This is the hardware safe latch, not a late software correction.

The default backend is an in-process deterministic simulation. It is suitable
for CI and for a QEMU/proxy integration seam, but it is not evidence that a
particular STM32/NXP board's CAN peripheral or BOR threshold has been
validated. A physical adapter can implement the same operations in
`SoftwareHilBackend` and retain the scenario assertions; the target procedure
and limitations are recorded in
[`docs/qa/hil-fault-injection-report.md`](../../docs/qa/hil-fault-injection-report.md).

## Run

No third-party Python package is required:

```sh
python3 tests/hil/hil_fault_injection.py --scenario all
python3 tests/hil/hil_fault_injection.py --scenario all \
  --json-output build/hil_fault_injection.json
python3 -m pytest tests/hil/test_fault_injection.py
```

The CLI exits `0` only when every hardware-first assertion passes. JSON output
is sorted and contains the event order, metrics and assertion list so the
report can be archived without wall-clock data or random identifiers.
