/*
 * CANcestry - portable event model.
 *
 * Normative references:
 *   docs/system/event-ordering.md  sections 1, 3, 4, 8
 *   docs/software/SwRS.md          SW-FR-EVENT-001, SW-FR-EVENT-002, SW-FR-EVENT-003
 *   docs/system/SyRS.md            SYS-NF-001 (determinism), SYS-NF-002 (bounded resources)
 *
 * Design notes:
 *   - The event struct is a fixed-size value type. It owns no memory, holds no
 *     pointers to dynamically allocated data, and can be copied by assignment.
 *   - Strings referenced by a payload (signal names, state names) are borrowed
 *     pointers with static lifetime. The core never copies or frees them.
 *   - All identifiers are integers. Name resolution is owned by the codec,
 *     recipe, and FSM layers, which intern names into stable IDs at load time.
 *   - The struct is not packed. Padding is left to the compiler so that the
 *     same layout rules apply on host and target.
 */

#ifndef CANCESTRY_EVENT_TYPES_H
#define CANCESTRY_EVENT_TYPES_H

#include "cancestry/event/clock.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------------- */
/* Identifiers                                                               */
/* ------------------------------------------------------------------------- */

typedef uint32_t cancestry_event_id_t;
typedef uint32_t cancestry_sequence_t;
typedef uint32_t cancestry_signal_id_t;
typedef uint32_t cancestry_timer_id_t;
typedef uint32_t cancestry_instance_id_t;
typedef uint32_t cancestry_state_id_t;
typedef uint32_t cancestry_fault_code_t;
typedef uint32_t cancestry_source_id_t;
typedef uint16_t cancestry_interface_id_t;

/** Reserved "no value" identifier shared by all identifier typedefs. */
#define CANCESTRY_ID_NONE ((uint32_t)0u)

/** event_id value meaning "not yet assigned"; the queue assigns one on push. */
#define CANCESTRY_EVENT_ID_NONE ((cancestry_event_id_t)0u)

/** sequence value meaning "no causing event". */
#define CANCESTRY_SEQUENCE_NONE ((cancestry_sequence_t)0u)

/** Classic CAN payload limit. CAN FD is out of scope for v0.2. */
#define CANCESTRY_CAN_FRAME_MAX_LENGTH ((uint8_t)8u)

/* ------------------------------------------------------------------------- */
/* Enumerations                                                              */
/* ------------------------------------------------------------------------- */

/**
 * Event types.
 *
 * The seven normative types are the ones listed in
 * docs/packages/fsm-spec.md section 5 and SW-FR-EVENT-003.
 */
typedef enum cancestry_event_type {
    CANCESTRY_EVENT_TYPE_INVALID = 0,
    CANCESTRY_EVENT_TYPE_CAN_RX = 1,
    CANCESTRY_EVENT_TYPE_SIGNAL_CHANGED = 2,
    CANCESTRY_EVENT_TYPE_TIMER_EXPIRED = 3,
    CANCESTRY_EVENT_TYPE_STATE_ENTERED = 4,
    CANCESTRY_EVENT_TYPE_STATE_EXITED = 5,
    CANCESTRY_EVENT_TYPE_FAULT_RAISED = 6,
    CANCESTRY_EVENT_TYPE_POWER_MODE_CHANGED = 7,
    CANCESTRY_EVENT_TYPE_COUNT
} cancestry_event_type_t;

/**
 * Priority classes. Lower number means higher priority.
 *
 * Values are normative; see docs/system/event-ordering.md section 3.
 */
typedef enum cancestry_priority_class {
    CANCESTRY_PRIORITY_CLASS_FAULT = 0,
    CANCESTRY_PRIORITY_CLASS_MODE = 1,
    CANCESTRY_PRIORITY_CLASS_TIMER = 2,
    CANCESTRY_PRIORITY_CLASS_CAN_RX = 3,
    CANCESTRY_PRIORITY_CLASS_HOST = 4,
    CANCESTRY_PRIORITY_CLASS_GENERATED = 5,
    CANCESTRY_PRIORITY_CLASS_TRACE = 6,
    CANCESTRY_PRIORITY_CLASS_COUNT
} cancestry_priority_class_t;

