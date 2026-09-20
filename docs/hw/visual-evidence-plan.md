# CANcestry Visual Evidence Plan

| Field | Value |
|---|---|
| **Document** | CANcestry Visual Evidence Plan |
| **Version** | 0.1.0 |
| **Status** | Draft — H-03, pending QA approval |
| **Owner** | System Engineer |
| **Approver** | QA Lead |
| **Last Review** | 2026-09-20 |
| **Repository location** | `docs/hw/visual-evidence-plan.md` |
| **Governing documents** | `docs/hw/HW-PLAN.md` v1.0.0 (sections 10.1–10.3, 10.8), `HwAGENTS.md` rule 13 |

## 1. Principle

A picture is not evidence. In the hardware program the physical truth is,
for now, the Modelica model in `hw/model/CancestryLib/`; a rendered SVG is a
*view* of an evidence artifact that already exists and is already hashed. The
view can be wrong (bad renderer, wrong data, stale file) and the viewer cannot
tell by looking. Therefore:

> **The picture is never the proof; the hash chain is.**

`ci/check_hw_evidence.py` enforces this fail-closed. Rendering a plot creates
no evidence, raises no credibility level, and closes no requirement. A visual
without provenance is an assertion and is rejected at review
(`HwAGENTS.md` rule 13).

## 2. The hash chain

```
   source artifact(s)                hw/tests/evidence/<case_id>.json
   (models, cases, oracles,  ---->   pins every source by sha256,
    schemas, toolchain)              records pass/oracle/credibility
            |                                   |
            |                                   v
            |                        hw/tests/evidence/<plot_id>.plot.json
            |                        (schema: hw-plot-data-0.1.0) pins the
            |                        evidence by path + sha256 and carries
            |                        the renderer identity + digest
            |                                   |
            |                                   v
            |                        hw/tests/evidence/renders/<plot_id>.svg
            |                        embedded cancestry-provenance comment +
            |                        visual provenance footer
            |                                   |
            v                                   v
   hw/tests/traceability.csv      hw/tests/evidence/renders/manifest.json
   (ledger: evidence hash)        (plot-data <-> SVG <-> renderer hashes)
```

Three independent gates:

| Gate | Tool | Answers |
|---|---|---|
| Honest ledger | `ci/check_hw_traceability.py` | Is the requirement traced, oracled and hashed? |
| Visual evidence | `ci/check_hw_evidence.py` | Is the picture a faithful view of that hashed evidence? |
| Renderer determinism | `ci/check_hw_evidence.py --rerender --strict` (nightly) | Does the committed renderer reproduce the committed bytes? |

## 3. The plot-data contract

Every SVG, HTML page or PDF committed under `hw/tests/evidence/` must be
rendered from exactly one plot-data file conforming to
`schemas/hw/hw-plot-data-0.1.0.schema.json`, named
`hw/tests/evidence/<plot_id>.plot.json`. The file is the render input and the
only thing a renderer is allowed to read. It pins:

- identity: `plot_id`, `plot_version`, optional title/description;
- provenance: `source_evidence_path` + `source_evidence_hash` (the evidence
  JSON this view belongs to), `source_requirement`, `oracle_id`;
- disposition: `method`, `credibility_level`, `status`, `provisional` — copied
  from, and checked against, the source evidence artifact;
- the renderer: `renderer_tool`, `renderer_tool_version`, `renderer_tool_digest`;
- the data: `series` (each with `name`, `role`, `x`, `y`, `x_units`,
  `y_units`, `non_color_encoding`) and an optional `tolerance_band` that
  references its two band series **by name**.

Numeric properties the checker enforces beyond schema validation
(`plot-data-semantics`):

| Property | Severity |
|---|---|
| `x` and `y` have equal length in every series | error |
| every series has at least one sample in each of the two roles' domains | error |
| `x` is strictly increasing within a series | warning |
| the tolerance band's `lower` never exceeds its `upper` at a shared sample | error |
| `tolerance_band.lower_series` / `upper_series` name existing series whose `role` is `tolerance_lower` / `tolerance_upper` | error |
| series names are unique | error |

