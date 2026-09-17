/*
 * CANcestry - Hardware Abstraction Layer: hardware-agnostic types.
 *
 * Normative references:
 *   docs/software/SwRS.md              SW-FR-HAL-001 .. SW-FR-HAL-012,
 *                                      SW-FR-CANFD-002 .. SW-FR-CANFD-005
 *   docs/system/SyRS.md                SYS-NF-001 (determinism),
 *                                      SYS-NF-002 (zero heap allocation),
 *                                      SYS-SF-002 (fail-closed)
 *   schemas/hal-0.1.0.schema.json      HAL configuration schema (schema is law)
 *
 * Design notes:
 *   - The HAL frame is a fixed-size value type. It owns no pointers to
 *     dynamically allocated data and can be copied by assignment. The
 *     payload buffer is statically sized for the widest frame CANcestry
 *     accepts (CAN FD, 64 bytes) so no frame is ever resized at runtime;
 *     classic traffic copies only its declared length (SW-FR-CANFD-005).
 *   - Timestamps are CANcestry monotonic microseconds (cancestry_time_us_t) so
 *     they slot directly into core/event without conversion or floating point.
 *   - All buffers are caller-owned. The HAL never calls malloc/calloc/realloc/
 *     free; RX/TX rings, control messages, and scratch storage are passed in
 *     at init time (SYS-NF-002, constraint 1 of issue #17).
 *   - The HAL is strictly non-blocking (SW-FR-HAL-003). poll_rx drains whatever
 *     the kernel has waiting right now and returns; send_tx performs one
 *     non-blocking write. Either function must complete in bounded time.
 *   - Fail-closed I/O (SYS-SF-002, constraint 4): every syscall failure, bus
 *     error, or dropped frame is surfaced through a HAL fault event enqueued
 *     into the core/event queue rather than crashing the runtime.
 *
 * Implements: SW-FR-HAL-001, SW-FR-HAL-002 (hardware-agnostic types)
 */

#ifndef CANCESTRY_HAL_TYPES_H
#define CANCESTRY_HAL_TYPES_H

#include "cancestry/event/clock.h"
#include "cancestry/event/types.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------------- */
/* Limits (bounded resources, SYS-NF-002)                                    */
/* ------------------------------------------------------------------------- */

/** Classic CAN payload length in bytes. */
#define CANCESTRY_HAL_CLASSIC_FRAME_MAX_LENGTH ((uint8_t)CANCESTRY_CAN_FRAME_MAX_LENGTH)

/**
 * Widest payload a HAL frame can carry, in bytes (CAN FD, issue #20).
 *
 * The buffer is part of the caller-owned frame struct and is never resized;
 * the HAL rejects rather than truncates any frame that does not fit
 * (SW-FR-CANFD-003, fail closed).
 */
#define CANCESTRY_HAL_FRAME_MAX_LENGTH ((uint8_t)CANCESTRY_CAN_FD_FRAME_MAX_LENGTH)

/** Maximum interface name length accepted by the HAL ("vcan0" etc.). */
#define CANCESTRY_HAL_INTERFACE_NAME_MAX ((size_t)16u)

/** Maximum number of interfaces one HAL instance can drive. */
#define CANCESTRY_HAL_MAX_INTERFACES ((uint8_t)4u)

/**
 * Fault codes raised by the HAL into core/event.
 *
 * Every code maps to a CANCESTRY_EVENT_TYPE_FAULT_RAISED with severity
 * WARNING or higher, per docs/system/mode-fault-state-machine.md. The numeric
 * values are part of the normative contract (the conformance suite pins them
 * so trace captures are stable).
 */
