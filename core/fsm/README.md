# CANcestry FSM runtime

Portable, allocation-free, deterministic FSM runtime implementing
`docs/packages/fsm-spec.md` (v0.2.1) against `schemas/fsm-0.2.0.schema.json`,
plus the mandatory Phase 4 conformance suite. Built for
[issue #11](https://github.com/mfreazer/CANcestry-/issues/11).

## Libraries

| Target | Contents | Allocates? |
|---|---|---|
| `cancestry_fsm` | engine (instance manager, dispatcher, guard evaluator, action executor, timer manager, variable store, trace logger) + the safe expression evaluator | never |
| `cancestry_fsm_loader` | YAML subset parser, schema validation, definition compiler | load time only |

The split mirrors `core/recipe` and `core/codec`: `ci/check_no_alloc.py` runs on
`libcancestry_fsm.a` in CTest (`cancestry_fsm_no_malloc_symbols`), and
`tests/unit/core/fsm/test_no_alloc.c` arms an allocator tripwire around every
engine entry point. Both are green; the tripwire path is skipped under
AddressSanitizer (where overriding `malloc` would fight the sanitizer runtime)
and the archive scan covers that configuration.

## What "no heap in the runtime" means here

`cancestry_fsm_engine_process_event()` and `cancestry_fsm_engine_tick()` allocate
nothing, and neither does any other engine call: every table the runtime mutates
is caller-owned storage handed in through `cancestry_fsm_engine_config_t`
(instances, per-instance incoming queues, variable tables, timer tables, the
trace ring). A loaded definition set is read-only after loading, so the runtime
never copies or frees anything.

Instance creation does not allocate either — the caller provides
`cancestry_fsm_instance_storage_t` per instance. If a table is too small for the
machine that uses it, `cancestry_fsm_engine_init()` fails and leaves the engine
zeroed (safely unusable) rather than growing anything.

## Files and layout

```
core/fsm/include/cancestry/fsm/types.h    definitions, actions, transitions, set
core/fsm/include/cancestry/fsm/loader.h   load / validate / compile
core/fsm/include/cancestry/fsm/engine.h   engine, instance manager, governor, sink, trace
core/fsm/src/engine.c                     runtime
core/fsm/src/expression.c                 safe expression evaluator
core/fsm/src/loader.c                     YAML subset parser + schema validation
core/fsm/src/fsm_expression.h             evaluator interface (private to this module)
```

The evaluator is deliberately **not** a public header. The only expressions an
FSM may contain are guards and value operands, and both are evaluated by the
engine, so the conformance suite exercises the shipping path instead of a parallel
API. The loader includes the private header to grammar-check expressions at load
time (SwAD.md section 10 lists invalid expressions as definition errors).

## Execution model

1. `cancestry_fsm_engine_process_event()` pushes the event into the incoming
   queue of every **running** instance that subscribes to it, then each instance
   drains its queue in declaration order (`docs/system/event-ordering.md`
   section 7: FSM instances run in package load order, then instance declaration
   order).
2. Each popped event selects **at most one** transition: the first declaration of
   the current state whose event selector, filters and guard match
   (SW-FR-FSM-010..012).
3. The selected transition runs exit actions, transition actions, then entry
   actions (SW-FR-FSM-014), and a self-transition runs both sets (SW-FR-FSM-015).
4. A `transition` action requests a **deferred** transition, which runs after the
   current transition completed and before the next queued event is taken
   (fsm-spec.md section 8 rules 3-5, SW-FR-FSM-054).
5. Deferred transitions count toward the chain depth; the limit is 4 including the
   event-driven transition. The fifth step is refused, counted
   (`chain_limit_exceeded`), warned, and the instance is suspended
   (SW-FR-FSM-016, `mode-fault-state-machine.md` section 5).
6. `cancestry_fsm_engine_tick()` is one 1 ms tick (SW-FR-FSM-030): it advances
   the time base, evaluates timers, queues one `timer_expired` event per expired
   timer, and drains.

**The clock is read only in `tick()`.** With an injected `cancestry_clock_t` the
engine samples it at each tick; without one, the engine's own counter advances by
exactly 1 ms per tick, which is the normative tick period. Event processing never
reads a clock: generated events inherit the causing event's timestamp
(`event-ordering.md` section 8). That makes "same event sequence + same time base
→ identical result" (SW-FR-FSM-046) a structural property rather than a hope, and
it is what `tests/conformance/fsm/test_event_order.c` pins with a golden trace
comparison.

## Timers

* One-shot: armed by `start_timer` (or `auto_start`), fires once, then stops.
* Periodic: advances a **scheduled deadline** by whole periods instead of
  re-basing on "now", so it cannot drift. When a clock jump skips several periods,
  exactly one `timer_expired` event is emitted with `missed_count` set to the
  number of skipped periods (`fsm-spec.md` section 9; the "Timer overrun → emit
  one event with missed_count" row of `mode-fault-state-machine.md` section 5).
* `stop_timer` disarms; `reset_timer` re-arms the countdown and keeps the running
  state, so resetting a stopped timer does not start it.
* Expiry order is deadline, then instance (declaration order), then timer
  declaration order (`SwAD.md` section 8). There is no name or hash ordering.
* A suspended instance is not evaluated, but time keeps passing for it: on resume
  the next tick reports the skipped periods through `missed_count`, so nothing is
  silently lost.

## Guards, expressions and failures

Implemented per `docs/system/expression-language.md`: recursive descent over the
documented grammar, `sig.` / `var.` / `evt.` namespaces only, the five built-in
functions only, bounded depth (32 recursive levels), no allocation, no code
execution (SW-FR-FSM-035..037).

Fault model, and the reaction to it:

| Situation | Reaction |
|---|---|
| guard or operand expression faults (overflow, `/ 0`, `% 0`, NaN/Inf, invalid coercion, undefined identifier, unauthorized `sig.` read) | instance suspended, `guard_errors` / `action_errors` counted, reason recorded in the trace |
| other action failures (governor denial, capability denial, encode failure, unknown name, missing sink) | recorded, counted, execution continues with the next action (SW-FR-FSM-024) |
| chain limit exceeded | pending deferred request dropped, instance suspended |
| `raise_fault` with `severity: critical` | instance moved to FAULT (only `reset()` recovers it) |
| action budget or per-activation event budget exhausted | activation stops, remainder stays queued, `budget_exhausted` counted |

Interpretations the specs leave open, chosen deliberately (each is asserted by a
test so a future change is visible):

* **A failed guard is not a false guard.** `mode-fault-state-machine.md` rates an
  expression evaluation error as "Suspend FSM or recipe", so a fault suspends the
  instance instead of silently taking the "otherwise" branch, which could disable
  a safety transition. `SW-FR-FSM-024` ("record the failure and continue safely")
  is satisfied by the other action failures, which are recorded and skipped.
* **Float `%` is a fault.** The spec defines integer remainder and float
  arithmetic but no float remainder; refusing matches `core/recipe`.
* **`round()` breaks ties away from zero**, matching the codec's and recipe
  engine's rounding rule, so one rounding rule exists across the runtime.
* **Assigning a real to an integer variable rounds** (same rule), and only that;
  a number into a `boolean` variable is refused. Lossy or type-crossing writes are
  never guessed.
* **Validation mode.** At load time, names are checked for namespace and shape but
  not resolved, and a literal's *value* is not type-checked, so `10 / 0` loads and
  faults at run time exactly like `var.x / 0` would. Syntax, the `evt.` field
  vocabulary, and built-in arity are checked at load.
* **No hexadecimal inside expressions.** `0x10` is a YAML integer literal in the
  loader; the expression grammar (spec section 2) defines `NUMBER` as decimal, and
  the evaluator does not invent more.

## Governor and capabilities

`send_message` and `set_signal` are the only governed actions, matching
`docs/system/governor.md` (stub level). Order of checks, all fail-closed:

1. **Capabilities** (`cancestry_fsm_capabilities_t`, from the package manifest):
   the interface must be declared with `tx: true`, and the message's CAN id —
   resolved through the codec namespace, never from the FSM file — must be in the
   declared `tx_ids` list. `set_signal` requires the signal in
   `capabilities.signals.write`. No capability table at all denies everything.
2. **Governor**: a caller-provided function pointer (NULL denies). The request
   carries the resolved interface id/name, message name, CAN id and encoded frame,
   or signal name, id and value, plus the instance and causing event. Pointers are
   valid only for the duration of the callback.
3. **Sink**: after approval the effect is handed to the sink. With no sink the
   action fails as unsupported rather than pretending delivery
   (SW-FR-FSM-025: no path to hardware of the engine's own making).

Denials are counted separately (`capability_denials`, `governor_denials` — the
SW-FR-GOV-005 violation counter), a denial performs no partial effect, and
`raise_fault` publishes a FAULT-class event.

`evt.` reads and `sig.` reads are capability-gated too: a signal outside the read
allowlist is an "unauthorized signal access" expression fault, per spec section 5.

**RX permission is not enforced here.** The package manifest declares `rx` per
interface, but receiving is not an FSM action (SW-FR-FSM-023/-039 govern actions),
and frame delivery to subscribers belongs to the runtime dispatcher and the
package layer. The FSM filters `can_rx` by interface and id, and does not decide
whether a package may listen; flagging this because it is a deliberate non-goal,
not an oversight.

## Instance lifecycle (fsm-spec.md section 3)

```
DISABLED --enable--> READY --start--> RUNNING --suspend--> SUSPENDED
   ^                                    |    \__resume--> RUNNING
   |                                    v
   +-----------------reset<---------- FAULT <--- any critical fault
```

Only RUNNING processes events and runs timers. `start()` enters the initial state
(so entry actions and `auto_start` timers run then, not at load), `suspend()` is a
pause that runs no exit actions, `reset()` re-initialises from the declaration and
returns to READY, and `disable()` clears the queue and stops the timers. An
illegal request returns `CANCESTRY_FSM_ERR_STATE` and changes nothing.

Events that arrive while an instance is not RUNNING are **not queued**: they are
skipped and visible in the engine counters (`events_ignored` when no instance took
one). A pause that silently buffers the world would let a resumed instance act on
stale inputs.

## Queues and overflow

Each instance owns a bounded incoming queue (default depth 64,
`CANCESTRY_FSM_INCOMING_QUEUE_DEFAULT_CAPACITY`; SW-FR-FSM-019). It is
`cancestry_event_queue_t` from `core/event`, so the policy is the shared one:
drop-newest for non-fault events, and a fault admitted by evicting the newest
non-fault event (SW-FR-FSM-020, `event-ordering.md` sections 9 and 11.1). Every
drop is counted on both the queue and the instance.

> **Flagged for the maintainer.** `docs/system/event-ordering.md` section 11.2
> describes fault-on-fault saturation as *evicting the oldest queued fault* and
> admitting the new one. The event core that shipped in Phase 2 documents and tests
> the opposite for that case: the incoming fault is dropped (`ERR_FULL_FAULT`,
> `core/event/README.md` policy table). `docs/qa`'s "Queue Policy Instantiation"
> note (section 12) says one primitive implements one policy. The FSM runtime uses
> the shipped primitive unmodified, so the FSM inherits its behaviour, and
> `tests/conformance/fsm/test_queue_overflow.c` asserts what the runtime actually
> guarantees (a fault is never dropped *for admission* reasons while a non-fault
> victim exists; saturation stays bounded and counted). Reconciling section 11.2
> with the event core is a spec/queue decision outside issue #11's scope —
> changing it belongs in `core/event`, where the codec, recipe and FSM layers all
> pick it up at once.

Recursion and runaway work are bounded three ways: the chain depth, the per-event
action budget (`max_actions_per_event`, default 128) and the per-activation event
budget (`max_events_per_activation`, default 64) — SW-FR-FSM-021 and -045. A
machine that reacts to its own `state_entered` events therefore throttles instead
of growing the stack or the queue without limit, and a re-entrant `sink` callback
cannot nest a second activation (`processing` refuses it).

## Subscriptions

`subscriptions` is a delivery gate applied before transition matching:

* a declared list filters that class (`can_rx` entries by interface and either id
  or message; `signals` by signal name);
* `timers: false`, `faults: false`, `power_mode: false` or an empty list means
  that class is never delivered, and the refusal is counted as `events_suppressed`;
* a class that is **not declared at all** is delivered when the machine's
  transitions select it, so a minimal file works without a subscription block.

An FSM's own generated events (a `set_signal` write, a `raise_fault`) are
published to the global queue and are additionally queued for the instance that
produced them **only** when that instance declared the matching subscription —
`signals` naming the signal, or `faults: true`. Self-reaction is therefore an
explicit author decision rather than an accident of ordering (generated events are
"not processed recursively", `event-ordering.md` section 8).

## Trace, counters and golden tests

Transitions are recorded unconditionally (SW-FR-FSM-048); events are recorded when
`record_events` is set (SW-FR-FSM-049). The trace is a bounded caller-owned ring;
records that fall out are counted, never rewritten. `cancestry_fsm_engine_trace_render()`
renders the retained records as deterministic text, which is the golden-output hook
(SW-FR-FSM-053) — `test_event_order.c` compares two independent runs byte for
byte. Counters are per instance (`cancestry_fsm_instance_counters_t`) and cover
events, transitions, deferred steps, chain refusals, guard outcomes, action
outcomes, both denial counters, sends, writes, timer starts/stops/resets/expiries,
missed ticks and budget exhaustion (SW-FR-FSM-050);
`cancestry_fsm_engine_totals()` sums them on demand, so no quantity is stored
twice and can disagree with itself.

Test and simulation hooks (SW-FR-FSM-051, -052): `cancestry_fsm_engine_inject_event`
(one instance only), `cancestry_fsm_instance_get_variable`,
`cancestry_fsm_instance_get_timer`, the counters, the trace, and a time base the
caller owns.

## Determinism rules the code follows

* instance order = set order, then instance declaration order;
* transition order = declaration order; the first match wins and selection stops;
* action order = declaration order; the first `transition` action wins
  (SW-FR-FSM-055 — later ones are ignored with a warning and counted);
* queue pop order = timestamp, then priority class, then sequence;
* no hash iteration, no `rand`, no wall clock, no time read outside `tick()`;
* `raise_fault` codes hash with the same FNV-1a function `core/recipe` uses, so one
  code string maps to one numeric code runtime-wide.

## Bounds

Names ≤ 64 bytes, descriptions ≤ 512, expressions ≤ 256, log messages ≤ 256,
fault codes ≤ 64 (`CANCESTRY_FSM_*_MAX`); queue, variable, timer and trace tables
are caller-owned with fixed capacity. Exceeding a bound is an error, never a
truncation or an allocation.

## Loader: YAML subset, schema and compile

The loader accepts exactly the subset the codec and recipe loaders document
(`core/codec/README.md`): block mappings and block sequences, `#` comments,
quoted and plain scalars, decimal / `0x` / `0o` / `0b` integers and decimal reals,
two-space indentation, and nothing else. Flow collections, anchors, aliases, tags,
block scalars, tabs in indentation and duplicate keys are rejected with a
line/column diagnostic — the loader never guesses at input.

`schemas/fsm-0.2.0.schema.json` is enforced field by field in C: required fields,
types, enums, the numeric minima, `additionalProperties: false` at every level,
the `fsm_action` oneOf (exactly one action key), and the conditional transition
fields (`signal_changed` → `signal`, `timer_expired` → `timer`).

Then it compiles (SW-FR-FSM-002): state and machine references become indices,
`initial` is resolved, `set_variable` / timer names are checked against the
declarations, and every guard and value operand is grammar-checked. An unresolved
reference is a load error, so the runtime resolves no names to find states.

Two error classes, per SW-FR-FSM-003: an invalid definition is
`CANCESTRY_FSM_ERR_PARSE`, and a duplicate machine name or instance id is
`CANCESTRY_FSM_ERR_CONFLICT`. Instance
ids must be unique across the whole array of sets the engine is given, because the
id is the runtime handle.

Memory: one header block plus the arena that owns the parse tree and every
compiled definition, all released by `cancestry_fsm_set_free()`. `core/recipe`
packs a loaded set into exactly one block; the FSM loader keeps the arena instead
of re-measuring and copying, because every definition string is already a
NUL-terminated scalar in it. The observable contract is the same — one call frees
everything, the input text is not referenced afterwards, and nothing at run time
allocates. Consolidating all three parsers into one shared subset reader remains
the recorded follow-up in `core/recipe/README.md`.

`strtod` follows the C locale's decimal separator; hosts and targets shall run with
`LC_NUMERIC=C`, as the codec and recipe READMEs already require.

## Conformance suite

`tests/conformance/fsm/` holds the seven suites the issue and QA gate require.
`docs/software/SwAD.md` section 11 names them as directories; the deliverable list
in issue #11 names them as one file each, and the mapping is one to one:

| Suite (SwAD directory) | File | Test ids |
|---|---|---|
| `instance_lifecycle/` | `test_instance_lifecycle.c` | `FSM-LIFECYCLE-001..008` |
| `event_order/` | `test_event_order.c` | `FSM-EVENT-ORDER-001..006` |
| `timer_semantics/` | `test_timer_semantics.c` | `FSM-TIMER-001..006` |
| `expression_evaluation/` | `test_expression_evaluation.c` | `EXPR-GRAMMAR/TYPES/FAULTS/NAMESPACE/FUNCTIONS/DEPTH/ACTIONS/NO-CODE-001` |
| `transition_precedence/` | `test_transition_precedence.c` | `FSM-PRECEDENCE-001..007` |
| `capability_enforcement/` | `test_capability_enforcement.c` | `FSM-CAPABILITY-001..010` |
| `queue_overflow/` | `test_queue_overflow.c` | `FSM-QUEUE-001..005` |

`tests/conformance/support/cancestry_fsm_conformance.h` is the shared fixture: it
loads the document under test through the real loader, registers the `demo` codec
map, installs the two-interface capability set (can0: rx+tx with `tx_ids:
[0x100, 0x200]`; can1: rx only), a governor whose modes cover approve, deny,
deny-by-kind, deny-by-CAN-id and rate limiting, a recording sink, an in-memory
signal bus and the trace ring. Every test therefore runs the shipping loader and
engine, and asserts on recorded effects, counters and traces.

Loader/schema behaviour is additionally pinned by `tests/unit/core/fsm/test_loader.c`
(`FSM-LOAD-001..006`).

## Open questions for the maintainer

Documented here rather than silently resolved, per `agents.md` and the issue:

1. **`fsm-0.3.0.schema.json` does not exist.** The issue allows "0.2.0 with `layout`
   extensions if unified". The Phase 4 runtime needs no new file-level fields —
   chain depth, deferred transitions, missed-tick handling and the queue policy are
   runtime behaviour, not declaration syntax — so the loader enforces the shipped
   0.2.0 schema and rejects `schema_version: "0.3.0"` and any `layout` key. If
   v0.3.0 files need fields, the schema and this loader grow together.
2. **Fault-on-fault queue saturation** — see the queue section above; the event
   core and `event-ordering.md` section 11.2 disagree, and this module follows the
   shipped core.
3. **Suspend on expression fault vs. skip the action.** `SW-FR-FSM-024` and
   `mode-fault-state-machine.md` section 5 point in different directions; this
   runtime suspends (see the failure table). A maintainer may prefer
   "skip the action, raise a package fault" as the default with suspend as a
   policy; the engine isolates that decision in the two fault sites.
4. **`state_entered` / `state_exited` delivery.** The schema declares these
   selectors but no fields to scope them, so they are delivered to the owning
   instance's queue and rely on the chain and budget limits for termination. If
   v0.3 wants a "no internal events" notion, an `internal` transition flag in the
   schema is the clean fix (a statechart phase already plans for it).
5. **`evt.` field vocabulary.** The spec fixes the namespace, not the field list.
   This runtime exposes the seven payload shapes of `core/event` read-only, and
   rejects unknown fields at load time. Extending the list is a spec edit, not an
   implementation detail.
6. **Instance lifecycle edges.** `fsm-spec.md` section 3 names the five states but
   not the legal edges; the edge set above (including `SUSPENDED → RUNNING` and
   `FAULT → READY` by `reset()`) is this module's interpretation and is what
   `test_instance_lifecycle.c` pins.
