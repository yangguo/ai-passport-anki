#include "pa_store.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    uint8_t *data;
    uint32_t size;
    int fail_write_number;
    int writes;
} ram_flash;

static bool ram_read(void *ctx, uint32_t offset, void *dst, uint32_t length)
{
    ram_flash *flash = ctx;
    if (offset > flash->size || length > flash->size - offset) {
        return false;
    }
    memcpy(dst, flash->data + offset, length);
    return true;
}

static bool ram_write(void *ctx, uint32_t offset, const void *src, uint32_t length)
{
    ram_flash *flash = ctx;
    ++flash->writes;
    if (flash->fail_write_number > 0 && flash->writes == flash->fail_write_number) {
        return false;
    }
    if (offset > flash->size || length > flash->size - offset) {
        return false;
    }
    memcpy(flash->data + offset, src, length);
    return true;
}

static bool ram_erase(void *ctx, uint32_t offset, uint32_t length)
{
    ram_flash *flash = ctx;
    if ((offset % PA_STORE_SECTOR) != 0 || (length % PA_STORE_SECTOR) != 0 ||
        offset > flash->size || length > flash->size - offset) {
        return false;
    }
    memset(flash->data + offset, 0xff, length);
    return true;
}

static void bind(pa_flash *flash, ram_flash *ram)
{
    *flash = (pa_flash){.ctx = ram, .size = ram->size, .read = ram_read, .write = ram_write, .erase = ram_erase};
}

static void make_flash(ram_flash *ram, pa_flash *flash, uint32_t size)
{
    ram->data = malloc(size);
    assert(ram->data != NULL);
    memset(ram->data, 0xff, size);
    ram->size = size;
    ram->fail_write_number = 0;
    ram->writes = 0;
    bind(flash, ram);
}

static void free_flash(ram_flash *ram)
{
    free(ram->data);
    ram->data = NULL;
}

static size_t append_card(uint8_t *payload, size_t used, uint32_t id, const char *deck, const char *front,
                          const char *back)
{
    size_t packed = pa_store_pack_card(payload + used, 64, id, deck, front, back);
    assert(packed != 0);
    return used + packed;
}

static bool import_bytes(pa_store *store, const uint8_t *payload, uint32_t count, uint32_t length)
{
    uint32_t resume = 0;
    if (!pa_store_import_begin(store, count, pa_crc32(payload, length), length, &resume) || resume != 0) {
        return false;
    }
    uint32_t offset = 0;
    while (offset < length) {
        uint32_t n = length - offset;
        if (n > 480) {
            n = 480;
        }
        uint32_t accepted = 0;
        if (!pa_store_import_write(store, offset, payload + offset, n, &accepted) || accepted != offset + n) {
            return false;
        }
        offset += n;
    }
    return pa_store_import_commit(store, NULL, NULL);
}

static void test_pack_vector(void)
{
    uint8_t out[32];
    size_t n = pa_store_pack_card(out, sizeof(out), 1, "D", "Q", "A");
    const uint8_t expect[] = {0x01, 0x00, 0x00, 0x00, 0x01, 0x01, 0x00, 0x01, 0x00, 0x44, 0x51, 0x41};
    assert(n == sizeof(expect));
    assert(memcmp(out, expect, sizeof(expect)) == 0);
    assert(sizeof(pa_store) <= PA_STORE_RAM_BUDGET);
}

