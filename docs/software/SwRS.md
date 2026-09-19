# CANcestry Software Requirements Specification

| Field | Value |
|---|---|
| Document | CANcestry Software Requirements Specification |
| Version | 1.0.0-rc.1 |
| Status | Release candidate; QA-EV-01 remains open |
| Release gate | Final 1.0.0 bump, tag, and milestone closure are deferred until QA-EV-01 is formally closed |
| Owner | System Engineer |
| Approver | Maintainer + QA |
| Last Review | 2026-09-18 |

## 1. Software Scope

The software includes:

- firmware runtime,
- package loader,
- codec engine,
- recipe engine,
- state machine runtime,
- event bus,
- safety governor,
- logger,
- host CLI,
- simulator,
- package validator.

## 2. Package Requirements

| ID | Requirement | Priority |
|---|---|---|
| SW-FR-PKG-001 | The software shall parse package manifests. | High |
| SW-FR-PKG-002 | The software shall validate package schemas. | High |
| SW-FR-PKG-003 | The software shall enforce package capabilities. | High |
| SW-FR-PKG-004 | The software shall support package enable/disable state. | High |
| SW-FR-PKG-005 | The software shall report package load errors with source location when possible. | Medium |
| SW-FR-PKG-006 | The software shall support package-level interface bindings. | High |
| SW-FR-PKG-007 | The software shall reject ambiguous short signal names. | High |

## 3. Codec Requirements

| ID | Requirement | Priority |
|---|---|---|
| SW-FR-CODEC-001 | The software shall decode CAN frames into named signals. | High |
| SW-FR-CODEC-002 | The software shall encode named signals into CAN frames. | High |
| SW-FR-CODEC-003 | The software shall support little-endian and big-endian signals using the canonical LSB0 bit model. | High |
| SW-FR-CODEC-004 | The software shall support scaling and offset. | High |
| SW-FR-CODEC-005 | The software shall support signed and unsigned integer signals. | High |
| SW-FR-CODEC-006 | The software shall support boolean and enum value mappings. | Medium |
| SW-FR-CODEC-007 | The software shall detect signal definition conflicts. | High |
| SW-FR-CODEC-008 | The software shall drop frames too short for declared signals and raise a codec warning. | High |

## 4. Event Bus Requirements

| ID | Requirement | Priority |
|---|---|---|
| SW-FR-EVENT-001 | The software shall define a common event structure. | High |
| SW-FR-EVENT-002 | The software shall support event timestamps. | High |
| SW-FR-EVENT-003 | The software shall support can_rx, signal_changed, timer_expired, state_entered, state_exited, fault_raised, and power_mode_changed events. | High |
| SW-FR-EVENT-004 | The software shall maintain bounded event queues. | High |
| SW-FR-EVENT-005 | The software shall expose event drop counters. | High |
| SW-FR-EVENT-006 | The software shall process events according to the normative event-ordering specification, including deterministic fault admission and saturation behavior. | High |
| SW-FR-EVENT-007 | The event queue shall reserve a configured number of physical slots for fault events so ordinary traffic cannot consume the fault reserve. | High |
| SW-FR-EVENT-008 | When a full queue contains only fault events, the runtime shall preserve the bounded fault set and invoke the configured HAL fail-safe and IWDG escalation hooks in that order. | High |

## 5. Recipe Requirements

| ID | Requirement | Priority |
|---|---|---|
| SW-FR-RECIPE-001 | The software shall execute recipes triggered by events. | High |
| SW-FR-RECIPE-002 | The software shall support conditions on signals and messages. | High |
| SW-FR-RECIPE-003 | The software shall support send_message, set_signal, set_variable, start_timer, stop_timer, reset_timer, log, and raise_fault actions. | High |
| SW-FR-RECIPE-004 | The software shall support message timeout watches. | Medium |
| SW-FR-RECIPE-005 | The software shall support directional filtering for gateway behavior. | High |
| SW-FR-RECIPE-006 | The recipe validator shall reject transition actions in recipes. | High |
| SW-FR-RECIPE-007 | Recipes shall resolve interfaces using physical interface names or package-level bindings. | High |

## 6. Governor Requirements

