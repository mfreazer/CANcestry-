/*
 * CANcestry - ISO-TP transport engine.
 *
 * Normative references:
 *   ISO 15765-2                       transport protocol, classic CAN framing
 *   docs/software/SwRS.md             SW-FR-TP-001 .. SW-FR-TP-010 (Phase 10)
 *   docs/system/event-ordering.md     sections 2-4 (clock, fault priority)
 *   docs/system/governor.md           (the transport itself is ungoverned; the
 *                                     UDS layer governs the side effects)
 *   docs/system/SyRS.md               SYS-NF-001 (determinism), SYS-NF-002 (bounded resources)
 *
 * Design notes:
 *   - Caller-owned everything: the engine struct, the RX/TX buffers, the sink
 *     and the event queue are bound at init and never allocate (SW-FR-TP-001).
 *   - Time advances only through cancestry_transport_tick(): one call is one
 *     1 ms tick, exactly like the FSM engine (SW-FR-TP-006). With an injected
 *     clock the tick reads it; without one the engine advances its internal
 *     clock by exactly 1000 us. No sleep, no wall clock, no hidden time.
 *   - Segmentation is tick-driven: cancestry_transport_send() emits the first
 *     frame (SF or FF) synchronously; Consecutive Frames are paced out by
 *     ticks under the receiver's Block Size and STmin (SW-FR-TP-007/008).
 *   - Fail-closed transmission: a frame the sink declines is retried on
 *     subsequent ticks until the N_As timer expires; the engine never
 *     silently drops a frame it accepted (SW-FR-TP-009).
 *   - Faults are FAULT_RAISED events in the FAULT priority class, encoded
 *     (source_id << 16) | fault exactly like the HAL (SW-FR-TP-010).
 */

#ifndef CANCESTRY_TP_ENGINE_H
#define CANCESTRY_TP_ENGINE_H

#include "cancestry/event/queue.h"
#include "cancestry/transport/types.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------------- */
/* Sink                                                                      */
/* ------------------------------------------------------------------------- */

/**
 * Kind of frame the engine wants to transmit, reported to the sink for
 * observability and testing.
 */
typedef enum cancestry_tp_tx_frame_kind {
    CANCESTRY_TP_TX_FRAME_NONE = 0,
    /** Single Frame carrying a whole short message. */
    CANCESTRY_TP_TX_FRAME_SF = 1,
    /** First Frame opening a multi-frame transmission. */
    CANCESTRY_TP_TX_FRAME_FF = 2,
    /** Consecutive Frame continuing a transmission. */
    CANCESTRY_TP_TX_FRAME_CF = 3,
    /** Flow Control frame (CTS or OVFLW) for a received First Frame. */
    CANCESTRY_TP_TX_FRAME_FC = 4,
    CANCESTRY_TP_TX_FRAME_COUNT
} cancestry_tp_tx_frame_kind_t;

/** One requested frame transmission. */
typedef struct cancestry_tp_tx_frame {
    cancestry_tp_tx_frame_kind_t kind;
    /** Frame bytes, @c length long (1..8). */
    const uint8_t *data;
    uint8_t length;
} cancestry_tp_tx_frame_t;

/**
 * Runtime sink: the engine's only path to the bus.
 *
 * Both callbacks are optional (NULL) at init:
 *   - a NULL on_frame_tx makes cancestry_transport_send() fail with
 *     CANCESTRY_TP_ERR_NULL (fail-closed: the engine refuses to start a
 *     transmission it cannot perform), and a received First Frame is still
 *     aborted with an OVERFLOW/PROTOCOL fault path but no OVFLW frame can be
 *     emitted;
 *   - a NULL on_message means completed RX messages are counted in
 *     rx_messages_dropped_no_sink instead of delivered.
 */
typedef struct cancestry_tp_sink {
    /** Opaque context passed to every callback. */
    void *user_data;
    /**
     * Hand one frame to the platform.
     *
     * @return true when the platform accepted the frame; false means the
     *         platform could not transmit it now (for example a full TX
     *         ring), and the engine retries the same frame on subsequent
     *         ticks until the N_As timer expires (SW-FR-TP-009). The frame
     *         bytes are borrowed and only valid during the callback.
     */
    bool (*on_frame_tx)(void *user_data, const cancestry_tp_tx_frame_t *frame);
    /**
     * A complete message was reassembled. @p data is borrowed, only valid
     * during the callback; a sink that needs it afterwards must copy it.
     */
    void (*on_message)(void *user_data, const uint8_t *data, size_t length);
} cancestry_tp_sink_t;

/* ------------------------------------------------------------------------- */
/* Pending frame (N_As retransmission slot, one per session)                 */
/* ------------------------------------------------------------------------- */

