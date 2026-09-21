"""H-04 real FMU numerical regression for HW-FR-004; qualification is pending.

No snapshots: reference/fixture scalars bound measured output features.
These checks do not qualify an incomplete pulse against a standard. Missing tools fail closed (only explicit local opt-out may skip).
Negative fixtures run in hw-fast alongside the real compiler/executor tests.

H-06 (#49) adds pulse 5a (ISO 16750-2:2012 §4.6.4.2.2 Test A, Figure 8 /
Table 5) invariant checks. They may pass as regression invariants against
the OR-002 5a engineering tabulation; they do NOT constitute standards
qualification, which remains deferred to #41 (honest split: numeric
regression green, qualification pending).
"""
from __future__ import annotations

import json
import math
import shutil
from types import SimpleNamespace
from unittest.mock import Mock

import pytest

from test_power_sim import (
    REPO_ROOT, BUILD_DIR, EVIDENCE_DIR, _json, _load_module,
    _require_toolchain, _validate_against_schema, build_fmu, measured_tool,
    sha256_file, write_trace_artifacts,
)

CASE_PATH = REPO_ROOT / 'hw/tests/cases/pulse_7637_001.simcase.json'
ORACLE_PATH = REPO_ROOT / 'hw/tests/oracles/or_002_pulse7637.py'
ORACLE = _load_module('or_002_pulse7637', ORACLE_PATH)
PULSES = tuple(ORACLE.PULSE_PARAMETERS)


def feature_grid(pulse, solver):
    """Fixed deterministic grids resolve each rise/decay, not a 1-us snapshot."""
    p = ORACLE.get_pulse_params(pulse)
    tr, td = p['tr'], p['td']
    points = {tr*i/solver['rise_intervals'] for i in range(solver['rise_intervals']+1)}
    intervals = max(solver['decay_intervals'], math.ceil(td / solver['step_s']))
    points.update(tr + td*i/intervals for i in range(intervals+1))
    points.update(tr + td + tr*i/solver['rise_intervals']
                  for i in range(1, solver['rise_intervals']+1))
    tail_intervals = max(1, math.ceil(tr / solver['step_s']))
    points.update(td + 2*tr + tr*i/tail_intervals
                  for i in range(1, tail_intervals+1))
    times = sorted(points)
    assert times[-1] <= solver['stop_s'], 'pulse exceeds sim-case stop ceiling'
    assert max(b-a for a, b in zip(times, times[1:])) <= solver['step_s'] * (1+1e-9)
    return times


def finish_event_iteration(fmu):
    """Bound FMI event iteration; a stalled/terminated FMU is never a pass."""
    for _ in range(100):
        needed, terminate, *_ = fmu.newDiscreteStates()
        assert not terminate, 'pulse FMU requested premature termination'
        if not needed:
            return
    raise AssertionError('pulse FMU event iteration did not converge')


