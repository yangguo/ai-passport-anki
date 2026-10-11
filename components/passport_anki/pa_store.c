#include "pa_store.h"

#include <stdlib.h>
#include <string.h>

#define PA_SUPER_MAGIC "PSS2"
#define PA_PROGRESS_MAGIC "PIM1"
#define PA_SLOT_MAGIC "PCT1"
#define PA_CHUNK_MAGIC "PSCH"
#define PA_NONE 0xffffu

_Static_assert(PA_SCHED_PER_CHUNK == 203u, "schedule chunk capacity");
_Static_assert(PA_SCHED_CHUNKS == 60u, "schedule chunk count");
_Static_assert(PA_PAYLOAD_OFF % PA_STORE_SECTOR == 0u, "payload alignment");
_Static_assert(sizeof(pa_store) <= PA_STORE_RAM_BUDGET, "pa_store exceeds the RAM budget");
_Static_assert(PA_STORE_PAYLOAD_MAX > 1500u * 1024u, "card payload room");

static size_t heap_used;
static size_t heap_hwm;
static const pa_collection *adopt_cards;

size_t pa_store_heap_in_use(void) { return heap_used; }
size_t pa_store_heap_hwm(void) { return heap_hwm; }
void pa_store_heap_reset(void) { heap_hwm = heap_used; }

static void *pa_heap_alloc(size_t bytes)
{
    if (bytes > SIZE_MAX - sizeof(size_t)) {
        return NULL;
    }
    size_t *block = malloc(sizeof(size_t) + bytes);
    if (block == NULL) {
        return NULL;
    }
    *block = bytes;
    heap_used += bytes;
    if (heap_used > heap_hwm) {
        heap_hwm = heap_used;
    }
    return block + 1;
}

static void pa_heap_free(void *ptr)
{
    if (ptr == NULL) {
        return;
    }
    size_t *block = (size_t *)ptr - 1;
    heap_used -= *block;
    free(block);
}

static uint16_t rd_u16(const uint8_t *p)
{
    return (uint16_t)p[0] | (uint16_t)((uint16_t)p[1] << 8);
}

static uint32_t rd_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void wr_u16(uint8_t *p, uint16_t value)
{
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
}

static void wr_u32(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
    p[2] = (uint8_t)(value >> 16);
    p[3] = (uint8_t)(value >> 24);
}

static bool flash_ok(const pa_flash *flash, uint32_t offset, uint32_t length)
{
    return flash != NULL && flash->read != NULL && flash->write != NULL && flash->erase != NULL &&
           length <= flash->size && offset <= flash->size - length;
}

static bool flash_read(const pa_flash *flash, uint32_t offset, void *dst, uint32_t length)
{
    return length == 0 || (flash_ok(flash, offset, length) && flash->read(flash->ctx, offset, dst, length));
}

static bool flash_erase_write(pa_flash *flash, uint32_t offset, const void *src, uint32_t length)
{
    if (length == 0 || length > PA_STORE_SECTOR || (offset % PA_STORE_SECTOR) != 0 ||
        !flash_ok(flash, offset, PA_STORE_SECTOR)) {
        return false;
    }
    return flash->erase(flash->ctx, offset, PA_STORE_SECTOR) &&
           flash->write(flash->ctx, offset, src, length);
}

static uint32_t slot_base(const pa_store *store, uint8_t slot)
{
    return (uint32_t)slot * (store->cards->size / 2u);
}

static uint32_t payload_room(const pa_store *store)
{
    uint32_t slot = store->cards->size / 2u;
    return slot > PA_PAYLOAD_OFF ? slot - PA_PAYLOAD_OFF : 0u;
}

static uint16_t chunks_for(uint16_t count)
{
    if (count == 0) {
        return 0;
    }
    return (uint16_t)((count + PA_SCHED_PER_CHUNK - 1u) / PA_SCHED_PER_CHUNK);
}

static uint32_t pool_offset(uint16_t phys)
{
    return ((uint32_t)PA_META_SECTORS + phys) * PA_STORE_SECTOR;
}

static void clear_chunk_map(pa_store *store)
{
    for (uint16_t i = 0; i < PA_SCHED_CHUNKS; ++i) {
        store->chunk_phys[i] = PA_NONE;
    }
}

static bool write_super(pa_store *store)
{
    uint8_t block[36];
    memset(block, 0xff, sizeof(block));
    memcpy(block, PA_SUPER_MAGIC, 4);
    wr_u16(block + 4, PA_PROTOCOL_VERSION);
    wr_u16(block + 6, store->count);
    uint32_t generation = store->super_generation + 1u;
    if (generation == 0) {
        generation = 1;
    }
    wr_u32(block + 8, generation);
    wr_u32(block + 12, store->now);
    wr_u32(block + 16, store->epoch);
    block[20] = store->active_slot;
    block[21] = (uint8_t)store->deck_count;
    wr_u16(block + 22, 0);
    wr_u32(block + 24, 0);
    wr_u32(block + 28, pa_crc32(block, 28));
    uint8_t target = store->super_generation == 0 ? 0u : (uint8_t)(1u - store->super_slot);
    if (!flash_erase_write(store->sched, (uint32_t)target * PA_STORE_SECTOR, block, 32)) {
        return false;
    }
    store->super_generation = generation;
    store->super_slot = target;
    if (store->status != PA_STORE_CORRUPT) {
        store->status = PA_STORE_READY;
    }
    return true;
}

static bool read_super(pa_store *store, uint8_t slot, uint32_t *generation)
{
    uint8_t block[32];
    if (!flash_read(store->sched, (uint32_t)slot * PA_STORE_SECTOR, block, sizeof(block)) ||
        memcmp(block, PA_SUPER_MAGIC, 4) != 0 || rd_u16(block + 4) != PA_PROTOCOL_VERSION ||
        pa_crc32(block, 28) != rd_u32(block + 28)) {
        return false;
    }
    uint16_t count = rd_u16(block + 6);
    uint16_t decks = block[21];
    if (count > PA_STORE_MAX_CARDS || decks > PA_STORE_MAX_DECKS || block[20] > 1u) {
        return false;
    }
    store->count = count;
    store->now = rd_u32(block + 12);
    store->epoch = rd_u32(block + 16);
    store->active_slot = block[20];
    store->deck_count = decks;
    *generation = rd_u32(block + 8);
    return *generation != 0;
}

