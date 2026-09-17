/*
 * CANcestry - top-level gateway integration harness (Phase 5, issue #13).
 *
 * Implements:
 *   SYS-FR-003/004  decode/encode through the codec engine end to end
 *   SYS-FR-005/006  recipe and FSM execution in one gateway loop
 *   SYS-FR-010      one shared signal namespace over the codec maps
 *   SYS-FR-014      determinism: two independent runs are byte-identical
 *   SYS-NF-001      same event sequence -> same output sequence
 *   SYS-NF-002      zero heap allocation in the processing loop; every buffer
 *                   is statically allocated or caller-owned
 *   SYS-NF-005      counters for frames, events, drops and governor violations
 *   SW-FR-EVENT-004/006  the bounded event queue and its selection order
 *   SW-FR-FSM-024/038    fail-closed: an expression fault suspends the
 *                        offending instance and the loop continues
 *   SW-FR-FSM-042, SW-FR-GOV-005/006  every side effect passes the governor;
 *                        a denial is recorded, never silently dropped
 *   SW-FR-FSM-046        deterministic FSM behavior
 *
 * Test ids (docs/trace/traceability.csv):
 *   GATEWAY-LOOP-001         SYS-FR-003/004/005/006, SW-FR-EVENT-004/006
 *   GATEWAY-DETERMINISM-001  SYS-NF-001, SYS-FR-014, SW-FR-FSM-046
 *   GATEWAY-NO-ALLOC-001     SYS-NF-002
 *   GATEWAY-FAILCLOSED-001   SW-FR-FSM-024, SW-FR-GOV-005/006, SYS-SF-002
 *   GATEWAY-COUNTERS-001     SYS-NF-005
 *   GATEWAY-TRACE-001        SW-FR-FSM-053, SYS-FR-019
 *
 * The harness runs the full mock cycle named by issue #13:
 *
 *   raw CAN frame ingress -> core/codec decode -> core/event queue ->
 *   core/recipe + core/fsm state transitions -> core/codec encode ->
 *   egress sink
 *
 * Ingress is a fixed sequence of frames on interface can0. The codec decodes
 * them into signal values, the gateway publishes one signal_changed event per
 * changed signal on the shared event bus (core/event), the drain loop
 * dispatches every bus event to the recipe engine (speed mirroring) and the
 * FSM engine (overspeed latch plus a deliberately broken watcher), and
 * approved side effects leave through the engines' sinks as encoded frames -
 * the FSM's and recipe's send_message paths encode through core/codec.
 *
 * Fail-closed demonstration (issue #13 constraint 3):
 *   - watcher.broken carries a guard that faults at run time; the engine
 *     suspends that instance (SW-FR-FSM-038), the gateway logs it and keeps
 *     draining the queue (SW-FR-FSM-024, SW-FR-FSM-047 isolation);
 *   - the governor denies the AlertFrame transmission (rate-limit shape),
 *     which is counted and logged without partial effect;
 *   - a set_signal on a signal missing from the capability write allowlist
 *     is a recorded capability denial before the governor is consulted.
 *
 * Zero-allocation proof (issue #13 constraint 1): loading the declarative
 * files is the one phase that may allocate (the loaders do, by contract).
 * Everything from ingress through the final drain runs under an allocator
 * tripwire on glibc hosts; the tripwire path is skipped under AddressSanitizer
 * exactly like tests/unit/core/fsm/test_no_alloc.c, where the archive scan
 * (ci/check_no_alloc.py, registered for the core libraries and for this
 * object file) covers the property instead.
 *
 * Determinism proof (issue #13 constraint 2): the identical world is built
 * twice from the same definitions and run independently; the full output log
 * and the egress frames must compare byte-identical.
 *
 * Integration policy notes (documented, not guessed):
 *   - Dispatch order per popped event: recipe engine first, then FSM engine.
 *     No spec orders the two engines against each other yet; this harness
 *     pins one order and the golden output depends on it.
 *   - The FSM engine routes its own timer_expired and state_entered/exited
 *     events to instances internally (tick and subscription machinery), so
 *     the drain loop does not hand those two classes back to the FSM engine;
 *     re-injecting them would deliver them twice. Every other class is
 *     dispatched to both engines.
 *   - This file never calls an allocator; CI symbol-scans the compiled
 *     object (cancestry_gateway_no_alloc_symbols) in addition to the core
 *     archives.
 */

#include "cancestry/codec/decoder.h"
#include "cancestry/codec/encoder.h"
#include "cancestry/codec/loader.h"
#include "cancestry/codec/namespace.h"
#include "cancestry/event/clock.h"
#include "cancestry/event/queue.h"
#include "cancestry/event/types.h"
#include "cancestry/fsm/engine.h"
#include "cancestry/fsm/loader.h"
#include "cancestry/recipe/engine.h"
#include "cancestry/recipe/loader.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------------- */
/* Allocator tripwire (glibc hosts, disabled under ASan; see file header)    */
/* ------------------------------------------------------------------------- */

#if defined(__GLIBC__) && !defined(__SANITIZE_ADDRESS__)
#if defined(__has_feature)
#if __has_feature(address_sanitizer)
#define GATEWAY_SANITIZER_ACTIVE 1
#endif
#endif
#if !defined(GATEWAY_SANITIZER_ACTIVE)
#define GATEWAY_ALLOC_TRIPWIRE 1
#endif
#endif

#if defined(GATEWAY_ALLOC_TRIPWIRE)

extern void *__libc_malloc(size_t size);
extern void *__libc_calloc(size_t count, size_t size);
extern void *__libc_realloc(void *ptr, size_t size);
extern void __libc_free(void *ptr);

static int gateway_alloc_guard = 0;
static unsigned long gateway_alloc_calls = 0u;

void *malloc(size_t size)
{
    if (gateway_alloc_guard != 0) {
        gateway_alloc_calls++;
    }
    return __libc_malloc(size);
}

void *calloc(size_t count, size_t size)
{
    if (gateway_alloc_guard != 0) {
        gateway_alloc_calls++;
    }
    return __libc_calloc(count, size);
}

void *realloc(void *ptr, size_t size)
{
    if (gateway_alloc_guard != 0) {
        gateway_alloc_calls++;
    }
    return __libc_realloc(ptr, size);
}

void free(void *ptr)
{
    if (gateway_alloc_guard != 0 && ptr != NULL) {
        gateway_alloc_calls++;
    }
    __libc_free(ptr);
}

#endif /* GATEWAY_ALLOC_TRIPWIRE */

/* ------------------------------------------------------------------------- */
/* World dimensions (bounded resources, SYS-NF-002)                          */
/* ------------------------------------------------------------------------- */

#define GATEWAY_IFACE_CAN0 ((cancestry_interface_id_t)1u)
#define GATEWAY_IFACE_CAN1 ((cancestry_interface_id_t)2u)

#define GATEWAY_MSG_VEHICLE_STATUS ((uint32_t)0x120u)
#define GATEWAY_MSG_CLUSTER_DISPLAY ((uint32_t)0x321u)
#define GATEWAY_MSG_ALERT ((uint32_t)0x322u)

#define GATEWAY_BUS_CAPACITY ((uint16_t)64u)
#define GATEWAY_SIGNAL_SLOTS ((size_t)8u)
#define GATEWAY_FSM_INSTANCES ((size_t)2u)
#define GATEWAY_FSM_QUEUE_DEPTH ((uint16_t)64u)
#define GATEWAY_FSM_VARIABLES ((uint16_t)4u)
#define GATEWAY_FSM_TIMERS ((uint16_t)2u)
#define GATEWAY_FSM_TRACE ((uint16_t)128u)
#define GATEWAY_RECIPE_VARIABLES ((size_t)4u)
#define GATEWAY_EGRESS_CAPACITY ((size_t)8u)
#define GATEWAY_DECODE_CAPACITY ((size_t)8u)
#define GATEWAY_LOG_CAPACITY ((size_t)16384u)
#define GATEWAY_TRACE_TEXT_CAPACITY ((size_t)8192u)
#define GATEWAY_MAX_DISPATCHES ((uint32_t)1024u)
#define GATEWAY_TICKS ((uint32_t)20u)
#define GATEWAY_FRAME_PERIOD_US ((cancestry_time_us_t)1000u)

