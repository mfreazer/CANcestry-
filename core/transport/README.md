# core/transport - ISO-TP (ISO 15765-2) over classic CAN

Phase 10 ([issue #26](https://github.com/mfreazer/CANcestry-/issues/26)).
`cancestry_transport` reassembles and segments multi-frame diagnostic
messages over classic CAN (8-byte) frames. It is the transport half of the
diagnostic stack; `core/uds` is the service half on top of it.

The engine is **zero-allocation** (caller-owned static buffers, verified by
the `cancestry_transport_no_malloc_symbols` archive gate), **deterministic**
(time advances only through `cancestry_transport_tick()`, one call = one 1 ms
tick of the `core/event` clock abstraction - no `sleep`, no blocking wait, no
wall-clock read) and **fail-closed** (any protocol violation drops the
session, clears the buffer and raises a fault; the engine never crashes,
hangs, truncates or allocates).

## Layout

```
core/transport/
  include/cancestry/transport/types.h    statuses, states, faults, config, counters
  include/cancestry/transport/engine.h   the engine struct and its API
  src/types.c                            name tables, STmin codec, status helpers
  src/engine.c                           the state machine
```

Library: `cancestry_transport` (links `cancestry_event` only).

## Frame contract (classic CAN, 8 bytes)

| PCI nibble | Frame | Layout | Notes |
|---|---|---|---|
| `0x0` | Single Frame (SF) | `0x0L` + L bytes, L = 1..7 | L = 0 is invalid; a frame longer than L + 1 is a protocol violation |
| `0x1` | First Frame (FF) | `0x1H`, `L`, 6 payload bytes | total length = `(H << 8) \| L`, 8..4095; the frame itself is always 8 bytes |
| `0x2` | Consecutive Frame (CF) | `0x2S` + up to 7 payload bytes | S = rolling sequence number, starts at 1, wraps 0xF -> 0x0 |
| `0x3` | Flow Control (FC) | `0x3F`, BS, STmin | F: 0 = CTS, 1 = Wait, 2 = OVFLW; length must be exactly 3 |

PCI nibbles `0x4..0xF` are invalid and raise `TRANSPORT_PROTOCOL_FAULT`.
A frame type that cannot apply in the current state (an SF or a new FF while
reassembly is in progress, a CF with no session, an FC with no sender
waiting) is likewise a protocol violation, never a crash (SW-FR-TP-005).

## API

```c
#include "cancestry/transport/engine.h"

cancestry_transport_t tp;
cancestry_tp_config_t config = { 0 };          /* NULL config = the same defaults */
uint8_t rx_buf[128], tx_buf[256];
cancestry_tp_sink_t sink = { .user_data = &platform,
                             .on_frame_tx = platform_send,   /* may decline */
                             .on_message  = handle_message };

cancestry_transport_init(&tp, &config, rx_buf, sizeof(rx_buf),
                         tx_buf, sizeof(tx_buf), &sink, &queue, &clock);

cancestry_transport_rx_frame(&tp, frame, len); /* platform -> engine (RX) */
cancestry_transport_send(&tp, data, len);      /* engine -> bus (TX, len <= 4095) */
cancestry_transport_tick(&tp);                 /* one 1 ms tick */
```

- `cancestry_transport_rx_frame()` returns `CANCESTRY_TP_OK_MESSAGE` exactly
  when a message completed; the payload is delivered through
  `sink.on_message` as a **borrowed pointer** valid only during the callback.
- `cancestry_transport_send()` emits an SF synchronously when
  `len <= 7`; for larger payloads it emits the FF synchronously and the CFs on
  later ticks, paced by the receiver's Flow Control. It fails closed with
  `CANCESTRY_TP_ERR_STATE` while a transmission is in flight, and with
  `CANCESTRY_TP_ERR_CAPACITY` above 4095 bytes or the caller's TX buffer.
- `sink.on_frame_tx` returning `false` **declines** the frame: it is retried
  on every subsequent tick until N_As expires; a frame is never silently
  dropped (SW-FR-TP-009).
- `cancestry_transport_advance(&tp, n)` runs `n` ticks and stops early on
  the first negative status (the test convenience wrapper).

## Tick order (the deterministic contract)

One `cancestry_transport_tick()` call, in order:

1. **RX N_Cr** - a reassembly session whose next CF did not arrive within
   `n_cr_ms` aborts with `TRANSPORT_TIMEOUT`.
2. **RX pending FC retry** - a Flow Control frame the sink declined is
   retransmitted (bounded by N_As).
3. **TX N_Bs** - a sender whose FF (or last CF of a block) saw no Flow
   Control within `n_bs_ms` aborts with `TRANSPORT_TIMEOUT`.
4. **TX pending retry** - the sink-declined FF/CF, retried (bounded by N_As).
5. **TX CF burst** - as many CFs as Block Size and STmin allow in this tick.

`tick()` returns `CANCESTRY_TP_ERR_TIMEOUT` exactly when a timer expired
(this is the only negative status it can return), so a supervisor can log
timeouts without polling internal state. Status codes:
`OK = 0`, `OK_MESSAGE = 1`, `ERR_NULL = -1`, `ERR_ARGUMENT = -2`,
`ERR_STATE = -3`, `ERR_CAPACITY = -4`, `ERR_PROTOCOL = -5`,
`ERR_OVERFLOW = -6`, `ERR_TIMEOUT = -7`.

## Timers (SW-FR-TP-006)

| Timer | Bounds | Default | On expiry |
|---|---|---|---|
| N_Cr | RX: FF/CF to next CF | 1000 ms | drop session, clear buffer, `TRANSPORT_TIMEOUT` |
| N_Bs | TX: FF/CF-of-block to FC | 1000 ms | abort transmission, `TRANSPORT_TIMEOUT` |
| N_As | TX: a sink-declined frame to its eventual acceptance | 1000 ms | abort transmission, `TRANSPORT_TIMEOUT` |

All three are plain ms fields in `cancestry_tp_config_t` (0 = default) and
are evaluated at 1 ms tick granularity against the injectable clock - the
same `core/event` clock semantics as Phase 4, no separate timer mechanism.

## Flow Control (SW-FR-TP-008)

When the engine receives an FF it answers with FC CTS carrying its own
`fc_block_size`/`fc_stmin` (the BS/STmin it wants the *peer* to use).
As a sender it honours the receiver's FC:

- **CTS** with BS = 0: unlimited CF burst, paced only by STmin.
- **CTS with BS > 0**: exactly BS CFs, then wait for the next FC.
- **STmin**: `0x00..0x7F` = 0..127 ms; `0xF1..0xF9` = 100..900 us
  (evaluated at tick granularity, so sub-ms values mean "next tick");
  `0x80..0xF0` are reserved and treated as 0 (no separation) - the
  `cancestry_tp_stmin_decode_us()`/`_encode_ms()` helpers are the single
  source of that mapping.
- **Wait (FS = 1)**: tolerated up to `max_wait_frames` (default 8)
  consecutive WT frames; one more aborts with `TRANSPORT_PROTOCOL_FAULT`.
- **OVFLW (FS = 2)**: the receiver announced a buffer overflow - the
  transmission aborts with `CANCESTRY_TP_ERR_OVERFLOW`.

## Fail-closed behaviour

Every abort path: clear the session buffers, reset the state to IDLE, raise
the fault into the configured `core/event` queue as a `FAULT_RAISED` event in
the FAULT priority class, and judge the next frame as a fresh start
(SW-FR-TP-004/005). Nothing is half-delivered: a message is handed to
`on_message` only when it is complete and exactly as long as the FF
announced.

| Fault | Raised when | Severity |
|---|---|---|
| `TRANSPORT_PROTOCOL_FAULT` | invalid PCI, length contradicting the PCI, wrong state, CF sequence-number skip, FC Wait overflow | WARNING |
| `TRANSPORT_BUFFER_OVERFLOW` | FF total length above the static RX buffer (answered with FC OVFLW before any payload byte is stored), FC OVFLW as a sender | WARNING |
| `TRANSPORT_TIMEOUT` | N_Cr, N_Bs or N_As expiry | WARNING |

The numeric event code is `((source_id & 0xFFFF) << 16) | fault`, the same
shape as `core/hal`, and resolves statically through
`cancestry_tp_fault_name()` - no string lookup in the fault path
(SW-FR-TP-010).

## Example

```c
/* A 20-byte diagnostic response, segmented. */
static const uint8_t response[20] = { 0x62, 0xF1, 0x90, /* ... */ };

cancestry_transport_send(&tp, response, sizeof(response));
/* tick 0: FF  10 14 62 F1 90 ...  (6 payload bytes)          */
/* peer:  FC  30 00 00            (CTS, BS 0, STmin 0)        */
/* feed the FC with rx_frame(); then:                          */
/* tick 1: CF  21 .. (7 bytes), CF 22 .. (7 bytes) -> complete */
```

## Requirement trace

`SW-FR-TP-001..010` (`docs/software/SwRS.md` section 14.1), traced to
`TP-RX-001..008` / `TP-TX-001..009`
(`tests/conformance/transport/`) and the no-alloc gate in
`docs/trace/traceability.csv`.

## Build and test

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug   # ASan/UBSan on
cmake --build build --parallel
ctest --test-dir build -R 'tp_iso_tp' --output-on-failure
```

## Deliberately out of scope

- **CAN FD ISO-TP** (up to 62-byte SF payloads): the engine is classic CAN
  only, like the rest of the diagnostic stack at this phase.
- **Addressing modes** (normal fixed, extended, mixed): the engine sees
  already-demultiplexed diagnostic frames; addressing belongs to the HAL
  integration.
- **Concurrent sessions**: one RX and one TX session per engine instance,
  which is the diagnostic gateway's actual topology.
