/*
 * CANcestry - real CAN gateway harness over SocketCAN (Phase 6, issue #17).
 *
 * This example wires the deterministic core (codec/recipe/fsm) to a Linux
 * SocketCAN HAL instance running over two virtual CAN interfaces (vcan0,
 * vcan1). It demonstrates the complete loop:
 *
 *     physical frame RX on vcan0 -> decode -> signal_changed events ->
 *     recipe/FSM tick -> encode -> physical frame TX on vcan1
 *
 * To run, first create the vcan interfaces (see README.md) and pass their
 * names as argv[1] and argv[2]. If SocketCAN is not available (e.g., running
 * on a non-Linux host or in an unprivileged CI container) the binary prints
 * a SKIP message and exits 0 so the CTest wrapper can mark it skipped.
 *
 * Implements: SW-FR-HAL-003, SW-FR-HAL-004, SW-FR-HAL-005, SW-FR-HAL-010
 * Test id:    HAL-REAL-LOOP-001
 */

#define _DEFAULT_SOURCE 1
#define _POSIX_C_SOURCE 200809L

#include "cancestry/codec/decoder.h"
#include "cancestry/codec/encoder.h"
#include "cancestry/codec/loader.h"
#include "cancestry/codec/namespace.h"
#include "cancestry/event/clock.h"
#include "cancestry/event/queue.h"
#include "cancestry/event/types.h"
#include "cancestry/fsm/engine.h"
#include "cancestry/fsm/loader.h"
#include "cancestry/hal/hal.h"
#include "cancestry/hal/types.h"
#include "cancestry/recipe/engine.h"
#include "cancestry/recipe/loader.h"
#include "socketcan.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ------------------------------------------------------------------------- */
/* Bounds                                                                    */
/* ------------------------------------------------------------------------- */

#define REAL_IFACE_CAN0 ((cancestry_interface_id_t)1u)
#define REAL_IFACE_CAN1 ((cancestry_interface_id_t)2u)
#define REAL_BUS_CAPACITY ((uint16_t)128u)
#define REAL_RX_RING_CAP ((uint16_t)64u)
#define REAL_TX_RING_CAP ((uint16_t)64u)
#define REAL_SIGNAL_SLOTS ((size_t)8u)
#define REAL_FSM_INSTANCES ((size_t)1u)
#define REAL_FSM_QUEUE_DEPTH ((uint16_t)64u)
#define REAL_FSM_VARIABLES ((uint16_t)4u)
#define REAL_FSM_TIMERS ((uint16_t)2u)
#define REAL_FSM_TRACE ((uint16_t)64u)
#define REAL_RECIPE_VARIABLES ((size_t)4u)
#define REAL_DECODE_CAPACITY ((size_t)8u)
#define REAL_MAX_TICKS ((uint32_t)100u)

/* ------------------------------------------------------------------------- */
/* Codec / FSM / Recipe definitions (same schema as the mock gateway)       */
/* ------------------------------------------------------------------------- */

static const char real_codec_yaml[] =
    "schema_version: \"0.3.0\"\n"
    "codec_map:\n"
    "  name: real_gateway\n"
    "  version: 1.0.0\n"
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
    "    - id: 0x321\n"
    "      name: ClusterDisplay\n"
    "      dlc: 8\n"
    "      signals:\n"
    "        - name: DisplaySpeed\n"
    "          start_bit: 0\n"
    "          bit_length: 16\n"
    "          type: uint\n"
    "          endianness: little\n";

static const char real_recipe_yaml[] =
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

static const char real_fsm_yaml[] =
    "schema_version: \"0.3.0\"\n"
    "state_machines:\n"
    "  - name: Idle\n"
    "    initial: IDLE\n"
    "    states:\n"
    "      - name: IDLE\n"
    "        transitions:\n"
    "          - event: fault_raised\n"
    "            target: IDLE\n"
    "instances:\n"
    "  - id: idle.monitor\n"
    "    machine: Idle\n"
    "    enabled: true\n";

/* ------------------------------------------------------------------------- */
/* World                                                                     */
/* ------------------------------------------------------------------------- */