def execute_pulse(fmu, pulse, case, parameters=None):
    """Execute the stateless pulse FMU via FMI 2.0 ModelExchange/FMPy.

    OpenModelica 1.24 CS cannot step this zero-state source. ModelExchange
    executes the same compiled equations, not a Python waveform substitute.
    There are no differential/discrete state variables to integrate; fail
    closed if that contract changes. Re-evaluate event relations at each
    sample so finite edges are not hidden by cached branch conditions.
    """
    import fmpy
    from fmpy.fmi2 import FMU2Model

    description = fmpy.read_model_description(str(fmu))
    # HW-FR-004 / N2: reject unsupported FMU-declared states before any
    # extraction or native-code instantiation; this is the pulse path only.
    assert description.modelExchange is not None, 'pulse FMU must support ModelExchange'
    assert description.numberOfContinuousStates == 0, 'pulse evaluator requires a stateless FMU'
    assert not any(v.variability == 'discrete' for v in description.modelVariables), \
        'pulse evaluator does not support discrete state variables'
    variables = {v.name: v.valueReference for v in description.modelVariables}
    params = parameters or case['parameters']
    assert set(params) <= variables.keys(), 'FMU omitted declared case parameters'
    directory = fmpy.extract(str(fmu))
    slave = FMU2Model(guid=description.guid, unzipDirectory=directory,
                     modelIdentifier=description.modelExchange.modelIdentifier,
                     instanceName=pulse)
    times = feature_grid(pulse, case['solver'])
    instantiated = False
    try:
        slave.instantiate()
        instantiated = True
        slave.setupExperiment(startTime=0.0, stopTime=times[-1])
        slave.setReal([variables[name] for name in params], list(params.values()))
        slave.setInteger([variables['pulse_selector']], [PULSES.index(pulse)+1])
        slave.enterInitializationMode()
        slave.exitInitializationMode()
        finish_event_iteration(slave)
        slave.enterContinuousTimeMode()
        voltages = [float(slave.getReal([variables['v_out']])[0])]
        for current in times[1:]:
            slave.setTime(current)
            slave.enterEventMode()
            finish_event_iteration(slave)
            slave.enterContinuousTimeMode()
            voltages.append(float(slave.getReal([variables['v_out']])[0]))
            _, terminate = slave.completedIntegratorStep()
            assert not terminate, 'pulse FMU requested premature termination'
        slave.terminate()
        return times, voltages
    finally:
        if instantiated:
            slave.freeInstance()
        shutil.rmtree(directory)


def assert_invariants(pulse, times, voltages, nominal, tolerances):
    """Measure features from the simulated trace and compare to OR-002 scalars."""
    import numpy as np

    t, v = np.asarray(times), np.asarray(voltages)
    assert len(t) == len(v) and len(t) > 10, 'insufficient pulse samples'
    assert np.all(np.isfinite(t)) and np.all(np.isfinite(v)), 'non-finite trace'
    assert t[0] == 0 and np.all(np.diff(t) > 0), 'invalid trace time axis'
    reference = ORACLE.expected_invariants(pulse, nominal)
    direction = 1 if reference['peak_v'] > nominal else -1
    excursion = direction * (v - nominal)
    peak_index = int(np.argmax(excursion))
    peak = float(v[peak_index])
    measured_peak = peak if reference['absolute_peak'] else peak - nominal
    expected_peak = reference['peak_reference_v']
    assert abs(measured_peak - expected_peak) <= abs(expected_peak)*tolerances['peak_relative'], \
        f'{pulse}: peak voltage invariant failed'
    time_to_peak = float(t[peak_index])
    expected_time = reference['time_to_peak_s']
    assert abs(time_to_peak - expected_time) <= expected_time*tolerances['time_to_peak_relative'], \
        f'{pulse}: time-to-peak invariant failed'
    tau = None
    if reference['decay_tau_s'] is not None:
        # Log-slope fit on the tail, not a comparison to copied waveform samples.
        window = ((t > expected_time + .15*reference['duration_s']) &
                  (t < expected_time + .75*reference['duration_s']))
        assert np.count_nonzero(window) > 10 and np.all(excursion[window] > 0), 'missing decay tail'
        slope = float(np.polyfit(t[window] - expected_time, np.log(excursion[window]), 1)[0])
        assert slope < 0, f'{pulse}: decay time constant invariant failed (non-decaying trace)'
        tau = -1/slope
        assert abs(tau-reference['decay_tau_s']) <= reference['decay_tau_s']*tolerances['decay_tau_relative'], \
            f'{pulse}: decay time constant invariant failed'
    # Detect shortened pulses and a missing return to nominal as well.
    recovered = np.flatnonzero((t > time_to_peak) & (excursion <= .01*excursion[peak_index]))
    assert len(recovered), f'{pulse}: missing recovery to nominal'
    duration = float(t[recovered[0]]) - time_to_peak
    expected_duration = reference['duration_s'] + (expected_time if tau is None else 0)
    assert abs(duration - expected_duration) <= expected_duration*tolerances['duration_relative'], \
        f'{pulse}: duration invariant failed'
    assert abs(float(v[-1]) - nominal) <= 1e-6, f'{pulse}: final voltage is not nominal'
    return {'peak_v': peak, 'time_to_peak_s': time_to_peak,
            'decay_tau_s': tau, 'duration_s': duration, 'n_samples': len(t)}


