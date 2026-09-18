/*
 * CANcestry ISO-TP conformance: transmission segmentation (Phase 10, issue #26).
 *
 * Contract under test (docs/software/SwRS.md section 14.1):
 *
 *   - SW-FR-TP-002: Flow Control frames received from the peer are decoded
 *     (CTS / WT / OVFLW, Block Size, STmin).
 *   - SW-FR-TP-006: the sender N_Bs timer (until Flow Control) and the N_As
 *     retransmission bound are tick-driven and abort with TRANSPORT_TIMEOUT.
 *   - SW-FR-TP-007: payloads of at most 7 bytes go out as one Single Frame;
 *     larger payloads as a First Frame plus Consecutive Frames with the
 *     rolling sequence number, up to the 4095-byte FF limit and the
 *     caller's static TX buffer.
 *   - SW-FR-TP-008: after the First Frame and after every block of BS
 *     Consecutive Frames the sender waits for Flow Control; STmin (ms and
 *     100-900 us encodings, 1 ms tick granularity) separates Consecutive
 *     Frames; OVFLW aborts; more than the configured Wait frames aborts.
 *   - SW-FR-TP-009: a frame the platform sink declines is retried on the
 *     next ticks until N_As expires; nothing is silently dropped.
 *   - SW-FR-TP-005: a Flow Control frame that cannot apply in the current
 *     state is a protocol fault.
 *
 * Test ids: TP-TX-001 .. TP-TX-009.
 */

#include "iso_tp_fixture.h"

/** A sink that accepts every frame and records nothing (for validation cases). */
static bool tp_tx_accept_always(void *user_data, const cancestry_tp_tx_frame_t *frame)
{
    (void)user_data;
    (void)frame;
    return true;
}

/** @return true when captured frame @p index is a CF with sequence number @p sn. */
static bool tp_tx_cf_is(const tp_test_env_t *env, size_t index, uint8_t sn)
{
    return index < env->frames.count &&
           env->frames.frames[index].kind == CANCESTRY_TP_TX_FRAME_CF &&
           (env->frames.frames[index].data[0] & 0x0Fu) == (uint8_t)(sn & 0x0Fu);
}

/* TP-TX-001: a payload of at most 7 bytes is emitted as one Single Frame,
 * synchronously with the send request (SW-FR-TP-007). */
static void tp_tx_single_frame(void)
{
    tp_test_env_t env;
    const uint8_t payload[4] = {0x62u, 0xF1u, 0x90u, 0x01u};

    tp_test_env_init(&env, NULL);
    CANCESSTRY_TEST_CHECK(cancestry_transport_send(&env.tp, payload, 4u) == CANCESTRY_TP_OK);
    CANCESSTRY_TEST_CHECK(env.frames.count == 1u);
    CANCESSTRY_TEST_CHECK(env.frames.frames[0].kind == CANCESTRY_TP_TX_FRAME_SF);
    CANCESSTRY_TEST_CHECK(env.frames.frames[0].length == 5u);
    CANCESSTRY_TEST_CHECK(env.frames.frames[0].data[0] == 0x04u);
    CANCESSTRY_TEST_CHECK(memcmp(&env.frames.frames[0].data[1], payload, 4u) == 0);
    CANCESSTRY_TEST_CHECK(!cancestry_transport_tx_busy(&env.tp));
    CANCESSTRY_TEST_CHECK(env.tp.counters.tx_messages_requested == 1u);
    CANCESSTRY_TEST_CHECK(env.tp.counters.tx_messages_completed == 1u);

    /* Boundary: 7 bytes is still a Single Frame, 8 needs a First Frame. */
    {
        const uint8_t seven[7] = {0u};

        CANCESSTRY_TEST_CHECK(cancestry_transport_send(&env.tp, seven, 7u) ==
                              CANCESTRY_TP_OK);
        CANCESSTRY_TEST_CHECK(env.frames.frames[1].kind == CANCESTRY_TP_TX_FRAME_SF);
        CANCESSTRY_TEST_CHECK(env.frames.frames[1].data[0] == 0x07u);
        {
            const uint8_t eight[8] = {0u};

            CANCESSTRY_TEST_CHECK(cancestry_transport_send(&env.tp, eight, 8u) ==
                                  CANCESTRY_TP_OK);
            CANCESSTRY_TEST_CHECK(env.frames.frames[2].kind == CANCESTRY_TP_TX_FRAME_FF);
            CANCESSTRY_TEST_CHECK(cancestry_transport_tx_busy(&env.tp));
        }
    }
}