The plot-data file never contains a wall-clock timestamp: `generated_at` is
optional and, when present, must be `SOURCE_DATE_EPOCH`-derived or copied from
an already-hashed input, so re-rendering the same plot-data is byte-identical.

## 4. Provenance footer and embedded comment

Each rendered artifact carries the provenance **twice**: machine-readably in an
XML comment, and visibly in the footer a human reviewer reads.

### 4.1 Embedded comment (schema)

```xml
<!-- cancestry-provenance
plot_id=<...>
plot_version=<...>
plot_data_hash=sha256:<64-hex>
status=<passing|pending|failing>
method=<analysis|sim|virtual_bench|protocol|bench>
credibility_level=<CL0|CL1|CL2|CL3>
provisional=<true|false>
source_evidence_path=<...>
source_evidence_hash=sha256:<64-hex>
oracle_id=<...>
renderer_tool=<...>
renderer_tool_version=<semver>
renderer_tool_digest=sha256:<64-hex>
generated_at=<ISO 8601 or absent>
-->
```

### 4.2 Footer lines

The footer is rendered as visible text. `ci/check_hw_evidence.py` hard-codes
the five required lines below (`FOOTER_LINES`); they are matched as ordered
whitespace-separated tokens after XML-unescaping, and each MUST agree with the
embedded comment. If this template and the checker ever diverge, the checker is
wrong and this document wins.

The literal marker `[,provisional]` in the STATUS template expands to
`,provisional` when the plot-data declares `provisional: true` and to nothing
when it declares `false`; nothing else in the template is conditional. The
checker decodes XML entities (named and numeric) before comparing, so a
renderer may escape footer values.

```
EVIDENCE   {plot_id}@{plot_version}
STATUS     {status}({method},{credibility_level}[,provisional])
PLOT-DATA  {plot_data_hash}
ORACLE     {oracle_id}
RENDERER   {renderer_tool} {renderer_tool_version}
```

A renderer MAY emit the two additional lines below between the required ones;
when present they must agree with the embedded comment
(`SOURCE  {source_evidence_path} {source_evidence_hash}`,
`GENERATED  {generated_at}`, the latter omitted when `generated_at` is absent).
Extra lines are optional for this revision and are not a substitute for the
five required lines.

### 4.3 Sidecar (optional)

A renderer MAY commit `hw/tests/evidence/renders/<plot_id>.svg.provenance.json`
containing the same key/value pairs as the embedded comment. When present it
must match the comment and the manifest entry, otherwise the checker reports a
`manifest` error. The sidecar is a convenience for tooling; it is never the
authority — the comment inside the SVG is.

## 5. Failure rendering

A visual that hides a failure is worse than no visual.

- The `status` in the plot-data file and the footer is the disposition of the
  **source evidence**, not the outcome of the render. `pending` and `failing`
  plots are rendered and committed under their true status; the checker
  rejects a plot-data file whose `status`/`provisional`/`credibility_level`/
  `oracle_id` disagree with the evidence JSON that it pins
  (`status-consistency`).
- Pending/failing renders carry a visible status banner drawn with the same
  non-color encoding rules as the data, plus the footer line, so a screenshot
  cannot be mistaken for a passing result.
- The tolerance band is shaded with a hatch pattern, never with color alone.
- Missing data is rendered as missing (no interpolation across a gap, no
  silently dropped series); the checker requires every declared series and
  rejects ragged `x`/`y` pairs rather than letting a renderer guess.
- A plot may fail to render. That is a build failure, not an excuse to commit a
  partial or hand-edited SVG: the committed bytes must be reproducible from the
  committed plot-data by the committed renderer (`--rerender`).

## 6. Renderer drift

Renderer drift is a change in rendered bytes when the plot-data and its pinned
inputs have not changed. It is a **non-evidence** change, and the rule is to
make it visible rather than to silently re-render:

- The manifest (`hw/tests/evidence/renders/manifest.json`) records, per plot,
  `plot_data_sha256`, `expected_renderer_tool`,
  `expected_renderer_tool_version`, `expected_renderer_tool_digest`,
  `expected_svg_sha256` and `last_updated`.
- On the fast path, `ci/check_hw_evidence.py` verifies the manifest against the
  committed bytes (plot-data hash, SVG hash, and the renderer digest when the
  renderer is present) but does not re-render.
- On the nightly path, `--rerender --strict` re-runs each renderer
  (`<renderers-dir>/<renderer_tool>.py --plot-data <path> --version
  <renderer_tool_version> --output -`, stdout is the SVG) and compares the
  bytes. A difference is a `renderer-drift` error unless the PR body *declares*
  it -- a line whose first text is `renderer-drift:` followed by the reason --
  and `RENDERER_DRIFT_ALLOWED=1` is set, in which case it is reported as a
  warning that a reviewer must disposition. A mention of the phrase elsewhere
  in the body is not a declaration, and neither is a bare prefix with no
  stated reason; the parser in `.github/workflows/hw-fast.yml` is pinned by
  `HW-EVIDENCE-GATE-DECL-001` so that both stay fail-closed.
- A missing renderer is a warning on the fast path and a hard setup failure
  (exit 2) under `--strict`: the nightly job exists precisely to prove the
  renderers still reproduce the committed bytes.
- Renderers are deterministic by construction: fixed seeds, no wall-clock
  timestamps (`SOURCE_DATE_EPOCH` when a timestamp is unavoidable), pinned
  dependencies, and dependency digests recorded in `ci/docker/`.
- Renderers live in `ci/renderers/<renderer_tool>.py`. The checker's
  `--renderers-dir` flag defaults to that directory (issue #36 section 5.3,
  option A) so fixtures can point at a stub directory without shipping a
  parallel `ci/renderers/` tree.

## 7. Accessibility

- **No color-only encoding.** Every series declares `non_color_encoding` with a
  `line_style` and a `marker`; the schema requires it and the renderer must use
  it. Distinguishability must survive greyscale printing.
- **Greyscale-safe band.** The tolerance band is hatched, not tinted.
- **Status is textual.** Status is carried by the footer text and a banner, not
  by red/green alone.
- **Limits and faults are labelled.** Threshold, floor and fault markers carry
  a glyph and a text label, not just a color change.
- **Readable at review size.** Axis labels, units and the footer are rendered
  at a size that stays legible when the SVG is embedded in a PR comment.
- **Self-describing document.** Rendered artifacts carry an SVG `<title>` and
  `<desc>` and are pure vector text; no rasterized screenshots are committed as
  evidence views.

## 8. Integration with the traceability gate

- The ledger (`hw/tests/traceability.csv`) links a requirement to its evidence
  JSON and its hash; `ci/check_hw_traceability.py` owns that link. The visual
  evidence gate never changes a ledger row.
- A plot inherits the disposition of its evidence. `pending`/`failing` evidence
  can only produce `pending`/`failing` plot-data; a `passing` plot-data file
  must pin evidence that records `pass: true` with a matching oracle,
  requirement and credibility level, or the checker fails (`status-consistency`,
  `hash-chain`).
- Requirement closure remains governed by `HwAGENTS.md` rule 4 and
  `docs/hw/HwRS.md` section 1: a T1/T2 safety closure stays
  `passing(sim,CLn,provisional)` and only T4 correlation promotes it to
  `fully-verified(CL3)`. Rendering an overlay does not change that status, and
  a plot may not be cited as closure evidence for a row that is not passing.
- Tool confidence is unchanged by rendering. The renderer is a pure function of
  hashed inputs; its classification is recorded in
  `docs/hw/tool-qualification.md`, and any producer gap inherited from the
  Modelica toolchain stays with the evidence, not with the picture.

## Change log

| Version | Date | Change |
|---|---|---|
| 0.1.0 | 2026-09-20 | H-03 #36: initial plan — hash chain, plot-data contract, footer/comment contract, failure rendering, renderer drift, accessibility, traceability integration. |
