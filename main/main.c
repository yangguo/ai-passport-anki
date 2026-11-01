#include "bsp_battery.h"
#include "bsp_button.h"
#include "bsp_display.h"
#include "bsp_power.h"
#include "bsp_pins.h"
#include "esp_flash_internal.h"
#include "esp_image_format.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "launcher_contract.h"
#include "lvgl.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "pa_cover.h"
#include "pa_layout.h"
#include "pa_protocol.h"
#include "pa_store.h"
#include "pa_tail.h"
#include "passport_anki.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PA_LINE_MAX 1200
#define IMPORT_TIMEOUT_US (60LL * 1000 * 1000)
#define MINUTE_US (60LL * 1000 * 1000)
#define AUTOPLAY_MAX_WAIT_S (10u * 60u)

extern const lv_font_t pa_font_16;
extern const lv_font_t pa_font_20;
static const char *TAG = "anki";
static pa_store store;
static pa_flash cards_flash;
static pa_flash sched_flash;
static pa_session session;
static nvs_handle_t nvs;
static bool nvs_ready, storage_ready, storage_corrupt;
static int64_t import_last_input, last_checkpoint;
static int64_t last_clock_us;
static char card_deck[PA_DECK_BYTES];
static char card_front[PA_STORE_TEXT_MAX + 1u];
static char card_back[PA_STORE_TEXT_MAX + 1u];
static uint32_t glyph_checks;

typedef enum { UI_DECKS, UI_QUESTION, UI_ANSWER, UI_RATING, UI_EMPTY,
               UI_CONFIRM_DAY, UI_IMPORTING, UI_ERROR } ui_state;
_Static_assert((int)UI_DECKS == PA_COVER_UI_DECKS, "deck list is the cover gate");
_Static_assert((int)BSP_BTN_UP == PA_COVER_BTN_UP, "cover gate uses UP");
_Static_assert((int)BSP_BTN_LONG == PA_COVER_BTN_LONG, "cover gate uses long press");
typedef enum { EVENT_BUTTON, EVENT_LINE, EVENT_TICK, EVENT_IDLE } event_type;
typedef struct {
    event_type type;
    bsp_btn_t button;
    bsp_btn_ev_t button_event;
    char line[PA_LINE_MAX + 2];
} event_t;
static QueueHandle_t events;
static event_t button_event_storage = {.type = EVENT_BUTTON};
static event_t tick_event_storage = {.type = EVENT_TICK};
static event_t idle_event_storage = {.type = EVENT_IDLE};
static ui_state ui = UI_DECKS;
static pa_rating selected_rating = PA_GOOD;
static uint16_t deck_index, question_page, answer_page;
static bool study_all = false;
static uint16_t study_deck = 0;
static bool confirming_day, grade_save_failed;
static const char *last_error = "";
static uint8_t setting_brightness = 70;
static uint8_t setting_sleep_min = 3;
static uint8_t setting_autopage_sec = 0;
static uint8_t setting_new_limit = 20;
static uint8_t setting_shuffle = 0;
static uint8_t setting_autoplay = 0;
static uint16_t setting_new_done = 0;
static uint32_t setting_new_day = 0;
static bool autoplay_active = false;
static int64_t autoplay_empty_since_us = 0;
static int64_t page_shown_us;
static bool battery_usable = false;
static bool screen_dimmed = false;
static int64_t last_activity_us, dim_start_us, last_usb_line_us;
static lv_obj_t *title_label, *body_label, *footer_label;
static lv_obj_t *header_rule, *footer_panel, *grade_chip[4], *grade_chip_label[4];

static bool part_read(void *ctx, uint32_t offset, void *dst, uint32_t length)
{
    const esp_partition_t *part = ctx;
    return esp_partition_read(part, offset, dst, length) == ESP_OK;
}

static bool part_write(void *ctx, uint32_t offset, const void *src, uint32_t length)
{
    const esp_partition_t *part = ctx;
    uint8_t tail[16];
    uint32_t aligned = length & ~15u;
    if (aligned != 0 && esp_partition_write(part, offset, src, aligned) != ESP_OK) {
        return false;
    }
    uint32_t rest = length - aligned;
    if (rest == 0) {
        return true;
    }
    memset(tail, 0xff, sizeof(tail));
    memcpy(tail, (const uint8_t *)src + aligned, rest);
    return esp_partition_write(part, offset + aligned, tail, sizeof(tail)) == ESP_OK;
}

static bool part_erase(void *ctx, uint32_t offset, uint32_t length)
{
    return esp_partition_erase_range(ctx, offset, length) == ESP_OK;
}

typedef struct {
    const esp_partition_t *part;
    uint32_t base;
    uint32_t size;
} pa_part_win;

static pa_part_win cards_win;
static pa_part_win sched_win;
static pa_tail_bounds tail_bounds;

static bool tail_set_protect(void *ctx, bool protect)
{
    const esp_partition_t *part = ctx;
    if (part == NULL || part->flash_chip == NULL) {
        return false;
    }
    return esp_flash_set_dangerous_write_protection(part->flash_chip, protect) == ESP_OK;
}

static bool part_erase_io(void *ctx, uint32_t offset, uint32_t length, const void *src)
{
    (void)src;
    return part_erase(ctx, offset, length);
}

static bool part_write_io(void *ctx, uint32_t offset, uint32_t length, const void *src)
{
    return part_write(ctx, offset, src, length);
}

static bool win_span(const pa_part_win *win, uint32_t offset, uint32_t span, uint32_t *absolute)
{
    if (win == NULL || win->part == NULL || absolute == NULL || span == 0 || win->size == 0 ||
        offset > win->size || span > win->size - offset || offset > UINT32_MAX - win->base) {
        return false;
    }
    *absolute = win->base + offset;
    return true;
}

static bool win_read(void *ctx, uint32_t offset, void *dst, uint32_t length)
{
    pa_part_win *win = ctx;
    uint32_t absolute = 0;
    if (dst == NULL || !win_span(win, offset, length, &absolute)) {
        return false;
    }
    return part_read((void *)win->part, absolute, dst, length);
}

/* The tail is inside the running app partition. A direct erase or write
 * calls abort() while IDF dangerous-write protection is on, which is the
 * default. These two wrappers allow only the post-image span and drop that
 * protection for the call. */
static bool win_write(void *ctx, uint32_t offset, const void *src, uint32_t length)
{
    pa_part_win *win = ctx;
    uint32_t absolute = 0;
    uint32_t span = pa_tail_programmed_length(length);
    if (src == NULL || !win_span(win, offset, span, &absolute)) {
        return false;
    }
    return pa_tail_protected_io(&tail_bounds, tail_set_protect, (void *)win->part, absolute, length, src,
                                true, part_write_io, (void *)win->part);
}

static bool win_erase(void *ctx, uint32_t offset, uint32_t length)
{
    pa_part_win *win = ctx;
    uint32_t absolute = 0;
    if (!win_span(win, offset, length, &absolute)) {
        return false;
    }
    return pa_tail_protected_io(&tail_bounds, tail_set_protect, (void *)win->part, absolute, length, NULL,
                                false, part_erase_io, (void *)win->part);
}

static bool sidecar_magic(const esp_partition_t *part, uint32_t offset)
{
    char magic[4];
    if (offset > part->size || 4u > part->size - offset) {
        return false;
    }
    return esp_partition_read(part, offset, magic, sizeof(magic)) == ESP_OK &&
           memcmp(magic, "DPS1", 4) == 0;
}

static bool running_is_factory(const esp_partition_t *part)
{
    return part->type == ESP_PARTITION_TYPE_APP &&
           part->subtype == ESP_PARTITION_SUBTYPE_APP_FACTORY &&
           strcmp(part->label, "factory") == 0;
}

/* Card bytes follow the verified image. Recognized slot size or a DPS1
 * sidecar keeps the last 64 KiB. Only factory may use the rest of the
 * partition. Any other size is refused so an import cannot erase a sidecar
 * or the gap. Addresses are not logged. */
