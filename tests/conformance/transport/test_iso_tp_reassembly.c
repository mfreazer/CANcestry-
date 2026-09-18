/*
 * CANcestry ISO-TP conformance: multi-frame reassembly (Phase 10, issue #26).
 *
 * Contract under test (docs/software/SwRS.md section 14.1):
 *
 *   - SW-FR-TP-001: caller-owned static RX buffer, zero heap allocation,
 *     largest receivable message bounded by the buffer.
 *   - SW-FR-TP-002: Single Frame, First Frame and Consecutive Frame PCI
 *     decoding; the receiver answers a First Frame with Flow Control CTS.
 *   - SW-FR-TP-003: a First Frame larger than the buffer aborts with an
 *     OVFLW Flow Control and a TRANSPORT_BUFFER_OVERFLOW fault, before any
 *     payload byte is stored.
 *   - SW-FR-TP-004: a Consecutive Frame sequence-number skip (expects 2,
 *     gets 3) drops the session, clears the buffer and raises
 *     TRANSPORT_PROTOCOL_FAULT; the session never resumes.
 *   - SW-FR-TP-005: invalid PCI, contradictory frame lengths, and frame
 *     types that cannot apply in the current state are protocol faults.
 *   - SW-FR-TP-006: the receiver N_Cr timer is tick-driven; each Consecutive
 *     Frame restarts it; expiry aborts with TRANSPORT_TIMEOUT.
 *   - SW-FR-TP-009: completed messages are delivered synchronously through
 *     the caller callback.
 *   - SW-FR-TP-010: faults are FAULT-priority events with deterministic
 *     (source_id << 16) | fault codes and WARNING severity.
 *
 * Test ids: TP-RX-001 .. TP-RX-008.
 */

#include "iso_tp_fixture.h"

/* TP-RX-001: a Single Frame completes immediately and is delivered
 * synchronously through the callback (SW-FR-TP-002, SW-FR-TP-009). */
static void tp_rx_single_frame(void)
{
    tp_test_env_t env;
    const uint8_t payload[5] = {0x22u, 0xF1u, 0x90u, 0x11u, 0x22u};
    cancestry_tp_status_t status;

    tp_test_env_init(&env, NULL);

    status = tp_test_rx_sf(&env, payload, 5u);
    CANCESSTRY_TEST_CHECK(status == CANCESTRY_TP_OK_MESSAGE);
    CANCESSTRY_TEST_CHECK(env.messages.count == 1u);
    CANCESSTRY_TEST_CHECK(env.messages.messages[0].length == 5u);
    CANCESSTRY_TEST_CHECK(memcmp(env.messages.messages[0].data, payload, 5u) == 0);
    CANCESSTRY_TEST_CHECK(cancestry_transport_rx_state(&env.tp) == CANCESTRY_TP_STATE_IDLE);
    CANCESSTRY_TEST_CHECK(env.tp.counters.rx_messages_completed == 1u);
    CANCESSTRY_TEST_CHECK(env.tp.counters.rx_messages_delivered == 1u);
    CANCESSTRY_TEST_CHECK(env.tp.counters.sf_rx == 1u);
    CANCESSTRY_TEST_CHECK(tp_test_fault_count(&env) == 0u);

    /* Without an on_message callback the completed message is counted as
     * dropped instead of guessed at (fail-closed, SW-FR-TP-009). */
    {
        tp_test_env_t quiet;

        tp_test_env_init(&quiet, NULL);
        quiet.sink.on_message = NULL;
        status = tp_test_rx_sf(&quiet, payload, 5u);
        CANCESSTRY_TEST_CHECK(status == CANCESTRY_TP_OK_MESSAGE);
        CANCESSTRY_TEST_CHECK(quiet.messages.count == 0u);
        CANCESSTRY_TEST_CHECK(quiet.tp.counters.rx_messages_dropped_no_sink == 1u);
    }
}

/* TP-RX-002: First Frame + Consecutive Frames reassemble exactly; the engine
 * answers the First Frame with Flow Control CTS carrying its configured
 * block size and STmin (SW-FR-TP-002). */
