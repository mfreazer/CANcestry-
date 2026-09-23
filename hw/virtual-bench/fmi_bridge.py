#!/usr/bin/env python3
"""Deterministic FMI 2.0 co-simulation bridge for the CANcestry T2 virtual bench.

Implements the H-07 (issue #53) bridge contract over the platform model in
``hw/virtual-bench/renode/``:

FMI version (H-08 bring-up finding F-2, docs/hw/t2-bringup-report.md): the
plant is an FMI 2.0 CoSimulation FMU. The plan's "FMI 3.0" wording is a
planning detail, not a requirement; the pinned OpenModelica 1.24.0 cannot
export FMI 3.0 (its FMI.mo checkFMIVersion accepts only 1.0/2.0), while the
same toolchain is proven to produce the FMI 2.0 CS FMU the T1 holdup_001
evidence was built with. FMPy's bounded doStep/readout role is unchanged.

    OpenModelica plant (Holdup FMU, OR-001)  <->  Renode (v1.0.0 firmware ELF)

* Plant outputs (VBAT node voltage) are marshalled to the MCU side through
  the platform's supervisor input register; firmware outputs (GPIO output
  data register, i.e. the safe-latch pins) are observed back into the trace.
* Fixed-step synchronization: 100 us master step, 1 us FMU internal step
  (virtual-bench-plan section 6). All scheduling is integer microseconds.
* Discrete events (retention write, safe latch, IWDG fire) are captured with
  their ACTUAL emulation occurrence times: the platform's cpu symbol hooks
  stamp exact virtual-time microseconds into the T2 trace area, so the
  100 us master step never quantizes an event timestamp (QA ruling VB-Q1,
  virtual-bench-plan section 6).
* Determinism (HwAGENTS.md rule 5): no wall clock, no randomness, no
  floating-point scheduling. The only floats are FMU output values, which
  are recorded as integer millivolts.

Requirements traced: HW-SF-002, HW-SF-004 (QA-EV-01 escalation ordering),
HW-FR-001; HwAGENTS.md rules 1, 4 and 5. Oracle: OR-001 (registered).
"""

from __future__ import annotations

import re
import socket

# ---------------------------------------------------------------------------
# Fixed timing contract (virtual-bench-plan section 6; issue #53 deliverable 2)
# ---------------------------------------------------------------------------
MASTER_STEP_US = 100
FMU_STEP_US = 1
SCENARIO_DURATION_US = 150_000  # 150 ms: brownout (50 ms) + removal (100 ms)

# Fixed Renode seed; must agree with cancestry-hw.resc ($t2_seed default).
FIXED_SEED = "123456789"

# ---------------------------------------------------------------------------
# T2 trace-area contract (hw/virtual-bench/renode/stm32g474-cancestry.repl)
# ---------------------------------------------------------------------------
T2_TRACE_MAGIC_ADDR = 0x60000000
T2_TRACE_MAGIC = 0x54324353  # "T2CS"
T2_TRACE_FLAGS_ADDR = 0x60000004
RETENTION_WRITE_US_ADDR = 0x60000008
SAFE_LATCH_US_ADDR = 0x6000000C
ESCALATION_US_ADDR = 0x60000010
IWDG_FIRE_US_ADDR = 0x60000014
VBAT_MV_IN_ADDR = 0x60000020
RETENTION_SHADOW_ADDR = 0x60000100

# Firmware-visible registers (watchdog.c / hal_stm32.c register surface).
RTC_BKP0R_ADDR = 0x40002850
RCC_CSR_ADDR = 0x40021094
RCC_CSR_IWDGRSTF = 1 << 29
GPIOA_ODR_ADDR = 0x48000014

# Safe-latch pins driven LOW by cancestry_hardware_set_safe_state (watchdog.c):
# PA8 (inverter enable / torque) and PA9 (main contactor).
SAFE_LATCH_PINS = (8, 9)

EVENT_RETENTION_WRITE = "retention_write"
EVENT_SAFE_LATCH = "safe_latch"
EVENT_IWDG_FIRE = "iwdg_fire"

# The QA-EV-01 / HW-SF-004 ordering contract.
EXPECTED_ORDER = (EVENT_RETENTION_WRITE, EVENT_SAFE_LATCH, EVENT_IWDG_FIRE)


class BridgeError(Exception):
    """A fail-closed bridge contract violation: the run aborts, nothing passes."""


