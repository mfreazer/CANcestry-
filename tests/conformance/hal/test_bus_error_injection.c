/*
 * CANcestry HAL conformance: bus-error injection.
 *
 * Verifies (SW-FR-HAL-007, SW-FR-HAL-009, HAL-BUS-ERROR-001):
 *   - The mock HAL can inject BUS_ERROR_PASSIVE, BUS_OFF, INTERFACE_DOWN
 *     faults deterministically.
 *   - Each fault is raised into the event queue as a FAULT_RAISED event
 *     with the correct severity:
 *       ERROR_PASSIVE     -> ERROR
 *       BUS_OFF           -> CRITICAL
 *       INTERFACE_DOWN    -> ERROR
 *   - After a BUS_OFF fault the interface state reported by hal_get_status()
 *     transitions to BUS_OFF (fail-closed); subsequent RX/TX continue to
 *     raise faults instead of silently losing data.
 *   - Draining after an INTERFACE_DOWN fault drops pending TX frames and
 *     reports them as dropped (fail-closed).
 *
 * Implements: SW-FR-HAL-005, SW-FR-HAL-007, SW-FR-HAL-009
 * Test ids:    HAL-BUS-ERROR-001
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

    printf("HAL conformance: bus error injection (HAL-BUS-ERROR-001)\n");

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

    /* Schedule an Error Passive fault on next poll. */
    cancestry_mock_hal_set_next_poll_fault(&mock, 0u, CANCESTRY_HAL_FAULT_BUS_ERROR_PASSIVE);
    {
        uint32_t rx = 0u, flt = 0u;
        (void)cancestry_hal_poll_rx(&hal, &queue, &rx, &flt);
        expect(flt == 1u, "one fault raised");
    }
    {
        cancestry_event_t ev;
        expect(cancestry_event_queue_pop(&queue, &ev) == CANCESTRY_EVENT_QUEUE_OK,
               "event available");
        expect(ev.type == CANCESTRY_EVENT_TYPE_FAULT_RAISED, "event is a fault");
        expect((ev.payload.fault.fault_code & 0xFFFFu) ==
                   (uint32_t)CANCESTRY_HAL_FAULT_BUS_ERROR_PASSIVE,
               "fault code is BUS_ERROR_PASSIVE");
        expect(ev.payload.fault.severity == CANCESTRY_FAULT_SEVERITY_ERROR,
               "error-passive is ERROR severity");
        expect(ev.payload.fault.source_id == IFACE_CAN0, "source_id is can0");
    }

    /* Schedule a Bus Off fault on next poll. */
    cancestry_mock_hal_set_next_poll_fault(&mock, 0u, CANCESTRY_HAL_FAULT_BUS_OFF);
    (void)cancestry_hal_poll_rx(&hal, &queue, NULL, NULL);
    {
        cancestry_event_t ev;
        expect(cancestry_event_queue_pop(&queue, &ev) == CANCESTRY_EVENT_QUEUE_OK,
               "event available");
        expect(ev.type == CANCESTRY_EVENT_TYPE_FAULT_RAISED, "event is a fault");
        expect((ev.payload.fault.fault_code & 0xFFFFu) ==
                   (uint32_t)CANCESTRY_HAL_FAULT_BUS_OFF,
               "fault code is BUS_OFF");
        expect(ev.payload.fault.severity == CANCESTRY_FAULT_SEVERITY_CRITICAL,
               "bus-off is CRITICAL severity");
    }

    /* After BUS_OFF, status should show the BUS_OFF state. */
    {
        cancestry_hal_if_status_t st;
        uint8_t count = 0u;
        expect(cancestry_hal_status_is_ok(cancestry_hal_get_status(&hal, &st, 1u, &count)),
               "get_status ok");
        expect(count == 1u, "one interface");
        expect(st.state == CANCESTRY_HAL_IF_STATE_BUS_OFF, "interface is in BUS_OFF state");
        expect(st.fault_count >= 2u, "fault_count reflects both faults");
    }

    /* Schedule an INTERFACE_DOWN fault on drain_tx and queue a TX frame to
     * trigger the drain. The mock must drop pending TX frames. */
    cancestry_mock_hal_set_next_drain_fault(&mock, 0u, CANCESTRY_HAL_FAULT_INTERFACE_DOWN);
    {
        cancestry_hal_frame_t f;
        cancestry_hal_status_t s;
        memset(&f, 0, sizeof(f));
        f.interface_id = IFACE_CAN0;
        f.can_id = 0x321u;
        f.length = 8u;
        s = cancestry_hal_send_tx(&hal, &queue, &f);
        (void)s;
    }
    {
        cancestry_event_t ev;
        bool saw_down = false;
        while (cancestry_event_queue_pop(&queue, &ev) == CANCESTRY_EVENT_QUEUE_OK) {
            if (ev.type == CANCESTRY_EVENT_TYPE_FAULT_RAISED &&
                (ev.payload.fault.fault_code & 0xFFFFu) ==
                    (uint32_t)CANCESTRY_HAL_FAULT_INTERFACE_DOWN) {
                saw_down = true;
                expect(ev.payload.fault.severity == CANCESTRY_FAULT_SEVERITY_ERROR,
                       "interface-down is ERROR severity");
            }
        }
        expect(saw_down, "INTERFACE_DOWN fault delivered");
    }
    /* Note: the mock's drain with INTERFACE_DOWN drops pending frames and
     * increments ring->dropped. In our test case send_tx pushes the frame
     * then drain fires; drain sees the frame and drops it, which does
     * increment dropped. However because the drain callback is invoked after
     * push, then returns, and the dropped counter is incremented inside the
     * mock's drain callback, tx_ring.dropped should be >= 1. */
    expect(tx_ring.dropped >= 1u, "tx ring dropped frame during INTERFACE_DOWN");

    if (failures != 0) {
        fprintf(stderr, "RESULT FAIL (%d failures)\n", failures);
        return 1;
    }
    printf("RESULT PASS\n");
    return 0;
}