static void tp_rx_multi_frame_happy_path(void)
{
    tp_test_env_t env;
    uint8_t payload[20];
    uint8_t i;
    cancestry_tp_status_t status;

    tp_test_env_init(&env, NULL);
    for (i = 0u; i < 20u; ++i) {
        payload[i] = (uint8_t)(i + 1u);
    }

    status = tp_test_rx_ff(&env, 20u, payload);
    CANCESSTRY_TEST_CHECK(status == CANCESTRY_TP_OK);
    CANCESSTRY_TEST_CHECK(cancestry_transport_rx_state(&env.tp) ==
                          CANCESTRY_TP_STATE_FIRST_FRAME);
    /* The receiver answered with FC CTS (default BS 0 / STmin 0). */
    CANCESSTRY_TEST_CHECK(env.frames.count == 1u);
    CANCESSTRY_TEST_CHECK(env.frames.frames[0].kind == CANCESTRY_TP_TX_FRAME_FC);
    CANCESSTRY_TEST_CHECK(env.frames.frames[0].length == 3u);
    CANCESSTRY_TEST_CHECK(env.frames.frames[0].data[0] == 0x30u);
    CANCESSTRY_TEST_CHECK(env.messages.count == 0u);

    /* CF1 (SN 1, 7 bytes) then CF2 (SN 2, final 7 bytes: 6 + 7 + 7 = 20). */
    status = tp_test_rx_cf(&env, 1u, &payload[6], 7u);
    CANCESSTRY_TEST_CHECK(status == CANCESTRY_TP_OK);
    CANCESSTRY_TEST_CHECK(env.messages.count == 0u);
    status = tp_test_rx_cf(&env, 2u, &payload[13], 7u);
    CANCESSTRY_TEST_CHECK(status == CANCESTRY_TP_OK_MESSAGE);

    CANCESSTRY_TEST_CHECK(env.messages.count == 1u);
    CANCESSTRY_TEST_CHECK(env.messages.messages[0].length == 20u);
    CANCESSTRY_TEST_CHECK(memcmp(env.messages.messages[0].data, payload, 20u) == 0);
    CANCESSTRY_TEST_CHECK(cancestry_transport_rx_state(&env.tp) == CANCESTRY_TP_STATE_IDLE);
    CANCESSTRY_TEST_CHECK(env.tp.counters.rx_messages_completed == 1u);
    CANCESSTRY_TEST_CHECK(env.tp.counters.ff_rx == 1u);
    CANCESSTRY_TEST_CHECK(env.tp.counters.cf_rx == 2u);
    CANCESSTRY_TEST_CHECK(env.tp.counters.fc_tx == 1u);
    CANCESSTRY_TEST_CHECK(tp_test_fault_count(&env) == 0u);
}

/* TP-RX-003: the normative abort. A CF whose sequence number skips (expects
 * 2, gets 3) drops the session, clears the buffer and raises
 * TRANSPORT_PROTOCOL_FAULT; a later CF cannot resume the session
 * (SW-FR-TP-004). */
static void tp_rx_sequence_number_skip(void)
{
    tp_test_env_t env;
    uint8_t payload[20];
    cancestry_tp_status_t status;
    cancestry_tp_fault_code_t fault;

    tp_test_env_init(&env, NULL);
    memset(payload, 0xA5, sizeof(payload));

    (void)tp_test_rx_ff(&env, 20u, payload);
    (void)tp_test_rx_cf(&env, 1u, &payload[6], 7u);

    /* Expect SN 2, deliver SN 3: deterministic abort. */
    status = tp_test_rx_cf(&env, 3u, &payload[13], 7u);
    CANCESSTRY_TEST_CHECK(status == CANCESTRY_TP_ERR_PROTOCOL);
    CANCESSTRY_TEST_CHECK(cancestry_transport_rx_state(&env.tp) == CANCESTRY_TP_STATE_IDLE);
    CANCESSTRY_TEST_CHECK(env.tp.counters.protocol_faults == 1u);
    CANCESSTRY_TEST_CHECK(cancestry_transport_last_fault(&env.tp) ==
                          CANCESTRY_TP_FAULT_PROTOCOL);
    CANCESSTRY_TEST_CHECK(env.messages.count == 0u);
    CANCESSTRY_TEST_CHECK(tp_test_fault_count(&env) == 1u);
    CANCESSTRY_TEST_CHECK(tp_test_pop_fault(&env, &fault));
    CANCESSTRY_TEST_CHECK(fault == CANCESTRY_TP_FAULT_PROTOCOL);

    /* The aborted session never resumes: the correct SN 2 now arrives with
     * no session in progress and is itself a protocol fault (fail-closed).
     * The reassembled buffer stays empty. */
    status = tp_test_rx_cf(&env, 2u, &payload[13], 7u);
    CANCESSTRY_TEST_CHECK(status == CANCESTRY_TP_ERR_PROTOCOL);
    CANCESSTRY_TEST_CHECK(env.tp.counters.protocol_faults == 2u);
    CANCESSTRY_TEST_CHECK(env.messages.count == 0u);
    CANCESSTRY_TEST_CHECK(env.tp.counters.rx_messages_completed == 0u);
}