# ---------------------------------------------------------------------------
# Pure marshalling helpers (unit-tested in fmi_bridge_test.py)
# ---------------------------------------------------------------------------

def format_seconds(us):
    """Format an integer microsecond duration as a Renode time literal.

    Integer-only, so the command bytes never depend on float formatting.
    """
    us = int(us)
    if us < 0:
        raise ValueError("negative duration: %d us" % us)
    return "%d.%06d" % divmod(us, 1_000_000)


def format_address(addr):
    """Format an address for the Renode monitor."""
    return "0x%08x" % int(addr)


_HEX_TOKEN = re.compile(r"^0x[0-9a-fA-F]+$|^[0-9]+$")


def parse_u32(text):
    """Parse the last numeric token of a monitor response as a u32.

    The monitor prints values as hexadecimal (``0x...``) or decimal; the last
    token is the value. Any other shape fails closed.
    """
    tokens = str(text).split()
    for token in reversed(tokens):
        if _HEX_TOKEN.match(token):
            value = int(token, 0)
            if not 0 <= value <= 0xFFFFFFFF:
                raise BridgeError("monitor value out of u32 range: %r" % token)
            return value
    raise BridgeError("no numeric value in monitor response: %r" % text)


def vbat_to_millivolts(vbat_v):
    """Marshalling, plant -> MCU: VBAT voltage [V] to integer millivolts."""
    vbat_v = float(vbat_v)
    if vbat_v != vbat_v or vbat_v in (float("inf"), float("-inf")):
        raise BridgeError("non-finite plant voltage: %r" % vbat_v)
    return int(round(vbat_v * 1000.0))


def safe_latch_asserted(odr):
    """True when both safe-latch pins are driven LOW in the ODR sample."""
    odr = int(odr) & 0xFFFF
    return all((odr >> pin) & 1 == 0 for pin in SAFE_LATCH_PINS)


# ---------------------------------------------------------------------------
# Endpoints
# ---------------------------------------------------------------------------

class RenodeEndpoint(object):
    """Abstract Renode side of the bridge (test doubles subclass this)."""

    def command(self, text):
        raise NotImplementedError

    def run_for_us(self, us):
        raise NotImplementedError

    def read_u32(self, addr):
        raise NotImplementedError

    def write_u32(self, addr, value):
        raise NotImplementedError


