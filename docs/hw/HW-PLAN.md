# CANcestry Hardware Development Plan

| Field | Value |
|---|---|
| **Document** | CANcestry Hardware Development Plan |
| **Version** | 1.0.0 |
| **Status** | Approved — foundation document |
| **Owner** | System Engineer |
| **Co-author** | QA Lead |
| **Approver** | Release Manager |
| **Last Review** | 2026-09-19 |
| **Repository location** | `docs/hw/HW-PLAN.md` |
| **Supersedes** | Ad-hoc hardware planning memos dated 2026-09-19 |
| **Governing standards** | ISO 26262 (alignment, not certification), ASPICE SYS.1–SYS.3 (alignment), ASME V&V 40 (credibility) |

---

## 1. Purpose

This document is the foundation for CANcestry hardware development. It defines the goals, constraints, roles, methodology, toolchain, environment, repository structure, validation strategy, and governance that govern the design and verification of the CANcestry board.

It is the hardware counterpart to the software baseline (`SyRS`, `SyAD`, `SwRS`, `SwAD`) that carried CANcestry to v1.0.0. It is written to be:

- the single source of truth for hardware program decisions,
- actionable by human engineers and AI agents,
- auditable by external safety assessors,
- consistent with the discipline that closed QA-EV-01 and shipped v1.0.0.

**This document is normative.** Any change to hardware process, tooling, or validation requires a PR against this document with QA review.

---

## 2. Goals

The hardware program shall:

**G1 — Support the v1.0.0 software safety case.** Every hardware assumption embedded in the software safety case — `RTC_BKP0R` retention, IWDG watchdog, safe-latch GPIO topology, CAN physical layer — shall be realized as a hardware requirement, verified by an independent oracle, and traced end-to-end.

**G2 — Simulation-first, physical-second.** No schematic capture begins until the Modelica power model analytically proves sleep current and load-dump survival. No layout begins until the schematic passes ERC and QA review. No fabrication begins until the virtual bench has exercised the safety-critical paths.

**G3 — Presentable safety case.** The hardware shall have a coherent, traced, independently-oracled safety case that an external ISO 26262 assessor can audit without needing to reverse-engineer intent from source files.

**G4 — Same discipline as software.** Traceability is CI-gated. Schemas are law. Every requirement has a test. Every test has an oracle. Every `passing` row is evidence-backed, never asserted.

**G5 — Honest ledger.** Requirements that cannot yet be validated — because the board does not exist, the model does not cover the effect, or no oracle is available — shall be recorded as `bench-pending`, never marked `passing` on simulation alone.

**G6 — Longevity.** Tooling, components, and documentation shall be maintainable by a small team over years, not dependent on commercial licenses, vendor lock-in, or single-source parts where avoidable.

---

## 3. Scope

**In scope:**

- Hardware Requirements Specification (HwRS)
- Hardware Architecture Description (HAD) via Capella/Arcadia
- Electrical schematic and PCB layout
- Bill of Materials with FIT data and lifecycle status
- WCCA, derating, thermal, and FMEDA analysis
- Virtual bench (Renode + FMI + Modelica)
- Design Verification Test (DVT) plan and bench entry/exit criteria
- Hardware-in-the-loop harness and test plan
- Tool qualification evidence
- Safety case documentation
- Traceability from HwRS to software safety requirements

**Out of scope for the current phase:**

- ASIL-D certification (target posture is ASIL-B alignment)
- Full automotive EMC/EMI pre-compliance (reviewed at design, measured at DVT)
- Production manufacturing transfer
- Vehicle-program integration and road validation
- CAN FD physical layer beyond what ISO 11898-2 requires

---

## 4. Constraints

**C1 — No physical bench initially.** All validation prior to board arrival shall be performed in the virtual bench: Renode executes the real v1.0.0 ARM ELF; Modelica supplies the electrical and thermal plant; FMI 3.0 couples them.