| ID | Requirement | Priority |
|---|---|---|
| SW-FR-GOV-001 | The software shall enforce declared TX ID permissions. | High |
| SW-FR-GOV-002 | The software shall enforce TX rate limits using token buckets. | High |
| SW-FR-GOV-003 | The software shall block actions from disabled packages. | High |
| SW-FR-GOV-004 | The software shall enter SAFE mode on critical violations. | High |
| SW-FR-GOV-005 | The software shall expose governor counters and violation logs. | High |
| SW-FR-GOV-006 | The governor shall fail closed. | High |

## 7. CAN Core Requirements

| ID | Requirement | Priority |
|---|---|---|
| SW-FR-CAN-001 | The software shall initialize CAN interfaces from configuration. | High |
| SW-FR-CAN-002 | The software shall support listen-only mode. | High |
| SW-FR-CAN-003 | The software shall support transmission only when enabled. | High |
| SW-FR-CAN-004 | The software shall report bus-off state. | High |
| SW-FR-CAN-005 | The software shall support automatic bus-off recovery while respecting mode policy. | High |
| SW-FR-CAN-006 | The software shall support hardware/software filtering where available. | Medium |

## 8. Logging Requirements

| ID | Requirement | Priority |
|---|---|---|
| SW-FR-LOG-001 | The software shall log raw CAN frames. | Medium |
| SW-FR-LOG-002 | The software shall log decoded signal changes. | Medium |
| SW-FR-LOG-003 | The software shall log FSM state transitions. | High |
| SW-FR-LOG-004 | The software shall log faults. | High |
| SW-FR-LOG-005 | The software shall support log retrieval via host interface. | Medium |

**Platform-Dependent Strength:**

- *Targets with retention registers:* The specific fault code shall be persisted and reported on next boot.
- *Targets without retention registers:* A generic "Hard Fault Escalation" reset reason shall be recorded on next boot.

## 9. Simulation Requirements

| ID | Requirement | Priority |
|---|---|---|
| SW-FR-SIM-001 | The simulator shall execute the same package runtime core as the target firmware where practical. | High |
| SW-FR-SIM-002 | The simulator shall support virtual CAN interfaces. | High |
| SW-FR-SIM-003 | The simulator shall support replay of recorded CAN traffic. | Medium |
| SW-FR-SIM-004 | The simulator shall support expected-output assertions. | High |

## 10. FSM Runtime Requirements

