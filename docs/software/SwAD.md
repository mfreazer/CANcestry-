# CANcestry Software Architecture / Design

| Field | Value |
|---|---|
| Document | CANcestry Software Architecture / Design |
| Version | 0.2.1 |
| Status | Draft for approval |
| Owner | System Engineer |
| Approver | Maintainer + QA |
| Last Review | 2026-09-14 |

## 1. Architecture Overview

The portable CANcestry core shall be usable in firmware and host simulation.

Major modules:

- core/event
- core/codec
- core/recipe
- core/fsm
- core/gov
- core/pkg
- core/log
- platform/can
- platform/storage
- tools/cli
- tools/sim

## 2. Repository Layout

    cancestry/
      docs/
      firmware/
      core/
      host/
      packages/
      schemas/
      tests/
      hardware/
      ci/

## 3. Core Modules

### core/event

Responsibilities:

- event types,
- event queues,
- dispatch,
- timestamping,
- overflow counters.

### core/codec

Responsibilities:

- codec map loading,
- signal decode,
- signal encode,
- signal namespace management.

### core/recipe

Responsibilities:

- trigger matching,
- condition evaluation,
- action execution.

### core/fsm

Responsibilities:

- FSM loading,
- instance lifecycle,
- event processing,
- timers,
- variables,
- actions.

### core/gov

Responsibilities:

- permission checks,
- rate limiting,
- fault escalation.

### core/pkg

Responsibilities:

- manifest parsing,
- schema validation,
- package lifecycle.

### core/log

Responsibilities:

- trace records,
- fault logs,
- log export support.

## 4. FSM Runtime Architecture

The FSM runtime contains:

- Definition Loader
- Definition Validator
- Compiler
- Instance Manager
- Event Dispatcher
- Guard/Expression Evaluator
- Action Executor
- Timer Manager
- Variable Store
- Trace Logger

## 5. FSM Data Model

### StateMachineDefinition

Contains:

- name,
- initial state,
- variables,
- timers,
- states.

### State

Contains:

- name,
- entry actions,
- exit actions,
- transitions.

### Transition

Contains:

- event selector,
- guard,
- actions,
- target state.

### FsmInstance

Contains:

- definition reference,
- current state,
- variables,
- timers,
- incoming event queue,
- counters,
- fault state,
- bindings.

## 6. Event Processing Model

High-level loop:

1. Update timers.
2. Dequeue next event according to event ordering.
3. Dispatch event to subscribers.
4. Actions request side effects through the governor.
5. Approved side effects are executed.
6. Trace and counters are updated.

Actions shall not execute side effects before governor approval.

## 7. Transition Semantics

For each event:

1. Select at most one event-driven transition.
2. Execute exit actions.
3. Execute transition actions.
4. Enter target state and execute entry actions.
5. Process deferred action transitions if requested.
6. Enforce transition chain limit.

## 8. Timer Semantics

- Tick period: 1 ms.
- Periodic timers use scheduled deadlines.
- Missed periodic ticks emit one event with missed_count.
- Timer ordering is deterministic by deadline, instance ID, and timer name.

## 9. Expression Evaluation

Expression evaluation shall be:

- compiled at package load time,
- bounded,
- deterministic,
- free of arbitrary code execution.

See:

    docs/system/expression-language.md

## 10. Error Handling

### Definition errors

Detected at load time:

- invalid schema,
- missing fields,
- unknown references,
- invalid expressions.

Action: reject package.

### Runtime errors

Detected during execution:

- expression failure,
- queue overflow,
- action rejection,
- timer exhaustion.

Action: raise package fault or suspend instance.

### Critical errors

Detected by core:

- watchdog timeout,
- governor internal failure,
- storage corruption,
- unrecoverable runtime fault.

Action: SAFE containment or controlled reset into SAFE.

## 11. Testing Strategy

Required test levels:

- unit tests,
- integration tests,
- simulation tests,
- golden tests,
- schema validation tests,
- FSM conformance tests,
- hardware-in-the-loop tests.

FSM conformance suite directories:

    tests/conformance/fsm/instance_lifecycle/
    tests/conformance/fsm/event_order/
    tests/conformance/fsm/timer_semantics/
    tests/conformance/fsm/expression_evaluation/
    tests/conformance/fsm/transition_precedence/
    tests/conformance/fsm/capability_enforcement/
    tests/conformance/fsm/queue_overflow/
