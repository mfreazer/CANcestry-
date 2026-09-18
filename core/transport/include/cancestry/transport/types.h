/*
 * CANcestry - portable ISO-TP (ISO 15765-2) transport: shared types.
 *
 * Normative references:
 *   ISO 15765-2                       road vehicles -- diagnostic communication
 *                                     over CAN, part 2: transport protocol and
 *                                     network layer services (classic CAN only
 *                                     at this level)
 *   docs/software/SwRS.md             SW-FR-TP-001 .. SW-FR-TP-010 (Phase 10)
 *   docs/system/event-ordering.md     sections 2, 3 (time base, fault priority)
 *   docs/system/mode-fault-state-machine.md section 4 (fault severities)
 *   docs/system/SyRS.md               SYS-NF-001 (determinism), SYS-NF-002 (bounded resources)
 *
 * Design notes:
 *   - The engine operates on classic 8-byte CAN frames only. CAN FD ISO-TP
 *     (single-frame escape sequences, 32-bit FF lengths) is a later phase and
 *     is deliberately absent (SW-FR-TP-002 scope).
 *   - Zero heap allocation. The RX reassembly buffer and the TX segmentation
 *     buffer are caller-owned, statically sized arrays bound at init time.
 *     A message that does not fit is refused with TRANSPORT_BUFFER_OVERFLOW
 *     before a single payload byte is stored (SW-FR-TP-001, SW-FR-TP-003).
 *   - One engine instance serves one diagnostic addressing pair: an inbound
 *     (reception) session and an outbound (transmission) session, each with
 *     its own state variable, its own buffer and its own timers. The two
 *     sessions are independent, so a misbehaving tester interleaving a new
 *     request with an in-flight response cannot corrupt either session.
 *   - Fail-closed: a protocol violation (unexpected sequence number, invalid
 *     PCI, frame type that cannot apply in the current state, timeout)
 *     immediately drops the session, clears the buffer and raises the
 *     corresponding fault into the event queue (SW-FR-TP-004 .. SW-FR-TP-006).
 *     The engine never crashes, hangs, allocates or silently drops a frame it
 *     promised to transmit (SW-FR-TP-009).
 */

#ifndef CANCESTRY_TP_TYPES_H
#define CANCESTRY_TP_TYPES_H

#include "cancestry/event/types.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------------- */
/* Bounds (bounded resources: SYS-NF-002)                                    */
/* ------------------------------------------------------------------------- */

/** Classic CAN frame payload the transport accepts, in bytes. */
#define CANCESTRY_TP_FRAME_MAX_LENGTH ((uint8_t)8u)

/** Payload bytes a Single Frame can carry on classic CAN. */
#define CANCESTRY_TP_SF_DATA_MAX ((size_t)7u)

/** Payload bytes a First Frame carries on classic CAN. */
#define CANCESTRY_TP_FF_DATA_MAX ((size_t)6u)

/** Payload bytes a Consecutive Frame carries on classic CAN. */
#define CANCESTRY_TP_CF_DATA_MAX ((size_t)7u)

/**
 * Largest message the 12-bit First Frame length field can describe
 * (SW-FR-TP-007).
 */
#define CANCESTRY_TP_MESSAGE_MAX_LENGTH ((size_t)4095u)

/**
 * Default timer values in milliseconds, used when a
 * ::cancestry_tp_config_t field is left 0.
 *
 * N_As bounds the retransmission of a frame the platform sink declined;
 * N_Bs bounds the wait for Flow Control after the First Frame and after each
 * completed block; N_Cr bounds the receiver's wait for the next Consecutive
 * Frame (SW-FR-TP-006).
 */
#define CANCESTRY_TP_DEFAULT_N_AS_MS ((uint32_t)1000u)
#define CANCESTRY_TP_DEFAULT_N_BS_MS ((uint32_t)1000u)
#define CANCESTRY_TP_DEFAULT_N_CR_MS ((uint32_t)1000u)

/**
 * Default maximum number of consecutive Flow Control "Wait" frames accepted
 * before the transmission aborts with TRANSPORT_PROTOCOL_FAULT. ISO 15765-2
 * leaves the bound to the implementation; a bounded, deterministic limit is
 * mandatory here (SYS-NF-002).
 */
#define CANCESTRY_TP_DEFAULT_MAX_WAIT_FRAMES ((uint8_t)8u)

/* ------------------------------------------------------------------------- */
/* Status                                                                    */
/* ------------------------------------------------------------------------- */

/**
 * Operation results. Values >= 0 mean the operation succeeded.
 *
 * The negative ERR_PROTOCOL, ERR_OVERFLOW and ERR_TIMEOUT results report that
 * the engine aborted a session and raised the matching fault event; they never
 * mean the engine is unusable. Every abort leaves both sessions in a clean,
 * deterministic state.
 */
