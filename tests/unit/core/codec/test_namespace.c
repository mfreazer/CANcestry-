/*
 * Unit tests for the signal namespace.
 *
 * Verifies:
 *   SW-FR-CODEC-007  The software shall detect signal definition conflicts.
 *   SYS-FR-010       Shared signal namespace derived from loaded codec maps.
 *   SYS-NF-001       Stable ids for a fixed registration order.
 *   SYS-NF-002       Bounded resource usage: caller-owned storage only.
 *
 * Traceability: SIGNAL-NS-001
 *
 * Normative source: docs/packages/codec-map-spec.md section 2.
 */

#include "cancestry/codec/namespace.h"

#include "cancestry_test.h"

#include <string.h>

/* The trailing `false` in each map initializer is cancestry_codec_map_t::can_fd
 * (Phase 7, issue #20): every map here is a classic CAN map. */
/* map_a: signals Speed, Gear in message 0x100. */
static cancestry_codec_signal_t a_signals[2] = {
    {"Speed", 0u, 8u, CANCESTRY_CODEC_SIGNAL_TYPE_UINT, CANCESTRY_CODEC_ENDIANNESS_LITTLE,
     1.0, 0.0, NULL, NULL, 0u, false, false, 0.0, 0.0, true, 0u, 7u, 0x100u, CANCESTRY_CODEC_LAYOUT_CONTIGUOUS},
    {"Gear", 8u, 3u, CANCESTRY_CODEC_SIGNAL_TYPE_ENUM, CANCESTRY_CODEC_ENDIANNESS_LITTLE,
     1.0, 0.0, NULL, NULL, 0u, false, false, 0.0, 0.0, true, 8u, 10u, 0x100u, CANCESTRY_CODEC_LAYOUT_CONTIGUOUS}};
static cancestry_codec_message_t a_message = {0x100u, "MessageA", 2u, 0u, NULL, a_signals, 2u};
static cancestry_codec_map_t map_a = {"map_a", "1.0.0", NULL, &a_message, 1u, false};

/* map_b: signals Speed (collides with map_a's), Temp in message 0x200. */
static cancestry_codec_signal_t b_signals[2] = {
    {"Speed", 0u, 8u, CANCESTRY_CODEC_SIGNAL_TYPE_UINT, CANCESTRY_CODEC_ENDIANNESS_LITTLE,
     1.0, 0.0, NULL, NULL, 0u, false, false, 0.0, 0.0, true, 0u, 7u, 0x200u, CANCESTRY_CODEC_LAYOUT_CONTIGUOUS},
    {"Temp", 8u, 8u, CANCESTRY_CODEC_SIGNAL_TYPE_INT, CANCESTRY_CODEC_ENDIANNESS_BIG,
     1.0, 0.0, NULL, NULL, 0u, false, false, 0.0, 0.0, true, 8u, 15u, 0x200u, CANCESTRY_CODEC_LAYOUT_CONTIGUOUS}};
static cancestry_codec_message_t b_message = {0x200u, "MessageB", 2u, 0u, NULL, b_signals, 2u};
static cancestry_codec_map_t map_b = {"map_b", "1.0.0", NULL, &b_message, 1u, false};

/* map_dup: one signal name defined twice (in two messages). */
static cancestry_codec_signal_t dup_x1 = {"X", 0u, 1u, CANCESTRY_CODEC_SIGNAL_TYPE_UINT,
                                          CANCESTRY_CODEC_ENDIANNESS_LITTLE, 1.0, 0.0,
                                          NULL, NULL, 0u, false, false, 0.0, 0.0, true,
                                          0u, 0u, 0x300u, CANCESTRY_CODEC_LAYOUT_CONTIGUOUS};
static cancestry_codec_signal_t dup_x2 = {"X", 1u, 1u, CANCESTRY_CODEC_SIGNAL_TYPE_UINT,
                                          CANCESTRY_CODEC_ENDIANNESS_LITTLE, 1.0, 0.0,
                                          NULL, NULL, 0u, false, false, 0.0, 0.0, true,
                                          1u, 1u, 0x301u, CANCESTRY_CODEC_LAYOUT_CONTIGUOUS};
