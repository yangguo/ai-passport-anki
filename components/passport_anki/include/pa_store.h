#ifndef PA_STORE_H
#define PA_STORE_H

#include "passport_anki.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Flash-backed collection for up to 12,000 cards.
 * Card text lives in the cards partition and is read per card.
 * Scheduling records live in wear-leveled 4 KiB chunks in the sched partition.
 * Steady-state RAM is one pa_store (see PA_STORE_RAM_BUDGET), not one object per card.
 *
 * The legacy PAK1 snapshot (pa_encode/pa_decode, 64 cards) still migrates in. */

#define PA_STORE_MAX_CARDS 12000u
#define PA_STORE_TEXT_MAX 511u
#define PA_STORE_MAX_DECKS 64u
#define PA_STORE_SECTOR 4096u
#define PA_INDEX_ENTRY 16u
#define PA_INDEX_BYTES (PA_STORE_MAX_CARDS * PA_INDEX_ENTRY)
#define PA_INDEX_OFF PA_STORE_SECTOR
#define PA_PAYLOAD_OFF (PA_INDEX_OFF + ((PA_INDEX_BYTES + PA_STORE_SECTOR - 1u) & ~(PA_STORE_SECTOR - 1u)))
/* Factory stays 64 KiB-aligned at 0x50000. NVS stays 256 KiB so a legacy PAK1 blob can be read. */
#define PA_CARDS_PARTITION_SIZE 0x460000u
#define PA_SCHED_PARTITION_SIZE 0xD0000u
#define PA_STORE_SLOT_SIZE (PA_CARDS_PARTITION_SIZE / 2u)
#define PA_STORE_PAYLOAD_MAX (PA_STORE_SLOT_SIZE - PA_PAYLOAD_OFF)
#define PA_SCHED_HEADER 32u
#define PA_SCHED_RECORD 20u
#define PA_SCHED_PER_CHUNK ((PA_STORE_SECTOR - PA_SCHED_HEADER) / PA_SCHED_RECORD)
#define PA_SCHED_CHUNKS ((PA_STORE_MAX_CARDS + PA_SCHED_PER_CHUNK - 1u) / PA_SCHED_PER_CHUNK)
#define PA_POOL_CAP 240u
#define PA_META_SECTORS 4u
#define PA_STORE_RAM_BUDGET 12288u
#define PA_STORE_COMMIT_HEAP_MAX 65536u
#define PA_PROTOCOL_VERSION 2u

typedef struct pa_flash {
    void *ctx;
    uint32_t size;
    bool (*read)(void *ctx, uint32_t offset, void *dst, uint32_t length);
    bool (*write)(void *ctx, uint32_t offset, const void *src, uint32_t length);
    bool (*erase)(void *ctx, uint32_t offset, uint32_t length);
} pa_flash;

typedef enum {
    PA_STORE_EMPTY = 0,
    PA_STORE_READY,
    PA_STORE_CORRUPT
} pa_store_status;

typedef bool (*pa_store_text_fn)(const char *deck, const char *front, const char *back, void *user);

typedef struct pa_store {
    pa_flash *cards;
    pa_flash *sched;
    pa_store_status status;
    uint32_t now;
    uint32_t epoch;
    uint32_t super_generation;
    uint8_t super_slot;
    uint8_t active_slot;
    uint16_t count;
    uint16_t deck_count;
    char deck_names[PA_STORE_MAX_DECKS][PA_DECK_BYTES];
    uint16_t chunk_phys[PA_SCHED_CHUNKS];
    uint16_t erase_count[PA_POOL_CAP];
    uint16_t pool_count;
    uint8_t sector[PA_STORE_SECTOR];
    bool import_active;
    bool import_pending;
    uint16_t import_asm;
    uint8_t import_slot;
    uint32_t import_count;
    uint32_t import_crc;
    uint32_t import_length;
    uint32_t import_durable;
    uint32_t import_progress_generation;
} pa_store;

void pa_store_init(pa_store *store, pa_flash *cards, pa_flash *sched);
pa_store_status pa_store_mount(pa_store *store);
uint16_t pa_store_count(const pa_store *store);
uint32_t pa_store_now(const pa_store *store);
uint16_t pa_store_deck_count(const pa_store *store);
const char *pa_store_deck_name(const pa_store *store, uint16_t index);
bool pa_store_advance(pa_store *store, uint32_t minutes);
bool pa_store_checkpoint(pa_store *store);
uint16_t pa_store_next(pa_store *store, const char *deck);
bool pa_store_read(pa_store *store, uint16_t index, char *deck, size_t deck_cap,
                   char *front, size_t front_cap, char *back, size_t back_cap);
bool pa_store_grade(pa_store *store, uint16_t index, pa_rating rating);

/* Canonical card record, little-endian: id:u32, deck_len:u8, front_len:u16, back_len:u16, bytes. */
size_t pa_store_pack_card(uint8_t *out, size_t cap, uint32_t id, const char *deck,
                          const char *front, const char *back);

bool pa_store_import_begin(pa_store *store, uint32_t count, uint32_t crc, uint32_t length,
                           uint32_t *resume);
bool pa_store_import_write(pa_store *store, uint32_t offset, const uint8_t *data, uint32_t length,
                           uint32_t *accepted);
bool pa_store_import_commit(pa_store *store, pa_store_text_fn text_ok, void *user);
void pa_store_import_abort(pa_store *store);
bool pa_store_import_resume(const pa_store *store, uint32_t *offset, uint32_t *count,
                            uint32_t *crc, uint32_t *length);
bool pa_store_import_collection(pa_store *store, const pa_collection *collection);
bool pa_store_migrate_pak1(pa_store *store, const uint8_t *data, size_t size);

size_t pa_store_heap_in_use(void);
size_t pa_store_heap_hwm(void);
void pa_store_heap_reset(void);

#endif