static bool load_decks(pa_store *store)
{
    if (store->count == 0) {
        store->deck_count = 0;
        return true;
    }
    uint8_t header[64];
    uint32_t base = slot_base(store, store->active_slot);
    if (!flash_read(store->cards, base, header, sizeof(header)) || memcmp(header, PA_SLOT_MAGIC, 4) != 0 ||
        rd_u16(header + 4) != 1 || rd_u16(header + 6) != store->count ||
        rd_u32(header + 8) != store->epoch || pa_crc32(header, 32) != rd_u32(header + 32)) {
        return false;
    }
    uint16_t decks = rd_u16(header + 28);
    if (decks != store->deck_count || decks > PA_STORE_MAX_DECKS) {
        return false;
    }
    uint32_t payload_len = rd_u32(header + 16);
    uint32_t payload_crc = rd_u32(header + 20);
    uint32_t index_crc = rd_u32(header + 24);
    if (payload_len > payload_room(store)) {
        return false;
    }
    uint8_t names[PA_DECK_BYTES];
    for (uint16_t i = 0; i < decks; ++i) {
        if (!flash_read(store->cards, base + 64u + (uint32_t)i * PA_DECK_BYTES, names, PA_DECK_BYTES) ||
            !pa_text_valid((char *)names, PA_DECK_BYTES)) {
            return false;
        }
        memcpy(store->deck_names[i], names, PA_DECK_BYTES);
    }
    uint32_t index_bytes = (uint32_t)store->count * PA_INDEX_ENTRY;
    uint32_t crc = 0xffffffffu;
    uint32_t done = 0;
    while (done < index_bytes) {
        uint32_t n = index_bytes - done;
        if (n > PA_STORE_SECTOR) {
            n = PA_STORE_SECTOR;
        }
        if (!flash_read(store->cards, base + PA_INDEX_OFF + done, store->sector, n)) {
            return false;
        }
        for (uint32_t i = 0; i < n; ++i) {
            crc ^= store->sector[i];
            for (unsigned bit = 0; bit < 8; ++bit) {
                crc = (crc >> 1) ^ (0xedb88320u & (uint32_t)-(int32_t)(crc & 1u));
            }
        }
        for (uint32_t i = 0; i + PA_INDEX_ENTRY <= n; i += PA_INDEX_ENTRY) {
            uint32_t front_off = rd_u32(store->sector + i + 4);
            uint16_t front_len = rd_u16(store->sector + i + 8);
            uint16_t back_len = rd_u16(store->sector + i + 10);
            uint32_t back_off = rd_u32(store->sector + i + 12);
            uint32_t end = PA_PAYLOAD_OFF + payload_len;
            if (front_len == 0 || back_len == 0 || front_len > PA_STORE_TEXT_MAX || back_len > PA_STORE_TEXT_MAX ||
                front_off < PA_PAYLOAD_OFF || back_off < PA_PAYLOAD_OFF ||
                front_off > end - front_len || back_off > end - back_len) {
                return false;
            }
        }
        done += n;
    }
    crc ^= 0xffffffffu;
    if (crc != index_crc) {
        return false;
    }
    crc = 0xffffffffu;
    done = 0;
    while (done < payload_len) {
        uint32_t n = payload_len - done;
        if (n > PA_STORE_SECTOR) {
            n = PA_STORE_SECTOR;
        }
        if (!flash_read(store->cards, base + PA_PAYLOAD_OFF + done, store->sector, n)) {
            return false;
        }
        for (uint32_t i = 0; i < n; ++i) {
            crc ^= store->sector[i];
            for (unsigned bit = 0; bit < 8; ++bit) {
                crc = (crc >> 1) ^ (0xedb88320u & (uint32_t)-(int32_t)(crc & 1u));
            }
        }
        done += n;
    }
    return (crc ^ 0xffffffffu) == payload_crc;
}

static bool chunk_header_ok(const uint8_t *header, uint32_t epoch, uint16_t count, uint32_t *generation,
                            uint16_t *chunk_index, uint16_t *erase_count)
{
    if (memcmp(header, PA_CHUNK_MAGIC, 4) != 0 || rd_u32(header + 12) != epoch) {
        return false;
    }
    uint16_t index = rd_u16(header + 16);
    uint16_t records = rd_u16(header + 18);
    uint16_t need = chunks_for(count);
    if (index >= need || records == 0 || records > PA_SCHED_PER_CHUNK) {
        return false;
    }
    uint16_t expect = index + 1u == need
                          ? (uint16_t)(count - (uint32_t)index * PA_SCHED_PER_CHUNK)
                          : (uint16_t)PA_SCHED_PER_CHUNK;
    if (records != expect || pa_crc32(header + PA_SCHED_HEADER, (size_t)records * PA_SCHED_RECORD) !=
                                 rd_u32(header + 20)) {
        return false;
    }
    *generation = rd_u32(header + 4);
    *chunk_index = index;
    uint32_t erases = rd_u32(header + 8);
    *erase_count = erases > UINT16_MAX ? UINT16_MAX : (uint16_t)erases;
    return *generation != 0;
}