static cancestry_codec_message_t dup_messages[2] = {
    {0x300u, "DupA", 1u, 0u, NULL, &dup_x1, 1u},
    {0x301u, "DupB", 1u, 0u, NULL, &dup_x2, 1u}};
static cancestry_codec_map_t map_dup = {"map_dup", "1.0.0", NULL, dup_messages, 2u, false};

/* map_same_name: another map that claims map_a's name. */
static cancestry_codec_signal_t same_signal = {"S", 0u, 1u, CANCESTRY_CODEC_SIGNAL_TYPE_UINT,
                                               CANCESTRY_CODEC_ENDIANNESS_LITTLE, 1.0, 0.0,
                                               NULL, NULL, 0u, false, false, 0.0, 0.0, true,
                                               0u, 0u, 0x400u, CANCESTRY_CODEC_LAYOUT_CONTIGUOUS};
static cancestry_codec_message_t same_message = {0x400u, "Same", 1u, 0u, NULL, &same_signal, 1u};
static cancestry_codec_map_t map_same_name = {"map_a", "1.0.0", NULL, &same_message, 1u, false};

/* map_dotted: a signal whose short name contains a dot. */
static cancestry_codec_signal_t dotted_signal = {"a.b", 0u, 8u, CANCESTRY_CODEC_SIGNAL_TYPE_UINT,
                                                 CANCESTRY_CODEC_ENDIANNESS_LITTLE, 1.0, 0.0,
                                                 NULL, NULL, 0u, false, false, 0.0, 0.0, true,
                                                 0u, 7u, 0x500u, CANCESTRY_CODEC_LAYOUT_CONTIGUOUS};
static cancestry_codec_message_t dotted_message = {0x500u, "Dotted", 1u, 0u, NULL,
                                                   &dotted_signal, 1u};
static cancestry_codec_map_t map_dotted = {"map_dotted", "1.0.0", NULL, &dotted_message, 1u, false};

static const cancestry_codec_map_t *test_slots[8];
static cancestry_codec_namespace_t test_namespace;

static void test_init(void)
{
    CANCESSTRY_TEST_CHECK(!cancestry_codec_namespace_init(NULL, test_slots, 8u));
    CANCESSTRY_TEST_CHECK(!cancestry_codec_namespace_init(&test_namespace, NULL, 8u));
    CANCESSTRY_TEST_CHECK(!cancestry_codec_namespace_init(&test_namespace, test_slots, 0u));
    CANCESSTRY_TEST_CHECK(!cancestry_codec_namespace_init(
        &test_namespace, test_slots, CANCESTRY_CODEC_NAMESPACE_MAX_CAPACITY + 1u));
    CANCESSTRY_TEST_CHECK(cancestry_codec_namespace_init(&test_namespace, test_slots, 8u));
    CANCESSTRY_TEST_CHECK(cancestry_codec_namespace_is_valid(&test_namespace));
    CANCESSTRY_TEST_CHECK(!cancestry_codec_namespace_is_valid(NULL));
    CANCESSTRY_TEST_CHECK_U64(cancestry_codec_namespace_size(&test_namespace), 0u);
    CANCESSTRY_TEST_CHECK_U64(cancestry_codec_namespace_signal_count(&test_namespace), 0u);
}

static void test_register_and_ids(void)
{
    CANCESSTRY_TEST_CHECK(cancestry_codec_namespace_register(&test_namespace, &map_a) ==
                          CANCESTRY_CODEC_OK);
    CANCESSTRY_TEST_CHECK(cancestry_codec_namespace_register(&test_namespace, &map_b) ==
                          CANCESTRY_CODEC_OK);
    CANCESSTRY_TEST_CHECK_U64(cancestry_codec_namespace_size(&test_namespace), 2u);
    CANCESSTRY_TEST_CHECK_U64(cancestry_codec_namespace_signal_count(&test_namespace), 4u);

    CANCESSTRY_TEST_CHECK(cancestry_codec_namespace_register(NULL, &map_a) ==
                          CANCESTRY_CODEC_ERR_NULL);
    CANCESSTRY_TEST_CHECK(cancestry_codec_namespace_register(&test_namespace, NULL) ==
                          CANCESTRY_CODEC_ERR_NULL);
    CANCESSTRY_TEST_CHECK(cancestry_codec_namespace_register(&test_namespace, &map_same_name) ==
                          CANCESTRY_CODEC_ERR_CONFLICT);
    CANCESSTRY_TEST_CHECK(cancestry_codec_namespace_register(&test_namespace, &map_dup) ==
                          CANCESTRY_CODEC_ERR_CONFLICT);
    /* Conflicts must not consume a slot. */
    CANCESSTRY_TEST_CHECK_U64(cancestry_codec_namespace_size(&test_namespace), 2u);
}

