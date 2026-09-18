/*
 * CANcestry - ISO-TP transport engine implementation.
 *
 * Normative references:
 *   ISO 15765-2             section 9 (frame types), section 10 (timers)
 *   docs/software/SwRS.md   SW-FR-TP-001 .. SW-FR-TP-010 (Phase 10, issue #26)
 *   docs/system/SyRS.md     SYS-NF-001 (determinism), SYS-NF-002 (bounded resources)
 *
 * Behavioural contract (the conformance suites pin every clause):
 *   - One engine, two independent sessions. The reception session state is
 *     IDLE or FIRST_FRAME; the transmission session state is IDLE,
 *     FLOW_CONTROL (waiting for the receiver's Flow Control) or
 *     CONSECUTIVE_FRAME (pacing Consecutive Frames under BS/STmin).
 *   - Fail-closed (SW-FR-TP-004/005): any protocol violation - a sequence
 *     number skip, an invalid PCI, a frame whose length contradicts its PCI
 *     type, or a frame type that cannot apply in the current state - drops
 *     the affected session, clears its buffer and raises TRANSPORT_PROTOCOL_
 *     FAULT. The engine never resumes an aborted session; the next frame is
 *     judged as a fresh start.
 *   - Time only moves in cancestry_transport_tick() (SW-FR-TP-006). One tick
 *     is 1 ms: the injected clock is read when present, otherwise the
 *     internal time base advances by exactly 1000 us. Timers are compared
 *     against the tick time base; there is no sleep and no wall clock.
 *   - Transmission (SW-FR-TP-007..009): send() emits the first frame
 *     synchronously; Consecutive Frames are emitted by ticks, at most one
 *     burst per tick bounded by STmin and the block limit. A frame the sink
 *     declines occupies the session's pending slot and is retried on every
 *     subsequent tick until the N_As deadline aborts the session with
 *     TRANSPORT_TIMEOUT. Nothing is silently dropped.
 *   - Tick evaluation order is fixed: reception session first (N_Cr, pending
 *     Flow Control), then transmission session (N_Bs, pending frame, Consecutive
 *     Frame burst) - SYS-NF-001.
 *
 * This file is allocation-free by construction (SYS-NF-002); CI symbol-scans
 * the cancestry_transport archive.
 */

#include "cancestry/transport/engine.h"

#include <string.h>

/* ------------------------------------------------------------------------- */
/* Internal helpers                                                          */
/* ------------------------------------------------------------------------- */

/** Effective transmit capability: is a sink callback bound? */
static bool tp_can_transmit(const cancestry_transport_t *tp)
{
    return tp->sink != NULL && tp->sink->on_frame_tx != NULL;
}

/**
 * Hand one frame to the sink. Counts acceptance/decline and the per-type
 * transmit counters; the caller owns session state transitions.
 *
 * @return true when the platform accepted the frame.
 */
static bool tp_emit_frame(cancestry_transport_t *tp,
                          cancestry_tp_tx_frame_kind_t kind,
                          const uint8_t *frame,
                          uint8_t length)
{
    cancestry_tp_tx_frame_t request;

    request.kind = kind;
    request.data = frame;
    request.length = length;
    if (!tp_can_transmit(tp)) {
        /* No transmit path bound: the attempt counts as declined so the
         * caller stores the frame in its pending slot (fail-closed). */
        tp->counters.frames_tx_declined++;
        return false;
    }
    if (!tp->sink->on_frame_tx(tp->sink->user_data, &request)) {
        tp->counters.frames_tx_declined++;
        return false;
    }
    tp->counters.frames_tx++;
    switch (kind) {
    case CANCESTRY_TP_TX_FRAME_SF:
        tp->counters.sf_tx++;
        break;
    case CANCESTRY_TP_TX_FRAME_FF:
        tp->counters.ff_tx++;
        break;
    case CANCESTRY_TP_TX_FRAME_CF:
        tp->counters.cf_tx++;
        break;
    case CANCESTRY_TP_TX_FRAME_FC:
        tp->counters.fc_tx++;
        break;
    case CANCESTRY_TP_TX_FRAME_NONE:
    case CANCESTRY_TP_TX_FRAME_COUNT:
    default:
        break;
    }
    return true;
}

/** Store a declined frame in a pending slot, arming N_As on first attempt. */
static void tp_pending_store(cancestry_tp_pending_frame_t *slot,
                             cancestry_tp_tx_frame_kind_t kind,
                             const uint8_t *frame,
                             uint8_t length,
                             cancestry_time_us_t now_us)
{
    slot->kind = kind;
    memcpy(slot->data, frame, (size_t)length);
    slot->length = length;
    if (!slot->armed) {
        slot->armed = true;
        slot->first_attempt_us = now_us;
    }
}

/** Clear a pending slot after the frame was accepted. */
static void tp_pending_clear(cancestry_tp_pending_frame_t *slot)
{
    slot->kind = CANCESTRY_TP_TX_FRAME_NONE;
    slot->length = 0u;
    slot->armed = false;
}