/* TP-TX-002: a 20-byte message is segmented FF + CF1 + CF2; Flow Control CTS
 * with BS 0 releases the whole burst on the next tick, sequence numbers
 * start at 1 and roll 15 -> 0 (SW-FR-TP-002, SW-FR-TP-007, SW-FR-TP-008). */
static void tp_tx_multi_frame_burst(void)
{
    tp_test_env_t env;
    uint8_t payload[20];
    uint8_t i;

    tp_test_env_init(&env, NULL);
    for (i = 0u; i < 20u; ++i) {
        payload[i] = (uint8_t)(0xE0u + i);
    }

    CANCESSTRY_TEST_CHECK(cancestry_transport_send(&env.tp, payload, 20u) == CANCESTRY_TP_OK);
    CANCESSTRY_TEST_CHECK(env.frames.count == 1u);
    CANCESSTRY_TEST_CHECK(env.frames.frames[0].kind == CANCESTRY_TP_TX_FRAME_FF);
    CANCESSTRY_TEST_CHECK(env.frames.frames[0].length == 8u);
    CANCESSTRY_TEST_CHECK(env.frames.frames[0].data[0] == 0x10u);
    CANCESSTRY_TEST_CHECK(env.frames.frames[0].data[1] == 0x14u);
    CANCESSTRY_TEST_CHECK(memcmp(&env.frames.frames[0].data[2], payload, 6u) == 0);
    CANCESSTRY_TEST_CHECK(cancestry_transport_tx_state(&env.tp) ==
                          CANCESTRY_TP_STATE_FLOW_CONTROL);

    /* Nothing moves without Flow Control. */
    CANCESSTRY_TEST_CHECK(tp_test_advance(&env, 3u) == CANCESTRY_TP_OK);
    CANCESSTRY_TEST_CHECK(env.frames.count == 1u);

    /* CTS, BS 0 (no block limit), STmin 0: the burst completes on one tick. */
    CANCESSTRY_TEST_CHECK(tp_test_rx_fc(&env, 0x0u, 0u, 0u) == CANCESTRY_TP_OK);
    CANCESSTRY_TEST_CHECK(cancestry_transport_tx_state(&env.tp) ==
                          CANCESTRY_TP_STATE_CONSECUTIVE_FRAME);
    CANCESSTRY_TEST_CHECK(tp_test_advance(&env, 1u) == CANCESTRY_TP_OK);

    CANCESSTRY_TEST_CHECK(env.frames.count == 3u);
    CANCESSTRY_TEST_CHECK(tp_tx_cf_is(&env, 1u, 1u));
    CANCESSTRY_TEST_CHECK(tp_tx_cf_is(&env, 2u, 2u));
    CANCESSTRY_TEST_CHECK(env.frames.frames[1].length == 8u);
    CANCESSTRY_TEST_CHECK(memcmp(&env.frames.frames[1].data[1], &payload[6], 7u) == 0);
    CANCESSTRY_TEST_CHECK(env.frames.frames[2].length == 8u);
    CANCESSTRY_TEST_CHECK(memcmp(&env.frames.frames[2].data[1], &payload[13], 7u) == 0);
    CANCESSTRY_TEST_CHECK(!cancestry_transport_tx_busy(&env.tp));
    CANCESSTRY_TEST_CHECK(env.tp.counters.tx_messages_completed == 1u);

    /* Sequence numbers roll over for a long message: 120 bytes need 17 CFs
     * (SN 1..15, 0, 1). */
    {
        uint8_t big[120];
        size_t cf_index;
        uint8_t sn;

        for (i = 0u; i < 120u; ++i) {
            big[i] = (uint8_t)(i);
        }
        CANCESSTRY_TEST_CHECK(cancestry_transport_send(&env.tp, big, 120u) == CANCESTRY_TP_OK);
        CANCESSTRY_TEST_CHECK(tp_test_rx_fc(&env, 0x0u, 0u, 0u) == CANCESTRY_TP_OK);
        CANCESSTRY_TEST_CHECK(tp_test_advance(&env, 1u) == CANCESTRY_TP_OK);
        CANCESSTRY_TEST_CHECK(env.frames.count == 3u + 1u + 17u);
        sn = 1u;
        for (cf_index = 0u; cf_index < 17u; ++cf_index) {
            CANCESSTRY_TEST_CHECK(tp_tx_cf_is(&env, 4u + cf_index, sn));
            sn = (uint8_t)((sn + 1u) & 0x0Fu);
        }
        CANCESSTRY_TEST_CHECK(env.tp.counters.tx_messages_completed == 2u);
    }
}