static void test_ten_thousand_and_heap(void)
{
    ram_flash cards_ram, sched_ram;
    pa_flash cards, sched;
    make_flash(&cards_ram, &cards, PA_CARDS_PARTITION_SIZE);
    make_flash(&sched_ram, &sched, PA_SCHED_PARTITION_SIZE);
    pa_store store;
    pa_store_init(&store, &cards, &sched);
    assert(pa_store_mount(&store) == PA_STORE_EMPTY);

    const uint32_t count = 10000;
    uint8_t *payload = malloc((size_t)count * 16u);
    assert(payload != NULL);
    size_t used = 0;
    for (uint32_t i = 1; i <= count; ++i) {
        char deck[4];
        deck[0] = 'D';
        deck[1] = (char)('0' + (i % 10));
        deck[2] = '\0';
        used = append_card(payload, used, i, deck, "Q", "A");
    }
    pa_store_heap_reset();
    assert(import_bytes(&store, payload, count, (uint32_t)used));
    free(payload);
    assert(pa_store_heap_in_use() == 0);
    assert(pa_store_heap_hwm() > 0);
    assert(pa_store_heap_hwm() <= PA_STORE_COMMIT_HEAP_MAX);
    assert(pa_store_count(&store) == count);
    assert(pa_store_deck_count(&store) == 10);

    pa_store_heap_reset();
    assert(pa_store_next(&store, "D1") == 0);
    char deck[PA_DECK_BYTES], front[PA_STORE_TEXT_MAX + 1], back[PA_STORE_TEXT_MAX + 1];
    assert(pa_store_read(&store, 0, deck, sizeof(deck), front, sizeof(front), back, sizeof(back)));
    assert(strcmp(deck, "D1") == 0);
    assert(strcmp(front, "Q") == 0 && strcmp(back, "A") == 0);
    assert(pa_store_grade(&store, 0, PA_GOOD));
    assert(pa_store_next(&store, "D1") != PA_NO_CARD);
    assert(pa_store_heap_in_use() == 0);
    assert(pa_store_heap_hwm() == 0);

    pa_store fresh;
    pa_store_init(&fresh, &cards, &sched);
    assert(pa_store_mount(&fresh) == PA_STORE_READY);
    assert(pa_store_count(&fresh) == count);
    assert(pa_store_next(&fresh, "D1") != PA_NO_CARD);
    free_flash(&cards_ram);
    free_flash(&sched_ram);
    puts("store: 10000 cards, streaming next, heap high-water PASS");
}

static void test_power_loss_and_publish(void)
{
    ram_flash cards_ram, sched_ram;
    pa_flash cards, sched;
    make_flash(&cards_ram, &cards, PA_CARDS_PARTITION_SIZE);
    make_flash(&sched_ram, &sched, PA_SCHED_PARTITION_SIZE);
    pa_store store;
    pa_store_init(&store, &cards, &sched);
    assert(pa_store_mount(&store) == PA_STORE_EMPTY);

    uint8_t first[32];
    size_t first_len = append_card(first, 0, 1, "D", "Keep", "A");
    assert(import_bytes(&store, first, 1, (uint32_t)first_len));
    uint8_t super_saved[PA_STORE_SECTOR * 2];
    memcpy(super_saved, sched_ram.data, sizeof(super_saved));

    uint8_t second[32];
    size_t second_len = append_card(second, 0, 2, "D", "Replace", "B");
    assert(import_bytes(&store, second, 1, (uint32_t)second_len));
    char deck[PA_DECK_BYTES], front[64], back[64];
    assert(pa_store_read(&store, 0, deck, sizeof(deck), front, sizeof(front), back, sizeof(back)));
    assert(strcmp(front, "Replace") == 0);

    memcpy(sched_ram.data, super_saved, sizeof(super_saved));
    pa_store remounted;
    pa_store_init(&remounted, &cards, &sched);
    assert(pa_store_mount(&remounted) == PA_STORE_READY);
    assert(pa_store_count(&remounted) == 1);
    assert(pa_store_read(&remounted, 0, deck, sizeof(deck), front, sizeof(front), back, sizeof(back)));
    assert(strcmp(front, "Keep") == 0);

    sched_ram.writes = 0;
    sched_ram.fail_write_number = 1;
    assert(!pa_store_grade(&remounted, 0, PA_AGAIN));
    pa_store after_tear;
    pa_store_init(&after_tear, &cards, &sched);
    assert(pa_store_mount(&after_tear) == PA_STORE_READY);
    assert(pa_store_read(&after_tear, 0, deck, sizeof(deck), front, sizeof(front), back, sizeof(back)));
    assert(strcmp(front, "Keep") == 0);
    assert(pa_store_next(&after_tear, "D") == 0);

    uint16_t before = 0;
    for (uint16_t i = 0; i < after_tear.pool_count; ++i) {
        before = after_tear.erase_count[i] > before ? after_tear.erase_count[i] : before;
    }
    for (int n = 0; n < 300; ++n) {
        assert(pa_store_advance(&after_tear, 1));
        assert(pa_store_grade(&after_tear, 0, PA_AGAIN));
    }
    uint16_t max_erase = 0;
    uint16_t used = 0;
    for (uint16_t i = 0; i < after_tear.pool_count; ++i) {
        if (after_tear.erase_count[i] > 0) {
            ++used;
        }
        if (after_tear.erase_count[i] > max_erase) {
            max_erase = after_tear.erase_count[i];
        }
    }
    assert(used > 100);
    assert(max_erase < before + 8);
    free_flash(&cards_ram);
    free_flash(&sched_ram);
    puts("store: power-loss publish point and wear spread PASS");
}