/**
 * Raise a transport fault: counters, last_fault and (when a queue is bound)
 * a FAULT_RAISED event in the FAULT priority class (SW-FR-TP-010). The queue
 * overflow policy owns drops; this function never fails.
 */
static void tp_raise_fault(cancestry_transport_t *tp, cancestry_tp_fault_code_t fault)
{
    cancestry_event_t event;
    cancestry_event_payload_t payload;

    tp->last_fault = fault;
    switch (fault) {
    case CANCESTRY_TP_FAULT_PROTOCOL:
        tp->counters.protocol_faults++;
        break;
    case CANCESTRY_TP_FAULT_BUFFER_OVERFLOW:
        tp->counters.buffer_overflows++;
        break;
    case CANCESTRY_TP_FAULT_TIMEOUT:
        tp->counters.timeouts++;
        break;
    case CANCESTRY_TP_FAULT_NONE:
    case CANCESTRY_TP_FAULT_COUNT:
    default:
        break;
    }
    tp->counters.faults_raised++;

    if (tp->queue != NULL) {
        memset(&payload, 0, sizeof(payload));
        payload.fault.fault_code =
            ((tp->config.source_id & 0xFFFFu) << 16u) | (uint32_t)fault;
        payload.fault.severity = cancestry_tp_fault_severity(fault);
        payload.fault.source_id = tp->config.source_id;

        cancestry_event_init(&event);
        event.type = CANCESTRY_EVENT_TYPE_FAULT_RAISED;
        event.priority_class = CANCESTRY_PRIORITY_CLASS_FAULT;
        event.timestamp_us = tp->now_us;
        event.payload = payload;
        (void)cancestry_event_queue_push(tp->queue, &event);
    }
}

/**
 * Drop the reception session: clear the reassembled bytes, reset the state
 * machine and cancel a pending Flow Control frame (SW-FR-TP-004/005).
 */
static void tp_abort_rx(cancestry_transport_t *tp)
{
    if (tp->rx_state != CANCESTRY_TP_STATE_IDLE) {
        if (tp->rx_buffer != NULL && tp->rx_received > 0u) {
            memset(tp->rx_buffer, 0, (size_t)tp->rx_received);
        }
    }
    tp->rx_state = CANCESTRY_TP_STATE_IDLE;
    tp->rx_total = 0u;
    tp->rx_received = 0u;
    tp->rx_expected_sn = 0u;
    tp->rx_deadline_us = 0u;
    tp_pending_clear(&tp->rx_pending_fc);
}

/**
 * Drop the transmission session: clear the unsent payload, reset the state
 * machine and cancel a pending frame.
 */
static void tp_abort_tx(cancestry_transport_t *tp)
{
    if (tp->tx_state != CANCESTRY_TP_STATE_IDLE) {
        if (tp->tx_buffer != NULL && tp->tx_total > 0u) {
            memset(tp->tx_buffer, 0, (size_t)tp->tx_total);
        }
    }
    tp->tx_state = CANCESTRY_TP_STATE_IDLE;
    tp->tx_total = 0u;
    tp->tx_sent = 0u;
    tp->tx_next_sn = 0u;
    tp->tx_block_remaining = 0u;
    tp->tx_block_unlimited = false;
    tp->tx_stmin_us = 0u;
    tp->tx_next_cf_us = 0u;
    tp->tx_deadline_us = 0u;
    tp->tx_wait_frames = 0u;
    tp_pending_clear(&tp->tx_pending);
}

/** Deliver a completed message through the sink (SW-FR-TP-009). */
static void tp_deliver_rx_message(cancestry_transport_t *tp)
{
    tp->counters.rx_messages_completed++;
    if (tp->sink != NULL && tp->sink->on_message != NULL) {
        tp->counters.rx_messages_delivered++;
        tp->sink->on_message(tp->sink->user_data, tp->rx_buffer, (size_t)tp->rx_received);
    } else {
        tp->counters.rx_messages_dropped_no_sink++;
    }
}

/** Arm the reception N_Cr deadline for the next Consecutive Frame. */
static void tp_arm_n_cr(cancestry_transport_t *tp)
{
    tp->rx_deadline_us = cancestry_time_sat_add(tp->now_us,
                                                (cancestry_time_us_t)tp->n_cr_ms * 1000u);
}

/** Arm the transmission N_Bs deadline for the next Flow Control frame. */
static void tp_arm_n_bs(cancestry_transport_t *tp)
{
    tp->tx_deadline_us = cancestry_time_sat_add(tp->now_us,
                                                (cancestry_time_us_t)tp->n_bs_ms * 1000u);
}

/** @return true when the N_As budget of a pending frame is exhausted. */
static bool tp_pending_n_as_expired(const cancestry_transport_t *tp,
                                    const cancestry_tp_pending_frame_t *slot)
{
    if (!slot->armed) {
        return false;
    }
    return tp->now_us >= cancestry_time_sat_add(slot->first_attempt_us,
                                                (cancestry_time_us_t)tp->n_as_ms * 1000u);
}

