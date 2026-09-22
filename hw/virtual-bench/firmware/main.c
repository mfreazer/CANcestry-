/*
 * CANcestry T2 virtual-bench firmware - bring-up driver (H-08, issue #55).
 *
 * OFF-TREE driver, linked with the tag v1.0.0 firmware sources (core/ +
 * platform/cortex_m/, unmodified; see build_firmware.sh and the plan
 * section 2). It reproduces the QA-EV-01 hard-fault escalation recipe on
 * the virtual bench so the Renode platform can capture the three event
 * timestamps the H-07 bridge contract requires:
 *
 *   1. retention_write : cancestry_watchdog_write_retention_register entry
 *   2. safe_latch      : cancestry_hardware_set_safe_state entry
 *   3. iwdg_fire       : IWDG expiry (scripted platform reset)
 *
 * Wiring notes (full rationale in docs/hw/t2-bringup-report.md):
 *
 *  - The escalation is driven by the REAL
 *    cancestry_event_hard_fault_escalate wrapper with the real firmware
 *    retention and escalation hooks. The hal_fail_safe binding is omitted:
 *    the production fail-safe hook's effect IS the safe latch, and
 *    cancestry_watchdog_escalate asserts that latch by design (the
 *    deliberate double-latch documented in watchdog.c); binding it as well
 *    would trip the platform's exactly-once capture contract. The wrapper's
 *    incomplete-binding safe-spin tail is unreachable: the iwdg action
 *    parks until the IWDG reset completes.
 *
 *  - cancestry_watchdog_init is deliberately NOT called: it enforces the
 *    safe state at boot, which would stamp a safe_latch event before the
 *    scenario starts. Instead the driver starts the IWDG directly with the
 *    identical register sequence and the same timeout as
 *    cancestry_watchdog_init's default configuration.
 *
 *  - The iwdg hook adapter polls IWDG_SR after escalating because the
 *    scripted Renode IWDG evaluates expiry on bus access, while real
 *    silicon fires asynchronously. This is a model adaptation, not a
 *    firmware deviation (tool-qualification.md section 4.3).
 *
 *  - After the scripted IWDG reset the driver must NOT re-enter the
 *    escalation recipe (the H-07 capture contract requires exactly one
 *    event per slot). It detects the post-reset boot through the IWDG
 *    reset-cause flag in RCC_CSR (which the platform model persists across
 *    the reset, as the STM32G4 RCC does until RMVF) and parks in a
 *    safe-state nop spin without touching any capture slot.
 */

#include <stdint.h>

#include "cancestry/event/fault.h"
#include "watchdog.h"

/* Register access identical to the IWDG block in cancestry_watchdog_init
 * (platform/cortex_m/watchdog.c at tag v1.0.0). */
#define T2_IWDG_KR  (*(volatile uint32_t *)0x40003000u)
#define T2_IWDG_PR  (*(volatile uint32_t *)0x40003004u)
#define T2_IWDG_RLR (*(volatile uint32_t *)0x40003008u)
#define T2_IWDG_SR  (*(volatile uint32_t *)0x4000300Cu)
#define T2_RCC_CSR  (*(volatile uint32_t *)0x40021094u)
#define T2_RCC_IWDGRSTF_BIT (1u << 29u)

/* Default timeout of cancestry_watchdog_init when config is NULL. */
#define T2_IWDG_TIMEOUT_MS 50u

/* Deterministic busy-waits. At -O0 (no optimization, no inlining) the loop
 * bodies are compiled as written, so the cycle counts - and therefore the
 * virtual-time separation of the captured events - are reproducible across
 * runs of the same image and compiler. The gaps only need to be distinct
 * integer microseconds on both sides of each stamp; they are sized to stay
 * inside the IWDG window (102 ms from arming) on any plausible CPU clock. */
#define T2_SETTLE_DELAY_ITERS 200000u
#define T2_LATCH_GAP_ITERS 20000u

static cancestry_watchdog_t s_wdg;

static void t2_busy_delay(uint32_t iterations)
{
    volatile uint32_t i = 0u;
    while (i < iterations) {
        __asm volatile("nop");
        i += 1u;
    }
}

static void t2_iwdg_arm(void)
{
    T2_IWDG_KR = 0x5555u;      /* key write: unlock */
    T2_IWDG_PR = 0x03u;        /* prescaler 64 (2 ms ticks at 32 kHz) */
    T2_IWDG_RLR = T2_IWDG_TIMEOUT_MS & 0x0FFFu;
    T2_IWDG_KR = 0xAAAAu;      /* reload */
    T2_IWDG_KR = 0xCCCCu;      /* start */
}

/* T2 iwdg escalation hook: assert the safe state and drop the reload
 * window to its minimum through the real firmware escalation action, then
 * wait for the scripted IWDG reset to complete. */
static void t2_iwdg_escalate_hook(void *context)
{
    t2_busy_delay(T2_LATCH_GAP_ITERS);
    cancestry_watchdog_escalate((cancestry_watchdog_t *)context);
    for (;;) {
        (void)T2_IWDG_SR;      /* scripted model evaluates expiry on read */
    }
}

int main(void)
{
    if ((T2_RCC_CSR & T2_RCC_IWDGRSTF_BIT) != 0u) {
        /* Post IWDG-reset boot: the fault code and safe latch were written
         * before the reset. Park in the safe state; do not touch the
         * capture slots. */
        for (;;) {
            __asm volatile("nop");
        }
    }

    t2_iwdg_arm();
    t2_busy_delay(T2_SETTLE_DELAY_ITERS);

    cancestry_event_hard_fault_hooks_t hooks;
    hooks.write_retention_register = cancestry_watchdog_write_retention_register;
    hooks.hal_fail_safe = 0;
    hooks.iwdg_escalate = t2_iwdg_escalate_hook;
    hooks.context = &s_wdg;

    cancestry_event_hard_fault_escalate(&hooks);

    for (;;) {
        __asm volatile("nop");
    }
    return 0;
}