static void test_resume_and_migration(void)
{
    ram_flash cards_ram, sched_ram;
    pa_flash cards, sched;
    make_flash(&cards_ram, &cards, PA_CARDS_PARTITION_SIZE);
    make_flash(&sched_ram, &sched, PA_SCHED_PARTITION_SIZE);
    pa_store store;
    pa_store_init(&store, &cards, &sched);
    assert(pa_store_mount(&store) == PA_STORE_EMPTY);

    const uint32_t batch = 342;
    uint8_t *payload = malloc(batch * 16u);
    assert(payload != NULL);
    size_t length = 0;
    for (uint32_t i = 1; i <= batch; ++i) {
        length = append_card(payload, length, i, "D", "Q", "A");
    }
    assert(length > PA_STORE_SECTOR);
    uint32_t resume = 0;
    uint32_t crc = pa_crc32(payload, length);
    assert(pa_store_import_begin(&store, batch, crc, (uint32_t)length, &resume));
    uint32_t accepted = 0;
    assert(pa_store_import_write(&store, 0, payload, PA_STORE_SECTOR, &accepted) && accepted == PA_STORE_SECTOR);
    pa_store rebooted;
    pa_store_init(&rebooted, &cards, &sched);
    assert(pa_store_mount(&rebooted) == PA_STORE_EMPTY);
    uint32_t got_off = 1, got_count = 0, got_crc = 0, got_len = 0;
    assert(pa_store_import_resume(&rebooted, &got_off, &got_count, &got_crc, &got_len));
    assert(got_off == PA_STORE_SECTOR && got_count == batch && got_crc == crc && got_len == length);
    assert(pa_store_import_begin(&rebooted, batch, crc, (uint32_t)length, &resume) && resume == PA_STORE_SECTOR);
    assert(pa_store_import_write(&rebooted, PA_STORE_SECTOR, payload + PA_STORE_SECTOR,
                                 (uint32_t)length - PA_STORE_SECTOR, &accepted));
    assert(accepted == length);
    assert(pa_store_import_commit(&rebooted, NULL, NULL));
    assert(pa_store_count(&rebooted) == batch);
    free(payload);

    pa_collection collection;
    pa_init(&collection);
    collection.now = 50;
    assert(pa_add(&collection, 9, "Old", "Front", "Back"));
    pa_session session;
    pa_begin(&session, &collection, "Old");
    assert(pa_flip(&session));
    assert(pa_grade(&collection, &session, PA_AGAIN));
    uint8_t snapshot[PA_SNAPSHOT_MAX];
    size_t snapshot_size = pa_encode(&collection, snapshot, sizeof(snapshot));
    assert(snapshot_size != 0);

    ram_flash cards2, sched2;
    pa_flash cflash, sflash;
    make_flash(&cards2, &cflash, PA_CARDS_PARTITION_SIZE);
    make_flash(&sched2, &sflash, PA_SCHED_PARTITION_SIZE);
    pa_store migrated;
    pa_store_init(&migrated, &cflash, &sflash);
    assert(pa_store_mount(&migrated) == PA_STORE_EMPTY);
    assert(pa_store_migrate_pak1(&migrated, snapshot, snapshot_size));
    assert(pa_store_count(&migrated) == 1);
    assert(pa_store_now(&migrated) == 50);
    assert(pa_store_next(&migrated, "Old") == PA_NO_CARD);
    assert(pa_store_advance(&migrated, 1));
    assert(pa_store_next(&migrated, "Old") == 0);
    char deck[PA_DECK_BYTES], front[64], back[64];
    assert(pa_store_read(&migrated, 0, deck, sizeof(deck), front, sizeof(front), back, sizeof(back)));
    assert(strcmp(deck, "Old") == 0 && strcmp(front, "Front") == 0 && strcmp(back, "Back") == 0);
    assert(!pa_store_migrate_pak1(&migrated, snapshot, snapshot_size));

    free_flash(&cards_ram);
    free_flash(&sched_ram);
    free_flash(&cards2);
    free_flash(&sched2);
    puts("store: resume, PAK1 migration, duplicate publish guard PASS");
}