typedef struct real_world {
    cancestry_codec_map_t *codec_map;
    cancestry_fsm_set_t *fsm_set;
    cancestry_recipe_set_t *recipe_set;
    const cancestry_codec_map_t *namespace_slots[1];
    cancestry_codec_namespace_t namespace;
    cancestry_recipe_signal_store_t signals;
    cancestry_recipe_signal_slot_t signal_slots[REAL_SIGNAL_SLOTS];
    cancestry_event_queue_t bus;
    cancestry_event_t bus_slots[REAL_BUS_CAPACITY];
    cancestry_clock_t clock;

    /* HAL */
    cancestry_hal_t hal;
    cancestry_platform_socketcan_t sc_ctx;
    cancestry_hal_frame_t rx0_slots[REAL_RX_RING_CAP];
    cancestry_hal_frame_t rx1_slots[REAL_RX_RING_CAP];
    cancestry_hal_frame_t tx0_slots[REAL_TX_RING_CAP];
    cancestry_hal_frame_t tx1_slots[REAL_TX_RING_CAP];
    cancestry_hal_rx_ring_t rx0;
    cancestry_hal_rx_ring_t rx1;
    cancestry_hal_tx_ring_t tx0;
    cancestry_hal_tx_ring_t tx1;

    /* FSM + Recipe */
    cancestry_fsm_engine_t fsm;
    cancestry_fsm_instance_t fsm_instances[REAL_FSM_INSTANCES];
    cancestry_fsm_instance_storage_t fsm_storage[REAL_FSM_INSTANCES];
    cancestry_event_t fsm_queue_slots[REAL_FSM_INSTANCES][REAL_FSM_QUEUE_DEPTH];
    cancestry_fsm_variable_slot_t fsm_variable_slots[REAL_FSM_INSTANCES][REAL_FSM_VARIABLES];
    cancestry_fsm_timer_state_t fsm_timer_slots[REAL_FSM_INSTANCES][REAL_FSM_TIMERS];
    cancestry_fsm_trace_record_t fsm_trace[REAL_FSM_TRACE];
    cancestry_fsm_capabilities_t capabilities;
    cancestry_fsm_interface_capability_t interface_caps[2];
    uint32_t can1_tx_ids[1];
    const char *signal_read_allow[2];
    const char *signal_write_allow[2];
    cancestry_fsm_signal_bus_t fsm_bus;
    cancestry_fsm_sink_t fsm_sink;
    cancestry_recipe_engine_t recipe;
    cancestry_recipe_variable_t recipe_variables[REAL_RECIPE_VARIABLES];
    cancestry_recipe_interface_t recipe_interfaces[2];
    cancestry_recipe_sink_t recipe_sink;

    /* Counters */
    uint32_t frames_seen;
    uint32_t frames_tx;
    uint32_t faults_seen;
} real_world_t;

static real_world_t world;

/* ------------------- Sink callbacks with correct signatures ------------- */

static void real_fsm_send(void *u, const cancestry_fsm_invocation_t *inv,
                           const char *iname, cancestry_interface_id_t iid,
                           const char *mname, uint32_t can_id,
                           const uint8_t *data, uint8_t len)
{
    cancestry_hal_frame_t f;
    (void)u; (void)inv; (void)iname; (void)mname;
    memset(&f, 0, sizeof(f));
    f.interface_id = iid;
    f.can_id = can_id;
    f.length = len;
    memcpy(f.data, data, len);
    if (cancestry_hal_status_is_ok(cancestry_hal_send_tx(&world.hal, &world.bus, &f))) {
        world.frames_tx++;
    }
}

static void real_recipe_send(void *u, const cancestry_recipe_invocation_t *inv,
                              cancestry_interface_id_t iid, const char *iname,
                              uint32_t can_id, const uint8_t *data, uint8_t len)
{
    cancestry_hal_frame_t f;
    (void)u; (void)inv; (void)iname;
    memset(&f, 0, sizeof(f));
    f.interface_id = iid;
    f.can_id = can_id;
    f.length = len;
    memcpy(f.data, data, len);
    if (cancestry_hal_status_is_ok(cancestry_hal_send_tx(&world.hal, &world.bus, &f))) {
        world.frames_tx++;
    }
}

