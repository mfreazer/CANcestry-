/*
 * CANcestry T2 virtual-bench firmware - CAN Bus-Off / CRC fault driver
 * (H-10, issue #62).
 *
 * OFF-TREE driver, linked with the tag v1.0.0 firmware sources (core/ +
 * platform/cortex_m/, unmodified; see build_firmware.sh with
 * CANCESTRY_T2_DRIVER=can_fault and virtual-bench-plan.md section 2). It
 * responds to the fault conditions the CAN fault injector (the shared bus
 * medium of renode/can_fault_injector.py) projects into the FDCAN1 register
 * scratch, and it handles them through the REAL v1.0.0 fault path:
 *
 *   Bus-Off (HIL-BUSOFF-001, docs/qa/hil-fault-injection-report.md 4.1):
 *     FDCAN1_PSR.BO set by the medium
 *       -> t2_can_busoff_detect(): cancestry_hal_raise_fault(
 *              CANCESTRY_HAL_FAULT_BUS_OFF, CRITICAL) - the real code that
 *              moves the interface to CANCESTRY_HAL_IF_STATE_BUS_OFF and
 *              enqueues a FAULT_RAISED event (core/hal/src/hal.c:261)
 *       -> t2_can_busoff_recover(): clear FDCAN1_CCCR.INIT, the ISO 11898-1 /
 *          RM0440 Bus-Off recovery request (the STM32G4 FDCAN has NO
 *          auto-recovery: software must clear INIT once)
 *       -> the medium releases the bus 128 * 11 recessive bits (704 us at the
 *          HW-FR-003 nominal 2 Mbit/s) after the request; the driver
 *          completes when the projected PSR.BO reads 0.
 *
 *   CRC error (HIL-CRC-001, same report 4.2):
 *     FDCAN1_PSR.LEC == 2 (CRC error) raised by the medium
 *       -> t2_can_crc_detect(): increment the firmware CRC error counter and
 *          raise the fault through the real shaping logic. v1.0.0 has no
 *          CRC-specific fault code; its nearest normative code for a frame
 *          whose integrity checks fail on the wire is
 *          CANCESTRY_HAL_FAULT_MALFORMED_FRAME = 8 (HAL fault-code contract,
 *          core/hal/include/cancestry/hal/types.h); that mapping is
 *          documented, never normalized to a fake code.
 *       -> acknowledge the error-logging interrupt with an FDCAN_IR write
 *          (write-1-to-clear on real silicon: writing a value with IR.ELO
 *          clear acknowledges it); the medium then releases the CRC
 *          condition.
 *
 * What the driver deliberately does NOT do:
 *   - It does not arm the IWDG and never calls cancestry_watchdog_init: no
 *     reset is part of these scenarios. A hung firmware stays hung and is
 *     reported as such through alive_counter / run_complete (fail-closed,
 *     negative crash evidence).
 *   - It does not touch DWT/DE MCR (bring-up finding F-27: those cores are
 *     owned by the Renode CPU model and are not load-bearing here either;
 *     the HAL is given a deterministic virtual clock instead of the
 *     platform clock, exactly like the H-08 timing-bypass decision).
 *   - It does not modify, wrap or reimplement any v1.0.0 function: the
 *     detection/recovery wrappers call the tag sources EXACTLY as linked
 *     (firmware read-only, issue #62 constraint 1).
 *
 * Event capture: the platform stamps emulation virtual time at the hook
 * points (cancestry-hw-fault.resc): t2_can_busoff_detect (entry, the poll
 * pass that observed PSR.BO and entered the real fault path),
 * t2_can_busoff_recover (entry, the ISO 11898-1 recovery request),
 * t2_can_crc_detect (entry). The three functions are GLOBAL symbols on
 * purpose (the AddSymbolHook name lookup must resolve them; building at -O0
 * additionally keeps them as real entry points, bring-up finding F-9).
 *
 * Determinism: no wall clock, no randomness, no library time source. The
 * only advancing source on the firmware side is the virtual clock, advanced
 * by exactly 1 microsecond per poll pass (integer arithmetic); every
 * load-bearing timestamp is stamped by the platform hooks from emulation
 * virtual time.
 *
 * Requirements traced: HW-FR-003; HwAGENTS.md rules 1, 2 and 5.
 */

#include <stdbool.h>
#include <stdint.h>

#include "cancestry/event/clock.h"
#include "cancestry/event/queue.h"
#include "cancestry/hal/hal.h"
#include "cancestry/hal/types.h"
#include "hal_stm32.h"

/* ------------------------------------------------------------------------- */
/* CAN fault injector + FDCAN register surface (contract constants).          */
/* These MUST match renode/can_fault_injector.py and                           */
/* renode/cancestry-hw-fault.resc; hw/virtual-bench/test_t2_busoff.py pins    */
/* all three copies so none of them drifts silently.                          */
/* ------------------------------------------------------------------------- */

