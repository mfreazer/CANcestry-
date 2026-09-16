/*
 * CANcestry FSM conformance: capability enforcement and the governor checkpoint.
 *
 * Contract under test (docs/system/governor.md, docs/software/SwRS.md
 * SW-FR-FSM-023..025, SW-FR-FSM-039..042, SW-FR-FSM-024, SW-FR-GOV-005,
 * SW-FR-GOV-006, docs/packages/package-spec.md sections 5 and 7):
 *
 *   - every send_message and set_signal is a governor request, and the request
 *     happens before any effect;
 *   - no governor means every side effect is denied (fail closed);
 *   - the package capability set is checked first: an interface without TX
 *     permission, a CAN id outside the declared allowlist, a signal outside the
 *     write allowlist and an unknown interface are blocked before the governor is
 *     consulted, and each refusal is counted;
 *   - a denial produces no partial effect and does not stop the rest of the
 *     transition's actions (the failure is recorded, execution continues safely);
 *   - instance bindings override package bindings for that instance;
 *   - the engine's only path to the outside world is the sink: with no sink, an
 *     approved effect fails instead of being lost silently;
 *   - rate limiting (a governor policy) is visible to the runtime only as a
 *     denial, which is exactly how the stub behaves here.
 *
 * Test ids: FSM-CAPABILITY-001, FSM-CAPABILITY-002, FSM-CAPABILITY-003,
 * FSM-CAPABILITY-004, FSM-CAPABILITY-005, FSM-CAPABILITY-006,
 * FSM-CAPABILITY-007, FSM-CAPABILITY-008, FSM-CAPABILITY-009 and
 * FSM-CAPABILITY-010. The subset named by docs/trace/traceability.csv is
 * 001, 002, 003, 005, 007 and 009; 004, 006, 008 and 010 are additional cases.
 */

#include "cancestry_fsm_conformance.h"

static const char *const capability_yaml = "schema_version: \"0.2.0\"\n"
                                           "state_machines:\n"
                                           "  - name: lab\n"
                                           "    initial: S0\n"
                                           "    variables:\n"
                                           "      - name: n\n"
                                           "        type: integer\n"
                                           "        default: 1\n"
                                           "    states:\n"
                                           "      - name: S0\n"
                                           "        transitions:\n"
                                           "          - event: signal_changed\n"
                                           "            signal: SendOk\n"
                                           "            target: S0\n"
                                           "            actions:\n"
                                           "              - send_message:\n"
                                           "                  interface: can0\n"
                                           "                  message: StatusMsg\n"
                                           "                  signals:\n"
                                           "                    VehicleSpeed: 42\n"
                                           "              - log:\n"
                                           "                  level: info\n"
                                           "                  message: after send\n"
                                           "          - event: signal_changed\n"
                                           "            signal: SendRxOnly\n"
                                           "            target: S0\n"
                                           "            actions:\n"
                                           "              - send_message:\n"
                                           "                  interface: can1\n"
                                           "                  message: StatusMsg\n"
                                           "                  signals:\n"
                                           "                    VehicleSpeed: 1\n"
                                           "          - event: signal_changed\n"
                                           "            signal: SendUnknownId\n"
                                           "            target: S0\n"
                                           "            actions:\n"
                                           "              - send_message:\n"
                                           "                  interface: can0\n"
                                           "                  message: AlertMsg\n"
                                           "                  signals:\n"
                                           "                    AlertLevel: 3\n"
                                           "          - event: signal_changed\n"
                                           "            signal: SendUnknownIface\n"
                                           "            target: S0\n"
                                           "            actions:\n"
                                           "              - send_message:\n"
                                           "                  interface: can9\n"
                                           "                  message: StatusMsg\n"
                                           "                  signals:\n"
                                           "                    VehicleSpeed: 1\n"
                                           "          - event: signal_changed\n"
                                           "            signal: WriteReadonly\n"
                                           "            target: S0\n"
                                           "            actions:\n"
                                           "              - set_signal:\n"
                                           "                  signal: VehicleSpeed\n"
                                           "                  value: 7\n"
                                           "          - event: signal_changed\n"
                                           "            signal: WriteAllowed\n"
                                           "            target: S0\n"
                                           "            actions:\n"
                                           "              - set_signal:\n"
                                           "                  signal: ClusterSpeed\n"
                                           "                  value: 99\n"
                                           "              - log:\n"
                                           "                  level: info\n"
                                           "                  message: after write\n"
                                           "          - event: signal_changed\n"
                                           "            signal: SendAlias\n"
                                           "            target: S0\n"
                                           "            actions:\n"
                                           "              - send_message:\n"
                                           "                  interface: car\n"
                                           "                  message: demo.ClusterMsg\n"
                                           "                  signals:\n"
                                           "                    ClusterSpeed: \"var.n * 100\"\n"
                                           "      - name: S1\n"
                                           "instances:\n"
                                           "  - id: lab.one\n"
                                           "    machine: lab\n"
                                           "    enabled: true\n"
                                           "  - id: lab.bound\n"
                                           "    machine: lab\n"
                                           "    enabled: true\n"
                                           "    bindings:\n"
                                           "      car: can1\n";

