#!/usr/bin/env python3
"""Unit tests for the deterministic T2 FMI bridge (H-07, issue #53).

These tests verify the bridge CONTRACT (marshalling, fixed-step scheduling,
exact-time event capture, ordering assertion, fail-closed behavior) without
Renode or FMPy: the co-simulation endpoints are replaced by deterministic
test doubles. The full-stack execution path (real Renode + real FMU) is
covered by the integration test in ``test_t2_retention.py`` and runs only on
Renode-equipped infrastructure.

Implementations under test: hw/virtual-bench/fmi_bridge.py.
Requirements traced: HW-SF-002, HW-SF-004; HwAGENTS.md rules 4 and 5.
Test ids: HW-T2-BRIDGE-001 .. HW-T2-BRIDGE-020.
"""

from __future__ import annotations

import inspect
import os
import shutil
import socket
import subprocess
import threading
import time
import zipfile
from pathlib import Path

import pytest

SCRIPT_DIR = Path(__file__).resolve().parent
SMOKE_SLAVE_C = SCRIPT_DIR / "fmi2_smoke_slave.c"

from fmi_bridge import (
    BridgeError,
    EVENT_IWDG_FIRE,
    EVENT_RETENTION_WRITE,
    EVENT_SAFE_LATCH,
    FIXED_SEED,
    FMU_STEP_US,
    GPIOA_ODR_ADDR,
    MASTER_STEP_US,
    RCC_CSR_ADDR,
    RCC_CSR_IWDGRSTF,
    RETENTION_SHADOW_ADDR,
    RETENTION_WRITE_US_ADDR,
    RTC_BKP0R_ADDR,
    SAFE_LATCH_US_ADDR,
    SCENARIO_DURATION_US,
    T2_TRACE_MAGIC,
    T2_TRACE_MAGIC_ADDR,
    VBAT_MV_IN_ADDR,
    FmpyFmuSlave,
    RetentionBridge,
    RenodeEndpoint,
    RenodeMonitorEndpoint,
    format_address,
    format_seconds,
    ordering_violation,
    parse_u32,
    safe_latch_asserted,
    vbat_to_millivolts,
)

# ---------------------------------------------------------------------------
# Test doubles (deterministic; never used as evidence)
# ---------------------------------------------------------------------------


class ScriptedFmu(object):
    """Deterministic FMU double: linear discharge, counts doStep calls."""

    def __init__(self, v0_mv=3300, floor_mv=1650):
        self._v0_mv = v0_mv
        self._floor_mv = floor_mv
        self.steps = 0

    def read_real(self, name):
        assert name == "v"
        elapsed_ms = self.steps * FMU_STEP_US / 1000.0
        drop_mv = (self._v0_mv - self._floor_mv) * (elapsed_ms / 150.0)
        return max(self._floor_mv, self._v0_mv - drop_mv) / 1000.0

    def do_step_us(self, current_us, step_us):
        assert step_us == FMU_STEP_US
        self.steps += 1


class ScriptedRenode(RenodeEndpoint):
    """Deterministic Renode double with a scripted per-step timeline.

    ``schedule`` maps a master-step index k to register writes that take
    effect after that step's RunFor (i.e. at emulation time k*100 us, inside
    the quantum ((k-1)*100, k*100]); values are observed exactly as the
    scripted platform hooks would have stamped them (arbitrary microsecond
    precision, not multiples of the master step).
    """

    def __init__(self, schedule=None, odr=0, initial=None):
        self.commands = []
        self.emulation_us = 0
        self.regs = {
            T2_TRACE_MAGIC_ADDR: T2_TRACE_MAGIC,
            RETENTION_WRITE_US_ADDR: 0,
            SAFE_LATCH_US_ADDR: 0,
            GPIOA_ODR_ADDR: odr,
            RETENTION_SHADOW_ADDR: 0,
            RCC_CSR_ADDR: 0,
            0x60000010: 0,  # escalation slot
            0x60000014: 0,  # iwdg fire slot
            VBAT_MV_IN_ADDR: 0,
        }
        if initial:
            self.regs.update(initial)
        self.schedule = dict(schedule or {})
        self.written_vbat = []

    def command(self, text):
        self.commands.append(text)
        return "0x0 (ok)"

    def run_for_us(self, us):
        assert us == MASTER_STEP_US
        self.emulation_us += us
        step = self.emulation_us // MASTER_STEP_US
        for addr, value in self.schedule.get(step, {}).items():
            self.regs[addr] = value

    def read_u32(self, addr):
        return self.regs.get(addr, 0)

    def write_u32(self, addr, value):
        self.regs[addr] = value & 0xFFFFFFFF
        if addr == VBAT_MV_IN_ADDR:
            self.written_vbat.append(value)


