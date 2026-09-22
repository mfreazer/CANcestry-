# Scripted RCC reset-cause model for the CANcestry T2 virtual bench
# (H-07, issue #53; H-08 bring-up fix).
#
# Implements: the RCC_CSR register surface used by platform/cortex_m/watchdog.c:
#
#   cancestry_watchdog_init:  reads RCC_CSR.IWDGRSTF (bit 29) to classify the
#                             reset cause; writes RCC_CSR.RMVF (bit 23) to clear
#                             the reset flags.
#
# The IWDG model sets bit 29 from its expiry path (see iwdg_model.py); RMVF
# writes clear it. All other RCC registers read as 0 (not_simulated,
# virtual-bench-plan section 3).
#
# Reset-cause persistence across machine reset (H-08 bring-up finding F-5):
# on real STM32G4 the RCC reset-cause flags live in the retention domain and
# persist across an IWDG reset until RMVF is written. Machine resets
# re-initialize scripted peripherals, so the canonical CSR value lives in the
# T2 trace area (0x60000024), a plain mapped memory that is NOT cleared by a
# Renode machine reset - the same pattern the RTC_BKP0R model uses for its
# retention shadow. The model:
#
#   - restores the CSR lazily from the shadow on first access after each
#     re-initialization (lazy so the model does not depend on peripheral
#     creation order),
#   - echoes every write into the shadow: the IWDG model sets IWDGRSTF by
#     writing the post-event CSR value through the bus, and that write must
#     be honored, not only the model's own register state,
#   - treats RMVF as write-1-to-clear: an RMVF write clears IWDGRSTF and the
#     RMVF bit itself never persists (it reads back as 0).
#
# All register logic is kept in the top-level script body (no nested
# functions) to stay within the plain Renode PythonPeripheral scripting
# contract (IronPython 2 scope semantics).
#
# Requirements traced: HW-SF-002, HW-SF-003; HwAGENTS.md rules 2 and 5.

IWDGRSTF = 1 << 29
RMVF = 1 << 23
CSR_OFFSET = 0x94
SHADOW_ADDR = 0x60000024

if request.IsInit:
    rcc_csr = None  # restored lazily from the shadow on first access
elif request.IsWrite:
    if request.Offset == CSR_OFFSET:
        value = request.Value & 0xFFFFFFFF
        if rcc_csr is None:
            rcc_csr = self.Machine['sysbus'].ReadDoubleWord(SHADOW_ADDR)
        if (value & RMVF) != 0:
            # Write-1-to-clear: reset-cause flags, then drop RMVF itself.
            rcc_csr = rcc_csr & ~IWDGRSTF & 0xFFFFFFFF
        else:
            # Event-style write (IWDG expiry carries the post-event value);
            # persist everything except the W1C RMVF bit.
            rcc_csr = value & ~RMVF & 0xFFFFFFFF
        self.Machine['sysbus'].WriteDoubleWord(SHADOW_ADDR, rcc_csr)
elif request.IsRead:
    if request.Offset == CSR_OFFSET:
        if rcc_csr is None:
            rcc_csr = self.Machine['sysbus'].ReadDoubleWord(SHADOW_ADDR)
        request.Value = rcc_csr
    else:
        request.Value = 0x0
