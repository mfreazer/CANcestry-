/*
 * CANcestry - Mock HAL backend.
 *
 * Zero heap allocation, strictly non-blocking, fully deterministic
 * (SYS-NF-001, SYS-NF-002).
 *
 * Implements: SW-FR-HAL-008, SW-FR-HAL-009, SW-FR-CANFD-003
 */

#include "mock_hal.h"

#include <string.h>

/* ------------------------------------------------------------------------- */
/* Backend callbacks                                                         */
/* ------------------------------------------------------------------------- */

static bool mock_hal_open(void *ctx, const cancestry_hal_if_config_t *cfg, uint8_t iface_index)
{
    cancestry_mock_hal_t *mock = (cancestry_mock_hal_t *)ctx;

    (void)cfg;
    if (mock == NULL || iface_index >= CANCESTRY_MOCK_HAL_MAX_INTERFACES) {
        return false;
    }
    if (iface_index >= mock->iface_count) {
        mock->iface_count = (uint8_t)(iface_index + 1u);
    }
    mock->ifaces[iface_index].opened = true;
    return true;
}

static void mock_hal_close(void *ctx, uint8_t iface_index)
{
    cancestry_mock_hal_t *mock = (cancestry_mock_hal_t *)ctx;

    if (mock == NULL || iface_index >= CANCESTRY_MOCK_HAL_MAX_INTERFACES) {
        return;
    }
    mock->ifaces[iface_index].opened = false;
}

/**
 * Report the negotiated transport capability of one mock interface.
 *
 * The mock is a bus simulator: it delivers exactly what the test injected,
 * including CAN FD frames on a classic-only interface. Rejecting those is
 * the core HAL's job (hal.c), which is precisely why the fallback test can
 * prove the contract against the same code path the real backend uses.
 */
static bool mock_hal_caps(void *ctx,
                          uint8_t iface_index,
                          cancestry_hal_transport_caps_t *out_caps)
{
    cancestry_mock_hal_t *mock = (cancestry_mock_hal_t *)ctx;

    if (mock == NULL || out_caps == NULL ||
        iface_index >= CANCESTRY_MOCK_HAL_MAX_INTERFACES) {
        return false;
    }
    out_caps->can_fd = mock->ifaces[iface_index].fd_support;
    out_caps->max_length = mock->ifaces[iface_index].fd_support
                               ? CANCESTRY_HAL_FRAME_MAX_LENGTH
                               : CANCESTRY_HAL_CLASSIC_FRAME_MAX_LENGTH;
    return true;
}

static cancestry_hal_status_t mock_hal_poll(void *ctx,
                                              uint8_t iface_index,
                                              cancestry_hal_rx_ring_t *ring,
                                              cancestry_hal_fault_code_t *out_fault)
{
    cancestry_mock_hal_t *mock = (cancestry_mock_hal_t *)ctx;
    cancestry_mock_iface_t *mif;

    if (mock == NULL || iface_index >= CANCESTRY_MOCK_HAL_MAX_INTERFACES || ring == NULL) {
        return CANCESTRY_HAL_ERR_NULL;
    }
    mif = &mock->ifaces[iface_index];
    mif->poll_call_count++;
    if (mif->inject_count > 0u && mif->inject_slots[0].is_fd != 0u) {
        /* Counted for the test's benefit only; the mock never drops. */
        mif->fd_frames_injected++;
    }

    /* Deliver scheduled fault, if any. */
    if (mif->next_poll_fault != CANCESTRY_HAL_FAULT_NONE) {
        if (out_fault != NULL) {
            *out_fault = mif->next_poll_fault;
        }
        mif->next_poll_fault = CANCESTRY_HAL_FAULT_NONE;
        /* Continue to deliver frames too: faults and data can coexist. */
    }

    /* Drain injection queue into the RX ring. */
    while (mif->inject_count > 0u) {
        cancestry_hal_frame_t frame = mif->inject_slots[0];
        cancestry_hal_status_t s;

        /* Shift the queue down (bounded copy; 256 max, acceptable for tests). */
        if (mif->inject_count > 1u) {
            memmove(&mif->inject_slots[0], &mif->inject_slots[1],
                    sizeof(cancestry_hal_frame_t) * ((size_t)mif->inject_count - 1u));
        }
        mif->inject_count--;

        /* Auto-timestamp if requested and frame doesn't already have one. */
        if (mif->inject_auto_timestamps && frame.timestamp_us == 0u) {
            frame.timestamp_us = mif->inject_next_timestamp;
            mif->inject_next_timestamp += mif->inject_timestamp_step;
        }

        s = cancestry_hal_rx_ring_push(ring, &frame);
        if (s == CANCESTRY_HAL_ERR_RING_FULL) {
            /* Injection queue outruns the ring: stop draining and leave the
             * frame to be re-injected next poll (deterministic). */
            /* Put the frame back at slot 0 so the test can observe overflow. */
            if (mif->inject_count > 0u) {
                memmove(&mif->inject_slots[1], &mif->inject_slots[0],
                        sizeof(cancestry_hal_frame_t) * (size_t)mif->inject_count);
            }
            mif->inject_slots[0] = frame;
            mif->inject_count++;
            break;
        }
    }
    return CANCESTRY_HAL_OK;
}