@pytest.fixture(scope='session')
def pulse_case():
    return _json(CASE_PATH)


@pytest.fixture(scope='session')
def pulse_fmu(pulse_case):
    _require_toolchain()
    return build_fmu(pulse_case, BUILD_DIR / pulse_case['case_id'], fmi_types=('me',))


def test_case_parameters_and_tolerances(pulse_case):
    _validate_against_schema(pulse_case, 'hw-sim-0.1.0.schema.json', 'pulse case')
    assert ORACLE.self_check()['pass']
    assert pulse_case['tolerances'] == {
        'peak_relative': .02, 'time_to_peak_relative': .05,
        'decay_tau_relative': .10, 'duration_relative': .02}
    assert pulse_case['solver']['sampling'] == 'pulse_features'
    for pulse in PULSES:
        ref = ORACLE.get_pulse_params(pulse)
        for key in ('Us', 'td', 'tr', 'Ri'):
            name = f'{key}_{pulse}'
            if key != 'Ri' or name in pulse_case['parameters']:
                assert pulse_case['parameters'][name] == ref[key]
    # ISO 16750-2:2012 §4.6.4.2.3 Figure 9/Table 6 (12 V Test B Us*=35 V).
    assert (pulse_case['parameters']['Us_pulse5b'], pulse_case['parameters']['Ri_pulse5b'],
            pulse_case['parameters']['td_pulse5b']) == (35.0, .5, .35)
    # ISO 16750-2:2012 §4.6.4.2.2 Figure 8/Table 5 (12 V Test A; H-06): unclamped
    # lower Us bound paired with the lower Ri bound per footnote a.
    assert (pulse_case['parameters']['Us_pulse5a'], pulse_case['parameters']['Ri_pulse5a'],
            pulse_case['parameters']['td_pulse5a']) == (79.0, .5, .35)


def test_fmu_pulse_invariants(pulse_fmu, pulse_case):
    tool = measured_tool(_require_toolchain())
    results, traces = {}, {}
    for pulse in PULSES:
        times, volts = execute_pulse(pulse_fmu, pulse, pulse_case)
        result = assert_invariants(pulse, times, volts, pulse_case['parameters']['V_nominal'],
                                   pulse_case['tolerances'])
        run = write_trace_artifacts(f'{pulse_case["case_id"]}_{pulse}', times, volts,
                                    result, tool, qualification_pending=True)
        results[pulse], traces[pulse] = result, run['trace']
    # Every numerical check must pass, but qualification remains pending.
    (BUILD_DIR / 'pulse_7637_001.runlog.json').write_text(json.dumps({
        'case_id': pulse_case['case_id'], 'pass': False, 'regression_pass': True,
        'status': 'pending', 'provisional': True, 'credibility_level': 'CL0',
        'tool_measured': tool,
        'results': results, 'traces': traces,
    }, sort_keys=True, indent=2)+'\n', encoding='utf-8')
    pins = _json(EVIDENCE_DIR / 'pulse_7637_001.json')['tool_pins']
    assert pins['openmodelica'] in tool['openmodelica']
    assert tool['fmpy'] == pins['fmpy'] and tool['numpy'] == pins['numpy']


@pytest.mark.parametrize('parameter,factor,message', [
    ('Us_pulse1', 1.2, 'peak voltage'), ('tr_pulse1', 1.2, 'time-to-peak'),
    ('td_pulse1', 1.2, 'decay time constant'),
])
def test_real_fmu_parameter_faults_are_detected(pulse_fmu, pulse_case, parameter, factor, message):
    """Negative FMU fixtures prove the harness detects compiler/output drift."""
    parameters = dict(pulse_case['parameters'])
    parameters[parameter] *= factor
    times, volts = execute_pulse(pulse_fmu, 'pulse1', pulse_case, parameters)
    with pytest.raises(AssertionError, match=message):
        assert_invariants('pulse1', times, volts, parameters['V_nominal'], pulse_case['tolerances'])


