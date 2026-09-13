# CANcestry FSM Specification

Version: 0.2.1

## 1. Purpose

The FSM specification defines state machines and instances for high-level emulation.

## 2. Definitions and Instances

A state machine definition is a template.

An instance is a runnable object with:

- unique ID,
- machine reference,
- enabled state,
- bindings,
- initial variables,
- subscriptions.

## 3. Instance Lifecycle States

Instances support:

- DISABLED
- READY
- RUNNING
- SUSPENDED
- FAULT

## 4. States

Each state may define:

- entry actions,
- exit actions,
- transitions.

The initial state is required.

## 5. Events

FSM transitions may subscribe to:

- can_rx
- signal_changed
- timer_expired
- state_entered
- state_exited
- fault_raised
- power_mode_changed

## 6. Guards

Guards are expressions.

Example:

    guard: "sig.IgnitionState == 2"

## 7. Actions

FSM actions may use:

- send_message
- set_signal
- set_variable
- start_timer
- stop_timer
- reset_timer
- log
- raise_fault
- transition

## 8. Transition Precedence

For each event:

1. At most one event-driven transition is selected.
2. The selected transition completes.
3. transition actions schedule deferred transitions.
4. Deferred transitions run before the next external event.
5. Deferred transitions count toward transition chain depth.
6. If multiple transition actions are requested, the first wins.

## 9. Timers

Timers support:

- one-shot,
- periodic,
- auto_start,
- stop,
- reset.

Periodic timers use scheduled deadlines and emit one event with missed_count when ticks are missed.

## 10. Variables

Variables are scoped per instance.

Supported types:

- boolean
- integer
- float

## 11. Queues

Per-instance incoming event queue default depth:

    64

Overflow policy:

- drop-newest for non-fault events,
- never drop fault events.
