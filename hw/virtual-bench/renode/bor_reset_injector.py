# Brown-out reset (BOR) injector for the CANcestry T2 virtual bench
# (H-11, issue #64).
#
# Implements: HW-SF-002 sub-events (ii) main-rail brownout to BOR level 3 and
# (iii) main-rail removal - the RESET side of the retention claim - and the
# HW-SF-004 BOR threshold ordering. It exercises the REAL v1.0.0 firmware
# (off-tree driver firmware/bor_brownout.c) on the unmodified H-07 platform:
# the injector pulls the active-low NRST line, the machine takes the BOR
# reset, and the firmware's post-reset path recovers the retained fault code.
# This file is a Renode platform peripheral, not firmware (issue #64
# constraint: firmware read-only).
#
# ---------------------------------------------------------------------------
# What this peripheral models - and what it deliberately does not
# ---------------------------------------------------------------------------
#
# MODELED (device-external reset line + the reset's retention consequences):
#   * The NRST pin as an ACTIVE-LOW reset line (issue #64 deliverable 4):
#     `InjectBrownout` pulls NRST LOW (level 0 = asserted) and the injector
#     releases it (level 1 = high) exactly NRST_PULSE_US later - the 100 us
#     brownout pulse the issue fixes, evaluated as a pure function of
#     emulation virtual time (the lazy-release pattern of iwdg_model.py).
#   * The BOR reset itself: the machine reset request happens at the assertion
#     instant, so the REAL firmware re-enters its reset handler and runs its
#     post-reset path (Reset_Handler in the off-tree startup.s) - nothing is
#     reimplemented in the peripheral.
#   * The reset cause: RCC_CSR.BORRSTF (bit 27, RM0440 / CMSIS
#     stm32g474xx.h BORRSTF position) is set through the bus, so the scripted
#     RCC model persists it into its retention shadow exactly as it does for
#     the IWDG flag. The firmware classifies the reset from that flag.
#   * MAIN SRAM LOSS: the T2-declared main-SRAM marker word
#     (T2_SRAM_CANARY_ADDR, 0x20017000 - reserved by the platform contract,
#     outside the ELF's .data/.bss) is discarded by the reset: a BOR reset
#     does not preserve main SRAM content. The post-reset firmware reads the
#     cleared word and reports it, while the retention domain survives.
#
# NOT MODELED (documented; no claim is derived from any of these):
#   * BOR cell physics (threshold spread, hysteresis, reset filter, the
#     finite fall time of the rail, retention-domain switching): the T1 plant
#     is hw/model/CancestryLib/Power/BOR.mo, which DRIVES this injector in the
#     t2_brownout_001 scenario (the orchestrator issues the injection on the
#     modelled BOR assertion). The peripheral itself takes the injection
#     instant from the command, never from a modelled voltage.
#   * Any other BOR consequence on silicon (option-byte BOR level,
#     backup-domain switching behaviour, startup timing): external, un-modelled.
#   * The supply itself: the rail is the FMU's business; the platform's
#     supervisor input register carries it (bridge marshalling).
#
# ---------------------------------------------------------------------------
# Interfaces
# ---------------------------------------------------------------------------
#
# (a) Command interface for the orchestrator (issue #64 deliverable 4). The
#     monitor reaches PythonPeripheral.ControlWrite / ControlRead as
#
#         sysbus.bor_reset_injector ControlWrite 0x42 0x1  # InjectBrownout
#         sysbus.bor_reset_injector ControlRead  0x4C 0x0  # NRST level
#         sysbus.bor_reset_injector ControlRead  0x4E 0x0  # counters
#         sysbus.bor_reset_injector ControlRead  0x54 0x0  # emulation time
#
#     The device path is ONE dotted token: a bare 'sysbus' resolves to the
#     SystemBus object, so the two-token form 'sysbus bor_reset_injector
#     ...' fails with a recoverable error and this module never sees the
#     command (dispatch 17; fmi_bridge.command() fails closed on the
#     monitor's error marker).
#
#     Command codes are ASCII so the monitor transcript stays readable:
#         'B' 0x42 InjectBrownout (assert NRST, take the BOR reset)
#         'L' 0x4C NRST level  'I' 0x49 assertion time (us)
#         'R' 0x52 release time (us)     'N' 0x4E counters
#         'T' 0x54 emulation time (us)   'S' 0x53 state (NRST level)
#         'X' 0x58 reset (scenario re-arm, pre-flight only)
# (b) Register window at the peripheral base (0x40001000 in
#     cancestry-hw-bor.resc, size 0x400 = one guest page per bring-up finding
#     F-29). The window exposes the same values as the command interface, so
#     the orchestrator and the .resc preflight observe ONE reset state.
# (c) All peripheral state lives in the T2 trace area (plain memory that a
#     machine reset does NOT clear) - a Renode machine reset re-initializes
#     scripted peripherals, so an instance-variable state machine would lose
#     the pending release across exactly the event it models. The window and
#     the command interface are therefore STATELESS views over:
#
#       0x60000070  bor_inject_us        injector: NRST asserted (brownout)
#       0x60000074  nrst_release_us      injector: NRST released (pulse end)
#       0x60000078  bor_detect_us        .resc symbol hook, t2_bor_detect
#       0x6000007C  bor_recover_us       .resc symbol hook,
#                                        cancestry_hardware_set_safe_state
#                                        (the real safe-state restoration)
#       0x60000080  sram_magic_at_boot   firmware: the main-SRAM marker word
#                                        read at the post-BOR boot (0 = the
#                                        reset discarded main SRAM)
#       0x60000084  bor_run_complete     firmware: 1 once the recovery
#                                        completed (no crash, no park)
#       0x60000088  bor_alive_counter    firmware: advances every service-loop
#                                        pass (a stopped counter is visible
#                                        negative crash evidence)
#       0x6000008C  retention_code_at_detect  firmware: the retained code the
#                                        post-reset path recovered through the
#                                        real v1.0.0 retention read API
#       0x60000104  nrst_level           injector: NRST pin level (1 released,
#                                        0 asserted) - STATE word, not an event
#       0x60000108  sram_canary_lost     injector: 1 once the main-SRAM marker
#                                        word was discarded by the reset
#       0x6000010C  injector_error       injector: persistent command status
#                                        (0 accepted; 1 unknown command;
#                                        2 second-injection rejection) - a
#                                        command status must outlive the access
#                                        that produced it, so the orchestrator
#                                        can read it back fail-closed
#
#     One writer per slot (reviewable): the injector owns the two stamps, the
#     level and the SRAM-lost flag; the .resc symbol hooks own the two event
#     stamps; the firmware owns its own slots. The existing H-07 retention
#     slot 0x60000008 (retention_write_us) is stamped by a first-event-guarded
#     .resc hook: the firmware writes the retention register again on the
#     post-reset path (if a later feature drains it), and the scenario's
#     ordering claim is about the FIRST write - the one committed before the
#     brownout.
#
# Determinism (HwAGENTS.md rule 5): every transition is a function of the
# emulation virtual time and of the fixed command the orchestrator issues at a
# fixed master-step boundary. Integer microseconds only; the single float in
# the path (the Renode virtual-time read) is rounded once, exactly as the
# H-07 symbol hooks and the H-10 injector do. At most ONE brownout injection
# is accepted per scenario: a second one would invalidate the exactly-once
# capture contract and is rejected fail-closed.
#
# Peripheral-script API (Renode v1.16.1, pin ci/docker/renode-1.16.1.pin):
# request.IsInit/IsRead/IsWrite/IsUser/Value/Offset and self.NoisyLog; the
# machine is reached through `monitor.Machine` (bring-up finding F-34: the
# PythonPeripheral scope has NO Machine attribute). All logic stays in the
# top-level script body - no nested functions - inside the IronPython 2 scope
# contract, exactly as iwdg_model.py, rcc_model.py and can_fault_injector.py
# do: every value is re-derived from the trace area on each access.
#
# Requirements traced: HW-SF-002, HW-SF-004; HwAGENTS.md rules 1, 2, 5 and 6.
# Tool qualification: `bor_reset_injector`, TCL1 candidate for the bounded
# reset-line role (docs/hw/tool-qualification.md section 3 and section 4.6):
# a defect can at worst fail to produce the scenario events (the runners abort
# fail-closed before evidence) or be caught by the per-access readback, the
# injector error register and the H-11 contract-lockstep tests.