def synthetic_trace(pulse_case, amplitude=1.0, peak_scale=1.0, decay_scale=1.0):
    """Independent fabricated traces for invariant negatives, never evidence."""
    times = feature_grid('pulse1', pulse_case['solver'])
    p = ORACLE.get_pulse_params('pulse1')
    peak_time, tau = p['tr']*peak_scale, p['td']/3*decay_scale
    values = [13.5 + p['Us']*amplitude*(t/peak_time if t < peak_time else
              math.exp(-(t-peak_time)/tau) if t < peak_time+p['td'] else 0)
              for t in times]
    return times, values


@pytest.mark.parametrize('change,message', [
    ({'amplitude': 1.021}, 'peak voltage'), ({'peak_scale': 1.2}, 'time-to-peak'),
    ({'decay_scale': 1.101}, 'decay time constant'),
])
def test_invariant_negatives_without_toolchain(pulse_case, change, message):
    times, values = synthetic_trace(pulse_case, **change)
    with pytest.raises(AssertionError, match=message):
        assert_invariants('pulse1', times, values, 13.5, pulse_case['tolerances'])


def test_invariant_positive_without_toolchain(pulse_case):
    times, values = synthetic_trace(pulse_case)
    assert assert_invariants('pulse1', times, values, 13.5, pulse_case['tolerances'])['peak_v'] == -86.5


def test_evidence_matches_sources(pulse_case):
    document = _json(EVIDENCE_DIR / 'pulse_7637_001.json')
    for key in ('case_id', 'requirement_id', 'oracle_id'):
        assert document[key] == pulse_case[key]
    _validate_against_schema(document, 'hw-pulse-evidence-0.1.0.schema.json', 'pulse evidence')
    assert document['pass'] is False and document['provisional'] is True
    assert document['status'] == 'pending' and document['credibility_level'] == 'CL0'
    assert all(r['status'] == 'pending' for r in document['pulses'].values())
    # H-06 (#49): the aggregate records pulse 5a and pins the case manifest
    # plus its pending placeholder plot-data by hash (rule 9, transitive).
    assert set(document['pulses']) == set(PULSES)
    assert 'pulse5a' in document['pulses']
    assert document['not_covered'] == pulse_case['not_covered']
    for relative, digest in document['source_hashes'].items():
        assert sha256_file(REPO_ROOT / relative) == digest, f'evidence source drift: {relative}'
    for relative in ('hw/tests/evidence/pulse_5a_001.json',
                     'hw/tests/evidence/pulse_5a_001.plot.json'):
        assert relative in document['source_hashes'], f'aggregate must pin {relative}'


