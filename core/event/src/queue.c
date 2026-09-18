/*
 * CANcestry - bounded, caller-owned event queue implementation.
 *
 * Implementation notes:
 *   - The queue is a binary min-heap over a fixed array of events. Ordering
 *     follows docs/system/event-ordering.md section 4 exactly: timestamp_us,
 *     then priority_class, then sequence, all ascending.
 *   - Sequences are unique and strictly increasing, so the ordering is a total
 *     order: for any push sequence the pop sequence is fully determined
 *     (SYS-NF-001).
 *   - No allocation, no recursion, no global state, no threading primitives.
 *     Every operation is O(log n) except the fault-admission path, which scans
 *     the queue once to pick the drop-newest victim.
 *   - Path A reserves physical slots for faults. Ordinary events cannot use
 *     those slots; a fault may use any free slot or evict the newest non-fault.
 *     An all-fault full queue invokes the configured fail-safe/IWDG hooks.
 */

#include "cancestry/event/queue.h"

#include <stdint.h>
#include <string.h>

#define CANCESTRY_QUEUE_NO_INDEX ((size_t)SIZE_MAX)

/* ------------------------------------------------------------------------- */
/* Heap internals                                                            */
/* ------------------------------------------------------------------------- */

static void queue_sift_up(cancestry_event_t *slots, size_t index)
{
    while (index > 0u) {
        size_t parent = (index - 1u) / 2u;
        if (cancestry_event_compare_order(&slots[index], &slots[parent]) < 0) {
            cancestry_event_t tmp = slots[index];
            slots[index] = slots[parent];
            slots[parent] = tmp;
            index = parent;
        } else {
            break;
        }
    }
}

static void queue_sift_down(cancestry_event_t *slots, size_t size, size_t index)
{
    for (;;) {
        size_t left = (2u * index) + 1u;
        size_t right = left + 1u;
        size_t smallest = index;

        if (left < size && cancestry_event_compare_order(&slots[left], &slots[smallest]) < 0) {
            smallest = left;
        }
        if (right < size && cancestry_event_compare_order(&slots[right], &slots[smallest]) < 0) {
            smallest = right;
        }
        if (smallest == index) {
            break;
        }
        {
            cancestry_event_t tmp = slots[index];
            slots[index] = slots[smallest];
            slots[smallest] = tmp;
        }
        index = smallest;
    }
}

/**
 * Remove the event at @p index, preserving the heap property.
 *
 * fault_count is owned by the caller: this helper only shrinks the heap.
 */
static void queue_remove_at(cancestry_event_queue_t *queue, size_t index)
{
    size_t last;

    if (queue == NULL || queue->size == 0u || index >= (size_t)queue->size) {
        return;
    }

    last = (size_t)queue->size - 1u;
    if (index != last) {
        queue->slots[index] = queue->slots[last];
        queue->size = (uint16_t)last;
        queue_sift_down(queue->slots, (size_t)queue->size, index);
        queue_sift_up(queue->slots, index);
    } else {
        queue->size = (uint16_t)last;
    }
}

/**
 * Locate the newest non-fault event: the non-fault event with the highest
 * ordering key, i.e. the one that would be popped last.
 *
 * Ties are impossible because sequence numbers are unique.
 */
static size_t queue_find_newest_non_fault(const cancestry_event_queue_t *queue)
{
    size_t best = CANCESTRY_QUEUE_NO_INDEX;
    size_t i;

    if (queue == NULL) {
        return CANCESTRY_QUEUE_NO_INDEX;
    }

    for (i = 0u; i < (size_t)queue->size; ++i) {
        if (queue->slots[i].priority_class == CANCESTRY_PRIORITY_CLASS_FAULT) {
            continue;
        }
        if (best == CANCESTRY_QUEUE_NO_INDEX ||
            cancestry_event_compare_order(&queue->slots[i], &queue->slots[best]) > 0) {
            best = i;
        }
    }
    return best;
}

