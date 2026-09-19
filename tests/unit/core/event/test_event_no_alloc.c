/*
 * Verifies that the event queue performs no dynamic memory allocation.
 *
 * Verifies:
 *   SYS-NF-002    Bounded resource usage (no heap in the runtime path).
 *   issue #1      "No dynamic memory allocation is used in the queue
 *                 implementation."
 *
 * Test ids (docs/trace/traceability.csv):
 *   EVENT-NO-ALLOC-001  SYS-NF-002
 *
 * The test installs a tripwire over the C allocator for the duration of a
 * queue exercise. The tripwire is only available on glibc and is disabled
 * under AddressSanitizer, where overriding malloc would fight the sanitizer
 * runtime; the companion check ci/check_no_alloc.py covers every build by
 * inspecting the compiled archive for allocator references.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cancestry/event/queue.h"

#include "cancestry_test.h"

#if defined(__GLIBC__) && !defined(__SANITIZE_ADDRESS__)
#if defined(__has_feature)
#if __has_feature(address_sanitizer)
#define CANCESSTRY_TEST_SANITIZER_ACTIVE 1
#endif
#endif
#if !defined(CANCESSTRY_TEST_SANITIZER_ACTIVE)
#define CANCESSTRY_TEST_ALLOC_TRIPWIRE 1
#endif
#endif

#if defined(CANCESSTRY_TEST_ALLOC_TRIPWIRE)

/* glibc entry points that bypass the public allocator symbols. */
extern void *__libc_malloc(size_t size);
extern void *__libc_calloc(size_t count, size_t size);
extern void *__libc_realloc(void *ptr, size_t size);
extern void __libc_free(void *ptr);

static int cancestry_test_alloc_guard = 0;
static unsigned long cancestry_test_alloc_calls = 0u;

void *malloc(size_t size)
{
    if (cancestry_test_alloc_guard != 0) {
        cancestry_test_alloc_calls++;
    }
    return __libc_malloc(size);
}

void *calloc(size_t count, size_t size)
{
    if (cancestry_test_alloc_guard != 0) {
        cancestry_test_alloc_calls++;
    }
    return __libc_calloc(count, size);
}

void *realloc(void *ptr, size_t size)
{
    if (cancestry_test_alloc_guard != 0) {
        cancestry_test_alloc_calls++;
    }
    return __libc_realloc(ptr, size);
}

void free(void *ptr)
{
    if (cancestry_test_alloc_guard != 0) {
        cancestry_test_alloc_calls++;
    }
    __libc_free(ptr);
}

#endif /* CANCESSTRY_TEST_ALLOC_TRIPWIRE */

#define EXERCISE_CAPACITY 8u
#define EXERCISE_STEPS 64u

/**
 * The exercise deliberately drives the queue past its capacity, so overflow
 * results are expected as well. Anything else is a failure.
 */
static void no_op_retention(uint32_t code, void *context)
{
    (void)code;
    (void)context;
}

static void no_op_action(void *context)
{
    (void)context;
}

static bool install_no_op_hooks(cancestry_event_queue_t *queue)
{
    cancestry_event_hard_fault_hooks_t hooks;

    hooks.write_retention_register = no_op_retention;
    hooks.hal_fail_safe = no_op_action;
    hooks.iwdg_escalate = no_op_action;
    hooks.context = NULL;
    return cancestry_event_queue_set_hard_fault_hooks(queue, &hooks);
}

static bool status_is_acceptable(cancestry_event_queue_status_t status)
{
    switch (status) {
    case CANCESTRY_EVENT_QUEUE_OK:
    case CANCESTRY_EVENT_QUEUE_OK_EVICTED_VICTIM:
    case CANCESTRY_EVENT_QUEUE_ERR_FULL:
    case CANCESTRY_EVENT_QUEUE_ERR_FULL_FAULT:
    case CANCESTRY_EVENT_QUEUE_ERR_RESERVED_FAULT_SLOTS:
    case CANCESTRY_EVENT_QUEUE_ERR_TERMINAL:
        return true;
    case CANCESTRY_EVENT_QUEUE_ERR_NULL:
    case CANCESTRY_EVENT_QUEUE_ERR_CAPACITY:
    case CANCESTRY_EVENT_QUEUE_ERR_EVENT:
    case CANCESTRY_EVENT_QUEUE_ERR_EMPTY:
    default:
        return false;
    }
}

/**
 * Exercise every queue path: push, pop, peek, overflow, fault admission,
 * clear, emit and generated emit.
 *
 * Nothing in here may print: stdout buffering allocates on first use, which
 * would trip the guard for reasons unrelated to the queue.
 */