static void signal_event(cancestry_event_t *event, const char *signal, uint64_t timestamp_us)
{
    fsm_test_event_int(event, timestamp_us, signal, 1);
}

/* FSM-CAPABILITY-001: with no governor, nothing leaves the engine. */
static void case_no_governor_denies(void)
{
    static fsm_test_fixture_t fx;
    fsm_test_options_t options = fsm_test_options_default();
    cancestry_event_t event;

    CANCESSTRY_TEST_CASE("FSM-CAPABILITY-001 no governor denies every side effect");
    options.no_governor = true;
    CANCESSTRY_TEST_CHECK(fsm_test_init_with(&fx, capability_yaml, &options));
    CANCESSTRY_TEST_CHECK_U64(fsm_test_start(&fx, "lab.one"), CANCESTRY_FSM_OK);
    fsm_test_reset(&fx);
    signal_event(&event, "SendOk", 1000u);
    CANCESSTRY_TEST_CHECK_U64(fsm_test_process(&fx, &event), CANCESTRY_FSM_OK);
    /* Denied, counted, and no frame was handed to anyone. */
    CANCESSTRY_TEST_CHECK(fx.governor == NULL);
    CANCESSTRY_TEST_CHECK(!fsm_test_has_line(&fx, "tx "));
    CANCESSTRY_TEST_CHECK(fsm_test_has_line(&fx, "log 1 info after send"));
    CANCESSTRY_TEST_CHECK_U64(fsm_test_counters(&fx, "lab.one")->governor_denials, 1u);
    CANCESSTRY_TEST_CHECK_U64(fsm_test_counters(&fx, "lab.one")->capability_denials, 0u);
    CANCESSTRY_TEST_CHECK_U64(fsm_test_counters(&fx, "lab.one")->messages_sent, 0u);
    CANCESSTRY_TEST_CHECK_U64(fsm_test_counters(&fx, "lab.one")->action_errors, 1u);
    /* The instance keeps running: a denial is recorded, not fatal. */
    CANCESSTRY_TEST_CHECK_U64(fsm_test_lifecycle(&fx, "lab.one"),
                              CANCESTRY_FSM_INSTANCE_LIFECYCLE_RUNNING);

    /* A signal write is governed the same way. */
    cancestry_event_init(&event);
    signal_event(&event, "WriteAllowed", 2000u);
    CANCESSTRY_TEST_CHECK_U64(fsm_test_process(&fx, &event), CANCESTRY_FSM_OK);
    CANCESSTRY_TEST_CHECK(!fsm_test_signal_is_set(&fx, "ClusterSpeed"));
    CANCESSTRY_TEST_CHECK_U64(fsm_test_counters(&fx, "lab.one")->signals_set, 0u);
    CANCESSTRY_TEST_CHECK_U64(fsm_test_counters(&fx, "lab.one")->governor_denials, 2u);
    fsm_test_destroy(&fx);
}

/* FSM-CAPABILITY-002, FSM-CAPABILITY-003, FSM-CAPABILITY-004 and
 * FSM-CAPABILITY-005: the capability gate runs before the governor. */
