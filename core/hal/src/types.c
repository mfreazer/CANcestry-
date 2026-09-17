/*
 * CANcestry - HAL: ring helpers and status-name tables.
 *
 * All tables here are statically allocated and read-only. The ring helpers
 * operate on caller-owned storage and never allocate (SYS-NF-002).
 *
 * Implements: SW-FR-HAL-001, SW-FR-HAL-002, SW-FR-HAL-006 (bounded rings),
 *             SW-FR-CANFD-002 (frame validation)
 */

#include "cancestry/hal/types.h"

#include <string.h>

/* ------------------------------------------------------------------------- */
/* RX ring                                                                   */
/* ------------------------------------------------------------------------- */

bool cancestry_hal_rx_ring_init(cancestry_hal_rx_ring_t *ring,
                                cancestry_hal_frame_t *storage,
                                uint16_t capacity)
{
    if (ring == NULL || storage == NULL || capacity == 0u) {
        return false;
    }
    memset(ring, 0, sizeof(*ring));
    ring->slots = storage;
    ring->capacity = capacity;
    return true;
}

uint16_t cancestry_hal_rx_ring_count(const cancestry_hal_rx_ring_t *ring)
{
    if (ring == NULL) {
        return 0u;
    }
    return ring->count;
}

cancestry_hal_status_t cancestry_hal_rx_ring_push(cancestry_hal_rx_ring_t *ring,
                                                   const cancestry_hal_frame_t *frame)
{
    if (ring == NULL || frame == NULL) {
        return CANCESTRY_HAL_ERR_NULL;
    }
    if (ring->slots == NULL || ring->capacity == 0u) {
        return CANCESTRY_HAL_ERR_ARGUMENT;
    }
    if (ring->count >= ring->capacity) {
        ring->dropped++;
        return CANCESTRY_HAL_ERR_RING_FULL;
    }
    ring->slots[ring->write_idx] = *frame;
    ring->write_idx = (uint16_t)((ring->write_idx + 1u) % ring->capacity);
    ring->count++;
    ring->rx_total++;
    return CANCESTRY_HAL_OK;
}

cancestry_hal_status_t cancestry_hal_rx_ring_pop(cancestry_hal_rx_ring_t *ring,
                                                  cancestry_hal_frame_t *out_frame)
{
    if (ring == NULL || out_frame == NULL) {
        return CANCESTRY_HAL_ERR_NULL;
    }
    if (ring->count == 0u || ring->slots == NULL) {
        return CANCESTRY_HAL_OK_IDLE;
    }
    *out_frame = ring->slots[ring->read_idx];
    memset(&ring->slots[ring->read_idx], 0, sizeof(ring->slots[ring->read_idx]));
    ring->read_idx = (uint16_t)((ring->read_idx + 1u) % ring->capacity);
    ring->count--;
    return CANCESTRY_HAL_OK;
}

/* ------------------------------------------------------------------------- */
/* TX ring                                                                   */
/* ------------------------------------------------------------------------- */

bool cancestry_hal_tx_ring_init(cancestry_hal_tx_ring_t *ring,
                                cancestry_hal_frame_t *storage,
                                uint16_t capacity)
{
    if (ring == NULL || storage == NULL || capacity == 0u) {
        return false;
    }
    memset(ring, 0, sizeof(*ring));
    ring->slots = storage;
    ring->capacity = capacity;
    return true;
}

uint16_t cancestry_hal_tx_ring_count(const cancestry_hal_tx_ring_t *ring)
{
    if (ring == NULL) {
        return 0u;
    }
    return ring->count;
}

cancestry_hal_status_t cancestry_hal_tx_ring_push(cancestry_hal_tx_ring_t *ring,
                                                   const cancestry_hal_frame_t *frame)
{
    if (ring == NULL || frame == NULL) {
        return CANCESTRY_HAL_ERR_NULL;
    }
    if (ring->slots == NULL || ring->capacity == 0u) {
        return CANCESTRY_HAL_ERR_ARGUMENT;
    }
    ring->tx_total++;
    if (ring->count >= ring->capacity) {
        ring->dropped++;
        return CANCESTRY_HAL_ERR_RING_FULL;
    }
    ring->slots[ring->write_idx] = *frame;
    ring->write_idx = (uint16_t)((ring->write_idx + 1u) % ring->capacity);
    ring->count++;
    return CANCESTRY_HAL_OK;
}

