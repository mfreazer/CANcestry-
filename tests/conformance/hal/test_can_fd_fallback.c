/*
 * CANcestry HAL conformance: CAN FD on a classic-only interface.
 *
 * This test pins the exact behavioural contract of the CAN FD fallback, the
 * most common failure mode in a mixed-protocol deployment. It is written
 * against the mock HAL so the contract is proven on the same platform-
 * independent code path (core/hal/src/hal.c) that the SocketCAN backend uses.
 *
 * Verifies (SW-FR-CANFD-003, SW-FR-CANFD-005, HAL-CANFD-FALLBACK-001):
 *   1. An interface that did not negotiate CAN FD rejects an ingress CAN FD
 *      frame: a FAULT_RAISED event with code PROTOCOL_UNSUPPORTED and
 *      severity ERROR is enqueued, the frame is dropped (no CAN_RX event, RX
 *      ring empty) and it is never truncated to 8 bytes.
 *   2. The rejection is counted and observable: hal_get_status() reports
 *      last_fault PROTOCOL_UNSUPPORTED, a non-zero rx_protocol_rejected and
 *      can_fd false.
 *   3. Classic traffic on the same interface is unaffected, and the fault is
 *      ordered before the CAN_RX events of the same poll (FAULT priority
 *      class pre-empts CAN_RX, event-ordering.md section 3) - deterministically,
 *      exactly like every other HAL fault.
 *   4. Transmitting a CAN FD frame on a classic-only interface returns
 *      CANCESTRY_HAL_ERR_UNSUPPORTED, raises the same fault, and puts nothing
 *      on the wire (the mock captures zero TX frames).
 *   5. An FD frame whose payload length the wire format cannot express
 *      (9 bytes) is MALFORMED_FRAME, not PROTOCOL_UNSUPPORTED, and is dropped.
 *   6. An interface that did negotiate CAN FD (cancestry_hal_iface_can_fd()
 *      true) delivers the same 64-byte frame intact, byte for byte.
 *   7. The fault code's numeric value and name are part of the normative
 *      contract and are pinned here.
 *
 * Implements: SW-FR-CANFD-002, SW-FR-CANFD-003, SW-FR-CANFD-005
 * Test ids:    HAL-CANFD-FALLBACK-001
 */

#include "cancestry/event/clock.h"
#include "cancestry/event/queue.h"
#include "cancestry/event/types.h"
#include "cancestry/hal/hal.h"
#include "cancestry/hal/types.h"
#include "mock_hal.h"

#include <stdio.h>
#include <string.h>

#define RING_CAP ((uint16_t)16u)
#define QUEUE_CAP ((uint16_t)64u)
#define IFACE_CAN0 ((cancestry_interface_id_t)1u)
#define IFACE_CAN1 ((cancestry_interface_id_t)2u)
#define FD_PAYLOAD_BYTES ((size_t)64u)

static int failures = 0;

static void expect(bool cond, const char *what)
{
    if (!cond) {
        fprintf(stderr, "FAIL: %s\n", what);
        failures++;
    }
}

static void expect_u64(uint64_t actual, uint64_t expected, const char *what)
{
    if (actual != expected) {
        fprintf(stderr, "FAIL: %s (got %llu, expected %llu)\n", what,
                (unsigned long long)actual, (unsigned long long)expected);
        failures++;
    }
}

/** Fill a payload with a deterministic, position-dependent pattern. */
static void fill_pattern(uint8_t *data, size_t length)
{
    size_t i;
    for (i = 0u; i < length; ++i) {
        data[i] = (uint8_t)((i * 7u + 3u) & 0xFFu);
    }
}

/** Build a CAN FD frame carrying @p length payload bytes. */
static cancestry_hal_frame_t make_fd_frame(cancestry_interface_id_t iface,
                                           uint32_t can_id,
                                           uint8_t length)
{
    cancestry_hal_frame_t frame;

    memset(&frame, 0, sizeof(frame));
    frame.interface_id = iface;
    frame.can_id = can_id;
    frame.is_extended = 1u; /* 29-bit id: extended frames ride along with FD */
    frame.is_fd = 1u;
    frame.length = length;
    fill_pattern(frame.data, (size_t)length);
    return frame;
}

