/*
 * CANcestry - Bare-metal allocation stubs and safety tripwires.
 *
 * Implements: SW-FR-BM-001 (linker-level zero-alloc enforcement),
 *             SYS-NF-002 (zero heap allocation).
 */

#include "alloc_stubs.h"

#include <stdlib.h>

void cancestry_zero_alloc_tripwire(void)
{
#if defined(__arm__) || defined(__thumb__)
    /* Trigger Cortex-M HardFault / Breakpoint */
    __asm volatile("bkpt #0\n"
                   "1: b 1b\n");
#else
    /* Host environment tripwire */
    abort();
#endif
}

bool cancestry_is_heap_allocated(const void *ptr)
{
    (void)ptr;
    return false;
}

#if defined(CANCESTRY_ALLOC_TRIPWIRE)

/*
 * When explicitly built with tripwires enabled (for runtime crash testing),
 * these stubs intercept any accidental libc calls and halt execution.
 */
void *malloc(size_t size)
{
    (void)size;
    cancestry_zero_alloc_tripwire();
    return NULL;
}

void free(void *ptr)
{
    (void)ptr;
    cancestry_zero_alloc_tripwire();
}

void *calloc(size_t nmemb, size_t size)
{
    (void)nmemb;
    (void)size;
    cancestry_zero_alloc_tripwire();
    return NULL;
}

void *realloc(void *ptr, size_t size)
{
    (void)ptr;
    (void)size;
    cancestry_zero_alloc_tripwire();
    return NULL;
}

void *_sbrk(ptrdiff_t incr)
{
    (void)incr;
    cancestry_zero_alloc_tripwire();
    return (void *)-1;
}

#endif