| ID | Requirement | Priority |
|---|---|---|
| SW-FR-FSM-001 | The runtime shall load FSM definitions from declarative files. | High |
| SW-FR-FSM-002 | The runtime shall compile definitions into an internal representation before execution. | High |
| SW-FR-FSM-003 | The runtime shall reject invalid FSM definitions. | High |
| SW-FR-FSM-004 | The runtime shall support multiple named FSM instances. | High |
| SW-FR-FSM-005 | Each FSM definition shall have exactly one initial state. | High |
| SW-FR-FSM-006 | States shall be named and unique within a state machine. | High |
| SW-FR-FSM-007 | States shall support entry actions. | High |
| SW-FR-FSM-008 | States shall support exit actions. | High |
| SW-FR-FSM-009 | The v0.2 runtime shall support flat states only. | High |
| SW-FR-FSM-010 | The runtime shall transition between states based on events. | High |
| SW-FR-FSM-011 | Transitions shall support optional guard conditions. | High |
| SW-FR-FSM-012 | Transition selection shall be deterministic and use declaration order. | High |
| SW-FR-FSM-013 | Transitions shall support actions. | High |
| SW-FR-FSM-014 | On transition, exit actions, transition actions, then entry actions shall execute. | High |
| SW-FR-FSM-015 | Self-transitions shall execute exit and entry actions unless future internal transitions are defined. | Medium |
| SW-FR-FSM-016 | The runtime shall limit transition chain depth. Default limit: 4. | High |
| SW-FR-FSM-017 | The runtime shall support can_rx, signal_changed, timer_expired, state_entered, state_exited, fault_raised, and power_mode_changed events. | High |
| SW-FR-FSM-018 | Events shall carry type, timestamp, source, and payload. | High |
| SW-FR-FSM-019 | Each FSM instance shall use a bounded incoming event queue. Default depth: 64. | High |
| SW-FR-FSM-020 | Per-FSM incoming queue overflow shall reserve fault slots, drop-newest ordinary events at the reserve boundary, admit faults deterministically, and escalate when all slots contain faults. | High |
| SW-FR-FSM-021 | The runtime shall prevent uncontrolled recursive event generation. | High |
| SW-FR-FSM-022 | The runtime shall support send_message, set_signal, set_variable, start_timer, stop_timer, reset_timer, log, raise_fault, and transition actions. | High |
| SW-FR-FSM-023 | Actions shall be validated against package capabilities before execution. | High |
| SW-FR-FSM-024 | If an action fails, the runtime shall record the failure and continue safely. | High |
| SW-FR-FSM-025 | FSM actions shall not access hardware directly. | High |
| SW-FR-FSM-026 | The runtime shall support one-shot timers. | High |
| SW-FR-FSM-027 | The runtime shall support periodic timers. | High |
| SW-FR-FSM-028 | Expired timers shall generate timer events. | High |
| SW-FR-FSM-029 | The runtime shall support starting, stopping, and resetting timers. | High |
| SW-FR-FSM-030 | Timer tick shall be 1 ms. | Medium |
| SW-FR-FSM-031 | The runtime shall support named variables per FSM instance. | High |
| SW-FR-FSM-032 | Variables shall support boolean, integer, and float types. | High |
| SW-FR-FSM-033 | Variables shall support default initialization. | High |
| SW-FR-FSM-034 | Variables shall be scoped to an FSM instance. | Medium |
| SW-FR-FSM-035 | The runtime shall include a safe expression evaluator. | High |
| SW-FR-FSM-036 | The expression evaluator shall support arithmetic, comparison, logical operators, and built-in functions. | High |
| SW-FR-FSM-037 | The expression evaluator shall not permit arbitrary code execution. | High |
| SW-FR-FSM-038 | Expression evaluation failure shall raise a package error or fault. | High |
| SW-FR-FSM-039 | A state machine shall only perform actions allowed by package capabilities. | High |
| SW-FR-FSM-040 | The runtime shall block transmission of CAN IDs not declared by the package. | High |
| SW-FR-FSM-041 | The runtime shall block writing to signals not permitted by the package. | High |
| SW-FR-FSM-042 | All FSM side effects shall pass through the safety governor. | High |
| SW-FR-FSM-043 | The runtime shall process events sequentially per FSM instance. | High |
| SW-FR-FSM-044 | The runtime shall support a periodic tick for timer management. | High |
| SW-FR-FSM-045 | The runtime shall enforce an execution budget per event. | High |
| SW-FR-FSM-046 | For the same event sequence and time base, the runtime shall produce deterministic behavior. | High |
| SW-FR-FSM-047 | A fault in one FSM instance shall not corrupt other instances. | High |
| SW-FR-FSM-048 | The runtime shall record state transitions. | High |
| SW-FR-FSM-049 | The runtime shall optionally record processed events. | Medium |
| SW-FR-FSM-050 | The runtime shall expose counters for events, transitions, timers, faults, and queue overflows. | High |
| SW-FR-FSM-051 | The FSM runtime shall be executable on host for simulation and CI. | High |
| SW-FR-FSM-052 | The runtime shall expose test hooks for injecting events and inspecting state. | High |
| SW-FR-FSM-053 | The runtime shall support golden-output tests. | High |
| SW-FR-FSM-054 | Deferred action transitions shall not interrupt the current event-driven transition. | High |
| SW-FR-FSM-055 | If multiple transition actions are requested in one action sequence, the first wins and later ones are ignored with warning. | High |

## 11. Hardware Abstraction Layer Requirements (Phase 6, issue #17)