/* Injector register window (sysbus 0x40000000). */
#define T2_INJ_MAGIC      (*(volatile const uint32_t *)0x40000000u)
#define T2_INJ_BUS_STATE  (*(volatile const uint32_t *)0x40000010u)
#define T2_INJ_MAGIC_VALUE 0x43464931u   /* "CFI1" */
#define T2_STATE_BUS_ACTIVE 0x0u
#define T2_STATE_BUS_OFF    0x1u
#define T2_STATE_CRC_ERROR  0x2u

/* STM32G4 FDCAN1 (RM0440 43.4; offsets from st's CMSIS stm32g474xx.h). */
#define T2_FDCAN1_CCCR (*(volatile uint32_t *)0x40006418u) /* off 0x18 */
#define T2_FDCAN1_ECR  (*(volatile const uint32_t *)0x40006440u) /* off 0x40 */
#define T2_FDCAN1_PSR  (*(volatile const uint32_t *)0x40006444u) /* off 0x44 */
#define T2_FDCAN1_IR   (*(volatile uint32_t *)0x40006450u) /* off 0x50 */
#define T2_FDCAN_CCCR_INIT (1u << 0u)
#define T2_FDCAN_PSR_LEC_MASK 0x7u
#define T2_FDCAN_PSR_BO (1u << 7u)
#define T2_FDCAN_IR_ELO (1u << 16u)
#define T2_LEC_CRC_ERROR 2u

/* T2 fault-capture slots in the trace area (see stm32g474-cancestry.repl). */
#define T2_SLOT_CRC_ERROR_COUNT (*(volatile uint32_t *)0x60000064u)
#define T2_SLOT_RUN_COMPLETE (*(volatile uint32_t *)0x60000068u)
#define T2_SLOT_ALIVE_COUNTER (*(volatile uint32_t *)0x6000006cu)

/* ------------------------------------------------------------------------- */
/* Static storage only (agents.md: the runtime never allocates).              */
/* ------------------------------------------------------------------------- */

#define T2_QUEUE_CAPACITY 16u

static cancestry_event_t s_event_slots[T2_QUEUE_CAPACITY];
static cancestry_event_queue_t s_queue;
static cancestry_hal_t s_hal;
static hal_stm32_context_t s_hal_ctx;
static cancestry_virtual_clock_t s_vclock;
static uint32_t s_crc_error_count = 0u;

/* The gateway interface under test (channel 1, VCU-facing). */
static const cancestry_hal_if_config_t s_iface_config = {
    0u,           /* interface_id */
    "fdcan1",     /* name */
    2000000u,     /* bitrate: HW-FR-003 nominal 2 Mbit/s */
    0u,           /* listen_only */
    1u,           /* can_fd */
    5000000u      /* data_bitrate: HW-FR-003 data phase 5 Mbit/s */
};

static void t2_clock_tick(void)
{
    cancestry_virtual_clock_advance(&s_vclock, 1u);
}

/* ------------------------------------------------------------------------- */
/* Hook-point wrappers. Each is the scenario's capture point for the real     */
/* fault path and deliberately does its FULL action inside (the hook fires at */
/* entry, so the stamped time is the entry into the firmware's handling of    */
/* the fault condition).                                                      */
/* ------------------------------------------------------------------------- */

/* Bus-Off detection: the poll pass observed FDCAN1_PSR.BO. Enter the real
 * v1.0.0 fault path: cancestry_hal_raise_fault sets if_states to
 * CANCESTRY_HAL_IF_STATE_BUS_OFF (TX no longer permitted) and enqueues a
 * CRITICAL FAULT_RAISED event into the real bounded queue. */
void t2_can_busoff_detect(void)
{
    (void)cancestry_hal_raise_fault(&s_hal, &s_queue,
                                    (cancestry_interface_id_t)0u,
                                    CANCESTRY_HAL_FAULT_BUS_OFF,
                                    CANCESTRY_FAULT_SEVERITY_CRITICAL);
}

/* Bus-Off recovery request: the ISO 11898-1 / RM0440 sequence. The STM32G4
 * FDCAN has no automatic Bus-Off recovery: software must clear CCCR.INIT
 * once, after which the controller-side hardware (modeled by the medium)
 * waits through 128 occurrences of 11 consecutive recessive bits before the
 * node may transmit again. The driver clears INIT and returns; the release
 * is observed by polling the projected PSR.BO from main(). */
void t2_can_busoff_recover(void)
{
    T2_FDCAN1_CCCR = 0u;   /* INIT clear: recovery request to the medium */
}

/* CRC error detection: increment the firmware CRC error counter (mirrored to
 * the trace slot so the orchestrator can assert the increment), then raise
 * the fault through the real shaping logic. See the header: v1.0.0 maps a
 * frame with a failed wire integrity check onto
 * CANCESTRY_HAL_FAULT_MALFORMED_FRAME; a CRC error leaves the interface UP
 * and is WARNING, not CRITICAL (mode-fault-state-machine.md). */