/* ------------------------------------------------------------------------- */
/* Declarative definitions (loaded at setup; loading may allocate)           */
/* ------------------------------------------------------------------------- */

/* Codec map for both buses, loaded against the v0.3.0 codec schema. */
static const char gateway_codec_yaml[] =
    "schema_version: \"0.3.0\"\n"
    "codec_map:\n"
    "  name: gateway_demo\n"
    "  version: 1.0.0\n"
    "  description: Two-bus gateway demo map\n"
    "  messages:\n"
    "    - id: 0x120\n"
    "      name: VehicleStatus\n"
    "      dlc: 8\n"
    "      signals:\n"
    "        - name: VehicleSpeed\n"
    "          start_bit: 0\n"
    "          bit_length: 16\n"
    "          type: uint\n"
    "          endianness: little\n"
    "        - name: IgnitionState\n"
    "          start_bit: 16\n"
    "          bit_length: 8\n"
    "          type: uint\n"
    "          endianness: little\n"
    "    - id: 0x321\n"
    "      name: ClusterDisplay\n"
    "      dlc: 8\n"
    "      signals:\n"
    "        - name: DisplaySpeed\n"
    "          start_bit: 0\n"
    "          bit_length: 16\n"
    "          type: uint\n"
    "          endianness: little\n"
    "        - name: WarnLamp\n"
    "          start_bit: 16\n"
    "          bit_length: 8\n"
    "          type: uint\n"
    "          endianness: little\n"
    "    - id: 0x322\n"
    "      name: AlertFrame\n"
    "      dlc: 1\n"
    "      signals:\n"
    "        - name: AlertLevel\n"
    "          start_bit: 0\n"
    "          bit_length: 8\n"
    "          type: uint\n"
    "          endianness: little\n";

/*
 * FSM file against the finalized v0.3.0 schema (issue #13): SpeedGate latches
 * an overspeed alert at 200, with a periodic heartbeat timer; BrokenWatcher
 * carries a guard that faults at run time (undefined identifier), so the
 * engine suspends that instance while the rest of the loop continues
 * (SW-FR-FSM-024/038).
 */
static const char gateway_fsm_yaml[] =
    "schema_version: \"0.3.0\"\n"
    "state_machines:\n"
    "  - name: SpeedGate\n"
    "    description: Latches an overspeed alert at 200\n"
    "    initial: NORMAL\n"
    "    variables:\n"
    "      - name: alerts\n"
    "        type: integer\n"
    "        default: 0\n"
    "      - name: beats\n"
    "        type: integer\n"
    "        default: 0\n"
    "    timers:\n"
    "      - name: heartbeat\n"
    "        duration_ms: 10\n"
    "        repeat: true\n"
    "        auto_start: true\n"
    "    states:\n"
    "      - name: NORMAL\n"
    "        entry:\n"
    "          - log:\n"
    "              level: info\n"
    "              message: gate normal\n"
    "        transitions:\n"
    "          - event: signal_changed\n"
    "            signal: VehicleSpeed\n"
    "            guard: \"sig.VehicleSpeed >= 200\"\n"
    "            target: ALERTING\n"
    "            actions:\n"
    "              - set_variable:\n"
    "                  variable: alerts\n"
    "                  value: \"var.alerts + 1\"\n"
    "              - send_message:\n"
    "                  interface: can1\n"
    "                  message: AlertFrame\n"
    "                  signals:\n"
    "                    AlertLevel: 2\n"
    "              - set_signal:\n"
    "                  signal: VehicleSpeed\n"
    "                  value: 0\n"
    "              - set_signal:\n"
    "                  signal: WarnLamp\n"
    "                  value: 1\n"
    "          - event: timer_expired\n"
    "            timer: heartbeat\n"
    "            target: NORMAL\n"
    "            actions:\n"
    "              - set_variable:\n"
    "                  variable: beats\n"
    "                  value: \"var.beats + 1\"\n"
    "      - name: ALERTING\n"
    "        entry:\n"
    "          - log:\n"
    "              level: warning\n"
    "              message: overspeed latched\n"
    "        transitions:\n"
    "          - event: signal_changed\n"
    "            signal: VehicleSpeed\n"
    "            guard: \"sig.VehicleSpeed < 100\"\n"
    "            target: NORMAL\n"
    "          - event: timer_expired\n"
    "            timer: heartbeat\n"
    "            target: ALERTING\n"
    "            actions:\n"
    "              - set_variable:\n"
    "                  variable: beats\n"
    "                  value: \"var.beats + 1\"\n"
    "  - name: BrokenWatcher\n"
    "    description: Deliberately broken guard for the fail-closed demo\n"
    "    initial: WATCH\n"
    "    states:\n"
    "      - name: WATCH\n"
    "        transitions:\n"
    "          - event: signal_changed\n"
    "            signal: VehicleSpeed\n"
    "            guard: \"sig.NoSuchSignal > 0\"\n"
    "            target: WATCH\n"
    "instances:\n"
    "  - id: gate.primary\n"
    "    machine: SpeedGate\n"
    "    enabled: true\n"
    "    subscriptions:\n"
    "      signals:\n"
    "        - VehicleSpeed\n"
    "  - id: watcher.broken\n"
    "    machine: BrokenWatcher\n"
    "    enabled: true\n"
    "    subscriptions:\n"
    "      signals:\n"
    "        - VehicleSpeed\n";

/*
 * Recipe file (recipe schema is 0.2.0 in v0.3.0): mirrors VehicleSpeed onto
 * the device bus as ClusterDisplay.DisplaySpeed - the classic gateway
 * forwarding behavior, conditions-free and direction-fixed by the trigger.
 */
static const char gateway_recipe_yaml[] =
    "schema_version: \"0.2.0\"\n"
    "recipes:\n"
    "  - name: speed_mirror\n"
    "    trigger:\n"
    "      event: signal_changed\n"
    "      signal: VehicleSpeed\n"
    "    actions:\n"
    "      - send_message:\n"
    "          interface: can1\n"
    "          message: ClusterDisplay\n"
    "          signals:\n"
    "            DisplaySpeed: \"sig.VehicleSpeed\"\n";

/* Fixed ingress sequence (issue #13 constraint 2): same frames every run. */
typedef struct gateway_frame {
    cancestry_interface_id_t interface_id;
    uint32_t can_id;
    uint8_t length;
    uint8_t data[CANCESTRY_CAN_FRAME_MAX_LENGTH];
} gateway_frame_t;

static const gateway_frame_t gateway_ingress_frames[] = {
    /* VehicleSpeed = 5, IgnitionState = 1. */
    {GATEWAY_IFACE_CAN0, GATEWAY_MSG_VEHICLE_STATUS, 8u,
     {0x05u, 0x00u, 0x01u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u}},
    /* VehicleSpeed = 120. */
    {GATEWAY_IFACE_CAN0, GATEWAY_MSG_VEHICLE_STATUS, 8u,
     {0x78u, 0x00u, 0x01u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u}},
    /* Too short for VehicleStatus: dropped with a codec warning. */
    {GATEWAY_IFACE_CAN0, GATEWAY_MSG_VEHICLE_STATUS, 1u,
     {0x99u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u}},
    /* Unknown CAN id: no message matches, frame is not forwarded. */
    {GATEWAY_IFACE_CAN0, 0x7FFu, 8u,
     {0xDEu, 0xADu, 0xBEu, 0xEFu, 0x00u, 0x01u, 0x02u, 0x03u}},
    /* VehicleSpeed = 260: overspeed, latches ALERTING. */
    {GATEWAY_IFACE_CAN0, GATEWAY_MSG_VEHICLE_STATUS, 8u,
     {0x04u, 0x01u, 0x01u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u}},
};