| ID | Requirement | Priority |
|---|---|---|
| SW-FR-HAL-001 | The HAL shall define a hardware-agnostic CAN frame type with fixed payload, identifier, flags, and monotonic timestamp. | High |
| SW-FR-HAL-002 | The HAL shall expose caller-owned bounded RX/TX rings that never allocate heap memory. | High |
| SW-FR-HAL-003 | hal_poll_rx and hal_send_tx shall be strictly non-blocking and bounded in time. | High |
| SW-FR-HAL-004 | The HAL shall capture hardware/kernel RX timestamps and map them into the CANcestry monotonic microsecond time base without floating-point conversion; timestamps shall be strictly monotonic per interface. | High |
| SW-FR-HAL-005 | The HAL shall fail-closed: every syscall failure or bus error shall log a fault, transition the interface to a safe state, and inject a FAULT_RAISED event into the core/event queue rather than crashing. | High |
| SW-FR-HAL-006 | The HAL RX/TX rings shall never silently overwrite unread data; overflows shall drop the incoming frame, increment a counter, and raise a RING_OVERFLOW fault. | High |
| SW-FR-HAL-007 | The HAL shall deterministically raise FAULT_RAISED events for bus states Error Passive, Bus Off, Interface Down, and back-pressure so the FSM can react. | High |
| SW-FR-HAL-008 | A mock HAL backend shall exist for CI that supports deterministic RX frame injection and TX frame capture without touching the kernel. | High |
| SW-FR-HAL-009 | The mock HAL shall support deterministic injection of bus error faults (Error Passive, Bus Off, Interface Down) to exercise fail-closed behaviour. | High |
| SW-FR-HAL-010 | A Linux SocketCAN backend shall be provided using non-blocking recvmsg/sendmsg with SO_TIMESTAMPNS. | High |
| SW-FR-HAL-011 | The HAL shall provide a non-blocking get_status call returning per-interface counters, state, last fault, and last RX timestamp. | Medium |
| SW-FR-HAL-012 | HAL configuration (interface names, bitrates, listen-only) shall be validated against schemas/hal-0.1.0.schema.json before the runtime opens any interface. | High |

## 12. CAN FD Requirements (Phase 7, issue #20)

| ID | Requirement | Priority |
|---|---|---|
| SW-FR-CANFD-001 | The codec engine shall encode and decode payloads of up to 64 bytes. A codec map that declares `can_fd: true` shall be allowed to declare the CAN FD payload lengths (0-8, 12, 16, 20, 24, 32, 48, 64) as `dlc` and to address payload bits 0..511; a map without the flag shall keep the classic limits (`dlc` 0..8, payload bits 0..63) unchanged. | High |
| SW-FR-CANFD-002 | The runtime shall define exactly one normative test for a representable CAN payload length - classic 0..8 bytes, CAN FD 0-8, 12, 16, 20, 24, 32, 48 and 64 bytes - and the codec, the HAL and the platform backends shall all use it, rejecting any other length as a malformed frame instead of truncating it. | High |
| SW-FR-CANFD-003 | The HAL shall deliver or transmit a CAN FD frame only on an interface that negotiated CAN FD. On an interface without CAN FD the frame shall be dropped, counted in the interface status, and reported by a `PROTOCOL_UNSUPPORTED` `FAULT_RAISED` event raised through the same deterministic path and priority class as every other HAL fault. Truncating a CAN FD frame to 8 bytes is prohibited on both ingress and egress. | High |
| SW-FR-CANFD-004 | CAN FD capability shall be negotiated once at interface open and reported by the platform backend. The Linux SocketCAN backend shall request `CAN_RAW_FD_FRAMES`, handle `CANFD_MTU` messages on ingress and egress, and continue as a classic-only interface without failing the open when the interface refuses CAN FD. The codec loader shall refuse a `can_fd: true` map at load time, with a distinct status, when the declared platform capabilities do not include CAN FD. | High |
| SW-FR-CANFD-005 | CAN FD support shall not allocate heap memory in the runtime path and shall not degrade classic CAN handling: the 64-byte payload buffer shall be statically sized inside the caller-owned frame struct, platform wire buffers shall be fixed-size (`CAN_MTU`/`CANFD_MTU`), and a classic frame shall copy only its declared length. | High |
| SW-FR-CANFD-006 | The declarative egress path (recipe and FSM `send_message`) shall refuse a message whose declared `dlc` exceeds the classic 8-byte payload instead of truncating it, and shall record the refusal as an action error. CAN FD transmit is performed through the HAL. | High |

## 13. Toolchain Requirements (Phase 8, issue #22)

