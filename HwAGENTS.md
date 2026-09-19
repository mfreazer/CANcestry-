# CANcestry Hardware Agent Policy (HwAGENTS.md)

| Field | Value |
|---|---|
| **Document** | CANcestry Hardware Agent Policy |
| **Version** | 1.1.0 |
| **Status** | Approved — load-bearing policy |
| **Owner** | System Engineer |
| **Approver** | QA Lead |
| **Last Review** | 2026-09-19 |
| **Repository location** | `HwAGENTS.md` (repository root) |
| **Governing document** | `docs/hw/HW-PLAN.md` v1.0.0 |
| **Applies to** | `hw/`, `docs/hw/`, `schemas/hw/`, future `hw/ecad/`, and `@cancestry-hw-agent` |

---

1. **Requirements first.** Every change references an HwRS ID
   (`HW-SF-*` / `HW-FR-*` / `HW-NF-*`). Planning documents (`HW-PLAN`,
   `mbse-plan`, `virtual-bench-plan`, `safety-case`) are not requirements and
   shall not be cited as authority for a change. If a needed change has no
   HwRS coverage, that is a signal the HwRS is incomplete: raise the
   requirement via an HwRS PR (QA review) before implementing.

2. **No hallucinated physics or parts.** Component values, FIT rates, sleep
   currents, and thermal parameters cite a schema-validated parameter extract
   under `hw/bom/datasheets/` (manufacturer, document ID, page/table,
   retrieval date), referenced by the BOM's mandatory `datasheet_ref` column.
   Numbers in CSV comments, or values without an extract, are defects.

3. **Oracle rule.** A simulation or analysis shall not close a requirement
   without an oracle ID listed in `hw/tests/oracles/registry.csv` — the
   source of truth for oracles (HW-PLAN §10.2).
   `docs/hw/virtual-bench-plan.md` §4 is a rendered view of that registry.

4. **Honest ledger.** Row statuses are: `draft`, `analysis-pending`,
   `sim-pending`, `bench-pending`, `passing(analysis)`, `passing(sim,CLn)`,
   `passing(sim,CLn,provisional)`, `fully-verified(CL3)`. A row reads
   `passing*` only with evidence artifact hashes and credibility ≥ required.
   Safety rows (`HW-SF-*`) closed at T1/T2 carry `provisional`; only T4
   correlation promotes them to `fully-verified(CL3)`. Bench-trigger rows
   (HW-PLAN §10.4) stay `bench-pending` until measured.

5. **Determinism.** Digest-pinned toolchain images, fixed seeds, hashed
   traces. Evidence format mirrors `docs/qa/hil-fault-injection-report.md`.

6. **Fail-closed hardware.** Safe state must exist without firmware
   (HW-SF-
