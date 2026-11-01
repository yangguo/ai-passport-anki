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
    assert(pa_store_capacity(&store) == PA_STORE_MAX_CARDS);

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

static void mount_pair(pa_store *store, ram_flash *cards_ram, pa_flash *cards, ram_flash *sched_ram,
                       pa_flash *sched, uint32_t cards_size, uint32_t sched_size)
{
    make_flash(cards_ram, cards, cards_size);
    make_flash(sched_ram, sched, sched_size);
    pa_store_init(store, cards, sched);
}

static void test_region_plan_and_compact_slot(void)
{
    uint32_t cards_bytes = 0;
    uint32_t sched_bytes = 0;
    assert(!pa_region_plan(0, &cards_bytes, &sched_bytes));
    assert(pa_region_plan(PA_CARDS_PARTITION_SIZE + PA_SCHED_PARTITION_SIZE, &cards_bytes, &sched_bytes));
    assert(cards_bytes == PA_CARDS_PARTITION_SIZE && sched_bytes == PA_SCHED_PARTITION_SIZE);
    assert(pa_region_plan(PA_CARDS_PARTITION_SIZE + PA_SCHED_PARTITION_SIZE + 4096u, &cards_bytes,
                          &sched_bytes));
    assert(cards_bytes == PA_CARDS_PARTITION_SIZE && sched_bytes == PA_SCHED_PARTITION_SIZE);

    assert(pa_region_plan(61440u, &cards_bytes, &sched_bytes));
    assert(cards_bytes + sched_bytes <= 61440u);
    assert(pa_store_capacity(NULL) == 0);

    ram_flash cards_ram, sched_ram;
    pa_flash cards, sched;
    pa_store store;
    mount_pair(&store, &cards_ram, &cards, &sched_ram, &sched, cards_bytes, sched_bytes);
    assert(pa_store_mount(&store) == PA_STORE_EMPTY);
    uint16_t cap = pa_store_capacity(&store);
    assert(cap == 256);
    assert(pa_store_payload_limit(&store) == 4096u);
    uint32_t resume = 0;
    assert(!pa_store_import_begin(&store, (uint32_t)cap + 1u, 1, (uint32_t)(cap + 1u) * 11u, &resume));
    assert(!pa_store_import_begin(&store, 1, 1, pa_store_payload_limit(&store) + 1u, &resume));

    uint8_t payload[64];
    size_t length = append_card(payload, 0, 3, "D", "Q", "A");
    assert(import_bytes(&store, payload, 1, (uint32_t)length));
    assert(pa_store_grade(&store, 0, PA_GOOD));
    pa_store remounted;
    pa_store_init(&remounted, &cards, &sched);
    assert(pa_store_mount(&remounted) == PA_STORE_READY);
    assert(pa_store_count(&remounted) == 1);
    assert(pa_store_capacity(&remounted) == cap);
    pa_export_info info;
    assert(pa_store_export_begin(&remounted, &info));
    assert(info.revlog_count == 1 && info.dropped == 0);
    free_flash(&cards_ram);
    free_flash(&sched_ram);

    mount_pair(&store, &cards_ram, &cards, &sched_ram, &sched, 24576u, 24576u);
    assert(pa_store_mount(&store) == PA_STORE_EMPTY);
    assert(pa_store_capacity(&store) > 0 && pa_store_capacity(&store) < PA_STORE_MAX_CARDS);
    length = append_card(payload, 0, 4, "D", "Q", "A");
    assert(import_bytes(&store, payload, 1, (uint32_t)length));
    assert(pa_store_grade(&store, 0, PA_AGAIN));
    assert(pa_store_export_begin(&store, &info));
    assert(info.revlog_count == 0 && info.dropped == 1);
    free_flash(&cards_ram);
    free_flash(&sched_ram);
    puts("store: compact region plan PASS");
}