static bool load_chunks(pa_store *store)
{
    clear_chunk_map(store);
    uint16_t need = chunks_for(store->count);
    uint32_t best_gen[PA_SCHED_CHUNKS];
    memset(best_gen, 0, sizeof(best_gen));
    for (uint16_t phys = 0; phys < store->pool_count; ++phys) {
        if (!flash_read(store->sched, pool_offset(phys), store->sector, PA_SCHED_HEADER)) {
            return false;
        }
        if (memcmp(store->sector, PA_CHUNK_MAGIC, 4) != 0) {
            store->erase_count[phys] = 0;
            continue;
        }
        uint32_t erases = rd_u32(store->sector + 8);
        store->erase_count[phys] = erases > UINT16_MAX ? UINT16_MAX : (uint16_t)erases;
        if (store->count == 0 || rd_u32(store->sector + 12) != store->epoch) {
            continue;
        }
        if (!flash_read(store->sched, pool_offset(phys) + PA_SCHED_HEADER, store->sector + PA_SCHED_HEADER,
                        (uint32_t)PA_SCHED_PER_CHUNK * PA_SCHED_RECORD)) {
            return false;
        }
        uint32_t generation;
        uint16_t index;
        uint16_t erase_count;
        if (!chunk_header_ok(store->sector, store->epoch, store->count, &generation, &index, &erase_count)) {
            continue;
        }
        if (generation >= best_gen[index]) {
            best_gen[index] = generation;
            store->chunk_phys[index] = phys;
            store->erase_count[phys] = erase_count;
        }
    }
    for (uint16_t i = 0; i < need; ++i) {
        if (store->chunk_phys[i] == PA_NONE) {
            return false;
        }
    }
    return true;
}

static bool load_progress(pa_store *store)
{
    store->import_pending = false;
    store->import_active = false;
    store->import_asm = 0;
    uint32_t best_gen = 0;
    bool found = false;
    uint8_t block[36];
    for (uint8_t slot = 0; slot < 2; ++slot) {
        if (!flash_read(store->sched, (2u + slot) * PA_STORE_SECTOR, block, 36) ||
            memcmp(block, PA_PROGRESS_MAGIC, 4) != 0 || pa_crc32(block, 32) != rd_u32(block + 32)) {
            continue;
        }
        uint32_t generation = rd_u32(block + 4);
        uint32_t count = rd_u32(block + 8);
        uint32_t length = rd_u32(block + 16);
        uint32_t received = rd_u32(block + 20);
        if (generation <= best_gen || count < 1 || count > PA_STORE_MAX_CARDS || length == 0 ||
            received > length || block[28] > 1u ||
            (received != length && (received % PA_STORE_SECTOR) != 0)) {
            continue;
        }
        best_gen = generation;
        found = true;
        store->import_count = count;
        store->import_crc = rd_u32(block + 12);
        store->import_length = length;
        store->import_durable = received;
        store->import_slot = block[28];
        store->import_progress_generation = generation;
    }
    store->import_pending = found;
    return true;
}

void pa_store_init(pa_store *store, pa_flash *cards, pa_flash *sched)
{
    memset(store, 0, sizeof(*store));
    store->cards = cards;
    store->sched = sched;
    clear_chunk_map(store);
}

pa_store_status pa_store_mount(pa_store *store)
{
    store->import_active = false;
    store->import_asm = 0;
    store->status = PA_STORE_EMPTY;
    store->count = 0;
    store->now = 0;
    store->epoch = 0;
    store->deck_count = 0;
    store->super_generation = 0;
    store->super_slot = 0;
    store->active_slot = 0;
    clear_chunk_map(store);
    if (store->cards == NULL || store->sched == NULL ||
        (store->cards->size % (PA_STORE_SECTOR * 2u)) != 0 || store->cards->size < PA_STORE_SECTOR * 2u ||
        store->sched->size < (PA_META_SECTORS + PA_SCHED_CHUNKS + 1u) * PA_STORE_SECTOR ||
        (store->sched->size % PA_STORE_SECTOR) != 0) {
        store->status = PA_STORE_CORRUPT;
        return store->status;
    }
    store->pool_count = (uint16_t)(store->sched->size / PA_STORE_SECTOR - PA_META_SECTORS);
    if (store->pool_count > PA_POOL_CAP || payload_room(store) < 1024u) {
        store->status = PA_STORE_CORRUPT;
        return store->status;
    }
    uint32_t gen0 = 0, gen1 = 0;
    bool ok0 = read_super(store, 0, &gen0);
    uint32_t now0 = store->now, epoch0 = store->epoch;
    uint16_t count0 = store->count, decks0 = store->deck_count;
    uint8_t slot0 = store->active_slot;
    bool ok1 = read_super(store, 1, &gen1);
    if (ok0 && (!ok1 || gen0 >= gen1)) {
        store->now = now0;
        store->epoch = epoch0;
        store->count = count0;
        store->deck_count = decks0;
        store->active_slot = slot0;
        store->super_generation = gen0;
        store->super_slot = 0;
        store->status = PA_STORE_READY;
    } else if (ok1) {
        store->super_generation = gen1;
        store->super_slot = 1;
        store->status = PA_STORE_READY;
    } else {
        store->status = PA_STORE_EMPTY;
        store->count = 0;
        store->now = 0;
        store->epoch = 0;
        store->deck_count = 0;
    }
    if (store->status == PA_STORE_READY && (!load_decks(store) || !load_chunks(store))) {
        store->status = PA_STORE_CORRUPT;
    }
    if (!load_progress(store) && store->status != PA_STORE_CORRUPT) {
        store->status = PA_STORE_CORRUPT;
    }
    return store->status;
}

uint16_t pa_store_count(const pa_store *store) { return store->count; }
uint32_t pa_store_now(const pa_store *store) { return store->now; }
uint16_t pa_store_deck_count(const pa_store *store) { return store->deck_count; }

const char *pa_store_deck_name(const pa_store *store, uint16_t index)
{
    return index < store->deck_count ? store->deck_names[index] : NULL;
}

bool pa_store_checkpoint(pa_store *store)
{
    if (store == NULL || store->import_active || store->status == PA_STORE_CORRUPT) {
        return false;
    }
    return write_super(store);
}

bool pa_store_advance(pa_store *store, uint32_t minutes)
{
    if (store == NULL || store->import_active || store->status == PA_STORE_CORRUPT ||
        minutes > UINT32_MAX - store->now) {
        return false;
    }
    store->now += minutes;
    return true;
}