static bool open_tail_storage(void)
{
    const esp_partition_t *part = esp_ota_get_running_partition();
    esp_partition_pos_t pos;
    esp_image_metadata_t meta;
    uint32_t image_len, data_start, data_end, data_size, cards_bytes, sched_bytes;
    uint32_t bank_a, bank_b;
    bool sidecar = false;
    memset(&tail_bounds, 0, sizeof(tail_bounds));
    if (part == NULL) {
        return false;
    }
    pos.offset = part->address;
    pos.size = part->size;
    if (esp_image_verify(ESP_IMAGE_VERIFY_SILENT, &pos, &meta) != ESP_OK || meta.image_len == 0 ||
        meta.image_len > part->size) {
        return false;
    }
    image_len = meta.image_len;
    if (pa_tail_sidecar_banks(part->size, image_len, &bank_a, &bank_b)) {
        sidecar = sidecar_magic(part, bank_a) || sidecar_magic(part, bank_b);
    }
    if (!pa_tail_data_end(part->size, image_len, running_is_factory(part), sidecar, &data_start,
                          &data_end)) {
        ESP_LOGE(TAG, "card storage refused; partition is not a slot, DPS1 sidecar, or factory");
        return false;
    }
    data_size = data_end - data_start;
    if (!pa_region_plan(data_size, &cards_bytes, &sched_bytes) ||
        cards_bytes > data_size || sched_bytes > data_size - cards_bytes) {
        return false;
    }
    tail_bounds = (pa_tail_bounds){
        .part_size = part->size,
        .image_len = image_len,
        .data_start = data_start,
        .data_end = data_end,
    };
    cards_win = (pa_part_win){.part = part, .base = data_start, .size = cards_bytes};
    sched_win = (pa_part_win){.part = part, .base = data_start + cards_bytes, .size = sched_bytes};
    cards_flash = (pa_flash){.ctx = &cards_win, .size = cards_bytes,
                             .read = win_read, .write = win_write, .erase = win_erase};
    sched_flash = (pa_flash){.ctx = &sched_win, .size = sched_bytes,
                             .read = win_read, .write = win_write, .erase = win_erase};
    return true;
}

static uint16_t glyph_width(uint32_t cp, void *user) {
    (void)user;
    lv_font_glyph_dsc_t glyph;
    if (!lv_font_get_glyph_dsc(&pa_font_20, &glyph, cp, 0) || glyph.is_placeholder) {
        return 0;
    }
    return glyph.adv_w;
}

static bool text_fits(const char *text) {
    pa_text_page pages[PA_LAYOUT_MAX_PAGES];
    return pa_layout_pages(text, strlen(text), PA_STUDY_TEXT_WIDTH, PA_STUDY_TEXT_LINES,
                           glyph_width, NULL, pages) != 0;
}

static bool text_supported(const char *deck, const char *front, const char *back, void *user) {
    (void)deck;
    (void)user;
    if ((++glyph_checks & 127u) == 0) {
        vTaskDelay(1);
    }
    /* Deck titles use pa_font_16; only study card front/back use pa_font_20. */
    return text_fits(front) && text_fits(back);
}

static size_t pages_for(const char *text, pa_text_page pages[PA_LAYOUT_MAX_PAGES]) {
    return pa_layout_pages(text, strlen(text), PA_STUDY_TEXT_WIDTH, PA_STUDY_TEXT_LINES,
                           glyph_width, NULL, pages);
}

static uint16_t deck_count(void) { return pa_store_deck_count(&store); }

/* Deck list: stats summary, decks..., then "next day", brightness, sleep,
 * auto-page, battery, new-card limit, shuffle, forecast, autoplay rows. */
#define DECK_EXTRA_ROWS 9u

static uint16_t deck_entries(void)
{
    uint16_t count = deck_count();
    /* Index 0 is the stats summary when decks exist (the empty message when
     * none); decks sit at 1..count, then the DECK_EXTRA_ROWS setting rows. */
    return count > 0 ? (uint16_t)(count + 1u + DECK_EXTRA_ROWS)
                     : (uint16_t)(DECK_EXTRA_ROWS + 1u);
}

static void clamp_deck_index(void) {
    uint16_t entries = deck_entries();
    if (entries == 0 || deck_index > entries - 1u) {
        deck_index = entries ? (uint16_t)(entries - 1u) : 0;
    }
    uint16_t count = deck_count();
    if (count > 0 && study_deck >= count) {
        study_deck = 0;
        study_all = true;
    }
}

static uint8_t load_u8_setting(const char *key, uint8_t fallback, const uint8_t *allowed,
                               size_t allowed_count)
{
    uint8_t value = fallback;
    if (nvs_ready && key != NULL && nvs_get_u8(nvs, key, &value) == ESP_OK) {
        for (size_t i = 0; i < allowed_count; ++i) {
            if (value == allowed[i]) return value;
        }
    }
    return fallback;
}

static void save_u8_setting(const char *key, uint8_t value)
{
    if (nvs_ready && key != NULL && nvs_set_u8(nvs, key, value) == ESP_OK) {
        (void)nvs_commit(nvs);
    }
}

static uint32_t load_u32_setting(const char *key, uint32_t fallback)
{
    uint32_t value = fallback;
    if (nvs_ready && key != NULL && nvs_get_u32(nvs, key, &value) == ESP_OK) {
        return value;
    }
    return fallback;
}

static void save_u32_setting(const char *key, uint32_t value)
{
    if (nvs_ready && key != NULL && nvs_set_u32(nvs, key, value) == ESP_OK) {
        (void)nvs_commit(nvs);
    }
}

static const char *brightness_label(uint8_t level)
{
    switch (level) {
    case 30: return "暗";
    case 100: return "亮";
    default: return "中";
    }
}

static const char *autopage_label(uint8_t sec)
{
    switch (sec) {
    case 3: return "3秒";
    case 5: return "5秒";
    case 10: return "10秒";
    default: return "关";
    }
}

static const char *new_limit_label(uint8_t limit)
{
    switch (limit) {
    case 0: return "不限";
    case 10: return "10张";
    case 50: return "50张";
    default: return "20张";
    }
}

static uint32_t grade_color(pa_rating rating) {
    static const uint32_t colors[] = {0xE05A5A, 0xE0A15A, 0x5EBE7A, 0x6AA6FF};
    return rating <= PA_EASY ? colors[rating] : colors[PA_GOOD];
}

static bool load_current_card(void) {
    return pa_store_read(&store, session.current, card_deck, sizeof(card_deck), card_front,
                         sizeof(card_front), card_back, sizeof(card_back));
}

static bool study_card_ui(void) {
    return ui == UI_QUESTION || ui == UI_ANSWER || ui == UI_RATING;
}

static void set_obj_hidden(lv_obj_t *obj, bool hidden) {
    if (!obj) return;
    if (hidden) {
        lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_clear_flag(obj, LV_OBJ_FLAG_HIDDEN);
    }
}

static void sync_study_chrome(void) {
    const bool card = study_card_ui();
    set_obj_hidden(header_rule, !card);
    set_obj_hidden(footer_panel, !card);
    for (int i = 0; i < 4; ++i) {
        set_obj_hidden(grade_chip[i], !card || ui != UI_RATING || grade_save_failed);
    }
    set_obj_hidden(footer_label, card && ui == UI_RATING && !grade_save_failed);
    if (card) {
        for (int i = 0; i < 4; ++i) {
            const bool selected = (pa_rating)i == selected_rating;
            lv_obj_set_style_bg_color(grade_chip[i], lv_color_hex(selected ? grade_color((pa_rating)i)
                                                                           : 0x1E2630),
                                      0);
            lv_obj_set_style_border_color(grade_chip[i],
                                          lv_color_hex(selected ? grade_color((pa_rating)i)
                                                                : 0x3A4552),
                                          0);
            lv_obj_set_style_border_width(grade_chip[i], selected ? 2 : 1, 0);
            lv_obj_set_style_text_color(grade_chip_label[i],
                                        lv_color_hex(selected ? 0xFFFFFF : 0xC8D0DA), 0);
        }
    }
    /* Official card CSS is text-align:left; centering wasted the margins and
     * left one-glyph lines (pitch marks) floating mid-panel. */
    lv_obj_set_style_text_align(body_label, LV_TEXT_ALIGN_LEFT, 0);
    if (footer_panel) {
        lv_obj_set_style_bg_opa(footer_panel, card ? LV_OPA_70 : LV_OPA_TRANSP, 0);
    }
}

/* Forward declarations: defined before do_grade, called from handle_line. */
static void study_day_sync(void);
static void study_day_reset(void);
static bool study_has_due(void);
static bool autoplay_scope_earliest(uint32_t *due_out);

