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
   (HW-SF-001): the passive fail-safe topology — reset, supervisor and
   fail-safe latch paths — is a requirement, not an implementation detail.
   Any change touching those paths is safety-relevant (HW-PLAN §11.1) and
   requires Human Reviewer sign-off, not just an agent merge.

---

## Change Log

- **v1.1.0 (2026-09-19)** — H-Phase 1 operational baseline (issue #33):
  rules 1–6 bind the committed hardware tree — `hw/` (Modelica
  `CancestryLib`, BOM + datasheet extracts, oracle registry, sim cases,
  ledger), `schemas/hw/`, `ci/check_hw_traceability.py` (honest-ledger
  gate), and the `hw-fast` CI job on the digest-pinned `ci/docker` image.
  The rule 4 status vocabulary (including `passing(sim,CLn,provisional)`
  for T1/T2 safety rows) is enforced by the ledger gate; rule 6 names
  HW-SF-001 as the fail-closed baseline.
- **v1.0.0** — Initial hardware agent policy (approved with HW-PLAN v1.0.0).

