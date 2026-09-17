/*
 * CANcestry HAL conformance: hardware-timestamp monotonicity.
 *
 * Verifies (SW-FR-HAL-004, HAL-TIMESTAMP-MONO-001):
 *   - Hardware timestamps injected by the HAL are strictly monotonic across
 *     all frames delivered on one interface.
 *   - If the backend produces a non-monotonic timestamp the HAL drops the
 *     offending frame and raises a TIMESTAMP_NON_MONOTONIC fault (fail-
 *     closed: SYS-SF-002, SW-FR-HAL-005).
 *   - Timestamps on enqueued CAN_RX events match the HAL's timestamp_us
 *     field (no jitter, no float conversion).
 *
 * Implements: SW-FR-HAL-004, SW-FR-HAL-005
 * Test ids:    HAL-TIMESTAMP-MONO-001
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

static int failures = 0;

static void expect(bool cond, const char *what)
{
    if (!cond) {
        fprintf(stderr, "FAIL: %s\n", what);
        failures++;
    }
}

int main(void)
{
    static cancestry_mock_hal_t mock;
    static cancestry_hal_t hal;
    static cancestry_virtual_clock_t vclock;
    cancestry_clock_t clock;
    static cancestry_hal_frame_t rx_storage[RING_CAP];
    static cancestry_hal_frame_t tx_storage[RING_CAP];
    cancestry_hal_rx_ring_t rx_ring;
    cancestry_hal_tx_ring_t tx_ring;
    static cancestry_event_t queue_storage[QUEUE_CAP];
    cancestry_event_queue_t queue;
    cancestry_hal_if_config_t iface;
    cancestry_hal_rx_ring_t *rx_rings[1];
    cancestry_hal_tx_ring_t *tx_rings[1];
    cancestry_hal_config_t config;
    uint8_t i;

    printf("HAL conformance: timestamp monotonicity (HAL-TIMESTAMP-MONO-001)\n");

    cancestry_mock_hal_init(&mock);
    /* Don't use auto-timestamps: we manually stamp to engineer the test. */
    expect(cancestry_hal_rx_ring_init(&rx_ring, rx_storage, RING_CAP), "rx init");
    expect(cancestry_hal_tx_ring_init(&tx_ring, tx_storage, RING_CAP), "tx init");
    cancestry_virtual_clock_init(&vclock, 0u);
    clock = cancestry_clock_from_virtual(&vclock);
    expect(cancestry_event_queue_init(&queue, queue_storage, QUEUE_CAP), "queue init");

    memset(&iface, 0, sizeof(iface));
    iface.interface_id = IFACE_CAN0;
    {
        const char *n = "vcan0";
        size_t k = 0u;
        while (k < CANCESTRY_HAL_INTERFACE_NAME_MAX - 1u && n[k] != '\0') {
            iface.name[k] = n[k];
            k++;
        }
        iface.name[k] = '\0';
    }
    iface.bitrate = 500000u;
    rx_rings[0] = &rx_ring;
    tx_rings[0] = &tx_ring;
    memset(&config, 0, sizeof(config));
    config.backend = cancestry_mock_hal_backend();
    config.backend_context = &mock;
    config.clock = &clock;
    config.ifaces = &iface;
    config.iface_count = 1u;
    config.rx_rings = rx_rings;
    config.tx_rings = tx_rings;
    expect(cancestry_hal_init(&hal, &config), "hal init");

    /* Inject 5 frames with strictly increasing timestamps 1000,2000,...,5000us. */
    for (i = 0u; i < 5u; ++i) {
        cancestry_hal_frame_t f;
        memset(&f, 0, sizeof(f));
        f.interface_id = IFACE_CAN0;
        f.can_id = 0x100u + i;
        f.length = 8u;
        f.data[0] = i;
        f.timestamp_us = (cancestry_time_us_t)(i + 1u) * 1000u;
        cancestry_mock_hal_inject_rx(&mock, 0u, &f);
    }
    /* Inject a frame with timestamp going BACKWARD (4000us, less than 5000). */
    {
        cancestry_hal_frame_t f;
        memset(&f, 0, sizeof(f));
        f.interface_id = IFACE_CAN0;
        f.can_id = 0x1FFu;
        f.length = 8u;
        f.timestamp_us = 4000u; /* regression */
        cancestry_mock_hal_inject_rx(&mock, 0u, &f);
    }
    /* Inject another frame with timestamp == previous (equal, not strictly greater). */
    {
        cancestry_hal_frame_t f;
        memset(&f, 0, sizeof(f));
        f.interface_id = IFACE_CAN0;
        f.can_id = 0x1FEu;
        f.length = 8u;
        f.timestamp_us = 5000u; /* equal to last valid -> must be dropped */
        cancestry_mock_hal_inject_rx(&mock, 0u, &f);
    }
    /* Finally a good frame at 6000 us. */
    {
        cancestry_hal_frame_t f;
        memset(&f, 0, sizeof(f));
        f.interface_id = IFACE_CAN0;
        f.can_id = 0x105u;
        f.length = 8u;
        f.timestamp_us = 6000u;
        cancestry_mock_hal_inject_rx(&mock, 0u, &f);
    }

    /* Poll. Expect:
     *   - 6 CAN_RX events with timestamps strictly increasing (1000..5000, then 6000)
     *   - 2 FAULT events for the two non-monotonic frames, delivered onto
     *     the queue directly by the HAL (timestamp faults are raised during
     *     frame delivery, not by the backend poll hook, so the out_fault_count
     *     only counts backend-level faults; we verify by reading the queue).
     */
    {
        uint32_t rx = 0u, flt = 0u;
        expect(cancestry_hal_status_is_ok(cancestry_hal_poll_rx(&hal, &queue, &rx, &flt)),
               "poll ok");
        expect(rx == 6u, "six CAN_RX delivered (5 initial + last good)");
        /* flt only counts backend-reported faults; timestamp violations are
         * raised inline during delivery and may not appear here. Count from
         * the queue directly. */
    }

    /* Verify the events on the queue: timestamps must be strictly increasing
     * across CAN_RX events (faults may interleave by priority; since faults
     * have higher priority they dequeue first). */
    {
        cancestry_time_us_t last_can_ts = 0u;
        uint32_t can_rx_seen = 0u;
        uint32_t fault_seen = 0u;
        bool fault_class_correct = true;
        while (!cancestry_event_queue_is_empty(&queue)) {
            cancestry_event_t ev;
            if (cancestry_event_queue_pop(&queue, &ev) != CANCESTRY_EVENT_QUEUE_OK) {
                break;
            }
            if (ev.type == CANCESTRY_EVENT_TYPE_CAN_RX) {
                can_rx_seen++;
                expect(ev.timestamp_us > last_can_ts,
                       "CAN_RX timestamps strictly increasing");
                last_can_ts = ev.timestamp_us;
            } else if (ev.type == CANCESTRY_EVENT_TYPE_FAULT_RAISED) {
                fault_seen++;
                if ((ev.payload.fault.fault_code & 0xFFFFu) !=
                    (uint32_t)CANCESTRY_HAL_FAULT_TIMESTAMP_NON_MONOTONIC) {
                    fault_class_correct = false;
                }
                expect(ev.priority_class == CANCESTRY_PRIORITY_CLASS_FAULT,
                       "fault priority is FAULT");
            }
        }
        expect(can_rx_seen == 6u, "six CAN_RX events popped");
        expect(fault_seen == 2u, "two FAULT events popped");
        expect(fault_class_correct, "both faults are TIMESTAMP_NON_MONOTONIC");
        expect(last_can_ts == 6000u, "last CAN_RX timestamp is 6000us");
    }

    if (failures != 0) {
        fprintf(stderr, "RESULT FAIL (%d failures)\n", failures);
        return 1;
    }
    printf("RESULT PASS\n");
    return 0;
}
