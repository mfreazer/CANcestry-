/*
 * QA-P31-01: retention persistence precedes reset escalation.
 *
 * Verifies SW-FR-LOG-004, SYS-SF-004 and SYS-FR-016: the terminal queue
 * saturation code is retained before the IWDG action can reset the MCU.
 *
 * Test id: FAULT-RETENTION-001.
 */

#include "cancestry/event/fault.h"

#include "cancestry_test.h"

#include <stdint.h>

typedef struct probe {
    uint32_t retention_code;
    uint32_t order[3u];
    uint32_t order_count;
} probe_t;

static void retain(uint32_t code, void *context)
{
    probe_t *probe = (probe_t *)context;
    probe->retention_code = code;
    if (probe->order_count < 3u) {
        probe->order[probe->order_count++] = 1u;
    }
}

static void safe(void *context)
{
    probe_t *probe = (probe_t *)context;
    if (probe->order_count < 3u) {
        probe->order[probe->order_count++] = 2u;
    }
}

static void watchdog(void *context)
{
    probe_t *probe = (probe_t *)context;
    if (probe->order_count < 3u) {
        probe->order[probe->order_count++] = 3u;
    }
}

int main(void)
{
    cancestry_event_hard_fault_hooks_t hooks;
    probe_t probe = {0u, {0u, 0u, 0u}, 0u};

    CANCESSTRY_TEST_SUITE_BEGIN("fault retention ordering");
    CANCESSTRY_TEST_CASE("FAULT-RETENTION-001: retention precedes the IWDG path");

    hooks.write_retention_register = retain;
    hooks.hal_fail_safe = safe;
    hooks.iwdg_escalate = watchdog;
    hooks.context = &probe;

    CANCESSTRY_TEST_CHECK(cancestry_event_hard_fault_escalate(&hooks));
    CANCESSTRY_TEST_CHECK_U64(probe.retention_code,
                              CANCESTRY_FAULT_CODE_QUEUE_SATURATION);
    CANCESSTRY_TEST_CHECK_U64(probe.order_count, 3u);
    CANCESSTRY_TEST_CHECK_U64(probe.order[0], 1u);
    CANCESSTRY_TEST_CHECK_U64(probe.order[1], 2u);
    CANCESSTRY_TEST_CHECK_U64(probe.order[2], 3u);
    CANCESSTRY_TEST_CHECK(probe.order[0] < probe.order[2]);

    return CANCESSTRY_TEST_SUITE_END();
}
