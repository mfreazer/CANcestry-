# CANcestry Test Strategy

| Field | Value |
|---|---|
| Document | CANcestry Test Strategy |
| Version | 1.0.0-rc.1 |
| Status | Release-candidate QA baseline; QA-EV-01 remains open |
| Owner | QA |
| Approver | System Engineer + Maintainer |
| Last Review | 2026-09-18 |
| Change Log | v1.0.0-rc.1: Phase 12 safety evidence plus QA-EV-01 reserved-slot correction |
| Repository location | `docs/qa/test-strategy.md` |

---

## 1. Purpose

This document defines the formal test strategy for **CANcestry**, a microcontroller-based CAN codec, translation, emulation, and gateway platform.

The strategy establishes:

- test levels,
- test environments,
- CI/CD gates,
- Docker and emulation infrastructure,
- hardware-in-the-loop requirements,
- traceability expectations,
- entry and exit criteria,
- defect and release gates.

This strategy is normative for the CANcestry 1.0.0-rc.1 candidate and later until superseded. Final 1.0.0 release, tagging, and milestone closure remain deferred until QA-EV-01 is formally closed.

---

## 2. Scope

This strategy covers testing of:

- portable CANcestry core,
- package manifest and schema validation,
- codec engine,
- recipe engine,
- FSM runtime,
- event bus and ordering,
- safety governor,
- CAN core and HAL,
- logger and replay,
- host CLI and simulator,
- firmware integration,
- hardware-in-the-loop behavior.

This strategy does **not** cover:

- automotive functional safety certification,
- road-legality validation,
- CAN FD,
- full UDS diagnostics,
- cloud services or marketplace infrastructure.

Those remain outside the current release-candidate scope or are explicitly deferred in the v1.1.0 ledger.

---

## 3. References

- `docs/system/SyRS.md`
- `docs/system/SyAD.md`
- `docs/software/SwRS.md`
- `docs/software/SwAD.md`
- `docs/packages/package-spec.md`
- `docs/packages/codec-map-spec.md`
- `docs/packages/recipe-spec.md`
- `docs/packages/fsm-spec.md`
- `docs/system/mode-fault-state-machine.md`
- `docs/system/event-ordering.md`
- `docs/system/governor.md`
- `docs/system/expression-language.md`
- `docs/system/logging-replay.md`
- `docs/safety/ai-agent-policy.md`
- `AGENTS.md`

---

## 4. Test Objectives

The test program shall verify that CANcestry:

1. Loads and validates third-party packages safely.
2. Decodes and encodes CAN signals deterministically.
3. Executes recipes and FSMs with bounded, deterministic behavior.
4. Enforces package capabilities and governor limits.
5. Enters safe states on critical faults.
6. Produces identical behavior in simulation and on target for the same event sequence, except for hardware timing jitter.
7. Supports third-party development without hardware.
8. Supports hardware CAN operation under governor control.
9. Preserves traceability from requirements to tests.
10. Prevents unsafe or unauthorized CAN transmission.

---

## 5. Test Levels and Types

| Level | Purpose | Environment | Automation | Gate |
|---|---|---|---|---|
| Static / Schema | Validate manifests, codec maps, recipes, FSMs | Docker, CI | Required | Blocks merge |
| Unit | Test core logic in isolation | Host, Docker | Required | Blocks merge |
| Integration | Test module interactions | Host, Docker, simulator | Required | Blocks merge |
| Simulation | Execute full runtime against virtual CAN | QEMU / Renode / host sim | Required | Blocks main |
| Conformance | Verify normative contracts | Docker, CI | Required | Blocks Phase 4 FSM merge |
| HIL | Verify real CAN, timing, bus-off | Self-hosted runner | Required for release | Blocks release |
| Security / Integrity | Verify hashes, signatures, trust levels | Docker, CI | Required | Blocks release |
| Performance / Latency | Verify timing and rate limits | Simulator + HIL | Required | Blocks release |
| Fault Injection | Verify safe-state and recovery | Simulator + HIL | Required | Blocks release |
| Regression | Prevent re-breakage | All environments | Required | Continuous |

---

## 6. Test Environments

| Environment | Description | Hosted by | Hardware |
|---|---|---|---|
| Local Dev | Developer workstation, host build, unit tests | Developer | None |
| GitHub-Hosted CI | Docker-based build, schema, unit, simulation | GitHub | None |
| Emulated Target CI | QEMU or Renode running firmware | GitHub-hosted container | None |
| Virtual CAN Host | `vcan` + `python-can` / `socketcan` | Self-hosted Linux | None |
| HIL Runner | Real CANcestry device + CAN interface | Self-hosted runner | Required |