# ---------------------------------------------------------------------------
# HW-T2-BRIDGE-001..004: marshalling helpers
# ---------------------------------------------------------------------------

def test_format_seconds_is_integer_deterministic():
    """HW-T2-BRIDGE-001: Renode time literals derive from integer us only."""
    assert format_seconds(MASTER_STEP_US) == "0.000100"
    assert format_seconds(SCENARIO_DURATION_US) == "0.150000"
    assert format_seconds(1_000_000) == "1.000000"
    assert format_seconds(267) == "0.000267"
    with pytest.raises(ValueError):
        format_seconds(-1)


def test_parse_u32_accepts_monitor_output_shapes():
    """HW-T2-BRIDGE-002: the last numeric token parses, fail-closed else."""
    assert parse_u32("0x54324353") == T2_TRACE_MAGIC
    assert parse_u32("0x00000000") == 0
    assert parse_u32("singlePeripheral 0x0000ABCD some note") == 0xABCD
    assert parse_u32("4096") == 4096
    with pytest.raises(BridgeError):
        parse_u32("")
    with pytest.raises(BridgeError):
        parse_u32("no numbers here")
    with pytest.raises(BridgeError):
        parse_u32("0x1FFFFFFFF")


def test_vbat_millivolts_marshalling():
    """HW-T2-BRIDGE-003: plant volts become integer millivolts."""
    assert vbat_to_millivolts(3.3) == 3300
    assert vbat_to_millivolts(1.6549) == 1655
    assert vbat_to_millivolts(0.0) == 0
    for bad in (float("nan"), float("inf")):
        with pytest.raises(BridgeError):
            vbat_to_millivolts(bad)


def test_safe_latch_odr_decoding():
    """HW-T2-BRIDGE-004: the safe-latch pins decode from the ODR sample."""
    assert safe_latch_asserted(0x0000) is True
    assert safe_latch_asserted(0x0300) is False  # PA8 + PA9 driven HIGH
    assert safe_latch_asserted(0x0100) is False  # PA8 only
    assert safe_latch_asserted(0x0200) is False  # PA9 only
    assert safe_latch_asserted(0x0004) is True   # unrelated pin is irrelevant
    assert format_address(0x48000014) == "0x48000014"


# ---------------------------------------------------------------------------
# HW-T2-BRIDGE-005: Renode monitor protocol client
# ---------------------------------------------------------------------------


class FakeMonitorServer(threading.Thread):
    """Minimal scripted Renode monitor: banner, prompt, canned responses."""

    def __init__(self, responses):
        super(FakeMonitorServer, self).__init__(daemon=True)
        self.responses = responses
        self.received = []
        self._server = socket.socket()
        self._server.bind(("127.0.0.1", 0))
        self._server.listen(1)
        self.port = self._server.getsockname()[1]
        self._done = threading.Event()

    def run(self):
        conn, _ = self._server.accept()
        with conn:
            conn.sendall(b"Renodefake monitor\n(monitor) ")
            pending = b""
            while not self._done.is_set():
                chunk = conn.recv(1024)
                if not chunk:
                    break
                pending += chunk
                while b"\n" in pending:
                    line, pending = pending.split(b"\n", 1)
                    text = line.decode().strip()
                    self.received.append(text)
                    response = self.responses.get(text, "0x00000000\n")
                    # F-24: the real monitor echoes the command line to the
                    # terminal before producing its output (see the
                    # dispatch transcripts); the endpoint relies on that
                    # echo to pair a response with the command that issued
                    # it while queued startup input is still draining.
                    conn.sendall((text + "\n" + response + "\n(monitor) ")
                                 .encode())
        self._server.close()

    def stop(self):
        self._done.set()