**C2 — Software safety case is fixed.** The v1.0.0 software assumes specific hardware behaviors. Hardware must satisfy those assumptions or the software safety case must be amended with documented rationale. No silent mismatch.

**C3 — Repository evidence is the baseline.** `platform/cortex_m/hal_stm32.c`, `watchdog.c`, `cancestry_baremetal.ld`, and `docs/qa/hil-fault-injection-report.md` define what the board must support. The MCU must have FDCAN or bxCAN, DWT `CYCCNT`, and boot-surviving retention (RTC backup domain or equivalent).

**C4 — Open-source tooling preferred.** Commercial add-ons (e.g., ATICA) are permitted but must have a documented fallback path that does not block the program.

**C5 — CI resource budget.** Hosted GitHub runners shall remain sufficient for schema validation, Modelica compilation, FMPy execution, and Capella model parsing. Heavy simulation and physical HIL shall run on self-hosted infrastructure.

**C6 — Documentation freeze.** No release tag shall be cut while safety-relevant design questions remain open. This mirrors the software policy that produced v1.0.0.

**C7 — Fewer, sharper requirements.** The QA-EV-01 lesson applies to hardware: prefer one clear requirement with a test over three overlapping requirements with ambiguous closure criteria.

---

## 5. Roles and Responsibilities

| Role | Holder | Responsibilities |
|---|---|---|
| **System Engineer (SE)** | Lead SE | HwRS authorship, Capella model, Modelica `CancestryLib`, tool qualification, schematic review, toolchain maintenance |
| **QA Lead** | QA Lead | Validation matrix, oracle rule enforcement, credibility scheme, traceability audits, schematic and layout review, closure of findings |
| **Release Manager** | Release Manager | Version control, milestone approval, tag authorization, `docs/versions.md` reconciliation |
| **Hardware Agent** | `@cancestry-hw-agent` | Implements under `HwAGENTS.md`; produces PRs against open issues; does not merge |
| **Human Reviewer** | Designated team member | Required review for safety-relevant changes: safety monitor chain, fail-safe topology, retention domain, watchdog wiring |
| **External Assessor** | TBD (future) | Reviews safety case at readiness milestone |

**Decision rights:**

- SE owns architecture, tooling, and hardware design decisions.
- QA owns validation strategy and closure of findings.
- Release Manager owns versioning and release timing.
- Human Reviewer owns sign-off on safety-relevant changes. AI agents may not self-approve these.

---

## 6. Methodology

### 6.1 Model-Based Systems Engineering with Arcadia

The hardware program uses **Eclipse Capella** as the MBSE tool and **Arcadia** as the methodology.

Arcadia's four levels structure the model:

| Level | Purpose | CANcestry artifacts |
|---|---|---|
| **OA** — Operational Analysis | Actors, capabilities, operational scenarios | Vehicle, bench, and diagnostic actors; operational modes |
| **SA** — System Analysis | System functions, mission, external interfaces | CAN interfaces, power interfaces, safety functions |
| **LA** — Logical Architecture | Logical components independent of implementation | Power supervisor, CAN physical layer, safety monitor, fail-safe latch |
| **PA** — Physical Architecture | Component-level implementation | STM32G474, transceiver, regulator, retention domain, connector |

**Rationale:** Arcadia maps onto ASPICE SYS.1–SYS.3 and ISO 26262 concept-phase workflows. Papyrus with SysML was rejected because it is a modeling tool without a method; Arcadia is a method with a tool.

### 6.2 Two-Layer MBSE

The model has two layers with strict separation:

**Layer 1 — Structural and requirements model (Capella).** Blocks, ports, interfaces, power-mode state machine, parametric diagrams for budgets. Requirements live in `docs/hw/HwRS.md`; the Capella model *links* to them and does not duplicate them. CI verifies that every HwRS requirement has a Capella element and every Capella safety requirement has an HwRS ID.