/* TP-TX-003: Block Size. With BS 1 the sender emits one CF per Flow Control
 * frame and waits in FLOW_CONTROL between blocks (SW-FR-TP-008). */
static void tp_tx_block_size(void)
{
    tp_test_env_t env;
    uint8_t payload[20];
    uint8_t i;

    tp_test_env_init(&env, NULL);
    for (i = 0u; i < 20u; ++i) {
        payload[i] = (uint8_t)(0x10u + i);
    }

    CANCESSTRY_TEST_CHECK(cancestry_transport_send(&env.tp, payload, 20u) == CANCESTRY_TP_OK);
    CANCESSTRY_TEST_CHECK(tp_test_rx_fc(&env, 0x0u, 1u, 0u) == CANCESTRY_TP_OK);

    /* One CF, then the block is exhausted: back to waiting for FC. */
    CANCESSTRY_TEST_CHECK(tp_test_advance(&env, 1u) == CANCESTRY_TP_OK);
    CANCESSTRY_TEST_CHECK(env.frames.count == 2u); /* FF + CF1 */
    CANCESSTRY_TEST_CHECK(tp_tx_cf_is(&env, 1u, 1u));
    CANCESSTRY_TEST_CHECK(cancestry_transport_tx_state(&env.tp) ==
                          CANCESTRY_TP_STATE_FLOW_CONTROL);

    /* Time passes: without the next FC nothing is sent. */
    CANCESSTRY_TEST_CHECK(tp_test_advance(&env, 3u) == CANCESTRY_TP_OK);
    CANCESSTRY_TEST_CHECK(env.frames.count == 2u);

    /* Second FC releases the final CF. */
    CANCESSTRY_TEST_CHECK(tp_test_rx_fc(&env, 0x0u, 1u, 0u) == CANCESTRY_TP_OK);
    CANCESSTRY_TEST_CHECK(tp_test_advance(&env, 1u) == CANCESTRY_TP_OK);
    CANCESSTRY_TEST_CHECK(env.frames.count == 3u);
    CANCESSTRY_TEST_CHECK(tp_tx_cf_is(&env, 2u, 2u));
    CANCESSTRY_TEST_CHECK(!cancestry_transport_tx_busy(&env.tp));
    CANCESSTRY_TEST_CHECK(env.tp.counters.tx_messages_completed == 1u);
    CANCESSTRY_TEST_CHECK(env.tp.counters.cf_tx == 2u);

    /* BS 2 on a 27-byte message (FF 6 + CF 7 + CF 7 + CF 7): two CFs per FC. */
    {
        uint8_t bigger[27];

        for (i = 0u; i < 27u; ++i) {
            bigger[i] = (uint8_t)(0x40u + i);
        }
        CANCESSTRY_TEST_CHECK(cancestry_transport_send(&env.tp, bigger, 27u) ==
                              CANCESTRY_TP_OK);
        CANCESSTRY_TEST_CHECK(tp_test_rx_fc(&env, 0x0u, 2u, 0u) == CANCESTRY_TP_OK);
        CANCESSTRY_TEST_CHECK(tp_test_advance(&env, 1u) == CANCESTRY_TP_OK);
        CANCESSTRY_TEST_CHECK(env.frames.count == 6u); /* + CF1, CF2 */
        CANCESSTRY_TEST_CHECK(cancestry_transport_tx_state(&env.tp) ==
                              CANCESTRY_TP_STATE_FLOW_CONTROL);
        CANCESSTRY_TEST_CHECK(tp_test_rx_fc(&env, 0x0u, 2u, 0u) == CANCESTRY_TP_OK);
        CANCESSTRY_TEST_CHECK(tp_test_advance(&env, 1u) == CANCESTRY_TP_OK);
        CANCESSTRY_TEST_CHECK(env.frames.count == 7u); /* + CF3 (final) */
        CANCESSTRY_TEST_CHECK(env.tp.counters.tx_messages_completed == 2u);
    }
}

