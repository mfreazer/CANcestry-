#!/usr/bin/env python3
"""Shared machinery for the H-10 T2 CAN fault scenarios (issue #62).

The two H-10 scenarios - ``scenarios/t2_busoff_001.py`` (Bus-Off recovery)
and ``scenarios/t2_crc_001.py`` (CRC error handling) - are thin clients of
this module. Both execute the REAL v1.0.0 firmware ELF (the off-tree
``firmware/can_fault.c`` driver, ``CANCESTRY_T2_DRIVER=can_fault``) on the
platform described by ``renode/stm32g474-cancestry.repl`` plus the CAN bus
fault injector (``renode/can_fault_injector.py``), brought up by
``renode/cancestry-hw-fault.resc``.

Scenario shape (HwAGENTS.md rule 5: deterministic, hashed trace):

1. preflight: pinned Renode version, fault ELF, platform magic, injector
   magic/state/counters, all trace slots at 0.
2. The machine is advanced exclusively with fixed ``emulation RunFor``
   quanta (100 us master step, the H-07 bridge contract - no FMU/plant is
   involved in these scenarios: the CAN bus medium is the injector).
3. At t = 10 ms the orchestrator injects the fault through the injector's
   command interface (monitor ``ControlWrite``; the register readback must
   confirm the accepted command, fail-closed).
4. Event slots (symbol hooks, the injector, the .resc write hooks) are
   scanned after every quantum and recorded with their exact stamped times.
5. After the scenario window the invariants are asserted; ANY violation
   aborts BEFORE evidence is written (fail-closed).
6. The evidence artifact is written (deterministic bytes, no wall clock).
   ``--emit-pending`` writes the honest PENDING manifest (pass=false, CL0)
   without touching a run - the H-06/H-07 pattern. ``--check`` re-runs and
   requires byte-identical evidence (issue #53 determinism gate pattern).

No FMU is built for these scenarios: the hash chain is ELF + bridge
(monitor client) + injector + trace - exactly the chain the schema
``hw-t2-fault-evidence-0.1.0.schema.json`` declares for the pending and the
passing forms (the documented deviation from the issue's wording is
recorded in the schema description and in the pending manifest).

Requirements traced: HW-FR-003; HwAGENTS.md rules 1, 4 and 5.
Oracle: none (issue #62: no OR-XXX for Bus-Off timing or CRC handling;
both scenarios stay CL0 / sim-pending).
"""

from __future__ import annotations

import argparse
import os
import subprocess
import sys
import threading
import time
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
BRIDGE_DIR = Path(__file__).resolve().parent
SCENARIOS_DIR = BRIDGE_DIR / "scenarios"
BUILD_DIR = REPO_ROOT / "build" / "hw"
SCHEMA_PATH = REPO_ROOT / "schemas" / "hw" / "hw-t2-fault-evidence-0.1.0.schema.json"

sys.path.insert(0, str(BRIDGE_DIR))

from fmi_bridge import FIXED_SEED, RenodeMonitorEndpoint  # noqa: E402
from run_t2_retention import (  # noqa: E402
    T2SetupError,
    _free_port,
    check_renode_version,
    connect_monitor,
    locate_elf,
    locate_renode,
    render_evidence_bytes,
    sha256_file,
)

# ---------------------------------------------------------------------------
# Contract constants - mirrored copies of renode/can_fault_injector.py,
# renode/cancestry-hw-fault.resc, stm32g474-cancestry.repl (H-10 slot contract)
# and firmware/can_fault.c. hw/virtual-bench/test_t2_busoff.py and
# test_t2_crc.py pin every copy against this module so a drift anywhere fails
# the offline test suite instead of corrupting a run.
# ---------------------------------------------------------------------------

# Timing (the H-07 bridge contract; no FMU is involved in these scenarios).
MASTER_STEP_US = 100
INJECT_AT_US = 10_000        # t = 10 ms (issue #62, both scenarios)
SCENARIO_DURATION_US = 50_000  # 50 ms visualization window (issue #62)

