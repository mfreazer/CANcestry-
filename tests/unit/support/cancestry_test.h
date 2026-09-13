/*
 * Minimal header-only unit test harness for the CANcestry core.
 *
 * Deliberately dependency-free: the repository does not vendor a test
 * framework yet, and the host unit-test gate must run with nothing but a C99
 * compiler. Each test file is its own executable and returns a non-zero exit
 * code when any check fails, which is what CTest uses to pass or fail.
 *
 * Usage:
 *     int main(void) {
 *         CANCESSTRY_TEST_SUITE_BEGIN("suite name");
 *         CANCESSTRY_TEST_CASE("case name");
 *         CANCESSTRY_TEST_CHECK(condition);
 *         return CANCESSTRY_TEST_SUITE_END();
 *     }
 */

#ifndef CANCESSTRY_TEST_H
#define CANCESSTRY_TEST_H

#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

/*
 * Compile-time assertion usable at file scope; C99 has no _Static_assert and
 * -Wpedantic rejects repeated typedef names, so the name is made unique per
 * line through a two-level paste.
 */
#define CANCESSTRY_TEST_CONCAT_(left, right) left##right
#define CANCESSTRY_TEST_CONCAT(left, right) CANCESSTRY_TEST_CONCAT_(left, right)
#define CANCESSTRY_TEST_STATIC_ASSERT(cond) \
    typedef char CANCESSTRY_TEST_CONCAT(cancestry_test_static_assert_, __LINE__)[(cond) ? 1 : -1]

static const char *cancestry_test_suite = "";
static const char *cancestry_test_case = "";
static unsigned int cancestry_test_checks = 0u;
static unsigned int cancestry_test_failures = 0u;

static inline void cancestry_test_suite_begin(const char *name)
{
    cancestry_test_suite = name;
    printf("[ SUITE ] %s\n", name);
    fflush(stdout);
}

static inline void cancestry_test_case_begin(const char *name)
{
    cancestry_test_case = name;
    printf("  [ CASE ] %s\n", name);
    fflush(stdout);
}

static inline void cancestry_test_report_failure(const char *file, int line, const char *message)
{
    cancestry_test_failures++;
    printf("    FAIL %s:%d (%s) %s\n", file, line, cancestry_test_case, message);
    fflush(stdout);
}

static inline bool cancestry_test_check_impl(bool ok, const char *expr, const char *file, int line)
{
    cancestry_test_checks++;
    if (!ok) {
        cancestry_test_report_failure(file, line, expr);
    }
    return ok;
}

static inline void cancestry_test_check_u64(uint64_t actual,
                                            uint64_t expected,
                                            const char *expr,
                                            const char *file,
                                            int line)
{
    cancestry_test_checks++;
    if (actual != expected) {
        char message[192];
        (void)snprintf(message,
                       sizeof(message),
                       "%s (expected %" PRIu64 ", got %" PRIu64 ")",
                       expr,
                       expected,
                       actual);
        cancestry_test_report_failure(file, line, message);
    }
}

static inline void cancestry_test_check_i64(int64_t actual,
                                            int64_t expected,
                                            const char *expr,
                                            const char *file,
                                            int line)
{
    cancestry_test_checks++;
    if (actual != expected) {
        char message[192];
        (void)snprintf(message,
                       sizeof(message),
                       "%s (expected %" PRId64 ", got %" PRId64 ")",
                       expr,
                       expected,
                       actual);
        cancestry_test_report_failure(file, line, message);
    }
}

static inline void cancestry_test_check_double(double actual,
                                               double expected,
                                               double tolerance,
                                               const char *expr,
                                               const char *file,
                                               int line)
{
    cancestry_test_checks++;
    if (!((actual >= expected - tolerance) && (actual <= expected + tolerance))) {
        char message[192];
        (void)snprintf(message, sizeof(message), "%s (expected %f, got %f)", expr, expected, actual);
        cancestry_test_report_failure(file, line, message);
    }
}

static inline void cancestry_test_check_string(const char *actual,
                                               const char *expected,
                                               const char *expr,
                                               const char *file,
                                               int line)
{
    cancestry_test_checks++;
    if (actual == NULL || expected == NULL || strcmp(actual, expected) != 0) {
        char message[256];
        (void)snprintf(message,
                       sizeof(message),
                       "%s (expected \"%s\", got \"%s\")",
                       expr,
                       (expected != NULL) ? expected : "(null)",
                       (actual != NULL) ? actual : "(null)");
        cancestry_test_report_failure(file, line, message);
    }
}

static inline int cancestry_test_suite_end(void)
{
    printf("[ %s ] %s: %u checks, %u failures\n",
           (cancestry_test_failures == 0u) ? "PASS" : "FAIL",
           cancestry_test_suite,
           cancestry_test_checks,
           cancestry_test_failures);
    fflush(stdout);
    return (cancestry_test_failures == 0u) ? 0 : 1;
}

#define CANCESSTRY_TEST_SUITE_BEGIN(name) cancestry_test_suite_begin(name)
#define CANCESSTRY_TEST_CASE(name) cancestry_test_case_begin(name)
#define CANCESSTRY_TEST_SUITE_END() cancestry_test_suite_end()

#define CANCESSTRY_TEST_CHECK(cond) \
    (void)cancestry_test_check_impl((cond) ? true : false, #cond, __FILE__, __LINE__)

#define CANCESSTRY_TEST_CHECK_U64(actual, expected) \
    cancestry_test_check_u64((uint64_t)(actual), (uint64_t)(expected), #actual " == " #expected, __FILE__, __LINE__)

#define CANCESSTRY_TEST_CHECK_I64(actual, expected) \
    cancestry_test_check_i64((int64_t)(actual), (int64_t)(expected), #actual " == " #expected, __FILE__, __LINE__)

#define CANCESSTRY_TEST_CHECK_DOUBLE(actual, expected, tolerance)                                 \
    cancestry_test_check_double((double)(actual), (double)(expected), (double)(tolerance),        \
                                #actual " ~= " #expected, __FILE__, __LINE__)

#define CANCESSTRY_TEST_CHECK_STRING(actual, expected) \
    cancestry_test_check_string((actual), (expected), #actual " == " #expected, __FILE__, __LINE__)

#endif /* CANCESSTRY_TEST_H */
