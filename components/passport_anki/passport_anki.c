#include "passport_anki.h"

#include <limits.h>
#include <string.h>

#define PA_DAY_MINUTES 1440u

static bool pa_utf8_decode(const unsigned char **cursor, uint32_t *codepoint)
{
    const unsigned char *p = *cursor;
    uint32_t value;
    unsigned need;

    if (p[0] < 0x80u) {
        *codepoint = p[0];
        *cursor = p + 1;
        return true;
    }
    if (p[0] >= 0xc2u && p[0] <= 0xdfu) {
        value = (uint32_t)(p[0] & 0x1fu);
        need = 1;
    } else if (p[0] >= 0xe0u && p[0] <= 0xefu) {
        value = (uint32_t)(p[0] & 0x0fu);
        need = 2;
    } else if (p[0] >= 0xf0u && p[0] <= 0xf4u) {
        value = (uint32_t)(p[0] & 0x07u);
        need = 3;
    } else {
        return false;
    }

    for (unsigned i = 1; i <= need; ++i) {
        if (p[i] == '\0' || (p[i] & 0xc0u) != 0x80u) {
            return false;
        }
        value = (value << 6) | (uint32_t)(p[i] & 0x3fu);
    }
    if ((need == 1 && value < 0x80u) ||
        (need == 2 && value < 0x800u) ||
        (need == 3 && value < 0x10000u) ||
        (value >= 0xd800u && value <= 0xdfffu) || value > 0x10ffffu) {
        return false;
    }
    *codepoint = value;
    *cursor = p + need + 1;
    return true;
}

bool pa_text_valid(const char *s, size_t cap)
{
    if (s == NULL || cap == 0) {
        return false;
    }
    size_t length = 0;
    while (length < cap && s[length] != '\0') {
        ++length;
    }
    if (length == 0 || length == cap) {
        return false;
    }

    const unsigned char *p = (const unsigned char *)s;
    const unsigned char *end = p + length;
    while (p < end) {
        uint32_t cp;
        if (!pa_utf8_decode(&p, &cp) || p > end) {
            return false;
        }
        if ((cp < 0x20u && cp != '\n') || (cp >= 0x7fu && cp <= 0x9fu)) {
            return false;
        }
    }
    return true;
}