/* Remaining today in scope: (LEARNING+REVIEW due) + NEW within budget. */
static void study_remaining(bool all, uint16_t deck, uint16_t *x_out, uint16_t *y_out)
{
    if (x_out != NULL) {
        *x_out = 0;
    }
    if (y_out != NULL) {
        *y_out = 0;
    }
    if (!storage_ready || storage_corrupt) {
        return;
    }
    static uint16_t due[PA_STORE_MAX_DECKS];
    static uint16_t ndue[PA_STORE_MAX_DECKS];
    uint16_t count = deck_count();
    pa_store_due_counts(&store, due);
    pa_store_new_due_counts(&store, ndue);
    uint32_t total = 0, ntotal = 0;
    if (all) {
        for (uint16_t i = 0; i < count; ++i) {
            total += due[i];
            ntotal += ndue[i];
        }
    } else if (deck < count) {
        total = due[deck];
        ntotal = ndue[deck];
    }
    uint32_t left = ntotal;
    if (setting_new_limit != 0) {
        uint32_t budget = setting_new_done >= setting_new_limit
                              ? 0u
                              : (uint32_t)setting_new_limit - setting_new_done;
        if (left > budget) {
            left = budget;
        }
    }
    uint32_t x = total >= ntotal ? total - ntotal + left : left;
    if (x > UINT16_MAX) {
        x = UINT16_MAX;
    }
    if (left > UINT16_MAX) {
        left = UINT16_MAX;
    }
    if (x_out != NULL) {
        *x_out = (uint16_t)x;
    }
    if (y_out != NULL) {
        *y_out = (uint16_t)left;
    }
}

static void render(void) {
    if (!title_label || !body_label || !footer_label || !bsp_lvgl_lock(100)) return;
    char title[96], body[PA_LINE_MAX + 1], footer[160];
    uint32_t day = pa_store_now(&store) / 1440U;
    uint32_t title_color = 0x8B95A1;
    uint32_t body_color = 0xF4F1EA;
    uint32_t footer_color = 0x9AA3AD;
    bool body_wrap = true;
    snprintf(title, sizeof(title), "Anki");
    body[0] = '\0';
    footer[0] = '\0';
    uint16_t count = deck_count();
    if (ui == UI_DECKS) {
        static uint16_t due[PA_STORE_MAX_DECKS];
        if (deck_index == 0 && count > 0) {
            uint32_t by_rating[4] = {0, 0, 0, 0}, today_total = 0;
            pa_store_today_stats(&store, by_rating, &today_total);
            uint16_t remain_x = 0, remain_y = 0, forecast[3] = {0, 0, 0};
            study_remaining(true, 0, &remain_x, &remain_y);
            pa_store_forecast(&store, forecast);
            snprintf(body, sizeof(body), "今日已评 %" PRIu32 "（%u/%u/%u/%u）\n待学 %u（含新%u） · %u 牌组\n明%u/后%u/大后%u\n学习日 %" PRIu32,
                     today_total, (unsigned)by_rating[0], (unsigned)by_rating[1],
                     (unsigned)by_rating[2], (unsigned)by_rating[3],
                     remain_x, remain_y, count,
                     (unsigned)forecast[0], (unsigned)forecast[1], (unsigned)forecast[2], day);
            snprintf(footer, sizeof(footer), "上下翻页 OK 进牌组");
        } else {
            const uint16_t extra = deck_index >= count + 1u ? (uint16_t)(deck_index - count - 1u)
                                                            : 0u;
            if (count == 0 && deck_index == 0) {
                snprintf(body, sizeof(body), "还没有牌组\n通过 USB 导入 TSV\n\n学习日 %" PRIu32, day);
                snprintf(footer, sizeof(footer), "OK 推进学习日");
            } else if (deck_index - 1u < count) {
                const uint16_t shown = (uint16_t)(deck_index - 1u);
                pa_store_due_counts(&store, due);
                snprintf(body, sizeof(body), "%s·到期%u\n%u / %u\n学习日 %" PRIu32,
                         shown < count ? pa_store_deck_name(&store, shown) : "还没有牌组",
                         (unsigned)due[shown], (unsigned)(shown + 1u), count, day);
                snprintf(footer, sizeof(footer), "OK 开始");
            } else if (extra == 0u) {
                snprintf(body, sizeof(body), "推进到下一学习日？\n\n当前学习日 %" PRIu32, day);
                snprintf(footer, sizeof(footer), "OK 确认推进");
            } else if (extra == 1u) {
                snprintf(body, sizeof(body), "亮度：%s\n\nOK 切换 暗/中/亮\n当前 %" PRIu32 "%%",
                         brightness_label(setting_brightness), (uint32_t)setting_brightness);
                snprintf(footer, sizeof(footer), "OK 切换");
            } else if (extra == 2u) {
                snprintf(body, sizeof(body), "休眠：%" PRIu32 " 分钟无操作熄屏\n\nOK 切换 1/3/5 分钟\n熄屏 5 分钟后深睡",
                         (uint32_t)setting_sleep_min);
                snprintf(footer, sizeof(footer), "OK 切换");
            } else if (extra == 3u) {
                snprintf(body, sizeof(body), "自动翻页：%s\n\nOK 切换 关/3秒/5秒/10秒\n多页卡面到时自动下翻",
                         autopage_label(setting_autopage_sec));
                snprintf(footer, sizeof(footer), "OK 切换");
            } else if (extra == 4u) {
                int soc = battery_usable ? bsp_battery_soc() : -1;
                if (soc < 0) {
                    snprintf(body, sizeof(body), "电量：不可用\n\n电量计未响应");
                } else {
                    snprintf(body, sizeof(body), "电量：%d%%\n\n%s", soc,
                             soc <= 20 ? "电量低，请充电" : "剩余电量");
                }
                snprintf(footer, sizeof(footer), "OK 返回");
            } else if (extra == 5u) {
                snprintf(body, sizeof(body), "新卡限额：%s\n\nOK 切换 不限/10/20/50\n今日已学 %u",
                         new_limit_label(setting_new_limit), (unsigned)setting_new_done);
                snprintf(footer, sizeof(footer), "OK 切换");
            } else if (extra == 6u) {
                snprintf(body, sizeof(body), "打乱顺序：%s\n\nOK 切换 开/关\n同级内随机",
                         setting_shuffle ? "开" : "关");
                snprintf(footer, sizeof(footer), "OK 切换");
            } else if (extra == 7u) {
                uint16_t forecast[3] = {0, 0, 0};
                pa_store_forecast(&store, forecast);
                snprintf(body, sizeof(body), "未来3天：明%u/后%u/大后%u\n\n到期分布（全部牌组）",
                         (unsigned)forecast[0], (unsigned)forecast[1], (unsigned)forecast[2]);
                snprintf(footer, sizeof(footer), "OK 返回");
            } else if (extra == 8u) {
                snprintf(body, sizeof(body), "自动播放：%s\n\nOK %s\n播完今日全部，自动记Good",
                         setting_autoplay ? "开" : "关",
                         setting_autoplay ? "关闭" : "开始播放");
                snprintf(footer, sizeof(footer), "OK 执行");
            }
        }
    } else if (ui == UI_QUESTION || ui == UI_ANSWER || ui == UI_RATING) {
        const char *text = ui == UI_QUESTION ? card_front : card_back;
        pa_text_page pages[PA_LAYOUT_MAX_PAGES];
        size_t pages_count = pages_for(text, pages);
        uint16_t page = ui == UI_QUESTION ? question_page : answer_page;
        body_wrap = false;
        if (!pages_count) {
            snprintf(title, sizeof(title), "字体缺字");
            snprintf(body, sizeof(body), "这张卡含有当前字体不支持的文字。需重新导入受支持的牌组。");
            snprintf(footer, sizeof(footer), "长按 OK 返回");
            body_wrap = true;
            goto draw_labels;
        }
        if (page >= pages_count) page = 0;
        if (!pa_layout_format_page(text, strlen(text), pages[page], PA_STUDY_TEXT_WIDTH,
                                   glyph_width, NULL, body, sizeof(body))) {
            snprintf(title, sizeof(title), "字体缺字");
            snprintf(body, sizeof(body), "这张卡含有当前字体不支持的文字。需重新导入受支持的牌组。");
            snprintf(footer, sizeof(footer), "长按 OK 返回");
            body_wrap = true;
            goto draw_labels;
        }
        snprintf(title, sizeof(title), "%s", card_deck);
        uint16_t remain_x = 0, remain_y = 0;
        study_remaining(study_all, study_deck, &remain_x, &remain_y);
        if (ui == UI_QUESTION) {
            snprintf(footer, sizeof(footer), "%u/%u 剩%u·新%u OK 翻面", page + 1, (unsigned)pages_count,
                     remain_x, remain_y);
        } else if (ui == UI_ANSWER) {
            body_color = 0xE7F0FA;
            snprintf(footer, sizeof(footer), "%u/%u 剩%u·新%u OK 评分", page + 1, (unsigned)pages_count,
                     remain_x, remain_y);
        } else if (grade_save_failed) {
            body_color = 0xE7F0FA;
            footer_color = 0xE05A5A;
            snprintf(footer, sizeof(footer), "保存失败  OK 重试");
        } else {
            body_color = 0xE7F0FA;
            footer_color = 0xC8D0DA;
            snprintf(footer, sizeof(footer), "剩%u·新%u 上下选择 OK", remain_x, remain_y);
        }
    } else if (ui == UI_EMPTY) {
        uint32_t by_rating[4] = {0, 0, 0, 0}, today_total = 0;
        pa_store_today_stats(&store, by_rating, &today_total);
        {
            const char *study_name = study_all ? "全部" : pa_store_deck_name(&store, study_deck);
            uint32_t wait_due = 0;
            uint32_t wait_s = 0;
            bool waiting = autoplay_active && autoplay_scope_earliest(&wait_due) &&
                           (wait_s = wait_due > pa_store_now(&store)
                                         ? (wait_due - pa_store_now(&store)) * 60u
                                         : 0u,
                            wait_s <= AUTOPLAY_MAX_WAIT_S);
            if (waiting && autoplay_empty_since_us != 0) {
                int64_t elapsed = (esp_timer_get_time() - autoplay_empty_since_us) / (1000 * 1000);
                wait_s = elapsed >= 0 && (uint64_t)elapsed < wait_s ? wait_s - (uint32_t)elapsed : 0u;
            }
            if (waiting) {
                snprintf(body, sizeof(body), "自动播放等待中\n约 %" PRIu32 " 秒后继续\n牌组：%s",
                         wait_s, study_name != NULL ? study_name : "全部");
            } else {
                /* New-card budget exhausted but NEW cards remain (LEARNING/
                 * REVIEW all clear): surface the reason instead of "done". */
                bool limit_hit = false;
                if (setting_new_limit != 0 && setting_new_done >= setting_new_limit) {
                    static uint16_t scoped_ndue[PA_STORE_MAX_DECKS];
                    pa_store_new_due_counts(&store, scoped_ndue);
                    uint16_t scoped = 0;
                    if (study_all) {
                        for (uint16_t i = 0; i < count; ++i) {
                            scoped += scoped_ndue[i];
                        }
                    } else if (study_deck < count) {
                        scoped = scoped_ndue[study_deck];
                    }
                    limit_hit = scoped > 0 && !study_has_due();
                }
                if (limit_hit) {
                    snprintf(body, sizeof(body), "今日新卡已学完\n牌组：%s\n今日已评 %" PRIu32 "（%u/%u/%u/%u）\n可推进学习日。",
                             study_name != NULL ? study_name : "全部", today_total,
                             (unsigned)by_rating[0], (unsigned)by_rating[1],
                             (unsigned)by_rating[2], (unsigned)by_rating[3]);
                } else {
                    snprintf(body, sizeof(body), "今天已完成\n牌组：%s\n今日已评 %" PRIu32 "（%u/%u/%u/%u）\n可推进学习日。",
                             study_name != NULL ? study_name : "全部", today_total,
                             (unsigned)by_rating[0], (unsigned)by_rating[1],
                             (unsigned)by_rating[2], (unsigned)by_rating[3]);
                }
            }
        }
        snprintf(footer, sizeof(footer), autoplay_active ? "按任意键转手动" : "OK 重新检查");
    } else if (ui == UI_CONFIRM_DAY) {
        snprintf(body, sizeof(body), "推进学习日 +1？\n\n当前选择：%s\n设备时间保持不变。",
                 confirming_day ? "确认" : "取消");
        snprintf(footer, sizeof(footer), "OK 执行");
    } else if (ui == UI_IMPORTING) {
        snprintf(body, sizeof(body), "正在导入牌组\n已接收 %" PRIu32 " / %" PRIu32 " 字节\n长按 OK 可取消。",
                 store.import_durable + store.import_asm, store.import_length);
        snprintf(footer, sizeof(footer), "请保持 USB 连接");
    } else {
        snprintf(body, sizeof(body), "存档或设备错误\n%s\n长按 OK 返回。", last_error);
        snprintf(footer, sizeof(footer), storage_corrupt ? "需重新导入牌组" :
                 (!storage_ready ? "卡片空间不可用" : ""));
    }
draw_labels:
    sync_study_chrome();
    lv_obj_set_style_text_font(body_label, study_card_ui() ? &pa_font_20 : &pa_font_16, 0);
    lv_label_set_long_mode(title_label, LV_LABEL_LONG_DOT);
    lv_label_set_long_mode(body_label, body_wrap ? LV_LABEL_LONG_WRAP : LV_LABEL_LONG_CLIP);
    lv_label_set_long_mode(footer_label, LV_LABEL_LONG_CLIP);
    if (study_card_ui()) {
        title_color = 0x7A8694;
    }
    lv_obj_set_style_text_color(title_label, lv_color_hex(title_color), 0);
    lv_obj_set_style_text_color(body_label, lv_color_hex(body_color), 0);
    lv_obj_set_style_text_color(footer_label, lv_color_hex(footer_color), 0);
    lv_label_set_text(title_label, title);
    lv_label_set_text(body_label, body);
    lv_label_set_text(footer_label, footer);
    bsp_lvgl_unlock();
}

