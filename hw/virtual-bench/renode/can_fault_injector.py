# CAN bus fault injector for the CANcestry T2 virtual bench (H-10, issue #62).
#
# Implements: HW-FR-003 (each CAN interface shall meet ISO 11898-2:2016; the
# Bus-Off recovery sequence and the CRC error handling of the gateway node are
# exercised on the virtual bench). It exercises the REAL v1.0.0 fault path
# (cancestry_hal_raise_fault -> CANCESTRY_HAL_IF_STATE_BUS_OFF plus the
# FAULT_RAISED event) without modifying it: this file is a Renode platform
# peripheral, not firmware (issue #62 constraint 1).
#
# ---------------------------------------------------------------------------
# What this peripheral models - and what it deliberately does not
# ---------------------------------------------------------------------------
#
# MODELED (shared-medium fault state):
#   * The CAN bus as ONE shared medium. `InjectBusOff` puts the medium into
#     the Bus-Off condition and that condition is projected into EVERY
#     controller window of the platform (the FDCAN1 and FDCAN2 register
#     scratch of stm32g474-cancestry.repl): a Bus-Off on the gateway is a bus
#     state, so all nodes see the same PSR.BO. That is the shared-medium
#     property the issue asks for, expressed as bus state rather than as
#     bit-level arbitration.
#   * The ISO 11898-1 Bus-Off recovery sequence: the controller enters INIT on
#     Bus-Off (RM0440: the FDCAN controller enters INIT state when detecting
#     bus-off), software clears CCCR.INIT to request recovery, and the medium
#     releases only after 128 occurrences of 11 consecutive recessive bits at
#     the nominal bitrate. At the HW-FR-003 nominal 2 Mbit/s that interval is
#     exactly 128 * 11 / 2e6 s = 704 us of emulation virtual time (integer
#     microseconds; no float scheduling).
#   * The CRC error condition of one frame: PSR.LEC = 0b010 (CRC error, the
#     ISO 11898-1 / RM0440 LEC encoding), the error counters (ECR.REC / ECR.CEL)
#     and the error-logging interrupt flag (IR.ELO) - i.e. exactly the
#     controller registers a target run has to capture per
#     docs/qa/hil-fault-injection-report.md section 4.2 and section 5.
#
# NOT MODELED (documented; no claim is derived from any of these):
#   * Bit-level CAN protocol, arbitration, bit stuffing and the physical layer
#     stay `not_simulated` (stm32g474-cancestry.repl header). The H-07 scope
#     is unchanged: this peripheral is an EXTENSION of the platform, no
#     existing peripheral is modified (issue #62 constraint 4).
#   * Brownout / BOR physics are not touched here at all (issue #62
#     constraint 2: deferred to H-11; HwAGENTS.md rule 6).
#   * Frame delivery: the injector corrupts the CRC condition of the medium,
#     it does not synthesize RX FIFO traffic.
#
# ---------------------------------------------------------------------------
# Interfaces
# ---------------------------------------------------------------------------
#
# (a) Command interface for the orchestrator (issue #62 deliverable 1). It is
#     a USER request in the Renode PythonPeripheral sense: PythonPeripheral
#     exposes ControlWrite(long command, ulong value) / ControlRead(long
#     command, ulong value) (src/Emulator/Main/Peripherals/Python/
#     PythonPeripheral.cs, v1.16.1) and the monitor reaches them as
#
#         sysbus.can_fault_injector ControlWrite 0x42 0x1   # InjectBusOff
#         sysbus.can_fault_injector ControlWrite 0x43 0x1   # InjectCRCError
#         sysbus.can_fault_injector ControlRead  0x53 0x0   # bus state
#
#     The device path is ONE dotted token: a bare 'sysbus' resolves to the
#     SystemBus object, so the two-token form 'sysbus can_fault_injector
#     ...' fails with a recoverable error and this module never sees the
#     command (dispatch 17: every watch slot read 0). fmi_bridge.command()
#     fails closed on the monitor's error marker.
#
#     Command codes are ASCII so the monitor transcript stays readable:
#         'B' 0x42 InjectBusOff        'C' 0x43 InjectCRCError
#         'S' 0x53 bus state           'O' 0x4F bus-off injection time (us)
#         'E' 0x45 CRC injection time  'R' 0x52 recovery-request time
#         'A' 0x41 bus-release time    'N' 0x4E injection counters
#         'T' 0x54 emulation time      'X' 0x58 reset (scenario re-arm)
# (b) Register window at the peripheral base (0x40000000 in
#     cancestry-hw-fault.resc, size 0x400 = one guest page per bring-up
#     finding F-29). The window exposes the same commands and the same
#     readouts, so the firmware side and the orchestrator side observe ONE
#     medium state and can never disagree.
# (c) One bus write hook, installed by cancestry-hw-fault.resc on the
#     UNCHANGED fdcan1_scratch memory, carries the firmware's CCCR.INIT
#     transition into the trace-area slot RECOVERY_REQUEST_US. The injector
#     never writes that slot and the hook never writes a projection: one
#     writer per slot keeps every transition deterministic.
#
# T2 fault-evidence trace slots (the H-10 extension of the H-07
# event-capture contract in stm32g474-cancestry.repl). All values are integer
# microseconds of emulation virtual time; 0 means "did not occur".
#
#   0x60000040  bus_off_inject_us     injector: InjectBusOff accepted
#   0x60000044  crc_inject_us         injector: InjectCRCError accepted
#   0x60000048  recovery_request_us   .resc CCCR write hook (firmware cleared
#                                     CCCR.INIT while the medium was Bus-Off)
#   0x6000004C  bus_active_us         injector: medium released after the
#                                     128 * 11 recessive-bit sequence
#   0x60000050  crc_cleared_us        injector: medium released the CRC
#                                     condition after the acknowledge below
#   0x60000054  crc_ack_us            .resc IR write hook (firmware wrote
#                                     FDCAN_IR with IR.ELO cleared, the
#                                     write-1-to-clear acknowledge)
#   0x60000058  busoff_detect_us      .resc symbol hook, t2_can_busoff_detect
#                                     (the real v1.0.0 fault path is entered)
#   0x6000005C  busoff_recover_us     .resc symbol hook, t2_can_busoff_recover
#   0x60000060  crc_detect_us         .resc symbol hook, t2_can_crc_detect
#   0x60000064  crc_error_count       firmware CRC error counter (driver)
#   0x60000068  run_complete          firmware: 1 when the scenario loop ended
#                                     normally (no crash, no park)
#   0x6000006C  alive_counter         firmware: increments every poll pass, so
#                                     a hung firmware is visible as a stopped
#                                     counter (negative crash evidence)
#   0x60000070  hook_liveness         cancestry-hw-fault.resc write-hook
#                                     self-test only (dispatch 19); the
#                                     include clears it to 0 before the
#                                     machine is left paused. This
#                                     peripheral never writes it.
#
# Determinism (HwAGENTS.md rule 5): every transition is a function of the
# emulation virtual time and of the fixed command sequence the orchestrator
# issues at fixed master-step boundaries. No wall clock, no randomness, no
# float scheduling: times are integer microseconds, and the single float in
# the path (the Renode virtual-time read) is rounded once, exactly as the
# H-07 symbol hooks do.
#
# Peripheral-script API (Renode v1.16.1, pin ci/docker/renode-1.16.1.pin):
# request.IsInit/IsRead/IsWrite/IsUser/Value/Offset and self.NoisyLog
# (PeripheralPythonEngine.InitScope); the machine is reached through
# `monitor.Machine` (bring-up finding F-34: the PythonPeripheral scope has NO
# Machine attribute). All logic stays in the top-level script body - no nested
# functions - inside the IronPython 2 scope contract, exactly as iwdg_model.py
# and rcc_model.py do.
#
# Requirements traced: HW-FR-003; HwAGENTS.md rules 1, 2 and 5.

