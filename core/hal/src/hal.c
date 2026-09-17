/*
 * CANcestry - HAL: core implementation (platform-independent logic).
 *
 * This file implements the dispatch logic: convert RX ring frames into
 * core/event CAN_RX events, shape HAL faults into FAULT_RAISED events with
 * the correct priority, and perform the timestamp monotonicity check
 * required by SW-FR-HAL-004 and constraint 3 of issue #17.
 *
 * The actual I/O lives behind the backend vtable; this file makes no
 * syscalls and performs no heap allocation (SYS-NF-002).
 *
 * Implements: SW-FR-HAL-003 (non-blocking dispatch),
 *             SW-FR-HAL-004 (hardware timestamps mapped to event time base),
 *             SW-FR-HAL-005 (fail-closed I/O),
 *             SW-FR-HAL-006 (bounded rings),
 *             SW-FR-HAL-007 (bus fault events)
 */

#include "cancestry/hal/hal.h"

#include <string.h>

/* ------------------------------------------------------------------------- */
/* Helpers                                                                   */
/* ------------------------------------------------------------------------- */

/**
 * Translate a HAL fault code into an event fault severity.
 *
 * Severity mapping follows docs/system/mode-fault-state-machine.md section 4
 * and the fail-closed policy: ring overflow is a WARNING, anything that
 * suggests loss of bus control is ERROR or CRITICAL.
 */
static cancestry_fault_severity_t hal_fault_severity(cancestry_hal_fault_code_t fault)
{
    switch (fault) {
    case CANCESTRY_HAL_FAULT_RX_RING_OVERFLOW:
    case CANCESTRY_HAL_FAULT_TX_RING_OVERFLOW:
    case CANCESTRY_HAL_FAULT_BUS_BACKPRESSURE:
    case CANCESTRY_HAL_FAULT_MALFORMED_FRAME:
        return CANCESTRY_FAULT_SEVERITY_WARNING;
    case CANCESTRY_HAL_FAULT_TIMESTAMP_NON_MONOTONIC:
    case CANCESTRY_HAL_FAULT_INTERFACE_DOWN:
    case CANCESTRY_HAL_FAULT_BUS_ERROR_PASSIVE:
    case CANCESTRY_HAL_FAULT_INIT_FAILED:
        return CANCESTRY_FAULT_SEVERITY_ERROR;
    case CANCESTRY_HAL_FAULT_BUS_OFF:
        return CANCESTRY_FAULT_SEVERITY_CRITICAL;
    case CANCESTRY_HAL_FAULT_NONE:
    case CANCESTRY_HAL_FAULT_COUNT:
    default:
        return CANCESTRY_FAULT_SEVERITY_INFO;
    }
}

/**
 * Find the index within hal->ifaces[] for @p interface_id, or return the
 * iface_count sentinel when not found.
 */
static uint8_t hal_find_iface(const cancestry_hal_t *hal, cancestry_interface_id_t interface_id)
{
    uint8_t i;

    for (i = 0u; i < hal->iface_count; ++i) {
        if (hal->ifaces[i].interface_id == interface_id) {
            return i;
        }
    }
    return hal->iface_count;
}

/**
 * Push one CAN_RX event for a HAL frame into @p queue.
 *
 * Enforces the timestamp-monotonicity invariant: if the new frame's
 * timestamp is not strictly greater than the previous timestamp observed
 * on this interface, a TIMESTAMP_NON_MONOTONIC fault is raised and the
 * frame is dropped (SW-FR-HAL-004).
 *
 * @return true when the frame was delivered as a CAN_RX event, false when
 *         it was dropped due to a monotonicity violation.
 */