/** Reception session: retry a pending Flow Control frame. */
static void tp_pump_rx_pending(cancestry_transport_t *tp)
{
    if (tp->rx_pending_fc.kind == CANCESTRY_TP_TX_FRAME_NONE) {
        return;
    }
    if (tp_pending_n_as_expired(tp, &tp->rx_pending_fc)) {
        /* The receiver cannot even tell the peer to stop: fail-closed abort
         * with a timeout fault (SW-FR-TP-006/009). */
        tp_raise_fault(tp, CANCESTRY_TP_FAULT_TIMEOUT);
        tp_abort_rx(tp);
        return;
    }
    if (tp_emit_frame(tp, CANCESTRY_TP_TX_FRAME_FC, tp->rx_pending_fc.data,
                      tp->rx_pending_fc.length)) {
        tp_pending_clear(&tp->rx_pending_fc);
    }
}

/**
 * Consecutive Frame burst for one tick (SW-FR-TP-008): emit every CF due
 * under STmin and the block limit, stopping on completion, block exhaustion
 * (wait for the next Flow Control) or a sink decline (pending retry).
 */
static void tp_tx_pump_cf(cancestry_transport_t *tp)
{
    while (tp->tx_state == CANCESTRY_TP_STATE_CONSECUTIVE_FRAME &&
           tp->tx_sent < tp->tx_total &&
           (tp->tx_block_unlimited || tp->tx_block_remaining > 0u) &&
           tp->now_us >= tp->tx_next_cf_us) {
        uint16_t remaining = (uint16_t)(tp->tx_total - tp->tx_sent);
        uint8_t chunk = (remaining > (uint16_t)CANCESTRY_TP_CF_DATA_MAX)
                            ? (uint8_t)CANCESTRY_TP_CF_DATA_MAX
                            : (uint8_t)remaining;
        uint8_t frame[CANCESTRY_TP_FRAME_MAX_LENGTH];

        frame[0] = (uint8_t)(0x20u | (tp->tx_next_sn & 0x0Fu));
        memcpy(&frame[1], &tp->tx_buffer[tp->tx_sent], (size_t)chunk);
        if (tp_emit_frame(tp, CANCESTRY_TP_TX_FRAME_CF, frame, (uint8_t)(chunk + 1u))) {
            tp->tx_sent = (uint16_t)(tp->tx_sent + chunk);
            tp->tx_next_sn = (uint8_t)((tp->tx_next_sn + 1u) & 0x0Fu);
            if (!tp->tx_block_unlimited && tp->tx_block_remaining > 0u) {
                tp->tx_block_remaining--;
            }
            /* STmin separates consecutive frames (SW-FR-TP-008); at the 1 ms
             * tick granularity a sub-millisecond STmin waits one tick. */
            tp->tx_next_cf_us =
                cancestry_time_sat_add(tp->now_us, (cancestry_time_us_t)tp->tx_stmin_us);
            if (tp->tx_sent >= tp->tx_total) {
                tp->counters.tx_messages_completed++;
                tp->tx_state = CANCESTRY_TP_STATE_IDLE;
                tp->tx_total = 0u;
                tp->tx_sent = 0u;
                return;
            }
            if (!tp->tx_block_unlimited && tp->tx_block_remaining == 0u) {
                /* Block complete: wait for the next Flow Control (SW-FR-TP-008). */
                tp->tx_state = CANCESTRY_TP_STATE_FLOW_CONTROL;
                tp_arm_n_bs(tp);
                return;
            }
        } else {
            tp_pending_store(&tp->tx_pending, CANCESTRY_TP_TX_FRAME_CF, frame,
                             (uint8_t)(chunk + 1u), tp->now_us);
            return;
        }
    }
}

/**
 * Transmission session: retry a pending SF/FF/CF frame, then run the CF burst.
 * Handles the N_As expiry of a frame the platform keeps declining.
 */