# Injector register window (sysbus 0x40000000) and command codes.
INJ_MAGIC = 0x43464931
INJ_BASE = 0x40000000
INJ_OFF_MAGIC = 0x00
INJ_OFF_COMMAND = 0x04
INJ_OFF_BUSOFF_COUNT = 0x08
INJ_OFF_CRC_COUNT = 0x0C
INJ_OFF_BUS_STATE = 0x10
INJ_OFF_ERROR = 0x2C
CMD_INJECT_BUSOFF = 0x42     # 'B'
CMD_INJECT_CRC = 0x43        # 'C'
STATE_BUS_ACTIVE = 0x0
STATE_BUS_OFF = 0x1

# Trace-area slots (stm32g474-cancestry.repl H-10 contract; one writer each).
SLOT_MAGIC_ADDR = 0x60000000
SLOT_MAGIC = 0x54324353
SLOT_BUSOFF_INJECT_US = 0x60000040
SLOT_CRC_INJECT_US = 0x60000044
SLOT_RECOVERY_REQUEST_US = 0x60000048
SLOT_BUS_ACTIVE_US = 0x6000004C
SLOT_CRC_CLEARED_US = 0x60000050
SLOT_CRC_ACK_US = 0x60000054
SLOT_BUSOFF_DETECT_US = 0x60000058
SLOT_BUSOFF_RECOVER_US = 0x6000005C
SLOT_CRC_DETECT_US = 0x60000060
SLOT_CRC_ERROR_COUNT = 0x60000064
SLOT_RUN_COMPLETE = 0x60000068
SLOT_ALIVE_COUNTER = 0x6000006C

# FDCAN1 register surface (RM0440 43.4; st CMSIS stm32g474xx.h offsets).
FDCAN1_BASE = 0x40006400
FDCAN_OFF_PSR = 0x44
FDCAN_PSR_BO = 1 << 7
FDCAN_PSR_LEC_MASK = 0x7

# ISO 11898-1: Bus-Off recovery needs 128 occurrences of 11 consecutive
# recessive bits; HW-FR-003 nominal bitrate is 2 Mbit/s -> exactly 704 us.
BUSOFF_RECOVERY_SEQUENCES = 128
BUSOFF_RECOVERY_BITS = 11
NOMINAL_BITRATE_MBPS = 2
BUSOFF_RECOVERY_US = (  # purpose: the issue's 128 * 11 bit-times invariant
    BUSOFF_RECOVERY_SEQUENCES * BUSOFF_RECOVERY_BITS * 1_000_000
    // (NOMINAL_BITRATE_MBPS * 1_000_000)
)
assert BUSOFF_RECOVERY_US == 704, BUSOFF_RECOVERY_US

REQUIREMENT_ID = "HW-FR-003"
ORACLE_ID = "none"

# Tool pins recorded by the scenario evidence. The fault scenarios use NO
# FMU: OpenModelica/FMPy are not producers here, so they are not pinned and
# their gap is not inherited (the issue's evidence-shape deviation is
# documented in the schema description). can_fault_injector is TCL1 and
# carries no gap (docs/hw/tool-qualification.md section 3); renode stays
# TCL2 and its gap below is copied verbatim into every artifact - the unit
# tests compare this constant with the controlled table so it cannot drift.
TOOL_PINS = {
    "can_fault_injector": "0.1.0",
    "python": ">=3.10",
    "renode": "1.16",
}
RENODE_TOOL_GAP = (
    "Renode peripheral models (IWDG, GPIO, RTC_BKP, DWT) are scripted "
    "emulations, not vendor-validated silicon models: platform-model bugs "
    "can alter firmware-visible timing or register semantics, and this gap "
    "is inherited by every T2 evidence artifact; golden-trace correlation "
    "against vendor reference behavior and T4 bench correlation are required "
    "before any promotion.")
INHERITED_VALIDATION_GAP = "renode: %s" % RENODE_TOOL_GAP

CONSOLE_LOG_NAME = "t2_fault_renode_console.log"
TRANSCRIPT_LOG_NAME = "t2_fault_monitor_transcript.log"


class T2FaultError(Exception):
    """A fail-closed scenario contract violation: no evidence is written."""


class ScenarioSpec(object):
    """Everything one H-10 scenario adds on top of the shared machinery."""

    def __init__(self, *, case_id, fault_command, fault, trace_columns,
                 watch_slots, evidence_relative, scenario_name,
                 evidence_of, pending_reason, not_covered, pinned_sources):
        self.case_id = case_id
        self.fault_command = fault_command
        self.fault = fault                    # "bus_off" or "crc_error"
        self.trace_columns = trace_columns
        self.watch_slots = dict(watch_slots)  # {slot_name: address}
        self.evidence_relative = evidence_relative
        self.scenario_name = scenario_name
        self.evidence_of = evidence_of
        self.pending_reason = pending_reason
        self.not_covered = list(not_covered)
        self.pinned_sources = tuple(pinned_sources)

    @property
    def evidence_path(self):
        return REPO_ROOT / self.evidence_relative