| ID | Requirement | Priority |
|---|---|---|
| SW-FR-TOOL-001 | The `tools/yaml2c` code generator shall convert a valid FSM YAML document (`schemas/fsm-0.3.0.schema.json`) into a compilable C99 header that statically initializes the runtime FSM definition types with `const static` data, so the runtime YAML loader can be eliminated on the target. | High |
| SW-FR-TOOL-002 | `tools/yaml2c` shall convert a valid codec-map YAML document (`schemas/codec-map-0.3.0.schema.json`) into a compilable C99 header statically initializing `cancestry_codec_map_t`, with every derived field (payload bit windows, sawtooth bounds, value mappings) computed exactly as `core/codec/src/loader.c` computes it. | High |
| SW-FR-TOOL-003 | Generated headers shall contain zero-allocation `const static` data only, shall map 1:1 onto the runtime struct types with no runtime pointer arithmetic or initialization fixes, and a binary linking them shall pass `ci/check_no_alloc.py`. | High |
| SW-FR-TOOL-004 | `tools/yaml2c` shall validate every input against the corresponding canonical JSON Schema (Draft 2020-12, with format checkers) before emitting code ("schema is law") and shall enforce the C loaders' structural rules (duplicate keys, YAML 1.1 scalar handling, number grammar, limits) with loader-identical verdicts, refusing to emit C on any violation. | High |
| SW-FR-TOOL-005 | `tools/trace_viewer` shall parse binary trace dumps in the format normatively documented in `docs/system/trace-dump-format.md` and shall reconstruct the chronological order from the monotonically increasing sequence numbers when the ring has wrapped; a dump with more than one descent point shall be refused, never guessed. | High |
| SW-FR-TOOL-006 | `tools/trace_viewer` shall render a human-readable chronological timeline of events, state transitions, actions, guard results, timers, lifecycle steps and faults (FSM and HAL origin), with relative or absolute timestamps and a JSON form. | High |
| SW-FR-TOOL-007 | `tools/trace_viewer` shall optionally emit a Graphviz `.dot` graph of the state transitions actually taken, drawing an edge in red when a fault for the same instance was recorded before that instance's next transition. | Medium |
| SW-FR-TOOL-008 | `ci/check_schemas_valid.py` shall perform full Draft-2020-12 metaschema (logical) validation of every schema document in `schemas/` using the `jsonschema` library with format checkers, in addition to the structural checks (dialect, `$id`/file-name version match); without `jsonschema` installed it shall degrade to an explicit SKIP notice, never a silent pass. | High |
| SW-FR-TOOL-009 | The Python tools shall be deterministic: identical input bytes produce identical output, and trace reconstruction shall depend only on the dump content, never on wall-clock time or environment. | High |
| SW-FR-TOOL-010 | The Python CLIs shall follow the repository exit-code convention (0 success, 1 rejected input/data, 2 usage or environment error) and shall introduce no new C dependencies in `core/` or `platform/`; host-side tool dependencies are limited to the standard library plus `PyYAML`, `jsonschema` and `pytest`. | High |

## 14. Diagnostic Transport and UDS Requirements (Phase 10, issue #26)

Scope note: this phase specifies an ISO 15765-2 (ISO-TP) transport engine over
classic 8-byte CAN frames and an ISO 14229-1 (UDS) server subset for the three
core data services. CAN FD ISO-TP (SF escape frames), UDS sessions/security
levels, functional addressing and the remaining UDS services are out of scope
and belong to later phases.

### 14.1 ISO-TP transport (core/transport)

