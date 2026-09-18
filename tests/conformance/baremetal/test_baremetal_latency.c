/*
 * CANcestry Bare-Metal Conformance: Sub-50µs Latency and Interrupt Safety.
 *
 * Contract under test (docs/software/SwRS.md section 15):
 *   - SW-FR-BM-003: interrupt safety and bounded ISR execution. The CAN RX
 *     ISR shall be strictly bounded in execution time, performing only
 *     hardware FIFO drain, timestamp capture, and push to the lock-free ISR
 *     queue. No decoding, no UDS parsing, and no blocking waits inside ISR.
 *   - SW-FR-BM-004: hardware timestamp capture and monotonicity. Monotonic
 *     cycle counter tracking guarantees zero rollover glitches over extended
 *     uptime.
 *   - SW-FR-BM-007: sub-50µs latency from CAN RX interrupt arrival to FSM
 *     event processing.
 *
 * Test ids: BM-LAT-001 .. BM-LAT-004.
 */

#if !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 199309L
#endif

#include "cancestry/event/clock.h"
#include "cancestry/event/isr_queue.h"
#include "cancestry/event/queue.h"
#include "cancestry/fsm/engine.h"
#include "hal_stm32.h"

#include "cancestry_fsm_conformance.h"
#include "cancestry_test.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#define ISR_QUEUE_CAPACITY ((uint16_t)32u)
#define MAIN_QUEUE_CAPACITY ((uint16_t)64u)

static const char *const latency_fsm_yaml =
    "schema_version: \"0.2.0\"\n"
    "state_machines:\n"
    "  - name: latency\n"
    "    description: Real-time latency benchmark machine\n"
    "    initial: STANDBY\n"
    "    states:\n"
    "      - name: STANDBY\n"
    "        transitions:\n"
    "          - event: can_rx\n"
    "            message: StatusMsg\n"
    "            target: OPERATIONAL\n"
    "      - name: OPERATIONAL\n"
    "        transitions:\n"
    "          - event: can_rx\n"
    "            message: StatusMsg\n"
    "            target: OPERATIONAL\n"
    "instances:\n"
    "  - id: latency.main\n"
    "    machine: latency\n"
    "    enabled: true\n";

static uint64_t get_host_time_ns(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0u;
    }
    return ((uint64_t)ts.tv_sec * 1000000000ULL) + (uint64_t)ts.tv_nsec;
}

/* ------------------------------------------------------------------------- */
/* BM-LAT-001: Sub-50µs Latency from ISR arrival to FSM event insertion     */
/* ------------------------------------------------------------------------- */
static void test_baremetal_single_frame_latency(void)
{
    fsm_test_fixture_t fixture;
    hal_stm32_context_t hal_ctx;
    cancestry_event_t isr_slots[ISR_QUEUE_CAPACITY];
    cancestry_event_isr_queue_t isr_queue;
    cancestry_event_t main_slots[MAIN_QUEUE_CAPACITY];
    cancestry_event_queue_t main_queue;
    cancestry_hal_frame_t frame;
    cancestry_event_t event;
    uint64_t start_ns;
    uint64_t end_ns;
    uint64_t elapsed_us;

    CANCESSTRY_TEST_CASE("BM-LAT-001: single frame ISR-to-FSM latency < 50µs");

    CANCESSTRY_TEST_CHECK(fsm_test_init(&fixture, latency_fsm_yaml));
    CANCESSTRY_TEST_CHECK_U64(fsm_test_start(&fixture, "latency.main"), CANCESTRY_FSM_OK);
    CANCESSTRY_TEST_CHECK_STRING(fsm_test_state(&fixture, "latency.main"), "STANDBY");

    /* Initialize hardware HAL & lock-free ISR queue */
    memset(&hal_ctx, 0, sizeof(hal_ctx));
    hal_stm32_clock_init(&hal_ctx.clock, 160000000u); /* 160 MHz */
    CANCESSTRY_TEST_CHECK(cancestry_event_isr_queue_init(&isr_queue, isr_slots, ISR_QUEUE_CAPACITY));
    hal_stm32_set_isr_queue(&hal_ctx, &isr_queue);
    hal_ctx.controllers[0].opened = true;

    /* Initialize main priority queue */
    CANCESSTRY_TEST_CHECK(cancestry_event_queue_init(&main_queue, main_slots, MAIN_QUEUE_CAPACITY));

    /* Prepare incoming hardware CAN frame (0x100 = StatusMsg) */
    memset(&frame, 0, sizeof(frame));
    frame.interface_id = FSM_TEST_IFACE_CAN0;
    frame.can_id = FSM_TEST_MSG_STATUS;
    frame.length = 8u;
    frame.data[0] = 0xAAu;

    /* Inject frame into hardware FIFO */
    hal_stm32_sim_inject_hardware_frame(&hal_ctx, 0, &frame);

    /* Measure latency from hardware interrupt to FSM completion */
    start_ns = get_host_time_ns();

    /* 1. Hardware CAN RX ISR executes */
    hal_stm32_can_rx_isr(&hal_ctx, 0);

    /* 2. Main loop drains lock-free ISR queue into prioritized event queue */
    CANCESSTRY_TEST_CHECK_U64(cancestry_event_isr_queue_drain(&isr_queue, &main_queue), 1u);

    /* 3. Main loop pops event */
    CANCESSTRY_TEST_CHECK(cancestry_event_queue_pop(&main_queue, &event) == CANCESTRY_EVENT_QUEUE_OK);

    /* 4. FSM engine processes event */
    CANCESSTRY_TEST_CHECK(cancestry_fsm_engine_process_event(&fixture.engine, &event) == CANCESTRY_FSM_OK);

    end_ns = get_host_time_ns();
    elapsed_us = (end_ns - start_ns) / 1000ULL;

    /* Verify sub-50µs latency (SW-FR-BM-007) */
    printf("      [BM-LAT-001] Measured ISR-to-FSM latency: %llu µs (budget: 50 µs)\n",
           (unsigned long long)elapsed_us);
    CANCESSTRY_TEST_CHECK(elapsed_us < 50ULL);

    /* Verify FSM transitioned cleanly to OPERATIONAL */
    CANCESSTRY_TEST_CHECK_STRING(fsm_test_state(&fixture, "latency.main"), "OPERATIONAL");

    fsm_test_destroy(&fixture);
}

