"""H-04 structural negatives: HW-SF-001..005, HW-FR-002/004/008/009.

The software-only job need not install capellambse; hw-fast pins it and runs
this entire suite with branch coverage. Missing capellambse still fails the CLI.
"""
import json
import shutil

import pytest

from conftest import REPO_ROOT

pytest.importorskip('capellambse')
from lxml import etree
from ci import check_capella_model as gate

XSI = '{http://www.w3.org/2001/XMLSchema-instance}type'


@pytest.fixture
def repo(tmp_path):
    for directory in ('hw/model', 'schemas/hw'):
        shutil.copytree(REPO_ROOT / directory, tmp_path / directory)
    for name in ('docs/hw/HwRS.md', 'docs/system/mode-fault-state-machine.md'):
        target = tmp_path / name
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(REPO_ROOT / name, target)
    return tmp_path


def xml_change(repo, change):
    path = repo / 'hw/model/capella/cancestry.capella'
    tree = etree.parse(str(path))
    change(tree)
    tree.write(str(path), encoding='UTF-8', xml_declaration=True)


def element(tree, uuid):
    return tree.xpath('//*[@id=$uuid]', uuid=uuid)[0]


def bridge_change(repo, change):
    path = repo / 'hw/model/bridge.json'
    document = json.loads(path.read_text())
    change(document['mappings'])
    path.write_text(json.dumps(document))


def run(repo, capsys, message=None):
    code = gate.main(['checker', str(repo)])
    output = capsys.readouterr().out
    assert code == (1 if message else 0), output
    assert (message or 'PASS:') in output
    return output


def test_repository_seed_passes(capsys):
    run(REPO_ROOT, capsys)


@pytest.mark.parametrize('change,message', [
    (lambda rows: rows.pop(), 'TestInterface'),
    (lambda rows: rows.append(rows[0]), 'exactly once'),
    (lambda rows: rows[0].update(la_component='UnknownLA'), 'UnknownLA'),
    (lambda rows: rows[0].update(modelica_block='CancestryLib.Power.Ghost'), 'Ghost'),
    (lambda rows: rows[1].update(modelica_block=rows[0]['modelica_block']), 'exactly once'),
    (lambda rows: rows[0].update(status='not_simulated', modelica_block=None,
                               rationale='not-yet-modeled'), 'Holdup'),
    (lambda rows: rows[0].update(modelica_block=None), "not of type 'string'"),
    (lambda rows: rows[2].update(modelica_block='CancestryLib.Power.Holdup'), "not of type 'null'"),
    (lambda rows: rows[2].update(rationale='anything goes'), 'rationale'),
    (lambda rows: rows[2].update(rationale='n/a'), 'rationale'),
    (lambda rows: rows[0].update(rationale='not-simulatable'), 'rationale'),
    (lambda rows: rows[0].update(status='unknown'), 'status'),
    (lambda rows: rows[0].pop('rationale'), 'rationale'),
    (lambda rows: rows[0].update(coverage_note=''), 'coverage_note'),
    (lambda rows: rows[0].update(extra='not allowed'), 'Additional properties'),
])
def test_bridge_rejects_structural_drift(repo, capsys, change, message):
    bridge_change(repo, change)
    run(repo, capsys, message)


def test_live_la_inventory_not_hard_coded(repo, capsys):
    xml_change(repo, lambda tree: element(tree, 'la-comp-testinterface').set('name', 'RenamedAccess'))
    run(repo, capsys, 'RenamedAccess')
    bridge_change(repo, lambda rows: rows[-1].update(la_component='RenamedAccess'))
    run(repo, capsys)


@pytest.mark.parametrize('name', ['RetentionDomain', ''])
def test_ambiguous_la_names_rejected(repo, capsys, name):
    xml_change(repo, lambda tree: element(tree, 'la-comp-testinterface').set('name', name))
    run(repo, capsys, 'LA')


def test_unused_new_model_is_not_silently_ignored(repo, capsys):
    (repo / 'hw/model/CancestryLib/Power/NewPlant.mo').write_text(
        'within CancestryLib.Power;\nblock NewPlant\nend NewPlant;\n')
    run(repo, capsys, 'NewPlant')
    bridge_change(repo, lambda rows: rows[-1].update(status='simulated',
                  modelica_block='CancestryLib.Power.NewPlant', rationale='n/a'))
    run(repo, capsys)


@pytest.mark.parametrize('text,message', [
    ('within Wrong; model Holdup end Holdup;', 'within/name'),
    ('within CancestryLib.Power; model Wrong end Wrong;', 'within/name'),
    ('model Holdup end Holdup;', 'top-level'),
    ('within CancestryLib.Power; model Holdup model Nested end Nested; end Holdup;', 'top-level'),
    ('within CancestryLib.Power; model Holdup end Wrong;', 'matching model/block end'),
])
def test_modelica_inventory_fails_closed(repo, capsys, text, message):
    (repo / 'hw/model/CancestryLib/Power/Holdup.mo').write_text(text)
    run(repo, capsys, message)


def test_modelica_comments_strings_and_packages_are_not_blocks(repo, capsys):
    path = repo / 'hw/model/CancestryLib/Power/Holdup.mo'
    path.write_text('// model Ghost end Ghost;\n/* block Other */\n'
                    'within CancestryLib.Power; model Holdup "model Fake"\n'
                    'end Holdup;')
    run(repo, capsys)


def test_empty_modelica_inventory(repo, capsys):
    shutil.rmtree(repo / 'hw/model/CancestryLib')
    run(repo, capsys, 'No top-level Modelica')


@pytest.mark.parametrize('file', ['hw/model/bridge.json', 'hw/model/capella/cancestry.aird'])
def test_missing_required_files(repo, capsys, file):
    (repo / file).unlink()
    run(repo, capsys, 'missing' if file.endswith('.aird') else 'Bridge validation failed')


def test_bad_bridge_json(repo, capsys):
    (repo / 'hw/model/bridge.json').write_text('{')
    run(repo, capsys, 'Bridge validation failed')


def test_orphan_block(repo, capsys):
    def orphan(tree):
        child = element(tree, 'la-comp-testinterface')
        element(tree, 'la-comp-pkg').append(child)
    xml_change(repo, orphan)
    run(repo, capsys, 'Orphan block')


def test_missing_requirement(repo, capsys):
    def delete(tree):
        req = element(tree, 'cap-req-hw_sf_001')
        req.getparent().remove(req)
    xml_change(repo, delete)
    run(repo, capsys, 'HW-SF-001')


def test_unknown_hwrs_id(repo, capsys):
    xml_change(repo, lambda tree: element(tree, 'cap-req-hw_sf_001').set('hwrs_id', 'HW-SF-999'))
    run(repo, capsys, 'absent from HwRS.md')


def test_pa_without_la_realization(repo, capsys):
    def delete(tree):
        obj = element(tree, 'pa-real-mcu')
        obj.getparent().remove(obj)
    xml_change(repo, delete)
    run(repo, capsys, 'has no LA parent/realization link')


def test_corrupt_model(repo, capsys):
    (repo / 'hw/model/capella/cancestry.capella').write_text('<broken')
    run(repo, capsys, 'Failed to load Capella model')


def test_invalid_root(capsys):
    run('/nonexistent/directory', capsys, 'is not a directory')