/* TP-RX-004: the receiver N_Cr timer is driven by the 1 ms tick; each CF
 * restarts it and its expiry aborts with TRANSPORT_TIMEOUT
 * (SW-FR-TP-006). */
static void tp_rx_n_cr_timeout(void)
{
    tp_test_env_t env;
    cancestry_tp_config_t config;
    uint8_t payload[20];
    cancestry_tp_status_t status;
    cancestry_tp_fault_code_t fault;

    memset(&config, 0, sizeof(config));
    config.n_cr_ms = 5u; /* 5 ms for a short deterministic run */
    tp_test_env_init(&env, &config);
    memset(payload, 0x5A, sizeof(payload));

    (void)tp_test_rx_ff(&env, 20u, payload);
    (void)tp_test_rx_cf(&env, 1u, &payload[6], 7u);

    /* 4 ms without a CF: still waiting (each CF restarted N_Cr). */
    CANCESSTRY_TEST_CHECK(tp_test_advance(&env, 4u) == CANCESTRY_TP_OK);
    CANCESSTRY_TEST_CHECK(cancestry_transport_rx_state(&env.tp) ==
                          CANCESTRY_TP_STATE_FIRST_FRAME);

    /* A CF inside the window restarts the timer again. */
    CANCESSTRY_TEST_CHECK(tp_test_rx_cf(&env, 2u, &payload[13], 7u) ==
                          CANCESTRY_TP_OK_MESSAGE);
    CANCESSTRY_TEST_CHECK(env.messages.count == 1u);

    /* New session, then let N_Cr expire exactly: 5 ticks of 1 ms. */
    (void)tp_test_rx_ff(&env, 20u, payload);
    (void)tp_test_rx_cf(&env, 1u, &payload[6], 7u);
    status = tp_test_advance(&env, 5u);
    CANCESSTRY_TEST_CHECK(status == CANCESTRY_TP_ERR_TIMEOUT);
    CANCESSTRY_TEST_CHECK(cancestry_transport_rx_state(&env.tp) == CANCESTRY_TP_STATE_IDLE);
    CANCESSTRY_TEST_CHECK(env.tp.counters.timeouts == 1u);
    CANCESSTRY_TEST_CHECK(cancestry_transport_last_fault(&env.tp) == CANCESTRY_TP_FAULT_TIMEOUT);
    CANCESSTRY_TEST_CHECK(env.messages.count == 1u); /* nothing new delivered */
    CANCESSTRY_TEST_CHECK(tp_test_fault_count(&env) == 1u);
    CANCESSTRY_TEST_CHECK(tp_test_pop_fault(&env, &fault));
    CANCESSTRY_TEST_CHECK(fault == CANCESTRY_TP_FAULT_TIMEOUT);
}

/* TP-RX-005: a First Frame announcing more than the statically allocated RX
 * buffer aborts before storing any payload byte, answers with Flow Control
 * OVFLW and raises TRANSPORT_BUFFER_OVERFLOW; a message that exactly fits
 * still succeeds (SW-FR-TP-001, SW-FR-TP-003). */