#define GATEWAY_INGRESS_COUNT \
    (sizeof(gateway_ingress_frames) / sizeof(gateway_ingress_frames[0]))

/* ------------------------------------------------------------------------- */
/* World: one complete gateway over static storage                           */
/* ------------------------------------------------------------------------- */

typedef struct gateway_egress_frame {
    const char *origin; /* "recipe" or "fsm" - which engine emitted it. */
    cancestry_interface_id_t interface_id;
    uint32_t can_id;
    uint8_t length;
    uint8_t data[CANCESTRY_CAN_FRAME_MAX_LENGTH];
} gateway_egress_frame_t;

typedef struct gateway_world {
    /* Loaded definitions (setup phase only; freed after the run). */
    cancestry_codec_map_t *codec_map;
    cancestry_fsm_set_t *fsm_set;
    cancestry_recipe_set_t *recipe_set;

    /* Shared signal namespace (SYS-FR-010). */
    const cancestry_codec_map_t *namespace_slots[1];
    cancestry_codec_namespace_t namespace;

    /* Shared signal value store: decode writes, engines read and write. */
    cancestry_recipe_signal_store_t signals;
    cancestry_recipe_signal_slot_t signal_slots[GATEWAY_SIGNAL_SLOTS];

    /* Global event bus (core/event). */
    cancestry_event_queue_t bus;
    cancestry_event_t bus_slots[GATEWAY_BUS_CAPACITY];

    /* Virtual monotonic clock: the only time base in the world. */
    cancestry_virtual_clock_t virtual_clock;
    cancestry_clock_t clock;

    /* FSM engine and caller-owned storage. */
    cancestry_fsm_engine_t fsm;
    cancestry_fsm_instance_t fsm_instances[GATEWAY_FSM_INSTANCES];
    cancestry_fsm_instance_storage_t fsm_storage[GATEWAY_FSM_INSTANCES];
    cancestry_event_t fsm_queue_slots[GATEWAY_FSM_INSTANCES][GATEWAY_FSM_QUEUE_DEPTH];
    cancestry_fsm_variable_slot_t fsm_variable_slots[GATEWAY_FSM_INSTANCES][GATEWAY_FSM_VARIABLES];
    cancestry_fsm_timer_state_t fsm_timer_slots[GATEWAY_FSM_INSTANCES][GATEWAY_FSM_TIMERS];
    cancestry_fsm_trace_record_t fsm_trace[GATEWAY_FSM_TRACE];
    char fsm_trace_text[GATEWAY_TRACE_TEXT_CAPACITY];

    /* FSM capabilities, signal bus adapter, sink. */
    cancestry_fsm_capabilities_t capabilities;
    cancestry_fsm_interface_capability_t interface_caps[2];
    uint32_t can1_tx_ids[2];
    const char *signal_read_allow[4];
    const char *signal_write_allow[2];
    cancestry_fsm_signal_bus_t fsm_bus;
    cancestry_fsm_sink_t fsm_sink;

    /* Recipe engine, environment tables, sink. */
    cancestry_recipe_engine_t recipe;
    cancestry_recipe_variable_t recipe_variables[GATEWAY_RECIPE_VARIABLES];
    cancestry_recipe_interface_t recipe_interfaces[2];
    cancestry_recipe_sink_t recipe_sink;

    /* Governor bookkeeping. */
    uint32_t fsm_governor_approvals;
    uint32_t fsm_governor_denials;
    uint32_t recipe_governor_approvals;

    /* Ingress/decode bookkeeping. */
    cancestry_codec_warnings_t codec_warnings;
    uint32_t frames_seen;
    uint32_t frames_decoded;
    uint32_t frames_unknown;
    uint32_t signals_emitted;
    uint32_t dispatches;
    bool overflow; /* dispatch bound exceeded -> fail */

    /* Egress record (bit-exact golden expectation). */
    gateway_egress_frame_t egress[GATEWAY_EGRESS_CAPACITY];
    size_t egress_count;

    /* Deterministic output log (golden text). */
    char log[GATEWAY_LOG_CAPACITY];
    size_t log_used;
} gateway_world_t;

/* ------------------------------------------------------------------------- */
/* Output log                                                                */
/* ------------------------------------------------------------------------- */

static void gateway_log(gateway_world_t *world, const char *format, ...)
{
    va_list args;
    int written;
    size_t remaining;

    if (world->log_used >= GATEWAY_LOG_CAPACITY - 1u) {
        return;
    }
    remaining = GATEWAY_LOG_CAPACITY - world->log_used;
    va_start(args, format);
    written = vsnprintf(world->log + world->log_used, remaining, format, args);
    va_end(args);
    if (written > 0) {
        size_t added = (size_t)written;

        if (added >= remaining) {
            added = remaining - 1u;
        }
        world->log_used += added;
    }
}

static const char *gateway_iface_name(cancestry_interface_id_t interface_id)
{
    if (interface_id == GATEWAY_IFACE_CAN0) {
        return "can0";
    }
    if (interface_id == GATEWAY_IFACE_CAN1) {
        return "can1";
    }
    return "can?";
}

/* ------------------------------------------------------------------------- */
/* Signal namespace helpers                                                  */
/* ------------------------------------------------------------------------- */

static cancestry_signal_id_t gateway_signal_id(const gateway_world_t *world, const char *name)
{
    cancestry_codec_resolution_t resolution;

    if (cancestry_codec_namespace_resolve(&world->namespace, name, &resolution) !=
        CANCESTRY_CODEC_OK) {
        return CANCESTRY_ID_NONE;
    }
    return resolution.signal_id;
}

static bool gateway_value_equals(const cancestry_value_t *lhs, const cancestry_value_t *rhs)
{
    if (lhs->kind != rhs->kind) {
        return false;
    }
    switch (lhs->kind) {
    case CANCESTRY_VALUE_KIND_BOOL:
        return lhs->value.boolean == rhs->value.boolean;
    case CANCESTRY_VALUE_KIND_INT:
        return lhs->value.integer == rhs->value.integer;
    case CANCESTRY_VALUE_KIND_UINT:
        return lhs->value.unsigned_integer == rhs->value.unsigned_integer;
    case CANCESTRY_VALUE_KIND_REAL:
        return lhs->value.real == rhs->value.real;
    default:
        return true; /* both UNSET */
    }
}

/* ------------------------------------------------------------------------- */
/* FSM signal bus adapter over the shared recipe signal store                */
/* ------------------------------------------------------------------------- */

static cancestry_fsm_status_t gateway_bus_read(void *user_data,
                                               const char *name,
                                               cancestry_value_t *value_out)
{
    gateway_world_t *world = (gateway_world_t *)user_data;
    cancestry_signal_id_t id = gateway_signal_id(world, name);

    if (id == CANCESTRY_ID_NONE) {
        return CANCESTRY_FSM_ERR_NOT_FOUND;
    }
    if (cancestry_recipe_signal_store_get(&world->signals, id, value_out) !=
        CANCESTRY_RECIPE_OK) {
        return CANCESTRY_FSM_ERR_NOT_FOUND;
    }
    return CANCESTRY_FSM_OK;
}

static cancestry_fsm_status_t gateway_bus_write(void *user_data,
                                                const char *name,
                                                const cancestry_value_t *value)
{
    gateway_world_t *world = (gateway_world_t *)user_data;
    cancestry_signal_id_t id = gateway_signal_id(world, name);

    if (id == CANCESTRY_ID_NONE) {
        return CANCESTRY_FSM_ERR_NOT_FOUND;
    }
    if (cancestry_recipe_signal_store_set(&world->signals, id, value) != CANCESTRY_RECIPE_OK) {
        return CANCESTRY_FSM_ERR_CAPACITY;
    }
    return CANCESTRY_FSM_OK;
}

static cancestry_fsm_status_t gateway_bus_resolve(void *user_data,
                                                  const char *name,
                                                  cancestry_signal_id_t *id_out)
{
    gateway_world_t *world = (gateway_world_t *)user_data;
    cancestry_signal_id_t id = gateway_signal_id(world, name);

    if (id == CANCESTRY_ID_NONE) {
        return CANCESTRY_FSM_ERR_NOT_FOUND;
    }
    *id_out = id;
    return CANCESTRY_FSM_OK;
}