# ---------------------------------------------------------------------------
# Contract constants. The orchestrator mirrors them in
# hw/virtual-bench/t2_bor_common.py and the firmware in
# hw/virtual-bench/firmware/bor_brownout.c; hw/virtual-bench/test_t2_brownout.py
# pins all copies so they cannot diverge silently.
# ---------------------------------------------------------------------------
BOR_INJ_MAGIC = 0x424F5231          # "BOR1"

# Register-window offsets.
BOR_OFF_MAGIC = 0x00
BOR_OFF_COMMAND = 0x04              # write: command; read: last command status
BOR_OFF_NRST_LEVEL = 0x08
BOR_OFF_INJECT_COUNT = 0x0C
BOR_OFF_INJECT_US = 0x10
BOR_OFF_RELEASE_US = 0x14
BOR_OFF_SRAM_LOST = 0x18
BOR_OFF_ERROR = 0x1C
BOR_OFF_TIME_US = 0x20

# Commands (ASCII, shared by the USER command interface and the window).
BOR_CMD_INJECT_BROWNOUT = 0x42      # 'B'
BOR_CMD_NRST_LEVEL = 0x4C           # 'L'
BOR_CMD_INJECT_US = 0x49            # 'I'
BOR_CMD_RELEASE_US = 0x52           # 'R'
BOR_CMD_COUNTERS = 0x4E             # 'N'
BOR_CMD_STATE = 0x53                # 'S'
BOR_CMD_TIME_US = 0x54              # 'T'
BOR_CMD_RESET = 0x58                # 'X'