/** Advance a 1-based monotonic counter, skipping the reserved zero value. */
static uint32_t queue_next_id(uint32_t *counter)
{
    uint32_t value = *counter + 1u;
    if (value == 0u) {
        value = 1u;
    }
    *counter = value;
    return value;
}

/** Insert into a queue that is known to have room. */
static cancestry_event_queue_status_t queue_enqueue(cancestry_event_queue_t *queue,
                                                    const cancestry_event_t *event)
{
    size_t index = (size_t)queue->size;
    cancestry_event_t stored = *event;

    stored.sequence = queue_next_id(&queue->next_sequence);
    if (stored.event_id == CANCESTRY_EVENT_ID_NONE) {
        stored.event_id = queue_next_id(&queue->next_event_id);
    }

    queue->slots[index] = stored;
    queue->size = (uint16_t)(index + 1u);
    if (stored.priority_class == CANCESTRY_PRIORITY_CLASS_FAULT) {
        queue->fault_count = (uint16_t)(queue->fault_count + 1u);
    }
    queue_sift_up(queue->slots, index);

    queue->counters.pushed++;
    if ((uint32_t)queue->size > queue->counters.high_water) {
        queue->counters.high_water = (uint32_t)queue->size;
    }
    return CANCESTRY_EVENT_QUEUE_OK;
}

/* ------------------------------------------------------------------------- */
/* Lifecycle                                                                 */
/* ------------------------------------------------------------------------- */

bool cancestry_event_queue_init(cancestry_event_queue_t *queue,
                                cancestry_event_t *storage,
                                uint16_t capacity)
{
    uint16_t reserved_fault_slots = CANCESTRY_EVENT_QUEUE_DEFAULT_RESERVED_FAULT_SLOTS;

    /* A one-slot queue cannot reserve a slot and still accept an ordinary
     * event. The explicit initializer remains available for callers that want
     * a different trade-off. */
    if (capacity <= reserved_fault_slots) {
        reserved_fault_slots = (capacity > 0u) ? (uint16_t)(capacity - 1u) : 0u;
    }
    return cancestry_event_queue_init_with_reserved_fault_slots(
        queue, storage, capacity, reserved_fault_slots);
}

bool cancestry_event_queue_init_with_reserved_fault_slots(
    cancestry_event_queue_t *queue,
    cancestry_event_t *storage,
    uint16_t capacity,
    uint16_t reserved_fault_slots)
{
    if (queue == NULL) {
        return false;
    }

    memset(queue, 0, sizeof(*queue));

    if (storage == NULL || capacity == 0u || capacity > CANCESTRY_EVENT_QUEUE_MAX_CAPACITY ||
        reserved_fault_slots > capacity) {
        return false;
    }

    queue->slots = storage;
    queue->capacity = capacity;
    queue->size = 0u;
    queue->fault_count = 0u;
    queue->reserved_fault_slots = reserved_fault_slots;
    queue->next_sequence = 0u;
    queue->next_event_id = 0u;
    return true;
}

bool cancestry_event_queue_is_valid(const cancestry_event_queue_t *queue)
{
    return (queue != NULL) && (queue->slots != NULL) && (queue->capacity > 0u) &&
           (queue->capacity <= CANCESTRY_EVENT_QUEUE_MAX_CAPACITY) &&
           (queue->reserved_fault_slots <= queue->capacity) &&
           (queue->size <= queue->capacity) && (queue->fault_count <= queue->size);
}

uint16_t cancestry_event_queue_reserved_fault_slots(
    const cancestry_event_queue_t *queue)
{
    if (!cancestry_event_queue_is_valid(queue)) {
        return 0u;
    }
    return queue->reserved_fault_slots;
}

bool cancestry_event_queue_set_hard_fault_hooks(
    cancestry_event_queue_t *queue,
    const cancestry_event_hard_fault_hooks_t *hooks)
{
    if (!cancestry_event_queue_is_valid(queue) || hooks == NULL) {
        return false;
    }
    queue->hard_fault_hooks = *hooks;
    return true;
}

