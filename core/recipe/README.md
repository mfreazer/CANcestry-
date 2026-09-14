# CANcestry recipe engine

Portable recipe engine implementing `docs/packages/recipe-spec.md` (v0.2.1)
against `schemas/recipe-0.2.0.schema.json`, sitting between the event bus
(`core/event`) and the codec engine (`core/codec`): events come in, recipes
match triggers, evaluate conditions and run actions.

## Libraries

| Target | Contents | Allocates? |
|---|---|---|
| `cancestry_recipe` | types, engine (trigger matching, evaluator, actions, governor integration) | never |
| `cancestry_recipe_loader` | YAML subset parser + schema validation | load time only |

The split mirrors `core/codec`: a loaded `cancestry_recipe_set_t` is one heap
block owning all recipes, actions and strings; the runtime library only reads
borrowed pointers into that block, so `cancestry_recipe_engine_process_event`
is allocation-free (verified by `ci/check_no_alloc.py` on the built archive
in CI). Loading may allocate; execution may not (SYS-NF-002).

## Execution model

- **Dispatch order** is fixed and total: recipe set order (i.e. package load
  order, then recipe file order, then recipe definition order), and within a
  recipe, action definition order (SYS-NF-001). Recipe ordinals are 1-based
  and continue across sets: the engine's ordinal counter is engine-wide, so
  a recipe loaded into a running engine keeps a stable, unique ordinal.
- **Actions run sequentially.** An action that fails (encoding error, denied
  by the governor, full variable table, ...) increments `action_errors` and,
  with the default `on_error: stop`, halts the rest of that recipe
  (`recipes_halted`). `on_error: continue` records the error and proceeds.
- **Disabled recipes** (`enabled: false`) are skipped entirely.
- **Counters** (`cancestry_recipe_engine_counters_t`) expose every
  interesting quantity; the governor violation counter required by
  SW-FR-GOV-005 is `governor_denials`.

## Trigger matching

Schema-legal trigger events: `can_rx`, `signal_changed`, `timer_expired`,
`timeout`, `fault_raised`, `power_mode_changed`. Conditional trigger fields
follow the schema (`signal_changed` → `signal`, `timer_expired` → `timer`,
`timeout` → `timeout_ms`); unknown event names are rejected at load time.

Fail-closed rules:

- A trigger with a filter that does not apply to the event (e.g. a
  `message:` filter on a `signal_changed` event) simply does not match.
- A trigger naming an interface, message, signal or timer that cannot be
  resolved through the codec namespace or interface table never matches, and
  each failed resolution is counted in `trigger_unresolved` (per trigger
  evaluation, i.e. per relevant event).
- `timeout` triggers **never match** in this phase: the stub engine has no
  runtime clock, so it cannot decide that a timeout elapsed. Full timeout
  watches (SW-FR-RECIPE-004) are out of scope for issue #6; refusing to
  match is the safe direction.
- `signal_changed` triggers match by **signal id** resolved from the codec
  namespace, so the triggering event's payload id decides; a recipe whose
  signal name does not resolve fails closed as above.

## Expression evaluator

Implements the safe subset of `docs/system/expression-language.md`: integer
and float literals, `sig.NAME`, `var.NAME` and `evt.FIELD` references,
parentheses, unary minus, `* / %`, `+ -`, comparisons (`== != < <= > >=`) and
`not/and/or`. No function calls, no assignment, no dynamic code: recipes stay
declarative (AI policy: no arbitrary code execution). The evaluator is a
recursive-descent parser over the expression text with a bounded stack, so it
allocates nothing and cannot run away.

Interpretations (normative for this implementation):

- **No short-circuit**: `and`/`or` always evaluate both sides. The spec does
  not mandate short-circuiting; always evaluating keeps the fault model
  uniform (a fault on the right side is reported even when the left side
  already decides the result) and matches "same event sequence → same action
  sequence" trivially. Cost is bounded by expression length.
- `and`/`or`/`not` require boolean operands; `sig.NAME` reads the shared
  signal store (or, for the triggering `signal_changed` event, the new value
  from the event payload); `var.NAME` reads the recipe's private variable
  table; undefined names are faults, not `NULL`s.
- Integer arithmetic follows the codec fault model: overflow, division or
  modulo by zero, `INT64_MIN / -1`, NaN/Infinity results and type mismatches
  are expression faults. UINT values above `INT64_MAX` widen to REAL.
- A failed condition is a fault: the recipe does not run
  (`recipes_skipped_conditions`, `condition_errors`), never a silent `false`.

The evaluator lives behind one internal entry point, so the full expression
language can replace the subset later without touching action execution.

## Values and coercion

Operand literals keep YAML kinds: integers become `INT` (literals beyond
int64 degrade to `REAL`), floats become `REAL`, booleans become `BOOL`, and
strings are expressions preserved verbatim. Quoted scalars are always
strings, so `value: "0x10"` is the *expression* `0x10`, not the number 16.