static void tp_pump_tx(cancestry_transport_t *tp)
{
    if (tp->tx_pending.kind != CANCESTRY_TP_TX_FRAME_NONE) {
        if (tp_pending_n_as_expired(tp, &tp->tx_pending)) {
            tp_raise_fault(tp, CANCESTRY_TP_FAULT_TIMEOUT);
            tp_abort_tx(tp);
            return;
        }
        if (tp_emit_frame(tp, tp->tx_pending.kind, tp->tx_pending.data,
                          tp->tx_pending.length)) {
            uint8_t chunk = (uint8_t)(tp->tx_pending.length - 1u);

            switch (tp->tx_pending.kind) {
            case CANCESTRY_TP_TX_FRAME_SF:
                tp_pending_clear(&tp->tx_pending);
                tp->counters.tx_messages_completed++;
                break;
            case CANCESTRY_TP_TX_FRAME_FF:
                tp_pending_clear(&tp->tx_pending);
                tp->tx_sent = (uint16_t)CANCESTRY_TP_FF_DATA_MAX;
                tp->tx_state = CANCESTRY_TP_STATE_FLOW_CONTROL;
                tp->tx_wait_frames = 0u;
                tp_arm_n_bs(tp);
                break;
            case CANCESTRY_TP_TX_FRAME_CF: {
                uint8_t sn = (uint8_t)(tp->tx_pending.data[0] & 0x0Fu);

                tp_pending_clear(&tp->tx_pending);
                tp->tx_sent = (uint16_t)(tp->tx_sent + chunk);
                tp->tx_next_sn = (uint8_t)((sn + 1u) & 0x0Fu);
                if (!tp->tx_block_unlimited && tp->tx_block_remaining > 0u) {
                    tp->tx_block_remaining--;
                }
                tp->tx_next_cf_us =
                    cancestry_time_sat_add(tp->now_us, (cancestry_time_us_t)tp->tx_stmin_us);
                if (tp->tx_sent >= tp->tx_total) {
                    tp->counters.tx_messages_completed++;
                    tp->tx_state = CANCESTRY_TP_STATE_IDLE;
                    tp->tx_total = 0u;
                    tp->tx_sent = 0u;
                } else if (!tp->tx_block_unlimited && tp->tx_block_remaining == 0u) {
                    tp->tx_state = CANCESTRY_TP_STATE_FLOW_CONTROL;
                    tp_arm_n_bs(tp);
                }
                break;
            }
            case CANCESTRY_TP_TX_FRAME_NONE:
            case CANCESTRY_TP_TX_FRAME_FC:
            case CANCESTRY_TP_TX_FRAME_COUNT:
            default:
                tp_pending_clear(&tp->tx_pending);
                break;
            }
        }
    }
    tp_tx_pump_cf(tp);
}

/**
 * Reception session handler for one received frame (PCI dispatched by the
 * caller). Every violation path aborts the session and raises the fault.
 */
static cancestry_tp_status_t tp_rx_handle_sf(cancestry_transport_t *tp,
                                             const uint8_t *data,
                                             uint8_t length)
{
    uint8_t dl = (uint8_t)(data[0] & 0x0Fu);

    tp->counters.sf_rx++;
    if (tp->rx_state != CANCESTRY_TP_STATE_IDLE) {
        /* A Single Frame while reassembly is in progress is a state violation
         * (SW-FR-TP-005): fail-closed abort, the SF is not processed. */
        tp_raise_fault(tp, CANCESTRY_TP_FAULT_PROTOCOL);
        tp_abort_rx(tp);
        return CANCESTRY_TP_ERR_PROTOCOL;
    }
    if (dl == 0u || dl > (uint8_t)CANCESTRY_TP_SF_DATA_MAX || length != (uint8_t)(dl + 1u)) {
        /* SF_DL 0 is not representable on classic CAN; the frame length must
         * match the declared length exactly. */
        tp_raise_fault(tp, CANCESTRY_TP_FAULT_PROTOCOL);
        return CANCESTRY_TP_ERR_PROTOCOL;
    }
    if ((size_t)dl > tp->rx_capacity) {
        /* The message cannot fit the statically sized buffer (SW-FR-TP-003). */
        tp_raise_fault(tp, CANCESTRY_TP_FAULT_BUFFER_OVERFLOW);
        return CANCESTRY_TP_ERR_OVERFLOW;
    }
    memcpy(tp->rx_buffer, &data[1], (size_t)dl);
    tp->rx_received = (uint16_t)dl;
    tp->rx_total = (uint16_t)dl;
    tp_deliver_rx_message(tp);
    tp->rx_state = CANCESTRY_TP_STATE_IDLE;
    tp->rx_total = 0u;
    tp->rx_received = 0u;
    return CANCESTRY_TP_OK_MESSAGE;
}

