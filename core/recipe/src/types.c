/*
 * CANcestry - recipe engine: status and enum name helpers.
 *
 * No allocation, no global state (SYS-NF-002).
 */

#include "cancestry/recipe/types.h"

#include <stdbool.h>

bool cancestry_recipe_status_is_ok(cancestry_recipe_status_t status)
{
    return (int)status >= 0;
}

const char *cancestry_recipe_status_name(cancestry_recipe_status_t status)
{
    switch (status) {
    case CANCESTRY_RECIPE_OK:
        return "OK";
    case CANCESTRY_RECIPE_ERR_NULL:
        return "ERR_NULL";
    case CANCESTRY_RECIPE_ERR_ARGUMENT:
        return "ERR_ARGUMENT";
    case CANCESTRY_RECIPE_ERR_NOT_FOUND:
        return "ERR_NOT_FOUND";
    case CANCESTRY_RECIPE_ERR_AMBIGUOUS:
        return "ERR_AMBIGUOUS";
    case CANCESTRY_RECIPE_ERR_DENIED:
        return "ERR_DENIED";
    case CANCESTRY_RECIPE_ERR_EXPRESSION:
        return "ERR_EXPRESSION";
    case CANCESTRY_RECIPE_ERR_ENCODING:
        return "ERR_ENCODING";
    case CANCESTRY_RECIPE_ERR_CAPACITY:
        return "ERR_CAPACITY";
    case CANCESTRY_RECIPE_ERR_PARSE:
        return "ERR_PARSE";
    case CANCESTRY_RECIPE_ERR_CONFLICT:
        return "ERR_CONFLICT";
    case CANCESTRY_RECIPE_ERR_NO_MEMORY:
        return "ERR_NO_MEMORY";
    case CANCESTRY_RECIPE_ERR_UNSUPPORTED:
        return "ERR_UNSUPPORTED";
    default:
        return "UNKNOWN";
    }
}

const char *cancestry_recipe_trigger_event_name(cancestry_recipe_trigger_event_t event)
{
    switch (event) {
    case CANCESTRY_RECIPE_TRIGGER_CAN_RX:
        return "can_rx";
    case CANCESTRY_RECIPE_TRIGGER_SIGNAL_CHANGED:
        return "signal_changed";
    case CANCESTRY_RECIPE_TRIGGER_TIMER_EXPIRED:
        return "timer_expired";
    case CANCESTRY_RECIPE_TRIGGER_TIMEOUT:
        return "timeout";
    case CANCESTRY_RECIPE_TRIGGER_FAULT_RAISED:
        return "fault_raised";
    case CANCESTRY_RECIPE_TRIGGER_POWER_MODE_CHANGED:
        return "power_mode_changed";
    default:
        return "invalid";
    }
}

cancestry_event_type_t cancestry_recipe_trigger_event_type(cancestry_recipe_trigger_event_t event)
{
    switch (event) {
    case CANCESTRY_RECIPE_TRIGGER_CAN_RX:
        return CANCESTRY_EVENT_TYPE_CAN_RX;
    case CANCESTRY_RECIPE_TRIGGER_SIGNAL_CHANGED:
        return CANCESTRY_EVENT_TYPE_SIGNAL_CHANGED;
    case CANCESTRY_RECIPE_TRIGGER_TIMER_EXPIRED:
        return CANCESTRY_EVENT_TYPE_TIMER_EXPIRED;
    case CANCESTRY_RECIPE_TRIGGER_FAULT_RAISED:
        return CANCESTRY_EVENT_TYPE_FAULT_RAISED;
    case CANCESTRY_RECIPE_TRIGGER_POWER_MODE_CHANGED:
        return CANCESTRY_EVENT_TYPE_POWER_MODE_CHANGED;
    default:
        /* timeout has no event-model type; such triggers never match. */
        return CANCESTRY_EVENT_TYPE_INVALID;
    }
}

const char *cancestry_recipe_action_kind_name(cancestry_recipe_action_kind_t kind)
{
    switch (kind) {
    case CANCESTRY_RECIPE_ACTION_SEND_MESSAGE:
        return "send_message";
    case CANCESTRY_RECIPE_ACTION_SET_SIGNAL:
        return "set_signal";
    case CANCESTRY_RECIPE_ACTION_SET_VARIABLE:
        return "set_variable";
    case CANCESTRY_RECIPE_ACTION_START_TIMER:
        return "start_timer";
    case CANCESTRY_RECIPE_ACTION_STOP_TIMER:
        return "stop_timer";
    case CANCESTRY_RECIPE_ACTION_RESET_TIMER:
        return "reset_timer";
    case CANCESTRY_RECIPE_ACTION_LOG:
        return "log";
    case CANCESTRY_RECIPE_ACTION_RAISE_FAULT:
        return "raise_fault";
    default:
        return "invalid";
    }
}

const char *cancestry_recipe_on_error_name(cancestry_recipe_on_error_t on_error)
{
    switch (on_error) {
    case CANCESTRY_RECIPE_ON_ERROR_STOP:
        return "stop";
    case CANCESTRY_RECIPE_ON_ERROR_CONTINUE:
        return "continue";
    default:
        return "invalid";
    }
}

const char *cancestry_recipe_log_level_name(cancestry_recipe_log_level_t level)
{
    switch (level) {
    case CANCESTRY_RECIPE_LOG_INFO:
        return "info";
    case CANCESTRY_RECIPE_LOG_WARNING:
        return "warning";
    case CANCESTRY_RECIPE_LOG_ERROR:
        return "error";
    default:
        return "invalid";
    }
}
