#define _POSIX_C_SOURCE 200809L

#include "pa_protocol.h"
#include "pa_store.h"

#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static pa_store store;
static pa_flash cards_flash;
static pa_flash sched_flash;
static uint8_t *cards_image;
static uint8_t *sched_image;
static uint8_t *cards_backup;
static uint8_t *sched_backup;
static pa_session session;
static const char *state_path;
static char selected_deck[PA_DECK_BYTES];
static char shown_deck[PA_DECK_BYTES];
static char shown_front[PA_STORE_TEXT_MAX + 1u];
static char shown_back[PA_STORE_TEXT_MAX + 1u];
static bool deck_selected;
static bool quitting;

typedef enum {
    INPUT_READY,
    INPUT_END,
    INPUT_TOO_LONG,
    INPUT_INVALID,
    INPUT_ERROR
} input_result;

typedef struct {
    uint8_t *data;
    uint32_t size;
} ram_ctx;

static bool ram_read(void *ctx, uint32_t offset, void *dst, uint32_t length)
{
    ram_ctx *ram = ctx;
    if (offset > ram->size || length > ram->size - offset) {
        return false;
    }
    memcpy(dst, ram->data + offset, length);
    return true;
}

static bool ram_write(void *ctx, uint32_t offset, const void *src, uint32_t length)
{
    ram_ctx *ram = ctx;
    if (offset > ram->size || length > ram->size - offset) {
        return false;
    }
    memcpy(ram->data + offset, src, length);
    return true;
}

static bool ram_erase(void *ctx, uint32_t offset, uint32_t length)
{
    ram_ctx *ram = ctx;
    if ((offset % PA_STORE_SECTOR) != 0 || (length % PA_STORE_SECTOR) != 0 ||
        offset > ram->size || length > ram->size - offset) {
        return false;
    }
    memset(ram->data + offset, 0xff, length);
    return true;
}

static ram_ctx cards_ctx;
static ram_ctx sched_ctx;

static uint32_t crc_feed(uint32_t crc, const uint8_t *data, size_t length)
{
    for (size_t i = 0; i < length; ++i) {
        crc ^= data[i];
        for (unsigned bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^ (0xedb88320u & (uint32_t)-(int32_t)(crc & 1u));
        }
    }
    return crc;
}

static uint32_t images_crc(void)
{
    uint32_t crc = crc_feed(0xffffffffu, cards_image, PA_CARDS_PARTITION_SIZE);
    crc = crc_feed(crc, sched_image, PA_SCHED_PARTITION_SIZE);
    return crc ^ 0xffffffffu;
}

static void put_u16(uint8_t *p, uint16_t value)
{
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
}

static void put_u32(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
    p[2] = (uint8_t)(value >> 16);
    p[3] = (uint8_t)(value >> 24);
}

static uint16_t get_u16(const uint8_t *p)
{
    return (uint16_t)p[0] | (uint16_t)((uint16_t)p[1] << 8);
}

static uint32_t get_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void bind_flash(void)
{
    cards_ctx = (ram_ctx){.data = cards_image, .size = PA_CARDS_PARTITION_SIZE};
    sched_ctx = (ram_ctx){.data = sched_image, .size = PA_SCHED_PARTITION_SIZE};
    cards_flash = (pa_flash){.ctx = &cards_ctx, .size = PA_CARDS_PARTITION_SIZE,
                             .read = ram_read, .write = ram_write, .erase = ram_erase};
    sched_flash = (pa_flash){.ctx = &sched_ctx, .size = PA_SCHED_PARTITION_SIZE,
                             .read = ram_read, .write = ram_write, .erase = ram_erase};
    pa_store_init(&store, &cards_flash, &sched_flash);
}

static void erase_images(void)
{
    memset(cards_image, 0xff, PA_CARDS_PARTITION_SIZE);
    memset(sched_image, 0xff, PA_SCHED_PARTITION_SIZE);
}

