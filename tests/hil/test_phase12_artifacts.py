"""Phase 12 release artifact checks.

Implements: SW-FR-HIL-005..006 and SW-FR-SAFETY-001..005.
Test ids: HIL-REPORT-001, SAFETY-MANUAL-001, SAFETY-FMEA-001,
SAFETY-TSR-001, TRACE-V1-001, SAFETY-RELEASE-001.
"""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]


def test_hil_report_identifies_backend_and_limitation():
    """HIL-REPORT-001: simulation status is explicit and reproducible."""

    report = (ROOT / "docs/qa/hil-fault-injection-report.md").read_text(encoding="utf-8")
    assert "Deterministic software hardware-boundary simulation" in report
    assert "Target hardware result" in report
    assert "HIL-BUSOFF-001" in report
    assert "HIL-CRC-001" in report
    assert "HIL-BROWNOUT-001" in report


def test_safety_manual_contains_architecture_fmea_and_tsr_mapping():
    """SAFETY-MANUAL-001, SAFETY-FMEA-001, SAFETY-TSR-001."""

    manual = (ROOT / "docs/safety/SafetyManual.md").read_text(encoding="utf-8")
    assert "## 3. System Architecture & Data Flow" in manual
    assert "## 5. Failure Modes and Effects Analysis summary" in manual
    assert "## 6. ISO 26262 ASIL-B Technical Safety Requirement Mapping" in manual
    assert "TSR-BMS-01" in manual
    assert "SW-FR-SAFETY-003" in manual


def test_final_trace_report_has_v1_scope_and_deferred_ledger():
    """TRACE-V1-001: all v1 claims and v1.1 deferrals are named."""

    report = (ROOT / "docs/trace/final_v1_report.md").read_text(encoding="utf-8")
    assert "100%" in report
    assert "v1.1.0" in report
    assert "SW-FR-BMS-001..006" in report
    assert "SW-FR-HIL-001..006" in report
    assert "SW-FR-SAFETY-001..005" in report


def test_release_markers_and_no_failed_rows():
    """SAFETY-RELEASE-001: v1 release markers are internally consistent."""

    # The final version bump is explicitly deferred until QA-EV-01 closes.
    assert (ROOT / "VERSION").read_text(encoding="utf-8").strip() == "1.0.0-rc.1"
    assert "QA-EV-01" in (ROOT / "docs/versions.md").read_text(encoding="utf-8")
    changelog = (ROOT / "CHANGELOG.md").read_text(encoding="utf-8")
    assert "Phase 12" in changelog
    matrix = (ROOT / "docs/trace/traceability.csv").read_text(encoding="utf-8")
    assert ",failed\n" not in matrix
