/*
 * CANcestry - Linux SocketCAN backend.
 *
 * Feature test macros: struct timespec, struct ifreq, and SOCK_CLOEXEC are
 * POSIX.1-2008 / glibc extensions; expose them when building under -std=c99.
 */
#if !defined(_POSIX_C_SOURCE) || _POSIX_C_SOURCE < 200809L
#undef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE 1
#endif

/*
 * I/O model (constraints 2 and 3, issue #17):
 *   - Every socket is opened SOCK_NONBLOCK; recvmsg/sendmsg never block.
 *   - SO_TIMESTAMPNS is enabled so the kernel supplies the nanosecond
 *     timestamp of frame reception; we convert to microseconds for
 *     core/event using pure integer arithmetic (no floats).
 *   - Error frames (CAN_ERR_MASK) are enabled via CAN_RAW_ERR_FILTER so bus
 *     Error Passive and Bus Off conditions surface as HAL faults
 *     (SW-FR-HAL-007, fail-closed).
 *
 * Zero allocation proof (constraint 1, SYS-NF-002):
 *   - No malloc/calloc/realloc/free appear in this file.
 *   - The control-message buffer for SO_TIMESTAMPNS is a fixed-size stack
 *     array (CMSG_SPACE(sizeof(struct timespec))) bounded at compile time.
 *   - ci/check_no_alloc.py extends to libcancestry_platform_linux.a to
 *     enforce this at build time.
 *
 * Implements: SW-FR-HAL-003, SW-FR-HAL-004, SW-FR-HAL-005, SW-FR-HAL-010
 */

#include "socketcan.h"

#include <errno.h>
#include <string.h>
#include <time.h>

#include <linux/can.h>
#include <linux/can/error.h>
#include <linux/can/raw.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#ifndef IFNAMSIZ
#define IFNAMSIZ 16
#endif

/* ------------------------------------------------------------------------- */
/* Helpers                                                                   */
/* ------------------------------------------------------------------------- */

static cancestry_platform_socketcan_iface_t *sc_iface(void *ctx, uint8_t idx)
{
    cancestry_platform_socketcan_t *sc = (cancestry_platform_socketcan_t *)ctx;
    if (sc == NULL || idx >= CANCESTRY_PLATFORM_SOCKETCAN_MAX_INTERFACES) {
        return NULL;
    }
    return &sc->ifaces[idx];
}

/**
 * Convert a kernel timespec to monotonic microseconds.
 *
 * Pure integer arithmetic - no floating point. Nanoseconds are floored to
 * microseconds so the value never rounds ahead of true time.
 */
static cancestry_time_us_t timespec_to_us(const struct timespec *ts)
{
    return (cancestry_time_us_t)ts->tv_sec * 1000000u +
           (cancestry_time_us_t)((uint32_t)ts->tv_nsec / 1000u);
}

/**
 * Interpret a CAN error frame and set *out_fault to the corresponding HAL
 * fault (when the frame indicates a state transition the FSM must react to).
 */
static void sc_interpret_error_frame(const struct can_frame *wire,
                                      cancestry_hal_fault_code_t *out_fault)
{
    if (out_fault == NULL || wire == NULL) {
        return;
    }
    if (wire->can_id & CAN_ERR_BUSOFF) {
        *out_fault = CANCESTRY_HAL_FAULT_BUS_OFF;
        return;
    }
    if (wire->can_id & CAN_ERR_CRTL) {
        uint8_t ctrl = wire->data[1];
        if (ctrl & (CAN_ERR_CRTL_RX_PASSIVE | CAN_ERR_CRTL_TX_PASSIVE)) {
            *out_fault = CANCESTRY_HAL_FAULT_BUS_ERROR_PASSIVE;
            return;
        }
    }
}

/* ------------------------------------------------------------------------- */
/* Backend callbacks                                                         */
/* ------------------------------------------------------------------------- */