/* TP-TX-004: STmin. A millisecond STmin separates CFs by whole ticks; a
 * 100 us STmin rounds up to one tick (SW-FR-TP-008). */
static void tp_tx_stmin_pacing(void)
{
    uint8_t payload[20];
    uint8_t i;

    for (i = 0u; i < 20u; ++i) {
        payload[i] = (uint8_t)(0x70u + i);
    }

    /* STmin 3 ms: CF1 on tick 1, CF2 on tick 4. */
    {
        tp_test_env_t env;

        tp_test_env_init(&env, NULL);
        CANCESSTRY_TEST_CHECK(cancestry_transport_send(&env.tp, payload, 20u) ==
                              CANCESTRY_TP_OK);
        CANCESSTRY_TEST_CHECK(tp_test_rx_fc(&env, 0x0u, 0u, 3u) == CANCESTRY_TP_OK);
        CANCESSTRY_TEST_CHECK(tp_test_advance(&env, 1u) == CANCESTRY_TP_OK);
        CANCESSTRY_TEST_CHECK(env.frames.count == 2u); /* FF + CF1 */
        CANCESSTRY_TEST_CHECK(tp_test_advance(&env, 2u) == CANCESTRY_TP_OK);
        CANCESSTRY_TEST_CHECK(env.frames.count == 2u); /* still waiting */
        CANCESSTRY_TEST_CHECK(tp_test_advance(&env, 1u) == CANCESTRY_TP_OK);
        CANCESSTRY_TEST_CHECK(env.frames.count == 3u); /* CF2 at tick 4 */
        CANCESSTRY_TEST_CHECK(env.tp.counters.tx_messages_completed == 1u);
    }

    /* STmin 0xF1 (100 us) rounds up to one tick per CF. */
    {
        tp_test_env_t env;

        tp_test_env_init(&env, NULL);
        CANCESSTRY_TEST_CHECK(cancestry_transport_send(&env.tp, payload, 20u) ==
                              CANCESTRY_TP_OK);
        CANCESSTRY_TEST_CHECK(tp_test_rx_fc(&env, 0x0u, 0u, 0xF1u) == CANCESTRY_TP_OK);
        CANCESSTRY_TEST_CHECK(tp_test_advance(&env, 1u) == CANCESTRY_TP_OK);
        CANCESSTRY_TEST_CHECK(env.frames.count == 2u); /* one CF per tick */
        CANCESSTRY_TEST_CHECK(tp_test_advance(&env, 1u) == CANCESTRY_TP_OK);
        CANCESSTRY_TEST_CHECK(env.frames.count == 3u);
        CANCESSTRY_TEST_CHECK(env.tp.counters.tx_messages_completed == 1u);
    }

    /* STmin 0: the burst is not paced. */
    {
        tp_test_env_t env;

        tp_test_env_init(&env, NULL);
        CANCESSTRY_TEST_CHECK(cancestry_transport_send(&env.tp, payload, 20u) ==
                              CANCESTRY_TP_OK);
        CANCESSTRY_TEST_CHECK(tp_test_rx_fc(&env, 0x0u, 0u, 0u) == CANCESTRY_TP_OK);
        CANCESSTRY_TEST_CHECK(tp_test_advance(&env, 1u) == CANCESTRY_TP_OK);
        CANCESSTRY_TEST_CHECK(env.frames.count == 3u); /* both CFs on one tick */
    }
}

/* TP-TX-005: Flow Control handling. OVFLW aborts the transmission with
 * TRANSPORT_BUFFER_OVERFLOW; Wait frames are accepted within the configured
 * limit and each restarts N_Bs; one Wait frame too many aborts with
 * TRANSPORT_PROTOCOL_FAULT (SW-FR-TP-003/008). */