# ---------------------------------------------------------------------------
# Renode launch / preflight / injection
# ---------------------------------------------------------------------------

def launch_renode(renode_bin, elf_path, port, console_log_path):
    """Start Renode headless with the fault bring-up script; return process.

    Same invocation discipline as run_t2_retention.launch_renode (F-20
    console-drain thread, F-32 clean quoted $elf with no '@' marker), but
    against cancestry-hw-fault.resc.
    """
    command = [
        str(renode_bin), "--disable-xwt", "--port", str(port),
        "-e", '$elf="%s"' % elf_path.resolve(),
        "-e", "include @cancestry-hw-fault.resc",
    ]
    process = subprocess.Popen(
        command, cwd=str(BRIDGE_DIR / "renode"),
        stdin=subprocess.PIPE, stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT)
    console_log_path.parent.mkdir(parents=True, exist_ok=True)

    def _drain_console():
        with open(console_log_path, "wb") as log:
            while True:
                chunk = process.stdout.read(65536)
                if not chunk:
                    break
                log.write(chunk)
                log.flush()

    process._t2_console_thread = threading.Thread(
        target=_drain_console, name="renode-console-drain", daemon=True)
    process._t2_console_thread.start()
    return process


def _read_u32(endpoint, addr):
    return endpoint.read_u32(addr)


def preflight(endpoint, spec):
    """Assert the platform + injector are in the only admissible state.

    Every check fails closed: a wrong machine configuration aborts BEFORE
    any stimulus, so a misconfigured platform can never produce evidence.
    """
    def eq(addr, expected, what):
        actual = _read_u32(endpoint, addr)
        if actual != expected:
            raise T2FaultError(
                "preflight: %s at 0x%08x reads 0x%08x, expected 0x%08x "
                "(platform/injector not in the contracted initial state)"
                % (what, addr, actual, expected))

    eq(SLOT_MAGIC_ADDR, SLOT_MAGIC, "t2 trace magic (cancestry-hw-fault.resc)")
    eq(INJ_BASE + INJ_OFF_MAGIC, INJ_MAGIC, "injector window magic")
    eq(INJ_BASE + INJ_OFF_BUS_STATE, STATE_BUS_ACTIVE, "initial bus state")
    eq(INJ_BASE + INJ_OFF_ERROR, 0, "injector error register")
    eq(INJ_BASE + INJ_OFF_BUSOFF_COUNT, 0, "Bus-Off injection counter")
    eq(INJ_BASE + INJ_OFF_CRC_COUNT, 0, "CRC injection counter")
    for name, addr in sorted(spec.watch_slots.items()):
        eq(addr, 0, "trace slot %s" % name)
    # The initial projection must match an idle medium: no Bus-Off, LEC 0.
    psr = _read_u32(endpoint, FDCAN1_BASE + FDCAN_OFF_PSR)
    if psr & (FDCAN_PSR_BO | FDCAN_PSR_LEC_MASK):
        raise T2FaultError(
            "preflight: FDCAN1 PSR reads 0x%08x, expected an error-free "
            "idle medium (BO=0, LEC=0)" % psr)


def inject_command(endpoint, command):
    """Issue one fault command through the injector's command interface.

    Primary path (issue #62 deliverable 1): monitor method call on the
    peripheral - ``sysbus can_fault_injector ControlWrite <ascii> 0x1``,
    wrapped by PythonPeripheral.ControlWrite -> USER request. The injector's
    own error register and the injection trace slot are then read back;
    anything but a clean acceptance aborts the run (fail-closed).
    """
    endpoint.command(
        "sysbus can_fault_injector ControlWrite 0x%02X 0x1" % command,
        echo_fragment="ControlWrite")
    error = _read_u32(endpoint, INJ_BASE + INJ_OFF_ERROR)
    if error != 0:
        raise T2FaultError(
            "injector rejected command 0x%02x with error code %d "
            "(fail-closed: medium state is not trusted for this run)"
            % (command, error))