static void abort_import(void) {
    pa_store_import_abort(&store);
    if (ui == UI_IMPORTING) ui = UI_DECKS;
    autoplay_active = false;
    autoplay_empty_since_us = 0;
}

static void reply_error(const char *code) { printf("ERR %s\n", code); fflush(stdout); }

static void reply_export(const char *kind, const pa_export_info *info) {
    printf("OK %s cards=%u revlog=%" PRIu32 " crc=%" PRIu32 " length=%" PRIu32
           " epoch=%" PRIu32 " dropped=%" PRIu32 " now=%" PRIu32 " offset=%" PRIu32 "\n",
           kind, info->card_count, info->revlog_count, info->crc, info->length, info->epoch,
           info->dropped, info->now, info->served);
    fflush(stdout);
}

static void handle_line(char *line) {
    pa_protocol_command command;
    if (!pa_protocol_parse(line, strlen(line), &command)) { reply_error("COMMAND"); return; }
    switch (command.kind) {
    case PA_PROTOCOL_INFO:
        printf("OK INFO version=%u cards=%u day=%" PRIu32 " cap=%u room=%" PRIu32 "\n",
               PA_PROTOCOL_VERSION, pa_store_count(&store), pa_store_now(&store) / 1440U,
               pa_store_capacity(&store), pa_store_payload_limit(&store));
        fflush(stdout);
        return;
    case PA_PROTOCOL_BEGIN: {
        if (store.import_active) { reply_error("TRANSACTION"); return; }
        if (!storage_ready) { reply_error("STORAGE"); return; }
        uint32_t resume = 0;
        if (!pa_store_import_begin(&store, command.value, command.crc, command.length, &resume)) {
            reply_error("TRANSACTION");
            return;
        }
        session.current = PA_NO_CARD;
        session.revealed = false;
        import_last_input = esp_timer_get_time();
        ui = UI_IMPORTING;
        autoplay_active = false;
        autoplay_empty_since_us = 0;
        printf("OK BEGIN resume=%" PRIu32 "\n", resume);
        fflush(stdout);
        render();
        return;
    }
    case PA_PROTOCOL_CHUNK: {
        if (!store.import_active) { reply_error("TRANSACTION"); return; }
        import_last_input = esp_timer_get_time();
        uint32_t accepted = 0;
        if (!pa_store_import_write(&store, command.value, command.chunk, command.length, &accepted)) {
            reply_error("STORAGE");
            render();
            return;
        }
        /* Import streams hundreds of chunks; redrawing the LVGL progress text
         * per chunk stalls the USB reader. The UI_IMPORTING screen already
         * shows durable progress on BEGIN/COMMIT/RESUME/phase changes, which
         * is frequent enough for a headless bulk transfer. */
        printf("OK CHUNK %" PRIu32 "\n", accepted);
        fflush(stdout);
        return;
    }
    case PA_PROTOCOL_RESUME: {
        uint32_t offset = 0, count = 0, crc = 0, length = 0;
        if (!pa_store_import_resume(&store, &offset, &count, &crc, &length)) {
            reply_error("TRANSACTION");
            return;
        }
        printf("OK RESUME offset=%" PRIu32 " count=%" PRIu32 " crc=%" PRIu32 " length=%" PRIu32 "\n",
               offset, count, crc, length);
        fflush(stdout);
        return;
    }
    case PA_PROTOCOL_COMMIT:
        if (!store.import_active || store.import_durable + store.import_asm != store.import_length) {
            reply_error("TRANSACTION");
            return;
        }
        if (!pa_store_import_commit(&store, text_supported, NULL)) {
            reply_error("STORAGE");
            ui = storage_corrupt ? UI_ERROR : UI_DECKS;
            render();
            return;
        }
        storage_corrupt = false;
        clamp_deck_index();
        ui = UI_DECKS;
        study_day_reset();
        autoplay_active = false;
        autoplay_empty_since_us = 0;
        printf("OK COMMIT\n");
        fflush(stdout);
        render();
        return;
    case PA_PROTOCOL_ABORT:
        abort_import();
        ui = storage_corrupt ? UI_ERROR : UI_DECKS;
        printf("OK ABORT\n");
        fflush(stdout);
        render();
        return;
    case PA_PROTOCOL_DAY: {
        const uint32_t day = command.value;
        uint32_t now = pa_store_now(&store);
        if (storage_corrupt || store.import_active || day < now / 1440U || day > UINT32_MAX / 1440U) {
            reply_error("DAY");
            return;
        }
        const uint32_t target = day * 1440U;
        if ((target > now && !pa_store_advance(&store, target - now)) || !pa_store_checkpoint(&store)) {
            reply_error("STORAGE");
            return;
        }
        printf("OK DAY\n");
        fflush(stdout);
        render();
        return;
    }
    case PA_PROTOCOL_EXPORT: {
        pa_export_info info;
        if (store.import_active || !pa_store_export_begin(&store, &info)) {
            reply_error(store.import_active ? "TRANSACTION" : "REVLOG");
            return;
        }
        reply_export("EXPORT", &info);
        return;
    }
    case PA_PROTOCOL_XCHUNK: {
        uint8_t chunk[PA_PROTOCOL_CHUNK_MAX];
        uint32_t got = 0;
        char hex[PA_PROTOCOL_CHUNK_MAX * 2u + 1u];
        if (!pa_store_export_read(&store, command.value, chunk, sizeof(chunk), &got)) {
            reply_error("RANGE");
            return;
        }
        pa_protocol_write_hex(chunk, got, hex);
        printf("OK XCHUNK %" PRIu32 " %s\n", command.value + got, hex);
        fflush(stdout);
        return;
    }
    case PA_PROTOCOL_XRESUME: {
        pa_export_info info;
        if (!pa_store_export_status(&store, &info)) {
            reply_error("TRANSACTION");
            return;
        }
        reply_export("XRESUME", &info);
        return;
    }
    case PA_PROTOCOL_CARD:
        reply_error("COMMAND");
        return;
    }
    reply_error("COMMAND");
}