static void real_fsm_signal_write(void *u, const cancestry_fsm_invocation_t *inv,
                                   const char *name, cancestry_signal_id_t sid,
                                   const cancestry_value_t *ov, const cancestry_value_t *nv)
{
    (void)u; (void)inv; (void)name; (void)sid; (void)ov; (void)nv;
}
static void real_fsm_log(void *u, const cancestry_fsm_invocation_t *inv,
                          cancestry_fsm_log_level_t lvl, const char *msg)
{
    (void)u; (void)inv; (void)lvl; (void)msg;
}
static void real_fsm_fault(void *u, const cancestry_fsm_invocation_t *inv,
                            const char *code, cancestry_fault_severity_t sev,
                            cancestry_fault_code_t num)
{
    (void)u; (void)inv; (void)code; (void)sev; (void)num;
    world.faults_seen++;
}
static void real_fsm_state(void *u, const cancestry_fsm_invocation_t *inv,
                            const char *from, const char *to)
{
    (void)u; (void)inv; (void)from; (void)to;
}
static void real_fsm_warn(void *u, const cancestry_fsm_invocation_t *inv, const char *txt)
{
    (void)u; (void)inv; (void)txt;
}
static void real_recipe_log(void *u, const cancestry_recipe_invocation_t *inv,
                             cancestry_recipe_log_level_t lvl, const char *msg)
{
    (void)u; (void)inv; (void)lvl; (void)msg;
}
static void real_recipe_fault(void *u, const cancestry_recipe_invocation_t *inv,
                               const char *code, cancestry_fault_severity_t sev,
                               cancestry_fault_code_t num)
{
    (void)u; (void)inv; (void)code; (void)sev; (void)num;
    world.faults_seen++;
}

/* ------------------- Signal bus ----------------------------------------- */

static cancestry_signal_id_t real_signal_id(const char *name)
{
    cancestry_codec_resolution_t r;
    if (cancestry_codec_namespace_resolve(&world.namespace, name, &r) != CANCESTRY_CODEC_OK) {
        return CANCESTRY_ID_NONE;
    }
    return r.signal_id;
}

static cancestry_fsm_status_t real_bus_read(void *u, const char *name, cancestry_value_t *out)
{
    cancestry_signal_id_t id = real_signal_id(name);
    (void)u;
    if (id == CANCESTRY_ID_NONE) return CANCESTRY_FSM_ERR_NOT_FOUND;
    return cancestry_recipe_signal_store_get(&world.signals, id, out) == CANCESTRY_RECIPE_OK
               ? CANCESTRY_FSM_OK : CANCESTRY_FSM_ERR_NOT_FOUND;
}

static cancestry_fsm_status_t real_bus_write(void *u, const char *name,
                                              const cancestry_value_t *v)
{
    cancestry_signal_id_t id = real_signal_id(name);
    (void)u;
    if (id == CANCESTRY_ID_NONE) return CANCESTRY_FSM_ERR_NOT_FOUND;
    return cancestry_recipe_signal_store_set(&world.signals, id, v) == CANCESTRY_RECIPE_OK
               ? CANCESTRY_FSM_OK : CANCESTRY_FSM_ERR_CAPACITY;
}

static cancestry_fsm_status_t real_bus_resolve(void *u, const char *name,
                                                cancestry_signal_id_t *id_out)
{
    (void)u;
    *id_out = real_signal_id(name);
    return (*id_out == CANCESTRY_ID_NONE) ? CANCESTRY_FSM_ERR_NOT_FOUND : CANCESTRY_FSM_OK;
}

static cancestry_fsm_governor_decision_t real_fsm_gov(void *u,
    const cancestry_fsm_governor_request_t *r)
{ (void)u; (void)r; return CANCESTRY_FSM_GOVERNOR_APPROVE; }

static cancestry_recipe_governor_decision_t real_recipe_gov(void *u,
    const cancestry_recipe_governor_request_t *r)
{ (void)u; (void)r; return CANCESTRY_RECIPE_GOVERNOR_APPROVE; }

/* ------------------- World init ----------------------------------------- */

static void copy_name(char *dst, const char *src)
{
    size_t k = 0u;
    while (k < CANCESTRY_HAL_INTERFACE_NAME_MAX - 1u && src[k] != '\0') {
        dst[k] = src[k];
        k++;
    }
    dst[k] = '\0';
}

