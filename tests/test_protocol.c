#include "../main/pa_protocol.h"

#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

static pa_protocol_command command;
#define REJECTS(text) rejects_unchanged((text), sizeof(text) - 1)

static void rejects_unchanged(const char *line, size_t length)
{
    pa_protocol_command before;
    memset(&command, 0xa5, sizeof(command));
    before = command;
    assert(!pa_protocol_parse(line, length, &command));
    assert(memcmp(&command, &before, sizeof(command)) == 0);
}

int main(void)
{
    assert(pa_protocol_parse("INFO\r\n", 6, &command));
    assert(command.kind == PA_PROTOCOL_INFO);
    assert(pa_protocol_parse("BEGIN 64 0 704\n", 15, &command));
    assert(command.kind == PA_PROTOCOL_BEGIN && command.value == 64 && command.crc == 0 && command.length == 704);
    assert(pa_protocol_parse("BEGIN 12000 99 132000", 21, &command));
    assert(command.kind == PA_PROTOCOL_BEGIN && command.value == 12000 && command.crc == 99);
    assert(pa_protocol_parse("RESUME\n", 7, &command) && command.kind == PA_PROTOCOL_RESUME);
    assert(pa_protocol_parse("CHUNK 0 010000000101000100445141", 32, &command));
    assert(command.kind == PA_PROTOCOL_CHUNK && command.value == 0 && command.length == 12);
    assert(memcmp(command.chunk, "\x01\x00\x00\x00\x01\x01\x00\x01\x00\x44\x51\x41", 12) == 0);
    assert(pa_protocol_parse("CHUNK 4 00ff", 12, &command));
    assert(command.length == 2 && command.chunk[0] == 0 && command.chunk[1] == 0xff);
    assert(pa_protocol_parse("DAY 4294967295", 14, &command));
    assert(command.kind == PA_PROTOCOL_DAY && command.value == UINT32_MAX);
    const char *card_line = " CARD\t7\tE8AFADE8A880\tE4BDA0E5A5BD\t68690A7468657265 \r\n";
    assert(pa_protocol_parse(card_line, strlen(card_line), &command));
    assert(command.kind == PA_PROTOCOL_CARD && command.id == 7);
    assert(strcmp(command.deck, "语言") == 0);
    assert(strcmp(command.front, "你好") == 0);
    assert(strcmp(command.back, "hi\nthere") == 0);
    assert(pa_protocol_parse("COMMIT", 6, &command) && command.kind == PA_PROTOCOL_COMMIT);
    assert(pa_protocol_parse("ABORT", 5, &command) && command.kind == PA_PROTOCOL_ABORT);
    assert(pa_protocol_parse("EXPORT\n", 7, &command) && command.kind == PA_PROTOCOL_EXPORT);
    assert(pa_protocol_parse("XRESUME", 7, &command) && command.kind == PA_PROTOCOL_XRESUME);
    assert(pa_protocol_parse("XCHUNK 12", 9, &command));
    assert(command.kind == PA_PROTOCOL_XCHUNK && command.value == 12);

    REJECTS("BEGIN 0");
    REJECTS("BEGIN 64");
    REJECTS("BEGIN 12001 0 132011");
    REJECTS("BEGIN 1 0 10");
    REJECTS("BEGIN 4294967296 0 11");
    REJECTS("CHUNK 0 0");
    REJECTS("CHUNK 0 gg");
    char huge_chunk[1100];
    memset(huge_chunk, 'A', sizeof(huge_chunk));
    memcpy(huge_chunk, "CHUNK 0 ", 8);
    huge_chunk[8 + 962] = '\0';
    REJECTS(huge_chunk);
    REJECTS("DAY -1");
    REJECTS("DAY 12x");
    REJECTS("INFO trailing");
    REJECTS("INFO\nINFO");
    REJECTS("CARD 0 44 61 62");
    REJECTS("CARD 1 4 61 62");
    REJECTS("CARD 1 4g 61 62");
    REJECTS("CARD 1 00 61 62");
    REJECTS("CARD 1 e2 82 61");
    REJECTS("CARD 1 c0af 61 62");
    REJECTS("CARD 1 eda080 61 62");
    REJECTS("XCHUNK");
    REJECTS("EXPORT now");
    REJECTS("UNKNOWN");
    rejects_unchanged(NULL, 0);

    char embedded_nul[] = {'I', 'N', 'F', 'O', '\0', 'X'};
    rejects_unchanged(embedded_nul, sizeof(embedded_nul));
    char oversized[PA_PROTOCOL_MAX_LINE + 1];
    memset(oversized, ' ', sizeof(oversized));
    rejects_unchanged(oversized, sizeof(oversized));
    char boundary_card[1200] = "CARD 1 ";
    size_t position = strlen(boundary_card);
    for (size_t i = 0; i < 47; ++i) {
        boundary_card[position++] = '4';boundary_card[position++] = '1';
    }
    memcpy(boundary_card + position, " 61 62", 7);position += 6;
    assert(pa_protocol_parse(boundary_card, position, &command));
    assert(strlen(command.deck) == 47);
    boundary_card[101] = '4';boundary_card[102] = '1';
    boundary_card[103] = ' ';boundary_card[104] = '6';
    boundary_card[105] = '1';boundary_card[106] = ' ';
    boundary_card[107] = '6';boundary_card[108] = '2';
    rejects_unchanged(boundary_card, 109);
    puts("protocol: framing, token counts, numeric bounds, hex and atomic failure PASS");
    return 0;
}
