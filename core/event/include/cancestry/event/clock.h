/*
 * CANcestry - portable monotonic clock abstraction.
 *
 * Normative references:
 *   docs/system/event-ordering.md            sections 2 (Time Base), 4 (Event Selection Order)
 *   docs/system/SyRS.md                      SYS-IR-005 (monotonic time source)
 *   docs/software/SwRS.md                    SW-FR-EVENT-002 (event timestamps)
 *
 * Design notes:
 *   - The runtime never reads a global time source directly. Every producer of
 *     timestamps receives a ::cancestry_clock_t. This keeps the runtime free of
 *     global mutable state (SYS-NF-001) and lets simulation inject a
 *     deterministic virtual clock.
 *   - Clocks are caller-owned. A clock object holds no storage of its own; it
 *     only binds a function to an opaque context.
 *   - All time values are monotonic microseconds. They are never converted to
 *     wall-clock time inside the runtime.
 */

#ifndef CANCESTRY_EVENT_CLOCK_H
#define CANCESTRY_EVENT_CLOCK_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Monotonic time in microseconds. */
typedef uint64_t cancestry_time_us_t;

/** Largest representable timestamp. Additions saturate at this value. */
#define CANCESTRY_TIME_US_MAX ((cancestry_time_us_t)UINT64_MAX)

/**
 * Clock callback.
 *
 * The callback shall return a monotonically non-decreasing value in
 * microseconds. It shall not allocate memory, block, or read global mutable
 * state other than the platform time source.
 *
 * @param context Opaque caller-owned context, never retained.
 */
typedef cancestry_time_us_t (*cancestry_clock_fn)(void *context);

/**
 * Injectable monotonic clock source.
 *
 * A clock is valid when @c now_us is not NULL. @c context may be NULL for
 * clocks that need no state (for example the platform clock).
 */
typedef struct cancestry_clock {
    cancestry_clock_fn now_us;
    void *context;
} cancestry_clock_t;

/**
 * Deterministic virtual monotonic clock used by simulation and unit tests.
 *
 * The clock only advances when a caller advances it, which makes event
 * ordering reproducible for a fixed input sequence (SYS-NF-001,
 * SW-FR-SIM-004).
 */
typedef struct cancestry_virtual_clock {
    cancestry_time_us_t now_us;
} cancestry_virtual_clock_t;

/**
 * Initialize a virtual clock.
 *
 * @param clock     Clock to initialize; ignored when NULL.
 * @param start_us  Initial time. Time may start at any value, including 0.
 */
void cancestry_virtual_clock_init(cancestry_virtual_clock_t *clock, cancestry_time_us_t start_us);

/**
 * Read the current virtual time without advancing it.
 *
 * @return Current time, or 0 when @p clock is NULL.
 */
cancestry_time_us_t cancestry_virtual_clock_now(cancestry_virtual_clock_t *clock);

/**
 * Advance a virtual clock by a positive number of microseconds.
 *
 * @param clock     Clock to advance; ignored when NULL.
 * @param delta_us  Amount to advance. Zero is allowed and has no effect.
 */
void cancestry_virtual_clock_advance(cancestry_virtual_clock_t *clock, cancestry_time_us_t delta_us);

/**
 * Set the absolute virtual time.
 *
 * Setting a value lower than the current time is rejected, because the clock
 * must remain monotonic.
 *
 * @param clock   Clock to set; ignored when NULL.
 * @param now_us  New absolute time.
 * @return true when the time was changed, false when @p clock is NULL or the
 *         value would move time backwards.
 */
bool cancestry_virtual_clock_set(cancestry_virtual_clock_t *clock, cancestry_time_us_t now_us);

/**
 * Bind a virtual clock to the generic clock interface.
 *
 * @return A valid clock when @p clock is non-NULL, otherwise a nulled clock.
 */
cancestry_clock_t cancestry_clock_from_virtual(cancestry_virtual_clock_t *clock);

/**
 * Platform monotonic time in microseconds.
 *
 * Host builds derive this from the operating system monotonic clock. Target
 * builds shall provide this symbol; the default implementation is weakly
 * defined and returns 0 so that the core links before a platform port exists.
 *
 * @return Monotonic microseconds since an arbitrary platform epoch.
 */
cancestry_time_us_t cancestry_platform_now_us(void);

/**
 * Bind the platform monotonic clock to the generic clock interface.
 */
cancestry_clock_t cancestry_clock_platform(void);

/**
 * Saturating addition for timestamps.
 *
 * Time never wraps: the result is clamped at CANCESTRY_TIME_US_MAX so that a
 * delayed event can never sort before the event that caused it.
 *
 * @return @p base + @p delta, clamped at CANCESTRY_TIME_US_MAX.
 */
cancestry_time_us_t cancestry_time_sat_add(cancestry_time_us_t base, cancestry_time_us_t delta);

/**
 * @return true when @p clock is non-NULL and carries a callback.
 */
bool cancestry_clock_is_valid(const cancestry_clock_t *clock);

/**
 * Read a clock.
 *
 * @return Current time, or 0 when @p clock is invalid.
 */
cancestry_time_us_t cancestry_clock_now_us(const cancestry_clock_t *clock);

#ifdef __cplusplus
}
#endif

#endif /* CANCESTRY_EVENT_CLOCK_H */
