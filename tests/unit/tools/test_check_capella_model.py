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


def first_not_simulated(rows):
    """The first not_simulated bridge row, independent of row order.

    H-11 (#64) inserted a simulated row into the seed, so an index-based
    negative target would silently retarget a different row (and pass for the
    wrong reason). The negative cases below select by property instead.
    """
    return next(row for row in rows if row['status'] == 'not_simulated')


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
    (lambda rows: first_not_simulated(rows).update(
        modelica_block='CancestryLib.Power.Holdup'), "not of type 'null'"),
    (lambda rows: first_not_simulated(rows).update(rationale='anything goes'),
     'rationale'),
    (lambda rows: first_not_simulated(rows).update(rationale='n/a'),
     'rationale'),
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


def test_hwrs_id_property_required_not_name_fallback(repo, capsys):
    # The name/identifier still contain a valid ID; they may not mask deletion.
    xml_change(repo, lambda tree: element(tree, 'cap-req-hw_sf_002').attrib.pop('hwrs_id'))
    run(repo, capsys, "lacks a valid 'hwrs_id'")


def test_duplicate_hwrs_property(repo, capsys):
    xml_change(repo, lambda tree: element(tree, 'cap-req-hw_sf_002').set('hwrs_id', 'HW-SF-001'))
    run(repo, capsys, 'Duplicate Capella requirement')


def test_hwrs_parser_fails_closed(repo, capsys):
    (repo / 'docs/hw/HwRS.md').write_text('# format changed\n')
    run(repo, capsys, 'no HW-* requirement rows')


def test_pa_realization_must_target_la(repo, capsys):
    xml_change(repo, lambda tree: element(tree, 'pa-real-mcu').set('targetElement', '#sa-sys-context'))
    run(repo, capsys, 'no LA parent/realization')


def delete_link(tree, uuid='trace-sf_002-retentiondomain'):
    obj = element(tree, uuid)
    obj.getparent().remove(obj)


def test_safety_requirement_object_without_trace_fails(repo, capsys):
    xml_change(repo, delete_link)
    run(repo, capsys, 'HW-SF-002 has no downstream LA safety_mechanism')


@pytest.mark.parametrize('target', ['#la-comp-testinterface', '#sa-sys-context', '#la-root-sys'])
def test_safety_link_to_unmarked_or_non_la_does_not_qualify(repo, capsys, target):
    xml_change(repo, lambda tree: element(tree, 'trace-sf_002-retentiondomain').set('target', target))
    run(repo, capsys, 'HW-SF-002 has no downstream LA safety_mechanism')


@pytest.mark.parametrize('change', [
    lambda tree: element(tree, 'safety-retentiondomain').set('value', 'false'),
    lambda tree: element(tree, 'safety-retentiondomain').set(XSI,
        'org.polarsys.capella.core.data.capellacore:StringPropertyValue'),
    lambda tree: element(tree, 'safety-retentiondomain').set('name', 'unrelated'),
])
def test_safety_flag_must_be_typed_true(repo, capsys, change):
    xml_change(repo, change)
    run(repo, capsys, 'HW-SF-002 has no downstream LA safety_mechanism')


def test_explicit_safety_attribute_supported(repo, capsys):
    """HW-SF-002: the exact true attribute is still a supported Boolean marker."""
    def change(tree):
        element(tree, 'safety-retentiondomain').set('value', 'false')
        element(tree, 'la-comp-retentiondomain').set('safety_mechanism', 'true')
    xml_change(repo, change)
    run(repo, capsys)


def test_indirect_requirement_trace_supported(repo, capsys):
    xml_change(repo, lambda tree: element(tree, 'trace-sf_002-retentiondomain').set('target', '#cap-req-hw_fr_009'))
    run(repo, capsys)


def generic_trace(tree, uuid, source, target):
    child = etree.SubElement(element(tree, 'la-layer'), 'ownedTraces',
                             id=uuid, sourceElement=source, targetElement=target)
    child.set(XSI, 'org.polarsys.capella.core.data.capellacommon:GenericTrace')


def test_trace_cycles_do_not_confer_coverage(repo, capsys):
    def change(tree):
        delete_link(tree)
        delete_link(tree, 'trace-fr_009-retentiondomain')
        generic_trace(tree, 'cycle-a', '#cap-req-hw_sf_002', '#cap-req-hw_fr_009')
        generic_trace(tree, 'cycle-b', '#cap-req-hw_fr_009', '#cap-req-hw_sf_002')
    xml_change(repo, change)
    run(repo, capsys, 'HW-SF-002 has no downstream LA safety_mechanism')


def test_reversed_trace_not_downstream(repo, capsys):
    def change(tree):
        delete_link(tree)
        generic_trace(tree, 'reversed', '#la-comp-retentiondomain', '#cap-req-hw_sf_002')
    xml_change(repo, change)
    run(repo, capsys, 'HW-SF-002 has no downstream LA safety_mechanism')


def test_reqif_outgoing_storage_direction_supported(repo, capsys):
    def change(tree):
        delete_link(tree)
        child = etree.SubElement(element(tree, 'la-comp-retentiondomain'),
             'ownedExtensions', id='outgoing-relation', source='#la-comp-retentiondomain',
             target='#cap-req-hw_sf_002')
        child.set(XSI, 'CapellaRequirements:CapellaOutgoingRelation')
    xml_change(repo, change)
    run(repo, capsys)


@pytest.mark.parametrize('target', ['#missing-component', ''])
def test_broken_trace_fails_even_with_another_valid_path(repo, capsys, target):
    xml_change(repo, lambda tree: element(tree, 'trace-sf_004-powersupervisor').set('target', target))
    run(repo, capsys, 'Broken safety trace')