static void test_due_counts(void)
{
    ram_flash cards_ram, sched_ram;
    pa_flash cards, sched;
    make_flash(&cards_ram, &cards, PA_CARDS_PARTITION_SIZE);
    make_flash(&sched_ram, &sched, PA_SCHED_PARTITION_SIZE);
    pa_store store;
    pa_store_init(&store, &cards, &sched);
    assert(pa_store_mount(&store) == PA_STORE_EMPTY);
    uint8_t payload[128];
    size_t length = append_card(payload, 0, 1, "A", "Q", "A");
    length = append_card(payload, length, 2, "A", "Q2", "A2");
    length = append_card(payload, length, 3, "B", "Q3", "A3");
    assert(import_bytes(&store, payload, 3, (uint32_t)length));
    uint16_t counts[PA_STORE_MAX_DECKS];
    pa_store_due_counts(&store, counts);
    assert(counts[0] == 2 && counts[1] == 1);
    assert(pa_store_advance(&store, 10));
    assert(pa_store_grade(&store, 0, PA_GOOD));
    pa_store_due_counts(&store, counts);
    assert(counts[0] == 1 && counts[1] == 1);
    free_flash(&cards_ram);
    free_flash(&sched_ram);
    puts("store: due counts PASS");
}

static void test_today_stats(void)
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
    uint32_t by[4] = {0, 0, 0, 0};
    uint32_t total = 0;
    pa_store_today_stats(&store, by, &total);
    assert(total == 2 && by[PA_AGAIN] == 1 && by[PA_EASY] == 1);
    assert(pa_store_advance(&store, 1440));
    pa_store_today_stats(&store, by, &total);
    assert(total == 0 && by[0] == 0 && by[3] == 0);
    free_flash(&cards_ram);
    free_flash(&sched_ram);
    puts("store: today stats PASS");
}

static void test_limits_and_is_new(void)
{
    ram_flash cards_ram, sched_ram;
    pa_flash cards, sched;
    make_flash(&cards_ram, &cards, PA_CARDS_PARTITION_SIZE);
    make_flash(&sched_ram, &sched, PA_SCHED_PARTITION_SIZE);
    pa_store store;
    pa_store_init(&store, &cards, &sched);
    assert(pa_store_mount(&store) == PA_STORE_EMPTY);
    uint8_t payload[64];
    size_t length = append_card(payload, 0, 1, "D", "Q", "A");
    length = append_card(payload, length, 2, "D", "Q2", "A2");
    assert(import_bytes(&store, payload, 2, (uint32_t)length));
    assert(!pa_store_is_new(NULL, 0));
    assert(!pa_store_is_new(&store, 99));
    assert(pa_store_is_new(&store, 0));
    assert(pa_store_is_new(&store, 1));
    pa_store_set_limits(NULL, 1, 1);
    pa_store_set_shuffle(NULL, true, 42);
    pa_store_set_limits(&store, 20, 0);
    assert(pa_store_grade(&store, 0, PA_GOOD));
    assert(!pa_store_is_new(&store, 0));
    assert(pa_store_is_new(&store, 1));
    free_flash(&cards_ram);
    free_flash(&sched_ram);
    puts("store: limits setters and is_new PASS");
}

static void test_new_limit_skip(void)
{
    ram_flash cards_ram, sched_ram;
    pa_flash cards, sched;
    make_flash(&cards_ram, &cards, PA_CARDS_PARTITION_SIZE);
    make_flash(&sched_ram, &sched, PA_SCHED_PARTITION_SIZE);
    pa_store store;
    pa_store_init(&store, &cards, &sched);
    assert(pa_store_mount(&store) == PA_STORE_EMPTY);
    uint8_t payload[64];
    size_t length = append_card(payload, 0, 1, "D", "Q", "A");
    length = append_card(payload, length, 2, "D", "Q2", "A2");
    assert(import_bytes(&store, payload, 2, (uint32_t)length));
    /* Card 0: NEW -> Good (LEARNING s1, due=10) -> advance -> Good
     * (REVIEW I=1, due=now+1440) -> advance -> REVIEW due. */
    assert(pa_store_grade(&store, 0, PA_GOOD));
    assert(pa_store_advance(&store, 10));
    assert(pa_store_grade(&store, 0, PA_GOOD));
    assert(pa_store_advance(&store, 1440));
    /* Card 1 is still NEW due (due=0 <= now). Limit reached: only REVIEW surfaces. */
    pa_store_set_limits(&store, 1, 1);
    assert(pa_store_next(&store, "D") == 0);
    uint16_t counts[PA_STORE_MAX_DECKS];
    pa_store_new_due_counts(&store, counts);
    assert(counts[0] == 1);
    /* Budget left: push card 0 to the future so only NEW remains, then NEW surfaces. */
    pa_store_set_limits(&store, 1, 0);
    assert(pa_store_grade(&store, 0, PA_GOOD));
    assert(pa_store_next(&store, "D") == 1);
    /* Limit 0 means unlimited even with huge done. */
    pa_store_set_limits(&store, 0, 999);
    assert(pa_store_next(&store, "D") == 1);
    pa_store_new_due_counts(NULL, counts);
    free_flash(&cards_ram);
    free_flash(&sched_ram);
    puts("store: new limit skip PASS");
}