static int deck_index(const pa_store *store, const char *deck)
{
    if (deck == NULL) {
        return -1;
    }
    for (uint16_t i = 0; i < store->deck_count; ++i) {
        if (strcmp(store->deck_names[i], deck) == 0) {
            return (int)i;
        }
    }
    return -1;
}

static bool read_record(pa_store *store, uint16_t index, uint8_t out[PA_SCHED_RECORD])
{
    if (index >= store->count) {
        return false;
    }
    uint16_t chunk = (uint16_t)(index / PA_SCHED_PER_CHUNK);
    uint16_t phys = store->chunk_phys[chunk];
    if (phys == PA_NONE) {
        return false;
    }
    uint32_t offset = pool_offset(phys) + PA_SCHED_HEADER +
                      (uint32_t)(index % PA_SCHED_PER_CHUNK) * PA_SCHED_RECORD;
    return flash_read(store->sched, offset, out, PA_SCHED_RECORD);
}

uint16_t pa_store_next(pa_store *store, const char *deck)
{
    if (store == NULL || store->status != PA_STORE_READY || store->import_active) {
        return PA_NO_CARD;
    }
    int deck_id = deck_index(store, deck);
    if (deck_id < 0) {
        return PA_NO_CARD;
    }
    uint16_t best = PA_NO_CARD;
    unsigned best_priority = 3;
    uint32_t best_due = 0;
    uint32_t best_id = 0;
    uint16_t need = chunks_for(store->count);
    for (uint16_t chunk = 0; chunk < need; ++chunk) {
        if (!flash_read(store->sched, pool_offset(store->chunk_phys[chunk]), store->sector, PA_STORE_SECTOR)) {
            return PA_NO_CARD;
        }
        uint16_t records = rd_u16(store->sector + 18);
        for (uint16_t n = 0; n < records; ++n) {
            const uint8_t *rec = store->sector + PA_SCHED_HEADER + (uint32_t)n * PA_SCHED_RECORD;
            if (rec[14] != (uint8_t)deck_id) {
                continue;
            }
            uint32_t due = rd_u32(rec);
            if (due > store->now) {
                continue;
            }
            unsigned priority = rec[12] == PA_LEARNING ? 0u : rec[12] == PA_REVIEW ? 1u : 2u;
            uint32_t id = rd_u32(rec + 16);
            uint16_t index = (uint16_t)(chunk * PA_SCHED_PER_CHUNK + n);
            if (best == PA_NO_CARD || priority < best_priority ||
                (priority == best_priority && (due < best_due || (due == best_due && id < best_id)))) {
                best = index;
                best_priority = priority;
                best_due = due;
                best_id = id;
            }
        }
    }
    return best;
}

bool pa_store_read(pa_store *store, uint16_t index, char *deck, size_t deck_cap, char *front,
                   size_t front_cap, char *back, size_t back_cap)
{
    if (store == NULL || store->status != PA_STORE_READY || index >= store->count || deck == NULL ||
        front == NULL || back == NULL) {
        return false;
    }
    uint8_t ent[PA_INDEX_ENTRY];
    uint8_t rec[PA_SCHED_RECORD];
    uint32_t base = slot_base(store, store->active_slot);
    if (!flash_read(store->cards, base + PA_INDEX_OFF + (uint32_t)index * PA_INDEX_ENTRY, ent, sizeof(ent)) ||
        !read_record(store, index, rec)) {
        return false;
    }
    uint8_t deck_id = rec[14];
    uint16_t front_len = rd_u16(ent + 8);
    uint16_t back_len = rd_u16(ent + 10);
    if (deck_id >= store->deck_count || front_len >= front_cap || back_len >= back_cap ||
        strlen(store->deck_names[deck_id]) >= deck_cap) {
        return false;
    }
    memcpy(deck, store->deck_names[deck_id], strlen(store->deck_names[deck_id]) + 1u);
    if (!flash_read(store->cards, base + rd_u32(ent + 4), front, front_len) ||
        !flash_read(store->cards, base + rd_u32(ent + 12), back, back_len)) {
        return false;
    }
    front[front_len] = '\0';
    back[back_len] = '\0';
    return pa_text_valid(front, front_cap) && pa_text_valid(back, back_cap);
}

static bool phys_in_use(const pa_store *store, const uint16_t *extra, uint16_t extra_count, uint16_t phys)
{
    uint16_t need = chunks_for(store->count);
    for (uint16_t i = 0; i < need; ++i) {
        if (store->chunk_phys[i] == phys) {
            return true;
        }
    }
    for (uint16_t i = 0; i < extra_count; ++i) {
        if (extra[i] == phys) {
            return true;
        }
    }
    return false;
}

static uint16_t pick_free(const pa_store *store, const uint16_t *extra, uint16_t extra_count)
{
    uint16_t best = PA_NONE;
    uint16_t best_erase = UINT16_MAX;
    for (uint16_t phys = 0; phys < store->pool_count; ++phys) {
        if (phys_in_use(store, extra, extra_count, phys)) {
            continue;
        }
        if (best == PA_NONE || store->erase_count[phys] < best_erase) {
            best = phys;
            best_erase = store->erase_count[phys];
        }
    }
    return best;
}

static void record_from_sched(uint8_t *rec, const pa_sched *sched, uint8_t deck_id)
{
    memset(rec, 0, PA_SCHED_RECORD);
    wr_u32(rec, sched->due);
    wr_u16(rec + 4, sched->interval);
    wr_u16(rec + 6, sched->ease);
    wr_u16(rec + 8, sched->reviews);
    wr_u16(rec + 10, sched->lapses);
    rec[12] = sched->stage;
    rec[13] = sched->step;
    rec[14] = deck_id;
    wr_u32(rec + 16, sched->id);
}