static cancestry_tp_status_t tp_rx_handle_ff(cancestry_transport_t *tp,
                                             const uint8_t *data,
                                             uint8_t length)
{
    uint16_t total;
    uint8_t fc[3];

    tp->counters.ff_rx++;
    if (tp->rx_state != CANCESTRY_TP_STATE_IDLE) {
        /* A new First Frame while a reception is in progress: sequence error,
         * fail-closed abort (SW-FR-TP-004/005). */
        tp_raise_fault(tp, CANCESTRY_TP_FAULT_PROTOCOL);
        tp_abort_rx(tp);
        return CANCESTRY_TP_ERR_PROTOCOL;
    }
    if (length != CANCESTRY_TP_FRAME_MAX_LENGTH) {
        /* A First Frame is always 8 bytes on classic CAN. */
        tp_raise_fault(tp, CANCESTRY_TP_FAULT_PROTOCOL);
        return CANCESTRY_TP_ERR_PROTOCOL;
    }
    total = (uint16_t)(((uint16_t)(data[0] & 0x0Fu) << 8) | (uint16_t)data[1]);
    if (total < (uint16_t)CANCESTRY_TP_SF_DATA_MAX + 1u) {
        /* A message of at most 7 bytes belongs in a Single Frame. */
        tp_raise_fault(tp, CANCESTRY_TP_FAULT_PROTOCOL);
        return CANCESTRY_TP_ERR_PROTOCOL;
    }
    if ((size_t)total > tp->rx_capacity) {
        /* Oversized for the caller's static buffer: abort before storing any
         * payload byte, tell the peer with FC OVFLW and raise the fault
         * (SW-FR-TP-003). The engine never allocates or truncates. */
        fc[0] = 0x32u; /* FlowStatus OVFLW */
        fc[1] = 0u;
        fc[2] = 0u;
        tp_pending_store(&tp->rx_pending_fc, CANCESTRY_TP_TX_FRAME_FC, fc, 3u, tp->now_us);
        if (tp_emit_frame(tp, CANCESTRY_TP_TX_FRAME_FC, fc, 3u)) {
            tp_pending_clear(&tp->rx_pending_fc);
        }
        tp_raise_fault(tp, CANCESTRY_TP_FAULT_BUFFER_OVERFLOW);
        return CANCESTRY_TP_ERR_OVERFLOW;
    }
    /* Accept the session: store the first 6 payload bytes, expect SN 1. */
    memcpy(tp->rx_buffer, &data[2], CANCESTRY_TP_FF_DATA_MAX);
    tp->rx_total = total;
    tp->rx_received = (uint16_t)CANCESTRY_TP_FF_DATA_MAX;
    tp->rx_expected_sn = 1u;
    tp->rx_state = CANCESTRY_TP_STATE_FIRST_FRAME;
    tp_arm_n_cr(tp);
    /* Answer with Flow Control CTS carrying our block size and STmin. */
    fc[0] = 0x30u; /* FlowStatus CTS */
    fc[1] = tp->config.fc_block_size;
    fc[2] = tp->config.fc_stmin;
    tp_pending_store(&tp->rx_pending_fc, CANCESTRY_TP_TX_FRAME_FC, fc, 3u, tp->now_us);
    if (tp_emit_frame(tp, CANCESTRY_TP_TX_FRAME_FC, fc, 3u)) {
        tp_pending_clear(&tp->rx_pending_fc);
    }
    return CANCESTRY_TP_OK;
}

static cancestry_tp_status_t tp_rx_handle_cf(cancestry_transport_t *tp,
                                             const uint8_t *data,
                                             uint8_t length)
{
    uint8_t sn = (uint8_t)(data[0] & 0x0Fu);
    uint16_t remaining;
    uint8_t chunk;

    tp->counters.cf_rx++;
    if (tp->rx_state != CANCESTRY_TP_STATE_FIRST_FRAME) {
        /* A Consecutive Frame with no reception in progress (SW-FR-TP-005). */
        tp_raise_fault(tp, CANCESTRY_TP_FAULT_PROTOCOL);
        return CANCESTRY_TP_ERR_PROTOCOL;
    }
    if (sn != tp->rx_expected_sn) {
        /* The normative abort: sequence number skip drops the session, clears
         * the buffer and raises the fault (SW-FR-TP-004). */
        tp_raise_fault(tp, CANCESTRY_TP_FAULT_PROTOCOL);
        tp_abort_rx(tp);
        return CANCESTRY_TP_ERR_PROTOCOL;
    }
    remaining = (uint16_t)(tp->rx_total - tp->rx_received);
    chunk = (remaining > (uint16_t)CANCESTRY_TP_CF_DATA_MAX)
                ? (uint8_t)CANCESTRY_TP_CF_DATA_MAX
                : (uint8_t)remaining;
    if (length != (uint8_t)(chunk + 1u)) {
        /* Frame length contradicts the PCI type: a non-final CF must be full,
         * the final CF must carry exactly the remaining bytes. */
        tp_raise_fault(tp, CANCESTRY_TP_FAULT_PROTOCOL);
        tp_abort_rx(tp);
        return CANCESTRY_TP_ERR_PROTOCOL;
    }
    memcpy(&tp->rx_buffer[tp->rx_received], &data[1], (size_t)chunk);
    tp->rx_received = (uint16_t)(tp->rx_received + chunk);
    tp->rx_expected_sn = (uint8_t)((sn + 1u) & 0x0Fu);
    tp_arm_n_cr(tp);
    if (tp->rx_received >= tp->rx_total) {
        tp_deliver_rx_message(tp);
        tp->rx_state = CANCESTRY_TP_STATE_IDLE;
        tp->rx_total = 0u;
        tp->rx_received = 0u;
        return CANCESTRY_TP_OK_MESSAGE;
    }
    return CANCESTRY_TP_OK;
}

