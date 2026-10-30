#include "bsp_battery.h"
#include "bsp_button.h"
#include "bsp_display.h"
#include "bsp_power.h"
#include "bsp_pins.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "pa_layout.h"
#include "pa_protocol.h"
#include "pa_store.h"
#include "passport_anki.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PA_LINE_MAX 1200
#define IMPORT_TIMEOUT_US (60LL * 1000 * 1000)
#define MINUTE_US (60LL * 1000 * 1000)

extern const lv_font_t pa_font_16;
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
typedef enum { EVENT_BUTTON, EVENT_LINE, EVENT_TICK } event_type;
typedef struct {
    event_type type;
    bsp_btn_t button;
    bsp_btn_ev_t button_event;
    char line[PA_LINE_MAX + 2];
} event_t;
static QueueHandle_t events;
static event_t button_event_storage = {.type = EVENT_BUTTON};
static event_t tick_event_storage = {.type = EVENT_TICK};
static ui_state ui = UI_DECKS;
static pa_rating selected_rating = PA_GOOD;
static uint16_t deck_index, question_page, answer_page;
static bool confirming_day, grade_save_failed;
static const char *last_error = "";
static lv_obj_t *title_label, *body_label, *footer_label;

static bool part_read(void *ctx, uint32_t offset, void *dst, uint32_t length)
{
    return esp_partition_read(ctx, offset, dst, length) == ESP_OK;
}

static bool part_write(void *ctx, uint32_t offset, const void *src, uint32_t length)
{
    uint8_t tail[16];
    uint32_t aligned = length & ~15u;
    if (aligned != 0 && esp_partition_write(ctx, offset, src, aligned) != ESP_OK) {
        return false;
    }
    uint32_t rest = length - aligned;
    if (rest == 0) {
        return true;
    }
    memset(tail, 0xff, sizeof(tail));
    memcpy(tail, (const uint8_t *)src + aligned, rest);
    return esp_partition_write(ctx, offset + aligned, tail, sizeof(tail)) == ESP_OK;
}

static bool part_erase(void *ctx, uint32_t offset, uint32_t length)
{
    return esp_partition_erase_range(ctx, offset, length) == ESP_OK;
}

static bool open_partition(pa_flash *flash, const char *name, uint32_t expected)
{
    const esp_partition_t *part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, name);
    if (part == NULL || part->size != expected) {
        return false;
    }
    *flash = (pa_flash){.ctx = (void *)part, .size = part->size, .read = part_read, .write = part_write, .erase = part_erase};
    return true;
}

static uint16_t glyph_width(uint32_t cp, void *user) {
    (void)user;
    lv_font_glyph_dsc_t glyph;
    return lv_font_get_glyph_dsc(&pa_font_16, &glyph, cp, 0) && !glyph.is_placeholder
               ? glyph.adv_w : 0;
}

static bool text_fits(const char *text) {
    pa_text_page pages[PA_LAYOUT_MAX_PAGES];
    return pa_layout_pages(text, strlen(text), BSP_LCD_W - 36, 4, glyph_width, NULL, pages) != 0;
}

static bool text_supported(const char *deck, const char *front, const char *back, void *user) {
    (void)user;
    if ((++glyph_checks & 127u) == 0) {
        vTaskDelay(1);
    }
    return text_fits(deck) && text_fits(front) && text_fits(back);
}

static size_t pages_for(const char *text, pa_text_page pages[PA_LAYOUT_MAX_PAGES]) {
    return pa_layout_pages(text, strlen(text), BSP_LCD_W - 36, 4, glyph_width, NULL, pages);
}

static uint16_t deck_count(void) { return pa_store_deck_count(&store); }

static const char *deck_selected(void) {
    return pa_store_deck_name(&store, deck_index);
}

static void clamp_deck_index(void) {
    uint16_t count = deck_count();
    if (deck_index > count) {
        deck_index = count;
    }
}

static const char *rating_name(pa_rating rating) {
    static const char *names[] = {"Again", "Hard", "Good", "Easy"};
    return rating <= PA_EASY ? names[rating] : "Good";
}

static bool load_current_card(void) {
    return pa_store_read(&store, session.current, card_deck, sizeof(card_deck), card_front,
                         sizeof(card_front), card_back, sizeof(card_back));
}