/* ------------------------------------------------------------------------- */
/* Governor stubs (docs/system/governor.md, stub level; fail closed)         */
/* ------------------------------------------------------------------------- */

/*
 * The FSM-side governor models a TX rate/allowlist policy: AlertFrame
 * (0x322) is refused, everything else is approved. A denial is recorded by
 * the engine and leaves no partial effect (SW-FR-FSM-042, SW-FR-GOV-006).
 */
static cancestry_fsm_governor_decision_t gateway_fsm_governor(
    void *user_data, const cancestry_fsm_governor_request_t *request)
{
    gateway_world_t *world = (gateway_world_t *)user_data;

    if (request->kind == CANCESTRY_FSM_GOVERNOR_SEND_MESSAGE &&
        request->can_id == GATEWAY_MSG_ALERT) {
        world->fsm_governor_denials++;
        return CANCESTRY_FSM_GOVERNOR_DENY;
    }
    world->fsm_governor_approvals++;
    return CANCESTRY_FSM_GOVERNOR_APPROVE;
}

/* The recipe-side governor approves the mirrored traffic. */
static cancestry_recipe_governor_decision_t gateway_recipe_governor(
    void *user_data, const cancestry_recipe_governor_request_t *request)
{
    gateway_world_t *world = (gateway_world_t *)user_data;

    (void)request;
    world->recipe_governor_approvals++;
    return CANCESTRY_RECIPE_GOVERNOR_APPROVE;
}

/* ------------------------------------------------------------------------- */
/* Egress record and sinks                                                   */
/* ------------------------------------------------------------------------- */

static void gateway_record_egress(gateway_world_t *world,
                                  const char *origin,
                                  cancestry_interface_id_t interface_id,
                                  uint32_t can_id,
                                  const uint8_t *frame,
                                  uint8_t frame_length)
{
    gateway_egress_frame_t *slot;
    size_t i;

    if (world->egress_count >= GATEWAY_EGRESS_CAPACITY) {
        world->overflow = true;
        return;
    }
    slot = &world->egress[world->egress_count];
    world->egress_count++;
    slot->origin = origin;
    slot->interface_id = interface_id;
    slot->can_id = can_id;
    slot->length = frame_length;
    memset(slot->data, 0, sizeof(slot->data));
    for (i = 0u; i < frame_length && i < CANCESTRY_CAN_FRAME_MAX_LENGTH; ++i) {
        slot->data[i] = frame[i];
    }
    gateway_log(world, "egress %s %s 0x%03X len=%u %02X%02X%02X%02X%02X%02X%02X%02X\n", origin,
                gateway_iface_name(interface_id), (unsigned)can_id, (unsigned)frame_length,
                (unsigned)frame[0], (unsigned)((frame_length > 1u) ? frame[1] : 0u),
                (unsigned)((frame_length > 2u) ? frame[2] : 0u),
                (unsigned)((frame_length > 3u) ? frame[3] : 0u),
                (unsigned)((frame_length > 4u) ? frame[4] : 0u),
                (unsigned)((frame_length > 5u) ? frame[5] : 0u),
                (unsigned)((frame_length > 6u) ? frame[6] : 0u),
                (unsigned)((frame_length > 7u) ? frame[7] : 0u));
}

static void gateway_fsm_sink_send(void *user_data,
                                  const cancestry_fsm_invocation_t *invocation,
                                  const char *interface_name,
                                  cancestry_interface_id_t interface_id,
                                  const char *message_name,
                                  uint32_t can_id,
                                  const uint8_t *frame,
                                  uint8_t frame_length)
{
    gateway_world_t *world = (gateway_world_t *)user_data;

    (void)invocation;
    (void)interface_name;
    (void)message_name;
    gateway_record_egress(world, "fsm", interface_id, can_id, frame, frame_length);
}

static void gateway_fsm_sink_signal(void *user_data,
                                    const cancestry_fsm_invocation_t *invocation,
                                    const char *signal_name,
                                    cancestry_signal_id_t signal_id,
                                    const cancestry_value_t *old_value,
                                    const cancestry_value_t *new_value)
{
    gateway_world_t *world = (gateway_world_t *)user_data;

    (void)invocation;
    (void)signal_id;
    (void)old_value;
    if (new_value != NULL && new_value->kind == CANCESTRY_VALUE_KIND_INT) {
        gateway_log(world, "fsm signal %s set=%lld\n", signal_name,
                    (long long)new_value->value.integer);
    } else {
        gateway_log(world, "fsm signal %s set\n", signal_name);
    }
}

static void gateway_fsm_sink_log(void *user_data,
                                 const cancestry_fsm_invocation_t *invocation,
                                 cancestry_fsm_log_level_t level,
                                 const char *message)
{
    gateway_world_t *world = (gateway_world_t *)user_data;
    const char *instance_id = "(engine)";

    if (invocation != NULL && invocation->def != NULL && invocation->def->id != NULL) {
        instance_id = invocation->def->id;
    }
    gateway_log(world, "fsm log %s %s %s\n", instance_id, cancestry_fsm_log_level_name(level),
                message);
}

static void gateway_fsm_sink_fault(void *user_data,
                                   const cancestry_fsm_invocation_t *invocation,
                                   const char *code,
                                   cancestry_fault_severity_t severity,
                                   cancestry_fault_code_t numeric_code)
{
    gateway_world_t *world = (gateway_world_t *)user_data;

    (void)invocation;
    gateway_log(world, "fsm fault %s %s code=%u\n", code,
                cancestry_fault_severity_name(severity), (unsigned)numeric_code);
}

static void gateway_fsm_sink_state(void *user_data,
                                   const cancestry_fsm_invocation_t *invocation,
                                   const char *from,
                                   const char *to)
{
    gateway_world_t *world = (gateway_world_t *)user_data;
    const char *instance_id = "(engine)";

    if (invocation != NULL && invocation->def != NULL && invocation->def->id != NULL) {
        instance_id = invocation->def->id;
    }
    gateway_log(world, "fsm state %s %s->%s\n", instance_id,
                (from != NULL) ? from : "(initial)", to);
}

static void gateway_fsm_sink_warning(void *user_data,
                                     const cancestry_fsm_invocation_t *invocation,
                                     const char *text)
{
    gateway_world_t *world = (gateway_world_t *)user_data;
    const char *instance_id = "(engine)";

    if (invocation != NULL && invocation->def != NULL && invocation->def->id != NULL) {
        instance_id = invocation->def->id;
    }
    gateway_log(world, "fsm warn %s %s\n", instance_id, text);
}

static void gateway_recipe_sink_send(void *user_data,
                                     const cancestry_recipe_invocation_t *invocation,
                                     cancestry_interface_id_t interface_id,
                                     const char *interface_name,
                                     uint32_t can_id,
                                     const uint8_t *frame,
                                     uint8_t frame_length)
{
    gateway_world_t *world = (gateway_world_t *)user_data;

    (void)invocation;
    (void)interface_name;
    gateway_record_egress(world, "recipe", interface_id, can_id, frame, frame_length);
}

static void gateway_recipe_sink_log(void *user_data,
                                    const cancestry_recipe_invocation_t *invocation,
                                    cancestry_recipe_log_level_t level,
                                    const char *message)
{
    gateway_world_t *world = (gateway_world_t *)user_data;
    const char *recipe_name = "(recipe)";

    (void)level;
    if (invocation != NULL && invocation->recipe != NULL && invocation->recipe->name != NULL) {
        recipe_name = invocation->recipe->name;
    }
    gateway_log(world, "recipe log %s %s\n", recipe_name, message);
}

static void gateway_recipe_sink_fault(void *user_data,
                                      const cancestry_recipe_invocation_t *invocation,
                                      const char *code,
                                      cancestry_fault_severity_t severity,
                                      cancestry_fault_code_t numeric_code)
{
    gateway_world_t *world = (gateway_world_t *)user_data;

    (void)invocation;
    gateway_log(world, "recipe fault %s %s code=%u\n", code,
                cancestry_fault_severity_name(severity), (unsigned)numeric_code);
}

