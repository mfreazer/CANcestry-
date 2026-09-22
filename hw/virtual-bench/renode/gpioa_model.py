# Scripted GPIOA model for the CANcestry T2 virtual bench (H-07, issue #53).
#
# Implements: the GPIOA register surface used by platform/cortex_m/watchdog.c:
#
#   cancestry_hardware_set_safe_state:
#       GPIOA_BSRR = (1 << 24) | (1 << 25)   (reset bits 8 and 9)
#   -> PA8 (inverter enable / torque) LOW, PA9 (main contactor) LOW
#
# BSRR semantics: bits 0..15 set the corresponding ODR bits, bits 16..31
# reset them (reset wins on conflict, matching the STM32 RM0440 contract).
# The FMI bridge observes ODR (offset 0x14) to marshal the firmware output
# state into the plant; the safe-latch EVENT timestamp is captured by the
# cpu symbol hook in cancestry-hw.resc, not by this model.
#
# Requirements traced: HW-SF-001, HW-SF-004; HwAGENTS.md rules 2 and 6.

if request.IsInit:
    gpioa_odr = 0
    gpioa_moder = 0
elif request.IsWrite:
    if request.Offset == 0x18:
        value = request.Value & 0xFFFFFFFF
        set_bits = value & 0xFFFF
        reset_bits = (value >> 16) & 0xFFFF
        # Reset wins over set per RM0440.
        gpioa_odr = (gpioa_odr | set_bits) & ~reset_bits & 0xFFFF
    elif request.Offset == 0x14:
        gpioa_odr = request.Value & 0xFFFF
    elif request.Offset == 0x0:
        gpioa_moder = request.Value & 0xFFFFFFFF
elif request.IsRead:
    if request.Offset == 0x14:
        request.Value = gpioa_odr
    elif request.Offset == 0x0:
        request.Value = gpioa_moder
    elif request.Offset == 0x18:
        request.Value = 0x0  # BSRR is write-only
    else:
        request.Value = 0x0