static bool save_state(void)
{
    if (state_path == NULL) {
        return true;
    }
    size_t path_length = strlen(state_path);
    if (path_length > SIZE_MAX - sizeof(".tmp.XXXXXX")) {
        return false;
    }
    char *temporary_path = malloc(path_length + sizeof(".tmp.XXXXXX"));
    if (temporary_path == NULL) {
        return false;
    }
    (void)snprintf(temporary_path, path_length + sizeof(".tmp.XXXXXX"), "%s.tmp.XXXXXX", state_path);
    int fd = mkstemp(temporary_path);
    if (fd < 0) {
        free(temporary_path);
        return false;
    }
    FILE *file = fdopen(fd, "wb");
    if (file == NULL) {
        (void)close(fd);
        (void)unlink(temporary_path);
        free(temporary_path);
        return false;
    }
    uint8_t header[20];
    memcpy(header, "PAK2", 4);
    put_u16(header + 4, 2);
    put_u16(header + 6, 0);
    put_u32(header + 8, PA_CARDS_PARTITION_SIZE);
    put_u32(header + 12, PA_SCHED_PARTITION_SIZE);
    put_u32(header + 16, images_crc());
    bool ok = fwrite(header, 1, sizeof(header), file) == sizeof(header) &&
              fwrite(cards_image, 1, PA_CARDS_PARTITION_SIZE, file) == PA_CARDS_PARTITION_SIZE &&
              fwrite(sched_image, 1, PA_SCHED_PARTITION_SIZE, file) == PA_SCHED_PARTITION_SIZE;
    if (fflush(file) != 0 || fsync(fileno(file)) != 0 || fclose(file) != 0) {
        ok = false;
    }
    if (ok && rename(temporary_path, state_path) != 0) {
        ok = false;
    }
    if (!ok) {
        (void)unlink(temporary_path);
    }
    free(temporary_path);
    return ok;
}

static void free_backup(void)
{
    free(cards_backup);
    free(sched_backup);
    cards_backup = NULL;
    sched_backup = NULL;
}

static bool take_backup(void)
{
    free_backup();
    if (state_path == NULL) {
        return true;
    }
    cards_backup = malloc(PA_CARDS_PARTITION_SIZE);
    sched_backup = malloc(PA_SCHED_PARTITION_SIZE);
    if (cards_backup == NULL || sched_backup == NULL) {
        free_backup();
        return false;
    }
    memcpy(cards_backup, cards_image, PA_CARDS_PARTITION_SIZE);
    memcpy(sched_backup, sched_image, PA_SCHED_PARTITION_SIZE);
    return true;
}

static void restore_backup(void)
{
    if (cards_backup == NULL || sched_backup == NULL) {
        return;
    }
    memcpy(cards_image, cards_backup, PA_CARDS_PARTITION_SIZE);
    memcpy(sched_image, sched_backup, PA_SCHED_PARTITION_SIZE);
    free_backup();
    bind_flash();
    (void)pa_store_mount(&store);
}

static bool load_pak1(const uint8_t *data, size_t size)
{
    erase_images();
    bind_flash();
    if (pa_store_mount(&store) != PA_STORE_EMPTY || !pa_store_migrate_pak1(&store, data, size)) {
        return false;
    }
    return save_state();
}