static bool world_init(const char *iface0, const char *iface1)
{
    cancestry_codec_load_error_t ce;
    cancestry_fsm_load_error_t fe;
    cancestry_recipe_load_error_t re;
    cancestry_fsm_engine_config_t fc;
    cancestry_recipe_engine_config_t rc;
    cancestry_hal_if_config_t ifaces[2];
    cancestry_hal_rx_ring_t *rx_rings[2];
    cancestry_hal_tx_ring_t *tx_rings[2];
    cancestry_hal_config_t hc;
    size_t i;

    memset(&world, 0, sizeof(world));

    world.codec_map = cancestry_codec_map_load(real_codec_yaml, sizeof(real_codec_yaml)-1, &ce);
    if (!world.codec_map) return false;
    world.fsm_set = cancestry_fsm_set_load(real_fsm_yaml, sizeof(real_fsm_yaml)-1, &fe);
    if (!world.fsm_set) return false;
    world.recipe_set = cancestry_recipe_set_load(real_recipe_yaml, sizeof(real_recipe_yaml)-1, &re);
    if (!world.recipe_set) return false;

    cancestry_codec_namespace_init(&world.namespace, world.namespace_slots, 1);
    world.namespace_slots[0] = world.codec_map;
    cancestry_codec_namespace_register(&world.namespace, world.codec_map);
    cancestry_recipe_signal_store_init(&world.signals, world.signal_slots, REAL_SIGNAL_SLOTS);
    cancestry_event_queue_init(&world.bus, world.bus_slots, REAL_BUS_CAPACITY);
    world.clock = cancestry_clock_platform();

    cancestry_hal_rx_ring_init(&world.rx0, world.rx0_slots, REAL_RX_RING_CAP);
    cancestry_hal_rx_ring_init(&world.rx1, world.rx1_slots, REAL_RX_RING_CAP);
    cancestry_hal_tx_ring_init(&world.tx0, world.tx0_slots, REAL_TX_RING_CAP);
    cancestry_hal_tx_ring_init(&world.tx1, world.tx1_slots, REAL_TX_RING_CAP);

    memset(ifaces, 0, sizeof(ifaces));
    ifaces[0].interface_id = REAL_IFACE_CAN0;
    ifaces[0].bitrate = 500000u;
    copy_name(ifaces[0].name, iface0);
    ifaces[1].interface_id = REAL_IFACE_CAN1;
    ifaces[1].bitrate = 500000u;
    copy_name(ifaces[1].name, iface1);
    rx_rings[0] = &world.rx0; rx_rings[1] = &world.rx1;
    tx_rings[0] = &world.tx0; tx_rings[1] = &world.tx1;

    cancestry_platform_socketcan_init(&world.sc_ctx);
    memset(&hc, 0, sizeof(hc));
    hc.backend = cancestry_platform_socketcan_backend();
    hc.backend_context = &world.sc_ctx;
    hc.clock = &world.clock;
    hc.ifaces = ifaces;
    hc.iface_count = 2u;
    hc.rx_rings = rx_rings;
    hc.tx_rings = tx_rings;

    if (!cancestry_hal_init(&world.hal, &hc)) {
        return false;
    }

    world.can1_tx_ids[0] = 0x321u;
    world.interface_caps[0].name = "can0"; world.interface_caps[0].id = REAL_IFACE_CAN0;
    world.interface_caps[0].rx = true; world.interface_caps[0].tx = false;
    world.interface_caps[0].tx_ids = NULL; world.interface_caps[0].tx_id_count = 0u;
    world.interface_caps[1].name = "can1"; world.interface_caps[1].id = REAL_IFACE_CAN1;
    world.interface_caps[1].rx = true; world.interface_caps[1].tx = true;
    world.interface_caps[1].tx_ids = world.can1_tx_ids; world.interface_caps[1].tx_id_count = 1u;
    world.signal_read_allow[0] = "VehicleSpeed"; world.signal_read_allow[1] = NULL;
    world.signal_write_allow[0] = "DisplaySpeed"; world.signal_write_allow[1] = NULL;
    world.capabilities.interfaces = world.interface_caps;
    world.capabilities.interface_count = 2u;
    world.capabilities.signal_read = world.signal_read_allow;
    world.capabilities.signal_read_count = 1u;
    world.capabilities.signal_write = world.signal_write_allow;
    world.capabilities.signal_write_count = 1u;

    world.fsm_bus.user_data = NULL;
    world.fsm_bus.read = real_bus_read;
    world.fsm_bus.write = real_bus_write;
    world.fsm_bus.resolve = real_bus_resolve;

    world.fsm_sink.user_data = NULL;
    world.fsm_sink.on_send_message = real_fsm_send;
    world.fsm_sink.on_signal_write = real_fsm_signal_write;
    world.fsm_sink.on_log = real_fsm_log;
    world.fsm_sink.on_fault = real_fsm_fault;
    world.fsm_sink.on_state_change = real_fsm_state;
    world.fsm_sink.on_warning = real_fsm_warn;

    world.recipe_interfaces[0].name = "can0"; world.recipe_interfaces[0].id = REAL_IFACE_CAN0;
    world.recipe_interfaces[1].name = "can1"; world.recipe_interfaces[1].id = REAL_IFACE_CAN1;
    world.recipe_sink.user_data = NULL;
    world.recipe_sink.on_send_message = real_recipe_send;
    world.recipe_sink.on_log = real_recipe_log;
    world.recipe_sink.on_fault = real_recipe_fault;

    for (i = 0u; i < REAL_FSM_INSTANCES; ++i) {
        world.fsm_storage[i].event_slots = world.fsm_queue_slots[i];
        world.fsm_storage[i].event_capacity = REAL_FSM_QUEUE_DEPTH;
        world.fsm_storage[i].variables = world.fsm_variable_slots[i];
        world.fsm_storage[i].variable_capacity = REAL_FSM_VARIABLES;
        world.fsm_storage[i].timers = world.fsm_timer_slots[i];
        world.fsm_storage[i].timer_capacity = REAL_FSM_TIMERS;
    }
    memset(&fc, 0, sizeof(fc));
    fc.sets = world.fsm_set; fc.set_count = 1u;
    fc.instances = world.fsm_instances; fc.storage = world.fsm_storage;
    fc.instance_capacity = REAL_FSM_INSTANCES; fc.clock = &world.clock;
    fc.capabilities = &world.capabilities; fc.namespace = &world.namespace;
    fc.signal_bus = &world.fsm_bus; fc.global_queue = &world.bus;
    fc.governor = real_fsm_gov; fc.governor_user_data = NULL;
    fc.sink = &world.fsm_sink; fc.trace = world.fsm_trace; fc.trace_capacity = REAL_FSM_TRACE;
    if (!cancestry_fsm_engine_init(&world.fsm, &fc)) return false;

    memset(&rc, 0, sizeof(rc));
    rc.sets = world.recipe_set; rc.set_count = 1u;
    rc.signal_namespace = &world.namespace; rc.interfaces = world.recipe_interfaces;
    rc.interface_count = 2u; rc.signal_store = &world.signals;
    rc.event_queue = &world.bus; rc.sink = &world.recipe_sink;
    rc.governor = real_recipe_gov; rc.variable_storage = world.recipe_variables;
    rc.variable_capacity = REAL_RECIPE_VARIABLES;
    if (!cancestry_recipe_engine_init(&world.recipe, &rc)) return false;

    for (i = 0u; i < cancestry_fsm_engine_instance_count(&world.fsm); ++i) {
        cancestry_fsm_instance_t *inst = cancestry_fsm_instance_at(&world.fsm, i);
        if (inst != NULL &&
            cancestry_fsm_instance_lifecycle(inst) == CANCESTRY_FSM_INSTANCE_LIFECYCLE_READY) {
            cancestry_fsm_instance_start(&world.fsm, inst);
        }
    }
    return true;
}

