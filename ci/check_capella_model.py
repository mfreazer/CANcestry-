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
6. The bridge file ``hw/model/bridge.csv`` must be complete for all LA safety-relevant
   components, and every ``not_simulated`` entry must carry a non-empty rationale.

Exit codes:
    0  all Capella model structural gates and bridge checks passed
    1  at least one structural gate failed
"""

import argparse
import csv
import os
import re
import sys

try:
    import capellambse
    CAPELLAMBSE_AVAILABLE = True
except ImportError:  # pragma: no cover
    CAPELLAMBSE_AVAILABLE = False

HWRs_RELATIVE_PATH = os.path.join("docs", "hw", "HwRS.md")
MODEL_RELATIVE_PATH = os.path.join("hw", "model", "capella", "cancestry.aird")
BRIDGE_RELATIVE_PATH = os.path.join("hw", "model", "bridge.csv")

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


def check_bridge_csv(root, report):
    """Rule 6: Validate hw/model/bridge.csv for LA safety-relevant components."""
    bridge_path = os.path.join(root, BRIDGE_RELATIVE_PATH)
    if not os.path.isfile(bridge_path):
        report.fail(6, f"Bridge file {BRIDGE_RELATIVE_PATH} is missing")
        return

    with open(bridge_path, "r", encoding="utf-8", newline="") as handle:
        rows = list(csv.reader(handle))

    if not rows or rows[0] != ["la_component", "modelica_block", "status", "rationale"]:
        report.fail(6, f"{BRIDGE_RELATIVE_PATH} header invalid, expected ['la_component', 'modelica_block', 'status', 'rationale']")
        return

    required_la = {"PowerSupervisor", "CanPhy1", "CanPhy2", "CanPhy3", "SafetyMonitor", "FailSafeLatch", "RetentionDomain", "TestInterface"}
    seen_la = set()

    for row_idx, row in enumerate(rows[1:], start=2):
        if len(row) != 4:
            report.fail(6, f"{BRIDGE_RELATIVE_PATH} line {row_idx}: expected 4 columns, got {len(row)}")
            continue
        la_comp, modelica_block, status, rationale = [col.strip() for col in row]
        seen_la.add(la_comp)

        if status == "not_simulated" or modelica_block == "not_simulated":
            if not rationale:
                report.fail(6, f"{BRIDGE_RELATIVE_PATH} line {row_idx}: component {la_comp!r} is not_simulated but lacks a rationale")

    missing = required_la - seen_la
    if missing:
        report.fail(6, f"{BRIDGE_RELATIVE_PATH} is missing required LA safety-relevant components: {sorted(missing)}")


def main(argv=None):
    if argv is None:
        argv = sys.argv

    parser = argparse.ArgumentParser(description="Check Capella seed model structural gates and bridge CSV.")
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
        except Exception as err:
            report.fail(1, f"Failed to load Capella model {MODEL_RELATIVE_PATH}: {err}")

    check_bridge_csv(root, report)

    if not report.ok:
        print(f"FAIL: {len(report.failures)} Capella model problem(s)")
        for failure in report.failures:
            print(f"      {failure}")
        return 1

    print("PASS: Capella seed model and bridge CSV pass all structural gates")
    return 0


if __name__ == "__main__":  # pragma: no cover
    sys.exit(main())