| ID | Requirement | Priority |
|---|---|---|
| SW-FR-TP-001 | The software shall provide an ISO-TP transport engine that reassembles and segments multi-frame diagnostic messages over classic CAN frames using caller-owned, statically sized RX and TX buffers, with zero heap allocation in the runtime path. | High |
| SW-FR-TP-002 | The engine shall decode and handle the four ISO 15765-2 frame types on classic CAN: Single Frame (PCI 0x0), First Frame (PCI 0x1), Consecutive Frame (PCI 0x2) and Flow Control (PCI 0x3), including the 12-bit First Frame length and the 4-bit rolling Consecutive Frame sequence number. | High |
| SW-FR-TP-003 | On a First Frame whose total message length exceeds the statically allocated receive buffer, the engine shall abort the transfer before storing any payload byte, transmit a Flow Control frame with status OVFLW, and raise a `TRANSPORT_BUFFER_OVERFLOW` fault. The engine shall never allocate, truncate or accept an oversized message. | High |
| SW-FR-TP-004 | On a Consecutive Frame whose sequence number is not the expected next value, the engine shall immediately drop the session, clear the partially reassembled buffer, and raise a `TRANSPORT_PROTOCOL_FAULT`. The engine shall not resume the aborted session; a subsequent frame is judged as a fresh start. | High |
| SW-FR-TP-005 | On a frame whose PCI is invalid (nibble 0x4..0xF), whose frame length contradicts its PCI type, or whose frame type cannot apply in the current session state (for example a Single Frame or a new First Frame while reassembly is in progress), the engine shall drop the session, clear the buffer, and raise a `TRANSPORT_PROTOCOL_FAULT`. The engine shall never crash or hang on any input. | High |
| SW-FR-TP-006 | ISO-TP timers shall be driven exclusively by a deterministic 1 ms tick using the `core/event` clock abstraction: no `sleep`, blocking wait or wall-clock read shall exist in the transport path. The receiver N_Cr timer (between Consecutive Frames) and the sender N_Bs timer (until Flow Control) shall abort the session with a `TRANSPORT_TIMEOUT` fault on expiry, and the N_As timer shall bound the retransmission of a frame the platform sink declined before the session aborts with a `TRANSPORT_TIMEOUT` fault. | High |
| SW-FR-TP-007 | The transmit path shall segment a payload of up to 4095 bytes (the 12-bit First Frame limit) into a First Frame followed by Consecutive Frames with the rolling sequence number starting at 1, and shall emit a Single Frame for payloads of up to 7 bytes. | High |
| SW-FR-TP-008 | The transmit path shall honour the receiver's Flow Control parameters: it shall wait for Flow Control after the First Frame and after every block of BS Consecutive Frames (BS = 0 meaning no block limit), separate Consecutive Frames by at least STmin (milliseconds and the 100-900 us encodings, evaluated at the 1 ms tick granularity), abort with `TRANSPORT_BUFFER_OVERFLOW` on Flow Control status OVFLW, and abort with `TRANSPORT_PROTOCOL_FAULT` after more than the configured maximum of consecutive Flow Control Wait frames. | High |
| SW-FR-TP-009 | Completed reassembled messages shall be delivered through a caller-registered callback as a borrowed pointer, and frames to transmit through a caller-registered callback that may decline the frame; a declined frame shall be retried on subsequent ticks until the N_As timer expires, and shall never be silently dropped. All callbacks shall be non-blocking and allocation-free. | High |
| SW-FR-TP-010 | Transport faults shall be injected into the configured `core/event` queue as `FAULT_RAISED` events in the FAULT priority class, with a deterministic numeric code `(source_id << 16) \| fault` that is statically resolvable through a name table, so the fault manager can react without any string lookup. | High |

### 14.2 UDS server (core/uds)

