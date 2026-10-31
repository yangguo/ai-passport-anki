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
static int64_t page_shown_us;
static bool battery_usable = false;
static bool screen_dimmed = false;
static int64_t last_activity_us, dim_start_us;
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
 * auto-page, battery rows. */
#define DECK_EXTRA_ROWS 5u

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
            uint32_t total_due = 0;
            uint32_t by_rating[4] = {0, 0, 0, 0}, today_total = 0;
            pa_store_due_counts(&store, due);
            for (uint16_t i = 0; i < count; ++i) total_due += due[i];
            pa_store_today_stats(&store, by_rating, &today_total);
            snprintf(body, sizeof(body), "今日已评 %" PRIu32 "（%u/%u/%u/%u）\n待学 %" PRIu32 " · %u 牌组\n学习日 %" PRIu32,
                     today_total, (unsigned)by_rating[0], (unsigned)by_rating[1],
                     (unsigned)by_rating[2], (unsigned)by_rating[3],
                     total_due, count, day);
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
        if (ui == UI_QUESTION) {
            snprintf(footer, sizeof(footer), "%u/%u  OK 翻面", page + 1, (unsigned)pages_count);
        } else if (ui == UI_ANSWER) {
            body_color = 0xE7F0FA;
            snprintf(footer, sizeof(footer), "%u/%u  OK 评分", page + 1, (unsigned)pages_count);
        } else if (grade_save_failed) {
            body_color = 0xE7F0FA;
            footer_color = 0xE05A5A;
            snprintf(footer, sizeof(footer), "保存失败  OK 重试");
        } else {
            body_color = 0xE7F0FA;
            footer_color = 0xC8D0DA;
            snprintf(footer, sizeof(footer), "上下选择  OK 确认");
        }
    } else if (ui == UI_EMPTY) {
        uint32_t by_rating[4] = {0, 0, 0, 0}, today_total = 0;
        pa_store_today_stats(&store, by_rating, &today_total);
        {
            const char *study_name = study_all ? "全部" : pa_store_deck_name(&store, study_deck);
            snprintf(body, sizeof(body), "今天已完成\n牌组：%s\n今日已评 %" PRIu32 "（%u/%u/%u/%u）\n可推进学习日。",
                     study_name != NULL ? study_name : "全部", today_total,
                     (unsigned)by_rating[0], (unsigned)by_rating[1],
                     (unsigned)by_rating[2], (unsigned)by_rating[3]);
        }
        snprintf(footer, sizeof(footer), "OK 重新检查");
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
        render();
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

static void console_reader(void *arg) {
    (void)arg;
    event_t event = {.type = EVENT_LINE};
    for (;;) {
        if (!fgets(event.line, sizeof(event.line), stdin)) { vTaskDelay(pdMS_TO_TICKS(100)); continue; }
        size_t n = strlen(event.line);
        if (!n || event.line[n - 1] != '\n') {
            int ch;
            while ((ch = getchar()) != '\n' && ch != EOF) {}
            reply_error("LINE"); continue;
        }
        if (n > PA_LINE_MAX + 1) { reply_error("LINE"); continue; }
        event.line[n - 1] = '\0';
        if (n >= 2 && event.line[n - 2] == '\r') event.line[n - 2] = '\0';
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
 * Any button wakes from dim and consumes the press; USB input wakes too. */
#define IDLE_US (10LL * 1000 * 1000)
#define DEEP_SLEEP_AFTER_DIM_US (5LL * 60 * 1000 * 1000)

static void note_activity(int64_t now)
{
    last_activity_us = now;
}

static void check_idle_sleep(int64_t now)
{
    if (store.import_active || !storage_ready || storage_corrupt) return;
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

static void do_grade(void) {
    if (session.current == PA_NO_CARD || !session.revealed) return;
    if (!pa_store_grade(&store, session.current, selected_rating)) {
        grade_save_failed = true;
        return;
    }
    grade_save_failed = false;
    begin_study();
}

static void process_button(const event_t *event) {
    const bool long_press = event->button_event == BSP_BTN_LONG;
    if (long_press && event->button == BSP_BTN_OK) {
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
         * at settings_base (next day, brightness, sleep, auto-page, battery). */
        const uint16_t settings_base = count > 0 ? (uint16_t)(count + 1u) : 1u;
        if (event->button == BSP_BTN_UP) deck_index = (uint16_t)((deck_index + entries - 1) % entries);
        else if (event->button == BSP_BTN_DOWN) deck_index = (uint16_t)((deck_index + 1) % entries);
        else if (count > 0 && deck_index == 0) { study_all = true; study_deck = 0; begin_study(); }
        else if (count > 0 && deck_index >= 1u && deck_index <= count) {
            study_all = false;
            study_deck = (uint16_t)(deck_index - 1u);
            begin_study();
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
        }
    } else if (ui == UI_CONFIRM_DAY) {
        if (event->button == BSP_BTN_UP || event->button == BSP_BTN_DOWN) confirming_day = !confirming_day;
        else if (!confirming_day) ui = UI_DECKS;
        else if (pa_store_now(&store) > UINT32_MAX - 1440U || !pa_store_advance(&store, 1440U) ||
                 !pa_store_checkpoint(&store)) {
            ui = UI_ERROR; last_error = "学习日无法保存";
        } else { confirming_day = false; ui = UI_DECKS; }
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
            note_activity(esp_timer_get_time());
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
                    if (ui == UI_EMPTY && study_has_due()) begin_study();
                    render();
                }
                check_idle_sleep(now);
            }
        } else if (event.type == EVENT_IDLE) {
            int64_t now = esp_timer_get_time();
            check_idle_sleep(now);
            if (!screen_dimmed && setting_autopage_sec > 0 &&
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
