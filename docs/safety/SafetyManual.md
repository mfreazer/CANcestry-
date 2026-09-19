# CANcestry Safety Manual

| Field | Value |
|---|---|
| Document | CANcestry Safety Manual |
| Version | 1.0.0-rc.1 candidate safety-case baseline |
| Status | Candidate software safety-case evidence package; QA-EV-01 remains open; not an ISO 26262 certification |
| Intended safety integrity | ASIL-B alignment target for a gateway software element |
| Scope | `core/` execution archives, `core/governor/`, `platform/`, HIL verification and release evidence |
| Owner | CANcestry safety and verification review |
| Last review | 2026-09-18 |
| Requirements | `SW-FR-EVENT-007..008`, `SW-FR-BMS-001..006`, `SW-FR-HIL-001..006`, `SW-FR-SAFETY-001..005`, `SW-FR-BM-005..006`, `QA-EV-01` plus the existing core/HAL requirements |

## 1. Purpose and safety claim

CANcestry is a declarative CAN codec, event, recipe and FSM runtime for a
safety-related gateway. This manual defines the software safety argument for
the v1.0.0-rc.1 candidate and identifies the evidence that an external assessor
must review. Final `1.0.0` release, tagging and milestone closure are deferred
until QA-EV-01 is formally closed.

The bounded claim is:

> For a validated package, a conforming caller, and a platform that implements
the documented hardware contract, CANcestry execution is deterministic,
bounded and allocation-free on the runtime path. Invalid input, unavailable
policy, BMS derating and hardware fault indications fail closed: the affected
side effect is blocked and the configured safe state is retained.

This is an engineering alignment claim, not a claim that a vehicle item,
MCU, transceiver, compiler, board, operating system or complete gateway has
been certified. A product safety case still requires the item definition,
HARA, safety goals, hardware metrics, dependent-failure analysis,
freedom-from-interference evidence, tool confidence/qualification, production
process evidence and independent assessment required by the applicable ISO
26262 work products.

## 2. Item boundary, assumptions and exclusions

### 2.1 Software boundary

The safety boundary contains:

* the event, codec, recipe, FSM, transport and UDS runtime archives;
* the stateless BMS thermal/torque governor in `core/governor/`;
* the platform HAL, Cortex-M ISR/SPSC hand-off and watchdog integration;
* the caller-owned storage, fault/event sinks and transmission authorization
  supplied by the integrator; and
* the host conformance, HIL simulation and traceability evidence.

Load-time YAML parsing is deliberately separate from execution archives. A
validated package must remain alive for all runtime objects that borrow its
strings and arrays. No core execution path loads code, evaluates arbitrary
programs, starts threads or allocates from the heap.

### 2.2 Integrator assumptions

The integrator shall provide and verify:

1. a compiler/toolchain with C99 fixed-width integer behavior, warnings as
   errors and the target's documented integer/FP rules;
2. caller-owned buffers and queues with valid lifetime/alignment, serialized
   queue access and the documented capacity bounds;
3. a monotonic hardware clock and a target measurement of worst-case execution
   time, interrupt latency and queue service time;
4. CAN controller configuration, bit timing, hardware CRC/error handling,
   transceiver behavior, Bus-Off state and recovery policy;
5. external GPIO/contactor/torque safe-latch behavior on reset, BOR and IWDG;
6. a governor callback or BMS integration that treats `DERATE` as a new hard
   output limit, records `GOVERNOR_INTERVENTION`, and never emits the original
   request; and
7. package capabilities, TX allowlists, signal write allowlists and recovery
   authorization. Missing policy is denial, not permission.

Out of scope for this manual are the vehicle HARA, the physical proof of a
specific board's BOR threshold, motor-control plant dynamics, CAN wiring
installation, and ISO 26262 certification of third-party tools.

## 3. System Architecture & Data Flow

### 3.1 Layered architecture

The layers have one-way dependencies:

1. **Hardware/platform boundary.** The CAN peripheral, CRC checker, Bus-Off
   controller, BOR/IWDG and transceiver produce frames and fault indications.
   The HAL validates frame length/capability, captures a monotonic timestamp,
   and exposes caller-owned types. Core code never reads a peripheral register.