static void tp_rx_buffer_overflow(void)
{
    tp_test_env_t env;
    uint8_t payload[TP_TEST_RX_CAPACITY + 1u];
    cancestry_tp_status_t status;
    cancestry_tp_fault_code_t fault;

    tp_test_env_init(&env, NULL);
    memset(payload, 0xC3, sizeof(payload));

    /* RX capacity is 64; announce 65 bytes. */
    status = tp_test_rx_ff(&env, (uint16_t)(TP_TEST_RX_CAPACITY + 1u), payload);
    CANCESSTRY_TEST_CHECK(status == CANCESTRY_TP_ERR_OVERFLOW);
    CANCESSTRY_TEST_CHECK(cancestry_transport_rx_state(&env.tp) == CANCESTRY_TP_STATE_IDLE);
    CANCESSTRY_TEST_CHECK(env.tp.counters.buffer_overflows == 1u);
    CANCESSTRY_TEST_CHECK(cancestry_transport_last_fault(&env.tp) ==
                          CANCESTRY_TP_FAULT_BUFFER_OVERFLOW);
    /* The peer was told with FC OVFLW. */
    CANCESSTRY_TEST_CHECK(env.frames.count == 1u);
    CANCESSTRY_TEST_CHECK(env.frames.frames[0].kind == CANCESTRY_TP_TX_FRAME_FC);
    CANCESSTRY_TEST_CHECK(env.frames.frames[0].data[0] == 0x32u);
    CANCESSTRY_TEST_CHECK(env.messages.count == 0u);
    CANCESSTRY_TEST_CHECK(tp_test_fault_count(&env) == 1u);
    CANCESSTRY_TEST_CHECK(tp_test_pop_fault(&env, &fault));
    CANCESSTRY_TEST_CHECK(fault == CANCESTRY_TP_FAULT_BUFFER_OVERFLOW);
    /* No payload byte was stored: the reassembly buffer is still all zero
     * after the abort cleared everything it had touched. */
    {
        size_t i;
        bool clean = true;

        for (i = 0u; i < TP_TEST_RX_CAPACITY; ++i) {
            if (env.rx_buffer[i] != 0u) {
                clean = false;
            }
        }
        CANCESSTRY_TEST_CHECK(clean);
    }

    /* A message that exactly fits the buffer completes (bounded, exact). */
    (void)tp_test_rx_ff(&env, (uint16_t)TP_TEST_RX_CAPACITY, payload);
    {
        uint16_t offset = 6u;
        uint8_t sn = 1u;

        while (offset < TP_TEST_RX_CAPACITY) {
            uint8_t chunk =
                (uint16_t)(TP_TEST_RX_CAPACITY - offset) > 7u ? 7u
                                                              : (uint8_t)(TP_TEST_RX_CAPACITY - offset);

            status = tp_test_rx_cf(&env, sn, &payload[offset], chunk);
            CANCESSTRY_TEST_CHECK(status == CANCESTRY_TP_OK ||
                                  offset + chunk == TP_TEST_RX_CAPACITY);
            offset = (uint16_t)(offset + chunk);
            sn = (uint8_t)((sn + 1u) & 0x0Fu);
        }
    }
    CANCESSTRY_TEST_CHECK(status == CANCESTRY_TP_OK_MESSAGE);
    CANCESSTRY_TEST_CHECK(env.messages.count == 1u);
    CANCESSTRY_TEST_CHECK(env.messages.messages[0].length == TP_TEST_RX_CAPACITY);
    CANCESSTRY_TEST_CHECK(memcmp(env.messages.messages[0].data, payload, TP_TEST_RX_CAPACITY) ==
                          0);
}

/* TP-RX-006: invalid PCI nibbles, contradictory frame lengths and frame
 * types that cannot apply in the current state are deterministic protocol
 * faults; the engine never crashes or hangs (SW-FR-TP-005). */