static void exercise_queue(bool *ok)
{
    cancestry_event_t storage[EXERCISE_CAPACITY];
    cancestry_event_t fallback_storage[2u];
    cancestry_event_queue_t queue;
    cancestry_event_queue_t other;
    cancestry_virtual_clock_t vclock;
    cancestry_clock_t clock;
    cancestry_event_t event;
    cancestry_event_t cause;
    cancestry_event_payload_t payload;
    uint32_t step;

    cancestry_virtual_clock_init(&vclock, 1000u);
    clock = cancestry_clock_from_virtual(&vclock);

    *ok = cancestry_event_queue_init(&queue, storage, EXERCISE_CAPACITY) && *ok;
    *ok = install_no_op_hooks(&queue) && *ok;
    *ok = cancestry_event_queue_init(&other, fallback_storage, 2u) && *ok;
    *ok = install_no_op_hooks(&other) && *ok;

    memset(&payload, 0, sizeof(payload));
    payload.can_rx.can_id = 0x321u;
    payload.can_rx.length = 8u;

    cause.timestamp_us = 0u;
    cause.sequence = 0u;
    cause.event_id = 0u;
    cause.type = CANCESTRY_EVENT_TYPE_CAN_RX;
    cause.priority_class = CANCESTRY_PRIORITY_CLASS_CAN_RX;
    cause.cause_sequence = CANCESTRY_SEQUENCE_NONE;
    memset(&cause.payload, 0, sizeof(cause.payload));

    for (step = 0u; step < EXERCISE_STEPS; ++step) {
        const cancestry_priority_class_t priority =
            ((step % 5u) == 0u) ? CANCESTRY_PRIORITY_CLASS_FAULT
                                : CANCESTRY_PRIORITY_CLASS_CAN_RX;

        *ok = status_is_acceptable(cancestry_event_queue_emit(
                  &queue, &clock, CANCESTRY_EVENT_TYPE_CAN_RX, priority, CANCESTRY_SEQUENCE_NONE,
                  &payload)) &&
              *ok;

        if ((step % 3u) == 0u) {
            *ok = status_is_acceptable(cancestry_event_queue_emit_generated(
                      &queue, &cause, CANCESTRY_EVENT_TYPE_SIGNAL_CHANGED, &payload,
                      (cancestry_time_us_t)step)) &&
                  *ok;
        }

        cancestry_virtual_clock_advance(&vclock, 7u);

        if (cancestry_event_queue_peek(&queue) != NULL && (step % 2u) == 0u) {
            *ok = (cancestry_event_queue_pop(&queue, &event) == CANCESTRY_EVENT_QUEUE_OK) && *ok;
            cause = event;
        }

        /* Also drive a second queue to prove there is no shared state. */
        *ok = status_is_acceptable(cancestry_event_queue_emit(
                  &other, &clock, CANCESTRY_EVENT_TYPE_TIMER_EXPIRED,
                  CANCESTRY_PRIORITY_CLASS_TIMER, CANCESTRY_SEQUENCE_NONE, NULL)) &&
              *ok;
        if (!cancestry_event_queue_is_empty(&other)) {
            *ok = (cancestry_event_queue_pop(&other, NULL) == CANCESTRY_EVENT_QUEUE_OK) && *ok;
        }
    }

    while (!cancestry_event_queue_is_empty(&queue)) {
        *ok = (cancestry_event_queue_pop(&queue, &event) == CANCESTRY_EVENT_QUEUE_OK) && *ok;
    }
    cancestry_event_queue_clear(&queue);
    cancestry_event_queue_clear(&other);

    /* Reject path and overflow path. */
    cancestry_event_init(&event);
    *ok = (cancestry_event_queue_push(&queue, &event) == CANCESTRY_EVENT_QUEUE_ERR_EVENT) && *ok;
    *ok = (cancestry_event_queue_init(&queue, storage, 1u)) && *ok;
    *ok = install_no_op_hooks(&queue) && *ok;
    *ok = (cancestry_event_queue_emit(&queue, &clock, CANCESTRY_EVENT_TYPE_CAN_RX,
                                      CANCESTRY_PRIORITY_CLASS_CAN_RX, CANCESTRY_SEQUENCE_NONE,
                                      NULL) == CANCESTRY_EVENT_QUEUE_OK) &&
          *ok;
    *ok = (cancestry_event_queue_emit(&queue, &clock, CANCESTRY_EVENT_TYPE_CAN_RX,
                                      CANCESTRY_PRIORITY_CLASS_HOST, CANCESTRY_SEQUENCE_NONE,
                                      NULL) == CANCESTRY_EVENT_QUEUE_ERR_FULL) &&
          *ok;
    *ok = (cancestry_event_queue_emit(&queue, &clock, CANCESTRY_EVENT_TYPE_FAULT_RAISED,
                                      CANCESTRY_PRIORITY_CLASS_FAULT, CANCESTRY_SEQUENCE_NONE,
                                      NULL) == CANCESTRY_EVENT_QUEUE_OK_EVICTED_VICTIM) &&
          *ok;
    *ok = (cancestry_event_queue_pop(&queue, NULL) == CANCESTRY_EVENT_QUEUE_OK) && *ok;
}

int main(void)
{
    bool ok = true;

    CANCESSTRY_TEST_SUITE_BEGIN("event queue allocation");

    CANCESSTRY_TEST_CASE("queue operations do not touch the allocator");
#if defined(CANCESSTRY_TEST_ALLOC_TRIPWIRE)
    cancestry_test_alloc_guard = 1;
    exercise_queue(&ok);
    cancestry_test_alloc_guard = 0;
    CANCESSTRY_TEST_CHECK_U64(cancestry_test_alloc_calls, 0u);
    printf("    allocator calls during queue exercise: %lu\n", cancestry_test_alloc_calls);
#else
    exercise_queue(&ok);
    printf("    allocator tripwire unavailable on this platform (covered by "
           "ci/check_no_alloc.py)\n");
#endif
    CANCESSTRY_TEST_CHECK(ok);

    return CANCESSTRY_TEST_SUITE_END();
}
