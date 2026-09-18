/*
 * Shared fixture for the ISO-TP transport conformance suite (Phase 10,
 * issue #26).
 *
 * The suite verifies the behavioural contract of docs/software/SwRS.md
 * section 14.1 (SW-FR-TP-001..010) against the shipping engine: frames are
 * fed through cancestry_transport_rx_frame(), time only moves through
 * cancestry_transport_tick() (a deterministic virtual clock drives the 1 ms
 * tick), and every observable effect - transmitted frames, delivered
 * messages, injected fault events - is recorded here instead of reaching a
 * bus. A test that passes through this fixture has exercised the PCI
 * dispatch, the session state machines, the tick-driven timers, the pending
 * retransmission path and the fault shaping at once.
 *
 * Everything is static inline: the suite has no build-time dependency beyond
 * the core libraries, and each test stays an independent executable (the
 * convention of tests/unit/support/cancestry_test.h).
 */

#ifndef CANCESTRY_TP_CONFORMANCE_FIXTURE_H
#define CANCESTRY_TP_CONFORMANCE_FIXTURE_H

#include "cancestry/event/queue.h"
#include "cancestry/transport/engine.h"

#include "cancestry_test.h"

#include <string.h>

/* ------------------------------------------------------------------------- */
/* Sizes                                                                     */
/* ------------------------------------------------------------------------- */

#define TP_TEST_RX_CAPACITY ((size_t)192u)
#define TP_TEST_TX_CAPACITY ((size_t)192u)
#define TP_TEST_FRAME_LOG_MAX ((size_t)160u)
#define TP_TEST_MESSAGE_MAX ((size_t)256u)
#define TP_TEST_QUEUE_DEPTH ((uint16_t)32u)

/** Fault source id used by the fixture engine (visible in fault events). */
#define TP_TEST_SOURCE_ID ((cancestry_source_id_t)0x0701u)

/* ------------------------------------------------------------------------- */
/* Capture logs                                                             */
/* ------------------------------------------------------------------------- */

/** One captured transmitted frame. */
typedef struct tp_test_frame {
    cancestry_tp_tx_frame_kind_t kind;
    uint8_t data[CANCESTRY_TP_FRAME_MAX_LENGTH];
    uint8_t length;
} tp_test_frame_t;

typedef struct tp_test_frame_log {
    tp_test_frame_t frames[TP_TEST_FRAME_LOG_MAX];
    size_t count;
    /** While positive, on_frame_tx declines frames (fail-closed retry path). */
    size_t decline_budget;
} tp_test_frame_log_t;

/** One captured delivered message. */
typedef struct tp_test_message {
    uint8_t data[TP_TEST_MESSAGE_MAX];
    size_t length;
} tp_test_message_t;

typedef struct tp_test_message_log {
    tp_test_message_t messages[16u];
    size_t count;
} tp_test_message_log_t;

/* ------------------------------------------------------------------------- */
/* Environment                                                               */
/* ------------------------------------------------------------------------- */

typedef struct tp_test_env {
    cancestry_transport_t tp;
    uint8_t rx_buffer[TP_TEST_RX_CAPACITY];
    uint8_t tx_buffer[TP_TEST_TX_CAPACITY];
    cancestry_tp_sink_t sink;
    tp_test_frame_log_t frames;
    tp_test_message_log_t messages;
    cancestry_event_queue_t queue;
    cancestry_event_t queue_slots[TP_TEST_QUEUE_DEPTH];
    cancestry_virtual_clock_t clock;
    cancestry_clock_t clock_binding;
} tp_test_env_t;

static inline bool tp_test_on_frame_tx(void *user_data, const cancestry_tp_tx_frame_t *frame)
{
    tp_test_env_t *env = (tp_test_env_t *)user_data;

    if (env->frames.decline_budget > 0u) {
        env->frames.decline_budget--;
        return false;
    }
    if (env->frames.count < TP_TEST_FRAME_LOG_MAX) {
        tp_test_frame_t *slot = &env->frames.frames[env->frames.count];

        slot->kind = frame->kind;
        slot->length = frame->length;
        memcpy(slot->data, frame->data, (size_t)frame->length);
        env->frames.count++;
    }
    return true;
}

static inline void tp_test_on_message(void *user_data, const uint8_t *data, size_t length)
{
    tp_test_env_t *env = (tp_test_env_t *)user_data;

    if (env->messages.count < 16u && length <= TP_TEST_MESSAGE_MAX) {
        tp_test_message_t *slot = &env->messages.messages[env->messages.count];

        memcpy(slot->data, data, length);
        slot->length = length;
        env->messages.count++;
    }
}

/**
 * Bring up the fixture engine over caller-owned storage.
 *
 * @param config   Optional engine config (timers, block size, wait limit);
 *                 NULL selects every default. The fixture always binds the
 *                 fault queue, the sink and a deterministic virtual clock.
 */