static void tp_rx_protocol_violations(void)
{
    static const uint8_t junk[8] = {0u};
    uint8_t payload[20];
    uint8_t frame[8];
    tp_test_env_t env;

    memset(payload, 0x11, sizeof(payload));

    /* Invalid PCI nibble 0x4 (reserved by ISO 15765-2). */
    tp_test_env_init(&env, NULL);
    frame[0] = 0x40u;
    memcpy(&frame[1], junk, 7u);
    CANCESSTRY_TEST_CHECK(tp_test_rx(&env, frame, 8u) == CANCESTRY_TP_ERR_PROTOCOL);
    CANCESSTRY_TEST_CHECK(env.tp.counters.protocol_faults == 1u);

    /* Single Frame with SF_DL 0 is not representable on classic CAN. */
    tp_test_env_init(&env, NULL);
    CANCESSTRY_TEST_CHECK(tp_test_rx_sf(&env, junk, 0u) == CANCESTRY_TP_ERR_PROTOCOL);
    CANCESSTRY_TEST_CHECK(env.tp.counters.protocol_faults == 1u);

    /* Single Frame whose length contradicts SF_DL. */
    tp_test_env_init(&env, NULL);
    frame[0] = 0x05u;
    memcpy(&frame[1], junk, 4u);
    CANCESSTRY_TEST_CHECK(tp_test_rx(&env, frame, 5u) == CANCESTRY_TP_ERR_PROTOCOL);

    /* First Frame shorter than 8 bytes. */
    tp_test_env_init(&env, NULL);
    frame[0] = 0x10u;
    frame[1] = 0x14u;
    memcpy(&frame[2], junk, 3u);
    CANCESSTRY_TEST_CHECK(tp_test_rx(&env, frame, 5u) == CANCESTRY_TP_ERR_PROTOCOL);

    /* First Frame announcing 7 bytes (belongs in a Single Frame). */
    tp_test_env_init(&env, NULL);
    frame[0] = 0x10u;
    frame[1] = 0x07u;
    memcpy(&frame[2], junk, 6u);
    CANCESSTRY_TEST_CHECK(tp_test_rx(&env, frame, 8u) == CANCESTRY_TP_ERR_PROTOCOL);

    /* Consecutive Frame with no reception in progress. */
    tp_test_env_init(&env, NULL);
    CANCESSTRY_TEST_CHECK(tp_test_rx_cf(&env, 1u, junk, 7u) == CANCESTRY_TP_ERR_PROTOCOL);

    /* A new Single Frame while reassembly is in progress: state violation,
     * the SF is not processed. */
    tp_test_env_init(&env, NULL);
    (void)tp_test_rx_ff(&env, 20u, payload);
    CANCESSTRY_TEST_CHECK(tp_test_rx_sf(&env, junk, 4u) == CANCESTRY_TP_ERR_PROTOCOL);
    CANCESSTRY_TEST_CHECK(cancestry_transport_rx_state(&env.tp) == CANCESTRY_TP_STATE_IDLE);

    /* A new First Frame while reassembly is in progress: sequence error. */
    tp_test_env_init(&env, NULL);
    (void)tp_test_rx_ff(&env, 20u, payload);
    (void)tp_test_rx_cf(&env, 1u, &payload[6], 7u);
    CANCESSTRY_TEST_CHECK(tp_test_rx_ff(&env, 20u, payload) == CANCESTRY_TP_ERR_PROTOCOL);
    CANCESSTRY_TEST_CHECK(cancestry_transport_rx_state(&env.tp) == CANCESTRY_TP_STATE_IDLE);

    /* A non-final CF must be a full 8-byte frame. */
    tp_test_env_init(&env, NULL);
    (void)tp_test_rx_ff(&env, 20u, payload);
    CANCESSTRY_TEST_CHECK(tp_test_rx_cf(&env, 1u, &payload[6], 5u) == CANCESTRY_TP_ERR_PROTOCOL);
    CANCESSTRY_TEST_CHECK(cancestry_transport_rx_state(&env.tp) == CANCESTRY_TP_STATE_IDLE);

    /* The final CF must carry exactly the remaining bytes. */
    tp_test_env_init(&env, NULL);
    (void)tp_test_rx_ff(&env, 20u, payload);
    (void)tp_test_rx_cf(&env, 1u, &payload[6], 7u);
    /* remaining = 7, so a 3-byte payload is a length contradiction */
    CANCESSTRY_TEST_CHECK(tp_test_rx_cf(&env, 2u, &payload[13], 3u) ==
                          CANCESTRY_TP_ERR_PROTOCOL);
    CANCESSTRY_TEST_CHECK(cancestry_transport_rx_state(&env.tp) == CANCESTRY_TP_STATE_IDLE);

    /* Zero-length and over-long frames are rejected as arguments. */
    tp_test_env_init(&env, NULL);
    CANCESSTRY_TEST_CHECK(tp_test_rx(&env, frame, 0u) == CANCESTRY_TP_ERR_ARGUMENT);
    CANCESSTRY_TEST_CHECK(cancestry_transport_rx_frame(&env.tp, frame, 9u) ==
                          CANCESTRY_TP_ERR_ARGUMENT);

    /* Every violation left the engine usable: a clean SF still works. */
    {
        const uint8_t ok[3] = {0x01u, 0x02u, 0x03u};

        CANCESSTRY_TEST_CHECK(tp_test_rx_sf(&env, ok, 3u) == CANCESTRY_TP_OK_MESSAGE);
        CANCESSTRY_TEST_CHECK(env.messages.count == 1u);
    }
}

