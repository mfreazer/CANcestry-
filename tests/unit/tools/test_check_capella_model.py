"""Tests for ci/check_capella_model.py (issue #35, H-02).

Validates that the Capella structural gate passes on the repository seed model
and correctly rejects negative fixtures (orphan blocks, unlinked safety requirements,
Capella requirements lacking hwrs_id, PA elements without LA realization, broken bridge.csv).
"""

import importlib.util
import os
import pathlib
import sys
import textwrap
import pytest

from conftest import REPO_ROOT

CHECK_CAPELLA = REPO_ROOT / "ci" / "check_capella_model.py"

_spec = importlib.util.spec_from_file_location("ci_check_capella", CHECK_CAPELLA)
ci_check_capella = importlib.util.module_from_spec(_spec)
sys.modules["ci_check_capella"] = ci_check_capella
_spec.loader.exec_module(ci_check_capella)


def test_valid_capella_seed_passes(capsys):
    """The committed Capella seed model and bridge.csv pass all structural gates."""
    code = ci_check_capella.main(["check_capella_model.py", str(REPO_ROOT)])
    out = capsys.readouterr().out
    assert code == 0, out
    assert "PASS: Capella seed model and bridge CSV pass all structural gates" in out


def _create_fixture_repo(tmp_path, capella_xml_content, bridge_csv_content=None):
    """Helper to create a temporary repo layout with HwRS.md and custom Capella model."""
    hw_dir = tmp_path / "hw" / "model" / "capella"
    hw_dir.mkdir(parents=True, exist_ok=True)
    docs_dir = tmp_path / "docs" / "hw"
    docs_dir.mkdir(parents=True, exist_ok=True)

    # Copy HwRS.md from real repo
    hwrs_real = (REPO_ROOT / "docs" / "hw" / "HwRS.md").read_text(encoding="utf-8")
    (docs_dir / "HwRS.md").write_text(hwrs_real, encoding="utf-8")

    # Write bridge.csv
    if bridge_csv_content is None:
        bridge_real = (REPO_ROOT / "hw" / "model" / "bridge.csv").read_text(encoding="utf-8")
        (tmp_path / "hw" / "model" / "bridge.csv").write_text(bridge_real, encoding="utf-8")
    elif bridge_csv_content != "DELETE":
        (tmp_path / "hw" / "model" / "bridge.csv").write_text(bridge_csv_content, encoding="utf-8")

    # Write Capella model files
    afm_xml = textwrap.dedent("""<?xml version="1.0" encoding="UTF-8"?>
    <metadata:Metadata xmi:version="2.0" xmlns:xmi="http://www.omg.org/XMI" xmlns:metadata="http://www.polarsys.org/kitalpha/ad/metadata/1.0.0" id="meta-id">
      <viewpointReferences vpId="org.polarsys.capella.core.viewpoint" version="5.0.0"/>
      <viewpointReferences vpId="org.polarsys.capella.vp.requirements" version="1.0.0"/>
      <viewpointReferences vpId="org.polarsys.kitalpha.vp.requirements" version="1.0.0"/>
    </metadata:Metadata>
    """)
    aird_xml = textwrap.dedent("""<?xml version="1.0" encoding="UTF-8"?>
    <xmi:XMI xmi:version="2.0" xmlns:xmi="http://www.omg.org/XMI" xmlns:viewpoint="http://www.eclipse.org/sirius/1.1.0">
      <viewpoint:DAnalysis xmi:id="_aird_root" version="14.5.0.202102121000">
        <semanticResources>cancestry.capella</semanticResources>
        <semanticResources>cancestry.afm</semanticResources>
      </viewpoint:DAnalysis>
    </xmi:XMI>
    """)

    (hw_dir / "cancestry.afm").write_text(afm_xml, encoding="utf-8")
    (hw_dir / "cancestry.aird").write_text(aird_xml, encoding="utf-8")
    if capella_xml_content != "DELETE":
        (hw_dir / "cancestry.capella").write_text(capella_xml_content, encoding="utf-8")


