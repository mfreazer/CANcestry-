/*
 * CANcestry - Bare-metal allocation stubs and safety tripwires.
 *
 * Implements: SW-FR-BM-001 (linker-level zero-alloc enforcement),
 *             SYS-NF-002 (zero heap allocation).
 *
 * This header defines compile-time and runtime tripwires that prevent any
 * heap allocation on Cortex-M targets. If any hidden allocation is attempted,
 * it triggers an immediate HardFault or abort() instead of corrupting memory.
 */

#ifndef CANCESTRY_PLATFORM_CORTEX_M_ALLOC_STUBS_H
#define CANCESTRY_PLATFORM_CORTEX_M_ALLOC_STUBS_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Tripwire invoked whenever an allocation attempt is caught at runtime.
 *
 * Triggers a breakpoint or hardware HardFault on Cortex-M, and abort() on
 * host test builds.
 */
void cancestry_zero_alloc_tripwire(void);

/**
 * Returns false always: the bare-metal runtime owns no heap memory.
 */
bool cancestry_is_heap_allocated(const void *ptr);

#ifdef __cplusplus
}
#endif

#endif /* CANCESTRY_PLATFORM_CORTEX_M_ALLOC_STUBS_H */