# ---------------------------------------------------------------------------
# Scenario execution
# ---------------------------------------------------------------------------

def run_fault_scenario(renode_bin, elf_path, spec, build_dir=BUILD_DIR):
    """Execute one H-10 fault scenario; return (passed_body, runlog).

    ``passed_body`` carries the events/invariants/hashes/toolchain blocks of
    the passing evidence document; ``runlog`` carries the diagnostic channel
    (console/transcript tails are attached on failure).
    """
    console_log = build_dir / ("%s_renode_console.log" % spec.case_id)
    transcript_log = build_dir / ("%s_monitor_transcript.log" % spec.case_id)
    port = _free_port()
    process = launch_renode(renode_bin, elf_path, port, console_log)
    endpoint = None
    try:
        endpoint = connect_monitor(port, transcript_path=transcript_log,
                                   clock=time.time)
        preflight(endpoint, spec)
        previous = {name: 0 for name in spec.watch_slots}
        events = {}
        samples = []
        steps = SCENARIO_DURATION_US // MASTER_STEP_US
        for step in range(steps):
            endpoint.run_for_us(MASTER_STEP_US)
            time_us = (step + 1) * MASTER_STEP_US
            if time_us == INJECT_AT_US:
                inject_command(endpoint, spec.fault_command)
            watch = {}
            for name, addr in sorted(spec.watch_slots.items()):
                value = _read_u32(endpoint, addr)
                if value != previous[name]:
                    if previous[name] != 0:
                        raise T2FaultError(
                            "trace slot %s transitioned twice (0->%d->%d): "
                            "the exactly-once capture contract is broken"
                            % (name, previous[name], value))
                    if value == 0:
                        raise T2FaultError(
                            "trace slot %s cleared after being stamped "
                            "(single-writer contract broken)" % name)
                    if value > time_us:
                        raise T2FaultError(
                            "%s timestamp %d us is ahead of emulation time "
                            "%d us" % (name, value, time_us))
                    events[name] = value
                    previous[name] = value
                watch[name] = value
            sample = {
                "time_us": time_us,
                "alive_counter": _read_u32(endpoint, SLOT_ALIVE_COUNTER),
                "bus_state": _read_u32(endpoint, INJ_BASE + INJ_OFF_BUS_STATE),
                "crc_error_count": _read_u32(endpoint, SLOT_CRC_ERROR_COUNT),
                "fdcan1_psr": _read_u32(endpoint, FDCAN1_BASE + FDCAN_OFF_PSR),
            }
            samples.append(sample)

        final_counters = {
            "busoff_injections": _read_u32(
                endpoint, INJ_BASE + INJ_OFF_BUSOFF_COUNT),
            "crc_injections": _read_u32(
                endpoint, INJ_BASE + INJ_OFF_CRC_COUNT),
            "injector_error": _read_u32(endpoint, INJ_BASE + INJ_OFF_ERROR),
        }
        summary = {
            "alive_counter": samples[-1]["alive_counter"],
            "crc_error_count": samples[-1]["crc_error_count"],
            "run_complete": _read_u32(endpoint, SLOT_RUN_COMPLETE),
        }

        failures = check_invariants(spec, events, summary,
                                    final_counters, samples)
        if failures:
            raise T2FaultError("scenario invariants failed: %s"
                               % "; ".join(failures))

        build_dir.mkdir(parents=True, exist_ok=True)
        trace_path = build_dir / ("%s_trace.csv" % spec.case_id)
        trace_path.write_text(
            "%s\n" % spec.trace_columns + "".join(
                trace_row(spec, sample) for sample in samples),
            encoding="utf-8")
        runlog = {
            "bridge_sha256": sha256_file(BRIDGE_DIR / "fmi_bridge.py"),
            "elf_sha256": sha256_file(elf_path),
            "injector_sha256": sha256_file(
                BRIDGE_DIR / "renode" / "can_fault_injector.py"),
            "measured_tools": {
                "renode": subprocess.run(
                    [str(renode_bin), "--version"], stdout=subprocess.PIPE,
                    timeout=60).stdout.decode("utf-8", "replace").strip(),
            },
            "trace_sha256": sha256_file(trace_path),
        }
        (build_dir / ("%s.runlog.json" % spec.case_id)).write_text(
            render_evidence_bytes(dict(runlog)), encoding="utf-8")
        return events, summary, runlog
    finally:
        if endpoint is not None:
            endpoint.close()
        if process.poll() is None:
            process.kill()
        process.wait(timeout=30)
        drain = getattr(process, "_t2_console_thread", None)
        if drain is not None:
            drain.join(timeout=10)