static bool load_state(void)
{
    cards_image = malloc(PA_CARDS_PARTITION_SIZE);
    sched_image = malloc(PA_SCHED_PARTITION_SIZE);
    if (cards_image == NULL || sched_image == NULL) {
        return false;
    }
    erase_images();
    bind_flash();
    if (state_path == NULL) {
        return pa_store_mount(&store) != PA_STORE_CORRUPT;
    }
    FILE *file = fopen(state_path, "rb");
    if (file == NULL) {
        return errno == ENOENT && pa_store_mount(&store) != PA_STORE_CORRUPT;
    }
    if (fseek(file, 0, SEEK_END) != 0) {
        (void)fclose(file);
        return false;
    }
    long file_size = ftell(file);
    if (file_size < 0 || fseek(file, 0, SEEK_SET) != 0) {
        (void)fclose(file);
        return false;
    }
    uint8_t *data = malloc((size_t)file_size);
    if (data == NULL) {
        (void)fclose(file);
        return false;
    }
    bool read_ok = fread(data, 1, (size_t)file_size, file) == (size_t)file_size && fclose(file) == 0;
    if (!read_ok) {
        free(data);
        return false;
    }
    bool ok = false;
    if ((size_t)file_size >= 4 && memcmp(data, "PAK1", 4) == 0 && (size_t)file_size <= PA_SNAPSHOT_MAX) {
        ok = load_pak1(data, (size_t)file_size);
    } else if ((size_t)file_size == 20u + PA_CARDS_PARTITION_SIZE + PA_SCHED_PARTITION_SIZE &&
               memcmp(data, "PAK2", 4) == 0 && get_u16(data + 4) == 2 &&
               get_u32(data + 8) == PA_CARDS_PARTITION_SIZE &&
               get_u32(data + 12) == PA_SCHED_PARTITION_SIZE) {
        memcpy(cards_image, data + 20, PA_CARDS_PARTITION_SIZE);
        memcpy(sched_image, data + 20 + PA_CARDS_PARTITION_SIZE, PA_SCHED_PARTITION_SIZE);
        ok = images_crc() == get_u32(data + 16);
        if (ok) {
            bind_flash();
            pa_store_status status = pa_store_mount(&store);
            ok = status == PA_STORE_READY || status == PA_STORE_EMPTY;
        }
    }
    free(data);
    return ok;
}

static void reply_error(const char *code)
{
    printf("ERR %s\n", code);
    fflush(stdout);
}

static void reply_export(const char *kind, const pa_export_info *info)
{
    printf("OK %s cards=%u revlog=%" PRIu32 " crc=%" PRIu32 " length=%" PRIu32
           " epoch=%" PRIu32 " dropped=%" PRIu32 " now=%" PRIu32 " offset=%" PRIu32 "\n",
           kind, info->card_count, info->revlog_count, info->crc, info->length, info->epoch,
           info->dropped, info->now, info->served);
    fflush(stdout);
}

static bool no_arguments(const char *arguments)
{
    while (*arguments == ' ' || *arguments == '\t') {
        ++arguments;
    }
    return *arguments == '\0';
}

static bool parse_u32(const char *text, uint32_t *value)
{
    if (*text == '\0') {
        return false;
    }
    uint32_t parsed = 0;
    for (const unsigned char *p = (const unsigned char *)text; *p != '\0'; ++p) {
        if (*p < '0' || *p > '9') {
            return false;
        }
        uint32_t digit = (uint32_t)(*p - '0');
        if (parsed > (UINT32_MAX - digit) / 10u) {
            return false;
        }
        parsed = parsed * 10u + digit;
    }
    *value = parsed;
    return true;
}

static bool collection_has_deck(const char *name)
{
    for (uint16_t i = 0; i < pa_store_deck_count(&store); ++i) {
        const char *deck = pa_store_deck_name(&store, i);
        if (deck != NULL && strcmp(deck, name) == 0) {
            return true;
        }
    }
    return false;
}

static bool show_current(void)
{
    return pa_store_read(&store, session.current, shown_deck, sizeof(shown_deck), shown_front,
                         sizeof(shown_front), shown_back, sizeof(shown_back));
}

static void clear_session(void)
{
    session.current = PA_NO_CARD;
    session.revealed = false;
}