static cancestry_tp_status_t tp_rx_handle_fc(cancestry_transport_t *tp,
                                             const uint8_t *data,
                                             uint8_t length)
{
    uint8_t fs;
    uint8_t bs;
    uint8_t stmin;

    tp->counters.fc_rx++;
    if (tp->tx_state != CANCESTRY_TP_STATE_FLOW_CONTROL) {
        /* FC with no transmission waiting for it: protocol violation
         * (SW-FR-TP-005). Abort an active transmission session, if any. */
        tp_raise_fault(tp, CANCESTRY_TP_FAULT_PROTOCOL);
        tp_abort_tx(tp);
        return CANCESTRY_TP_ERR_PROTOCOL;
    }
    if (length != 3u) {
        tp_raise_fault(tp, CANCESTRY_TP_FAULT_PROTOCOL);
        tp_abort_tx(tp);
        return CANCESTRY_TP_ERR_PROTOCOL;
    }
    fs = (uint8_t)(data[0] & 0x0Fu);
    bs = data[1];
    stmin = data[2];
    if (fs == 0x0u) {
        /* CTS: pace the next block under BS/STmin (SW-FR-TP-008). */
        tp->tx_state = CANCESTRY_TP_STATE_CONSECUTIVE_FRAME;
        tp->tx_wait_frames = 0u;
        if (bs == 0u) {
            tp->tx_block_unlimited = true;
            tp->tx_block_remaining = 0u;
        } else {
            tp->tx_block_unlimited = false;
            tp->tx_block_remaining = bs;
        }
        tp->tx_stmin_us = cancestry_tp_stmin_decode_us(stmin);
        tp->tx_next_cf_us = tp->now_us; /* first CF due on the next tick */
        return CANCESTRY_TP_OK;
    }
    if (fs == 0x1u) {
        /* Wait: restart N_Bs, bounded by the wait-frame limit. */
        tp->counters.wait_frames_rx++;
        tp->tx_wait_frames++;
        if (tp->tx_wait_frames > tp->max_wait_frames) {
            tp_raise_fault(tp, CANCESTRY_TP_FAULT_PROTOCOL);
            tp_abort_tx(tp);
            return CANCESTRY_TP_ERR_PROTOCOL;
        }
        tp_arm_n_bs(tp);
        return CANCESTRY_TP_OK;
    }
    if (fs == 0x2u) {
        /* OVFLW: the receiver cannot hold the message; abort (SW-FR-TP-003/008). */
        tp_raise_fault(tp, CANCESTRY_TP_FAULT_BUFFER_OVERFLOW);
        tp_abort_tx(tp);
        return CANCESTRY_TP_ERR_OVERFLOW;
    }
    /* Reserved FlowStatus values are violations (fail-closed). */
    tp_raise_fault(tp, CANCESTRY_TP_FAULT_PROTOCOL);
    tp_abort_tx(tp);
    return CANCESTRY_TP_ERR_PROTOCOL;
}

/* ------------------------------------------------------------------------- */
/* Public API                                                                */
/* ------------------------------------------------------------------------- */

bool cancestry_transport_init(cancestry_transport_t *tp,
                              const cancestry_tp_config_t *config,
                              uint8_t *rx_buffer,
                              size_t rx_capacity,
                              uint8_t *tx_buffer,
                              size_t tx_capacity,
                              const cancestry_tp_sink_t *sink,
                              cancestry_event_queue_t *queue,
                              const cancestry_clock_t *clock)
{
    if (tp == NULL) {
        return false;
    }
    if ((rx_capacity > 0u && rx_buffer == NULL) ||
        (tx_capacity > 0u && tx_buffer == NULL)) {
        memset(tp, 0, sizeof(*tp));
        return false;
    }

    memset(tp, 0, sizeof(*tp));
    if (config != NULL) {
        tp->config = *config;
    }
    tp->n_as_ms = (config != NULL && config->n_as_ms != 0u)
                      ? config->n_as_ms
                      : (uint32_t)CANCESTRY_TP_DEFAULT_N_AS_MS;
    tp->n_bs_ms = (config != NULL && config->n_bs_ms != 0u)
                      ? config->n_bs_ms
                      : (uint32_t)CANCESTRY_TP_DEFAULT_N_BS_MS;
    tp->n_cr_ms = (config != NULL && config->n_cr_ms != 0u)
                      ? config->n_cr_ms
                      : (uint32_t)CANCESTRY_TP_DEFAULT_N_CR_MS;
    tp->max_wait_frames = (config != NULL && config->max_wait_frames != 0u)
                              ? config->max_wait_frames
                              : (uint8_t)CANCESTRY_TP_DEFAULT_MAX_WAIT_FRAMES;

    tp->rx_buffer = rx_buffer;
    tp->rx_capacity = rx_capacity;
    tp->tx_buffer = tx_buffer;
    tp->tx_capacity = tx_capacity;
    tp->sink = sink;
    tp->queue = queue;
    tp->clock = (clock != NULL && cancestry_clock_is_valid(clock)) ? clock : NULL;
    tp->owns_time_base = (tp->clock == NULL);

    tp->rx_state = CANCESTRY_TP_STATE_IDLE;
    tp->tx_state = CANCESTRY_TP_STATE_IDLE;
    tp->last_fault = CANCESTRY_TP_FAULT_NONE;
    tp->rx_pending_fc.kind = CANCESTRY_TP_TX_FRAME_NONE;
    tp->tx_pending.kind = CANCESTRY_TP_TX_FRAME_NONE;
    tp->initialized = true;
    return true;
}

