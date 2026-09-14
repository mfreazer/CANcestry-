# CANcestry Safety Governor Specification

Version: 0.2.1 (stub level)

> **Status**: this document currently specifies only the stub-level
> integration that ships with the recipe engine (phase 3). The full governor
> — TX ID permissions, token-bucket rate limits, package enablement and
> SAFE-mode escalation — is specified when it is implemented; this section
> will then grow without changing the interfaces defined here.

## 1. Role

The governor is the single approval point for observable side effects. No
recipe, FSM or package may send a CAN frame or write a shared signal without
an approved request (SyAD: "No package may bypass the safety governor").

The governor is not a normal event subscriber; it observes and approves
side-effect requests synchronously, at the point the effect is requested
(`docs/system/event-ordering.md` section on the governor).

## 2. Request interface (stub level)

A side-effect request is a `cancestry_recipe_governor_request_t`
(`cancestry/recipe/engine.h`): kind (`send_message`/`set_signal`), the
requesting recipe, the causing event, and the resolved effect (interface
name/id plus message name/id and encoded frame, or signal name/id and new
value). All pointers in a request are valid only for the duration of the
callback; a governor that needs the data afterwards must copy it.

The engine calls the configured governor function synchronously before
performing the effect. The decision is approve or deny.

## 3. Fail-closed rules (SW-FR-GOV-006, stub scope)

- No governor configured (NULL function or NULL governor) denies **every**
  side effect. Denial is the default; approval is the exception that must be
  arranged.
- A denied request produces no partial effect: no frame is handed to the
  interface, no signal value changes.
- Denial increments the violation counter (`governor_denials`, visible in
  the engine counters per SW-FR-GOV-005) and is reported as an action error
  to the requesting recipe; with the default `on_error: stop` the recipe
  halts.

## 4. Deliberately out of scope at stub level

- TX ID allowlists (SYS-SF-002 / SW-FR-GOV-001), token-bucket rate limits
  (SYS-SF-003 / SW-FR-GOV-002), disabled-package blocking (SW-FR-GOV-003)
  and SAFE-mode escalation (SYS-SF-004 / SW-FR-GOV-004). The stub replaces
  all of these with a caller-provided decision function so the engine's
  integration surface is final while the policy is not.
- Recipe-local effects (`set_variable`, `log`, `raise_fault`, timer
  requests) are not governed at stub level.