def trace_row(spec, sample):
    """One trace-CSV row, in the column order declared by the scenario."""
    values = {
        "time_us": "%d" % sample["time_us"],
        "alive_counter": "%d" % sample["alive_counter"],
        "bus_state": "0x%02x" % sample["bus_state"],
        "crc_error_count": "%d" % sample["crc_error_count"],
        "fdcan1_psr": "0x%02x" % sample["fdcan1_psr"],
    }
    return ",".join(values[column.strip()]
                    for column in spec.trace_columns.split(",")) + "\n"


# ---------------------------------------------------------------------------
# Invariants (the scenario contracts; any violation fails closed)
# ---------------------------------------------------------------------------

def check_invariants(spec, events, summary, final_counters, samples):
    """Return the list of invariant violations (empty = the run may pass)."""
    if spec.fault == "bus_off":
        return _check_busoff(events, summary, final_counters, samples)
    if spec.fault == "crc_error":
        return _check_crc(events, summary, final_counters, samples)
    return ["unknown fault kind %r" % spec.fault]


def _alive_progressed(samples):
    inject_step = INJECT_AT_US // MASTER_STEP_US - 1
    return samples[-1]["alive_counter"] > samples[inject_step]["alive_counter"]


def _check_busoff(events, summary, final_counters, samples):
    failures = []
    required = ("injection_us", "detection_us", "recovery_request_us",
                "recovery_us", "bus_active_us")
    for name in required:
        if events.get(name, 0) == 0:
            failures.append("missing event: %s" % name)
    if failures:
        return failures
    if events["injection_us"] != INJECT_AT_US:
        failures.append(
            "injection stamped at %d us, expected exactly %d us "
            "(the master-step boundary)" % (events["injection_us"],
                                            INJECT_AT_US))
    if events["detection_us"] < events["injection_us"]:
        failures.append("detection precedes injection (%d < %d)"
                        % (events["detection_us"], events["injection_us"]))
    if events["recovery_request_us"] < events["detection_us"]:
        failures.append("recovery request precedes detection (%d < %d)"
                        % (events["recovery_request_us"],
                           events["detection_us"]))
    if events["recovery_us"] < events["detection_us"]:
        failures.append("recovery action precedes detection (%d < %d)"
                        % (events["recovery_us"], events["detection_us"]))
    # Issue #62 invariant: the firmware's recovery action lands within
    # 128 * 11 bit times of its detection, strictly inside the bound.
    action_delta = events["recovery_us"] - events["detection_us"]
    if not action_delta < BUSOFF_RECOVERY_US:
        failures.append(
            "recovery action took %d us >= %d us (128 * 11 bit times at "
            "%d Mbit/s)" % (action_delta, BUSOFF_RECOVERY_US,
                            NOMINAL_BITRATE_MBPS))
    # The medium must reproduce exactly the ISO 11898-1 recovery sequence.
    bus_delta = events["bus_active_us"] - events["recovery_request_us"]
    if bus_delta != BUSOFF_RECOVERY_US:
        failures.append(
            "the medium released the bus %d us after the recovery request, "
            "expected exactly %d us (128 x 11 recessive bits)"
            % (bus_delta, BUSOFF_RECOVERY_US))
    if events["bus_active_us"] < events["recovery_us"]:
        failures.append("the bus became active before the firmware's "
                        "recovery action (%d < %d)"
                        % (events["bus_active_us"], events["recovery_us"]))
    if summary["run_complete"] != 1:
        failures.append("run_complete is %d, expected 1 (firmware did not "
                        "finish: crash, hang or logic error)"
                        % summary["run_complete"])
    if final_counters["injector_error"] != 0:
        failures.append("injector error register is %d"
                        % final_counters["injector_error"])
    if final_counters["busoff_injections"] != 1:
        failures.append("expected exactly 1 Bus-Off injection, counted %d"
                        % final_counters["busoff_injections"])
    if final_counters["crc_injections"] != 0:
        failures.append("unexpected CRC injections: %d"
                        % final_counters["crc_injections"])
    if not _alive_progressed(samples):
        failures.append("the firmware alive counter did not advance after "
                        "the injection (firmware hung)")
    return failures