# ---------------------------------------------------------------------------
# Contract constants. The orchestrator mirrors them in
# hw/virtual-bench/t2_fault_common.py and hw/virtual-bench/firmware/
# t2_can_fault.h; hw/virtual-bench/test_t2_busoff.py pins all three copies so
# they cannot diverge silently.
# ---------------------------------------------------------------------------
INJ_MAGIC = 0x43464931          # "CFI1"

# Register-window offsets.
INJ_OFF_MAGIC = 0x00
INJ_OFF_COMMAND = 0x04          # write: command; read: last command status
INJ_OFF_BUSOFF_COUNT = 0x08
INJ_OFF_CRC_COUNT = 0x0C
INJ_OFF_BUS_STATE = 0x10
INJ_OFF_BUSOFF_INJECT_US = 0x14
INJ_OFF_CRC_INJECT_US = 0x18
INJ_OFF_RECOVERY_REQUEST_US = 0x1C
INJ_OFF_BUS_ACTIVE_US = 0x20
INJ_OFF_CRC_CLEARED_US = 0x24
INJ_OFF_TIME_US = 0x28
INJ_OFF_ERROR = 0x2C

# Commands (ASCII, shared by the USER command interface and the window).
CMD_INJECT_BUSOFF = 0x42        # 'B'
CMD_INJECT_CRC = 0x43           # 'C'
CMD_STATE = 0x53                # 'S'
CMD_BUSOFF_AT_US = 0x4F         # 'O'
CMD_CRC_AT_US = 0x45            # 'E'
CMD_RECOVERY_REQUEST_US = 0x52  # 'R'
CMD_BUS_ACTIVE_US = 0x41        # 'A'
CMD_COUNTERS = 0x4E             # 'N'
CMD_TIME_US = 0x54              # 'T'
CMD_RESET = 0x58                # 'X'

