/*
 * CANcestry - bounded, caller-owned event queue.
 *
 * Normative references:
 *   docs/system/event-ordering.md  sections 3, 4, 8, 9
 *   docs/software/SwRS.md          SW-FR-EVENT-004 .. SW-FR-EVENT-008, QA-EV-01
 *   docs/system/SyRS.md            SYS-NF-001 (determinism), SYS-NF-002 (bounded resources)
 *
 * Design notes:
 *   - Zero heap allocation. The queue operates entirely on a buffer supplied by
 *     the caller through cancestry_event_queue_init().
 *   - No global mutable state. Two queues are fully independent.
 *   - Single-threaded. Access shall be serialized by the caller for v0.1.
 *   - Pop order is the normative selection order: timestamp_us, then
 *     priority_class, then sequence, all ascending. The queue implements it
 *     with a binary min-heap, so pushes and pops are O(log n) with no
 *     allocation and no worst-case pathological behavior.
 *   - Path A overflow policy (see section 9 of the ordering spec): ordinary
 *     events stop at capacity - reserved_fault_slots; faults may use any free
 *     slot and may evict only the newest non-fault event when physical storage
 *     is full. A physically full all-fault queue retains its bounded fault set,
 *     increments hard_fault_escalations and invokes the configured HAL/IWDG
 *     hooks. Every drop increments counters.dropped and counters.overflow_events.
 */

#ifndef CANCESTRY_EVENT_QUEUE_H
#define CANCESTRY_EVENT_QUEUE_H

#include "cancestry/event/clock.h"
#include "cancestry/event/fault.h"
#include "cancestry/event/types.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Default queue depth, matching the FSM default of SW-FR-FSM-019. */
#define CANCESTRY_EVENT_QUEUE_DEFAULT_CAPACITY ((uint16_t)64u)

/**
 * Default number of physical slots reserved for fault admission (QA-EV-01).
 *
 * The initializer clamps this value to capacity - 1 for tiny queues so a
 * one-slot queue remains useful for an ordinary event. Callers that need a
 * fault-only queue or a different safety margin use the explicit initializer.
 */
#define CANCESTRY_EVENT_QUEUE_DEFAULT_RESERVED_FAULT_SLOTS ((uint16_t)2u)

/**
 * Largest capacity accepted by cancestry_event_queue_init().
 *
 * The bound keeps heap index arithmetic (2 * index + 2) inside 16 bits, which
 * matters on targets where int is 16 bits wide.
 */
#define CANCESTRY_EVENT_QUEUE_MAX_CAPACITY ((uint16_t)32767u)

/**
 * Number of consecutive overflow events after which the condition is reported
 * as persistent. docs/system/mode-fault-state-machine.md rates a single
 * "Event queue overflow" as WARNING and "Persistent event overflow" as ERROR.
 * The queue only counts; raising the fault is owned by the fault manager.
 */
#define CANCESTRY_EVENT_QUEUE_PERSISTENT_OVERFLOW_THRESHOLD ((uint32_t)8u)

/** Operation results. Values >= 0 mean the requested operation succeeded. */
typedef enum cancestry_event_queue_status {
    /** The event was enqueued. */
    CANCESTRY_EVENT_QUEUE_OK = 0,
    /** The event was enqueued; a queued non-fault event was dropped to admit a fault. */
    CANCESTRY_EVENT_QUEUE_OK_EVICTED_VICTIM = 1,
    /** A required pointer argument was NULL, or the queue was never initialized. */
    CANCESTRY_EVENT_QUEUE_ERR_NULL = -1,
    /** The requested capacity is zero or above the supported maximum. */
    CANCESTRY_EVENT_QUEUE_ERR_CAPACITY = -2,
    /** The event is malformed: invalid type or invalid priority class. */
    CANCESTRY_EVENT_QUEUE_ERR_EVENT = -3,
    /** Queue full; the new non-fault event was dropped (drop-newest). */
    CANCESTRY_EVENT_QUEUE_ERR_FULL = -4,
    /** Queue full of fault events; the new fault event was not admitted. */
    CANCESTRY_EVENT_QUEUE_ERR_FULL_FAULT = -5,
    /** Queue has reached its non-fault admission limit; a slot is reserved. */
    CANCESTRY_EVENT_QUEUE_ERR_RESERVED_FAULT_SLOTS = -7,
    /** Terminal hard-fault state; no further event admission is permitted. */
    CANCESTRY_EVENT_QUEUE_ERR_TERMINAL = -8,
    /** Queue empty; there is nothing to pop or peek. */
    CANCESTRY_EVENT_QUEUE_ERR_EMPTY = -6
} cancestry_event_queue_status_t;

