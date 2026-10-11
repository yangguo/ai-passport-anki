#include "pa_import.h"

#include <string.h>

bool pa_import_begin(pa_import_tx *tx, pa_collection *staging,
                     const pa_collection *live, uint16_t expected) {
    if (!tx || !staging || !live || tx->active || expected == 0 ||
        expected > PA_MAX_CARDS) return false;
    pa_init(staging);
    staging->now = live->now;
    tx->expected = expected;
    tx->received = 0;
    tx->invalid = false;
    tx->active = true;
    return true;
}

bool pa_import_add(pa_import_tx *tx, pa_collection *staging, uint32_t id,
                   const char *deck, const char *front, const char *back,
                   pa_import_validate_fn validate, void *user) {
    if (!tx || !staging || !tx->active || tx->invalid ||
        tx->received >= tx->expected || !pa_add(staging, id, deck, front, back)) {
        if (tx && tx->active) tx->invalid = true;
        return false;
    }
    if (validate && !validate(&staging->cards[staging->count - 1], user)) {
        tx->invalid = true;
        return false;
    }
    ++tx->received;
    return true;
}

bool pa_import_commit(pa_import_tx *tx, pa_collection *staging,
                      pa_collection *live, pa_import_persist_fn persist,
                      void *user) {
    if (!tx || !staging || !live || !persist || !tx->active || tx->invalid ||
        tx->received != tx->expected) return false;
    staging->now = live->now;
    if (!persist(staging, user)) {
        pa_import_abort(tx);
        return false;
    }
    *live = *staging;
    pa_import_abort(tx);
    return true;
}

void pa_import_abort(pa_import_tx *tx) {
    if (!tx) return;
    tx->active = false;
    tx->invalid = false;
    tx->expected = 0;
    tx->received = 0;
}
