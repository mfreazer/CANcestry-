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

7. Every HW-SF-* requirement reaches an explicitly marked LA safety mechanism.
8. Regulatory authority is a linked OA constraint; SA modes resolve to the
   normative firmware FSM without inventing software states.

Exit codes:
    0  all Capella model structural gates and bridge checks passed
    1  at least one structural gate failed
"""

import argparse
from collections import Counter, defaultdict
import copy
from pathlib import Path
import os
import re
import sys

# Also support direct CLI execution, where sys.path starts at ci/.
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from ci.check_hw_contracts import load_contract, read_json, validate
from ci.check_hw_traceability import parse_hwrs

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
    return parse_hwrs(read_text(hwrs_path))


def get_capella_req_hwrs_id(req_obj):
    """HW-SF-001..005: require the actual property, not an ID in prose."""
    value = req_obj._element.get("hwrs_id", "")
    return value if HW_ID_PATTERN.fullmatch(value) else None


def check_orphan_blocks(model, report):
    """Rule 2: Ensure no orphan blocks in OA, SA, LA, PA.

    In Capella Arcadia, all components in SA, LA, PA must be contained within
    the layer's root system component (SystemContext, Logical System, Physical System)
    or their sub-components. Any component directly under a package or unparented
    is an orphan.
    """
    layers = [
        ("OA", model.oa.all_entities, model.oa),
        ("SA", model.sa.all_components, model.sa),
        ("LA", model.la.all_components, model.la),
        ("PA", model.pa.all_components, model.pa),
    ]
    for layer_name, components, architecture in layers:
        try:
            root = architecture if layer_name == "OA" else architecture.root_component
        except (ValueError, RuntimeError) as error:
            report.fail(2, f"Orphan block / ambiguous root in {layer_name}: {error}")
            continue
        for component in components:
            ancestors = {component.uuid, *(a.get("id") for a in
                                          component._element.iterancestors())}
            if root.uuid not in ancestors:
                report.fail(2, f"Orphan block in {layer_name}: component {component.name!r} (uuid={component.uuid}) is not contained within the root system component hierarchy")


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
            if hwrs_id in found_hwrs_ids:
                report.fail(4, f"Duplicate Capella requirement hwrs_id {hwrs_id!r}")
            found_hwrs_ids.add(hwrs_id)

    # Rule 3: Check all HW-SF-* and HW-FR-* requirements from HwRS.md are linked
    for req_id in hwrs_reqs:
        if req_id.startswith("HW-SF-") or req_id.startswith("HW-FR-"):
            if req_id not in found_hwrs_ids:
                report.fail(3, f"HwRS requirement {req_id} is not linked in Capella model (unlinked safety/functional requirement)")


def check_pa_elements_have_la_parent(model, report):
    """Rule 5: resolve realizations to actual non-root LA components."""
    la_ids = {c.uuid for c in model.la.all_components
              if c.uuid != model.la.root_component.uuid}
    for comp in model.pa.all_components:
        if comp.uuid == model.pa.root_component.uuid:
            continue
        realized = list(comp.realized_components)
        if not realized or any(c.uuid not in la_ids for c in realized):
            report.fail(5, f"PA physical component {comp.name!r} (uuid={comp.uuid}) has no LA parent/realization link to a non-root LA component")


def is_safety_mechanism(component):
    """A typed boolean/property or explicit stereotype, never truthy prose."""
    elem = component._element
    if elem.get("safety_mechanism") == "true":
        return True
    stereotypes = elem.get("stereotype", "") + " " + elem.get("stereotypes", "")
    if "safety_mechanism" in re.findall(r"[A-Za-z_]+", stereotypes):
        return True
    return any(p.name == "safety_mechanism" and
               p.xtype.endswith(":BooleanPropertyValue") and p.value is True
               for p in component.property_values)


def check_safety_linkage(model, report):
    """Rule 7: HW-SF-001..005 must reach a marked LA safety mechanism.

    Traverse directed ReqIF incoming/outgoing/internal relations (capellambse
    normalizes outgoing relation storage direction), plus GenericTrace edges.
    Container membership is NOT a trace link; cycles never confer coverage.
    Broken references fail closed even if another valid path exists.
    """
    adjacency = defaultdict(set)
    for link in model.search("CapellaIncomingRelation", "CapellaOutgoingRelation",
                             "InternalRelation", "GenericTrace"):
        try:
            source, target = link.source, link.target
            if source is None or target is None:
                raise ValueError("missing source or target")
            adjacency[source.uuid].add(target.uuid)
        except (KeyError, ValueError, TypeError) as error:
            report.fail(7, f"Broken safety trace {link.uuid}: {error}")
    safety = {c.uuid for c in model.la.all_components
              if c.uuid != model.la.root_component.uuid and is_safety_mechanism(c)}
    for req in model.search("Requirement"):
        hwrs_id = get_capella_req_hwrs_id(req)
        if not hwrs_id or not re.fullmatch(r"HW-SF-\d+", hwrs_id):
            continue
        pending, seen = [req.uuid], set()
        while pending:
            current = pending.pop()
            if current in seen:
                continue
            seen.add(current)
            pending.extend(sorted(adjacency[current] - seen))
        if not (seen & safety):
            report.fail(7, f"{hwrs_id} has no downstream LA safety_mechanism via trace links")


FIRMWARE_FSM = "docs/system/mode-fault-state-machine.md"
MODE_MAPPING = {"idle": "LISTEN_ONLY", "active": "ACTIVE",
                "diagnosing": "CONFIG", "safe-latch": "SAFE"}


def check_authority_and_modes(root, model, report):
    """Rule 8: HW-SF-001 / HW-FR-003/004/008: constraint and FSM linkage."""
    authorities = [c for c in model.search("Constraint")
                   if c.name == "RegulatoryAuthority" and c.layer.uuid == model.oa.uuid]
    if len(authorities) != 1:
        report.fail(8, "OA must have exactly one RegulatoryAuthority constraint")
    else:
        required = {"HW-FR-003", "HW-FR-004", "HW-SF-005", "HW-NF-004"}
        linked = {get_capella_req_hwrs_id(r) for r in authorities[0].constrained_elements}
        if not required <= linked:
            report.fail(8, "RegulatoryAuthority constraint must link the standards/qualification requirements")
    for entity in model.oa.all_entities:
        if re.search(r"regulat|authority", entity.name, re.I):
            report.fail(8, f"Authority {entity.name!r} must be a Constraint, not a functional Actor/Entity")
    modes = [m for m in model.search("Mode") if m.layer.uuid == model.sa.uuid]
    counts = Counter(m.name for m in modes)
    if counts != Counter(MODE_MAPPING.keys()):
        report.fail(8, "SA must contain exactly the modes idle, active, diagnosing, safe-latch")
    firmware = Path(root, FIRMWARE_FSM).read_text(encoding="utf-8")
    # Only normative §1 bullets are states, not incidental mentions in prose.
    section = firmware.split("## 1. System Modes", 1)[-1].split("## 2.", 1)[0]
    states = set(re.findall(r"^- ([A-Z_]+)$", section, re.M))
    for mode in modes:
        props = {p.name: p.value for p in mode.property_values}
        expected = MODE_MAPPING.get(mode.name)
        if props.get("firmware_fsm") != FIRMWARE_FSM + "#1-system-modes" or not (
                expected in states and props.get("firmware_mode") == expected):
            report.fail(8, f"SA mode {mode.name!r} lacks a valid normative firmware FSM link/mapping")


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
            check_safety_linkage(model, report)
            check_authority_and_modes(root, model, report)
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
