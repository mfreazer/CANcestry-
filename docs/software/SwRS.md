# CANcestry Software Requirements Specification

| Field | Value |
|---|---|
| Document | CANcestry Software Requirements Specification |
| Version | 0.2.1 |
| Status | Draft for approval |
| Owner | System Engineer |
| Approver | Maintainer + QA |
| Last Review | 2026-09-14 |

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
| SW-FR-EVENT-006 | The software shall process events according to the normative event-ordering specification. | High |

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
| SW-FR-FSM-020 | Per-FSM incoming queue overflow shall drop-newest for non-fault events and never drop fault events. | High |
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