static void test_resolve_canonical(void)
{
    cancestry_codec_resolution_t resolution;

    CANCESSTRY_TEST_CHECK(cancestry_codec_namespace_resolve(&test_namespace, "map_a.Speed",
                                                            &resolution) == CANCESTRY_CODEC_OK);
    CANCESSTRY_TEST_CHECK_U64(resolution.signal_id, 1u);
    CANCESSTRY_TEST_CHECK(resolution.map == &map_a);
    CANCESSTRY_TEST_CHECK(resolution.signal == &a_signals[0]);

    CANCESSTRY_TEST_CHECK(cancestry_codec_namespace_resolve(&test_namespace, "map_a.Gear",
                                                            &resolution) == CANCESTRY_CODEC_OK);
    CANCESSTRY_TEST_CHECK_U64(resolution.signal_id, 2u);

    CANCESSTRY_TEST_CHECK(cancestry_codec_namespace_resolve(&test_namespace, "map_b.Speed",
                                                            &resolution) == CANCESTRY_CODEC_OK);
    CANCESSTRY_TEST_CHECK_U64(resolution.signal_id, 3u);
    CANCESSTRY_TEST_CHECK(resolution.signal == &b_signals[0]);

    CANCESSTRY_TEST_CHECK(cancestry_codec_namespace_resolve(&test_namespace, "map_b.Temp",
                                                            &resolution) == CANCESTRY_CODEC_OK);
    CANCESSTRY_TEST_CHECK_U64(resolution.signal_id, 4u);

    CANCESSTRY_TEST_CHECK(cancestry_codec_namespace_resolve(&test_namespace, "map_a.Missing",
                                                            &resolution) ==
                          CANCESTRY_CODEC_ERR_NOT_FOUND);
    CANCESSTRY_TEST_CHECK(cancestry_codec_namespace_resolve(&test_namespace, "missing.Speed",
                                                            &resolution) ==
                          CANCESTRY_CODEC_ERR_NOT_FOUND);
    /* Malformed canonical names. */
    CANCESSTRY_TEST_CHECK(cancestry_codec_namespace_resolve(&test_namespace, ".Speed",
                                                            &resolution) ==
                          CANCESTRY_CODEC_ERR_ARGUMENT);
    CANCESSTRY_TEST_CHECK(cancestry_codec_namespace_resolve(&test_namespace, "map_a.",
                                                            &resolution) ==
                          CANCESTRY_CODEC_ERR_ARGUMENT);
}

static void test_resolve_short_names(void)
{
    cancestry_codec_resolution_t resolution;

    /* Unambiguous short names resolve. */
    CANCESSTRY_TEST_CHECK(cancestry_codec_namespace_resolve(&test_namespace, "Gear",
                                                            &resolution) == CANCESTRY_CODEC_OK);
    CANCESSTRY_TEST_CHECK_U64(resolution.signal_id, 2u);
    CANCESSTRY_TEST_CHECK(cancestry_codec_namespace_resolve(&test_namespace, "Temp",
                                                            &resolution) == CANCESTRY_CODEC_OK);
    CANCESSTRY_TEST_CHECK_U64(resolution.signal_id, 4u);

    /* "Speed" exists in both maps: ambiguous. */
    CANCESSTRY_TEST_CHECK(cancestry_codec_namespace_resolve(&test_namespace, "Speed",
                                                            &resolution) ==
                          CANCESTRY_CODEC_ERR_AMBIGUOUS);
    CANCESSTRY_TEST_CHECK(cancestry_codec_namespace_resolve(&test_namespace, "Missing",
                                                            &resolution) ==
                          CANCESTRY_CODEC_ERR_NOT_FOUND);
}