static bool hal_deliver_rx(cancestry_hal_t *hal,
                           cancestry_event_queue_t *queue,
                           uint8_t iface_index,
                           const cancestry_hal_frame_t *frame)
{
    cancestry_event_t event;
    cancestry_event_payload_t payload;
    cancestry_event_queue_status_t qstatus;

    /* Monotonicity check. */
    if (frame->timestamp_us != 0u &&
        hal->if_last_ts[iface_index] != 0u &&
        frame->timestamp_us <= hal->if_last_ts[iface_index]) {
        (void)cancestry_hal_raise_fault(hal, queue,
                                         hal->ifaces[iface_index].interface_id,
                                         CANCESTRY_HAL_FAULT_TIMESTAMP_NON_MONOTONIC,
                                         CANCESTRY_FAULT_SEVERITY_ERROR);
        return false;
    }
    if (frame->timestamp_us != 0u) {
        hal->if_last_ts[iface_index] = frame->timestamp_us;
    }

    memset(&payload, 0, sizeof(payload));
    payload.can_rx.interface_id = frame->interface_id;
    payload.can_rx.can_id = frame->can_id;
    payload.can_rx.is_extended = frame->is_extended;
    payload.can_rx.length = frame->length;
    memcpy(payload.can_rx.data, frame->data, CANCESTRY_CAN_FRAME_MAX_LENGTH);

    cancestry_event_init(&event);
    event.type = CANCESTRY_EVENT_TYPE_CAN_RX;
    event.priority_class = CANCESTRY_PRIORITY_CLASS_CAN_RX;
    event.timestamp_us = (frame->timestamp_us != 0u)
                             ? frame->timestamp_us
                             : cancestry_clock_now_us(&hal->clock);
    event.payload = payload;

    qstatus = cancestry_event_queue_push(queue, &event);
    if (!cancestry_event_queue_status_is_ok(qstatus)) {
        /* Queue rejected the event (full or invalid). The queue's counters
         * already recorded this; we don't double-count. Treat as delivered
         * at the HAL level; the queue layer owns drop semantics. */
        return true;
    }
    return true;
}

/* ------------------------------------------------------------------------- */
/* Public API                                                                */
/* ------------------------------------------------------------------------- */

bool cancestry_hal_init(cancestry_hal_t *hal, const cancestry_hal_config_t *config)
{
    uint8_t i;

    if (hal == NULL || config == NULL) {
        return false;
    }
    if (config->backend == NULL || config->clock == NULL ||
        !cancestry_clock_is_valid(config->clock)) {
        return false;
    }
    if (config->iface_count == 0u ||
        config->iface_count > CANCESTRY_HAL_INSTANCE_MAX_INTERFACES) {
        return false;
    }
    if (config->ifaces == NULL) {
        return false;
    }

    memset(hal, 0, sizeof(*hal));
    hal->backend = config->backend;
    hal->backend_context = config->backend_context;
    hal->clock = *config->clock;
    hal->iface_count = config->iface_count;

    for (i = 0u; i < config->iface_count; ++i) {
        hal->ifaces[i] = config->ifaces[i];
        hal->rx_rings[i] = (config->rx_rings != NULL) ? config->rx_rings[i] : NULL;
        hal->tx_rings[i] = (config->tx_rings != NULL) ? config->tx_rings[i] : NULL;
        hal->if_states[i] = CANCESTRY_HAL_IF_STATE_DOWN;
        hal->if_last_ts[i] = 0u;
        hal->if_faults[i] = 0u;
        hal->if_last_fault[i] = CANCESTRY_HAL_FAULT_NONE;
    }

    /* Open every interface through the backend. If any open fails we record
     * a fault and return false (fail-closed: SW-FR-HAL-005). */
    for (i = 0u; i < config->iface_count; ++i) {
        if (hal->backend->open != NULL &&
            hal->backend->open(hal->backend_context, &hal->ifaces[i], i)) {
            hal->if_states[i] = CANCESTRY_HAL_IF_STATE_UP;
        } else {
            hal->if_states[i] = CANCESTRY_HAL_IF_STATE_DOWN;
            hal->if_last_fault[i] = CANCESTRY_HAL_FAULT_INIT_FAILED;
            hal->if_faults[i]++;
            /* Close any interfaces we did manage to open, to keep state
             * consistent on failure. */
            while (i > 0u) {
                --i;
                if (hal->backend->close != NULL) {
                    hal->backend->close(hal->backend_context, i);
                }
                hal->if_states[i] = CANCESTRY_HAL_IF_STATE_DOWN;
            }
            return false;
        }
    }

    hal->initialized = true;
    return true;
}