static void handle_import(const pa_protocol_command *command)
{
    switch (command->kind) {
    case PA_PROTOCOL_INFO:
        printf("OK INFO version=%u cards=%u day=%" PRIu32 " cap=%u\n", PA_PROTOCOL_VERSION,
               pa_store_count(&store), pa_store_now(&store) / 1440u, pa_store_capacity(&store));
        fflush(stdout);
        break;
    case PA_PROTOCOL_BEGIN: {
        if (store.import_active || session.current != PA_NO_CARD || !take_backup()) {
            reply_error("TRANSACTION");
            break;
        }
        uint32_t resume = 0;
        if (!pa_store_import_begin(&store, command->value, command->crc, command->length, &resume) ||
            !save_state()) {
            restore_backup();
            reply_error("STORAGE");
            break;
        }
        free_backup();
        printf("OK BEGIN resume=%" PRIu32 "\n", resume);
        fflush(stdout);
        break;
    }
    case PA_PROTOCOL_CHUNK: {
        if (!store.import_active || !take_backup()) {
            reply_error("TRANSACTION");
            break;
        }
        uint32_t accepted = 0;
        if (!pa_store_import_write(&store, command->value, command->chunk, command->length, &accepted) ||
            !save_state()) {
            restore_backup();
            reply_error("STORAGE");
            break;
        }
        free_backup();
        printf("OK CHUNK %" PRIu32 "\n", accepted);
        fflush(stdout);
        break;
    }
    case PA_PROTOCOL_COMMIT:
        if (!store.import_active || !take_backup()) {
            reply_error("TRANSACTION");
        } else if (!pa_store_import_commit(&store, NULL, NULL) || !save_state()) {
            restore_backup();
            reply_error("STORAGE");
        } else {
            free_backup();
            deck_selected = false;
            clear_session();
            printf("OK COMMIT\n");
            fflush(stdout);
        }
        break;
    case PA_PROTOCOL_ABORT:
        if (!take_backup()) {
            reply_error("STORAGE");
            break;
        }
        pa_store_import_abort(&store);
        if (!save_state()) {
            restore_backup();
            reply_error("STORAGE");
            break;
        }
        free_backup();
        printf("OK ABORT\n");
        fflush(stdout);
        break;
    case PA_PROTOCOL_RESUME: {
        uint32_t offset = 0, count = 0, crc = 0, length = 0;
        if (!pa_store_import_resume(&store, &offset, &count, &crc, &length)) {
            reply_error("TRANSACTION");
            break;
        }
        printf("OK RESUME offset=%" PRIu32 " count=%" PRIu32 " crc=%" PRIu32 " length=%" PRIu32 "\n",
               offset, count, crc, length);
        fflush(stdout);
        break;
    }
    case PA_PROTOCOL_DAY: {
        uint32_t day = command->value;
        if (store.import_active || session.current != PA_NO_CARD || day < pa_store_now(&store) / 1440u ||
            day > UINT32_MAX / 1440u) {
            reply_error("DAY");
            break;
        }
        if (!take_backup()) {
            reply_error("STORAGE");
            break;
        }
        uint32_t target = day * 1440u;
        if ((target > pa_store_now(&store) && !pa_store_advance(&store, target - pa_store_now(&store))) ||
            !pa_store_checkpoint(&store) || !save_state()) {
            restore_backup();
            reply_error("STORAGE");
            break;
        }
        free_backup();
        printf("OK DAY\n");
        fflush(stdout);
        break;
    }
    case PA_PROTOCOL_EXPORT: {
        pa_export_info info;
        if (store.import_active || !pa_store_export_begin(&store, &info)) {
            reply_error(store.import_active ? "TRANSACTION" : "REVLOG");
            break;
        }
        reply_export("EXPORT", &info);
        break;
    }
    case PA_PROTOCOL_XCHUNK: {
        uint8_t chunk[PA_PROTOCOL_CHUNK_MAX];
        uint32_t got = 0;
        char hex[PA_PROTOCOL_CHUNK_MAX * 2u + 1u];
        if (!pa_store_export_read(&store, command->value, chunk, sizeof(chunk), &got)) {
            reply_error("RANGE");
            break;
        }
        pa_protocol_write_hex(chunk, got, hex);
        printf("OK XCHUNK %" PRIu32 " %s\n", command->value + got, hex);
        fflush(stdout);
        break;
    }
    case PA_PROTOCOL_XRESUME: {
        pa_export_info info;
        if (!pa_store_export_status(&store, &info)) {
            reply_error("TRANSACTION");
            break;
        }
        reply_export("XRESUME", &info);
        break;
    }
    case PA_PROTOCOL_CARD:
        reply_error("COMMAND");
        break;
    }
}

