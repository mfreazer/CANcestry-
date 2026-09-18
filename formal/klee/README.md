# KLEE bounded symbolic execution

The harnesses in this directory are verification artifacts, not firmware code.
They exercise the allocation-free expression evaluator and the event ordering
relation with symbolic inputs:

| Harness | Property | Requirements |
|---|---|---|
| `expression_harness.c` | Every bounded expression string returns normally; parser depth and string length stay bounded; division by zero and signed division overflow fail closed. | `SW-FR-FSM-035`, `SW-FR-FSM-037`, `SW-FR-FSM-038`, `SYS-NF-002` |
| `event_order_harness.c` | Lexicographic ordering is antisymmetric and transitive for arbitrary timestamps, priority classes and sequence numbers. | `SW-FR-EVENT-006`, `SW-FR-FSM-046`, `SYS-NF-001` |

The harnesses use no production-only hooks and do not change the compiled
runtime. KLEE is deliberately optional because it is a host verification tool,
not a target dependency.

## Reproduce

The script requires `clang`, LLVM bitcode support and KLEE on `PATH`:

```sh
formal/klee/run.sh
```

The script performs two separate runs so a failure identifies either the
expression evaluator or the event comparator. It uses a fresh output directory
under `${KLEE_OUT:-build/klee}` and fails on KLEE error reports, assertion
failures, or a non-zero KLEE exit status.

The expression harness treats all bytes up to
`CANCESTRY_FSM_EXPRESSION_MAX` as symbolic, constrains only the terminating
NUL, and separately makes resolver values symbolic. Therefore an invalid text
is reported as a parser error rather than being silently treated as a valid
expression. The division checks cover both zero denominators and the
`INT64_MIN / -1` C signed-overflow boundary.

The event harness does not restrict timestamps. In particular, zero,
`UINT64_MAX`, and values on either side of a wrap boundary are all explored.
Only the priority domain is constrained to the public enumeration; sequence
numbers remain fully symbolic.
