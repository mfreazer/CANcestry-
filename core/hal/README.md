# CANcestry HAL (`core/hal`)

The Hardware Abstraction Layer connects the deterministic core
(`core/event`, `core/codec`, `core/recipe`, `core/fsm`) to physical CAN
interfaces. It is part of the runtime path and therefore follows the same
zero-allocation, deterministic, fail-closed rules as the rest of the core
(see `agents.md`, issue #17).

## Layout

```
core/hal/
├── include/cancestry/hal/
│   ├── types.h      # cancestry_hal_frame_t, bounded RX/TX rings, status codes
│   └── hal.h        # hal_init / hal_poll_rx / hal_send_tx / hal_get_status
└── src/
    ├── types.c      # ring helpers and static name tables
    └── hal.c        # platform-independent dispatch (event shaping,
                     # timestamp monotonicity, fault injection)
```

Platform backends live in `platform/`:

- `platform/mock/mock_hal.{c,h}` — deterministic in-process backend used by
  the conformance suite. Supports RX injection, TX capture, and fault
  injection (`cancestry_mock_hal_set_next_poll_fault`, etc.).
- `platform/linux/socketcan.{c,h}` — Linux SocketCAN backend using
  non-blocking `recvmsg`/`sendmsg` with `SO_TIMESTAMPNS` hardware
  timestamps.

## Contract summary

| Property | Enforcement |
|---|---|
| Zero heap allocation (SYS-NF-002) | All buffers are caller-owned; `ci/check_no_alloc.py` scans `libcancestry_hal.a` and both backends; `-Wall -Wextra -Werror -Wpedantic` builds. |
| Non-blocking (SW-FR-HAL-003) | Backends use non-blocking I/O only; the conformance suite (`HAL-NONBLOCK-001`) calls poll 10 000 times with no injected frames and asserts zero faults/frames. |
| Strictly monotonic timestamps (SW-FR-HAL-004) | `hal_deliver_rx` rejects frames whose timestamp does not advance and raises `TIMESTAMP_NON_MONOTONIC` (`HAL-TIMESTAMP-MONO-001`). |
| Fail-closed I/O (SYS-SF-002, SW-FR-HAL-005) | Every syscall failure, bus error, and ring overflow is raised as a `FAULT_RAISED` event with `CANCESTRY_PRIORITY_CLASS_FAULT`, so FSMs react before data events dequeue (`HAL-BUS-ERROR-001`). |
| Bounded rings (SW-FR-HAL-006) | Rings never overwrite unread data; overflow increments a counter and raises a fault (`HAL-RING-BOUNDS-001`). |
| Schema is law (SW-FR-HAL-012) | HAL configuration validates against `schemas/hal-0.1.0.schema.json` before any socket is opened. |

## Requirement trace

| Requirement | Verified by |
|---|---|
| SW-FR-HAL-001, SW-FR-HAL-002 | `core/hal/include/cancestry/hal/types.h` (inspection, `HAL-TYPES-001`) |
| SW-FR-HAL-003 | `HAL-NONBLOCK-001` |
| SW-FR-HAL-004 | `HAL-TIMESTAMP-MONO-001` |
| SW-FR-HAL-005, SW-FR-HAL-007 | `HAL-BUS-ERROR-001` |
| SW-FR-HAL-006, SW-FR-HAL-011 | `HAL-RING-BOUNDS-001`, `HAL-RING-BOUNDS-002` |
| SW-FR-HAL-008, SW-FR-HAL-009 | `platform/mock/mock_hal.c` (used by all conformance tests) |
| SW-FR-HAL-010 | `platform/linux/socketcan.c` (inspection; live vcan test deferred per `docs/trace/traceability.md` ledger — CI sandbox lacks vcan) |
| SW-FR-HAL-012 | `schemas/hal-0.1.0.schema.json` (schema check) |

## Using the HAL

```c
cancestry_hal_t hal;
cancestry_platform_socketcan_t sc;
cancestry_hal_frame_t rx_storage[64], tx_storage[64];
cancestry_hal_rx_ring_t rx; cancestry_hal_tx_ring_t tx;
cancestry_hal_if_config_t iface = { .interface_id = 1, .bitrate = 500000 };
memcpy(iface.name, "vcan0", 5);
cancestry_hal_rx_ring_init(&rx, rx_storage, 64);
cancestry_hal_tx_ring_init(&tx, tx_storage, 64);
cancestry_platform_socketcan_init(&sc);
cancestry_hal_config_t cfg = {
    .backend = cancestry_platform_socketcan_backend(),
    .backend_context = &sc,
    .clock = &clock,
    .ifaces = &iface, .iface_count = 1,
    .rx_rings = (cancestry_hal_rx_ring_t*[]){ &rx },
    .tx_rings = (cancestry_hal_tx_ring_t*[]){ &tx },
};
cancestry_hal_init(&hal, &cfg);

while (running) {
    (void)cancestry_hal_poll_rx(&hal, &event_queue, NULL, NULL);
    drain_event_queue_into_engines();
    (void)cancestry_fsm_engine_tick(&fsm);
}
```
