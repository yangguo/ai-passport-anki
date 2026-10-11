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
    assert(pa_protocol_parse("BEGIN 64\n", 9, &command));
    assert(command.kind == PA_PROTOCOL_BEGIN && command.value == 64);
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

    REJECTS("BEGIN 0");
    REJECTS("BEGIN 65");
    REJECTS("BEGIN 4294967296");
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
