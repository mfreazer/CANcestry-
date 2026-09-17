/*
 * CANcestry FSM conformance: CAN FD messages on the declarative egress path.
 *
 * The FSM send_message action builds its frame on the classic 8-byte path
 * (SW-FR-CANFD-006). A message from a CAN FD codec map therefore has to be
 * refused instead of truncated: the engine records an action error, the sink
 * never sees a frame, and the instance keeps running so the rest of the
 * transition's actions still execute (fail closed, but not fatal).
 *
 * The refusal must be attributable to the frame width and not to the usual
 * capability gate, so the test declares 0x400 in can0's TX allowlist first.
 *
 * Implements: SW-FR-CANFD-006
 * Test ids:    FSM-CANFD-EGRESS-001
 */

#include "cancestry_fsm_conformance.h"

static const char *const fd_egress_yaml =
    "schema_version: \"0.2.0\"\n"
    "state_machines:\n"
    "  - name: lab\n"
    "    initial: S0\n"
    "    states:\n"
    "      - name: S0\n"
    "        transitions:\n"
    "          - event: signal_changed\n"
    "            signal: SendFd\n"
    "            target: S0\n"
    "            actions:\n"
    "              - send_message:\n"
    "                  interface: can0\n"
    "                  message: FdMsg\n"
    "                  signals:\n"
    "                    FdTail: 5\n"
    "              - log:\n"
    "                  level: info\n"
    "                  message: after send\n"
    "          - event: signal_changed\n"
    "            signal: SendClassic\n"
    "            target: S0\n"
    "            actions:\n"
    "              - send_message:\n"
    "                  interface: can0\n"
    "                  message: StatusMsg\n"
    "                  signals:\n"
    "                    VehicleSpeed: 42\n"
    "      - name: S1\n"
    "instances:\n"
    "  - id: lab.one\n"
    "    machine: lab\n"
    "    enabled: true\n";

/* FSM-CANFD-EGRESS-001: an FD message is refused, a classic one still goes. */
static void case_fd_message_is_refused(void)
{
    static fsm_test_fixture_t fx;
    fsm_test_options_t options = fsm_test_options_default();
    cancestry_event_t event;

    CANCESSTRY_TEST_CASE("FSM-CANFD-EGRESS-001 an FD message is refused, not truncated");
    CANCESSTRY_TEST_CHECK(
        fsm_test_init_codec(&fx, fd_egress_yaml, &options, fsm_test_codec_fd_yaml()));
    /* Put FdMsg (0x400) in can0's declared TX allowlist so the refusal can
     * only come from the frame width. */
    fx.can0_tx_ids[0] = FSM_TEST_MSG_STATUS;
    fx.can0_tx_ids[1] = FSM_TEST_MSG_FD;
    CANCESSTRY_TEST_CHECK_U64(fsm_test_start(&fx, "lab.one"), CANCESTRY_FSM_OK);

    fsm_test_reset(&fx);
    fsm_test_event_int(&event, 1000u, "SendFd", 1);
    CANCESSTRY_TEST_CHECK_U64(fsm_test_process(&fx, &event), CANCESTRY_FSM_OK);
    CANCESSTRY_TEST_CHECK(!fsm_test_has_line(&fx, "tx "));
    CANCESSTRY_TEST_CHECK_U64(fsm_test_counters(&fx, "lab.one")->messages_sent, 0u);
    CANCESSTRY_TEST_CHECK_U64(fsm_test_counters(&fx, "lab.one")->capability_denials, 0u);
    CANCESSTRY_TEST_CHECK_U64(fsm_test_counters(&fx, "lab.one")->action_errors, 1u);
    /* The rest of the action sequence still runs, and the instance survives. */
    CANCESSTRY_TEST_CHECK(fsm_test_has_line(&fx, "log 1 info after send"));
    CANCESSTRY_TEST_CHECK_U64(fsm_test_lifecycle(&fx, "lab.one"),
                              CANCESTRY_FSM_INSTANCE_LIFECYCLE_RUNNING);

    /* A classic message on the same interface and map is unaffected. */
    cancestry_event_init(&event);
    fsm_test_reset(&fx);
    fsm_test_event_int(&event, 2000u, "SendClassic", 1);
    CANCESSTRY_TEST_CHECK_U64(fsm_test_process(&fx, &event), CANCESTRY_FSM_OK);
    CANCESSTRY_TEST_CHECK(fsm_test_has_line(&fx, "tx "));
    CANCESSTRY_TEST_CHECK_U64(fsm_test_counters(&fx, "lab.one")->messages_sent, 1u);

    fsm_test_destroy(&fx);
}

int main(void)
{
    CANCESSTRY_TEST_SUITE_BEGIN("FSM CAN FD egress");
    case_fd_message_is_refused();
    return CANCESSTRY_TEST_SUITE_END();
}