GitHub-hosted runners shall not be used for tests requiring kernel modules, real CAN interfaces, flashing, or timing-sensitive HIL validation.

---

## 7. Docker and Tooling Strategy

The project shall maintain or use pinned Docker images for reproducible CI.

| Image | Purpose | Contents |
|---|---|---|
| `cancestry/build-env:0.2.0` | Cross-compile firmware and core | `arm-none-eabi-gcc`, CMake, Ninja, Unity/CMock, Python |
| `cancestry/schema-env:0.2.0` | Validate package schemas | Python, `jsonschema`, `ajv`, YAML/TOML parsers |
| `cancestry/sim-env:0.2.0` | Simulate firmware and CAN | QEMU, Renode, `can-utils`, `python-can` |
| `cancestry/hil-runner:0.2.0` | Flash and test real hardware | Flash tools, `pytest`, SocketCAN utilities |

Requirements:

- Images shall be pinned by digest.
- Dockerfiles shall live under `ci/docker/`.
- Images shall be built and published by CI.
- No test shall depend on unpinned tool versions.
- Hosted CI shall use software simulation when hardware is unavailable.

---

## 8. GitHub Actions CI/CD Design

The CI pipeline shall follow a fast-feedback-first model.

### 8.1 Pull Request Workflow — `pr-fast.yml`

Runs on every pull request.

Jobs:

1. Schema validation.
2. Host unit tests.
3. Host core build.
4. Firmware cross-build.
5. Static analysis and formatting.

No hardware required.

### 8.1.1 Phase 9 formal verification (issue #24)

The ASIL-B alignment evidence is run in a verification environment with pinned
Frama-C/WP, KLEE and cppcheck/MISRA tool versions. These tools are deliberately
not linked into the firmware build:

1. `formal/frama-c/verify_wp.sh` runs WP with runtime-error checks over the
   queue, codec encode/decode and bounded bit helpers.
2. `formal/klee/run.sh` runs symbolic expression and event-order harnesses,
   including adversarial timestamps and arithmetic fault paths.
3. `formal/misra/run_cppcheck.sh` runs the MISRA C:2012 addon over `core/` and
   `platform/`; deviations are recorded in `docs/safety/MISRA_Deviations.md`.
4. `docs/safety/SafetyManual.md` records the assumptions, fail-closed/fault
   saturation behavior and release evidence required by the safety review.

A missing formal tool is an environment failure, not a skipped passing test.
Formal reports are retained as CI/release evidence and are not committed as
compiler-dependent generated output.

### 8.2 Main / Nightly Workflow — `main-sim.yml`

Runs on merge to `main` and nightly.

Jobs:

1. Full unit and integration suite.
2. QEMU or Renode firmware simulation.
3. FSM conformance suite.
4. Event-ordering conformance suite.
5. Governor conformance suite.
6. Codec bitpacking conformance suite.
7. Golden replay tests.
8. Determinism tests.

### 8.3 HIL Workflow — `hil.yml`

Runs on self-hosted runner.

Jobs:

1. Flash device.
2. Real CAN RX/TX tests.
3. Bus-off recovery tests.
4. Latency and jitter tests.
5. Governor enforcement on real bus.
6. Storage and logging tests.

Trigger:

- manual dispatch, or
- merge to `main`, or
- release candidate.

### 8.4 Release Workflow — `release.yml`

Runs on release candidate.

Jobs:

1. Full CI suite.
2. Full HIL suite.
3. Security and integrity tests.
4. Traceability completeness check.
5. Release artifact signing.

---

## 9. Test Data and Replay

The canonical replay format shall be JSON Lines:

```text
.canl.jsonl
```

Requirements:

- Replay shall preserve event order.
- Replay shall preserve relative timestamps.
- Replay shall support time scaling.
- Replay shall preserve sequence numbers.
- Simulation shall use a deterministic virtual monotonic clock.
- Golden outputs shall be committed for known event sequences.
- Replay tests shall run in CI without hardware.

---

## 10. Traceability and Coverage

The repository shall contain:

```text
docs/trace/traceability.md
docs/trace/traceability.csv
```

The CSV shall map:

```csv
requirement_id,artifact_type,artifact_id,verification_method,test_id,status
```

Requirements:

