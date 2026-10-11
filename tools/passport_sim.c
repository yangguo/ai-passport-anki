#define _POSIX_C_SOURCE 200809L

#include "pa_import.h"
#include "pa_protocol.h"
#include "passport_anki.h"

#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static pa_collection live;
static pa_collection staging;
static pa_session session;
static pa_import_tx import_tx;
static const char *state_path;
static char selected_deck[PA_DECK_BYTES];
static bool deck_selected;
static bool quitting;

typedef enum {
    INPUT_READY,
    INPUT_END,
    INPUT_TOO_LONG,
    INPUT_INVALID,
    INPUT_ERROR
} input_result;

static bool persist_collection(const pa_collection *collection, void *user)
{
    (void)user;
    if (state_path == NULL) {
        return true;
    }

    uint8_t snapshot[PA_SNAPSHOT_MAX];
    size_t size = pa_encode(collection, snapshot, sizeof(snapshot));
    if (size == 0) {
        return false;
    }
    size_t path_length = strlen(state_path);
    if (path_length > SIZE_MAX - sizeof(".tmp.XXXXXX")) {
        return false;
    }
    char *temporary_path = malloc(path_length + sizeof(".tmp.XXXXXX"));
    if (temporary_path == NULL) {
        return false;
    }
    (void)snprintf(temporary_path, path_length + sizeof(".tmp.XXXXXX"),
                   "%s.tmp.XXXXXX", state_path);
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

    bool ok = fwrite(snapshot, 1, size, file) == size;
    if (fflush(file) != 0) {
        ok = false;
    }
    if (fsync(fileno(file)) != 0) {
        ok = false;
    }
    if (fclose(file) != 0) {
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

static bool load_snapshot(void)
{
    if (state_path == NULL) {
        pa_init(&live);
        return true;
    }
    FILE *file = fopen(state_path, "rb");
    if (file == NULL) {
        if (errno == ENOENT) {
            pa_init(&live);
            return true;
        }
        return false;
    }

    uint8_t snapshot[PA_SNAPSHOT_MAX];
    size_t size = fread(snapshot, 1, sizeof(snapshot), file);
    bool too_large = !feof(file);
    bool read_ok = !ferror(file);
    bool close_ok = fclose(file) == 0;
    if (!read_ok || !close_ok || too_large ||
        !pa_decode(&staging, snapshot, size)) {
        return false;
    }
    live = staging;
    return true;
}

static void reply_error(const char *code)
{
    printf("ERR %s\n", code);
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
    for (uint16_t i = 0; i < live.count; ++i) {
        if (strcmp(live.cards[i].deck, name) == 0) {
            return true;
        }
    }
    return false;
}

static void handle_import(const pa_protocol_command *command)
{
    switch (command->kind) {
    case PA_PROTOCOL_INFO:
        printf("OK INFO version=1 cards=%u day=%" PRIu32 "\n",
               live.count, live.now / 1440u);
        fflush(stdout);
        break;
    case PA_PROTOCOL_BEGIN:
        if (import_tx.active || session.current != PA_NO_CARD) {
            reply_error("TRANSACTION");
        } else if (!pa_import_begin(&import_tx, &staging, &live,
                                    (uint16_t)command->value)) {
            reply_error("TRANSACTION");
        } else {
            printf("OK BEGIN\n");
            fflush(stdout);
        }
        break;
    case PA_PROTOCOL_CARD:
        if (!import_tx.active || import_tx.invalid) {
            reply_error("TRANSACTION");
        } else if (!pa_import_add(&import_tx, &staging, command->id,
                                  command->deck, command->front, command->back,
                                  NULL, NULL)) {
            reply_error("CARD");
        } else {
            printf("OK CARD\n");
            fflush(stdout);
        }
        break;
    case PA_PROTOCOL_COMMIT:
        if (!import_tx.active || import_tx.invalid ||
            import_tx.received != import_tx.expected) {
            reply_error("TRANSACTION");
        } else if (!pa_import_commit(&import_tx, &staging, &live,
                                     persist_collection, NULL)) {
            reply_error("STORAGE");
        } else {
            deck_selected = false;
            session.current = PA_NO_CARD;
            session.revealed = false;
            printf("OK COMMIT\n");
            fflush(stdout);
        }
        break;
    case PA_PROTOCOL_ABORT:
        pa_import_abort(&import_tx);
        printf("OK ABORT\n");
        fflush(stdout);
        break;
    case PA_PROTOCOL_DAY: {
        uint32_t day = command->value;
        if (import_tx.active || session.current != PA_NO_CARD ||
            day < live.now / 1440u || day > UINT32_MAX / 1440u) {
            reply_error("DAY");
            break;
        }
        staging = live;
        uint32_t target = day * 1440u;
        if ((target > staging.now && !pa_advance(&staging, target - staging.now)) ||
            !persist_collection(&staging, NULL)) {
            reply_error("STORAGE");
            break;
        }
        live = staging;
        printf("OK DAY\n");
        fflush(stdout);
        break;
    }
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
        puts("导入协议：INFO, BEGIN n, CARD id deck_hex front_hex back_hex, COMMIT, ABORT, DAY n");
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
        while (length > 0 && (arguments[length - 1] == ' ' ||
                              arguments[length - 1] == '\t')) {
            arguments[--length] = '\0';
        }
        if (import_tx.active) {
            reply_error("TRANSACTION");
        } else if (length == 0 || length >= sizeof(selected_deck) ||
                   !pa_text_valid(arguments, sizeof(selected_deck))) {
            reply_error("DECK");
        } else if (!collection_has_deck(arguments)) {
            reply_error("DECK");
        } else {
            memcpy(selected_deck, arguments, length + 1);
            deck_selected = true;
            session.current = PA_NO_CARD;
            session.revealed = false;
            printf("已选择牌组：%s\n", selected_deck);
            fflush(stdout);
        }
        return;
    }
    if (strcmp(line, "NEXT") == 0 && no_arguments(arguments)) {
        if (import_tx.active) {
            reply_error("TRANSACTION");
        } else if (!deck_selected) {
            reply_error("DECK_REQUIRED");
        } else {
            if (session.current == PA_NO_CARD) {
                pa_begin(&session, &live, selected_deck);
            }
            if (session.current == PA_NO_CARD) {
                puts("今天没有到期卡片。");
            } else {
                const pa_card *card = &live.cards[session.current];
                printf("正面：%s\n", card->front);
            }
            fflush(stdout);
        }
        return;
    }
    if (strcmp(line, "FLIP") == 0 && no_arguments(arguments)) {
        if (import_tx.active) {
            reply_error("TRANSACTION");
        } else if (session.current == PA_NO_CARD) {
            reply_error("NO_CARD");
        } else if (!pa_flip(&session)) {
            reply_error("ALREADY_FLIPPED");
        } else {
            printf("背面：%s\n", live.cards[session.current].back);
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
        if (import_tx.active) {
            reply_error("TRANSACTION");
        } else if (session.current == PA_NO_CARD) {
            reply_error("NO_CARD");
        } else if (!session.revealed) {
            reply_error("FLIP_REQUIRED");
        } else {
            static const char *display_names[] = {"Again", "Hard", "Good", "Easy"};
            pa_collection candidate = live;
            pa_session candidate_session = session;
            if (!pa_grade(&candidate, &candidate_session, rating)) {
                reply_error("GRADE");
            } else if (!persist_collection(&candidate, NULL)) {
                reply_error("STORAGE");
            } else {
                live = candidate;
                session = candidate_session;
                printf("已评分：%s\n", display_names[rating]);
                fflush(stdout);
            }
        }
        return;
    }
    if (strcmp(line, "ADVANCE") == 0) {
        uint32_t minutes;
        if (!parse_u32(arguments, &minutes)) {
            reply_error("MINUTES");
        } else if (import_tx.active || session.current != PA_NO_CARD) {
            reply_error("TRANSACTION");
        } else {
            staging = live;
            if (!pa_advance(&staging, minutes)) {
                reply_error("TIME");
            } else if (!persist_collection(&staging, NULL)) {
                reply_error("STORAGE");
            } else {
                live = staging;
                printf("OK ADVANCE now=%" PRIu32 "\n", live.now);
                fflush(stdout);
            }
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
    session.current = PA_NO_CARD;
    session.revealed = false;
    pa_import_abort(&import_tx);
    if (!load_snapshot()) {
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
    if (import_tx.active) {
        pa_import_abort(&import_tx);
    }
    return 0;
}