static void case_capability_gate(void)
{
    static fsm_test_fixture_t fx;
    cancestry_event_t event;
    uint32_t before = 0u;

    CANCESSTRY_TEST_CASE("FSM-CAPABILITY-002 undeclared TX is refused before approval");
    CANCESSTRY_TEST_CHECK(fsm_test_init(&fx, capability_yaml));
    CANCESSTRY_TEST_CHECK_U64(fsm_test_start(&fx, "lab.one"), CANCESTRY_FSM_OK);

    /* -002: can1 has tx: false. Counters are never reset by the engine, so each
     * step below compares against the value before the event. */
    before = fsm_test_counters(&fx, "lab.one")->capability_denials;
    fsm_test_reset(&fx);
    signal_event(&event, "SendRxOnly", 1000u);
    CANCESSTRY_TEST_CHECK_U64(fsm_test_process(&fx, &event), CANCESTRY_FSM_OK);
    CANCESSTRY_TEST_CHECK_U64(fx.governor_calls, 0u);
    CANCESSTRY_TEST_CHECK_U64(fsm_test_counters(&fx, "lab.one")->capability_denials - before,
                              1u);
    /* The refusal is attributed in the trace, so an operator can tell a governor
     * denial from a capability denial without running a debugger. */
    {
        char trace[2048];

        (void)fsm_test_render_trace(&fx, trace, sizeof(trace));
        CANCESSTRY_TEST_CHECK(strstr(trace, "denied") != NULL);
        CANCESSTRY_TEST_CHECK(strstr(trace, "capability") != NULL);
    }

    /* FSM-CAPABILITY-003: AlertMsg (0x300) is not in can0's declared TX id list. */
    before = fsm_test_counters(&fx, "lab.one")->capability_denials;
    fx.governor_calls = 0u;
    fsm_test_reset(&fx);
    cancestry_event_init(&event);
    signal_event(&event, "SendUnknownId", 1000u);
    CANCESSTRY_TEST_CHECK_U64(fsm_test_process(&fx, &event), CANCESTRY_FSM_OK);
    CANCESSTRY_TEST_CHECK_U64(fx.governor_calls, 0u);
    CANCESSTRY_TEST_CHECK_U64(fsm_test_counters(&fx, "lab.one")->capability_denials - before,
                              1u);

    /* FSM-CAPABILITY-004: an interface the package never declared does not
     * resolve at all. */
    fx.governor_calls = 0u;
    cancestry_event_init(&event);
    signal_event(&event, "SendUnknownIface", 1000u);
    CANCESSTRY_TEST_CHECK_U64(fsm_test_process(&fx, &event), CANCESTRY_FSM_OK);
    CANCESSTRY_TEST_CHECK_U64(fx.governor_calls, 0u);
    CANCESSTRY_TEST_CHECK_U64(fsm_test_counters(&fx, "lab.one")->capability_denials - before,
                              2u);

    /* FSM-CAPABILITY-005: VehicleSpeed is readable but not writable. */
    before = fsm_test_counters(&fx, "lab.one")->capability_denials;
    cancestry_event_init(&event);
    signal_event(&event, "WriteReadonly", 1000u);
    CANCESSTRY_TEST_CHECK_U64(fsm_test_process(&fx, &event), CANCESTRY_FSM_OK);
    CANCESSTRY_TEST_CHECK_U64(fx.governor_calls, 0u);
    CANCESSTRY_TEST_CHECK_U64(fsm_test_counters(&fx, "lab.one")->capability_denials - before,
                              1u);
    CANCESSTRY_TEST_CHECK(fsm_test_get_int(&fx, "VehicleSpeed") == INT64_MIN);
    fsm_test_destroy(&fx);
}

/* FSM-CAPABILITY-006: a declared write is approved and applied. */
static void case_allowed_effects(void)
{
    static fsm_test_fixture_t fx;
    cancestry_event_t event;

    CANCESSTRY_TEST_CASE("FSM-CAPABILITY-006 approved effects apply once");
    CANCESSTRY_TEST_CHECK(fsm_test_init(&fx, capability_yaml));
    CANCESSTRY_TEST_CHECK_U64(fsm_test_start(&fx, "lab.one"), CANCESTRY_FSM_OK);
    fsm_test_reset(&fx);
    signal_event(&event, "WriteAllowed", 1000u);
    CANCESSTRY_TEST_CHECK_U64(fsm_test_process(&fx, &event), CANCESTRY_FSM_OK);
    CANCESSTRY_TEST_CHECK_U64(fx.governor_calls, 1u);
    CANCESSTRY_TEST_CHECK_U64(fx.governor_approvals, 1u);
    CANCESSTRY_TEST_CHECK_I64(fsm_test_get_int(&fx, "ClusterSpeed"), 99);
    CANCESSTRY_TEST_CHECK(fsm_test_has_line(&fx, "signal 1 ClusterSpeed unset->99"));
    CANCESSTRY_TEST_CHECK(fsm_test_has_line(&fx, "log 1 info after write"));
    CANCESSTRY_TEST_CHECK_U64(fsm_test_counters(&fx, "lab.one")->signals_set, 1u);
    CANCESSTRY_TEST_CHECK_U64(fsm_test_counters(&fx, "lab.one")->action_errors, 0u);

    /* A declared send: the frame is encoded from the FSM's own signal values and
     * the CAN id comes from the codec map, not from the FSM file. */
    fsm_test_reset(&fx);
    cancestry_event_init(&event);
    signal_event(&event, "SendOk", 2000u);
    CANCESSTRY_TEST_CHECK_U64(fsm_test_process(&fx, &event), CANCESTRY_FSM_OK);
    CANCESSTRY_TEST_CHECK(fsm_test_has_line(&fx, "tx 1 can0 StatusMsg 0x100 [2A00000000000000]"));
    CANCESSTRY_TEST_CHECK_U64(fsm_test_counters(&fx, "lab.one")->messages_sent, 1u);

    /* An alias plus a canonical message name and an expression value. */
    fsm_test_reset(&fx);
    cancestry_event_init(&event);
    signal_event(&event, "SendAlias", 3000u);
    CANCESSTRY_TEST_CHECK_U64(fsm_test_process(&fx, &event), CANCESTRY_FSM_OK);
    CANCESSTRY_TEST_CHECK(fsm_test_has_line(&fx, "tx 1 can0 demo.ClusterMsg 0x200 [6400]"));
    fsm_test_destroy(&fx);
}