/** Build a classic 8-byte frame. */
static cancestry_hal_frame_t make_classic_frame(cancestry_interface_id_t iface, uint32_t can_id)
{
    cancestry_hal_frame_t frame;

    memset(&frame, 0, sizeof(frame));
    frame.interface_id = iface;
    frame.can_id = can_id;
    frame.is_extended = 0u;
    frame.is_fd = 0u;
    frame.length = 8u;
    fill_pattern(frame.data, 8u);
    return frame;
}

/** Drain the queue, counting CAN_RX events and collecting fault codes. */
typedef struct poll_result {
    uint32_t can_rx_count;
    uint32_t fault_count;
    uint32_t fault_codes[8];
    cancestry_event_t last_can_rx;
    bool have_can_rx;
} poll_result_t;

static void drain(cancestry_event_queue_t *queue, poll_result_t *out)
{
    cancestry_event_t ev;

    memset(out, 0, sizeof(*out));
    while (cancestry_event_queue_pop(queue, &ev) == CANCESTRY_EVENT_QUEUE_OK) {
        if (ev.type == CANCESTRY_EVENT_TYPE_CAN_RX) {
            out->can_rx_count++;
            out->last_can_rx = ev;
            out->have_can_rx = true;
        } else if (ev.type == CANCESTRY_EVENT_TYPE_FAULT_RAISED) {
            if (out->fault_count < 8u) {
                out->fault_codes[out->fault_count] = ev.payload.fault.fault_code & 0xFFFFu;
            }
            out->fault_count++;
            /* A fault must never be delivered after data of the same poll:
             * FAULT (0) pre-empts CAN_RX (3) in the priority order. */
            expect(out->can_rx_count == 0u, "fault is ordered before CAN_RX data");
        }
    }
}

static bool result_has_fault(const poll_result_t *result, cancestry_hal_fault_code_t code)
{
    uint32_t i;
    for (i = 0u; i < result->fault_count && i < 8u; ++i) {
        if (result->fault_codes[i] == (uint32_t)code) {
            return true;
        }
    }
    return false;
}