static input_result read_line(char *line, size_t capacity, size_t *length)
{
    size_t used = 0;
    bool too_long = false;
    bool saw_newline = false;
    int byte;
    while ((byte = fgetc(stdin)) != EOF) {
        if (byte == '\n') {
            saw_newline = true;
            break;
        }
        if (used + 1 < capacity) {
            line[used++] = (char)byte;
        } else {
            too_long = true;
        }
    }
    if (byte == EOF && ferror(stdin)) {
        return INPUT_ERROR;
    }
    if (byte == EOF && used == 0 && !too_long && !saw_newline) {
        return INPUT_END;
    }
    if (too_long) {
        return INPUT_TOO_LONG;
    }

    line[used] = '\0';
    if (memchr(line, '\0', used) != NULL) {
        return INPUT_INVALID;
    }
    if (saw_newline && used > 0 && line[used - 1] == '\r') {
        line[--used] = '\0';
    }
    if (memchr(line, '\r', used) != NULL) {
        return INPUT_INVALID;
    }
    *length = used;
    return INPUT_READY;
}

static void handle_line(char *line, size_t original_length)
{
    char protocol_line[PA_PROTOCOL_MAX_LINE + 2u];
    memcpy(protocol_line, line, original_length + 1);
    char *separator = line;
    while (*separator != '\0' && *separator != ' ' && *separator != '\t') {
        ++separator;
    }
    char *arguments = separator;
    if (*separator != '\0') {
        *separator++ = '\0';
        while (*separator == ' ' || *separator == '\t') {
            ++separator;
        }
        arguments = separator;
    }

    if (strcmp(line, "HELP") == 0 && no_arguments(arguments)) {
        puts("命令：HELP, DECK <牌组>, NEXT, FLIP, RATE AGAIN|HARD|GOOD|EASY, ADVANCE <分钟>, QUIT");
        puts("导入协议：INFO, BEGIN count crc length, CHUNK offset hex, COMMIT, ABORT, RESUME, DAY n");
        puts("复习导出：EXPORT, XCHUNK offset, XRESUME");
        puts("RATE 只能在 FLIP 显示背面后执行；DAY 只能向前推进。");
        fflush(stdout);
        return;
    }
    if (strcmp(line, "QUIT") == 0 && no_arguments(arguments)) {
        quitting = true;
        puts("再见。");
        fflush(stdout);
        return;
    }
    if (strcmp(line, "DECK") == 0) {
        size_t length = strlen(arguments);
        while (length > 0 && (arguments[length - 1] == ' ' || arguments[length - 1] == '\t')) {
            arguments[--length] = '\0';
        }
        if (store.import_active) {
            reply_error("TRANSACTION");
        } else if (length == 0 || length >= sizeof(selected_deck) ||
                   !pa_text_valid(arguments, sizeof(selected_deck))) {
            reply_error("DECK");
        } else if (!collection_has_deck(arguments)) {
            reply_error("DECK");
        } else {
            memcpy(selected_deck, arguments, length + 1);
            deck_selected = true;
            clear_session();
            printf("已选择牌组：%s\n", selected_deck);
            fflush(stdout);
        }
        return;
    }
    if (strcmp(line, "NEXT") == 0 && no_arguments(arguments)) {
        if (store.import_active) {
            reply_error("TRANSACTION");
        } else if (!deck_selected) {
            reply_error("DECK_REQUIRED");
        } else {
            if (session.current == PA_NO_CARD) {
                session.current = pa_store_next(&store, selected_deck);
                session.revealed = false;
            }
            if (session.current == PA_NO_CARD) {
                puts("今天没有到期卡片。");
            } else if (!show_current()) {
                reply_error("STORAGE");
            } else {
                printf("正面：%s\n", shown_front);
            }
            fflush(stdout);
        }
        return;
    }
    if (strcmp(line, "FLIP") == 0 && no_arguments(arguments)) {
        if (store.import_active) {
            reply_error("TRANSACTION");
        } else if (session.current == PA_NO_CARD) {
            reply_error("NO_CARD");
        } else if (session.revealed) {
            reply_error("ALREADY_FLIPPED");
        } else if (!show_current()) {
            reply_error("STORAGE");
        } else {
            session.revealed = true;
            printf("背面：%s\n", shown_back);
            fflush(stdout);
        }
        return;
    }
    if (strcmp(line, "RATE") == 0) {
        pa_rating rating;
        if (strcmp(arguments, "AGAIN") == 0) {
            rating = PA_AGAIN;
        } else if (strcmp(arguments, "HARD") == 0) {
            rating = PA_HARD;
        } else if (strcmp(arguments, "GOOD") == 0) {
            rating = PA_GOOD;
        } else if (strcmp(arguments, "EASY") == 0) {
            rating = PA_EASY;
        } else {
            reply_error("RATING");
            return;
        }
        if (store.import_active) {
            reply_error("TRANSACTION");
        } else if (session.current == PA_NO_CARD) {
            reply_error("NO_CARD");
        } else if (!session.revealed) {
            reply_error("FLIP_REQUIRED");
        } else if (!take_backup()) {
            reply_error("STORAGE");
        } else if (!pa_store_grade(&store, session.current, rating)) {
            restore_backup();
            reply_error("GRADE");
        } else if (!save_state()) {
            restore_backup();
            reply_error("STORAGE");
        } else {
            free_backup();
            clear_session();
            static const char *display_names[] = {"Again", "Hard", "Good", "Easy"};
            printf("已评分：%s\n", display_names[rating]);
            fflush(stdout);
        }
        return;
    }
    if (strcmp(line, "ADVANCE") == 0) {
        uint32_t minutes;
        if (!parse_u32(arguments, &minutes)) {
            reply_error("MINUTES");
        } else if (store.import_active || session.current != PA_NO_CARD) {
            reply_error("TRANSACTION");
        } else if (!take_backup()) {
            reply_error("STORAGE");
        } else if (!pa_store_advance(&store, minutes)) {
            restore_backup();
            reply_error("TIME");
        } else if (!pa_store_checkpoint(&store) || !save_state()) {
            restore_backup();
            reply_error("STORAGE");
        } else {
            free_backup();
            printf("OK ADVANCE now=%" PRIu32 "\n", pa_store_now(&store));
            fflush(stdout);
        }
        return;
    }

    pa_protocol_command command;
    if (!pa_protocol_parse(protocol_line, original_length, &command)) {
        reply_error("COMMAND");
        return;
    }
    handle_import(&command);
}