class RenodeMonitorEndpoint(RenodeEndpoint):
    """Renode monitor (telnet) client for the paused-machine RunFor protocol.

    The machine is driven exclusively with fixed-size ``emulation RunFor``
    quanta; it is never free-run. This is what makes the co-simulation
    deterministic (HwAGENTS.md rule 5).
    """

    # F-23 (issue #55): the real monitor prompt after a machine is
    # selected is the machine name in parentheses with a trailing space,
    # ANSI-colored - e.g. b"\x1b[33;1m(cancestry) \x1b[0m" (dispatch 11,
    # run 35730854679 transcript); before any machine exists it is the
    # plain monitor prompt. The F-3 form only matched "(monitor)>" /
    # "(machine-<name>)>" and therefore never recognized the
    # machine-prompt form: every response after machine selection sat in
    # the buffer unrecognized until the 30 s timeout (dispatches 8-11 -
    # the transcript shows the preflight response arriving at +0.2 s,
    # then 30 s of silence, then our own 'quit'). Two changes: ANSI
    # escape sequences are stripped from the buffer (partial trailing
    # sequences are left for the next chunk), and the prompt matcher
    # accepts any "(name)" form.
    _ANSI_ESCAPE = re.compile(rb"\x1b\[[0-9;]*[A-Za-z]")
    _PROMPT = re.compile(rb"\([A-Za-z0-9_.:-]+\)>?[ \t]*\r?$")

    def __init__(self, host, port, timeout=30.0, transcript_path=None,
                 clock=None):
        self._timeout = float(timeout)
        self._last_command = None
        self._buffer = b""
        # F-22 (issue #55): raw monitor transcript - every byte in both
        # directions. Diagnostics only (never evidence): dispatches 8-10
        # (runs 35724334649 / 35726064092 / 35727339896) show the preflight
        # read executing (console warning logged) yet no response ever
        # reaching this client, and the console alone cannot say which side
        # dropped it. The bridge stays wall-clock-free (HW-T2-BRIDGE-013):
        # lines are numbered by a deterministic sequence; the caller may
        # inject a ``clock`` callable (the runner passes its wall clock)
        # to stamp the diagnostic file.
        self._transcript = None
        self._transcript_clock = clock
        self._transcript_line = 0
        if transcript_path is not None:
            self._transcript = open(transcript_path, "wb")
        try:
            self._socket = socket.create_connection((host, int(port)),
                                                    timeout=self._timeout)
            self._socket.settimeout(self._timeout)
            self._read_prompt()  # banner + initial prompt
        except Exception:
            # F-22: a failed connect must not leak a live socket - the
            # runner's retry loop (connect_monitor) would otherwise stack
            # multiple clients on Renode's socket server.
            try:
                self._socket.close()
            except (AttributeError, OSError):
                pass
            if self._transcript is not None:
                self._transcript.close()
                self._transcript = None
            raise

    def _record(self, direction, data):
        """Append one line of the raw monitor transcript (F-22)."""
        if self._transcript is None:
            return
        stamp = "line %d" % self._transcript_line
        if self._transcript_clock is not None:
            stamp = "line %d [%.3f]" % (self._transcript_line,
                                        self._transcript_clock())
        line = "%s %s %r\n" % (stamp, direction, data)
        self._transcript.write(line.encode("utf-8", "replace"))
        self._transcript.flush()
        self._transcript_line += 1

    # -- low-level line protocol ------------------------------------------
    def _read_prompt(self):
        while True:
            # F-23: drop ANSI escapes so the colored prompt
            # ("\x1b[33;1m(cancestry) \x1b[0m") matches _PROMPT. A partial
            # escape at the buffer tail (no terminating letter) is left in
            # place and completes on the next chunk.
            self._buffer = self._ANSI_ESCAPE.sub(b"", self._buffer)
            match = self._PROMPT.search(self._buffer)
            if match:
                tail = self._buffer[:match.start()]
                self._buffer = self._buffer[match.end():]
                return tail
            try:
                chunk = self._socket.recv(4096)
            except socket.timeout as error:
                if self._last_command is None:
                    # Startup banner read: re-raise as OSError so
                    # connect_monitor() can retry the connection.
                    raise
                # F-20 (issue #55): a silent monitor must fail closed with
                # context, not as a bare TimeoutError traceback. Dispatch 8
                # (run 35724334649) hung here on the first command; the
                # runner now attaches the captured Renode console tail.
                raise BridgeError(
                    "Renode monitor did not respond within %.0f s to %r; "
                    "the startup script may still be executing or the "
                    "monitor thread may be blocked (check the renode "
                    "console tail)" % (self._timeout, self._last_command)) \
                    from error
            if not chunk:
                # F-33 (issue #60, dispatch 21): an EOF must say WHERE it
                # happened - banner handshake vs a named in-flight command -
                # the way the timeout path already does (F-20). Dispatch 21
                # (run 35893814425) died with a bare "closed the connection"
                # right after the first complete include, leaving the EOF
                # point unknown; without it the annotation cannot classify
                # the fault (connect vs preflight vs the first RunFor).
                if self._last_command is None:
                    raise BridgeError(
                        "Renode monitor closed the connection during the "
                        "startup banner read")
                raise BridgeError(
                    "Renode monitor closed the connection while awaiting "
                    "response to %r" % (self._last_command,))
            self._record("RX", chunk)
            self._buffer += chunk

    def command(self, text, echo_fragment=None):
        self._last_command = text
        payload = (text.rstrip("\n") + "\n").encode("utf-8")
        try:
            self._socket.sendall(payload)
        except OSError as error:
            raise BridgeError("Renode monitor socket send failed for %r: %s"
                              % (text, error)) from error
        self._record("TX", payload)
        decoded = self._read_prompt().decode("utf-8", "replace")
        if echo_fragment is None:
            return decoded
        # F-24 (issue #55): the startup include is injected as queued
        # shell input (-e). While it is still draining, the prompt we
        # match belongs to ITS output (the -e echo, command errors and
        # help text - dispatch 14, run 35773043927: the preflight
        # 'response' was exactly that, and its real response only
        # arrived after our own 'quit'), not to our command. Our command
        # is echoed by the terminal before its result, so the response
        # we want carries our command's echo; keep reading prompts until
        # it does (bounded - a silent monitor still fails closed via
        # _read_prompt's timeout).
        for _ in range(5):
            if echo_fragment in decoded:
                return decoded
            decoded = self._read_prompt().decode("utf-8", "replace")
        raise BridgeError(
            "Renode monitor response to %r never carried its command echo "
            "within 5 prompts (startup input may still be draining): %r"
            % (text, decoded[:200]))

    # -- endpoint interface -------------------------------------------------
    def run_for_us(self, us):
        self.command('emulation RunFor "%s"' % format_seconds(us),
                     echo_fragment="RunFor")

    def read_u32(self, addr):
        return parse_u32(self.command("sysbus ReadDoubleWord %s"
                                      % format_address(addr),
                                      echo_fragment="ReadDoubleWord"))

    def write_u32(self, addr, value):
        self.command("sysbus WriteDoubleWord %s 0x%08x"
                     % (format_address(addr), int(value) & 0xFFFFFFFF),
                     echo_fragment="WriteDoubleWord")

    def close(self):
        try:
            self.command("quit")
        except (OSError, BridgeError):
            pass
        self._socket.close()
        if self._transcript is not None:
            self._transcript.close()
            self._transcript = None


