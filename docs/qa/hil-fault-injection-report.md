# Phase 12 HIL fault-injection verification report

| Field | Value |
|---|---|
| Candidate | `1.0.0-rc.1` |
| Release gate | QA-EV-01 remains open; final `1.0.0` bump/tag/milestone closure are deferred |
| Activity | Hardware-in-the-loop fault injection |
| Requirement boundary | `SW-FR-EVENT-007..008`, `SW-FR-BM-005..006`, `SW-FR-HIL-001..006`, `SW-FR-GOV-006`, `QA-EV-01` |
| Test runner | [`tests/hil/hil_fault_injection.py`](../../tests/hil/hil_fault_injection.py) |
| Host tests | [`tests/hil/test_fault_injection.py`](../../tests/hil/test_fault_injection.py) |
| Backend used in this checkout | Deterministic software hardware-boundary simulation |
| Target hardware result | Not claimed; board/QEMU adapter remains an integration activity |
| Evidence status | Passing simulation; target confirmation required before a vehicle release |

## 1. Scope and limitation

Physical HIL equipment (NI VeriStand, dSPACE, a CAN fault-injection
interface, or a configured QEMU target) is not available in the repository
execution environment. The checked-in runner therefore uses a high-fidelity,
deterministic software model. It models the ordering relevant to the safety
claim: hardware CRC and Bus-Off decisions happen before software admission,
and a brown-out supervisor latches outputs before MCU power-down.

This is not a claim that a particular MCU's CAN CRC checker, TEC/REC Bus-Off
threshold, IWDG period, BOR voltage or GPIO reset timing has been measured.
Those values must be replaced with target measurements and attached to the
release evidence package. The simulation is intentionally useful even without
that equipment: it makes the fail-closed contract executable and prevents a
software test from accidentally passing by treating a corrupted frame as an
ordinary input.

## 2. QA-EV-01 hard-fault escalation evidence

The event queue uses Path A reserved fault slots. Ordinary traffic is stopped after
`capacity - reserved_fault_slots` non-fault events; fault events can use the
reserved physical slots, and a fault at a physically full queue may evict only the deterministic
newest non-fault event. If all physical slots contain faults, the queue retains
the bounded diagnostic set and invokes `cancestry_event_hard_fault_escalate()`.
The Cortex-M hook binding calls the HAL safe-state action first and then leaves
the IWDG on its reset path.

The executable edge-case records are:

* `EVENT-RESERVED-SLOTS-001..005` in
  `tests/unit/core/event/test_reserved_fault_slots.c`;
* `HARD-FAULT-ESCALATION-001..002` in
  `tests/conformance/baremetal/test_hard_fault_escalation.c`.

These host/conformance results demonstrate ordering, boundedness and the
platform hook binding. They do not replace target measurement of GPIO latch
latency, IWDG reset latency, BOR behavior or external contactor timing. Formal
QA closure of QA-EV-01 is therefore still required.

## 3. Reproduction

```sh
python3 tests/hil/hil_fault_injection.py --scenario all \
  --json-output build/hil_fault_injection.json
python3 -m pytest tests/hil/test_fault_injection.py
```

Expected scenario ids, in stable order:

* `HIL-BUSOFF-001`
* `HIL-CRC-001`
* `HIL-BROWNOUT-001`

The JSON artifact is deterministic for identical source and configuration;
it does not contain wall-clock timestamps, host paths or random values.

## 4. Scenario evidence

### 4.1 Bus-Off recovery (`HIL-BUSOFF-001`)

* The controller asserts `BUS_OFF_ASSERTED` first.
* The gateway observes it and enters `FSM_SAFE_STATE`; TX authorization is
  false and the torque output is zero.
* The controller reaches `BUS_OFF_RECOVERED` after the configured bounded
  recovery interval.
* The FSM enters `RECOVERING` and only returns to `ACTIVE` after two healthy
  observations. `FSM_RECOVERY_COMPLETE` is the only point at which TX is
  authorized again.

This verifies the automatic recovery protocol without pretending that
software can repair a physically corrupted bus. A target run must additionally
record TEC/REC values, controller reset registers and measured recovery time.

### 4.2 Electrical noise / CRC rejection (`HIL-CRC-001`)

A frame with `crc_valid = false` is injected. The modeled controller records
`CAN_CRC_REJECTED`, increments the hardware rejection counter, and returns no
frame to the software queue. The FSM processed-frame count remains unchanged.
The test therefore proves the negative property required by the safety case:
malformed payload bytes cannot be decoded or acted upon by the software.

A target run must inject physical bit errors and capture the controller's CRC
/error-passive counters and FIFO behavior.

### 4.3 Brownout (`HIL-BROWNOUT-001`)

The supply is set to the configured BOR threshold. The hardware records
`BOR_SAFE_LATCH` with torque `0`, open contactors and revoked TX, then records
`MCU_POWERED_DOWN`. The test checks the latch event precedes power-down in the
trace. The target procedure must measure the BOR/IWDG reset cause and confirm
that the external transceiver and contactor pins have the same electrical
state before MCU firmware executes.

## 5. Target/QEMU completion procedure

1. Bind the adapter to the target CAN controller and expose frame injection,
   controller status, supply/BOR control and reset-cause reads.
2. Run the same three scenario ids with hardware timestamps and append the raw
   controller/register capture to the release evidence archive.
3. Confirm that no corrupted frame reaches the software RX queue, that Bus-Off
   is not cleared by a software frame rewrite, and that the safe output latch
   precedes reset/power loss.
4. Record board revision, firmware build hash, compiler flags, CAN bit timing,
   BOR/IWDG configuration, instrumentation uncertainty and reviewer sign-off.

Until those steps are completed, this report remains simulation evidence and
is not a vehicle-level ISO 26262 safety case.
