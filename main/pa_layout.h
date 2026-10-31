#ifndef PA_LAYOUT_H
#define PA_LAYOUT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PA_LAYOUT_MAX_PAGES 64

/* Study screen on the 240×320 rounded panel.
 * Title/footer/chips use pa_font_16 (31 px line height). Card front/back use
 * pa_font_20 (38 px line height). Text width is in pixels; glyph widths for
 * layout use the same units as LVGL adv_w on the body font. Five body lines fit
 * between deck chrome and the action footer without crowding the grade chips. */
#define PA_STUDY_TITLE_LINE_PX  31
#define PA_STUDY_BODY_LINE_PX   38
#define PA_STUDY_MARGIN_X       12
#define PA_STUDY_TEXT_WIDTH     (240 - 2 * PA_STUDY_MARGIN_X)
#define PA_STUDY_TEXT_LINES     5

#define PA_STUDY_TOP_Y          10
#define PA_STUDY_TITLE_Y        PA_STUDY_TOP_Y
#define PA_STUDY_TITLE_H        PA_STUDY_TITLE_LINE_PX
#define PA_STUDY_BODY_Y         (PA_STUDY_TITLE_Y + PA_STUDY_TITLE_H + 4)
#define PA_STUDY_BODY_LINE_SP   2
#define PA_STUDY_BODY_H         (PA_STUDY_TEXT_LINES * PA_STUDY_BODY_LINE_PX + \
                                 (PA_STUDY_TEXT_LINES - 1) * PA_STUDY_BODY_LINE_SP)
#define PA_STUDY_FOOTER_GAP     6
#define PA_STUDY_FOOTER_H       54
#define PA_STUDY_FOOTER_Y       (PA_STUDY_BODY_Y + PA_STUDY_BODY_H + PA_STUDY_FOOTER_GAP)
#define PA_STUDY_GRADE_ROW_H    22
#define PA_STUDY_GRADE_PAD      4
#define PA_STUDY_GRADE_GAP      4

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
