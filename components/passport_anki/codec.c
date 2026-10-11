#include "passport_anki.h"

#include <string.h>

#define PA_CODEC_HEADER_SIZE 12u
#define PA_CODEC_CARD_FIXED_SIZE 18u
#define PA_CODEC_CRC_SIZE 4u
#define PA_CODEC_VERSION 1u

typedef struct {
    const uint8_t *data;
    size_t size;
    size_t offset;
} pa_reader;

typedef struct {
    uint8_t *data;
    size_t capacity;
    size_t offset;
} pa_writer;

uint32_t pa_crc32(const uint8_t *data, size_t size)
{
    uint32_t crc = 0xffffffffu;
    if (data == NULL && size != 0) {
        return 0;
    }
    for (size_t i = 0; i < size; ++i) {
        crc ^= data[i];
        for (unsigned bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^ (0xedb88320u & (uint32_t)-(int32_t)(crc & 1u));
        }
    }
    return crc ^ 0xffffffffu;
}

static bool pa_write_bytes(pa_writer *w, const void *bytes, size_t length)
{
    if (length > w->capacity - w->offset) {
        return false;
    }
    memcpy(w->data + w->offset, bytes, length);
    w->offset += length;
    return true;
}

static bool pa_write_u16(pa_writer *w, uint16_t value)
{
    const uint8_t bytes[2] = {(uint8_t)value, (uint8_t)(value >> 8)};
    return pa_write_bytes(w, bytes, sizeof(bytes));
}

static bool pa_write_u32(pa_writer *w, uint32_t value)
{
    const uint8_t bytes[4] = {(uint8_t)value, (uint8_t)(value >> 8),
                              (uint8_t)(value >> 16), (uint8_t)(value >> 24)};
    return pa_write_bytes(w, bytes, sizeof(bytes));
}

static bool pa_write_string(pa_writer *w, const char *text)
{
    size_t length = strlen(text);
    return length <= UINT16_MAX && pa_write_u16(w, (uint16_t)length) &&
           pa_write_bytes(w, text, length);
}

static bool pa_read_bytes(pa_reader *r, void *out, size_t length)
{
    if (length > r->size - r->offset) {
        return false;
    }
    if (out != NULL) {
        memcpy(out, r->data + r->offset, length);
    }
    r->offset += length;
    return true;
}

static bool pa_read_u16(pa_reader *r, uint16_t *value)
{
    uint8_t bytes[2];
    if (!pa_read_bytes(r, bytes, sizeof(bytes))) {
        return false;
    }
    *value = (uint16_t)bytes[0] | (uint16_t)((uint16_t)bytes[1] << 8);
    return true;
}