typedef enum cancestry_hal_fault_code {
    CANCESTRY_HAL_FAULT_NONE = 0,
    /** RX ring was full when a frame arrived; the new frame was dropped. */
    CANCESTRY_HAL_FAULT_RX_RING_OVERFLOW = 1,
    /** TX ring was full when send was called; the outgoing frame was dropped. */
    CANCESTRY_HAL_FAULT_TX_RING_OVERFLOW = 2,
    /** A syscall returned EAGAIN/EWOULDBLOCK; the bus is back-pressuring. */
    CANCESTRY_HAL_FAULT_BUS_BACKPRESSURE = 3,
    /** File descriptor became invalid (EBADF/EPIPE); the interface is down. */
    CANCESTRY_HAL_FAULT_INTERFACE_DOWN = 4,
    /** Controller reports Error Passive (REC/TEC >= 128). */
    CANCESTRY_HAL_FAULT_BUS_ERROR_PASSIVE = 5,
    /** Controller reports Bus Off (TEC >= 256); interface must be reset. */
    CANCESTRY_HAL_FAULT_BUS_OFF = 6,
    /** RX timestamp was not strictly monotonic; dropped frame to preserve order. */
    CANCESTRY_HAL_FAULT_TIMESTAMP_NON_MONOTONIC = 7,
    /** Frame on the wire violated DLC/ID expectations (malformed). */
    CANCESTRY_HAL_FAULT_MALFORMED_FRAME = 8,
    /** Initialization failed (e.g., socket() returned an error). */
    CANCESTRY_HAL_FAULT_INIT_FAILED = 9,
    /**
     * A CAN FD frame reached an interface that does not support CAN FD (or a
     * CAN FD transmit was attempted on one). The frame is dropped, never
     * truncated (SW-FR-CANFD-003, fail closed).
     */
    CANCESTRY_HAL_FAULT_PROTOCOL_UNSUPPORTED = 10,
    CANCESTRY_HAL_FAULT_COUNT
} cancestry_hal_fault_code_t;

/** HAL operational state for one interface. */
typedef enum cancestry_hal_if_state {
    /** Not configured / not opened. */
    CANCESTRY_HAL_IF_STATE_DOWN = 0,
    /** Opened, actively receiving and transmitting. */
    CANCESTRY_HAL_IF_STATE_UP = 1,
    /** Error Passive; RX/TX continue but faults are raised. */
    CANCESTRY_HAL_IF_STATE_ERROR_PASSIVE = 2,
    /** Bus Off; no TX permitted, RX may continue for diagnosis. */
    CANCESTRY_HAL_IF_STATE_BUS_OFF = 3,
    CANCESTRY_HAL_IF_STATE_COUNT
} cancestry_hal_if_state_t;

/* ------------------------------------------------------------------------- */
/* Frame                                                                     */
/* ------------------------------------------------------------------------- */

/**
 * A received or to-be-transmitted CAN frame.
 *
 * The layout deliberately mirrors cancestry_can_rx_payload_t so that an RX
 * frame can be placed into core/event without copying fields one-by-one.
 */
typedef struct cancestry_hal_frame {
    cancestry_interface_id_t interface_id;
    uint32_t can_id;
    /** Non-zero when can_id carries an extended (29-bit) identifier. */
    uint8_t is_extended;
    /**
     * Non-zero when this is a CAN FD frame.
     *
     * Only an FD frame may carry more than
     * CANCESTRY_HAL_CLASSIC_FRAME_MAX_LENGTH bytes, and only an interface
     * whose negotiated capabilities include CAN FD accepts one.
     */
    uint8_t is_fd;
    /**
     * Payload length in bytes, not a raw DLC code: for CAN FD the platform
     * backend has already translated DLC 9..15 to 12/16/20/24/32/48/64.
     * cancestry_can_payload_length_is_valid(is_fd, length) holds for every
     * frame the HAL accepts (SW-FR-CANFD-002).
     */
    uint8_t length;
    uint8_t data[CANCESTRY_HAL_FRAME_MAX_LENGTH];
    /**
     * Hardware/kernel timestamp in the CANcestry monotonic time base. For the
     * Linux SocketCAN backend this is the SO_TIMESTAMPNS value converted to
     * microseconds. The conformance suite proves this sequence is strictly
     * monotonic (HAL-TIMESTAMP-MONO-001).
     */
    cancestry_time_us_t timestamp_us;
} cancestry_hal_frame_t;

/* ------------------------------------------------------------------------- */
/* Bounded RX/TX rings (caller-owned, SYS-NF-002)                            */
/* ------------------------------------------------------------------------- */

/**
 * Caller-owned RX ring.
 *
 * Frames received from the hardware are appended at @c write_idx and consumed
 * by hal_poll_rx from @c read_idx. Indices wrap modulo capacity, giving a
 * lock-free single-producer / single-consumer ring that needs no allocation.
 *
 * The ring never silently overwrites unread data: full means the new frame
 * is dropped, @c dropped++ is incremented, and a fault event is raised
 * (SW-FR-HAL-006, HAL-RING-BOUNDS-001).
 */
typedef struct cancestry_hal_rx_ring {
    cancestry_hal_frame_t *slots;
    uint16_t capacity;
    uint16_t read_idx;
    uint16_t write_idx;
    uint16_t count;
    uint32_t dropped;     /* frames dropped because the ring was full */
    uint32_t rx_total;    /* total frames successfully accepted */
} cancestry_hal_rx_ring_t;

