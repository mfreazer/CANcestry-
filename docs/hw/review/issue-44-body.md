# H-05 (#44) landing 1 review record — the renderer

Reviewer-facing record for the first H-05 landing. Kept in the repository
because the automation token cannot comment on issues (HTTP 403).

Landing 1 is the piece of #44 that does not need the FMU toolchain: the
renderer, its tests, and the tool-qualification classification. The two plots
and the mint step are **landing 2** and are a separate decision (§5).

## 1. Answers to the review questions

### Q1 (B5) — the `LA_PENDING` ledger row: outcome 1, plus a latent finding

The ledger row, verbatim:

```
HW-SF-002,sim,la-comp-retentiondomain,CL2,OR-001,CL3,"passing(sim,CL2,provisional)",hw/tests/evidence/holdup_001.json,sha256:290daa59a1d8ba6b27d9d99170b400b379d130baf4f275a989bf8188d17f68b5,openmodelica: Compiler semantics outside independently validated output remain unqualified; OR-001/OR-002 regressions do not cover all translation and solver behavior.
```

- `capella_element_id` is `la-comp-retentiondomain`, a real element id in
  `hw/model/capella/cancestry.capella` (`id="la-comp-retentiondomain"`, one of
  six LA component ids in the seed). **No row in `hw/tests/traceability.csv`
  carries `LA_PENDING` or `CAP_PENDING`**; the issue text's `LA_PENDING`
  instruction was stale, and regressing the row to it would have been a
  downgrade.
- Rule 7 (`ci/check_capella_model.py::check_safety_linkage`) does **not** read
  the ledger column. It walks the Capella model's own trace graph from each
  `HW-SF-*` requirement to a LA component carrying the typed
  `safety_mechanism` marker, failing closed on broken relations, on cycles
  conferring coverage, and on container membership. The gate is green on
  `d162af0` (`hw-fast` run `35537019553`, gate 2.5) — locally it cannot run,
  because `capellambse` lives only in the pinned image.
- **Latent finding, recorded not shrugged:** nothing cross-validates the
  ledger's `capella_element_id` against the model's element ids, and
  `ci/check_hw_traceability.py` (lines 521-524) explicitly *no-ops* the
  placeholder values — "they pass the non-empty ledger check but are not
  resolved against a Capella model". Today no row uses a placeholder, so rule 7
  has no hole on this tree; the day a row does, the ledger would claim a
  linkage that nothing verifies. That cross-check is exactly the extension the
  H-05 issue assigns to H-06, and this record is the request to keep it there.

### Q2 (B2) — the holdup plot-data shape: four series with a declared band

The schema enforces the relationship in both directions (`allOf`): a
`tolerance_lower`/`tolerance_upper` series requires `tolerance_band`, and a
`tolerance_band` requires both roles; `ci/check_hw_evidence.py::
check_tolerance_band` then fails closed unless the two names resolve to series
with exactly those roles, share an x axis, and are non-inverted. The renderer
draws a band **only** when `tolerance_band` is declared, so a shaded band that
was not declared cannot be produced: it would be a gate error, never a silent
pass.

The verified pipeline output (local proof, preview attached to the PR) is four
series — `model`, `oracle`, `band_lower`, `band_upper` — with

```json
"tolerance_band": {"lower_series": "band_lower", "upper_series": "band_upper",
                   "description": "±1 mV per sample, per OR-001",
                   "hatch": "///", "units": "V"}
```

**Pulse 5b is deliberately different, and this is the record before the mint
runs:** its tolerance is scalar (peak within 2 %, time-to-peak within 5 %), not
a per-sample band. The pulse plot will therefore declare **two series and no
`tolerance_band`**, and the limits will be drawn as labelled glyph markers at
the peak and at time-to-peak, with the relative tolerances carried in the
evidence JSON and the plot description. The labelled-feature-limit drawing is
landing 2 work, added with the plot that needs it. A ±1 mV-style band must
never be drawn for pulse just because the holdup plot has one.

### Q3 (B1) — where the evidence hash lives: plot-data, manifest, ledger

The hash lives in the artifacts written *after* the evidence file, and nowhere
else:

| Artifact | Field | What it pins |
|---|---|---|
| plot-data | `source_evidence_hash` | the evidence JSON bytes |
| manifest entry | `plot_data_sha256`, `expected_svg_sha256`, renderer digest | the view and the tool |
| ledger row | `evidence_sha256` | the same evidence JSON, for the honest-ledger gate |