uint16_t cancestry_event_queue_capacity(const cancestry_event_queue_t *queue)
{
    if (!cancestry_event_queue_is_valid(queue)) {
        return 0u;
    }
    return queue->capacity;
}

uint16_t cancestry_event_queue_size(const cancestry_event_queue_t *queue)
{
    if (!cancestry_event_queue_is_valid(queue)) {
        return 0u;
    }
    return queue->size;
}

uint16_t cancestry_event_queue_fault_depth(const cancestry_event_queue_t *queue)
{
    if (!cancestry_event_queue_is_valid(queue)) {
        return 0u;
    }
    return queue->fault_count;
}

bool cancestry_event_queue_is_empty(const cancestry_event_queue_t *queue)
{
    return cancestry_event_queue_is_valid(queue) && (queue->size == 0u);
}

bool cancestry_event_queue_is_full(const cancestry_event_queue_t *queue)
{
    return cancestry_event_queue_is_valid(queue) && (queue->size >= queue->capacity);
}

/* ------------------------------------------------------------------------- */
/* Producer side                                                             */
/* ------------------------------------------------------------------------- */

/*@
  requires queue != \null;
  requires event != \null;
  requires \valid(queue);
  requires \valid_read(event);
  requires queue->slots != \null;
  requires 0 < queue->capacity <= CANCESTRY_EVENT_QUEUE_MAX_CAPACITY;
  requires queue->size <= queue->capacity;
  requires queue->fault_count <= queue->size;
  requires \valid(queue->slots + (0 .. queue->capacity - 1));
  requires cancestry_event_is_valid(event);
  assigns queue->slots[0 .. queue->capacity - 1], queue->size,
          queue->fault_count, queue->next_sequence, queue->next_event_id,
          queue->counters;
  ensures queue->size <= queue->capacity;
  ensures queue->fault_count <= queue->size;
  ensures \result == CANCESTRY_EVENT_QUEUE_OK ||
          \result == CANCESTRY_EVENT_QUEUE_OK_EVICTED_VICTIM ||
          \result < 0;
*/
cancestry_event_queue_status_t cancestry_event_queue_push(cancestry_event_queue_t *queue,
                                                          const cancestry_event_t *event)
{
    size_t victim;

    if (queue == NULL || event == NULL) {
        return CANCESTRY_EVENT_QUEUE_ERR_NULL;
    }
    if (!cancestry_event_queue_is_valid(queue)) {
        return CANCESTRY_EVENT_QUEUE_ERR_NULL;
    }
    if (!cancestry_event_is_valid(event)) {
        queue->counters.rejected++;
        return CANCESTRY_EVENT_QUEUE_ERR_EVENT;
    }

    /* Path A: ordinary traffic can never consume more than the unreserved
     * non-fault share. Faults already occupying reserved slots do not reduce
     * the remaining non-fault share. */
    if (event->priority_class != CANCESTRY_PRIORITY_CLASS_FAULT &&
        queue->reserved_fault_slots != 0u &&
        (uint16_t)(queue->size - queue->fault_count) >=
            (uint16_t)(queue->capacity - queue->reserved_fault_slots)) {
        queue->counters.overflow_events++;
        queue->counters.consecutive_overflows++;
        queue->counters.dropped++;
        return CANCESTRY_EVENT_QUEUE_ERR_RESERVED_FAULT_SLOTS;
    }

    if (queue->size < queue->capacity) {
        queue->counters.consecutive_overflows = 0u;
        return queue_enqueue(queue, event);
    }

    /*
     * Physical overflow. A fault is admitted by evicting the newest non-fault
     * event. A full queue containing only faults has no safe software victim:
     * preserve the existing fault set and escalate to the HAL/IWDG path.
     */
    queue->counters.overflow_events++;
    queue->counters.consecutive_overflows++;

    if (event->priority_class != CANCESTRY_PRIORITY_CLASS_FAULT) {
        queue->counters.dropped++;
        return CANCESTRY_EVENT_QUEUE_ERR_FULL;
    }

    victim = queue_find_newest_non_fault(queue);
    if (victim == CANCESTRY_QUEUE_NO_INDEX) {
        /* The queue holds nothing but faults; do not evict a diagnostic fault. */
        queue->counters.dropped++;
        queue->counters.hard_fault_escalations++;
        (void)cancestry_event_hard_fault_escalate(&queue->hard_fault_hooks);
        return CANCESTRY_EVENT_QUEUE_ERR_FULL_FAULT;
    }

    queue_remove_at(queue, victim);
    queue->counters.dropped++;
    if (queue_enqueue(queue, event) != CANCESTRY_EVENT_QUEUE_OK) {
        /* Unreachable: the queue has room right after the eviction. */
        return CANCESTRY_EVENT_QUEUE_ERR_FULL_FAULT;
    }
    return CANCESTRY_EVENT_QUEUE_OK_EVICTED_VICTIM;
}

