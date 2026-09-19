# QA findings and closure record

| Field | Value |
|---|---|
| Candidate | `1.0.0-rc.1` |
| Review date | 2026-09-18 |
| Open release blocker | `QA-EV-01` |
| Final `1.0.0` status | Deferred; no tag or milestone closure |

## Closed findings

| Finding | Closure evidence | State |
|---|---|---|
| Phase 12 governor lacked complete deterministic BMS evidence | `tests/conformance/governor/test_bms_governor.c`, strict and sanitizer runs, and the governor trace rows | Closed for the release candidate |
| HIL fault-injection scenarios lacked a single reproducible evidence record | `docs/qa/hil-fault-injection-report.md` and `tests/hil/hil_fault_injection.py` | Closed for the release candidate; physical target measurements remain an explicit limitation |
| Safety Manual did not identify each required Phase 12 safety ID | `docs/safety/SafetyManual.md` sections 5, 6 and 10 | Closed for the release candidate |
| Traceability record used an undocumented five-column CSV | `docs/trace/traceability.csv`, `docs/trace/traceability.md`, and `ci/check_traceability.py` | Closed by the six-column migration |
| Release metadata claimed final `1.0.0` before event saturation evidence was accepted | `VERSION`, root `CMakeLists.txt`, `docs/versions.md` | Corrected to `1.0.0-rc.1`; final release remains deferred |

## QA-EV-01 status

QA-EV-01 remains **open pending formal QA closure**. The implementation now
uses Path A reserved fault slots in `core/event/`:

- ordinary events cannot consume the configured fault reserve;
- faults use reserved capacity and may evict only a deterministic newest
  non-fault victim; and
- an all-fault full queue retains its bounded fault set and invokes the HAL
  fail-safe hook followed by the IWDG escalation hook.

The deterministic unit and Cortex-M conformance tests are present:

- `EVENT-RESERVED-SLOTS-001..005`
- `HARD-FAULT-ESCALATION-001..002`

Passing those tests is implementation evidence, not the QA sign-off itself.
The release owner must review the trace, strict/sanitized results, platform
binding, and target/HIL limitations, then record a dated closure decision. Until
that decision exists, no final `1.0.0` bump, tag, or v1.0.0 milestone closure is
permitted.

## QA-EV-01: Event Fault Saturation Path (Closed)

- **Disposition**: Closed
- **Merge Commit**: `ad81ca79d17da6ea6e97a036f9f10ac8555fe87e`
- **Resolution**: Implemented Path A reserved fault slots with boot-surviving retention persistence, terminal queue state refusal, and fail-closed safe-spin on unconfigured hooks.
- **Verification Tests**: `FAULT-RETENTION-001`, `TERMINAL-STATE-001`, `RESET-REASON-001`, `FALLBACK-SPIN-001`, `EVENT-RESERVED-SLOTS-001..005`, `HARD-FAULT-ESCALATION-001`.
