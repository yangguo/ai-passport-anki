#include "bsp_battery.h"
#include "bsp_button.h"
#include "bsp_display.h"
#include "bsp_power.h"
#include "bsp_pins.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "pa_layout.h"
#include "pa_import.h"
#include "pa_protocol.h"
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
static pa_collection live, staging;
static pa_session session;
static uint8_t snapshot[PA_SNAPSHOT_MAX];
static char deck_names[PA_MAX_CARDS][PA_DECK_BYTES];
static uint16_t deck_count, deck_index;
static nvs_handle_t nvs;
static bool storage_ready, storage_corrupt;
static pa_import_tx import_tx;
static int64_t import_last_input, last_checkpoint;
static int64_t last_clock_us;

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
static uint16_t question_page, answer_page;
static bool confirming_day, grade_save_failed;
static const char *last_error = "";
static lv_obj_t *title_label, *body_label, *footer_label;

static bool persist(const pa_collection *collection) {
    if (!storage_ready) return false;
    size_t size = pa_encode(collection, snapshot, sizeof(snapshot));
    if (!size || nvs_set_blob(nvs, "snapshot", snapshot, size) != ESP_OK ||
        nvs_commit(nvs) != ESP_OK) return false;
    return true;
}

static bool persist_import(const pa_collection *collection, void *user) {
    (void)user;
    return persist(collection);
}

static bool load_snapshot(void) {
    size_t size = 0;
    esp_err_t err = nvs_get_blob(nvs, "snapshot", NULL, &size);
    if (err == ESP_ERR_NVS_NOT_FOUND) { pa_init(&live); return persist(&live); }
    if (err != ESP_OK || size > sizeof(snapshot)) return false;
    if (nvs_get_blob(nvs, "snapshot", snapshot, &size) != ESP_OK ||
        !pa_decode(&staging, snapshot, size)) return false;
    live = staging;
    return true;
}

static bool collection_save_then_swap(const pa_collection *next) {
    if (!persist(next)) return false;
    live = *next;
    return true;
}

static uint16_t glyph_width(uint32_t cp, void *user) {
    (void)user;
    lv_font_glyph_dsc_t glyph;
    return lv_font_get_glyph_dsc(&pa_font_16, &glyph, cp, 0) && !glyph.is_placeholder
               ? glyph.adv_w : 0;
}

static bool card_supported(const pa_card *card) {
    pa_text_page pages[PA_LAYOUT_MAX_PAGES];
    const size_t width = BSP_LCD_W - 36;
    return pa_layout_pages(card->deck, strlen(card->deck), width, 4,
                           glyph_width, NULL, pages) != 0 &&
           pa_layout_pages(card->front, strlen(card->front), width, 4,
                           glyph_width, NULL, pages) != 0 &&
           pa_layout_pages(card->back, strlen(card->back), width, 4,
                           glyph_width, NULL, pages) != 0;
}

static bool validate_import_card(const pa_card *card, void *user) {
    (void)user;
    return card_supported(card);
}

static size_t pages_for(const char *text, pa_text_page pages[PA_LAYOUT_MAX_PAGES]) {
    return pa_layout_pages(text, strlen(text), BSP_LCD_W - 36, 4,
                           glyph_width, NULL, pages);
}

static void collect_decks(void) {
    deck_count = 0;
    for (uint16_t i = 0; i < live.count; ++i) {
        bool found = false;
        for (uint16_t j = 0; j < deck_count; ++j)
            if (strcmp(live.cards[i].deck, deck_names[j]) == 0) { found = true; break; }
        if (!found && deck_count < PA_MAX_CARDS)
            strcpy(deck_names[deck_count++], live.cards[i].deck);
    }
    if (deck_index >= deck_count) deck_index = deck_count ? deck_count - 1 : 0;
}

static const char *deck_selected(void) {
    return deck_index < deck_count ? deck_names[deck_index] : NULL;
}

static const char *rating_name(pa_rating rating) {
    static const char *names[] = {"Again", "Hard", "Good", "Easy"};
    return rating <= PA_EASY ? names[rating] : "Good";
}

