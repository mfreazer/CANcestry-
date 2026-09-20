#!/usr/bin/env python3
"""Check the Capella seed model and structural gates (H-02, issue #35).

Governed by ``HwAGENTS.md`` and ``docs/hw/mbse-plan.md``. Uses capellambse to load
and inspect the Arcadia model under ``hw/model/capella/``.

Rules enforced:
1. Model loads without error via capellambse.
2. No orphan blocks: all non-root components in OA, SA, LA, and PA must be
   properly contained within their layer's root hierarchy.
3. Every HwRS requirement ID (HW-SF-* and HW-FR-*) defined in ``docs/hw/HwRS.md``
   must be linked from a Capella requirement object in the model.
4. Every Capella requirement object in the model must carry a valid, non-empty
   ``hwrs_id`` property matching an HwRS ID.
5. Every physical component in PA (excluding the root physical system) must be
   allocated to/realize at least one Logical Architecture (LA) parent component.
6. Implements HW-SF-001..005 / HW-FR-002,004,008,009: the bridge file ``hw/model/bridge.json`` must map every non-root LA component exactly once and every top-level
   Modelica block exactly once, using schema-controlled exemption rationales.

Exit codes:
    0  all Capella model structural gates and bridge checks passed
    1  at least one structural gate failed
"""

import argparse
from collections import Counter
import copy
from pathlib import Path
import os
import re
import sys

# Also support direct CLI execution, where sys.path starts at ci/.
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from ci.check_hw_contracts import load_contract, read_json, validate

try:
    import capellambse
    CAPELLAMBSE_AVAILABLE = True
except ImportError:  # pragma: no cover
    CAPELLAMBSE_AVAILABLE = False

HWRs_RELATIVE_PATH = os.path.join("docs", "hw", "HwRS.md")
MODEL_RELATIVE_PATH = os.path.join("hw", "model", "capella", "cancestry.aird")
BRIDGE_RELATIVE_PATH = os.path.join("hw", "model", "bridge.json")

HW_ROW = re.compile(r"^\|\s*(HW-(?:SF|FR|NF)-\d{3})\s*\|")
HW_ID_PATTERN = re.compile(r"^HW-(?:SF|FR|NF)-\d{3}$")


class Report:
    """Collect gate failure messages."""

    def __init__(self):
        self.failures = []

    def fail(self, rule_num, message):
        self.failures.append(f"rule {rule_num}: {message}")

    @property
    def ok(self):
        return not self.failures


def read_text(path):
    with open(path, "r", encoding="utf-8", errors="replace") as handle:
        return handle.read()


def get_hwrs_requirements(root):
    """Parse HwRS.md returning dict of requirement_id -> required_cl."""
    hwrs_path = os.path.join(root, HWRs_RELATIVE_PATH)
    reqs = {}
    if not os.path.isfile(hwrs_path):
        return reqs
    for line in read_text(hwrs_path).splitlines():
        match = HW_ROW.match(line)
        if match:
            req_id = match.group(1)
            cells = [cell.strip() for cell in line.strip().strip("|").split("|")]
            reqs[req_id] = cells[4] if len(cells) > 4 else "CL1"
    return reqs


def get_capella_req_hwrs_id(req_obj):
    """Extract hwrs_id attribute/property from a Capella requirement object."""
    elem = getattr(req_obj, "_element", None)
    if elem is not None:
        for attr in ("hwrs_id", "reqId", "identifier", "name"):
            val = elem.get(attr)
            if val and HW_ID_PATTERN.match(val):
                return val
            if val and " " in val:
                first_word = val.split()[0]
                if HW_ID_PATTERN.match(first_word):
                    return first_word

    # Fallback to python attributes
    for attr in ("identifier", "name", "text"):
        val = getattr(req_obj, attr, None)
        if val and isinstance(val, str):
            if HW_ID_PATTERN.match(val):
                return val
            first_word = val.split()[0] if val else ""
            if HW_ID_PATTERN.match(first_word):
                return first_word
    return None


