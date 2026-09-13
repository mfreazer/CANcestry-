# CANcestry AI Agent CI Gates

This document defines the automated CI checks that enforce the rules in `AGENTS.md`. All PRs touching core logic must pass these gates.

## 1. Schema Validation Gate
- **Trigger**: Any change to `packages/`, `tools/`, or schema files.
- **Action**: Run `pytest tests/schema/` to validate all example and generated YAML/TOML against `schemas/*.schema.json`.
- **Fail Condition**: Any schema validation error or missing required field.

## 2. Static Analysis & Build Gate
- **Trigger**: Any code change in `core/` or `tools/`.
- **Action**: 
  - Compile with GCC and Clang using `-Wall -Wextra -Werror -Wpedantic`.
  - Run `clang-tidy` with the project's `.clang-tidy` config.
- **Fail Condition**: Any compiler warning, error, or clang-tidy violation.

## 3. Memory Safety Gate (ASan/UBSan)
- **Trigger**: Any code change in `core/` or `tests/`.
- **Action**: Build with `-fsanitize=address,undefined` and run `ctest`.
- **Fail Condition**: Any memory leak, out-of-bounds access, or undefined behavior detected.

## 4. Parity Test Gate (Phase 2.5+)
- **Trigger**: Any change to `core/codec/` or `tools/dbc2codec/`.
- **Action**: Run `tests/integration/opendbc-parity/` against the pinned `opendbc` commit.
- **Fail Condition**: Any decoded signal value diverges from `opendbc`'s `CANParser` beyond the defined floating-point tolerance.

## 5. Human Review Gate
- **Trigger**: All PRs.
- **Action**: Require at least one approval from a human maintainer with `MAINTAINER` or `SYSTEM_ENGINEER` role.
- **Fail Condition**: PR merged without approval, or approval from another AI agent.
