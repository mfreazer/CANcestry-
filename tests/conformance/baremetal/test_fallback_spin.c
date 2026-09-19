/*
 * QA-P31-03: incomplete escalation configuration is terminal.
 *
 * Verifies SYS-SF-004 and SYS-FR-016: NULL or incomplete hard-fault hooks do
 * not return a false status and cannot resume ordinary execution; the process
 * remains in the non-returning safe-spin path.
 *
 * Test id: FALLBACK-SPIN-001.
 */

#define _POSIX_C_SOURCE 200809L

#include "cancestry/event/fault.h"

#include "cancestry_test.h"

#include <signal.h>
#include <stdbool.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static void unused_retention(uint32_t code, void *context)
{
    (void)code;
    (void)context;
}

static void unused_safe(void *context)
{
    (void)context;
}

static void unused_watchdog(void *context)
{
    (void)context;
}

static void assert_child_spins(const cancestry_event_hard_fault_hooks_t *hooks)
{
    pid_t child;
    int status = 0;
    bool finished = false;
    uint32_t tick;

    child = fork();
    CANCESSTRY_TEST_CHECK(child >= 0);
    if (child == 0) {
        (void)cancestry_event_hard_fault_escalate(hooks);
        _exit(77);
    }

    for (tick = 0u; tick < 100u; ++tick) {
        pid_t result = waitpid(child, &status, WNOHANG);
        if (result == child) {
            finished = true;
            break;
        }
        CANCESSTRY_TEST_CHECK(result == 0);
        {
            const struct timespec delay = {0, 1000000L};
            (void)nanosleep(&delay, NULL);
        }
    }

    /* A returning child would indicate that the fallback was not terminal. */
    CANCESSTRY_TEST_CHECK(!finished);
    if (!finished) {
        CANCESSTRY_TEST_CHECK(kill(child, SIGKILL) == 0);
        CANCESSTRY_TEST_CHECK(waitpid(child, &status, 0) == child);
        CANCESSTRY_TEST_CHECK(WIFSIGNALED(status));
        CANCESSTRY_TEST_CHECK_U64(WTERMSIG(status), SIGKILL);
    }
}

int main(void)
{
    cancestry_event_hard_fault_hooks_t incomplete;

    CANCESSTRY_TEST_SUITE_BEGIN("hard-fault fallback spin");
    CANCESSTRY_TEST_CASE("FALLBACK-SPIN-001: missing hooks never return");

    assert_child_spins(NULL);

    incomplete.write_retention_register = unused_retention;
    incomplete.hal_fail_safe = NULL;
    incomplete.iwdg_escalate = unused_watchdog;
    incomplete.context = NULL;
    assert_child_spins(&incomplete);

    incomplete.write_retention_register = unused_retention;
    incomplete.hal_fail_safe = unused_safe;
    incomplete.iwdg_escalate = NULL;
    assert_child_spins(&incomplete);

    return CANCESSTRY_TEST_SUITE_END();
}