bool cancestry_transport_is_valid(const cancestry_transport_t *tp)
{
    return tp != NULL && tp->initialized;
}

cancestry_tp_status_t cancestry_transport_rx_frame(cancestry_transport_t *tp,
                                                   const uint8_t *data,
                                                   uint8_t length)
{
    uint8_t pci;

    if (tp == NULL || !tp->initialized) {
        return CANCESTRY_TP_ERR_NULL;
    }
    if (data == NULL || length == 0u || length > CANCESTRY_TP_FRAME_MAX_LENGTH) {
        return CANCESTRY_TP_ERR_ARGUMENT;
    }
    tp->counters.frames_rx++;
    pci = (uint8_t)(data[0] >> 4);
    switch (pci) {
    case 0x0u:
        return tp_rx_handle_sf(tp, data, length);
    case 0x1u:
        return tp_rx_handle_ff(tp, data, length);
    case 0x2u:
        return tp_rx_handle_cf(tp, data, length);
    case 0x3u:
        return tp_rx_handle_fc(tp, data, length);
    default:
        /* PCI nibble 0x4..0xF is not defined by ISO 15765-2 for classic CAN:
         * invalid PCI, fail-closed abort of the reception session
         * (SW-FR-TP-005). */
        tp_raise_fault(tp, CANCESTRY_TP_FAULT_PROTOCOL);
        if (tp->rx_state != CANCESTRY_TP_STATE_IDLE) {
            tp_abort_rx(tp);
        }
        return CANCESTRY_TP_ERR_PROTOCOL;
    }
}

cancestry_tp_status_t cancestry_transport_send(cancestry_transport_t *tp,
                                               const uint8_t *data,
                                               size_t length)
{
    uint8_t frame[CANCESTRY_TP_FRAME_MAX_LENGTH];

    if (tp == NULL || !tp->initialized) {
        return CANCESTRY_TP_ERR_NULL;
    }
    if (data == NULL || length == 0u) {
        return CANCESTRY_TP_ERR_ARGUMENT;
    }
    if (length > CANCESTRY_TP_MESSAGE_MAX_LENGTH) {
        /* The 12-bit FF length field cannot describe more (SW-FR-TP-007). */
        return CANCESTRY_TP_ERR_CAPACITY;
    }
    if (cancestry_transport_tx_busy(tp)) {
        return CANCESTRY_TP_ERR_STATE;
    }
    if (length > tp->tx_capacity) {
        /* Does not fit the caller's static TX buffer: refused, never
         * truncated (SW-FR-TP-001/003). */
        return CANCESTRY_TP_ERR_CAPACITY;
    }
    if (!tp_can_transmit(tp)) {
        /* Fail-closed: refuse to start a transmission that cannot leave the
         * engine (SW-FR-TP-009). */
        return CANCESTRY_TP_ERR_NULL;
    }
    tp->counters.tx_messages_requested++;

    if (length <= CANCESTRY_TP_SF_DATA_MAX) {
        /* Single Frame, emitted synchronously (SW-FR-TP-007). */
        frame[0] = (uint8_t)(0x00u | (uint8_t)length);
        memcpy(&frame[1], data, length);
        if (tp_emit_frame(tp, CANCESTRY_TP_TX_FRAME_SF, frame, (uint8_t)(length + 1u))) {
            tp->counters.tx_messages_completed++;
            return CANCESTRY_TP_OK;
        }
        tp_pending_store(&tp->tx_pending, CANCESTRY_TP_TX_FRAME_SF, frame,
                         (uint8_t)(length + 1u), tp->now_us);
        return CANCESTRY_TP_OK;
    }

    /* First Frame, emitted synchronously; CFs follow on ticks after Flow
     * Control (SW-FR-TP-007/008). The FF itself carries the first 6 payload
     * bytes, so the CF phase starts at offset 6. */
    memcpy(tp->tx_buffer, data, length);
    tp->tx_total = (uint16_t)length;
    tp->tx_next_sn = 1u;
    tp->tx_wait_frames = 0u;
    frame[0] = (uint8_t)(0x10u | (uint8_t)((length >> 8) & 0x0Fu));
    frame[1] = (uint8_t)(length & 0xFFu);
    memcpy(&frame[2], data, CANCESTRY_TP_FF_DATA_MAX);
    if (tp_emit_frame(tp, CANCESTRY_TP_TX_FRAME_FF, frame, CANCESTRY_TP_FRAME_MAX_LENGTH)) {
        tp->tx_sent = (uint16_t)CANCESTRY_TP_FF_DATA_MAX;
        tp->tx_state = CANCESTRY_TP_STATE_FLOW_CONTROL;
        tp_arm_n_bs(tp);
        return CANCESTRY_TP_OK;
    }
    tp->tx_sent = 0u; /* the FF is still pending; CFs start after it goes out */
    tp_pending_store(&tp->tx_pending, CANCESTRY_TP_TX_FRAME_FF, frame,
                     CANCESTRY_TP_FRAME_MAX_LENGTH, tp->now_us);
    return CANCESTRY_TP_OK;
}

