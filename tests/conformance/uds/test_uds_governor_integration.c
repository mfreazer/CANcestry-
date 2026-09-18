/*
 * CANcestry UDS conformance: governor integration (Phase 10, issue #26).
 *
 * Contract under test (docs/software/SwRS.md section 14.2 and
 * docs/system/governor.md):
 *
 *   - SW-FR-UDS-007: every WriteDataByIdentifier and RoutineControl side
 *     effect passes through the fail-closed governor stub before any effect
 *     happens. A NULL governor denies everything (SW-FR-GOV-006); a denial
 *     produces no partial effect, increments the violation counter
 *     (SW-FR-GOV-005) and is answered with NRC 0x22.
 *   - SW-FR-UDS-001: the UDS server composes with the ISO-TP transport
 *     engine: a reassembled request produces a segmented response.
 *
 * The governor installed here mirrors the FSM capability semantics
 * (cancestry_fsm_capabilities_t::signal_write): writes to read-only DIDs and
 * to signals outside the write allowlist are denied - the same policy shape
 * core/fsm enforces for its set_signal actions.
 *
 * Test ids: UDS-GOV-001 .. UDS-GOV-005.
 */

#include "uds_conformance_support.h"

#include "cancestry/transport/engine.h"

/* ------------------------------------------------------------------------- */
/* ISO-TP wiring for the full-stack cases                                    */
/* ------------------------------------------------------------------------- */

typedef struct uds_gov_frame {
    uint8_t data[CANCESTRY_TP_FRAME_MAX_LENGTH];
    uint8_t length;
} uds_gov_frame_t;

typedef struct uds_gov_stack {
    uds_test_env_t uds;
    cancestry_transport_t tp;
    uint8_t tp_rx_buffer[64];
    uint8_t tp_tx_buffer[64];
    cancestry_tp_sink_t tp_sink;
    uds_gov_frame_t frames[16];
    size_t frame_count;
} uds_gov_stack_t;

static bool uds_gov_on_frame_tx(void *user_data, const cancestry_tp_tx_frame_t *frame)
{
    uds_gov_stack_t *stack = (uds_gov_stack_t *)user_data;

    if (stack->frame_count < 16u && frame->length <= CANCESTRY_TP_FRAME_MAX_LENGTH) {
        uds_gov_frame_t *slot = &stack->frames[stack->frame_count];

        memcpy(slot->data, frame->data, (size_t)frame->length);
        slot->length = frame->length;
        stack->frame_count++;
    }
    return true;
}

/* The platform wiring: a reassembled request is processed by the UDS server
 * and the response is handed back to the transport for segmentation
 * (SW-FR-UDS-001). */
static void uds_gov_on_message(void *user_data, const uint8_t *data, size_t length)
{
    uds_gov_stack_t *stack = (uds_gov_stack_t *)user_data;
    uint8_t response[128];
    size_t response_length = 0u;
    cancestry_uds_status_t status;

    status = cancestry_uds_server_process(&stack->uds.server, data, length, response,
                                          sizeof(response), &response_length);
    /* Positive and negative responses alike go back on the bus; only a
     * local-capacity failure (ERR_CAPACITY) leaves nothing to send. */
    if (status != CANCESTRY_UDS_ERR_CAPACITY && response_length > 0u) {
        (void)cancestry_transport_send(&stack->tp, response, response_length);
    }
}

/** Bring up the full diagnostic stack (transport + UDS server). */
static void uds_gov_stack_init(uds_gov_stack_t *stack, size_t signal_slots,
                               bool with_governor)
{
    memset(stack, 0, sizeof(*stack));
    uds_test_env_init(&stack->uds, NULL, signal_slots, with_governor);

    stack->tp_sink.user_data = stack;
    stack->tp_sink.on_frame_tx = uds_gov_on_frame_tx;
    stack->tp_sink.on_message = uds_gov_on_message;
    CANCESSTRY_TEST_CHECK(cancestry_transport_init(
        &stack->tp, NULL, stack->tp_rx_buffer, sizeof(stack->tp_rx_buffer),
        stack->tp_tx_buffer, sizeof(stack->tp_tx_buffer), &stack->tp_sink, NULL, NULL));
}

/** Feed one classic CAN request frame into the stack. */
static void uds_gov_rx_frame(uds_gov_stack_t *stack, const uint8_t *data, uint8_t length)
{
    (void)cancestry_transport_rx_frame(&stack->tp, data, length);
}

/* ------------------------------------------------------------------------- */
/* Cases                                                                     */
/* ------------------------------------------------------------------------- */