static bool write_chunk_sector(pa_store *store, uint16_t chunk, uint16_t records, uint32_t epoch,
                               uint16_t phys, uint32_t generation)
{
    uint8_t *header = store->sector;
    memcpy(header, PA_CHUNK_MAGIC, 4);
    wr_u32(header + 4, generation);
    uint32_t erases = (uint32_t)store->erase_count[phys] + 1u;
    if (erases > UINT16_MAX) {
        erases = UINT16_MAX;
    }
    wr_u32(header + 8, erases);
    wr_u32(header + 12, epoch);
    wr_u16(header + 16, chunk);
    wr_u16(header + 18, records);
    wr_u32(header + 20, pa_crc32(header + PA_SCHED_HEADER, (size_t)records * PA_SCHED_RECORD));
    wr_u32(header + 24, 0);
    if (!flash_erase_write(store->sched, pool_offset(phys), header, PA_STORE_SECTOR)) {
        return false;
    }
    store->erase_count[phys] = (uint16_t)erases;
    return true;
}

bool pa_store_grade(pa_store *store, uint16_t index, pa_rating rating)
{
    if (store == NULL || store->status != PA_STORE_READY || store->import_active || index >= store->count) {
        return false;
    }
    uint16_t chunk = (uint16_t)(index / PA_SCHED_PER_CHUNK);
    uint16_t phys = store->chunk_phys[chunk];
    if (phys == PA_NONE || !flash_read(store->sched, pool_offset(phys), store->sector, PA_STORE_SECTOR)) {
        return false;
    }
    uint8_t *rec = store->sector + PA_SCHED_HEADER + (uint32_t)(index % PA_SCHED_PER_CHUNK) * PA_SCHED_RECORD;
    pa_sched sched = {
        .id = rd_u32(rec + 16),
        .due = rd_u32(rec),
        .interval = rd_u16(rec + 4),
        .ease = rd_u16(rec + 6),
        .reviews = rd_u16(rec + 8),
        .lapses = rd_u16(rec + 10),
        .stage = rec[12],
        .step = rec[13],
    };
    uint8_t deck_id = rec[14];
    if (!pa_grade_sched(&sched, store->now, rating)) {
        return false;
    }
    record_from_sched(rec, &sched, deck_id);
    uint16_t records = rd_u16(store->sector + 18);
    uint32_t generation = rd_u32(store->sector + 4) + 1u;
    uint16_t dest = pick_free(store, NULL, 0);
    if (dest == PA_NONE || !write_chunk_sector(store, chunk, records, store->epoch, dest, generation)) {
        return false;
    }
    store->chunk_phys[chunk] = dest;
    (void)write_super(store);
    return true;
}

size_t pa_store_pack_card(uint8_t *out, size_t cap, uint32_t id, const char *deck, const char *front,
                          const char *back)
{
    if (out == NULL || id == 0 || !pa_text_valid(deck, PA_DECK_BYTES) ||
        !pa_text_valid(front, PA_STORE_TEXT_MAX + 1u) || !pa_text_valid(back, PA_STORE_TEXT_MAX + 1u)) {
        return 0;
    }
    size_t deck_len = strlen(deck);
    size_t front_len = strlen(front);
    size_t back_len = strlen(back);
    size_t need = 9u + deck_len + front_len + back_len;
    if (need > cap || front_len > PA_STORE_TEXT_MAX || back_len > PA_STORE_TEXT_MAX) {
        return 0;
    }
    wr_u32(out, id);
    out[4] = (uint8_t)deck_len;
    wr_u16(out + 5, (uint16_t)front_len);
    wr_u16(out + 7, (uint16_t)back_len);
    memcpy(out + 9, deck, deck_len);
    memcpy(out + 9 + deck_len, front, front_len);
    memcpy(out + 9 + deck_len + front_len, back, back_len);
    return need;
}

static bool save_progress(pa_store *store)
{
    uint8_t block[36];
    memset(block, 0, sizeof(block));
    memcpy(block, PA_PROGRESS_MAGIC, 4);
    uint32_t generation = store->import_progress_generation + 1u;
    if (generation == 0) {
        generation = 1;
    }
    wr_u32(block + 4, generation);
    wr_u32(block + 8, store->import_count);
    wr_u32(block + 12, store->import_crc);
    wr_u32(block + 16, store->import_length);
    wr_u32(block + 20, store->import_durable);
    wr_u32(block + 24, 0);
    block[28] = store->import_slot;
    wr_u32(block + 32, pa_crc32(block, 32));
    uint8_t slot = (generation & 1u) ? 0u : 1u;
    if (!flash_erase_write(store->sched, (2u + slot) * PA_STORE_SECTOR, block, 36)) {
        return false;
    }
    store->import_progress_generation = generation;
    store->import_pending = true;
    return true;
}

static void forget_progress_ram(pa_store *store)
{
    store->import_active = false;
    store->import_pending = false;
    store->import_asm = 0;
    store->import_durable = 0;
    store->import_count = 0;
    store->import_length = 0;
    store->import_crc = 0;
}

static bool clear_progress(pa_store *store)
{
    uint8_t blank[16];
    memset(blank, 0xff, sizeof(blank));
    bool ok = flash_erase_write(store->sched, 2u * PA_STORE_SECTOR, blank, sizeof(blank)) &&
              flash_erase_write(store->sched, 3u * PA_STORE_SECTOR, blank, sizeof(blank));
    forget_progress_ram(store);
    store->import_progress_generation = 0;
    return ok;
}

static uint8_t staging_slot(const pa_store *store)
{
    if (store->super_generation == 0 || store->count == 0) {
        return 0;
    }
    return (uint8_t)(1u - store->active_slot);
}