cancestry_event_queue_status_t cancestry_hal_raise_fault(cancestry_hal_t *hal,
                                                         cancestry_event_queue_t *event_queue,
                                                         cancestry_interface_id_t interface_id,
                                                         cancestry_hal_fault_code_t fault,
                                                         cancestry_fault_severity_t severity)
{
    cancestry_event_t event;
    cancestry_event_payload_t payload;
    uint8_t idx;

    if (hal == NULL || event_queue == NULL) {
        return CANCESTRY_EVENT_QUEUE_ERR_NULL;
    }
    idx = hal_find_iface(hal, interface_id);
    if (idx < hal->iface_count) {
        hal->if_faults[idx]++;
        hal->if_last_fault[idx] = fault;
        /* Update interface state for terminal faults. */
        if (fault == CANCESTRY_HAL_FAULT_BUS_ERROR_PASSIVE) {
            hal->if_states[idx] = CANCESTRY_HAL_IF_STATE_ERROR_PASSIVE;
        } else if (fault == CANCESTRY_HAL_FAULT_BUS_OFF ||
                   fault == CANCESTRY_HAL_FAULT_INTERFACE_DOWN) {
            hal->if_states[idx] = CANCESTRY_HAL_IF_STATE_BUS_OFF;
        }
    }

    memset(&payload, 0, sizeof(payload));
    /* Encode the (interface_id << 16) | fault_code into fault_code so the
     * FSM can disambiguate sources without a separate allocator-owned
     * string. The fault code is still statically resolvable via the name
     * table in types.c. */
    payload.fault.fault_code = ((uint32_t)interface_id << 16u) | (uint32_t)fault;
    payload.fault.severity = severity;
    payload.fault.source_id = interface_id;

    cancestry_event_init(&event);
    event.type = CANCESTRY_EVENT_TYPE_FAULT_RAISED;
    event.priority_class = CANCESTRY_PRIORITY_CLASS_FAULT;
    event.timestamp_us = cancestry_clock_now_us(&hal->clock);
    event.payload = payload;

    return cancestry_event_queue_push(event_queue, &event);
}

cancestry_hal_status_t cancestry_hal_poll_rx(cancestry_hal_t *hal,
                                              cancestry_event_queue_t *event_queue,
                                              uint32_t *out_rx_count,
                                              uint32_t *out_fault_count)
{
    uint32_t rx_count = 0u;
    uint32_t fault_count = 0u;
    uint8_t i;

    if (hal == NULL || event_queue == NULL) {
        return CANCESTRY_HAL_ERR_NULL;
    }
    if (!hal->initialized) {
        return CANCESTRY_HAL_ERR_NULL;
    }

    for (i = 0u; i < hal->iface_count; ++i) {
        cancestry_hal_rx_ring_t *ring = hal->rx_rings[i];
        cancestry_hal_fault_code_t fault = CANCESTRY_HAL_FAULT_NONE;

        if (ring == NULL) {
            continue;
        }

        /* 1. Let the backend push any newly arrived frames into the ring. */
        if (hal->backend->poll != NULL) {
            (void)hal->backend->poll(hal->backend_context, i, ring, &fault);
        }

        /* 2. Raise any backend-reported fault before delivering data so that
         *    FSMs see faults at higher priority (they pre-empt CAN_RX events
         *    by priority class, per event-ordering.md section 3). */
        if (fault != CANCESTRY_HAL_FAULT_NONE) {
            (void)cancestry_hal_raise_fault(hal, event_queue, hal->ifaces[i].interface_id,
                                             fault, hal_fault_severity(fault));
            fault_count++;
        }

        /* 3. Drain RX ring -> event queue. The backend may not know the
         *    logical interface_id (it works with physical indices), so we
         *    stamp it here before delivery. */
        while (cancestry_hal_rx_ring_count(ring) > 0u) {
            cancestry_hal_frame_t frame;
            cancestry_hal_status_t s = cancestry_hal_rx_ring_pop(ring, &frame);

            if (!cancestry_hal_status_is_ok(s)) {
                break;
            }
            frame.interface_id = hal->ifaces[i].interface_id;
            if (hal_deliver_rx(hal, event_queue, i, &frame)) {
                rx_count++;
            }
        }

        /* 4. Drain TX ring via backend (opportunistic non-blocking drain). */
        if (hal->tx_rings[i] != NULL && hal->backend->drain_tx != NULL) {
            cancestry_hal_fault_code_t tx_fault = CANCESTRY_HAL_FAULT_NONE;
            (void)hal->backend->drain_tx(hal->backend_context, i, hal->tx_rings[i],
                                         &tx_fault);
            if (tx_fault != CANCESTRY_HAL_FAULT_NONE) {
                (void)cancestry_hal_raise_fault(hal, event_queue,
                                                 hal->ifaces[i].interface_id,
                                                 tx_fault, hal_fault_severity(tx_fault));
                fault_count++;
            }
        }

        /* 5. If the RX ring reported drops during backend poll, raise a
         *    single RX_RING_OVERFLOW fault summarising them. */
        if (ring->dropped > 0u) {
            (void)cancestry_hal_raise_fault(hal, event_queue, hal->ifaces[i].interface_id,
                                             CANCESTRY_HAL_FAULT_RX_RING_OVERFLOW,
                                             CANCESTRY_FAULT_SEVERITY_WARNING);
            fault_count++;
            /* Reset the dropped counter so we raise at most one overflow
             * fault per poll; counters are still monotonic via rx_total. */
            ring->dropped = 0u;
        }
    }

    if (out_rx_count != NULL) {
        *out_rx_count = rx_count;
    }
    if (out_fault_count != NULL) {
        *out_fault_count = fault_count;
    }
    return CANCESTRY_HAL_OK;
}

