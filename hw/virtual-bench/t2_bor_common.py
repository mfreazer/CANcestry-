#!/usr/bin/env python3
"""Shared machinery for the H-11 T2 brownout scenario (issue #64).

The scenario ``scenarios/t2_brownout_001.py`` is a thin client of this module.
It executes the REAL v1.0.0 firmware ELF (the off-tree
``firmware/bor_brownout.c`` driver, ``CANCESTRY_T2_DRIVER=bor_brownout``) on
the platform described by ``renode/stm32g474-cancestry.repl`` plus the BOR
reset injector (``renode/bor_reset_injector.py``), brought up by
``renode/cancestry-hw-bor.resc`` - AND it steps the H-11 plant
(``CancestryLib.Power.BOR``, compiled headless by the pinned OpenModelica)
over the deterministic 100 us master / 1 us FMU co-simulation contract of the
H-07 bridge.

Scenario shape:

1. preflight: pinned Renode version, brownout ELF, platform magic, injector
   window magic / NRST level / counters, all trace slots at 0.
2. The machine is advanced exclusively with fixed ``emulation RunFor`` quanta
   (100 us master step); the plant is advanced in 1 us FMU sub-steps.
3. The MODELLED BOR ASSERTION drives the reset injection: the first master
   step at which the plant's ``nrst`` signal reads asserted (the modelled
   threshold crossing at t = 10 ms, issue #64) is the instant the orchestrator
   commands ``InjectBrownout`` - so the injection is a plant-driven BOR event,
   not a scenario constant that happens to be 10 ms. The command is confirmed
   by readback (fail-closed).
4. The injector asserts NRST (active-low, 100 us per issue #64), discards the
   main-SRAM marker word, sets ``RCC_CSR.BORRSTF`` and takes the BOR machine
   reset; the REAL firmware re-enters its reset handler and runs its
   post-reset path.
5. Event slots (the retention-write hook, the injector stamps, the two symbol
   hooks) are scanned after every quantum and recorded with their exact
   stamped times; the firmware's own report slots (SRAM marker observed at
   boot, retained code recovered, run complete, alive counter) are read at the
   end of the window.
6. The issue #64 ordering chain is asserted:

       retention_write < BOR detection < recovery

   together with the retention/SRAM split (the RTC backup domain preserved,
   main SRAM lost), the BOR reset cause and the NRST pulse length. ANY
   violation aborts BEFORE evidence is written (fail-closed).
7. The evidence artifact is written (deterministic bytes, no wall clock).
   ``--emit-pending`` writes the honest PENDING manifest (pass=false, CL0)
   without touching a run - the H-06/H-07/H-10 pattern. ``--check`` re-runs
   and requires byte-identical evidence (issue #53 determinism gate pattern).

The hash chain is FMU + ELF + bridge (monitor client) + injector + trace,
exactly the chain ``schemas/hw/hw-t2-brownout-evidence-0.1.0.schema.json``
declares for the pending and the passing forms. Both TCL2 producer gaps
(openmodelica, renode) are inherited; ``bor_reset_injector`` and
``fmi_bridge`` are TCL1 and carry none.

Requirements traced: HW-SF-002 (sub-events (ii) and (iii)), HW-SF-004;
HwAGENTS.md rules 1, 4, 5 and 6.
Oracle: none (issue #64: no OR-XXX for the BOR threshold behaviour, the
hysteresis or the reset timing - the row stays sim-pending / CL0).
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
SCHEMA_PATH = (REPO_ROOT / "schemas" / "hw"
               / "hw-t2-brownout-evidence-0.1.0.schema.json")
SIM_CASE_PATH = REPO_ROOT / "hw" / "tests" / "cases" / "bor_brownout_001.simcase.json"
MODEL_PATH = REPO_ROOT / "hw" / "model" / "CancestryLib" / "Power" / "BOR.mo"

sys.path.insert(0, str(BRIDGE_DIR))

from fmi_bridge import (  # noqa: E402
    FIXED_SEED,
    FMU_STEP_US,
    MASTER_STEP_US,
    FmpyFmuSlave,
    RenodeMonitorEndpoint,
    safe_latch_asserted,
    vbat_to_millivolts,
)
from run_t2_retention import (  # noqa: E402
    T2SetupError,
    _free_port,
    check_renode_version,
    connect_monitor,
    locate_elf,
    locate_omc,
    locate_renode,
    render_evidence_bytes,
    sha256_file,
)

# ---------------------------------------------------------------------------
# Contract constants - mirrored copies of renode/bor_reset_injector.py,
# renode/cancestry-hw-bor.resc, stm32g474-cancestry.repl (H-11 slot contract)
# and firmware/bor_brownout.c. hw/virtual-bench/test_t2_brownout.py pins every
# copy against this module so a drift anywhere fails the offline test suite
# instead of corrupting a run.
# ---------------------------------------------------------------------------

# Timing (the H-07 bridge contract plus the issue #64 brownout constants).
SCENARIO_DURATION_US = 50_000  # 50 ms visualization window (issue #64)
BROWNOUT_AT_US = 10_000        # the modelled BOR assertion / injection
NRST_PULSE_US = 100            # asserted NRST length (issue #64)

# Injector register window (sysbus 0x40001000) and command codes.
BOR_INJ_MAGIC = 0x424F5231
BOR_BASE = 0x40001000
BOR_OFF_MAGIC = 0x00
BOR_OFF_COMMAND = 0x04
BOR_OFF_NRST_LEVEL = 0x08
BOR_OFF_INJECT_COUNT = 0x0C
BOR_OFF_INJECT_US = 0x10
BOR_OFF_RELEASE_US = 0x14
BOR_OFF_SRAM_LOST = 0x18
BOR_OFF_ERROR = 0x1C
BOR_OFF_TIME_US = 0x20
BOR_CMD_INJECT_BROWNOUT = 0x42
BOR_CMD_NRST_LEVEL = 0x4C
BOR_CMD_INJECT_US = 0x49
BOR_CMD_RELEASE_US = 0x52
BOR_CMD_COUNTERS = 0x4E
BOR_CMD_TIME_US = 0x54
NRST_RELEASED = 0x1
NRST_ASSERTED = 0x0
BOR_ERR_NONE = 0
BOR_ERR_UNKNOWN_COMMAND = 1
BOR_ERR_ALREADY_INJECTED = 2

# Trace-area slots (stm32g474-cancestry.repl H-11 contract; one writer each).
SLOT_MAGIC_ADDR = 0x60000000
SLOT_MAGIC = 0x54324353
SLOT_RETENTION_WRITE_US = 0x60000008
SLOT_BOR_INJECT_US = 0x60000070
SLOT_NRST_RELEASE_US = 0x60000074
SLOT_BOR_DETECT_US = 0x60000078
SLOT_BOR_RECOVER_US = 0x6000007C
SLOT_SRAM_MAGIC_AT_BOOT = 0x60000080
SLOT_RUN_COMPLETE = 0x60000084
SLOT_ALIVE_COUNTER = 0x60000088
SLOT_RETENTION_CODE_AT_DETECT = 0x6000008C
SLOT_NRST_LEVEL = 0x60000104
SLOT_SRAM_CANARY_LOST = 0x60000108

# Firmware-visible registers / T2-declared SRAM marker word.
VBAT_MV_IN_ADDR = 0x60000020
GPIOA_ODR_ADDR = 0x48000014
RTC_BKP0R_ADDR = 0x40002850
RCC_CSR_ADDR = 0x40021094
RCC_CSR_BORRSTF = 1 << 27
RCC_CSR_IWDGRSTF = 1 << 29
SRAM_CANARY_ADDR = 0x20017000
SRAM_CANARY_MAGIC = 0x5AA5C0DE

# CANCESTRY_FAULT_CODE_QUEUE_SATURATION (core/event/include/cancestry/event/
# fault.h) - the canonical retained terminal code the real escalation path
# writes, and the code this scenario requires to survive the BOR reset.
FAULT_CODE = 0x45565101

# Plant-side FMU variables (CancestryLib.Power.BOR).
FMU_RAIL = "v"
FMU_NRST = "nrst"

REQUIREMENT_ID = "HW-SF-002"
ORACLE_ID = "none"

# Tool pins recorded by the scenario evidence. openmodelica IS a producer
# (the BOR plant FMU) and renode IS a producer (the platform model and its
# scripted peripherals): both TCL2 gaps are inherited verbatim in sorted
# producer order, exactly the string ci/check_hw_traceability.py rule 8
# computes. bor_reset_injector and fmi_bridge are TCL1 and carry no gap.
TOOL_PINS = {
    "bor_reset_injector": "0.1.0",
    "fmpy": "0.3.24",
    "openmodelica": "1.24",
    "python": ">=3.10",
    "renode": "1.16",
}
OPENMODELICA_TOOL_GAP = (
    "Compiler semantics outside independently validated output remain "
    "unqualified; OR-001/OR-002 regressions do not cover all translation and "
    "solver behavior.")
RENODE_TOOL_GAP = (
    "Renode peripheral models (IWDG, GPIO, RTC_BKP, DWT) are scripted "
    "emulations, not vendor-validated silicon models: platform-model bugs "
    "can alter firmware-visible timing or register semantics, and this gap "
    "is inherited by every T2 evidence artifact; golden-trace correlation "
    "against vendor reference behavior and T4 bench correlation are required "
    "before any promotion.")
INHERITED_VALIDATION_GAP = (
    "openmodelica: %s | renode: %s" % (OPENMODELICA_TOOL_GAP, RENODE_TOOL_GAP))

TRACE_COLUMNS = "time_us,rail_mv,nrst,gpio_odr,alive_counter"

CONSOLE_LOG_NAME = "t2_brownout_renode_console.log"
TRANSCRIPT_LOG_NAME = "t2_brownout_monitor_transcript.log"


class T2BrownoutError(Exception):
    """A fail-closed scenario contract violation: no evidence is written."""


class ScenarioSpec(object):
    """Everything the H-11 scenario adds on top of the shared machinery."""

    def __init__(self, *, case_id, evidence_relative, scenario_name,
                 evidence_of, pending_reason, not_covered, pinned_sources,
                 watch_slots):
        self.case_id = case_id
        self.evidence_relative = evidence_relative
        self.scenario_name = scenario_name
        self.evidence_of = evidence_of
        self.pending_reason = pending_reason
        self.not_covered = list(not_covered)
        self.pinned_sources = tuple(pinned_sources)
        self.watch_slots = dict(watch_slots)  # {slot_name: address}

    @property
    def evidence_path(self):
        return REPO_ROOT / self.evidence_relative


# ---------------------------------------------------------------------------
# Renode launch / preflight / injection
# ---------------------------------------------------------------------------

def launch_renode(renode_bin, elf_path, port, console_log_path):
    """Start Renode headless with the brownout bring-up script.

    Same invocation discipline as the H-07/H-10 runners (F-20 console-drain
    thread, F-32 clean quoted $elf with no '@' marker), against
    cancestry-hw-bor.resc.
    """
    command = [
        str(renode_bin), "--disable-xwt", "--port", str(port),
        "-e", '$elf="%s"' % elf_path.resolve(),
        "-e", "include @cancestry-hw-bor.resc",
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


def preflight(endpoint, spec):
    """Assert the platform + injector are in the only admissible state.

    Every check fails closed: a wrong machine configuration aborts BEFORE any
    stimulus, so a misconfigured platform can never produce evidence.
    """
    def eq(addr, expected, what):
        actual = endpoint.read_u32(addr)
        if actual != expected:
            raise T2BrownoutError(
                "preflight: %s at 0x%08x reads 0x%08x, expected 0x%08x "
                "(platform/injector not in the contracted initial state)"
                % (what, addr, actual, expected))

    eq(SLOT_MAGIC_ADDR, SLOT_MAGIC,
       "t2 trace magic (cancestry-hw-bor.resc)")
    eq(BOR_BASE + BOR_OFF_MAGIC, BOR_INJ_MAGIC, "BOR injector window magic")
    eq(BOR_BASE + BOR_OFF_NRST_LEVEL, NRST_RELEASED, "initial NRST level")
    eq(BOR_BASE + BOR_OFF_INJECT_COUNT, 0, "initial injection count")
    eq(BOR_BASE + BOR_OFF_ERROR, 0, "injector error register")
    eq(BOR_BASE + BOR_OFF_SRAM_LOST, 0, "initial SRAM-lost flag")
    eq(SLOT_NRST_LEVEL, NRST_RELEASED, "NRST level state word")
    eq(SLOT_SRAM_CANARY_LOST, 0, "SRAM-lost state word")
    for name, addr in sorted(spec.watch_slots.items()):
        eq(addr, 0, "trace slot %s" % name)
    # The main-SRAM marker belongs to the ELF: before the first boot it reads
    # whatever the freshly created machine holds (deterministically 0).
    eq(SRAM_CANARY_ADDR, 0, "main-SRAM marker word before the ELF runs")


def inject_brownout(endpoint):
    """Issue the brownout injection through the injector command interface.

    Primary path: monitor method call on the peripheral -
    ``sysbus bor_reset_injector ControlWrite 0x42 0x1``, wrapped by
    PythonPeripheral.ControlWrite -> USER request. The injector's error
    register and the stamp slot are then read back; anything but a clean
    acceptance aborts the run (fail-closed).
    """
    endpoint.command(
        "sysbus bor_reset_injector ControlWrite 0x%02X 0x1"
        % BOR_CMD_INJECT_BROWNOUT,
        echo_fragment="ControlWrite")
    error = endpoint.read_u32(BOR_BASE + BOR_OFF_ERROR)
    if error != BOR_ERR_NONE:
        raise T2BrownoutError(
            "injector rejected the brownout command with error code %d "
            "(fail-closed: the reset state is not trusted for this run)"
            % error)


# ---------------------------------------------------------------------------
# The plant-driven co-simulation
# ---------------------------------------------------------------------------

class BrownoutBridge(object):
    """Couples the BOR plant FMU and Renode for the brownout scenario.

    Per master step (100 us):

    1. the FMU advances in 1 us internal sub-steps (the H-07 bridge contract);
       until the injection has happened every sub-step is checked for the
       modelled BOR assertion, so the plant-side stamp is exact to 1 us;
    2. Renode advances by exactly one master quantum (``emulation RunFor``);
    3. plant -> MCU marshalling: the modelled rail voltage (millivolts) to the
       platform's supervisor input register;
    4. MCU -> plant observation: GPIO ODR (safe-latch pins);
    5. event capture: new values in the trace slots are recorded with their
       exact hook-stamped timestamps (never the master-step time).

    The BOR reset injection is issued exactly once, at the first master-step
    boundary at which the plant's reset signal reads asserted. After the
    machine reset the co-simulation continues: the post-reset firmware path is
    what the scenario captures.
    """

    def __init__(self, renode, fmu, spec,
                 duration_us=SCENARIO_DURATION_US,
                 master_step_us=MASTER_STEP_US, fmu_step_us=FMU_STEP_US):
        if duration_us <= 0 or duration_us % master_step_us:
            raise T2BrownoutError(
                "duration must be a positive multiple of the master step")
        if master_step_us % fmu_step_us:
            raise T2BrownoutError("the FMU step must divide the master step")
        self._renode = renode
        self._fmu = fmu
        self._spec = spec
        self._duration_us = int(duration_us)
        self._master_step_us = int(master_step_us)
        self._fmu_step_us = int(fmu_step_us)

    def run(self):
        self._preflight()
        previous = {name: self._renode.read_u32(addr)
                    for name, addr in self._spec.watch_slots.items()}
        events = {}
        samples = []
        model_assert_us = 0
        model_release_us = 0
        injected = False
        steps = self._duration_us // self._master_step_us
        for step in range(steps):
            step_start_us = step * self._master_step_us
            for sub in range(self._master_step_us // self._fmu_step_us):
                self._fmu.do_step_us(
                    step_start_us + sub * self._fmu_step_us, self._fmu_step_us)
                if not injected and model_assert_us == 0:
                    if self._fmu.read_real(FMU_NRST) < 0.5:
                        model_assert_us = (step_start_us
                                           + (sub + 1) * self._fmu_step_us)
            self._renode.run_for_us(self._master_step_us)
            time_us = step_start_us + self._master_step_us

            rail_v = self._fmu.read_real(FMU_RAIL)
            self._renode.write_u32(VBAT_MV_IN_ADDR,
                                   vbat_to_millivolts(rail_v))
            odr = self._renode.read_u32(GPIOA_ODR_ADDR)

            # Plant-driven injection: the modelled BOR assertion is the
            # scenario's trigger, and it must land on this master-step
            # boundary (issue #64 fixes it at 10 ms).
            if not injected and model_assert_us != 0:
                if model_assert_us != time_us:
                    raise T2BrownoutError(
                        "the modelled BOR assertion at %d us is not aligned to "
                        "the %d us master-step boundary %d us (the injection "
                        "instant must be exact)"
                        % (model_assert_us, self._master_step_us, time_us))
                inject_brownout(self._renode)
                injected = True

            if model_release_us == 0:
                if self._fmu.read_real(FMU_NRST) > 0.5:
                    model_release_us = time_us

            for name, addr in sorted(self._spec.watch_slots.items()):
                value = self._renode.read_u32(addr)
                if value != previous[name]:
                    if previous[name] != 0:
                        raise T2BrownoutError(
                            "trace slot %s transitioned twice (0->%d->%d): "
                            "the exactly-once capture contract is broken"
                            % (name, previous[name], value))
                    if value == 0:
                        raise T2BrownoutError(
                            "trace slot %s cleared after being stamped "
                            "(single-writer contract broken)" % name)
                    if value > time_us:
                        raise T2BrownoutError(
                            "%s timestamp %d us is ahead of emulation time "
                            "%d us" % (name, value, time_us))
                    events[name] = value
                    previous[name] = value

            samples.append({
                "time_us": time_us,
                "rail_mv": vbat_to_millivolts(rail_v),
                "nrst": self._renode.read_u32(SLOT_NRST_LEVEL),
                "gpio_odr": odr,
                "alive_counter": self._renode.read_u32(SLOT_ALIVE_COUNTER),
            })

        if not injected:
            raise T2BrownoutError(
                "the plant never asserted the modelled BOR reset inside the "
                "%d us window: the scenario stimulus did not occur"
                % self._duration_us)
        events["model_assert_us"] = model_assert_us
        events["model_release_us"] = model_release_us

        summary = {
            "alive_counter": samples[-1]["alive_counter"],
            "alive_at_inject": next(
                (sample["alive_counter"] for sample in samples
                 if sample["time_us"] == events["bor_inject_us"]), 0),
            "gpioa_odr": samples[-1]["gpio_odr"],
            "retention_code": self._renode.read_u32(RTC_BKP0R_ADDR),
            "retention_code_at_detect": self._renode.read_u32(
                SLOT_RETENTION_CODE_AT_DETECT),
            "sram_canary": self._renode.read_u32(SRAM_CANARY_ADDR),
            "sram_magic_at_boot": self._renode.read_u32(SLOT_SRAM_MAGIC_AT_BOOT),
            "run_complete": self._renode.read_u32(SLOT_RUN_COMPLETE),
            "rcc_csr": self._renode.read_u32(RCC_CSR_ADDR),
        }
        counters = {
            "injections": self._renode.read_u32(BOR_BASE + BOR_OFF_INJECT_COUNT),
            "injector_error": self._renode.read_u32(BOR_BASE + BOR_OFF_ERROR),
            "sram_lost": self._renode.read_u32(BOR_BASE + BOR_OFF_SRAM_LOST),
        }
        return events, summary, counters, samples

    def _preflight(self):
        magic = self._renode.read_u32(SLOT_MAGIC_ADDR)
        if magic != SLOT_MAGIC:
            raise T2BrownoutError(
                "T2 trace area magic is 0x%08x, expected 0x%08x: the platform "
                "was not brought up by cancestry-hw-bor.resc (or the trace "
                "contract drifted)" % (magic, SLOT_MAGIC))


# ---------------------------------------------------------------------------
# Invariants (the scenario contracts; any violation fails closed)
# ---------------------------------------------------------------------------

def check_invariants(events, summary, counters, samples):
    """Return the list of invariant violations (empty = the run may pass)."""
    failures = []
    required = ("retention_write_us", "bor_inject_us", "nrst_release_us",
                "bor_detect_us", "bor_recover_us", "model_assert_us",
                "model_release_us")
    for name in required:
        if events.get(name, 0) == 0:
            failures.append("missing event: %s" % name)
    if failures:
        return failures

    # Issue #64: the brownout is injected at t = 10 ms, and the injection is
    # driven by the modelled BOR assertion - the plant stamp and the emulator
    # stamp must be the same instant.
    if events["bor_inject_us"] != BROWNOUT_AT_US:
        failures.append(
            "brownout injected at %d us, expected exactly %d us (issue #64: "
            "t = 10 ms)" % (events["bor_inject_us"], BROWNOUT_AT_US))
    if events["model_assert_us"] != events["bor_inject_us"]:
        failures.append(
            "the plant asserted the modelled BOR reset at %d us but the "
            "injection was stamped at %d us (the injection must be driven by "
            "the model, not by a scenario constant)"
            % (events["model_assert_us"], events["bor_inject_us"]))
    # Issue #64: NRST is asserted for exactly 100 us.
    pulse = events["nrst_release_us"] - events["bor_inject_us"]
    if pulse != NRST_PULSE_US:
        failures.append(
            "NRST was asserted for %d us, expected exactly %d us (issue #64)"
            % (pulse, NRST_PULSE_US))
    if events["model_release_us"] != events["nrst_release_us"]:
        failures.append(
            "the modelled release gate opened at %d us but the injector "
            "released NRST at %d us (model and reset line must agree)"
            % (events["model_release_us"], events["nrst_release_us"]))

    # Issue #64 ordering chain: retention write < BOR detection < recovery.
    if not events["retention_write_us"] < events["bor_inject_us"]:
        failures.append(
            "the firmware published the retained code at %d us, not before "
            "the brownout at %d us (HW-SF-004 ordering)"
            % (events["retention_write_us"], events["bor_inject_us"]))
    if not events["bor_inject_us"] < events["bor_detect_us"]:
        failures.append(
            "BOR detection at %d us does not follow the reset injection at "
            "%d us" % (events["bor_detect_us"], events["bor_inject_us"]))
    if not events["bor_detect_us"] < events["bor_recover_us"]:
        failures.append(
            "safe state was restored at %d us before the fault was detected "
            "at %d us (HW-SF-004 ordering)"
            % (events["bor_recover_us"], events["bor_detect_us"]))

    # Retention domain preserved, main SRAM lost (HW-SF-002 (ii)/(iii)).
    if summary["retention_code_at_detect"] != FAULT_CODE:
        failures.append(
            "the post-BOR boot recovered the retained code 0x%08x, expected "
            "0x%08x (the retention domain did not preserve it across the "
            "reset)" % (summary["retention_code_at_detect"], FAULT_CODE))
    if summary["retention_code"] != FAULT_CODE:
        failures.append(
            "the retained code read back at the end of the window is 0x%08x, "
            "expected 0x%08x (a BOR reset must not disturb the RTC backup "
            "domain)" % (summary["retention_code"], FAULT_CODE))
    if summary["sram_magic_at_boot"] != 0:
        failures.append(
            "the post-BOR boot observed the main-SRAM marker 0x%08x, expected "
            "0 (a BOR reset does not preserve main SRAM)"
            % summary["sram_magic_at_boot"])
    if summary["sram_canary"] != 0:
        failures.append(
            "the main-SRAM marker still reads 0x%08x at the end of the "
            "window, expected 0" % summary["sram_canary"])
    if counters["sram_lost"] != 1:
        failures.append("the injector SRAM-lost flag is %d, expected 1"
                        % counters["sram_lost"])

    # Reset cause classification and firmware liveness.
    if not summary["rcc_csr"] & RCC_CSR_BORRSTF:
        failures.append("RCC_CSR.BORRSTF is clear at the end of the window "
                        "(the reset cause was not classified as BOR)")
    if summary["rcc_csr"] & RCC_CSR_IWDGRSTF:
        failures.append("RCC_CSR.IWDGRSTF is set: no IWDG reset belongs to "
                        "this scenario")
    if summary["run_complete"] != 1:
        failures.append(
            "run_complete is %d, expected 1 (the post-reset path did not "
            "complete: crash, hang or logic error)" % summary["run_complete"])
    if not summary["alive_counter"] > summary["alive_at_inject"]:
        failures.append(
            "the firmware alive counter did not advance after the recovery "
            "(%d at injection, %d at the end)"
            % (summary["alive_at_inject"], summary["alive_counter"]))
    if counters["injector_error"] != BOR_ERR_NONE:
        failures.append("injector error register is %d" % counters["injector_error"])
    if counters["injections"] != 1:
        failures.append("expected exactly 1 brownout injection, counted %d"
                        % counters["injections"])
    if not safe_latch_asserted(summary["gpioa_odr"]):
        failures.append(
            "the safe-latch pins are not both de-energised after the recovery "
            "(GPIOA ODR 0x%08x)" % summary["gpioa_odr"])
    if len(samples) != SCENARIO_DURATION_US // MASTER_STEP_US:
        failures.append("trace has %d samples, expected %d"
                        % (len(samples),
                           SCENARIO_DURATION_US // MASTER_STEP_US))
    return failures


def trace_row(sample):
    """One trace-CSV row, in the order declared by TRACE_COLUMNS."""
    return "%d,%d,0x%x,0x%04x,%d\n" % (
        sample["time_us"], sample["rail_mv"], sample["nrst"],
        sample["gpio_odr"], sample["alive_counter"])


# ---------------------------------------------------------------------------
# Scenario execution
# ---------------------------------------------------------------------------

def run_brownout_scenario(renode_bin, elf_path, fmu_path, spec,
                          build_dir=BUILD_DIR):
    """Execute the H-11 brownout scenario; return (events, summary, runlog)."""
    console_log = build_dir / CONSOLE_LOG_NAME
    transcript_log = build_dir / TRANSCRIPT_LOG_NAME
    port = _free_port()
    process = launch_renode(renode_bin, elf_path, port, console_log)
    endpoint = None
    try:
        endpoint = connect_monitor(port, transcript_path=transcript_log,
                                   clock=time.time)
        preflight(endpoint, spec)
        fmu = FmpyFmuSlave(fmu_path)
        try:
            events, summary, counters, samples = BrownoutBridge(
                endpoint, fmu, spec).run()
        finally:
            fmu.close()

        failures = check_invariants(events, summary, counters, samples)
        if failures:
            raise T2BrownoutError("scenario invariants failed: %s"
                                  % "; ".join(failures))

        build_dir.mkdir(parents=True, exist_ok=True)
        trace_path = build_dir / ("%s_trace.csv" % spec.case_id)
        trace_path.write_text(
            "%s\n" % TRACE_COLUMNS
            + "".join(trace_row(sample) for sample in samples),
            encoding="utf-8")
        runlog = {
            "bridge_sha256": sha256_file(BRIDGE_DIR / "fmi_bridge.py"),
            "elf_sha256": sha256_file(elf_path),
            "fmu_sha256": sha256_file(fmu_path),
            "injector_sha256": sha256_file(
                BRIDGE_DIR / "renode" / "bor_reset_injector.py"),
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


# ---------------------------------------------------------------------------
# Plant FMU build (pinned OpenModelica; the T1 model is the T2 plant)
# ---------------------------------------------------------------------------

def omc_build_script_text():
    """Text of the omc script that builds the BOR FMU (pure, pinned).

    FMPy 0.3.24's CoSimulation path is used, so the FMU must be an FMI 2.0 CS
    archive (H-08 bring-up finding F-2: the pinned OpenModelica 1.24.0 cannot
    export FMI 3.0). ``platforms={"static"}`` keeps the archive
    platform-independent, exactly as the retention runner does.
    """
    return (
        'loadFile("%s");\n'
        "getErrorString();\n"
        'setCommandLineOptions("-d=nogen,noevalfunc");\n'
        'buildModelFMU(CancestryLib.Power.BOR, version="2.0", fmuType="cs", '
        'platforms={"static"});\n'
        "getErrorString();\n" % MODEL_PATH)


def build_fmu(omc, build_dir=BUILD_DIR):
    """Build the BOR FMU headless; return its path (deterministic inputs)."""
    import re

    build_dir.mkdir(parents=True, exist_ok=True)
    script = build_dir / "omc_build_t2_bor.mos"
    script.write_text(omc_build_script_text(), encoding="utf-8")
    result = subprocess.run([omc, "--showErrorMessages", str(script)],
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            cwd=str(build_dir), timeout=1800)
    output = result.stdout.decode("utf-8", "replace")
    if result.returncode != 0:
        raise T2SetupError("omc buildModelFMU failed (last output):\n%s"
                           % output[-2000:])
    match = re.findall(r'([^\s"\'()]+\.fmu)', output)
    if match:
        fmu = Path(match[-1])
        if not fmu.is_absolute():
            fmu = build_dir / fmu
        if fmu.is_file():
            return fmu
    raise T2SetupError("omc buildModelFMU produced no FMU archive:\n%s"
                       % output[-2000:])


# ---------------------------------------------------------------------------
# Evidence documents (deterministic pending + passing forms)
# ---------------------------------------------------------------------------

def live_source_hashes(spec):
    return {relative: sha256_file(REPO_ROOT / relative)
            for relative in sorted(spec.pinned_sources)}


def expected_pending_manifest(spec):
    """The PENDING manifest of the scenario (pass=false): honest disposition."""
    return {
        "case_id": spec.case_id,
        "credibility_level": "CL0",
        "evidence_of": spec.evidence_of,
        "firmware": {
            "elf": ("built off-tree from tag v1.0.0 with the pinned T2 "
                    "toolchain image (ci/docker/Dockerfile.t2) and "
                    "CANCESTRY_T2_DRIVER=bor_brownout "
                    "(hw/virtual-bench/firmware/build_firmware.sh); supplied "
                    "at run time via CANCESTRY_T2_BOR_ELF or --elf; never "
                    "committed; its sha256 is recorded at run time"),
            "modification": "none; the firmware is read-only (issue #64)",
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
            "fmu_step_us": FMU_STEP_US,
            "inject_at_us": BROWNOUT_AT_US,
            "master_step_us": MASTER_STEP_US,
            "method": "virtual_bench",
            "nrst_pulse_us": NRST_PULSE_US,
            "seed": FIXED_SEED,
        },
        "schema_version": "0.1.0",
        "sim_case": "hw/tests/cases/bor_brownout_001.simcase.json",
        "source_hashes": live_source_hashes(spec),
        "status": "pending",
        "tool_pins": dict(TOOL_PINS),
        "trace": {
            "artifact": ("build/hw/%s_trace.csv (per-run artifact, "
                         "gitignored)" % spec.case_id),
            "columns": TRACE_COLUMNS,
            "hashing": ("sha256 of the trace CSV, measured invariants and "
                        "measured tool versions recorded in build/hw/%s."
                        "runlog.json" % spec.case_id),
        },
    }


def passing_document(spec, events, summary, runlog):
    """Assemble the PASSING evidence document (deterministic bytes)."""
    document = expected_pending_manifest(spec)
    document.update({
        "credibility_level": "CL1",
        "events": {
            "bor_detect_us": events["bor_detect_us"],
            "bor_inject_us": events["bor_inject_us"],
            "bor_recover_us": events["bor_recover_us"],
            "model_assert_us": events["model_assert_us"],
            "model_release_us": events["model_release_us"],
            "nrst_release_us": events["nrst_release_us"],
            "retention_write_us": events["retention_write_us"],
        },
        "hashes": {
            "bridge": runlog["bridge_sha256"],
            "elf": runlog["elf_sha256"],
            "fmu": runlog["fmu_sha256"],
            "injector": runlog["injector_sha256"],
            "trace": runlog["trace_sha256"],
        },
        "invariants": {
            "brownout_before_detection": (
                events["bor_inject_us"] < events["bor_detect_us"]),
            "detection_before_recovery": (
                events["bor_detect_us"] < events["bor_recover_us"]),
            "firmware_alive_after_recovery": (
                summary["alive_counter"] > summary["alive_at_inject"]),
            "main_sram_lost": (summary["sram_magic_at_boot"] == 0
                               and summary["sram_canary"] == 0),
            "model_assert_matches_injection": (
                events["model_assert_us"] == events["bor_inject_us"]),
            "nrst_pulse_us": (events["nrst_release_us"]
                              - events["bor_inject_us"]),
            "reset_cause_is_bor": bool(summary["rcc_csr"] & RCC_CSR_BORRSTF)
            and not bool(summary["rcc_csr"] & RCC_CSR_IWDGRSTF),
            "retention_before_brownout": (
                events["retention_write_us"] < events["bor_inject_us"]),
            "retention_code_preserved": (
                summary["retention_code_at_detect"] == FAULT_CODE
                and summary["retention_code"] == FAULT_CODE),
        },
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
    """Argparse + dispatch shared by the scenario script."""
    description = ("T2 scenario %s (%s, issue #64 H-11)."
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
                        help="brownout-scenario firmware ELF (default: "
                             "CANCESTRY_T2_BOR_ELF/CANCESTRY_T2_ELF)")
    parser.add_argument("--fmu", default=None,
                        help="prebuilt BOR FMU (default: build it via omc; "
                             "the --check twin reuses the same FMU bytes so "
                             "the determinism comparison pairs identical "
                             "plant artifacts)")
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
        elf = args.elf or os.environ.get("CANCESTRY_T2_BOR_ELF") \
            or os.environ.get("CANCESTRY_T2_ELF")
        elf_path = locate_elf(elf)
        if args.fmu:
            fmu_path = Path(args.fmu)
            if not fmu_path.is_file():
                raise T2SetupError("prebuilt FMU not found: %s" % fmu_path)
        else:
            fmu_path = build_fmu(locate_omc())
        events, summary, runlog = run_brownout_scenario(
            renode_bin, elf_path, fmu_path, spec)
    except (T2SetupError, T2BrownoutError) as error:
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