static void button_callback(bsp_btn_t button, bsp_btn_ev_t ev, void *user) {
    (void)user;
    if (ev != BSP_BTN_CLICK && ev != BSP_BTN_LONG) return;
    button_event_storage.button = button;
    button_event_storage.button_event = ev;
    (void)xQueueSend(events, &button_event_storage, 0);
}

/* USB Serial/JTAG delivers ~64B packets. The old fgets-per-packet reader
 * treated each packet as a line, so any command over ~64 chars fragmented
 * into ERR LINE and the host had to shrink CHUNK to 20 bytes (13413
 * round-trips for n3). Reassemble bytes until '\n' so the host can use the
 * full 480-byte protocol chunk (~974-char lines, still under PA_LINE_MAX). */
static void console_reader(void *arg) {
    (void)arg;
    static char staging[PA_LINE_MAX + 2];
    size_t used = 0;
    bool discarding = false;
    event_t event = {.type = EVENT_LINE};
    for (;;) {
        int ch = getchar();
        if (ch == EOF) { vTaskDelay(pdMS_TO_TICKS(10)); continue; }
        if (ch != '\n') {
            if (!discarding) {
                /* Cap content at PA_LINE_MAX chars (old fgets path replied
                 * LINE above 1200); the parser still rejects >1200. */
                if (used < (size_t)PA_LINE_MAX) {
                    staging[used++] = (char)ch;
                } else {
                    discarding = true;
                }
            }
            continue;
        }
        if (discarding) {
            discarding = false;
            used = 0;
            reply_error("LINE");
            continue;
        }
        /* Match the old fgets path: strip one trailing '\r' only. Interior
         * '\r' stays in the line so the protocol parser rejects it. */
        if (used > 0 && staging[used - 1u] == '\r') {
            --used;
        }
        staging[used] = '\0';
        memcpy(event.line, staging, used + 1u);
        used = 0;
        if (xQueueSend(events, &event, 0) != pdTRUE) reply_error("BUSY");
    }
}

static void clock_callback(void *arg) {
    (void)arg;
    (void)xQueueSend(events, &tick_event_storage, 0);
}

static void idle_callback(void *arg) {
    (void)arg;
    (void)xQueueSend(events, &idle_event_storage, 0);
}

/* Two-stage auto sleep: dim first (clock keeps running), deep sleep 5 min later.
 * Any button wakes from dim and consumes the press; USB input wakes too.
 * IDLE_US is 1s so auto page turn (3s minimum) fires on time; sleep uses
 * minute-granularity timestamps so the 1s tick does not dim early. */
#define IDLE_US (1LL * 1000 * 1000)
#define DEEP_SLEEP_AFTER_DIM_US (5LL * 60 * 1000 * 1000)
/* USB Serial/JTAG has no VBUS sense pin on this board, so "connected" is
 * inferred from line traffic: any received line within this window blocks
 * dim/deep sleep. Import already inhibits sleep via store.import_active;
 * this covers an idle-but-plugged console (e.g. sitting at a shell). */
#define USB_ACTIVITY_WINDOW_US (30LL * 1000 * 1000)

static void note_activity(int64_t now)
{
    last_activity_us = now;
}

static bool usb_recently_active(int64_t now)
{
    return last_usb_line_us != 0 && now - last_usb_line_us < USB_ACTIVITY_WINDOW_US;
}

static void check_idle_sleep(int64_t now)
{
    if (store.import_active || !storage_ready || storage_corrupt) return;
    if (usb_recently_active(now)) {
        /* Plugged into a host console: stay awake so INFO/协议交互不断背光。
         * No VBUS sense on this board; any line within the window counts. */
        if (screen_dimmed) {
            screen_dimmed = false;
            bsp_display_backlight(setting_brightness);
            render();
        }
        last_activity_us = now;
        dim_start_us = now;
        return;
    }
    if (autoplay_active) {
        /* Hands-free playback keeps the screen on like an import. */
        if (screen_dimmed) {
            screen_dimmed = false;
            bsp_display_backlight(setting_brightness);
            render();
        }
        last_activity_us = now;
        dim_start_us = now;
        return;
    }
    if (!screen_dimmed) {
        if (now - last_activity_us >= (int64_t)setting_sleep_min * 60 * 1000 * 1000) {
            screen_dimmed = true;
            dim_start_us = now;
            bsp_display_backlight(0);
            render();
        }
        return;
    }
    if (now - dim_start_us >= DEEP_SLEEP_AFTER_DIM_US) {
        (void)pa_store_checkpoint(&store);
        (void)bsp_power_enter_deep_sleep();
    }
}

/* Study context survives menu-index restores: the old begin_deck() read the
 * menu cursor (deck_index), so after the stats row shifted decks to 1..count
 * the first deck row had no OK branch and grading re-entered the wrong deck. */
static void begin_study(void) {
    if (storage_corrupt) { ui = UI_ERROR; return; }
    study_day_sync();
    session.current = PA_NO_CARD;
    if (study_all) {
        for (uint16_t i = 0; i < pa_store_deck_count(&store); ++i) {
            session.current = pa_store_next(&store, pa_store_deck_name(&store, i));
            if (session.current != PA_NO_CARD) break;
        }
    } else {
        const char *name = pa_store_deck_name(&store, study_deck);
        if (name != NULL) {
            session.current = pa_store_next(&store, name);
        }
    }
    session.revealed = false;
    if (session.current == PA_NO_CARD) { ui = UI_EMPTY; return; }
    if (!load_current_card()) { ui = UI_ERROR; last_error = "无法读取卡片"; return; }
    ui = UI_QUESTION;
    question_page = answer_page = 0;
    page_shown_us = esp_timer_get_time();
}