def test_5a_evidence_matches_sources(pulse_case):
    """HW-FR-004 / H-06 (#49): the pulse_5a_001 case manifest stays fail-closed.

    Invariant check on the qualification record, not a qualification: the
    manifest must remain pending / pass=false / CL0 / provisional, defer to
    issue #41 through pending_reason, carry exactly the pulse5a coverage
    record, mirror the aggregate's record and tool pins verbatim, and re-pin
    its live sources by hash. Nothing here authorizes a promotion.
    """
    document = _json(EVIDENCE_DIR / 'pulse_5a_001.json')
    aggregate = _json(EVIDENCE_DIR / 'pulse_7637_001.json')
    _validate_against_schema(document, 'hw-pulse-evidence-0.1.0.schema.json',
                             'pulse 5a evidence')
    assert document['case_id'] == 'pulse_5a_001'
    assert document['requirement_id'] == pulse_case['requirement_id'] == 'HW-FR-004'
    assert document['oracle_id'] == pulse_case['oracle_id'] == 'OR-002'
    assert document['sim_case'] == 'hw/tests/cases/pulse_7637_001.simcase.json'
    assert document['pass'] is False and document['provisional'] is True
    assert document['status'] == 'pending' and document['credibility_level'] == 'CL0'
    assert 'issues/41' in document['pending_reason']
    assert set(document['pulses']) == {'pulse5a'}
    record = document['pulses']['pulse5a']
    assert record['coverage'] == 'partial' and record['status'] == 'pending'
    assert record['provisional'] is True and record['oracle_credibility'] == 'CL0'
    assert record['follow_up'] == 'https://github.com/mfreazer/CANcestry-/issues/41'
    # The aggregate and the case manifest must never disagree about 5a.
    assert aggregate['pulses']['pulse5a'] == record
    assert document['tool_pins'] == aggregate['tool_pins']
    assert document['inherited_validation_gap'] == aggregate['inherited_validation_gap']
    assert document['tolerances'] == pulse_case['tolerances']
    assert document['unexercised_parameters'] == ['Ri_pulse5a']
    # Rule 9 mirror: every pinned source must match its live bytes.
    for relative, digest in document['source_hashes'].items():
        assert sha256_file(REPO_ROOT / relative) == digest, f'5a evidence source drift: {relative}'
    # The view inherits the disposition: plot-data pins this manifest and may
    # not improve its status (HwAGENTS rule 13; check_hw_evidence enforces the
    # full chain including the rendered bytes).
    plot = _json(EVIDENCE_DIR / 'pulse_5a_001.plot.json')
    _validate_against_schema(plot, 'hw-plot-data-0.1.0.schema.json', 'pulse 5a plot-data')
    assert plot['source_evidence_path'] == 'hw/tests/evidence/pulse_5a_001.json'
    assert plot['source_evidence_hash'] == sha256_file(EVIDENCE_DIR / 'pulse_5a_001.json')
    assert plot['status'] == 'pending' and plot['provisional'] is True
    assert plot['credibility_level'] == 'CL0' and plot['oracle_id'] == 'OR-002'
    assert plot['source_requirement'] == 'HW-FR-004'
    assert {series['role'] for series in plot['series']} == {
        'tolerance_lower', 'tolerance_upper'}, 'pending placeholder carries no data series'


@pytest.mark.parametrize('needed,terminate,message', [
    (True, False, 'did not converge'), (False, True, 'premature termination'),
    (False, False, None),
])
def test_event_iteration_fails_closed(needed, terminate, message):
    class FakeFMU:
        def newDiscreteStates(self):
            return needed, terminate, False, False, False, 0.0
    if message:
        with pytest.raises(AssertionError, match=message):
            finish_event_iteration(FakeFMU())
    else:
        finish_event_iteration(FakeFMU())


@pytest.mark.parametrize('pulse', PULSES)
def test_every_feature_grid_respects_declared_bounds(pulse_case, pulse):
    times = feature_grid(pulse, pulse_case['solver'])
    reference = ORACLE.get_pulse_params(pulse)
    assert times[0] == 0
    assert reference['tr'] in times
    assert reference['tr'] + reference['td'] in times
    assert times[-1] == pytest.approx(reference['td'] + 3*reference['tr'])
    assert len(times) >= pulse_case['solver']['rise_intervals'] + pulse_case['solver']['decay_intervals']


def test_feature_grid_fails_closed_on_truncated_time_ceiling(pulse_case):
    with pytest.raises(AssertionError, match='stop ceiling'):
        feature_grid('pulse2b', dict(pulse_case['solver'], stop_s=.1))


def test_test_b_reference_matches_schema_validated_source(pulse_case):
    """HW-FR-004: the corrected 35 V value comes from Table 6, not issue prose."""
    extract = _json(REPO_ROOT / 'hw/bom/datasheets/extract-iso16750-2-2012.json')
    _validate_against_schema(extract, 'hw-datasheet-extract-0.1.0.schema.json', 'ISO extract')
    values = {entry['name']: entry['value'] for entry in extract['entries']}
    p = ORACLE.get_pulse_params('pulse5b')
    assert p['Us'] == values['Us_star_12V'] == pulse_case['parameters']['Us_pulse5b']
    assert values['Ri_min_12V'] <= p['Ri'] <= values['Ri_max_12V']
    assert values['td_min_12V'] <= p['td'] <= values['td_max_12V']
    assert 'ISO 16750-2:2012 §4.6.4.2.3' in p['standard']
    assert 'Figure 9 / Table 6' in p['standard']