/** Fault severities; see docs/system/mode-fault-state-machine.md section 4. */
typedef enum cancestry_fault_severity {
    CANCESTRY_FAULT_SEVERITY_INFO = 0,
    CANCESTRY_FAULT_SEVERITY_WARNING = 1,
    CANCESTRY_FAULT_SEVERITY_ERROR = 2,
    CANCESTRY_FAULT_SEVERITY_CRITICAL = 3,
    CANCESTRY_FAULT_SEVERITY_COUNT
} cancestry_fault_severity_t;

/** System modes; see docs/system/mode-fault-state-machine.md section 1. */
typedef enum cancestry_mode {
    CANCESTRY_MODE_BOOT = 0,
    CANCESTRY_MODE_CONFIG = 1,
    CANCESTRY_MODE_LISTEN_ONLY = 2,
    CANCESTRY_MODE_ACTIVE = 3,
    CANCESTRY_MODE_SAFE = 4,
    CANCESTRY_MODE_OFF = 5,
    CANCESTRY_MODE_COUNT
} cancestry_mode_t;

/** Signal / variable value kinds; boolean, integer and float are required. */
typedef enum cancestry_value_kind {
    CANCESTRY_VALUE_KIND_UNSET = 0,
    CANCESTRY_VALUE_KIND_BOOL = 1,
    CANCESTRY_VALUE_KIND_INT = 2,
    CANCESTRY_VALUE_KIND_UINT = 3,
    CANCESTRY_VALUE_KIND_REAL = 4,
    CANCESTRY_VALUE_KIND_COUNT
} cancestry_value_kind_t;

/* ------------------------------------------------------------------------- */
/* Payloads                                                                  */
/* ------------------------------------------------------------------------- */

/**
 * Tagged signal value.
 *
 * Only the union member selected by @c kind is meaningful. Callers shall not
 * read a member that does not match @c kind.
 */
typedef struct cancestry_value {
    cancestry_value_kind_t kind;
    union {
        bool boolean;
        int64_t integer;
        uint64_t unsigned_integer;
        double real;
    } value;
} cancestry_value_t;

/** Received classic CAN frame. */
typedef struct cancestry_can_rx_payload {
    cancestry_interface_id_t interface_id;
    uint32_t can_id;
    uint8_t is_extended;
    uint8_t length;
    uint8_t data[CANCESTRY_CAN_FRAME_MAX_LENGTH];
} cancestry_can_rx_payload_t;

/** Decoded signal change. */
typedef struct cancestry_signal_changed_payload {
    cancestry_signal_id_t signal_id;
    const char *signal_name;
    cancestry_value_t old_value;
    cancestry_value_t new_value;
} cancestry_signal_changed_payload_t;

/** Timer expiry, including periodic timers that missed one or more ticks. */
typedef struct cancestry_timer_expired_payload {
    cancestry_timer_id_t timer_id;
    cancestry_instance_id_t instance_id;
    uint32_t missed_count;
    uint8_t is_periodic;
} cancestry_timer_expired_payload_t;

/** Shared payload for state_entered and state_exited. */
typedef struct cancestry_state_payload {
    cancestry_instance_id_t instance_id;
    cancestry_state_id_t state_id;
    const char *state_name;
} cancestry_state_payload_t;

/** Raised fault. */
typedef struct cancestry_fault_raised_payload {
    cancestry_fault_code_t fault_code;
    cancestry_fault_severity_t severity;
    cancestry_source_id_t source_id;
} cancestry_fault_raised_payload_t;

/** System mode (power mode) transition. */
typedef struct cancestry_mode_changed_payload {
    cancestry_mode_t from_mode;
    cancestry_mode_t to_mode;
    uint32_t reason_code;
} cancestry_mode_changed_payload_t;

/**
 * Typed event payload.
 *
 * The member that is valid is determined by cancestry_event_t::type. The
 * mapping is:
 *
 *   CANCESTRY_EVENT_TYPE_CAN_RX             -> can_rx
 *   CANCESTRY_EVENT_TYPE_SIGNAL_CHANGED     -> signal_changed
 *   CANCESTRY_EVENT_TYPE_TIMER_EXPIRED      -> timer_expired
 *   CANCESTRY_EVENT_TYPE_STATE_ENTERED      -> state
 *   CANCESTRY_EVENT_TYPE_STATE_EXITED       -> state
 *   CANCESTRY_EVENT_TYPE_FAULT_RAISED       -> fault
 *   CANCESTRY_EVENT_TYPE_POWER_MODE_CHANGED -> mode
 */