bool cancestry_transport_tx_busy(const cancestry_transport_t *tp)
{
    if (tp == NULL || !tp->initialized) {
        return false;
    }
    return tp->tx_state != CANCESTRY_TP_STATE_IDLE ||
           tp->tx_pending.kind != CANCESTRY_TP_TX_FRAME_NONE;
}

cancestry_tp_status_t cancestry_transport_tick(cancestry_transport_t *tp)
{
    uint32_t timeouts_before;

    if (tp == NULL || !tp->initialized) {
        return CANCESTRY_TP_ERR_NULL;
    }
    timeouts_before = tp->counters.timeouts;

    /* Advance the time base: one tick is exactly 1 ms (SW-FR-TP-006). */
    if (tp->clock != NULL) {
        cancestry_time_us_t now = cancestry_clock_now_us(tp->clock);

        if (now > tp->now_us) {
            tp->now_us = now;
        }
    } else {
        tp->now_us = cancestry_time_sat_add(tp->now_us, 1000u);
    }
    tp->ticks++;
    tp->counters.ticks++;

    /* 1. Reception session: N_Cr, then a pending Flow Control retry. */
    if (tp->rx_state == CANCESTRY_TP_STATE_FIRST_FRAME &&
        tp->now_us >= tp->rx_deadline_us) {
        /* Missing Consecutive Frame: deterministic abort (SW-FR-TP-006). */
        tp_raise_fault(tp, CANCESTRY_TP_FAULT_TIMEOUT);
        tp_abort_rx(tp);
    }
    tp_pump_rx_pending(tp);

    /* 2. Transmission session: N_Bs, pending frame retry, CF burst. */
    if (tp->tx_state == CANCESTRY_TP_STATE_FLOW_CONTROL &&
        tp->now_us >= tp->tx_deadline_us) {
        /* Missing Flow Control: deterministic abort (SW-FR-TP-006). */
        tp_raise_fault(tp, CANCESTRY_TP_FAULT_TIMEOUT);
        tp_abort_tx(tp);
    }
    tp_pump_tx(tp);

    /* A timeout is reported but never leaves the engine unusable: the abort
     * paths above returned both sessions to a clean state (SW-FR-TP-006). */
    if (tp->counters.timeouts > timeouts_before) {
        return CANCESTRY_TP_ERR_TIMEOUT;
    }
    return CANCESTRY_TP_OK;
}

cancestry_tp_status_t cancestry_transport_advance(cancestry_transport_t *tp,
                                                  uint32_t tick_count)
{
    cancestry_tp_status_t result = CANCESTRY_TP_OK;
    uint32_t i;

    if (tp == NULL || !tp->initialized) {
        return CANCESTRY_TP_ERR_NULL;
    }
    for (i = 0u; i < tick_count; ++i) {
        cancestry_tp_status_t tick_result = cancestry_transport_tick(tp);

        if (tick_result < CANCESTRY_TP_OK && result == CANCESTRY_TP_OK) {
            result = tick_result;
        }
    }
    return result;
}

cancestry_tp_state_t cancestry_transport_rx_state(const cancestry_transport_t *tp)
{
    if (tp == NULL || !tp->initialized) {
        return CANCESTRY_TP_STATE_IDLE;
    }
    return tp->rx_state;
}

cancestry_tp_state_t cancestry_transport_tx_state(const cancestry_transport_t *tp)
{
    if (tp == NULL || !tp->initialized) {
        return CANCESTRY_TP_STATE_IDLE;
    }
    return tp->tx_state;
}

const cancestry_tp_counters_t *cancestry_transport_counters(const cancestry_transport_t *tp)
{
    if (tp == NULL || !tp->initialized) {
        return NULL;
    }
    return &tp->counters;
}

cancestry_tp_fault_code_t cancestry_transport_last_fault(const cancestry_transport_t *tp)
{
    if (tp == NULL || !tp->initialized) {
        return CANCESTRY_TP_FAULT_NONE;
    }
    return tp->last_fault;
}

cancestry_time_us_t cancestry_transport_now_us(const cancestry_transport_t *tp)
{
    if (tp == NULL || !tp->initialized) {
        return 0u;
    }
    return tp->now_us;
}