The evidence JSON carries **no self-hash** — a file containing the sha256 of
its own final bytes is a fixed-point problem, and the issue text's "sha256 of
this file itself, computed after writing" is corrected to the arrangement
above. What the evidence JSON does carry is `source_hashes`: the sha256 of
every *upstream input* (schemas, Modelica model, BOM extracts, oracle scripts,
sim case, tool pin documents), which `ci/check_hw_traceability.py` rule 7
re-verifies on every run — evidence drift is a gate failure, not a warning.

### N1 — disposition

Deleted and recorded. See `docs/hw/review/issue-36-body.md` §3: the
stereotype/tokenizing branch was removed from
`ci/check_capella_model.py::is_safety_mechanism` before H-04 merged,
`docs/hw/h04-enforcement.md` records the disposition, and negative tests at
`tests/unit/tools/test_check_capella_model.py:361-385` pin it.

## 2. What landing 1 contains

| Commit | Contents |
|---|---|
| `H-05: deterministic pure-Python renderer for hardware plot-data` | `ci/renderers/cancestry-render-modelica.py`; `tests/unit/tools/test_renderer.py` (14 tests); `cancestry-render-modelica` classified TCL1 in `docs/hw/tool-qualification.md` (v0.2.4); the hash chain that doc edit invalidated re-issued |
| `H-05 review record` | this file and `issue-36-body.md` |

Why pure Python rather than matplotlib: matplotlib's SVG writer is not
byte-stable across runs without `svg.hashsalt` + `metadata={"Date": None}`
(measured locally: two runs produced different bytes; with both mitigations,
identical bytes on 3.9.2), and its output depends on font resolution in the
image. A dependency-free renderer removes the class of failure instead of
mitigating it, and lets the committed SVG be reproduced outside the pinned
image: same input, same bytes, on any Python ≥ 3.10. Determinism is the
property the nightly `--rerender --strict` check asserts, so it is the property
the renderer is built around. The issue permits "matplotlib or similar"; this
is the "similar" that survives review.

Fail-closed behaviour (all exit 2, nothing written): missing schema, invalid
plot-data, stale source-evidence hash, renderer version mismatch, unknown
marker/line-style/hatch, `jsonschema` absent. Non-colour encoding is
structural: each series carries a line style, a hand-drawn marker shape and a
legend entry; the tolerance band is a hatched polygon.

## 3. Verification performed

- `python3 ci/check_schemas_valid.py schemas`, `ci/check_hw_contracts.py .`,
  `ci/check_hw_traceability.py .`, `ci/check_hw_evidence.py --root . --rerender
  --strict` — all PASS.
- `python3 -m pytest tests/unit/tools` — 471 passed, 1 skipped (the skip is the
  Capella test that needs the pinned image's `capellambse`).
- A locally rendered hold-up overlay (`holdup_decay_001`) passes the H-03 gate
  end to end, including `--rerender --strict`, and two consecutive renders hash
  identically. The overlay in that run uses an OR-001-derived model curve as a
  *fixture*: it is a pipeline proof, not evidence, and it is not committed.

## 4. Deviations and disclosures

- **Branch sequencing.** Arena fixes this session to
  `arena/01a0c035-cancestry`, so the renderer could not be committed to another
  branch while PR #45 (H-03) was open. It was committed locally, never pushed,
  and is pushed only after #45 merged, which is why landing 1 appears on the
  same branch as #45. Recorded here rather than discovered by a reviewer.
- **B3 ordering.** The H-05 issue ordered the renderer as commit 3, after two
  commits that need it. Since the renderer exists, the plots in landing 2 can
  be produced by it directly; the deviation is ordering only, and it resolves
  in practice.
- **Records in the repository.** Issue comments return HTTP 403 for this
  automation token, so review records live in `docs/hw/review/` and in PR
  bodies. Fixing that at the token-scope level is a process action outside this
  PR's scope, but it is the third occurrence and should be tracked.

## 5. Landing 2 (separate decision, not in this PR)

Landing 2 mints the two plots in CI and is materially larger than a rendering
change: it introduces a CI process that **writes to the repository**. It needs
its own review with its own written rules:

1. **Label gating** — which label enables the mint, and who may apply it.
2. **Divergence handling** — what happens when a mint run produces bytes that
   differ from the committed evidence, plot-data or SVG (fail the run, open a
   finding, or require a fresh human approval before commit-back).
3. **Concurrency** — how concurrent or repeated mint runs are serialised so two
   runs cannot race on the same commit-back.
4. **Provenance of the mint itself** — the mint run must record the pinned image
   digest and the tool versions it used, so the minted evidence inherits the
   qualification limits of the producers that generated it.
5. **Pulse honesty** — the pulse plot lands `pending`, per the evidence
   (`pass: false`) and rule 6/rule 9; a mint step must not be able to promote
   it.