cancestry_event_queue_status_t cancestry_event_queue_emit(cancestry_event_queue_t *queue,
                                                          const cancestry_clock_t *clock,
                                                          cancestry_event_type_t type,
                                                          cancestry_priority_class_t priority_class,
                                                          cancestry_sequence_t cause_sequence,
                                                          const cancestry_event_payload_t *payload)
{
    cancestry_event_t event;

    if (queue == NULL || clock == NULL) {
        return CANCESTRY_EVENT_QUEUE_ERR_NULL;
    }
    if (!cancestry_clock_is_valid(clock)) {
        return CANCESTRY_EVENT_QUEUE_ERR_NULL;
    }

    cancestry_event_init(&event);
    event.type = type;
    event.priority_class = priority_class;
    event.cause_sequence = cause_sequence;
    event.timestamp_us = cancestry_clock_now_us(clock);
    if (payload != NULL) {
        event.payload = *payload;
    }
    return cancestry_event_queue_push(queue, &event);
}

cancestry_event_queue_status_t cancestry_event_queue_emit_generated(
    cancestry_event_queue_t *queue,
    const cancestry_event_t *cause,
    cancestry_event_type_t type,
    const cancestry_event_payload_t *payload,
    cancestry_time_us_t delay_us)
{
    cancestry_event_t event;

    if (queue == NULL || cause == NULL) {
        return CANCESTRY_EVENT_QUEUE_ERR_NULL;
    }

    cancestry_event_init(&event);
    event.type = type;
    /* Section 8: generated events are placed in the GENERATED priority class. */
    event.priority_class = CANCESTRY_PRIORITY_CLASS_GENERATED;
    event.cause_sequence = cause->sequence;
    /* ... and inherit the causing timestamp unless explicitly delayed. */
    event.timestamp_us = cancestry_time_sat_add(cause->timestamp_us, delay_us);
    if (payload != NULL) {
        event.payload = *payload;
    }
    return cancestry_event_queue_push(queue, &event);
}

/* ------------------------------------------------------------------------- */
/* Consumer side                                                             */
/* ------------------------------------------------------------------------- */

const cancestry_event_t *cancestry_event_queue_peek(const cancestry_event_queue_t *queue)
{
    if (!cancestry_event_queue_is_valid(queue) || queue->size == 0u) {
        return NULL;
    }
    return &queue->slots[0];
}