static void render(void) {
    if (!title_label || !body_label || !footer_label || !bsp_lvgl_lock(100)) return;
    char title[96], body[PA_LINE_MAX + 1], footer[160];
    snprintf(title, sizeof(title), "Anki  学习日 %" PRIu32, live.now / 1440U);
    body[0] = '\0';
    snprintf(footer, sizeof(footer), "UP/DOWN 选择   OK 确认");
    if (ui == UI_DECKS) {
        if (deck_count == 0) {
            snprintf(body, sizeof(body), "还没有牌组\n通过 USB 导入 TSV\n\n学习日：%" PRIu32,
                     live.now / 1440U);
            snprintf(footer, sizeof(footer), "OK可推进学习日；长按返回");
        } else if (deck_index == deck_count) {
            snprintf(body, sizeof(body), "推进到下一学习日？\n\n当前学习日：%" PRIu32,
                     live.now / 1440U);
            snprintf(footer, sizeof(footer), "UP/DOWN选择   OK确认推进");
        } else {
            snprintf(body, sizeof(body), "%s\n牌组 %u/%u\nOK 开始复习",
                     deck_count ? deck_selected() : "还没有牌组",
                     deck_count ? deck_index + 1 : 0, deck_count);
            if (deck_count == 0) snprintf(footer, sizeof(footer), "连接电脑后导入 TSV 牌组");
        }
    } else if (ui == UI_QUESTION || ui == UI_ANSWER || ui == UI_RATING) {
        const pa_card *card = &live.cards[session.current];
        const char *text = ui == UI_QUESTION ? card->front : card->back;
        pa_text_page pages[PA_LAYOUT_MAX_PAGES];
        size_t count = pages_for(text, pages);
        uint16_t page = ui == UI_QUESTION ? question_page : answer_page;
        if (!count) {
            snprintf(title, sizeof(title), "字体缺字");
            snprintf(body, sizeof(body), "这张卡含有当前字体不支持的文字。需重新导入受支持的牌组。");
            snprintf(footer, sizeof(footer), "长按 OK 返回");
            goto draw_labels;
        }
        if (page >= count) page = 0;
        if (!pa_layout_format_page(text, strlen(text), pages[page], BSP_LCD_W - 36,
                                   glyph_width, NULL, body, sizeof(body))) {
            snprintf(title, sizeof(title), "字体缺字");
            snprintf(body, sizeof(body), "这张卡含有当前字体不支持的文字。需重新导入受支持的牌组。");
            snprintf(footer, sizeof(footer), "长按 OK 返回");
            goto draw_labels;
        }
        snprintf(title, sizeof(title), "%s", card->deck);
        if (ui == UI_QUESTION) snprintf(footer, sizeof(footer), "D%" PRIu32 " C%u/%u Q%u/%u UP/DOWN 翻面OK",
                                        live.now / 1440U, session.current + 1, live.count,
                                        page + 1, (unsigned)count);
        else if (ui == UI_ANSWER) snprintf(footer, sizeof(footer), "D%" PRIu32 " C%u/%u A%u/%u UP/DOWN 评分OK",
                                          live.now / 1440U, session.current + 1, live.count,
                                          page + 1, (unsigned)count);
        else if (grade_save_failed) snprintf(footer, sizeof(footer), "存档失败；OK重试  长按返回");
        else snprintf(footer, sizeof(footer), "%s  D%" PRIu32 " C%u/%u UP/DOWN 保存OK",
                      rating_name(selected_rating), live.now / 1440U, session.current + 1, live.count);
    } else if (ui == UI_EMPTY) {
        snprintf(body, sizeof(body), "今天已完成\n牌组：%s\n可推进学习日。",
                 deck_selected() ? deck_selected() : "全部");
        snprintf(footer, sizeof(footer), "OK重新检查   长按返回牌组");
    } else if (ui == UI_CONFIRM_DAY) {
        snprintf(body, sizeof(body), "推进学习日 +1？\n\n当前选择：%s\n设备时间保持不变。",
                 confirming_day ? "确认" : "取消");
        snprintf(footer, sizeof(footer), "UP/DOWN选择  OK执行  长按返回");
    } else if (ui == UI_IMPORTING) {
        snprintf(body, sizeof(body), "正在导入牌组\n已接收 %u / %u 张\n长按 OK 可取消。",
                 import_tx.received, import_tx.expected);
        snprintf(footer, sizeof(footer), "请保持 USB 连接");
    } else {
        snprintf(body, sizeof(body), "存档或设备错误\n%s\n长按 OK 返回。", last_error);
        snprintf(footer, sizeof(footer), storage_corrupt ? "需显式导入新牌组" :
                 (!storage_ready ? "NVS 初始化失败；未擦除" : ""));
    }
draw_labels:
    lv_label_set_text(title_label, title);
    lv_label_set_text(body_label, body);
    lv_label_set_text(footer_label, footer);
    bsp_lvgl_unlock();
}

