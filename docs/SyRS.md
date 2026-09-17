# CANcestry System Requirements Specification

| Field | Value |
|---|---|
| Document | CANcestry System Requirements Specification |
| Version | 0.2.1 |
| Status | Draft for approval |
| Owner | System Engineer |
| Approver | Maintainer + QA |
| Last Review | 2026-09-14 |
| Change Log | v0.2.1: Closed QA-v0.2-R02, R03, R04, R05, R06, R07, R08 |

## 1. Purpose

This document defines system-level requirements for CANcestry, an open-source CAN codec, translation, emulation, and gateway platform.

CANcestry shall allow third parties to develop:

- codec maps,
- recipes,
- emulation profiles,
- gateway behaviors,

using declarative packages executed by a deterministic runtime.





## 2. Scope

The system includes:

- embedded firmware runtime,
- host-side simulator,
- package validator,
- package specification,
- logging and replay,
- configuration and diagnostics interface.
- tooling for ingestion of industry-standard DBC files (e.g., via comma.ai `opendbc`).
- 
v0.2.1 targets classic CAN. From Phase 7 (issue #20) the runtime also carries
CAN FD: 64-byte payloads through the codec engine, the HAL and the Linux
SocketCAN backend, with a deterministic fallback for interfaces that do not
negotiate CAN FD.

Full automotive functional safety certification and production road-legal certification are out of scope.

## 3. Definitions

- Codec Map: declarat
- ive mapping between CAN frames and named signals.
- Recipe: stateless or lightly stateful transformation, filter, or injection rule.
- Emulation Profile: stateful behavior package that emulates a CAN node.
- Package: versioned bundle containing manifest, maps, recipes, FSMs, tests, and metadata.
- State Machine Runtime: deterministic engine executing FSM definitions and instances.
- Safety Governor: enforcement layer for permissions, rate limits, and safe-state behavior.

## 4. Operational Modes

The normative system modes are:

- BOOT
- CONFIG
- LISTEN_ONLY
- ACTIVE
- SAFE
- OFF

GATEWAY and EMULATION are ACTIVE sub-profiles, not independent system modes.

## 5. System Functional Requirements

| ID | Requirement | Priority | Verification |
|---|---|---|---|
| SYS-FR-001 | The system shall load third-party packages containing manifest, codec maps, recipes, FSMs, and tests. | High | Test |
| SYS-FR-002 | The system shall validate packages against schemas, capabilities, and runtime compatibility. | High | Test |
| SYS-FR-003 | The system shall decode CAN frames into named signals using codec maps. | High | Test |
| SYS-FR-004 | The system shall encode named signals into CAN frames using codec maps. | High | Test |
| SYS-FR-005 | The system shall execute recipes triggered by CAN events, signal changes, timers, faults, and mode changes. | High | Test |
| SYS-FR-006 | The system shall execute one or more FSM instances concurrently. | High | Test |
| SYS-FR-007 | The system shall support emulation profiles using periodic behavior, event responses, timers, variables, and fault behavior. | High | Test |
| SYS-FR-008 | The system shall support at least one physical CAN interface and expose logical interfaces for future two-channel gateway use. | High | Test |
| SYS-FR-009 | The system shall filter CAN traffic by interface, CAN ID, message name, direction, and package capability. | High | Test |
| SYS-FR-010 | The system shall maintain a shared signal namespace derived from loaded codec maps. | High | Test |
| SYS-FR-011 | The system shall log raw CAN frames, decoded signals, FSM transitions, faults, and package lifecycle events. | Medium | Test |
| SYS-FR-012 | The system shall provide a host interface for status, package management, mode control, log retrieval, and diagnostics. | High | Demonstration |
| SYS-FR-013 | The system shall provide a host-side simulator capable of executing packages against virtual CAN inputs. | High | Test |
| SYS-FR-014 | The system shall provide deterministic behavior for the same package set, configuration, and event sequence. | High | Test |
| SYS-FR-015 | The system shall boot into a non-transmitting safe state unless explicitly enabled by user configuration. | High | Test |
| SYS-FR-016 | The system shall detect and handle invalid packages, runtime faults, CAN bus-off, resource exhaustion, governor violations, and storage failure. | High | Test |
| SYS-FR-017 | The system shall support package semantic versioning and compatibility checks. | Medium | Inspection |
| SYS-FR-018 | The system shall allow third parties to develop packages without modifying firmware source code. | High | Demonstration |
| SYS-FR-019 | The system shall expose runtime trace information for state transitions, recipe triggers, emitted messages, and faults. | Medium | Test |
| SYS-FR-020 | The system shall support replay of logged CAN traffic in simulation and testing. | Medium | Test |

## 6. System Interface Requirements

| ID | Requirement | Priority | Verification |
|---|---|---|---|
| SYS-IR-001 | The system shall support classic CAN communication up to at least 1 Mbps on supported hardware. | High | Test |
| SYS-IR-002 | The system shall expose named logical CAN interfaces such as can0 and can1. | High | Test |
| SYS-IR-003 | The system shall provide a host interface over USB, serial, local network, or filesystem exchange. | High | Demonstration |
| SYS-IR-004 | The system shall define stable file formats for manifest, codec map, recipe, FSM, and tests. | High | Inspection |
| SYS-IR-005 | The system shall provide a monotonic time source for timers and event timestamps. | High | Test |
| SYS-IR-006 | The system shall support persistent storage for configuration, packages, logs, and faults. | Medium | Demonstration |

## 7. Non-Functional Requirements

| ID | Requirement | Target | Verification |
|---|---|---|---|
| SYS-NF-001 | Runtime determinism | Same input sequence produces same output sequence | Test |
| SYS-NF-002 | Bounded resource usage | Enforced queue, timer, variable, and instance limits | Test |
| SYS-NF-003 | Latency measurement | Event-to-action latency shall be measurable | Test |
| SYS-NF-004 | CAN bus-off recovery | CAN controller may auto-recover, but ACTIVE TX shall not auto-resume by default | Test |
| SYS-NF-005 | Observability | Counters for frames, events, faults, drops, and governor violations | Test |
| SYS-NF-006 | Maintainability | Runtime shall be modular and testable without hardware | Inspection |
| SYS-NF-007 | Portability | Core runtime shall run in firmware and host simulation | Inspection |
| SYS-NF-008 | Testability | Every high-priority requirement shall have at least one verification artifact | Inspection |

## 8. Safety Requirements

| ID | Requirement | Priority | Verification |
|---|---|---|---|
| SYS-SF-001 | The system shall not transmit CAN frames until explicitly enabled. | High | Test |
| SYS-SF-002 | The system shall prevent packages from transmitting CAN IDs or signals not explicitly declared in capabilities. | High | Test |
| SYS-SF-003 | The system shall enforce package-level and ID-level transmission rate limits. | High | Test |
| SYS-SF-004 | Critical faults shall immediately place the affected runtime domain into SAFE mode or controlled-reset into SAFE. No critical fault shall result in continued ACTIVE transmission. | High | Test |
| SYS-SF-005 | The system shall use a watchdog or equivalent mechanism to detect runtime lockups. | High | Test |
| SYS-SF-006 | The system shall reject invalid packages before execution. | High | Test |
| SYS-SF-007 | The system shall monitor CAN error counters and report abnormal bus conditions. | High | Test |
| SYS-SF-008 | The v0.1 package system shall not require arbitrary user code execution for codec maps, recipes, or FSMs. | High | Inspection |

## 9. Security Requirements

| ID | Requirement | Priority | Verification |
|---|---|---|---|
| SYS-SEC-001 | The system shall verify package integrity using cryptographic hash and optional signature. | Medium | Test |
| SYS-SEC-002 | The system shall provide a mechanism to disable debug interfaces in production builds. | Medium | Inspection |
| SYS-SEC-003 | The system shall store logs locally by default, allow local clearing, require no cloud connectivity, and make export explicit. | Low | Demonstration |

## 10. Assumptions

- The user has legal authority to test, modify, or emulate the target system.
- The system is not certified for safety-critical road use by default.
- Third-party packages must be validated by the deployer.
- Third-party DBC data (e.g., from `opendbc`) is used as a starting point for codec maps but remains strictly subject to CANcestry's schema validation, capability enforcement, and safety governor.


## 11. Out of Scope

- CAN FD support in v0.2.1 (delivered in Phase 7, issue #20: see
  SW-FR-CANFD-001..006 in `docs/software/SwRS.md`).
- CAN FD on the declarative egress path: recipe and FSM `send_message` build
  classic 8-byte frames and refuse a wider message instead of truncating it
  (SW-FR-CANFD-006); CAN FD transmit goes through the HAL.
- CAN XL.
- Full UDS diagnostic stack.
- Automotive functional safety certification.
- Cloud package registry.
- Production gateway firewall certification.
