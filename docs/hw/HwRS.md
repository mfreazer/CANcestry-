# CANcestry Hardware Requirements Specification

| Field | Value |
|---|---|
| **Document** | CANcestry Hardware Requirements Specification |
| **Version** | 0.3.0 |
| **Status** | Approved — H-Phase 1 baseline; the v0.3.0 HW-FR-004 amendment (H-06, issue #49) is pending QA pass-1 approval per the issue #49 two-pass review protocol |
| **Owner** | System Engineer |
| **Approver** | QA Lead (validation), Release Manager (baseline) |
| **Last Review** | 2026-09-21 |
| **Repository location** | `docs/hw/HwRS.md` |
| **Governing documents** | `docs/hw/HW-PLAN.md` v1.0.0, `docs/safety/SafetyManual.md` v1.0.0, `docs/system/SyRS.md` |

## 1. Purpose and scope

This specification defines what the CANcestry board **shall** do to realize the hardware assumptions of the v1.0.0 software safety case (HW-PLAN G1, C2, C3). It is the source of truth for hardware requirements; the Capella model links to these IDs and never duplicates them (HW-PLAN §6.2).

Derivation sources, per C3: `platform/cortex_m/hal_stm32.c`, `watchdog.c`, `startup.c`, `cancestry_baremetal.ld`, `docs/qa/hil-fault-injection-report.md`, `docs/safety/SafetyManual.md`.

ID scheme: `HW-SF-*` safety, `HW-FR-*` functional, `HW-NF-*` non-functional. Verification tiers and credibility levels (CL) per HW-PLAN §10.1/§10.3 and `docs/qa/hw-validation-matrix.md` §2–§4. Oracle classes per `docs/qa/hw-validation-matrix.md` §5: **(a)** closed-form analytical, **(b)** standard/datasheet tabulated, **(c)** golden measurement, **(d)** independent model or tool.

**Closure policy (QA ruling Q4):** all `HW-SF-*` and any `HW-FR-*` supporting a safety property require **CL3 at final closure**. A T1/T2 row may close provisionally at `passing (CL2)`; the requirement is `fully-verified (CL3)` only after T4 bench correlation. Traceability carries both states.

## 2. Safety requirements

| ID | Requirement (shall) | Derivation | Method / Tier | Req. CL | Oracle class | Initial status |
|---|---|---|---|---|---|---|
| HW-SF-001 | With the MCU unpowered or held in reset, every CAN transceiver shall be in TX-disabled state and every contactor-driver output de-energized, by passive pull networks alone, with no firmware execution. | SYS-SF-004; `watchdog.c` reset behavior | T0 netlist/WCCA analysis; T2 Renode pre-boot model; T4 measurement | CL3 | (a) divider solve + (d) KiCad netlist query independent of schematic author; T4 (c) measurement | `analysis-pending` |
| HW-SF-002 | The retention domain shall preserve the RTC backup fault code (`0x45565101`) across (i) IWDG reset, (ii) main-rail brownout to BOR level 3 (2.8 V) for ≤ 50 ms, (iii) main-rail removal from BOR to 0 V for ≤ 100 ms. Hold-up mechanism per HW-FR-009. The achieved form (strong/weak) of `SW-FR-LOG-004` shall be declared here and in `docs/versions.md`. | SW-FR-LOG-004; QA-EV-01 | T1 hold-up sim; T2; T4 | CL3 | (a) RC hold-up closed form (OR-001); (b) MCU VBAT-current table (OR-006); T4 (c) | `sim-pending` |
| HW-SF-003 | Loss-of-control detection shall be provided by an external window watchdog IC (HW-FR-010) as the **primary** layer with window [7.5 ms, 25 ms] and timebase accuracy ≤ ±5% over −40…+85 °C. The MCU IWDG on the LSI oscillator serves as a **secondary, best-effort** layer whose timeout is not load-bearing for the safety case, because the STM32G4 LSI tolerance cannot close the required window over temperature. | SYS-FR-016; SafetyManual fault chain | T0 LSI/window analysis; T2; T4 | CL3 | (b) datasheet LSI accuracy (OR-007); (a) window arithmetic; T4 (c) | `analysis-pending` |
| HW-SF-004 | On loss of vehicle supply within the declared operating range, stored energy shall suffice to complete, in order: retention write, safe-state latch, one canonical safe broadcast frame (ID `0x100`) at arbitration bitrate, before the 3V3 rail falls below the MCU BOR level 3 threshold (**2.8 V falling**, configured in option bytes). | SYS-SF-004; `watchdog.c` broadcast; QA-EV-01 ordering | T1 energy-balance sim; T4 | CL3 | (a) energy-balance closed form (OR-001); (b) STM32G4 BOR table (OR-008); T4 (c) | `sim-pending` |
| HW-SF-005 | No single open/short failure of any component in the reset, supervisor, or fail-safe latch path shall prevent safe-state assertion; demonstrated by FTA cut-set analysis and reflected in the FMEDA. | ISO 26262-5 single-point faults; SafetyManual FMEA | T0 FTA/FMEDA | CL3 | (d) independent cut-set recomputation (OR-004); ISO 26262-5 Annex D gate | `analysis-pending` |

## 3. Functional requirements

| ID | Requirement (shall) | Derivation | Method / Tier | Req. CL | Oracle class | Initial status |
|---|---|---|---|---|---|---|
| HW-FR-001 | The MCU shall provide ≥2 FDCAN or bxCAN instances, DWT CYCCNT, IWDG, RTC with backup domain (≥1 backup register), and a register map matching `platform/cortex_m` at v1.0.0. | C3; SW-FR-BM-001..008 | T0 reference-manual review; T2 (v1.0.0 ELF boots and passes bare-metal conformance in Renode) | CL2 | (b) vendor reference manual | `analysis-pending` |
| HW-FR-002 | The board shall provide three CAN interfaces with independent transceivers and independent TXE/TXD control: CH1 VCU-facing, CH2 pack-facing, CH3 diagnostic. | SyRS interfaces; gateway mission | T0 review; T2 | CL2 | (d) Capella interface consistency check | `draft` |
| HW-FR-003 | Each CAN interface shall meet ISO 11898-2:2016 including FD at 2 Mbit/s nominal / 5 Mbit/s data phase. Operational environment: ≤ 4 nodes, ≤ 10 m harness. Termination shall be switchable (120 Ω) per channel. Conformance measured against the standard test setup defined in ISO 11898-2 §12. | SW-FR-CANFD-001..006 | T1 bus sim (sample point, eye margin); T4 | CL3 | (b) ISO 11898-2 tables (OR-003); (a) line calc; T4 (c) | `sim-pending` |
| HW-FR-004 | The board shall operate from 9–16 V DC (12 V nominal) and withstand ISO 7637-2 pulses 1, 2a, 2b, 3a, 3b, 4 and the ISO 16750-2:2012 load-dump transients — pulse 5b (Test B, with centralized load dump suppression: suppressed Us* per §4.6.4.2.3, Figure 9 / Table 6) and pulse 5a (Test A, without centralized load dump suppression: unclamped generator Us per §4.6.4.2.2, Figure 8 / Table 5) — without damage, without violating HW-SF-002, and without spurious TX assertion. Verification posture (v0.3.0 amendment, H-06 issue #49): T1 simulation pending for both load-dump pulses; T4 bench correlation is tracked by issue #41; this amendment extends coverage only and makes no qualification claim and no status promotion. | Vehicle environment; G1 | T1 transient sim; T4 | CL3 | (b) ISO 7637-2 tabulated waveforms (OR-002); T4 (c) | `sim-pending` |
| HW-FR-005 | In sleep mode with all transceivers in low-power listen, total board current shall be ≤ 2.0 mA at 12 V, 25 °C. **Design target: ≤ 1.5 mA** to absorb production spread. The Modelica test asserts the design target, not the requirement. | Aftermarket parasitic-drain budget | T1 power model; T4 | CL1 | (b) datasheet sleep-current tables (OR-006) | `sim-pending` |
| HW-FR-006 | The MCU shall provide Flash ≥ 512 KiB and RAM ≥ 128 KiB with ECC or parity, such that v1.0.0 linker sections (`.cancestry_core`, `.cancestry_rings`, `.cancestry_ram`) occupy ≤ 70 % of each region. | `cancestry_baremetal.ld`; margin rule | T0 map arithmetic + datasheet | CL1 | (a) linker-map arithmetic | `analysis-pending` |
| HW-FR-007 | Main-clock total accuracy (initial + temperature + 10 y aging) shall be ≤ **±0.5 %** over −40…+85 °C. Derived from CAN FD bit-timing budget: with NBT = 20 TQ, SJW = 4 TQ, phase_seg2 = 4 TQ, sample point 80 %, the theoretical maximum df is 0.78 %; the requirement is set at 0.5 % to provide 35 % margin. Implies a crystal oscillator; the internal HSI is inadequate. | ISO 11898-1/2 bit timing | T0 budget calc (OR-003) | CL3 | (a) bit-timing budget closed form; (b) crystal datasheet; T4 (c) | `analysis-pending` |
| HW-FR-008 | The board shall expose: SWD connector; one series sense shunt per power rail with test pads; test points on every safe-latch and transceiver-TXE net; retention read-back via a UDS DID. | Testability rule (HwAGENTS); DVT needs | T0 review; T4 usability | CL1 | (d) netlist query for required nets (OR-005) | `draft` |
| HW-FR-009 | The VBAT domain shall include a hold-up capacitor of value **C ≥ 10 µF** with leakage **≤ 5 µA at 85 °C**, providing retention across the durations in HW-SF-002 with ≥ 10× margin against the STM32G4 RTC VBAT current (typ. 1.2 µA) at the VBAT minimum operating voltage (1.65 V typical). Derivation: C × ΔV = I × t, with ΔV = 3.3 V − 1.65 V = 1.65 V, t = 100 ms, I = 12 µA (10× margin on 1.2 µA) → C ≥ 0.73 µF; standard value 10 µF provides additional margin for self-discharge and PCB leakage. | HW-SF-002; QA finding HwRS-F1 | T1 hold-up sim; T4 | CL3 | (a) RC hold-up closed form (OR-001); (b) MCU VBAT-current table (OR-006); T4 (c) | `sim-pending` |
| HW-FR-010 | The board shall provide an external window watchdog IC with timebase accuracy ≤ ±5 % over −40…+85 °C and window [7.5 ms, 25 ms]. Window upper bound derived from the SafetyManual loss-of-control tolerance (25 ms). Window lower bound derived from 1.5× the v1.0.0 control cycle (10 ms). The IC shall drive the MCU reset line independently of the MCU's own power domain and shall be kickable only via a dedicated GPIO. | HW-SF-003; QA ruling Q1 | T0 window analysis; T2; T4 | CL3 | (b) IC datasheet; (a) window arithmetic; T4 (c) | `analysis-pending` |

## 4. Non-functional requirements

| ID | Requirement (shall) | Derivation | Method / Tier | Req. CL | Oracle class | Initial status |
|---|---|---|---|---|---|---|
| HW-NF-001 | Operating ambient −40…+85 °C non-condensing; storage −40…+105 °C. | EVT environment declaration | T0 grade review; T4 chamber | CL1 | (b) component grade data | `draft` |
| HW-NF-002 | At worst-case ambient and worst-case bus load, every component junction temperature shall be ≤ 80 % of rated Tj,max. | Derating rule | T1 thermal RC model; T4 IR | CL2 | (a) steady-state RC closed form | `sim-pending` |
| HW-NF-003 | Continuous electrical stress shall be ≤ 70 % of rated maximum (voltage and current); capacitors ≤ 80 % of rated voltage; exceptions require a QA-recorded waiver in `wcca-derating.md`. | Derating rule | T0 WCCA | CL1 | (a) WCCA computation | `analysis-pending` |
| HW-NF-004 | Safety-related parts (supervisor, transceivers, retention energy store, contactor driver, external watchdog) shall be AEC-Q100/Q200 qualified, lifecycle "active", with ≥ 5 y availability or a documented second source in `hw/bom/bom.csv`. | G6; BOM schema | T0 schema + lifecycle check | CL1 | (b) manufacturer data | `draft` |
| HW-NF-005 | Active gateway-mode board power shall be ≤ 3.5 W at 12 V. | Thermal envelope feeding HW-NF-002 | T1 power model; T4 | CL1 | (a) rail-by-rail power sum | `sim-pending` |

## 5. Bench-required seed list (HW-PLAN §10.4)

The following are pre-classified `bench-pending` regardless of simulation outcome; they seed `bench-required.md` at H-Phase 1 exit:

| Seed | Trigger (§10.4) |
|---|---|
| ESD/EMC behavior of connectors and bus stubs | Discretization gap |
| Oscillator aging over life | Model boundary |
| Real transceiver loop delay and ISO 11898-2 conformance measurement | Model boundary |
| Contactor-driver fall time into real inductive load | Model boundary |
| Retention across real vehicle brownout shapes | Oracle absence (no golden measurement yet) |
| Thermal IR correlation of the RC network | Oracle absence |
| External watchdog IC kick timing under real firmware scheduling | Oracle absence |

## 6. Traceability contract

Rows enter `docs/trace/traceability.csv` with the HW-PLAN §10.8 extension columns `capella_element_id, credibility_level, oracle_id`. Upward links:

- HW-SF-001, HW-SF-003, HW-SF-004 → SYS-SF-004, SYS-FR-016
- HW-SF-002 → SW-FR-LOG-004
- HW-SF-005 → SafetyManual FMEA
- HW-FR-001 → SW-FR-BM-*
- HW-FR-003, HW-FR-007 → SW-FR-CANFD-*
- HW-FR-009 → HW-SF-002
- HW-FR-010 → HW-SF-003

A row may read `passing` only with an oracle ID and `credibility ≥ required` (CI-enforced by `ci/check_hw_traceability.py`). Safety rows progress `passing (CL2) → fully-verified (CL3)` per §1.

## 7. Open questions for QA ruling

None outstanding as of v0.2.0. Rulings Q1–Q4 from the v0.1.0 review are incorporated.

## 8. Change Log

| Version | Date | Change |
|---|---|---|
| 0.1.0 | 2026-09-19 | Initial draft from HW-PLAN v1.0.0 and repo evidence |
| 0.2.0 | 2026-09-19 | QA review applied: HW-SF-002 references hold-up per HW-FR-009 (HwRS-F1); HW-SF-004 BOR level 3 explicit (HwRS-F2); HW-FR-003 clarified as operational environment + ISO 11898-2 test setup (HwRS-F4); HW-FR-007 tightened to ±0.5% with derivation (HwRS-F3); HW-SF-003 rewritten for external watchdog, HW-FR-010 added (Q1); HW-FR-005 design target 1.5 mA (Q2); Req. CL = CL3 for all safety-relevant rows, closure policy added to §1 (Q4). |
| 0.3.0 | 2026-09-21 | H-06 (issue #49), QA pass-1 gate: HW-FR-004 extended to the ISO 16750-2:2012 Test A load-dump transient pulse 5a (without centralized suppression; unclamped generator Us per §4.6.4.2.2, Figure 8 / Table 5 — confirmed against the normative source, which places the Test A parameters in Table 5, not Table 6); pulse 5b (Test B, §4.6.4.2.3 / Table 6) unchanged. Explicit verification posture recorded: T1 simulation pending, T4 bench per issue #41; no qualification claim, no new traceability rows, no status promotion. No other requirement touched. |