cancestry_hal_status_t cancestry_hal_send_tx(cancestry_hal_t *hal,
                                              cancestry_event_queue_t *event_queue,
                                              const cancestry_hal_frame_t *frame)
{
    uint8_t idx;
    cancestry_hal_tx_ring_t *ring;
    cancestry_hal_status_t status;

    if (hal == NULL || event_queue == NULL || frame == NULL) {
        return CANCESTRY_HAL_ERR_NULL;
    }
    if (!hal->initialized) {
        return CANCESTRY_HAL_ERR_NULL;
    }
    idx = hal_find_iface(hal, frame->interface_id);
    if (idx >= hal->iface_count) {
        return CANCESTRY_HAL_ERR_NO_INTERFACE;
    }
    ring = hal->tx_rings[idx];
    if (ring == NULL) {
        return CANCESTRY_HAL_ERR_NO_INTERFACE;
    }

    status = cancestry_hal_tx_ring_push(ring, frame);
    if (status == CANCESTRY_HAL_ERR_RING_FULL) {
        (void)cancestry_hal_raise_fault(hal, event_queue, frame->interface_id,
                                         CANCESTRY_HAL_FAULT_TX_RING_OVERFLOW,
                                         CANCESTRY_FAULT_SEVERITY_WARNING);
        return status;
    }
    if (!cancestry_hal_status_is_ok(status)) {
        return status;
    }

    /* Opportunistically drain; any backend fault is raised into the queue. */
    if (hal->backend->drain_tx != NULL) {
        cancestry_hal_fault_code_t tx_fault = CANCESTRY_HAL_FAULT_NONE;
        (void)hal->backend->drain_tx(hal->backend_context, idx, ring, &tx_fault);
        if (tx_fault != CANCESTRY_HAL_FAULT_NONE) {
            (void)cancestry_hal_raise_fault(hal, event_queue, frame->interface_id,
                                             tx_fault, hal_fault_severity(tx_fault));
        }
    }
    return CANCESTRY_HAL_OK;
}

cancestry_hal_status_t cancestry_hal_get_status(const cancestry_hal_t *hal,
                                                 cancestry_hal_if_status_t *out,
                                                 uint8_t capacity,
                                                 uint8_t *out_count)
{
    uint8_t i;
    uint8_t written = 0u;

    if (hal == NULL || out == NULL) {
        return CANCESTRY_HAL_ERR_NULL;
    }
    for (i = 0u; i < hal->iface_count && written < capacity; ++i) {
        cancestry_hal_if_status_t *s = &out[written];
        const cancestry_hal_rx_ring_t *rx = hal->rx_rings[i];
        const cancestry_hal_tx_ring_t *tx = hal->tx_rings[i];

        memset(s, 0, sizeof(*s));
        s->interface_id = hal->ifaces[i].interface_id;
        {
            size_t n = 0u;
            const char *name = hal->ifaces[i].name;
            while (n < CANCESTRY_HAL_INTERFACE_NAME_MAX - 1u && name[n] != '\0') {
                s->name[n] = name[n];
                n++;
            }
            s->name[n] = '\0';
        }
        s->state = hal->if_states[i];
        s->fault_count = hal->if_faults[i];
        s->last_fault = hal->if_last_fault[i];
        s->last_rx_timestamp_us = hal->if_last_ts[i];
        s->rx_count = (rx != NULL) ? rx->rx_total : 0u;
        s->rx_dropped = (rx != NULL) ? rx->dropped : 0u;
        s->tx_count = (tx != NULL) ? tx->sent : 0u;
        s->tx_dropped = (tx != NULL) ? tx->dropped : 0u;
        written++;
    }
    if (out_count != NULL) {
        *out_count = written;
    }
    return CANCESTRY_HAL_OK;
}
