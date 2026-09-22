# Scripted RCC reset-cause model for the CANcestry T2 virtual bench
# (H-07, issue #53).
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
# Requirements traced: HW-SF-002, HW-SF-003; HwAGENTS.md rules 2 and 5.

IWDGRSTF = 1 << 29
RMVF = 1 << 23
CSR_OFFSET = 0x94

if request.IsInit:
    rcc_csr = 0x0
elif request.IsWrite:
    if request.Offset == CSR_OFFSET:
        if (request.Value & RMVF) != 0:
            rcc_csr = rcc_csr & ~IWDGRSTF & 0xFFFFFFFF
elif request.IsRead:
    if request.Offset == CSR_OFFSET:
        request.Value = rcc_csr
    else:
        request.Value = 0x0