/** A frame the sink declined, retried each tick until the N_As deadline. */
typedef struct cancestry_tp_pending_frame {
    /** NONE when no frame is pending. */
    cancestry_tp_tx_frame_kind_t kind;
    uint8_t data[CANCESTRY_TP_FRAME_MAX_LENGTH];
    uint8_t length;
    /** true once at least one attempt was made (deadline armed). */
    bool armed;
    cancestry_time_us_t first_attempt_us;
} cancestry_tp_pending_frame_t;

/* ------------------------------------------------------------------------- */
/* Engine                                                                    */
/* ------------------------------------------------------------------------- */

/**
 * ISO-TP engine over caller-owned storage. Never allocates.
 */
typedef struct cancestry_transport {
    /* --- bound configuration --- */
    cancestry_tp_config_t config;
    /** Resolved timers in ms (defaults applied). */
    uint32_t n_as_ms;
    uint32_t n_bs_ms;
    uint32_t n_cr_ms;
    uint8_t max_wait_frames;

    uint8_t *rx_buffer;
    size_t rx_capacity;
    uint8_t *tx_buffer;
    size_t tx_capacity;

    const cancestry_tp_sink_t *sink;
    cancestry_event_queue_t *queue;
    const cancestry_clock_t *clock;

    /* --- time base --- */
    cancestry_time_us_t now_us;
    uint64_t ticks;
    bool owns_time_base;

    /* --- reception session --- */
    cancestry_tp_state_t rx_state;
    /** Total message length announced by the First Frame. */
    uint16_t rx_total;
    /** Payload bytes already stored. */
    uint16_t rx_received;
    /** Sequence number the next Consecutive Frame must carry. */
    uint8_t rx_expected_sn;
    /** N_Cr deadline for the next Consecutive Frame. */
    cancestry_time_us_t rx_deadline_us;
    /** Pending Flow Control frame for the reception session. */
    cancestry_tp_pending_frame_t rx_pending_fc;

    /* --- transmission session --- */
    cancestry_tp_state_t tx_state;
    /** Total payload length of the message being sent. */
    uint16_t tx_total;
    /** Payload bytes already accepted by the sink. */
    uint16_t tx_sent;
    /** Sequence number the next Consecutive Frame will carry. */
    uint8_t tx_next_sn;
    /** Consecutive Frames left in the current block; 0 = unlimited. */
    uint16_t tx_block_remaining;
    /** true while the current block has no limit (receiver sent BS = 0). */
    bool tx_block_unlimited;
    /** Decoded STmin in microseconds. */
    uint32_t tx_stmin_us;
    /** Earliest time the next Consecutive Frame may be emitted. */
    cancestry_time_us_t tx_next_cf_us;
    /** N_Bs deadline for the next Flow Control frame. */
    cancestry_time_us_t tx_deadline_us;
    /** Consecutive FC Wait frames received for the current transmission. */
    uint8_t tx_wait_frames;
    /** Pending SF/FF/CF frame for the transmission session. */
    cancestry_tp_pending_frame_t tx_pending;

    /* --- observability --- */
    cancestry_tp_counters_t counters;
    cancestry_tp_fault_code_t last_fault;
    /** Set by init; a zeroed struct is safely unusable. */
    bool initialized;
} cancestry_transport_t;

/* ------------------------------------------------------------------------- */
/* Lifecycle                                                                 */
/* ------------------------------------------------------------------------- */

/**
 * Initialize an engine over caller-owned storage.
 *
 * @param tp           Engine to initialize; ignored when NULL.
 * @param config       Timer/blocking configuration, or NULL for every default.
 * @param rx_buffer    Caller-owned reassembly buffer. Its capacity is the
 *                     largest receivable message; a First Frame announcing
 *                     more is aborted with TRANSPORT_BUFFER_OVERFLOW before
 *                     any payload byte is stored (SW-FR-TP-003). May be NULL
 *                     only when @p rx_capacity is 0 (then every multi-frame
 *                     reception aborts with OVERFLOW).
 * @param rx_capacity  Size of @p rx_buffer in bytes.
 * @param tx_buffer    Caller-owned segmentation buffer; the largest sendable
 *                     message. May be NULL only when @p tx_capacity is 0.
 * @param tx_capacity  Size of @p tx_buffer in bytes.
 * @param sink         Transmit/message sink; may be NULL (see
 *                     ::cancestry_tp_sink_t for the fail-closed defaults).
 * @param queue        Event queue for FAULT_RAISED events; may be NULL (faults
 *                     are then counted and exposed via counters/last_fault
 *                     only).
 * @param clock        Monotonic clock read by every tick; NULL selects the
 *                     engine-internal 1000 us-per-tick time base.
 * @return true when the engine is ready for use. On failure the engine is
 *         left zeroed (safely unusable).
 */
