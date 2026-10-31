#include "pa_store.h"
#include "pa_tail.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Padded image from the firmware layout check: length % 65536 == 4096. */
static const uint32_t PADDED_IMAGE = 2232320u;

static uint32_t align_up(uint32_t value, uint32_t boundary)
{
    return (value + boundary - 1u) & ~(boundary - 1u);
}

static void test_span_refuses_image_and_pad_past_end(void);
static void test_factory_tail_begin_and_abort(void);

int main(void)
{
    uint32_t start = 1;
    uint32_t end = 1;
    uint32_t bank_a = 0;
    uint32_t bank_b = 0;
    uint32_t slot;

    assert(PADDED_IMAGE % PA_TAIL_SLOT_ALIGN == 4096u);
    slot = align_up(PADDED_IMAGE, PA_TAIL_SLOT_ALIGN) + PA_TAIL_SIDECAR;

    assert(pa_tail_data_end(slot, PADDED_IMAGE, false, false, &start, &end));
    assert(start == PADDED_IMAGE);
    assert(end == slot - PA_TAIL_SIDECAR);
    assert(end - start == 61440u);

    assert(pa_tail_data_end(0x7F0000u, PADDED_IMAGE, true, false, &start, &end));
    assert(start == PADDED_IMAGE);
    assert(end == 0x7F0000u);

    /* Same large partition, neither a slot nor factory nor a sidecar. */
    start = 1;
    end = 1;
    assert(!pa_tail_data_end(0x7F0000u, PADDED_IMAGE, false, false, &start, &end));
    assert(start == 1 && end == 1);
    assert(!pa_tail_data_end(0x200000u, 0x100000u, false, false, &start, &end));

    assert(pa_tail_sidecar_banks(0x180000u, 0x100000u, &bank_a, &bank_b));
    assert(bank_a == 0x170000u + PA_TAIL_DPS1_A);
    assert(bank_b == 0x170000u + PA_TAIL_DPS1_B);
    assert(pa_tail_data_end(0x180000u, 0x100000u, false, true, &start, &end));
    assert(end == 0x170000u);
    assert(!pa_tail_data_end(0x180000u, 0x100000u, false, false, &start, &end));

    assert(!pa_tail_sidecar_banks(0x180000u, 0x170001u, &bank_a, &bank_b));
    assert(!pa_tail_data_end(0x180000u, 0x170001u, false, true, &start, &end));

    /* A slot still reserves the sidecar when that partition is factory. */
    assert(pa_tail_data_end(slot, PADDED_IMAGE, true, true, &start, &end));
    assert(end == slot - PA_TAIL_SIDECAR);

    assert(!pa_tail_data_end(slot, 0, true, true, &start, &end));
    assert(!pa_tail_data_end(PADDED_IMAGE, PADDED_IMAGE, true, false, &start, &end));
    test_span_refuses_image_and_pad_past_end();
    test_factory_tail_begin_and_abort();
    return 0;
}

/* Stand-in for ESP-IDF SPI_FLASH_DANGEROUS_WRITE_ABORTS. The running app
 * partition is the whole factory image, including the post-image tail.
 * Erase or write there while protection is on counts as abort() and does
 * not modify flash. */
typedef struct {
    uint8_t *flash;
    uint32_t part_size;
    bool protect;
    bool fail_unprotect;
    int aborts;
    int erases;
    int writes;
    uint32_t max_erase;
    uint32_t bytes_erased;
} idf_model;

typedef struct {
    idf_model *model;
    const pa_tail_bounds *bounds;
    uint32_t base;
    uint32_t size;
} tail_win;

static bool overlaps_running(const idf_model *model, uint32_t offset, uint32_t length)
{
    return length != 0 && offset < model->part_size;
}

static bool idf_mutate(idf_model *model, uint32_t offset, uint32_t length, const void *src, bool is_erase)
{
    if (model->protect && overlaps_running(model, offset, length)) {
        ++model->aborts;
        return false;
    }
    if (length == 0 || offset > model->part_size || length > model->part_size - offset) {
        return false;
    }
    if (is_erase) {
        if ((offset % PA_TAIL_SECTOR) != 0 || (length % PA_TAIL_SECTOR) != 0) {
            return false;
        }
        memset(model->flash + offset, 0xff, length);
        ++model->erases;
        if (length > model->max_erase) {
            model->max_erase = length;
        }
        model->bytes_erased += length;
        return true;
    }
    if (src == NULL) {
        return false;
    }
    memcpy(model->flash + offset, src, length);
    ++model->writes;
    return true;
}

static bool set_protect(void *ctx, bool protect)
{
    idf_model *model = ctx;
    if (model->fail_unprotect && !protect) {
        return false;
    }
    model->protect = protect;
    return true;
}

