"""Determinism guard for the committed H-01 evidence artifact.

The ledger pins the evidence JSON by SHA-256. This test regenerates the
artifact from deterministic metadata and live source hashes, then reports an
exact unified diff if the committed bytes drift.

Requirements traced: HW-SF-002, HW-PLAN section 10.8, HwAGENTS.md rule 5.
Test ids: HW-EVIDENCE-DETERMINISM-001..002.
"""

from __future__ import annotations

import copy
import difflib
import hashlib
import json
import re
from pathlib import Path

import pytest

REPO_ROOT = Path(__file__).resolve().parents[2]
EVIDENCE_PATH = REPO_ROOT / "hw" / "tests" / "evidence" / "holdup_001.json"

# Keep artifact metadata separate from generated source digests. This makes
# regeneration independent of the committed artifact and prevents a changed
# timestamp/path/order from becoming self-justifying.
EXPECTED_EVIDENCE = {
    "case_id": "holdup_001",
    "credibility_level": "CL2",
    "evidence_of": (
        "T1 block simulation of CancestryLib.Power.Holdup (FMU via omc, "
        "executed with FMPy) against oracle OR-001: HW-SF-002 sub-event "
        "(ii) brownout to BOR level 3 (2.8 V) for 50 ms, then sub-event "
        "(iii) removal to 0 V for 100 ms, worst-case durations. VBAT(t) "
        "asserted within the sim-case tolerance; retention floor 1.65 V "
        "held across the event. The 0.001 V tolerance is provisional pending "
        "H-02 vendor selection and ESR/R_path sensitivity rerun."
    ),
    "not_covered": [
        "HW-SF-002 sub-event (i) IWDG reset: firmware/reset-domain behavior, "
        "T2 virtual bench (H-02/H-03)",
        "Charge-path turn-on and recharging dynamics: OR-001 is valid only "
        "with iCh=0; a future H-02 oracle covers this behavior",
        "T4 physical correlation (real brownout shapes, golden measurement): "
        "bench-pending per HwRS section 5 seed list (oracle absence)",
    ],
    "oracle_id": "OR-001",
    "pass": True,
    "provisional": True,
    "requirement_id": "HW-SF-002",
    "sim_case": "hw/tests/cases/holdup_001.simcase.json",
    "source_hashes": {
        "hw/bom/bom.json": None,
        "hw/bom/datasheets/extract-holdup-cap.json": None,
        "hw/bom/datasheets/extract-mcu-vbat.json": None,
        "hw/model/CancestryLib/Power/Holdup.mo": None,
        "hw/model/CancestryLib/Power/package.mo": None,
        "hw/model/CancestryLib/package.mo": None,
        "hw/tests/cases/holdup_001.simcase.json": None,
        "hw/tests/oracles/or_001_holdup.py": None,
        "hw/tests/oracles/registry.csv": None,
        "hw/tests/test_evidence_determinism.py": None,
        "hw/tests/test_power_sim.py": None,
        "schemas/hw/hw-bom-0.1.0.schema.json": None,
        "schemas/hw/hw-sim-0.1.0.schema.json": None,
        "schemas/hw/hw-traceability-0.1.0.schema.json": None,
    },
    "tolerance_v": 0.001,
    "tool_pins": {
        "fmpy": "0.3.24",
        "numpy": "2.1.3",
        "openmodelica": "1.24",
        "python": ">=3.10",
    },
    "trace": {
        "artifact": "build/hw/holdup_001.trace.csv (per-run artifact, gitignored)",
        "columns": "time_s,v_v",
        "hashing": (
            "sha256 of each run's trace recorded in "
            "build/hw/holdup_001.runlog.json (HwAGENTS.md rule 5: hashed "
            "traces, deterministic evidence)"
        ),
    },
    "unexercised_parameters": ["R_path"],
}

_FORBIDDEN_METADATA_KEYS = re.compile(
    r'"(?:timestamp|generated_at|created_at|updated_at)"\s*:')


def _sha256_file(path):
    return "sha256:" + hashlib.sha256(path.read_bytes()).hexdigest()


def _regenerate_evidence():
    """Rebuild canonical evidence bytes using live pinned-source hashes."""
    document = copy.deepcopy(EXPECTED_EVIDENCE)
    document["source_hashes"] = {
        relative: _sha256_file(REPO_ROOT / relative)
        for relative in sorted(document["source_hashes"])
    }
    return json.dumps(document, sort_keys=True, indent=2) + "\n"


def _string_values(value):
    if isinstance(value, dict):
        for child in value.values():
            yield from _string_values(child)
    elif isinstance(value, list):
        for child in value:
            yield from _string_values(child)
    elif isinstance(value, str):
        yield value


def _assert_stable_metadata(raw, document):
    assert not _FORBIDDEN_METADATA_KEYS.search(raw)
    absolute = [value for value in _string_values(document)
                if Path(value).is_absolute()
                or re.match(r"^[A-Za-z]:[\\/]", value)]
    assert not absolute, "evidence contains absolute paths: %r" % absolute


def test_evidence_is_byte_for_byte_deterministic():
    """HW-EVIDENCE-DETERMINISM-001: canonical regeneration is identical."""
    committed = EVIDENCE_PATH.read_text(encoding="utf-8")
    regenerated = _regenerate_evidence()
    if committed != regenerated:
        diff = "".join(difflib.unified_diff(
            committed.splitlines(keepends=True),
            regenerated.splitlines(keepends=True),
            fromfile="committed evidence",
            tofile="regenerated evidence"))
        pytest.fail("evidence JSON is not deterministic; exact diff:\n%s" % diff)


def test_evidence_has_no_time_or_absolute_path_metadata():
    """HW-EVIDENCE-DETERMINISM-002: metadata stays portable and stable."""
    raw = EVIDENCE_PATH.read_text(encoding="utf-8")
    _assert_stable_metadata(raw, json.loads(raw))
    generated = _regenerate_evidence()
    _assert_stable_metadata(generated, json.loads(generated))
