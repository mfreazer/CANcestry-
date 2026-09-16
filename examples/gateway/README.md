# CANcestry gateway integration harness

Top-level integration harness for
[issue #13](https://github.com/mfreazer/CANcestry-/issues/13) (Phase 5,
v0.3.0 Release Candidate): one deterministic, zero-heap mock gateway loop that
proves the isolated core modules operate together.

Implements: SYS-FR-003/004/005/006/010/014, SYS-NF-001/002/005,
SW-FR-EVENT-004/006, SW-FR-FSM-024/038/042/046, SW-FR-GOV-005/006.

## The cycle

```
raw CAN frame ingress (can0)
  -> core/codec decode          (cancestry_codec_decode_frame)
  -> core/event queue           (signal_changed events on the shared bus)
  -> core/recipe                (speed_mirror: VehicleSpeed -> ClusterDisplay)
     core/fsm                   (SpeedGate overspeed latch + BrokenWatcher)
  -> core/codec encode          (send_message paths of both engines)
  -> egress sink (can1)         (recorded, bit-exact golden expectation)
```

The scenario is fixed: five frames on `can0` (speeds 5, 120, a too-short
frame, an unknown CAN id, then 260), a decode-to-bus publish per changed
signal, a drain loop that dispatches every bus event to the recipe engine and
the FSM engine, and 20 idle ticks that fire the FSM heartbeat timer twice.
The overspeed frame latches `gate.primary` into `ALERTING`; the
deliberately broken guard of `watcher.broken` faults and suspends that
instance while the loop continues (fail-closed, SW-FR-FSM-024/038).

## Fail-closed demonstrations

| Situation | Reaction |
|---|---|
| `watcher.broken` guard reads `sig.NoSuchSignal` (not in the capability read allowlist) | guard error + capability denial, instance suspended, queue keeps draining |
| `send_message AlertFrame` (0x322) | governor denial (rate/allowlist shape); recorded, counted, no partial effect |
| `set_signal VehicleSpeed` (not in the write allowlist) | capability denial before the governor is consulted (SW-FR-FSM-041) |
| short frame / unknown CAN id | codec `frame_too_short` warning / unknown-message drop, counted, no event |

## Zero heap allocation

Loading the three declarative files is the one phase that allocates (the
loaders do, by contract). The entire processing loop — ingress, decode,
queueing, dispatch, recipe and FSM execution, ticks, encode and egress — runs
on statically allocated storage only.

Two complementary proofs:

1. **Runtime tripwire** (glibc hosts, non-ASan builds): `main.c` interposes
   the allocator and asserts zero calls while the guard is armed, exactly like
   `tests/unit/core/fsm/test_no_alloc.c`. Under AddressSanitizer the tripwire
   is disabled (overriding `malloc` would fight the sanitizer runtime).
2. **Symbol scans**: `ci/check_no_alloc.py` runs in CTest on the core runtime
   archives and on this example's own object file
   (`cancestry_gateway_no_alloc_symbols`), so `main.c` may not reference any
   allocator entry point in any configuration.

## Determinism

The world is built twice from the same definitions and run independently; the
full output log and the egress frames must compare byte-identical
(SYS-NF-001, SW-FR-FSM-046). The harness also prints the golden output of
world A; separate invocations reproduce it bit-for-bit. Time comes only from
the injected virtual clock — no wall clock, no `rand`, no unordered iteration.

## Integration policies pinned by this harness

Documented decisions the specs leave open (each is visible in the golden
output, so a future change is not silent):

- **Dispatch order per bus event**: recipe engine first, then FSM engine.
- **No FSM re-entry for engine-internal events**: `timer_expired` and
  `state_entered`/`state_exited` reach FSM instances through the engine's own
  tick/subscription machinery; the drain loop dispatches those classes to the
  recipe engine only, because re-injecting them into the FSM engine would
  deliver them twice.
- **Change-only publish**: decode publishes a `signal_changed` event only when
  the signal value actually moved (the recipe engine's `set_signal` policy).

## Running

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug   # ASan + UBSan
cmake --build build
ctest --test-dir build -R gateway --output-on-failure
```

Registered tests:

| Test | Proves |
|---|---|
| `cancestry_gateway_loop` | full cycle, bit-exact egress, fail-closed, determinism, zero allocations |
| `cancestry_gateway_no_alloc_symbols` | `main.c` references no allocator entry point |

The target compiles with the project's strict warning set plus `-Wconversion`
(issue #13 acceptance criteria).