bool cancestry_transport_init(cancestry_transport_t *tp,
                              const cancestry_tp_config_t *config,
                              uint8_t *rx_buffer,
                              size_t rx_capacity,
                              uint8_t *tx_buffer,
                              size_t tx_capacity,
                              const cancestry_tp_sink_t *sink,
                              cancestry_event_queue_t *queue,
                              const cancestry_clock_t *clock);

/** @return true when @p tp is non-NULL and initialized. */
bool cancestry_transport_is_valid(const cancestry_transport_t *tp);

/* ------------------------------------------------------------------------- */
/* Reception                                                                 */
/* ------------------------------------------------------------------------- */

/**
 * Feed one received classic CAN frame into the engine.
 *
 * Dispatches on the PCI: SF/FF/CF feed the reception session, FC feeds the
 * transmission session. Protocol violations abort the affected session, clear
 * its buffer and raise the matching fault event (SW-FR-TP-004/005).
 *
 * @param tp      Engine.
 * @param data    Frame payload bytes.
 * @param length  Payload length in bytes, 1..8.
 * @return CANCESTRY_TP_OK, CANCESTRY_TP_OK_MESSAGE when this frame completed a
 *         message, or a negative ::cancestry_tp_status_t.
 */
cancestry_tp_status_t cancestry_transport_rx_frame(cancestry_transport_t *tp,
                                                   const uint8_t *data,
                                                   uint8_t length);

/* ------------------------------------------------------------------------- */
/* Transmission                                                              */
/* ------------------------------------------------------------------------- */

/**
 * Request transmission of one message.
 *
 * A payload of at most 7 bytes is emitted as a Single Frame (synchronously);
 * anything longer is copied into the TX buffer and emitted as a First Frame,
 * with Consecutive Frames paced out by ticks after the receiver's Flow
 * Control arrives (SW-FR-TP-007/008).
 *
 * @param tp      Engine.
 * @param data    Payload bytes (borrowed; copied before returning).
 * @param length  Payload length, 1..min(TX capacity, 4095).
 * @return CANCESTRY_TP_OK, or CANCESTRY_TP_ERR_NULL / ERR_ARGUMENT /
 *         ERR_STATE while a transmission is pending / ERR_CAPACITY when the
 *         payload does not fit / ERR_NULL when no sink is configured.
 */
cancestry_tp_status_t cancestry_transport_send(cancestry_transport_t *tp,
                                               const uint8_t *data,
                                               size_t length);

/** @return true while a transmission is in progress or pending. */
bool cancestry_transport_tx_busy(const cancestry_transport_t *tp);

/* ------------------------------------------------------------------------- */
/* Time                                                                      */
/* ------------------------------------------------------------------------- */

/**
 * One 1 ms tick (SW-FR-TP-006).
 *
 * Advances the time base (the injected clock is read; without one the engine
 * advances by exactly 1000 us), then, in this fixed order:
 *   1. reception session: N_Cr expiry check, pending Flow Control retry
 *      under N_As;
 *   2. transmission session: N_Bs expiry check, pending frame retry under
 *      N_As, then all Consecutive Frames due under STmin and the block limit.
 *
 * @return CANCESTRY_TP_OK, or CANCESTRY_TP_ERR_TIMEOUT when a timer expired
 *         (the session was aborted and a TRANSPORT_TIMEOUT fault raised), or
 *         CANCESTRY_TP_ERR_NULL.
 */
cancestry_tp_status_t cancestry_transport_tick(cancestry_transport_t *tp);

/** Convenience: @p tick_count ticks in sequence (each one evaluated fully). */
cancestry_tp_status_t cancestry_transport_advance(cancestry_transport_t *tp,
                                                  uint32_t tick_count);

/* ------------------------------------------------------------------------- */
/* Inspection                                                                */
/* ------------------------------------------------------------------------- */

/** @return Reception session state, or IDLE when @p tp is invalid. */
cancestry_tp_state_t cancestry_transport_rx_state(const cancestry_transport_t *tp);

/** @return Transmission session state, or IDLE when @p tp is invalid. */
cancestry_tp_state_t cancestry_transport_tx_state(const cancestry_transport_t *tp);

/** @return Pointer to the live counters, or NULL when @p tp is invalid. */
const cancestry_tp_counters_t *cancestry_transport_counters(const cancestry_transport_t *tp);

/** @return The most recently raised fault, or NONE. */
cancestry_tp_fault_code_t cancestry_transport_last_fault(const cancestry_transport_t *tp);

/** @return Current engine time in microseconds. */
cancestry_time_us_t cancestry_transport_now_us(const cancestry_transport_t *tp);

#ifdef __cplusplus
}
#endif

#endif /* CANCESTRY_TP_ENGINE_H */