# Shared-medium state encoding (window + CMD_STATE).
STATE_BUS_ACTIVE = 0x0
STATE_BUS_OFF = 0x1
STATE_CRC_ERROR = 0x2

# Fail-closed error codes (window INJ_OFF_ERROR / command status).
ERR_NONE = 0
ERR_UNKNOWN_COMMAND = 1

# ISO 11898-1 Bus-Off recovery: 128 occurrences of 11 consecutive recessive
# bits at the HW-FR-003 nominal bitrate of 2 Mbit/s -> exactly 704 us.
BUSOFF_RECOVERY_SEQUENCES = 128
BUSOFF_RECOVERY_BITS = 11
NOMINAL_BITRATE_MBPS = 2
BUSOFF_RECOVERY_US = (BUSOFF_RECOVERY_SEQUENCES * BUSOFF_RECOVERY_BITS
                      * 1000000) / (NOMINAL_BITRATE_MBPS * 1000000)

# STM32G4 FDCAN register surface (RM0440 section 43.4; offsets and bit
# positions taken from the ST CMSIS device header stm32g474xx.h: CCCR 0x018,
# ECR 0x040, PSR 0x044, IR 0x050).
FDCAN1_BASE = 0x40006400
FDCAN2_BASE = 0x40006800
FDCAN_OFF_CCCR = 0x018
FDCAN_OFF_ECR = 0x040
FDCAN_OFF_PSR = 0x044
FDCAN_OFF_IR = 0x050
CCCR_INIT = 1 << 0
PSR_LEC_MASK = 0x7
PSR_ACT_MASK = 0x3 << 3
PSR_ACT_SYNCHRONIZING = 0x1 << 3
PSR_EP = 1 << 5
PSR_EW = 1 << 6
PSR_BO = 1 << 7
ECR_REC_ONE = 1 << 8
ECR_RP = 1 << 15
ECR_CEL_ONE = 1 << 16
IR_ELO = 1 << 16
LEC_NO_ERROR = 0
LEC_CRC_ERROR = 2