def test_test_a_reference_matches_schema_validated_source(pulse_case):
    """HW-FR-004 / H-06: the 5a fixture values come from Table 5, not issue prose.

    The OR-002 pulse5a tabulation is an engineering fixture pending #41 shape
    qualification; agreement with the schema-validated extract is a citation
    and range check (HwAGENTS rule 2 chain), never a standards-conformance
    verdict. Citation disposition: the H-06 issue body cited Table 6 for 5a;
    the normative source places Test A parameters in Table 5.
    """
    extract = _json(REPO_ROOT / 'hw/bom/datasheets/extract-iso16750-2-2012.json')
    _validate_against_schema(extract, 'hw-datasheet-extract-0.1.0.schema.json', 'ISO extract')
    values = {entry['name']: entry['value'] for entry in extract['entries']}
    p = ORACLE.get_pulse_params('pulse5a')
    # Table 5 footnote a pairing: the fixture takes the lower unclamped Us
    # bound together with the lower Ri bound.
    assert p['Us'] == values['Us_pulse5a_min_12V'] == pulse_case['parameters']['Us_pulse5a']
    assert values['Us_pulse5a_min_12V'] <= p['Us'] <= values['Us_pulse5a_max_12V']
    assert values['Ri_pulse5a_min_12V'] <= p['Ri'] <= values['Ri_pulse5a_max_12V']
    assert values['td_pulse5a_min_12V'] <= p['td'] <= values['td_pulse5a_max_12V']
    assert values['tr_pulse5a_min'] <= p['tr'] <= values['tr_pulse5a_max']
    assert pulse_case['parameters']['tr_pulse5a'] == p['tr']
    assert 'ISO 16750-2:2012 §4.6.4.2.2' in p['standard']
    assert 'Figure 8 / Table 5' in p['standard']
    # The extract itself carries the pending flag for the whole 5a block.
    assert any('shape_qualification_pending: true' in entry['condition']
               for entry in extract['entries'] if entry['name'].endswith('pulse5a_min_12V'))


def synthetic_pulse5a_trace(pulse_case, amplitude=1.0, peak_scale=1.0, decay_scale=1.0):
    """Independent fabricated pulse 5a traces for invariant checks, never evidence.

    Mirrors the reduced-model 5a convention (absolute unclamped level, linear
    edge over tr, td/3 exponential decay, snap back to nominal after td). The
    amplitude scales the 65.5 V excursion above nominal: at the 79 V absolute
    reference a 2.5% excursion fault (+1.64 V) exceeds the declared 2% peak
    tolerance (+-1.58 V) and must be detected.
    """
    times = feature_grid('pulse5a', pulse_case['solver'])
    p = ORACLE.get_pulse_params('pulse5a')
    peak_time, tau = p['tr']*peak_scale, p['td']/3*decay_scale
    excursion = (p['Us'] - 13.5)*amplitude
    values = [13.5 + excursion*(t/peak_time if t < peak_time else
              math.exp(-(t-peak_time)/tau) if t < peak_time+p['td'] else 0)
              for t in times]
    return times, values


def test_pulse5a_invariants_positive_without_toolchain(pulse_case):
    """H-06 (#49): 5a invariant regression against OR-002 — NOT qualification.

    Peak vs the OR-002 5a tabulation within the declared 2%, rise
    (time-to-peak) within 5% and td/3 decay within 10%. Passing here does
    not constitute standards qualification: shape qualification is deferred
    to #41 and every HW-FR-004 ledger/evidence status remains pending.
    """
    times, values = synthetic_pulse5a_trace(pulse_case)
    result = assert_invariants('pulse5a', times, values, 13.5, pulse_case['tolerances'])
    assert result['peak_v'] == 79.0
    assert result['time_to_peak_s'] == pytest.approx(0.005)
    assert result['decay_tau_s'] == pytest.approx(0.35/3, rel=0.1)