static void abort_import(void) {
    pa_import_abort(&import_tx);
    if (ui == UI_IMPORTING) ui = UI_DECKS;
}

static void reply_error(const char *code) { printf("ERR %s\n", code); fflush(stdout); }

static void handle_line(char *line) {
    pa_protocol_command command;
    if (!pa_protocol_parse(line, strlen(line), &command)) { reply_error("COMMAND"); return; }
    switch (command.kind) {
    case PA_PROTOCOL_INFO:
        printf("OK INFO version=1 cards=%u day=%" PRIu32 "\n", live.count, live.now / 1440U);
        fflush(stdout);
        return;
    case PA_PROTOCOL_BEGIN:
        if (import_tx.active) { reply_error("TRANSACTION"); return; }
        if (!storage_ready) { reply_error("STORAGE"); return; }
        if (!pa_import_begin(&import_tx, &staging, &live, (uint16_t)command.value)) { reply_error("TRANSACTION"); return; }
        import_last_input = esp_timer_get_time();
        ui = UI_IMPORTING;
        render();
        printf("OK BEGIN\n");
        fflush(stdout);
        return;
    case PA_PROTOCOL_CARD:
        if (!import_tx.active || import_tx.invalid) { reply_error("TRANSACTION"); return; }
        import_last_input = esp_timer_get_time();
        if (!pa_import_add(&import_tx, &staging, command.id, command.deck, command.front,
                           command.back, validate_import_card, NULL)) {
            reply_error("CARD");
            render();
            return;
        }
        render();
        printf("OK CARD\n");
        fflush(stdout);
        return;
    case PA_PROTOCOL_COMMIT:
        if (!import_tx.active || import_tx.invalid || import_tx.received != import_tx.expected) { reply_error("TRANSACTION"); return; }
        if (!pa_import_commit(&import_tx, &staging, &live,
                              persist_import, NULL)) {
            reply_error("STORAGE");
            ui = storage_corrupt ? UI_ERROR : UI_DECKS;
            render();
            return;
        }
        storage_corrupt = false;
        collect_decks();
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
        if (storage_corrupt || import_tx.active || day < live.now / 1440U || day > UINT32_MAX / 1440U) { reply_error("DAY"); return; }
        staging = live;
        const uint32_t target = day * 1440U;
        if (target > staging.now && !pa_advance(&staging, target - staging.now)) { reply_error("DAY"); return; }
        if (!collection_save_then_swap(&staging)) { reply_error("STORAGE"); return; }
        printf("OK DAY\n");
        fflush(stdout);
        render();
        return;
    }
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
    pa_begin(&session, &live, deck_selected());
    if (session.current == PA_NO_CARD) ui = UI_EMPTY;
    else { ui = UI_QUESTION; question_page = answer_page = 0; }
}

static void do_grade(void) {
    if (session.current == PA_NO_CARD || !session.revealed) return;
    const uint16_t current = session.current;
    pa_card old = live.cards[current];
    const pa_session old_session = session;
    if (!pa_grade(&live, &session, selected_rating)) {
        live.cards[current] = old; session = old_session; grade_save_failed = true; return;
    }
    if (!persist(&live)) {
        live.cards[current] = old; session = old_session; grade_save_failed = true; return;
    }
    grade_save_failed = false;
    begin_deck();
}