/* FSM-CAPABILITY-007: a governor denial has no partial effect, and the rest of
 * the actions still run. */
static void case_governor_denial(void)
{
    static fsm_test_fixture_t fx;
    cancestry_event_t event;

    CANCESSTRY_TEST_CASE("FSM-CAPABILITY-007 denial has no partial effect");
    CANCESSTRY_TEST_CHECK(fsm_test_init(&fx, capability_yaml));
    CANCESSTRY_TEST_CHECK_U64(fsm_test_start(&fx, "lab.one"), CANCESTRY_FSM_OK);
    fx.governor_mode = FSM_TEST_GOV_DENY_SEND;
    fsm_test_reset(&fx);
    signal_event(&event, "SendOk", 1000u);
    CANCESSTRY_TEST_CHECK_U64(fsm_test_process(&fx, &event), CANCESTRY_FSM_OK);
    CANCESSTRY_TEST_CHECK_U64(fx.governor_calls, 1u);
    CANCESSTRY_TEST_CHECK_U64(fx.governor_denials, 1u);
    CANCESSTRY_TEST_CHECK(!fsm_test_has_line(&fx, "tx "));
    CANCESSTRY_TEST_CHECK(fsm_test_has_line(&fx, "log 1 info after send"));
    CANCESSTRY_TEST_CHECK_U64(fsm_test_counters(&fx, "lab.one")->messages_sent, 0u);
    CANCESSTRY_TEST_CHECK_U64(fsm_test_counters(&fx, "lab.one")->governor_denials, 1u);
    /* Only set_signal was approved. */
    cancestry_event_init(&event);
    signal_event(&event, "WriteAllowed", 2000u);
    CANCESSTRY_TEST_CHECK_U64(fsm_test_process(&fx, &event), CANCESTRY_FSM_OK);
    CANCESSTRY_TEST_CHECK_I64(fsm_test_get_int(&fx, "ClusterSpeed"), 99);
    CANCESSTRY_TEST_CHECK_U64(fsm_test_counters(&fx, "lab.one")->governor_denials, 1u);
    fsm_test_destroy(&fx);
}

/* FSM-CAPABILITY-008: a rate limit is a denial once the budget is gone, and the
 * runtime does not retry, queue or otherwise push back. */
static void case_rate_limit(void)
{
    static fsm_test_fixture_t fx;
    cancestry_event_t event;
    uint32_t i;

    CANCESSTRY_TEST_CASE("FSM-CAPABILITY-008 rate limiting surfaces as denial");
    CANCESSTRY_TEST_CHECK(fsm_test_init(&fx, capability_yaml));
    CANCESSTRY_TEST_CHECK_U64(fsm_test_start(&fx, "lab.one"), CANCESTRY_FSM_OK);
    fx.governor_mode = FSM_TEST_GOV_RATE_LIMIT;
    fx.governor_budget = 2u;
    for (i = 0u; i < 4u; ++i) {
        cancestry_event_init(&event);
        signal_event(&event, "SendOk", (uint64_t)(1000u + (i * 1000u)));
        CANCESSTRY_TEST_CHECK_U64(fsm_test_process(&fx, &event), CANCESTRY_FSM_OK);
    }
    CANCESSTRY_TEST_CHECK_U64(fsm_test_counters(&fx, "lab.one")->messages_sent, 2u);
    CANCESSTRY_TEST_CHECK_U64(fsm_test_counters(&fx, "lab.one")->governor_denials, 2u);
    CANCESSTRY_TEST_CHECK_U64(fx.governor_calls, 4u);
    fsm_test_destroy(&fx);
}

