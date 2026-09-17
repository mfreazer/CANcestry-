/*
 * CANcestry - Hardware Abstraction Layer: platform-independent interface.
 *
 * Normative references:
 *   docs/software/SwRS.md              SW-FR-HAL-003 (non-blocking),
 *                                      SW-FR-HAL-004 (hardware timestamping),
 *                                      SW-FR-HAL-005 (fail-closed),
 *                                      SW-FR-HAL-006 (bounded rings),
 *                                      SW-FR-HAL-007 (bus fault injection)
 *   docs/software/SwRS.md              SW-FR-CANFD-003 (deterministic CAN FD
 *                                      fallback), SW-FR-CANFD-005 (classic
 *                                      CAN keeps zero-overhead handling)
 *   docs/system/SyRS.md                SYS-NF-001 (determinism),
 *                                      SYS-NF-002 (zero heap allocation)
 *
 * The HAL connects the deterministic software core to physical CAN
 * interfaces. It owns the transition from hardware timestamps to the
 * CANcestry time base, and it is the only layer in the runtime path that
 * is allowed to issue syscalls (platform I/O). Everything above the HAL is
 * pure, synchronous logic.
 *
 * Operations:
 *   hal_init         Bind caller-owned RX/TX rings and interface config to a
 *                    HAL instance. All storage is caller-owned (SYS-NF-002).
 *   hal_poll_rx      Non-blocking read from the hardware. Delivers zero or
 *                    more RX frames as CAN_RX events into the provided
 *                    event queue, and raises FAULT_RAISED events on any
 *                    bus error (SYS-SF-002, SW-FR-HAL-007).
 *   hal_send_tx      Non-blocking enqueue a frame for transmission. The
 *                    frame is placed into the TX ring and the platform
 *                    backend drains it as fast as the bus allows.
 *   hal_get_status   Read a snapshot of per-interface status (counters,
 *                    state, last fault). Bounded, non-blocking.
 *
 * Zero-allocation rule (constraint 1 of issue #17, SYS-NF-002):
 *   The HAL shall never call malloc/calloc/realloc/free after init has
 *   returned. All ring buffers, control message buffers, and scratch space
 *   must be supplied by the caller. The conformance check in
 *   ci/check_no_alloc.py is extended to cover libcancestry_hal.a and
 *   libcancestry_platform_linux.a.
 *
 * CAN FD rule (issue #20, SW-FR-CANFD-003):
 *   A frame is only ever delivered or transmitted when the interface
 *   negotiated the protocol it uses. A CAN FD frame that reaches a
 *   classic-only interface is dropped and a
 *   CANCESTRY_HAL_FAULT_PROTOCOL_UNSUPPORTED fault is raised through the
 *   same deterministic path as every other HAL fault; it is never truncated
 *   to 8 bytes and never passed on. The check lives here, in the
 *   platform-independent core, so every backend (SocketCAN, mock, future
 *   targets) gets identical behaviour.
 *
 * Non-blocking rule (constraint 2, SW-FR-HAL-003):
 *   hal_poll_rx and hal_send_tx must return in bounded time. The SocketCAN
 *   backend uses non-blocking recvmsg/sendmsg only; a poll(2) with a zero
 *   timeout is permitted but blocking reads are not. The conformance suite
 *   verifies this contract on the mock HAL.
 *
 * Implements: SW-FR-HAL-003, SW-FR-HAL-004, SW-FR-HAL-005, SW-FR-HAL-006,
 *             SW-FR-HAL-007
 */

#ifndef CANCESTRY_HAL_H
#define CANCESTRY_HAL_H

#include "cancestry/event/clock.h"
#include "cancestry/event/queue.h"
#include "cancestry/hal/types.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------------- */
/* HAL instance                                                              */
/* ------------------------------------------------------------------------- */

/**
 * Maximum number of interfaces one HAL instance can manage.
 *
 * Bounded to keep per-frame dispatch arrays on the stack cheap.
 */
#define CANCESTRY_HAL_INSTANCE_MAX_INTERFACES ((uint8_t)CANCESTRY_HAL_MAX_INTERFACES)

/**
 * Opaque backend vtable.
 *
 * Platform implementations (Linux SocketCAN, mock) fill one out and bind it
 * to a HAL instance at init time. The vtable keeps the core HAL free of
 * any platform-specific headers.
 */
typedef struct cancestry_hal_backend cancestry_hal_backend_t;

/**
 * HAL instance.
 *
 * All mutable state lives here; it is caller-owned (typically static
 * storage). There is no global HAL state.
 */