static void process_button(const event_t *event) {
    const bool long_press = event->button_event == BSP_BTN_LONG;
    if (long_press && event->button == BSP_BTN_OK) {
        if (import_tx.active) { abort_import(); ui = storage_corrupt ? UI_ERROR : UI_DECKS; }
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
    if (ui == UI_DECKS) {
        const uint16_t entries = deck_count + 1;
        if (event->button == BSP_BTN_UP) deck_index = (deck_index + entries - 1) % entries;
        else if (event->button == BSP_BTN_DOWN) deck_index = (deck_index + 1) % entries;
        else if (deck_index == deck_count) { confirming_day = true; ui = UI_CONFIRM_DAY; }
        else begin_deck();
    } else if (ui == UI_CONFIRM_DAY) {
        if (event->button == BSP_BTN_UP || event->button == BSP_BTN_DOWN) confirming_day = !confirming_day;
        else if (!confirming_day) ui = UI_DECKS;
        else {
            staging = live;
            if (staging.now > UINT32_MAX - 1440U || !pa_advance(&staging, 1440U) || !collection_save_then_swap(&staging)) {
                ui = UI_ERROR; last_error = "学习日无法保存";
            } else { confirming_day = false; ui = UI_DECKS; }
        }
    } else if (ui == UI_QUESTION || ui == UI_ANSWER) {
        const char *text = ui == UI_QUESTION ? live.cards[session.current].front : live.cards[session.current].back;
        pa_text_page pages[PA_LAYOUT_MAX_PAGES]; size_t count = pages_for(text, pages);
        uint16_t *page = ui == UI_QUESTION ? &question_page : &answer_page;
        if (event->button == BSP_BTN_UP && *page > 0) --*page;
        else if (event->button == BSP_BTN_DOWN && *page + 1 < count) ++*page;
        else if (event->button == BSP_BTN_OK && ui == UI_QUESTION) { if (pa_flip(&session)) { ui = UI_ANSWER; answer_page = 0; } }
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
            char *line = event.line;
            handle_line(line);
            if (ui != UI_ERROR && ui != UI_IMPORTING) render();
        } else if (event.type == EVENT_TICK) {
            int64_t now = esp_timer_get_time();
            if (import_tx.active && now - import_last_input >= IMPORT_TIMEOUT_US) {
                abort_import(); ui = storage_corrupt ? UI_ERROR : UI_DECKS; reply_error("TIMEOUT"); render();
            } else if (storage_ready && !storage_corrupt) {
                uint32_t elapsed = (uint32_t)((now - last_clock_us) / MINUTE_US);
                if (elapsed && pa_advance(&live, elapsed)) {
                    last_clock_us += (int64_t)elapsed * MINUTE_US;
                    if (now - last_checkpoint >= 5 * MINUTE_US && persist(&live)) last_checkpoint = now;
                    if (ui == UI_EMPTY && pa_next(&live, deck_selected()) != PA_NO_CARD) begin_deck();
                    render();
                }
            }
        }
    }
}

void app_main(void) {
    ESP_ERROR_CHECK(bsp_power_wait_for_wake_release());
    esp_err_t err = nvs_flash_init();
    if (err == ESP_OK) err = nvs_open("pa_anki", NVS_READWRITE, &nvs);
    if (err == ESP_OK) {
        storage_ready = true;
        if (!load_snapshot()) { storage_corrupt = true; last_error = "存档损坏或无法读取"; }
    } else { last_error = "NVS 初始化失败；未擦除任何数据"; }
    collect_decks();
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
        xTaskCreate(app_task, "anki_app", 12288, NULL, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "app task creation failed");
        return;
    }
    esp_timer_handle_t clock;
    const esp_timer_create_args_t timer_args = {.callback = clock_callback, .name = "anki_clock"};
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &clock));
    last_clock_us = esp_timer_get_time();
    ESP_ERROR_CHECK(esp_timer_start_periodic(clock, MINUTE_US));
    if (storage_corrupt || !storage_ready) ui = UI_ERROR;
    render();
}
