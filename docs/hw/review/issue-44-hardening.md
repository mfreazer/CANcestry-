# H-05 (#44) hardening review record — placeholder fail-closed, Rule 14, and mint workflow draft

Reviewer-facing record for the H-05 hardening follow-up. Kept in the repository
because the automation token cannot comment on issues (HTTP 403), so the PR body
and this file constitute the official review trail of record.

This change hardens the visual evidence and traceability gates following the
merge of Landing 1 (PR #46, commit `1e053ac`), resolves the latent placeholder
finding, introduces Rule 14 into `HwAGENTS.md`, and drafts the CI mint workflow
in §4 as the prerequisite for Landing 2.

---

## 1. Placeholder fail-closed disposition (closing the latent finding)

### 1.1 Context and landed decision
In Landing 1 (`docs/hw/review/issue-44-body.md` §1 Q1), the agent audited the
ledger and identified that no row in `hw/tests/traceability.csv` carried
`LA_PENDING` or `CAP_PENDING`. Both tracked rows use real, verified Capella
element IDs from `hw/model/capella/cancestry.capella`:
- `HW-SF-002` -> `la-comp-retentiondomain`
- `HW-FR-004` -> `la-comp-powersupervisor`

The agent recorded a latent finding: `ci/check_hw_traceability.py` (lines
521–524) explicitly no-oped placeholder values (`if row["capella_element_id"] in
CAPELLA_PLACEHOLDERS: pass`), meaning any row claiming a placeholder passed the
gate without validation.

The placeholder decision is now landed with PR #46. With actual element IDs
established, the QA condition requires that placeholders fail closed rather than
relying on future deferral to H-06.

### 1.2 Enforcement implementation
`ci/check_hw_traceability.py` now enforces two complementary fail-closed rules:

1. **Passing rows cannot use placeholders (Rule 4 / honest ledger):**
   If a ledger row has status `passing*` or `fully-verified*` and specifies a
   placeholder (`CAP_PENDING` or `LA_PENDING`), the gate immediately fails:
   ```text
   rule 2: line <N>: <req> / <method>: passing row carries placeholder capella_element_id <id> (honest ledger: passing evidence requires resolved Capella linkage)
   ```
   Pending rows (`sim-pending`, `bench-pending`) may retain placeholders only
   while MBSE component allocation is actively underway.

2. **Model element cross-validation:**
   When `hw/model/capella/cancestry.capella` is present in the repository,
   `ci/check_hw_traceability.py` extracts all declared element IDs via standard
   library XML parsing (no external dependencies, fully operational outside the
   heavy Docker image). Any non-placeholder `capella_element_id` not found in
   the Capella model fails closed:
   ```text
   rule 2: line <N>: <req> / <method>: capella_element_id <id> does not exist in the Capella model (hw/model/capella/cancestry.capella)
   ```

### 1.3 Test coverage
`tests/unit/tools/test_check_hw_traceability.py` pins this behavior with four
new dedicated tests:
- `test_passing_row_rejects_capella_placeholder`: asserts rejection of `CAP_PENDING` and `LA_PENDING` on passing rows.
- `test_pending_row_permits_capella_placeholder`: asserts permitted use on pending rows.
- `test_unknown_capella_element_id_fails_closed`: asserts rejection when an ID does not exist in the model.
- `test_known_capella_element_id_passes`: asserts acceptance of valid model element IDs.

---

## 2. Rule 14 and the binding tolerance band rule

### 2.1 Principle and policy
In Landing 1 review (`docs/hw/review/issue-44-body.md` §1 Q2), the relationship
between data envelopes and visual bands was clarified: a shaded or hatched band
is an engineering contract, not decorative illustration.

`HwAGENTS.md` is updated to version 1.1.3, adding **Rule 14**:

> **14. Tolerance bands are binding contracts.** A shaded or hatched tolerance
> band in a rendered visual is a binding verification contract, never an
> aesthetic illustration or decorative fill. A visual shall display a tolerance
> band only when backed by an explicit `tolerance_band` definition in the
> plot-data file referencing validated `tolerance_lower` and `tolerance_upper`
> series that share an x-axis and are non-inverted. When requirement tolerances
> are scalar feature limits (e.g. peak voltage or rise time) rather than
> continuous per-sample envelopes, the visual shall encode them as labelled glyph
> markers with explicit tolerances in the evidence metadata—never as a
> synthesized continuous band. Drawing an undeclared or ungrounded band is a gate
> error and shall be rejected at review.

### 2.2 Checker enforcement (`ci/check_hw_evidence.py`)
`ci/check_hw_evidence.py` enforces Rule 14 across both plot-data semantics and
rendered SVG vector elements:

1. **Plot-data semantics:** If any series in a plot-data file declares the role
   `tolerance_lower` or `tolerance_upper` but no top-level `tolerance_band`
   block is declared, the checker fails with:
   ```text
   plot-data-semantics: <file>: series <names> declared with tolerance role but no tolerance_band is defined (HwAGENTS.md rule 14: tolerance bands are binding contracts)
   ```
2. **Undeclared band in SVG:** If an SVG contains a hatched tolerance band
   pattern (`fill="url(#hatch..."`) while `tolerance_band` is not declared in the
   accompanying plot-data file, the visual evidence gate fails closed:
   ```text
   visual-semantics: <file>: SVG contains a hatched tolerance band polygon but no tolerance_band is declared in plot-data (HwAGENTS.md rule 14: tolerance bands are binding contracts)
   ```
3. **Missing band in SVG:** If plot-data defines `tolerance_band` but the SVG lacks
   the hatched fill polygon, the gate fails closed:
   ```text
   visual-semantics: <file>: plot-data declares tolerance_band but SVG has no matching hatched tolerance band polygon (HwAGENTS.md rule 14)
   ```

### 2.3 Application to Pulse 5b
Pulse 5b tolerance is strictly scalar (peak voltage within 2 %, time-to-peak
within 5 %). In Landing 2, Pulse 5b will be rendered with **two series and no
`tolerance_band`**. The scalar limits will be drawn as distinct, labelled glyph
markers at the peak and time-to-peak samples. No synthetic continuous band
shall be drawn for Pulse 5b.

---

## 3. What this hardening change contains

| Commit | Contents |
|---|---|
| Commit 1 | `HwAGENTS.md` (Rule 14 added, v1.1.3); `docs/hw/visual-evidence-plan.md` (Rule 14 binding band rule in §3 and §7, v0.1.1); `ci/check_hw_traceability.py` (placeholder fail-closed and Capella ID cross-check); `ci/check_hw_evidence.py` (Rule 14 semantic checks); unit test extensions in `tests/unit/tools/test_check_hw_traceability.py` and `tests/unit/tools/test_check_hw_evidence.py`. |
| Commit 2 | `docs/hw/review/issue-44-hardening.md` (this record, including §4 mint workflow draft). |

### Verification performed
```bash
python3 ci/check_schemas_valid.py schemas               # PASS (17/17 schemas valid)
python3 ci/check_hw_contracts.py .                      # PASS
python3 ci/check_hw_traceability.py .                   # PASS (4 rows, 0 errors)
python3 ci/check_hw_evidence.py --root . --rerender --strict  # PASS (0 errors, 0 warnings)
pytest tests/unit/tools                                 # 480 passed, 1 skipped
```

---

## 4. Mint workflow draft (Landing 2 prerequisite)

> **QA Condition Notice:** Per explicit QA Lead directive, Landing 2 (the
> execution of the mint workflow to produce the Holdup and Pulse 5b plot-data,
> SVGs, manifest entries, and ledger rows) **SHALL NOT START** until this draft
> has been reviewed and authorized.

Below is the draft specification for `.github/workflows/hw-mint.yml`,
incorporating all five Landing 2 gating rules:

### 4.1 Specification of `.github/workflows/hw-mint.yml`

```yaml
name: hw-mint

on:
  pull_request:
    types: [labeled, synchronize]

# Rule 3: Concurrency serialisation
concurrency:
  group: mint-${{ github.workflow }}-${{ github.event.pull_request.number || github.ref }}
  cancel-in-progress: true

jobs:
  mint-visual-evidence:
    # Rule 1: Label gating (runs only when explicitly authorized by maintainers)
    if: contains(github.event.pull_request.labels.*.name, 'mint:visual-evidence')
    runs-on: ubuntu-latest
    container:
      image: ghcr.io/mfreazer/cancestry-hw-toolchain:pinned
    permissions:
      contents: write
      pull-requests: write

    steps:
      - name: Checkout repository
        uses: actions/checkout@v4
        with:
          ref: ${{ github.event.pull_request.head.ref }}
          fetch-depth: 0

      # Rule 4: Mint provenance recording
      - name: Record and verify toolchain provenance
        id: provenance
        run: |
          set -euo pipefail
          EXPECTED_IMAGE_DIGEST=$(cat ci/docker/base-image.digest)
          OMC_VER=$(omc --version | tr '\n' ' ')
          FMPY_VER=$(python3 -c "import fmpy; print(fmpy.__version__)")
          PYTHON_VER=$(python3 --version)
          RENDERER_DIGEST=$(sha256sum ci/renderers/cancestry-render-modelica.py | cut -d' ' -f1)
          COMMIT_EPOCH=$(git log -1 --format=%ct)

          echo "image_digest=${EXPECTED_IMAGE_DIGEST}" >> $GITHUB_OUTPUT
          echo "omc_version=${OMC_VER}" >> $GITHUB_OUTPUT
          echo "fmpy_version=${FMPY_VER}" >> $GITHUB_OUTPUT
          echo "python_version=${PYTHON_VER}" >> $GITHUB_OUTPUT
          echo "renderer_digest=sha256:${RENDERER_DIGEST}" >> $GITHUB_OUTPUT
          echo "source_date_epoch=${COMMIT_EPOCH}" >> $GITHUB_OUTPUT

      # Execution step 1: Modelica simulation & evidence generation
      - name: Run Modelica simulations and generate evidence artifacts
        env:
          SOURCE_DATE_EPOCH: ${{ steps.provenance.outputs.source_date_epoch }}
        run: |
          set -euo pipefail
          python3 -m pytest hw/tests/test_power_sim.py -v
          python3 -m pytest hw/tests/test_pulse_sim.py -v

      # Execution step 2: Render views via cancestry-render-modelica
      - name: Render deterministic SVG views
        run: |
          set -euo pipefail
          python3 ci/renderers/cancestry-render-modelica.py \
            --plot-data hw/tests/evidence/holdup_decay_001.plot.json \
            --version 0.1.0 \
            --output hw/tests/evidence/renders/holdup_decay_001.svg

          python3 ci/renderers/cancestry-render-modelica.py \
            --plot-data hw/tests/evidence/pulse_5b_001.plot.json \
            --version 0.1.0 \
            --output hw/tests/evidence/renders/pulse_5b_001.svg

      # Rule 5: Pulse honesty verification
      - name: Enforce pulse honesty invariant
        run: |
          set -euo pipefail
          python3 -c "
          import json
          pulse_ev = json.load(open('hw/tests/evidence/pulse_7637_001.json'))
          assert pulse_ev.get('pass') is False, 'Pulse evidence must land pass=false'
          assert pulse_ev.get('status') == 'pending', 'Pulse evidence must land status=pending'
          assert pulse_ev.get('credibility_level') == 'CL0', 'Pulse evidence must land CL0'

          pulse_plot = json.load(open('hw/tests/evidence/pulse_5b_001.plot.json'))
          assert pulse_plot.get('status') == 'pending', 'Pulse plot-data must land status=pending'
          assert 'tolerance_band' not in pulse_plot, 'Rule 14: Pulse 5b must not declare a tolerance_band'
          "

      # Rule 2: Divergence handling and gates
      - name: Validate evidence and traceability gates
        run: |
          set -euo pipefail
          python3 ci/check_schemas_valid.py schemas
          python3 ci/check_hw_contracts.py .
          python3 ci/check_hw_traceability.py .
          python3 ci/check_hw_evidence.py --root . --rerender --strict

      - name: Detect divergence against committed tree
        id: diff
        run: |
          if git diff --quiet hw/tests/evidence/; then
            echo "drift=0" >> $GITHUB_OUTPUT
          else
            echo "drift=1" >> $GITHUB_OUTPUT
            git diff --stat hw/tests/evidence/
          fi

      - name: Fail closed on undeclared divergence
        if: steps.diff.outputs.drift == '1' && !contains(github.event.pull_request.body, 'renderer-drift:')
        run: |
          echo "ERROR: Mint step produced bytes differing from workspace without a declared 'renderer-drift:' reason."
          exit 1
```

### 4.2 Detailed analysis of the five landing rules

1. **Label Gating (`mint:visual-evidence`):**
   Automated generation of repository evidence artifacts consumes significant
   simulation time and generates repository commits. It shall only trigger when
   the PR carries the label `mint:visual-evidence`. GitHub repository permission
   rules restrict label application to project maintainers and the QA Lead.
   Unauthorized contributors cannot trigger automated mint runs.

2. **Divergence Handling:**
   If the mint step produces evidence or SVG bytes that differ from committed
   files, the job inspects the PR body for a line-anchored `renderer-drift:
   <reason>` declaration (as enforced by `HW-EVIDENCE-GATE-DECL-001`). If
   undeclared, the workflow fails closed (exit 1) and outputs a detailed diff,
   preventing silent mutation of verification artifacts.

3. **Concurrency and Race Prevention:**
   The workflow assigns `concurrency.group` keyed on the PR number with
   `cancel-in-progress: true`. If a contributor pushes additional commits while a
   mint run is executing, the obsolete run is immediately cancelled, preventing
   split-brain commits or conflicting commit-backs.

4. **Mint Provenance:**
   The workflow explicitly captures the base image digest from
   `ci/docker/base-image.digest`, OpenModelica compiler version, FMPy version,
   Python version, and renderer script SHA256 digest. Timestamps are fixed
   via `SOURCE_DATE_EPOCH` anchored to the latest commit epoch, ensuring
   byte-level reproducibility.

5. **Pulse Honesty Guard:**
   Pulse 5b qualification remains pending bench testing (per `HW-PLAN` §10.4).
   The mint workflow contains a dedicated invariant assertion checking that
   `pulse_7637_001.json` and `pulse_5b_001.plot.json` declare `pass: false`,
   `status: pending`, and `credibility_level: CL0`, and asserts that no
   `tolerance_band` is declared for Pulse 5b under Rule 14. The mint workflow
   is programmatically barred from promoting Pulse 5b to passing.

### 4.3 Review hold
Landing 2 will not proceed until this draft specification is formally reviewed
and approved by QA.
