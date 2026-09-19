# CANcestry Tool Qualification Plan and Evidence (ISO 26262-8 §13)

| Field | Value |
|---|---|
| **Document** | CANcestry Tool Qualification Plan and Evidence |
| **Version** | 0.1.0 |
| **Status** | Approved — H-Phase 1 baseline |
| **Owner** | System Engineer |
| **Approver** | QA Lead |
| **Last Review** | 2026-09-19 |
| **Repository location** | `docs/hw/tool-qualification.md` |
| **Governing documents** | `docs/hw/HW-PLAN.md` v1.0.0 §7/§10.5, ISO 26262-8:2018 §13 |

---

## 1. Purpose and Scope

This document defines the evaluation, classification, and qualification of software tools used in the CANcestry hardware program, in accordance with ISO 26262-8:2018 §13 (*Evaluation of software tools*).

Per HW-PLAN §7 and §10.5, any tool that produces verification evidence cited in the hardware safety case shall be evaluated for Tool Impact (TI), Tool Error Detection (TD), and Tool Confidence Level (TCL). Tools assigned TCL2 or TCL3 require formal qualification evidence.

---

## 2. Tool Classification Scheme (ISO 26262-8 §13.4)

### 2.1 Tool Impact (TI)
- **TI1**: There is no argument that the tool cannot introduce or fail to detect errors in a safety-related item.
- **TI2**: All other cases (e.g. simulation or static check tools whose output is independently verified by automated test gates).

### 2.2 Tool Error Detection (TD)
- **TD1**: High degree of confidence that a malfunction and its corresponding erroneous output will be detected.
- **TD2**: Medium degree of confidence.
- **TD3**: Low degree of confidence.

### 2.3 Tool Confidence Level (TCL)
- **TCL1**: Combination of (TI1, TD1/TD2/TD3) or (TI2, TD1) — no tool qualification required.
- **TCL2 / TCL3**: Formal qualification required via qualification test suite, evaluation of tool development process, or validation of tool output.

---

## 3. Tool Classification Table

| Tool | Role / Usage | TI | TD | TCL | Status / Qualification Method |
|---|---|---|---|---|---|
| **OpenModelica** | Compiles Modelica plant models (`CancestryLib`) into FMI 2.0/3.0 FMUs for T1 simulation. | TI2 | TD1 | **TCL1** | Qualified by regression against independent analytical/tabulated oracles (OR-001, OR-002) in CI. |
| **FMPy** | Executes compiled FMUs and extracts time-series simulation traces during CI test runs. | TI2 | TD1 | **TCL1** | Qualified by regression against independent analytical/tabulated oracles (OR-001, OR-002) in CI. |
| **`capellambse`** | Reads Eclipse Capella Arcadia models; enforces structural integrity and linkage gates in CI (`ci/check_capella_model.py`). | TI2 | TD1 | **TCL1** | Qualified as a model reader by unit tests and negative fixture suites in CI. |
| **Python FMEDA Calculator** | Computes quantitative SPFM, LFM, and PMHF metrics from BOM and FIT database. | TI1 | TD1 | **Pending (H-03)** | Qualification gated on reproducing the ISO 26262-5 Annex D worked example (oracle OR-004) to published precision. |
| **Renode** | Virtual target emulator running real ARM ELF for T2 co-simulation. | TI2 | TD1 | **Pending (H-04)** | Qualification gated on replaying Phase 12 fault injection suite against golden physical traces. |

---

## 4. Qualification Evidence Records

### 4.1 Record 1: OpenModelica + FMPy vs. Analytical Oracle OR-001
- **Target Toolchain**: OpenModelica 1.24 + FMPy 0.3.24
- **Verification Case**: `hw/tests/cases/holdup_001.simcase.json`
- **Requirement Traced**: HW-SF-002, HW-FR-009
- **Oracle**: OR-001 (`hw/tests/oracles/or_001_holdup.py`), Class (a) RC hold-up / energy-balance closed form.
- **Qualification Evidence**: Headless FMU compilation and simulation in `hw/tests/test_power_sim.py`. The simulated $V_{\text{BAT}}(t)$ voltage trace matches the closed-form analytical solution within the declared tolerance ($0.001\text{ V}$) across a 150 ms transient event.
- **Evidence Artifact**: `hw/tests/evidence/holdup_001.json` (sha256-pinned).

### 4.2 Record 2: OpenModelica + FMPy vs. Standard Tabulated Oracle OR-002
- **Target Toolchain**: OpenModelica 1.24 + FMPy 0.3.24
- **Verification Case**: `hw/tests/cases/pulse_7637_001.simcase.json`
- **Requirement Traced**: HW-FR-004
- **Oracle**: OR-002 (`hw/tests/oracles/or_002_pulse7637.py`), Class (b) ISO 7637-2 / ISO 16750-2 tabulated parameters.
- **Qualification Evidence**: Simulation and verification of transient pulses 1, 2a, 2b, 3a, 3b, 4, and 5b in `hw/tests/test_pulse_sim.py`. Simulated peak voltages, pulse durations, and rise/fall times match the standard tables within declared tolerances ($\le 0.01\text{ V}$, $\le 1\ \mu\text{s}$).
- **Evidence Artifact**: `hw/tests/evidence/pulse_7637_001.json` (sha256-pinned).

---

## 5. Validation Gap Inheritance Rule

> **Validation Gap Inheritance Rule (Normative):**
> Any hardware requirement closed using evidence produced by a qualified software tool inherits the tool's declared **validation gap** (recorded in `hw/tests/oracles/registry.csv`).
> 
> The validation gap explicitly defines the physical, thermal, or modeling boundaries that the simulation does not cover. Inherited validation gaps are tracked in the hardware traceability ledger (`hw/tests/traceability.csv`) and must be resolved by physical measurement (T4) or golden-board correlation prior to final CL3 verification exit.

---

## 6. Change Log

| Version | Date | Change |
|---|---|---|
| 0.1.0 | 2026-09-19 | Initial release (issue #35): TCL classification table for OpenModelica, FMPy, capellambse, FMEDA calculator, and Renode; qualification evidence records for OR-001 and OR-002; validation gap inheritance rule defined. |