static void test_shuffle_within_priority(void)
{
    ram_flash cards_ram, sched_ram;
    pa_flash cards, sched;
    make_flash(&cards_ram, &cards, PA_CARDS_PARTITION_SIZE);
    make_flash(&sched_ram, &sched, PA_SCHED_PARTITION_SIZE);
    pa_store store;
    pa_store_init(&store, &cards, &sched);
    assert(pa_store_mount(&store) == PA_STORE_EMPTY);
    uint8_t payload[128];
    size_t length = 0;
    for (uint32_t i = 1; i <= 5; ++i) {
        length = append_card(payload, length, i, "D", "Q", "A");
    }
    assert(import_bytes(&store, payload, 5, (uint32_t)length));
    /* One LEARNING-due card beats NEW cards even when shuffled. */
    assert(pa_store_grade(&store, 0, PA_AGAIN));
    assert(pa_store_advance(&store, 1));
    pa_store_set_shuffle(&store, true, 1234);
    assert(pa_store_next(&store, "D") == 0);
    /* Deterministic: same seed, same pick. */
    pa_store_set_shuffle(&store, true, 777);
    uint16_t first = pa_store_next(&store, "D");
    pa_store_set_shuffle(&store, true, 777);
    assert(pa_store_next(&store, "D") == first);
    free_flash(&cards_ram);
    free_flash(&sched_ram);

    /* All-NEW deck: shuffled picks spread across indices. */
    make_flash(&cards_ram, &cards, PA_CARDS_PARTITION_SIZE);
    make_flash(&sched_ram, &sched, PA_SCHED_PARTITION_SIZE);
    pa_store_init(&store, &cards, &sched);
    assert(pa_store_mount(&store) == PA_STORE_EMPTY);
    length = 0;
    for (uint32_t i = 1; i <= 5; ++i) {
        length = append_card(payload, length, i, "E", "Q", "A");
    }
    assert(import_bytes(&store, payload, 5, (uint32_t)length));
    bool seen[5] = {false, false, false, false, false};
    for (uint32_t seed = 1; seed <= 100; ++seed) {
        pa_store_set_shuffle(&store, true, seed);
        uint16_t got = pa_store_next(&store, "E");
        assert(got < 5);
        seen[got] = true;
    }
    int distinct = 0;
    for (int i = 0; i < 5; ++i) {
        distinct += seen[i] ? 1 : 0;
    }
    assert(distinct >= 3);
    /* Shuffle off restores due/id order: all NEW due=0, lowest id wins. */
    pa_store_set_shuffle(&store, false, 0);
    assert(pa_store_next(&store, "E") == 0);
    free_flash(&cards_ram);
    free_flash(&sched_ram);
    puts("store: shuffle within priority PASS");
}

static void test_forecast_and_earliest(void)
{
    ram_flash cards_ram, sched_ram;
    pa_flash cards, sched;
    make_flash(&cards_ram, &cards, PA_CARDS_PARTITION_SIZE);
    make_flash(&sched_ram, &sched, PA_SCHED_PARTITION_SIZE);
    pa_store store;
    pa_store_init(&store, &cards, &sched);
    assert(pa_store_mount(&store) == PA_STORE_EMPTY);
    uint8_t payload[64];
    size_t length = append_card(payload, 0, 1, "D", "Q", "A");
    length = append_card(payload, length, 2, "D", "Q2", "A2");
    assert(import_bytes(&store, payload, 2, (uint32_t)length));
    /* idx0 NEW+Good -> LEARNING s1 due=10. idx1 NEW+Good -> LEARNING s1 due=10. */
    assert(pa_store_grade(&store, 0, PA_GOOD));
    assert(pa_store_grade(&store, 1, PA_GOOD));
    /* advance(10): now=10. idx1 +Good -> REVIEW I=1 due=1450. */
    assert(pa_store_advance(&store, 10));
    assert(pa_store_grade(&store, 1, PA_GOOD));
    /* advance(1440): now=1450. idx1 +Hard -> I=max(2,ceil(1.2))=2, due=4330. */
    assert(pa_store_advance(&store, 1440));
    assert(pa_store_grade(&store, 1, PA_HARD));
    /* idx0 +Good at now=1450: LEARNING s1 -> REVIEW I=1, due=2890. */
    assert(pa_store_grade(&store, 0, PA_GOOD));
    uint16_t buckets[3] = {9, 9, 9};
    pa_store_forecast(&store, buckets);
    /* now=1450: idx0 due=2890 -> bucket0; idx1 due=4330 -> bucket1. */
    assert(buckets[0] == 1 && buckets[1] == 1 && buckets[2] == 0);
    uint32_t earliest = 0;
    assert(pa_store_earliest_due(&store, "D", &earliest) && earliest == 2890);
    assert(!pa_store_earliest_due(&store, "missing", &earliest));
    assert(!pa_store_earliest_due(&store, "D", NULL));
    assert(!pa_store_earliest_due(NULL, "D", &earliest));
    /* NULL store zeroes buckets (same convention as the due-count queries). */
    pa_store_forecast(NULL, buckets);
    assert(buckets[0] == 0 && buckets[1] == 0 && buckets[2] == 0);
    free_flash(&cards_ram);
    free_flash(&sched_ram);
    puts("store: forecast and earliest due PASS");
}