- Every High-priority requirement shall have at least one verification row.
- Every test source shall reference at least one requirement ID.
- Every conformance suite shall map to SwRS requirements.
- Reserved fault slots and hard-fault escalation shall have deterministic edge-case tests (`EVENT-RESERVED-SLOTS-*`, `HARD-FAULT-ESCALATION-*`).
- Traceability completeness shall be checked in CI by `ci/check_traceability.py`.

---

## 11. Entry and Exit Criteria

| Phase | Entry Criteria | Exit Criteria |
|---|---|---|
| Phase 0 — Docs and schemas | QA review accepted | Schemas committed, traceability skeleton committed |
| Phase 1 — Portable core | Phase 0 complete | Core builds host + target, event/error models unit-tested |
| Phase 2 — Codec engine | Phase 1 complete | Bitpacking conformance passes, decode/encode tests pass |
| Phase 3 — Recipe engine | Phase 2 complete | Recipe sim tests pass, governor hook verified |
| Phase 4 — FSM runtime | Phase 3 complete, FSM conformance suite exists | FSM conformance passes, event-order/timer/expression/instance tests pass |
| Phase 5 — CAN integration | Phase 4 complete | HIL CAN RX/TX and bus-off tests pass |
| Phase 6 — CLI and simulator | Phase 3+ complete | Third-party package can be developed without hardware |
| Phase 12 — v1.0.0-rc.1 safety evidence | Governor, HIL, safety artifacts and QA-EV-01 correction present | Strict build/tests, traceability and safety review pass; final 1.0.0 gate remains explicitly deferred while QA-EV-01 is open |

No FSM runtime code shall be merged before the FSM conformance suite exists and passes.

---

## 12. Risk-Based Test Priorities

Highest-risk areas shall receive the most test coverage:

1. Safety governor bypass.
2. Capability enforcement for TX IDs and signal writes.
3. FSM determinism and transition selection.
4. Event ordering and generated-event handling.
5. Timer drift and missed ticks.
6. Expression evaluator safety and overflow behavior.
7. Codec bitpacking and endianness.
8. Bus-off recovery and SAFE-mode entry.
9. Package schema validation and dependency resolution.
10. Integrity and trust-level enforcement.

---

## 13. Roles and Responsibilities

| Role | Responsibility |
|---|---|
| QA | Owns test strategy, conformance suites, traceability, release gate |
| System Engineer | Owns requirements, mode/fault model, acceptance criteria |
| Maintainer | Approves safety-sensitive changes |
| Developers | Provide unit tests and requirement-linked PRs |
| AI Agents | Follow `AGENTS.md`, provide tests, never bypass governor |
| Human Reviewer | Reviews governor, permissions, CAN TX path, fault handling |

---

## 14. Defect Management

Defect severity:

| Severity | Definition | Action |
|---|---|---|
| Critical | Safety bypass, governor failure, unsafe TX, data corruption | Block merge and release |
| High | Requirement violation, nondeterminism, FSM failure | Block merge |
| Medium | Incorrect behavior without safety impact | Fix before release |
| Low | Documentation, naming, minor UX | Track |

Any change to the following requires explicit human review:

- governor,
- permissions,
- CAN TX path,
- fault handling,
- SAFE-mode transitions.

---

## 15. Metrics and Reporting

The project shall track:

- requirement coverage percentage,
- test pass rate,
- conformance suite pass rate,
- HIL stability,
- open defects by severity,
- mean time to detect regression,
- queue overflow counters,
- governor violation counters.

Reports shall be generated per release candidate.

---

## 16. Deliverables

The test program shall produce:

```text
docs/qa/test-strategy.md
docs/qa/test-plan.md
docs/trace/traceability.csv
schemas/*.schema.json
tests/schema/
tests/unit/
tests/integration/
tests/simulation/
tests/conformance/fsm/
tests/conformance/event-order/
tests/conformance/governor/
tests/conformance/codec-bitpacking/
tests/hil/
ci/docker/
.github/workflows/
```

---

## 17. Approval

This test strategy is approved when:

1. The release-candidate schemas and requirements are committed.
2. The FSM, governor, event-reserve and hard-fault conformance suites exist.
3. Traceability is complete, with all deferred requirements named in the embedded ledger.
4. HIL evidence identifies the simulation limitation and target completion procedure.
5. QA-EV-01 is formally closed by QA and System Engineer.
6. Only then may the final version bump, tag and milestone closure be approved.

Until those conditions hold, CANcestry remains **conditionally approved** as 1.0.0-rc.1; no final 1.0.0 claim is authorized.