typedef struct cancestry_hal {
    /** Backend vtable for the current platform. */
    const cancestry_hal_backend_t *backend;
    /** Opaque backend-private context; backend may store fd table here. */
    void *backend_context;

    /** Monotonic clock source the HAL uses to tag events. */
    cancestry_clock_t clock;

    /** Per-interface configuration, in registration order. */
    cancestry_hal_if_config_t ifaces[CANCESTRY_HAL_INSTANCE_MAX_INTERFACES];
    /** Per-interface RX rings (caller-owned, index-aligned with ifaces). */
    cancestry_hal_rx_ring_t *rx_rings[CANCESTRY_HAL_INSTANCE_MAX_INTERFACES];
    /** Per-interface TX rings (caller-owned, index-aligned with ifaces). */
    cancestry_hal_tx_ring_t *tx_rings[CANCESTRY_HAL_INSTANCE_MAX_INTERFACES];
    uint8_t iface_count;

    /** Per-interface runtime state (UP/DOWN/ERROR_PASSIVE/BUS_OFF). */
    cancestry_hal_if_state_t if_states[CANCESTRY_HAL_INSTANCE_MAX_INTERFACES];
    /**
     * Per-interface transport capabilities negotiated by the backend at open
     * time. Interfaces whose backend reports nothing are classic-only
     * (fail closed).
     */
    cancestry_hal_transport_caps_t if_caps[CANCESTRY_HAL_INSTANCE_MAX_INTERFACES];
    /** Per-interface count of frames rejected as PROTOCOL_UNSUPPORTED. */
    uint32_t if_protocol_rejected[CANCESTRY_HAL_INSTANCE_MAX_INTERFACES];
    /** Per-interface fault counters. */
    uint32_t if_faults[CANCESTRY_HAL_INSTANCE_MAX_INTERFACES];
    cancestry_hal_fault_code_t if_last_fault[CANCESTRY_HAL_INSTANCE_MAX_INTERFACES];
    /** Monotonic timestamp last observed on each interface (us). */
    cancestry_time_us_t if_last_ts[CANCESTRY_HAL_INSTANCE_MAX_INTERFACES];
    /** true after a successful init. */
    bool initialized;
} cancestry_hal_t;

/**
 * Initialization parameters.
 *
 * All pointer members refer to caller-owned storage that must outlive the
 * HAL instance.
 */
typedef struct cancestry_hal_config {
    /** Platform backend vtable; required. */
    const cancestry_hal_backend_t *backend;
    /** Backend-private context; passed to every backend call. May be NULL. */
    void *backend_context;
    /** Monotonic clock used for event timestamps; required. */
    const cancestry_clock_t *clock;
    /** Interface configurations, one per physical interface. */
    const cancestry_hal_if_config_t *ifaces;
    uint8_t iface_count;
    /**
     * Per-interface RX rings, index-aligned with @c ifaces. Each must have
     * been initialised with cancestry_hal_rx_ring_init() before being passed
     * in. May be NULL; when NULL the interface receives no RX (listen-only
     * or TX-only).
     */
    cancestry_hal_rx_ring_t *const *rx_rings;
    /**
     * Per-interface TX rings, index-aligned with @c ifaces. Each must have
     * been initialised with cancestry_hal_tx_ring_init() before being passed
     * in. May be NULL; when NULL the interface is RX-only.
     */
    cancestry_hal_tx_ring_t *const *tx_rings;
} cancestry_hal_config_t;

/* ------------------------------------------------------------------------- */
/* Core API                                                                  */
/* ------------------------------------------------------------------------- */

/**
 * Initialize a HAL instance.
 *
 * Opens every configured interface through the backend, binds RX/TX rings,
 * and validates all configuration. After a successful return the HAL is
 * ready for non-blocking poll/send. If any interface fails to open the
 * function returns false and the HAL is left in a safely unusable state
 * (fail-closed: SW-FR-HAL-005).
 *
 * @return true on success, false on NULL arguments or on any backend open
 *         failure.
 */
bool cancestry_hal_init(cancestry_hal_t *hal, const cancestry_hal_config_t *config);

/**
 * Non-blocking poll: drain whatever hardware has waiting right now into the
 * RX rings, then convert every dequeued RX frame into a CANCESTRY_EVENT_TYPE_CAN_RX
 * on @p event_queue. Any bus errors surfaced by the backend become
 * CANCESTRY_EVENT_TYPE_FAULT_RAISED events with FAULT priority
 * (SYS-SF-002, SW-FR-HAL-007).
 *
 * This function MUST NOT block. The backend is contractually required to use
 * non-blocking I/O; if the backend blocks, the bug is in the backend.
 *
 * @param hal          Initialized HAL.
 * @param event_queue  Destination queue for CAN_RX and FAULT events. Faults
 *                     are enqueued with CANCESTRY_PRIORITY_CLASS_FAULT so
 *                     they pre-empt data (event-ordering.md section 3).
 * @param out_rx_count If non-NULL, receives the number of RX frames
 *                     delivered to the queue in this call.
 * @param out_fault_count If non-NULL, receives the number of fault events
 *                     enqueued in this call.
 * @return CANCESTRY_HAL_OK on success (including the idle case where zero
 *         frames arrived), or a negative status on API misuse.
 */
cancestry_hal_status_t cancestry_hal_poll_rx(cancestry_hal_t *hal,
                                              cancestry_event_queue_t *event_queue,
                                              uint32_t *out_rx_count,
                                              uint32_t *out_fault_count);