static bool study_has_due(void) {
    if (study_all) {
        for (uint16_t i = 0; i < pa_store_deck_count(&store); ++i) {
            if (pa_store_next(&store, pa_store_deck_name(&store, i)) != PA_NO_CARD) {
                return true;
            }
        }
        return false;
    }
    const char *name = pa_store_deck_name(&store, study_deck);
    return name != NULL && pa_store_next(&store, name) != PA_NO_CARD;
}

static bool autoplay_scope_earliest(uint32_t *due_out)
{
    uint32_t best = 0;
    bool found = false;
    if (study_all) {
        for (uint16_t i = 0; i < pa_store_deck_count(&store); ++i) {
            uint32_t d = 0;
            if (pa_store_earliest_due(&store, pa_store_deck_name(&store, i), &d) &&
                (!found || d < best)) {
                best = d;
                found = true;
            }
        }
    } else {
        const char *name = pa_store_deck_name(&store, study_deck);
        found = name != NULL && pa_store_earliest_due(&store, name, &best);
    }
    if (due_out != NULL && found) {
        *due_out = best;
    }
    return found;
}

/* New-card day rollover: NVS persists the counts, the store enforces them. */
static void study_day_sync(void)
{
    if (!storage_ready || storage_corrupt) {
        return;
    }
    uint32_t today = pa_store_now(&store) / 1440U;
    if (setting_new_day != today) {
        setting_new_day = today;
        setting_new_done = 0;
        save_u8_setting("new_done", 0);
        save_u32_setting("new_day", today);
        pa_store_set_limits(&store, setting_new_limit, 0);
    }
}

static void study_day_reset(void)
{
    uint32_t today = (!storage_ready || storage_corrupt) ? setting_new_day
                                                         : pa_store_now(&store) / 1440U;
    setting_new_day = today;
    setting_new_done = 0;
    save_u8_setting("new_done", 0);
    save_u32_setting("new_day", today);
    if (storage_ready && !storage_corrupt) {
        pa_store_set_limits(&store, setting_new_limit, 0);
    }
}

static void do_grade(void) {
    if (session.current == PA_NO_CARD || !session.revealed) return;
    study_day_sync();
    bool graded_new = pa_store_is_new(&store, session.current);
    if (!pa_store_grade(&store, session.current, selected_rating)) {
        grade_save_failed = true;
        return;
    }
    grade_save_failed = false;
    if (graded_new && setting_new_done < UINT16_MAX) {
        setting_new_done++;
        uint8_t save = setting_new_done > UINT8_MAX ? UINT8_MAX : (uint8_t)setting_new_done;
        save_u8_setting("new_done", save);
        pa_store_set_limits(&store, setting_new_limit, setting_new_done);
    }
    begin_study();
}

/* Hands-free play: page -> flip -> auto Good, until nothing is due today. */
static void autoplay_step(int64_t now)
{
    if (ui != UI_QUESTION && ui != UI_ANSWER) {
        return;
    }
    if (grade_save_failed) {
        autoplay_active = false;
        render();
        return;
    }
    uint32_t interval = setting_autopage_sec != 0 ? (uint32_t)setting_autopage_sec : 3u;
    if (now - page_shown_us < (int64_t)interval * 1000 * 1000) {
        return;
    }
    const char *text = ui == UI_QUESTION ? card_front : card_back;
    pa_text_page pages[PA_LAYOUT_MAX_PAGES];
    size_t pages_count = pages_for(text, pages);
    if (pages_count == 0) {
        autoplay_active = false;
        render();
        return;
    }
    uint16_t *page = ui == UI_QUESTION ? &question_page : &answer_page;
    if (*page >= pages_count) {
        *page = 0;
    }
    if (*page + 1 < pages_count) {
        (*page)++;
        page_shown_us = now;
        render();
        return;
    }
    if (ui == UI_QUESTION) {
        ui = UI_ANSWER;
        answer_page = 0;
        session.revealed = true;
        page_shown_us = now;
        render();
        return;
    }
    selected_rating = PA_GOOD;
    do_grade();
    page_shown_us = esp_timer_get_time();
    autoplay_empty_since_us = (autoplay_active && ui == UI_EMPTY) ? page_shown_us : 0;
    if (ui == UI_ERROR) {
        autoplay_active = false;
    }
    render();
}

static void autoplay_wait_tick(int64_t now)
{
    if (study_has_due()) {
        begin_study();
        if (ui != UI_QUESTION) {
            autoplay_active = false;
        } else {
            autoplay_empty_since_us = 0;
            page_shown_us = now;
        }
        render();
        return;
    }
    uint32_t due = 0;
    if (autoplay_scope_earliest(&due)) {
        uint32_t store_now = pa_store_now(&store);
        uint32_t wait_s = due > store_now ? (due - store_now) * 60u : 0u;
        if (wait_s <= AUTOPLAY_MAX_WAIT_S) {
            render();
            return;
        }
    }
    autoplay_active = false;
    autoplay_empty_since_us = 0;
    render();
}

static void process_button(const event_t *event) {
    const bool long_press = event->button_event == BSP_BTN_LONG;
    if (long_press && event->button == BSP_BTN_OK) {
        autoplay_active = false;
        autoplay_empty_since_us = 0;
        if (store.import_active) { abort_import(); ui = storage_corrupt ? UI_ERROR : UI_DECKS; }
        else if (ui == UI_ERROR && (storage_corrupt || !storage_ready)) {
            /* Keep unrecoverable storage failures visible without changing data. */
        }
        else if (ui == UI_CONFIRM_DAY) ui = UI_DECKS;
        else if (ui == UI_RATING) ui = UI_ANSWER;
        else if (ui == UI_QUESTION || ui == UI_ANSWER || ui == UI_EMPTY) ui = UI_DECKS;
        else ui = UI_DECKS;
        render(); return;
    }
    if (long_press) return;
    uint16_t count = deck_count();
    if (ui == UI_DECKS) {
        const uint16_t entries = deck_entries();
        if (entries == 0) { render(); return; }
        /* Index 0 is the stats summary when decks exist (the empty message
         * when none); decks sit at 1..count, then the setting rows starting
         * at settings_base (next day, brightness, sleep, auto-page, battery, new limit, shuffle, forecast, autoplay). */
        const uint16_t settings_base = count > 0 ? (uint16_t)(count + 1u) : 1u;
        if (event->button == BSP_BTN_UP) deck_index = (uint16_t)((deck_index + entries - 1) % entries);
        else if (event->button == BSP_BTN_DOWN) deck_index = (uint16_t)((deck_index + 1) % entries);
        else if (count > 0 && deck_index == 0) {
            study_all = true; study_deck = 0; begin_study();
            autoplay_active = setting_autoplay != 0 && ui == UI_QUESTION;
            autoplay_empty_since_us = 0;
        }
        else if (count > 0 && deck_index >= 1u && deck_index <= count) {
            study_all = false;
            study_deck = (uint16_t)(deck_index - 1u);
            begin_study();
            autoplay_active = setting_autoplay != 0 && ui == UI_QUESTION;
            autoplay_empty_since_us = 0;
        }
        else if (deck_index == settings_base ||
                 (count == 0 && deck_index == 0)) { confirming_day = true; ui = UI_CONFIRM_DAY; }
        else if (deck_index == settings_base + 1u) {
            static const uint8_t levels[] = {30, 70, 100};
            size_t next = 0;
            for (size_t i = 0; i < 3; ++i) {
                if (setting_brightness == levels[i]) { next = (i + 1) % 3; break; }
            }
            setting_brightness = levels[next];
            bsp_display_backlight(setting_brightness);
            save_u8_setting("brightness", setting_brightness);
        } else if (deck_index == settings_base + 2u) {
            setting_sleep_min = setting_sleep_min == 1u ? 3u : setting_sleep_min == 3u ? 5u : 1u;
            save_u8_setting("sleep_min", setting_sleep_min);
        } else if (deck_index == settings_base + 3u) {
            setting_autopage_sec = setting_autopage_sec == 0u ? 3u : setting_autopage_sec == 3u ? 5u
                                 : setting_autopage_sec == 5u ? 10u : 0u;
            save_u8_setting("autopage_sec", setting_autopage_sec);
        } else if (deck_index == settings_base + 4u) {
            /* Battery row is read-only; OK gives visible feedback by
             * returning to the stats summary instead of doing nothing. */
            deck_index = 0;
        } else if (deck_index == settings_base + 5u) {
            setting_new_limit = setting_new_limit == 0u ? 10u : setting_new_limit == 10u ? 20u
                                : setting_new_limit == 20u ? 50u : 0u;
            save_u8_setting("new_limit", setting_new_limit);
            pa_store_set_limits(&store, setting_new_limit, setting_new_done);
        } else if (deck_index == settings_base + 6u) {
            setting_shuffle = setting_shuffle ? 0u : 1u;
            save_u8_setting("shuffle", setting_shuffle);
            pa_store_set_shuffle(&store, setting_shuffle != 0, (uint32_t)esp_random());
        } else if (deck_index == settings_base + 7u) {
            /* Forecast row is read-only; OK gives visible feedback by
             * returning to the stats summary instead of doing nothing. */
            deck_index = 0;
        } else if (deck_index == settings_base + 8u) {
            setting_autoplay = setting_autoplay ? 0u : 1u;
            save_u8_setting("autoplay", setting_autoplay);
            if (setting_autoplay) {
                autoplay_active = true;
                autoplay_empty_since_us = 0;
                begin_study();
                if (ui != UI_QUESTION) {
                    autoplay_active = false;
                }
            } else {
                autoplay_active = false;
                autoplay_empty_since_us = 0;
            }
        }
    } else if (ui == UI_CONFIRM_DAY) {
        if (event->button == BSP_BTN_UP || event->button == BSP_BTN_DOWN) confirming_day = !confirming_day;
        else if (!confirming_day) ui = UI_DECKS;
        else if (pa_store_now(&store) > UINT32_MAX - 1440U || !pa_store_advance(&store, 1440U) ||
                 !pa_store_checkpoint(&store)) {
            ui = UI_ERROR; last_error = "学习日无法保存";
        } else { confirming_day = false; ui = UI_DECKS; study_day_sync(); }
    } else if (ui == UI_QUESTION || ui == UI_ANSWER) {
        const char *text = ui == UI_QUESTION ? card_front : card_back;
        pa_text_page pages[PA_LAYOUT_MAX_PAGES]; size_t pages_count = pages_for(text, pages);
        uint16_t *page = ui == UI_QUESTION ? &question_page : &answer_page;
        page_shown_us = esp_timer_get_time();
        if (event->button == BSP_BTN_UP && *page > 0) --*page;
        else if (event->button == BSP_BTN_DOWN && *page + 1 < pages_count) ++*page;
        else if (event->button == BSP_BTN_OK && ui == UI_QUESTION) { ui = UI_ANSWER; answer_page = 0; session.revealed = true; }
        else if (event->button == BSP_BTN_OK) { ui = UI_RATING; selected_rating = PA_GOOD; }
    } else if (ui == UI_RATING) {
        if (event->button == BSP_BTN_UP) selected_rating = (pa_rating)((selected_rating + 3) % 4);
        else if (event->button == BSP_BTN_DOWN) selected_rating = (pa_rating)((selected_rating + 1) % 4);
        else do_grade();
    } else if (ui == UI_EMPTY && event->button == BSP_BTN_OK) begin_study();
    render();
}

