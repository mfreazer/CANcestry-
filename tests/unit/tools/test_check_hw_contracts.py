"""H-04 negative contracts: HW-SF-001..005, HW-FR-001..010 provenance."""
import copy
import json
import shutil
from pathlib import Path

import pytest

from conftest import REPO_ROOT
from ci import check_hw_contracts as gate


@pytest.fixture
def repo(tmp_path):
    for directory in ('schemas/hw', 'hw/bom', 'hw/model', 'hw/tests/oracles', 'docs/hw'):
        shutil.copytree(REPO_ROOT / directory, tmp_path / directory)
    return tmp_path


def update(root, path, change):
    document = gate.read_json(root / path)
    change(document)
    (root / path).write_text(json.dumps(document), encoding='utf-8')


def test_repository_contracts_and_exports(repo):
    assert gate.main([str(repo)]) == 0
    assert gate.main([str(repo), '--export']) == 0
    for name in ('hw/tests/oracles/registry.csv', 'docs/hw/mbse-plan.md',
                 'docs/hw/virtual-bench-plan.md'):
        assert (repo / name).read_bytes() == (REPO_ROOT / name).read_bytes()
    assert gate.main([str(repo)]) == 0


@pytest.mark.parametrize('path,name', [
    (gate.REGISTRY, 'oracle-registry'), (gate.BRIDGE, 'bridge'),
    (gate.TRADES, 'trade'), ('hw/bom/datasheets/extract-mcu-vbat.json', 'datasheet-extract')])
def test_every_object_rejects_extra_properties(path, name):
    """No nested object in any of the four contracts is an escape hatch."""
    original = gate.read_json(REPO_ROOT / path)
    schema = gate.read_json(REPO_ROOT / f'schemas/hw/hw-{name}-0.1.0.schema.json')

    def mutate_at_every_object(value, location=()):
        if isinstance(value, dict):
            yield location
            for k, v in value.items():
                yield from mutate_at_every_object(v, (*location, k))
        elif isinstance(value, list):
            for k, v in enumerate(value):
                yield from mutate_at_every_object(v, (*location, k))

    for location in mutate_at_every_object(original):
        document = copy.deepcopy(original)
        target = document
        for key in location:
            target = target[key]
        target['unexpected'] = True
        with pytest.raises(ValueError, match='Additional properties'):
            gate.validate(document, schema, path)


@pytest.mark.parametrize('change', [
    lambda d: d['oracles'][0].update(oracle_id='OR-005b'),
    lambda d: d['oracles'][0].update(**{'class': '(a)'}),
    lambda d: d['oracles'][0].update(source_citation='uncited source'),
    lambda d: d['oracles'][0].pop('source_citation'),
    lambda d: d['oracles'][0].update(validation_gap=' '),
    lambda d: d['oracles'].append(d['oracles'][0]),
    lambda d: d['oracles'][0].update(serves=['HW-SF-999']),
])
def test_invalid_registry_fails_closed(repo, change, capsys):
    update(repo, gate.REGISTRY, change)
    assert gate.main([str(repo), '--export']) == 1
    assert 'FAIL:' in capsys.readouterr().out
    # Never overwrite compatibility CSV with an invalid JSON registry.
    assert (repo / 'hw/tests/oracles/registry.csv').read_bytes() == (
        REPO_ROOT / 'hw/tests/oracles/registry.csv').read_bytes()


@pytest.mark.parametrize('source,valid', [
    ({'source_pdf_hash': None, 'source_url': 'https://example.com/source'}, True),
    ({'source_pdf_hash': 'sha256:' + 'a'*64}, True),
    ({'source_pdf_hash': None}, False),
    ({'source_pdf_hash': 'sha256:' + 'A'*64}, False),
    ({'source_pdf_hash': 'sha256:abc'}, False),
    ({'source_pdf_hash': None, 'source_url': ''}, False),
    ({'source_pdf_hash': None, 'source_url': 'not-a-url'}, False),
    ({'source_url': 'https://example.com/source'}, False),
])
def test_pdf_hash_or_explicit_fallback(source, valid):
    document = gate.read_json(REPO_ROOT / 'hw/bom/datasheets/extract-mcu-vbat.json')
    document['source'].pop('source_pdf_hash')
    document['source'].pop('source_url')
    document['source'].update(source)
    schema = gate.read_json(REPO_ROOT / 'schemas/hw/hw-datasheet-extract-0.1.0.schema.json')
    if valid:
        gate.validate(document, schema, 'extract')
    else:
        with pytest.raises(ValueError):
            gate.validate(document, schema, 'extract')