static void test_resolve_by_id(void)
{
    cancestry_codec_resolution_t resolution;

    CANCESSTRY_TEST_CHECK(cancestry_codec_namespace_resolve_id(&test_namespace, 1u,
                                                               &resolution) ==
                          CANCESTRY_CODEC_OK);
    CANCESSTRY_TEST_CHECK(resolution.map == &map_a);
    CANCESSTRY_TEST_CHECK(resolution.signal == &a_signals[0]);
    CANCESSTRY_TEST_CHECK(cancestry_codec_namespace_resolve_id(&test_namespace, 2u,
                                                               &resolution) ==
                          CANCESTRY_CODEC_OK);
    CANCESSTRY_TEST_CHECK(resolution.signal == &a_signals[1]);
    CANCESSTRY_TEST_CHECK(cancestry_codec_namespace_resolve_id(&test_namespace, 4u,
                                                               &resolution) ==
                          CANCESTRY_CODEC_OK);
    CANCESSTRY_TEST_CHECK(resolution.signal == &b_signals[1]);
    CANCESSTRY_TEST_CHECK(cancestry_codec_namespace_resolve_id(&test_namespace, 5u,
                                                               &resolution) ==
                          CANCESTRY_CODEC_ERR_NOT_FOUND);
    CANCESSTRY_TEST_CHECK(cancestry_codec_namespace_resolve_id(&test_namespace,
                                                               CANCESTRY_ID_NONE,
                                                               &resolution) ==
                          CANCESTRY_CODEC_ERR_ARGUMENT);
}

static void test_resolve_arguments(void)
{
    cancestry_codec_resolution_t resolution;

    CANCESSTRY_TEST_CHECK(cancestry_codec_namespace_resolve(NULL, "Speed", &resolution) ==
                          CANCESTRY_CODEC_ERR_NULL);
    CANCESSTRY_TEST_CHECK(cancestry_codec_namespace_resolve(&test_namespace, NULL, &resolution) ==
                          CANCESTRY_CODEC_ERR_NULL);
    CANCESSTRY_TEST_CHECK(cancestry_codec_namespace_resolve(&test_namespace, "Speed", NULL) ==
                          CANCESTRY_CODEC_ERR_NULL);
    CANCESSTRY_TEST_CHECK(cancestry_codec_namespace_resolve(&test_namespace, "", &resolution) ==
                          CANCESTRY_CODEC_ERR_ARGUMENT);
    CANCESSTRY_TEST_CHECK(cancestry_codec_namespace_resolve_id(NULL, 1u, &resolution) ==
                          CANCESTRY_CODEC_ERR_NULL);
    CANCESSTRY_TEST_CHECK(cancestry_codec_namespace_resolve_id(&test_namespace, 1u, NULL) ==
                          CANCESTRY_CODEC_ERR_NULL);
}

static void test_dotted_signal_name(void)
{
    cancestry_codec_resolution_t resolution;

    /* Canonical names split at the first dot, so a signal named "a.b" is
     * addressable as "map_dotted.a.b". */
    CANCESSTRY_TEST_CHECK(cancestry_codec_namespace_register(&test_namespace, &map_dotted) ==
                          CANCESTRY_CODEC_OK);
    CANCESSTRY_TEST_CHECK(cancestry_codec_namespace_resolve(&test_namespace, "map_dotted.a.b",
                                                            &resolution) == CANCESTRY_CODEC_OK);
    CANCESSTRY_TEST_CHECK_U64(resolution.signal_id, 5u);
    CANCESSTRY_TEST_CHECK(resolution.signal == &dotted_signal);
    /* The dotted short name itself is parsed as a canonical name. */
    CANCESSTRY_TEST_CHECK(cancestry_codec_namespace_resolve(&test_namespace, "a.b",
                                                            &resolution) ==
                          CANCESTRY_CODEC_ERR_NOT_FOUND);
}

