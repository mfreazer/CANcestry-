/*
 * CANcestry - Lock-free ISR-to-main-loop event queue implementation.
 *
 * Implements: SW-FR-BM-003 (interrupt safety and bounded ISR execution).
 */

#include "cancestry/event/isr_queue.h"

#include <string.h>

#if defined(__arm__) || defined(__thumb__)
#define CANCESTRY_BARRIER() __asm volatile("dmb" ::: "memory")
#elif defined(__GNUC__) || defined(__clang__)
#define CANCESTRY_BARRIER() __sync_synchronize()
#else
#define CANCESTRY_BARRIER() ((void)0)
#endif

bool cancestry_event_isr_queue_init(cancestry_event_isr_queue_t *queue,
                                    cancestry_event_t *storage,
                                    uint16_t capacity)
{
    if (queue == NULL || storage == NULL || capacity < 2u) {
        return false;
    }

    memset(queue, 0, sizeof(*queue));
    queue->slots = storage;
    queue->capacity = capacity;
    queue->head = 0u;
    queue->tail = 0u;
    queue->dropped = 0u;
    queue->pushed = 0u;
    queue->popped = 0u;
    queue->high_water = 0u;
    return true;
}

bool cancestry_event_isr_queue_push(cancestry_event_isr_queue_t *queue,
                                    const cancestry_event_t *event)
{
    uint16_t current_head;
    uint16_t current_tail;
    uint16_t next_head;
    uint16_t count;

    if (queue == NULL || queue->slots == NULL || event == NULL) {
        return false;
    }

    current_head = queue->head;
    current_tail = queue->tail;
    next_head = (uint16_t)((current_head + 1u) % queue->capacity);

    if (next_head == current_tail) {
        /* Queue is full: drop frame, strictly non-blocking (SW-FR-BM-003) */
        queue->dropped++;
        return false;
    }

    queue->slots[current_head] = *event;
    CANCESTRY_BARRIER();
    queue->head = next_head;
    queue->pushed++;

    /* Update occupancy snapshot */
    if (next_head >= current_tail) {
        count = (uint16_t)(next_head - current_tail);
    } else {
        count = (uint16_t)(queue->capacity - (current_tail - next_head));
    }
    if ((uint32_t)count > queue->high_water) {
        queue->high_water = (uint32_t)count;
    }

    return true;
}

bool cancestry_event_isr_queue_pop(cancestry_event_isr_queue_t *queue,
                                   cancestry_event_t *out_event)
{
    uint16_t current_tail;
    uint16_t current_head;
    uint16_t next_tail;

    if (queue == NULL || queue->slots == NULL) {
        return false;
    }

    current_tail = queue->tail;
    current_head = queue->head;

    if (current_tail == current_head) {
        return false;
    }

    if (out_event != NULL) {
        *out_event = queue->slots[current_tail];
    }

    next_tail = (uint16_t)((current_tail + 1u) % queue->capacity);
    CANCESTRY_BARRIER();
    queue->tail = next_tail;
    queue->popped++;
    return true;
}

uint32_t cancestry_event_isr_queue_drain(cancestry_event_isr_queue_t *isr_queue,
                                         cancestry_event_queue_t *dest_queue)
{
    cancestry_event_t event;
    uint32_t drained = 0u;

    if (isr_queue == NULL || dest_queue == NULL) {
        return 0u;
    }

    while (cancestry_event_isr_queue_pop(isr_queue, &event)) {
        cancestry_event_queue_status_t status = cancestry_event_queue_push(dest_queue, &event);
        if (cancestry_event_queue_status_is_ok(status)) {
            drained++;
        }
    }

    return drained;
}

uint16_t cancestry_event_isr_queue_count(const cancestry_event_isr_queue_t *queue)
{
    uint16_t head;
    uint16_t tail;

    if (queue == NULL || queue->capacity == 0u) {
        return 0u;
    }

    head = queue->head;
    tail = queue->tail;

    if (head >= tail) {
        return (uint16_t)(head - tail);
    }
    return (uint16_t)(queue->capacity - (tail - head));
}

bool cancestry_event_isr_queue_is_empty(const cancestry_event_isr_queue_t *queue)
{
    if (queue == NULL) {
        return true;
    }
    return queue->head == queue->tail;
}