typedef enum cancestry_tp_status {
    /** Success. */
    CANCESTRY_TP_OK = 0,
    /** Success, and the call completed a message (delivered via the sink). */
    CANCESTRY_TP_OK_MESSAGE = 1,
    /** A required pointer argument was NULL or the engine is not initialized. */
    CANCESTRY_TP_ERR_NULL = -1,
    /** Malformed argument: zero-length payload or a frame longer than 8 bytes. */
    CANCESTRY_TP_ERR_ARGUMENT = -2,
    /** A transmission was requested while one is already in progress. */
    CANCESTRY_TP_ERR_STATE = -3,
    /** A transmit payload exceeded the TX buffer (or the 4095-byte FF limit). */
    CANCESTRY_TP_ERR_CAPACITY = -4,
    /**
     * Protocol violation: the session was dropped, the buffer cleared and a
     * TRANSPORT_PROTOCOL_FAULT raised (SW-FR-TP-004, SW-FR-TP-005).
     */
    CANCESTRY_TP_ERR_PROTOCOL = -5,
    /**
     * A received message exceeded the RX buffer: the transfer was aborted, an
     * OVFLW Flow Control frame was sent and a TRANSPORT_BUFFER_OVERFLOW fault
     * raised (SW-FR-TP-003).
     */
    CANCESTRY_TP_ERR_OVERFLOW = -6,
    /** A timer expired during the tick: the session was aborted and a
     *  TRANSPORT_TIMEOUT fault raised (SW-FR-TP-006). */
    CANCESTRY_TP_ERR_TIMEOUT = -7
} cancestry_tp_status_t;

/* ------------------------------------------------------------------------- */
/* Session state machine                                                     */
/* ------------------------------------------------------------------------- */

/**
 * Session states.
 *
 * One enum serves both sessions; the states a session may take are normative
 * (SW-FR-TP-001):
 *   - IDLE: no transfer in progress.
 *   - FIRST_FRAME: reception session; a First Frame was accepted, Consecutive
 *     Frames are being collected into the RX buffer.
 *   - FLOW_CONTROL: transmission session; the First Frame (or a block of
 *     Consecutive Frames) was sent and the engine waits for Flow Control.
 *   - CONSECUTIVE_FRAME: transmission session; Flow Control CTS was received
 *     and Consecutive Frames are being paced out under BS/STmin.
 */
typedef enum cancestry_tp_state {
    CANCESTRY_TP_STATE_IDLE = 0,
    CANCESTRY_TP_STATE_FIRST_FRAME = 1,
    CANCESTRY_TP_STATE_CONSECUTIVE_FRAME = 2,
    CANCESTRY_TP_STATE_FLOW_CONTROL = 3,
    CANCESTRY_TP_STATE_COUNT
} cancestry_tp_state_t;

/* ------------------------------------------------------------------------- */
/* Faults                                                                    */
/* ------------------------------------------------------------------------- */

/**
 * Transport fault codes.
 *
 * The numeric event code is ((source_id & 0xFFFFu) << 16) | fault, mirroring
 * the HAL encoding, so the fault manager can disambiguate sources without any
 * string lookup (SW-FR-TP-010).
 */
typedef enum cancestry_tp_fault_code {
    CANCESTRY_TP_FAULT_NONE = 0,
    /** Unexpected sequence number, invalid PCI, inapplicable frame type. */
    CANCESTRY_TP_FAULT_PROTOCOL = 1,
    /** Message exceeds the statically allocated buffer (either direction). */
    CANCESTRY_TP_FAULT_BUFFER_OVERFLOW = 2,
    /** N_Cr / N_Bs / N_As timer expiry. */
    CANCESTRY_TP_FAULT_TIMEOUT = 3,
    CANCESTRY_TP_FAULT_COUNT
} cancestry_tp_fault_code_t;

/* ------------------------------------------------------------------------- */
/* Configuration                                                             */
/* ------------------------------------------------------------------------- */

/**
 * Engine configuration. Zero-initialized fields select the defaults; a NULL
 * config pointer selects every default.
 */
