# H-04 enforcement implementation notes

Version: 0.1.0 — draft, pending QA and Human Reviewer sign-off.
Issue: [#38](https://github.com/mfreazer/CANcestry-/issues/38).

## Requirement coverage

- HW-SF-001..005: safety-mechanism trace links, bridge completeness and honest
  evidence/qualification boundaries.
- HW-FR-001..003, HW-FR-008..010: structured architectural trades and live
  logical-to-Modelica mappings; no new component selections.
- HW-FR-004: transient-source oracle provenance, invariant assertions and
  pulse-coverage limits.
- HW-SF-002 / HW-FR-009: datasheet provenance and an explicit *planned*
  charge-path fidelity step; no new charge-path simulation case in H-04.

## Authorized scope exception: datasheet schema ID

H-04 requests `urn:cancestry:schema:hw:datasheet-extract-0.1.0`. The existing
shared `ci/check_schemas_valid.py` requires a `.schema.json` URL suffix.
The user explicitly chose to keep software gates untouched rather than
allow a compatibility change. Therefore the existing datasheet schema `$id`
URL is retained; the URN change is **blocked pending a separately approved
shared-validator change**. The PDF hash/fallback and closed-object corrections
are implemented independently. H-04 must not be represented as fully complete
or merged while this acceptance item remains blocked.

## Repository reconciliation

The actual hardware ledger is `hw/tests/traceability.csv`; its evidence hash
column is `evidence_sha256`. `docs/trace/traceability.csv` is a six-column
software-only ledger with no OR-002 row. H-04 corrections apply to the hardware
ledger and do not alter the software gate or frozen HwRS text. The seed OR-002
row already used CL2, not the CL4 mentioned in the issue.

The strict `OR-NNN` schema requires renumbering planned OR-005b to OR-010.
This does not create a measurement: its validation gap explicitly records
that no first-board data exists. `registry.json` is authoritative;
`registry.csv` and the plan tables are deterministic, checked exports.

Trade selections and criterion weights remain null until SE/QA decide them.
The mandatory external-watchdog requirement is not reopened by the trade.
Datasheet PDF hashes are null where no PDF has been retained; URLs are
fallbacks, **not** invented PDF hashes or claims of manufacturer verification.

## Commands

```sh
python3 ci/check_hw_contracts.py .           # validate contracts and view drift
python3 ci/check_hw_contracts.py . --export  # regenerate CSV and Markdown views
python3 ci/check_capella_model.py .         # live inventories and structural gates
python3 ci/check_hw_traceability.py .       # honest ledger / evidence hashes
python3 -m pytest tests/unit/tools/test_check_hw_contracts.py tests/unit/tools/test_check_capella_model.py tests/unit/tools/test_check_hw_traceability.py
python3 -m pytest hw/tests                 # mandatory omc/FMPy in hw-fast image
```

`CANCESTRY_HW_ALLOW_SKIP=1` is only for explicit local development without
OpenModelica. It is never set by hardware CI and is not simulation evidence.
No new physics-model file, rendered visual, Capella component, software runtime
change or software CI-gate change is part of this issue.