static void tp_tx_flow_control_statuses(void)
{
    uint8_t payload[20];
    uint8_t i;

    for (i = 0u; i < 20u; ++i) {
        payload[i] = (uint8_t)(0x90u + i);
    }

    /* OVFLW aborts. */
    {
        tp_test_env_t env;
        cancestry_tp_fault_code_t fault;

        tp_test_env_init(&env, NULL);
        CANCESSTRY_TEST_CHECK(cancestry_transport_send(&env.tp, payload, 20u) ==
                              CANCESTRY_TP_OK);
        CANCESSTRY_TEST_CHECK(tp_test_rx_fc(&env, 0x2u, 0u, 0u) ==
                              CANCESTRY_TP_ERR_OVERFLOW);
        CANCESSTRY_TEST_CHECK(!cancestry_transport_tx_busy(&env.tp));
        CANCESSTRY_TEST_CHECK(env.tp.counters.buffer_overflows == 1u);
        CANCESSTRY_TEST_CHECK(tp_test_pop_fault(&env, &fault));
        CANCESSTRY_TEST_CHECK(fault == CANCESTRY_TP_FAULT_BUFFER_OVERFLOW);
        CANCESSTRY_TEST_CHECK(env.tp.counters.tx_messages_completed == 0u);
    }

    /* Wait frames within the limit (default 8) keep the session; the 9th
     * aborts. */
    {
        tp_test_env_t env;
        uint8_t wait;

        tp_test_env_init(&env, NULL);
        CANCESSTRY_TEST_CHECK(cancestry_transport_send(&env.tp, payload, 20u) ==
                              CANCESTRY_TP_OK);
        for (wait = 0u; wait < 8u; ++wait) {
            CANCESSTRY_TEST_CHECK(tp_test_rx_fc(&env, 0x1u, 0u, 0u) == CANCESTRY_TP_OK);
            CANCESSTRY_TEST_CHECK(cancestry_transport_tx_state(&env.tp) ==
                                  CANCESTRY_TP_STATE_FLOW_CONTROL);
        }
        CANCESSTRY_TEST_CHECK(env.tp.counters.wait_frames_rx == 8u);
        CANCESSTRY_TEST_CHECK(tp_test_rx_fc(&env, 0x1u, 0u, 0u) ==
                              CANCESTRY_TP_ERR_PROTOCOL);
        CANCESSTRY_TEST_CHECK(!cancestry_transport_tx_busy(&env.tp));
        CANCESSTRY_TEST_CHECK(env.tp.counters.protocol_faults == 1u);

        /* After the abort, a fresh transmission starts cleanly. */
        CANCESSTRY_TEST_CHECK(cancestry_transport_send(&env.tp, payload, 20u) ==
                              CANCESTRY_TP_OK);
        CANCESSTRY_TEST_CHECK(cancestry_transport_tx_busy(&env.tp));
    }
}

/* TP-TX-006: the N_Bs timer. No Flow Control after the First Frame aborts
 * with TRANSPORT_TIMEOUT on exactly the configured tick (SW-FR-TP-006). */
static void tp_tx_n_bs_timeout(void)
{
    tp_test_env_t env;
    cancestry_tp_config_t config;
    const uint8_t payload[20] = {0u};
    cancestry_tp_fault_code_t fault;

    memset(&config, 0, sizeof(config));
    config.n_bs_ms = 6u;
    tp_test_env_init(&env, &config);

    CANCESSTRY_TEST_CHECK(cancestry_transport_send(&env.tp, payload, 20u) == CANCESTRY_TP_OK);
    CANCESSTRY_TEST_CHECK(tp_test_advance(&env, 5u) == CANCESTRY_TP_OK);
    CANCESSTRY_TEST_CHECK(cancestry_transport_tx_busy(&env.tp));
    CANCESSTRY_TEST_CHECK(tp_test_advance(&env, 1u) == CANCESTRY_TP_ERR_TIMEOUT);
    CANCESSTRY_TEST_CHECK(!cancestry_transport_tx_busy(&env.tp));
    CANCESSTRY_TEST_CHECK(env.tp.counters.timeouts == 1u);
    CANCESSTRY_TEST_CHECK(cancestry_transport_last_fault(&env.tp) ==
                          CANCESTRY_TP_FAULT_TIMEOUT);
    CANCESSTRY_TEST_CHECK(tp_test_pop_fault(&env, &fault));
    CANCESSTRY_TEST_CHECK(fault == CANCESTRY_TP_FAULT_TIMEOUT);
    CANCESSTRY_TEST_CHECK(env.tp.counters.tx_messages_completed == 0u);
}

