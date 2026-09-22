# Scripted IWDG model for the CANcestry T2 virtual bench (H-07, issue #53).
#
# Implements: HW-SF-002 sub-event (i) and HW-SF-003's secondary IWDG layer as
# exercised by platform/cortex_m/watchdog.c at v1.0.0 (read-only firmware):
#
#   cancestry_watchdog_init:    KR=0x5555 (unlock), PR=3 (/32 -> 1 kHz tick),
#                               RLR=timeout_ms, KR=0xAAAA (reload), KR=0xCCCC (start)
#   cancestry_watchdog_feed:    KR=0xAAAA
#   cancestry_watchdog_escalate: KR=0x5555, RLR=1, KR=0xAAAA
#
# On expiry the model sets RCC_CSR.IWDGRSTF (bit 29) and requests a machine
# reset, writing the fire timestamp into the T2 trace area (0x60000014).
#
# Requirements traced: HW-SF-002, HW-SF-003; HwAGENTS.md rules 2, 5 and 6.
# Script API: Renode Python.PythonPeripheral (request.IsInit/IsRead/IsWrite/
# Value/Offset, self.NoisyLog); the peripheral evaluates expiry lazily on bus
# access - the FMI bridge polls after every 100 us master step, so the
# detection quantization is the master step while the recorded timestamp is
# the exact emulation virtual time of the detecting access.

if request.IsInit:
    iwdg_unlocked = False
    iwdg_pr = 0
    iwdg_rlr = 0xFFF
    iwdg_running = False
    iwdg_started_at = 0.0
    iwdg_fired = False
elif request.IsWrite:
    if request.Offset == 0x0:
        if request.Value == 0x5555:
            iwdg_unlocked = True
        elif request.Value == 0xAAAA:
            # Reload: restart the countdown window.
            iwdg_started_at = self.Machine.ElapsedVirtualTime.TimeElapsed.TotalSeconds
        elif request.Value == 0xCCCC:
            # Start: watchdog becomes live from now.
            iwdg_running = True
            iwdg_started_at = self.Machine.ElapsedVirtualTime.TimeElapsed.TotalSeconds
            iwdg_unlocked = False
        else:
            self.NoisyLog("IWDG_KR ignored write 0x%x" % request.Value)
    elif request.Offset == 0x4:
        if iwdg_unlocked:
            iwdg_pr = request.Value & 0x7
    elif request.Offset == 0x8:
        if iwdg_unlocked:
            iwdg_rlr = request.Value & 0xFFF
    elif request.Offset == 0x10:
        pass  # WINR: windowing not_simulated (watchdog.c never sets it)
elif request.IsRead:
    if iwdg_running and not iwdg_fired:
        now = self.Machine.ElapsedVirtualTime.TimeElapsed.TotalSeconds
        # LSI 32 kHz (OR-007); STM32G4 IWDG prescaler (RM0440 table):
        # division = 2^(PR+2) = 4*2^PR, so PR=0 -> /4 ... PR=3 -> /32
        # (1 kHz tick) ... PR=7 -> /512. The v1.0.0 firmware arms with
        # PR=0x03, i.e. 1 ms ticks.
        tick_s = float(4 * (2 ** iwdg_pr)) / 32000.0
        timeout_s = (iwdg_rlr + 1) * tick_s
        if (now - iwdg_started_at) >= timeout_s:
            iwdg_fired = True
            fire_us = int(round(now * 1000000.0))
            sysbus = self.Machine['sysbus']
            # Record the fire time in the T2 trace area.
            sysbus.WriteDoubleWord(0x60000014, fire_us)
            # Set IWDGRSTF (bit 29) in RCC_CSR at 0x40021094.
            csr = sysbus.ReadDoubleWord(0x40021094)
            sysbus.WriteDoubleWord(0x40021094, csr | (1 << 29))
            self.NoisyLog("IWDG reset fired at %d us (RLR=%d PR=%d)"
                          % (fire_us, iwdg_rlr, iwdg_pr))
            # HW-SF-002 (i): the IWDG reset takes its bounded course.
            self.Machine.RequestReset()
    if request.Offset == 0x0:
        request.Value = 0x0  # KR reads as 0
    elif request.Offset == 0x4:
        request.Value = iwdg_pr
    elif request.Offset == 0x8:
        request.Value = iwdg_rlr
    elif request.Offset == 0xC:
        # SR: PVU/RVU clear (updates complete synchronously in this model).
        request.Value = 0x0
    elif request.Offset == 0x10:
        request.Value = 0xFFF  # WINR reset value