static void test_shuffle_with_limit(void)
{
    ram_flash cards_ram, sched_ram;
    pa_flash cards, sched;
    make_flash(&cards_ram, &cards, PA_CARDS_PARTITION_SIZE);
    make_flash(&sched_ram, &sched, PA_SCHED_PARTITION_SIZE);
    pa_store store;
    pa_store_init(&store, &cards, &sched);
    assert(pa_store_mount(&store) == PA_STORE_EMPTY);
    /* idx0 becomes REVIEW-due, idx1..idx3 stay NEW-due.
     * Card 0: NEW -> Good (LEARNING s1, due=10) -> advance -> Good
     * (REVIEW I=1, due=now+1440) -> advance -> REVIEW due. */
    uint8_t payload[128];
    size_t length = 0;
    for (uint32_t i = 1; i <= 4; ++i) {
        length = append_card(payload, length, i, "D", "Q", "A");
    }
    assert(import_bytes(&store, payload, 4, (uint32_t)length));
    assert(pa_store_grade(&store, 0, PA_GOOD));
    assert(pa_store_advance(&store, 10));
    assert(pa_store_grade(&store, 0, PA_GOOD));
    assert(pa_store_advance(&store, 1440));
    /* Limit reached (1 new already counted) + shuffle on: the REVIEW card
     * must win for every seed. */
    pa_store_set_limits(&store, 1, 1);
    for (uint32_t seed = 1; seed <= 10; ++seed) {
        pa_store_set_shuffle(&store, true, seed);
        assert(pa_store_next(&store, "D") == 0);
    }
    /* Push the REVIEW card to the future: only NEW remains, still gated,
     * so nothing is due even under shuffle. */
    pa_store_set_shuffle(&store, true, 42);
    assert(pa_store_grade(&store, 0, PA_GOOD));
    assert(pa_store_next(&store, "D") == PA_NO_CARD);
    /* Budget left: shuffled NEW cards surface with spread across seeds. */
    pa_store_set_limits(&store, 1, 0);
    bool seen[4] = {false, false, false, false};
    for (uint32_t seed = 1; seed <= 50; ++seed) {
        pa_store_set_shuffle(&store, true, seed);
        uint16_t got = pa_store_next(&store, "D");
        assert(got >= 1 && got <= 3);
        seen[got] = true;
    }
    int distinct = 0;
    for (int i = 1; i <= 3; ++i) {
        distinct += seen[i] ? 1 : 0;
    }
    assert(distinct >= 2);
    free_flash(&cards_ram);
    free_flash(&sched_ram);
    puts("store: shuffle with new limit PASS");
}

int main(void)
{
    test_pack_vector();
    test_duplicate_rejected();
    test_resume_and_migration();
    test_power_loss_and_publish();
    test_ten_thousand_and_heap();
    test_revlog_export_roundtrip();
    test_region_plan_and_compact_slot();
    test_due_counts();
    test_today_stats();
    test_limits_and_is_new();
    test_new_limit_skip();
    test_shuffle_within_priority();
    test_shuffle_with_limit();
    test_forecast_and_earliest();
    return 0;
}