# Trace-area slots written by THIS peripheral.
SLOT_BUSOFF_INJECT_US = 0x60000040
SLOT_CRC_INJECT_US = 0x60000044
SLOT_BUS_ACTIVE_US = 0x6000004C
SLOT_CRC_CLEARED_US = 0x60000050
# Trace-area slots written by the .resc hooks and only READ here.
SLOT_RECOVERY_REQUEST_US = 0x60000048
SLOT_CRC_ACK_US = 0x60000054

# ---------------------------------------------------------------------------
# Request dispatch. The command carried by this access (USER command offset or
# window write value) is normalized into inj_command so the shared-medium
# evaluation below is ONE code path for both interfaces.
# ---------------------------------------------------------------------------
if request.IsInit:
    # Init is the only place the medium state is created; a machine reset
    # re-runs it, so a re-armed scenario always starts from a clean medium.
    inj_bus_off = False
    inj_crc_pending = False
    inj_busoff_count = 0
    inj_crc_count = 0
    inj_busoff_at_us = 0
    inj_crc_at_us = 0
    inj_recovery_request_us = 0
    inj_bus_active_us = 0
    inj_crc_cleared_us = 0
    inj_crc_ack_us = 0
    inj_error = ERR_NONE
    # Register mirrors of the projected FDCAN words. The injector is the ONLY
    # writer of the projections, so a mirror never races with the firmware's
    # own CCCR write (that one is observed by the .resc hook, not mirrored
    # back). Reset state: controller in INIT, medium active, no error.
    inj_cccr = CCCR_INIT
    inj_psr = PSR_ACT_SYNCHRONIZING | LEC_NO_ERROR
    inj_ecr = 0
    inj_ir = 0
    inj_command = 0
    request.Value = 0
elif request.IsUser:
    inj_command = request.Offset
    request.Value = 0
elif request.IsWrite:
    if request.Offset == INJ_OFF_COMMAND:
        inj_command = request.Value
    else:
        # Every other window offset is a read-only status register: an
        # unexpected write never changes the medium state silently.
        inj_command = 0
else:
    inj_command = 0