/* ------------------------------------------------------------------------- */
/* World setup (the only phase that may allocate: the loaders do)            */
/* ------------------------------------------------------------------------- */

static bool gateway_world_init(gateway_world_t *world)
{
    cancestry_codec_load_error_t codec_error;
    cancestry_fsm_load_error_t fsm_error;
    cancestry_recipe_load_error_t recipe_error;
    cancestry_fsm_engine_config_t fsm_config;
    cancestry_recipe_engine_config_t recipe_config;
    size_t i;

    memset(world, 0, sizeof(*world));

    world->codec_map = cancestry_codec_map_load(gateway_codec_yaml,
                                                sizeof(gateway_codec_yaml) - 1u, &codec_error);
    if (world->codec_map == NULL) {
        printf("FAIL codec map load: %s (line %u)\n", codec_error.message,
               (unsigned)codec_error.line);
        return false;
    }
    world->fsm_set = cancestry_fsm_set_load(gateway_fsm_yaml, sizeof(gateway_fsm_yaml) - 1u,
                                            &fsm_error);
    if (world->fsm_set == NULL) {
        printf("FAIL fsm load: %s (line %u)\n", fsm_error.message, (unsigned)fsm_error.line);
        return false;
    }
    world->recipe_set = cancestry_recipe_set_load(gateway_recipe_yaml,
                                                  sizeof(gateway_recipe_yaml) - 1u,
                                                  &recipe_error);
    if (world->recipe_set == NULL) {
        printf("FAIL recipe load: %s (line %u)\n", recipe_error.message,
               (unsigned)recipe_error.line);
        return false;
    }

    if (!cancestry_codec_namespace_init(&world->namespace, world->namespace_slots, 1u)) {
        printf("FAIL namespace init\n");
        return false;
    }
    world->namespace_slots[0] = world->codec_map;
    if (cancestry_codec_namespace_register(&world->namespace, world->codec_map) !=
        CANCESTRY_CODEC_OK) {
        printf("FAIL namespace register\n");
        return false;
    }
    if (!cancestry_recipe_signal_store_init(&world->signals, world->signal_slots,
                                            GATEWAY_SIGNAL_SLOTS)) {
        printf("FAIL signal store init\n");
        return false;
    }
    if (!cancestry_event_queue_init(&world->bus, world->bus_slots, GATEWAY_BUS_CAPACITY)) {
        printf("FAIL bus init\n");
        return false;
    }
    cancestry_virtual_clock_init(&world->virtual_clock, 0u);
    world->clock = cancestry_clock_from_virtual(&world->virtual_clock);

    /* Capabilities: can0 RX only, can1 TX for the two display/alert ids.
     * Signal reads for the demo inputs, writes only for the two outputs -
     * VehicleSpeed is deliberately not writable, which the FSM's denied
     * set_signal demonstrates. NoSuchSignal is not in the read allowlist,
     * which is what faults the BrokenWatcher guard. */
    world->can1_tx_ids[0] = GATEWAY_MSG_CLUSTER_DISPLAY;
    world->can1_tx_ids[1] = GATEWAY_MSG_ALERT;
    world->interface_caps[0].name = "can0";
    world->interface_caps[0].id = GATEWAY_IFACE_CAN0;
    world->interface_caps[0].rx = true;
    world->interface_caps[0].tx = false;
    world->interface_caps[0].tx_ids = NULL;
    world->interface_caps[0].tx_id_count = 0u;
    world->interface_caps[1].name = "can1";
    world->interface_caps[1].id = GATEWAY_IFACE_CAN1;
    world->interface_caps[1].rx = true;
    world->interface_caps[1].tx = true;
    world->interface_caps[1].tx_ids = world->can1_tx_ids;
    world->interface_caps[1].tx_id_count = 2u;
    world->signal_read_allow[0] = "VehicleSpeed";
    world->signal_read_allow[1] = "IgnitionState";
    world->signal_read_allow[2] = "DisplaySpeed";
    world->signal_read_allow[3] = "WarnLamp";
    world->signal_write_allow[0] = "DisplaySpeed";
    world->signal_write_allow[1] = "WarnLamp";
    world->capabilities.interfaces = world->interface_caps;
    world->capabilities.interface_count = 2u;
    world->capabilities.signal_read = world->signal_read_allow;
    world->capabilities.signal_read_count = 4u;
    world->capabilities.signal_write = world->signal_write_allow;
    world->capabilities.signal_write_count = 2u;

    world->fsm_bus.user_data = world;
    world->fsm_bus.read = gateway_bus_read;
    world->fsm_bus.write = gateway_bus_write;
    world->fsm_bus.resolve = gateway_bus_resolve;

    world->fsm_sink.user_data = world;
    world->fsm_sink.on_send_message = gateway_fsm_sink_send;
    world->fsm_sink.on_signal_write = gateway_fsm_sink_signal;
    world->fsm_sink.on_log = gateway_fsm_sink_log;
    world->fsm_sink.on_fault = gateway_fsm_sink_fault;
    world->fsm_sink.on_state_change = gateway_fsm_sink_state;
    world->fsm_sink.on_warning = gateway_fsm_sink_warning;

    for (i = 0u; i < GATEWAY_FSM_INSTANCES; ++i) {
        world->fsm_storage[i].event_slots = world->fsm_queue_slots[i];
        world->fsm_storage[i].event_capacity = GATEWAY_FSM_QUEUE_DEPTH;
        world->fsm_storage[i].variables = world->fsm_variable_slots[i];
        world->fsm_storage[i].variable_capacity = GATEWAY_FSM_VARIABLES;
        world->fsm_storage[i].timers = world->fsm_timer_slots[i];
        world->fsm_storage[i].timer_capacity = GATEWAY_FSM_TIMERS;
    }

    memset(&fsm_config, 0, sizeof(fsm_config));
    fsm_config.sets = world->fsm_set;
    fsm_config.set_count = 1u;
    fsm_config.instances = world->fsm_instances;
    fsm_config.storage = world->fsm_storage;
    fsm_config.instance_capacity = GATEWAY_FSM_INSTANCES;
    fsm_config.clock = &world->clock;
    fsm_config.capabilities = &world->capabilities;
    fsm_config.namespace = &world->namespace;
    fsm_config.signal_bus = &world->fsm_bus;
    fsm_config.global_queue = &world->bus;
    fsm_config.governor = gateway_fsm_governor;
    fsm_config.governor_user_data = world;
    fsm_config.sink = &world->fsm_sink;
    fsm_config.trace = world->fsm_trace;
    fsm_config.trace_capacity = GATEWAY_FSM_TRACE;
    fsm_config.record_events = true;
    if (!cancestry_fsm_engine_init(&world->fsm, &fsm_config)) {
        printf("FAIL fsm engine init\n");
        return false;
    }

    world->recipe_interfaces[0].name = "can0";
    world->recipe_interfaces[0].id = GATEWAY_IFACE_CAN0;
    world->recipe_interfaces[1].name = "can1";
    world->recipe_interfaces[1].id = GATEWAY_IFACE_CAN1;
    world->recipe_sink.user_data = world;
    world->recipe_sink.on_send_message = gateway_recipe_sink_send;
    world->recipe_sink.on_log = gateway_recipe_sink_log;
    world->recipe_sink.on_fault = gateway_recipe_sink_fault;

    memset(&recipe_config, 0, sizeof(recipe_config));
    recipe_config.sets = world->recipe_set;
    recipe_config.set_count = 1u;
    recipe_config.signal_namespace = &world->namespace;
    recipe_config.interfaces = world->recipe_interfaces;
    recipe_config.interface_count = 2u;
    recipe_config.signal_store = &world->signals;
    recipe_config.event_queue = &world->bus;
    recipe_config.sink = &world->recipe_sink;
    recipe_config.governor = gateway_recipe_governor;
    recipe_config.governor_user_data = world;
    recipe_config.variable_storage = world->recipe_variables;
    recipe_config.variable_capacity = GATEWAY_RECIPE_VARIABLES;
    if (!cancestry_recipe_engine_init(&world->recipe, &recipe_config)) {
        printf("FAIL recipe engine init\n");
        return false;
    }

    /* READY -> RUNNING for every enabled instance, in declaration order. */
    for (i = 0u; i < cancestry_fsm_engine_instance_count(&world->fsm); ++i) {
        cancestry_fsm_instance_t *instance = cancestry_fsm_instance_at(&world->fsm, i);

        if (instance != NULL &&
            cancestry_fsm_instance_lifecycle(instance) ==
                CANCESTRY_FSM_INSTANCE_LIFECYCLE_READY &&
            cancestry_fsm_instance_start(&world->fsm, instance) != CANCESTRY_FSM_OK) {
            printf("FAIL starting instance %u\n", (unsigned)i);
            return false;
        }
    }
    gateway_log(world, "setup: maps=1 recipes=1 machines=%u instances=%u\n",
                (unsigned)world->fsm_set->machine_count,
                (unsigned)world->fsm_set->instance_count);
    return true;
}