static void test_capacity(void)
{
    cancestry_codec_namespace_t tiny;
    const cancestry_codec_map_t *one_slot[1];
    cancestry_codec_resolution_t resolution;

    CANCESSTRY_TEST_CHECK(cancestry_codec_namespace_init(&tiny, one_slot, 1u));
    CANCESSTRY_TEST_CHECK(cancestry_codec_namespace_register(&tiny, &map_a) ==
                          CANCESTRY_CODEC_OK);
    CANCESSTRY_TEST_CHECK(cancestry_codec_namespace_register(&tiny, &map_b) ==
                          CANCESTRY_CODEC_ERR_CAPACITY);
    CANCESSTRY_TEST_CHECK_U64(cancestry_codec_namespace_size(&tiny), 1u);
    /* The failed registration did not disturb the registered map. */
    CANCESSTRY_TEST_CHECK(cancestry_codec_namespace_resolve(&tiny, "map_a.Gear", &resolution) ==
                          CANCESTRY_CODEC_OK);
    CANCESSTRY_TEST_CHECK_U64(resolution.signal_id, 2u);
}

static void test_ids_depend_on_registration_order(void)
{
    cancestry_codec_namespace_t first;
    cancestry_codec_namespace_t second;
    const cancestry_codec_map_t *first_slots[2];
    const cancestry_codec_map_t *second_slots[2];
    cancestry_codec_resolution_t resolution;

    CANCESSTRY_TEST_CHECK(cancestry_codec_namespace_init(&first, first_slots, 2u));
    CANCESSTRY_TEST_CHECK(cancestry_codec_namespace_register(&first, &map_a) ==
                          CANCESTRY_CODEC_OK);
    CANCESSTRY_TEST_CHECK(cancestry_codec_namespace_register(&first, &map_b) ==
                          CANCESTRY_CODEC_OK);

    CANCESSTRY_TEST_CHECK(cancestry_codec_namespace_init(&second, second_slots, 2u));
    CANCESSTRY_TEST_CHECK(cancestry_codec_namespace_register(&second, &map_b) ==
                          CANCESTRY_CODEC_OK);
    CANCESSTRY_TEST_CHECK(cancestry_codec_namespace_register(&second, &map_a) ==
                          CANCESTRY_CODEC_OK);

    /* Reversed registration order reverses the id assignment. */
    CANCESSTRY_TEST_CHECK(cancestry_codec_namespace_resolve(&first, "map_a.Speed", &resolution) ==
                          CANCESTRY_CODEC_OK);
    CANCESSTRY_TEST_CHECK_U64(resolution.signal_id, 1u);
    CANCESSTRY_TEST_CHECK(cancestry_codec_namespace_resolve(&second, "map_a.Speed",
                                                            &resolution) == CANCESTRY_CODEC_OK);
    CANCESSTRY_TEST_CHECK_U64(resolution.signal_id, 3u);
}

int main(void)
{
    CANCESSTRY_TEST_SUITE_BEGIN("codec namespace");

    CANCESSTRY_TEST_CASE("init over caller-owned storage");
    test_init();

    CANCESSTRY_TEST_CASE("registration assigns ids and rejects conflicts");
    test_register_and_ids();

    CANCESSTRY_TEST_CASE("canonical name resolution");
    test_resolve_canonical();

    CANCESSTRY_TEST_CASE("short names resolve only while unambiguous");
    test_resolve_short_names();

    CANCESSTRY_TEST_CASE("resolve by stable id");
    test_resolve_by_id();

    CANCESSTRY_TEST_CASE("resolve argument validation");
    test_resolve_arguments();

    CANCESSTRY_TEST_CASE("dotted signal names via canonical form");
    test_dotted_signal_name();

    CANCESSTRY_TEST_CASE("full namespace rejects with ERR_CAPACITY");
    test_capacity();

    CANCESSTRY_TEST_CASE("ids depend only on registration order");
    test_ids_depend_on_registration_order();

    return CANCESSTRY_TEST_SUITE_END();
}
