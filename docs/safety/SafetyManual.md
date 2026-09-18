# CANcestry Safety Manual (Phase 9 draft)

| Field | Value |
|---|---|
| Document | CANcestry Safety Manual |
| Version | Phase 9 draft, 0.6.0 work in progress |
| Intended safety integrity | ASIL-B alignment target; this document is not an ISO 26262 safety case or certification |
| Scope | Portable CANcestry runtime (`core/`) and its platform boundary (`platform/`) |
| Owner | CANcestry safety and verification review |
| Last review | 2026-09-18 |

## 1. Purpose and safety claim

CANcestry is a declarative CAN codec, event, recipe and FSM runtime. The Phase
9 objective is to make its safety argument reviewable for a safety-related
automotive gateway at an ASIL-B alignment level. The claim is deliberately
bounded:

> For a configured, validated package, legal CAN input and a caller that
> satisfies the documented API contracts, the portable runtime is deterministic,
bounded, allocation-free on its execution path, and fails closed when a
configuration, capability, arithmetic or transport assumption is violated.

This is an engineering alignment target. It does not claim that a vehicle
item, hardware platform, compiler, operating system or complete gateway has
been safety-certified. A product safety case must add item definition, HARA,
technical safety requirements, hardware metrics, freedom-from-interference
analysis, tool qualification, production-process evidence and a qualified
independent assessment.

## 2. Scope and assumptions

### 2.1 In scope

* Event timestamps, priority and sequence ordering and the bounded min-heap.
* CAN classic and CAN FD frame length and bit packing/unpacking.
* Declarative FSM execution, bounded expression evaluation, action capability
  checks and fault recording.
* Caller-owned storage and zero-allocation runtime paths in `core/` and
  `platform/`.
* Static analysis, formal contracts, symbolic execution and host/conformance
  tests described in section 7.

### 2.2 Assumptions that the integrator must provide

* The package loader validates schemas and retains definitions for the entire
  lifetime of the runtime objects that borrow their strings and arrays.
* The caller serializes access to a queue and supplies storage that remains
  valid and correctly aligned. The runtime is single-threaded in this release.
* A platform clock is monotonic, or its adapter provides the documented
  monotonic timestamp semantics. A weak clock fallback is compile-only and is
  not acceptable for production.
* The target compiler uses the C99 fixed-width integer and IEEE-754 behavior
  documented by the portability requirements, and its warnings are treated as
  errors.
* The HAL validates physical interface state and reports malformed, bus-off,
  unsupported and transport-error conditions; core code never accesses a CAN
  controller directly.
* Package capabilities, TX allowlists, signal write allowlists and governor
  callbacks are configured by the integrator. A missing callback denies the
  action; it does not grant access.

## 3. Safety architecture

The architecture is split into layers with one-way dependencies:

1. **Platform/HAL boundary** translates hardware frames, timestamps and faults
   into caller-owned CANcestry types. It never exposes a hardware pointer to
   the declarative runtime.
2. **Codec** validates the message definition and frame length, then performs
   bounded bit operations. A short frame is dropped atomically; it is never
   partially decoded. Encoding rejects values that cannot be represented and
   never truncates a CAN FD payload.
3. **Event bus** copies valid events into a fixed caller-owned min-heap. The
   total order is timestamp, priority class, sequence. Fault events have
   admission priority; non-fault overflow follows the documented drop-newest
   policy. Drop, overflow and high-water counters remain observable.
4. **Recipe/FSM runtime** processes one instance sequentially. State transitions,
   deferred transitions, action count and generated event work are bounded.
   Every side effect crosses a capability and governor checkpoint.
5. **Trace and diagnostics** record errors and deterministic counters through
   caller-owned sinks. Diagnostics do not change the safety decision or create
   a hidden retry path.

No layer in the runtime path starts a thread, loads code, evaluates a user
program, allocates heap memory or silently repairs an invalid safety input.
The loader is intentionally separate because package parsing may allocate;
loaded runtime objects are not confused with the execution archive.

## 4. Fault handling and safe behavior

### 4.1 Fail-closed

The following conditions deny the operation and preserve a safe state:

* NULL, malformed or out-of-domain API arguments;
* invalid schema, duplicate/conflicting definition or unsupported CAN FD map;
* unknown or unauthorized `sig.*`, `var.*` or `evt.*` name;
* invalid expression syntax, excessive nesting, integer overflow, division or
  modulo by zero, invalid float result, bad coercion or failed built-in domain;
* a frame too short for any declared signal, an illegal wire length or a value
  outside a strict declared range;
* a capability/governor denial, absent sink or missing runtime hook;
* queue capacity exhaustion under the documented overflow policy;
* malformed HAL frame, unsupported protocol, bus error, bus-off or clock fault.

An action failure is recorded and processing continues only in the safe,
non-side-effecting direction specified by the FSM status model. In particular,
a failed guard does not become true, an unauthorized send is not attempted,
and a short frame does not update a partial signal set.

### 4.2 Fault saturation

Fault saturation refers to a full event queue, not an unbounded retry. Queue
overflow has a consecutive-overflow threshold; persistence is exposed so an
integrator can raise at least a warning or move to SAFE mode without relying
on log parsing. The queue never evicts a fault event to admit a non-fault event.
If a full queue contains only faults, the new fault is explicitly reported as
undeliverable. The operation remains bounded and the drop is counted.

