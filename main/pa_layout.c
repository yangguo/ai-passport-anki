#include "pa_layout.h"

static int next_utf8(const unsigned char *s, size_t n, size_t *at, uint32_t *out) {
    if (*at >= n) return 0;
    const size_t i = *at;
    const unsigned char a = s[i];
    uint32_t c;
    size_t k;
    if (a < 0x80) { c = a; k = 1; }
    else if (a >= 0xc2 && a <= 0xdf) { c = a & 0x1f; k = 2; }
    else if (a >= 0xe0 && a <= 0xef) { c = a & 0x0f; k = 3; }
    else if (a >= 0xf0 && a <= 0xf4) { c = a & 7; k = 4; }
    else return -1;
    if (i + k > n) return -1;
    for (size_t j = 1; j < k; ++j) {
        if ((s[i + j] & 0xc0) != 0x80) return -1;
        c = (c << 6) | (s[i + j] & 0x3f);
    }
    if ((k == 3 && c < 0x800) || (k == 4 && c < 0x10000) ||
        (c >= 0xd800 && c <= 0xdfff) || c > 0x10ffff) return -1;
    *at += k;
    *out = c;
    return 1;
}

size_t pa_layout_pages(const char *text, size_t length, uint16_t width,
                       uint8_t lines_per_page, pa_glyph_width_fn glyph_width,
                       void *user, pa_text_page pages[PA_LAYOUT_MAX_PAGES]) {
    if (!text || !length || !width || !lines_per_page || !glyph_width || !pages) return 0;
    size_t page_count = 0, page_start = 0, at = 0;
    uint32_t line_width = 0;
    uint8_t lines = 1;
    while (at < length) {
        const size_t char_start = at;
        uint32_t cp;
        if (next_utf8((const unsigned char *)text, length, &at, &cp) <= 0) return 0;
        uint16_t glyph = cp == '\n' ? 0 : glyph_width(cp, user);
        if (cp != '\n' && glyph == 0) return 0;
        if (cp == '\n' || (line_width && line_width + glyph > width)) {
            if (cp == '\n') at = char_start + 1;
            ++lines;
            line_width = 0;
            if (lines > lines_per_page) {
                if (page_count >= PA_LAYOUT_MAX_PAGES) return 0;
                pages[page_count++] = (pa_text_page){(uint16_t)page_start,
                                                     (uint16_t)(char_start - page_start)};
                page_start = char_start;
                lines = 1;
                if (cp == '\n') { page_start = at; continue; }
                /* The glyph that wrapped starts the next line/page. */
                at = char_start;
                continue;
            }
            if (cp == '\n') continue;
        }
        if (cp != '\n') line_width += glyph;
    }
    if (page_count >= PA_LAYOUT_MAX_PAGES) return 0;
    pages[page_count++] = (pa_text_page){(uint16_t)page_start,
                                         (uint16_t)(length - page_start)};
    return page_count;
}
