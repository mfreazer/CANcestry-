# CANcestry System Architecture / Design

| Field | Value |
|---|---|
| Document | CANcestry System Architecture / Design |
| Version | 0.2.1 |
| Status | Draft for approval |
| Owner | System Engineer |
| Approver | Maintainer + QA |
| Last Review | 2026-09-14 |

## 1. Architecture Goals

The CANcestry architecture shall:

- support third-party declarative packages,
- enforce safety and capability boundaries,
- remain deterministic,
- separate hardware concerns from behavior,
- support simulation without hardware,
- support future CAN FD and diagnostic extensions.

## 2. Architecture Principles

### Declarative first

Core behavior shall be described declaratively before custom code is introduced.

### Signals first

Packages should operate on named signals, not primarily raw bytes.

### Deterministic runtime

The runtime shall use bounded queues, bounded memory, and deterministic event processing.

### Simulation parity

The same package definitions should run in simulation and on target hardware.

### Safety governor authority

No package may bypass the safety governor.

## 3. System Context

    +------------------+       +----------------------+       +----------------------+
    | Car / test bench |<----->| CANcestry device     |<----->| Replacement equipment|
    +------------------+       |                      |       +----------------------+
                               | - CAN interfaces     |
                               | - Codec engine       |
                               | - Recipe engine      |
                               | - FSM runtime        |
                               | - Safety governor    |
                               +----------+-----------+
                                          |
                                          v
                               +----------------------+
                               | Host tools / CLI /   |
                               | simulator / logs     |
                               +----------------------+

## 4. Logical Architecture

The system shall contain the following components:

- Package Manager
- Codec Engine
- Recipe Engine
- FSM Runtime
- Event Bus
- Safety Governor
- Logger / Trace
- Storage
- Configuration
- CAN Core
- Hardware Abstraction
- Host Tooling

## 5. Component Responsibilities

### Package Manager

- load manifests,
- validate schemas,
- resolve dependencies,
- enforce capabilities,
- enable/disable packages.

### Codec Engine

- decode CAN frames into signals,
- encode signals into CAN frames,
- maintain signal namespace,
- detect codec conflicts.

### Recipe Engine

- subscribe to events,
- evaluate conditions,
- transform signals,
- send governed messages.

### FSM Runtime

- instantiate state machines,
- process events,
- evaluate guards,
- execute actions,
- manage timers and variables.

### Event Bus

- carry events between components,
- maintain event ordering,
- enforce bounded queues.

### Safety Governor

- enforce TX permissions,
- enforce rate limits,
- block unauthorized signal writes,
- trigger SAFE mode.

### Logger / Trace

- record frames,
- record signal changes,
- record FSM transitions,
- record faults.

## 6. Data Flow

A typical runtime flow is:

1. CAN frame received.
2. CAN Core timestamps and queues frame.
3. Codec Engine decodes frame into signals.
4. Event Bus dispatches events.
5. Recipes and FSM instances process events.
6. Actions request side effects.
7. Safety Governor approves or denies actions.
8. Codec Engine encodes approved messages.
9. CAN Core transmits approved frames.
10. Logger records trace.

## 7. Mode and Fault Architecture

System modes and fault reactions are defined in:

    docs/system/mode-fault-state-machine.md

## 8. Event Ordering

Event ordering is defined in:

    docs/system/event-ordering.md

## 9. Governor Architecture

Governor behavior is defined in:

    docs/system/governor.md

## 10. Deployment Architecture

### Embedded target

- firmware runtime,
- packages,
- configuration,
- logs.

### Host simulation

- same portable runtime core,
- virtual CAN interfaces,
- replay input,
- test assertions.

## 11. Key Architectural Decisions

| ID | Decision | Rationale |
|---|---|---|
| AD-001 | Declarative package model | Safety, validation, AI-agent friendliness |
| AD-002 | Signals-first abstraction | Readability and maintainability |
| AD-003 | Flat elementary FSM first | Simpler verification |
| AD-004 | Simulation parity | Enables third-party development without hardware |
| AD-005 | Mandatory safety governor checkpoint | Prevents unsafe bus activity |
