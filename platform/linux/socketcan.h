/*
 * CANcestry - Linux SocketCAN backend.
 *
 * Implements non-blocking I/O over PF_CAN/SOCK_RAW sockets with hardware
 * timestamping (SO_TIMESTAMPNS) per constraint 3 of issue #17.
 *
 * Zero-allocation: this translation unit never calls malloc/calloc/realloc/
 * free. All socket control-message buffers are declared on the stack in
 * poll() and sized to hold exactly one timestamp; file descriptors live in
 * the caller-supplied context struct.
 *
 * CAN FD (issue #20): an interface configured with can_fd negotiates
 * CAN_RAW_FD_FRAMES at open time and records the result in @c fd_enabled.
 * Negotiation failure is not an open failure - the interface stays classic
 * and the HAL rejects CAN FD frames on it (SW-FR-CANFD-003/004).
 *
 * Implements: SW-FR-HAL-010 (Linux SocketCAN backend),
 *             SW-FR-HAL-003 (non-blocking I/O),
 *             SW-FR-HAL-004 (hardware/kernel timestamping),
 *             SW-FR-HAL-005 (fail-closed on syscall errors),
 *             SW-FR-CANFD-003, SW-FR-CANFD-004
 */

#ifndef CANCESTRY_PLATFORM_LINUX_SOCKETCAN_H
#define CANCESTRY_PLATFORM_LINUX_SOCKETCAN_H

#include "cancestry/hal/hal.h"
#include "cancestry/hal/types.h"

#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h> /* for ssize_t */

#ifdef __cplusplus
extern "C" {
#endif

/** Maximum number of interfaces one socketcan backend can manage. */
#define CANCESTRY_PLATFORM_SOCKETCAN_MAX_INTERFACES ((uint8_t)CANCESTRY_HAL_MAX_INTERFACES)

/** Sentinel "not opened" file descriptor. */
#define CANCESTRY_PLATFORM_SOCKETCAN_INVALID_FD (-1)

/**
 * Per-interface SocketCAN runtime state (caller-owned).
 *
 * Declare an array of these inside a cancestry_platform_socketcan_t and pass
 * it as backend_context to cancestry_hal_init().
 */
/**
 * Per-interface SocketCAN runtime state (caller-owned).
 *
 * Declare an array of these inside a cancestry_platform_socketcan_t and pass
 * it as backend_context to cancestry_hal_init().
 */
typedef struct cancestry_platform_socketcan_iface {
    int fd;
    uint32_t interface_index; /* rtnetlink ifindex, set at open */
    /** True when CAN_RAW_FD_FRAMES was negotiated at open (SW-FR-CANFD-004). */
    bool fd_enabled;
    /** True when transmitted CAN FD frames request bit-rate switching. */
    bool fd_brs;
} cancestry_platform_socketcan_iface_t;

/**
 * SocketCAN backend context.
 *
 * All state lives here (caller-owned, zero-alloc).
 */
typedef struct cancestry_platform_socketcan {
    cancestry_platform_socketcan_iface_t ifaces[CANCESTRY_PLATFORM_SOCKETCAN_MAX_INTERFACES];
    uint8_t iface_count;
} cancestry_platform_socketcan_t;

/** Return the singleton SocketCAN backend vtable. */
const cancestry_hal_backend_t *cancestry_platform_socketcan_backend(void);

/**
 * Initialize the SocketCAN context (mark every fd as invalid).
 *
 * Call before cancestry_hal_init().
 */
void cancestry_platform_socketcan_init(cancestry_platform_socketcan_t *ctx);

#ifdef __cplusplus
}
#endif

#endif /* CANCESTRY_PLATFORM_LINUX_SOCKETCAN_H */
