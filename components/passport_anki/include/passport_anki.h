#ifndef PASSPORT_ANKI_H
#define PASSPORT_ANKI_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define PA_MAX_CARDS 64
#define PA_TEXT_BYTES 256
#define PA_DECK_BYTES 48
#define PA_SNAPSHOT_MAX 40000
#define PA_NO_CARD UINT16_MAX
#define PA_MAX_INTERVAL 3650

typedef enum { PA_NEW, PA_LEARNING, PA_REVIEW } pa_stage;
typedef enum { PA_AGAIN, PA_HARD, PA_GOOD, PA_EASY } pa_rating;
typedef struct {
 uint32_t id, due;
 uint16_t interval, ease, reviews, lapses;
 uint8_t stage, step;
 char deck[PA_DECK_BYTES], front[PA_TEXT_BYTES], back[PA_TEXT_BYTES];
} pa_card;
typedef struct { uint32_t now; uint16_t count; pa_card cards[PA_MAX_CARDS]; } pa_collection;
typedef struct { uint16_t current; bool revealed; } pa_session;
void pa_init(pa_collection *c);
bool pa_add(pa_collection *c, uint32_t id, const char *deck, const char *front, const char *back);
bool pa_text_valid(const char *s, size_t cap);
uint16_t pa_next(const pa_collection *c, const char *deck);
void pa_begin(pa_session *s, const pa_collection *c, const char *deck);
bool pa_flip(pa_session *s);
bool pa_grade(pa_collection *c, pa_session *s, pa_rating rating);
bool pa_advance(pa_collection *c, uint32_t minutes);
size_t pa_encode(const pa_collection *c, uint8_t *out, size_t cap);
bool pa_decode(pa_collection *out, const uint8_t *data, size_t size);
uint32_t pa_crc32(const uint8_t *data, size_t size);
#endif
