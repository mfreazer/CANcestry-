"""Determinism guard for the committed HW-SF-002 / HW-FR-004 evidence manifests.

The ledger pins the evidence JSON by SHA-256. This test regenerates the
artifact from deterministic metadata and live source hashes, then reports an
exact unified diff if the committed bytes drift.

H-06 (issue #49) adds the pulse_5a_001 case manifest to the same guard; the
aggregate pulse_7637_001 manifest pins that artifact and its pending
placeholder plot-data view by hash. ``*.plot.json`` views stay under the
time-metadata discipline but may carry the schema-optional fixed
``generated_at`` (see ``_FORBIDDEN_VIEW_METADATA_KEYS``).

Requirements traced: HW-SF-002, HW-FR-004, HW-FR-009; HwAGENTS.md rule 5.
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
        "executed by FMPy as explicit CoSimulation) against oracle OR-001: "
        "HW-SF-002 sub-event "
        "(ii) brownout to BOR level 3 (2.8 V) for 50 ms, then sub-event "
        "(iii) removal to 0 V for 100 ms, worst-case durations. VBAT(t) "
        "asserted within the sim-case tolerance; retention floor 1.65 V "
        "held across the event. The 0.001 V tolerance is provisional pending "
        "vendor selection and future ESR/R_path sensitivity rerun."
    ),
    "not_covered": [
        "HW-SF-002 sub-event (i) IWDG reset: firmware/reset-domain behavior, "
        "future T2 virtual bench",
        "Charge-path turn-on and recharging dynamics: OR-001 is valid only "
        "with iCh=0; extended charge-path oracle remains planned, not verified",
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
        "hw/tests/oracles/registry.json": None,
        "docs/hw/tool-qualification.md": None,
        "ci/docker/Dockerfile": None,
        "ci/docker/base-image.digest": None,
        "schemas/hw/hw-datasheet-extract-0.1.0.schema.json": None,
        "schemas/hw/hw-oracle-registry-0.1.0.schema.json": None,
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

# H-06 (issue #49): the pulse 5a coverage record, shared verbatim by the
# aggregate pulse_7637_001 manifest and the pulse_5a_001 case manifest so the
# two can never disagree about the Test A disposition.
PULSE5A_COVERAGE = {
    'coverage': 'partial',
    'status': 'pending',
    'provisional': True,
    'oracle_credibility': 'CL0',
    'modeled': ('Unclamped generator level Us=79 V per ISO 16750-2:2012 '
                '§4.6.4.2.2 Figure 8 / Table 5 lower bound (footnote a pairs '
                'it with the lower Ri bound), bound from the schema-validated '
                'datasheet extract through the shared sim case '
                '(pulse_selector 8). Current fixture uses a 5 ms full linear '
                'edge to the 79 V absolute peak and td/3 decay with td=350 '
                'ms; Ri=0.5 ohm is unused metadata.'),
    'not_modeled': [
        'Figure 8 0.9(Us-UA)+UA / 0.1(Us-UA)+UA edge and duration measurement '
        'definitions; td/3 decay is not a qualified oracle.',
        'Unclamped-generator topology and exercised source impedance.',
        'Table 5 ranges beyond the lower-bound operating point (Us to 101 V, '
        'Ri to 4 ohm, td to 400 ms) and the 10 ms -5/+0 rising-slope '
        'tolerance.',
        'Ten-pulse sequence at 60 s intervals; loaded DUT response.',
        'T4 physical correlation (bench-pending).',
    ],
    'oracle_confidence': ('CL0 for complete Test A qualification: the OR-002 '
                          'pulse5a tabulation is an engineering fixture '
                          'transcribed from the normative extract, not a '
                          'qualified Figure 8 waveform. Pending #41; never '
                          'promote based on numeric regression alone.'),
    'follow_up': 'https://github.com/mfreazer/CANcestry-/issues/41',
}

PULSE_EXPECTED_EVIDENCE = {'case_id': 'pulse_7637_001',
 'credibility_level': 'CL0',
 'evidence_of': 'HW-FR-004 qualification is PENDING. The compiled stateless pulse '
                'source is exercised as a numerical regression only; successful '
                'peak/time/decay assertions do not establish a standard-qualified '
                'oracle or complete pulse coverage. Pulse 4 is an engineering dip '
                'fixture. Test B/5b has a corrected 35 V suppressed level but an '
                'incomplete source waveform. Test A/5a (H-06, #49) is a reduced '
                'unclamped-level fixture at the Table 5 lower-bound operating point; '
                'its invariant regression qualifies no Figure 8 waveform, and the '
                'pulse_5a_001 case manifest plus its pending placeholder plot share '
                'this disposition. See issue #41. No HW-FR-004 closure or passing '
                'plot is authorized by this manifest.',
 'not_covered': ['DUT immunity and loaded source/protection response; Ri is unused '
                 'metadata.',
                 'Pulse 4: current 13.5 V -> 6 V dip uses 1 ms fall, 20 ms dwell, 1 ms '
                 'rise. No full ISO 16750-2:2012 §4.6.3.2 starting profile, '
                 'temperature dependence, battery ESR variation or alternator '
                 'recovery. Qualification CL0, pending #41.',
                 'Pulse 5b: ISO 16750-2:2012 §4.6.4.2.3 Figure 9 / Table 6 is '
                 'normative. Us* corrected to 35 V, but separate unclamped Us, '
                 'suppressed plateau, source-impedance placement, standard '
                 'edge/duration definitions and repetition remain unqualified; pending '
                 '#41.',
                 'Pulse 5a: ISO 16750-2:2012 §4.6.4.2.2 Figure 8 / Table 5 is '
                 'normative (Test A, unsuppressed). The H-06 fixture binds '
                 'unclamped Us=79 V (Table 5 lower bound with the footnote a '
                 'lower-Ri pairing), td=350 ms and tr=5 ms, but the Figure 8 '
                 '0.9(Us-UA)+UA / 0.1(Us-UA)+UA edge and duration measurement '
                 'definitions, the unclamped-generator topology, exercised source '
                 'impedance and the 10-pulse repetition at 1 min intervals remain '
                 'unqualified; pending #41.',
                 'Linear edges, excursion conventions and td/3 decay are fixture '
                 'assumptions, not an independently qualified standard waveform '
                 'oracle.',
                 'Pulse bursts, parasitics, EMC and T4 physical correlation remain '
                 'pending.'],
 'oracle_id': 'OR-002',
 'pass': False,
 'provisional': True,
 'requirement_id': 'HW-FR-004',
 'sim_case': 'hw/tests/cases/pulse_7637_001.simcase.json',
 'source_hashes': {'ci/docker/Dockerfile': None,
                   'ci/docker/base-image.digest': None,
                   'docs/hw/tool-qualification.md': None,
                   'hw/model/CancestryLib/Power/PulseISO7637_2.mo': None,
                   'hw/model/CancestryLib/Power/package.mo': None,
                   'hw/model/CancestryLib/package.mo': None,
                   'hw/tests/cases/pulse_7637_001.simcase.json': None,
                   'hw/tests/oracles/or_002_pulse7637.py': None,
                   'hw/tests/oracles/registry.csv': None,
                   'hw/tests/oracles/registry.json': None,
                   'hw/tests/test_evidence_determinism.py': None,
                   'hw/tests/test_power_sim.py': None,
                   'hw/tests/test_pulse_sim.py': None,
                   'schemas/hw/hw-oracle-registry-0.1.0.schema.json': None,
                   'schemas/hw/hw-sim-0.1.0.schema.json': None,
                   'schemas/hw/hw-traceability-0.1.0.schema.json': None,
                   'schemas/hw/hw-pulse-evidence-0.1.0.schema.json': None,
                   'hw/bom/datasheets/extract-iso16750-2-2012.json': None,
                   'schemas/hw/hw-datasheet-extract-0.1.0.schema.json': None,
                   'docs/hw/pulse-coverage.md': None,
                   'hw/tests/evidence/pulse_5a_001.json': None,
                   'hw/tests/evidence/pulse_5a_001.plot.json': None,
                   'schemas/hw/hw-plot-data-0.1.0.schema.json': None},
 'tolerances': {'decay_tau_relative': 0.1,
                'duration_relative': 0.02,
                'peak_relative': 0.02,
                'time_to_peak_relative': 0.05},
 'tool_pins': {'fmpy': '0.3.24',
               'numpy': '2.1.3',
               'openmodelica': '1.24',
               'python': '>=3.10'},
 'trace': {'artifact': 'build/hw/pulse_7637_001_<pulse>.trace.csv (per-run artifacts, '
                       'gitignored)',
           'columns': 'time_s,v_v',
           'hashing': 'sha256 of every pulse trace, measured invariants and measured '
                      'tool pins recorded in build/hw/pulse_7637_001.runlog.json'},
 'unexercised_parameters': ['Ri_pulse1', 'Ri_pulse2a', 'Ri_pulse5b',
                            'Ri_pulse5a'],
 'schema_version': '0.1.0',
 'status': 'pending',
 'inherited_validation_gap': 'openmodelica: Compiler semantics outside independently '
                             'validated output remain unqualified; OR-001/OR-002 '
                             'regressions do not cover all translation and solver '
                             'behavior.',
 'pulses': {'pulse1': {'coverage': 'partial',
                       'status': 'pending',
                       'provisional': True,
                       'oracle_credibility': 'CL0',
                       'modeled': 'Reduced unloaded voltage-source fixture; numeric '
                                  'peak and timing regression only.',
                       'not_modeled': ['Standard-qualified complete waveform, pulse '
                                       'sequence, loaded DUT immunity and physical '
                                       'correlation.'],
                       'oracle_confidence': 'CL0 for qualification: numeric fixture '
                                            'agreement is not an independent standards '
                                            'qualification.',
                       'follow_up': 'https://github.com/mfreazer/CANcestry-/issues/41'},
            'pulse2a': {'coverage': 'partial',
                        'status': 'pending',
                        'provisional': True,
                        'oracle_credibility': 'CL0',
                        'modeled': 'Reduced unloaded voltage-source fixture; numeric '
                                   'peak and timing regression only.',
                        'not_modeled': ['Standard-qualified complete waveform, pulse '
                                        'sequence, loaded DUT immunity and physical '
                                        'correlation.'],
                        'oracle_confidence': 'CL0 for qualification: numeric fixture '
                                             'agreement is not an independent '
                                             'standards qualification.',
                        'follow_up': 'https://github.com/mfreazer/CANcestry-/issues/41'},
            'pulse2b': {'coverage': 'partial',
                        'status': 'pending',
                        'provisional': True,
                        'oracle_credibility': 'CL0',
                        'modeled': 'Reduced unloaded voltage-source fixture; numeric '
                                   'peak and timing regression only.',
                        'not_modeled': ['Standard-qualified complete waveform, pulse '
                                        'sequence, loaded DUT immunity and physical '
                                        'correlation.'],
                        'oracle_confidence': 'CL0 for qualification: numeric fixture '
                                             'agreement is not an independent '
                                             'standards qualification.',
                        'follow_up': 'https://github.com/mfreazer/CANcestry-/issues/41'},
            'pulse3a': {'coverage': 'partial',
                        'status': 'pending',
                        'provisional': True,
                        'oracle_credibility': 'CL0',
                        'modeled': 'Reduced unloaded voltage-source fixture; numeric '
                                   'peak and timing regression only.',
                        'not_modeled': ['Standard-qualified complete waveform, pulse '
                                        'sequence, loaded DUT immunity and physical '
                                        'correlation.'],
                        'oracle_confidence': 'CL0 for qualification: numeric fixture '
                                             'agreement is not an independent '
                                             'standards qualification.',
                        'follow_up': 'https://github.com/mfreazer/CANcestry-/issues/41'},
            'pulse3b': {'coverage': 'partial',
                        'status': 'pending',
                        'provisional': True,
                        'oracle_credibility': 'CL0',
                        'modeled': 'Reduced unloaded voltage-source fixture; numeric '
                                   'peak and timing regression only.',
                        'not_modeled': ['Standard-qualified complete waveform, pulse '
                                        'sequence, loaded DUT immunity and physical '
                                        'correlation.'],
                        'oracle_confidence': 'CL0 for qualification: numeric fixture '
                                             'agreement is not an independent '
                                             'standards qualification.',
                        'follow_up': 'https://github.com/mfreazer/CANcestry-/issues/41'},
            'pulse4': {'coverage': 'partial',
                       'status': 'pending',
                       'provisional': True,
                       'oracle_credibility': 'CL0',
                       'modeled': '13.5 V -> 6 V linear fall in 0.001 s; 6 V dwell for '
                                  '0.020 s; linear rise to 13.5 V in 0.001 s; nominal '
                                  'restored at 0.022 s and held thereafter.',
                       'not_modeled': ['Temperature dependence.',
                                       'Battery ESR variation / loaded source '
                                       'impedance.',
                                       'Alternator recovery dynamics.',
                                       'ISO 16750-2:2012 §4.6.3.2 multi-stage starting '
                                       'profile and repeated starts.'],
                       'oracle_confidence': 'CL0: the modeled 1/20/1 ms dip has no '
                                            'independently qualified standard or '
                                            'measured oracle. Regression thresholds '
                                            '(2% peak, 5% edge, 2% duration) are '
                                            'numerical checks, not confidence or '
                                            'conformance.',
                       'follow_up': 'https://github.com/mfreazer/CANcestry-/issues/41'},
            'pulse5b': {'coverage': 'partial',
                        'status': 'pending',
                        'provisional': True,
                        'oracle_credibility': 'CL0',
                        'modeled': 'Legacy Us key represents Us*=35 V, corrected from '
                                   'ISO 16750-2:2012 §4.6.4.2.3 Figure 9/Table 6. '
                                   'Current fixture uses a 5 ms full edge and td/3 '
                                   'decay with td=350 ms; Ri=0.5 ohm is unused '
                                   'metadata.',
                        'not_modeled': ['Separate unclamped Us and centralized-clamp '
                                        'plateau/topology.',
                                        'Standard edge/duration measurement '
                                        'definitions; td/3 is not a qualified oracle.',
                                        'Loaded source impedance and protection '
                                        'response.',
                                        'Five-pulse sequence at 60 s intervals; DUT '
                                        'response.'],
                        'oracle_confidence': 'CL0 for complete Test B qualification: '
                                             'citation and clamp level are corrected, '
                                             'but the current decay waveform is not '
                                             'Figure 9. Pending #41; never promote '
                                             'based on numeric regression alone.',
                        'follow_up': 'https://github.com/mfreazer/CANcestry-/issues/41'},
            'pulse5a': PULSE5A_COVERAGE}}

# H-06 (issue #49): pulse_5a_001 case-manifest metadata. Same fail-closed
# disposition as the aggregate (pending / pass=false / CL0 / provisional) plus
# the mandatory pending_reason pointing at issue #41. It pins the shared
# sim-case chain but never the aggregate manifest or its own plot-data view:
# the plot-data pins this artifact and the aggregate pins both, so the hash
# chain stays acyclic and re-issuance has a single deterministic order
# (5a manifest -> plot-data -> render -> aggregate -> holdup -> ledger hash).
PULSE_5A_EXPECTED_EVIDENCE = {
    'schema_version': '0.1.0',
    'case_id': 'pulse_5a_001',
    'requirement_id': 'HW-FR-004',
    'oracle_id': 'OR-002',
    'status': 'pending',
    'pass': False,
    'provisional': True,
    'credibility_level': 'CL0',
    'pending_reason': (
        'ISO 16750-2:2012 Test A (legacy pulse 5a) waveform qualification is '
        'deferred to https://github.com/mfreazer/CANcestry-/issues/41: the '
        'H-06 fixture binds only the unclamped Us, td and tr values (plus '
        'unused Ri metadata) from the datasheet extract, while the Figure 8 '
        'waveform shape, its 0.9(Us-UA)+UA / 0.1(Us-UA)+UA edge and duration '
        'measurement definitions, the unclamped-generator topology, the '
        'exercised source impedance and the ten-pulse repetition at 1 min '
        'intervals remain unqualified (shape_qualification_pending in the '
        'extract). No qualification promotion is claimed; H-06 scope: issue '
        '#49.'),
    'evidence_of': (
        'HW-FR-004 pulse 5a qualification is PENDING. This case manifest '
        'records the reduced ISO 16750-2:2012 Test A (legacy pulse 5a, '
        'unsuppressed load dump) fixture added by H-06 (issue #49): unclamped '
        'Us=79 V (Table 5 lower bound with the footnote a lower-Ri pairing), '
        'td=350 ms, tr=5 ms, Ri=0.5 ohm unused metadata, bound from the '
        'schema-validated hw/bom/datasheets/extract-iso16750-2-2012.json '
        'through hw/tests/cases/pulse_7637_001.simcase.json (pulse_selector '
        '8). The executed checks are numerical invariant regressions against '
        'the OR-002 pulse5a tabulation; they establish no standard-qualified '
        'oracle, do not qualify the Figure 8 waveform and authorize no '
        'HW-FR-004 closure or passing plot.'),
    'not_covered': [
        'Figure 8 waveform shape and the 0.9(Us-UA)+UA / 0.1(Us-UA)+UA '
        'edge/duration measurement definitions; the fixture uses linear edges '
        'and td/3 decay (shape_qualification_pending in the extract; pending '
        '#41).',
        'Unclamped-generator topology with exercised source impedance Ri; '
        'Ri_pulse5a=0.5 ohm is unused metadata.',
        'Ten-pulse repetition at 1 min intervals, loaded DUT immunity and '
        'protection response.',
        'Table 5 upper operating point (Us=101 V with Ri=4 ohm) and the full '
        'Us/Ri/td ranges.',
        'T4 physical correlation: bench-pending per HwRS section 5 (oracle '
        'absence), issue #41.',
    ],
    'sim_case': 'hw/tests/cases/pulse_7637_001.simcase.json',
    'source_hashes': {
        'ci/docker/Dockerfile': None,
        'ci/docker/base-image.digest': None,
        'docs/hw/pulse-coverage.md': None,
        'docs/hw/tool-qualification.md': None,
        'hw/bom/datasheets/extract-iso16750-2-2012.json': None,
        'hw/model/CancestryLib/Power/PulseISO7637_2.mo': None,
        'hw/model/CancestryLib/Power/package.mo': None,
        'hw/model/CancestryLib/package.mo': None,
        'hw/tests/cases/pulse_7637_001.simcase.json': None,
        'hw/tests/oracles/or_002_pulse7637.py': None,
        'hw/tests/oracles/registry.csv': None,
        'hw/tests/oracles/registry.json': None,
        'hw/tests/test_evidence_determinism.py': None,
        'hw/tests/test_power_sim.py': None,
        'hw/tests/test_pulse_sim.py': None,
        'schemas/hw/hw-datasheet-extract-0.1.0.schema.json': None,
        'schemas/hw/hw-oracle-registry-0.1.0.schema.json': None,
        'schemas/hw/hw-pulse-evidence-0.1.0.schema.json': None,
        'schemas/hw/hw-sim-0.1.0.schema.json': None,
    },
    'tool_pins': {
        'fmpy': '0.3.24',
        'numpy': '2.1.3',
        'openmodelica': '1.24',
        'python': '>=3.10',
    },
    'inherited_validation_gap': (
        'openmodelica: Compiler semantics outside independently validated '
        'output remain unqualified; OR-001/OR-002 regressions do not cover '
        'all translation and solver behavior.'),
    'tolerances': {
        'peak_relative': 0.02,
        'time_to_peak_relative': 0.05,
        'decay_tau_relative': 0.1,
        'duration_relative': 0.02,
    },
    'trace': {
        'artifact': ('build/hw/pulse_7637_001_pulse5a.trace.csv (per-run '
                     'artifact, gitignored)'),
        'columns': 'time_s,v_v',
        'hashing': ('sha256 of the pulse5a trace, measured invariants and '
                    'measured tool pins recorded in '
                    'build/hw/pulse_7637_001.runlog.json'),
    },
    'unexercised_parameters': ['Ri_pulse5a'],
    'pulses': {'pulse5a': PULSE5A_COVERAGE},
}

_FORBIDDEN_METADATA_KEYS = re.compile(
    r'"(?:timestamp|generated_at|created_at|updated_at)"\s*:')

# A plot-data *view* (hw-plot-data-0.1.0) may carry the schema-optional fixed
# generated_at (visual-evidence-plan §3: SOURCE_DATE_EPOCH-derived or copied
# from an already-hashed input, never a wall clock) because the pinned
# renderer emits the optional GENERATED footer line and
# ci/check_hw_evidence.py requires every present footer line to agree with
# the embedded provenance comment. All other time metadata stays forbidden
# everywhere; view bytes are bound by the H-03 hash chain
# (renders/manifest.json) and the nightly --rerender gate, not by this guard.
_FORBIDDEN_VIEW_METADATA_KEYS = re.compile(
    r'"(?:timestamp|created_at|updated_at)"\s*:')


def _sha256_file(path):
    return "sha256:" + hashlib.sha256(path.read_bytes()).hexdigest()


def _regenerate_evidence(case_id="holdup_001"):
    """Rebuild canonical evidence bytes using live pinned-source hashes."""
    document = copy.deepcopy({"holdup_001": EXPECTED_EVIDENCE,
                              "pulse_7637_001": PULSE_EXPECTED_EVIDENCE,
                              "pulse_5a_001": PULSE_5A_EXPECTED_EVIDENCE}[case_id])
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


def _assert_stable_metadata(raw, document, forbidden=_FORBIDDEN_METADATA_KEYS):
    assert not forbidden.search(raw)
    absolute = [value for value in _string_values(document)
                if Path(value).is_absolute()
                or re.match(r"^[A-Za-z]:[\\/]", value)]
    assert not absolute, "evidence contains absolute paths: %r" % absolute


@pytest.mark.parametrize("case_id", ["holdup_001", "pulse_7637_001",
                                     "pulse_5a_001"])
def test_evidence_is_byte_for_byte_deterministic(case_id):
    """HW-EVIDENCE-DETERMINISM-001: canonical regeneration is identical."""
    committed = (EVIDENCE_PATH.parent / f"{case_id}.json").read_text(encoding="utf-8")
    regenerated = _regenerate_evidence(case_id)
    if committed != regenerated:
        diff = "".join(difflib.unified_diff(
            committed.splitlines(keepends=True),
            regenerated.splitlines(keepends=True),
            fromfile="committed evidence",
            tofile="regenerated evidence"))
        pytest.fail("evidence JSON is not deterministic; exact diff:\n%s" % diff)


def test_evidence_has_no_time_or_absolute_path_metadata():
    """HW-EVIDENCE-DETERMINISM-002: metadata stays portable and stable across all evidence files."""
    for path in (REPO_ROOT / "hw" / "tests" / "evidence").glob("*.json"):
        raw = path.read_text(encoding="utf-8")
        forbidden = (_FORBIDDEN_VIEW_METADATA_KEYS
                     if path.name.endswith(".plot.json")
                     else _FORBIDDEN_METADATA_KEYS)
        _assert_stable_metadata(raw, json.loads(raw), forbidden)
