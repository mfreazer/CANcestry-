/*
 * CANcestry - portable monotonic clock implementation.
 *
 * The virtual clock is fully portable and is the clock used by simulation and
 * unit tests. The platform clock is only compiled in when the host or target
 * provides a monotonic time source; otherwise a weak default is emitted so the
 * core links before a platform port exists.
 */

/*
 * clock_gettime() is not exposed by glibc under a strict -std=c99 build unless
 * a POSIX feature macro is defined. The macro has to be defined before any
 * system header is included, which is why this block sits at the top of the
 * file instead of next to the platform clock implementation.
 */
#if !defined(_POSIX_C_SOURCE) && (defined(__unix__) || defined(__APPLE__) || defined(__linux__))
#define _POSIX_C_SOURCE 199309L
#endif

#include "cancestry/event/clock.h"

#if defined(_WIN32)
#include <windows.h>
#elif defined(__unix__) || defined(__APPLE__) || defined(__linux__)
#include <time.h>
#define CANCESTRY_CLOCK_HAS_POSIX 1
#endif

#if defined(__GNUC__) || defined(__clang__)
#define CANCESTRY_WEAK __attribute__((weak))
#else
#define CANCESTRY_WEAK
#endif

void cancestry_virtual_clock_init(cancestry_virtual_clock_t *clock, cancestry_time_us_t start_us)
{
    if (clock == NULL) {
        return;
    }
    clock->now_us = start_us;
}

cancestry_time_us_t cancestry_virtual_clock_now(cancestry_virtual_clock_t *clock)
{
    if (clock == NULL) {
        return 0u;
    }
    return clock->now_us;
}

void cancestry_virtual_clock_advance(cancestry_virtual_clock_t *clock, cancestry_time_us_t delta_us)
{
    if (clock == NULL) {
        return;
    }
    clock->now_us = cancestry_time_sat_add(clock->now_us, delta_us);
}

bool cancestry_virtual_clock_set(cancestry_virtual_clock_t *clock, cancestry_time_us_t now_us)
{
    if (clock == NULL) {
        return false;
    }
    if (now_us < clock->now_us) {
        return false;
    }
    clock->now_us = now_us;
    return true;
}

cancestry_time_us_t cancestry_time_sat_add(cancestry_time_us_t base, cancestry_time_us_t delta)
{
    if (base > (CANCESTRY_TIME_US_MAX - delta)) {
        return CANCESTRY_TIME_US_MAX;
    }
    return base + delta;
}

static cancestry_time_us_t cancestry_virtual_clock_read(void *context)
{
    cancestry_virtual_clock_t *clock = (cancestry_virtual_clock_t *)context;
    if (clock == NULL) {
        return 0u;
    }
    return clock->now_us;
}

cancestry_clock_t cancestry_clock_from_virtual(cancestry_virtual_clock_t *clock)
{
    cancestry_clock_t result;
    result.now_us = cancestry_virtual_clock_read;
    result.context = (void *)clock;
    return result;
}

static cancestry_time_us_t cancestry_platform_clock_read(void *context)
{
    (void)context;
    return cancestry_platform_now_us();
}

cancestry_clock_t cancestry_clock_platform(void)
{
    cancestry_clock_t result;
    result.now_us = cancestry_platform_clock_read;
    result.context = NULL;
    return result;
}

#if defined(_WIN32)

cancestry_time_us_t cancestry_platform_now_us(void)
{
    LARGE_INTEGER counter;
    LARGE_INTEGER frequency;
    if (QueryPerformanceCounter(&counter) == 0 || QueryPerformanceFrequency(&frequency) == 0) {
        return 0u;
    }
    if (frequency.QuadPart <= 0) {
        return 0u;
    }
    return (cancestry_time_us_t)((counter.QuadPart * 1000000LL) / frequency.QuadPart);
}

#elif defined(CANCESTRY_CLOCK_HAS_POSIX)

cancestry_time_us_t cancestry_platform_now_us(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0u;
    }
    return ((cancestry_time_us_t)ts.tv_sec * 1000000u) + (cancestry_time_us_t)(ts.tv_nsec / 1000L);
}

#else

/*
 * Bare-metal fallback. A platform port is expected to provide a strong
 * definition backed by a hardware timer tick.
 */
CANCESTRY_WEAK cancestry_time_us_t cancestry_platform_now_us(void)
{
    return 0u;
}

#endif

bool cancestry_clock_is_valid(const cancestry_clock_t *clock)
{
    return (clock != NULL) && (clock->now_us != NULL);
}

cancestry_time_us_t cancestry_clock_now_us(const cancestry_clock_t *clock)
{
    if (!cancestry_clock_is_valid(clock)) {
        return 0u;
    }
    return clock->now_us(clock->context);
}