# ---------------------------------------------------------------------------
# Command execution + shared-medium evaluation. Lazy, on EVERY access after
# init (the iwdg_model.py expiry pattern): the firmware polls the window and
# the FDCAN projections, and the orchestrator polls the trace slots, so a
# transition that became due is applied by whichever access observes it first.
# A command issued by THIS access is applied by THIS access, so the injection
# timestamp and the projected registers are already consistent for the very
# next read.
# ---------------------------------------------------------------------------
if not request.IsInit:
    inj_now_us = int(round(monitor.Machine.ElapsedVirtualTime.TimeElapsed.TotalSeconds * 1000000.0))
    inj_sysbus = monitor.Machine['sysbus']

    if inj_command == CMD_RESET:
        inj_bus_off = False
        inj_crc_pending = False
        inj_busoff_count = 0
        inj_crc_count = 0
        inj_busoff_at_us = 0
        inj_crc_at_us = 0
        inj_recovery_request_us = 0
        inj_bus_active_us = 0
        inj_crc_cleared_us = 0
        inj_crc_ack_us = 0
        inj_error = ERR_NONE
        inj_cccr = CCCR_INIT
        inj_psr = PSR_ACT_SYNCHRONIZING | LEC_NO_ERROR
        inj_ecr = 0
        inj_ir = 0
        inj_sysbus.WriteDoubleWord(SLOT_BUSOFF_INJECT_US, 0)
        inj_sysbus.WriteDoubleWord(SLOT_CRC_INJECT_US, 0)
        inj_sysbus.WriteDoubleWord(SLOT_BUS_ACTIVE_US, 0)
        inj_sysbus.WriteDoubleWord(SLOT_CRC_CLEARED_US, 0)
        inj_sysbus.WriteDoubleWord(SLOT_RECOVERY_REQUEST_US, 0)
        inj_sysbus.WriteDoubleWord(SLOT_CRC_ACK_US, 0)
        inj_sysbus.WriteDoubleWord(FDCAN1_BASE + FDCAN_OFF_CCCR, inj_cccr)
        inj_sysbus.WriteDoubleWord(FDCAN1_BASE + FDCAN_OFF_PSR, inj_psr)
        inj_sysbus.WriteDoubleWord(FDCAN1_BASE + FDCAN_OFF_ECR, inj_ecr)
        inj_sysbus.WriteDoubleWord(FDCAN1_BASE + FDCAN_OFF_IR, inj_ir)
        inj_sysbus.WriteDoubleWord(FDCAN2_BASE + FDCAN_OFF_PSR, inj_psr)
    elif inj_command == CMD_INJECT_BUSOFF:
        if not inj_bus_off:
            # Shared medium: ONE Bus-Off condition, visible to every node.
            inj_bus_off = True
            inj_busoff_count = inj_busoff_count + 1
            inj_busoff_at_us = inj_now_us
            inj_recovery_request_us = 0
            inj_bus_active_us = 0
            inj_sysbus.WriteDoubleWord(SLOT_BUSOFF_INJECT_US, inj_busoff_at_us)
            inj_sysbus.WriteDoubleWord(SLOT_BUS_ACTIVE_US, 0)
            # Controller enters INIT on Bus-Off (RM0440); TEC exceeded 255 so
            # the node is error-passive and Bus-Off.
            inj_cccr = inj_cccr | CCCR_INIT
            inj_psr = (inj_psr & ~PSR_LEC_MASK & ~PSR_ACT_MASK) | PSR_BO | PSR_EP | PSR_ACT_SYNCHRONIZING
            inj_ecr = (inj_ecr & ~ECR_REC_ONE) | ECR_RP
            inj_sysbus.WriteDoubleWord(FDCAN1_BASE + FDCAN_OFF_CCCR, inj_cccr)
            inj_sysbus.WriteDoubleWord(FDCAN1_BASE + FDCAN_OFF_PSR, inj_psr)
            inj_sysbus.WriteDoubleWord(FDCAN1_BASE + FDCAN_OFF_ECR, inj_ecr)
            # Every node sees the same medium: project BO to FDCAN2 as well.
            inj_sysbus.WriteDoubleWord(FDCAN2_BASE + FDCAN_OFF_PSR, inj_psr)
            self.NoisyLog("can_fault_injector: Bus-Off injected at %d us (shared medium: FDCAN1+FDCAN2 PSR.BO set, CCCR.INIT set)" % inj_busoff_at_us)
    elif inj_command == CMD_INJECT_CRC:
        if not inj_crc_pending:
            inj_crc_pending = True
            inj_crc_count = inj_crc_count + 1
            inj_crc_at_us = inj_now_us
            inj_crc_cleared_us = 0
            inj_sysbus.WriteDoubleWord(SLOT_CRC_INJECT_US, inj_crc_at_us)
            inj_sysbus.WriteDoubleWord(SLOT_CRC_CLEARED_US, 0)
            # One frame with a corrupted CRC: LEC = CRC error, REC and the
            # CAN error-logging counter advance, IR.ELO is raised.
            inj_psr = (inj_psr & ~PSR_LEC_MASK) | LEC_CRC_ERROR
            inj_ecr = inj_ecr + ECR_REC_ONE + ECR_CEL_ONE
            inj_ir = inj_ir | IR_ELO
            inj_sysbus.WriteDoubleWord(FDCAN1_BASE + FDCAN_OFF_PSR, inj_psr)
            inj_sysbus.WriteDoubleWord(FDCAN1_BASE + FDCAN_OFF_ECR, inj_ecr)
            inj_sysbus.WriteDoubleWord(FDCAN1_BASE + FDCAN_OFF_IR, inj_ir)
            inj_sysbus.WriteDoubleWord(FDCAN2_BASE + FDCAN_OFF_PSR, inj_psr)
            self.NoisyLog("can_fault_injector: CRC error injected at %d us (PSR.LEC=%d, ECR.REC/CEL advanced, IR.ELO set)" % (inj_crc_at_us, LEC_CRC_ERROR))
    elif inj_command == 0:
        pass
    elif not (inj_command == CMD_STATE or inj_command == CMD_BUSOFF_AT_US
              or inj_command == CMD_CRC_AT_US
              or inj_command == CMD_RECOVERY_REQUEST_US
              or inj_command == CMD_BUS_ACTIVE_US
              or inj_command == CMD_COUNTERS or inj_command == CMD_TIME_US):
        # Fail-closed: an unknown command is reported, never silently ignored.
        inj_error = ERR_UNKNOWN_COMMAND
        self.NoisyLog("can_fault_injector: unknown command 0x%x (fail-closed, medium unchanged)" % inj_command)

    # ISO 11898-1 recovery sequence. The firmware requested recovery by
    # clearing CCCR.INIT (observed by the .resc write hook into the trace
    # slot); the medium releases exactly BUSOFF_RECOVERY_US later and not
    # before, so the measured detection-to-release interval IS the modeled
    # standard recovery sequence rather than an artifact of a poll loop.
    if inj_bus_off and inj_recovery_request_us == 0:
        inj_recovery_request_us = inj_sysbus.ReadDoubleWord(SLOT_RECOVERY_REQUEST_US)
    if inj_bus_off and inj_recovery_request_us != 0:
        if inj_now_us >= inj_recovery_request_us + BUSOFF_RECOVERY_US:
            inj_bus_off = False
            inj_bus_active_us = inj_recovery_request_us + BUSOFF_RECOVERY_US
            inj_sysbus.WriteDoubleWord(SLOT_BUS_ACTIVE_US, inj_bus_active_us)
            inj_cccr = inj_cccr & ~CCCR_INIT
            inj_psr = (inj_psr & ~PSR_BO & ~PSR_EP & ~PSR_ACT_MASK) | LEC_NO_ERROR
            inj_ecr = inj_ecr & ~ECR_RP
            inj_sysbus.WriteDoubleWord(FDCAN1_BASE + FDCAN_OFF_CCCR, inj_cccr)
            inj_sysbus.WriteDoubleWord(FDCAN1_BASE + FDCAN_OFF_PSR, inj_psr)
            inj_sysbus.WriteDoubleWord(FDCAN1_BASE + FDCAN_OFF_ECR, inj_ecr)
            inj_sysbus.WriteDoubleWord(FDCAN2_BASE + FDCAN_OFF_PSR, inj_psr)
            self.NoisyLog("can_fault_injector: Bus-Off released at %d us (recovery requested at %d us + %d us = 128 x 11 recessive bits at %d Mbit/s)" % (inj_bus_active_us, inj_recovery_request_us, BUSOFF_RECOVERY_US, NOMINAL_BITRATE_MBPS))

    # CRC error handling. The firmware acknowledges the error-logging
    # interrupt by writing FDCAN_IR (write-1-to-clear on real silicon); the
    # .resc IR write hook stamps that access into SLOT_CRC_ACK_US and this
    # peripheral - the medium - releases the CRC condition from that moment.
    # The injector never reads the firmware's counters and the hook never
    # writes a projection: one writer per slot.
    if inj_crc_pending and inj_crc_ack_us == 0:
        inj_crc_ack_us = inj_sysbus.ReadDoubleWord(SLOT_CRC_ACK_US)
    if inj_crc_pending and inj_crc_ack_us != 0:
        inj_crc_pending = False
        inj_crc_cleared_us = inj_crc_ack_us
        inj_sysbus.WriteDoubleWord(SLOT_CRC_CLEARED_US, inj_crc_cleared_us)
        inj_psr = (inj_psr & ~PSR_LEC_MASK) | LEC_NO_ERROR
        inj_ir = inj_ir & ~IR_ELO
        inj_sysbus.WriteDoubleWord(FDCAN1_BASE + FDCAN_OFF_PSR, inj_psr)
        inj_sysbus.WriteDoubleWord(FDCAN1_BASE + FDCAN_OFF_IR, inj_ir)
        inj_sysbus.WriteDoubleWord(FDCAN2_BASE + FDCAN_OFF_PSR, inj_psr)
        self.NoisyLog("can_fault_injector: CRC error cleared at %d us (firmware acknowledged IR.ELO)" % inj_crc_cleared_us)

