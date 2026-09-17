/*
 * CANcestry - Mock HAL backend (for CI and conformance testing).
 *
 * The mock HAL is a deterministic in-process backend that simulates a CAN
 * bus without touching the kernel. Tests use it to inject RX frames,
 * capture TX frames, and synthesise bus errors on command (SW-FR-HAL-008,
 * SW-FR-HAL-009).
 *
 * It is strictly non-blocking and zero-alloc, like the real backend.
 *
 * Implements: SW-FR-HAL-008 (mock backend for CI),
 *             SW-FR-HAL-009 (deterministic error injection),
 *             SW-FR-CANFD-003 (classic-only vs FD-capable interface, used by
 *             the CAN FD fallback conformance test)
 */

#ifndef CANCESTRY_PLATFORM_MOCK_HAL_H
#define CANCESTRY_PLATFORM_MOCK_HAL_H

#include "cancestry/hal/hal.h"
#include "cancestry/hal/types.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Maximum number of interfaces supported by the mock. */
#define CANCESTRY_MOCK_HAL_MAX_INTERFACES ((uint8_t)CANCESTRY_HAL_MAX_INTERFACES)

/** Maximum number of pre-loaded RX frames per interface (injection queue). */
#define CANCESTRY_MOCK_HAL_INJECT_CAPACITY ((uint16_t)256u)

/** Maximum number of TX frames captured per interface. */
#define CANCESTRY_MOCK_HAL_TX_CAPTURE_CAPACITY ((uint16_t)256u)

/** Per-interface mock state. */
/**
 * Per-interface mock state.
 *
 * @c fd_support defaults to false, i.e. every mock interface is a classic
 * CAN interface until a test explicitly opts it into CAN FD with
 * cancestry_mock_hal_set_fd_support(). That default is what makes the
 * "CAN FD frame on a classic-only interface" fallback path testable
 * (SW-FR-CANFD-003).
 */
typedef struct cancestry_mock_iface {
    bool opened;
    bool fd_support; /**< negotiated CAN FD capability reported to the HAL */
    bool force_poll_fault;
    bool force_drain_fault;
    cancestry_hal_fault_code_t next_poll_fault;
    cancestry_hal_fault_code_t next_drain_fault;
    uint32_t poll_call_count;
    uint32_t drain_call_count;

    /* Frames the test has pre-injected for the next poll. */
    cancestry_hal_frame_t inject_slots[CANCESTRY_MOCK_HAL_INJECT_CAPACITY];
    uint16_t inject_count;
    bool inject_auto_timestamps; /**< when true, populate timestamp_us automatically */
    cancestry_time_us_t inject_next_timestamp;
    cancestry_time_us_t inject_timestamp_step;

    /* Frames that the HAL sent via drain_tx. */
    cancestry_hal_frame_t tx_capture[CANCESTRY_MOCK_HAL_TX_CAPTURE_CAPACITY];
    uint16_t tx_capture_count;
    uint32_t tx_total_captured;
    /** Number of injected RX frames that were CAN FD frames. */
    uint32_t fd_frames_injected;
} cancestry_mock_iface_t;

/**
 * Mock HAL instance (backend context).
 *
 * Pass this to cancestry_hal_config_t::backend_context when configuring the
 * HAL with the mock backend.
 */
typedef struct cancestry_mock_hal {
    cancestry_mock_iface_t ifaces[CANCESTRY_MOCK_HAL_MAX_INTERFACES];
    uint8_t iface_count;
} cancestry_mock_hal_t;

/**
 * Return a pointer to the singleton mock backend vtable.
 */
const cancestry_hal_backend_t *cancestry_mock_hal_backend(void);

/**
 * Initialize all interfaces in the mock to a known reset state.
 *
 * Call this before hal_init() in every test to get a clean fixture.
 */
void cancestry_mock_hal_init(cancestry_mock_hal_t *mock);

/**
 * Pre-inject an RX frame that the next cancestry_hal_poll_rx() will deliver.
 *
 * The mock copies @p frame. If @p frame->timestamp_us is 0 and auto
 * timestamps are enabled (default), the mock stamps the frame with a
 * strictly advancing synthetic clock.
 *
 * @return true on success, false when the injection queue is full.
 */
bool cancestry_mock_hal_inject_rx(cancestry_mock_hal_t *mock,
                                   uint8_t iface_index,
                                   const cancestry_hal_frame_t *frame);

/**
 * Schedule the next poll() to report the given fault code. The fault is
 * delivered exactly once on the next poll, then cleared.
 */
void cancestry_mock_hal_set_next_poll_fault(cancestry_mock_hal_t *mock,
                                              uint8_t iface_index,
                                              cancestry_hal_fault_code_t fault);

/**
 * Schedule the next drain_tx() to report the given fault code. Delivered
 * exactly once on the next drain (triggered by poll_rx or send_tx).
 */
void cancestry_mock_hal_set_next_drain_fault(cancestry_mock_hal_t *mock,
                                               uint8_t iface_index,
                                               cancestry_hal_fault_code_t fault);

/**
 * Declare whether @p iface_index speaks CAN FD.
 *
 * Call before cancestry_hal_init(): the HAL reads the capability once at open
 * time. With @p can_fd false (the default) the interface is classic-only and
 * the HAL rejects any injected CAN FD frame with a PROTOCOL_UNSUPPORTED
 * fault; with @p can_fd true the same frame is delivered with its full
 * payload (SW-FR-CANFD-003).
 */
void cancestry_mock_hal_set_fd_support(cancestry_mock_hal_t *mock,
                                        uint8_t iface_index,
                                        bool can_fd);

/**
 * @return true when @p iface_index is configured as a CAN FD interface.
 */
bool cancestry_mock_hal_fd_support(const cancestry_mock_hal_t *mock, uint8_t iface_index);

/**
 * Configure automatic timestamping of injected RX frames. When enabled,
 * every injected frame with timestamp_us == 0 is stamped at
 * next_timestamp which is then advanced by step. Both are in microseconds.
 */
void cancestry_mock_hal_set_auto_timestamps(cancestry_mock_hal_t *mock,
                                              uint8_t iface_index,
                                              cancestry_time_us_t start,
                                              cancestry_time_us_t step);

/**
 * @return Number of TX frames captured for @p iface_index (across all
 *         calls; wraps at CANCESTRY_MOCK_HAL_TX_CAPTURE_CAPACITY).
 */
uint16_t cancestry_mock_hal_tx_count(const cancestry_mock_hal_t *mock, uint8_t iface_index);

/**
 * Read a captured TX frame.
 *
 * @return Pointer to the captured frame at @p slot, or NULL when out of range.
 */
const cancestry_hal_frame_t *cancestry_mock_hal_tx_at(const cancestry_mock_hal_t *mock,
                                                        uint8_t iface_index,
                                                        uint16_t slot);

#ifdef __cplusplus
}
#endif

#endif /* CANCESTRY_PLATFORM_MOCK_HAL_H */
