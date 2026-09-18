# Frama-C/WP contracts

The production annotations for the Phase 9 proof boundary live next to the
implementation, inside `/*@ ... */` comments:

| Function | Contract goal |
|---|---|
| `cancestry_event_queue_push` | Caller-owned storage, bounded size/fault depth, and no write outside the queue buffer. |
| `cancestry_event_queue_pop` | Empty-queue handling and bounded heap mutation; an optional output pointer is checked before it is written. |
| `cancestry_codec_encode_signal` | A legal classic/CAN FD payload and signal bit span are required before any shift or write. |
| `cancestry_codec_decode_signal` / `cancestry_codec_decode_frame` | Frame bounds, signal capacity, and atomic short-frame behavior. |
| `codec_bits_*` | Shift counts and payload-bit accesses are bounded for contiguous and Motorola sawtooth layouts. |

The annotations are non-intrusive: they contain no preprocessor definitions,
expressions seen by the C compiler, or executable statements. The production
CMake targets are therefore unchanged by the proof layer.

## Reproduce

Install Frama-C with the WP plugin and an automatic prover (Alt-Ergo is the
baseline), then run:

```sh
formal/frama-c/verify_wp.sh
```

The driver enables `-wp-rte` as well as functional WP and restricts the proof
boundary to the four public operations above. `FRAMA_PROVERS` can be set to an
installed prover list, for example `FRAMA_PROVERS=qed,alt-ergo`. A missing
Frama-C or prover is an environment error and exits non-zero; it is never
reported as a passing proof.

A proof assumes the documented API preconditions: loaded signal definitions
have widths in 1..64, queue storage is caller-owned and initialized, and the
frame length is one of the legal CAN/CAN FD wire lengths. Those assumptions are
validated by the loaders and conformance tests; they are not unchecked runtime
casts or suppression of a warning.
