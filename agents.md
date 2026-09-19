# CANcestry AI Agent Development Policy

This document defines the strict rules and guardrails for AI agents contributing code to the CANcestry repository. Violating these rules will result in PR rejection.

## 1. Core Directives
- **Requirements First**: Every code change must reference a specific requirement ID (e.g., `Implements: SW-FR-EVENT-001`) in the PR description and code comments.
- **No Hallucinated Features**: Do not implement features, fields, or behaviors not explicitly defined in the `docs/` specifications or `schemas/`.
- **Zero Heap Allocation in Core**: The `core/` directory must not use `malloc`, `calloc`, `realloc`, or `free`. All memory must be statically allocated or caller-owned.
- **Determinism is Mandatory**: Do not introduce non-deterministic behavior (e.g., unordered map iteration, random seeds, unseeded floating-point operations without defined tolerance).

## 2. Safety & Security Guardrails
- **No Arbitrary Code Execution**: Do not add scripting engines, `eval()`, or dynamic plugin loading. The runtime is strictly declarative (YAML/TOML parsed to static structs).
- **Fail-Closed**: If a safety check fails (e.g., governor rate limit, capability violation), the action must be blocked, logged, and the system must not proceed with the unsafe operation.
- **No Direct Hardware Access in Core**: The `core/` modules must only interact with hardware via the `platform/` abstraction layer.

## 3. Schema & Validation Rules
- **Schema is Law**: Any new YAML/TOML generation or parsing logic must be validated against the corresponding JSON Schema in `schemas/`.
- **Provenance**: If generating codec maps from external sources (e.g., `opendbc`), the output MUST include a provenance header with the source URL and Git commit hash.

## 4. Testing & CI Requirements
- **No PR Without Tests**: Every new function or logic path must have corresponding unit or integration tests.
- **Parity Testing**: For codec changes, ensure the `opendbc` parity test harness passes.
- **Clean Builds**: Code must compile with `-Wall -Wextra -Werror -Wpedantic` on GCC and Clang, and pass AddressSanitizer (ASan) checks.

## 5. Definition of Done for AI Agents
A task is only complete when:
1. Code compiles cleanly on host and target.
2. All new and existing tests pass (including ASan).
3. Requirement IDs are explicitly referenced.
4. Documentation (`docs/`) is updated to reflect the change.
5. A human maintainer has reviewed and approved the PR.

## Scope and hardware policy pointer (added 2026-09-19)

- This policy governs the software trees: `core/`, `platform/`, `tools/`,
  `tests/`, `schemas/`, `formal/`, and software documents under `docs/`.
- Hardware trees (`hw/`, `docs/hw/`, `schemas/hw/`, future `hw/ecad/`) are
  governed by `HwAGENTS.md`.
- Shared invariants apply to both trees unchanged: requirements-first,
  schema-is-law, determinism, fail-closed, honest ledger, documented
  deviations, no self-approval of safety-relevant changes.
- Rule 1 extension: requirement IDs may be software (`SW-FR-*`, `SYS-SF-*`)
  or hardware (`HW-SF-*`, `HW-FR-*`, `HW-NF-*`), or an HW-PLAN clause.
- Traceability extension: `docs/trace/traceability.csv` now carries hardware
  rows with the columns `capella_element_id, credibility_level, oracle_id`.
  The oracle rule (HW-PLAN §10.2) applies to every `passing` row of any kind:
  no oracle ID, no pass.
---
*Last Updated: 2026-09-14*

*Version: 1.0.1*
