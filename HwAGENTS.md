# CANcestry Hardware Agent Policy (HwAGENTS.md)

Version 1.0.0 — governed by docs/hw/HW-PLAN.md v1.0.0.

1. **Requirements first.** Every change references an HwRS ID
   (HW-SF-*/HW-FR-*/HW-NF-*) or an HW-PLAN clause. No unrequested hardware.
2. **No hallucinated physics or parts.** Component values, FIT rates, sleep
   currents, and thermal parameters must cite a committed datasheet entry in
   hw/bom/ or a schema-validated parameter file. Invented numbers are defects.
3. **Oracle rule.** A simulation or analysis shall not close a requirement
   without an oracle ID from hw/tests/oracles/registry.csv (HW-PLAN §10.2).
4. **Honest ledger.** Row statuses are: draft, analysis-pending, sim-pending,
   passing(analysis), passing(sim,CLn), bench-pending. A row reads passing
   only with evidence artifact hashes and credibility >= required.
   Bench-trigger rows (HW-PLAN §10.4) stay bench-pending forever until measured.
5. **Determinism.** Digest-pinned toolchain images, fixed seeds, hashed
   traces. Evidence format mirrors docs/qa/hil-fault-injection-report.md.
6. **Fail-closed hardware.** Safe state must exist without firmware
   (HW-SF-001). Never propose a safe state that depends on code running.
7. **Schema is law.** BOM, sim cases, oracle registry, and bridge rows
   validate against schemas/hw/ in CI.
8. **No safety-relevant self-approval.** Safety monitor chain, fail-safe
   topology, retention domain, watchdog wiring, power supervisor: human
   reviewer sign-off required. Agents may not approve these.
9. **Tool qualification before evidence.** Any new simulation or analysis
   tool needs a TCL classification and qualification evidence in
   docs/hw/tool-qualification.md before its output may close a row.
10. **Scope boundaries.** Protocol logic stays in the C conformance suites;
    do not duplicate it in Modelica. No ECAD work before the HW-PLAN G2 gates.
    No Capella edits outside hw/model/capella/.
11. **CI gates are load-bearing.** hw-fast and hw-nightly may not be
    weakened, skipped, or marked continue-on-error.
12. **Document deviations.** Spec ambiguity: implement the logical choice,
    document it in the owning hw doc, flag it in the PR. Never silent.