def check_orphan_blocks(model, report):
    """Rule 2: Ensure no orphan blocks in OA, SA, LA, PA.

    In Capella Arcadia, all components in SA, LA, PA must be contained within
    the layer's root system component (SystemContext, Logical System, Physical System)
    or their sub-components. Any component directly under a package or unparented
    is an orphan.
    """
    root_names = {"Operational Entities", "SystemContext", "Logical System", "Physical System"}

    layers = [
        ("OA", getattr(model.oa, "all_entities", [])),
        ("SA", getattr(model.sa, "all_components", [])),
        ("LA", getattr(model.la, "all_components", [])),
        ("PA", getattr(model.pa, "all_components", [])),
    ]

    for layer_name, comps in layers:
        for comp in comps:
            name = getattr(comp, "name", "")
            if name in root_names:
                continue
            parent = getattr(comp, "parent", None)
            parent_name = getattr(parent, "name", "")
            if parent is None or parent_name.endswith("Pkg") or parent_name == "Structure":
                report.fail(2, f"Orphan block in {layer_name}: component {name!r} (uuid={comp.uuid}) is not contained within the root system component hierarchy")


def check_hwrs_linkage(model, hwrs_reqs, report):
    """Rules 3 & 4: HwRS requirement coverage and Capella req hwrs_id presence."""
    capella_reqs = model.search("Requirement")
    found_hwrs_ids = set()

    for req_obj in capella_reqs:
        hwrs_id = get_capella_req_hwrs_id(req_obj)
        if not hwrs_id:
            name = getattr(req_obj, "name", req_obj.uuid)
            report.fail(4, f"Capella requirement object {name!r} (uuid={req_obj.uuid}) lacks a valid 'hwrs_id' property")
        else:
            if hwrs_id not in hwrs_reqs:
                report.fail(4, f"Capella requirement {req_obj.uuid} carries hwrs_id {hwrs_id!r} which is absent from HwRS.md")
            found_hwrs_ids.add(hwrs_id)

    # Rule 3: Check all HW-SF-* and HW-FR-* requirements from HwRS.md are linked
    for req_id in hwrs_reqs:
        if req_id.startswith("HW-SF-") or req_id.startswith("HW-FR-"):
            if req_id not in found_hwrs_ids:
                report.fail(3, f"HwRS requirement {req_id} is not linked in Capella model (unlinked safety/functional requirement)")


def check_pa_elements_have_la_parent(model, report):
    """Rule 5: Every PA element (non-root) must realize an LA component."""
    for comp in model.pa.all_components:
        name = getattr(comp, "name", "")
        if name == "Physical System":
            continue
        realized = getattr(comp, "realized_components", [])
        if not realized:
            # Check component_realizations
            realizations = getattr(comp, "component_realizations", [])
            if not realizations:
                report.fail(5, f"PA physical component {name!r} (uuid={comp.uuid}) has no LA parent/realization link")


def modelica_inventory(root):
    """Resolve file-per-class top-level models/blocks, never comments/strings.

    Implements HW-FR-004, HW-FR-009. Package/within/name drift, nested model
    declarations and empty inventories fail closed rather than guessing.
    """
    library = Path(root) / "hw/model/CancestryLib"
    models = []
    for path in sorted(library.rglob("*.mo")):
        code = re.sub(r'//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\])*"',
                      ' ', path.read_text(encoding="utf-8"), flags=re.S)
        declarations = re.findall(r"\b(?:model|block)\s+([A-Za-z_]\w*)", code)
        if not declarations:
            continue
        header = re.match(
            r"\s*within\s+([A-Za-z_][\w.]*)\s*;\s*"
            r"(?:(?:encapsulated|partial|final)\s+)*(?:model|block)\s+(\w+)\b", code)
        if not header or len(declarations) != 1:
            raise ValueError(f"{path.name}: expected one top-level file-per-class model/block")
        package, name = header.groups()
        expected_package = '.'.join(('CancestryLib', *path.relative_to(library).parts[:-1]))
        if package != expected_package or name != path.stem:
            raise ValueError(f"{path.name}: Modelica within/name does not match its path")
        if not re.search(rf"\bend\s+{re.escape(name)}\s*;\s*$", code):
            raise ValueError(f"{path.name}: missing matching model/block end")
        models.append(package + '.' + name)
    if not models:
        raise ValueError("No top-level Modelica models/blocks found in CancestryLib")
    return models