def test_missing_authority_constraint(repo, capsys):
    def change(tree):
        obj = element(tree, 'oa-constraint-regulatory')
        obj.getparent().remove(obj)
    xml_change(repo, change)
    run(repo, capsys, 'RegulatoryAuthority constraint')


def test_unlinked_authority_constraint(repo, capsys):
    xml_change(repo, lambda tree: element(tree, 'oa-constraint-regulatory').set('constrainedElements', ''))
    run(repo, capsys, 'standards/qualification requirements')


def test_authority_cannot_be_functional_actor(repo, capsys):
    xml_change(repo, lambda tree: element(tree, 'oa-actor-bench').set('name', 'RegulatoryAuthority'))
    run(repo, capsys, 'not a functional Actor/Entity')


@pytest.mark.parametrize('change,message', [
    (lambda tree: element(tree, 'sa-mode-idle').set('name', 'new-state'), 'exactly the modes'),
    (lambda tree: element(tree, 'mode-idle-firmware').set('value', 'IDLE'), 'FSM link/mapping'),
    (lambda tree: element(tree, 'mode-idle-fsm').set('value', 'docs/no-such-fsm.md'), 'FSM link/mapping'),
])
def test_mode_contracts(repo, capsys, change, message):
    xml_change(repo, change)
    run(repo, capsys, message)


def test_firmware_mode_removal_detected(repo, capsys):
    path = repo / gate.FIRMWARE_FSM
    path.write_text(path.read_text().replace('- LISTEN_ONLY\n', ''))
    run(repo, capsys, 'FSM link/mapping')


def test_missing_reader_fails_cli(repo, capsys, monkeypatch):
    monkeypatch.setattr(gate, 'CAPELLAMBSE_AVAILABLE', False)
    run(repo, capsys, 'capellambse python package is not installed')


def test_bridge_la_without_row_fails_closed(repo, capsys):
    """HW-SF-001: a contained but unmapped LA component fails rule 6, not lint."""
    def add_component(tree):
        child = etree.SubElement(element(tree, 'la-root-sys'), 'ownedLogicalComponents',
                                 id='fixture-unmapped-la', name='UnmappedLA')
        child.set(XSI, 'org.polarsys.capella.core.data.la:LogicalComponent')
    xml_change(repo, add_component)
    output = run(repo, capsys, "LA component 'UnmappedLA' must appear exactly once")
    assert 'rule 6:' in output and 'found 0' in output and 'rule 2:' not in output


def test_bridge_row_without_la_fails_closed(repo, capsys):
    """HW-SF-001: a schema-shaped row cannot introduce a nonexistent LA name."""
    bridge_change(repo, lambda rows: rows.append({
        'la_component': 'GhostLA', 'modelica_block': None, 'status': 'not_simulated',
        'rationale': 'not-yet-modeled', 'coverage_note': 'Negative fixture only.'}))
    output = run(repo, capsys, 'GhostLA')
    assert 'rule 6:' in output and 'is not one of' in output


def test_bridge_duplicate_row_fails_closed(repo, capsys):
    """HW-FR-008: duplicate a null-target row, isolating the LA cardinality rule."""
    bridge_change(repo, lambda rows: rows.append(dict(rows[-1])))
    output = run(repo, capsys, "LA component 'TestInterface' must appear exactly once")
    assert 'rule 6:' in output and 'found 2' in output


def test_safety_trace_to_unmarked_la_fails_closed(repo, capsys):
    """HW-SF-002: preserve the real trace, remove the marker; prose cannot help."""
    def remove_marker(tree):
        marker = element(tree, 'safety-retentiondomain')
        marker.getparent().remove(marker)
        element(tree, 'la-comp-retentiondomain').set('description', 'safety_mechanism: true')
        assert element(tree, 'trace-sf_002-retentiondomain').get('target') == '#la-comp-retentiondomain'
    xml_change(repo, remove_marker)
    output = run(repo, capsys, 'HW-SF-002 has no downstream LA safety_mechanism')
    assert 'rule 7:' in output


@pytest.mark.parametrize('attribute', ['stereotype', 'stereotypes'])
@pytest.mark.parametrize('text', [
    'safety_mechanism', '<<safety_mechanism>>', '«safety_mechanism»',
    'unrelated, safety_mechanism', '<<unrelated>>, <<safety_mechanism>>',
    'not a safety_mechanism', 'not, safety_mechanism',
    'comment: safety_mechanism=true', 'safety_mechanism;unrelated',
])
def test_stereotype_strings_never_grant_safety_coverage(repo, capsys, attribute, text):
    """HW-SF-002 / N1: the removed fallback cannot grant coverage in any spelling."""
    def change(tree):
        marker = element(tree, 'safety-retentiondomain')
        marker.getparent().remove(marker)
        element(tree, 'la-comp-retentiondomain').set(attribute, text)
        assert element(tree, 'trace-sf_002-retentiondomain').get('target') == '#la-comp-retentiondomain'
    xml_change(repo, change)
    output = run(repo, capsys, 'HW-SF-002 has no downstream LA safety_mechanism')
    assert 'rule 7:' in output


def test_typed_safety_marker_does_not_depend_on_stereotype_strings(repo, capsys):
    """HW-SF-002 / N1: only the real typed marker grants coverage here."""
    def change(tree):
        component = element(tree, 'la-comp-retentiondomain')
        component.set('stereotype', 'not, safety_mechanism')
        component.set('stereotypes', '<<unsupported>>, arbitrary prose')
        assert element(tree, 'safety-retentiondomain').get('value') == 'true'
    xml_change(repo, change)
    run(repo, capsys)