int main(void)
{
    static cancestry_mock_hal_t mock;
    static cancestry_hal_t hal;
    static cancestry_virtual_clock_t vclock;
    cancestry_clock_t clock;
    static cancestry_hal_frame_t rx0_storage[RING_CAP];
    static cancestry_hal_frame_t tx0_storage[RING_CAP];
    static cancestry_hal_frame_t rx1_storage[RING_CAP];
    static cancestry_hal_frame_t tx1_storage[RING_CAP];
    cancestry_hal_rx_ring_t rx0;
    cancestry_hal_tx_ring_t tx0;
    cancestry_hal_rx_ring_t rx1;
    cancestry_hal_tx_ring_t tx1;
    static cancestry_event_t queue_storage[QUEUE_CAP];
    cancestry_event_queue_t queue;
    cancestry_hal_if_config_t ifaces[2];
    cancestry_hal_rx_ring_t *rx_rings[2];
    cancestry_hal_tx_ring_t *tx_rings[2];
    cancestry_hal_config_t config;
    cancestry_hal_frame_t fd_frame;
    poll_result_t result;

    printf("HAL conformance: CAN FD fallback (HAL-CANFD-FALLBACK-001)\n");

    /* 7. The fault code is part of the normative contract: trace captures and
     *    FSM fault tables key off these numbers, so they are pinned. */
    expect((int)CANCESTRY_HAL_FAULT_PROTOCOL_UNSUPPORTED == 10,
           "PROTOCOL_UNSUPPORTED keeps its normative numeric value");
    expect(strcmp(cancestry_hal_fault_code_name(CANCESTRY_HAL_FAULT_PROTOCOL_UNSUPPORTED),
                  "PROTOCOL_UNSUPPORTED") == 0,
           "PROTOCOL_UNSUPPORTED has a stable name");
    expect(strcmp(cancestry_hal_status_name(CANCESTRY_HAL_ERR_UNSUPPORTED), "ERR_UNSUPPORTED") == 0,
           "ERR_UNSUPPORTED has a stable name");
    expect(CANCESTRY_HAL_FRAME_MAX_LENGTH == CANCESTRY_CAN_FD_FRAME_MAX_LENGTH,
           "the HAL frame buffer is 64 bytes wide");
    expect(CANCESTRY_HAL_CLASSIC_FRAME_MAX_LENGTH == 8u,
           "the classic payload limit is still 8 bytes");

    cancestry_mock_hal_init(&mock);
    /* Interface 0 stays classic-only (the mock's default); interface 1 opts
     * into CAN FD before init, which is when the HAL reads capabilities. */
    cancestry_mock_hal_set_fd_support(&mock, 1u, true);
    expect(!cancestry_mock_hal_fd_support(&mock, 0u), "interface 0 is classic-only");
    expect(cancestry_mock_hal_fd_support(&mock, 1u), "interface 1 negotiated CAN FD");

    expect(cancestry_hal_rx_ring_init(&rx0, rx0_storage, RING_CAP), "rx0 init");
    expect(cancestry_hal_tx_ring_init(&tx0, tx0_storage, RING_CAP), "tx0 init");
    expect(cancestry_hal_rx_ring_init(&rx1, rx1_storage, RING_CAP), "rx1 init");
    expect(cancestry_hal_tx_ring_init(&tx1, tx1_storage, RING_CAP), "tx1 init");
    cancestry_virtual_clock_init(&vclock, 1000u);
    clock = cancestry_clock_from_virtual(&vclock);
    expect(cancestry_event_queue_init(&queue, queue_storage, QUEUE_CAP), "queue init");

    memset(ifaces, 0, sizeof(ifaces));
    ifaces[0].interface_id = IFACE_CAN0;
    ifaces[0].bitrate = 500000u;
    memcpy(ifaces[0].name, "vcan0", 5);
    ifaces[1].interface_id = IFACE_CAN1;
    ifaces[1].bitrate = 500000u;
    ifaces[1].can_fd = 1u;
    ifaces[1].data_bitrate = 2000000u;
    memcpy(ifaces[1].name, "vcan1", 5);
    rx_rings[0] = &rx0;
    rx_rings[1] = &rx1;
    tx_rings[0] = &tx0;
    tx_rings[1] = &tx1;
    memset(&config, 0, sizeof(config));
    config.backend = cancestry_mock_hal_backend();
    config.backend_context = &mock;
    config.clock = &clock;
    config.ifaces = ifaces;
    config.iface_count = 2u;
    config.rx_rings = rx_rings;
    config.tx_rings = tx_rings;
    expect(cancestry_hal_init(&hal, &config), "hal init");

    expect(!cancestry_hal_iface_can_fd(&hal, IFACE_CAN0), "can0 reports classic-only");
    expect(cancestry_hal_iface_can_fd(&hal, IFACE_CAN1), "can1 reports CAN FD");
    expect(!cancestry_hal_iface_can_fd(&hal, (cancestry_interface_id_t)99u),
           "an unknown interface reports classic-only (fail closed)");

    /* ------------------------------------------------------------------ */
    /* 1-3. A CAN FD frame on the classic-only interface.                  */
    /* ------------------------------------------------------------------ */
    fd_frame = make_fd_frame(IFACE_CAN0, 0x18FF1234u, (uint8_t)FD_PAYLOAD_BYTES);
    expect(cancestry_hal_frame_is_valid(&fd_frame), "the FD frame itself is well-formed");
    expect(cancestry_mock_hal_inject_rx(&mock, 0u, &fd_frame), "FD frame injected on can0");
    /* A classic frame in the same poll proves classic traffic keeps flowing
     * and that the fault is ordered ahead of the data. */
    {
        cancestry_hal_frame_t classic = make_classic_frame(IFACE_CAN0, 0x120u);
        expect(cancestry_mock_hal_inject_rx(&mock, 0u, &classic), "classic frame injected");
    }
    {
        uint32_t rx = 0u;
        uint32_t flt = 0u;
        expect(cancestry_hal_status_is_ok(cancestry_hal_poll_rx(&hal, &queue, &rx, &flt)),
               "poll_rx succeeds");
        expect_u64(rx, 1u, "exactly one frame (the classic one) was delivered");
        expect_u64(flt, 1u, "exactly one fault was raised");
    }
    drain(&queue, &result);
    expect_u64(result.can_rx_count, 1u, "one CAN_RX event");
    expect_u64(result.fault_count, 1u, "one FAULT_RAISED event");
    expect(result.have_can_rx, "the classic frame was delivered");
    expect(result.last_can_rx.payload.can_rx.can_id == 0x120u,
           "the delivered frame is the classic one, not the FD one");
    expect(result.last_can_rx.payload.can_rx.is_fd == 0u, "delivered frame is classic");
    expect(result_has_fault(&result, CANCESTRY_HAL_FAULT_PROTOCOL_UNSUPPORTED),
           "PROTOCOL_UNSUPPORTED fault was raised");
    expect(cancestry_hal_rx_ring_count(&rx0) == 0u, "the rejected frame left no residue in the ring");

    /* 2. Observable status. */
    {
        cancestry_hal_if_status_t status[2];
        uint8_t count = 0u;
        expect(cancestry_hal_status_is_ok(cancestry_hal_get_status(&hal, status, 2u, &count)),
               "get_status ok");
        expect_u64(count, 2u, "two interfaces reported");
        expect(status[0].last_fault == CANCESTRY_HAL_FAULT_PROTOCOL_UNSUPPORTED,
               "can0 last_fault is PROTOCOL_UNSUPPORTED");
        expect_u64(status[0].rx_protocol_rejected, 1u, "can0 counted one rejected frame");
        expect(!status[0].can_fd, "can0 status reports no CAN FD");
        expect(status[1].can_fd, "can1 status reports CAN FD");
        expect_u64(status[1].rx_protocol_rejected, 0u, "can1 rejected nothing");
    }

    /* A second rejection increments the counter deterministically. */
    {
        cancestry_hal_frame_t second = make_fd_frame(IFACE_CAN0, 0x18FF1235u, 32u);
        expect(cancestry_mock_hal_inject_rx(&mock, 0u, &second), "second FD frame injected");
        (void)cancestry_hal_poll_rx(&hal, &queue, NULL, NULL);
        drain(&queue, &result);
        expect_u64(result.can_rx_count, 0u, "the second FD frame produced no CAN_RX");
        expect(result_has_fault(&result, CANCESTRY_HAL_FAULT_PROTOCOL_UNSUPPORTED),
               "the second FD frame raised PROTOCOL_UNSUPPORTED");
    }
    {
        cancestry_hal_if_status_t status;
        uint8_t count = 0u;
        (void)cancestry_hal_get_status(&hal, &status, 1u, &count);
        expect_u64(status.rx_protocol_rejected, 2u, "rejections are counted monotonically");
    }

    /* ------------------------------------------------------------------ */
    /* 4. Egress: FD transmit on a classic-only interface is refused.      */
    /* ------------------------------------------------------------------ */
    {
        cancestry_hal_frame_t out = make_fd_frame(IFACE_CAN0, 0x321u, 16u);
        cancestry_hal_status_t s = cancestry_hal_send_tx(&hal, &queue, &out);
        expect(s == CANCESTRY_HAL_ERR_UNSUPPORTED, "send_tx refuses the FD frame");
    }
    drain(&queue, &result);
    expect(result_has_fault(&result, CANCESTRY_HAL_FAULT_PROTOCOL_UNSUPPORTED),
           "refused FD transmit raised PROTOCOL_UNSUPPORTED");
    expect_u64(cancestry_mock_hal_tx_count(&mock, 0u), 0u,
               "nothing reached the wire (no truncated FD frame)");
    expect(cancestry_hal_tx_ring_count(&tx0) == 0u, "the refused frame was not queued either");

    /* The same frame is accepted on the FD-capable interface. */
    {
        cancestry_hal_frame_t out = make_fd_frame(IFACE_CAN1, 0x321u, 16u);
        expect(cancestry_hal_status_is_ok(cancestry_hal_send_tx(&hal, &queue, &out)),
               "send_tx accepts the FD frame on can1");
    }
    expect_u64(cancestry_mock_hal_tx_count(&mock, 1u), 1u, "can1 transmitted one frame");
    {
        const cancestry_hal_frame_t *sent = cancestry_mock_hal_tx_at(&mock, 1u, 0u);
        expect(sent != NULL, "captured TX frame exists");
        if (sent != NULL) {
            expect(sent->is_fd == 1u, "captured frame is still an FD frame");
            expect_u64(sent->length, 16u, "captured frame kept its full 16-byte payload");
            expect(sent->data[15] == (uint8_t)((15u * 7u + 3u) & 0xFFu),
                   "captured payload byte 15 is intact");
        }
    }

    /* ------------------------------------------------------------------ */
    /* 5. An FD payload length the wire cannot express is malformed.       */
    /* ------------------------------------------------------------------ */
    {
        cancestry_hal_frame_t bad = make_fd_frame(IFACE_CAN1, 0x400u, 9u);
        expect(!cancestry_hal_frame_is_valid(&bad), "9 bytes is not a CAN FD payload length");
        expect(cancestry_mock_hal_inject_rx(&mock, 1u, &bad), "bad-length FD frame injected");
        (void)cancestry_hal_poll_rx(&hal, &queue, NULL, NULL);
        drain(&queue, &result);
        expect_u64(result.can_rx_count, 0u, "the malformed frame produced no CAN_RX");
        expect(result_has_fault(&result, CANCESTRY_HAL_FAULT_MALFORMED_FRAME),
               "the malformed frame raised MALFORMED_FRAME");
    }
    /* A classic frame claiming more than 8 bytes is malformed too. */
    {
        cancestry_hal_frame_t bad = make_classic_frame(IFACE_CAN0, 0x121u);
        bad.length = 12u; /* is_fd stays 0: 12 bytes is not classic */
        expect(!cancestry_hal_frame_is_valid(&bad), "12 bytes is not a classic payload length");
        expect(cancestry_mock_hal_inject_rx(&mock, 0u, &bad), "wide classic frame injected");
        (void)cancestry_hal_poll_rx(&hal, &queue, NULL, NULL);
        drain(&queue, &result);
        expect(result_has_fault(&result, CANCESTRY_HAL_FAULT_MALFORMED_FRAME),
               "the wide classic frame raised MALFORMED_FRAME");
    }

    /* ------------------------------------------------------------------ */
    /* 6. The FD-capable interface delivers the full 64-byte payload.      */
    /* ------------------------------------------------------------------ */
    fd_frame = make_fd_frame(IFACE_CAN1, 0x18FF5678u, (uint8_t)FD_PAYLOAD_BYTES);
    expect(cancestry_mock_hal_inject_rx(&mock, 1u, &fd_frame), "FD frame injected on can1");
    {
        uint32_t rx = 0u;
        uint32_t flt = 0u;
        (void)cancestry_hal_poll_rx(&hal, &queue, &rx, &flt);
        expect_u64(rx, 1u, "the FD frame was delivered on can1");
        expect_u64(flt, 0u, "no fault on the FD-capable interface");
    }
    drain(&queue, &result);
    expect_u64(result.can_rx_count, 1u, "one CAN_RX event for the FD frame");
    expect(result.have_can_rx, "FD frame event captured");
    expect(result.last_can_rx.payload.can_rx.is_fd == 1u, "event carries the FD flag");
    expect(result.last_can_rx.payload.can_rx.is_extended == 1u, "event carries the EFF flag");
    expect_u64(result.last_can_rx.payload.can_rx.length, FD_PAYLOAD_BYTES,
               "event carries the full 64-byte length");
    {
        size_t i;
        bool intact = true;
        for (i = 0u; i < FD_PAYLOAD_BYTES; ++i) {
            uint8_t expected = (uint8_t)((i * 7u + 3u) & 0xFFu);
            if (result.last_can_rx.payload.can_rx.data[i] != expected) {
                intact = false;
            }
        }
        expect(intact, "all 64 payload bytes survived ingress byte for byte");
    }

    if (failures != 0) {
        fprintf(stderr, "RESULT FAIL (%d failures)\n", failures);
        return 1;
    }
    printf("RESULT PASS\n");
    return 0;
}
