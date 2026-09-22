#!/usr/bin/env python3
"""T2 orchestration: HW-SF-002 retention verification on the virtual bench.

Implements the H-07 (issue #53) T2 pipeline over the platform model
(``renode/``) and the deterministic FMI 2.0 bridge (``fmi_bridge.py``):

    1. preflight: locate the pinned toolchain (omc, renode, firmware ELF)
    2. build the Holdup FMU (FMI 2.0 CoSimulation; bring-up finding F-2 -
       the pinned OpenModelica 1.24.0 cannot export FMI 3.0) headless via omc
    3. launch Renode headless with cancestry-hw.resc and the v1.0.0 ELF
    4. run the FMI bridge for 150 ms (100 us master / 1 us FMU steps)
    5. capture the QA-EV-01 events (retention write, safe latch, IWDG fire),
       assert the strict ordering, and anchor the plant trajectory to the
       OR-001 closed form within the sim-case tolerance
    6. write the evidence artifact (deterministic bytes, no wall clock)

Fail-closed rules (HwAGENTS.md rule 4, honest ledger):
  * a missing tool, ELF or any contract violation aborts BEFORE any evidence
    is written; a failed run never overwrites the committed artifact;
  * the only evidence that can exist without a real T2 run is the PENDING
    manifest (``--emit-pending``, ``pass=false``), exactly the H-06 pulse
    manifest pattern;
  * ``--check`` re-runs the full pipeline and requires the fresh evidence to
    be byte-identical to the committed artifact (determinism gate,
    issue #53 constraint: "if it doesn't [reproduce], stop and flag").

Requirements traced: HW-SF-002, HW-SF-004; HwAGENTS.md rules 1, 4, 5.
Oracle: OR-001 (hw/tests/oracles/registry.json, registered).

Usage:
    python3 hw/virtual-bench/run_t2_retention.py                # full run
    python3 hw/virtual-bench/run_t2_retention.py --check        # determinism gate
    python3 hw/virtual-bench/run_t2_retention.py --emit-pending # pending manifest

Environment:
    CANCESTRY_T2_ELF   path of the v1.0.0 firmware ELF (mandatory for a run;
                       the ELF is built off-tree from tag v1.0.0 with the
                       pinned toolchain image and is never committed)
    CANCESTRY_RENODE   path of the renode binary (default: $PATH)
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shutil
import socket
import subprocess
import sys
import threading
import time
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
BRIDGE_DIR = Path(__file__).resolve().parent
sys.path.insert(0, str(BRIDGE_DIR))

from fmi_bridge import (  # noqa: E402
    FIXED_SEED,
    FMU_STEP_US,
    MASTER_STEP_US,
    SCENARIO_DURATION_US,
    BridgeError,
    RetentionBridge,
    RenodeMonitorEndpoint,
    ordering_violation,
)

EVIDENCE_PATH = REPO_ROOT / "hw" / "tests" / "evidence" / "t2_retention_001.json"
SIM_CASE_PATH = REPO_ROOT / "hw" / "tests" / "cases" / "holdup_001.simcase.json"
MODEL_ROOT = REPO_ROOT / "hw" / "model" / "CancestryLib"
BUILD_DIR = REPO_ROOT / "build" / "hw"
# F-20 (issue #55): Renode's console transcript (per-run, gitignored).
# Renode's monitor protocol rides the TCP port; its console log goes to
# stdout, which used to sit in a PIPE nobody read - a full 64 KB pipe would
# block Renode's own shell thread on write(2) (the monitor then goes silent)
# and every include-script diagnostic (errors, IronPython exceptions, crash
# dumps) was lost, which is exactly the undiagnosable monitor timeout of
# dispatch 8 (run 35724334649). The drain thread below removes the
# backpressure and preserves the transcript; the runner attaches its tail
# to every failure.
RENODE_CONSOLE_LOG = BUILD_DIR / "t2_retention_001_renode_console.log"
SCHEMA_PATH = REPO_ROOT / "schemas" / "hw" / "hw-t2-evidence-0.1.0.schema.json"

MODEL = "CancestryLib.Power.Holdup"
CASE_ID = "t2_retention_001"
REQUIREMENT_ID = "HW-SF-002"
ORACLE_ID = "OR-001"

# Declared tool pins (evidence records these; measured versions go to the
# gitignored runlog, mirroring the H-06 pulse manifests).
TOOL_PINS = {
    "fmpy": "0.3.24",
    "openmodelica": "1.24",
    "python": ">=3.10",
    "renode": "1.16",
}
# Runner-side Renode policy pin: the platform's scripted peripherals and the
# T2 event-capture hooks (``cpu AddSymbolHook``) require Renode >= 1.16
# (bring-up finding F-1: AddSymbolHook does not exist in 1.15.x). 1.16.1 is
# the pinned release; the runner accepts any 1.16.x.
RENODE_VERSION_PIN = (1, 16)

# The retention-domain load scenario: OR-001 parameters of the committed
# holdup_001 sim case (single source of truth for the plant physics).
V_FLOOR_MV = 1650  # retention floor, HW-FR-009 (1.65 V)
V0_MV = 3300

TOLERANCE_VBAT_MV = 1  # holdup_001 sim-case tolerance (0.001 V), in mV

# Sources pinned by the evidence artifact (repo-relative). The set is fixed
# so regeneration stays deterministic; hashes are computed live.
PINNED_SOURCES = (
    "ci/docker/Dockerfile",
    "ci/docker/Dockerfile.t2",
    "ci/docker/base-image.digest",
    "ci/docker/renode-1.16.1.pin",
    "docs/hw/tool-qualification.md",
    "docs/hw/virtual-bench-plan.md",
    "hw/model/CancestryLib/Power/Holdup.mo",
    "hw/model/CancestryLib/Power/package.mo",
    "hw/model/CancestryLib/package.mo",
    "hw/tests/cases/holdup_001.simcase.json",
    "hw/tests/oracles/or_001_holdup.py",
    "hw/tests/oracles/registry.csv",
    "hw/tests/oracles/registry.json",
    "hw/virtual-bench/firmware/build_firmware.sh",
    "hw/virtual-bench/firmware/main.c",
    "hw/virtual-bench/firmware/startup.s",
    "hw/virtual-bench/firmware/t2_target_compat.h",
    "hw/virtual-bench/fmi2_smoke_slave.c",
    "hw/virtual-bench/fmi_bridge.py",
    "hw/virtual-bench/fmi_bridge_test.py",
    "hw/virtual-bench/renode/cancestry-hw.resc",
    "hw/virtual-bench/renode/gpioa_model.py",
    "hw/virtual-bench/renode/iwdg_model.py",
    "hw/virtual-bench/renode/rcc_model.py",
    "hw/virtual-bench/renode/rtc_backup_model.py",
    "hw/virtual-bench/renode/stm32g474-cancestry.repl",
    "hw/virtual-bench/run_t2_retention.py",
    "hw/virtual-bench/test_t2_retention.py",
    "schemas/hw/hw-oracle-registry-0.1.0.schema.json",
    "schemas/hw/hw-sim-0.1.0.schema.json",
    "schemas/hw/hw-t2-evidence-0.1.0.schema.json",
)

TRACE_COLUMNS = "time_us,vbat_mv,gpio_odr"


class T2SetupError(Exception):
    """The T2 toolchain is incomplete: fail closed, produce nothing."""


# ---------------------------------------------------------------------------
# Deterministic evidence rendering
# ---------------------------------------------------------------------------

def sha256_file(path):
    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        for chunk in iter(lambda: handle.read(65536), b""):
            digest.update(chunk)
    return "sha256:%s" % digest.hexdigest()


def render_evidence_bytes(document):
    """Canonical evidence bytes: sorted keys, 2-space indent, LF, no drama."""
    return json.dumps(document, sort_keys=True, indent=2) + "\n"


def _live_source_hashes():
    return {relative: sha256_file(REPO_ROOT / relative)
            for relative in sorted(PINNED_SOURCES)}


def expected_pending_manifest():
    """The PENDING T2 manifest (pass=false): the honest no-run disposition.

    Mirrors the H-06 pulse-5a precedent: infrastructure lands first, the
    qualification manifest records the deferral, and no passing claim is
    made. The pending_reason names exactly what the first executing run
    needs. Rendered with live source hashes.
    """
    document = {
        "case_id": CASE_ID,
        "credibility_level": "CL0",
        "evidence_of": (
            "T2 virtual-bench foundation for HW-SF-002 (H-07, issue #53): "
            "Renode platform model (hw/virtual-bench/renode/), deterministic "
            "FMI 2.0 co-simulation bridge (hw/virtual-bench/fmi_bridge.py) "
            "and orchestration (this file). No T2 run has been executed: "
            "this manifest records the pending disposition of the retention "
            "sequence (retention write < safe latch < IWDG fire) pending the "
            "first Renode-equipped run of the v1.0.0 firmware ELF."),
        "firmware": {
            "elf": ("built off-tree from tag v1.0.0 with the pinned "
                    "toolchain image (virtual-bench-plan section 2); supplied "
                    "at run time via CANCESTRY_T2_ELF; never committed; its "
                    "sha256 is recorded in the evidence at run time"),
            "modification": "none; the firmware is read-only (issue #53)",
        },
        "inherited_validation_gap": (
            "openmodelica: Compiler semantics outside independently validated "
            "output remain unqualified; OR-001/OR-002 regressions do not "
            "cover all translation and solver behavior."),
        "not_covered": [
            "No executed T2 run exists: no event timestamps, no trace hash "
            "and no ELF/FMU artifact hashes are claimed by this manifest.",
            "QA-EV-01 escalation stimulus injection (the queue-saturation "
            "fault recipe reaching the firmware through CAN traffic) is not "
            "part of this foundation; capture is passive and the runner "
            "fails closed if the sequence does not occur within 150 ms.",
            "BOR/power-supervisor reset on brownout is not_simulated in the "
            "platform model (HwAGENTS.md rule 6: safety-relevant, Human "
            "Reviewer-gated); the brownout energy margin itself is closed at "
            "T1 by OR-001 (holdup_001).",
            "DWT CYCCNT accuracy is not exercised by the ordering claim "
            "(events are captured from emulation virtual time); it carries "
            "the renode validation gap.",
            "CAN protocol simulation, thermal simulation and multi-channel "
            "support are out of H-07 scope (issue #53 constraints).",
            "T4 physical correlation: bench-pending per HwRS section 5; CL3 "
            "requires bench data (no qualification promotion).",
        ],
        "oracle_id": ORACLE_ID,
        "pass": False,
        "pending_reason": (
            "The T2 toolchain (pinned Renode 1.16 + the off-tree v1.0.0 "
            "firmware ELF) is now provisioned by the hw-nightly "
            "t2-virtual-bench job (issue #55, H-08), but no executed T2 run "
            "backs this artifact yet, so the retention sequence (retention "
            "write < safe latch < IWDG fire) remains sim-pending. Foundation "
            "delivered by https://github.com/mfreazer/CANcestry-/"
            "issues/53; the first executing run is produced by "
            "hw/virtual-bench/run_t2_retention.py on the T2 toolchain image "
            "and must reproduce this ledger chain deterministically before "
            "any passing claim."),
        "provisional": True,
        "requirement_id": REQUIREMENT_ID,
        "schema_version": "0.1.0",
        "scenario": {
            "duration_us": SCENARIO_DURATION_US,
            "fmu_step_us": FMU_STEP_US,
            "master_step_us": MASTER_STEP_US,
            "method": "virtual_bench",
            "seed": FIXED_SEED,
        },
        "sim_case": "hw/tests/cases/holdup_001.simcase.json",
        "source_hashes": _live_source_hashes(),
        "status": "pending",
        "tool_pins": dict(TOOL_PINS),
        "trace": {
            "artifact": "build/hw/t2_retention_001_trace.csv (per-run "
                        "artifact, gitignored)",
            "columns": TRACE_COLUMNS,
            "hashing": "sha256 of the trace CSV, measured invariants and "
                       "measured tool versions recorded in "
                       "build/hw/t2_retention_001.runlog.json",
        },
    }
    return document


# ---------------------------------------------------------------------------
# Toolchain preflight (fail-closed)
# ---------------------------------------------------------------------------

def locate_elf(explicit=None):
    candidate = explicit or os.environ.get("CANCESTRY_T2_ELF")
    if not candidate:
        raise T2SetupError(
            "no firmware ELF: set CANCESTRY_T2_ELF to the v1.0.0 ELF built "
            "off-tree (T2 executes the real firmware or nothing)")
    path = Path(candidate)
    if not path.is_file():
        raise T2SetupError("firmware ELF not found: %s" % path)
    return path


def locate_renode(explicit=None):
    candidate = explicit or os.environ.get("CANCESTRY_RENODE") \
        or shutil.which("renode")
    if not candidate:
        raise T2SetupError(
            "renode not found: set CANCESTRY_RENODE or install the pinned "
            "Renode on PATH (heavy simulation runs on self-hosted "
            "infrastructure, HW-PLAN C5)")
    return Path(candidate)


def check_renode_version(renode_bin):
    result = subprocess.run(
        [str(renode_bin), "--version"], stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT, timeout=60)
    text = result.stdout.decode("utf-8", "replace")
    # `renode --version` prints, e.g.:
    #   renode v1.0.0.0
    #     build: 1.16.1
    #     build type: Release
    #     runtime: .NET 8.x
    # The release version is the "build:" line (the assembly version is a
    # constant 1.0.0.0 and carries no release information). The legacy
    # "Renode X.Y" form is kept as a fallback. (Bring-up finding F-11:
    # the H-07 regex matched neither form of the real 1.16 output.)
    match = re.search(r"build:\s*(\d+)\.(\d+)\.(\d+)", text)
    if not match:
        match = re.search(r"[Rr]enode\s+v?(\d+)\.(\d+)", text)
        if match:
            version = (int(match.group(1)), int(match.group(2)))
        else:
            raise T2SetupError("cannot parse `renode --version` output: %r"
                               % text.strip()[:200])
    else:
        version = (int(match.group(1)), int(match.group(2)))
    if version != RENODE_VERSION_PIN:
        raise T2SetupError(
            "renode %d.%d does not match the runner policy pin %d.%d; the "
            "platform model and its scripted peripherals are qualified "
            "against the pin only (fail-closed)"
            % (version + RENODE_VERSION_PIN))


def locate_omc():
    omc = shutil.which("omc")
    if omc is None:
        raise T2SetupError(
            "omc (OpenModelica) not found on PATH; run inside the pinned "
            "toolchain image (ci/docker/)")
    return omc


# ---------------------------------------------------------------------------
# Pipeline stages
# ---------------------------------------------------------------------------

def omc_build_script_text():
    """Text of the omc script that builds the Holdup FMU (pure, pinned).

    F-18 (issue #55): the buildModelFMU line must format the model name
    into the script. An early draft left a bare ``%s`` in the generated
    .mos file and omc's lexer rejected it at build time ("Lexer failed to
    recognize '%s, version'", dispatch 6, run 35722295912). The text is a
    pure function of the pinned model layout so the unit test
    HW-T2-ORCH-007 can pin it without the toolchain.
    """
    return (
        "loadModel(Modelica);\n"
        "getErrorString();\n"
        'loadFile("%s");\n' % (MODEL_ROOT / "package.mo") +
        "getErrorString();\n"
        'loadFile("%s");\n' % (MODEL_ROOT / "Power" / "package.mo") +
        "getErrorString();\n"
        'loadFile("%s");\n' % (MODEL_ROOT / "Power" / "Holdup.mo") +
        "getErrorString();\n"
        'buildModelFMU(%s, version="2.0", fmuType="cs", '
        'fileNamePrefix="cancestry_t2_holdup");\n' % (MODEL,) +
        "getErrorString();\n"
    )


def build_fmu(omc, build_dir=BUILD_DIR):
    """Build the Holdup FMU (FMI 2.0, CoSimulation) headless; return path.

    FMI 2.0, not the plan's FMI 3.0 wording: bring-up finding F-2 - the
    pinned OpenModelica 1.24.0 cannot export FMI 3.0 (its FMI.mo
    checkFMIVersion accepts only 1.0/2.0), while the FMI 2.0 CS FMU is the
    exact configuration the T1 holdup_001 evidence was built with on this
    same toolchain (docs/hw/t2-bringup-report.md).
    """
    build_dir.mkdir(parents=True, exist_ok=True)
    script = build_dir / "omc_build_t2.mos"
    script.write_text(omc_build_script_text(), encoding="utf-8")
    result = subprocess.run(
        [omc, "--showErrorMessages", str(script)],
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        cwd=str(build_dir), timeout=600)
    match = re.findall(r'([^\s"\'()]+\.fmu)', result.stdout.decode("utf-8",
                                                                  "replace"))
    if result.returncode == 0 and match:
        fmu = Path(match[-1])
        if not fmu.is_absolute():
            fmu = build_dir / fmu
        if fmu.is_file():
            return fmu
    raise T2SetupError("omc buildModelFMU failed (last output):\n%s"
                       % result.stdout.decode("utf-8", "replace")[-4000:])


def _free_port():
    sock = socket.socket()
    sock.bind(("127.0.0.1", 0))
    port = sock.getsockname()[1]
    sock.close()
    return port


def launch_renode(renode_bin, elf_path, port,
                  console_log_path=RENODE_CONSOLE_LOG):
    """Start Renode headless with the platform script; return the process."""
    command = [
        str(renode_bin), "--disable-xwt", "--port", str(port),
        "-e", '$elf="@%s"' % elf_path.resolve(),
        "-e", "include @cancestry-hw.resc",
    ]
    process = subprocess.Popen(
        command, cwd=str(BRIDGE_DIR / "renode"),
        stdin=subprocess.PIPE, stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT)
    if console_log_path is not None:
        console_log_path.parent.mkdir(parents=True, exist_ok=True)

        def _drain_console():
            with open(console_log_path, "wb") as log:
                while True:
                    chunk = process.stdout.read(65536)
                    if not chunk:
                        break
                    log.write(chunk)
                    log.flush()

        # F-20: keep draining until the pipe closes (process exit).
        process._t2_console_thread = threading.Thread(
            target=_drain_console, name="renode-console-drain",
            daemon=True)
        process._t2_console_thread.start()
    return process


def _console_tail(path, lines=40):
    """Last ``lines`` of the Renode console transcript ('' when absent)."""
    try:
        text = Path(path).read_text(encoding="utf-8", errors="replace")
    except OSError:
        return ""
    tail = text.splitlines()[-lines:]
    return "\n".join(tail)


def connect_monitor(port, deadline_s=60.0):
    """Connect to the Renode monitor, retrying until the deadline."""
    deadline = time.monotonic() + deadline_s
    last = None
    while time.monotonic() < deadline:
        try:
            return RenodeMonitorEndpoint("127.0.0.1", port, timeout=30.0)
        except OSError as error:
            last = error
            time.sleep(0.2)
    raise T2SetupError("Renode monitor did not come up on port %d: %s"
                       % (port, last))


def or001_anchor(samples):
    """Anchor the plant trajectory to OR-001 within the sim-case tolerance.

    Uses the closed form from the registered oracle (hw/tests/oracles/
    or_001_holdup.py) with the committed holdup_001 parameters. Returns the
    worst absolute delta in integer millivolts.
    """
    import importlib.util

    spec = importlib.util.spec_from_file_location(
        "or_001_holdup", REPO_ROOT / "hw" / "tests" / "oracles"
        / "or_001_holdup.py")
    oracle = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(oracle)
    import json

    sim_case = json.loads(SIM_CASE_PATH.read_text(encoding="utf-8"))
    params = sim_case["parameters"]
    worst_mv = 0
    for sample in samples:
        t_s = sample["time_us"] / 1e6
        expected = oracle.vbat(t_s, params["V0"], params["I_mcu"],
                               params["I_leak"], params["C"],
                               esr=params["ESR"])
        delta_mv = abs(int(round(expected * 1000.0)) - sample["vbat_mv"])
        worst_mv = max(worst_mv, delta_mv)
    return worst_mv


def run_scenario(renode_bin, elf_path, fmu_path, build_dir=BUILD_DIR):
    """Execute the full T2 retention scenario; return the evidence body."""
    from fmi_bridge import FmpyFmuSlave

    port = _free_port()
    process = launch_renode(renode_bin, elf_path, port)
    endpoint = None
    try:
        endpoint = connect_monitor(port)
        fmu = FmpyFmuSlave(fmu_path)
        bridge = RetentionBridge(endpoint, fmu)
        timeline = bridge.run()
        violation = ordering_violation(timeline["events"])
        events = {event["event"]: event["time_us"]
                  for event in timeline["events"]}
        samples = timeline["samples"]
        worst_mv = or001_anchor(samples)

        # Persist the trace CSV and the run log (gitignored build artifacts).
        build_dir.mkdir(parents=True, exist_ok=True)
        trace_path = build_dir / "t2_retention_001_trace.csv"
        trace_path.write_text(
            "%s\n" % TRACE_COLUMNS +
            "".join("%d,%d,0x%04x\n" % (s["time_us"], s["vbat_mv"],
                                        s["gpio_odr"]) for s in samples),
            encoding="utf-8")
        runlog = {
            "bridge_sha256": sha256_file(BRIDGE_DIR / "fmi_bridge.py"),
            "elf_sha256": sha256_file(elf_path),
            "fmu_sha256": sha256_file(fmu_path),
            "measured_tools": {
                "omc": subprocess.run([locate_omc(), "--version"],
                                      stdout=subprocess.PIPE,
                                      timeout=60).stdout.decode(
                                          "utf-8", "replace").strip(),
                "renode": subprocess.run([str(renode_bin), "--version"],
                                         stdout=subprocess.PIPE,
                                         timeout=60).stdout.decode(
                                             "utf-8", "replace").strip(),
            },
            "ordering_violation": violation,
            "trace_sha256": sha256_file(trace_path),
        }
        (build_dir / "t2_retention_001.runlog.json").write_text(
            render_evidence_bytes(runlog), encoding="utf-8")

        if violation is not None:
            raise BridgeError("QA-EV-01 ordering contract failed: %s" % violation)
        if not timeline["iwdgrstf"]:
            raise BridgeError("IWDG reset cause flag not observed")
        if worst_mv > TOLERANCE_VBAT_MV:
            raise BridgeError(
                "plant trajectory deviates from OR-001 by %d mV (tolerance "
                "%d mV)" % (worst_mv, TOLERANCE_VBAT_MV))

        passed = {
            "events": {
                "iwdg_fire_us": events["iwdg_fire"],
                "retention_write_us": events["retention_write"],
                "safe_latch_us": events["safe_latch"],
            },
            "ordering": {
                "contract": "retention_write < safe_latch < iwdg_fire",
                "holds": True,
            },
            "plant": {
                "or001_max_abs_delta_mv": worst_mv,
                "tolerance_mv": TOLERANCE_VBAT_MV,
                "vbat_end_mv": samples[-1]["vbat_mv"],
                "vbat_start_mv": samples[0]["vbat_mv"],
            },
            "retention": {
                "code": timeline["retention_code"],
                "preserved_across_iwdg_reset": True,
            },
        }
        return passed, runlog
    finally:
        if endpoint is not None:
            endpoint.close()
        if process.poll() is None:
            process.kill()
        process.wait(timeout=30)
        # F-20: let the console drain finish so the post-mortem tail below
        # is complete (the pipe is at EOF once the process is reaped).
        drain = getattr(process, "_t2_console_thread", None)
        if drain is not None:
            drain.join(timeout=10)


def passing_document(run_body):
    """Assemble the PASSING evidence document (deterministic bytes)."""
    document = expected_pending_manifest()
    document.update({
        "credibility_level": "CL2",
        "evidence_of": (
            "T2 virtual-bench execution of the v1.0.0 firmware ELF against "
            "the CancestryLib.Power.Holdup plant: the QA-EV-01 escalation "
            "sequence (retention write, safe-state latch, IWDG fire) was "
            "captured with exact emulation timestamps and satisfies the "
            "strict ordering; the plant trajectory matches oracle OR-001 "
            "within the holdup_001 sim-case tolerance; the retained fault "
            "code survived the scripted IWDG reset. Provisional pending T4 "
            "bench correlation (HW-SF-002 requires CL3)."),
        "events": run_body["events"],
        "hashes": {
            "bridge": run_body["bridge_sha256"],
            "elf": run_body["elf_sha256"],
            "fmu": run_body["fmu_sha256"],
            "trace": run_body["trace_sha256"],
        },
        "ordering": run_body["ordering"],
        "plant": run_body["plant"],
        "retention": run_body["retention"],
        "not_covered": [
            "T4 physical correlation: bench-pending per HwRS section 5; "
            "CL3 requires bench data (no qualification promotion).",
            "BOR/power-supervisor reset on brownout is not_simulated in the "
            "platform model (HwAGENTS.md rule 6, Human-Reviewer-gated).",
            "CAN protocol simulation, thermal simulation and multi-channel "
            "support are out of H-07 scope (issue #53 constraints).",
        ],
        "pass": True,
        "status": "passing",
        "toolchain": {
            "measured": run_body["measured_tools"],
            "pins": dict(TOOL_PINS),
        },
    })
    # The pending_reason is meaningless on a passing artifact; the schema
    # forbids it there (it would read as an unresolved deferral).
    document.pop("pending_reason", None)
    return document


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------

def emit_pending(output=EVIDENCE_PATH):
    output = Path(output)
    output.write_text(render_evidence_bytes(expected_pending_manifest()),
                      encoding="utf-8")
    print("pending T2 manifest written: %s" % output)
    return 0


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--check", action="store_true",
                        help="re-run and require byte-identical evidence "
                             "(determinism gate)")
    parser.add_argument("--emit-pending", action="store_true",
                        help="write the pending manifest (no run executed)")
    parser.add_argument("--output", default=str(EVIDENCE_PATH),
                        help="evidence output path")
    parser.add_argument("--elf", default=None,
                        help="firmware ELF (default: CANCESTRY_T2_ELF)")
    parser.add_argument("--renode", default=None,
                        help="renode binary (default: CANCESTRY_RENODE/$PATH)")
    parser.add_argument("--fmu", default=None,
                        help="prebuilt Holdup FMU (default: build it via omc; "
                             "reuse a single FMU artifact across the "
                             "determinism pair so the evidence's fmu hash "
                             "compares identical bytes - same contract as "
                             "the ELF)")
    args = parser.parse_args(argv[1:] if argv is None else argv[1:])

    if args.emit_pending:
        return emit_pending(args.output)

    try:
        omc = locate_omc()
        renode_bin = locate_renode(args.renode)
        check_renode_version(renode_bin)
        elf_path = locate_elf(args.elf)
        if args.fmu:
            fmu_path = Path(args.fmu)
            if not fmu_path.is_file():
                raise T2SetupError("prebuilt FMU not found: %s" % fmu_path)
        else:
            fmu_path = build_fmu(omc)
        run_body, _ = run_scenario(renode_bin, elf_path, fmu_path)
    except (T2SetupError, BridgeError) as error:
        print("T2 FAILED (fail-closed, no evidence written): %s" % error)
        # F-20: attach the captured Renode console transcript so a
        # silent-monitor failure is diagnosable from the CI log.
        tail = _console_tail(RENODE_CONSOLE_LOG)
        if tail:
            print("== renode console tail (last %d lines) =="
                  % len(tail.splitlines()))
            print(tail)
        return 2

    document = passing_document(run_body)
    output = Path(args.output)
    fresh = render_evidence_bytes(document)
    if args.check:
        committed = output.read_text(encoding="utf-8") if output.is_file() \
            else ""
        if committed != fresh:
            print("T2 CHECK FAILED: fresh evidence differs from %s "
                  "(nondeterministic run or drifted committed artifact - "
                  "stop and flag, do not overwrite)" % output)
            return 2
        print("T2 CHECK PASSED: byte-identical evidence reproduced")
        return 0
    output.write_text(fresh, encoding="utf-8")
    print("T2 evidence written: %s" % output)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