cancestry_hal_status_t cancestry_hal_tx_ring_pop(cancestry_hal_tx_ring_t *ring,
                                                  cancestry_hal_frame_t *out_frame)
{
    if (ring == NULL || out_frame == NULL) {
        return CANCESTRY_HAL_ERR_NULL;
    }
    if (ring->count == 0u || ring->slots == NULL) {
        return CANCESTRY_HAL_OK_IDLE;
    }
    *out_frame = ring->slots[ring->read_idx];
    memset(&ring->slots[ring->read_idx], 0, sizeof(ring->slots[ring->read_idx]));
    ring->read_idx = (uint16_t)((ring->read_idx + 1u) % ring->capacity);
    ring->count--;
    return CANCESTRY_HAL_OK;
}

/* ------------------------------------------------------------------------- */
/* Frame validation (SW-FR-CANFD-002)                                        */
/* ------------------------------------------------------------------------- */

bool cancestry_hal_frame_is_valid(const cancestry_hal_frame_t *frame)
{
    if (frame == NULL) {
        return false;
    }
    /* A frame wider than the buffer can never be carried, whatever it
     * claims; checking this first keeps the length table lookup bounded. */
    if ((size_t)frame->length > (size_t)CANCESTRY_HAL_FRAME_MAX_LENGTH) {
        return false;
    }
    return cancestry_can_payload_length_is_valid(frame->is_fd != 0u, (size_t)frame->length);
}

/* ------------------------------------------------------------------------- */
/* Name tables (static, read-only)                                           */
/* ------------------------------------------------------------------------- */

const char *cancestry_hal_if_state_name(cancestry_hal_if_state_t state)
{
    switch (state) {
    case CANCESTRY_HAL_IF_STATE_DOWN:
        return "DOWN";
    case CANCESTRY_HAL_IF_STATE_UP:
        return "UP";
    case CANCESTRY_HAL_IF_STATE_ERROR_PASSIVE:
        return "ERROR_PASSIVE";
    case CANCESTRY_HAL_IF_STATE_BUS_OFF:
        return "BUS_OFF";
    case CANCESTRY_HAL_IF_STATE_COUNT:
    default:
        return "INVALID";
    }
}

const char *cancestry_hal_fault_code_name(cancestry_hal_fault_code_t fault)
{
    switch (fault) {
    case CANCESTRY_HAL_FAULT_NONE:
        return "NONE";
    case CANCESTRY_HAL_FAULT_RX_RING_OVERFLOW:
        return "RX_RING_OVERFLOW";
    case CANCESTRY_HAL_FAULT_TX_RING_OVERFLOW:
        return "TX_RING_OVERFLOW";
    case CANCESTRY_HAL_FAULT_BUS_BACKPRESSURE:
        return "BUS_BACKPRESSURE";
    case CANCESTRY_HAL_FAULT_INTERFACE_DOWN:
        return "INTERFACE_DOWN";
    case CANCESTRY_HAL_FAULT_BUS_ERROR_PASSIVE:
        return "BUS_ERROR_PASSIVE";
    case CANCESTRY_HAL_FAULT_BUS_OFF:
        return "BUS_OFF";
    case CANCESTRY_HAL_FAULT_TIMESTAMP_NON_MONOTONIC:
        return "TIMESTAMP_NON_MONOTONIC";
    case CANCESTRY_HAL_FAULT_MALFORMED_FRAME:
        return "MALFORMED_FRAME";
    case CANCESTRY_HAL_FAULT_INIT_FAILED:
        return "INIT_FAILED";
    case CANCESTRY_HAL_FAULT_PROTOCOL_UNSUPPORTED:
        return "PROTOCOL_UNSUPPORTED";
    case CANCESTRY_HAL_FAULT_COUNT:
    default:
        return "INVALID";
    }
}

bool cancestry_hal_status_is_ok(cancestry_hal_status_t status)
{
    return status >= CANCESTRY_HAL_OK;
}

const char *cancestry_hal_status_name(cancestry_hal_status_t status)
{
    switch (status) {
    case CANCESTRY_HAL_OK:
        return "OK";
    case CANCESTRY_HAL_OK_FRAMES_AVAILABLE:
        return "FRAMES_AVAILABLE";
    case CANCESTRY_HAL_OK_IDLE:
        return "IDLE";
    case CANCESTRY_HAL_ERR_NULL:
        return "ERR_NULL";
    case CANCESTRY_HAL_ERR_ARGUMENT:
        return "ERR_ARGUMENT";
    case CANCESTRY_HAL_ERR_NO_INTERFACE:
        return "ERR_NO_INTERFACE";
    case CANCESTRY_HAL_ERR_RING_FULL:
        return "ERR_RING_FULL";
    case CANCESTRY_HAL_ERR_IO:
        return "ERR_IO";
    case CANCESTRY_HAL_ERR_INIT:
        return "ERR_INIT";
    case CANCESTRY_HAL_ERR_UNSUPPORTED:
        return "ERR_UNSUPPORTED";
    default:
        return "INVALID";
    }
}