/**
 * Drop and operation counters.
 *
 * All counters are monotonic except @c consecutive_overflows, which resets as
 * soon as a push does not drop anything.
 */
typedef struct cancestry_event_queue_counters {
    /** Events successfully enqueued, including faults that evicted a victim. */
    uint32_t pushed;
    /** Events successfully dequeued. */
    uint32_t popped;
    /** Events discarded because of overflow, including evicted victims. */
    uint32_t dropped;
    /** Push attempts rejected because of malformed arguments. */
    uint32_t rejected;
    /** Number of distinct overflow occurrences (SW-FR-EVENT-005). */
    uint32_t overflow_events;
    /** Overflow occurrences since the last push that did not drop an event. */
    uint32_t consecutive_overflows;
    /** Highest occupancy observed since initialization. */
    uint32_t high_water;
    /** Times an all-fault queue forced the hard-fault escalation path. */
    uint32_t hard_fault_escalations;
} cancestry_event_queue_counters_t;

/**
 * Bounded event queue.
 *
 * Instances are caller-owned and may live in static storage, on the stack, or
 * in a caller-managed arena. The queue never allocates.
 */
typedef struct cancestry_event_queue {
    /** Caller-owned storage of @c capacity events. */
    cancestry_event_t *slots;
    /** Number of events the storage can hold. */
    uint16_t capacity;
    /** Number of events currently queued. */
    uint16_t size;
    /** Number of queued events in the FAULT priority class. */
    uint16_t fault_count;
    /** Physical slots protected from ordinary (non-fault) admission. */
    uint16_t reserved_fault_slots;
    /** Actions used if all physical slots already contain faults. */
    cancestry_event_hard_fault_hooks_t hard_fault_hooks;
    /** Set after escalation; terminal queues reject every later push. */
    bool hard_fault_terminal;
    /** Next sequence number to assign; sequences start at 1. */
    uint32_t next_sequence;
    /** Next event id to assign; ids start at 1. */
    uint32_t next_event_id;
    /** Drop and operation counters. */
    cancestry_event_queue_counters_t counters;
} cancestry_event_queue_t;

/**
 * Initialize a queue over caller-owned storage.
 *
 * Any previously queued events and counters are discarded.
 *
 * @param queue     Queue to initialize; ignored when NULL.
 * @param storage   Event array that backs the queue. It shall outlive the queue
 *                  and shall not move.
 * @param capacity  Number of events in @p storage, in
 *                  [1, CANCESTRY_EVENT_QUEUE_MAX_CAPACITY]. The default
 *                  initializer reserves CANCESTRY_EVENT_QUEUE_DEFAULT_RESERVED_FAULT_SLOTS
 *                  physical slots for fault events (clamped for tiny queues).
 * @return true when the queue is ready for use, false when @p queue is NULL,
 *         @p storage is NULL, or @p capacity is out of range. On failure the
 *         queue is left in a safely unusable state.
 */
bool cancestry_event_queue_init(cancestry_event_queue_t *queue,
                                cancestry_event_t *storage,
                                uint16_t capacity);

/**
 * Initialize a queue with an explicit reserved fault-slot count.
 *
 * @param reserved_fault_slots Number of physical slots that non-fault events
 *                             may never consume. It may be zero for a legacy
 *                             best-effort queue, or equal to @p capacity for a
 *                             fault-only queue. No heap allocation occurs.
 * @return true when all arguments are valid.
 */
bool cancestry_event_queue_init_with_reserved_fault_slots(
    cancestry_event_queue_t *queue,
    cancestry_event_t *storage,
    uint16_t capacity,
    uint16_t reserved_fault_slots);

/** @return Number of physical slots protected for faults, or 0 if invalid. */
uint16_t cancestry_event_queue_reserved_fault_slots(
    const cancestry_event_queue_t *queue);

/**
 * Bind the platform fail-safe and IWDG actions used for unrecoverable fault
 * saturation. The hooks are copied; the caller retains ownership of context.
 */
bool cancestry_event_queue_set_hard_fault_hooks(
    cancestry_event_queue_t *queue,
    const cancestry_event_hard_fault_hooks_t *hooks);

/** @return true when @p queue is non-NULL and initialized. */
bool cancestry_event_queue_is_valid(const cancestry_event_queue_t *queue);

/** @return Configured capacity, or 0 when @p queue is invalid. */
uint16_t cancestry_event_queue_capacity(const cancestry_event_queue_t *queue);

