# H-05 (#44) hardening review record — placeholder fail-closed, Rule 14, and mint workflow draft

Reviewer-facing record for the H-05 hardening follow-up. Kept in the repository
because the automation token cannot comment on issues (HTTP 403), so the PR body
and this file constitute the official review trail of record.

> **Governance Note (N9):** Storing review records under `docs/hw/review/` is a
> practical mechanism to maintain an attributable, auditable engineering log
> within Git while the automation token's lack of issue-comment permissions
> (HTTP 403) persists. It does not substitute for branch protection rules,
> cryptographically signed commits, or formal pull request approval states.

This change hardens the visual evidence and traceability gates following the
merge of Landing 1 (PR #46, commit `1e053ac`), resolves the latent placeholder
finding, introduces Rule 14 and Rule 15 into `HwAGENTS.md`, and drafts the CI
mint workflow in §4 as the prerequisite for Landing 2.

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

1. **Passing rows cannot use placeholders (Honest Ledger / HwAGENTS Rule 4):**
   If a ledger row has status `passing*` or `fully-verified*` and specifies a
   placeholder (`CAP_PENDING` or `LA_PENDING`), the gate immediately fails under
   Rule 2 of the traceability checker:
   ```text
   rule 2: line <N>: <req> / <method>: passing row carries placeholder capella_element_id <id> (honest ledger / HwAGENTS rule 4: passing evidence requires resolved Capella linkage)
   ```
   *Note on rule numbering (N7):* `rule 2:` denotes `ci/check_hw_traceability.py`'s
   internal row-level validation rule, enforcing the honest-ledger mandate
   defined in `HwAGENTS.md` Rule 4.
   *Note on pending rows (N3):* Pending rows (`sim-pending`, `bench-pending`)
   are permitted to retain placeholders while MBSE component allocation is
   unresolved.

2. **Model element cross-validation and corrupted model fail-closed (B4, N5):**
   When `hw/model/capella/cancestry.capella` is present in the repository,
   `ci/check_hw_traceability.py` extracts all declared element IDs via standard
   library XML parsing (no external dependencies, fully operational outside the
   heavy Docker image).
   - **Fail-closed unparseable model check (B4):** If the Capella model file is
     present on disk but malformed, corrupted, or unparseable, `load_capella_element_ids`
     reports an explicit error (`rule 2: Capella model present at ... but failed to parse`)
     and returns an empty set, ensuring the gate fails closed rather than
     silently disabling cross-validation.
   - **Element ID matching (N5):** Any non-placeholder `capella_element_id` that
     does not match an element ID declared in the Capella model XML tree fails closed:
     ```text
     rule 2: line <N>: <req> / <method>: capella_element_id <id> does not match any element ID in the Capella model (hw/model/capella/cancestry.capella)
     ```

### 1.3 Test coverage
`tests/unit/tools/test_check_hw_traceability.py` pins this behavior with five
dedicated tests:
- `test_passing_row_rejects_capella_placeholder`: asserts rejection of `CAP_PENDING` and `LA_PENDING` on passing rows.
- `test_pending_row_permits_capella_placeholder`: asserts permitted use on pending rows.
- `test_unknown_capella_element_id_fails_closed`: asserts rejection when an ID does not exist in the model.
- `test_corrupted_capella_model_fails_closed` (B4): asserts fail-closed rejection when the `.capella` file is malformed.
- `test_known_capella_element_id_passes`: asserts acceptance of valid model element IDs.

---

## 2. Rule 14, Rule 15, and the binding tolerance band rule

### 2.1 Principle and policy (Rule 14)
In Landing 1 review (`docs/hw/review/issue-44-body.md` §1 Q2), the relationship
between data envelopes and visual bands was clarified: a shaded or hatched band
is an engineering contract, not decorative illustration.

`HwAGENTS.md` adds **Rule 14**:

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

### 2.4 Generalisation: Rule 15 (Free-text allowances must fail closed)
During review of the CI wiring and mint drafts, a recurring pattern was identified:
free-text matching using whole-document substring tests (such as workflow `contains()`
checks) creates live fail-open holes whenever the keyword naming the allowance is
quoted or discussed.

`HwAGENTS.md` v1.1.4 adds **Rule 15** to formalise this principle across all agent
and automation workflows:

> **15. Free-text allowances must fail closed and be line-anchored.** Any CI gate,
> verification script, or workflow that grants an allowance or override from
> free text (such as pull request bodies, commit messages, or review
> comments) shall parse it using an explicit, line-anchored declaration with
> a mandatory, non-empty reason. Parsers shall never use unanchored substring
> matches (such as workflow `contains()` expressions) over entire documents.
> The keyword or token naming the allowance shall never be capable of
> granting it through mere citation or quotation. Any absence of an exact
> declaration, malformed declaration, or unstated reason shall fail closed.

*Status note (N6):* In accordance with QA policy, Rules 13, 14, and 15 remain
pending formal QA approval in `HwAGENTS.md`.

---

## 3. What this hardening change contains

| Commit | Contents |
|---|---|
| Commit 1 | `ci/check_hw_traceability.py` (placeholder fail-closed and Capella ID cross-check); `tests/unit/tools/test_check_hw_traceability.py`. |
| Commit 2 | `HwAGENTS.md` (Rules 14 & 15, v1.1.4); `docs/hw/visual-evidence-plan.md` (Rule 14 in §3/§7, v0.1.1); `ci/check_hw_evidence.py` (Rule 14 semantic checks); `tests/unit/tools/test_check_hw_evidence.py` (t29–t31); `docs/hw/review/issue-44-hardening.md`. |
| Follow-up | Fix B4 (fail-closed unparseable Capella model) and test; revise §4 mint workflow addressing B1, B2, B3, N1, N2; add Rule 15 to `HwAGENTS.md`. |

### Verification performed (N8)
- Hosted CI: Green on `main` (commit `1e053ac`).
- Local workspace verification:
  - `python3 ci/check_schemas_valid.py schemas/hw/` — PASS (all 9 schemas valid)
  - `python3 ci/check_hw_contracts.py .` — PASS
  - `python3 ci/check_hw_traceability.py .` — PASS (4 rows, 0 errors)
  - `python3 ci/check_hw_evidence.py --root . --rerender --strict` — PASS (0 errors, 0 warnings)
  - `CANCESTRY_HW_ALLOW_SKIP=1 pytest tests/unit/tools hw/tests` — 513 passed, 9 skipped (the 9 skips are hardware-dependent skips in `hw/tests/` requiring native OpenModelica `omc` which is verified in hosted CI container).

---

## 4. Mint workflow specification & draft (Landing 2 prerequisite)

> **QA Condition Notice:** Landing 2 remains **ON HOLD**. The draft below has been
> revised to resolve all five reviewer findings (B1, B2, B3, N1, N2). It will
> not be committed to `.github/workflows/` or triggered until formally approved.

### 4.1 Resolution of review findings in the mint design

1. **B1 (Line-anchored declaration parsing):**
   Replaced the broken `contains(github.event.pull_request.body, 'renderer-drift:')`
   substring match with the line-anchored regex parser (`DRIFT_DECL` pinned by
   `HW-EVIDENCE-GATE-DECL-001` in `test_check_hw_evidence.py`). The check runs in
   a dedicated shell step and fails closed if drift is detected without a valid,
   line-anchored declaration. Merely quoting the token in PR comments or descriptions
   no longer grants the allowance.

2. **B2 (Authorization gate & execution threat model):**
   - *Threat Model:* The mint workflow executes test harnesses and rendering scripts
     checked out from the PR branch (`github.event.pull_request.head.ref`) while
     possessing a `contents: write` token for commit-back. If triggered on arbitrary
     untrusted branches, modified harnesses could execute arbitrary code with elevated
     token privileges.
   - *Control:* Added strict authorization gating. The workflow runs only when
     both the `mint:visual-evidence` label is present AND `github.event.pull_request.author_association`
     is confirmed to be `OWNER`, `MEMBER`, or `COLLABORATOR`. Unauthorized users
     or third-party triage actors cannot trigger mint execution.

3. **B3 (Divergence detection covering untracked new files):**
   `git diff --quiet` fails to detect newly created untracked files (such as initial
   mint outputs `holdup_decay_001.svg` and `pulse_5b_001.svg`). The divergence
   detector now executes `git add -N hw/tests/evidence/` prior to running diffing,
   and checks `git status --porcelain hw/tests/evidence/`. Any new, untracked, or
   modified files under `hw/tests/evidence/` are correctly detected.

4. **N1 (Explicit plot-data generation step):**
   The workflow explicitly defines the plot-data extraction step between simulation
   execution and SVG rendering. Simulation test harnesses generate raw evidence
   JSON; dedicated extraction commands produce validated `*.plot.json` files matching
   the `hw-plot-data-0.1.0.schema.json` contract before the renderer consumes them.

5. **N2 (Attributable commit-back step):**
   Added the concrete `commit-and-push` step. When minting succeeds, the workflow
   configures git authorship as `github-actions[bot]`, records the commit with
   provenance metadata (image digest, tool versions, and source commit SHA), and
   pushes the update back to the PR head branch.

---

### 4.2 Revised specification of `.github/workflows/hw-mint.yml`

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
    # Rule 1 & B2: Label gating AND strict authorization check (threat model control)
    if: |
      contains(github.event.pull_request.labels.*.name, 'mint:visual-evidence') &&
      (github.event.pull_request.author_association == 'OWNER' ||
       github.event.pull_request.author_association == 'MEMBER' ||
       github.event.pull_request.author_association == 'COLLABORATOR')
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

      # Execution step 1: Modelica simulation & raw evidence generation
      - name: Run Modelica simulations and generate evidence artifacts
        env:
          SOURCE_DATE_EPOCH: ${{ steps.provenance.outputs.source_date_epoch }}
        run: |
          set -euo pipefail
          python3 -m pytest hw/tests/test_power_sim.py -v
          python3 -m pytest hw/tests/test_pulse_sim.py -v

      # Execution step 2 (N1): Extract plot-data contracts from evidence
      - name: Extract plot-data contracts
        run: |
          set -euo pipefail
          python3 hw/scripts/extract_plot_data.py \
            --evidence hw/tests/evidence/holdup_001.json \
            --output hw/tests/evidence/holdup_decay_001.plot.json

          python3 hw/scripts/extract_plot_data.py \
            --evidence hw/tests/evidence/pulse_7637_001.json \
            --output hw/tests/evidence/pulse_5b_001.plot.json

      # Execution step 3: Render views via cancestry-render-modelica
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

      # Validation step: All gates must pass on the minted workspace
      - name: Validate evidence and traceability gates
        run: |
          set -euo pipefail
          python3 ci/check_schemas_valid.py schemas
          python3 ci/check_hw_contracts.py .
          python3 ci/check_hw_traceability.py .
          python3 ci/check_hw_evidence.py --root . --rerender --strict

      # Rule 2, B3: Divergence detection covering untracked new files
      - name: Detect divergence against committed tree
        id: diff
        run: |
          set -euo pipefail
          # B3: Include untracked files in diff check
          git add -N hw/tests/evidence/
          if git diff --quiet hw/tests/evidence/; then
            echo "drift=0" >> $GITHUB_OUTPUT
          else
            echo "drift=1" >> $GITHUB_OUTPUT
            git status --porcelain hw/tests/evidence/
            git diff --stat hw/tests/evidence/
          fi

      # Rule 2, B1, Rule 15: Fail closed on undeclared divergence via line-anchored check
      - name: Fail closed on undeclared divergence
        if: steps.diff.outputs.drift == '1'
        env:
          HW_PR_BODY: ${{ github.event.pull_request.body }}
        run: |
          set -euo pipefail
          # B1 / Rule 15: Exact line-anchored regex from HW-EVIDENCE-GATE-DECL-001
          DRIFT_DECL='^[[:space:]]*([-*+>]|[0-9]+[.)])?[[:space:]]*[`*_]*renderer-drift:[[:space:]]*[^[:space:]]'
          if printf "%s\n" "${HW_PR_BODY:-}" | grep -qE "$DRIFT_DECL"; then
            echo "Renderer drift explicitly declared in PR body."
          else
            echo "ERROR: Mint step produced new or modified files under hw/tests/evidence/ without a declared 'renderer-drift: <reason>' line."
            exit 1
          fi

      # N2: Commit-back step for minted artifacts
      - name: Commit back minted visual evidence
        if: steps.diff.outputs.drift == '1'
        run: |
          set -euo pipefail
          git config user.name "github-actions[bot]"
          git config user.email "github-actions[bot]@users.noreply.github.com"
          git add hw/tests/evidence/
          git commit -m "chore(hw): mint visual evidence artifacts [skip ci]

          Provenance:
          - Image Digest: ${{ steps.provenance.outputs.image_digest }}
          - OMC Version: ${{ steps.provenance.outputs.omc_version }}
          - FMPy Version: ${{ steps.provenance.outputs.fmpy_version }}
          - Renderer Digest: ${{ steps.provenance.outputs.renderer_digest }}
          - Source Date Epoch: ${{ steps.provenance.outputs.source_date_epoch }}"
          git push origin HEAD:${{ github.event.pull_request.head.ref }}
```

### 4.3 Review hold
Landing 2 remains strictly on hold until this revised specification is formally
reviewed and authorized by QA.