/* TP-RX-007: raised faults are FAULT-priority events carrying the
 * deterministic (source_id << 16) | fault code, WARNING severity and the
 * source id, and the code resolves through the static name table
 * (SW-FR-TP-010). */
static void tp_rx_fault_event_shape(void)
{
    tp_test_env_t env;
    cancestry_event_t event;
    bool found = false;

    tp_test_env_init(&env, NULL);
    {
        static const uint8_t junk[8] = {0u};
        uint8_t frame[8];

        frame[0] = 0x2Bu; /* reserved PCI nibble */
        memcpy(&frame[1], junk, 7u);
        (void)tp_test_rx(&env, frame, 8u);
    }

    while (cancestry_event_queue_pop(&env.queue, &event) == CANCESTRY_EVENT_QUEUE_OK) {
        if (event.type == CANCESTRY_EVENT_TYPE_FAULT_RAISED) {
            found = true;
            CANCESSTRY_TEST_CHECK(event.priority_class == CANCESTRY_PRIORITY_CLASS_FAULT);
            CANCESSTRY_TEST_CHECK(event.payload.fault.severity ==
                                  CANCESTRY_FAULT_SEVERITY_WARNING);
            CANCESSTRY_TEST_CHECK(event.payload.fault.source_id == TP_TEST_SOURCE_ID);
            CANCESSTRY_TEST_CHECK(event.payload.fault.fault_code ==
                                  ((TP_TEST_SOURCE_ID << 16u) |
                                   (uint32_t)CANCESTRY_TP_FAULT_PROTOCOL));
            CANCESSTRY_TEST_CHECK(strcmp(cancestry_tp_fault_name(
                                             (cancestry_tp_fault_code_t)(
                                                 event.payload.fault.fault_code & 0xFFFFu)),
                                         "TRANSPORT_PROTOCOL_FAULT") == 0);
        }
    }
    CANCESSTRY_TEST_CHECK(found);
    CANCESSTRY_TEST_CHECK(strcmp(cancestry_tp_fault_name(CANCESTRY_TP_FAULT_BUFFER_OVERFLOW),
                                 "TRANSPORT_BUFFER_OVERFLOW") == 0);
    CANCESSTRY_TEST_CHECK(strcmp(cancestry_tp_fault_name(CANCESTRY_TP_FAULT_TIMEOUT),
                                 "TRANSPORT_TIMEOUT") == 0);
}

/* TP-RX-008: reassembly to the exact statically allocated size is exercised
 * under ASan/UBSan through the whole TP-RX-005 walk, and the STmin codec and
 * state/status name tables stay deterministic (SW-FR-TP-001, SW-FR-TP-002). */
