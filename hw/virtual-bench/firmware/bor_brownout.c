/*
 * CANcestry T2 virtual-bench firmware - brownout (BOR) driver
 * (H-11, issue #64).
 *
 * OFF-TREE driver, linked with the tag v1.0.0 firmware sources (core/ +
 * platform/cortex_m/, unmodified; see build_firmware.sh with
 * CANCESTRY_T2_DRIVER=bor_brownout and virtual-bench-plan.md section 2). It
 * exercises the reset side of the retention claim: the BOR reset injector
 * (renode/bor_reset_injector.py) pulls the active-low NRST line at the
 * scenario's brownout instant, the machine takes the BOR reset, and this
 * driver recovers on the post-reset path THROUGH THE REAL v1.0.0 API:
 *
 *   cold boot (before the brownout)
 *     cancestry_watchdog_write_retention_register(
 *         CANCESTRY_FAULT_CODE_QUEUE_SATURATION, 0)
 *       -> RTC_BKP0R = the canonical retained terminal code (0x45565101, the
 *          code the real escalation path in core/event/src/fault.c writes).
 *          The platform's first-event-guarded hook stamps
 *          retention_write_us (0x60000008) with the exact entry time.
 *     the driver then publishes the T2 main-SRAM marker word
 *     (0x20017000) and services its alive counter (0x60000088) until the
 *     injector takes the reset.
 *
 *   post-BOR boot (after the reset)
 *     RCC_CSR.BORRSTF (bit 27, RM0440 / CMSIS stm32g474xx.h) is set: the
 *     reset cause is a brown-out reset.
 *       -> the main-SRAM marker word reads 0 (the injector discarded it with
 *          the reset: a BOR reset does not preserve main SRAM) while the
 *          retention shadow still holds the fault code. The observed marker
 *          is mirrored to 0x60000080.
 *       -> t2_bor_detect(): recovers the retained code through the real
 *          cancestry_watchdog_read_retention_register and mirrors it to
 *          0x6000008C. The .resc hook stamps bor_detect_us (0x60000078) on
 *          entry.
 *       -> t2_bor_recover(): calls the REAL
 *          cancestry_hardware_set_safe_state (SW-FR-BM-006 safe-state
 *          restoration: contactor and torque pins de-energised). The .resc
 *          hook stamps bor_recover_us (0x6000007C) on entry.
 *       -> 0x60000084 = 1 (run complete) and the alive counter keeps
 *          advancing so a hang after the recovery is visible.
 *
 * What the driver deliberately does NOT do:
 *   - It does not arm the IWDG or call cancestry_watchdog_init: no watchdog
 *     reset is part of this scenario, and watchdog_init both arms the IWDG
 *     and enforces the safe state at boot, which on the cold path would
 *     stamp the recovery slot before the brownout. The post-reset path uses
 *     the same safe-state restoration function directly, exactly as the
 *     H-08 retention driver bypasses watchdog_init for the same reason.
 *   - It does not drain the retention register: the production log path
 *     (cancestry_watchdog_restore_retained_fault) clears RTC_BKP0R after
 *     admission, and the T2 scenario deliberately keeps the retained code
 *     readable across the whole window so the orchestrator can assert its
 *     preservation AFTER the reset from the register readback itself (the
 *     strongest available T2 form of HW-SF-002 (ii)/(iii)). The drain is
 *     therefore NOT exercised (recorded in the scenario's not_covered list).
 *   - It does not touch DWT/DEMCR (bring-up finding F-27) and installs no
 *     interrupt handler: events are captured from emulation virtual time by
 *     the platform hooks.
 *   - It does not modify, wrap or reimplement any v1.0.0 function (firmware
 *     read-only, issue #64 constraint 1): the two hook wrappers call the tag
 *     sources EXACTLY as linked.
 *
 * Event capture: t2_bor_detect and t2_bor_recover are GLOBAL symbols on
 * purpose (the AddSymbolHook name lookup must resolve them; building at -O0
 * additionally keeps them as real entry points, bring-up finding F-9).
 *
 * Determinism: no wall clock, no randomness, no library time source. Every
 * load-bearing timestamp is stamped by the platform hooks from emulation
 * virtual time; the alive counter is the only free-running value.
 *
 * Requirements traced: HW-SF-002 (sub-events (ii) and (iii)), HW-SF-004;
 * HwAGENTS.md rules 1, 2 and 5.
 */

#include <stdint.h>

#include "cancestry/event/fault.h"
#include "watchdog.h"

/* ------------------------------------------------------------------------- */
/* Contract constants. These MUST match renode/bor_reset_injector.py and      */
/* renode/cancestry-hw-bor.resc; hw/virtual-bench/test_t2_brownout.py pins    */
/* all three copies so none of them drifts silently.                          */
/* ------------------------------------------------------------------------- */

/* STM32G4 reset-cause register (RM0440 section 7.4.24; bit positions per the
 * ST CMSIS device header stm32g474xx.h: RMVF 23, BORRSTF 27, IWDGRSTF 29). */
#define T2_RCC_CSR (*(volatile uint32_t *)0x40021094u)
#define T2_RCC_BORRSTF_BIT (1u << 27u)

