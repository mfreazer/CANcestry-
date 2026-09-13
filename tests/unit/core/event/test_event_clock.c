/*
 * Unit tests for the monotonic clock abstraction.
 *
 * Verifies:
 *   SYS-IR-005       The system shall provide a monotonic time source for
 *                    timers and event timestamps.
 *   SW-FR-EVENT-002  The software shall support event timestamps.
 *   SYS-NF-001       Simulation uses a deterministic virtual monotonic clock.
 *
 * Normative source: docs/system/event-ordering.md section 2.
 */

#include "cancestry/event/clock.h"

#include "cancestry_test.h"

static void test_virtual_clock_is_deterministic(void)
{
    cancestry_virtual_clock_t vclock;
    cancestry_clock_t clock;

    cancestry_virtual_clock_init(&vclock, 5000u);
    CANCESSTRY_TEST_CHECK_U64(cancestry_virtual_clock_now(&vclock), 5000u);

    /* Reading does not advance the clock. */
    CANCESSTRY_TEST_CHECK_U64(cancestry_virtual_clock_now(&vclock), 5000u);

    cancestry_virtual_clock_advance(&vclock, 100u);
    CANCESSTRY_TEST_CHECK_U64(cancestry_virtual_clock_now(&vclock), 5100u);
    cancestry_virtual_clock_advance(&vclock, 0u);
    CANCESSTRY_TEST_CHECK_U64(cancestry_virtual_clock_now(&vclock), 5100u);

    /* The same script always produces the same timestamps. */
    clock = cancestry_clock_from_virtual(&vclock);
    CANCESSTRY_TEST_CHECK(cancestry_clock_is_valid(&clock));
    CANCESSTRY_TEST_CHECK_U64(cancestry_clock_now_us(&clock), 5100u);
    CANCESSTRY_TEST_CHECK_U64(cancestry_clock_now_us(&clock), 5100u);

    cancestry_virtual_clock_advance(&vclock, 1u);
    CANCESSTRY_TEST_CHECK_U64(cancestry_clock_now_us(&clock), 5101u);
}

static void test_virtual_clock_is_monotonic(void)
{
    cancestry_virtual_clock_t vclock;

    cancestry_virtual_clock_init(&vclock, 0u);
    CANCESSTRY_TEST_CHECK(cancestry_virtual_clock_set(&vclock, 1000u));
    CANCESSTRY_TEST_CHECK_U64(cancestry_virtual_clock_now(&vclock), 1000u);

    /* Time may stand still, but never move backwards. */
    CANCESSTRY_TEST_CHECK(cancestry_virtual_clock_set(&vclock, 1000u));
    CANCESSTRY_TEST_CHECK(!cancestry_virtual_clock_set(&vclock, 999u));
    CANCESSTRY_TEST_CHECK_U64(cancestry_virtual_clock_now(&vclock), 1000u);

    cancestry_virtual_clock_advance(&vclock, CANCESTRY_TIME_US_MAX - 1000u);
    CANCESSTRY_TEST_CHECK_U64(cancestry_virtual_clock_now(&vclock), CANCESTRY_TIME_US_MAX);
    /* Advancing past the end of time saturates instead of wrapping. */
    cancestry_virtual_clock_advance(&vclock, 1u);
    CANCESSTRY_TEST_CHECK_U64(cancestry_virtual_clock_now(&vclock), CANCESTRY_TIME_US_MAX);
}

static void test_clock_argument_validation(void)
{
    cancestry_clock_t null_clock;
    cancestry_clock_t from_null_virtual;

    null_clock.now_us = NULL;
    null_clock.context = NULL;

    CANCESSTRY_TEST_CHECK(!cancestry_clock_is_valid(NULL));
    CANCESSTRY_TEST_CHECK(!cancestry_clock_is_valid(&null_clock));
    CANCESSTRY_TEST_CHECK_U64(cancestry_clock_now_us(NULL), 0u);
    CANCESSTRY_TEST_CHECK_U64(cancestry_clock_now_us(&null_clock), 0u);

    /* NULL handling for the virtual clock helpers. */
    cancestry_virtual_clock_init(NULL, 0u);
    cancestry_virtual_clock_advance(NULL, 10u);
    CANCESSTRY_TEST_CHECK(!cancestry_virtual_clock_set(NULL, 10u));
    CANCESSTRY_TEST_CHECK_U64(cancestry_virtual_clock_now(NULL), 0u);

    from_null_virtual = cancestry_clock_from_virtual(NULL);
    CANCESSTRY_TEST_CHECK(cancestry_clock_is_valid(&from_null_virtual));
    CANCESSTRY_TEST_CHECK_U64(cancestry_clock_now_us(&from_null_virtual), 0u);
}

static void test_timestamp_arithmetic_saturates(void)
{
    CANCESSTRY_TEST_CHECK_U64(cancestry_time_sat_add(0u, 0u), 0u);
    CANCESSTRY_TEST_CHECK_U64(cancestry_time_sat_add(100u, 250u), 350u);
    CANCESSTRY_TEST_CHECK_U64(cancestry_time_sat_add(CANCESTRY_TIME_US_MAX, 0u),
                              CANCESTRY_TIME_US_MAX);
    CANCESSTRY_TEST_CHECK_U64(cancestry_time_sat_add(CANCESTRY_TIME_US_MAX, 1u),
                              CANCESTRY_TIME_US_MAX);
    CANCESSTRY_TEST_CHECK_U64(cancestry_time_sat_add(CANCESTRY_TIME_US_MAX - 10u, 25u),
                              CANCESTRY_TIME_US_MAX);
}

static void test_platform_clock_is_monotonic(void)
{
    cancestry_clock_t clock = cancestry_clock_platform();
    cancestry_time_us_t first;
    cancestry_time_us_t second;

    CANCESSTRY_TEST_CHECK(cancestry_clock_is_valid(&clock));

    first = cancestry_clock_now_us(&clock);
    second = cancestry_clock_now_us(&clock);
    CANCESSTRY_TEST_CHECK(second >= first);

    /* The platform clock is a separate source from any virtual clock. */
    CANCESSTRY_TEST_CHECK(cancestry_clock_now_us(&clock) == cancestry_platform_now_us() ||
                          cancestry_clock_now_us(&clock) >= cancestry_platform_now_us());
}

int main(void)
{
    CANCESSTRY_TEST_SUITE_BEGIN("event clock");

    CANCESSTRY_TEST_CASE("virtual clock is deterministic");
    test_virtual_clock_is_deterministic();

    CANCESSTRY_TEST_CASE("virtual clock is monotonic and saturates");
    test_virtual_clock_is_monotonic();

    CANCESSTRY_TEST_CASE("clock argument validation");
    test_clock_argument_validation();

    CANCESSTRY_TEST_CASE("timestamp arithmetic saturates");
    test_timestamp_arithmetic_saturates();

    CANCESSTRY_TEST_CASE("platform clock is monotonic");
    test_platform_clock_is_monotonic();

    return CANCESSTRY_TEST_SUITE_END();
}
