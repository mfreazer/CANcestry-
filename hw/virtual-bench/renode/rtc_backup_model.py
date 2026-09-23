# Scripted RTC backup-domain model for the CANcestry T2 virtual bench
# (H-07, issue #53).
#
# Implements: the RTC_BKP0R retention register (offset 0x50 of the RTC base
# 0x40002800) written by platform/cortex_m/watchdog.c:
#
#   cancestry_watchdog_write_retention_register: RTC_BKP0R = fault code
#   cancestry_watchdog_read_retention_register:  return RTC_BKP0R
#
# HW-SF-002 requires the retained fault code (0x45565101 class) to survive an
# IWDG reset. Machine resets re-initialize scripted peripherals, so the
# canonical value lives in the T2 trace area (0x60000100), a plain mapped
# memory that is NOT cleared by a Renode machine reset. The runner asserts
# the shadow still reads back after the scripted reset (that assertion is the
# HW-SF-002 (i) check, see hw/virtual-bench/run_t2_retention.py).
#
# Requirements traced: HW-SF-002; HwAGENTS.md rules 2 and 5.
# F-34: self.Machine -> monitor.Machine (PythonPeripheral scope carries
# no Machine attribute - see iwdg_model.py header; dispatch 23).
# initable is false: no reset-time reinitialization of the retained value.

RETENTION_SHADOW_ADDR = 0x60000100

if request.IsWrite:
    if request.Offset == 0x50:
        monitor.Machine['sysbus'].WriteDoubleWord(RETENTION_SHADOW_ADDR,
                                               request.Value & 0xFFFFFFFF)
        self.NoisyLog("RTC_BKP0R <- 0x%08x" % (request.Value & 0xFFFFFFFF))
elif request.IsRead:
    if request.Offset == 0x50:
        request.Value = monitor.Machine['sysbus'].ReadDoubleWord(RETENTION_SHADOW_ADDR)
    else:
        # Only RTC_BKP0R is modeled; other RTC/BKP registers read as 0
        # (not_simulated, virtual-bench-plan section 3).
        request.Value = 0x0