static cancestry_hal_status_t mock_hal_drain_tx(void *ctx,
                                                  uint8_t iface_index,
                                                  cancestry_hal_tx_ring_t *ring,
                                                  cancestry_hal_fault_code_t *out_fault)
{
    cancestry_mock_hal_t *mock = (cancestry_mock_hal_t *)ctx;
    cancestry_mock_iface_t *mif;

    if (mock == NULL || iface_index >= CANCESTRY_MOCK_HAL_MAX_INTERFACES || ring == NULL) {
        return CANCESTRY_HAL_ERR_NULL;
    }
    mif = &mock->ifaces[iface_index];
    mif->drain_call_count++;

    if (mif->next_drain_fault != CANCESTRY_HAL_FAULT_NONE) {
        if (out_fault != NULL) {
            *out_fault = mif->next_drain_fault;
        }
        mif->next_drain_fault = CANCESTRY_HAL_FAULT_NONE;
        /* On simulated interface-down fault, drop all pending TX. */
        if (*out_fault == CANCESTRY_HAL_FAULT_INTERFACE_DOWN ||
            *out_fault == CANCESTRY_HAL_FAULT_BUS_OFF) {
            cancestry_hal_frame_t discard;
            while (cancestry_hal_tx_ring_pop(ring, &discard) == CANCESTRY_HAL_OK) {
                ring->dropped++;
            }
            return CANCESTRY_HAL_OK;
        }
    }

    /* Consume the TX ring immediately - the mock bus is infinitely fast and
     * never back-pressures, which matches the "deterministic ideal" used in
     * the conformance tests. Real back-pressure is synthesised by injecting
     * faults through set_next_drain_fault(). */
    while (cancestry_hal_tx_ring_count(ring) > 0u) {
        cancestry_hal_frame_t frame;
        if (cancestry_hal_tx_ring_pop(ring, &frame) != CANCESTRY_HAL_OK) {
            break;
        }
        if (mif->tx_capture_count < CANCESTRY_MOCK_HAL_TX_CAPTURE_CAPACITY) {
            mif->tx_capture[mif->tx_capture_count] = frame;
            mif->tx_capture_count++;
        }
        mif->tx_total_captured++;
        ring->sent++;
    }
    return CANCESTRY_HAL_OK;
}

/* ------------------------------------------------------------------------- */
/* Public API                                                                */
/* ------------------------------------------------------------------------- */

static const cancestry_hal_backend_t mock_backend = {
    mock_hal_open,
    mock_hal_close,
    mock_hal_poll,
    mock_hal_drain_tx,
    mock_hal_caps,
};

const cancestry_hal_backend_t *cancestry_mock_hal_backend(void)
{
    return &mock_backend;
}

