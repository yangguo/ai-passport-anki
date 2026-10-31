#ifndef PA_LAYOUT_H
#define PA_LAYOUT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PA_LAYOUT_MAX_PAGES 64
/* Study body on the 240x320 panel. pa_font_16 is 31 px per line, so six lines
 * fill the area under a one-line title and above a one-line footer. 204 is the
 * label width at x = 18 inside the rounded bezel. */
#define PA_STUDY_TEXT_WIDTH 204
#define PA_STUDY_TEXT_LINES 6
typedef uint16_t (*pa_glyph_width_fn)(uint32_t codepoint, void *user);
typedef struct { uint16_t start, length; } pa_text_page;

/* Split at UTF-8 boundaries according to rendered glyph widths and line count.
 * Returns zero for invalid UTF-8, missing glyphs, or insufficient page space. */
size_t pa_layout_pages(const char *text, size_t length, uint16_t width,
                       uint8_t lines_per_page, pa_glyph_width_fn glyph_width,
                       void *user, pa_text_page pages[PA_LAYOUT_MAX_PAGES]);
bool pa_layout_format_page(const char *text, size_t length, pa_text_page page,
                           uint16_t width, pa_glyph_width_fn glyph_width,
                           void *user, char *out, size_t out_capacity);

#endif