bool pa_store_import_begin(pa_store *store, uint32_t count, uint32_t crc, uint32_t length, uint32_t *resume)
{
    if (store == NULL || resume == NULL || count < 1 ||
        count > PA_STORE_MAX_CARDS || length < count * 11u || length > payload_room(store) ||
        length > PA_STORE_PAYLOAD_MAX) {
        return false;
    }
    if (store->import_pending && !store->import_active && store->import_count == count &&
        store->import_crc == crc && store->import_length == length &&
        store->import_slot == staging_slot(store)) {
        store->import_asm = 0;
        store->import_active = true;
        *resume = store->import_durable;
        return true;
    }
    if (!clear_progress(store)) {
        return false;
    }
    store->import_count = count;
    store->import_crc = crc;
    store->import_length = length;
    store->import_durable = 0;
    store->import_asm = 0;
    store->import_slot = staging_slot(store);
    store->import_active = true;
    if (!save_progress(store)) {
        forget_progress_ram(store);
        return false;
    }
    *resume = 0;
    return true;
}

static bool flush_assembly(pa_store *store)
{
    if (store->import_asm == 0) {
        return true;
    }
    uint32_t offset = slot_base(store, store->import_slot) + PA_PAYLOAD_OFF + store->import_durable;
    if ((offset % PA_STORE_SECTOR) != 0 ||
        !flash_erase_write(store->cards, offset, store->sector, store->import_asm)) {
        return false;
    }
    uint16_t written = store->import_asm;
    store->import_durable += written;
    store->import_asm = 0;
    if (!save_progress(store)) {
        store->import_durable -= written;
        return false;
    }
    return true;
}

bool pa_store_import_write(pa_store *store, uint32_t offset, const uint8_t *data, uint32_t length,
                           uint32_t *accepted)
{
    if (store == NULL || data == NULL || accepted == NULL || !store->import_active || length == 0 ||
        offset != store->import_durable + store->import_asm ||
        length > store->import_length - offset) {
        return false;
    }
    uint32_t cursor = 0;
    while (cursor < length) {
        uint32_t room = PA_STORE_SECTOR - store->import_asm;
        uint32_t n = length - cursor;
        if (n > room) {
            n = room;
        }
        memcpy(store->sector + store->import_asm, data + cursor, n);
        store->import_asm = (uint16_t)(store->import_asm + n);
        cursor += n;
        if (store->import_asm == PA_STORE_SECTOR ||
            store->import_durable + store->import_asm == store->import_length) {
            if (!flush_assembly(store)) {
                store->import_asm = 0;
                return false;
            }
        }
    }
    *accepted = store->import_durable + store->import_asm;
    return true;
}

bool pa_store_import_resume(const pa_store *store, uint32_t *offset, uint32_t *count, uint32_t *crc,
                            uint32_t *length)
{
    if (store == NULL || offset == NULL || count == NULL || crc == NULL || length == NULL ||
        (!store->import_active && !store->import_pending)) {
        return false;
    }
    *offset = store->import_durable + store->import_asm;
    *count = store->import_count;
    *crc = store->import_crc;
    *length = store->import_length;
    return true;
}

void pa_store_import_abort(pa_store *store)
{
    if (store != NULL) {
        (void)clear_progress(store);
    }
}

static bool read_payload(pa_store *store, uint32_t offset, uint8_t *dst, uint32_t length, uint32_t *crc)
{
    if (offset + length > store->import_length ||
        !flash_read(store->cards, slot_base(store, store->import_slot) + PA_PAYLOAD_OFF + offset, dst, length)) {
        return false;
    }
    for (uint32_t i = 0; i < length; ++i) {
        *crc ^= dst[i];
        for (unsigned bit = 0; bit < 8; ++bit) {
            *crc = (*crc >> 1) ^ (0xedb88320u & (uint32_t)-(int32_t)(*crc & 1u));
        }
    }
    return true;
}

static bool lookup_deck(char names[PA_STORE_MAX_DECKS][PA_DECK_BYTES], uint16_t deck_count,
                        const char *deck, uint8_t *id)
{
    for (uint16_t i = 0; i < deck_count; ++i) {
        if (strcmp(names[i], deck) == 0) {
            *id = (uint8_t)i;
            return true;
        }
    }
    return false;
}

static bool find_or_add_deck(char names[PA_STORE_MAX_DECKS][PA_DECK_BYTES], uint16_t *deck_count,
                             const char *deck, uint8_t *id)
{
    for (uint16_t i = 0; i < *deck_count; ++i) {
        if (strcmp(names[i], deck) == 0) {
            *id = (uint8_t)i;
            return true;
        }
    }
    if (*deck_count >= PA_STORE_MAX_DECKS) {
        return false;
    }
    memcpy(names[*deck_count], deck, strlen(deck) + 1u);
    *id = (uint8_t)(*deck_count);
    ++*deck_count;
    return true;
}

static const pa_card *adopted_card(uint32_t id)
{
    if (adopt_cards == NULL) {
        return NULL;
    }
    for (uint16_t i = 0; i < adopt_cards->count; ++i) {
        if (adopt_cards->cards[i].id == id) {
            return &adopt_cards->cards[i];
        }
    }
    return NULL;
}

static int cmp_u32(const void *left, const void *right)
{
    uint32_t a = *(const uint32_t *)left;
    uint32_t b = *(const uint32_t *)right;
    return (a > b) - (a < b);
}

static void crc_feed(uint32_t *crc, const uint8_t *data, size_t length)
{
    for (size_t i = 0; i < length; ++i) {
        *crc ^= data[i];
        for (unsigned bit = 0; bit < 8; ++bit) {
            *crc = (*crc >> 1) ^ (0xedb88320u & (uint32_t)-(int32_t)(*crc & 1u));
        }
    }
}

static bool flush_index(pa_store *store, uint32_t index_bytes)
{
    uint32_t sector_off = index_bytes & ~(PA_STORE_SECTOR - 1u);
    return flash_erase_write(store->cards, slot_base(store, store->import_slot) + PA_INDEX_OFF + sector_off,
                             store->sector, PA_STORE_SECTOR);
}