/* UDS-GOV-001: a WriteDataByIdentifier request for a read-only DID is
 * blocked by the governor: NRC 0x22, violation counter, no partial effect,
 * and the governor actually saw the read-only entry (SW-FR-UDS-007). */
static void uds_gov_write_read_only_blocked(void)
{
    uds_test_env_t env;
    uint8_t request[20];
    uint8_t response[UDS_TEST_RESPONSE_MAX];
    size_t length = 0u;
    size_t i;

    uds_test_env_init(&env, NULL, UDS_TEST_SIGNAL_SLOTS_MAX, true);

    request[0] = 0x2Eu;
    request[1] = 0xF1u;
    request[2] = 0x90u;
    for (i = 3u; i < 20u; ++i) {
        request[i] = (uint8_t)(i);
    }
    CANCESSTRY_TEST_CHECK(uds_test_process(&env, request, 20u, response, &length) ==
                          CANCESTRY_UDS_ERR_DENIED);
    CANCESSTRY_TEST_CHECK(uds_test_is_nrc(response, length, 0x2Eu, 0x22u));

    /* The governor was consulted and saw the read-only entry. */
    CANCESSTRY_TEST_CHECK(env.policy.calls == 1u);
    CANCESSTRY_TEST_CHECK(env.policy.last_kind == CANCESTRY_UDS_GOVERNOR_WRITE_DID);
    CANCESSTRY_TEST_CHECK(env.policy.last_did == 0xF190u);
    CANCESSTRY_TEST_CHECK(env.policy.last_read_only);

    /* No partial effect: the DID store still serves the seeded default and
     * the violation counter was incremented (SW-FR-GOV-005). */
    CANCESSTRY_TEST_CHECK(env.did_data[0] == 0x57u);
    CANCESSTRY_TEST_CHECK(env.did_data[2] == 0x53u);
    CANCESSTRY_TEST_CHECK(env.server.counters.governor_denials == 1u);
    CANCESSTRY_TEST_CHECK(env.server.counters.nrc_conditions_not_correct == 1u);
    CANCESSTRY_TEST_CHECK(env.server.counters.did_writes == 0u);
    CANCESSTRY_TEST_CHECK(env.server.counters.positive_responses == 0u);

    uds_test_env_destroy(&env);
}

/* UDS-GOV-002: a WriteDataByIdentifier request whose target signal is
 * outside the FSM-style write allowlist is blocked; the same signal inside
 * the allowlist is approved and applied (SW-FR-UDS-007/008). */
static void uds_gov_signal_allowlist(void)
{
    static const char *const allowlist[] = {"diag.RpmTarget"};
    uds_test_env_t env;
    uint8_t request[5];
    uint8_t response[UDS_TEST_RESPONSE_MAX];
    size_t length = 0u;
    cancestry_value_t value;

    uds_test_env_init(&env, NULL, UDS_TEST_SIGNAL_SLOTS_MAX, true);
    env.policy.signal_write = allowlist;
    env.policy.signal_write_count = 1u;

    /* 0x0205 maps to diag.RpmActual, which is not in the allowlist. */
    request[0] = 0x2Eu;
    request[1] = 0x02u;
    request[2] = 0x05u;
    request[3] = 0x11u;
    request[4] = 0x22u;
    CANCESSTRY_TEST_CHECK(uds_test_process(&env, request, 5u, response, &length) ==
                          CANCESTRY_UDS_ERR_DENIED);
    CANCESSTRY_TEST_CHECK(uds_test_is_nrc(response, length, 0x2Eu, 0x22u));
    CANCESSTRY_TEST_CHECK(env.policy.last_has_signal);
    CANCESSTRY_TEST_CHECK(strcmp(env.policy.last_signal_name, "diag.RpmActual") == 0);
    CANCESSTRY_TEST_CHECK(env.server.counters.governor_denials == 1u);
    /* No effect: the slot still holds the seeded default. */
    CANCESSTRY_TEST_CHECK(env.did_data[env.config->dids[2].data_offset] == 0x00u);

    /* 0x0204 maps to diag.RpmTarget, which the allowlist permits. */
    request[1] = 0x02u;
    request[2] = 0x04u;
    CANCESSTRY_TEST_CHECK(uds_test_process(&env, request, 5u, response, &length) ==
                          CANCESTRY_UDS_OK);
    CANCESSTRY_TEST_CHECK(length == 3u);
    CANCESSTRY_TEST_CHECK(response[0] == 0x6Eu);
    CANCESSTRY_TEST_CHECK(cancestry_recipe_signal_store_get(
                              &env.signals, uds_test_signal_id(&env, "diag.RpmTarget"),
                              &value) == CANCESTRY_RECIPE_OK);
    CANCESSTRY_TEST_CHECK(value.value.unsigned_integer == 0x2211u);
    CANCESSTRY_TEST_CHECK(env.server.counters.governor_denials == 1u);

    uds_test_env_destroy(&env);
}