**Layer 2 — Executable physics model (Modelica).** The `CancestryLib` package provides power, bus, thermal, and safety supervisor models. Capella parametrics define the budgets; Modelica proves them. Protocol logic stays in the existing C conformance suites and is not duplicated in Modelica.

### 6.3 The Capella → Modelica Bridge

Architectural models and system simulations are not automatically coupled. The bridge is specified in `docs/hw/virtual-bench-plan.md`:

- Capella logical components → Modelica blocks
- Capella ports and interfaces → Modelica connectors
- Capella parametric constraints → Modelica parameter bindings
- Validated by CI: every safety-relevant Capella component must have a corresponding Modelica element, or be explicitly marked `not_simulated` with a rationale

`capellambse` provides the read path. A small Modelica generator provides the write path. If automated bridging proves impractical, a manual mapping documented in the plan is the fallback.

### 6.4 Safety Analysis

Qualitative analysis (FHA, FMEA, FTA) is performed in Capella using standard Arcadia safety viewpoints, with ATICA as an optional accelerator. Quantitative analysis (SPFM, LFM, PMHF) is performed by a Python calculator fed by the BOM and a curated FIT database.

`docs/hw/fmeda.md` is a *rendered view* of the model and the quantitative results, not the source of truth.

---

## 7. Toolchain

| Concern | Tool | Justification |
|---|---|---|
| MBSE modeling | **Eclipse Capella 6.x** | Method-first (Arcadia), ISO 26262 aligned, open source |
| Headless MBSE access | **`capellambse`** (Python) | CI/CD integration, no Java/GUI required |
| Safety analysis (qualitative) | Capella Arcadia safety viewpoints; ATICA optional | Model-integrated FHA/FMEA/FTA |
| Safety analysis (quantitative) | Python FMEDA calculator | Repo-native, CI-gated |
| Physics simulation | **OpenModelica** + `CancestryLib` | Acausal multi-domain, FMI export, headless CI |
| FMU execution | **FMPy** (Python) | Python-native, CI-friendly |
| Co-simulation | **FMI 3.0** | Standard coupling |
| MCU emulation | **Renode** (QEMU fallback) | Runs real ELF, peripheral models, Python scripting |
| ECAD | **KiCad 8/9** (later phase) | Open source, git-native text formats, `kicad-cli` headless ERC/DRC |
| Schemas | **JSON Schema Draft 2020-12** | Schema-is-law, CI-validated |
| Traceability | Python CI check (`ci/check_hw_traceability.py`) | Same discipline as software |
| Version control | GitHub + Git LFS (for Capella model if needed) | Existing project infrastructure |
| CI orchestration | GitHub Actions | Existing infrastructure |

**Tool qualification.** All tools used to produce verification evidence shall be classified per ISO 26262-8 §13 (TCL1–TCL3) and qualified by regression against analytical solutions or published tabulated references. Qualification evidence lives in `docs/hw/tool-qualification.md`.

---

## 8. Environment

### 8.1 Development Environment

Engineers and agents work in the CANcestry repository, using:

- Docker images for the toolchain (Capella, OpenModelica, FMPy, Renode, KiCad headless) pinned by digest under `ci/docker/`
- GitHub Codespaces or local environments provisioned from the same images
- Capella running locally for interactive modeling; `capellambse` for CI and scripting

### 8.2 CI Environment

GitHub-hosted runners execute:

- Schema validation (JSON Schema Draft 2020-12)
- Capella model parsing and orphan-block / unlinked-safety-requirement checks
- Modelica syntax and compilation checks
- FMPy FMU execution and Python test assertions
- Traceability checks (software + hardware)
- `check_no_alloc.py` and other existing gates, unchanged

### 8.3 Virtual Bench

The virtual bench is the primary validation environment until physical hardware exists:

```
 Renode (virtual MCU target)          OpenModelica (CancestryLib plant)
 - runs the REAL v1.0.0 ARM ELF       - power transients, ISO 7637-2 pulses
 - FDCAN/IWDG/DWT/RTC_BKP/GPIO        - CAN physical layer (ISO 11898-2)
 - fault stimulus injection           - thermal RC network
            \                                     /
             \---- FMI 3.0 co-simulation --------/
                            |
                pytest orchestration in CI
                oracles: analytics + tabulated refs
```

The virtual bench executes the actual compiled firmware binary. It is not a reimplementation of firmware logic.

### 8.4 Physical Bench (Deferred)

Physical HIL infrastructure shall be specified in `docs/hw/dvt-plan.md` and provisioned only when:

- All virtual-bench-exhaustible requirements have been verified
- `bench-required.md` is complete and QA-reviewed
- Tool qualification evidence for virtual tools is complete

The physical bench is not on the critical path for H-Phase 1 or H-Phase 2.

---

## 9. Repository Structure

```
cancestry/
├── docs/
│   ├── hw/
│   │   ├── HW-PLAN.md                    (this document)
│   │   ├── HwRS.md                       Hardware Requirements Specification
│   │   ├── mbse-plan.md                  Capella/Arcadia usage guide
│   │   ├── virtual-bench-plan.md         Tiers, credibility, oracle rule, bridge
│   │   ├── wcca-derating.md              Worst-case circuit analysis
│   │   ├── fmeda.md                      Rendered safety analysis view
│   │   ├── dvt-plan.md                   Design Verification Test plan
│   │   ├── bench-required.md             Requirements forced to physical bench
│   │   ├── tool-qualification.md         ISO 26262-8 §13 evidence
│   │   └── safety-case.md                Consolidated hardware safety case
│   ├── qa/
│   │   ├── test-strategy.md              (extended for hardware)
│   │   └── hw-validation-matrix.md       QA validation strategy
│   └── trace/
│       └── traceability.csv              (extended with HW columns)
├── schemas/
│   └── hw/
│       ├── hw-bom-0.1.0.schema.json
│       ├── hw-sim-0.1.0.schema.json
│       └── hw-traceability-0.1.0.schema.json
├── hw/
│   ├── model/
│   │   ├── capella/                      Capella project (OA/SA/LA/PA)
│   │   └── CancestryLib/                 Modelica package
│   │       ├── package.mo
│   │       ├── Power/
│   │       ├── Bus/
│   │       ├── Thermal/
│   │       └── Safety/
│   ├── bom/
│   │   ├── bom.csv                       Schema-validated
│   │   └── fit-database.csv              Curated FIT rates
│   ├── tests/
│   │   ├── test_power_sim.py
│   │   ├── test_bus_sim.py
│   │   └── test_safety_sim.py
│   └── ecad/                             (later phase — KiCad)
├── ci/
│   ├── check_hw_traceability.py
│   ├── check_capella_model.py
│   └── check_modelica.py
├── .github/
│   └── workflows/
│       ├── hw-fast.yml                   PR gate
│       └── hw-nightly.yml                Full sim + model checks
└── HwAGENTS.md                           Hardware agent rules
```

---

## 10. QA and Validation Strategy

### 10.1 Validation Tiers

| Tier | Method | Environment | Closes requirements when |
|---|---|---|---|
| **T0** | Analysis | WCCA, derating, FTA/FMEDA | Oracle exists (analytical or standard-based) |
| **T1** | Block simulation | Modelica | Oracle exists and credibility requirement is met |
| **T2** | Virtual bench | Renode + FMI + Modelica | Oracle exists and firmware binary executed |
| **T3** | Protocol conformance | Existing C conformance suites | Unchanged from software |
| **T4** | Physical DVT / HIL | Physical bench | Deferred — gated on `bench-required.md` |

### 10.2 The Oracle Rule

> **A simulation or analysis result shall not close a hardware requirement unless at least one independent oracle exists: (a) an analytical closed-form solution, (b) a tabulated reference from the governing standard, (c) a golden measurement from a prior qualified board, or (d) an independent model authored by a different tool or author.**