/**
 * Caller-owned TX ring.
 *
 * Outgoing frames are queued here by hal_send_tx and drained by the platform
 * driver during hal_poll_tx / hal_drain_tx (on Linux this is a non-blocking
 * sendmsg() loop).
 */
typedef struct cancestry_hal_tx_ring {
    cancestry_hal_frame_t *slots;
    uint16_t capacity;
    uint16_t read_idx;
    uint16_t write_idx;
    uint16_t count;
    uint32_t dropped;   /* frames rejected because the ring was full */
    uint32_t sent;      /* frames successfully handed to the hardware */
    uint32_t tx_total;  /* total frames submitted via hal_send_tx */
} cancestry_hal_tx_ring_t;

/* ------------------------------------------------------------------------- */
/* Error / status                                                            */
/* ------------------------------------------------------------------------- */

/** Per-interface status snapshot. */
typedef struct cancestry_hal_if_status {
    cancestry_interface_id_t interface_id;
    char name[CANCESTRY_HAL_INTERFACE_NAME_MAX];
    cancestry_hal_if_state_t state;
    uint32_t rx_count;
    uint32_t tx_count;
    uint32_t rx_dropped;
    uint32_t tx_dropped;
    uint32_t fault_count;
    /** Last fault code observed, or CANCESTRY_HAL_FAULT_NONE. */
    cancestry_hal_fault_code_t last_fault;
    /**
     * Frames rejected because their protocol is not supported by the
     * interface (CAN FD on a classic interface), or whose payload length is
     * not representable (SW-FR-CANFD-003).
     */
    uint32_t rx_protocol_rejected;
    /** Negotiated CAN FD support for this interface. */
    bool can_fd;
    /** Last hardware timestamp observed on this interface (monotonic us). */
    cancestry_time_us_t last_rx_timestamp_us;
} cancestry_hal_if_status_t;

/** HAL API return codes. Values >= 0 mean success. */
typedef enum cancestry_hal_status {
    CANCESTRY_HAL_OK = 0,
    /** One or more frames were delivered (poll_rx). */
    CANCESTRY_HAL_OK_FRAMES_AVAILABLE = 1,
    /** No frames ready / nothing to do. */
    CANCESTRY_HAL_OK_IDLE = 2,
    /** NULL argument or uninitialized HAL. */
    CANCESTRY_HAL_ERR_NULL = -1,
    /** An argument violates a contract (bad capacity, bad id). */
    CANCESTRY_HAL_ERR_ARGUMENT = -2,
    /** The interface is not configured / unknown. */
    CANCESTRY_HAL_ERR_NO_INTERFACE = -3,
    /** RX/TX ring is full; frame was dropped. */
    CANCESTRY_HAL_ERR_RING_FULL = -4,
    /** Underlying I/O error (e.g., bad fd); fail-closed, fault raised. */
    CANCESTRY_HAL_ERR_IO = -5,
    /** Initialization failed. */
    CANCESTRY_HAL_ERR_INIT = -6,
    /**
     * The interface does not support the frame's protocol (CAN FD on a
     * classic interface). The frame was not queued and not truncated; a
     * PROTOCOL_UNSUPPORTED fault was raised (SW-FR-CANFD-003).
     */
    CANCESTRY_HAL_ERR_UNSUPPORTED = -7
} cancestry_hal_status_t;

/* ------------------------------------------------------------------------- */
/* Configuration                                                             */
/* ------------------------------------------------------------------------- */

/**
 * Per-interface static configuration supplied to hal_init().
 *
 * Interface names and bitrates come from a YAML file validated against
 * schemas/hal-0.1.0.schema.json (SW-FR-HAL-012, "schema is law").
 */
typedef struct cancestry_hal_if_config {
    cancestry_interface_id_t interface_id;
    char name[CANCESTRY_HAL_INTERFACE_NAME_MAX];
    uint32_t bitrate;   /* bits per second, e.g. 500000 for 500 kbit/s */
    /** Non-zero to open in listen-only mode (no TX allowed). */
    uint8_t listen_only;
    /**
     * Non-zero to request CAN FD on this interface.
     *
     * Requesting CAN FD never fails the open: when the hardware or driver
     * cannot provide it the interface stays classic-only and every CAN FD
     * frame is rejected with a PROTOCOL_UNSUPPORTED fault instead of being
     * truncated (SW-FR-CANFD-003). Query the negotiated result with
     * cancestry_hal_iface_can_fd().
     */
    uint8_t can_fd;
    /**
     * CAN FD data bitrate in bits per second, or 0 to let the platform pick
     * its default. Only meaningful when @c can_fd is non-zero.
     */
    uint32_t data_bitrate;
} cancestry_hal_if_config_t;