/* UDS-GOV-003: the fail-closed default. With no governor configured every
 * write and every routine execution is denied, even for writable DIDs
 * (SW-FR-GOV-006, SW-FR-UDS-007). */
static void uds_gov_null_governor_denies_all(void)
{
    uds_test_env_t env;
    uint8_t request[8];
    uint8_t response[UDS_TEST_RESPONSE_MAX];
    size_t length = 0u;

    uds_test_env_init(&env, NULL, UDS_TEST_SIGNAL_SLOTS_MAX, false);

    /* A writable, non-mapped DID is still denied. */
    request[0] = 0x2Eu;
    request[1] = 0x12u;
    request[2] = 0x34u;
    request[3] = 0x01u;
    request[4] = 0x02u;
    request[5] = 0x03u;
    request[6] = 0x04u;
    CANCESSTRY_TEST_CHECK(uds_test_process(&env, request, 7u, response, &length) ==
                          CANCESTRY_UDS_ERR_DENIED);
    CANCESSTRY_TEST_CHECK(uds_test_is_nrc(response, length, 0x2Eu, 0x22u));
    CANCESSTRY_TEST_CHECK(env.server.counters.governor_denials == 1u);
    CANCESSTRY_TEST_CHECK(env.did_data[env.config->dids[3].data_offset] == 0xDEu);

    /* Reads are not governed: the read still succeeds. */
    request[0] = 0x22u;
    CANCESSTRY_TEST_CHECK(uds_test_process(&env, request, 3u, response, &length) ==
                          CANCESTRY_UDS_OK);
    CANCESSTRY_TEST_CHECK(response[0] == 0x62u);
    CANCESSTRY_TEST_CHECK(env.server.counters.governor_denials == 1u);

    /* Routine execution is denied too. */
    request[0] = 0x31u;
    request[1] = 0x01u;
    request[2] = 0x02u;
    request[3] = 0x03u;
    CANCESSTRY_TEST_CHECK(uds_test_process(&env, request, 4u, response, &length) ==
                          CANCESTRY_UDS_ERR_DENIED);
    CANCESSTRY_TEST_CHECK(uds_test_is_nrc(response, length, 0x31u, 0x22u));
    CANCESSTRY_TEST_CHECK(env.server.counters.governor_denials == 2u);
    CANCESSTRY_TEST_CHECK(env.server.counters.routine_controls == 0u);

    uds_test_env_destroy(&env);
}

/* UDS-GOV-004: a routine execution denied by the governor reports NRC 0x22
 * and increments the violation counter (SW-FR-UDS-004/007). */
static void uds_gov_routine_denied(void)
{
    uds_test_env_t env;
    uint8_t request[4];
    uint8_t response[UDS_TEST_RESPONSE_MAX];
    size_t length = 0u;

    uds_test_env_init(&env, NULL, UDS_TEST_SIGNAL_SLOTS_MAX, true);
    env.policy.deny_routines = true;

    request[0] = 0x31u;
    request[1] = 0x01u;
    request[2] = 0x02u;
    request[3] = 0x03u;
    CANCESSTRY_TEST_CHECK(uds_test_process(&env, request, 4u, response, &length) ==
                          CANCESTRY_UDS_ERR_DENIED);
    CANCESSTRY_TEST_CHECK(uds_test_is_nrc(response, length, 0x31u, 0x22u));
    CANCESSTRY_TEST_CHECK(env.policy.calls == 1u);
    CANCESSTRY_TEST_CHECK(env.policy.last_kind == CANCESTRY_UDS_GOVERNOR_RUN_ROUTINE);
    CANCESSTRY_TEST_CHECK(env.policy.last_routine == 0x0203u);
    CANCESSTRY_TEST_CHECK(env.policy.last_subfunction == 0x01u);
    CANCESSTRY_TEST_CHECK(env.server.counters.governor_denials == 1u);
    CANCESSTRY_TEST_CHECK(env.server.counters.routine_controls == 0u);

    uds_test_env_destroy(&env);
}

/* UDS-GOV-005: the full stack. A single-frame ISO-TP request carrying a
 * WDBI for a read-only DID reassembles, is processed, and the negative
 * response is segmented back onto the bus; with the governor approving, the
 * same request shape yields the positive response (SW-FR-UDS-001/007). */
