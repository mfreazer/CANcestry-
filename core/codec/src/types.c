/*
 * CANcestry - codec engine: status and enum name helpers.
 *
 * No allocation, no global state.
 */

#include "cancestry/codec/types.h"

#include <stdbool.h>

bool cancestry_codec_status_is_warning(cancestry_codec_status_t status)
{
    return (int)status > 0;
}

const char *cancestry_codec_status_name(cancestry_codec_status_t status)
{
    switch (status) {
    case CANCESTRY_CODEC_OK:
        return "OK";
    case CANCESTRY_CODEC_WARN_FRAME_TOO_SHORT:
        return "WARN_FRAME_TOO_SHORT";
    case CANCESTRY_CODEC_WARN_VALUE_CLAMPED:
        return "WARN_VALUE_CLAMPED";
    case CANCESTRY_CODEC_WARN_ENUM_UNKNOWN:
        return "WARN_ENUM_UNKNOWN";
    case CANCESTRY_CODEC_ERR_NULL:
        return "ERR_NULL";
    case CANCESTRY_CODEC_ERR_ARGUMENT:
        return "ERR_ARGUMENT";
    case CANCESTRY_CODEC_ERR_NOT_FOUND:
        return "ERR_NOT_FOUND";
    case CANCESTRY_CODEC_ERR_AMBIGUOUS:
        return "ERR_AMBIGUOUS";
    case CANCESTRY_CODEC_ERR_CONFLICT:
        return "ERR_CONFLICT";
    case CANCESTRY_CODEC_ERR_RANGE:
        return "ERR_RANGE";
    case CANCESTRY_CODEC_ERR_FRAME_TOO_SHORT:
        return "ERR_FRAME_TOO_SHORT";
    case CANCESTRY_CODEC_ERR_PARSE:
        return "ERR_PARSE";
    case CANCESTRY_CODEC_ERR_NO_MEMORY:
        return "ERR_NO_MEMORY";
    case CANCESTRY_CODEC_ERR_CAPACITY:
        return "ERR_CAPACITY";
    default:
        return "UNKNOWN";
    }
}

const char *cancestry_codec_signal_type_name(cancestry_codec_signal_type_t type)
{
    switch (type) {
    case CANCESTRY_CODEC_SIGNAL_TYPE_UINT:
        return "uint";
    case CANCESTRY_CODEC_SIGNAL_TYPE_INT:
        return "int";
    case CANCESTRY_CODEC_SIGNAL_TYPE_BOOLEAN:
        return "boolean";
    case CANCESTRY_CODEC_SIGNAL_TYPE_ENUM:
        return "enum";
    default:
        return "invalid";
    }
}

const char *cancestry_codec_endianness_name(cancestry_codec_endianness_t endianness)
{
    switch (endianness) {
    case CANCESTRY_CODEC_ENDIANNESS_LITTLE:
        return "little";
    case CANCESTRY_CODEC_ENDIANNESS_BIG:
        return "big";
    default:
        return "invalid";
    }
}