static void app_task(void *arg) {
    (void)arg;
    event_t event;
    for (;;) {
        if (xQueueReceive(events, &event, portMAX_DELAY) != pdTRUE) continue;
        if (event.type == EVENT_BUTTON) {
            note_activity(esp_timer_get_time());
            if (screen_dimmed) {
                /* Woke from dim: consume the press so it cannot flip or grade. */
                screen_dimmed = false;
                bsp_display_backlight(setting_brightness);
                render();
            } else if (autoplay_active && (study_card_ui() || ui == UI_EMPTY)) {
                /* Any key exits autoplay and is consumed: no flip, no grade. */
                int64_t press_now = esp_timer_get_time();
                autoplay_active = false;
                autoplay_empty_since_us = 0;
                note_activity(press_now);
                page_shown_us = press_now;
                render();
            } else if (pa_cover_requests_launcher((int)ui, (int)event.button, (int)event.button_event)) {
                if (storage_ready && !storage_corrupt && !store.import_active) {
                    (void)pa_store_checkpoint(&store);
                }
                esp_err_t cover_err = launcher_contract_return_to_factory();
                ESP_LOGW(TAG, "stay in play; launcher return failed (%s)", esp_err_to_name(cover_err));
            } else {
                process_button(&event);
            }
        }
        else if (event.type == EVENT_LINE) {
            int64_t line_now = esp_timer_get_time();
            note_activity(line_now);
            last_usb_line_us = line_now;
            handle_line(event.line);
            if (ui != UI_ERROR && ui != UI_IMPORTING) render();
        } else if (event.type == EVENT_TICK) {
            int64_t now = esp_timer_get_time();
            if (store.import_active && now - import_last_input >= IMPORT_TIMEOUT_US) {
                abort_import(); ui = storage_corrupt ? UI_ERROR : UI_DECKS; reply_error("TIMEOUT"); render();
            } else if (storage_ready && !storage_corrupt && !store.import_active) {
                uint32_t elapsed = (uint32_t)((now - last_clock_us) / MINUTE_US);
                if (elapsed && pa_store_advance(&store, elapsed)) {
                    last_clock_us += (int64_t)elapsed * MINUTE_US;
                    if (now - last_checkpoint >= 5 * MINUTE_US && pa_store_checkpoint(&store)) last_checkpoint = now;
                    study_day_sync();
                    if (ui == UI_EMPTY && study_has_due()) begin_study();
                    render();
                }
                check_idle_sleep(now);
            }
        } else if (event.type == EVENT_IDLE) {
            int64_t now = esp_timer_get_time();
            check_idle_sleep(now);
            if (autoplay_active && ui == UI_EMPTY) {
                autoplay_wait_tick(now);
            } else if (autoplay_active && (ui == UI_QUESTION || ui == UI_ANSWER) &&
                       !screen_dimmed) {
                autoplay_step(now);
            } else if (!autoplay_active && !screen_dimmed && setting_autopage_sec > 0 &&
                       (ui == UI_QUESTION || ui == UI_ANSWER)) {
                const char *text = ui == UI_QUESTION ? card_front : card_back;
                pa_text_page pages[PA_LAYOUT_MAX_PAGES];
                size_t pages_count = pages_for(text, pages);
                uint16_t *page = ui == UI_QUESTION ? &question_page : &answer_page;
                if (pages_count > 1 && *page + 1 < pages_count &&
                    now - page_shown_us >= (int64_t)setting_autopage_sec * 1000 * 1000) {
                    (*page)++;
                    page_shown_us = now;
                    render();
                }
            }
        }
    }
}

static void migrate_legacy_snapshot(void) {
    if (!nvs_ready || pa_store_mount(&store) != PA_STORE_EMPTY) {
        return;
    }
    size_t size = 0;
    if (nvs_get_blob(nvs, "snapshot", NULL, &size) != ESP_OK || size == 0 || size > PA_SNAPSHOT_MAX) {
        return;
    }
    uint8_t *blob = malloc(size);
    if (blob == NULL) {
        return;
    }
    if (nvs_get_blob(nvs, "snapshot", blob, &size) == ESP_OK && pa_store_migrate_pak1(&store, blob, size)) {
        if (nvs_erase_key(nvs, "snapshot") == ESP_OK) {
            (void)nvs_commit(nvs);
        }
        ESP_LOGI(TAG, "migrated legacy PAK1 snapshot");
    } else {
        ESP_LOGW(TAG, "legacy PAK1 snapshot was not migrated");
    }
    free(blob);
}