/** @return Number of queued events, or 0 when @p queue is invalid. */
uint16_t cancestry_event_queue_size(const cancestry_event_queue_t *queue);

/** @return Number of queued fault events, or 0 when @p queue is invalid. */
uint16_t cancestry_event_queue_fault_depth(const cancestry_event_queue_t *queue);

/** @return true when the queue is valid and holds no events. */
bool cancestry_event_queue_is_empty(const cancestry_event_queue_t *queue);

/** @return true when the queue is valid and holds @c capacity events. */
bool cancestry_event_queue_is_full(const cancestry_event_queue_t *queue);

/** @return true after all-fault saturation has entered terminal safe handling. */
bool cancestry_event_queue_is_terminal(const cancestry_event_queue_t *queue);

/**
 * Enqueue an event.
 *
 * The queue copies @p event, assigns @c sequence (and @c event_id when the
 * caller did not supply one), and maintains the normative pop order. The
 * caller's copy is not modified.
 *
 * @return CANCESTRY_EVENT_QUEUE_OK or CANCESTRY_EVENT_QUEUE_OK_EVICTED_VICTIM
 *         on success, otherwise a negative ::cancestry_event_queue_status_t.
 */
cancestry_event_queue_status_t cancestry_event_queue_push(cancestry_event_queue_t *queue,
                                                          const cancestry_event_t *event);

/**
 * Build and enqueue an event in one step.
 *
 * The timestamp is taken from @p clock, which is required: the runtime never
 * invents time.
 *
 * @param queue           Destination queue.
 * @param clock           Monotonic clock source; required.
 * @param type            Event type.
 * @param priority_class  Priority class.
 * @param cause_sequence  Sequence of the causing event, or CANCESTRY_SEQUENCE_NONE.
 * @param payload         Payload to copy, or NULL for a zeroed payload.
 */
cancestry_event_queue_status_t cancestry_event_queue_emit(cancestry_event_queue_t *queue,
                                                          const cancestry_clock_t *clock,
                                                          cancestry_event_type_t type,
                                                          cancestry_priority_class_t priority_class,
                                                          cancestry_sequence_t cause_sequence,
                                                          const cancestry_event_payload_t *payload);

/**
 * Enqueue a derived event, following docs/system/event-ordering.md section 8.
 *
 * The generated event is placed in the GENERATED priority class, carries the
 * causing event's sequence in @c cause_sequence, and inherits the causing
 * event's timestamp unless @p delay_us is non-zero.
 *
 * @return The result of the underlying push, or
 *         CANCESTRY_EVENT_QUEUE_ERR_NULL when @p cause is NULL.
 */
cancestry_event_queue_status_t cancestry_event_queue_emit_generated(
    cancestry_event_queue_t *queue,
    const cancestry_event_t *cause,
    cancestry_event_type_t type,
    const cancestry_event_payload_t *payload,
    cancestry_time_us_t delay_us);

/**
 * Inspect the next event without removing it.
 *
 * @return Pointer to the next event in selection order, valid until the queue
 *         is modified, or NULL when the queue is invalid or empty.
 */
const cancestry_event_t *cancestry_event_queue_peek(const cancestry_event_queue_t *queue);

/**
 * Remove and return the next event in selection order.
 *
 * @param queue      Source queue.
 * @param out_event  Destination for the popped event, or NULL to discard it.
 * @return CANCESTRY_EVENT_QUEUE_OK on success, otherwise a negative status.
 */
cancestry_event_queue_status_t cancestry_event_queue_pop(cancestry_event_queue_t *queue,
                                                         cancestry_event_t *out_event);

/**
 * Discard all queued events. Counters are preserved.
 */
void cancestry_event_queue_clear(cancestry_event_queue_t *queue);

/**
 * @return Pointer to the live counters, or NULL when @p queue is invalid.
 */
const cancestry_event_queue_counters_t *cancestry_event_queue_counters(
    const cancestry_event_queue_t *queue);

/**
 * @return true when the queue has overflowed at least
 *         CANCESTRY_EVENT_QUEUE_PERSISTENT_OVERFLOW_THRESHOLD times in a row,
 *         which the fault manager shall report as at least WARNING.
 */
bool cancestry_event_queue_overflow_is_persistent(const cancestry_event_queue_t *queue);

/** @return true when @p status indicates success. */
bool cancestry_event_queue_status_is_ok(cancestry_event_queue_status_t status);

/** @return Stable, statically allocated name for @p status. Never NULL. */
const char *cancestry_event_queue_status_name(cancestry_event_queue_status_t status);

#ifdef __cplusplus
}
#endif

#endif /* CANCESTRY_EVENT_QUEUE_H */