The runtime counters are fixed-width observability values. Their maximum-value
policy is part of the target integration's safety requirements and must be
specified before a production safety case; a release must not interpret a
wrapped diagnostic counter as proof that no faults occurred. Counter reads and
fault-state decisions therefore use the explicit status/fault path, not log or
counter absence.

### 4.3 Mode and transmission policy

The runtime must boot in a non-transmitting state until the integrator enables
transmission. The governor rejects IDs and signals outside package capabilities.
On a governor, HAL or expression fault, the decision is to block the affected
side effect, record the fault and leave the FSM suspended or in the configured
safe state. Recovery is an explicit, reviewed transition; there is no automatic
resume after bus-off or storage failure.

## 5. Resource and timing bounds

The principal bounds are compile-time constants and caller capacities:

| Resource | Bound / policy | Safety relevance |
|---|---|---|
| Event queue | 1..32767 caller-owned slots | No heap growth; heap operations are bounded and deterministic |
| CAN payload | 1..8 classic; 1..8, 12, 16, 20, 24, 32, 48 or 64 FD bytes | Every bit access has a representable wire bound |
| Expression text | 256 characters | Bounded scan and parser input |
| Expression nesting | 32 parser depth units | Bounds evaluator stack use and work |
| FSM transition chain | 4 transitions by default | Prevents uncontrolled generated recursion |
| FSM actions per event | 128 by default | Limits side effects and activation time |
| FSM generated events | 64 per activation by default | Limits internal work |
| Runtime allocation | zero in core execution archives | Prevents allocator failure and unbounded latency |

The integrator must measure worst-case execution time on the target and account
for interrupt, driver and OS scheduling budgets. The table is a resource-safety
bound, not a claim of a target WCET.

## 6. Verification strategy

Verification uses independent techniques so a passing unit test is not the
sole basis for a safety claim:

| Evidence | Scope | Acceptance |
|---|---|---|
| CTest, GCC/Clang warnings and ASan/UBSan | Functional behavior, memory and undefined behavior on host | Existing CI gates remain green with no warning or sanitizer failure |
| Frama-C/WP with `-wp-rte` | `event_queue_push`, `event_queue_pop`, codec encode/decode and bit helpers | [`formal/frama-c/verify_wp.sh`](../../formal/frama-c/verify_wp.sh) completes with all selected obligations discharged |
| KLEE | Symbolic bounded expression text, symbolic resolver values, zero-divide/overflow paths and arbitrary event time ordering | [`formal/klee/run.sh`](../../formal/klee/run.sh) has no assertion, execution or solver error |
| MISRA C:2012 cppcheck addon | `core/` and `platform/` coding-rule analysis | [`formal/misra/run_cppcheck.sh`](../../formal/misra/run_cppcheck.sh) exits zero; reviewed exceptions are in [`MISRA_Deviations.md`](MISRA_Deviations.md) |
| No-allocation archive scan | Runtime archives and mock/platform paths | `ci/check_no_alloc.py` finds no allocator symbol |
| Conformance and parity tests | CAN FD boundaries, opendbc bit parity, queue/fault policy and FSM behavior | CTest and integration workflow pass; output is deterministic |
| Traceability review | Requirement-to-evidence completeness | `ci/check_traceability.py` passes and every formal artifact cites its requirement boundary |

Formal tools are verification tools, not runtime dependencies. Their versions,
prover configuration, target compiler and report hashes must be recorded in the
release verification record. A missing tool or skipped proof is a failed
verification activity, not a passing result.

## 7. Requirements and evidence map

The Phase 9 artifacts implement and verify the existing requirements rather
than inventing a parallel safety API:

* `SW-FR-EVENT-004..006` and `SYS-NF-001..002`: bounded event storage,
  deterministic total ordering and resource bounds;
* `SW-FR-CODEC-001..008`: checked bit packing, representability and atomic
  short-frame behavior;
* `SW-FR-FSM-035..038`: expression grammar, safe evaluator and fail-closed
  expression faults;
* `SW-FR-FSM-021`, `SW-FR-FSM-045..046`: bounded execution and determinism;
* `SW-FR-FSM-023..025`, `SW-FR-FSM-039..042`: capability, hardware boundary and
  governor checkpoints.

The machine-readable requirement record remains
[`docs/trace/traceability.csv`](../trace/traceability.csv). The formal scripts
are deliberately explicit about their input source files so a new module
cannot silently enter the proof boundary.

## 8. Change control and release gate

A safety-relevant change must identify affected requirements, update the
contracts/tests/documentation, and be reviewed for determinism, resource
bounds and fail-closed behavior. Changes to a formal precondition require the
same review as a production behavior change; weakening a precondition to make a
proof pass is not acceptable.

Before an ASIL-B-aligned release candidate, the maintainer must attach:

1. the clean CTest, sanitizer, traceability and no-allocation outputs;
2. the Frama-C/WP and KLEE logs with tool versions and report hashes;
3. the cppcheck/MISRA report and reviewer disposition for each deviation;
4. target compiler flags, platform assumptions and WCET/resource measurements;
5. an updated HARA/technical-safety review from the responsible system owner.

This draft is complete as a software safety-manual baseline when those release
artifacts are attached. It must not be used as evidence that the whole vehicle
item has achieved ASIL-B certification.
