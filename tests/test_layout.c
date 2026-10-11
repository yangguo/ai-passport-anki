#include "../main/pa_layout.h"
#include <assert.h>
#include <string.h>

static uint16_t width(uint32_t cp, void *user) {
    (void)user;
    if (cp == 0x4e2d || cp == 0x6587) return 2;
    if (cp >= 0x20 && cp <= 0x7e) return 1;
    return 0;
}

int main(void) {
    const char text[] = "A\xe4\xb8\xad\xe6\x96\x87" "BC";
    pa_text_page pages[PA_LAYOUT_MAX_PAGES];
    size_t count = pa_layout_pages(text, strlen(text), 2, 2, width, NULL, pages);
    assert(count == 2);
    assert(pages[0].start == 0 && pages[0].length == 4);
    assert(pages[1].start == 4 && pages[1].length == 5);
    char rendered[32];
    assert(pa_layout_format_page(text, strlen(text), pages[0], 2, width, NULL,
                                 rendered, sizeof(rendered)));
    assert(strcmp(rendered, "A\n中") == 0);
    assert(pa_layout_format_page(text, strlen(text), pages[1], 2, width, NULL,
                                 rendered, sizeof(rendered)));
    assert(strcmp(rendered, "文\nBC") == 0);

    const char english[] = "ab cd";
    count = pa_layout_pages(english, strlen(english), 3, 2, width, NULL, pages);
    assert(count == 1);
    assert(pa_layout_format_page(english, strlen(english), pages[0], 3, width,
                                 NULL, rendered, sizeof(rendered)));
    assert(strcmp(rendered, "ab \ncd") == 0);

    const char invalid[] = { (char)0xe4, (char)0xb8, 0 };
    assert(pa_layout_pages(invalid, sizeof(invalid), 8, 2, width, NULL, pages) == 0);
    const char missing[] = "A\xf0\x9f\x99\x82";
    assert(pa_layout_pages(missing, sizeof(missing) - 1, 8, 2, width, NULL, pages) == 0);
    return 0;
}