@pytest.mark.parametrize('path', ['hw/tests/oracles/registry.csv',
                                 'docs/hw/mbse-plan.md', 'docs/hw/virtual-bench-plan.md'])
def test_generated_view_drift_fails(repo, path):
    target = repo / path
    target.write_text(target.read_text().replace('RC hold-up', 'tampered')
                      if 'virtual' in path or path.endswith('.csv') else
                      target.read_text().replace('Third CAN Controller', 'tampered'))
    assert gate.main([str(repo)]) == 1
    assert gate.main([str(repo), '--export']) == 0
    assert gate.main([str(repo)]) == 0


@pytest.mark.parametrize('change', [
    lambda d: d['trades'].__setitem__(0, d['trades'][1]),
    lambda d: d['trades'][0].update(selected_option='invented decision'),
    lambda d: d['trades'][0].update(motivating_requirement_or_risk='a plan, not an HwRS ID'),
    lambda d: d['trades'][0]['options_considered'].append(d['trades'][0]['options_considered'][0]),
    lambda d: d['trades'][0]['decision_criteria'].append(d['trades'][0]['decision_criteria'][0]),
])
def test_invalid_trade_records_fail(repo, change):
    update(repo, gate.TRADES, change)
    assert gate.main([str(repo)]) == 1


def decided(document):
    trade = document['trades'][0]
    trade['status'] = 'decided'
    trade['selected_option'] = trade['options_considered'][0]['name']
    for criterion in trade['decision_criteria']:
        criterion['weight'] = 1 / len(trade['decision_criteria'])
    trade['options_considered'][1]['rationale_for_rejection'] = 'Fixture rejection'


@pytest.mark.parametrize('bad', ['selection', 'weight', 'rejection', None])
def test_decided_trade_consistency(repo, bad):
    update(repo, gate.TRADES, decided)
    def change(document):
        trade = document['trades'][0]
        if bad == 'selection':
            trade['selected_option'] = 'absent option'
        elif bad == 'weight':
            trade['decision_criteria'][0]['weight'] = 0
        elif bad == 'rejection':
            trade['options_considered'][1]['rationale_for_rejection'] = None
    update(repo, gate.TRADES, change)
    assert gate.main([str(repo), '--export']) == (0 if bad is None else 1)


@pytest.mark.parametrize('contents', ['{}', '{', '{"schema_version":"0.1.0","schema_version":"0.2.0"}'])
def test_bad_json_and_duplicate_keys(repo, contents):
    (repo / gate.REGISTRY).write_text(contents)
    assert gate.main([str(repo)]) == 1


def test_missing_extract_and_input(repo):
    shutil.rmtree(repo / 'hw/bom/datasheets')
    assert gate.main([str(repo)]) == 1
    (repo / gate.REGISTRY).unlink()
    assert gate.main([str(repo)]) == 1


@pytest.mark.parametrize('value', ['missing', '<!-- END TRADES --><!-- BEGIN TRADES -->',
                                  '<!-- BEGIN TRADES --><!-- BEGIN TRADES --><!-- END TRADES -->'])
def test_bad_render_markers(repo, value):
    (repo / 'docs/hw/mbse-plan.md').write_text(value)
    assert gate.main([str(repo)]) == 1


def test_markdown_escaping():
    assert gate.markdown_table(['A'], [['pipe|line\nnext']]).endswith('| pipe&#124;line<br>next |')