void cancestry_mock_hal_init(cancestry_mock_hal_t *mock)
{
    uint8_t i;

    if (mock == NULL) {
        return;
    }
    memset(mock, 0, sizeof(*mock));
    for (i = 0u; i < CANCESTRY_MOCK_HAL_MAX_INTERFACES; ++i) {
        mock->ifaces[i].inject_timestamp_step = 1000u; /* 1 ms default */
    }
}

bool cancestry_mock_hal_inject_rx(cancestry_mock_hal_t *mock,
                                   uint8_t iface_index,
                                   const cancestry_hal_frame_t *frame)
{
    cancestry_mock_iface_t *mif;

    if (mock == NULL || frame == NULL || iface_index >= CANCESTRY_MOCK_HAL_MAX_INTERFACES) {
        return false;
    }
    mif = &mock->ifaces[iface_index];
    if (mif->inject_count >= CANCESTRY_MOCK_HAL_INJECT_CAPACITY) {
        return false;
    }
    mif->inject_slots[mif->inject_count] = *frame;
    mif->inject_count++;
    return true;
}

void cancestry_mock_hal_set_next_poll_fault(cancestry_mock_hal_t *mock,
                                              uint8_t iface_index,
                                              cancestry_hal_fault_code_t fault)
{
    if (mock == NULL || iface_index >= CANCESTRY_MOCK_HAL_MAX_INTERFACES) {
        return;
    }
    mock->ifaces[iface_index].force_poll_fault = (fault != CANCESTRY_HAL_FAULT_NONE);
    mock->ifaces[iface_index].next_poll_fault = fault;
}

void cancestry_mock_hal_set_next_drain_fault(cancestry_mock_hal_t *mock,
                                               uint8_t iface_index,
                                               cancestry_hal_fault_code_t fault)
{
    if (mock == NULL || iface_index >= CANCESTRY_MOCK_HAL_MAX_INTERFACES) {
        return;
    }
    mock->ifaces[iface_index].force_drain_fault = (fault != CANCESTRY_HAL_FAULT_NONE);
    mock->ifaces[iface_index].next_drain_fault = fault;
}

void cancestry_mock_hal_set_fd_support(cancestry_mock_hal_t *mock,
                                        uint8_t iface_index,
                                        bool can_fd)
{
    if (mock == NULL || iface_index >= CANCESTRY_MOCK_HAL_MAX_INTERFACES) {
        return;
    }
    mock->ifaces[iface_index].fd_support = can_fd;
}

bool cancestry_mock_hal_fd_support(const cancestry_mock_hal_t *mock, uint8_t iface_index)
{
    if (mock == NULL || iface_index >= CANCESTRY_MOCK_HAL_MAX_INTERFACES) {
        return false;
    }
    return mock->ifaces[iface_index].fd_support;
}

void cancestry_mock_hal_set_auto_timestamps(cancestry_mock_hal_t *mock,
                                              uint8_t iface_index,
                                              cancestry_time_us_t start,
                                              cancestry_time_us_t step)
{
    if (mock == NULL || iface_index >= CANCESTRY_MOCK_HAL_MAX_INTERFACES) {
        return;
    }
    mock->ifaces[iface_index].inject_auto_timestamps = true;
    mock->ifaces[iface_index].inject_next_timestamp = start;
    mock->ifaces[iface_index].inject_timestamp_step = step;
}

uint16_t cancestry_mock_hal_tx_count(const cancestry_mock_hal_t *mock, uint8_t iface_index)
{
    if (mock == NULL || iface_index >= CANCESTRY_MOCK_HAL_MAX_INTERFACES) {
        return 0u;
    }
    return mock->ifaces[iface_index].tx_capture_count;
}

const cancestry_hal_frame_t *cancestry_mock_hal_tx_at(const cancestry_mock_hal_t *mock,
                                                        uint8_t iface_index,
                                                        uint16_t slot)
{
    if (mock == NULL || iface_index >= CANCESTRY_MOCK_HAL_MAX_INTERFACES) {
        return NULL;
    }
    if (slot >= mock->ifaces[iface_index].tx_capture_count) {
        return NULL;
    }
    return &mock->ifaces[iface_index].tx_capture[slot];
}