static void test_duplicate_rejected(void)
{
    ram_flash cards_ram, sched_ram;
    pa_flash cards, sched;
    make_flash(&cards_ram, &cards, PA_CARDS_PARTITION_SIZE);
    make_flash(&sched_ram, &sched, PA_SCHED_PARTITION_SIZE);
    pa_store store;
    pa_store_init(&store, &cards, &sched);
    assert(pa_store_mount(&store) == PA_STORE_EMPTY);
    uint8_t payload[64];
    size_t length = append_card(payload, 0, 5, "D", "Q", "A");
    length = append_card(payload, length, 5, "D", "Q2", "A2");
    assert(!import_bytes(&store, payload, 2, (uint32_t)length));
    assert(pa_store_mount(&store) == PA_STORE_EMPTY);
    assert(pa_store_count(&store) == 0);
    free_flash(&cards_ram);
    free_flash(&sched_ram);
    puts("store: duplicate id rejected without publishing PASS");
}

static void test_revlog_export_roundtrip(void)
{
    ram_flash cards_ram, sched_ram;
    pa_flash cards, sched;
    make_flash(&cards_ram, &cards, PA_CARDS_PARTITION_SIZE);
    make_flash(&sched_ram, &sched, PA_SCHED_PARTITION_SIZE);
    pa_store store;
    pa_store_init(&store, &cards, &sched);
    assert(pa_store_mount(&store) == PA_STORE_EMPTY);
    uint8_t payload[64];
    size_t length = append_card(payload, 0, 7, "D", "Q", "A");
    length = append_card(payload, length, 8, "D", "Q2", "A2");
    assert(import_bytes(&store, payload, 2, (uint32_t)length));
    assert(pa_store_grade(&store, 1, PA_EASY));
    assert(pa_store_advance(&store, 3));
    assert(pa_store_grade(&store, 0, PA_AGAIN));

    pa_export_info info;
    assert(pa_store_export_begin(&store, &info));
    assert(info.card_count == 2 && info.revlog_count == 2 && info.dropped == 0);
    assert(info.length == PA_EXPORT_HEADER + 2u * PA_EXPORT_SCHED + 2u * PA_REVLOG_RECORD);
    uint8_t image[128];
    assert(info.length <= sizeof(image));
    uint32_t filled = 0;
    while (filled < info.length) {
        uint32_t got = 0;
        assert(pa_store_export_read(&store, filled, image + filled, 10, &got) && got > 0);
        filled += got;
    }
    assert(filled == info.length);
    assert(pa_crc32(image, info.length) == info.crc);
    assert(memcmp(image, "PREV", 4) == 0);
    const uint8_t *rev = image + PA_EXPORT_HEADER + 2u * PA_EXPORT_SCHED;
    assert(rev[0] == 1 && rev[1] == 0 && rev[2] == PA_EASY);
    assert(rev[8] == 0 && rev[9] == 0 && rev[10] == PA_AGAIN);
    pa_export_info status;
    assert(pa_store_export_status(&store, &status) && status.served == info.length);
    assert(!pa_store_export_read(&store, info.length, image, 1, &filled));

    pa_store remounted;
    pa_store_init(&remounted, &cards, &sched);
    assert(pa_store_mount(&remounted) == PA_STORE_READY);
    pa_export_info again;
    assert(pa_store_export_begin(&remounted, &again));
    assert(again.revlog_count == 2 && again.crc == info.crc && again.epoch == info.epoch);
    uint8_t prefix[16];
    uint32_t got = 0;
    assert(pa_store_export_read(&remounted, 0, prefix, sizeof(prefix), &got) && got == sizeof(prefix));
    assert(pa_store_export_status(&remounted, &status) && status.served == sizeof(prefix));
    assert(pa_store_grade(&remounted, 0, PA_HARD));
    assert(!pa_store_export_read(&remounted, 0, prefix, sizeof(prefix), &got));

    assert(import_bytes(&remounted, payload, 2, (uint32_t)length));
    assert(pa_store_export_begin(&remounted, &again));
    assert(again.revlog_count == 0 && again.dropped == 0 && again.epoch != info.epoch);
    free_flash(&cards_ram);
    free_flash(&sched_ram);
    printf("store: review log export PASS; pa_store %zu bytes; revlog max %u\n", sizeof(pa_store),
           PA_REVLOG_MAX);
}

int main(void)
{
    test_pack_vector();
    test_duplicate_rejected();
    test_resume_and_migration();
    test_power_loss_and_publish();
    test_ten_thousand_and_heap();
    test_revlog_export_roundtrip();
    return 0;
}