def test_monitor_endpoint_protocol():
    """HW-T2-BRIDGE-005: RunFor/read/write speak the documented protocol."""
    server = FakeMonitorServer({
        'emulation RunFor "0.000100"': "0x00000000",
        "sysbus ReadDoubleWord 0x60000000": "0x54324353",
        "sysbus WriteDoubleWord 0x60000020 0x00000ce4": "0x00000000",
    })
    server.start()
    try:
        endpoint = RenodeMonitorEndpoint("127.0.0.1", server.port, timeout=5.0)
        endpoint.run_for_us(MASTER_STEP_US)
        assert endpoint.read_u32(T2_TRACE_MAGIC_ADDR) == T2_TRACE_MAGIC
        endpoint.write_u32(VBAT_MV_IN_ADDR, vbat_to_millivolts(3.3))
        endpoint.close()
    finally:
        server.stop()
        server.join(timeout=5.0)
    assert server.received == [
        'emulation RunFor "0.000100"',
        "sysbus ReadDoubleWord 0x60000000",
        "sysbus WriteDoubleWord 0x60000020 0x00000ce4",
        "quit",
    ]


# ---------------------------------------------------------------------------
# HW-T2-BRIDGE-006..009: co-simulation loop, exact-time capture, ordering
# ---------------------------------------------------------------------------

# Schedule keys are the double's step index k (writes land at emulation time
# k*100 us); hook stamps must lie inside that step's quantum. The stamps are
# deliberately NOT multiples of the master step: exact-time capture (VB-Q1).
ESCALATION_SCHEDULE = {
    20: {RETENTION_WRITE_US_ADDR: 1_967, RETENTION_SHADOW_ADDR: 0x45565101,
         RTC_BKP0R_ADDR: 0x45565101},
    21: {SAFE_LATCH_US_ADDR: 2_042},
    40: {0x60000014: 3_967, RCC_CSR_ADDR: RCC_CSR_IWDGRSTF},
}


def _escalated_renode():
    return ScriptedRenode(schedule=ESCALATION_SCHEDULE)


def test_bridge_captures_exact_event_times():
    """HW-T2-BRIDGE-006: events carry hook-stamped times, not step times."""
    renode = _escalated_renode()
    bridge = RetentionBridge(renode, ScriptedFmu(), duration_us=5_000)
    timeline = bridge.run()
    events = {event["event"]: event["time_us"] for event in timeline["events"]}
    assert events == {
        EVENT_RETENTION_WRITE: 1_967,
        EVENT_SAFE_LATCH: 2_042,
        EVENT_IWDG_FIRE: 3_967,
    }
    assert ordering_violation(timeline["events"]) is None
    assert timeline["retention_code"] == 0x45565101
    assert timeline["iwdgrstf"] is True


