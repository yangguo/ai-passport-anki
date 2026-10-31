/* The checked-in UI font is produced by lv_font_conv without --no-compress.
 * LVGL 9.5 treats bitmap_format 1/2 as RLE and returns no glyph bitmap unless
 * LV_USE_FONT_COMPRESSED is on. The deck screen then stays the dark background. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *read_file(const char *path)
{
    FILE *file = fopen(path, "rb");
    if (!file) {
        fprintf(stderr, "cannot open %s\n", path);
        return NULL;
    }
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return NULL;
    }
    long size = ftell(file);
    if (size < 0 || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return NULL;
    }
    char *data = malloc((size_t)size + 1u);
    if (!data) {
        fclose(file);
        return NULL;
    }
    if (fread(data, 1, (size_t)size, file) != (size_t)size) {
        free(data);
        fclose(file);
        return NULL;
    }
    data[size] = '\0';
    fclose(file);
    return data;
}

int main(void)
{
    char font_path[512];
    char config_path[512];
    snprintf(font_path, sizeof(font_path), "%s/assets/fonts/pa_font_16.c", PA_SOURCE_DIR);
    snprintf(config_path, sizeof(config_path), "%s/sdkconfig.defaults", PA_SOURCE_DIR);

    char *font = read_file(font_path);
    char *config = read_file(config_path);
    if (!font || !config) {
        free(font);
        free(config);
        return 1;
    }

    const char *format_field = strstr(font, ".bitmap_format = ");
    if (!format_field) {
        fprintf(stderr, "pa_font_16.c has no bitmap_format field\n");
        free(font);
        free(config);
        return 1;
    }
    int format = atoi(format_field + strlen(".bitmap_format = "));
    int compressed = format == 1 || format == 2;
    int decompressor_enabled = strstr(config, "CONFIG_LV_USE_FONT_COMPRESSED=y") != NULL;
    if (compressed && !decompressor_enabled) {
        fprintf(stderr,
                "pa_font_16 bitmap_format=%d needs CONFIG_LV_USE_FONT_COMPRESSED=y; "
                "without it LVGL draws no glyphs\n",
                format);
        free(font);
        free(config);
        return 1;
    }

    free(font);
    free(config);
    return 0;
}
