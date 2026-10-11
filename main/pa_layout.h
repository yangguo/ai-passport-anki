#ifndef PA_LAYOUT_H
#define PA_LAYOUT_H

#include <stddef.h>
#include <stdint.h>

#define PA_LAYOUT_MAX_PAGES 64
typedef uint16_t (*pa_glyph_width_fn)(uint32_t codepoint, void *user);
typedef struct { uint16_t start, length; } pa_text_page;

/* Split at UTF-8 boundaries according to rendered glyph widths and line count.
 * Returns zero for invalid UTF-8, missing glyphs, or insufficient page space. */
size_t pa_layout_pages(const char *text, size_t length, uint16_t width,
                       uint8_t lines_per_page, pa_glyph_width_fn glyph_width,
                       void *user, pa_text_page pages[PA_LAYOUT_MAX_PAGES]);

#endif