/* TP-TX-007: a frame the platform declines is retried on later ticks and
 * never silently dropped; when the N_As budget is exhausted the transmission
 * aborts with TRANSPORT_TIMEOUT (SW-FR-TP-009, SW-FR-TP-006). */
static void tp_tx_sink_decline_retry(void)
{
    const uint8_t payload[20] = {0u};

    /* A declined Single Frame goes out one tick later. */
    {
        tp_test_env_t env;

        tp_test_env_init(&env, NULL);
        env.frames.decline_budget = 1u;
        CANCESSTRY_TEST_CHECK(cancestry_transport_send(&env.tp, payload, 4u) ==
                              CANCESTRY_TP_OK);
        CANCESSTRY_TEST_CHECK(env.frames.count == 0u);
        CANCESSTRY_TEST_CHECK(env.tp.counters.frames_tx_declined == 1u);
        CANCESSTRY_TEST_CHECK(cancestry_transport_tx_busy(&env.tp)); /* pending SF */
        CANCESSTRY_TEST_CHECK(tp_test_advance(&env, 1u) == CANCESTRY_TP_OK);
        CANCESSTRY_TEST_CHECK(env.frames.count == 1u);
        CANCESSTRY_TEST_CHECK(env.frames.frames[0].kind == CANCESTRY_TP_TX_FRAME_SF);
        CANCESSTRY_TEST_CHECK(!cancestry_transport_tx_busy(&env.tp));
        CANCESSTRY_TEST_CHECK(env.tp.counters.tx_messages_completed == 1u);
    }

    /* A declined First Frame is retried until accepted; the session then
     * proceeds normally. */
    {
        tp_test_env_t env;

        tp_test_env_init(&env, NULL);
        env.frames.decline_budget = 2u;
        CANCESSTRY_TEST_CHECK(cancestry_transport_send(&env.tp, payload, 20u) ==
                              CANCESTRY_TP_OK);
        CANCESSTRY_TEST_CHECK(env.frames.count == 0u);
        CANCESSTRY_TEST_CHECK(tp_test_advance(&env, 1u) == CANCESTRY_TP_OK);
        CANCESSTRY_TEST_CHECK(env.frames.count == 0u); /* declined again */
        CANCESSTRY_TEST_CHECK(tp_test_advance(&env, 1u) == CANCESTRY_TP_OK);
        CANCESSTRY_TEST_CHECK(env.frames.count == 1u); /* accepted on retry 3 */
        CANCESSTRY_TEST_CHECK(env.frames.frames[0].kind == CANCESTRY_TP_TX_FRAME_FF);
        CANCESSTRY_TEST_CHECK(cancestry_transport_tx_state(&env.tp) ==
                              CANCESTRY_TP_STATE_FLOW_CONTROL);
        CANCESSTRY_TEST_CHECK(tp_test_rx_fc(&env, 0x0u, 0u, 0u) == CANCESTRY_TP_OK);
        CANCESSTRY_TEST_CHECK(tp_test_advance(&env, 1u) == CANCESTRY_TP_OK);
        CANCESSTRY_TEST_CHECK(env.tp.counters.tx_messages_completed == 1u);
    }

    /* N_As: a frame the platform keeps declining aborts deterministically. */
    {
        tp_test_env_t env;
        cancestry_tp_config_t config;
        cancestry_tp_fault_code_t fault;

        memset(&config, 0, sizeof(config));
        config.n_as_ms = 3u;
        tp_test_env_init(&env, &config);
        env.frames.decline_budget = 100u; /* decline everything */
        CANCESSTRY_TEST_CHECK(cancestry_transport_send(&env.tp, payload, 4u) ==
                              CANCESTRY_TP_OK);
        CANCESSTRY_TEST_CHECK(tp_test_advance(&env, 2u) == CANCESTRY_TP_OK);
        CANCESSTRY_TEST_CHECK(cancestry_transport_tx_busy(&env.tp));
        CANCESSTRY_TEST_CHECK(tp_test_advance(&env, 1u) == CANCESTRY_TP_ERR_TIMEOUT);
        CANCESSTRY_TEST_CHECK(!cancestry_transport_tx_busy(&env.tp));
        CANCESSTRY_TEST_CHECK(env.tp.counters.timeouts == 1u);
        CANCESSTRY_TEST_CHECK(tp_test_pop_fault(&env, &fault));
        CANCESSTRY_TEST_CHECK(fault == CANCESTRY_TP_FAULT_TIMEOUT);
        CANCESSTRY_TEST_CHECK(env.tp.counters.tx_messages_completed == 0u);
    }
}