bool pa_store_import_commit(pa_store *store, pa_store_text_fn text_ok, void *user)
{
    if (store == NULL || !store->import_active ||
        store->import_durable + store->import_asm != store->import_length || !flush_assembly(store)) {
        return false;
    }
    uint32_t count32 = store->import_count;
    uint32_t *ids = pa_heap_alloc(count32 * sizeof(uint32_t));
    char (*names)[PA_DECK_BYTES] = pa_heap_alloc((size_t)PA_STORE_MAX_DECKS * PA_DECK_BYTES);
    if (ids == NULL || names == NULL) {
        pa_heap_free(ids);
        pa_heap_free(names);
        return false;
    }
    memset(names, 0, (size_t)PA_STORE_MAX_DECKS * PA_DECK_BYTES);
    uint16_t deck_count = 0;
    uint32_t crc = 0xffffffffu;
    uint32_t index_crc = 0xffffffffu;
    uint32_t cursor = 0;
    uint32_t index_bytes = 0;
    bool ok = true;
    memset(store->sector, 0xff, PA_STORE_SECTOR);
    for (uint32_t i = 0; ok && i < count32; ++i) {
        uint8_t head[9];
        if (!read_payload(store, cursor, head, sizeof(head), &crc)) {
            ok = false;
            break;
        }
        cursor += sizeof(head);
        uint32_t id = rd_u32(head);
        uint8_t deck_len = head[4];
        uint16_t front_len = rd_u16(head + 5);
        uint16_t back_len = rd_u16(head + 7);
        if (id == 0 || deck_len == 0 || deck_len >= PA_DECK_BYTES || front_len == 0 ||
            front_len > PA_STORE_TEXT_MAX || back_len == 0 || back_len > PA_STORE_TEXT_MAX ||
            cursor + deck_len + front_len + back_len > store->import_length) {
            ok = false;
            break;
        }
        char deck[PA_DECK_BYTES];
        char front[PA_STORE_TEXT_MAX + 1u];
        char back[PA_STORE_TEXT_MAX + 1u];
        memset(deck, 0, sizeof(deck));
        if (!read_payload(store, cursor, (uint8_t *)deck, deck_len, &crc) ||
            !pa_text_valid(deck, sizeof(deck))) {
            ok = false;
            break;
        }
        cursor += deck_len;
        if (!read_payload(store, cursor, (uint8_t *)front, front_len, &crc)) {
            ok = false;
            break;
        }
        front[front_len] = '\0';
        cursor += front_len;
        if (!read_payload(store, cursor, (uint8_t *)back, back_len, &crc)) {
            ok = false;
            break;
        }
        back[back_len] = '\0';
        cursor += back_len;
        if (!pa_text_valid(front, sizeof(front)) || !pa_text_valid(back, sizeof(back)) ||
            (text_ok != NULL && !text_ok(deck, front, back, user)) ||
            (adopt_cards != NULL && adopted_card(id) == NULL)) {
            ok = false;
            break;
        }
        uint8_t deck_id;
        if (!find_or_add_deck(names, &deck_count, deck, &deck_id)) {
            ok = false;
            break;
        }
        ids[i] = id;
        uint32_t front_off = PA_PAYLOAD_OFF + (cursor - back_len - front_len);
        uint32_t back_off = PA_PAYLOAD_OFF + (cursor - back_len);
        uint8_t ent[PA_INDEX_ENTRY];
        wr_u32(ent, id);
        wr_u32(ent + 4, front_off);
        wr_u16(ent + 8, front_len);
        wr_u16(ent + 10, back_len);
        wr_u32(ent + 12, back_off);
        crc_feed(&index_crc, ent, sizeof(ent));
        memcpy(store->sector + (index_bytes % PA_STORE_SECTOR), ent, sizeof(ent));
        index_bytes += PA_INDEX_ENTRY;
        if ((index_bytes % PA_STORE_SECTOR) == 0 || i + 1u == count32) {
            if (!flush_index(store, index_bytes - 1u)) {
                ok = false;
                break;
            }
            memset(store->sector, 0xff, PA_STORE_SECTOR);
        }
        (void)deck_id;
    }
    if (ok && (cursor != store->import_length || (crc ^ 0xffffffffu) != store->import_crc)) {
        ok = false;
    }
    if (ok) {
        qsort(ids, count32, sizeof(uint32_t), cmp_u32);
        for (uint32_t i = 1; i < count32; ++i) {
            if (ids[i] == ids[i - 1u]) {
                ok = false;
                break;
            }
        }
    }
    pa_heap_free(ids);
    if (!ok) {
        pa_heap_free(names);
        return false;
    }

    uint32_t new_epoch = store->epoch + 1u;
    if (new_epoch == 0) {
        new_epoch = 1;
    }
    uint16_t new_phys[PA_SCHED_CHUNKS];
    for (uint16_t i = 0; i < PA_SCHED_CHUNKS; ++i) {
        new_phys[i] = PA_NONE;
    }
    uint16_t need = chunks_for((uint16_t)count32);
    cursor = 0;
    uint32_t ignored_crc = 0xffffffffu;
    for (uint16_t chunk = 0; ok && chunk < need; ++chunk) {
        uint16_t records = chunk + 1u == need
                               ? (uint16_t)(count32 - (uint32_t)chunk * PA_SCHED_PER_CHUNK)
                               : (uint16_t)PA_SCHED_PER_CHUNK;
        memset(store->sector, 0xff, PA_STORE_SECTOR);
        for (uint16_t n = 0; n < records; ++n) {
            uint8_t head[9];
            if (!read_payload(store, cursor, head, sizeof(head), &ignored_crc)) {
                ok = false;
                break;
            }
            cursor += sizeof(head);
            uint32_t id = rd_u32(head);
            uint8_t deck_len = head[4];
            uint16_t front_len = rd_u16(head + 5);
            uint16_t back_len = rd_u16(head + 7);
            char deck[PA_DECK_BYTES];
            memset(deck, 0, sizeof(deck));
            if (!read_payload(store, cursor, (uint8_t *)deck, deck_len, &ignored_crc)) {
                ok = false;
                break;
            }
            cursor += deck_len;
            uint8_t skip[PA_STORE_TEXT_MAX];
            if (!read_payload(store, cursor, skip, front_len, &ignored_crc)) {
                ok = false;
                break;
            }
            cursor += front_len;
            if (!read_payload(store, cursor, skip, back_len, &ignored_crc)) {
                ok = false;
                break;
            }
            cursor += back_len;
            uint8_t deck_id = 0;
            if (!lookup_deck(names, deck_count, deck, &deck_id)) {
                ok = false;
                break;
            }
            pa_sched sched;
            const pa_card *src = adopted_card(id);
            if (src != NULL) {
                sched = (pa_sched){.id = src->id,
                                   .due = src->due,
                                   .interval = src->interval,
                                   .ease = src->ease,
                                   .reviews = src->reviews,
                                   .lapses = src->lapses,
                                   .stage = src->stage,
                                   .step = src->step};
            } else {
                sched = (pa_sched){.id = id,
                                   .due = store->now,
                                   .interval = 0,
                                   .ease = 2500,
                                   .reviews = 0,
                                   .lapses = 0,
                                   .stage = PA_NEW,
                                   .step = 0};
            }
            if (!pa_sched_valid(&sched)) {
                ok = false;
                break;
            }
            record_from_sched(store->sector + PA_SCHED_HEADER + (uint32_t)n * PA_SCHED_RECORD, &sched, deck_id);
        }
        if (!ok) {
            break;
        }
        uint16_t dest = pick_free(store, new_phys, chunk);
        uint32_t generation = 1;
        if (dest == PA_NONE || !write_chunk_sector(store, chunk, records, new_epoch, dest, generation)) {
            ok = false;
            break;
        }
        new_phys[chunk] = dest;
    }
    if (!ok) {
        pa_heap_free(names);
        return false;
    }

    memset(store->sector, 0xff, PA_STORE_SECTOR);
    memcpy(store->sector, PA_SLOT_MAGIC, 4);
    wr_u16(store->sector + 4, 1);
    wr_u16(store->sector + 6, (uint16_t)count32);
    wr_u32(store->sector + 8, new_epoch);
    wr_u32(store->sector + 12, PA_PAYLOAD_OFF);
    wr_u32(store->sector + 16, store->import_length);
    wr_u32(store->sector + 20, store->import_crc);
    wr_u32(store->sector + 24, index_crc ^ 0xffffffffu);
    wr_u16(store->sector + 28, deck_count);
    wr_u16(store->sector + 30, 0);
    wr_u32(store->sector + 32, pa_crc32(store->sector, 32));
    for (uint16_t i = 0; i < deck_count; ++i) {
        memcpy(store->sector + 64u + (uint32_t)i * PA_DECK_BYTES, names[i], PA_DECK_BYTES);
    }
    if (!flash_erase_write(store->cards, slot_base(store, store->import_slot), store->sector, PA_STORE_SECTOR)) {
        pa_heap_free(names);
        return false;
    }

    uint16_t old_count = store->count;
    uint32_t old_epoch = store->epoch;
    uint8_t old_slot = store->active_slot;
    uint16_t old_decks = store->deck_count;
    store->count = (uint16_t)count32;
    store->epoch = new_epoch;
    store->active_slot = store->import_slot;
    store->deck_count = deck_count;
    memcpy(store->deck_names, names, sizeof(store->deck_names));
    for (uint16_t i = 0; i < PA_SCHED_CHUNKS; ++i) {
        store->chunk_phys[i] = i < need ? new_phys[i] : PA_NONE;
    }
    pa_heap_free(names);
    if (!write_super(store)) {
        store->count = old_count;
        store->epoch = old_epoch;
        store->active_slot = old_slot;
        store->deck_count = old_decks;
        (void)pa_store_mount(store);
        return false;
    }
    store->status = PA_STORE_READY;
    (void)clear_progress(store);
    return true;
}