typedef union cancestry_event_payload {
    cancestry_can_rx_payload_t can_rx;
    cancestry_signal_changed_payload_t signal_changed;
    cancestry_timer_expired_payload_t timer_expired;
    cancestry_state_payload_t state;
    cancestry_fault_raised_payload_t fault;
    cancestry_mode_changed_payload_t mode;
} cancestry_event_payload_t;

/* ------------------------------------------------------------------------- */
/* Event                                                                     */
/* ------------------------------------------------------------------------- */

/**
 * The common event structure.
 *
 * Field semantics:
 *   event_id        Runtime-unique identity for this event instance, used for
 *                   trace and replay correlation. Assigned by the queue on
 *                   push when the caller supplies CANCESTRY_EVENT_ID_NONE. A
 *                   caller may supply an explicit non-zero id when re-injecting
 *                   recorded events; the queue then preserves it.
 *   type            Event type; selects the payload member.
 *   timestamp_us    Monotonic timestamp from the injected clock.
 *   sequence        Ordering sequence, assigned by the queue on push. It is the
 *                   final tie-breaker of the selection order and is therefore
 *                   unique among events that came from the same queue.
 *   cause_sequence  sequence of the event that caused this one, or
 *                   CANCESTRY_SEQUENCE_NONE. Generated events always carry it.
 *   priority_class  Priority class; lower number is higher priority.
 *   payload         Type-specific payload.
 */
typedef struct cancestry_event {
    cancestry_event_id_t event_id;
    cancestry_event_type_t type;
    cancestry_time_us_t timestamp_us;
    cancestry_sequence_t sequence;
    cancestry_sequence_t cause_sequence;
    cancestry_priority_class_t priority_class;
    cancestry_event_payload_t payload;
} cancestry_event_t;

/* ------------------------------------------------------------------------- */
/* Helpers                                                                   */
/* ------------------------------------------------------------------------- */

/**
 * Zero an event and put it into a known, invalid state
 * (CANCESTRY_EVENT_TYPE_INVALID).
 */
void cancestry_event_init(cancestry_event_t *event);

/**
 * @return true when @p event is non-NULL, has a valid type and a valid
 *         priority class.
 */
bool cancestry_event_is_valid(const cancestry_event_t *event);

/**
 * @return true when @p type is one of the normative event types.
 */
bool cancestry_event_type_is_valid(cancestry_event_type_t type);

/**
 * @return true when @p priority_class is one of the normative priority classes.
 */
bool cancestry_priority_class_is_valid(cancestry_priority_class_t priority_class);

/**
 * Compare two events using the normative selection order:
 *
 *   1. timestamp_us ascending
 *   2. priority_class ascending
 *   3. sequence ascending
 *
 * @return negative when @p lhs sorts before @p rhs, positive when it sorts
 *         after, and 0 when all three keys are equal.
 */
int cancestry_event_compare_order(const cancestry_event_t *lhs, const cancestry_event_t *rhs);

/**
 * @return Stable, statically allocated name such as "can_rx", or "invalid"
 *         when @p type is out of range. Never NULL.
 */
const char *cancestry_event_type_name(cancestry_event_type_t type);

/**
 * @return Stable, statically allocated name such as "CAN_RX", or "INVALID"
 *         when @p priority_class is out of range. Never NULL.
 */
const char *cancestry_priority_class_name(cancestry_priority_class_t priority_class);

/**
 * @return Stable, statically allocated name such as "WARNING", or "INVALID".
 *         Never NULL.
 */
const char *cancestry_fault_severity_name(cancestry_fault_severity_t severity);

/**
 * @return Stable, statically allocated name such as "LISTEN_ONLY", or
 *         "INVALID". Never NULL.
 */
const char *cancestry_mode_name(cancestry_mode_t mode);

#ifdef __cplusplus
}
#endif

#endif /* CANCESTRY_EVENT_TYPES_H */