def test_bridge_fixed_step_schedule():
    """HW-T2-BRIDGE-007: 100 us master step, 1 us FMU sub-steps, exact run."""
    renode = ScriptedRenode()
    fmu = ScriptedFmu()
    bridge = RetentionBridge(renode, fmu)  # full 150 ms default scenario
    timeline = bridge.run()
    steps = SCENARIO_DURATION_US // MASTER_STEP_US
    assert fmu.steps == steps * (MASTER_STEP_US // FMU_STEP_US)
    assert len(timeline["samples"]) == steps
    assert timeline["samples"][0]["time_us"] == MASTER_STEP_US
    assert timeline["samples"][-1]["time_us"] == SCENARIO_DURATION_US
    # Plant -> MCU marshalling ran every master step, monotonically falling.
    vbat = [sample["vbat_mv"] for sample in timeline["samples"]]
    assert vbat[0] == 3299  # one master step (0.1 ms) of scripted discharge
    assert all(a >= b for a, b in zip(vbat, vbat[1:]))
    assert renode.written_vbat[0] == 3299


def test_bridge_deterministic():
    """HW-T2-BRIDGE-008: two runs produce identical timelines."""
    timelines = [RetentionBridge(_escalated_renode(), ScriptedFmu(),
                                 duration_us=5_000).run() for _ in range(2)]
    assert timelines[0] == timelines[1]


def test_ordering_violations():
    """HW-T2-BRIDGE-009: the ordering contract fails closed."""
    good = [{"event": EVENT_RETENTION_WRITE, "time_us": 10},
            {"event": EVENT_SAFE_LATCH, "time_us": 20},
            {"event": EVENT_IWDG_FIRE, "time_us": 30}]
    assert ordering_violation(good) is None
    assert ordering_violation(good[:2]) is not None
    assert ordering_violation(good + good[:1]) is not None  # duplicate
    swapped = [good[0], {"event": EVENT_SAFE_LATCH, "time_us": 5}, good[2]]
    assert ordering_violation(swapped) is not None  # latch before retention
    equal = [{"event": EVENT_RETENTION_WRITE, "time_us": 10},
             {"event": EVENT_SAFE_LATCH, "time_us": 10},
             {"event": EVENT_IWDG_FIRE, "time_us": 30}]
    assert ordering_violation(equal) is not None  # strict inequality


# ---------------------------------------------------------------------------
# HW-T2-BRIDGE-010..012: fail-closed preconditions
# ---------------------------------------------------------------------------

def test_bridge_refuses_uninitialized_platform():
    """HW-T2-BRIDGE-010: no trace magic, no run."""
    renode = ScriptedRenode(initial={T2_TRACE_MAGIC_ADDR: 0xDEADBEEF})
    with pytest.raises(BridgeError, match="magic"):
        RetentionBridge(renode, ScriptedFmu(), duration_us=1_000).run()


def test_bridge_rejects_impossible_timestamps():
    """HW-T2-BRIDGE-011: hooks stamping ahead of / behind emulation fail."""
    ahead = ScriptedRenode(schedule={1: {RETENTION_WRITE_US_ADDR: 5_000_000}})
    with pytest.raises(BridgeError, match="ahead of emulation time"):
        RetentionBridge(ahead, ScriptedFmu(), duration_us=5_000).run()
    backwards = ScriptedRenode(
        schedule={1: {RETENTION_WRITE_US_ADDR: 50},
                  3: {RETENTION_WRITE_US_ADDR: 40}})
    with pytest.raises(BridgeError, match="backwards"):
        RetentionBridge(backwards, ScriptedFmu(), duration_us=5_000).run()


def test_bridge_rejects_misconfigured_steps():
    """HW-T2-BRIDGE-012: the step contract is validated before stepping."""
    renode, fmu = ScriptedRenode(), ScriptedFmu()
    with pytest.raises(BridgeError):
        RetentionBridge(renode, fmu, duration_us=1_234)
    with pytest.raises(BridgeError):
        RetentionBridge(renode, fmu, master_step_us=100, fmu_step_us=7)


# ---------------------------------------------------------------------------
# HW-T2-BRIDGE-013..015: determinism discipline and fail-closed executor
# ---------------------------------------------------------------------------

def test_bridge_module_has_no_nondeterministic_inputs():
    """HW-T2-BRIDGE-013: no wall clock, no randomness (HwAGENTS rule 5)."""
    import fmi_bridge
    source = inspect.getsource(fmi_bridge)
    for forbidden in ("import time", "import random", "import datetime",
                      "time.time", "datetime.now", "random.seed", "os.environ"):
        assert forbidden not in source, forbidden


def test_fmpy_slave_fails_closed_without_executor(monkeypatch):
    """HW-T2-BRIDGE-014: absent FMPy aborts the bridge, never degrades."""
    import builtins
    real_import = builtins.__import__

    def no_fmpy(name, *args, **kwargs):
        if name.startswith("fmpy"):
            raise ImportError("No module named %r" % name)
        return real_import(name, *args, **kwargs)

    monkeypatch.setattr(builtins, "__import__", no_fmpy)
    with pytest.raises(BridgeError, match="fail-closed"):
        FmpyFmuSlave("holdup.fmu")


def test_fixed_seed_is_pinned():
    """HW-T2-BRIDGE-015: the bridge and the .resc seed stay in lockstep."""
    resc = (SCRIPT_DIR / "renode" / "cancestry-hw.resc").read_text()
    assert '$t2_seed?="%s"' % FIXED_SEED in resc


def test_monitor_prompt_regex_accepts_renode_prompt_forms():
    """HW-T2-BRIDGE-016: the prompt matcher accepts the real monitor prompt.

    Renode's telnet monitor prompts are "(monitor)> " and, after
    "mach create <name>", "(machine-<name)> ". The H-07 matcher required the
    line to end right after the closing parenthesis and matched neither form
    (bring-up finding F-3); the bare "(monitor) " form of the test double
    must keep working.
    """
    pattern = RenodeMonitorEndpoint._PROMPT
    for line in (b"(monitor)> ", b"(machine-cancestry)> ", b"(monitor) ",
                 b"(monitor)> \r"):
        assert pattern.search(line), line
    for line in (b"(monitor) >", b"(monitor)foo", b"no prompt here",
                 b"welcome banner (monitor) trailing text"):
        assert not pattern.search(line), line

# ---------------------------------------------------------------------------
# HW-T2-BRIDGE-017..019: the pinned FMPy executor contract (F-19)
#
# Dispatch 7 (run 35723022901) failed with "cannot import name 'extract'
# from 'fmpy.util'": the bridge's FMPy usage was written against names that
# do not exist in the pinned fmpy 0.3.24, and no sandbox test could catch it
# because the sandbox has no FMPy. These tests run where the pinned FMPy is
# importable (the t2 CI image; a local venv with fmpy==0.3.24) and drive the
# exact executor path against a minimal FMU compiled from the pinned C
# fixture (fmi2_smoke_slave.c), so an API regression in the pin is caught
# before a live dispatch.
# ---------------------------------------------------------------------------

def _require_fmpy_gcc():
    """Fail-closed toolchain gate for the FMPy contract tests (F-19)."""
    try:
        import fmpy  # noqa: F401
    except ImportError as error:
        if os.environ.get("CANCESTRY_HW_ALLOW_SKIP") == "1":
            pytest.skip("fmpy (pinned FMI executor) not installed: %s" % error)
        pytest.fail("fmpy (pinned FMI executor) is required for the FMPy "
                    "contract tests (F-19): %s" % error)
    if shutil.which("gcc") is None:
        if os.environ.get("CANCESTRY_HW_ALLOW_SKIP") == "1":
            pytest.skip("gcc not installed (local development)")
        pytest.fail("gcc is required to build the FMPy smoke FMU (F-19)")


def _smoke_model_description(fmi_version, with_co_simulation):
    """Schema-conformant modelDescription.xml for the smoke slave.

    FMI 2.0: <CoSimulation>/<ModelExchange> + ScalarVariable-wrapped Real
    variables + ModelStructure (fmpy 0.3.24 validates against the FMI 2.0
    XSD). FMI 1.0: no interface element, required root state counts.
    """
    if fmi_version == "1.0":
        return (
            '<?xml version="1.0" encoding="utf-8"?>\n'
            '<fmiModelDescription fmiVersion="1.0" '
            'modelName="CancestryT2SmokeHoldup" '
            'modelIdentifier="cancestry_t2_holdup" '
            'guid="{99999999-8888-4777-8666-555555555555}" '
            'numberOfContinuousStates="0" numberOfEventIndicators="0">\n'
            '  <ModelVariables>\n'
            '    <ScalarVariable name="vBat" valueReference="0" '
            'causality="output">\n'
            '      <Real/>\n'
            '    </ScalarVariable>\n'
            '  </ModelVariables>\n'
            '</fmiModelDescription>\n'
        )
    interface = "CoSimulation" if with_co_simulation else "ModelExchange"
    attrs = ' canHandleVariableCommunicationStepSize="true"' \
        if with_co_simulation else ""
    return (
        '<?xml version="1.0" encoding="utf-8"?>\n'
        '<fmiModelDescription fmiVersion="2.0" '
        'modelName="CancestryT2SmokeHoldup" '
        'guid="{c3b1a2d4-5e6f-4a7b-8c9d-0e1f2a3b4c5d}">\n'
        '  <%s modelIdentifier="cancestry_t2_holdup"%s/>\n'
        % (interface, attrs)
        + '  <ModelVariables>\n'
        + '    <ScalarVariable name="vBat" valueReference="0" '
        + 'causality="output">\n'
        + '      <Real/>\n'
        + '    </ScalarVariable>\n'
        + '    <ScalarVariable name="t" valueReference="1" '
        + 'causality="local">\n'
        + '      <Real/>\n'
        + '    </ScalarVariable>\n'
        + '  </ModelVariables>\n'
        + '  <ModelStructure>\n'
        + '    <Outputs><Unknown index="1"/></Outputs>\n'
        + '  </ModelStructure>\n'
        + '</fmiModelDescription>\n'
    )


def _build_smoke_fmu(tmp_path, fmi_version="2.0", with_co_simulation=True):
    """Compile the pinned C fixture into a minimal FMI FMU; return path."""
    import fmpy
    so_name = "cancestry_t2_holdup" + fmpy.sharedLibraryExtension
    bin_dir = tmp_path / "build" / "binaries" / fmpy.platform
    bin_dir.mkdir(parents=True, exist_ok=True)
    so_path = bin_dir / so_name
    result = subprocess.run(
        ["gcc", "-shared", "-fPIC", "-O0", "-Wall", "-Wextra", "-Werror",
         "-o", str(so_path), str(SMOKE_SLAVE_C)],
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    if result.returncode != 0:
        pytest.fail("smoke slave C fixture failed to compile:\n%s"
                    % result.stdout.decode("utf-8", "replace"))
    xml_path = tmp_path / "build" / "modelDescription.xml"
    xml_path.write_text(
        _smoke_model_description(fmi_version, with_co_simulation),
        encoding="utf-8")
    fmu_path = tmp_path / "cancestry_t2_holdup.fmu"
    with zipfile.ZipFile(fmu_path, "w") as archive:
        archive.write(xml_path, "modelDescription.xml")
        archive.write(so_path, "binaries/%s/%s" % (fmpy.platform, so_name))
    return fmu_path


def test_fmpy_slave_contract_with_pinned_executor(tmp_path):
    """HW-T2-BRIDGE-017: FmpyFmuSlave drives pinned fmpy 0.3.24 end-to-end."""
    _require_fmpy_gcc()
    fmu = _build_smoke_fmu(tmp_path)
    slave = FmpyFmuSlave(fmu)
    # vBat = 3.3 V at t = 0 (the smoke slave's holdup shape).
    assert slave.read_real("vBat") == 3.3
    assert slave.read_real("t") == 0.0
    # One 1 ms communication step: 20 mV of discharge (20e-6 V/us).
    slave.do_step_us(0, 1000)
    assert abs(slave.read_real("vBat") - (3.3 - 20e-6 * 1000)) < 1e-9
    assert slave.read_real("t") == 1e-3
    # Unknown variables fail closed (the name map is the contract).
    with pytest.raises(BridgeError):
        slave.read_real("not_a_variable")
    slave.close()


def test_fmpy_slave_rejects_model_exchange_only_fmu(tmp_path):
    """HW-T2-BRIDGE-018: an FMU without a CoSimulation interface fails closed."""
    _require_fmpy_gcc()
    fmu = _build_smoke_fmu(tmp_path, with_co_simulation=False)
    with pytest.raises(BridgeError, match="no CoSimulation interface"):
        FmpyFmuSlave(fmu)


def test_fmpy_slave_rejects_wrong_fmi_version(tmp_path):
    """HW-T2-BRIDGE-019: a non-FMI-2 FMU fails closed (F-2 guard)."""
    _require_fmpy_gcc()
    fmu = _build_smoke_fmu(tmp_path, fmi_version="1.0")
    with pytest.raises(BridgeError, match="FMI 2.0"):
        FmpyFmuSlave(fmu)

def test_monitor_command_timeout_maps_to_bridge_error():
    """HW-T2-BRIDGE-020: a silent monitor fails closed, not as a raw timeout.

    Regression for F-20 (issue #55): dispatch 8 (run 35724334649) died with
    a bare ``TimeoutError`` traceback from ``socket.recv`` when the monitor
    did not answer the first command. The bridge must raise BridgeError
    (fail-closed, with the command named) so the runner can attach the
    Renode console tail; the startup banner read keeps its OSError
    semantics so connect_monitor() can retry.
    """
    server = socket.socket()
    server.bind(("127.0.0.1", 0))
    server.listen(1)
    port = server.getsockname()[1]
    stalled = {"conn": None}

    def _accept_and_stall():
        conn, _ = server.accept()
        stalled["conn"] = conn
        conn.sendall(b"Welcome to Renode (test double)\n(monitor) ")
        time.sleep(10.0)  # then say nothing, ever

    holder = threading.Thread(target=_accept_and_stall, daemon=True)
    holder.start()
    endpoint = None
    try:
        endpoint = RenodeMonitorEndpoint("127.0.0.1", port, timeout=0.3)
        with pytest.raises(BridgeError, match="did not respond"):
            endpoint.command("sysbus ReadDoubleWord 0x60000000")
    finally:
        if endpoint is not None:
            endpoint._socket.close()
        conn = stalled["conn"]
        if conn is not None:
            conn.close()
        server.close()

def test_monitor_transcript_captures_raw_traffic(tmp_path):
    """HW-T2-BRIDGE-021: the raw monitor transcript records both directions.

    F-22 (issue #55): dispatches 8-10 (runs 35724334649 / 35726064092 /
    35727339896) showed the preflight read executing on the Renode side
    (console warning logged) while the client saw no response for 30 s;
    the console alone cannot say what the monitor sent, so the
    endpoint now records every byte, wall-clock stamped.
    """
    server = socket.socket()
    server.bind(("127.0.0.1", 0))
    server.listen(1)
    port = server.getsockname()[1]

    def _serve_once():
        conn, _ = server.accept()
        conn.sendall(b"(monitor)> ")
        conn.recv(1024)
        conn.sendall(b"0x54324353\r\n(monitor)> ")
        conn.close()

    holder = threading.Thread(target=_serve_once, daemon=True)
    holder.start()
    transcript = tmp_path / "monitor_transcript.log"
    endpoint = None
    try:
        endpoint = RenodeMonitorEndpoint("127.0.0.1", port, timeout=5.0,
                                         transcript_path=str(transcript))
        value = parse_u32(endpoint.command(
            "sysbus ReadDoubleWord 0x60000000"))
    finally:
        if endpoint is not None:
            endpoint.close()
        holder.join(timeout=5.0)
        server.close()
    assert value == 0x54324353
    text = transcript.read_text(encoding="utf-8")
    assert "TX b'sysbus ReadDoubleWord 0x60000000\\n'" in text
    assert "RX" in text
    assert "0x54324353" in text


def test_failed_endpoint_init_closes_socket(tmp_path):
    """HW-T2-BRIDGE-022: a failed connect must not leak a live socket.

    F-22 (issue #55): connect_monitor() retries while the banner read
    times out; if each failed attempt leaks its accepted connection,
    multiple clients stack up on Renode's socket server - a prime
    suspect for the no-response preflight of dispatches 8-10.
    """
    server = socket.socket()
    server.bind(("127.0.0.1", 0))
    server.listen(1)
    port = server.getsockname()[1]
    accepted = []

    def _accept_silent():
        conn, _ = server.accept()
        accepted.append(conn)
        # then never send a prompt: the banner read must time out

    holder = threading.Thread(target=_accept_silent, daemon=True)
    holder.start()
    try:
        with pytest.raises(OSError):
            RenodeMonitorEndpoint("127.0.0.1", port, timeout=0.3,
                                  transcript_path=str(tmp_path / "t.log"))
    finally:
        holder.join(timeout=5.0)
        server.close()
    assert accepted, "server never accepted the connection"
    accepted[0].settimeout(2.0)
    assert accepted[0].recv(64) == b"", \
        "the failed endpoint left its socket open (F-22 leak)"
    accepted[0].close()

def test_monitor_accepts_ansi_colored_machine_prompt():
    """HW-T2-BRIDGE-023: the real machine prompt form is recognized (F-23).

    Dispatch 11 (run 35730854679) transcript: after 'mach create' the
    monitor prompt is the machine name in parentheses, trailing space,
    wrapped in ANSI color codes: b'\\x1b[33;1m(cancestry) \\x1b[0m'. The
    F-3 prompt regex only matched '(monitor)>' / '(machine-<name>)>', so
    the preflight response (value + this prompt) arrived at +0.2 s and
    sat unrecognized until the 30 s timeout. The byte sequence below is
    the exact dispatch-11 response.
    """
    server = socket.socket()
    server.bind(("127.0.0.1", 0))
    server.listen(1)
    port = server.getsockname()[1]

    def _serve_once():
        conn, _ = server.accept()
        conn.sendall(b"(monitor)> ")
        conn.recv(1024)
        # value line, then the ANSI-colored machine prompt (dispatch 11)
        conn.sendall(b"0x000000\r\r\n\x1b[33;1m(cancestry) \x1b[0m")
        conn.close()

    holder = threading.Thread(target=_serve_once, daemon=True)
    holder.start()
    endpoint = None
    try:
        endpoint = RenodeMonitorEndpoint("127.0.0.1", port, timeout=3.0)
        tail = endpoint.command("sysbus ReadDoubleWord 0x60000000")
    finally:
        if endpoint is not None:
            endpoint._socket.close()
        holder.join(timeout=5.0)
        server.close()
    assert parse_u32(tail) == 0x000000

def test_command_skips_queued_startup_output_until_its_echo():
    """HW-T2-BRIDGE-024: queued startup input does not masquerade as the
    command response (F-24).

    Dispatch 14 (run 35773043927): the preflight was queued behind the
    injected '-e' startup line; the prompt the endpoint matched first
    belonged to the startup line's output (its echo, the
    LoadPlatformDescription error and the command help), so
    parse_u32 failed with 'no numeric value' while the real response
    sat behind the next prompt. The response is now accepted only when
    it carries the command's own echo.
    """
    server = socket.socket()
    server.bind(("127.0.0.1", 0))
    server.listen(1)
    port = server.getsockname()[1]

    def _serve_once():
        conn, _ = server.accept()
        conn.sendall(b"(monitor)> ")  # prompt 1: consumed by the banner read
        time.sleep(0.2)  # let the client send its command
        # Consume the client's command line: a real monitor always reads
        # its input, and close() with an unread receive buffer sends a RST.
        conn.settimeout(3.0)
        try:
            conn.recv(1024)
        except socket.timeout:
            pass
        # startup -e line still draining: echo + error + prompt 2 (no echo
        # of the client command anywhere in this chunk)
        conn.sendall(b"(monitor) $elf=\"@x\"; include @y.resc\r\n"
                     b"There was an error executing command "
                     b"'machine LoadPlatformDescription y.repl'\r\n"
                     b"The following methods ...\r\n(monitor)> ")
        # now the client command really runs: echo + value + prompt 3
        conn.sendall(b"sysbus ReadDoubleWord 0x60000000\r\n"
                     b"0x54324353\r\n(monitor)> ")
        time.sleep(0.1)
        conn.close()

    holder = threading.Thread(target=_serve_once, daemon=True)
    holder.start()
    endpoint = None
    try:
        endpoint = RenodeMonitorEndpoint("127.0.0.1", port, timeout=5.0)
        tail = endpoint.command("sysbus ReadDoubleWord 0x60000000",
                                echo_fragment="ReadDoubleWord")
    finally:
        if endpoint is not None:
            endpoint._socket.close()
        holder.join(timeout=5.0)
        server.close()
    assert parse_u32(tail) == 0x54324353
