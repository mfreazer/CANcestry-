## Deferred contract correction from H-04 / PR #39

**Owner:** @mfreazer (maintainer / shared schema-gate change approval).
**Target:** Hardware v0.1 (virtual bench proven), before hardware URN schemas / visual-evidence contracts are adopted.
**Requirement coverage:** HW-SF-002, HW-FR-009 (parameter provenance); SW-FR-TOOL-008 (shared schema validation).

H-04 requests `urn:cancestry:schema:hw:datasheet-extract-0.1.0`, but the shared `ci/check_schemas_valid.py` requires a `.schema.json` suffix. The user explicitly chose to leave software gates untouched in PR #39. Retaining the current URL in that PR is therefore an authorized deferral, not a completed URN migration.

### Acceptance criteria
- [ ] Approve a narrowly scoped shared-validator change that recognizes versioned hardware URNs while preserving software URL IDs, version matching, invalid-schema rejection and existing software gate behavior.
- [ ] Add positive and negative URL/URN/version-mismatch fixtures; no validation-depth bypass.
- [ ] Migrate the datasheet schema to the exact requested URN; reconcile any related URN migrations required by #36 in one controlled plan.
- [ ] Update consumers/resolvers, documentation/version records, and deterministic evidence hashes together.
- [ ] All affected CI checks green; maintainer/QA review before closure.

This issue is the authoritative tracking record for the deferral in #38 / #39. Do not silently implement the shared software change in H-04.

## F2 — complete hardware schema-ID migration inventory

Inventory checked against PR #39 source baseline `67d06136a2f857579f4bde3de29f15fe32636a13` (2026-09-20). **All eight existing hardware schema IDs are in scope for the follow-up decision**, not just the datasheet extract. The target IDs below follow the hardware URN convention requested by #38/#36; changing them still requires the separate shared-gate approval. This inventory does not authorize implementation in H-04.

| Existing schema file | Current `$id` | Target `$id` after approved migration |
|---|---|---|
| `schemas/hw/hw-bom-0.1.0.schema.json` | `https://cancestry.dev/schemas/hw/hw-bom-0.1.0.schema.json` | `urn:cancestry:schema:hw:bom-0.1.0` |
| `schemas/hw/hw-bridge-0.1.0.schema.json` | `https://cancestry.dev/schemas/hw/hw-bridge-0.1.0.schema.json` | `urn:cancestry:schema:hw:bridge-0.1.0` |
| `schemas/hw/hw-datasheet-extract-0.1.0.schema.json` | `https://cancestry.dev/schemas/hw/hw-datasheet-extract-0.1.0.schema.json` | `urn:cancestry:schema:hw:datasheet-extract-0.1.0` |
| `schemas/hw/hw-oracle-registry-0.1.0.schema.json` | `https://cancestry.dev/schemas/hw/hw-oracle-registry-0.1.0.schema.json` | `urn:cancestry:schema:hw:oracle-registry-0.1.0` |
| `schemas/hw/hw-pulse-evidence-0.1.0.schema.json` | `https://cancestry.dev/schemas/hw/hw-pulse-evidence-0.1.0.schema.json` | `urn:cancestry:schema:hw:pulse-evidence-0.1.0` |
| `schemas/hw/hw-sim-0.1.0.schema.json` | `https://cancestry.dev/schemas/hw/hw-sim-0.1.0.schema.json` | `urn:cancestry:schema:hw:sim-0.1.0` |
| `schemas/hw/hw-traceability-0.1.0.schema.json` | `https://cancestry.dev/schemas/hw/hw-traceability-0.1.0.schema.json` | `urn:cancestry:schema:hw:traceability-0.1.0` |
| `schemas/hw/hw-trade-0.1.0.schema.json` | `https://cancestry.dev/schemas/hw/hw-trade-0.1.0.schema.json` | `urn:cancestry:schema:hw:trade-0.1.0` |

**Planned, not present on this branch:** `schemas/hw/hw-plot-data-0.1.0.schema.json`, target `urn:cancestry:schema:hw:plot-data-0.1.0`, is specified by #36. Coordinate that addition and #36's traceability-ID correction with this migration; do not pretend the plot-data schema already exists.

### Consumers / references / evidence to migrate or revalidate

1. **Shared gate:** `ci/check_schemas_valid.py::check_file` currently extracts an ID version only from `-<semver>.schema.json`. Approve explicit recognition of the hardware URN namespace with the same filename/version checks; retain all Draft 2020-12 logical validation and existing software-ID behavior. Update `tests/unit/tools/test_check_schemas_valid.py` with URL and URN positives, malformed/unknown namespace and version-mismatch negatives. No generic skip, suffix bypass or weaker validation.
2. **File-based loaders:** `ci/check_hw_contracts.py::load_contract` (registry, bridge, trades, extracts); `ci/check_capella_model.py::check_bridge_json` (live-inventory schema copy); `ci/check_hw_traceability.py` (row schema, tool metadata / witness `$defs`, pulse qualification); and `hw/tests/test_power_sim.py::_validate_against_schema` / `hw/tests/test_pulse_sim.py` (BOM, extracts, cases and pulse manifest). These read versioned schema filenames, not remote URLs. Preserve those filenames and verify offline instance validation after the ID change.
3. **Reference resolution inventory:** current hardware `$ref` values are document-local `#/$defs/...` fragments only: five distinct fragments in BOM (`bom`, `datasheet_extract`, `datasheet_ref`, `fit_source`, `parameter`), `tool_validation_reference` in traceability, and `pulse_coverage` in pulse evidence. The other five schema documents have no `$ref`. There are no current cross-file/network `$ref` values in this hardware schema set. Confirm local fragments still resolve under URN bases; future cross-file URNs need an explicit local registry, not network fetching. Recheck this inventory at implementation time.
4. **Instances and exports:** `hw/bom/bom.json`, every `hw/bom/datasheets/*.json`, `hw/model/{bridge,trades}.json`, `hw/tests/oracles/registry.json`, both `hw/tests/cases/*.simcase.json`, `hw/tests/traceability.csv` and both evidence manifests. Preserve their version fields, qualification statuses and physical values. Revalidate generated oracle CSV / Markdown views; an identity migration is not evidence promotion.
5. **Hash chain:** `hw/tests/test_evidence_determinism.py` pins schema paths in both canonical manifests. Reissue `hw/tests/evidence/{holdup_001,pulse_7637_001}.json` against the migrated source schemas, update the passing ledger row's `evidence_sha256`, and verify pending pulse source hashes too. Hash regeneration is not simulation verification; rerun all affected checks and actual FMU regressions. Do not add a closure-evidence reference to a pending ledger row.
6. **Fixtures / records / CI:** revalidate `test_check_hw_contracts.py`, `test_check_capella_model.py`, `test_check_hw_traceability.py`, the hardware tests, `docs/hw/h04-enforcement.md`, this issue's inventory and `docs/versions.md`. Exercise the recursive schema scan in `hw-fast` and the unchanged software consumers in `pr-fast`; record per-commit results.

**Deliberately unchanged identities:** the eight software schemas outside `schemas/hw/` keep their current URL IDs. The Draft 2020-12 `$schema` dialect URI, real datasheet/standard `source_url` values and source citations remain URLs. Do not blanket-replace `https://`. Do not rename `.schema.json` files merely because their `$id` becomes a URN.

The follow-up is complete only after this whole inventory is migrated or explicitly dispositioned with a rationale, all consumers/hash chains are consistent, and maintainer/QA approves the separately scoped change.