static bool sc_open(void *ctx, const cancestry_hal_if_config_t *cfg, uint8_t iface_index)
{
    cancestry_platform_socketcan_t *sc = (cancestry_platform_socketcan_t *)ctx;
    cancestry_platform_socketcan_iface_t *iface;
    struct ifreq ifr;
    int fd;
    int on;
    struct sockaddr_can addr;

    if (sc == NULL || cfg == NULL) {
        return false;
    }
    if (iface_index >= CANCESTRY_PLATFORM_SOCKETCAN_MAX_INTERFACES) {
        return false;
    }
    iface = &sc->ifaces[iface_index];
    iface->fd = CANCESTRY_PLATFORM_SOCKETCAN_INVALID_FD;

    fd = socket(PF_CAN, SOCK_RAW | SOCK_NONBLOCK | SOCK_CLOEXEC, CAN_RAW);
    if (fd < 0) {
        return false;
    }

    /* Resolve interface name -> ifindex via ioctl(SIOCGIFINDEX). */
    memset(&ifr, 0, sizeof(ifr));
    {
        size_t i = 0u;
        while (i < IFNAMSIZ - 1u && cfg->name[i] != '\0') {
            ifr.ifr_name[i] = cfg->name[i];
            ++i;
        }
        ifr.ifr_name[i] = '\0';
    }
    if (ioctl(fd, SIOCGIFINDEX, &ifr) < 0) {
        close(fd);
        return false;
    }
    iface->interface_index = (uint32_t)ifr.ifr_ifindex;

    /* Enable kernel timestamps on every received message. */
    on = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_TIMESTAMPNS, &on, sizeof(on)) < 0) {
        close(fd);
        return false;
    }

    /* Subscribe to error frames so we can detect BUS_OFF / ERROR_PASSIVE. */
    {
        can_err_mask_t err_mask = CAN_ERR_LOSTARB | CAN_ERR_CRTL | CAN_ERR_BUSOFF |
                                  CAN_ERR_RESTARTED;
        (void)setsockopt(fd, SOL_CAN_RAW, CAN_RAW_ERR_FILTER, &err_mask, sizeof(err_mask));
    }

    /* Bind to the resolved interface. */
    memset(&addr, 0, sizeof(addr));
    addr.can_family = AF_CAN;
    addr.can_ifindex = (int)iface->interface_index;
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return false;
    }

    iface->fd = fd;
    if (iface_index >= sc->iface_count) {
        sc->iface_count = (uint8_t)(iface_index + 1u);
    }
    return true;
}

static void sc_close(void *ctx, uint8_t iface_index)
{
    cancestry_platform_socketcan_iface_t *iface = sc_iface(ctx, iface_index);

    if (iface == NULL) {
        return;
    }
    if (iface->fd != CANCESTRY_PLATFORM_SOCKETCAN_INVALID_FD) {
        close(iface->fd);
        iface->fd = CANCESTRY_PLATFORM_SOCKETCAN_INVALID_FD;
    }
}