`send_message` values are coerced to what the target signal's encode path
accepts (see `core/codec/README.md`): the engine converts integer-typed
operands to `UINT`/`INT` and rounds REAL operands to the nearest integer,
ties away from zero, before encoding — the codec's unscaled integer paths
only accept exact integer values, and recipe-authored values (e.g.
`sig.VehicleSpeed * 0.5`) are frequently REAL. A value that cannot be
represented for the signal's type fails the action (fail-closed), it is
never silently truncated. Rounding matches the codec's own scaled-encode
rounding, so encode/decode stay inverses wherever both paths exist.

## Governor integration (stub level)

Every side effect (`send_message`, `set_signal`) is a
`cancestry_recipe_governor_request_t` passed to the configured governor
callback before anything happens. The request carries the resolved interface
name/id, message name/id and encoded frame, or signal name/id and value, plus
the requesting recipe and causing event. **All request pointers are valid
only during the callback**; a governor that needs the data afterwards must
copy it.

- No governor function, or a NULL governor pointer, means every side effect
  is **denied**: the stub fails closed (SW-FR-GOV-006). Denial increments
  `governor_denials` (the SW-FR-GOV-005 violation counter), is an action
  error, and with the default `on_error: stop` halts the recipe.
- The stub is a plain function pointer + user data (see `engine.h`); the
  real governor (TX ID permissions, token buckets, SAFE mode) plugs in later
  without engine changes. Real CAN TX obviously does not happen here.
- `set_variable`, `log`, `raise_fault`, timer requests and signal store
  updates within the engine are recipe-local or trace-only and are not
  governed at stub level; the real governor's scope is a later phase.

`raise_fault` queues a FAULT-class event (cause-linked to the triggering
event, `source_id` = recipe ordinal, `fault_code` = FNV-1a-32 of the code
string, 0 mapped to 1) and reports to the trace sink; `set_signal` updates
the shared signal store and emits a `signal_changed` generated event only
when the value actually changed, so recipes cannot create feedback loops
with themselves via no-op writes (a re-set that does not change the value
still counts in `signals_set` but emits nothing).

## Interface resolution

`interface:` in triggers and `send_message` resolves in spec order
(`docs/packages/package-spec.md` section 5): a physical interface name
first, then package-level alias bindings, then failure (fail closed).
Message names resolve through the shared codec namespace; canonical
`map.name` wins, and a short name defined by two registered maps is an
`ERR_AMBIGUOUS` load-time-visible condition at runtime (the trigger or
action fails closed and is counted).

## Transition actions are rejected

The schema allows the key; the validator does not (SW-FR-RECIPE-006). The
loader rejects `transition` with a dedicated error citing the requirement —
recipes are transformations, FSMs are a separate later component. The check
runs before action-body shape validation so the dedicated message is always
the one reported.

## Bounds

Recipe/signal/timer/interface/variable names ≤ 64 bytes, descriptions ≤ 512,
expressions ≤ 256, log messages ≤ 256 (`CANCESTRY_RECIPE_*_MAX`). The signal
store, variable table, timer table and trace sink are bounded, caller-owned
arrays; filling one is an action error, never an allocation (SYS-NF-002).

## YAML subset

The loader parses exactly the subset documented in `core/codec/README.md`
(block mappings/sequences, comments, quoted/plain scalars, decimal/hex/octal
integers, floats, booleans, no flow style, no tabs in indentation), then
enforces every constraint of `schemas/recipe-0.2.0.schema.json` in C
(required fields, `additionalProperties: false`, the trigger `allOf`
conditionals, non-empty action lists, ...) with line/column errors. The
parser is a consolidation candidate: it currently mirrors the codec loader's
parser; sharing one implementation is a mechanical follow-up that does not
change any accepted document.

## Requirement mapping

| Requirement | Where | Tests |
|---|---|---|
| SW-FR-RECIPE-001 (event-triggered recipes) | engine dispatch | `RECIPE-EXEC-001..007` |
| SW-FR-RECIPE-002 (conditions) | evaluator | `RECIPE-COND-001/002` |
| SW-FR-RECIPE-003 (action set) | actions | `RECIPE-ACTION-001..003` |
| SW-FR-RECIPE-005 (directional filtering) | trigger filters | `RECIPE-FILTER-001a..c` |
| SW-FR-RECIPE-006 (reject transition) | loader | `RECIPE-NO-TRANSITION-001` |
| SW-FR-RECIPE-007 (interface resolution) | resolver | `RECIPE-IFACE-BINDING-001` |
| SW-FR-GOV-005 (violation counters, stub) | governor integration | `RECIPE-GOV-DENY-001` |
| SW-FR-GOV-006 (fail closed, stub) | governor integration | `RECIPE-GOV-FAIL-CLOSED-001` |
| SYS-NF-001 (determinism) | whole engine | `RECIPE-DETERMINISM-001` |
| SYS-NF-002 (bounded resources, no runtime alloc) | engine + scan | `RECIPE-NO-ALLOC-001` |