def test_rejects_orphan_block(tmp_path, capsys):
    """Negative fixture: an unparented orphan block in LA triggers gate failure."""
    valid_capella = (REPO_ROOT / "hw" / "model" / "capella" / "cancestry.capella").read_text(encoding="utf-8")
    orphan_snippet = '<ownedLogicalComponents xsi:type="org.polarsys.capella.core.data.la:LogicalComponent" id="la-orphan-id" name="OrphanComponent"/>'
    bad_capella = valid_capella.replace('</ownedLogicalComponentPkg>', f'{orphan_snippet}\n</ownedLogicalComponentPkg>')

    _create_fixture_repo(tmp_path, bad_capella)
    code = ci_check_capella.main(["check_capella_model.py", str(tmp_path)])
    out = capsys.readouterr().out
    assert code == 1
    assert "Orphan block" in out
    assert "OrphanComponent" in out


def test_rejects_unlinked_safety_requirement(tmp_path, capsys):
    """Negative fixture: removing HW-SF-001 from Capella requirement objects fails the gate."""
    valid_capella = (REPO_ROOT / "hw" / "model" / "capella" / "cancestry.capella").read_text(encoding="utf-8")
    import re
    bad_capella = re.sub(r'<ownedRequirements[^>]*HW-SF-001.*?</ownedRequirements>', '', valid_capella, flags=re.DOTALL)

    _create_fixture_repo(tmp_path, bad_capella)
    code = ci_check_capella.main(["check_capella_model.py", str(tmp_path)])
    out = capsys.readouterr().out
    assert code == 1
    assert "unlinked safety/functional requirement" in out
    assert "HW-SF-001" in out


def test_rejects_capella_requirement_lacking_hwrs_id(tmp_path, capsys):
    """Negative fixture: a Capella requirement object without hwrs_id fails the gate."""
    valid_capella = (REPO_ROOT / "hw" / "model" / "capella" / "cancestry.capella").read_text(encoding="utf-8")
    bad_req = '<ownedRequirements xsi:type="requirements:Requirement" id="bad-req-01" name="Unknown Requirement" text="No hwrs_id"/>'
    bad_capella = valid_capella.replace('</ownedExtensions>', f'{bad_req}\n</ownedExtensions>')

    _create_fixture_repo(tmp_path, bad_capella)
    code = ci_check_capella.main(["check_capella_model.py", str(tmp_path)])
    out = capsys.readouterr().out
    assert code == 1
    assert "lacks a valid 'hwrs_id'" in out


def test_rejects_pa_element_without_la_parent(tmp_path, capsys):
    """Negative fixture: a PA component without an LA realization link fails the gate."""
    valid_capella = (REPO_ROOT / "hw" / "model" / "capella" / "cancestry.capella").read_text(encoding="utf-8")
    unlinked_pa = '<ownedPhysicalComponents xsi:type="org.polarsys.capella.core.data.pa:PhysicalComponent" id="pa-unlinked-id" name="Unlinked_PA_Hardware"/>'
    bad_capella = valid_capella.replace('</ownedPhysicalComponentPkg>', f'{unlinked_pa}\n</ownedPhysicalComponentPkg>')

    _create_fixture_repo(tmp_path, bad_capella)
    code = ci_check_capella.main(["check_capella_model.py", str(tmp_path)])
    out = capsys.readouterr().out
    assert code == 1
    assert "has no LA parent/realization link" in out


def test_rejects_bridge_csv_missing_rationale(tmp_path, capsys):
    """Negative fixture: bridge.csv with not_simulated but empty rationale fails the gate."""
    valid_capella = (REPO_ROOT / "hw" / "model" / "capella" / "cancestry.capella").read_text(encoding="utf-8")
    bad_bridge = textwrap.dedent("""la_component,modelica_block,status,rationale
    RetentionDomain,CancestryLib.Power.Holdup,simulated,Simulated by Holdup
    PowerSupervisor,not_simulated,not_simulated,
    SafetyMonitor,not_simulated,not_simulated,
    """)

    _create_fixture_repo(tmp_path, valid_capella, bridge_csv_content=bad_bridge)
    code = ci_check_capella.main(["check_capella_model.py", str(tmp_path)])
    out = capsys.readouterr().out
    assert code == 1
    assert "is not_simulated but lacks a rationale" in out