/* TP-TX-008: Flow Control frames that cannot apply in the current state are
 * protocol faults: FC with no transmission waiting, FC during the CF burst,
 * a reserved FlowStatus and a wrong FC length (SW-FR-TP-005). */
static void tp_tx_unexpected_flow_control(void)
{
    const uint8_t payload[20] = {0u};

    /* FC while idle. */
    {
        tp_test_env_t env;

        tp_test_env_init(&env, NULL);
        CANCESSTRY_TEST_CHECK(tp_test_rx_fc(&env, 0x0u, 0u, 0u) ==
                              CANCESTRY_TP_ERR_PROTOCOL);
        CANCESSTRY_TEST_CHECK(env.tp.counters.protocol_faults == 1u);
    }

    /* A second FC while the CF burst is running aborts the transmission. */
    {
        tp_test_env_t env;

        tp_test_env_init(&env, NULL);
        CANCESSTRY_TEST_CHECK(cancestry_transport_send(&env.tp, payload, 20u) ==
                              CANCESTRY_TP_OK);
        CANCESSTRY_TEST_CHECK(tp_test_rx_fc(&env, 0x0u, 0u, 5u) == CANCESTRY_TP_OK); /* BS 5 */
        CANCESSTRY_TEST_CHECK(cancestry_transport_tx_state(&env.tp) ==
                              CANCESTRY_TP_STATE_CONSECUTIVE_FRAME);
        CANCESSTRY_TEST_CHECK(tp_test_rx_fc(&env, 0x0u, 0u, 0u) ==
                              CANCESTRY_TP_ERR_PROTOCOL);
        CANCESSTRY_TEST_CHECK(!cancestry_transport_tx_busy(&env.tp));
        CANCESSTRY_TEST_CHECK(env.tp.counters.protocol_faults == 1u);
    }

    /* Reserved FlowStatus 0x05 and a malformed FC length. */
    {
        tp_test_env_t env;

        tp_test_env_init(&env, NULL);
        CANCESSTRY_TEST_CHECK(cancestry_transport_send(&env.tp, payload, 20u) ==
                              CANCESTRY_TP_OK);
        CANCESSTRY_TEST_CHECK(tp_test_rx_fc(&env, 0x5u, 0u, 0u) ==
                              CANCESTRY_TP_ERR_PROTOCOL);
        CANCESSTRY_TEST_CHECK(!cancestry_transport_tx_busy(&env.tp));
    }
    {
        tp_test_env_t env;
        uint8_t frame[8] = {0x30u, 0x00u, 0x00u, 0x00u, 0u, 0u, 0u, 0u};

        tp_test_env_init(&env, NULL);
        CANCESSTRY_TEST_CHECK(cancestry_transport_send(&env.tp, payload, 20u) ==
                              CANCESTRY_TP_OK);
        CANCESSTRY_TEST_CHECK(tp_test_rx(&env, frame, 4u) == CANCESTRY_TP_ERR_PROTOCOL);
        CANCESSTRY_TEST_CHECK(!cancestry_transport_tx_busy(&env.tp));
    }
}

/* TP-TX-009: request validation. A transmission already in progress refuses
 * a new one; payloads beyond the static TX buffer or the 4095-byte FF limit
 * are refused with ERR_CAPACITY, never truncated (SW-FR-TP-001,
 * SW-FR-TP-007). */
