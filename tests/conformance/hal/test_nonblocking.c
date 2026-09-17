/*
 * CANcestry HAL conformance: non-blocking contract.
 *
 * Verifies (SW-FR-HAL-003, HAL-NONBLOCK-001):
 *   - hal_poll_rx returns immediately when no frames are available (no
 *     blocking, no spinning waiting for input).
 *   - hal_poll_rx called repeatedly with no injected frames consumes zero
 *     frames and raises no faults (idempotent non-blocking).
 *   - hal_poll_rx can be safely called many times (10000) without
 *     deadlocking or raising spurious faults, proving the backend is
 *     strictly non-blocking.
 *
 * We can't easily assert wall-clock timing portably, but the mock backend
 * is structurally non-blocking (no select/epoll with infinite timeout, no
 * blocking reads) and this test proves repeated polling is safe and
 * bounded.
 *
 * Implements: SW-FR-HAL-003
 * Test ids:    HAL-NONBLOCK-001
 */

#include "cancestry/event/clock.h"
#include "cancestry/event/queue.h"
#include "cancestry/event/types.h"
#include "cancestry/hal/hal.h"
#include "cancestry/hal/types.h"
#include "mock_hal.h"

#include <stdio.h>
#include <string.h>

#define RING_CAP ((uint16_t)32u)
#define QUEUE_CAP ((uint16_t)256u)
#define IFACE_CAN0 ((cancestry_interface_id_t)1u)
#define ITERATIONS ((uint32_t)10000u)

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
    uint32_t i;
    uint32_t total_rx = 0u;
    uint32_t total_faults = 0u;

    printf("HAL conformance: non-blocking poll (HAL-NONBLOCK-001)\n");

    cancestry_mock_hal_init(&mock);
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

    /* Call poll many times without injecting any frames. */
    for (i = 0u; i < ITERATIONS; ++i) {
        uint32_t rx = 0u, flt = 0u;
        cancestry_hal_status_t s = cancestry_hal_poll_rx(&hal, &queue, &rx, &flt);
        expect(cancestry_hal_status_is_ok(s), "poll returns ok");
        total_rx += rx;
        total_faults += flt;
        cancestry_virtual_clock_advance(&vclock, 100u);
    }
    expect(total_rx == 0u, "zero RX frames after polling with no injects");
    expect(total_faults == 0u, "zero faults after polling with no injects");
    expect(cancestry_event_queue_is_empty(&queue), "queue is empty");
    expect(cancestry_hal_rx_ring_count(&rx_ring) == 0u, "rx ring empty");

    /* Inject one frame and poll ONCE; verify it's delivered. Then poll many
     * more times and confirm nothing extra appears. */
    {
        cancestry_hal_frame_t f;
        uint32_t rx = 0u, flt = 0u;
        memset(&f, 0, sizeof(f));
        f.interface_id = IFACE_CAN0;
        f.can_id = 0x123u;
        f.length = 8u;
        f.timestamp_us = 1u;
        cancestry_mock_hal_inject_rx(&mock, 0u, &f);
        (void)cancestry_hal_poll_rx(&hal, &queue, &rx, &flt);
        expect(rx == 1u, "one frame delivered");
        expect(flt == 0u, "no fault");
    }
    for (i = 0u; i < ITERATIONS; ++i) {
        uint32_t rx = 0u, flt = 0u;
        (void)cancestry_hal_poll_rx(&hal, &queue, &rx, &flt);
        expect(rx == 0u, "no more frames");
        expect(flt == 0u, "no more faults");
        cancestry_virtual_clock_advance(&vclock, 100u);
    }
    /* Queue should have exactly one CAN_RX event remaining (we never popped). */
    {
        uint32_t remaining = 0u;
        while (!cancestry_event_queue_is_empty(&queue)) {
            cancestry_event_t ev;
            if (cancestry_event_queue_pop(&queue, &ev) != CANCESTRY_EVENT_QUEUE_OK) {
                break;
            }
            if (ev.type == CANCESTRY_EVENT_TYPE_CAN_RX) {
                remaining++;
                expect(ev.payload.can_rx.can_id == 0x123u, "delivered correct can_id");
            }
        }
        expect(remaining == 1u, "exactly one CAN_RX event queued total");
    }

    /* send_tx must also be non-blocking; sending many frames while drain is
     * not possible (mock drains, but if TX ring fills it returns ERR_RING_FULL
     * immediately without blocking). */
    {
        cancestry_hal_frame_t f;
        uint32_t rejects = 0u;
        memset(&f, 0, sizeof(f));
        f.interface_id = IFACE_CAN0;
        f.length = 8u;
        for (i = 0u; i < (uint32_t)RING_CAP * 4u; ++i) {
            f.can_id = 0x200u + i;
            /* Mock drains immediately so TX ring never fills; send_tx always
             * succeeds. That still proves send_tx doesn't block. */
            if (!cancestry_hal_status_is_ok(cancestry_hal_send_tx(&hal, &queue, &f))) {
                rejects++;
            }
        }
        /* With the mock backend draining instantly all sends succeed. */
        expect(rejects == 0u, "no spurious rejects under infinite mock bandwidth");
    }

    if (failures != 0) {
        fprintf(stderr, "RESULT FAIL (%d failures)\n", failures);
        return 1;
    }
    printf("RESULT PASS\n");
    return 0;
}