bool pa_store_import_collection(pa_store *store, const pa_collection *collection)
{
    if (store == NULL || collection == NULL || collection->count == 0 || collection->count > PA_MAX_CARDS ||
        store->status == PA_STORE_CORRUPT) {
        return false;
    }
    size_t bytes = 0;
    for (uint16_t i = 0; i < collection->count; ++i) {
        bytes += 9u + strlen(collection->cards[i].deck) + strlen(collection->cards[i].front) +
                 strlen(collection->cards[i].back);
    }
    uint8_t *payload = pa_heap_alloc(bytes);
    if (payload == NULL) {
        return false;
    }
    size_t used = 0;
    for (uint16_t i = 0; i < collection->count; ++i) {
        const pa_card *card = &collection->cards[i];
        size_t packed = pa_store_pack_card(payload + used, bytes - used, card->id, card->deck, card->front, card->back);
        if (packed == 0) {
            pa_heap_free(payload);
            return false;
        }
        used += packed;
    }
    uint32_t crc = pa_crc32(payload, used);
    store->now = collection->now;
    adopt_cards = collection;
    pa_store_import_abort(store);
    uint32_t resume = 0;
    bool started = pa_store_import_begin(store, collection->count, crc, (uint32_t)used, &resume);
    uint32_t accepted = 0;
    bool written = started && pa_store_import_write(store, 0, payload, (uint32_t)used, &accepted);
    pa_heap_free(payload);
    bool committed = written && accepted == used && pa_store_import_commit(store, NULL, NULL);
    adopt_cards = NULL;
    return committed;
}

bool pa_store_migrate_pak1(pa_store *store, const uint8_t *data, size_t size)
{
    if (store == NULL || store->status != PA_STORE_EMPTY) {
        return false;
    }
    pa_collection *collection = pa_heap_alloc(sizeof(*collection));
    if (collection == NULL || !pa_decode(collection, data, size)) {
        pa_heap_free(collection);
        return false;
    }
    bool ok = collection->count == 0 || pa_store_import_collection(store, collection);
    if (ok && collection->count == 0) {
        store->now = collection->now;
        ok = write_super(store);
    }
    pa_heap_free(collection);
    return ok;
}