@pytest.mark.parametrize('change,message', [
    ({'amplitude': 1.025}, 'peak voltage'), ({'peak_scale': 1.2}, 'time-to-peak'),
    ({'decay_scale': 1.101}, 'decay time constant'),
])
def test_pulse5a_invariant_negatives_without_toolchain(pulse_case, change, message):
    """H-06: fabricated 5a drift is detected fail-closed; detection is not qualification."""
    times, values = synthetic_pulse5a_trace(pulse_case, **change)
    with pytest.raises(AssertionError, match=message):
        assert_invariants('pulse5a', times, values, 13.5, pulse_case['tolerances'])


def test_pulse4_implemented_regions_are_explicit(pulse_fmu, pulse_case):
    """HW-FR-004: verify the reported 1/20/1 ms fixture, not ISO conformance."""
    times, voltages = execute_pulse(pulse_fmu, 'pulse4', pulse_case)
    expected = [(0, 13.5), (.0005, 9.75), (.001, 6.0), (.011, 6.0),
                (.021, 6.0), (.0215, 9.75), (.022, 13.5), (.023, 13.5)]
    for time, voltage in expected:
        index = min(range(len(times)), key=lambda i: abs(times[i]-time))
        assert times[index] == pytest.approx(time, abs=1e-12)
        assert voltages[index] == pytest.approx(voltage, abs=1e-8)


@pytest.mark.parametrize('supports_me,states,variability,message', [
    (False, 0, 'continuous', 'must support ModelExchange'),
    (True, 1, 'continuous', 'requires a stateless FMU'),
    (True, -1, 'continuous', 'requires a stateless FMU'),
    (True, None, 'continuous', 'requires a stateless FMU'),
    (True, 0, 'discrete', 'does not support discrete state variables'),
])
def test_pulse_rejects_stateful_metadata_before_native_execution(
        monkeypatch, supports_me, states, variability, message):
    """HW-FR-004 / N2: metadata fixtures, not claimed extra FMU simulations."""
    import fmpy
    import fmpy.fmi2

    description = SimpleNamespace(
        modelExchange=SimpleNamespace(modelIdentifier='fixture') if supports_me else None,
        numberOfContinuousStates=states,
        modelVariables=[SimpleNamespace(name='v_out', valueReference=0,
                                        variability=variability)])
    reader = Mock(return_value=description)
    extract = Mock(side_effect=AssertionError('extraction must not be reached'))
    native = Mock(side_effect=AssertionError('native execution must not be reached'))
    monkeypatch.setattr(fmpy, 'read_model_description', reader)
    monkeypatch.setattr(fmpy, 'extract', extract)
    monkeypatch.setattr(fmpy.fmi2, 'FMU2Model', native)

    with pytest.raises(AssertionError, match=message):
        execute_pulse('fixture.fmu', 'pulse1', {'parameters': {}})
    reader.assert_called_once_with('fixture.fmu')
    extract.assert_not_called()
    native.assert_not_called()


def test_stateless_metadata_with_algebraic_output_reaches_extraction(monkeypatch):
    """HW-FR-004 / N2: continuous-valued algebraic output is not a state."""
    import fmpy
    import fmpy.fmi2

    description = SimpleNamespace(
        modelExchange=SimpleNamespace(modelIdentifier='fixture'),
        numberOfContinuousStates=0,
        modelVariables=[SimpleNamespace(name='v_out', valueReference=0,
                                        variability='continuous'),
                        SimpleNamespace(name='gain', valueReference=1,
                                        variability='fixed')])
    extract = Mock(side_effect=RuntimeError('accepted metadata; stop before native code'))
    native = Mock()
    monkeypatch.setattr(fmpy, 'read_model_description', Mock(return_value=description))
    monkeypatch.setattr(fmpy, 'extract', extract)
    monkeypatch.setattr(fmpy.fmi2, 'FMU2Model', native)

    with pytest.raises(RuntimeError, match='accepted metadata'):
        execute_pulse('fixture.fmu', 'pulse1', {'parameters': {'gain': 1.0}})
    extract.assert_called_once_with('fixture.fmu')
    native.assert_not_called()
