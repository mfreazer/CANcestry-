/*
 * KLEE harness for the total event ordering relation.
 *
 * Implements: SW-FR-EVENT-006, SW-FR-FSM-046, SYS-NF-001, SYS-NF-002.
 *
 * Timestamp values are deliberately unconstrained uint64_t values, including
 * UINT64_MAX. Priority and sequence are symbolic as well. The assertions
 * prove antisymmetry and transitivity of the lexicographic comparator without
 * assuming a friendly clock or insertion order.
 */

#include "cancestry/event/types.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __KLEE__
#include <klee/klee.h>
#else
extern void klee_assert(int condition);
extern void klee_assume(int condition);
extern void klee_make_symbolic(void *address, size_t size, const char *name);
#endif

static int order_leq(const cancestry_event_t *left, const cancestry_event_t *right)
{
    return cancestry_event_compare_order(left, right) <= 0;
}

int main(void)
{
    cancestry_event_t first = {0};
    cancestry_event_t second = {0};
    cancestry_event_t third = {0};
    int first_second;
    int second_first;

    klee_make_symbolic(&first.timestamp_us, sizeof(first.timestamp_us), "time_a");
    klee_make_symbolic(&second.timestamp_us, sizeof(second.timestamp_us), "time_b");
    klee_make_symbolic(&third.timestamp_us, sizeof(third.timestamp_us), "time_c");
    klee_make_symbolic(&first.priority_class, sizeof(first.priority_class), "priority_a");
    klee_make_symbolic(&second.priority_class, sizeof(second.priority_class), "priority_b");
    klee_make_symbolic(&third.priority_class, sizeof(third.priority_class), "priority_c");
    klee_make_symbolic(&first.sequence, sizeof(first.sequence), "sequence_a");
    klee_make_symbolic(&second.sequence, sizeof(second.sequence), "sequence_b");
    klee_make_symbolic(&third.sequence, sizeof(third.sequence), "sequence_c");

    klee_assume(first.priority_class >= CANCESTRY_PRIORITY_CLASS_FAULT);
    klee_assume(second.priority_class >= CANCESTRY_PRIORITY_CLASS_FAULT);
    klee_assume(third.priority_class >= CANCESTRY_PRIORITY_CLASS_FAULT);
    klee_assume(first.priority_class < CANCESTRY_PRIORITY_CLASS_COUNT);
    klee_assume(second.priority_class < CANCESTRY_PRIORITY_CLASS_COUNT);
    klee_assume(third.priority_class < CANCESTRY_PRIORITY_CLASS_COUNT);

    first_second = cancestry_event_compare_order(&first, &second);
    second_first = cancestry_event_compare_order(&second, &first);

    /* The implementation returns -1, 0 or 1 and never an incomparable value. */
    klee_assert(first_second >= -1 && first_second <= 1);
    klee_assert(second_first == -first_second);

    if (order_leq(&first, &second) && order_leq(&second, &third)) {
        klee_assert(order_leq(&first, &third));
    }
    return 0;
}
