#include "pa_protocol.h"

#include "pa_store.h"
#include "passport_anki.h"

#include <string.h>

typedef struct {
    const char *bytes;
    size_t length;
} pa_token;

static bool pa_is_separator(unsigned char ch)
{
    return ch == ' ' || ch == '\t' || ch == '\v' || ch == '\f' || ch == '\r';
}

static bool pa_token_equal(pa_token token, const char *text)
{
    size_t length = strlen(text);
    return token.length == length && memcmp(token.bytes, text, length) == 0;
}

static bool pa_parse_u32(pa_token token, uint32_t *out)
{
    if (token.length == 0) {
        return false;
    }
    uint32_t value = 0;
    for (size_t i = 0; i < token.length; ++i) {
        unsigned char ch = (unsigned char)token.bytes[i];
        if (ch < '0' || ch > '9') {
            return false;
        }
        uint32_t digit = ch - '0';
        if (value > (UINT32_MAX - digit) / 10u) {
            return false;
        }
        value = value * 10u + digit;
    }
    *out = value;
    return true;
}

static int pa_hex_value(unsigned char ch)
{
    if (ch >= '0' && ch <= '9') {
        return ch - '0';
    }
    if (ch >= 'a' && ch <= 'f') {
        return ch - 'a' + 10;
    }
    if (ch >= 'A' && ch <= 'F') {
        return ch - 'A' + 10;
    }
    return -1;
}

static bool pa_decode_raw_hex(pa_token token, uint8_t *out, size_t capacity, size_t *out_length)
{
    if (token.length == 0 || (token.length & 1u) != 0 || token.length / 2 > capacity) {
        return false;
    }
    size_t output_length = token.length / 2;
    for (size_t i = 0; i < output_length; ++i) {
        int high = pa_hex_value((unsigned char)token.bytes[i * 2]);
        int low = pa_hex_value((unsigned char)token.bytes[i * 2 + 1]);
        if (high < 0 || low < 0) {
            return false;
        }
        out[i] = (uint8_t)((high << 4) | low);
    }
    *out_length = output_length;
    return true;
}

static bool pa_decode_hex(pa_token token, char *out, size_t capacity)
{
    if (token.length == 0 || (token.length & 1u) != 0 || token.length / 2 >= capacity) {
        return false;
    }
    size_t output_length = token.length / 2;
    for (size_t i = 0; i < output_length; ++i) {
        int high = pa_hex_value((unsigned char)token.bytes[i * 2]);
        int low = pa_hex_value((unsigned char)token.bytes[i * 2 + 1]);
        if (high < 0 || low < 0) {
            return false;
        }
        unsigned char byte = (unsigned char)((high << 4) | low);
        if (byte == 0) {
            return false;
        }
        out[i] = (char)byte;
    }
    out[output_length] = '\0';
    return pa_text_valid(out, capacity);
}

static bool pa_split_tokens(const char *line, size_t length, pa_token tokens[6], size_t *count)
{
    size_t cursor = 0;
    *count = 0;
    while (cursor < length) {
        while (cursor < length && pa_is_separator((unsigned char)line[cursor])) {
            ++cursor;
        }
        if (cursor == length) {
            break;
        }
        if (*count == 6) {
            return false;
        }
        size_t start = cursor;
        while (cursor < length && !pa_is_separator((unsigned char)line[cursor])) {
            if (line[cursor] == '\n') {
                return false;
            }
            ++cursor;
        }
        tokens[*count] = (pa_token){.bytes = line + start, .length = cursor - start};
        ++*count;
    }
    return *count != 0;
}

void pa_protocol_write_hex(const uint8_t *data, size_t length, char *out)
{
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < length; ++i) {
        out[i * 2u] = hex[data[i] >> 4];
        out[i * 2u + 1u] = hex[data[i] & 0x0fu];
    }
    out[length * 2u] = '\0';
}

bool pa_protocol_parse(const char *line, size_t length, pa_protocol_command *out)
{
    if (line == NULL || out == NULL || length == 0 || length > PA_PROTOCOL_MAX_LINE ||
        memchr(line, '\0', length) != NULL) {
        return false;
    }
    if (line[length - 1] == '\n') {
        --length;
        if (length > 0 && line[length - 1] == '\r') {
            --length;
        }
    }
    if (length == 0) {
        return false;
    }

    pa_token tokens[6];
    size_t count;
    if (!pa_split_tokens(line, length, tokens, &count)) {
        return false;
    }

    pa_protocol_command candidate;
    memset(&candidate, 0, sizeof(candidate));
    if (pa_token_equal(tokens[0], "INFO") && count == 1) {
        candidate.kind = PA_PROTOCOL_INFO;
    } else if (pa_token_equal(tokens[0], "COMMIT") && count == 1) {
        candidate.kind = PA_PROTOCOL_COMMIT;
    } else if (pa_token_equal(tokens[0], "ABORT") && count == 1) {
        candidate.kind = PA_PROTOCOL_ABORT;
    } else if (pa_token_equal(tokens[0], "RESUME") && count == 1) {
        candidate.kind = PA_PROTOCOL_RESUME;
    } else if (pa_token_equal(tokens[0], "BEGIN") && count == 4) {
        candidate.kind = PA_PROTOCOL_BEGIN;
        if (!pa_parse_u32(tokens[1], &candidate.value) || candidate.value < 1 ||
            candidate.value > PA_STORE_MAX_CARDS || !pa_parse_u32(tokens[2], &candidate.crc) ||
            !pa_parse_u32(tokens[3], &candidate.length) ||
            candidate.length < candidate.value * 11u || candidate.length > PA_STORE_PAYLOAD_MAX) {
            return false;
        }
    } else if (pa_token_equal(tokens[0], "CHUNK") && count == 3) {
        size_t bytes = 0;
        if (!pa_parse_u32(tokens[1], &candidate.value) ||
            !pa_decode_raw_hex(tokens[2], candidate.chunk, PA_PROTOCOL_CHUNK_MAX, &bytes) || bytes == 0) {
            return false;
        }
        candidate.kind = PA_PROTOCOL_CHUNK;
        candidate.length = (uint32_t)bytes;
    } else if (pa_token_equal(tokens[0], "DAY") && count == 2) {
        candidate.kind = PA_PROTOCOL_DAY;
        if (!pa_parse_u32(tokens[1], &candidate.value)) {
            return false;
        }
    } else if (pa_token_equal(tokens[0], "EXPORT") && count == 1) {
        candidate.kind = PA_PROTOCOL_EXPORT;
    } else if (pa_token_equal(tokens[0], "XRESUME") && count == 1) {
        candidate.kind = PA_PROTOCOL_XRESUME;
    } else if (pa_token_equal(tokens[0], "XCHUNK") && count == 2) {
        candidate.kind = PA_PROTOCOL_XCHUNK;
        if (!pa_parse_u32(tokens[1], &candidate.value)) {
            return false;
        }
    } else if (pa_token_equal(tokens[0], "CARD") && count == 5) {
        candidate.kind = PA_PROTOCOL_CARD;
        if (!pa_parse_u32(tokens[1], &candidate.id) || candidate.id == 0 ||
            !pa_decode_hex(tokens[2], candidate.deck, sizeof(candidate.deck)) ||
            !pa_decode_hex(tokens[3], candidate.front, sizeof(candidate.front)) ||
            !pa_decode_hex(tokens[4], candidate.back, sizeof(candidate.back))) {
            return false;
        }
    } else {
        return false;
    }

    *out = candidate;
    return true;
}