static void tp_rx_helpers_and_bounds(void)
{
    /* STmin decoding (SW-FR-TP-02/08 helper contract). */
    CANCESSTRY_TEST_CHECK(cancestry_tp_stmin_decode_us(0x00u) == 0u);
    CANCESSTRY_TEST_CHECK(cancestry_tp_stmin_decode_us(0x05u) == 5000u);
    CANCESSTRY_TEST_CHECK(cancestry_tp_stmin_decode_us(0x7Fu) == 127000u);
    CANCESSTRY_TEST_CHECK(cancestry_tp_stmin_decode_us(0xF1u) == 100u);
    CANCESSTRY_TEST_CHECK(cancestry_tp_stmin_decode_us(0xF9u) == 900u);
    /* Reserved encodings decode to 0 deterministically. */
    CANCESSTRY_TEST_CHECK(cancestry_tp_stmin_decode_us(0x80u) == 0u);
    CANCESSTRY_TEST_CHECK(cancestry_tp_stmin_decode_us(0xFFu) == 0u);
    CANCESSTRY_TEST_CHECK(cancestry_tp_stmin_encode_ms(3u) == 0x03u);
    CANCESSTRY_TEST_CHECK(cancestry_tp_stmin_encode_ms(999u) == 0x7Fu);

    /* Name tables are stable and total. */
    CANCESSTRY_TEST_CHECK(strcmp(cancestry_tp_state_name(CANCESTRY_TP_STATE_FIRST_FRAME),
                                 "FIRST_FRAME") == 0);
    CANCESSTRY_TEST_CHECK(strcmp(cancestry_tp_state_name(CANCESTRY_TP_STATE_FLOW_CONTROL),
                                 "FLOW_CONTROL") == 0);
    CANCESSTRY_TEST_CHECK(strcmp(cancestry_tp_status_name(CANCESTRY_TP_ERR_PROTOCOL),
                                 "ERR_PROTOCOL") == 0);
    CANCESSTRY_TEST_CHECK(strcmp(cancestry_tp_status_name((cancestry_tp_status_t)99),
                                 "INVALID") == 0);
    CANCESSTRY_TEST_CHECK(cancestry_tp_status_is_ok(CANCESTRY_TP_OK_MESSAGE));
    CANCESSTRY_TEST_CHECK(!cancestry_tp_status_is_ok(CANCESTRY_TP_ERR_TIMEOUT));

    /* Sequence numbers roll over 15 -> 0 (SW-FR-TP-002): a 120-byte message
     * needs 17 CFs, exercising SN 1..15, 0, 1. */
    {
        tp_test_env_t env;
        uint8_t payload[120];
        uint16_t offset = 6u;
        uint8_t sn = 1u;
        uint8_t i;

        tp_test_env_init(&env, NULL);
        for (i = 0u; i < 120u; ++i) {
            payload[i] = (uint8_t)(0xD0u + i);
        }
        CANCESSTRY_TEST_CHECK(tp_test_rx_ff(&env, 120u, payload) == CANCESTRY_TP_OK);
        while (offset < 120u) {
            uint8_t chunk = (120u - offset > 7u) ? 7u : (uint8_t)(120u - offset);
            cancestry_tp_status_t status =
                tp_test_rx_cf(&env, sn, &payload[offset], chunk);

            CANCESSTRY_TEST_CHECK(status == CANCESTRY_TP_OK ||
                                  offset + chunk == 120u);
            offset = (uint16_t)(offset + chunk);
            sn = (uint8_t)((sn + 1u) & 0x0Fu);
        }
        CANCESSTRY_TEST_CHECK(env.messages.count == 1u);
        CANCESSTRY_TEST_CHECK(env.messages.messages[0].length == 120u);
        CANCESSTRY_TEST_CHECK(memcmp(env.messages.messages[0].data, payload, 120u) == 0);
        CANCESSTRY_TEST_CHECK(env.tp.counters.rx_messages_completed == 1u);
    }
}

int main(void)
{
    CANCESSTRY_TEST_SUITE_BEGIN("iso-tp reassembly conformance");
    CANCESSTRY_TEST_CASE("TP-RX-001 single frame");
    tp_rx_single_frame();
    CANCESSTRY_TEST_CASE("TP-RX-002 multi-frame happy path");
    tp_rx_multi_frame_happy_path();
    CANCESSTRY_TEST_CASE("TP-RX-003 sequence number skip aborts");
    tp_rx_sequence_number_skip();
    CANCESSTRY_TEST_CASE("TP-RX-004 N_Cr timeout aborts");
    tp_rx_n_cr_timeout();
    CANCESSTRY_TEST_CASE("TP-RX-005 buffer overflow aborts");
    tp_rx_buffer_overflow();
    CANCESSTRY_TEST_CASE("TP-RX-006 protocol violations");
    tp_rx_protocol_violations();
    CANCESSTRY_TEST_CASE("TP-RX-007 fault event shape");
    tp_rx_fault_event_shape();
    CANCESSTRY_TEST_CASE("TP-RX-008 bounds, helpers, SN wrap");
    tp_rx_helpers_and_bounds();
    return CANCESSTRY_TEST_SUITE_END();
}
