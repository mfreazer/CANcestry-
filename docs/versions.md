# CANcestry version and release policy

| Field | Value |
|---|---|
| Current candidate | `1.0.0-rc.1` |
| Source of truth | [`VERSION`](../VERSION) and root [`CMakeLists.txt`](../CMakeLists.txt) |
| Candidate review date | 2026-09-18 |
| Release gate | QA-EV-01 reserved fault-slot and hard-fault escalation correction |
| Final release | Deferred; no final `1.0.0` tag or milestone closure is authorized |

## Version history

| Version | State | Evidence / decision |
|---|---|---|
| `0.3.0-rc.1` | Previous candidate | Portable runtime, FSM, HAL, transport and UDS baseline. |
| `1.0.0-rc.1` | Current candidate | Phase 12 BMS, HIL, safety and traceability evidence is present; QA-EV-01 is corrected in `core/event/` but remains open for formal QA closure. |
| `1.0.0` | Deferred | May be written to `VERSION` and CMake, tagged, and used to close the v1.0.0 milestone only after QA-EV-01 is formally closed. |

## Release-candidate rules

1. `VERSION`, CMake project metadata, the SwRS, safety artifacts and traceability
   record must identify the current candidate as `1.0.0-rc.1`.
2. The final release marker must not be inferred from passing host tests alone.
   QA must review the reserved fault-slot behavior, all-fault saturation path,
   HAL safe-state action and IWDG escalation evidence.
3. A final release requires a clean strict build, sanitizer validation, the
   traceability gate, safety/HIL review, and a written QA closure for QA-EV-01.
4. Only after that closure may the maintainer perform the final version bump,
   create the release tag, and close the v1.0.0 milestone.

This file is a release-control record, not an authorization to claim final
1.0.0.