/*@
  requires queue != \null;
  requires \valid(queue);
  requires queue->slots != \null;
  requires 0 < queue->capacity <= CANCESTRY_EVENT_QUEUE_MAX_CAPACITY;
  requires queue->size <= queue->capacity;
  requires queue->fault_count <= queue->size;
  requires \valid(queue->slots + (0 .. queue->capacity - 1));
  requires out_event == \null || \valid(out_event);
  behavior discard:
    assumes out_event == \null;
    assigns queue->slots[0 .. queue->capacity - 1], queue->size,
            queue->fault_count, queue->counters;
  behavior copy:
    assumes out_event != \null;
    assigns queue->slots[0 .. queue->capacity - 1], queue->size,
            queue->fault_count, queue->counters, *out_event;
  complete behaviors;
  disjoint behaviors;
  ensures queue->size <= queue->capacity;
  ensures queue->fault_count <= queue->size;
  ensures \result == CANCESTRY_EVENT_QUEUE_OK || \result < 0;
*/
cancestry_event_queue_status_t cancestry_event_queue_pop(cancestry_event_queue_t *queue,
                                                         cancestry_event_t *out_event)
{
    if (queue == NULL) {
        return CANCESTRY_EVENT_QUEUE_ERR_NULL;
    }
    if (!cancestry_event_queue_is_valid(queue)) {
        return CANCESTRY_EVENT_QUEUE_ERR_NULL;
    }
    if (queue->size == 0u) {
        return CANCESTRY_EVENT_QUEUE_ERR_EMPTY;
    }

    if (out_event != NULL) {
        *out_event = queue->slots[0];
    }
    if (queue->slots[0].priority_class == CANCESTRY_PRIORITY_CLASS_FAULT) {
        queue->fault_count = (uint16_t)(queue->fault_count - 1u);
    }
    queue_remove_at(queue, 0u);

    queue->counters.popped++;
    return CANCESTRY_EVENT_QUEUE_OK;
}

void cancestry_event_queue_clear(cancestry_event_queue_t *queue)
{
    if (!cancestry_event_queue_is_valid(queue)) {
        return;
    }
    queue->size = 0u;
    queue->fault_count = 0u;
}

/* ------------------------------------------------------------------------- */
/* Diagnostics                                                               */
/* ------------------------------------------------------------------------- */

const cancestry_event_queue_counters_t *cancestry_event_queue_counters(
    const cancestry_event_queue_t *queue)
{
    if (!cancestry_event_queue_is_valid(queue)) {
        return NULL;
    }
    return &queue->counters;
}

bool cancestry_event_queue_overflow_is_persistent(const cancestry_event_queue_t *queue)
{
    if (!cancestry_event_queue_is_valid(queue)) {
        return false;
    }
    return queue->counters.consecutive_overflows >=
           CANCESTRY_EVENT_QUEUE_PERSISTENT_OVERFLOW_THRESHOLD;
}

bool cancestry_event_queue_status_is_ok(cancestry_event_queue_status_t status)
{
    return (int)status >= 0;
}

const char *cancestry_event_queue_status_name(cancestry_event_queue_status_t status)
{
    switch (status) {
    case CANCESTRY_EVENT_QUEUE_OK:
        return "OK";
    case CANCESTRY_EVENT_QUEUE_OK_EVICTED_VICTIM:
        return "OK_EVICTED_VICTIM";
    case CANCESTRY_EVENT_QUEUE_ERR_NULL:
        return "ERR_NULL";
    case CANCESTRY_EVENT_QUEUE_ERR_CAPACITY:
        return "ERR_CAPACITY";
    case CANCESTRY_EVENT_QUEUE_ERR_EVENT:
        return "ERR_EVENT";
    case CANCESTRY_EVENT_QUEUE_ERR_FULL:
        return "ERR_FULL";
    case CANCESTRY_EVENT_QUEUE_ERR_FULL_FAULT:
        return "ERR_FULL_FAULT";
    case CANCESTRY_EVENT_QUEUE_ERR_RESERVED_FAULT_SLOTS:
        return "ERR_RESERVED_FAULT_SLOTS";
    case CANCESTRY_EVENT_QUEUE_ERR_EMPTY:
        return "ERR_EMPTY";
    default:
        return "UNKNOWN";
    }
}
