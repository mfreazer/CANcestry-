/*
 * CANcestry - event type helpers.
 *
 * This translation unit contains no mutable global state. The name tables are
 * statically allocated and read-only.
 */

#include "cancestry/event/types.h"

#include <string.h>

/*
 * CAN FD payload lengths, indexed by the ISO 11898-1 DLC code. Codes 0..8
 * map to their own byte count; codes 9..15 map to the next larger legal
 * payload. The table is static and read-only (SYS-NF-002).
 */
static const uint8_t cancestry_canfd_length_by_dlc[16] = {
    0u, 1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u, 12u, 16u, 20u, 24u, 32u, 48u, 64u};

bool cancestry_can_payload_length_is_valid(bool is_fd, size_t length)
{
    size_t i;

    if (length > (size_t)CANCESTRY_CAN_FD_FRAME_MAX_LENGTH) {
        return false;
    }
    if (!is_fd) {
        return length <= (size_t)CANCESTRY_CAN_FRAME_MAX_LENGTH;
    }
    for (i = 0u; i < sizeof(cancestry_canfd_length_by_dlc); ++i) {
        if ((size_t)cancestry_canfd_length_by_dlc[i] == length) {
            return true;
        }
    }
    return false;
}

void cancestry_event_init(cancestry_event_t *event)
{
    if (event == NULL) {
        return;
    }
    memset(event, 0, sizeof(*event));
    event->type = CANCESTRY_EVENT_TYPE_INVALID;
}

bool cancestry_event_type_is_valid(cancestry_event_type_t type)
{
    return (type > CANCESTRY_EVENT_TYPE_INVALID) && (type < CANCESTRY_EVENT_TYPE_COUNT);
}

bool cancestry_priority_class_is_valid(cancestry_priority_class_t priority_class)
{
    return (priority_class >= CANCESTRY_PRIORITY_CLASS_FAULT) &&
           (priority_class < CANCESTRY_PRIORITY_CLASS_COUNT);
}

bool cancestry_event_is_valid(const cancestry_event_t *event)
{
    if (event == NULL) {
        return false;
    }
    return cancestry_event_type_is_valid(event->type) &&
           cancestry_priority_class_is_valid(event->priority_class);
}

int cancestry_event_compare_order(const cancestry_event_t *lhs, const cancestry_event_t *rhs)
{
    if (lhs == NULL || rhs == NULL) {
        if (lhs == rhs) {
            return 0;
        }
        return (lhs == NULL) ? -1 : 1;
    }

    if (lhs->timestamp_us < rhs->timestamp_us) {
        return -1;
    }
    if (lhs->timestamp_us > rhs->timestamp_us) {
        return 1;
    }

    /*
     * Enums are compared as integers so that the comparison is independent of
     * how the compiler chooses to represent the enumeration.
     */
    {
        const int lhs_priority = (int)lhs->priority_class;
        const int rhs_priority = (int)rhs->priority_class;
        if (lhs_priority < rhs_priority) {
            return -1;
        }
        if (lhs_priority > rhs_priority) {
            return 1;
        }
    }

    if (lhs->sequence < rhs->sequence) {
        return -1;
    }
    if (lhs->sequence > rhs->sequence) {
        return 1;
    }
    return 0;
}

const char *cancestry_event_type_name(cancestry_event_type_t type)
{
    switch (type) {
    case CANCESTRY_EVENT_TYPE_CAN_RX:
        return "can_rx";
    case CANCESTRY_EVENT_TYPE_SIGNAL_CHANGED:
        return "signal_changed";
    case CANCESTRY_EVENT_TYPE_TIMER_EXPIRED:
        return "timer_expired";
    case CANCESTRY_EVENT_TYPE_STATE_ENTERED:
        return "state_entered";
    case CANCESTRY_EVENT_TYPE_STATE_EXITED:
        return "state_exited";
    case CANCESTRY_EVENT_TYPE_FAULT_RAISED:
        return "fault_raised";
    case CANCESTRY_EVENT_TYPE_POWER_MODE_CHANGED:
        return "power_mode_changed";
    case CANCESTRY_EVENT_TYPE_INVALID:
    case CANCESTRY_EVENT_TYPE_COUNT:
    default:
        return "invalid";
    }
}

const char *cancestry_priority_class_name(cancestry_priority_class_t priority_class)
{
    switch (priority_class) {
    case CANCESTRY_PRIORITY_CLASS_FAULT:
        return "FAULT";
    case CANCESTRY_PRIORITY_CLASS_MODE:
        return "MODE";
    case CANCESTRY_PRIORITY_CLASS_TIMER:
        return "TIMER";
    case CANCESTRY_PRIORITY_CLASS_CAN_RX:
        return "CAN_RX";
    case CANCESTRY_PRIORITY_CLASS_HOST:
        return "HOST";
    case CANCESTRY_PRIORITY_CLASS_GENERATED:
        return "GENERATED";
    case CANCESTRY_PRIORITY_CLASS_TRACE:
        return "TRACE";
    case CANCESTRY_PRIORITY_CLASS_COUNT:
    default:
        return "INVALID";
    }
}

const char *cancestry_fault_severity_name(cancestry_fault_severity_t severity)
{
    switch (severity) {
    case CANCESTRY_FAULT_SEVERITY_INFO:
        return "INFO";
    case CANCESTRY_FAULT_SEVERITY_WARNING:
        return "WARNING";
    case CANCESTRY_FAULT_SEVERITY_ERROR:
        return "ERROR";
    case CANCESTRY_FAULT_SEVERITY_CRITICAL:
        return "CRITICAL";
    case CANCESTRY_FAULT_SEVERITY_COUNT:
    default:
        return "INVALID";
    }
}

const char *cancestry_mode_name(cancestry_mode_t mode)
{
    switch (mode) {
    case CANCESTRY_MODE_BOOT:
        return "BOOT";
    case CANCESTRY_MODE_CONFIG:
        return "CONFIG";
    case CANCESTRY_MODE_LISTEN_ONLY:
        return "LISTEN_ONLY";
    case CANCESTRY_MODE_ACTIVE:
        return "ACTIVE";
    case CANCESTRY_MODE_SAFE:
        return "SAFE";
    case CANCESTRY_MODE_OFF:
        return "OFF";
    case CANCESTRY_MODE_COUNT:
    default:
        return "INVALID";
    }
}