/* ------------------------------------------------------------------------- */
/* BM-LAT-002: Sustained Burst Processing Latency (< 50µs per frame)        */
/* ------------------------------------------------------------------------- */
static void test_baremetal_burst_latency(void)
{
    fsm_test_fixture_t fixture;
    hal_stm32_context_t hal_ctx;
    cancestry_event_t isr_slots[ISR_QUEUE_CAPACITY];
    cancestry_event_isr_queue_t isr_queue;
    cancestry_event_t main_slots[MAIN_QUEUE_CAPACITY];
    cancestry_event_queue_t main_queue;
    cancestry_hal_frame_t frame;
    uint32_t i;
    uint64_t max_latency_us = 0u;

    CANCESSTRY_TEST_CASE("BM-LAT-002: sustained burst latency under 50µs");

    CANCESSTRY_TEST_CHECK(fsm_test_init(&fixture, latency_fsm_yaml));
    CANCESSTRY_TEST_CHECK_U64(fsm_test_start(&fixture, "latency.main"), CANCESTRY_FSM_OK);

    memset(&hal_ctx, 0, sizeof(hal_ctx));
    hal_stm32_clock_init(&hal_ctx.clock, 160000000u);
    CANCESSTRY_TEST_CHECK(cancestry_event_isr_queue_init(&isr_queue, isr_slots, ISR_QUEUE_CAPACITY));
    hal_stm32_set_isr_queue(&hal_ctx, &isr_queue);
    hal_ctx.controllers[0].opened = true;
    CANCESSTRY_TEST_CHECK(cancestry_event_queue_init(&main_queue, main_slots, MAIN_QUEUE_CAPACITY));

    memset(&frame, 0, sizeof(frame));
    frame.interface_id = FSM_TEST_IFACE_CAN0;
    frame.can_id = FSM_TEST_MSG_STATUS;
    frame.length = 8u;

    for (i = 0u; i < 20u; ++i) {
        cancestry_event_t event;
        uint64_t t0;
        uint64_t t1;
        uint64_t lat_us;

        frame.data[0] = (uint8_t)i;
        hal_stm32_sim_inject_hardware_frame(&hal_ctx, 0, &frame);

        t0 = get_host_time_ns();
        hal_stm32_can_rx_isr(&hal_ctx, 0);
        cancestry_event_isr_queue_drain(&isr_queue, &main_queue);
        CANCESSTRY_TEST_CHECK(cancestry_event_queue_pop(&main_queue, &event) == CANCESTRY_EVENT_QUEUE_OK);
        CANCESSTRY_TEST_CHECK(cancestry_fsm_engine_process_event(&fixture.engine, &event) == CANCESTRY_FSM_OK);
        t1 = get_host_time_ns();

        lat_us = (t1 - t0) / 1000ULL;
        if (lat_us > max_latency_us) {
            max_latency_us = lat_us;
        }
        CANCESSTRY_TEST_CHECK(lat_us < 50ULL);
    }

    printf("      [BM-LAT-002] 20 frames burst: max latency = %llu µs (< 50 µs)\n",
           (unsigned long long)max_latency_us);
    CANCESSTRY_TEST_CHECK_U64(isr_queue.dropped, 0u);

    fsm_test_destroy(&fixture);
}