# NRST pin levels (active-low reset line, issue #64).
NRST_RELEASED = 0x1
NRST_ASSERTED = 0x0

# Fail-closed error codes (window BOR_OFF_ERROR / command status).
BOR_ERR_NONE = 0
BOR_ERR_UNKNOWN_COMMAND = 1
BOR_ERR_ALREADY_INJECTED = 2

# The brownout pulse the issue fixes: NRST is asserted for exactly 100 us.
NRST_PULSE_US = 100

# STM32G4 reset-cause register (RM0440 section 7.4.24; bit positions per the
# ST CMSIS device header stm32g474xx.h: RMVF 23, BORRSTF 27, IWDGRSTF 29).
BOR_RCC_CSR_ADDR = 0x40021094
BOR_RCC_CSR_BORRSTF = 1 << 27

# T2-declared main-SRAM marker word (platform contract, see the .repl header):
# inside the 96 KiB SRAM region (0x20000000 + 0x18000), outside the ELF's
# .data/.bss, > 4 KiB below the initial stack pointer 0x20018000.
BOR_SRAM_CANARY_ADDR = 0x20017000

# T2 trace-area slots owned by THIS peripheral.
BOR_SLOT_INJECT_US = 0x60000070
BOR_SLOT_NRST_RELEASE_US = 0x60000074
BOR_SLOT_NRST_LEVEL = 0x60000104
BOR_SLOT_SRAM_LOST = 0x60000108
BOR_SLOT_ERROR = 0x6000010C