def test_rejects_missing_bridge_file(tmp_path, capsys):
    """Negative fixture: missing bridge.csv triggers failure."""
    valid_capella = (REPO_ROOT / "hw" / "model" / "capella" / "cancestry.capella").read_text(encoding="utf-8")
    _create_fixture_repo(tmp_path, valid_capella, bridge_csv_content="DELETE")
    code = ci_check_capella.main(["check_capella_model.py", str(tmp_path)])
    out = capsys.readouterr().out
    assert code == 1
    assert "Bridge file" in out and "is missing" in out


def test_rejects_invalid_bridge_csv_header(tmp_path, capsys):
    """Negative fixture: bridge.csv with bad header or bad columns."""
    valid_capella = (REPO_ROOT / "hw" / "model" / "capella" / "cancestry.capella").read_text(encoding="utf-8")
    bad_bridge = "col1,col2\nval1,val2\n"
    _create_fixture_repo(tmp_path, valid_capella, bridge_csv_content=bad_bridge)
    code = ci_check_capella.main(["check_capella_model.py", str(tmp_path)])
    out = capsys.readouterr().out
    assert code == 1
    assert "Bridge file" in out or "header invalid" in out


def test_rejects_bridge_csv_missing_required_la(tmp_path, capsys):
    """Negative fixture: bridge.csv missing a required LA component."""
    valid_capella = (REPO_ROOT / "hw" / "model" / "capella" / "cancestry.capella").read_text(encoding="utf-8")
    bad_bridge = textwrap.dedent("""la_component,modelica_block,status,rationale
    RetentionDomain,CancestryLib.Power.Holdup,simulated,Simulated by Holdup
    """)
    _create_fixture_repo(tmp_path, valid_capella, bridge_csv_content=bad_bridge)
    code = ci_check_capella.main(["check_capella_model.py", str(tmp_path)])
    out = capsys.readouterr().out
    assert code == 1
    assert "missing required LA safety-relevant components" in out


def test_rejects_missing_model_file(tmp_path, capsys):
    """Negative fixture: missing capella model entrypoint triggers failure."""
    valid_capella = (REPO_ROOT / "hw" / "model" / "capella" / "cancestry.capella").read_text(encoding="utf-8")
    _create_fixture_repo(tmp_path, "DELETE")
    os.remove(tmp_path / "hw" / "model" / "capella" / "cancestry.aird")
    code = ci_check_capella.main(["check_capella_model.py", str(tmp_path)])
    out = capsys.readouterr().out
    assert code == 1
    assert "Capella model entrypoint" in out and "is missing" in out


def test_rejects_corrupt_model_file(tmp_path, capsys):
    """Negative fixture: malformed XML in capella file triggers failure."""
    _create_fixture_repo(tmp_path, "<xml>broken")
    code = ci_check_capella.main(["check_capella_model.py", str(tmp_path)])
    out = capsys.readouterr().out
    assert code == 1
    assert "Failed to load Capella model" in out


def test_main_handles_invalid_directory(capsys):
    """Main returns exit code 1 when repository root directory is invalid."""
    code = ci_check_capella.main(["check_capella_model.py", "/nonexistent/directory/path"])
    out = capsys.readouterr().out
    assert code == 1
    assert "is not a directory" in out


def test_get_capella_req_hwrs_id_fallback_helpers():
    """Unit test for get_capella_req_hwrs_id helper fallback paths."""
    class DummyReq:
        def __init__(self, identifier=None, name=None, text=None):
            self.identifier = identifier
            self.name = name
            self.text = text

    assert ci_check_capella.get_capella_req_hwrs_id(DummyReq(identifier="HW-SF-001")) == "HW-SF-001"
    assert ci_check_capella.get_capella_req_hwrs_id(DummyReq(name="HW-FR-004 Transient Protection")) == "HW-FR-004"
    assert ci_check_capella.get_capella_req_hwrs_id(DummyReq(name="Unknown Req")) is None
