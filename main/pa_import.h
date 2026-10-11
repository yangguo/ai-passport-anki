#ifndef PA_IMPORT_H
#define PA_IMPORT_H

#include "passport_anki.h"

typedef bool (*pa_import_validate_fn)(const pa_card *card, void *user);
typedef bool (*pa_import_persist_fn)(const pa_collection *collection, void *user);

typedef struct {
    uint16_t expected;
    uint16_t received;
    bool active;
    bool invalid;
} pa_import_tx;

bool pa_import_begin(pa_import_tx *tx, pa_collection *staging,
                     const pa_collection *live, uint16_t expected);
bool pa_import_add(pa_import_tx *tx, pa_collection *staging, uint32_t id,
                   const char *deck, const char *front, const char *back,
                   pa_import_validate_fn validate, void *user);
bool pa_import_commit(pa_import_tx *tx, pa_collection *staging,
                      pa_collection *live, pa_import_persist_fn persist,
                      void *user);
void pa_import_abort(pa_import_tx *tx);

#endif