def check_bridge_json(root, model, report):
    """Rule 6: live LA and Modelica 1:1 bridge, not a hard-coded name list.

    Implements HW-SF-001..005, HW-FR-002, HW-FR-004, HW-FR-008..009.
    Root LA container is excluded; every other component occurs once.
    Null not_simulated targets never count as simulated Modelica coverage.
    """
    root = Path(root)
    try:
        document = load_contract(root, BRIDGE_RELATIVE_PATH, 'bridge')
        la_names = [c.name for c in model.la.all_components
                    if c.uuid != model.la.root_component.uuid]
        if not la_names or any(not name for name in la_names):
            raise ValueError("LA inventory is empty or contains an unnamed component")
        duplicate_names = sorted(n for n, count in Counter(la_names).items() if count != 1)
        if duplicate_names:
            raise ValueError(f"Ambiguous duplicate LA component names: {duplicate_names}")
        blocks = modelica_inventory(root)
        # Bind the schema's reference fields to the real inventories at CI time.
        schema = copy.deepcopy(read_json(root / 'schemas/hw/hw-bridge-0.1.0.schema.json'))
        properties = schema['properties']['mappings']['items']['properties']
        properties['la_component']['enum'] = sorted(la_names)
        properties['modelica_block']['enum'] = [None, *sorted(blocks)]
        validate(document, schema, BRIDGE_RELATIVE_PATH)
        rows = document['mappings']
        la_counts = Counter(r['la_component'] for r in rows)
        block_counts = Counter(r['modelica_block'] for r in rows
                               if r['status'] == 'simulated')
        for name in sorted(la_names):
            if la_counts[name] != 1:
                report.fail(6, f"LA component {name!r} must appear exactly once in bridge.json; found {la_counts[name]}")
        for block in sorted(blocks):
            if block_counts[block] != 1:
                report.fail(6, f"Modelica block {block!r} must be referenced exactly once; found {block_counts[block]}")
    except (OSError, ValueError) as error:
        report.fail(6, f"Bridge validation failed: {error}")


def main(argv=None):
    if argv is None:
        argv = sys.argv

    parser = argparse.ArgumentParser(description="Check Capella seed model structural gates and bridge JSON.")
    parser.add_argument("repo_root", help="Path to repository root")
    args = parser.parse_args(argv[1:])

    root = args.repo_root
    if not os.path.isdir(root):
        print(f"FAIL: {root} is not a directory")
        return 1

    report = Report()

    if not CAPELLAMBSE_AVAILABLE:  # pragma: no cover
        print("FAIL: capellambse python package is not installed")
        return 1

    model_path = os.path.join(root, MODEL_RELATIVE_PATH)
    if not os.path.isfile(model_path):
        report.fail(1, f"Capella model entrypoint {MODEL_RELATIVE_PATH} is missing")
    else:
        try:
            model = capellambse.MelodyModel(model_path)
            hwrs_reqs = get_hwrs_requirements(root)
            check_orphan_blocks(model, report)
            check_hwrs_linkage(model, hwrs_reqs, report)
            check_pa_elements_have_la_parent(model, report)
            check_bridge_json(root, model, report)
        except Exception as err:
            report.fail(1, f"Failed to load Capella model {MODEL_RELATIVE_PATH}: {err}")

    if not report.ok:
        print(f"FAIL: {len(report.failures)} Capella model problem(s)")
        for failure in report.failures:
            print(f"      {failure}")
        return 1

    print("PASS: Capella seed model and bridge JSON pass all structural gates")
    return 0


if __name__ == "__main__":  # pragma: no cover
    sys.exit(main())
