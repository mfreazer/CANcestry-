# core/event - portable event model

The event model is the foundation that the codec, recipe, FSM, governor and
logger layers build on. It is a value-oriented C99 library with no heap
allocation, no global mutable state and no threading primitives, so the same
source builds for firmware and for host simulation.

| Field | Value |
|---|---|
| Status | Implemented (issue #1, Phase 1) |
| Normative sources | `docs/system/event-ordering.md`, `docs/software/SwRS.md`, `docs/system/SyRS.md` |
| Requirements | SW-FR-EVENT-001 .. SW-FR-EVENT-006, SYS-IR-005, SYS-NF-001, SYS-NF-002, QA-v0.2-R04 |

## Layout

```text
core/event/
  include/cancestry/event/
    clock.h    monotonic clock, virtual clock for simulation
    types.h    event struct, enumerations, typed payload union
    queue.h    bounded, caller-owned event queue
  src/
    clock.c
    queue.c
    types.c
tests/unit/core/event/   unit tests (CTest)
```

## Event metadata

Every event carries exactly the metadata listed in section 1 of
`docs/system/event-ordering.md`:

| Field | Meaning |
|---|---|
| `event_id` | Runtime-unique identity of this event instance, for trace and replay correlation. The queue assigns it on push when the caller leaves it at `CANCESTRY_EVENT_ID_NONE`; a caller may supply its own (for example when re-injecting recorded events). |
| `type` | One of the seven normative types. Selects the payload member. |
| `timestamp_us` | Monotonic microseconds from the injected clock. |
| `sequence` | Ordering sequence assigned by the queue on push. It is the last tie-breaker of the selection order, so it is unique per queue. |
| `cause_sequence` | `sequence` of the causing event, or `CANCESTRY_SEQUENCE_NONE`. Generated events always carry it. |
| `priority_class` | FAULT .. TRACE; lower number is higher priority. |
| `payload` | Type-specific payload union. |

Payloads reference names by ID, not by stored string: the codec and FSM layers
intern signal, timer and state names at load time. Name pointers such as
`signal_name` are borrowed, static-lifetime diagnostics that the core never
copies or frees.

## Ordering

Pop order is the normative selection order, ascending:

1. `timestamp_us`
2. `priority_class`
3. `sequence`

The queue implements it with a binary min-heap over a fixed array, so pushes and
pops are O(log n) with no allocation and no worst-case pathological behavior.
Because `sequence` is unique, the ordering is a total order: given the same push
sequence, the pop sequence is fully determined (SYS-NF-001).

`cancestry_event_compare_order()` is the single implementation of the
comparison, so the queue, any future dispatcher and the tests cannot drift
apart.

## Overflow policy

Section 9 of the ordering spec is implemented as follows when the queue is full:

| Incoming event | Queue content | Result |
|---|---|---|
| non-fault | any | the incoming event is dropped (drop-newest), `dropped` and `overflow_events` increment, status `ERR_FULL` |
| fault | at least one non-fault event | the fault is admitted and the **newest non-fault** event is evicted, `dropped` and `overflow_events` increment, status `OK_EVICTED_VICTIM` |
| fault | only fault events | the incoming fault is dropped, `dropped` and `overflow_events` increment, status `ERR_FULL_FAULT` |

The eviction victim is the non-fault event with the highest ordering key, which
is the event that would have been popped last. That is the direct analogue of
drop-newest and is deterministic because sequence numbers are unique. Faults are
therefore never dropped while any non-fault event remains queued
(SW-FR-FSM-020, QA-v0.2-R04).

`consecutive_overflows` counts overflow events since the last push that dropped
nothing. Once it reaches `CANCESTRY_EVENT_QUEUE_PERSISTENT_OVERFLOW_THRESHOLD`,
`cancestry_event_queue_overflow_is_persistent()` reports true. The queue only
counts; raising the WARNING (or ERROR, for persistent overflow) is owned by the
fault manager, which does not exist yet.

## Clocks

Nothing in the runtime reads a clock directly. Producers receive a
`cancestry_clock_t`, which binds a callback to an opaque caller-owned context:

```c
cancestry_virtual_clock_t vclock;      /* simulation and tests */
cancestry_virtual_clock_init(&vclock, 0u);
cancestry_clock_t clock = cancestry_clock_from_virtual(&vclock);

cancestry_clock_t hw = cancestry_clock_platform();   /* host or target */
```

`cancestry_platform_now_us()` uses the host monotonic clock on POSIX and
Windows. On a bare-metal target it is a weak symbol returning 0, which the
platform port overrides with a hardware timer tick.

## Usage

```c
static cancestry_event_t storage[64];
cancestry_event_queue_t queue;
cancestry_event_t event;

cancestry_event_queue_init(&queue, storage, 64u);

cancestry_event_payload_t payload;
memset(&payload, 0, sizeof(payload));
payload.can_rx.can_id = 0x1A0u;
payload.can_rx.length = 8u;

cancestry_event_queue_emit(&queue, &clock, CANCESTRY_EVENT_TYPE_CAN_RX,
                           CANCESTRY_PRIORITY_CLASS_CAN_RX,
                           CANCESTRY_SEQUENCE_NONE, &payload);

while (cancestry_event_queue_pop(&queue, &event) == CANCESTRY_EVENT_QUEUE_OK) {
    /* events arrive in selection order */
}
```

## Build and test

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug      # ASan + UBSan by default
cmake --build build
ctest --test-dir build --output-on-failure
```

`CANCESTRY_ENABLE_ASAN=OFF` disables sanitizers. On glibc, the non-sanitized
build also installs a tripwire over `malloc`/`calloc`/`realloc`/`free` and
asserts that queue operations perform no allocation; every build additionally
runs `ci/check_no_alloc.py`, which inspects the compiled archive for allocator
references.

## Deliberately out of scope

- Dispatching, subscription and bus fan-out (a later event-bus layer).
- CAN drivers and hardware timestamping.
- YAML/TOML parsing and file I/O.
- Thread safety and ISR/main-context handover. Access shall be serialized by
  the caller for v0.1.
- Sequence wraparound at 2^32 pushes. A runtime restart is expected long before
  that; the queue never wraps silently inside a single ordering window.