static bool pa_card_valid(const pa_card *card)
{
    if (card == NULL || card->id == 0 || card->ease < 1300 || card->ease > 3500 ||
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

void pa_init(pa_collection *c)
{
    if (c != NULL) {
        memset(c, 0, sizeof(*c));
    }
}

bool pa_add(pa_collection *c, uint32_t id, const char *deck, const char *front, const char *back)
{
    if (c == NULL || id == 0 || c->count >= PA_MAX_CARDS ||
        !pa_text_valid(deck, PA_DECK_BYTES) || !pa_text_valid(front, PA_TEXT_BYTES) ||
        !pa_text_valid(back, PA_TEXT_BYTES)) {
        return false;
    }
    for (uint16_t i = 0; i < c->count; ++i) {
        if (c->cards[i].id == id) {
            return false;
        }
    }

    pa_card *card = &c->cards[c->count];
    memset(card, 0, sizeof(*card));
    card->id = id;
    card->due = c->now;
    card->ease = 2500;
    card->stage = PA_NEW;
    memcpy(card->deck, deck, strlen(deck) + 1);
    memcpy(card->front, front, strlen(front) + 1);
    memcpy(card->back, back, strlen(back) + 1);
    ++c->count;
    return true;
}

uint16_t pa_next(const pa_collection *c, const char *deck)
{
    if (c == NULL || deck == NULL || c->count > PA_MAX_CARDS) {
        return PA_NO_CARD;
    }
    uint16_t best = PA_NO_CARD;
    for (uint16_t i = 0; i < c->count; ++i) {
        const pa_card *card = &c->cards[i];
        if (card->due > c->now || strcmp(card->deck, deck) != 0) {
            continue;
        }
        unsigned card_priority = card->stage == PA_LEARNING ? 0u :
                                 card->stage == PA_REVIEW ? 1u : 2u;
        unsigned best_priority = best == PA_NO_CARD ? UINT_MAX :
                                 c->cards[best].stage == PA_LEARNING ? 0u :
                                 c->cards[best].stage == PA_REVIEW ? 1u : 2u;
        if (best == PA_NO_CARD || card_priority < best_priority ||
            (card_priority == best_priority &&
             (card->due < c->cards[best].due ||
              (card->due == c->cards[best].due && card->id < c->cards[best].id)))) {
            best = i;
        }
    }
    return best;
}

void pa_begin(pa_session *s, const pa_collection *c, const char *deck)
{
    if (s != NULL) {
        s->current = pa_next(c, deck);
        s->revealed = false;
    }
}

bool pa_flip(pa_session *s)
{
    if (s == NULL || s->current == PA_NO_CARD || s->revealed) {
        return false;
    }
    s->revealed = true;
    return true;
}

static uint16_t pa_saturating_increment(uint16_t value)
{
    return value == UINT16_MAX ? value : (uint16_t)(value + 1u);
}

static uint16_t pa_reduce_ease(uint16_t ease, uint16_t amount)
{
    return ease > 1300u + amount ? (uint16_t)(ease - amount) : 1300u;
}

static uint16_t pa_increase_ease(uint16_t ease)
{
    return ease > 3350u ? 3500u : (uint16_t)(ease + 150u);
}

static uint32_t pa_interval_due(uint32_t interval)
{
    return interval * PA_DAY_MINUTES;
}

static uint32_t pa_round_div(uint64_t numerator, uint32_t denominator)
{
    return (uint32_t)((numerator + denominator / 2u) / denominator);
}

bool pa_grade(pa_collection *c, pa_session *s, pa_rating rating)
{
    if (c == NULL || s == NULL || !s->revealed || s->current >= c->count ||
        c->count > PA_MAX_CARDS || rating < PA_AGAIN || rating > PA_EASY) {
        return false;
    }
    const pa_card *live = &c->cards[s->current];
    if (!pa_card_valid(live)) {
        return false;
    }

    pa_card candidate = *live;
    uint32_t delay;
    if (candidate.stage == PA_NEW || candidate.stage == PA_LEARNING) {
        bool was_learning_step_one = candidate.stage == PA_LEARNING && candidate.step == 1;
        if (candidate.stage == PA_NEW) {
            candidate.stage = PA_LEARNING;
            candidate.step = 0;
        }
        if (rating == PA_AGAIN) {
            candidate.step = 0;
            delay = 1;
        } else if (rating == PA_HARD) {
            delay = 6;
        } else if (rating == PA_GOOD && candidate.step == 0) {
            candidate.step = 1;
            delay = 10;
        } else {
            candidate.stage = PA_REVIEW;
            candidate.step = 0;
            candidate.interval = was_learning_step_one && rating == PA_GOOD ? 1 : 4;
            delay = pa_interval_due(candidate.interval);
        }
    } else if (rating == PA_AGAIN) {
        candidate.stage = PA_LEARNING;
        candidate.step = 0;
        candidate.interval = 0;
        candidate.ease = pa_reduce_ease(candidate.ease, 200);
        candidate.lapses = pa_saturating_increment(candidate.lapses);
        delay = 1;
    } else {
        uint32_t interval = live->interval;
        uint32_t good_interval = pa_round_div((uint64_t)interval * live->ease, 1000u);
        if (good_interval < interval + 1u) {
            good_interval = interval + 1u;
        }
        if (rating == PA_HARD) {
            uint32_t scaled = (interval * 12u + 9u) / 10u;
            uint32_t next = interval + 1u > scaled ? interval + 1u : scaled;
            candidate.interval = (uint16_t)(next > PA_MAX_INTERVAL ? PA_MAX_INTERVAL : next);
            candidate.ease = pa_reduce_ease(candidate.ease, 150);
        } else if (rating == PA_GOOD) {
            candidate.interval = (uint16_t)(good_interval > PA_MAX_INTERVAL ? PA_MAX_INTERVAL : good_interval);
        } else {
            uint32_t easy_interval = pa_round_div((uint64_t)interval * live->ease * 13u, 10000u);
            uint32_t minimum = good_interval + 1u;
            if (easy_interval < minimum) {
                easy_interval = minimum;
            }
            candidate.interval = (uint16_t)(easy_interval > PA_MAX_INTERVAL ? PA_MAX_INTERVAL : easy_interval);
            candidate.ease = pa_increase_ease(candidate.ease);
        }
        delay = pa_interval_due(candidate.interval);
    }

    if (delay > UINT32_MAX - c->now) {
        return false;
    }
    candidate.due = c->now + delay;
    candidate.reviews = pa_saturating_increment(candidate.reviews);
    if (!pa_card_valid(&candidate)) {
        return false;
    }
    c->cards[s->current] = candidate;
    s->current = PA_NO_CARD;
    s->revealed = false;
    return true;
}

bool pa_advance(pa_collection *c, uint32_t minutes)
{
    if (c == NULL || minutes > UINT32_MAX - c->now) {
        return false;
    }
    c->now += minutes;
    return true;
}