static inline void tp_test_env_init(tp_test_env_t *env, const cancestry_tp_config_t *config)
{
    cancestry_tp_config_t cfg;

    memset(env, 0, sizeof(*env));
    if (config != NULL) {
        cfg = *config;
    } else {
        memset(&cfg, 0, sizeof(cfg));
    }
    cfg.source_id = TP_TEST_SOURCE_ID;

    env->sink.user_data = env;
    env->sink.on_frame_tx = tp_test_on_frame_tx;
    env->sink.on_message = tp_test_on_message;

    cancestry_virtual_clock_init(&env->clock, 0u);
    env->clock_binding = cancestry_clock_from_virtual(&env->clock);

    (void)cancestry_event_queue_init(&env->queue, env->queue_slots, TP_TEST_QUEUE_DEPTH);
    CANCESSTRY_TEST_CHECK(cancestry_transport_init(&env->tp, &cfg, env->rx_buffer,
                                                   TP_TEST_RX_CAPACITY, env->tx_buffer,
                                                   TP_TEST_TX_CAPACITY, &env->sink, &env->queue,
                                                   &env->clock_binding));
}

/**
 * Advance virtual time and tick the engine: exactly 1 ms per tick
 * (SW-FR-TP-006). All ticks run even when one reports a timeout, so the
 * observable end state is final; the first non-OK status is returned.
 */
static inline cancestry_tp_status_t tp_test_advance(tp_test_env_t *env, uint32_t ticks)
{
    cancestry_tp_status_t first = CANCESTRY_TP_OK;
    uint32_t i;

    for (i = 0u; i < ticks; ++i) {
        cancestry_tp_status_t status;

        cancestry_virtual_clock_advance(&env->clock, 1000u);
        status = cancestry_transport_tick(&env->tp);
        if (status != CANCESTRY_TP_OK && first == CANCESTRY_TP_OK) {
            first = status;
        }
    }
    return first;
}

/** Feed one received frame (classic CAN, 1..8 bytes). */
static inline cancestry_tp_status_t tp_test_rx(tp_test_env_t *env, const uint8_t *data, uint8_t length)
{
    return cancestry_transport_rx_frame(&env->tp, data, length);
}

/** Build and feed a Single Frame carrying @p length payload bytes. */
static inline cancestry_tp_status_t tp_test_rx_sf(tp_test_env_t *env, const uint8_t *payload,
                                           uint8_t length)
{
    uint8_t frame[CANCESTRY_TP_FRAME_MAX_LENGTH];

    frame[0] = (uint8_t)(0x00u | length);
    memcpy(&frame[1], payload, length);
    return tp_test_rx(env, frame, (uint8_t)(length + 1u));
}

/** Build and feed a First Frame announcing @p total payload bytes. */
static inline cancestry_tp_status_t tp_test_rx_ff(tp_test_env_t *env, uint16_t total,
                                           const uint8_t *first_six)
{
    uint8_t frame[CANCESTRY_TP_FRAME_MAX_LENGTH];

    frame[0] = (uint8_t)(0x10u | ((total >> 8) & 0x0Fu));
    frame[1] = (uint8_t)(total & 0xFFu);
    memcpy(&frame[2], first_six, 6u);
    return tp_test_rx(env, frame, 8u);
}

/** Build and feed a Consecutive Frame with sequence number @p sn. */
static inline cancestry_tp_status_t tp_test_rx_cf(tp_test_env_t *env, uint8_t sn, const uint8_t *payload,
                                           uint8_t length)
{
    uint8_t frame[CANCESTRY_TP_FRAME_MAX_LENGTH];

    frame[0] = (uint8_t)(0x20u | (sn & 0x0Fu));
    memcpy(&frame[1], payload, length);
    return tp_test_rx(env, frame, (uint8_t)(length + 1u));
}

/** Build and feed a Flow Control frame. */
static inline cancestry_tp_status_t tp_test_rx_fc(tp_test_env_t *env, uint8_t flow_status, uint8_t bs,
                                           uint8_t stmin)
{
    uint8_t frame[3];

    frame[0] = (uint8_t)(0x30u | (flow_status & 0x0Fu));
    frame[1] = bs;
    frame[2] = stmin;
    return tp_test_rx(env, frame, 3u);
}

/** @return Number of FAULT_RAISED events currently queued. */
static inline size_t tp_test_fault_count(tp_test_env_t *env)
{
    return (size_t)cancestry_event_queue_fault_depth(&env->queue);
}

/**
 * Pop the next event; @return true when it is a FAULT_RAISED event whose
 * code was written to @p fault_out.
 */
static inline bool tp_test_pop_fault(tp_test_env_t *env, cancestry_tp_fault_code_t *fault_out)
{
    cancestry_event_t event;

    while (cancestry_event_queue_pop(&env->queue, &event) ==
           CANCESTRY_EVENT_QUEUE_OK) {
        if (event.type == CANCESTRY_EVENT_TYPE_FAULT_RAISED) {
            *fault_out = (cancestry_tp_fault_code_t)(event.payload.fault.fault_code & 0xFFFFu);
            return true;
        }
    }
    return false;
}

/** Drain the queue; @return the number of FAULT_RAISED events seen. */
static inline size_t tp_test_drain_faults(tp_test_env_t *env)
{
    cancestry_tp_fault_code_t fault;
    size_t count = 0u;

    while (tp_test_pop_fault(env, &fault)) {
        count++;
    }
    return count;
}

#endif /* CANCESTRY_TP_CONFORMANCE_FIXTURE_H */