static cancestry_hal_status_t sc_poll(void *ctx,
                                       uint8_t iface_index,
                                       cancestry_hal_rx_ring_t *ring,
                                       cancestry_hal_fault_code_t *out_fault)
{
    cancestry_platform_socketcan_iface_t *iface = sc_iface(ctx, iface_index);

    if (iface == NULL || ring == NULL) {
        return CANCESTRY_HAL_ERR_NULL;
    }
    if (iface->fd == CANCESTRY_PLATFORM_SOCKETCAN_INVALID_FD) {
        if (out_fault != NULL) {
            *out_fault = CANCESTRY_HAL_FAULT_INTERFACE_DOWN;
        }
        return CANCESTRY_HAL_ERR_IO;
    }

    /* Drain as many non-blocking frames as are immediately available. */
    for (;;) {
        struct can_frame frame;
        struct iovec iov;
        struct msghdr msg;
        union {
            struct cmsghdr align;
            char buf[CMSG_SPACE(sizeof(struct timespec))];
        } cmsg;
        struct cmsghdr *cmsgp;
        ssize_t n;
        cancestry_time_us_t ts_us = 0u;
        cancestry_hal_frame_t hal_frame;
        cancestry_hal_status_t push_status;
        uint8_t i;

        memset(&frame, 0, sizeof(frame));
        memset(&cmsg, 0, sizeof(cmsg));
        iov.iov_base = &frame;
        iov.iov_len = sizeof(frame);
        memset(&msg, 0, sizeof(msg));
        msg.msg_iov = &iov;
        msg.msg_iovlen = 1;
        msg.msg_control = cmsg.buf;
        msg.msg_controllen = sizeof(cmsg.buf);

        n = recvmsg(iface->fd, &msg, MSG_DONTWAIT);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break; /* nothing more to read right now */
            }
            if (errno == EBADF || errno == EPIPE || errno == ECONNRESET) {
                if (out_fault != NULL) {
                    *out_fault = CANCESTRY_HAL_FAULT_INTERFACE_DOWN;
                }
                return CANCESTRY_HAL_ERR_IO;
            }
            break;
        }
        if ((size_t)n < sizeof(struct can_frame)) {
            if (out_fault != NULL && *out_fault == CANCESTRY_HAL_FAULT_NONE) {
                *out_fault = CANCESTRY_HAL_FAULT_MALFORMED_FRAME;
            }
            continue;
        }

        /* Extract kernel/hardware timestamp if present. */
        for (cmsgp = CMSG_FIRSTHDR(&msg); cmsgp != NULL;
             cmsgp = CMSG_NXTHDR(&msg, cmsgp)) {
            if (cmsgp->cmsg_level == SOL_SOCKET &&
                cmsgp->cmsg_type == SO_TIMESTAMPNS &&
                cmsgp->cmsg_len >= CMSG_LEN(sizeof(struct timespec))) {
                struct timespec ts;
                memcpy(&ts, CMSG_DATA(cmsgp), sizeof(ts));
                ts_us = timespec_to_us(&ts);
            }
        }

        /* Error frame: translate into bus state fault and skip. */
        if (frame.can_id & CAN_ERR_FLAG) {
            cancestry_hal_fault_code_t f = CANCESTRY_HAL_FAULT_NONE;
            sc_interpret_error_frame(&frame, &f);
            if (f != CANCESTRY_HAL_FAULT_NONE && out_fault != NULL &&
                *out_fault == CANCESTRY_HAL_FAULT_NONE) {
                *out_fault = f;
            }
            continue;
        }

        /* Convert wire frame -> HAL frame. interface_id is filled in by
         * the core HAL layer; see hal.c (it knows the iface_index -> id
         * mapping). We leave 0 here; the core overwrites it. */
        memset(&hal_frame, 0, sizeof(hal_frame));
        hal_frame.interface_id = 0u; /* overwritten by core HAL */
        hal_frame.can_id = frame.can_id & CAN_EFF_MASK;
        hal_frame.is_extended = (frame.can_id & CAN_EFF_FLAG) ? 1u : 0u;
        hal_frame.length = (frame.can_dlc > CANCESTRY_HAL_FRAME_MAX_LENGTH)
                               ? CANCESTRY_HAL_FRAME_MAX_LENGTH
                               : frame.can_dlc;
        for (i = 0u; i < hal_frame.length; ++i) {
            hal_frame.data[i] = frame.data[i];
        }
        hal_frame.timestamp_us = ts_us;

        push_status = cancestry_hal_rx_ring_push(ring, &hal_frame);
        if (push_status == CANCESTRY_HAL_ERR_RING_FULL) {
            if (out_fault != NULL && *out_fault == CANCESTRY_HAL_FAULT_NONE) {
                *out_fault = CANCESTRY_HAL_FAULT_RX_RING_OVERFLOW;
            }
            break;
        }
    }
    return CANCESTRY_HAL_OK;
}

