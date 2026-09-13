# CANcestry Event Ordering Specification

Version: 0.2.1

## 1. Event Metadata

Every event shall contain:

- event_id
- type
- timestamp_us
- sequence
- cause_sequence
- priority_class
- payload

## 2. Time Base

The runtime shall use a monotonic microsecond clock.

Simulation shall use a deterministic virtual monotonic clock.

## 3. Priority Classes

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

Critical faults may be handled synchronously and bypass normal queueing.

## 4. Event Selection Order

The dispatcher shall select events using:

1. timestamp_us ascending
2. priority_class ascending
3. sequence ascending

## 5. CAN RX Ordering

CAN frames shall be processed in hardware timestamp or ISR arrival sequence.

Frames shall not be reordered by higher-level code.

## 6. Codec Decoding Order

For each CAN RX frame:

1. Matching message definitions are selected.
2. Definitions are ordered by package load order, codec map order, then message ID ascending.
3. Signals are decoded in compiler-assigned signal order.
4. Only changed signals generate signal_changed events.
5. Signal events are emitted in compiler-assigned signal order.

## 7. Subscriber Dispatch Order

For each dispatched event, subscribers run in this order:

1. Built-in safety monitors
2. Recipe instances
3. FSM instances
4. Logger / trace

The governor is not a normal subscriber. It observes and approves side-effect requests.

Recipe order:

- package load order
- recipe file order
- recipe definition order

FSM instance order:

- package load order
- instance declaration order

## 8. Generated Events

Generated events shall:

- be placed in the GENERATED priority class,
- carry cause_sequence,
- inherit the timestamp of the causing event unless explicitly delayed,
- be appended in action execution order,
- not be processed recursively.

## 9. Queue Overflow Policies

| Queue | Policy |
|---|---|
| CAN RX queue | drop-oldest |
| Global event queue | drop-newest for non-fault events |
| Per-FSM incoming event queue | drop-newest for non-fault events |
| Per-FSM generated event queue | drop-newest |
| Fault events | never dropped |
| Trace queue | drop-newest |

Overflow shall increment counters.

Persistent overflow shall raise at least WARNING.

## 10. Event Identity

- `event_id`: Globally unique identifier assigned by the event producer
  at event creation time. Monotonically increasing across the entire
  runtime. Used for traceability, log correlation, and debugging.
  Never reused, even after queue eviction.

- `sequence`: Per-source monotonic counter. Each event producer (CAN RX,
  FSM instance, recipe engine, etc.) maintains its own sequence. Used
  only as the third tiebreaker in deterministic pop ordering.

## 11. Fault Admission and Saturation

### 11.1 Fault admission on non-fault-full queue

When a fault event arrives at a full queue containing non-fault events,
the non-fault event to evict is selected by:

1. Lowest priority class (highest numeric value).
2. Among equal priority: highest timestamp_us (newest).
3. Among equal timestamp: highest sequence.

### 11.2 Fault-on-fault saturation

When a fault event arrives at a full queue containing only fault events,
the existing fault event to evict is selected by:

1. Lowest priority class (highest numeric value).
2. Among equal priority: lowest timestamp_us (oldest).
3. Among equal timestamp: lowest sequence.

This ensures that under fault saturation, the most recent and
highest-priority faults are preserved.

### 11.3 Policy summary

- Fault events are never dropped due to non-fault admission.
- Under fault-only saturation, bounded deterministic eviction occurs.
- All eviction increments drop_count and sets the persistent-overflow
  flag for the fault manager.

## 12. Queue Policy Instantiation

The event queue primitive implements a single overflow policy. Different
queue classes (global event queue, per-FSM incoming queue, per-FSM
generated queue, CAN RX/TX queues) may require different policies.
Multiple instantiations with different policies may be added in future
phases if needed.