static void uds_gov_full_stack(void)
{
    uds_gov_stack_t stack;
    uint8_t frame[8];

    uds_gov_stack_init(&stack, UDS_TEST_SIGNAL_SLOTS_MAX, true);

    /* 2E D0 00 01 02 (5 bytes): a Single Frame request to write the
     * read-only 2-byte DID 0xD000. */
    frame[0] = 0x05u;
    frame[1] = 0x2Eu;
    frame[2] = 0xD0u;
    frame[3] = 0x00u;
    frame[4] = 0x01u;
    frame[5] = 0x02u;
    uds_gov_rx_frame(&stack, frame, 6u);

    /* The response 7F 2E 22 went out as one Single Frame. */
    CANCESSTRY_TEST_CHECK(stack.frame_count == 1u);
    CANCESSTRY_TEST_CHECK(stack.frames[0].length == 4u);
    CANCESSTRY_TEST_CHECK(stack.frames[0].data[0] == 0x03u);
    CANCESSTRY_TEST_CHECK(stack.frames[0].data[1] == 0x7Fu);
    CANCESSTRY_TEST_CHECK(stack.frames[0].data[2] == 0x2Eu);
    CANCESSTRY_TEST_CHECK(stack.frames[0].data[3] == 0x22u);
    CANCESSTRY_TEST_CHECK(stack.uds.server.counters.governor_denials == 1u);

    /* A WDBI for the writable 6-byte DID 0x1235 is 9 bytes (3 + 6), a
     * genuine multi-frame request: FF + CF, reassembled, approved, and the
     * 6E 12 35 response segmented back as a Single Frame. */
    {
        uint8_t ff[8];
        uint8_t cf[8];

        ff[0] = 0x10u;
        ff[1] = 0x09u; /* 9 bytes: 2E 12 35 01 02 03 04 05 06 */
        ff[2] = 0x2Eu;
        ff[3] = 0x12u;
        ff[4] = 0x35u;
        ff[5] = 0x01u;
        ff[6] = 0x02u;
        ff[7] = 0x03u;
        cf[0] = 0x21u; /* SN 1, final CF: 3 remaining bytes */
        cf[1] = 0x04u;
        cf[2] = 0x05u;
        cf[3] = 0x06u;
        uds_gov_rx_frame(&stack, ff, 8u);
        /* The engine answers the FF with FC CTS. */
        CANCESSTRY_TEST_CHECK(stack.frames[1].length == 3u);
        CANCESSTRY_TEST_CHECK(stack.frames[1].data[0] == 0x30u);
        uds_gov_rx_frame(&stack, cf, 4u);

        /* Response 6E 12 35 (3 bytes) as a Single Frame. */
        CANCESSTRY_TEST_CHECK(stack.frame_count == 3u);
        CANCESSTRY_TEST_CHECK(stack.frames[2].length == 4u);
        CANCESSTRY_TEST_CHECK(stack.frames[2].data[0] == 0x03u);
        CANCESSTRY_TEST_CHECK(stack.frames[2].data[1] == 0x6Eu);
        CANCESSTRY_TEST_CHECK(stack.frames[2].data[2] == 0x12u);
        CANCESSTRY_TEST_CHECK(stack.frames[2].data[3] == 0x35u);
        CANCESSTRY_TEST_CHECK(stack.uds.server.counters.did_writes == 1u);
        CANCESSTRY_TEST_CHECK(stack.uds.did_data[stack.uds.config->dids[5].data_offset] ==
                              0x01u);
        CANCESSTRY_TEST_CHECK(stack.uds.did_data[stack.uds.config->dids[5].data_offset + 5u] ==
                              0x06u);
    }

    uds_test_env_destroy(&stack.uds);
}

int main(void)
{
    CANCESSTRY_TEST_SUITE_BEGIN("uds governor integration conformance");
    CANCESSTRY_TEST_CASE("UDS-GOV-001 write to read-only DID blocked");
    uds_gov_write_read_only_blocked();
    CANCESSTRY_TEST_CASE("UDS-GOV-002 signal write allowlist");
    uds_gov_signal_allowlist();
    CANCESSTRY_TEST_CASE("UDS-GOV-003 NULL governor denies everything");
    uds_gov_null_governor_denies_all();
    CANCESSTRY_TEST_CASE("UDS-GOV-004 routine execution denied");
    uds_gov_routine_denied();
    CANCESSTRY_TEST_CASE("UDS-GOV-005 full transport + UDS loop");
    uds_gov_full_stack();
    return CANCESSTRY_TEST_SUITE_END();
}
