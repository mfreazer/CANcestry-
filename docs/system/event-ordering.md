# CANcestry Event Ordering Specification

Version: 1.0.0-rc.1

This specification is the normative event and queue policy for the release
candidate. QA-EV-01 is open until the reserved-slot and hard-fault escalation
proof is accepted; the final `1.0.0` release marker is intentionally deferred.

## 1. Event metadata

Every event shall contain:

- `event_id`
- `type`
- `timestamp_us`
- `sequence`
- `cause_sequence`
- `priority_class`
- `payload`

The queue assigns a non-zero `event_id` and `sequence` when the producer leaves
them unset. The caller owns event storage and no queue operation allocates.

## 2. Time base

The runtime shall use a monotonic microsecond clock. Simulation shall use a
deterministic virtual monotonic clock.

## 3. Priority classes

Lower number means higher priority.

| Class | Value |
|---|---:|
| FAULT | 0 |
| MODE | 1 |
| TIMER | 2 |
| CAN_RX | 3 |
| HOST | 4 |
| GENERATED | 5 |
| TRACE | 6 |

A critical fault may be handled synchronously by a safety monitor, but any fault
event that enters a queue still follows the bounded admission rules in section
11.

## 4. Event selection order

The dispatcher shall select events using:

1. `timestamp_us` ascending;
2. `priority_class` ascending;
3. `sequence` ascending.

The queue implements this total order with a fixed-storage binary min-heap.

## 5. CAN RX ordering

CAN frames shall be processed in hardware timestamp or ISR arrival sequence.
Frames shall not be reordered by higher-level code before they become events.

## 6. Codec decoding order

For each CAN RX frame:

1. matching message definitions are selected;
2. definitions are ordered by package load order, codec map order, then message
   ID ascending;
3. signals are decoded in compiler-assigned signal order;
4. only changed signals generate `signal_changed` events; and
5. signal events are emitted in compiler-assigned signal order.

## 7. Subscriber dispatch order

For each dispatched event, subscribers run in this order:

1. built-in safety monitors;
2. recipe instances;
3. FSM instances; and
4. logger / trace.

The governor is not a normal subscriber. It observes and approves side-effect
requests. Recipe and FSM instance order is declaration order after package load
order.

## 8. Generated events

Generated events shall:

- use the `GENERATED` priority class;
- carry `cause_sequence`;
- inherit the causing timestamp unless explicitly delayed;
- be appended in action execution order; and
- not be processed recursively.

## 9. Queue overflow policies

The portable queue primitive has one deterministic policy. A queue is initialized
with a physical capacity and a `reserved_fault_slots` count. The default
initializer reserves two slots (clamped so a one-slot queue remains usable);
callers may use `cancestry_event_queue_init_with_reserved_fault_slots()` for an
explicit value.

| Incoming event | Admission rule | Result |
|---|---|---|
| non-fault below the non-fault limit | ordinary storage remains | enqueue |
| non-fault depth at or above the non-fault limit | protected slots remain available | drop newest, increment `dropped` and `overflow_events`, return `ERR_RESERVED_FAULT_SLOTS` |
| fault with physical room | any physical slot is available | enqueue, including use of a reserved slot |
| fault at physical capacity with a non-fault present | evict the newest non-fault by the full ordering key | enqueue the fault, return `OK_EVICTED_VICTIM`, increment `dropped` and `overflow_events` |
| fault at physical capacity with only faults present | no safe software victim exists | retain all queued faults, reject the incoming fault, increment `dropped` and `overflow_events`, and invoke hard-fault escalation |

The non-fault limit is `capacity - reserved_fault_slots` **non-fault events**,
counted independently from queued faults. Reserved slots are physical capacity,
not a second buffer, so the storage bound is always exact.
A zero explicit reservation is supported for a non-safety queue that elects the
historical generic overflow policy; safety/FSM queues use the default reserve.

Every overflow is counted. `consecutive_overflows` resets after an admitted push
that did not drop or evict an event. Persistent overflow is reported when it
reaches `CANCESTRY_EVENT_QUEUE_PERSISTENT_OVERFLOW_THRESHOLD`.

## 10. Event identity

- `event_id` is a queue-local, non-zero identity for trace and replay
  correlation. It is never reused within a queue lifetime, including after
  eviction.
- `sequence` is a queue-local monotonic ordering tie-breaker. It is assigned on
  every admitted push and is not assigned to rejected events.

## 11. Fault admission and hard-fault saturation

### 11.1 Reserved-slot admission

Ordinary events are admitted only while the queued non-fault depth is below
the non-fault limit. Faults are not subject to that limit and may use every
free physical slot. Consequently, a burst of ordinary traffic cannot consume
the capacity needed to record the configured number of faults.

### 11.2 Full queue containing non-fault events

When a fault arrives at physical capacity and a non-fault is present, the victim
is the non-fault event with the greatest event ordering key:

1. greatest `timestamp_us`;
2. among equal timestamps, greatest `priority_class`; and
3. among equal values, greatest `sequence`.

This is the deterministic drop-newest analogue of the queue's pop order. The
incoming fault receives a fresh sequence and remains queued.

### 11.3 Full queue containing only faults

When all physical slots contain faults, the primitive does **not** evict an
existing fault and does not pretend the incoming fault was admitted. It records
`ERR_FULL_FAULT`, increments `hard_fault_escalations`, and calls
`cancestry_event_hard_fault_escalate()` with the configured hooks.

The escalation hook order is mandatory:

1. the HAL fail-safe action forces zero torque / open contactors and revokes TX;
2. the IWDG action leaves the independent watchdog on its reset path.

The portable core supplies the ordered hook boundary. The Cortex-M binding in
`platform/cortex_m/watchdog.c` connects those hooks to
`cancestry_hardware_set_safe_state()` and `cancestry_watchdog_escalate()`.
Missing hooks are a configuration error; the queue remains bounded and the
existing fault set is retained rather than evicted.

### 11.4 Policy summary

- Reserved fault slots protect fault admission from ordinary saturation.
- Faults may evict only a non-fault event, and the victim is selected
  deterministically.
- An all-fault full queue is a hard-fault condition, not an eviction case.
- All drops, evictions, and escalations are observable through queue counters.

## 12. Queue policy instantiation

The same primitive is used by global and per-FSM queues. Safety queues shall
use the default reserve or an explicit non-zero reserve and shall bind both hard-
fault hooks. A queue that deliberately uses zero reserved slots must document
that choice and must not be used as the sole safety fault sink.
