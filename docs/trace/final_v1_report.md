# CANcestry v1.0.0-rc.1 candidate traceability report

| Field | Value |
|---|---|
| Release | v1.0.0-rc.1 candidate |
| Report version | 1.0.0-rc.1 |
| Date | 2026-09-18 |
| Requirement source | [`docs/software/SwRS.md`](../software/SwRS.md), [`docs/SyRS.md`](../SyRS.md) |
| Machine-readable matrix | [`traceability.csv`](traceability.csv) |
| Gate | `python3 ci/check_traceability.py .` |
| Status | Passing candidate software evidence; QA-EV-01 remains open; target hardware confirmation remains a vehicle-release prerequisite |

## 1. Executive result

The v1.0.0-rc.1 candidate software scope has complete traceability:

* **222** software/system requirement ids are defined by the SwRS and SyRS.
* **166** defined requirement ids are in the v1.0.0-rc.1 implemented scope and have
  at least one `passing` row: **166/166 = 100%**.
* **56** defined requirement ids are explicitly deferred to v1.1.0 in the
  ledger in section 5; none is silently absent.
* The CSV contains **288 rows**: **264 passing**, **24 planned**, and **0
  failed**. Planned rows are supplemental paths for requirements that already
  have a passing primary path and are also named in the deferred ledger where
  appropriate.
* The Phase 12 additions and QA-EV-01 correction are passing: six BMS
  requirements, six HIL requirements, five safety-case requirements and two
  reserved-slot requirements (**19/19 = 100%**).
* The traceability gate resolves every passing test id to a source artifact and
  checks every C test source for a requirement citation.

The claim is software evidence, not an ISO 26262 certificate. The HIL report
identifies which physical measurements must still be captured on the selected
board or QEMU/target adapter.

## 2. Reproduce the result

From the repository root:

```sh
python3 ci/check_traceability.py .
python3 tests/hil/hil_fault_injection.py --scenario all \
  --json-output build/hil_fault_injection.json
python3 -m pytest tests/hil/test_fault_injection.py \
  tests/hil/test_phase12_artifacts.py
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

The expected traceability output is a `PASS` line with the counts above. The
HIL JSON is deterministic: it contains no wall-clock time, random identifier
or host-dependent path.

## 3. In-scope v1.0.0-rc.1 coverage

The following requirement groups are the implemented v1.0.0-rc.1 candidate software scope.
The number in the last column is the number of distinct requirement ids with
at least one passing row, not the number of CSV rows.

| Requirement group | Passing ids | Evidence families | Coverage |
|---|---|---|---:|
| `SW-FR-CODEC-001..008` | 8 | codec unit/conformance, parity, no-allocation gate | 8/8 |
| `SW-FR-EVENT-001..008` | 8 | event unit/conformance, reserved-slot and escalation tests | 8/8 |
| `SW-FR-RECIPE-001..003,005..007` | 6 | recipe unit tests, gateway loop, governor tests | 6/6 |
| `SW-FR-FSM-001..055` | 55 | loader, runtime and conformance suites | 55/55 |
| `SW-FR-GOV-005..006` | 2 | fail-closed recipe/FSM/gateway paths | 2/2 |
| `SW-FR-HAL-001..009,011..012` | 11 | mock HAL, ring/fault/timestamp tests, schema | 11/11 |
| `SW-FR-CANFD-001..006` | 6 | codec/HAL/FSM/recipe FD paths and schema | 6/6 |
| `SW-FR-TOOL-001..010` | 10 | yaml2c, trace viewer, schema checks | 10/10 |
| `SW-FR-TP-001..010` | 10 | ISO-TP RX/TX conformance and archive scan | 10/10 |
| `SW-FR-UDS-001..008` | 8 | UDS service/governor/transport integration | 8/8 |
| `SW-FR-BM-001..008` | 8 | Cortex-M latency, watchdog, linker and archive tests | 8/8 |
| `SW-FR-BMS-001..006` | 6 | `BMS-GOV-001..008`, pure governor archive | 6/6 |
| `SW-FR-HIL-001..006` | 6 | `HIL-BUSOFF-001`, `HIL-CRC-001`, `HIL-BROWNOUT-001`, report check | 6/6 |
| `SW-FR-SAFETY-001..005` | 5 | manual, FMEA/TSR, report and release-marker checks | 5/5 |
| Passing `SYS-*` ids | 17 | gateway, HAL, tool, event and no-allocation evidence | 17/17 |
| **Total defined and in candidate scope** | **166** | **all primary rows are `passing`** | **166/166 (100%)** |

The passing QA review identifiers (`QA-H02`, `QA-v0.2-R01`,
`QA-v0.2-R04`, `QA-v0.2-R06`, `QA-v0.2-R09`, `QA-v0.2-R10`) are supplementary
review evidence and are not counted in the 222 SwRS/SyRS requirement total.

## 4. Phase 12 evidence map

| Requirement | Implementation/document | Verification id | Result |
|---|---|---|---|
| `SW-FR-BMS-001` | `core/governor/bms_governor.c` | `BMS-GOV-006` | passing |
| `SW-FR-BMS-002` | fixed-point current/power calculation | `BMS-GOV-001`, `BMS-GOV-005` | passing |
| `SW-FR-BMS-003` | conservative ceil-current boundary | `BMS-GOV-005` | passing |
| `SW-FR-BMS-004` | derated power/torque output | `BMS-GOV-001` | passing |
| `SW-FR-BMS-005` | `GOVERNOR_INTERVENTION` fault output | `BMS-GOV-001` | passing |
| `SW-FR-BMS-006` | invalid/BMS-fault zero-output block | `BMS-GOV-003`, `BMS-GOV-004` | passing |
| `SW-FR-HIL-001` | `tests/hil/hil_fault_injection.py` | all three scenario ids | passing |
| `SW-FR-HIL-002` | hardware-first Bus-Off recovery model | `HIL-BUSOFF-001` | passing simulation |
| `SW-FR-HIL-003` | CRC rejection before software queue | `HIL-CRC-001` | passing simulation |
| `SW-FR-HIL-004` | BOR safe latch before power-down | `HIL-BROWNOUT-001` | passing simulation |
| `SW-FR-HIL-005` | deterministic simulation and limitation statement | `HIL-REPORT-001` | passing |
| `SW-FR-HIL-006` | structured HIL evidence report | `HIL-REPORT-001` | passing |
| `SW-FR-SAFETY-001` | final architecture/data-flow manual | `SAFETY-MANUAL-001` | passing |
| `SW-FR-SAFETY-002` | FMEA summary | `SAFETY-FMEA-001` | passing |
| `SW-FR-SAFETY-003` | ASIL-B-aligned TSR mapping | `SAFETY-TSR-001` | passing |
| `SW-FR-SAFETY-004` | this report and machine CSV | `TRACE-V1-001` | passing |
| `SW-FR-SAFETY-005` | version/changelog/release gate | `SAFETY-RELEASE-001` | passing |

### 4.1 Event reserve and hard-fault evidence

`EVENT-RESERVED-SLOTS-001..005` proves that the default queue reserve blocks
ordinary admission after `capacity - reserved_fault_slots` non-fault events,
admits faults into the reserved physical slots, covers the one-slot boundary,
and selects only the deterministic newest non-fault victim when physical
capacity is full. `HARD-FAULT-ESCALATION-001..002` binds
those semantics to the Cortex-M platform: HAL safe state is asserted first and
the IWDG reset path follows. QA-EV-01 is still open for formal review of this
evidence and target timing; passing tests do not authorize the final release.

### 4.2 BMS decision evidence

The canonical vector is 100,000 W at 400,000 mV, a 250,000 mA
`MaxDischargeCurrent` and 950 per-mille efficiency. `BMS-GOV-001..008`
cover the nominal, thermal, invalid, rounding, saturation, NULL and unknown
state cases. The pure governor returns
95,000 W, a proportional torque value, `DERATE`, and
`GOVERNOR_INTERVENTION`. The integration contract requires the returned value,
not 100,000 W, to be emitted. Invalid voltage/efficiency and a faulted BMS
return `BLOCK` with zero output. There are no mutable thresholds or heap
references in the governor archive.

### 4.3 HIL boundary evidence

The simulation deliberately proves negative properties:

* a corrupted frame is not merely ignored by software; it is rejected before
  the software queue;
* Bus-Off is asserted by the controller model before `FSM_SAFE_STATE`, and
  recovery requires healthy observations before TX authorization; and
* BOR safe output latch precedes the `MCU_POWERED_DOWN` trace entry.

The target-board measurements listed in
[`docs/qa/hil-fault-injection-report.md`](../qa/hil-fault-injection-report.md)
are not silently represented as passing hardware evidence.

## 5. v1.1.0 deferred ledger

The following 56 requirement ids are explicitly outside the v1.0.0-rc.1 candidate
scope. They are not missing: each has an owner and rationale. The existing
`planned` rows in `traceability.csv` are retained as forward-looking evidence
names and do not weaken the 100% claim for the implemented candidate scope.

| v1.1 owner | Deferred requirements | Rationale |
|---|---|---|
| Package/integrity layer | `SW-FR-PKG-001..007`, `SYS-FR-001`, `SYS-FR-002`, `SYS-FR-017`, `SYS-SF-006`, `SYS-SEC-001`, `SYS-SEC-002`, `SYS-IR-004` | Combined package manifests, compatibility, signing/hash verification, debug policy and stable package versioning require a separate product/integrity layer. |
| Physical CAN integration | `SW-FR-CAN-001..006`, `SYS-FR-008`, `SYS-IR-001..003`, `SYS-IR-006`, `SYS-NF-004`, `SYS-SF-005`, `SYS-SF-007`, `SW-FR-HAL-010` | Target network bit timing, live interface, physical Bus-Off/driver recovery, persistent storage, error counters and real CAN FD hardware require a selected board/network and lab evidence. |
| Logger/replay | `SW-FR-LOG-001..005`, `SYS-FR-011`, `SYS-FR-020`, `SYS-SEC-003` | Production log lifecycle, retrieval, privacy, clearing and replay are not part of the v1 runtime safety boundary. |
| Product simulator | `SW-FR-SIM-001..004`, `SYS-FR-012`, `SYS-FR-013` | A complete virtual-CAN product simulator and host management interface require a separate executable and interface specification. The Phase 12 HIL model covers only fault-boundary evidence. |
| Full policy governor | `SW-FR-GOV-001..004`, `SYS-SF-001`, `SYS-SF-003` | Token-bucket policy, complete TX ID allowlists, disabled-package policy and product SAFE-mode escalation need product configuration and vehicle-level review. The fail-closed stub and BMS governor are v1 evidence. |
| Mode/fault product state machine | `SYS-FR-015`, `SYS-FR-016`, `SYS-SF-004` | Complete product mode tables and all vehicle fault recovery transitions need the item HARA; the software safe latch and bounded Bus-Off path are still verified. |
| Recipe timing/static analysis/performance | `SW-FR-RECIPE-004`, `SYS-FR-007`, `SYS-SF-008`, `SYS-NF-003` | Timeout-watch product semantics, profile emulation, full static-analysis qualification and target event-to-action performance require later tooling/target data. |

The ledger expands ranges to the exact 56 ids in the machine check. A future
release may move an item into scope only after adding implementation evidence,
a passing row and a safety-owner review; deleting a deferred name without
that evidence is prohibited.

## 6. Release artifact checklist

| Artifact | Required state | Location |
|---|---|---|
| Version marker | `1.0.0-rc.1` | `VERSION` |
| Changelog | Phase 12 candidate entry; final release deferred | `CHANGELOG.md` |
| BMS governor | static, pure, unit/conformance covered | `core/governor/` |
| HIL harness | three hardware-first scenarios | `tests/hil/` |
| HIL report | simulation limitation and target procedure | `docs/qa/hil-fault-injection-report.md` |
| Safety Manual | architecture, FMEA, ASIL-B TSR mapping | `docs/safety/SafetyManual.md` |
| Traceability matrix | no failed row; all candidate ids passing | `docs/trace/traceability.csv` |
| Final report | counts, evidence and v1.1 ledger | this file |
| CI gates | build/tests, no-allocation and traceability green | CMake/CTest/CI |

## 7. Audit rules

The machine check enforces the following release properties:

1. CSV structure, ids, methods and statuses are valid;
2. every CSV software/system id is defined in the SwRS or SyRS;
3. every defined id is in the CSV or the explicit deferred ledger;
4. every passing test id resolves to a source artifact;
5. every C test source cites a requirement id; and
6. no requirement with only planned rows is omitted from the deferred ledger.

The authoritative command is the traceability gate, not the prose counts in
this report. If the matrix changes, regenerate/review this report and rerun
the gate before a final v1.0.0 release.