static void render(void) {
    if (!title_label || !body_label || !footer_label || !bsp_lvgl_lock(100)) return;
    char title[96], body[PA_LINE_MAX + 1], footer[160];
    uint32_t day = pa_store_now(&store) / 1440U;
    snprintf(title, sizeof(title), "Anki  学习日 %" PRIu32, day);
    body[0] = '\0';
    snprintf(footer, sizeof(footer), "UP/DOWN 选择   OK 确认");
    uint16_t count = deck_count();
    if (ui == UI_DECKS) {
        if (count == 0) {
            snprintf(body, sizeof(body), "还没有牌组\n通过 USB 导入 TSV\n\n学习日：%" PRIu32, day);
            snprintf(footer, sizeof(footer), "OK可推进学习日；长按返回");
        } else if (deck_index == count) {
            snprintf(body, sizeof(body), "推进到下一学习日？\n\n当前学习日：%" PRIu32, day);
            snprintf(footer, sizeof(footer), "UP/DOWN选择   OK确认推进");
        } else {
            snprintf(body, sizeof(body), "%s\n牌组 %u/%u\nOK 开始复习",
                     deck_selected() ? deck_selected() : "还没有牌组", deck_index + 1, count);
        }
    } else if (ui == UI_QUESTION || ui == UI_ANSWER || ui == UI_RATING) {
        const char *text = ui == UI_QUESTION ? card_front : card_back;
        pa_text_page pages[PA_LAYOUT_MAX_PAGES];
        size_t pages_count = pages_for(text, pages);
        uint16_t page = ui == UI_QUESTION ? question_page : answer_page;
        if (!pages_count) {
            snprintf(title, sizeof(title), "字体缺字");
            snprintf(body, sizeof(body), "这张卡含有当前字体不支持的文字。需重新导入受支持的牌组。");
            snprintf(footer, sizeof(footer), "长按 OK 返回");
            goto draw_labels;
        }
        if (page >= pages_count) page = 0;
        if (!pa_layout_format_page(text, strlen(text), pages[page], BSP_LCD_W - 36,
                                   glyph_width, NULL, body, sizeof(body))) {
            snprintf(title, sizeof(title), "字体缺字");
            snprintf(body, sizeof(body), "这张卡含有当前字体不支持的文字。需重新导入受支持的牌组。");
            snprintf(footer, sizeof(footer), "长按 OK 返回");
            goto draw_labels;
        }
        snprintf(title, sizeof(title), "%s", card_deck);
        if (ui == UI_QUESTION) snprintf(footer, sizeof(footer), "D%" PRIu32 " C%u/%u Q%u/%u UP/DOWN 翻面OK",
                                        day, session.current + 1, pa_store_count(&store),
                                        page + 1, (unsigned)pages_count);
        else if (ui == UI_ANSWER) snprintf(footer, sizeof(footer), "D%" PRIu32 " C%u/%u A%u/%u UP/DOWN 评分OK",
                                          day, session.current + 1, pa_store_count(&store),
                                          page + 1, (unsigned)pages_count);
        else if (grade_save_failed) snprintf(footer, sizeof(footer), "存档失败；OK重试  长按返回");
        else snprintf(footer, sizeof(footer), "%s  D%" PRIu32 " C%u/%u UP/DOWN 保存OK",
                      rating_name(selected_rating), day, session.current + 1, pa_store_count(&store));
    } else if (ui == UI_EMPTY) {
        snprintf(body, sizeof(body), "今天已完成\n牌组：%s\n可推进学习日。",
                 deck_selected() ? deck_selected() : "全部");
        snprintf(footer, sizeof(footer), "OK重新检查   长按返回牌组");
    } else if (ui == UI_CONFIRM_DAY) {
        snprintf(body, sizeof(body), "推进学习日 +1？\n\n当前选择：%s\n设备时间保持不变。",
                 confirming_day ? "确认" : "取消");
        snprintf(footer, sizeof(footer), "UP/DOWN选择  OK执行  长按返回");
    } else if (ui == UI_IMPORTING) {
        snprintf(body, sizeof(body), "正在导入牌组\n已接收 %" PRIu32 " / %" PRIu32 " 字节\n长按 OK 可取消。",
                 store.import_durable + store.import_asm, store.import_length);
        snprintf(footer, sizeof(footer), "请保持 USB 连接");
    } else {
        snprintf(body, sizeof(body), "存档或设备错误\n%s\n长按 OK 返回。", last_error);
        snprintf(footer, sizeof(footer), storage_corrupt ? "需显式导入新牌组" :
                 (!storage_ready ? "卡片分区不可用；未擦除 NVS" : ""));
    }
draw_labels:
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
        printf("OK INFO version=%u cards=%u day=%" PRIu32 " cap=%u\n", PA_PROTOCOL_VERSION,
               pa_store_count(&store), pa_store_now(&store) / 1440U, PA_STORE_MAX_CARDS);
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
        render();
        printf("OK BEGIN resume=%" PRIu32 "\n", resume);
        fflush(stdout);
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
        render();
        printf("OK ABORT\n");
        fflush(stdout);
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

static void begin_deck(void) {
    if (storage_corrupt) { ui = UI_ERROR; return; }
    session.current = pa_store_next(&store, deck_selected());
    session.revealed = false;
    if (session.current == PA_NO_CARD) { ui = UI_EMPTY; return; }
    if (!load_current_card()) { ui = UI_ERROR; last_error = "无法读取卡片"; return; }
    ui = UI_QUESTION;
    question_page = answer_page = 0;
}

static void do_grade(void) {
    if (session.current == PA_NO_CARD || !session.revealed) return;
    if (!pa_store_grade(&store, session.current, selected_rating)) {
        grade_save_failed = true;
        return;
    }
    grade_save_failed = false;
    begin_deck();
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
        const uint16_t entries = count + 1;
        if (event->button == BSP_BTN_UP) deck_index = (uint16_t)((deck_index + entries - 1) % entries);
        else if (event->button == BSP_BTN_DOWN) deck_index = (uint16_t)((deck_index + 1) % entries);
        else if (deck_index == count) { confirming_day = true; ui = UI_CONFIRM_DAY; }
        else begin_deck();
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
        if (event->button == BSP_BTN_UP && *page > 0) --*page;
        else if (event->button == BSP_BTN_DOWN && *page + 1 < pages_count) ++*page;
        else if (event->button == BSP_BTN_OK && ui == UI_QUESTION) { ui = UI_ANSWER; answer_page = 0; session.revealed = true; }
        else if (event->button == BSP_BTN_OK) { ui = UI_RATING; selected_rating = PA_GOOD; }
    } else if (ui == UI_RATING) {
        if (event->button == BSP_BTN_UP) selected_rating = (pa_rating)((selected_rating + 3) % 4);
        else if (event->button == BSP_BTN_DOWN) selected_rating = (pa_rating)((selected_rating + 1) % 4);
        else do_grade();
    } else if (ui == UI_EMPTY && event->button == BSP_BTN_OK) begin_deck();
    render();
}