/* ------------------- Ingress / drain ------------------------------------ */

static void ingest_hal_frames(void)
{
    while (!cancestry_event_queue_is_empty(&world.bus)) {
        const cancestry_event_t *head = cancestry_event_queue_peek(&world.bus);
        cancestry_event_t ev;
        if (head == NULL) break;
        if (head->type != CANCESTRY_EVENT_TYPE_CAN_RX) break;
        if (cancestry_event_queue_pop(&world.bus, &ev) != CANCESTRY_EVENT_QUEUE_OK) break;
        if (ev.type == CANCESTRY_EVENT_TYPE_CAN_RX) {
            cancestry_decoded_signal_t decoded[REAL_DECODE_CAPACITY];
            size_t count = 0u;
            cancestry_codec_warnings_t cw;
            size_t j;
            memset(&cw, 0, sizeof(cw));
            world.frames_seen++;
            if (cancestry_codec_decode_frame(world.codec_map, ev.payload.can_rx.can_id,
                                              ev.payload.can_rx.data, ev.payload.can_rx.length,
                                              decoded, REAL_DECODE_CAPACITY, &count, &cw)
                == CANCESTRY_CODEC_OK) {
                for (j = 0; j < count; ++j) {
                    cancestry_signal_id_t id = real_signal_id(decoded[j].signal->name);
                    cancestry_value_t old;
                    bool had_old = cancestry_recipe_signal_store_get(&world.signals, id, &old)
                                       == CANCESTRY_RECIPE_OK;
                    cancestry_event_t scev;
                    cancestry_event_payload_t pl;
                    (void)had_old; (void)old;
                    cancestry_recipe_signal_store_set(&world.signals, id, &decoded[j].value);
                    memset(&pl, 0, sizeof(pl));
                    pl.signal_changed.signal_id = id;
                    pl.signal_changed.signal_name = decoded[j].signal->name;
                    pl.signal_changed.new_value = decoded[j].value;
                    cancestry_event_init(&scev);
                    scev.type = CANCESTRY_EVENT_TYPE_SIGNAL_CHANGED;
                    scev.priority_class = CANCESTRY_PRIORITY_CLASS_CAN_RX;
                    scev.timestamp_us = ev.timestamp_us;
                    scev.payload = pl;
                    cancestry_event_queue_push(&world.bus, &scev);
                }
            }
        }
    }
}