static bool io_erase(void *ctx, uint32_t offset, uint32_t length, const void *src)
{
    (void)src;
    return idf_mutate(ctx, offset, length, NULL, true);
}

static bool io_write(void *ctx, uint32_t offset, uint32_t length, const void *src)
{
    return idf_mutate(ctx, offset, length, src, false);
}

static bool win_write(void *ctx, uint32_t offset, const void *src, uint32_t length)
{
    tail_win *win = ctx;
    uint32_t span = pa_tail_programmed_length(length);
    uint32_t abs;
    if (src == NULL || span == 0 || offset > win->size || span > win->size - offset ||
        offset > UINT32_MAX - win->base) {
        return false;
    }
    abs = win->base + offset;
    return pa_tail_protected_io(win->bounds, set_protect, win->model, abs, length, src, true, io_write,
                                win->model);
}

static bool win_erase(void *ctx, uint32_t offset, uint32_t length)
{
    tail_win *win = ctx;
    uint32_t abs;
    if (length == 0 || offset > win->size || length > win->size - offset || offset > UINT32_MAX - win->base) {
        return false;
    }
    abs = win->base + offset;
    return pa_tail_protected_io(win->bounds, set_protect, win->model, abs, length, NULL, false, io_erase,
                                win->model);
}

/* Reads must not go through the dangerous-write path. idf_mutate treats a
 * non-erase with a NULL source as a failed write, so copy directly. */
static bool win_read_bytes(void *ctx, uint32_t offset, void *dst, uint32_t length)
{
    tail_win *win = ctx;
    uint32_t abs;
    if (dst == NULL || length == 0 || offset > win->size || length > win->size - offset ||
        offset > UINT32_MAX - win->base) {
        return false;
    }
    abs = win->base + offset;
    if (abs > win->model->part_size || length > win->model->part_size - abs) {
        return false;
    }
    memcpy(dst, win->model->flash + abs, length);
    return true;
}

static void test_span_refuses_image_and_pad_past_end(void)
{
    uint8_t raw[8192];
    idf_model model;
    pa_tail_bounds factory;
    pa_tail_bounds partial;
    pa_tail_bounds bad_start;
    uint8_t byte = 0x5a;
    int aborts;

    memset(raw, 0xa5, sizeof(raw));
    memset(&model, 0, sizeof(model));
    model.flash = raw;
    model.part_size = sizeof(raw);
    model.protect = true;

    factory = (pa_tail_bounds){
        .part_size = 0x7F0000u, .image_len = PADDED_IMAGE, .data_start = PADDED_IMAGE, .data_end = 0x7F0000u};
    assert(pa_tail_span_allowed(&factory, PADDED_IMAGE, PA_TAIL_SECTOR));
    assert(!pa_tail_span_allowed(&factory, PADDED_IMAGE - PA_TAIL_SECTOR, PA_TAIL_SECTOR));
    assert(!pa_tail_span_allowed(&factory, 0, PA_TAIL_SECTOR));
    assert(!pa_tail_span_allowed(&factory, 0x7F0000u - PA_TAIL_SECTOR, PA_TAIL_SECTOR * 2u));
    assert(pa_tail_programmed_length(0) == 0);
    assert(pa_tail_programmed_length(16) == 16);
    assert(pa_tail_programmed_length(36) == 48);
    assert(pa_tail_programmed_length(UINT32_MAX) == 0);

    partial = (pa_tail_bounds){.part_size = 8192, .image_len = 100, .data_start = 4096, .data_end = 8192};
    bad_start = (pa_tail_bounds){.part_size = 8192, .image_len = 100, .data_start = 100, .data_end = 8192};
    assert(pa_tail_span_allowed(&partial, 4096, PA_TAIL_SECTOR));
    assert(!pa_tail_span_allowed(&bad_start, 4096, PA_TAIL_SECTOR));

    assert(!io_erase(&model, 4096, PA_TAIL_SECTOR, NULL));
    assert(model.aborts == 1);
    assert(model.erases == 0);
    assert(raw[4096] == 0xa5);

    aborts = model.aborts;
    assert(pa_tail_protected_io(&partial, set_protect, &model, 4096, PA_TAIL_SECTOR, NULL, false, io_erase,
                                &model));
    assert(model.aborts == aborts);
    assert(model.erases == 1);
    assert(model.max_erase == PA_TAIL_SECTOR);
    assert(model.protect);
    assert(raw[4095] == 0xa5);
    assert(raw[4096] == 0xff);

    aborts = model.aborts;
    assert(!pa_tail_protected_io(&partial, set_protect, &model, 0, PA_TAIL_SECTOR, NULL, false, io_erase, &model));
    assert(model.aborts == aborts);
    assert(model.erases == 1);
    assert(raw[0] == 0xa5);

    aborts = model.aborts;
    assert(!pa_tail_protected_io(&partial, set_protect, &model, 8192 - 36, 36, &byte, true, io_write, &model));
    assert(model.aborts == aborts);
    assert(model.writes == 0);

    model.fail_unprotect = true;
    assert(!pa_tail_protected_io(&partial, set_protect, &model, 4096, PA_TAIL_SECTOR, NULL, false, io_erase,
                                 &model));
    assert(model.aborts == aborts);
    assert(model.erases == 1);
    assert(model.protect);
}

