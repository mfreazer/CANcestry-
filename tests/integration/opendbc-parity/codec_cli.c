/*
 * CANcestry - codec map decode CLI (host-side test helper).
 *
 * Loads a codec map YAML file, then decodes CAN frames supplied on stdin and
 * prints the decoded signals deterministically. Used by the opendbc parity
 * harness (tests/integration/opendbc-parity) to drive the CANcestry C decoder
 * from Python.
 *
 * This is a *tool*, not part of core/: it may allocate (it is never linked
 * into firmware).
 *
 * Usage:
 *   codec_cli <codec_map.yaml>
 *
 * stdin: one frame per line:
 *   <can_id> <hex bytes>
 * e.g.
 *   297 3412000000000000
 *
 * stdout: one line per frame:
 *   <can_id> <signal>=<value> ...
 * Values are printed as integers for integer kinds and full-precision decimal
 * for REAL values; booleans are printed as 0 or 1. Signal order follows the
 * codec map, so output is deterministic for a fixed input (SYS-NF-001).
 */

#include "cancestry/codec/loader.h"
#include "cancestry/codec/decoder.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CLI_MAX_FRAME_BYTES 8u

static char *read_file(const char *path, size_t *out_len)
{
    FILE *fh = fopen(path, "rb");
    long size;
    char *buf;

    if (fh == NULL) {
        return NULL;
    }
    if (fseek(fh, 0, SEEK_END) != 0) {
        fclose(fh);
        return NULL;
    }
    size = ftell(fh);
    if (size < 0 || fseek(fh, 0, SEEK_SET) != 0) {
        fclose(fh);
        return NULL;
    }
    buf = (char *)malloc((size_t)size + 1u);
    if (buf == NULL) {
        fclose(fh);
        return NULL;
    }
    if (fread(buf, 1u, (size_t)size, fh) != (size_t)size) {
        free(buf);
        fclose(fh);
        return NULL;
    }
    fclose(fh);
    buf[size] = '\0';
    *out_len = (size_t)size;
    return buf;
}

static int parse_frame(const char *line, unsigned long *can_id, uint8_t *data,
                       size_t *length)
{
    unsigned long id = 0u;
    size_t nbytes = 0u;
    const char *p = line;
    char hex[3];

    while (*p == ' ' || *p == '\t') {
        ++p;
    }
    while (*p >= '0' && *p <= '9') {
        id = id * 10u + (unsigned long)(*p - '0');
        ++p;
    }
    if (id > 0x1FFFFFFFul) {
        return -1;
    }
    while (*p == ' ' || *p == '\t') {
        ++p;
    }
    while (*p != '\0' && *p != '\n' && *p != '\r' && nbytes < CLI_MAX_FRAME_BYTES) {
        unsigned int v;
        if (p[0] == '\0' || p[1] == '\0') {
            break;
        }
        hex[0] = p[0];
        hex[1] = p[1];
        hex[2] = '\0';
        if (sscanf(hex, "%2x", &v) != 1) {
            return -1;
        }
        data[nbytes++] = (uint8_t)v;
        p += 2;
    }
    if (nbytes == 0u) {
        return -1;
    }
    *can_id = id;
    *length = nbytes;
    return 0;
}

static void print_value(const cancestry_value_t *value)
{
    switch (value->kind) {
    case CANCESTRY_VALUE_KIND_BOOL:
        printf("%d", value->value.boolean ? 1 : 0);
        break;
    case CANCESTRY_VALUE_KIND_INT:
        printf("%lld", (long long)value->value.integer);
        break;
    case CANCESTRY_VALUE_KIND_UINT:
        printf("%llu", (unsigned long long)value->value.unsigned_integer);
        break;
    case CANCESTRY_VALUE_KIND_REAL:
        printf("%.17g", value->value.real);
        break;
    case CANCESTRY_VALUE_KIND_UNSET:
    default:
        printf("nan");
        break;
    }
}

int main(int argc, char **argv)
{
    cancestry_codec_map_t *map;
    cancestry_codec_load_error_t error;
    char *text;
    size_t text_len;
    char line[256];

    if (argc != 2) {
        fprintf(stderr, "usage: %s <codec_map.yaml>\n", argv[0]);
        return 2;
    }

    text = read_file(argv[1], &text_len);
    if (text == NULL) {
        fprintf(stderr, "failed to read %s\n", argv[1]);
        return 2;
    }

    map = cancestry_codec_map_load(text, text_len, &error);
    if (map == NULL) {
        fprintf(stderr, "failed to load codec map: %s (line %u, column %u)\n",
                error.message, (unsigned)error.line, (unsigned)error.column);
        free(text);
        return 2;
    }
    free(text);

    while (fgets(line, sizeof(line), stdin) != NULL) {
        unsigned long can_id;
        uint8_t data[CLI_MAX_FRAME_BYTES];
        size_t length;
        const cancestry_codec_message_t *message;
        cancestry_decoded_signal_t signals[128];
        size_t count = 0u;
        size_t i;
        cancestry_codec_status_t status;
        cancestry_codec_warnings_t warnings = {0u, 0u, 0u};

        if (parse_frame(line, &can_id, data, &length) != 0) {
            continue;
        }

        message = cancestry_codec_map_find_message(map, (uint32_t)can_id);
        if (message == NULL) {
            continue;
        }
        if (message->signal_count > 128u) {
            fprintf(stderr, "message %u has too many signals\n", (unsigned)can_id);
            continue;
        }

        status = cancestry_codec_decode_frame(map, (uint32_t)can_id, data, length,
                                              signals, 128u, &count, &warnings);
        if (status < 0 || status == CANCESTRY_CODEC_WARN_FRAME_TOO_SHORT) {
            continue;
        }

        printf("%lu", can_id);
        for (i = 0u; i < count; ++i) {
            printf(" %s=", signals[i].signal->name);
            print_value(&signals[i].value);
        }
        printf("\n");
    }

    cancestry_codec_map_free(map);
    return 0;
}
