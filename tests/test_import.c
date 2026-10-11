#include "../main/pa_import.h"

#include <assert.h>
#include <string.h>

static pa_collection live;
static pa_collection staging;
static bool allow_persist;

static bool validate_card(const pa_card *card, void *user) {
    (void)user;
    return strcmp(card->deck, "Deck") == 0;
}

static bool persist_collection(const pa_collection *collection, void *user) {
    (void)user;
    assert(collection->count == 2);
    assert(collection->now == 99);
    return allow_persist;
}

int main(void) {
    pa_import_tx tx = {0};
    pa_init(&live);
    assert(pa_add(&live, 1, "Old", "old question", "old answer"));
    live.now = 10;

    assert(pa_import_begin(&tx, &staging, &live, 2));
    assert(staging.now == 10 && staging.count == 0);
    assert(!pa_import_begin(&tx, &staging, &live, 1));
    assert(pa_import_add(&tx, &staging, 2, "Deck", "q1", "a1", validate_card, NULL));
    assert(pa_import_add(&tx, &staging, 3, "Deck", "q2", "a2", validate_card, NULL));
    assert(!pa_import_add(&tx, &staging, 4, "Deck", "q3", "a3", validate_card, NULL));
    assert(tx.invalid);
    pa_import_abort(&tx);
    assert(live.count == 1 && live.cards[0].id == 1);

    assert(pa_import_begin(&tx, &staging, &live, 2));
    assert(pa_import_add(&tx, &staging, 2, "Deck", "q1", "a1", validate_card, NULL));
    assert(!pa_import_commit(&tx, &staging, &live, persist_collection, NULL));
    assert(live.count == 1 && live.cards[0].id == 1);
    assert(pa_import_add(&tx, &staging, 3, "Deck", "q2", "a2", validate_card, NULL));
    live.now = 99;
    allow_persist = false;
    assert(!pa_import_commit(&tx, &staging, &live, persist_collection, NULL));
    assert(live.count == 1 && live.now == 99 && live.cards[0].id == 1);
    assert(!tx.active);

    assert(pa_import_begin(&tx, &staging, &live, 2));
    assert(pa_import_add(&tx, &staging, 2, "Deck", "q1", "a1", validate_card, NULL));
    assert(pa_import_add(&tx, &staging, 3, "Deck", "q2", "a2", validate_card, NULL));
    allow_persist = true;
    assert(pa_import_commit(&tx, &staging, &live, persist_collection, NULL));
    assert(!tx.active && live.count == 2 && live.now == 99);
    assert(live.cards[0].id == 2 && live.cards[1].id == 3);
    return 0;
}