/* BOR reset injector window (sysbus 0x40001000). */
#define T2_BOR_INJ_MAGIC (*(volatile const uint32_t *)0x40001000u)
#define T2_BOR_INJ_MAGIC_VALUE 0x424F5231u /* "BOR1" */
#define T2_BOR_INJ_NRST_LEVEL (*(volatile const uint32_t *)0x40001008u)
#define T2_NRST_RELEASED 0x1u

/* T2-declared main-SRAM marker word (platform contract, see the .repl
 * header): inside the 96 KiB SRAM region (0x20000000 + 0x18000), outside the
 * ELF's .data/.bss, > 4 KiB below the initial stack pointer 0x20018000. */
#define T2_SRAM_CANARY (*(volatile uint32_t *)0x20017000u)
#define T2_SRAM_CANARY_MAGIC 0x5AA5C0DEu

/* H-11 trace-area slots (stm32g474-cancestry.repl H-11 contract). */
#define T2_SLOT_SRAM_MAGIC_AT_BOOT (*(volatile uint32_t *)0x60000080u)
#define T2_SLOT_RUN_COMPLETE (*(volatile uint32_t *)0x60000084u)
#define T2_SLOT_ALIVE_COUNTER (*(volatile uint32_t *)0x60000088u)
#define T2_SLOT_RETENTION_CODE_AT_DETECT (*(volatile uint32_t *)0x6000008Cu)

/* ------------------------------------------------------------------------- */
/* Static storage only (agents.md: the runtime never allocates).              */
/* ------------------------------------------------------------------------- */

static cancestry_watchdog_t s_wdg;

/* ------------------------------------------------------------------------- */
/* Hook-point wrappers (the scenario's capture points). Each does its FULL    */
/* action inside: the .resc hook fires at entry, so the stamped time is the   */
/* entry into the firmware's handling of that step.                           */
/* ------------------------------------------------------------------------- */

/* Post-BOR detection (issue #64: "a symbol hook on the reset handler"). The
 * v1.0.0 ELF has no BOR classification in its reset handler, so this entry is
 * the firmware's post-reset BOR detection: it recovers the retained terminal
 * fault code through the REAL v1.0.0 retention read API and mirrors it for
 * the orchestrator. */
void t2_bor_detect(void)
{
    T2_SLOT_RETENTION_CODE_AT_DETECT =
        cancestry_watchdog_read_retention_register(&s_wdg);
}

/* Recovery: restore the safe state through the REAL v1.0.0 safe-state
 * restoration function (SW-FR-BM-006). The .resc hook stamps the recovery
 * timestamp on this function's entry. */
void t2_bor_recover(void)
{
    cancestry_hardware_set_safe_state(&s_wdg);
}

int main(void)
{
    /* Read the main-SRAM marker BEFORE publishing it: on the post-BOR boot
     * the injector has discarded it (a BOR reset does not preserve main
     * SRAM), on the cold boot it reads as the untouched SRAM value. */
    uint32_t sram_marker = T2_SRAM_CANARY;
    uint32_t reset_cause = T2_RCC_CSR;

    /* Platform preflight on the firmware side: if the BOR injector was never
     * registered its window magic cannot read back - park visibly (the
     * orchestrator reports the run as failed; it never pretends a pass). */
    if (T2_BOR_INJ_MAGIC != T2_BOR_INJ_MAGIC_VALUE) {
        for (;;) {
            __asm volatile("nop");
        }
    }

    if ((reset_cause & T2_RCC_BORRSTF_BIT) != 0u) {
        /* ---- post-BOR boot: the reset discarded main SRAM, the retention
         * domain kept the fault code, and the safe state must be restored
         * before anything else runs. ---- */
        T2_SLOT_SRAM_MAGIC_AT_BOOT = sram_marker;
        t2_bor_detect();  /* hook: detection timestamp */
        t2_bor_recover(); /* hook: recovery (safe-state) timestamp */
        T2_SLOT_RUN_COMPLETE = 1u;

        /* Service loop: keep the alive counter advancing to the end of the
         * scenario window so a hang after the recovery is visible. */
        for (;;) {
            T2_SLOT_ALIVE_COUNTER = T2_SLOT_ALIVE_COUNTER + 1u;
        }
    }

    /* ---- cold boot (before the brownout) ---- */
    T2_SRAM_CANARY = T2_SRAM_CANARY_MAGIC;

    /* Publish the canonical retained terminal code through the REAL v1.0.0
     * retention API - the same code and the same register write the
     * escalation path performs (core/event/src/fault.c). The platform's
     * first-event-guarded hook stamps retention_write_us here. */
    cancestry_watchdog_write_retention_register(
        CANCESTRY_FAULT_CODE_QUEUE_SATURATION, 0);

    /* Poll loop: advance the alive counter every pass and wait for the
     * injector to take the BOR reset (the injector reads the NRST level
     * itself; the driver only observes the line for liveness). */
    for (;;) {
        T2_SLOT_ALIVE_COUNTER = T2_SLOT_ALIVE_COUNTER + 1u;
        (void)T2_BOR_INJ_NRST_LEVEL;
    }
}