/**
 * Non-blocking transmit: enqueue a frame into the destination interface's
 * TX ring and ask the backend to drain as much as the bus will take right
 * now.
 *
 * If the TX ring is full the frame is dropped, a RX-ring overflow equivalent
 * fault is raised through @p event_queue, and CANCESTRY_HAL_ERR_RING_FULL
 * is returned (SW-FR-HAL-006, fail-closed).
 *
 * @param hal          Initialized HAL.
 * @param event_queue  Destination for any fault events caused by this send.
 * @param frame        Frame to transmit; copied into the TX ring.
 * @return CANCESTRY_HAL_OK when the frame was accepted, negative status on
 *         error.
 */
cancestry_hal_status_t cancestry_hal_send_tx(cancestry_hal_t *hal,
                                              cancestry_event_queue_t *event_queue,
                                              const cancestry_hal_frame_t *frame);

/**
 * @return true when @p interface_id refers to a configured interface whose
 *         backend negotiated CAN FD support.
 *
 * Returns false for an unknown interface, an uninitialized HAL, and any
 * interface whose backend did not report CAN FD, so callers can never send
 * an FD frame somewhere it would be dropped (SW-FR-CANFD-003).
 */
bool cancestry_hal_iface_can_fd(const cancestry_hal_t *hal, cancestry_interface_id_t interface_id);

/**
 * Read a bounded snapshot of per-interface status.
 *
 * @param hal      Initialized HAL.
 * @param out      Array of @c capacity status slots to fill.
 * @param capacity Maximum number of status entries to write.
 * @param out_count If non-NULL, receives the number of entries written
 *                 (clamped to @p capacity).
 * @return CANCESTRY_HAL_OK on success.
 */
cancestry_hal_status_t cancestry_hal_get_status(const cancestry_hal_t *hal,
                                                 cancestry_hal_if_status_t *out,
                                                 uint8_t capacity,
                                                 uint8_t *out_count);

/**
 * Raise a HAL fault into the event queue.
 *
 * Used by backends when they detect a bus error during polling or sending.
 * Exposed so the mock HAL and the SocketCAN backend share the same fault
 * shaping logic (deterministic, no allocation).
 */
cancestry_event_queue_status_t cancestry_hal_raise_fault(cancestry_hal_t *hal,
                                                         cancestry_event_queue_t *event_queue,
                                                         cancestry_interface_id_t interface_id,
                                                         cancestry_hal_fault_code_t fault,
                                                         cancestry_fault_severity_t severity);

/* ------------------------------------------------------------------------- */
/* Backend vtable (platform implementors fill these in)                      */
/* ------------------------------------------------------------------------- */

/**
 * Open the interface named by @c cfg->name and prepare for non-blocking I/O.
 *
 * @return true on success; false leaves the interface DOWN and hal_init
 *         reports the failure via a CANCESTRY_HAL_FAULT_INIT_FAILED event.
 */
typedef bool (*cancestry_hal_backend_open_fn)(void *ctx,
                                               const cancestry_hal_if_config_t *cfg,
                                               uint8_t iface_index);

/**
 * Close/release resources for one interface. Must be idempotent.
 */
typedef void (*cancestry_hal_backend_close_fn)(void *ctx, uint8_t iface_index);

/**
 * Non-blocking read from one interface. The backend should read all frames
 * currently available and push them into @p ring with
 * cancestry_hal_rx_ring_push(). It shall NOT block.
 *
 * @param[out] out_fault  Set to a non-NONE fault code when the backend
 *                        detects a bus error that should be raised; the
 *                        core converts it to an event.
 */
typedef cancestry_hal_status_t (*cancestry_hal_backend_poll_fn)(void *ctx,
                                                                 uint8_t iface_index,
                                                                 cancestry_hal_rx_ring_t *ring,
                                                                 cancestry_hal_fault_code_t *out_fault);

/**
 * Non-blocking drain of one interface's TX ring. The backend should pop
 * frames with cancestry_hal_tx_ring_pop() and send them to hardware using
 * non-blocking I/O until the ring is empty or sendmsg returns EAGAIN.
 */
typedef cancestry_hal_status_t (*cancestry_hal_backend_drain_tx_fn)(void *ctx,
                                                                     uint8_t iface_index,
                                                                     cancestry_hal_tx_ring_t *ring,
                                                                     cancestry_hal_fault_code_t *out_fault);

/**
 * Report the transport capabilities negotiated for one interface.
 *
 * Called by hal_init immediately after a successful open. The backend fills
 * @p out_caps; returning false, or providing no @c caps entry at all, means
 * "classic CAN only" (fail closed, SW-FR-CANFD-003).
 */
typedef bool (*cancestry_hal_backend_caps_fn)(void *ctx,
                                               uint8_t iface_index,
                                               cancestry_hal_transport_caps_t *out_caps);

struct cancestry_hal_backend {
    cancestry_hal_backend_open_fn open;
    cancestry_hal_backend_close_fn close;
    cancestry_hal_backend_poll_fn poll;
    cancestry_hal_backend_drain_tx_fn drain_tx;
    /** Optional; NULL is treated as "classic CAN only". */
    cancestry_hal_backend_caps_fn caps;
};

#ifdef __cplusplus
}
#endif

#endif /* CANCESTRY_HAL_H */