static void tp_tx_request_validation(void)
{
    tp_test_env_t env;
    const uint8_t payload[20] = {0u};

    tp_test_env_init(&env, NULL);
    CANCESSTRY_TEST_CHECK(cancestry_transport_send(&env.tp, payload, 0u) ==
                          CANCESTRY_TP_ERR_ARGUMENT);
    CANCESSTRY_TEST_CHECK(cancestry_transport_send(&env.tp, NULL, 4u) ==
                          CANCESTRY_TP_ERR_ARGUMENT);

    /* Beyond the static TX buffer (128 bytes): refused. */
    {
        static uint8_t big[TP_TEST_TX_CAPACITY + 1u];

        memset(big, 0x33, sizeof(big));
        CANCESSTRY_TEST_CHECK(cancestry_transport_send(&env.tp, big, sizeof(big)) ==
                              CANCESTRY_TP_ERR_CAPACITY);
    }

    /* Busy refusal while a transmission is in progress. */
    CANCESSTRY_TEST_CHECK(cancestry_transport_send(&env.tp, payload, 20u) == CANCESTRY_TP_OK);
    CANCESSTRY_TEST_CHECK(cancestry_transport_send(&env.tp, payload, 4u) ==
                          CANCESTRY_TP_ERR_STATE);

    /* The 12-bit FF limit is enforced even with a large buffer (SW-FR-TP-007). */
    {
        static uint8_t huge[4096];
        cancestry_transport_t tp;
        cancestry_tp_sink_t sink;
        uint8_t rx_buffer[8];
        static uint8_t tx_buffer[4096];

        memset(&sink, 0, sizeof(sink));
        sink.on_frame_tx = tp_tx_accept_always;
        CANCESSTRY_TEST_CHECK(cancestry_transport_init(&tp, NULL, rx_buffer, sizeof(rx_buffer),
                                                       tx_buffer, sizeof(tx_buffer), &sink, NULL,
                                                       NULL));
        CANCESSTRY_TEST_CHECK(cancestry_transport_send(&tp, huge, 4096u) ==
                              CANCESTRY_TP_ERR_CAPACITY);
        CANCESSTRY_TEST_CHECK(cancestry_transport_send(&tp, huge, 4095u) == CANCESTRY_TP_OK);
        CANCESSTRY_TEST_CHECK(cancestry_transport_tx_busy(&tp));
    }

    /* Without a transmit sink the request fails closed (SW-FR-TP-009). */
    {
        cancestry_transport_t tp;
        uint8_t rx_buffer[8];
        uint8_t tx_buffer[16];

        CANCESSTRY_TEST_CHECK(cancestry_transport_init(&tp, NULL, rx_buffer, sizeof(rx_buffer),
                                                       tx_buffer, sizeof(tx_buffer), NULL, NULL,
                                                       NULL));
        CANCESSTRY_TEST_CHECK(cancestry_transport_send(&tp, payload, 4u) ==
                              CANCESTRY_TP_ERR_NULL);
        CANCESSTRY_TEST_CHECK(!cancestry_transport_tx_busy(&tp));
    }
}

int main(void)
{
    CANCESSTRY_TEST_SUITE_BEGIN("iso-tp segmentation conformance");
    CANCESSTRY_TEST_CASE("TP-TX-001 single frame");
    tp_tx_single_frame();
    CANCESSTRY_TEST_CASE("TP-TX-002 first frame + consecutive burst");
    tp_tx_multi_frame_burst();
    CANCESSTRY_TEST_CASE("TP-TX-003 block size");
    tp_tx_block_size();
    CANCESSTRY_TEST_CASE("TP-TX-004 STmin pacing");
    tp_tx_stmin_pacing();
    CANCESSTRY_TEST_CASE("TP-TX-005 flow control statuses");
    tp_tx_flow_control_statuses();
    CANCESSTRY_TEST_CASE("TP-TX-006 N_Bs timeout");
    tp_tx_n_bs_timeout();
    CANCESSTRY_TEST_CASE("TP-TX-007 sink decline retry and N_As");
    tp_tx_sink_decline_retry();
    CANCESSTRY_TEST_CASE("TP-TX-008 unexpected flow control");
    tp_tx_unexpected_flow_control();
    CANCESSTRY_TEST_CASE("TP-TX-009 request validation");
    tp_tx_request_validation();
    return CANCESSTRY_TEST_SUITE_END();
}