static void app_task(void *arg) {
    (void)arg;
    event_t event;
    for (;;) {
        if (xQueueReceive(events, &event, portMAX_DELAY) != pdTRUE) continue;
        if (event.type == EVENT_BUTTON) process_button(&event);
        else if (event.type == EVENT_LINE) {
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
                    if (ui == UI_EMPTY && pa_store_next(&store, deck_selected()) != PA_NO_CARD) begin_deck();
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
    storage_ready = open_partition(&cards_flash, "cards", PA_CARDS_PARTITION_SIZE) &&
                    open_partition(&sched_flash, "sched", PA_SCHED_PARTITION_SIZE);
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
        last_error = "卡片分区不可用；未擦除任何数据";
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
        lv_obj_t *screen = lv_screen_active();
        lv_obj_set_style_bg_color(screen, lv_color_hex(0x101820), 0);
        title_label = lv_label_create(screen); lv_obj_set_pos(title_label, 18, 30); lv_obj_set_size(title_label, 204, 62);
        lv_label_set_long_mode(title_label, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_line_space(title_label, 0, 0);
        lv_obj_set_style_text_font(title_label, &pa_font_16, 0);
        lv_obj_set_style_text_color(title_label, lv_color_hex(0xf3f4f6), 0);
        body_label = lv_label_create(screen); lv_obj_set_pos(body_label, 18, 96); lv_obj_set_size(body_label, 204, 124);
        lv_label_set_long_mode(body_label, LV_LABEL_LONG_CLIP); lv_obj_set_style_text_font(body_label, &pa_font_16, 0);
        lv_obj_set_style_text_line_space(body_label, 0, 0);
        lv_obj_set_style_text_color(body_label, lv_color_hex(0xf3f4f6), 0);
        footer_label = lv_label_create(screen); lv_obj_set_pos(footer_label, 18, 224); lv_obj_set_size(footer_label, 204, 62);
        lv_label_set_long_mode(footer_label, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_line_space(footer_label, 0, 0);
        lv_obj_set_style_text_font(footer_label, &pa_font_16, 0);
        lv_obj_set_style_text_color(footer_label, lv_color_hex(0xf3f4f6), 0);
        bsp_lvgl_unlock();
    }
    bsp_display_backlight(70);
    (void)bsp_battery_init();
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
    ESP_ERROR_CHECK(esp_timer_start_periodic(clock, MINUTE_US));
    if (storage_corrupt || !storage_ready) ui = UI_ERROR;
    render();
}