static bool pa_read_u32(pa_reader *r, uint32_t *value)
{
    uint8_t bytes[4];
    if (!pa_read_bytes(r, bytes, sizeof(bytes))) {
        return false;
    }
    *value = (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
             ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
    return true;
}

static bool pa_read_string(pa_reader *r, char *out, size_t capacity)
{
    uint16_t length;
    if (!pa_read_u16(r, &length) || length == 0 || length >= capacity ||
        length > r->size - r->offset) {
        return false;
    }
    if (!pa_read_bytes(r, out, length)) {
        return false;
    }
    if (memchr(out, '\0', length) != NULL) {
        return false;
    }
    out[length] = '\0';
    return pa_text_valid(out, capacity);
}

static bool pa_read_card(pa_reader *r, pa_card *card)
{
    return pa_read_u32(r, &card->id) && pa_read_u32(r, &card->due) &&
           pa_read_u16(r, &card->interval) && pa_read_u16(r, &card->ease) &&
           pa_read_u16(r, &card->reviews) && pa_read_u16(r, &card->lapses) &&
           pa_read_bytes(r, &card->stage, 1) && pa_read_bytes(r, &card->step, 1) &&
           pa_read_string(r, card->deck, sizeof(card->deck)) &&
           pa_read_string(r, card->front, sizeof(card->front)) &&
           pa_read_string(r, card->back, sizeof(card->back));
}

static bool pa_codec_card_valid(const pa_card *card)
{
    if (card->id == 0 || card->ease < 1300 || card->ease > 3500 ||
        card->interval > PA_MAX_INTERVAL || !pa_text_valid(card->deck, sizeof(card->deck)) ||
        !pa_text_valid(card->front, sizeof(card->front)) ||
        !pa_text_valid(card->back, sizeof(card->back))) {
        return false;
    }
    switch ((pa_stage)card->stage) {
    case PA_NEW:
        return card->interval == 0 && card->step == 0;
    case PA_LEARNING:
        return card->interval == 0 && card->step <= 1;
    case PA_REVIEW:
        return card->interval >= 1 && card->step == 0;
    default:
        return false;
    }
}

static bool pa_collection_valid(const pa_collection *c)
{
    if (c == NULL || c->count > PA_MAX_CARDS) {
        return false;
    }
    for (uint16_t i = 0; i < c->count; ++i) {
        if (!pa_codec_card_valid(&c->cards[i])) {
            return false;
        }
        for (uint16_t j = 0; j < i; ++j) {
            if (c->cards[i].id == c->cards[j].id) {
                return false;
            }
        }
    }
    return true;
}

size_t pa_encode(const pa_collection *c, uint8_t *out, size_t cap)
{
    if (!pa_collection_valid(c) || out == NULL || cap < PA_CODEC_HEADER_SIZE + PA_CODEC_CRC_SIZE) {
        return 0;
    }
    pa_writer w = {.data = out, .capacity = cap - PA_CODEC_CRC_SIZE, .offset = 0};
    const uint8_t magic[4] = {'P', 'A', 'K', '1'};
    if (!pa_write_bytes(&w, magic, sizeof(magic)) || !pa_write_u16(&w, PA_CODEC_VERSION) ||
        !pa_write_u16(&w, c->count) || !pa_write_u32(&w, c->now)) {
        return 0;
    }
    for (uint16_t i = 0; i < c->count; ++i) {
        const pa_card *card = &c->cards[i];
        if (!pa_write_u32(&w, card->id) || !pa_write_u32(&w, card->due) ||
            !pa_write_u16(&w, card->interval) || !pa_write_u16(&w, card->ease) ||
            !pa_write_u16(&w, card->reviews) || !pa_write_u16(&w, card->lapses) ||
            !pa_write_bytes(&w, &card->stage, 1) || !pa_write_bytes(&w, &card->step, 1) ||
            !pa_write_string(&w, card->deck) || !pa_write_string(&w, card->front) ||
            !pa_write_string(&w, card->back)) {
            return 0;
        }
    }
    uint32_t crc = pa_crc32(out, w.offset);
    w.capacity = cap;
    if (!pa_write_u32(&w, crc)) {
        return 0;
    }
    return w.offset;
}

static bool pa_read_header(pa_reader *r, uint16_t *count, uint32_t *now)
{
    uint8_t magic[4];
    uint16_t version;
    if (!pa_read_bytes(r, magic, sizeof(magic)) || memcmp(magic, "PAK1", 4) != 0 ||
        !pa_read_u16(r, &version) || version != PA_CODEC_VERSION ||
        !pa_read_u16(r, count) || *count > PA_MAX_CARDS || !pa_read_u32(r, now)) {
        return false;
    }
    return true;
}

static bool pa_decode_validate(const uint8_t *data, size_t size, uint16_t *count, uint32_t *now)
{
    if (data == NULL || size < PA_CODEC_HEADER_SIZE + PA_CODEC_CRC_SIZE || size > PA_SNAPSHOT_MAX) {
        return false;
    }
    uint32_t stored_crc = (uint32_t)data[size - 4] | ((uint32_t)data[size - 3] << 8) |
                          ((uint32_t)data[size - 2] << 16) | ((uint32_t)data[size - 1] << 24);
    if (pa_crc32(data, size - PA_CODEC_CRC_SIZE) != stored_crc) {
        return false;
    }
    pa_reader r = {.data = data, .size = size - PA_CODEC_CRC_SIZE, .offset = 0};
    if (!pa_read_header(&r, count, now)) {
        return false;
    }
    uint32_t ids[PA_MAX_CARDS];
    for (uint16_t i = 0; i < *count; ++i) {
        pa_card card;
        memset(&card, 0, sizeof(card));
        if (!pa_read_card(&r, &card) || !pa_codec_card_valid(&card)) {
            return false;
        }
        for (uint16_t j = 0; j < i; ++j) {
            if (ids[j] == card.id) {
                return false;
            }
        }
        ids[i] = card.id;
    }
    return r.offset == r.size;
}

bool pa_decode(pa_collection *out, const uint8_t *data, size_t size)
{
    uint16_t count;
    uint32_t now;
    if (out == NULL || !pa_decode_validate(data, size, &count, &now)) {
        return false;
    }

    /* The validation pass guarantees that this second parse cannot fail. */
    out->count = count;
    out->now = now;
    pa_reader r = {.data = data, .size = size - PA_CODEC_CRC_SIZE, .offset = PA_CODEC_HEADER_SIZE};
    for (uint16_t i = 0; i < count; ++i) {
        (void)pa_read_card(&r, &out->cards[i]);
    }
    memset(&out->cards[count], 0, (PA_MAX_CARDS - count) * sizeof(out->cards[0]));
    return true;
}