# ---------------------------------------------------------------------------
# Lazy evaluation. A Renode machine reset re-initializes scripted peripherals,
# so no state is kept in the script scope: everything is read from the trace
# area on every access. The release of NRST is evaluated at the FIRST access
# at or after the modelled release instant and stamped with that modelled
# instant (assertion + 100 us) - the pulse is a scenario contract, not a
# poll-loop artifact.
# ---------------------------------------------------------------------------
if not request.IsInit:
    bor_now_us = int(round(monitor.Machine.ElapsedVirtualTime.TimeElapsed.TotalSeconds * 1000000.0))
    bor_sysbus = monitor.Machine['sysbus']

    bor_inject_us = bor_sysbus.ReadDoubleWord(BOR_SLOT_INJECT_US)
    bor_release_us = bor_sysbus.ReadDoubleWord(BOR_SLOT_NRST_RELEASE_US)
    # Persistent command status (0 = accepted): kept in the trace area so it
    # outlives the access that produced it and the orchestrator can read it
    # back over the window whatever the outcome was.
    bor_error = bor_sysbus.ReadDoubleWord(BOR_SLOT_ERROR)

    # --- command acceptance (USER requests and window writes) ---------------
    # A USER request carries the command in request.Value (ControlWrite /
    # ControlRead); a window write carries it only at BOR_OFF_COMMAND. Every
    # other offset is a plain register write and never commands anything.
    bor_command = 0
    if request.IsUser:
        bor_command = int(request.Value) & 0xFFFFFFFF
    elif request.IsWrite and request.Offset == BOR_OFF_COMMAND:
        bor_command = int(request.Value) & 0xFFFFFFFF

    if bor_command != 0:
        if bor_command == BOR_CMD_INJECT_BROWNOUT:
            if bor_inject_us != 0:
                # Fail-closed: exactly one brownout per scenario.
                bor_error = BOR_ERR_ALREADY_INJECTED
                bor_sysbus.WriteDoubleWord(BOR_SLOT_ERROR, bor_error)
                self.NoisyLog("bor_reset_injector: brownout already injected at %d us - second injection rejected (fail-closed)" % bor_inject_us)
            else:
                bor_inject_us = bor_now_us
                # 1) NRST low: the reset line is asserted (active-low).
                bor_sysbus.WriteDoubleWord(BOR_SLOT_NRST_LEVEL, NRST_ASSERTED)
                # 2) Stamp the injection instant (the scenario's BOR assertion).
                bor_sysbus.WriteDoubleWord(BOR_SLOT_INJECT_US, bor_inject_us)
                # 3) Main SRAM content is discarded by a BOR reset; the
                #    retention domain (RTC backup registers) is not.
                bor_sysbus.WriteDoubleWord(BOR_SRAM_CANARY_ADDR, 0)
                bor_sysbus.WriteDoubleWord(BOR_SLOT_SRAM_LOST, 1)
                # 4) Reset cause: set RCC_CSR.BORRSTF through the bus so the
                #    scripted RCC model persists it into its retention shadow
                #    (the same event-style write the IWDG model uses).
                bor_csr = bor_sysbus.ReadDoubleWord(BOR_RCC_CSR_ADDR)
                bor_sysbus.WriteDoubleWord(BOR_RCC_CSR_ADDR, (bor_csr | BOR_RCC_CSR_BORRSTF) & 0xFFFFFFFF)
                self.NoisyLog("bor_reset_injector: brownout injected at %d us - NRST asserted (low), RCC_CSR.BORRSTF set, main-SRAM marker discarded, BOR machine reset requested; NRST release scheduled at %d us (100 us pulse)" % (bor_inject_us, bor_inject_us + NRST_PULSE_US))
                # 5) The reset takes its bounded course: the REAL firmware
                #    re-enters its reset handler and runs its post-reset path.
                monitor.Machine.RequestReset()
        elif bor_command == BOR_CMD_RESET:
            # Scenario re-arm (pre-flight only): clears the injector-owned
            # words. The orchestrator asserts the all-zero preflight state
            # afterwards; the reset is NOT part of a running scenario.
            bor_sysbus.WriteDoubleWord(BOR_SLOT_INJECT_US, 0)
            bor_sysbus.WriteDoubleWord(BOR_SLOT_NRST_RELEASE_US, 0)
            bor_sysbus.WriteDoubleWord(BOR_SLOT_NRST_LEVEL, NRST_RELEASED)
            bor_sysbus.WriteDoubleWord(BOR_SLOT_SRAM_LOST, 0)
            bor_error = BOR_ERR_NONE
            bor_sysbus.WriteDoubleWord(BOR_SLOT_ERROR, bor_error)
            bor_inject_us = 0
            bor_release_us = 0
            self.NoisyLog("bor_reset_injector: re-armed (preflight state)")
        elif bor_command in (BOR_CMD_NRST_LEVEL, BOR_CMD_INJECT_US,
                             BOR_CMD_RELEASE_US, BOR_CMD_COUNTERS,
                             BOR_CMD_STATE, BOR_CMD_TIME_US):
            pass  # readouts: nothing to change
        else:
            bor_error = BOR_ERR_UNKNOWN_COMMAND
            bor_sysbus.WriteDoubleWord(BOR_SLOT_ERROR, bor_error)
            self.NoisyLog("bor_reset_injector: unknown command 0x%x (fail-closed, reset state unchanged)" % bor_command)

    # --- NRST pulse release (lazy, pure function of emulation time) --------
    # Re-read the stamp: the command above may have just set it.
    bor_inject_us = bor_sysbus.ReadDoubleWord(BOR_SLOT_INJECT_US)
    bor_release_us = bor_sysbus.ReadDoubleWord(BOR_SLOT_NRST_RELEASE_US)
    if bor_inject_us != 0 and bor_release_us == 0:
        if bor_now_us >= bor_inject_us + NRST_PULSE_US:
            bor_release_us = bor_inject_us + NRST_PULSE_US
            bor_sysbus.WriteDoubleWord(BOR_SLOT_NRST_LEVEL, NRST_RELEASED)
            bor_sysbus.WriteDoubleWord(BOR_SLOT_NRST_RELEASE_US, bor_release_us)
            self.NoisyLog("bor_reset_injector: NRST released at %d us (%d us after the %d us assertion)" % (bor_release_us, NRST_PULSE_US, bor_inject_us))