def _check_crc(events, summary, final_counters, samples):
    failures = []
    required = ("injection_us", "detection_us", "crc_ack_us",
                "crc_cleared_us")
    for name in required:
        if events.get(name, 0) == 0:
            failures.append("missing event: %s" % name)
    if failures:
        return failures
    if events["injection_us"] != INJECT_AT_US:
        failures.append(
            "injection stamped at %d us, expected exactly %d us"
            % (events["injection_us"], INJECT_AT_US))
    if events["detection_us"] < events["injection_us"]:
        failures.append("detection precedes injection (%d < %d)"
                        % (events["detection_us"], events["injection_us"]))
    if events["crc_ack_us"] < events["detection_us"]:
        failures.append("the firmware acknowledged the IR.ELO interrupt "
                        "before detecting the error (%d < %d)"
                        % (events["crc_ack_us"], events["detection_us"]))
    # The medium releases the CRC condition FROM the acknowledge (single
    # writer): cleared == ack is the injector's deterministic semantics.
    if events["crc_cleared_us"] != events["crc_ack_us"]:
        failures.append("CRC condition cleared at %d us, expected the "
                        "acknowledge time %d us"
                        % (events["crc_cleared_us"], events["crc_ack_us"]))
    if summary["crc_error_count"] < 1:
        failures.append("the firmware CRC error counter is %d, expected "
                        ">= 1 (the counter did not increment)"
                        % summary["crc_error_count"])
    if summary["run_complete"] != 1:
        failures.append("run_complete is %d, expected 1 (firmware crashed, "
                        "hung or missed the handle-complete state)"
                        % summary["run_complete"])
    if final_counters["injector_error"] != 0:
        failures.append("injector error register is %d"
                        % final_counters["injector_error"])
    if final_counters["crc_injections"] != 1:
        failures.append("expected exactly 1 CRC injection, counted %d"
                        % final_counters["crc_injections"])
    if final_counters["busoff_injections"] != 0:
        failures.append("unexpected Bus-Off injections: %d"
                        % final_counters["busoff_injections"])
    if not _alive_progressed(samples):
        failures.append("the firmware alive counter did not advance after "
                        "the injection (firmware hung)")
    return failures


# ---------------------------------------------------------------------------
# Evidence documents (deterministic pending + passing forms)
# ---------------------------------------------------------------------------

def live_source_hashes(spec):
    return {relative: sha256_file(REPO_ROOT / relative)
            for relative in sorted(spec.pinned_sources)}


def expected_pending_manifest(spec):
    """The PENDING manifest of one scenario (pass=false): the honest disposition."""
    return {
        "case_id": spec.case_id,
        "credibility_level": "CL0",
        "evidence_of": spec.evidence_of,
        "firmware": {
            "elf": ("built off-tree from tag v1.0.0 with the pinned T2 "
                    "toolchain image (ci/docker/Dockerfile.t2) and "
                    "CANCESTRY_T2_DRIVER=can_fault "
                    "(hw/virtual-bench/firmware/build_firmware.sh); supplied "
                    "at run time via CANCESTRY_T2_FAULT_ELF or --elf; never "
                    "committed; its sha256 is recorded at run time"),
            "modification": "none; the firmware is read-only (issue #62)",
        },
        "inherited_validation_gap": INHERITED_VALIDATION_GAP,
        "not_covered": list(spec.not_covered),
        "oracle_id": ORACLE_ID,
        "pass": False,
        "pending_reason": spec.pending_reason,
        "provisional": True,
        "requirement_id": REQUIREMENT_ID,
        "scenario": {
            "duration_us": SCENARIO_DURATION_US,
            "fault": spec.fault,
            "inject_at_us": INJECT_AT_US,
            "master_step_us": MASTER_STEP_US,
            "method": "virtual_bench",
            "seed": FIXED_SEED,
        },
        "schema_version": "0.1.0",
        "source_hashes": live_source_hashes(spec),
        "status": "pending",
        "tool_pins": dict(TOOL_PINS),
        "trace": {
            "artifact": ("build/hw/%s_trace.csv (per-run artifact, "
                         "gitignored)" % spec.case_id),
            "columns": spec.trace_columns,
            "hashing": ("sha256 of the trace CSV, measured invariants and "
                        "measured tool versions recorded in build/hw/%s."
                        "runlog.json" % spec.case_id),
        },
    }