2. **ISR hand-off.** `hal_stm32_can_rx_isr()` drains a bounded hardware FIFO,
   captures the timestamp and pushes an event into the lock-free SPSC queue.
   It performs no codec decode, UDS parsing, FSM evaluation or blocking call.
3. **Event queue.** The main loop transfers valid ISR events into a bounded
   deterministic min-heap ordered by timestamp, priority class and sequence.
   Path A reserves physical slots for faults: ordinary traffic stops at the
   non-fault limit, faults may use the reserve, and a full all-fault queue
   invokes the HAL fail-safe hook followed by the IWDG escalation hook rather
   than evicting an existing fault (`SW-FR-EVENT-007..008`, `QA-EV-01`).
4. **Codec and transport.** The codec atomically rejects short/malformed
   frames and out-of-range values. ISO-TP reassembly and UDS request handling
   use fixed caller-owned buffers. Hardware CRC rejection is upstream of both.
5. **Recipe/FSM.** Events are processed sequentially per instance. Guards,
   capabilities, execution budgets and governor checkpoints precede every
   observable side effect. A fault suspends or moves the configured instance
   toward the safe state rather than retrying indefinitely.
6. **BMS governor.** A complete VCU/BMS snapshot is evaluated by a pure fixed
   point function. `MaxDischargeCurrent` and bus voltage produce a hard power
   ceiling. `DERATE` returns the safe value and the explicit intervention fault;
   `BLOCK` returns zero. The governor retains no previous request or threshold.
7. **UDS/CAN response.** The integration emits only a value approved by the
   governor and the transmission authorization gate. An intervention is
   recorded before response emission. If the governor, HAL, BMS or sink is
   unavailable, the response is withheld.
8. **Diagnostics and trace.** Fault codes, counters and state transitions are
   copied to caller-owned sinks. Diagnostics do not change the safety decision,
   create a hidden retry or substitute for a hardware fault.

### 3.2 Nominal and fault data flow

```text
CAN transceiver / BMS / VCU
          |
          v
  CAN CRC + controller state  ---- Bus-Off / CRC / BOR / IWDG ---> safe latch
          |
          v
  bounded RX FIFO -> Cortex-M ISR -> SPSC ISR queue
                                      |
                                      v
                 HAL validation + monotonic timestamp
                                      |
                                      v
              event min-heap (fault priority, bounded)
                    |                         |
                    v                         v
             codec / ISO-TP / UDS       fault manager / FSM
                    |                         |
                    +----------+--------------+
                               v
                  BMS governor + capabilities + TX gate
                               |
                    approved derated response only
                               v
                    CAN TX ring / transceiver
```

The arrows from CRC, Bus-Off and brownout to the safe latch are intentionally
hardware-first. Software observes and records the safe condition; it does not
rewrite a corrupted frame, clear a controller fault by fiat, or postpone the
zero-torque latch until after MCU shutdown.

## 4. Safe-state policy and fail-closed rules

### 4.1 Safe state

`SAFE_STATE` means, at minimum:

* torque command is zero;
* battery contactors are open/de-energized;
* CAN transmission authorization is revoked except for a reviewed safe-state
  broadcast path; and
* the FSM is suspended or follows the explicitly reviewed recovery transition.

The Cortex-M watchdog recovery path constructs the canonical safe-state frame
(ID `0x100`) with zero torque, open contactors and the active flag. Recovery
requires explicit FSM authorization after boot checks; Bus-Off recovery also
requires the bounded healthy-observation protocol in the HIL model.

### 4.2 Fail-closed conditions

The affected action is denied on NULL or malformed API input, invalid schema,
unknown capability, invalid arithmetic, short/unsupported frame, queue
saturation, missing sink/governor, BMS fault, Bus-Off, interface failure,
clock fault, CRC rejection or watchdog/BOR reset. The system does not infer a
safe value from stale BMS data or a partially decoded payload.

For the BMS governor specifically:

* current is rounded up when deriving requested current;
* a request above the current-derived power ceiling returns `DERATE`, the
  lower `allowed_power_w`, and `GOVERNOR_INTERVENTION`;
