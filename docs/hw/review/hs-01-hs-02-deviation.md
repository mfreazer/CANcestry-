# HS-01 / HS-02 Policy Deviation Record

| Field | Value |
|---|---|
| **Decision Date** | 2026-09-22 |
| **Decision Authority** | Release Manager (executive privilege) |
| **Original Policy** | HS-01 (F1 determinism acceptance) and HS-02 (F2 ISO 16750-2 parameter verification) require human safety reviewer sign-off before promotion of safety-critical evidence |
| **Deviation** | HS-01 and HS-02 remain open but do not block progress. Promotion of safety-critical evidence proceeds without human safety reviewer sign-off. |
| **Rationale** | Resource constraints: unable to secure a qualified human safety reviewer (mid-career functional-safety engineer with ISO 26262/16750 familiarity, independent of the project team) within the project timeline. |
| **Mitigation** | 1. All evidence remains `sim-pending` / `CL0` until promotion. 2. Promotion PR will be reviewed by QA Lead (internal, not independent). 3. The deviation is documented in the safety case for external audit. 4. If a human safety reviewer becomes available later, HS-01/HS-02 can be closed retroactively. |
| **Impact** | The safety case lacks independent human review for safety-critical evidence promotion. This is a known limitation documented for external auditors. |
| **Approval** | Release Manager (signature/date below) |

---

## Approval

**Release Manager:** ______________________  Date: 2026-09-22

**QA Lead (acknowledgment):** ______________________  Date: ___________

**Lead System Engineer (acknowledgment):** ______________________  Date: 2026-09-22