void t2_can_crc_detect(void)
{
    s_crc_error_count += 1u;
    T2_SLOT_CRC_ERROR_COUNT = s_crc_error_count;
    (void)cancestry_hal_raise_fault(&s_hal, &s_queue,
                                    (cancestry_interface_id_t)0u,
                                    CANCESTRY_HAL_FAULT_MALFORMED_FRAME,
                                    CANCESTRY_FAULT_SEVERITY_WARNING);
}

/* ------------------------------------------------------------------------- */
/* Scenario loop.                                                             */
/* Each scenario injects exactly one fault at t = 10 ms on the orchestrator   */
/* side; the driver responds to whichever condition it observes (one driver   */
/* serves both H-10 scenarios). run_complete is written only after the        */
/* observed condition was fully handled: for Bus-Off, after the medium        */
/* released the bus AND the projected PSR.BO reads 0; for CRC, after the      */
/* acknowledge left the medium AND the projected PSR.LEC reads "no error".    */
/* ------------------------------------------------------------------------- */

int main(void)
{
    bool bus_off_seen = false;
    bool bus_off_recovered = false;
    bool crc_seen = false;

    cancestry_virtual_clock_init(&s_vclock, 0u);

    (void)cancestry_event_queue_init(&s_queue, s_event_slots,
                                     (uint16_t)T2_QUEUE_CAPACITY);

    {
        cancestry_hal_config_t hal_config = {
            cancestry_cortex_m_hal_backend(), /* backend */
            &s_hal_ctx,                       /* backend_context */
            NULL,                             /* clock, bound below */
            &s_iface_config,                  /* ifaces */
            1u,                               /* iface_count */
            NULL,                             /* rx_rings */
            NULL                              /* tx_rings */
        };
        cancestry_clock_t clock = cancestry_clock_from_virtual(&s_vclock);
        hal_config.clock = &clock;
        if (!cancestry_hal_init(&s_hal, &hal_config)) {
            /* Configuration error: park visibly (run_complete stays 0 and
             * alive_counter stops - the orchestrator reports the run as
             * failed; it never pretends a pass). */
            for (;;) {
                __asm volatile("nop");
            }
        }
    }

    /* Start the controller in the medium-active state: CCCR.INIT clear. The
     * injector owns all other projections; this is the driver's only CCCR
     * write outside the recovery request. */
    T2_FDCAN1_CCCR = 0u;

    /* Platform preflight on the firmware side: if the injector was never
     * registered the magic register cannot read back - park visibly like the
     * HAL-config failure above. */
    if (T2_INJ_MAGIC != T2_INJ_MAGIC_VALUE) {
        for (;;) {
            __asm volatile("nop");
        }
    }

    for (;;) {
        uint32_t alive = T2_SLOT_ALIVE_COUNTER + 1u;
        uint32_t bus_state;
        uint32_t psr;

        T2_SLOT_ALIVE_COUNTER = alive;
        t2_clock_tick();

        /* Read the medium state FIRST: the window read drives the injector's
         * lazy evaluation (release deadline / CRC clear), exactly the same
         * model pattern as the scripted IWDG read in the retention driver. */
        bus_state = T2_INJ_BUS_STATE;
        psr = T2_FDCAN1_PSR;
        (void)T2_FDCAN1_ECR;

        /* --- Bus-Off handling (t2_busoff_001) --- */
        if (!bus_off_seen && (psr & T2_FDCAN_PSR_BO) != 0u) {
            bus_off_seen = true;
            t2_can_busoff_detect();          /* hook: detection timestamp */
        }
        if (bus_off_seen && !bus_off_recovered) {
            bus_off_recovered = true;
            t2_can_busoff_recover();         /* hook: recovery request */
        }
        if (bus_off_recovered && bus_state == T2_STATE_BUS_ACTIVE
                && (psr & T2_FDCAN_PSR_BO) == 0u) {
            /* Medium released after the 128 x 11 sequence and the projected
             * PSR confirms it: the firmware's recovery is complete. */
            T2_SLOT_RUN_COMPLETE = 1u;
            for (;;) {
                __asm volatile("nop");
            }
        }

        /* --- CRC error handling (t2_crc_001) --- */
        if (!crc_seen && (psr & T2_FDCAN_PSR_LEC_MASK) == T2_LEC_CRC_ERROR) {
            crc_seen = true;
            t2_can_crc_detect();             /* hook: detection timestamp */
            /* Acknowledge the error-logging interrupt (W1C on real silicon):
             * writing IR with IR.ELO clear acknowledges it to the medium,
             * which then releases the CRC condition. */
            T2_FDCAN1_IR = 0u;
        }
        if (crc_seen && bus_state == T2_STATE_BUS_ACTIVE
                && (psr & T2_FDCAN_PSR_LEC_MASK) == 0u
                && (T2_FDCAN1_IR & T2_FDCAN_IR_ELO) == 0u) {
            /* The condition left the medium, LEC read-backs are clean and
             * the interrupt flag is acknowledged: handled without a crash. */
            T2_SLOT_RUN_COMPLETE = 1u;
            for (;;) {
                __asm volatile("nop");
            }
        }
    }
}