static void gateway_world_teardown(gateway_world_t *world)
{
    if (world->recipe_set != NULL) {
        cancestry_recipe_set_free(world->recipe_set);
        world->recipe_set = NULL;
    }
    if (world->fsm_set != NULL) {
        cancestry_fsm_set_free(world->fsm_set);
        world->fsm_set = NULL;
    }
    if (world->codec_map != NULL) {
        cancestry_codec_map_free(world->codec_map);
        world->codec_map = NULL;
    }
}

/* ------------------------------------------------------------------------- */
/* Ingress: raw frame -> codec decode -> signal_changed events on the bus    */
/* ------------------------------------------------------------------------- */

static void gateway_ingest_frame(gateway_world_t *world, const gateway_frame_t *frame)
{
    cancestry_decoded_signal_t decoded[GATEWAY_DECODE_CAPACITY];
    size_t count = 0u;
    cancestry_codec_status_t status;
    size_t i;

    world->frames_seen++;
    status = cancestry_codec_decode_frame(world->codec_map, frame->can_id, frame->data,
                                          frame->length, decoded, GATEWAY_DECODE_CAPACITY,
                                          &count, &world->codec_warnings);
    if (status == CANCESTRY_CODEC_WARN_FRAME_TOO_SHORT) {
        gateway_log(world, "ingress %s 0x%03X dlc=%u dropped=frame_too_short\n",
                    gateway_iface_name(frame->interface_id), (unsigned)frame->can_id,
                    (unsigned)frame->length);
        return;
    }
    if (status == CANCESTRY_CODEC_ERR_NOT_FOUND) {
        world->frames_unknown++;
        gateway_log(world, "ingress %s 0x%03X dlc=%u dropped=unknown_message\n",
                    gateway_iface_name(frame->interface_id), (unsigned)frame->can_id,
                    (unsigned)frame->length);
        return;
    }
    if (status != CANCESTRY_CODEC_OK) {
        gateway_log(world, "ingress %s 0x%03X dlc=%u decode_error=%s\n",
                    gateway_iface_name(frame->interface_id), (unsigned)frame->can_id,
                    (unsigned)frame->length, cancestry_codec_status_name(status));
        return;
    }
    world->frames_decoded++;
    gateway_log(world, "ingress %s 0x%03X dlc=%u decoded=%u\n",
                gateway_iface_name(frame->interface_id), (unsigned)frame->can_id,
                (unsigned)frame->length, (unsigned)count);

    /* Publish one signal_changed event per changed signal (the recipe
     * engine's set_signal policy: no event for a value that did not move). */
    for (i = 0u; i < count; ++i) {
        const cancestry_decoded_signal_t *signal = &decoded[i];
        cancestry_signal_id_t id = gateway_signal_id(world, signal->signal->name);
        cancestry_value_t old_value;
        bool had_old;
        cancestry_event_t event;
        cancestry_event_payload_t payload;

        if (id == CANCESTRY_ID_NONE) {
            gateway_log(world, "decode %s unresolved\n", signal->signal->name);
            continue;
        }
        had_old = cancestry_recipe_signal_store_get(&world->signals, id, &old_value) ==
                  CANCESTRY_RECIPE_OK;
        if (had_old && gateway_value_equals(&old_value, &signal->value)) {
            gateway_log(world, "decode %s unchanged\n", signal->signal->name);
            continue;
        }
        if (cancestry_recipe_signal_store_set(&world->signals, id, &signal->value) !=
            CANCESTRY_RECIPE_OK) {
            gateway_log(world, "decode %s store_full\n", signal->signal->name);
            continue;
        }
        memset(&payload, 0, sizeof(payload));
        payload.signal_changed.signal_id = id;
        payload.signal_changed.signal_name = signal->signal->name;
        payload.signal_changed.old_value = had_old ? old_value : payload.signal_changed.old_value;
        payload.signal_changed.new_value = signal->value;
        cancestry_event_init(&event);
        event.type = CANCESTRY_EVENT_TYPE_SIGNAL_CHANGED;
        event.priority_class = CANCESTRY_PRIORITY_CLASS_CAN_RX;
        event.timestamp_us = cancestry_clock_now_us(&world->clock);
        event.payload = payload;
        if (cancestry_event_queue_status_is_ok(cancestry_event_queue_push(&world->bus, &event))) {
            world->signals_emitted++;
            gateway_log(world, "emit %s id=%u\n", signal->signal->name, (unsigned)id);
        } else {
            gateway_log(world, "emit %s bus_full\n", signal->signal->name);
        }
    }
}

/* ------------------------------------------------------------------------- */
/* Drain: bus -> recipe engine + FSM engine (dispatch policy in file header) */
/* ------------------------------------------------------------------------- */

static bool gateway_event_for_fsm(cancestry_event_type_t type)
{
    /* timer_expired and state_entered/state_exited reach FSM instances
     * through the engine's own tick and subscription machinery; handing them
     * back would deliver them twice. */
    return type == CANCESTRY_EVENT_TYPE_CAN_RX ||
           type == CANCESTRY_EVENT_TYPE_SIGNAL_CHANGED ||
           type == CANCESTRY_EVENT_TYPE_FAULT_RAISED ||
           type == CANCESTRY_EVENT_TYPE_POWER_MODE_CHANGED;
}

static void gateway_drain_bus(gateway_world_t *world)
{
    cancestry_event_t event;

    while (!cancestry_event_queue_is_empty(&world->bus)) {
        const cancestry_event_t *head = cancestry_event_queue_peek(&world->bus);

        if (head == NULL) {
            break;
        }
        if (cancestry_event_queue_pop(&world->bus, &event) != CANCESTRY_EVENT_QUEUE_OK) {
            break;
        }
        world->dispatches++;
        if (world->dispatches > GATEWAY_MAX_DISPATCHES) {
            world->overflow = true;
            gateway_log(world, "drain dispatch bound exceeded\n");
            return;
        }
        gateway_log(world, "dispatch %s seq=%u\n", cancestry_event_type_name(event.type),
                    (unsigned)event.sequence);
        /* Dispatch order: recipe engine first, then FSM engine (pinned). */
        (void)cancestry_recipe_engine_process_event(&world->recipe, &event);
        if (gateway_event_for_fsm(event.type)) {
            (void)cancestry_fsm_engine_process_event(&world->fsm, &event);
        }
    }
}

/* ------------------------------------------------------------------------- */
/* One full mock cycle                                                       */
/* ------------------------------------------------------------------------- */