/* ------------------------------------------------------------------------- */
/* BM-LAT-003: Hardware Timestamp Capture & Monotonicity (SW-FR-BM-004)     */
/* ------------------------------------------------------------------------- */
static void test_baremetal_timestamp_monotonicity(void)
{
    hal_stm32_clock_t clk;
    cancestry_time_us_t ts1;
    cancestry_time_us_t ts2;
    cancestry_time_us_t ts3;

    CANCESSTRY_TEST_CASE("BM-LAT-003: hardware timestamp monotonicity & rollover");

    hal_stm32_clock_init(&clk, 160000000u); /* 160 MHz */

    /* Initial timestamp */
    ts1 = hal_stm32_get_timestamp_us(&clk);

    /* Advance simulated cycles */
    clk.last_raw_cycles = 1000u;
    ts2 = hal_stm32_get_timestamp_us(&clk);
    CANCESSTRY_TEST_CHECK(ts2 >= ts1);

    /* Simulate 32-bit hardware counter rollover (e.g. from 0xFFFFFF00 to 0x00000100) */
    clk.last_raw_cycles = 0xFFFFFF00u;
    /* Next read: counter has rolled over 32 bits to 0x100 */
    ts3 = hal_stm32_get_timestamp_us(&clk);
    CANCESSTRY_TEST_CHECK(ts3 >= ts2);

    /* Monotonicity check across multiple reads */
    {
        uint32_t step;
        cancestry_time_us_t prev = ts3;
        for (step = 0; step < 100; ++step) {
            cancestry_time_us_t cur = hal_stm32_get_timestamp_us(&clk);
            CANCESSTRY_TEST_CHECK(cur >= prev);
            prev = cur;
        }
    }
}

/* ------------------------------------------------------------------------- */
/* BM-LAT-004: Interrupt Safety & Bounded ISR Execution (SW-FR-BM-003)      */
/* ------------------------------------------------------------------------- */
static void test_baremetal_interrupt_bounds_overflow(void)
{
    hal_stm32_context_t hal_ctx;
    cancestry_event_t tiny_slots[3];
    cancestry_event_isr_queue_t isr_queue;
    cancestry_hal_frame_t frame;
    uint32_t i;

    CANCESSTRY_TEST_CASE("BM-LAT-004: ISR bounded queue overflow and non-blocking safety");

    memset(&hal_ctx, 0, sizeof(hal_ctx));
    hal_stm32_clock_init(&hal_ctx.clock, 160000000u);
    /* Small capacity: 3 slots */
    CANCESSTRY_TEST_CHECK(cancestry_event_isr_queue_init(&isr_queue, tiny_slots, 3u));
    hal_stm32_set_isr_queue(&hal_ctx, &isr_queue);
    hal_ctx.controllers[0].opened = true;

    memset(&frame, 0, sizeof(frame));
    frame.can_id = 0x123u;
    frame.length = 8u;

    /* Inject 3 frames to fill queue */
    for (i = 0; i < 3; ++i) {
        hal_stm32_sim_inject_hardware_frame(&hal_ctx, 0, &frame);
        hal_stm32_can_rx_isr(&hal_ctx, 0);
    }

    /* Queue should have admitted 2 items (capacity 3 holds max 2 in SPSC ring) */
    CANCESSTRY_TEST_CHECK_U64(isr_queue.pushed, 2u);
    CANCESSTRY_TEST_CHECK_U64(isr_queue.dropped, 1u);

    /* Subsequent push into full queue is safely dropped without blocking */
    hal_stm32_sim_inject_hardware_frame(&hal_ctx, 0, &frame);
    hal_stm32_can_rx_isr(&hal_ctx, 0);
    CANCESSTRY_TEST_CHECK_U64(isr_queue.dropped, 2u);
}

int main(void)
{
    CANCESSTRY_TEST_SUITE_BEGIN("Bare-Metal Latency & Real-Time Conformance");

    test_baremetal_single_frame_latency();
    test_baremetal_burst_latency();
    test_baremetal_timestamp_monotonicity();
    test_baremetal_interrupt_bounds_overflow();

    return CANCESSTRY_TEST_SUITE_END();
}