static void drain_bus(void)
{
    while (!cancestry_event_queue_is_empty(&world.bus)) {
        cancestry_event_t ev;
        if (cancestry_event_queue_pop(&world.bus, &ev) != CANCESTRY_EVENT_QUEUE_OK) break;
        (void)cancestry_recipe_engine_process_event(&world.recipe, &ev);
        if (ev.type != CANCESTRY_EVENT_TYPE_TIMER_EXPIRED &&
            ev.type != CANCESTRY_EVENT_TYPE_STATE_ENTERED &&
            ev.type != CANCESTRY_EVENT_TYPE_STATE_EXITED) {
            (void)cancestry_fsm_engine_process_event(&world.fsm, &ev);
        }
        if (ev.type == CANCESTRY_EVENT_TYPE_FAULT_RAISED) {
            world.faults_seen++;
        }
    }
}

/* ------------------- main ------------------------------------------------ */

int main(int argc, char **argv)
{
    const char *iface0 = (argc > 1) ? argv[1] : "vcan0";
    const char *iface1 = (argc > 2) ? argv[2] : "vcan1";
    uint32_t tick;

    printf("CANcestry real-gateway example (issue #17): %s -> %s\n", iface0, iface1);

    if (!world_init(iface0, iface1)) {
        printf("SKIP: unable to open SocketCAN interfaces (need vcan? see README)\n");
        return 0;
    }

    /* Inject a known VehicleStatus frame on the RX ring for vcan0 so the
     * loop is self-contained. In a production deployment the frame would
     * arrive from another ECU via hal_poll_rx. */
    {
        static const uint8_t data[8] = {0x78u, 0x00u, 0x01u, 0u,0u,0u,0u,0u};
        cancestry_hal_frame_t f;
        memset(&f, 0, sizeof(f));
        f.interface_id = REAL_IFACE_CAN0;
        f.can_id = 0x120u;
        f.length = 8u;
        memcpy(f.data, data, 8);
        cancestry_hal_rx_ring_push(&world.rx0, &f);
    }

    for (tick = 0u; tick < REAL_MAX_TICKS; ++tick) {
        (void)cancestry_hal_poll_rx(&world.hal, &world.bus, NULL, NULL);
        ingest_hal_frames();
        drain_bus();
        (void)cancestry_fsm_engine_tick(&world.fsm);
        drain_bus();
        if (world.frames_tx > 0u && cancestry_hal_tx_ring_count(&world.tx1) == 0u) {
            break;
        }
        usleep(1000);
    }

    (void)cancestry_hal_poll_rx(&world.hal, &world.bus, NULL, NULL);
    drain_bus();

    printf("frames_seen=%u frames_tx=%u faults=%u\n",
           world.frames_seen, world.frames_tx, world.faults_seen);
    if (world.frames_tx >= 1u) {
        printf("RESULT PASS\n");
        return 0;
    }
    printf("RESULT FAIL (no frames transmitted)\n");
    return 1;
}