static void gateway_world_run(gateway_world_t *world)
{
    size_t i;
    uint32_t tick;
    cancestry_fsm_instance_t *gate;
    cancestry_fsm_instance_t *watcher;
    cancestry_fsm_instance_counters_t totals;
    const cancestry_fsm_engine_counters_t *fsm_counters;
    const cancestry_recipe_engine_counters_t *recipe_counters;
    cancestry_value_t value;
    size_t trace_length;

    for (i = 0u; i < GATEWAY_INGRESS_COUNT; ++i) {
        cancestry_virtual_clock_advance(&world->virtual_clock, GATEWAY_FRAME_PERIOD_US);
        gateway_ingest_frame(world, &gateway_ingress_frames[i]);
        gateway_drain_bus(world);
    }

    /* Idle ticks: the periodic heartbeat timer expires twice (at 10 ms and
     * 20 ms) within the next 20 ms. */
    for (tick = 0u; tick < GATEWAY_TICKS; ++tick) {
        cancestry_virtual_clock_advance(&world->virtual_clock, GATEWAY_FRAME_PERIOD_US);
        (void)cancestry_fsm_engine_tick(&world->fsm);
    }
    gateway_log(world, "ticks=%u now_us=%llu\n", (unsigned)GATEWAY_TICKS,
                (unsigned long long)cancestry_fsm_engine_now_us(&world->fsm));
    gateway_drain_bus(world);
    gateway_log(world, "bus_empty=%s\n",
                cancestry_event_queue_is_empty(&world->bus) ? "true" : "false");

    /* Final report: everything observable, in a fixed order. */
    fsm_counters = cancestry_fsm_engine_counters(&world->fsm);
    recipe_counters = cancestry_recipe_engine_counters(&world->recipe);
    (void)cancestry_fsm_engine_totals(&world->fsm, &totals);
    gate = cancestry_fsm_instance_by_id(&world->fsm, "gate.primary");
    watcher = cancestry_fsm_instance_by_id(&world->fsm, "watcher.broken");

    gateway_log(world,
                "report frames_seen=%u decoded=%u short=%u unknown=%u emitted=%u dispatches=%u\n",
                (unsigned)world->frames_seen, (unsigned)world->frames_decoded,
                (unsigned)world->codec_warnings.frame_too_short,
                (unsigned)world->frames_unknown, (unsigned)world->signals_emitted,
                (unsigned)world->dispatches);
    gateway_log(world, "report egress=%u recipe_invoked=%u recipe_sent=%u recipe_gov=%u\n",
                (unsigned)world->egress_count,
                (unsigned)recipe_counters->recipes_invoked,
                (unsigned)recipe_counters->messages_sent,
                (unsigned)world->recipe_governor_approvals);
    gateway_log(world,
                "report fsm_received=%u fsm_delivered=%u fsm_ignored=%u ticks=%u\n",
                (unsigned)fsm_counters->events_received,
                (unsigned)fsm_counters->events_delivered,
                (unsigned)fsm_counters->events_ignored, (unsigned)fsm_counters->ticks);
    gateway_log(world,
                "report transitions=%u guard_errors=%u cap_denials=%u gov_denials=%u "
                "action_errors=%u signals_set=%u timer_expiries=%u\n",
                (unsigned)totals.transitions, (unsigned)totals.guard_errors,
                (unsigned)totals.capability_denials, (unsigned)totals.governor_denials,
                (unsigned)totals.action_errors, (unsigned)totals.signals_set,
                (unsigned)totals.timer_expiries);
    {
        const cancestry_fsm_instance_counters_t *gc = cancestry_fsm_instance_counters(gate);
        const cancestry_fsm_instance_counters_t *wc = cancestry_fsm_instance_counters(watcher);

        if (gc != NULL && wc != NULL) {
            gateway_log(world,
                        "report gate(cap=%u gov=%u proc=%u sup=%u) watcher(cap=%u gov=%u "
                        "proc=%u sup=%u)\n",
                        (unsigned)gc->capability_denials, (unsigned)gc->governor_denials,
                        (unsigned)gc->events_processed, (unsigned)gc->events_suppressed,
                        (unsigned)wc->capability_denials, (unsigned)wc->governor_denials,
                        (unsigned)wc->events_processed, (unsigned)wc->events_suppressed);
        }
    }
    gateway_log(world, "report gate=%s(%s) watcher=%s(%s)\n",
                cancestry_fsm_instance_state_name(gate),
                cancestry_fsm_lifecycle_name(cancestry_fsm_instance_lifecycle(gate)),
                cancestry_fsm_instance_state_name(watcher),
                cancestry_fsm_lifecycle_name(cancestry_fsm_instance_lifecycle(watcher)));
    if (cancestry_fsm_instance_get_variable(&world->fsm, gate, "alerts", &value) ==
            CANCESTRY_FSM_OK &&
        value.kind == CANCESTRY_VALUE_KIND_INT) {
        gateway_log(world, "report alerts=%lld ", (long long)value.value.integer);
    } else {
        gateway_log(world, "report alerts=? ");
    }
    if (cancestry_fsm_instance_get_variable(&world->fsm, gate, "beats", &value) ==
            CANCESTRY_FSM_OK &&
        value.kind == CANCESTRY_VALUE_KIND_INT) {
        gateway_log(world, "beats=%lld\n", (long long)value.value.integer);
    } else {
        gateway_log(world, "beats=?\n");
    }
    if (cancestry_recipe_signal_store_get(
            &world->signals, gateway_signal_id(world, "WarnLamp"), &value) ==
        CANCESTRY_RECIPE_OK) {
        if (value.kind == CANCESTRY_VALUE_KIND_INT) {
            gateway_log(world, "report WarnLamp=%lld\n", (long long)value.value.integer);
        } else if (value.kind == CANCESTRY_VALUE_KIND_UINT) {
            gateway_log(world, "report WarnLamp=%llu\n",
                        (unsigned long long)value.value.unsigned_integer);
        } else {
            gateway_log(world, "report WarnLamp=?\n");
        }
    } else {
        gateway_log(world, "report WarnLamp=unset\n");
    }

    /* The FSM trace ring is the golden-output hook (SW-FR-FSM-053). */
    trace_length = cancestry_fsm_engine_trace_render(&world->fsm, world->fsm_trace_text,
                                                     sizeof(world->fsm_trace_text));
    gateway_log(world, "trace %u records dropped=%u\n%s",
                (unsigned)cancestry_fsm_engine_trace_count(&world->fsm),
                (unsigned)cancestry_fsm_engine_trace_dropped(&world->fsm),
                world->fsm_trace_text);
    (void)trace_length;
}

/* ------------------------------------------------------------------------- */
/* Assertions                                                                */
/* ------------------------------------------------------------------------- */

static int gateway_failures = 0;

static void gateway_expect(bool condition, const char *what)
{
    if (!condition) {
        gateway_failures++;
        printf("FAIL %s\n", what);
    }
}

/** Field-wise egress comparison (pointer members compared by content). */
static bool gateway_egress_equals(const gateway_world_t *lhs, const gateway_world_t *rhs)
{
    size_t i;

    if (lhs->egress_count != rhs->egress_count) {
        return false;
    }
    for (i = 0u; i < lhs->egress_count; ++i) {
        const gateway_egress_frame_t *a = &lhs->egress[i];
        const gateway_egress_frame_t *b = &rhs->egress[i];

        if (strcmp(a->origin, b->origin) != 0 || a->interface_id != b->interface_id ||
            a->can_id != b->can_id || a->length != b->length ||
            memcmp(a->data, b->data, sizeof(a->data)) != 0) {
            return false;
        }
    }
    return true;
}

static void gateway_expect_egress(const gateway_world_t *world)
{
    /* Exactly the three mirrored ClusterDisplay frames, bit-exact:
     * DisplaySpeed is the little-endian uint16 at bit 0 of 0x321. */
    static const uint8_t expected[3][CANCESTRY_CAN_FRAME_MAX_LENGTH] = {
        {0x05u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u},
        {0x78u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u},
        {0x04u, 0x01u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u},
    };
    size_t i;

    gateway_expect(world->egress_count == 3u, "exactly three egress frames");
    for (i = 0u; i < world->egress_count && i < 3u; ++i) {
        const gateway_egress_frame_t *frame = &world->egress[i];

        gateway_expect(frame->interface_id == GATEWAY_IFACE_CAN1, "egress on can1");
        gateway_expect(frame->can_id == GATEWAY_MSG_CLUSTER_DISPLAY, "egress is ClusterDisplay");
        gateway_expect(frame->length == CANCESTRY_CAN_FRAME_MAX_LENGTH, "egress dlc is 8");
        gateway_expect(memcmp(frame->data, expected[i], sizeof(expected[i])) == 0,
                       "egress payload is bit-exact");
        gateway_expect(strcmp(frame->origin, "recipe") == 0, "egress came from the recipe");
    }
}