/* The governor sees a fully resolved request: interface, id, encoded frame and
 * the causing event, valid for the duration of the callback only. */
typedef struct {
    uint32_t calls;
    char interface_name[32];
    char message_name[64];
    char signal_name[32];
    uint32_t interface_id;
    uint32_t can_id;
    uint8_t frame[8];
    uint8_t frame_length;
    uint32_t instance_id;
    int32_t signal_value;
    uint32_t cause_type;
    uint64_t cause_timestamp_us;
    int approve;
} captured_request_t;

static captured_request_t captured;

static cancestry_fsm_governor_decision_t capturing_governor(
    void *user_data, const cancestry_fsm_governor_request_t *request)
{
    captured_request_t *out = (captured_request_t *)user_data;

    out->calls++;
    out->instance_id = (uint32_t)request->instance_id;
    out->can_id = request->can_id;
    out->interface_id = (uint32_t)request->interface_id;
    out->frame_length = request->frame_length;
    out->cause_type = (request->cause != NULL) ? (uint32_t)request->cause->type : 0u;
    out->cause_timestamp_us = (request->cause != NULL) ? request->cause->timestamp_us : 0u;
    (void)snprintf(out->interface_name, sizeof(out->interface_name), "%s",
                   (request->interface_name != NULL) ? request->interface_name : "");
    (void)snprintf(out->message_name, sizeof(out->message_name), "%s",
                   (request->message_name != NULL) ? request->message_name : "");
    (void)snprintf(out->signal_name, sizeof(out->signal_name), "%s",
                   (request->signal_name != NULL) ? request->signal_name : "");
    memset(out->frame, 0, sizeof(out->frame));
    if (request->frame != NULL && request->frame_length <= sizeof(out->frame)) {
        memcpy(out->frame, request->frame, request->frame_length);
    }
    if (request->value != NULL &&
        request->value->kind == CANCESTRY_VALUE_KIND_INT) {
        out->signal_value = (int32_t)request->value->value.integer;
    }
    return (out->approve != 0) ? CANCESTRY_FSM_GOVERNOR_APPROVE : CANCESTRY_FSM_GOVERNOR_DENY;
}

/* FSM-CAPABILITY-009: the approval request itself, and the two escape hatches
 * the engine has: a caller-provided governor and a caller-provided sink. */