class FmiSlave(object):
    """Abstract FMI 2.0 slave (CoSimulation) side of the bridge."""

    def read_real(self, name):
        raise NotImplementedError

    def do_step_us(self, current_us, step_us):
        raise NotImplementedError


class FmpyFmuSlave(FmiSlave):
    """FMPy-driven FMI 2.0 CoSimulation slave (guarded import).

    FMPy is TCL1 for the bounded doStep/readout role
    (docs/hw/tool-qualification.md section 3); the OpenModelica compiler gap
    is inherited by the evidence, not by this module. The FMI 2.0 state
    sequence is instantiate -> setupExperiment -> enterInitializationMode ->
    exitInitializationMode -> doStep*, as in the T1 holdup_001 pipeline.
    """

    def __init__(self, fmu_path):
        try:
            import fmpy
            from fmpy.model_description import read_model_description
        except ImportError as error:
            raise BridgeError(
                "FMPy is not installed; the T2 bridge refuses to run without "
                "the pinned FMU executor (fail-closed): %s" % error)
        # F-19 (issue #55): fmpy 0.3.24 - the TCL1 pin - exposes extract()
        # at the package TOP LEVEL (not in fmpy.util; that name does not
        # exist in this version), and FMU2Slave's constructor takes
        # guid/modelIdentifier/unzipDirectory keyword arguments, so the
        # canonical construction path is fmpy.instantiate_fmu(), the same
        # helper the T1 simulate_fmu pipeline drives. Dispatch 7 (run
        # 35723022901) failed with "cannot import name 'extract' from
        # 'fmpy.util'" before the FMU was ever touched.
        unzipdir = fmpy.extract(str(fmu_path))
        description = read_model_description(unzipdir)
        if not str(description.fmiVersion).startswith("2.0"):
            raise BridgeError(
                "T2 requires an FMI 2.0 CoSimulation FMU (bring-up finding "
                "F-2: the pinned OpenModelica 1.24.0 cannot export FMI 3.0); "
                "%s declares fmiVersion %r"
                % (fmu_path, description.fmiVersion))
        if description.coSimulation is None:
            raise BridgeError("%s has no CoSimulation interface" % fmu_path)
        self._description = description
        self._fmu = fmpy.instantiate_fmu(
            unzipdir, description, fmi_type="CoSimulation")
        self._fmu.setupExperiment(startTime=0.0)
        self._fmu.enterInitializationMode()
        self._fmu.exitInitializationMode()
        # FMPy 0.3.24 exposes no variableByName helper; build the name map
        # from the (complete) modelVariables list instead. (Bring-up finding
        # F-4.)
        self._refs = {variable.name: variable.valueReference
                      for variable in description.modelVariables}

    def read_real(self, name):
        if name not in self._refs:
            raise BridgeError("unknown FMU variable %r" % name)
        values = self._fmu.getReal([self._refs[name]])
        return float(values[0])

    def do_step_us(self, current_us, step_us):
        self._fmu.doStep(float(current_us) / 1e6, float(step_us) / 1e6)

    def close(self):
        self._fmu.terminate()


# ---------------------------------------------------------------------------
# The retention-ordering co-simulation
# ---------------------------------------------------------------------------