static void gateway_expect_world(const gateway_world_t *world)
{
    const cancestry_fsm_instance_t *gate =
        cancestry_fsm_instance_by_id(&world->fsm, "gate.primary");
    const cancestry_fsm_instance_t *watcher =
        cancestry_fsm_instance_by_id(&world->fsm, "watcher.broken");
    const cancestry_fsm_instance_counters_t *gate_counters;
    const cancestry_fsm_instance_counters_t *watcher_counters;
    cancestry_fsm_instance_counters_t totals;
    cancestry_value_t value;

    gateway_expect(!world->overflow, "no bounded-storage overflow");
    gateway_expect(world->frames_seen == (uint32_t)GATEWAY_INGRESS_COUNT, "five frames seen");
    gateway_expect(world->frames_decoded == 3u, "three frames decoded");
    gateway_expect(world->codec_warnings.frame_too_short == 1u, "one short frame dropped");
    gateway_expect(world->frames_unknown == 1u, "one unknown-id frame dropped");

    gateway_expect_egress(world);

    /* FSM end state (issue #13 constraint 3): the broken instance is
     * suspended, the healthy one latched in ALERTING, and the loop finished. */
    gateway_expect(gate != NULL && watcher != NULL, "both instances exist");
    if (gate != NULL) {
        gateway_expect(cancestry_fsm_instance_lifecycle(gate) ==
                           CANCESTRY_FSM_INSTANCE_LIFECYCLE_RUNNING,
                       "gate.primary still running");
        gateway_expect(cancestry_fsm_instance_state_name(gate) != NULL &&
                           strcmp(cancestry_fsm_instance_state_name(gate), "ALERTING") == 0,
                       "gate.primary latched in ALERTING");
        gate_counters = cancestry_fsm_instance_counters(gate);
        gateway_expect(gate_counters->governor_denials == 1u,
                       "one governor denial (AlertFrame TX refused)");
        gateway_expect(gate_counters->capability_denials == 1u,
                       "one capability denial (VehicleSpeed write refused)");
        gateway_expect(gate_counters->action_errors == 2u, "both denials recorded as errors");
        gateway_expect(gate_counters->signals_set == 1u, "WarnLamp written once");
        gateway_expect(gate_counters->timer_expiries == 2u, "two heartbeat expiries");
        gateway_expect(cancestry_fsm_instance_get_variable(&world->fsm,
                                                           cancestry_fsm_instance_by_id(
                                                               &world->fsm, "gate.primary"),
                                                           "alerts", &value) == CANCESTRY_FSM_OK &&
                           value.kind == CANCESTRY_VALUE_KIND_INT && value.value.integer == 1,
                       "alerts == 1");
        gateway_expect(cancestry_fsm_instance_get_variable(&world->fsm,
                                                           cancestry_fsm_instance_by_id(
                                                               &world->fsm, "gate.primary"),
                                                           "beats", &value) == CANCESTRY_FSM_OK &&
                           value.kind == CANCESTRY_VALUE_KIND_INT && value.value.integer == 2,
                       "beats == 2");
    }
    if (watcher != NULL) {
        watcher_counters = cancestry_fsm_instance_counters(watcher);
        gateway_expect(cancestry_fsm_instance_lifecycle(watcher) ==
                           CANCESTRY_FSM_INSTANCE_LIFECYCLE_SUSPENDED,
                       "watcher.broken suspended by its expression fault");
        gateway_expect(watcher_counters->guard_errors == 1u, "one guard fault recorded");
        /* An unauthorized sig. read is both an expression fault and a
         * capability violation (SW-FR-FSM-041), so the engine counts it in
         * capability_denials as well. */
        gateway_expect(watcher_counters->capability_denials == 1u,
                       "the unauthorized signal read is counted as a capability denial");
    }
    gateway_expect(world->fsm_governor_denials == 1u, "FSM governor counted the denial");
    gateway_expect(world->recipe_governor_approvals == 3u, "recipe governor approved three TX");
    gateway_expect(cancestry_event_queue_is_empty(&world->bus), "bus drained to empty");

    /* Engine-wide totals pin the entire scenario in one place. */
    if (cancestry_fsm_engine_totals(&world->fsm, &totals) == CANCESTRY_FSM_OK) {
        gateway_expect(totals.transitions == 5u, "five FSM transitions in total");
        gateway_expect(totals.guard_errors == 1u, "one guard fault in total");
        gateway_expect(totals.capability_denials == 2u,
                       "two capability denials (VehicleSpeed write + unauthorized read)");
        gateway_expect(totals.governor_denials == 1u, "one governor denial in total");
        gateway_expect(totals.action_errors == 2u, "both denied actions recorded");
        gateway_expect(totals.signals_set == 1u, "WarnLamp written once");
        gateway_expect(totals.timer_expiries == 2u, "two heartbeat expiries");
    } else {
        gateway_expect(false, "engine totals readable");
    }
}

/* ------------------------------------------------------------------------- */
/* main                                                                      */
/* ------------------------------------------------------------------------- */

int main(void)
{
    static gateway_world_t world_a;
    static gateway_world_t world_b;
    bool ran_a = false;
    bool ran_b = false;

    printf("CANcestry gateway integration harness (issue #13, v0.3.0-rc.1)\n");
    fflush(stdout); /* warm the stdio stream before the allocator guard arms */

    if (!gateway_world_init(&world_a)) {
        gateway_world_teardown(&world_a);
        return 1;
    }

#if defined(GATEWAY_ALLOC_TRIPWIRE)
    gateway_alloc_calls = 0u;
    gateway_alloc_guard = 1;
    gateway_world_run(&world_a);
    gateway_alloc_guard = 0;
    ran_a = true;
    printf("allocator tripwire active: %lu allocation calls during the processing loop\n",
           gateway_alloc_calls);
    gateway_expect(gateway_alloc_calls == 0u,
                   "zero heap allocations during the processing loop");
#else
    gateway_world_run(&world_a);
    ran_a = true;
    printf("allocator tripwire unavailable here (ASan or non-glibc); "
           "the symbol scans cover the zero-allocation property\n");
#endif

    /* Second, fully independent run over the same definitions: the output
     * must be byte-identical (SYS-NF-001, SW-FR-FSM-046). */
    if (!gateway_world_init(&world_b)) {
        gateway_world_teardown(&world_a);
        gateway_world_teardown(&world_b);
        return 1;
    }
    gateway_world_run(&world_b);
    ran_b = true;

    gateway_expect(ran_a && ran_b, "both worlds ran");
    gateway_expect(world_a.log_used == world_b.log_used &&
                       memcmp(world_a.log, world_b.log, world_a.log_used) == 0,
                   "two independent runs are byte-identical (determinism)");
    gateway_expect(world_a.egress_count == world_b.egress_count &&
                       gateway_egress_equals(&world_a, &world_b),
                   "egress frames identical across runs");
    gateway_expect_world(&world_a);

    /* Golden output: stable across runs, suitable for diffing in review. */
    fputs("---- golden output (world A) ----\n", stdout);
    fwrite(world_a.log, 1u, world_a.log_used, stdout);
    fputs("---- end golden output ----\n", stdout);

    gateway_world_teardown(&world_a);
    gateway_world_teardown(&world_b);

    if (gateway_failures != 0) {
        printf("RESULT FAIL (%d failed expectations)\n", gateway_failures);
        return 1;
    }
    printf("RESULT PASS\n");
    return 0;
}