# ---------------------------------------------------------------------------
# Readout: computed AFTER the evaluation above, so an access that triggers the
# release already reports the released line (single, consistent state per
# access - the H-10 injector pattern).
# ---------------------------------------------------------------------------
if request.IsUser:
    if request.Value == BOR_CMD_STATE or request.Value == BOR_CMD_NRST_LEVEL:
        request.Value = monitor.Machine['sysbus'].ReadDoubleWord(BOR_SLOT_NRST_LEVEL)
    elif request.Value == BOR_CMD_INJECT_US:
        request.Value = monitor.Machine['sysbus'].ReadDoubleWord(BOR_SLOT_INJECT_US)
    elif request.Value == BOR_CMD_RELEASE_US:
        request.Value = monitor.Machine['sysbus'].ReadDoubleWord(BOR_SLOT_NRST_RELEASE_US)
    elif request.Value == BOR_CMD_COUNTERS:
        bor_inject_count = 1 if monitor.Machine['sysbus'].ReadDoubleWord(BOR_SLOT_INJECT_US) != 0 else 0
        bor_sram_lost = 1 if monitor.Machine['sysbus'].ReadDoubleWord(BOR_SLOT_SRAM_LOST) != 0 else 0
        request.Value = (bor_inject_count << 16) | bor_sram_lost
    elif request.Value == BOR_CMD_TIME_US:
        request.Value = int(round(monitor.Machine.ElapsedVirtualTime.TimeElapsed.TotalSeconds * 1000000.0))
    else:
        # Injection, re-arm and unknown commands report the command status:
        # 0 = accepted, non-zero = the fail-closed error code.
        request.Value = bor_error
elif request.IsRead:
    bor_read_sysbus = monitor.Machine['sysbus']
    if request.Offset == BOR_OFF_MAGIC:
        request.Value = BOR_INJ_MAGIC
    elif request.Offset == BOR_OFF_COMMAND:
        request.Value = bor_error
    elif request.Offset == BOR_OFF_NRST_LEVEL:
        request.Value = bor_read_sysbus.ReadDoubleWord(BOR_SLOT_NRST_LEVEL)
    elif request.Offset == BOR_OFF_INJECT_COUNT:
        request.Value = 1 if bor_read_sysbus.ReadDoubleWord(BOR_SLOT_INJECT_US) != 0 else 0
    elif request.Offset == BOR_OFF_INJECT_US:
        request.Value = bor_read_sysbus.ReadDoubleWord(BOR_SLOT_INJECT_US)
    elif request.Offset == BOR_OFF_RELEASE_US:
        request.Value = bor_read_sysbus.ReadDoubleWord(BOR_SLOT_NRST_RELEASE_US)
    elif request.Offset == BOR_OFF_SRAM_LOST:
        request.Value = bor_read_sysbus.ReadDoubleWord(BOR_SLOT_SRAM_LOST)
    elif request.Offset == BOR_OFF_ERROR:
        request.Value = bor_error
    elif request.Offset == BOR_OFF_TIME_US:
        request.Value = int(round(monitor.Machine.ElapsedVirtualTime.TimeElapsed.TotalSeconds * 1000000.0))
    else:
        request.Value = 0