/**
 * Negotiated transport capabilities of one interface.
 *
 * Filled in by the platform backend at open time and reported through
 * cancestry_hal_iface_can_fd(). An interface whose backend reports nothing
 * is treated as classic-only (fail closed).
 */
typedef struct cancestry_hal_transport_caps {
    cancestry_interface_id_t interface_id;
    /** True when the interface negotiated CAN FD. */
    bool can_fd;
    /** Widest payload the interface accepts, in bytes (8 or 64). */
    uint8_t max_length;
} cancestry_hal_transport_caps_t;

/* ------------------------------------------------------------------------- */
/* Helpers                                                                   */
/* ------------------------------------------------------------------------- */

/** Zero and initialize a caller-owned RX ring. */
bool cancestry_hal_rx_ring_init(cancestry_hal_rx_ring_t *ring,
                                cancestry_hal_frame_t *storage,
                                uint16_t capacity);

/** Zero and initialize a caller-owned TX ring. */
bool cancestry_hal_tx_ring_init(cancestry_hal_tx_ring_t *ring,
                                cancestry_hal_frame_t *storage,
                                uint16_t capacity);

/** @return Current occupancy of the RX ring, or 0 if @p ring is NULL. */
uint16_t cancestry_hal_rx_ring_count(const cancestry_hal_rx_ring_t *ring);

/** @return Current occupancy of the TX ring, or 0 if @p ring is NULL. */
uint16_t cancestry_hal_tx_ring_count(const cancestry_hal_tx_ring_t *ring);

/**
 * Push a frame into the RX ring.
 *
 * Used by platform backends (SocketCAN, mock) on the producer side.
 *
 * @return CANCESTRY_HAL_OK on success, CANCESTRY_HAL_ERR_RING_FULL if the
 *         ring was full (the frame is dropped and @c dropped is incremented).
 */
cancestry_hal_status_t cancestry_hal_rx_ring_push(cancestry_hal_rx_ring_t *ring,
                                                   const cancestry_hal_frame_t *frame);

/**
 * Pop the oldest frame from the RX ring.
 *
 * @return CANCESTRY_HAL_OK on success, CANCESTRY_HAL_OK_IDLE if empty.
 */
cancestry_hal_status_t cancestry_hal_rx_ring_pop(cancestry_hal_rx_ring_t *ring,
                                                  cancestry_hal_frame_t *out_frame);

/**
 * Push a frame into the TX ring for the platform to drain.
 *
 * @return CANCESTRY_HAL_OK on success, CANCESTRY_HAL_ERR_RING_FULL if the
 *         ring was full (frame dropped, @c dropped incremented).
 */
cancestry_hal_status_t cancestry_hal_tx_ring_push(cancestry_hal_tx_ring_t *ring,
                                                   const cancestry_hal_frame_t *frame);

/**
 * Pop the oldest pending TX frame.
 *
 * @return CANCESTRY_HAL_OK on success, CANCESTRY_HAL_OK_IDLE if empty.
 */
cancestry_hal_status_t cancestry_hal_tx_ring_pop(cancestry_hal_tx_ring_t *ring,
                                                  cancestry_hal_frame_t *out_frame);

/**
 * @return true when @p frame is non-NULL and internally consistent:
 *         its payload length is representable for its frame kind and it fits
 *         the frame buffer.
 *
 * A classic frame carrying more than
 * CANCESTRY_HAL_CLASSIC_FRAME_MAX_LENGTH bytes, or an FD frame whose length
 * is not one of the CAN FD payload lengths, is malformed. The HAL applies
 * this check on ingress and egress so no layer ever has to guess what a
 * well-formed frame looks like (SW-FR-CANFD-002).
 */
bool cancestry_hal_frame_is_valid(const cancestry_hal_frame_t *frame);

/** @return Stable, statically allocated name for @p state. Never NULL. */
const char *cancestry_hal_if_state_name(cancestry_hal_if_state_t state);

/** @return Stable, statically allocated name for @p fault. Never NULL. */
const char *cancestry_hal_fault_code_name(cancestry_hal_fault_code_t fault);

/** @return true when @p status indicates success. */
bool cancestry_hal_status_is_ok(cancestry_hal_status_t status);

/** @return Stable, statically allocated name for @p status. Never NULL. */
const char *cancestry_hal_status_name(cancestry_hal_status_t status);

#ifdef __cplusplus
}
#endif

#endif /* CANCESTRY_HAL_TYPES_H */
