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
| `renode/can_fault_injector.py` | CAN bus fault injector (H-10, issue #62): shared-medium Bus-Off / CRC fault-condition projection with the ISO 11898-1 128 × 11 recovery sequence, TCL1. Monitor command interface (`sysbus.can_fault_injector ControlWrite 0x42/0x43` — one dotted token; a bare `sysbus` resolves to the SystemBus object) plus read-only register window at 0x40000000. |
| `renode/cancestry-hw-fault.resc` | H-10 bring-up for the same (unmodified) platform: injector registration, fault symbol hooks, and the single merged CCCR/IR write hook (one `SetHookBeforePeripheralWrite` registration: a second call on the same peripheral silently replaces the first in Renode v1.16.1) with a fail-closed include-time self-test. |
| `firmware/can_fault.c` | Off-tree CAN fault driver through the REAL v1.0.0 fault path (`cancestry_hal_raise_fault`); built with `CANCESTRY_T2_DRIVER=can_fault`. |
| `t2_fault_common.py` | Shared fail-closed machinery of the two H-10 scenarios: preflight, 100 µs `RunFor` quanta, 10 ms boundary injection, exactly-once slot capture, in-run invariants, `--check` determinism gate. |
| `scenarios/t2_busoff_001.py`, `scenarios/t2_crc_001.py` | The two H-10 scenarios (Bus-Off recovery with the 128 × 11-bit-time assert; CRC error handling with the counter/no-crash asserts). |
| `test_t2_busoff.py`, `test_t2_crc.py` | Their pytest wrappers: offline contract/invariant/manifest tests plus the toolchain-gated end-to-end `--check` run. |
| `renode/bor_reset_injector.py` | BOR reset injector (H-11, issue #64): ACTIVE-LOW NRST modeling with the issue's 100 µs pulse, main-SRAM marker discard, `RCC_CSR.BORRSTF` and the BOR machine reset, TCL1. Monitor command interface (`sysbus.bor_reset_injector ControlWrite 0x42` — one dotted token; a bare `sysbus` resolves to the SystemBus object) plus a read-only register window at the issue-pinned 0x40001000. All state lives in the trace area, so the pending release survives the machine reset. |
| `renode/cancestry-hw-bor.resc` | H-11 bring-up for the same (unmodified) platform: injector registration, H-11 slot initialization, the first-event-guarded retention-write hook and the two post-reset hooks. |
| `firmware/bor_brownout.c` | Off-tree brownout driver through the REAL v1.0.0 retention write/read and safe-state APIs; built with `CANCESTRY_T2_DRIVER=bor_brownout`. |
| `t2_bor_common.py` | Shared fail-closed machinery of the H-11 scenario: preflight, 100 µs `RunFor` quanta with 1 µs plant sub-steps (`MePlantSlave`: the guarded zero-state FMI 2.0 ModelExchange evaluator — explicit `setTime` per sub-step, bounded event iteration), plant-driven injection, in-run ordering/retention/SRAM/reset-cause invariants, `--check` determinism gate. |
| `scenarios/t2_brownout_001.py` | The H-11 scenario: the modelled BOR assertion drives the injection at t = 10 ms; asserts `retention write < BOR detection < recovery`. |
| `test_t2_brownout.py` | Its pytest wrapper: offline contract/invariant/manifest tests plus the toolchain-gated end-to-end `--check` run. |

## T2 CAN fault scenarios (H-10, issue #62)

The bench also carries the two non-brownout Phase-12 fault scenarios
Bus-Off recovery (`HW-T2-BUSOFF-001`) and CRC error handling
(`HW-T2-CRC-001`), against the gateway node's REAL v1.0.0 fault path. The
shared-medium condition is projected by the `can_fault_injector` TCL1
peripheral; the ISO 11898-1 recovery sequence (128 × 11 recessive bits,
704 µs at the HW-FR-003 nominal 2 Mbit/s) is evaluated as a pure function
of emulation virtual time. No FMU is involved; the hash chain is ELF +
bridge + injector + trace. **Brownout was out of scope for H-10**
(H-11, issue #64, delivers it - see the brownout section below; the
`not_simulated` BOR physics are now a T1/T2 fixture and every promotion stays
gated on HS-01/HS-02 plus a human safety reviewer, HwAGENTS.md rule 6).

Honest status: like the retention foundation, no T2 run has been executed
for these scenarios. The committed `hw/tests/evidence/t2_busoff_001.json`
and `t2_crc_001.json` are PENDING manifests (pass=false, CL0,
`oracle_id: "none"`), the ledger rows `(HW-FR-003, virtual_bench)` are
`sim-pending` / CL0, and the hw-nightly `t2-virtual-bench` job runs each
scenario with its `--check` twin. Nothing promotes: per issue #62 every
promotion path stays gated on a registered oracle plus HS-01/HS-02 and a
human safety reviewer.

## T2 brownout / BOR reset scenario (H-11, issue #64)

The bench also carries the Phase-12 brownout fault: the H-11 plant
`CancestryLib.Power.BOR` (nominal 3.3 V rail collapsing to 0 V at 10 ms for
100 µs, BOR level 3 threshold at 2.8 V falling with a hysteresis release gate,
the OR-001 retention-domain closed form across the collapse, main-SRAM loss and
the reset-deassertion → resumption recovery time) is compiled headless by the
pinned OpenModelica and advanced by the H-07 bridge contract (100 µs master /
1 µs plant sub-steps). The plant is a **zero-state algebraic source**, so it is
executed through the guarded FMI 2.0 **ModelExchange** evaluator
(`t2_bor_common.MePlantSlave`) — the same execution path the OR-002 pulse
fixture uses — and not through CoSimulation `doStep`, which the pinned
OpenModelica 1.24 runtime cannot drive for a zero-state model
(`docs/hw/tool-qualification.md` §3.1, "FMI execution-path finding"). The PLANT
drives the fault: at the modelled BOR assertion the orchestrator commands the
`bor_reset_injector` TCL1 peripheral, which pulls the active-low NRST line for
100 µs, discards the T2 main-SRAM marker, sets `RCC_CSR.BORRSTF` and takes the
BOR machine reset. The REAL v1.0.0 firmware then classifies the reset cause,
recovers the retained terminal code through
`cancestry_watchdog_read_retention_register` and restores the safe state
through `cancestry_hardware_set_safe_state`; the scenario asserts the issue's
ordering chain `retention write < BOR detection < recovery`, the 100 µs pulse,
that the retained code survived the reset while main SRAM did not, the BOR
reset cause and firmware liveness. Hash chain: FMU + ELF + bridge + injector +
trace.

Honest status: no T2 brownout run has been executed. The committed
`hw/tests/evidence/t2_brownout_001.json` is a PENDING manifest (pass=false,
CL0, `oracle_id: "none"` — no OR-XXX covers the BOR behaviour, issue #64), the
ledger row `(HW-SF-002, virtual_bench)` stays `sim-pending` / CL0 (the row
already covers the retention sub-events; the checker forbids a duplicate
(requirement, method) pair), and the hw-nightly `t2-virtual-bench` job runs the
scenario with its `--check` twin. The BOR FMU output is explicitly OUTSIDE the
bounded FMPy TD1 argument (`docs/hw/tool-qualification.md` §3.1, H-11
disposition): no passing claim may be consumed from it until an oracle
validates it or FMPy is reclassified for that configuration. Promotion beyond
sim-pending / CL0 stays gated on a registered oracle plus HS-01/HS-02 and a
human safety reviewer (the change touches the supervisor/reset path,
HwAGENTS.md rule 6). Scope is exactly brownout: no thermal coupling, no
multi-channel, no other fault scenarios.

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
