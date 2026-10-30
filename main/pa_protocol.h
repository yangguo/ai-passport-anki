#ifndef PA_PROTOCOL_H
#define PA_PROTOCOL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    PA_PROTOCOL_INFO,
    PA_PROTOCOL_BEGIN,
    PA_PROTOCOL_CARD,
    PA_PROTOCOL_CHUNK,
    PA_PROTOCOL_COMMIT,
    PA_PROTOCOL_ABORT,
    PA_PROTOCOL_DAY,
    PA_PROTOCOL_RESUME
} pa_protocol_kind;

#define PA_PROTOCOL_CHUNK_MAX 480u

typedef struct {
    pa_protocol_kind kind;
    uint32_t value;
    uint32_t crc;
    uint32_t length;
    uint32_t id;
    char deck[48];
    char front[256];
    char back[256];
    uint8_t chunk[PA_PROTOCOL_CHUNK_MAX];
} pa_protocol_command;

#define PA_PROTOCOL_MAX_LINE 1200u

bool pa_protocol_parse(const char *line, size_t length, pa_protocol_command *out);

#endif