* a faulted or invalid BMS snapshot returns `BLOCK`, zero power/torque and a
  non-zero fault code; and
* `cancestry_bms_governor_may_transmit()` is false for a blocked/invalid result.

The integration must not turn a `DERATE` result into an approval for the
original request. It shall log/raise the returned fault before it sends the
safe response or moves the FSM.

### 4.3 Hard-fault queue escalation

The queue's Path A reserve is a safety capacity partition, not a promise that
an unbounded number of faults can be stored. When every physical slot already
contains a fault, `cancestry_event_hard_fault_escalate()` preserves the bounded
fault set and invokes two caller-bound actions in a fixed order:

1. the HAL fail-safe action calls `cancestry_hardware_set_safe_state()`, which
   forces zero torque, opens contactors and revokes transmission authorization;
2. the IWDG action calls `cancestry_watchdog_escalate()`, leaving the independent
   watchdog on its reset path.

The queue increments `hard_fault_escalations` and returns `ERR_FULL_FAULT`; it
does not evict an existing diagnostic fault. The portable hook boundary is tested
by `EVENT-RESERVED-SLOTS-001..005`, and the Cortex-M binding is tested by
`HARD-FAULT-ESCALATION-001..002`. A host pass demonstrates software ordering and
boundedness; target GPIO, watchdog and contactor timing remain integrator/HIL
measurements required for QA-EV-01 closure.

## 5. Failure Modes and Effects Analysis summary

This is a software-level FMEA summary, not a replacement for the item HARA or
hardware FMEA. Severity/occurrence/detection ratings must be reconciled with
the vehicle program's rating scheme.

| Failure mode | Local effect | Detection / evidence | Safe reaction | Relevant requirements |
|---|---|---|---|---|
| Corrupted CAN bits or invalid CRC | Physical frame is not trustworthy | Controller CRC/error counter; `HIL-CRC-001` | Hardware drops frame before software queue; no decode or FSM action | `SW-FR-HIL-003`, `SW-FR-CODEC-008` |
| CAN controller Bus-Off | No trustworthy transmit path | Controller state, HAL fault, `HIL-BUSOFF-001` | Revoke TX, FSM `SAFE_STATE`; hardware recovery then bounded re-arm | `SW-FR-HIL-002`, `SW-FR-CAN-004..005`, `SW-FR-BM-006` |
| RX/TX ring or event queue full | Traffic or diagnostics may be lost | Reserved-slot, overflow and hard-fault escalation tests | Stop ordinary admission at the reserve boundary; retain faults; HAL safe state then IWDG on all-fault saturation | `SW-FR-EVENT-004..008`, `SW-FR-FSM-020`, `SW-FR-BM-005..006`, `QA-EV-01` |
| Short, unsupported or out-of-range frame | Partial signal could be unsafe | HAL/codec validation and conformance tests | Atomic drop; no partial signal update | `SW-FR-CODEC-008`, `SW-FR-CANFD-002..003` |
| ISR overrun or main-loop hang | Events are delayed; control deadline is missed | WCET/latency measurement and IWDG | IWDG reset; hardware zero torque/contactors open | `SW-FR-BM-003..007` |
| Brownout / supply collapse | MCU may execute unpredictably during power loss | BOR reset cause, voltage capture, `HIL-BROWNOUT-001` | BOR hardware latch forces zero torque/open contactors before power-down | `SW-FR-HIL-004`, `SW-FR-BM-005..006` |
| BMS thermal derating | VCU request exceeds available discharge current | Governor result and `GOVERNOR_INTERVENTION` fault; BMS vectors | Block original response; emit only returned derated value | `SW-FR-BMS-002..005` |
| BMS snapshot invalid or unavailable | Current limit cannot be trusted | Input validation and BMS fault code | `BLOCK`, zero output, no stale-value fallback | `SW-FR-BMS-001`, `SW-FR-BMS-006` |
| Unauthorized UDS/CAN side effect | A diagnostic or recipe could command unsafe output | Capability/governor denial counter and negative response | No partial store/effect; SAFE policy as configured | `SW-FR-GOV-005..006`, `SW-FR-UDS-007` |
| Heap allocation or non-deterministic state | Timing and memory safety cannot be bounded | Archive symbol scan, ASan/UBSan, review | Build/release gate rejects artifact | `SYS-NF-001..002`, `SW-FR-BMS-001` |
| Counter/trace saturation | Diagnostic evidence may wrap | Fixed-width policy, high-water/overflow flags and review | Safety decision uses explicit status, never counter absence | `SW-FR-EVENT-005`, `SW-FR-SAFETY-001..003` |

