# CANcestry Hardware Agent Policy (HwAGENTS.md)

| Field | Value |
|---|---|
| **Document** | CANcestry Hardware Agent Policy |
| **Version** | 1.1.4 |
| **Status** | Rules 13 (H-03), 14 (H-05), and 15 pending QA approval; existing rules remain load-bearing |
| **Owner** | System Engineer |
| **Approver** | QA Lead |
| **Last Review** | 2026-09-21 |
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
   without an oracle ID listed in `hw/tests/oracles/registry.json` — the
   schema-validated source of truth for oracles (HW-PLAN §10.2).
   `registry.csv` is a generated compatibility export, never an editable authority.
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
   The digest re-verification step requires the docker buildx plugin, which is pre-installed on ubuntu-latest runners.

6. **Fail-closed hardware.** Safe state must exist without firmware
   (HW-SF-001): the passive fail-safe topology — reset, supervisor and
   fail-safe latch paths — is a requirement, not an implementation detail.
   Any change touching those paths is safety-relevant (HW-PLAN §11.1) and
   requires Human Reviewer sign-off, not just an agent merge.

13. **Visual evidence is a view, never the proof.** Every visual artifact is
    rendered from a hashed plot-data file
    (schemas/hw/hw-plot-data-0.1.0.schema.json) that itself references a hashed
    evidence file. The provenance footer (tool+digest, input hashes, oracle,
    credibility, status) is mandatory and MUST agree with the embedded
    cancestry-provenance comment. A visual without provenance is an assertion
    and shall be rejected at review. Renderer drift (output changed, data hash
    unchanged) is a non-evidence change and must be flagged explicitly in the
    PR with the prefix `renderer-drift:`. The renderer-drift manifest is
    renders/manifest.json.

14. **Tolerance bands are binding contracts.** A shaded or hatched tolerance
    band in a rendered visual is a binding verification contract, never an
    aesthetic illustration or decorative fill. A visual shall display a
    tolerance band only when backed by an explicit `tolerance_band` definition
    in the plot-data file referencing validated `tolerance_lower` and
    `tolerance_upper` series that share an x-axis and are non-inverted. When
    requirement tolerances are scalar feature limits (e.g. peak voltage or
    rise time) rather than continuous per-sample envelopes, the visual shall
    encode them as labelled glyph markers with explicit tolerances in the
    evidence metadata—never as a synthesized continuous band. Drawing an
    undeclared or ungrounded band is a gate error and shall be rejected at
    review.

15. **Free-text allowances must fail closed and be line-anchored.** Any CI gate,
    verification script, or workflow that grants an allowance or override from
    free text (such as pull request bodies, commit messages, or review
    comments) shall parse it using an explicit, line-anchored declaration with
    a mandatory, non-empty reason. Parsers shall never use unanchored substring
    matches (such as workflow `contains()` expressions) over entire documents.
    The keyword or token naming the allowance shall never be capable of
    granting it through mere citation or quotation. Any absence of an exact
    declaration, malformed declaration, or unstated reason shall fail closed.

---

## Change Log

| Version | Date | Change |
|---|---|---|
| 1.1.4 | 2026-09-21 | H-05 #44 hardening review: Rule 15 added (free-text allowances must fail closed and be line-anchored; substring matches forbidden); status of Rules 13-15 set to pending QA approval. |
| 1.1.3 | 2026-09-21 | H-05 #44 hardening: Rule 14 added (tolerance bands are binding contracts, not decorative fill; undeclared or scalar bands rejected); placeholder fail-closed gate. |
| 1.1.2 | 2026-09-20 | H-03 #36: Rule 13 added (visual evidence is a view of a hashed plot-data file, never the proof); checker `ci/check_hw_evidence.py` and `docs/hw/visual-evidence-plan.md` bind it. |
| 1.1.1 | 2026-09-19 | H-04 #38: Rule 3 source-of-truth path migrated to JSON; CSV remains a deterministic export. |
| 1.1.0 | 2026-09-19 | QA findings HwA-F1..F4:<br>• Rule 1: HwRS IDs only.<br>• Rule 2: `hw/bom/datasheets/` extracts.<br>• Rule 3: Oracle registry CSV source of truth.<br>• Rule 4: `provisional` and `fully-verified(CL3)` statuses.<br>• Rule 10: ECAD gate named to H-Phase 3 start. |
| 1.0.0 | — | Initial hardware agent policy (approved with HW-PLAN v1.0.0). |