# ---------------------------------------------------------------------------
# Readout: computed AFTER the evaluation above so a read that triggers the
# release reports the released medium (single, consistent state per access).
# ---------------------------------------------------------------------------
if request.IsUser:
    if inj_command == CMD_STATE:
        if inj_bus_off:
            request.Value = STATE_BUS_OFF
        elif inj_crc_pending:
            request.Value = STATE_CRC_ERROR
        else:
            request.Value = STATE_BUS_ACTIVE
    elif inj_command == CMD_BUSOFF_AT_US:
        request.Value = inj_busoff_at_us
    elif inj_command == CMD_CRC_AT_US:
        request.Value = inj_crc_at_us
    elif inj_command == CMD_RECOVERY_REQUEST_US:
        request.Value = inj_recovery_request_us
    elif inj_command == CMD_BUS_ACTIVE_US:
        request.Value = inj_bus_active_us
    elif inj_command == CMD_COUNTERS:
        request.Value = (inj_busoff_count << 16) | inj_crc_count
    elif inj_command == CMD_TIME_US:
        request.Value = inj_now_us
    else:
        # Injection, reset and unknown commands all report the command status:
        # 0 = accepted, non-zero = the fail-closed error code.
        request.Value = inj_error
elif request.IsRead:
    inj_read_now_us = int(round(monitor.Machine.ElapsedVirtualTime.TimeElapsed.TotalSeconds * 1000000.0))
    if request.Offset == INJ_OFF_MAGIC:
        request.Value = INJ_MAGIC
    elif request.Offset == INJ_OFF_COMMAND:
        request.Value = inj_error
    elif request.Offset == INJ_OFF_BUSOFF_COUNT:
        request.Value = inj_busoff_count
    elif request.Offset == INJ_OFF_CRC_COUNT:
        request.Value = inj_crc_count
    elif request.Offset == INJ_OFF_BUS_STATE:
        if inj_bus_off:
            request.Value = STATE_BUS_OFF
        elif inj_crc_pending:
            request.Value = STATE_CRC_ERROR
        else:
            request.Value = STATE_BUS_ACTIVE
    elif request.Offset == INJ_OFF_BUSOFF_INJECT_US:
        request.Value = inj_busoff_at_us
    elif request.Offset == INJ_OFF_CRC_INJECT_US:
        request.Value = inj_crc_at_us
    elif request.Offset == INJ_OFF_RECOVERY_REQUEST_US:
        request.Value = inj_recovery_request_us
    elif request.Offset == INJ_OFF_BUS_ACTIVE_US:
        request.Value = inj_bus_active_us
    elif request.Offset == INJ_OFF_CRC_CLEARED_US:
        request.Value = inj_crc_cleared_us
    elif request.Offset == INJ_OFF_TIME_US:
        request.Value = inj_read_now_us
    elif request.Offset == INJ_OFF_ERROR:
        request.Value = inj_error
    else:
        request.Value = 0