## 6. ISO 26262 ASIL-B Technical Safety Requirement Mapping

The following mapping is a software allocation of ASIL-B-aligned technical
safety requirements (TSRs). It is evidence for assessor review; the vehicle
program must approve the final TSR wording and allocation. The completed
manual/release evidence requirements are `SW-FR-SAFETY-001`,
`SW-FR-SAFETY-002`, `SW-FR-SAFETY-003`, `SW-FR-SAFETY-004` and
`SW-FR-SAFETY-005`.

| TSR | Safety intent | CANcestry implementation | Verification evidence |
|---|---|---|---|
| TSR-CAN-01 | Do not act on corrupted bus data | Hardware CAN CRC boundary; malformed/short-frame atomic rejection | `HIL-CRC-001`; codec/HAL conformance; `SW-FR-HIL-003` |
| TSR-CAN-02 | Contain loss of CAN availability | Bus-Off fault event, TX revocation, SAFE_STATE and bounded recovery | `HIL-BUSOFF-001`; HAL fault tests; `SW-FR-HIL-002` |
| TSR-PWR-01 | Reach zero torque before reset/power loss | Cortex-M BOR/IWDG and GPIO safe latch; watchdog safe frame | `BM-SAFE-001..004`; `HIL-BROWNOUT-001`; `SW-FR-BM-005..006` |
| TSR-RT-01 | Keep interrupt hand-off bounded | O(1) ISR, static SPSC ring, main-loop processing only | `BM-LAT-001..004`; archive/no-alloc gate; `SW-FR-BM-003..007` |
| TSR-MEM-01 | Exclude runtime heap failure | Caller-owned storage, static governor, linker/ archive checks | `BM-ALLOC-001`; `cancestry_governor_no_malloc_symbols`; `SYS-NF-002` |
| TSR-DET-01 | Equal inputs yield equal outputs | Integer fixed-point governor, deterministic event ordering and no hidden state | `BMS-GOV-001..006`; golden/runtime tests; `SW-FR-BMS-001..003` |
| TSR-BMS-01 | Limit power to available discharge current | `MaxDischargeCurrent` × bus voltage ceiling with conservative rounding | `BMS-GOV-001`, `BMS-GOV-005`; `SW-FR-BMS-002..003` |
| TSR-BMS-02 | Prevent original request after derating | `DERATE` output and explicit intervention fault consumed by integration | `BMS-GOV-001`; governor README/integration contract; `SW-FR-BMS-004..005` |
| TSR-BMS-03 | Fail safe when BMS data is invalid | Zero-output `BLOCK` for invalid/faulted snapshot | `BMS-GOV-003..004`; `SW-FR-BMS-006` |
| TSR-UDS-01 | Gate diagnostic side effects | UDS governor checkpoint before any store/effect; NULL denies | `UDS-GOV-001..005`; `SW-FR-UDS-007` |
| TSR-RES-01 | Bound resource use and fault saturation | Fixed capacities, reserved fault slots, deterministic non-fault eviction, explicit drops/counters and ordered HAL/IWDG escalation | `EVENT-RESERVED-SLOTS-001..005`; `HARD-FAULT-ESCALATION-001..002`; `SW-FR-EVENT-004..008`; `QA-EV-01` |
| TSR-TRACE-01 | Make safety evidence auditable | Traceability CSV, final v1 report, deterministic HIL JSON and release gates | `final_v1_report.md`; `ci/check_traceability.py`; `SW-FR-SAFETY-004..005` |

## 7. Resource, timing and independence constraints