def passing_document(spec, events, summary, runlog):
    """Assemble the PASSING evidence document (deterministic bytes)."""
    document = expected_pending_manifest(spec)
    if spec.fault == "bus_off":
        event_block = {
            "bus_active_us": events["bus_active_us"],
            "detection_us": events["detection_us"],
            "injection_us": events["injection_us"],
            "recovery_request_us": events["recovery_request_us"],
            "recovery_us": events["recovery_us"],
        }
        invariants = {
            "bus_recovery_sequence_us": BUSOFF_RECOVERY_US,
            "recovery_action_delta_us": (
                events["recovery_us"] - events["detection_us"]),
            "recovery_action_limit_us": BUSOFF_RECOVERY_US,
            "recovery_action_within_limit": True,
        }
    else:
        event_block = {
            "crc_ack_us": events["crc_ack_us"],
            "crc_cleared_us": events["crc_cleared_us"],
            "detection_us": events["detection_us"],
            "injection_us": events["injection_us"],
        }
        invariants = {
            "crc_error_counter_at_end": summary["crc_error_count"],
            "firmware_crashed": False,
        }
    document.update({
        "credibility_level": "CL0",
        "events": event_block,
        "hashes": {
            "bridge": runlog["bridge_sha256"],
            "elf": runlog["elf_sha256"],
            "injector": runlog["injector_sha256"],
            "trace": runlog["trace_sha256"],
        },
        "invariants": invariants,
        "pass": True,
        "status": "passing",
        "toolchain": {
            "measured": runlog["measured_tools"],
            "pins": dict(TOOL_PINS),
        },
    })
    document.pop("pending_reason", None)
    return document


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def cli_main(spec, argv):
    """Argparse + dispatch shared by both scenario scripts."""
    description = ("T2 scenario %s (%s, issue #62 H-10)."
                   % (spec.case_id, spec.scenario_name))
    parser = argparse.ArgumentParser(description=description)
    parser.add_argument("--check", action="store_true",
                        help="re-run and require byte-identical evidence "
                             "(determinism gate)")
    parser.add_argument("--emit-pending", action="store_true",
                        help="write the pending manifest (no run executed)")
    parser.add_argument("--output", default=str(spec.evidence_path),
                        help="evidence output path")
    parser.add_argument("--elf", default=None,
                        help="fault-scenario firmware ELF (default: "
                             "CANCESTRY_T2_FAULT_ELF/CANCESTRY_T2_ELF)")
    parser.add_argument("--renode", default=None,
                        help="renode binary (default: CANCESTRY_RENODE/$PATH)")
    args = parser.parse_args(argv[1:] if argv is None else argv[1:])

    output = Path(args.output)
    if args.emit_pending:
        fresh = render_evidence_bytes(expected_pending_manifest(spec))
        output.write_text(fresh, encoding="utf-8")
        print("pending manifest written: %s" % output)
        return 0

    try:
        renode_bin = locate_renode(args.renode)
        check_renode_version(renode_bin)
        elf = args.elf or os.environ.get("CANCESTRY_T2_FAULT_ELF") \
            or os.environ.get("CANCESTRY_T2_ELF")
        elf_path = locate_elf(elf)
        events, summary, runlog = run_fault_scenario(renode_bin, elf_path,
                                                     spec)
    except (T2SetupError, T2FaultError) as error:
        print("T2 scenario %s FAILED (fail-closed, no evidence written): %s"
              % (spec.case_id, error))
        return 2

    fresh = render_evidence_bytes(
        passing_document(spec, events, summary, runlog))
    if args.check:
        committed = output.read_text(encoding="utf-8") if output.is_file() \
            else ""
        if committed != fresh:
            print("T2 CHECK FAILED for %s: fresh evidence differs from %s "
                  "(stop and flag, do not overwrite)" % (spec.case_id,
                                                         output))
            return 2
        print("T2 CHECK PASSED for %s: byte-identical evidence reproduced"
              % spec.case_id)
        return 0
    output.write_text(fresh, encoding="utf-8")
    print("T2 scenario evidence written: %s" % output)
    return 0