| ID | Requirement | Priority |
|---|---|---|
| SW-FR-UDS-001 | The software shall provide a UDS server that processes a reassembled diagnostic request (byte buffer in) and returns the response bytes for segmentation, implementing the ISO 14229-1 services ReadDataByIdentifier (0x22), WriteDataByIdentifier (0x2E) and RoutineControl (0x31). | High |
| SW-FR-UDS-002 | ReadDataByIdentifier shall return `0x62 + DID + data` for a configured DID (exactly one DID per request at this level) and the negative response codes `0x31` for an unknown DID, `0x13` for a malformed request, and `0x22` when the DID maps to a signal whose current value cannot be encoded in the declared DID length. | High |
| SW-FR-UDS-003 | WriteDataByIdentifier shall return `0x6E + DID` after storing exactly the declared number of data bytes, and the negative response codes `0x31` for an unknown DID, `0x13` for a wrong data length, `0x22` when the governor denies the write, and `0x72` when the mapped signal store cannot accept the mirrored value. A denied or failed write shall leave no partial effect. | High |
| SW-FR-UDS-004 | RoutineControl shall support the start (0x01), stop (0x02) and request-results (0x03) sub-functions for configured routines, returning `0x31 + sub-function + routine-id` with the configured response record, and the negative response codes `0x31` for an unknown routine, `0x12` for an unsupported or disallowed sub-function, `0x13` for a malformed request and `0x22` when the governor denies the routine execution. | High |
| SW-FR-UDS-005 | A request whose service identifier is not 0x22, 0x2E or 0x31 shall receive the negative response `0x7F + SID + 0x11` (serviceNotSupported). | Medium |
| SW-FR-UDS-006 | UDS server configuration (the DID and routine tables) shall be loaded from a YAML document validated against `schemas/uds-0.1.0.schema.json` at load time (schema is law), including the rejection of unknown fields, duplicate DIDs or routine ids, byte values outside 0..255, and signal-mapped DIDs whose length exceeds 8 bytes. | High |
| SW-FR-UDS-007 | Every WriteDataByIdentifier and RoutineControl side effect shall pass through the fail-closed governor stub following the pattern of SW-FR-GOV-005/SW-FR-GOV-006: a NULL governor denies every write and routine execution, a denial produces no partial effect, increments a violation counter, and is reported as the negative response code `0x22`. | High |
| SW-FR-UDS-008 | A DID may map to a shared signal through the codec namespace and the shared signal value store: ReadDataByIdentifier shall encode the current signal value little-endian into the response when one exists (falling back to the DID's stored bytes otherwise), and an approved WriteDataByIdentifier shall mirror the written bytes into the mapped signal as a little-endian unsigned integer. | High |

## 15. Bare-Metal Port & Hard Real-Time HAL Requirements (Phase 11, issue #28)

Scope note: this phase ports the Hardware Abstraction Layer to bare-metal ARM
Cortex-M targets (STM32G4 / NXP S32K) with direct register-level hardware
peripheral drivers (bxCAN/FDCAN), linker-enforced zero-allocation, independent
watchdog (IWDG) fail-safe recovery, and bounded sub-50µs real-time latency.

| ID | Requirement | Priority |
|---|---|---|
| SW-FR-BM-001 | The bare-metal linker script (`cancestry_baremetal.ld`) and toolchain stubs shall enforce zero heap allocation by removing standard library allocation functions (`malloc`, `free`, `realloc`, `calloc`, `_sbrk`, `sbrk`) in the `/DISCARD/` section and asserting against them, producing a linker undefined reference error if dynamic allocation is referenced, and providing tripwire stubs that trigger a HardFault or abort if invoked. | High |
| SW-FR-BM-002 | The software shall provide a bare-metal Hardware Abstraction Layer (`platform/cortex_m/hal_stm32.c`) that directly interfaces with CAN/FDCAN hardware peripheral registers and FIFOs on ARM Cortex-M without operating system dependencies or blocking syscalls, integrating with `cancestry_hal_backend_t`. | High |
| SW-FR-BM-003 | The CAN RX interrupt service routine (`hal_stm32_can_rx_isr`) shall be strictly bounded in execution time (O(1)), performing only hardware FIFO drain, timestamp capture, and non-blocking push into the lock-free ISR event queue. No decoding, UDS parsing, or FSM evaluation shall execute in interrupt context. | High |
| SW-FR-BM-004 | Hardware frame timestamps shall be captured at interrupt arrival from a hardware cycle counter (DWT CYCCNT) or high-resolution timer with microsecond resolution, and multi-word rollover tracking shall guarantee strictly monotonic timestamps over extended gateway uptime without rollover glitches. | High |
| SW-FR-BM-005 | The runtime shall integrate an Independent Watchdog (IWDG) timer (`platform/cortex_m/watchdog.c`). If the main execution loop misses its deadline or a hard-fault escalation occurs without a feed, the IWDG shall assert a hardware MCU reset. | High |
| SW-FR-BM-006 | Upon MCU reset, watchdog escalation, or hard-fault queue saturation, hardware GPIO and CAN transceiver pins shall be immediately latched into a safe "0 Torque / Contactor Open" state, and transmission authorization shall be revoked until explicitly authorized by the FSM. A safe-state broadcast frame (ID 0x100) shall be constructed and emitted upon recovery. | High |
| SW-FR-BM-007 | The software shall achieve deterministically bounded latency of less than 50 microseconds from hardware CAN RX interrupt FIFO arrival to FSM event processing into the event queue. | High |
| SW-FR-BM-008 | The bare-metal system shall execute without an external RTOS (`main() -> while(1)`), placing stack, vectors, static rings, and event queues into dedicated SRAM sections (`.cancestry_core`, `.cancestry_rings`, `.cancestry_ram`). | High |

## 16. Phase 12 BMS, HIL, and final safety-case requirements (issue #30)

### 16.1 BMS thermal and torque governor

| ID | Requirement | Priority |
|---|---|---|
| SW-FR-BMS-001 | The BMS governor shall evaluate a complete VCU/BMS snapshot as a pure, stateless, deterministic function with no heap allocation or hidden threshold state. | High |
| SW-FR-BMS-002 | The governor shall convert the BMS `MaxDischargeCurrent` and measured DC bus voltage into a conservative maximum electrical power using fixed-point integer arithmetic. | High |
| SW-FR-BMS-003 | The governor shall never authorize a requested power whose calculated input current exceeds `MaxDischargeCurrent`; current rounding shall be conservative. | High |
| SW-FR-BMS-004 | When a thermal limit derates a VCU request, the governor shall return the derated power/torque value and prevent the original value from being emitted by the UDS/CAN integration. | High |
| SW-FR-BMS-005 | A derating or block caused by a BMS limit shall expose the deterministic `GOVERNOR_INTERVENTION` fault code for recording as a `FAULT_RAISED` event. | High |
| SW-FR-BMS-006 | Missing, malformed, out-of-domain or faulted BMS data shall fail closed to a zero-output block; it shall never fall back to the VCU request. | High |

### 16.2 Hardware-in-the-loop fault injection

| ID | Requirement | Priority |
|---|---|---|
| SW-FR-HIL-001 | The HIL runner shall expose deterministic scenarios for CAN controller Bus-Off, corrupted-frame/CRC rejection, and power brownout. | High |
| SW-FR-HIL-002 | The Bus-Off scenario shall prove the controller fault is observed before the FSM enters `SAFE_STATE`, transmission is revoked, and the bounded automatic recovery protocol is exercised. | High |
| SW-FR-HIL-003 | The corrupted-frame scenario shall prove the hardware CRC boundary rejects the frame before software queue admission and the FSM does not process it. | High |
| SW-FR-HIL-004 | The brownout scenario shall prove the BOR or IWDG hardware safe latch forces zero torque and open contactors before MCU power-down. | High |
| SW-FR-HIL-005 | When target hardware is unavailable, the HIL simulation shall preserve hardware-first ordering, be deterministic, and state its target-validation limitation. | High |
| SW-FR-HIL-006 | HIL evidence shall identify the scenario ids, backend, event order, metrics, reproduction command and unresolved target measurements. | High |

### 16.3 Final software safety case and release evidence

| ID | Requirement | Priority |
|---|---|---|
| SW-FR-SAFETY-001 | The final Safety Manual shall describe CANcestry system architecture, data flow, assumptions, hardware boundary and fail-closed safe behavior for an external safety assessor. | High |
| SW-FR-SAFETY-002 | The Safety Manual shall include an FMEA summary covering malformed CAN/CRC, Bus-Off, brownout/watchdog, BMS derating, queue/resource faults and unauthorized transmission. | High |
| SW-FR-SAFETY-003 | The Safety Manual shall map implemented IWDG/BOR, CAN CRC boundary, zero-allocation execution, SPSC ISR queue, bounded event processing and BMS/UDS governor evidence to ASIL-B-aligned technical safety requirements. | High |
| SW-FR-SAFETY-004 | The v1.0.0-rc.1 traceability report shall identify all in-scope candidate requirements, passing evidence, and every deferred item with a justified v1.1.0 owner. | High |
| SW-FR-SAFETY-005 | The v1.0.0-rc.1 evidence package shall include the Safety Manual, candidate traceability report, HIL report, changelog and version marker, with final 1.0.0 release gating explicitly deferred while QA-EV-01 is open. | High |
