# ADR-001: Event Queue Implementation as Binary Min-Heap

## Status
Accepted

## Context
The event queue must support deterministic pop ordering by
(timestamp_us, priority_class, sequence) with O(log n) complexity
and zero heap allocation.

## Decision
Implement the event queue as a binary min-heap over caller-provided
storage. The heap invariant uses the three-field composite key.

## Rationale
- O(log n) push/pop is negligible for n≤512.
- Zero allocation satisfies SYS-NF-002.
- Deterministic ordering is guaranteed by the composite key.
- Simpler than bucketed priority queues for this use case.

## Consequences
- Heap stability under equal keys is guaranteed by the sequence
  tiebreaker.
- Future per-FSM queue variants may use different data structures
  if profiling shows a need.