static void case_request_surface(void)
{
    static fsm_test_fixture_t fx;
    cancestry_event_t event;
    fsm_test_options_t options = fsm_test_options_default();

    CANCESSTRY_TEST_CASE("FSM-CAPABILITY-009 request contents and no sink");
    CANCESSTRY_TEST_CHECK(fsm_test_init(&fx, capability_yaml));
    /* The governor is a plain function pointer by design (governor.md section 4),
     * so the suite can replace the fixture's with an inspecting one. */
    memset(&captured, 0, sizeof(captured));
    captured.approve = 1;
    fx.engine.governor = capturing_governor;
    fx.engine.governor_user_data = &captured;
    CANCESSTRY_TEST_CHECK_U64(fsm_test_start(&fx, "lab.one"), CANCESTRY_FSM_OK);
    signal_event(&event, "SendOk", 1000u);
    CANCESSTRY_TEST_CHECK_U64(fsm_test_process(&fx, &event), CANCESTRY_FSM_OK);
    CANCESSTRY_TEST_CHECK_U64(captured.calls, 1u);
    CANCESSTRY_TEST_CHECK_STRING(captured.interface_name, "can0");
    CANCESSTRY_TEST_CHECK_U64(captured.interface_id, FSM_TEST_IFACE_CAN0);
    CANCESSTRY_TEST_CHECK_STRING(captured.message_name, "StatusMsg");
    CANCESSTRY_TEST_CHECK_U64(captured.can_id, FSM_TEST_MSG_STATUS);
    CANCESSTRY_TEST_CHECK_U64(captured.frame_length, 8u);
    /* VehicleSpeed 42 encoded little-endian at bit 0 of an 8-byte frame. */
    CANCESSTRY_TEST_CHECK_U64(captured.frame[0], 42u);
    CANCESSTRY_TEST_CHECK_U64(captured.frame[1], 0u);
    CANCESSTRY_TEST_CHECK_U64(captured.instance_id, 1u);
    CANCESSTRY_TEST_CHECK_U64(captured.cause_type, CANCESTRY_EVENT_TYPE_SIGNAL_CHANGED);
    CANCESSTRY_TEST_CHECK_U64(captured.cause_timestamp_us, 1000u);

    /* A denied request performs nothing at all. */
    captured.approve = 0;
    fx.line_count = 0u;
    cancestry_event_init(&event);
    signal_event(&event, "SendOk", 2000u);
    CANCESSTRY_TEST_CHECK_U64(fsm_test_process(&fx, &event), CANCESTRY_FSM_OK);
    CANCESSTRY_TEST_CHECK_U64(captured.calls, 2u);
    CANCESSTRY_TEST_CHECK(!fsm_test_has_line(&fx, "tx "));
    CANCESSTRY_TEST_CHECK(fsm_test_has_line(&fx, "log 1 info after send"));
    fsm_test_destroy(&fx);

    /* With no sink installed, an approved send has nowhere to go and fails as
     * unsupported: the engine never invents a delivery path of its own, which is
     * the "no direct hardware access" rule (SW-FR-FSM-025) made observable. */
    options.no_sink = true;
    CANCESSTRY_TEST_CHECK(fsm_test_init_with(&fx, capability_yaml, &options));
    CANCESSTRY_TEST_CHECK_U64(fsm_test_start(&fx, "lab.one"), CANCESTRY_FSM_OK);
    cancestry_event_init(&event);
    signal_event(&event, "SendOk", 1000u);
    CANCESSTRY_TEST_CHECK_U64(fsm_test_process(&fx, &event), CANCESTRY_FSM_OK);
    CANCESSTRY_TEST_CHECK_U64(fx.governor_approvals, 1u);
    CANCESSTRY_TEST_CHECK_U64(fsm_test_counters(&fx, "lab.one")->messages_sent, 0u);
    CANCESSTRY_TEST_CHECK_U64(fsm_test_counters(&fx, "lab.one")->action_errors, 1u);
    CANCESSTRY_TEST_CHECK(fsm_test_counters(&fx, "lab.one")->action_errors == 1u);
    fsm_test_destroy(&fx);
}

/* Package-spec section 5: instance bindings override package bindings. */
static void case_instance_binding_override(void)
{
    static fsm_test_fixture_t fx;
    cancestry_event_t event;

    CANCESSTRY_TEST_CASE("FSM-CAPABILITY-010 instance binding shadows the package alias");
    CANCESSTRY_TEST_CHECK(fsm_test_init(&fx, capability_yaml));
    CANCESSTRY_TEST_CHECK_U64(fsm_test_start_all(&fx), 2u);
    fsm_test_reset(&fx);
    signal_event(&event, "SendAlias", 1000u);
    CANCESSTRY_TEST_CHECK_U64(fsm_test_process(&fx, &event), CANCESTRY_FSM_OK);
    /* lab.one resolves "car" through the package binding to can0 (TX allowed). */
    CANCESSTRY_TEST_CHECK(fsm_test_has_line(&fx, "tx 1 can0 demo.ClusterMsg"));
    /* lab.bound rebinds "car" to can1, which has no TX permission at all, so the
     * same action is refused before approval. */
    CANCESSTRY_TEST_CHECK(!fsm_test_has_line(&fx, "tx 2 "));
    CANCESSTRY_TEST_CHECK_U64(fsm_test_counters(&fx, "lab.bound")->capability_denials, 1u);
    CANCESSTRY_TEST_CHECK_U64(fsm_test_counters(&fx, "lab.one")->messages_sent, 1u);
    CANCESSTRY_TEST_CHECK_U64(fsm_test_counters(&fx, "lab.bound")->messages_sent, 0u);
    fsm_test_destroy(&fx);
}

int main(void)
{
    CANCESSTRY_TEST_SUITE_BEGIN("fsm/capability_enforcement");
    case_no_governor_denies();
    case_capability_gate();
    case_allowed_effects();
    case_governor_denial();
    case_rate_limit();
    case_request_surface();
    case_instance_binding_override();
    return CANCESSTRY_TEST_SUITE_END();
}