void app_main(void) {
    ESP_ERROR_CHECK(bsp_power_wait_for_wake_release());
    esp_err_t err = nvs_flash_init();
    if (err == ESP_OK) err = nvs_open("pa_anki", NVS_READWRITE, &nvs);
    nvs_ready = err == ESP_OK;
    if (!nvs_ready) {
        ESP_LOGE(TAG, "NVS init failed; flash was not erased");
    }
    storage_ready = open_tail_storage();
    if (storage_ready) {
        pa_store_init(&store, &cards_flash, &sched_flash);
        pa_store_status status = pa_store_mount(&store);
        if (status == PA_STORE_EMPTY) {
            migrate_legacy_snapshot();
            status = store.status;
        }
        storage_corrupt = status == PA_STORE_CORRUPT;
        if (storage_corrupt) last_error = "存档损坏或无法读取";
    } else {
        last_error = "卡片空间不可用；未擦除任何数据";
    }
    clamp_deck_index();
    ESP_LOGI(TAG, "free heap %" PRIu32 " bytes; pa_store %u bytes; cards %u",
             (uint32_t)esp_get_free_heap_size(), (unsigned)sizeof(pa_store), pa_store_count(&store));
    ESP_ERROR_CHECK(bsp_display_init());
    lv_display_t *display = bsp_lvgl_init();
    if (!display) { ESP_LOGE(TAG, "LVGL initialization failed"); return; }
    if (!bsp_lvgl_lock(1000)) {
        ESP_LOGE(TAG, "LVGL lock unavailable during screen creation");
        return;
    }
    {
        static const char *grade_names[] = {"Again", "Hard", "Good", "Easy"};
        const int chip_w = (PA_STUDY_TEXT_WIDTH - 2 * PA_STUDY_GRADE_PAD - PA_STUDY_GRADE_GAP) / 2;
        const int chip_h = PA_STUDY_GRADE_ROW_H;
        lv_obj_t *screen = lv_screen_active();
        lv_obj_set_style_bg_color(screen, lv_color_hex(0x12161C), 0);
        lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
        title_label = lv_label_create(screen);
        lv_obj_set_pos(title_label, PA_STUDY_MARGIN_X, PA_STUDY_TITLE_Y);
        lv_obj_set_size(title_label, PA_STUDY_TEXT_WIDTH, PA_STUDY_TITLE_H);
        lv_obj_set_style_pad_all(title_label, 0, 0);
        lv_label_set_long_mode(title_label, LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_line_space(title_label, 0, 0);
        lv_obj_set_style_text_font(title_label, &pa_font_16, 0);
        lv_obj_set_style_text_color(title_label, lv_color_hex(0x8B95A1), 0);
        header_rule = lv_obj_create(screen);
        lv_obj_remove_style_all(header_rule);
        lv_obj_set_pos(header_rule, PA_STUDY_MARGIN_X, PA_STUDY_TITLE_Y + PA_STUDY_TITLE_H + 2);
        lv_obj_set_size(header_rule, PA_STUDY_TEXT_WIDTH, 1);
        lv_obj_set_style_bg_color(header_rule, lv_color_hex(0x2A3340), 0);
        lv_obj_set_style_bg_opa(header_rule, LV_OPA_COVER, 0);
        lv_obj_add_flag(header_rule, LV_OBJ_FLAG_HIDDEN);
        body_label = lv_label_create(screen);
        lv_obj_set_pos(body_label, PA_STUDY_MARGIN_X, PA_STUDY_BODY_Y);
        lv_obj_set_size(body_label, PA_STUDY_TEXT_WIDTH, PA_STUDY_BODY_H);
        lv_obj_set_style_pad_all(body_label, 0, 0);
        lv_label_set_long_mode(body_label, LV_LABEL_LONG_CLIP);
        lv_obj_set_style_text_font(body_label, &pa_font_16, 0);
        lv_obj_set_style_text_line_space(body_label, PA_STUDY_BODY_LINE_SP, 0);
        lv_obj_set_style_text_color(body_label, lv_color_hex(0xF4F1EA), 0);
        footer_panel = lv_obj_create(screen);
        lv_obj_remove_style_all(footer_panel);
        lv_obj_set_pos(footer_panel, PA_STUDY_MARGIN_X, PA_STUDY_FOOTER_Y);
        lv_obj_set_size(footer_panel, PA_STUDY_TEXT_WIDTH, PA_STUDY_FOOTER_H);
        lv_obj_set_style_bg_color(footer_panel, lv_color_hex(0x0E1218), 0);
        lv_obj_set_style_bg_opa(footer_panel, LV_OPA_70, 0);
        lv_obj_set_style_radius(footer_panel, 8, 0);
        lv_obj_set_style_pad_all(footer_panel, PA_STUDY_GRADE_PAD, 0);
        lv_obj_add_flag(footer_panel, LV_OBJ_FLAG_HIDDEN);
        footer_label = lv_label_create(screen);
        lv_obj_set_pos(footer_label, PA_STUDY_MARGIN_X, PA_STUDY_FOOTER_Y);
        lv_obj_set_size(footer_label, PA_STUDY_TEXT_WIDTH, PA_STUDY_FOOTER_H);
        lv_obj_set_style_pad_all(footer_label, 0, 0);
        lv_label_set_long_mode(footer_label, LV_LABEL_LONG_CLIP);
        lv_obj_set_style_text_line_space(footer_label, 0, 0);
        lv_obj_set_style_text_font(footer_label, &pa_font_16, 0);
        lv_obj_set_style_text_color(footer_label, lv_color_hex(0x9AA3AD), 0);
        lv_obj_set_style_text_align(footer_label, LV_TEXT_ALIGN_CENTER, 0);
        for (int i = 0; i < 4; ++i) {
            const int col = i % 2;
            const int row = i / 2;
            grade_chip[i] = lv_obj_create(footer_panel);
            lv_obj_remove_style_all(grade_chip[i]);
            lv_obj_set_pos(grade_chip[i], PA_STUDY_GRADE_PAD + col * (chip_w + PA_STUDY_GRADE_GAP),
                           PA_STUDY_GRADE_PAD + row * (chip_h + PA_STUDY_GRADE_GAP));
            lv_obj_set_size(grade_chip[i], chip_w, chip_h);
            lv_obj_set_style_radius(grade_chip[i], 6, 0);
            lv_obj_set_style_bg_opa(grade_chip[i], LV_OPA_COVER, 0);
            lv_obj_set_style_border_opa(grade_chip[i], LV_OPA_COVER, 0);
            lv_obj_add_flag(grade_chip[i], LV_OBJ_FLAG_HIDDEN);
            grade_chip_label[i] = lv_label_create(grade_chip[i]);
            lv_obj_set_width(grade_chip_label[i], chip_w);
            lv_obj_set_style_text_align(grade_chip_label[i], LV_TEXT_ALIGN_CENTER, 0);
            lv_obj_set_style_text_font(grade_chip_label[i], &pa_font_16, 0);
            lv_label_set_text(grade_chip_label[i], grade_names[i]);
            lv_obj_center(grade_chip_label[i]);
        }
        bsp_lvgl_unlock();
    }
    {
        static const uint8_t brightness_levels[] = {30, 70, 100};
        static const uint8_t sleep_levels[] = {1, 3, 5};
        static const uint8_t autopage_levels[] = {0, 3, 5, 10};
        setting_brightness = load_u8_setting("brightness", 70, brightness_levels, 3);
        setting_sleep_min = load_u8_setting("sleep_min", 3, sleep_levels, 3);
        setting_autopage_sec = load_u8_setting("autopage_sec", 0, autopage_levels, 4);
        static const uint8_t new_limit_levels[] = {0, 10, 20, 50};
        static const uint8_t off_on_levels[] = {0, 1};
        setting_new_limit = load_u8_setting("new_limit", 20, new_limit_levels, 4);
        setting_shuffle = load_u8_setting("shuffle", 0, off_on_levels, 2);
        setting_autoplay = load_u8_setting("autoplay", 0, off_on_levels, 2);
        if (nvs_ready) {
            uint8_t done = 0;
            if (nvs_get_u8(nvs, "new_done", &done) == ESP_OK) {
                setting_new_done = done;
            }
            setting_new_day = load_u32_setting("new_day", 0);
        }
        pa_store_set_limits(&store, setting_new_limit, setting_new_done);
        pa_store_set_shuffle(&store, setting_shuffle != 0, (uint32_t)esp_random());
        study_day_sync();
        autoplay_active = false;
        autoplay_empty_since_us = 0;
        bsp_display_backlight(setting_brightness);
    }
    battery_usable = bsp_battery_init() == ESP_OK;
    events = xQueueCreate(4, sizeof(event_t));
    if (!events) { ESP_LOGE(TAG, "event queue allocation failed"); return; }
    ESP_ERROR_CHECK(bsp_button_init(button_callback, NULL));
    if (xTaskCreate(console_reader, "anki_usb", 4096, NULL, 4, NULL) != pdPASS ||
        xTaskCreate(app_task, "anki_app", 16384, NULL, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "app task creation failed");
        return;
    }
    esp_timer_handle_t clock;
    const esp_timer_create_args_t timer_args = {.callback = clock_callback, .name = "anki_clock"};
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &clock));
    last_clock_us = esp_timer_get_time();
    last_checkpoint = last_clock_us;
    last_activity_us = last_clock_us;
    ESP_ERROR_CHECK(esp_timer_start_periodic(clock, MINUTE_US));
    esp_timer_handle_t idle;
    const esp_timer_create_args_t idle_args = {.callback = idle_callback, .name = "anki_idle"};
    ESP_ERROR_CHECK(esp_timer_create(&idle_args, &idle));
    ESP_ERROR_CHECK(esp_timer_start_periodic(idle, IDLE_US));
    if (storage_corrupt || !storage_ready) ui = UI_ERROR;
    render();
}
