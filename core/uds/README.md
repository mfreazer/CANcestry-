# core/uds - UDS server (ISO 14229-1 subset)

Phase 10 ([issue #26](https://github.com/mfreazer/CANcestry-/issues/26)).
`cancestry_uds` is the service half of the diagnostic stack: it consumes a
reassembled diagnostic request (a byte buffer in) and produces the response
bytes for segmentation. The transport half is `core/transport`.

Implemented services: **ReadDataByIdentifier (0x22)**,
**WriteDataByIdentifier (0x2E)** and **RoutineControl (0x31)**. The runtime
path is **zero-allocation** (verified by `cancestry_uds_no_malloc_symbols`;
the YAML loader's heap use is load-time only and lives in the separate
`cancestry_uds_loader` library) and every write or routine side effect
passes through the **fail-closed governor stub** before anything happens
(SW-FR-UDS-007).

## Layout

```
core/uds/
  include/cancestry/uds/types.h      statuses, NRC names, config/DID/routine types
  include/cancestry/uds/services.h   server API + governor contract
  include/cancestry/uds/loader.h     cancestry_uds_config_load / _free
  src/types.c                        name tables
  src/services.c                     the server
  src/loader.c                       YAML-subset loader (own library)
```

Libraries: `cancestry_uds` (links `cancestry_event`, `cancestry_codec`,
`cancestry_recipe`) and `cancestry_uds_loader`.

## Server model

```c
#include "cancestry/uds/services.h"

cancestry_uds_server_t server;
cancestry_uds_server_config_t cfg = {
    .config = loaded,                    /* cancestry_uds_config_load() */
    .did_data = did_data,                /* caller-owned, did_data_length bytes */
    .did_signal_ids = ids,               /* caller-owned, did_count slots */
    .namespace = &ns,                    /* codec namespace (signal DIDs) */
    .signals = &store,                   /* shared signal store, optional */
    .governor = my_governor,             /* NULL = deny everything */
    .governor_user_data = &policy,
};
cancestry_uds_server_init(&server, &cfg);   /* fail-closed: unresolved signal -> false */

cancestry_uds_server_process(&server, request, request_length,
                             response, response_capacity, &response_length);
```

`process()` is pure request/response: it copies the response into the
caller's buffer and never retains the request. A zero-length request is
answered `7F 00 11` (SID 0, serviceNotSupported). The return status mirrors
the response: `CANCESTRY_UDS_OK` for a positive response, a specific
`CANCESTRY_UDS_ERR_*` for each negative response code, and
`CANCESTRY_UDS_ERR_CAPACITY` when the caller's response buffer is too small
(the response is then *not* written and `response_length` is untouched).

Wiring it to the transport is a five-line callback: on a reassembled
message, `process()` it and `cancestry_transport_send()` the response (see
`UDS-GOV-005` in `tests/conformance/uds/`).

## Services and negative response codes

| Service | Positive response | NRCs |
|---|---|---|
| 0x22 ReadDataByIdentifier | `62 DID data...` (exactly one DID per request at this level) | `0x13` malformed (length != 3, DID count != 1), `0x31` unknown DID, `0x22` mapped signal value not encodable in the DID length |
| 0x2E WriteDataByIdentifier | `6E DID` | `0x13` wrong data length, `0x31` unknown DID, `0x22` governor denial, `0x72` mapped signal store full (no partial effect) |
| 0x31 RoutineControl | `71 sub DID record...` | `0x13` request length != 4, `0x12` unknown routine / undeclared or disallowed sub-function, `0x22` governor denial |
| other / zero-length | - | `7F SID 11` (serviceNotSupported) |

A denied or failed write leaves **no partial effect**: the DID store is
untouched, the signal store is untouched, and the
`governor_denials` counter increments (SW-FR-UDS-007, mirroring
`SW-FR-GOV-005/006`).

## Governor contract

```c
typedef struct cancestry_uds_governor_request {
    cancestry_uds_governor_request_kind_t kind;   /* WRITE_DID or RUN_ROUTINE */
    const cancestry_uds_did_t *did_entry;         /* NULL for routines */
    uint16_t did, routine_id;
    const cancestry_uds_routine_t *routine_entry; /* NULL for writes */
    uint8_t subfunction;                          /* RoutineControl sub 0x01..0x03 */
    bool has_signal;                              /* DID maps a signal? */
    const char *signal_name; cancestry_signal_id_t signal_id;
    cancestry_value_t signal_value;               /* little-endian decoded payload */
    const uint8_t *data; size_t data_length;      /* raw write payload */
} cancestry_uds_governor_request_t;

bool my_governor(void *user_data, const cancestry_uds_governor_request_t *req);
```

- **NULL governor denies everything** - fail-closed by construction.
- The request carries the target's metadata (`did_entry->read_only`, the
  signal name/id/value), so a policy can implement the FSM capability
  semantics (a signal-write allowlist) exactly as `core/fsm` does.
- The governor runs **before** any store write; a `false` return produces
  NRC `0x22` and nothing else.

## DID/signal mirroring (SW-FR-UDS-008)

A DID may map a signal by canonical name (`diag.RpmTarget`). At
`server_init` every mapping is resolved through the codec namespace and the
resolved id stored in the caller's `did_signal_ids` array; a name that does
not resolve fails init (fail-closed, proven by `UDS-SVC-007`).

- **Read**: the current signal value from the shared store is encoded
  little-endian into the response when one exists; otherwise the DID's
  stored bytes are served (a REAL-typed or absent value serves the stored
  bytes; an unencodable value is NRC `0x22`).
- **Write** (after approval): the bytes are stored *and* mirrored into the
  shared store as a little-endian unsigned integer; a full store reports
  NRC `0x72` with no partial effect.

## Configuration and the loader (SW-FR-UDS-006)

`schemas/uds-0.1.0.schema.json` is law: `cancestry_uds_config_load()`
validates the YAML document against it (same parser family as the FSM/codec
loaders) and refuses unknown fields, duplicate DIDs or routine ids, byte
values outside 0..255, lengths over 8 for signal-mapped DIDs, and any
schema-version other than `0.1.0` - each with a 1-based line/column error.

```yaml
schema_version: "0.1.0"
uds:
  dids:
    - did: 0xF190          # VIN
      length: 17
      read_only: true
      default: [0x57, 0x41, 0x53, ...]
    - did: 0x0204
      length: 2
      signal: diag.RpmTarget
      default: [0x00, 0x00]
  routines:
    - routine: 0x0203
      start:   { response: [0x00] }
      stop:    { allowed: true }
      results: { response: [0x01, 0x02] }
```

`did_data_length` is the sum of all DID lengths; `dids[i].data_offset`
addresses each DID's slice in the caller's store. The config is released
with `cancestry_uds_config_free()`; the server borrows it and never frees
anything.

## Example

```c
/* 22 F1 90            -> 62 F1 90 + 17 VIN bytes                     */
/* 2E 12 34 DE AD BE EF -> 6E 12 34            (governor permitting)  */
/* 31 01 02 03          -> 71 01 02 03 00     (routine 0x0203 start)  */
/* 10 03                -> 7F 10 11            (not implemented)      */
```

## Requirement trace

`SW-FR-UDS-001..008` (`docs/software/SwRS.md` section 14.2), traced to
`UDS-SVC-001..007` / `UDS-GOV-001..005` (`tests/conformance/uds/`) and the
no-alloc gate in `docs/trace/traceability.csv`.

## Build and test

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug   # ASan/UBSan on
cmake --build build --parallel
ctest --test-dir build -R 'uds_' --output-on-failure
```

## Deliberately out of scope

- **Session/security management** (0x10, 0x27): no session state and no
  security access at this phase; the governor is the single policy gate.
- **0x7F NRC timing subtleties** beyond the codes above (no 0x21, 0x33,
  0x78 pending): responses are immediate and deterministic.
- **Multi-DID RDBI and DID ranges**: exactly one DID per request, per
  SW-FR-UDS-002.
- **Routine execution bodies**: RoutineControl returns the configured
  response records; it does not execute arbitrary logic (the governor would
  still gate it).