static bool parse_arguments(int argc, char **argv)
{
    if (argc == 1) {
        return true;
    }
    if (argc == 3 && strcmp(argv[1], "--state") == 0 && argv[2][0] != '\0') {
        state_path = argv[2];
        return true;
    }
    fprintf(stderr, "用法：%s [--state PATH]\n", argv[0]);
    return false;
}

int main(int argc, char **argv)
{
    if (!parse_arguments(argc, argv)) {
        return 2;
    }
    clear_session();
    if (!load_state()) {
        fprintf(stderr, "ERR SNAPSHOT\n");
        return 1;
    }

    puts("AI Passport Anki 学习模拟器。输入 HELP 查看命令。");
    fflush(stdout);
    char line[PA_PROTOCOL_MAX_LINE + 1u];
    while (!quitting) {
        size_t length;
        input_result input = read_line(line, sizeof(line), &length);
        if (input == INPUT_END) {
            break;
        }
        if (input == INPUT_TOO_LONG) {
            reply_error("LINE");
            continue;
        }
        if (input == INPUT_INVALID) {
            reply_error("COMMAND");
            continue;
        }
        if (input == INPUT_ERROR) {
            fprintf(stderr, "读取输入失败。\n");
            return 1;
        }
        handle_line(line, length);
    }
    if (store.import_active) {
        if (take_backup()) {
            pa_store_import_abort(&store);
            if (!save_state()) {
                restore_backup();
            } else {
                free_backup();
            }
        }
    }
    free(cards_image);
    free(sched_image);
    free_backup();
    return 0;
}
