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
 * CAN FD (issue #20, SW-FR-CANFD-004/005):
 *   - An interface configured with can_fd negotiates CAN_RAW_FD_FRAMES at
 *     open time. Negotiation is best-effort: when the interface or driver
 *     does not provide CAN FD the setsockopt fails, the socket stays a
 *     classic CAN socket, and iface->fd_enabled stays false. The HAL then
 *     rejects every CAN FD frame on that interface with a
 *     PROTOCOL_UNSUPPORTED fault instead of truncating it
 *     (deterministic fallback, SW-FR-CANFD-003).
 *   - With CAN_RAW_FD_FRAMES enabled the kernel delivers both frame kinds on
 *     one socket, distinguished by the recvmsg payload size: CAN_MTU
 *     (sizeof(struct can_frame)) for classic frames, CANFD_MTU
 *     (sizeof(struct canfd_frame)) for CAN FD frames. Any other size is
 *     malformed and is reported as CANCESTRY_HAL_FAULT_MALFORMED_FRAME.
 *   - canfd_frame.len is already a payload length in bytes (the kernel maps
 *     DLC codes 9..15 to 12/16/20/24/32/48/64), so no DLC table lives here;
 *     cancestry_can_payload_length_is_valid() is the single normative check.
 *   - The bitrates in cancestry_hal_if_config_t are declarative. Nominal and
 *     data bitrate are properties of the interface, configured out of band
 *     with `ip link set <if> type can bitrate <n> dbitrate <d> fd on`; a raw
 *     socket cannot set them. A non-zero data_bitrate is used only to decide
 *     whether transmitted FD frames request bit-rate switching (CANFD_BRS).
 *
 * Zero allocation proof (constraint 1, SYS-NF-002):
 *   - No malloc/calloc/realloc/free appear in this file.
 *   - The control-message buffer for SO_TIMESTAMPNS is a fixed-size stack
 *     array (CMSG_SPACE(sizeof(struct timespec))) bounded at compile time.
 *   - The RX wire buffer is a fixed-size stack union of struct can_frame and
 *     struct canfd_frame (CANFD_MTU bytes); it is never resized, so a 64-byte
 *     CAN FD payload costs no allocation (SW-FR-CANFD-005).
 *   - setsockopt(CAN_RAW_FD_FRAMES) is a syscall with a caller-owned,
 *     stack-allocated int argument; glibc's setsockopt performs no heap
 *     allocation. The archive is still scanned by ci/check_no_alloc.py, so
 *     the claim is verified on the built object rather than assumed.
 *
 * Implements: SW-FR-HAL-003, SW-FR-HAL-004, SW-FR-HAL-005, SW-FR-HAL-010,
 *             SW-FR-CANFD-003, SW-FR-CANFD-004, SW-FR-CANFD-005
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

/*
 * Compile-time proof of the two assumptions sc_poll/sc_drain_tx make about the
 * kernel UAPI: the wire union is exactly one CAN FD frame wide, and the two
 * layouts are distinguishable by message size (CANFD_MTU > CAN_MTU). C99 has
 * no _Static_assert, so the check is a sized typedef.
 */
typedef char cancestry_socketcan_wire_size_check[
    (sizeof(struct can_frame) == CAN_MTU && sizeof(struct canfd_frame) == CANFD_MTU &&
     CANFD_MTU > CAN_MTU)
        ? 1
        : -1];

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

/** Record @p fault in *out_fault unless a fault was already reported. */
static void sc_set_fault(cancestry_hal_fault_code_t *out_fault, cancestry_hal_fault_code_t fault)
{
    if (out_fault != NULL && *out_fault == CANCESTRY_HAL_FAULT_NONE) {
        *out_fault = fault;
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
    iface->fd_enabled = false;
    iface->fd_brs = false;

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

    /*
     * Negotiate CAN FD (SW-FR-CANFD-004). This is deliberately best-effort:
     * a classic-only interface (vcan without `fd on`, or a controller with no
     * FD mode) makes this setsockopt fail, and we keep the classic socket
     * rather than failing the open. The HAL reads the negotiated result
     * through sc_caps() and rejects CAN FD frames on this interface.
     */
    if (cfg->can_fd != 0u) {
        int fd_on = 1;
        if (setsockopt(fd, SOL_CAN_RAW, CAN_RAW_FD_FRAMES, &fd_on, sizeof(fd_on)) == 0) {
            iface->fd_enabled = true;
            iface->fd_brs = (cfg->data_bitrate != 0u);
        }
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
    iface->fd_enabled = false;
    iface->fd_brs = false;
}

/** Report what was actually negotiated at open time (SW-FR-CANFD-003). */
static bool sc_caps(void *ctx, uint8_t iface_index, cancestry_hal_transport_caps_t *out_caps)
{
    cancestry_platform_socketcan_iface_t *iface = sc_iface(ctx, iface_index);

    if (iface == NULL || out_caps == NULL) {
        return false;
    }
    out_caps->can_fd = iface->fd_enabled;
    out_caps->max_length = iface->fd_enabled ? CANCESTRY_HAL_FRAME_MAX_LENGTH
                                             : CANCESTRY_HAL_CLASSIC_FRAME_MAX_LENGTH;
    return true;
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
        /*
         * One fixed-size buffer for both frame kinds: CANFD_MTU covers a
         * struct can_frame (CAN_MTU) and a struct canfd_frame. Statically
         * sized on the stack, never resized (SYS-NF-002, SW-FR-CANFD-005).
         */
        union {
            struct can_frame classic;
            struct canfd_frame fd;
        } wire;
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
        uint32_t can_id;
        uint8_t length;
        bool is_fd;
        const uint8_t *payload;
        uint8_t i;

        memset(&wire, 0, sizeof(wire));
        memset(&cmsg, 0, sizeof(cmsg));
        iov.iov_base = &wire;
        iov.iov_len = sizeof(wire);
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

        /* Classify by payload size: the kernel uses CAN_MTU for classic
         * frames and CANFD_MTU for CAN FD frames, on both socket kinds. */
        if (n == (ssize_t)CAN_MTU) {
            is_fd = false;
            can_id = wire.classic.can_id;
            length = wire.classic.can_dlc;
            payload = wire.classic.data;
        } else if (n == (ssize_t)CANFD_MTU) {
            is_fd = true;
            can_id = wire.fd.can_id;
            length = wire.fd.len;
            payload = wire.fd.data;
        } else {
            sc_set_fault(out_fault, CANCESTRY_HAL_FAULT_MALFORMED_FRAME);
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

        /* Error frame: translate into bus state fault and skip. Error frames
         * always arrive with the classic layout. */
        if (can_id & CAN_ERR_FLAG) {
            cancestry_hal_fault_code_t f = CANCESTRY_HAL_FAULT_NONE;
            sc_interpret_error_frame(&wire.classic, &f);
            sc_set_fault(out_fault, f);
            continue;
        }

        /*
         * A CAN FD frame on a socket that did not negotiate CAN FD cannot
         * normally happen; if it does, report it as unsupported rather than
         * truncating (SW-FR-CANFD-003). The core HAL applies the same rule
         * for every backend.
         */
        if (is_fd && !iface->fd_enabled) {
            sc_set_fault(out_fault, CANCESTRY_HAL_FAULT_PROTOCOL_UNSUPPORTED);
            continue;
        }

        /* Reject lengths the wire format cannot express (SW-FR-CANFD-002). */
        if (!cancestry_can_payload_length_is_valid(is_fd, (size_t)length)) {
            sc_set_fault(out_fault, CANCESTRY_HAL_FAULT_MALFORMED_FRAME);
            continue;
        }

        /* Convert wire frame -> HAL frame. interface_id is filled in by
         * the core HAL layer; see hal.c (it knows the iface_index -> id
         * mapping). We leave 0 here; the core overwrites it. */
        memset(&hal_frame, 0, sizeof(hal_frame));
        hal_frame.interface_id = 0u; /* overwritten by core HAL */
        hal_frame.can_id = can_id & CAN_EFF_MASK;
        hal_frame.is_extended = (can_id & CAN_EFF_FLAG) ? 1u : 0u;
        hal_frame.is_fd = is_fd ? 1u : 0u;
        hal_frame.length = length;
        for (i = 0u; i < length; ++i) {
            hal_frame.data[i] = payload[i];
        }
        hal_frame.timestamp_us = ts_us;

        push_status = cancestry_hal_rx_ring_push(ring, &hal_frame);
        if (push_status == CANCESTRY_HAL_ERR_RING_FULL) {
            sc_set_fault(out_fault, CANCESTRY_HAL_FAULT_RX_RING_OVERFLOW);
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
        /*
         * Both wire layouts in one statically sized buffer; the iov length
         * selects which one the kernel reads (CAN_MTU vs CANFD_MTU).
         */
        union {
            struct can_frame classic;
            struct canfd_frame fd;
        } wire;
        struct iovec iov;
        struct msghdr msg;
        ssize_t n;
        size_t wire_len;
        uint32_t can_id;
        uint8_t i;
        cancestry_hal_status_t s;

        s = cancestry_hal_tx_ring_pop(ring, &hal_frame);
        if (!cancestry_hal_status_is_ok(s)) {
            break;
        }

        /* Defence in depth: the core HAL already refuses an FD frame on a
         * classic interface, so this only triggers for a backend used
         * without the core. Never truncate (SW-FR-CANFD-003). */
        if (hal_frame.is_fd != 0u && !iface->fd_enabled) {
            ring->dropped++;
            sc_set_fault(out_fault, CANCESTRY_HAL_FAULT_PROTOCOL_UNSUPPORTED);
            continue;
        }
        if (!cancestry_hal_frame_is_valid(&hal_frame)) {
            ring->dropped++;
            sc_set_fault(out_fault, CANCESTRY_HAL_FAULT_MALFORMED_FRAME);
            continue;
        }

        memset(&wire, 0, sizeof(wire));
        can_id = hal_frame.can_id;
        if (hal_frame.is_extended) {
            can_id |= CAN_EFF_FLAG;
        }
        if (hal_frame.is_fd != 0u) {
            wire.fd.can_id = can_id;
            wire.fd.len = hal_frame.length;
            if (iface->fd_brs) {
                wire.fd.flags = CANFD_BRS;
            }
            for (i = 0u; i < hal_frame.length; ++i) {
                wire.fd.data[i] = hal_frame.data[i];
            }
            wire_len = CANFD_MTU;
        } else {
            wire.classic.can_id = can_id;
            wire.classic.can_dlc = hal_frame.length;
            for (i = 0u; i < hal_frame.length; ++i) {
                wire.classic.data[i] = hal_frame.data[i];
            }
            wire_len = CAN_MTU;
        }

        iov.iov_base = &wire;
        iov.iov_len = wire_len;
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
                sc_set_fault(out_fault, CANCESTRY_HAL_FAULT_BUS_BACKPRESSURE);
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
    sc_caps,
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
        ctx->ifaces[i].fd_enabled = false;
        ctx->ifaces[i].fd_brs = false;
    }
}