static void test_factory_tail_begin_and_abort(void)
{
    uint32_t cards_bytes = 0;
    uint32_t sched_bytes = 0;
    uint32_t image_len = PA_TAIL_SECTOR;
    uint32_t data_start;
    uint32_t data_end;
    pa_tail_bounds bounds;
    idf_model model;
    tail_win cards_win;
    tail_win sched_win;
    pa_flash cards;
    pa_flash sched;
    pa_store store;
    uint8_t payload[64];
    size_t packed;
    uint32_t resume = 0;
    uint32_t accepted = 0;
    char deck[PA_DECK_BYTES];
    char front[16];
    char back[16];

    assert(pa_region_plan(61440u, &cards_bytes, &sched_bytes));
    data_start = image_len;
    data_end = data_start + cards_bytes + sched_bytes;
    bounds = (pa_tail_bounds){
        .part_size = data_end, .image_len = image_len, .data_start = data_start, .data_end = data_end};
    memset(&model, 0, sizeof(model));
    model.flash = malloc(data_end);
    assert(model.flash != NULL);
    memset(model.flash, 0xff, data_end);
    memset(model.flash, 0xa5, image_len);
    model.part_size = data_end;
    model.protect = true;
    cards_win = (tail_win){.model = &model, .bounds = &bounds, .base = data_start, .size = cards_bytes};
    sched_win = (tail_win){
        .model = &model, .bounds = &bounds, .base = data_start + cards_bytes, .size = sched_bytes};
    cards = (pa_flash){
        .ctx = &cards_win, .size = cards_bytes, .read = win_read_bytes, .write = win_write, .erase = win_erase};
    sched = (pa_flash){
        .ctx = &sched_win, .size = sched_bytes, .read = win_read_bytes, .write = win_write, .erase = win_erase};
    pa_store_init(&store, &cards, &sched);
    assert(pa_store_mount(&store) == PA_STORE_EMPTY);

    pa_store_import_abort(&store);
    assert(model.aborts == 0);
    assert(model.erases == 0);
    assert(model.flash[image_len - 1u] == 0xa5);

    packed = pa_store_pack_card(payload, sizeof(payload), 3, "D", "Q", "A");
    assert(packed >= 11u);
    assert(pa_store_import_begin(&store, 1, pa_crc32(payload, packed), (uint32_t)packed, &resume));
    assert(resume == 0);
    assert(model.aborts == 0);
    assert(model.erases == 3);
    assert(model.max_erase == PA_TAIL_SECTOR);
    assert(model.bytes_erased == 3u * PA_TAIL_SECTOR);
    assert(pa_store_import_write(&store, 0, payload, (uint32_t)packed, &accepted));
    assert(accepted == packed);
    assert(pa_store_import_commit(&store, NULL, NULL));
    assert(model.aborts == 0);
    assert(model.max_erase == PA_TAIL_SECTOR);
    assert(pa_store_count(&store) == 1);
    assert(pa_store_read(&store, 0, deck, sizeof(deck), front, sizeof(front), back, sizeof(back)));
    assert(strcmp(front, "Q") == 0 && strcmp(back, "A") == 0);
    assert(model.flash[0] == 0xa5 && model.flash[image_len - 1u] == 0xa5);

    assert(pa_store_import_begin(&store, 1, pa_crc32(payload, packed), (uint32_t)packed, &resume));
    pa_store_import_abort(&store);
    assert(model.aborts == 0);
    assert(model.max_erase == PA_TAIL_SECTOR);
    assert(!store.import_active && !store.import_pending);
    assert(pa_store_count(&store) == 1);
    {
        pa_store remounted;
        pa_store_init(&remounted, &cards, &sched);
        assert(pa_store_mount(&remounted) == PA_STORE_READY);
        assert(pa_store_count(&remounted) == 1);
        assert(!pa_store_import_resume(&remounted, &resume, &accepted, &accepted, &accepted));
    }
    free(model.flash);
    puts("tail: factory begin/abort does not abort PASS");
}