static cancestry_hal_status_t sc_drain_tx(void *ctx,
                                            uint8_t iface_index,
                                            cancestry_hal_tx_ring_t *ring,
                                            cancestry_hal_fault_code_t *out_fault)
{
    cancestry_platform_socketcan_iface_t *iface = sc_iface(ctx, iface_index);

    if (iface == NULL || ring == NULL) {
        return CANCESTRY_HAL_ERR_NULL;
    }
    if (iface->fd == CANCESTRY_PLATFORM_SOCKETCAN_INVALID_FD) {
        if (out_fault != NULL) {
            *out_fault = CANCESTRY_HAL_FAULT_INTERFACE_DOWN;
        }
        return CANCESTRY_HAL_ERR_IO;
    }

    while (cancestry_hal_tx_ring_count(ring) > 0u) {
        cancestry_hal_frame_t hal_frame;
        struct can_frame wire;
        struct iovec iov;
        struct msghdr msg;
        ssize_t n;
        uint8_t i;
        cancestry_hal_status_t s;

        s = cancestry_hal_tx_ring_pop(ring, &hal_frame);
        if (!cancestry_hal_status_is_ok(s)) {
            break;
        }

        memset(&wire, 0, sizeof(wire));
        wire.can_id = hal_frame.can_id;
        if (hal_frame.is_extended) {
            wire.can_id |= CAN_EFF_FLAG;
        }
        wire.can_dlc = hal_frame.length;
        for (i = 0u; i < hal_frame.length && i < CANCESTRY_HAL_FRAME_MAX_LENGTH; ++i) {
            wire.data[i] = hal_frame.data[i];
        }

        iov.iov_base = &wire;
        iov.iov_len = sizeof(wire);
        memset(&msg, 0, sizeof(msg));
        msg.msg_iov = &iov;
        msg.msg_iovlen = 1;

        n = sendmsg(iface->fd, &msg, MSG_DONTWAIT | MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == ENOBUFS) {
                /* Kernel back-pressure: push the frame back at the ring's
                 * read position (it is now at (read_idx - 1) mod cap since
                 * pop advanced read_idx) and stop draining. */
                uint16_t prev = (ring->read_idx == 0u)
                                    ? (uint16_t)(ring->capacity - 1u)
                                    : (uint16_t)(ring->read_idx - 1u);
                ring->slots[prev] = hal_frame;
                ring->count++;
                if (out_fault != NULL && *out_fault == CANCESTRY_HAL_FAULT_NONE) {
                    *out_fault = CANCESTRY_HAL_FAULT_BUS_BACKPRESSURE;
                }
                break;
            }
            if (errno == EBADF || errno == EPIPE || errno == ECONNRESET || errno == ENETDOWN) {
                ring->dropped++;
                if (out_fault != NULL) {
                    *out_fault = CANCESTRY_HAL_FAULT_INTERFACE_DOWN;
                }
                return CANCESTRY_HAL_ERR_IO;
            }
            /* Other transient errors: drop the single frame and continue. */
            ring->dropped++;
            continue;
        }
        ring->sent++;
    }
    return CANCESTRY_HAL_OK;
}

/* ------------------------------------------------------------------------- */
/* Public API                                                                */
/* ------------------------------------------------------------------------- */

static const cancestry_hal_backend_t sc_backend = {
    sc_open,
    sc_close,
    sc_poll,
    sc_drain_tx,
};

const cancestry_hal_backend_t *cancestry_platform_socketcan_backend(void)
{
    return &sc_backend;
}

void cancestry_platform_socketcan_init(cancestry_platform_socketcan_t *ctx)
{
    uint8_t i;

    if (ctx == NULL) {
        return;
    }
    memset(ctx, 0, sizeof(*ctx));
    for (i = 0u; i < CANCESTRY_PLATFORM_SOCKETCAN_MAX_INTERFACES; ++i) {
        ctx->ifaces[i].fd = CANCESTRY_PLATFORM_SOCKETCAN_INVALID_FD;
    }
}
