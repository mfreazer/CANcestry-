# CANcestry Recipe Specification

Version: 0.2.1

## 1. Purpose

A recipe is a stateless or lightly stateful rule that transforms, routes, filters, or injects CAN messages or signals.

## 2. Trigger Field

Recipes shall use:

    trigger:
      event: <event_name>

The field type is not allowed in v0.2.1.

Supported events:

- can_rx
- signal_changed
- timer_expired
- timeout
- fault_raised
- power_mode_changed

## 3. Interface Resolution

Recipe actions may reference:

- physical interface names,
- package-level aliases.

FSM instance aliases are not valid in recipes.

## 4. Allowed Actions

Recipes may use:

- send_message
- set_signal
- set_variable
- start_timer
- stop_timer
- reset_timer
- log
- raise_fault

Recipes shall not use:

- transition

## 5. Action Execution

Actions execute sequentially.

Default:

    on_error: stop

Optional:

    on_error: continue

## 6. Governor Check

All send_message and set_signal actions shall pass through the safety governor.

If denied:

- no side effect occurs,
- a violation counter increments.
