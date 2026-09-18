/*
 * CANcestry - Lock-free ISR-to-main-loop event queue.
 *
 * Normative references:
 *   docs/software/SwRS.md          SW-FR-EVENT-004, SW-FR-BM-003
 *   docs/system/SyRS.md            SYS-NF-001 (determinism), SYS-NF-002 (bounded resources)
 *
 * Design notes:
 *   - Zero heap allocation. The queue operates entirely on caller-owned storage.
 *   - Lock-free, wait-free Single-Producer Single-Consumer (SPSC) ring buffer.
 *   - Producer side is strictly safe for execution within an Interrupt Service
 *     Routine (ISR). The push operation executes in bounded O(1) time (< 1 µs)
 *     with no dynamic memory allocation, no loops, and no blocking waits.
 *   - Consumer side runs in thread/main() context and drains pending events
 *     directly into the prioritized core event queue.
 *
 * Implements: SW-FR-BM-003 (interrupt safety and bounded ISR execution).
 */

#ifndef CANCESTRY_EVENT_ISR_QUEUE_H
#define CANCESTRY_EVENT_ISR_QUEUE_H

#include "cancestry/event/queue.h"
#include "cancestry/event/types.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Lock-free SPSC ISR event queue.
 *
 * Instances are caller-owned. Head is updated exclusively by the producer
 * (ISR), and tail is updated exclusively by the consumer (main loop).
 */
typedef struct cancestry_event_isr_queue {
    /** Caller-owned array of @c capacity event slots. */
    cancestry_event_t *slots;
    /** Number of slots in @p slots. */
    uint16_t capacity;
    /** Head index where the producer (ISR) writes the next event. */
    volatile uint16_t head;
    /** Tail index where the consumer (main loop) reads the next event. */
    volatile uint16_t tail;
    /** Number of events dropped because the ISR queue was full. */
    volatile uint32_t dropped;
    /** Total events successfully pushed by the ISR. */
    volatile uint32_t pushed;
    /** Total events successfully consumed by the main loop. */
    volatile uint32_t popped;
    /** Highest occupancy observed. */
    volatile uint32_t high_water;
} cancestry_event_isr_queue_t;

/**
 * Initialize a lock-free ISR event queue.
 *
 * @param queue     Queue instance to initialize.
 * @param storage   Caller-owned event array of @c capacity elements.
 * @param capacity  Buffer size, must be at least 2.
 * @return true on success, false if parameters are invalid.
 */
bool cancestry_event_isr_queue_init(cancestry_event_isr_queue_t *queue,
                                    cancestry_event_t *storage,
                                    uint16_t capacity);

/**
 * Push an event from an Interrupt Service Routine (ISR).
 *
 * Lock-free, wait-free, bounded execution time (O(1)). If the queue is full,
 * the event is dropped (fail-closed, drop-newest), @c dropped is incremented,
 * and false is returned.
 *
 * @param queue  Target ISR queue.
 * @param event  Event to enqueue (copied by value).
 * @return true if enqueued, false if full or invalid.
 */
bool cancestry_event_isr_queue_push(cancestry_event_isr_queue_t *queue,
                                    const cancestry_event_t *event);

/**
 * Pop an event from the consumer (main loop) context.
 *
 * Lock-free, wait-free, O(1).
 *
 * @param queue      Target ISR queue.
 * @param out_event  Buffer to receive popped event (may be NULL to discard).
 * @return true if an event was popped, false if queue is empty.
 */
bool cancestry_event_isr_queue_pop(cancestry_event_isr_queue_t *queue,
                                   cancestry_event_t *out_event);

/**
 * Drain all pending events from the lock-free ISR queue into the main
 * prioritized event min-heap queue.
 *
 * Called periodically or during hal_poll_rx() by the main loop.
 *
 * @param isr_queue   Source lock-free queue filled by interrupts.
 * @param dest_queue  Destination min-heap prioritized event queue.
 * @return Number of events transferred.
 */
uint32_t cancestry_event_isr_queue_drain(cancestry_event_isr_queue_t *isr_queue,
                                         cancestry_event_queue_t *dest_queue);

/**
 * @return Current number of pending events in the ISR queue.
 */
uint16_t cancestry_event_isr_queue_count(const cancestry_event_isr_queue_t *queue);

/**
 * @return true if the ISR queue holds no events.
 */
bool cancestry_event_isr_queue_is_empty(const cancestry_event_isr_queue_t *queue);

#ifdef __cplusplus
}
#endif

#endif /* CANCESTRY_EVENT_ISR_QUEUE_H */
