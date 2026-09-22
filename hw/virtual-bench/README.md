# CANcestry T2 Virtual Bench (H-07, issue #53)

The T2 virtual bench executes the **real v1.0.0 firmware binary** against the
Modelica plant over an FMI 3.0 co-simulation (HW-PLAN §8.3,
virtual-bench-plan §1). It never reimplements firmware logic.

```
 Renode (virtual MCU target)               OpenModelica (CancestryLib plant)
 - runs the REAL v1.0.0 ARM ELF            - Holdup FMU (OR-001 plant)
 - scripted IWDG/RTC_BKP/GPIO/RCC          - 100 µs master / 1 µs FMU step
 - symbol hooks stamp exact event times    - deterministic, no wall clock
             \                                        /
              \-------- FMI 3.0 co-simulation --------/
                                 |
                run_t2_retention.py (T2 orchestrator)
                oracles: OR-001 (retention ordering)
                evidence: hw/tests/evidence/t2_retention_001.json
```

## Layout

| Path | Purpose |
|---|---|
| `renode/stm32g474-cancestry.repl` | Platform model: exactly the register surface used by `platform/cortex_m` at v1.0.0; everything else declared `not_simulated` with rationale (virtual-bench-plan §3). |
| `renode/cancestry-hw.resc` | Bring-up script: creates the machine, loads the platform, loads the `$elf` firmware ELF, installs the event-capture symbol hooks, fixes the quantum and seed. (The issue named `cancestry-hw.cs` "or equivalent Renode script"; `.resc` is Renode's script format.) |
| `renode/iwdg_model.py`, `rtc_backup_model.py`, `gpioa_model.py`, `rcc_model.py` | Scripted peripheral models (Renode `Python.PythonPeripheral`, IronPython 2). |
| `fmi_bridge.py` | Deterministic FMI 3.0 co-simulation master: fixed 100 µs master step, 1 µs FMU internal step, discrete event capture at actual occurrence times (virtual-bench-plan §6, QA ruling VB-Q1). |
| `run_t2_retention.py` | T2 orchestration: build the Holdup FMU, launch Renode with the v1.0.0 ELF, run the bridge for 150 ms, capture the retention write / safe-latch / IWDG fire timestamps, assert the ordering, write the evidence artifact. |
| `fmi_bridge_test.py`, `test_t2_retention.py` | Unit tests for the bridge and the orchestration/evidence logic; the full-stack integration test requires the T2 toolchain. |

## Execution environment

T2 is heavy simulation: per HW-PLAN C5 it runs on Renode-equipped
infrastructure, not in the GitHub-hosted `hw-fast` image (which has
OpenModelica + FMPy but no Renode). Required on the runner:

| Tool | Pin / source |
|---|---|
| `renode` | pinned version; checked by the runner before launch (fail-closed) |
| `omc` (OpenModelica) | `1.24` (`ci/docker/` toolchain image) |
| FMPy | `0.3.24` |
| firmware ELF | built from the `v1.0.0` tag with the pinned toolchain image; path via `CANCESTRY_T2_ELF`; its sha256 is recorded in the evidence artifact |

## Honest status (HwAGENTS.md rule 4)

This issue delivers the T2 **foundation**: platform model, deterministic
bridge, orchestration, schema, pending evidence manifest, ledger row and the
pending placeholder visual. **No T2 run has been executed yet**: the committed
`hw/tests/evidence/t2_retention_001.json` is a *pending* artifact
(`pass=false`, CL0) in exactly the pattern of the H-06 pulse manifests
(`pulse_5a_001.json`), because no Renode-equipped run of the v1.0.0 ELF
exists. The ledger row `(HW-SF-002, virtual_bench)` is `sim-pending` until a
real T2 run produces the hashed artifact; the orchestration script is what
produces it, deterministically. Nothing in this directory claims a `passing`
disposition.

## Requirements

Implements: HW-SF-002 (retention domain across IWDG reset; QA-EV-01
escalation ordering), HW-SF-004 (ordering: retention write < safe-latch <
IWDG fire), HW-FR-001 (register map). Oracle: OR-001 (registered,
`hw/tests/oracles/registry.json`). Governing plans: `docs/hw/virtual-bench-plan.md`,
`docs/hw/HW-PLAN.md` §8.3, §10.1 (T2), §10.2 (oracle rule).
