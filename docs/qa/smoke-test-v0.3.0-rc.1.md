# CANcestry v0.3.0-rc.1 system smoke test

| Field | Value |
|---|---|
| Document | CANcestry v0.3.0-rc.1 system smoke test record |
| Version | 0.3.0-rc.1 |
| Status | Executed: PASS on the build host. On-target section 5 is a procedure, not an executed run. |
| Owner | QA |
| Date | 2026-09-16 |
| Code state under test | `e1e23bd` — the release-prep branch head at run time (`20d9e98` plus the test-id labels and the traceability gate). The commits after it (`docs/qa/`, `docs/releases/`, `README.md`, `CHANGELOG.md`, and this record) touch documentation only and change no compiled artifact; the sequence was re-executed on that final tree with identical results. |
| Requirement ids | SYS-FR-014, SYS-NF-001, SYS-NF-002, SYS-NF-005, SYS-NF-006, SYS-NF-007, SW-FR-FSM-024/046/053 |
| Related | [issue #13](https://github.com/mfreazer/CANcestry-/issues/13), [`examples/gateway/README.md`](../../examples/gateway/README.md), [`docs/trace/traceability.md`](../trace/traceability.md) |

This record closes the "Final Smoke Test" deliverable of issue #13: run the
integration harness (`examples/gateway/main.c`) and report what it proves. It
also states, without hedging, what it does **not** prove and what a bench run on
target hardware can and cannot add (section 5).

## 1. Verdict

| Question | Answer |
|---|---|
| Does the full gateway loop run correctly? | Yes. `RESULT PASS` in every configuration executed. |
| Host test suite green? | Yes: **31/31** in Debug + ASan/UBSan and **31/31** in Release. |
| Zero heap allocation in the processing loop? | Yes, on two independent paths: the runtime tripwire reports **0 allocation calls**, and the symbol scans find no allocator reference in the core archives or the harness object. |
| Deterministic / bit-exact? | Yes: 5 independent runs byte-identical, and the golden block is identical between the ASan and the Release build. |
| Real-world CAN bus behaviour validated? | **No.** No target hardware or CAN interface was available, and the harness has no CAN backend (section 5.1). |

## 2. Environment

| Item | Value |
|---|---|
| Host | Linux 6.1.158+ x86_64, 2 vCPU, glibc (Debian 12) |
| Compiler | `gcc (Debian 12.2.0-14+deb12u1) 12.2.0` |
| Build system | CMake 4.4.3, GNU Make |
| Python (CI scripts) | 3.11.2 |
| Build A (sanitized) | `cmake -S . -B build-smoke -DCMAKE_BUILD_TYPE=Debug` (ASan + UBSan on) |
| Build B (release) | `cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release -DCANCESTRY_ENABLE_ASAN=OFF` (tripwire armed) |

Build B matters: `main.c` disables its allocator tripwire when the sanitizer
runtime is linked (the override would fight ASan), so only a non-ASan build
exercises the runtime zero-allocation proof. Both configurations were run.

## 3. Results

### 3.1 Host suite, Debug + AddressSanitizer/UndefinedBehaviorSanitizer

```sh
cmake -S . -B build-smoke -DCMAKE_BUILD_TYPE=Debug
cmake --build build-smoke --parallel
ctest --test-dir build-smoke --output-on-failure
```

```
100% tests passed out of 31
Total Test time (real) =   0.51 sec
```

The 31 tests are the 16 event/codec/recipe/FSM unit tests, the seven FSM
conformance suites, the seven repository checks (`cancestry_schemas_valid`,
`cancestry_traceability_consistent`, the four core archive scans and this
example's object scan) and the gateway loop test. No sanitizer diagnostic was
emitted and `-fno-sanitize-recover=all` is set, so any ASan/UBSan finding would
have failed the run rather than been reported.

### 3.2 Host suite, Release (sanitizers off, tripwire armed)

```sh
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release -DCANCESTRY_ENABLE_ASAN=OFF
cmake --build build-release --parallel
ctest --test-dir build-release --output-on-failure
```

```
100% tests passed out of 31
Total Test time (real) =   0.26 sec
```

### 3.3 Integration harness, allocator tripwire armed

```sh
./build-release/examples/gateway/cancestry_gateway; echo "exit=$?"
```

```
CANcestry gateway integration harness (issue #13, v0.3.0-rc.1)
allocator tripwire active: 0 allocation calls during the processing loop
---- golden output (world A) ----
...
---- end golden output ----
RESULT PASS
exit=0
```

The loop processed: 5 ingress frames (3 decoded, 1 `frame_too_short`, 1
`unknown_message`), 4 emitted bus events, 20 idle ticks, 2 timer expiries, 15
dispatches, 3 egress frames, 5 FSM transitions, and the two intended fail-closed
events (1 governor denial, 2 capability denials, 1 guard error with the
`watcher.broken` instance suspended). The harness asserts the fail-closed
reaction itself; the counters below are what it printed:

```
report frames_seen=5 decoded=3 short=1 unknown=1 emitted=4 dispatches=15
report egress=3 recipe_invoked=3 recipe_sent=3 recipe_gov=3
report fsm_received=5 fsm_delivered=3 fsm_ignored=2 ticks=20
report transitions=5 guard_errors=1 cap_denials=2 gov_denials=1 action_errors=2 signals_set=1 timer_expiries=2
report gate(cap=1 gov=1 proc=12 sup=0) watcher(cap=1 gov=0 proc=2 sup=0)
report gate=ALERTING(running) watcher=WATCH(suspended)
report alerts=1 beats=2
trace 46 records dropped=0
```

`gate=ALERTING(running)` with `watcher=WATCH(suspended)` is the fail-closed
signature required by SW-FR-FSM-024: the offending instance is suspended while
the loop keeps draining the queue to `bus_empty=true`.

### 3.4 Determinism

| Check | Result |
|---|---|
| 5 independent Release runs, full stdout | identical, `sha256 = c3cd130dd1cded4ae6f3dc3464f55070ac545e9f50ba0e8e3152538eca1649da` |
| ASan run vs Release run, golden block (line 3 onward) | identical, `sha256 = 87b6f5666e09abb8c2012beaed36b77669fd6daa6f04a10c7e59c23ca7991a4c` |
| Manually linked build (section 5.2 step 3) vs CMake build | identical golden block, same sha256 |

The only line that differs between configurations is line 2, the tripwire
notice, which is why the golden comparison starts at line 3. Time comes solely
from the injected virtual clock (0/10000/20000 µs appear in the trace); there is
no wall clock, no `rand` and no unordered iteration in the path.

### 3.5 Zero-heap static evidence

```sh
python3 ci/check_no_alloc.py <artifact>
```

| Artifact | Result |
|---|---|
| `libcancestry_event.a` | PASS, no allocator references |
| `libcancestry_codec.a` | PASS |
| `libcancestry_recipe.a` | PASS |
| `libcancestry_fsm.a` | PASS |
| `examples/gateway/…/main.c.o` | PASS, 66 symbols checked |

The three *loaders* (`codec_loader`, `recipe_loader`, `fsm_loader`) are excluded
by design and are not part of the processing loop: loading a declarative file may
allocate once, the runtime path may not (SYS-NF-002). The harness's tripwire is
armed only around the loop, after loading.

### 3.6 Warning set

Both builds compile the project with `-Wall -Wextra -Werror -Wpedantic` plus the
embedded coding-rule warnings, and the gateway target adds `-Wconversion`
(issue #13 acceptance criterion). Both were warning-clean; `-Werror` makes this
a hard result, not an observation. The manual link command in section 5.2 was
run with the same full flag set and was also clean.

## 4. What the executed run covers

| Area | Evidence in the golden output |
|---|---|
| Codec decode, LSB0/endian/scaling | `ingress can0 0x120 dlc=8 decoded=2`, and the three speed frames (5, 120, 260) reappearing as `0500…`, `7800…`, `0401…` on egress |
| Codec reject paths | `dropped=frame_too_short`, `dropped=unknown_message` |
| Event model, change-only publish | `emit VehicleSpeed id=1`, `decode IgnitionState unchanged` |
| Queue selection order and dispatch | `dispatch … seq=1..15` in timestamp/priority/sequence order |
| Recipe engine, interface binding, egress | `egress recipe can1 0x321 len=8 …` (three frames, ascending speed) |
| FSM runtime, guard/action/transition | `fsm state gate.primary NORMAL->ALERTING`, `action 2 set_variable ok` |
| Timers, missed-tick accounting | `timer 0 heartbeat expired` at 10000/20000 µs, `timer_expiries=2` |
| Capability + governor fail-closed | `denied 1 send_message/governor`, `denied 2 set_signal/governor`, `lifecycle 3 suspended` |
| Expression fault handling | `guard_error 6 sig.NoSuchSignal > 0: unauthorized signal acces`, instance suspended |
| Trace and counters (SYS-NF-005) | `trace 46 records dropped=0`, all `report …` lines |

## 5. On target hardware and on a real CAN bus

### 5.1 What was not executed, and why

| Item | Status | Reason |
|---|---|---|
| Run the harness on target hardware | Not executed | No target board, debug probe or cross toolchain was available in the build environment (Debian package mirrors are unreachable there). |
| Real CAN traffic on a physical bus | Not executed, and not reachable with this harness | `examples/gateway/main.c` is a **logic** harness: ingress is a fixed in-memory frame array and egress is a recording sink. It never opens a SocketCAN socket, never touches hardware, and its definitions are embedded string literals — deliberately, per `agents.md` ("no direct hardware access in core"). Driving a real bus from here belongs to the CAN integration phase. |
| Bus-off recovery, arbitration, error frames, latency/jitter | Out of scope for v0.3.0-rc.1 | Owned by the CAN integration phase (`SW-FR-CAN-001..006`, `SYS-NF-003/004`); those rows stay `planned` in `docs/trace/traceability.csv`. |

So the honest statement is: **the smoke test validates the deterministic core
loop end to end, on the host, at logic level.** A target run of the same harness
adds one real property — that the exact same byte stream comes out of a
different CPU/toolchain/libc — and nothing about the wire.

### 5.2 Bench procedure (run this on the target; expected results in 5.3)

1. **Fetch the tag** (valid only after `v0.3.0-rc.1` exists):

   ```sh
   git clone https://github.com/mfreazer/CANcestry-.git && cd CANcestry-
   git checkout v0.3.0-rc.1
   git rev-parse HEAD   # record it in the bench log
   ```

2. **Host baseline first.** A target result is only meaningful next to the
   host result on the same commit:

   ```sh
   cmake -S . -B build-rel -DCMAKE_BUILD_TYPE=Release -DCANCESTRY_ENABLE_ASAN=OFF
   cmake --build build-rel --parallel
   ctest --test-dir build-rel --output-on-failure        # expect 31/31
   ./build-rel/examples/gateway/cancestry_gateway | tail -n +3 > golden-host.txt
   sha256sum golden-host.txt   # must be 87b6f566…7991a4c
   ```

3. **Build the harness for the target.** The CMake example target is host-only
   (`CMakeLists.txt` adds `examples/` only when not cross-compiling), so build
   the core with CMake for the target and link the harness by hand with the
   target compiler — this is the command verified in section 3.4:

   ```sh
   cmake -S . -B build-target -DCMAKE_TOOLCHAIN_FILE=<your-toolchain.cmake> \
         -DCMAKE_BUILD_TYPE=Release -DCANCESTRY_ENABLE_ASAN=OFF \
         -DCANCESTRY_BUILD_TESTS=OFF -DCANCESTRY_BUILD_EXAMPLES=OFF
   cmake --build build-target --parallel

   $CROSS-gcc -std=c99 -Wall -Wextra -Werror -Wpedantic -Wconversion \
     -Icore/event/include -Icore/codec/include -Icore/recipe/include -Icore/fsm/include \
     -o gateway-target examples/gateway/main.c \
     build-target/core/fsm/libcancestry_fsm_loader.a \
     build-target/core/fsm/libcancestry_fsm.a \
     build-target/core/recipe/libcancestry_recipe_loader.a \
     build-target/core/recipe/libcancestry_recipe.a \
     build-target/core/codec/libcancestry_codec_loader.a \
     build-target/core/codec/libcancestry_codec.a \
     build-target/core/event/libcancestry_event.a -lm
   ```

   Archive order matters (a static link resolves left to right). On a
   hosted target (Linux SBC, QEMU) this is all that is needed; on a bare-metal
   target, retarget `printf` and skip the allocator tripwire (it is glibc-only
   and already degrades gracefully), then expect the golden block from section 6.

4. **Run and compare:**

   ```sh
   ./gateway-target > target-run.txt 2>&1; echo "exit=$?"
   tail -n +3 target-run.txt > golden-target.txt
   diff -u golden-host.txt golden-target.txt && echo "BYTE-IDENTICAL"
   grep -E "allocator tripwire|RESULT" target-run.txt
   ```

5. **Record and attach:** target name, toolchain version, `git rev-parse HEAD`,
   the three commands above, the sha256 of `golden-target.txt`, and the
   `RESULT PASS` line.

### 5.3 Pass/fail criteria and what a passing target run proves

| Criterion | Expected on a hosted target |
|---|---|
| Process exit status | `0` |
| Result line | `RESULT PASS` |
| Golden block vs host | byte-identical (`diff` empty), `sha256 = 87b6f566…7991a4c` |
| Tripwire line | `allocator tripwire active: 0 allocation calls during the processing loop` |
| Trace/counter lines | identical to the host run (same `report …` values) |

A pass proves **SYS-NF-007 portability** and **SW-FR-FSM-046/SYS-NF-001
determinism across a different CPU, ABI and C library** — the same events and
the same egress frames on the target as on the host. It does **not** prove
anything about bus behaviour; that is the CAN integration phase's HIL workflow
(`docs/qa/test-strategy.md` section 8.3), and it should be planned with a CAN analyser
in the loop, not with this harness.

### 5.4 Bench checklist

| # | Step | Expected | Record |
|---|---|---|---|
| 1 | Checkout tag, record SHA | tag resolves | SHA |
| 2 | Host baseline: 31/31, golden sha256 | `87b6f566…` | output |
| 3 | Cross-build core + manual harness link | no warnings (`-Werror`) | toolchain version |
| 4 | Target run | `RESULT PASS`, 0 allocations | full stdout |
| 5 | Golden diff | empty | sha256 |
| 6 | Bus-level validation | not covered here (out of scope) | planned with CAN phase |

## 6. Appendix: golden output (world A), the diff target for section 5.2

Identical in the Debug+ASan, Release and manually-linked builds
(`sha256 = 87b6f5666e09abb8c2012beaed36b77669fd6daa6f04a10c7e59c23ca7991a4c`).

```text
fsm state gate.primary (initial)->NORMAL
fsm log gate.primary info gate normal
fsm state watcher.broken (initial)->WATCH
setup: maps=1 recipes=1 machines=2 instances=2
ingress can0 0x120 dlc=8 decoded=2
emit VehicleSpeed id=1
emit IgnitionState id=2
dispatch state_entered seq=1
dispatch state_entered seq=2
dispatch signal_changed seq=3
egress recipe can1 0x321 len=8 0500000000000000
dispatch signal_changed seq=4
ingress can0 0x120 dlc=8 decoded=2
emit VehicleSpeed id=1
decode IgnitionState unchanged
dispatch signal_changed seq=5
egress recipe can1 0x321 len=8 7800000000000000
ingress can0 0x120 dlc=1 dropped=frame_too_short
ingress can0 0x7FF dlc=8 dropped=unknown_message
ingress can0 0x120 dlc=8 decoded=2
emit VehicleSpeed id=1
decode IgnitionState unchanged
dispatch signal_changed seq=6
egress recipe can1 0x321 len=8 0401000000000000
fsm signal WarnLamp set=1
fsm state gate.primary NORMAL->ALERTING
fsm log gate.primary warning overspeed latched
dispatch state_exited seq=7
dispatch signal_changed seq=8
dispatch state_entered seq=9
fsm state gate.primary ALERTING->ALERTING
fsm log gate.primary warning overspeed latched
fsm state gate.primary ALERTING->ALERTING
fsm log gate.primary warning overspeed latched
ticks=20 now_us=25000
dispatch timer_expired seq=10
dispatch state_exited seq=11
dispatch state_entered seq=12
dispatch timer_expired seq=13
dispatch state_exited seq=14
dispatch state_entered seq=15
bus_empty=true
report frames_seen=5 decoded=3 short=1 unknown=1 emitted=4 dispatches=15
report egress=3 recipe_invoked=3 recipe_sent=3 recipe_gov=3
report fsm_received=5 fsm_delivered=3 fsm_ignored=2 ticks=20
report transitions=5 guard_errors=1 cap_denials=2 gov_denials=1 action_errors=2 signals_set=1 timer_expiries=2
report gate(cap=1 gov=1 proc=12 sup=0) watcher(cap=1 gov=0 proc=2 sup=0)
report gate=ALERTING(running) watcher=WATCH(suspended)
report alerts=1 beats=2
report WarnLamp=1
trace 46 records dropped=0
0 1 lifecycle 1 ready
0 2 lifecycle 1 ready
0 1 lifecycle 2 running
0 1 transition 1 (initial)->NORMAL
0 1 action 0 log/info: gate normal
0 1 action 6 log ok
0 1 event 4 state_entered seq=1
0 2 lifecycle 2 running
0 2 transition 1 (initial)->WATCH
0 2 event 4 state_entered seq=1
0 1 event 2 signal_changed seq=2
0 1 guard_false 0 sig.VehicleSpeed >= 200
0 2 event 2 signal_changed seq=2
0 2 guard_error 6 sig.NoSuchSignal > 0: unauthorized signal acces
0 2 lifecycle 3 suspended
0 1 event 2 signal_changed seq=3
0 1 guard_false 0 sig.VehicleSpeed >= 200
0 1 event 2 signal_changed seq=4
0 1 guard_true 0 sig.VehicleSpeed >= 200
0 1 action 2 set_variable ok
0 1 action_error -5 send_message: denied
0 1 denied 1 send_message/governor
0 1 action_error -5 set_signal: denied
0 1 denied 2 set_signal/governor
0 1 action 1 set_signal ok
0 1 transition 1 NORMAL->ALERTING
0 1 action 1 log/warning: overspeed latched
0 1 action 6 log ok
0 1 event 5 state_exited seq=5
0 1 event 4 state_entered seq=6
10000 1 timer 0 heartbeat expired
10000 1 event 3 timer_expired seq=7
10000 1 action 2 set_variable ok
10000 1 transition 1 ALERTING->ALERTING
10000 1 action 1 log/warning: overspeed latched
10000 1 action 6 log ok
10000 1 event 5 state_exited seq=8
10000 1 event 4 state_entered seq=9
20000 1 timer 0 heartbeat expired
20000 1 event 3 timer_expired seq=10
20000 1 action 2 set_variable ok
20000 1 transition 1 ALERTING->ALERTING
20000 1 action 1 log/warning: overspeed latched
20000 1 action 6 log ok
20000 1 event 5 state_exited seq=11
20000 1 event 4 state_entered seq=12
```

> The trace text is truncated at 46 columns for the last field
> (`… unauthorized signal acces`) by the harness's own renderer; it is
> reproduced verbatim here, because a bench diff compares bytes.

## 7. Findings

1. **No defects found.** Both build configurations, the harness assertions, the
   symbol scans and the schema check are green, with no sanitizer diagnostic.
2. **Target/bus validation is still open, and is a scoping matter rather than a
   defect.** The harness is a logic harness by design and there is no
   `platform/` layer or CAN backend in v0.3.0-rc.1, so "real-world CAN bus
   behaviour" cannot be validated yet by any artifact in this release. Section
   5.2 gives the part that a bench can validate today (cross-architecture
   determinism); bus behaviour needs the CAN integration phase.
3. **The zero-allocation proof is configuration-dependent by design.** Under
   ASan the runtime tripwire is unavailable (it cannot override the sanitizer's
   allocator); the archive/object symbol scans cover that configuration. Both
   were executed here, which is why this record reports two independent zero-heap
   results rather than one.