class RetentionBridge(object):
    """Couples the plant FMU and Renode for the HW-SF-002 retention scenario.

    Per master step (100 us):

    1. the FMU advances in 1 us internal sub-steps (FMU internal step <= 1 us,
       virtual-bench-plan section 6);
    2. Renode advances by exactly one master quantum (``emulation RunFor``);
    3. plant -> MCU marshalling: VBAT millivolts to the supervisor input;
    4. MCU -> plant observation: GPIO ODR (safe-latch pins);
    5. event capture: new values in the T2 trace slots are recorded with
       their exact hook-stamped timestamps (never the master-step time).

    Any contract violation raises ``BridgeError``: the run aborts and the
    orchestration writes no evidence (fail-closed).
    """

    _SLOTS = (
        (EVENT_RETENTION_WRITE, RETENTION_WRITE_US_ADDR),
        (EVENT_SAFE_LATCH, SAFE_LATCH_US_ADDR),
        (EVENT_IWDG_FIRE, IWDG_FIRE_US_ADDR),
    )

    def __init__(self, renode, fmu, duration_us=SCENARIO_DURATION_US,
                 master_step_us=MASTER_STEP_US, fmu_step_us=FMU_STEP_US):
        if duration_us <= 0 or duration_us % master_step_us:
            raise BridgeError("duration must be a positive multiple of the "
                              "master step")
        if master_step_us % fmu_step_us:
            raise BridgeError("the FMU step must divide the master step")
        self._renode = renode
        self._fmu = fmu
        self._duration_us = int(duration_us)
        self._master_step_us = int(master_step_us)
        self._fmu_step_us = int(fmu_step_us)

    # -- deterministic co-simulation loop -----------------------------------
    def run(self):
        self._preflight()
        previous = {name: self._renode.read_u32(addr)
                    for name, addr in self._SLOTS}
        events = []
        samples = []
        steps = self._duration_us // self._master_step_us
        for step in range(steps):
            step_start_us = step * self._master_step_us
            for sub in range(self._master_step_us // self._fmu_step_us):
                self._fmu.do_step_us(step_start_us + sub * self._fmu_step_us,
                                     self._fmu_step_us)
            self._renode.run_for_us(self._master_step_us)
            time_us = step_start_us + self._master_step_us

            vbat_v = self._fmu.read_real("v")
            self._renode.write_u32(VBAT_MV_IN_ADDR,
                                   vbat_to_millivolts(vbat_v))
            odr = self._renode.read_u32(GPIOA_ODR_ADDR)

            for name, addr in self._SLOTS:
                value = self._renode.read_u32(addr)
                if value != previous[name]:
                    if value <= previous[name] and previous[name] != 0:
                        raise BridgeError(
                            "%s timestamp moved backwards: %d -> %d"
                            % (name, previous[name], value))
                    if value > time_us:
                        raise BridgeError(
                            "%s timestamp %d us is ahead of emulation time "
                            "%d us (platform contract violation)" %
                            (name, value, time_us))
                    events.append({"event": name, "time_us": value})
                    previous[name] = value

            samples.append({"time_us": time_us,
                            "vbat_mv": vbat_to_millivolts(vbat_v),
                            "gpio_odr": odr})

        events.sort(key=lambda event: (event["time_us"], event["event"]))
        return {
            "events": events,
            "samples": samples,
            "retention_code": self._renode.read_u32(RTC_BKP0R_ADDR),
            "iwdgrstf": bool(self._renode.read_u32(RCC_CSR_ADDR)
                             & RCC_CSR_IWDGRSTF),
        }

    def _preflight(self):
        magic = self._renode.read_u32(T2_TRACE_MAGIC_ADDR)
        if magic != T2_TRACE_MAGIC:
            raise BridgeError(
                "T2 trace area magic is 0x%08x, expected 0x%08x: the platform "
                "was not brought up by cancestry-hw.resc (or the trace "
                "contract drifted)" % (magic, T2_TRACE_MAGIC))


def ordering_violation(events):
    """Return None when the QA-EV-01 ordering holds, else a failure string.

    The contract (HW-SF-004, QA-EV-01): strict
    retention write < safe-latch < IWDG fire, each observed exactly once.
    """
    by_name = {}
    for event in events:
        name = event["event"]
        if name in by_name:
            return "%s observed more than once" % name
        by_name[name] = event["time_us"]
    missing = [name for name in EXPECTED_ORDER if name not in by_name]
    if missing:
        return "missing event(s): %s" % ", ".join(missing)
    first, second, third = (by_name[name] for name in EXPECTED_ORDER)
    if not first < second < third:
        return ("ordering violated: %s=%d, %s=%d, %s=%d (strict "
                "retention < latch < iwdg required)"
                % (EXPECTED_ORDER[0], first, EXPECTED_ORDER[1], second,
                   EXPECTED_ORDER[2], third))
    return None