CI shall refuse to mark a requirement `passing` from a sim or analysis row that lacks an oracle link. This applies to Modelica results, Renode traces, FMEDA outputs, and WCCA.

### 10.3 Model Credibility

Credibility is a property of the *claim*, not the artifact. The HwRS shall state, per requirement, the credibility level required (CL0–CL3, ASME V&V 40-style). Traceability rows gain a `credibility_level` column. A row cannot be `passing` unless `credibility ≥ required`.

### 10.4 Exit Criteria — Virtual to Physical

Three triggers force physical validation regardless of simulation results:

1. **Discretization gap** — physical outcome depends on behavior below the sim time step (sub-µs glitches, ESD, metastability).
2. **Model boundary** — outcome depends on a component the model abstracts (real transceiver loop delay, real oscillator drift, contactor fall time).
3. **Oracle absence** — no independent oracle exists. Simulation-only evidence is not evidence.

At H-Phase 1 exit, a `bench-required.md` listing every requirement that fails any trigger is published. This list becomes the DVT plan input.

### 10.5 Tool Qualification

Per ISO 26262-8 §13, each tool that produces verification evidence receives a TCL classification and qualification evidence:

- **OpenModelica** — qualified by regression against analytical solutions (RC response, pulse generator parameters from ISO 7637-2 tables).
- **Renode** — qualified by regression against known firmware traces and peripheral behavior reference.
- **`capellambse`** — qualified as a model reader (not a safety case producer).
- **Python FMEDA calculator** — qualified by reproducing ISO 26262-5 Annex D worked example to published precision.

Qualification evidence lives in `docs/hw/tool-qualification.md`.

### 10.6 FMEDA Oracle

The Python FMEDA tool reproduces the ISO 26262-5 Annex D worked example before it is trusted on our BOM. This is a CI gate.

### 10.7 QA Audit Points

QA reviews, with checklists stored in `docs/qa/hw-validation-matrix.md`:

- HwRS review — completeness, testability, traceability
- Capella model review — orphan blocks, unlinked safety requirements, level discipline
- Virtual bench plan review — oracle coverage, credibility assignments, bridge specification
- Schematic review — power tree, transceiver, retention, fail-safe topology
- Layout review — signal integrity, EMC, thermal, DFM/DFA
- DVT plan review — bench entry/exit criteria, coverage of `bench-required.md`
- Safety case review — end-to-end traceability, honest ledgers, no unoracled `passing`

### 10.8 Traceability

The traceability CSV extends with columns:

```
capella_element_id, credibility_level, oracle_id
```

Every hardware requirement shall trace:

- **up** to a software safety requirement (`SYS-SF-*`, `SW-FR-*`) where the hardware supports the software safety case
- **down** to a verification method (`analysis`, `sim(CLn)`, `bench-pending`) with its oracle ID

CI fails on orphan rows, missing oracle IDs for `passing` rows, or credibility levels below requirement.

---

## 11. Governance

### 11.1 GitHub Process

- One issue per phase deliverable. Issue H-01 is the first.
- PRs reference the issue and the requirements they satisfy.
- CI gates block merge: schema validity, model compilation, FMU execution, traceability, orphan checks.
- Human review required for safety-relevant changes: safety monitor chain, fail-safe topology, retention domain, watchdog wiring, power supervisor.
- AI agents operate under `HwAGENTS.md` and may not self-approve safety-relevant changes.

### 11.2 Documentation Discipline

- Every normative hardware document has the standard header (version, status, owner, approver, last review, change log).
- `docs/versions.md` reconciles release version, HwRS version, MBSE plan version, and any other versioned artifact.
- Documentation freeze before any release tag: no open safety-relevant design questions.
- `docs/qa/closed-findings.md` records finding closures with evidence and merge commits, mirroring the software discipline that closed QA-EV-01.

### 11.3 Honest Ledger Culture