typedef struct cancestry_tp_config {
    /** N_As: retransmission bound for a sink-declined frame, in ms. 0 = 1000. */
    uint32_t n_as_ms;
    /** N_Bs: wait bound for Flow Control, in ms. 0 = 1000. */
    uint32_t n_bs_ms;
    /** N_Cr: receiver wait bound for the next Consecutive Frame, in ms. 0 = 1000. */
    uint32_t n_cr_ms;
    /**
     * Block Size the engine advertises in its own Flow Control (CTS) frames:
     * how many Consecutive Frames the peer may send before the next FC.
     * 0 = no block limit.
     */
    uint8_t fc_block_size;
    /**
     * STmin the engine advertises in its own Flow Control frames, using the
     * ISO 15765-2 STmin encoding (0x00..0x7F ms, 0xF1..0xF9 100..900 us).
     */
    uint8_t fc_stmin;
    /** Maximum accepted consecutive FC Wait frames. 0 = 8. */
    uint8_t max_wait_frames;
    /** Fault source id encoded into raised fault events. */
    cancestry_source_id_t source_id;
} cancestry_tp_config_t;

/* ------------------------------------------------------------------------- */
/* Counters                                                                  */
/* ------------------------------------------------------------------------- */

/** Monotonic engine counters; never reset by the engine. */
typedef struct cancestry_tp_counters {
    /** 1 ms ticks executed. */
    uint32_t ticks;
    /** Frames accepted from the platform (cancestry_transport_rx_frame). */
    uint32_t frames_rx;
    /** Frames accepted by the sink (handed to the platform). */
    uint32_t frames_tx;
    /** Sink-declined frame transmission attempts. */
    uint32_t frames_tx_declined;
    /* Per frame type, received. */
    uint32_t sf_rx;
    uint32_t ff_rx;
    uint32_t cf_rx;
    uint32_t fc_rx;
    /* Per frame type, transmitted. */
    uint32_t sf_tx;
    uint32_t ff_tx;
    uint32_t cf_tx;
    uint32_t fc_tx;
    /** RX messages fully reassembled. */
    uint32_t rx_messages_completed;
    /** RX messages delivered to the on_message callback. */
    uint32_t rx_messages_delivered;
    /** RX messages completed while no on_message callback was configured. */
    uint32_t rx_messages_dropped_no_sink;
    /** TX messages accepted by cancestry_transport_send(). */
    uint32_t tx_messages_requested;
    /** TX messages whose last frame was accepted by the sink. */
    uint32_t tx_messages_completed;
    /** Sessions aborted by TRANSPORT_TIMEOUT. */
    uint32_t timeouts;
    /** Sessions aborted by TRANSPORT_PROTOCOL_FAULT. */
    uint32_t protocol_faults;
    /** Transfers aborted by TRANSPORT_BUFFER_OVERFLOW. */
    uint32_t buffer_overflows;
    /** FC Wait frames received. */
    uint32_t wait_frames_rx;
    /** Total fault events raised into the event queue. */
    uint32_t faults_raised;
} cancestry_tp_counters_t;

/* ------------------------------------------------------------------------- */
/* Helpers                                                                   */
/* ------------------------------------------------------------------------- */

/** @return true when @p status indicates success (>= 0). */
bool cancestry_tp_status_is_ok(cancestry_tp_status_t status);

/** @return Stable, statically allocated name for @p status. Never NULL. */
const char *cancestry_tp_status_name(cancestry_tp_status_t status);

/** @return Stable, statically allocated state name, or "INVALID". Never NULL. */
const char *cancestry_tp_state_name(cancestry_tp_state_t state);

/**
 * @return Stable, statically allocated fault name such as
 *         "TRANSPORT_PROTOCOL_FAULT", or "INVALID". Never NULL.
 */
const char *cancestry_tp_fault_name(cancestry_tp_fault_code_t fault);

/**
 * Severity of a transport fault for FAULT_RAISED events. All transport faults
 * abort one diagnostic session and leave the gateway function untouched, so
 * they map to WARNING; the fault manager may escalate on repetition
 * (docs/system/mode-fault-state-machine.md section 5).
 */
cancestry_fault_severity_t cancestry_tp_fault_severity(cancestry_tp_fault_code_t fault);

/**
 * Decode an ISO 15765-2 STmin byte into microseconds.
 *
 * 0x00..0x7F encode 0..127 ms; 0xF1..0xF9 encode 100..900 us. Reserved values
 * (0x80..0xF0, 0xFA..0xFF) decode to 0: they are accepted deterministically
 * and pace the fastest legal rate, matching the reference Linux ISO-TP
 * implementation, because the value only ever *slows* the sender down
 * (SW-FR-TP-008).
 */
uint32_t cancestry_tp_stmin_decode_us(uint8_t stmin);

/**
 * Encode milliseconds (0..127) into an ISO 15765-2 STmin byte. Values above
 * 127 clamp to 127.
 */
uint8_t cancestry_tp_stmin_encode_ms(uint32_t ms);

#ifdef __cplusplus
}
#endif

#endif /* CANCESTRY_TP_TYPES_H */