| Resource | Bound / policy | Safety relevance |
|---|---|---|
| Event queue | 1..32767 caller-owned slots; default reserve of two fault slots (clamped for tiny queues) | No heap growth; ordinary traffic cannot consume fault capacity; bounded heap operations |
| ISR queue | Caller-owned fixed SPSC ring | O(1) interrupt hand-off; overflow is observable |
| CAN payload | Classic 1..8; FD 1..8, 12, 16, 20, 24, 32, 48 or 64 bytes | No truncation or out-of-bounds bit access |
| Expression text | 256 characters | Bounded parser/evaluator work |
| FSM transition chain | Four transitions by default | Prevents uncontrolled generated recursion |
| FSM actions/events | 128 actions / 64 generated events by default | Bounded activation time |
| Governor arithmetic | 32-bit public values, 64-bit intermediates, fixed-point efficiency | Reviewable no-float safety decision |
| Runtime allocation | Zero in core/governor/platform execution archives | Excludes allocator failure and unbounded latency |
| Watchdog | Target-configured IWDG deadline | Resets a missed main-loop deadline |

The integrator shall measure target WCET, stack, ISR latency, queue high-water,
watchdog margin and power-latch timing. The bounds above are not a target WCET
claim.

## 8. Verification and release gate

The independent evidence set is:

1. **Functional/conformance tests:** CTest suites for event, codec, FSM, HAL,
   transport, UDS, bare-metal and BMS behavior under strict warnings.
2. **Sanitizers:** host ASan/UBSan for memory and undefined arithmetic behavior.
3. **Static allocation gate:** `ci/check_no_alloc.py` over runtime archives,
   including `libcancestry_governor.a`.
4. **HIL simulation:** `tests/hil/hil_fault_injection.py` and its host tests;
   the report explicitly marks physical target measurements as outstanding.
5. **Formal/static tools:** Frama-C/WP, KLEE and MISRA/cppcheck procedures
   listed in `formal/` and the existing safety records.
6. **Traceability:** `ci/check_traceability.py` resolves requirement ids and
   refuses silently absent or failed rows.
7. **Documentation review:** this manual, the FMEA/TSR mappings, BMS contract,
   HIL report, final v1 trace report and release notes.

The current `1.0.0-rc.1` candidate is not a final release. A final `1.0.0`
tag is permitted only when the build/test/traceability/no-allocation commands
pass, no CSV row has status `failed`, the HIL simulation is green, QA-EV-01 is
formally closed, and the target limitations in the HIL report are accepted by
the responsible safety owner. Target hardware confirmation is a prerequisite
for a vehicle release even though the repository's host evidence is complete.
Until those conditions hold, the final version bump, tag and milestone closure
are explicitly deferred.

## 9. Change control

Every safety-relevant change shall:

* cite its requirement ids in code and in the traceability matrix;
* add or update a test before changing a passing claim;
* preserve determinism, fixed bounds, no-allocation and fail-closed behavior;
* update the FMEA/TSR mapping and final report when the safety boundary
  changes; and
* receive maintainer and safety-owner review before release.

A weaker precondition, hidden retry, stale-BMS fallback, software repair of a
hardware fault or unreviewed allocator is a safety-case regression, even if a
host test still passes.

## 10. Evidence index

* BMS implementation: [`core/governor/README.md`](../../core/governor/README.md)
* BMS conformance: [`tests/conformance/governor/test_bms_governor.c`](../../tests/conformance/governor/test_bms_governor.c)
* HIL runner: [`tests/hil/README.md`](../../tests/hil/README.md)
* HIL report: [`docs/qa/hil-fault-injection-report.md`](../qa/hil-fault-injection-report.md)
* Final traceability report: [`docs/trace/final_v1_report.md`](../trace/final_v1_report.md)
* Machine-readable matrix: [`docs/trace/traceability.csv`](../trace/traceability.csv)
* Existing bare-metal safe-state evidence: [`platform/cortex_m/watchdog.c`](../../platform/cortex_m/watchdog.c)

This manual is the `1.0.0-rc.1` candidate software safety-manual baseline;
QA-EV-01 remains open. It must not be represented as an ISO 26262 certificate
or as evidence that a complete vehicle item has achieved ASIL-B.
