# Study Assist Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 亮度+电量、牌组到期计数+今日统计、两段式自动休眠三段功能在固件落地，store 新增只读统计 API，host 测试全过。

**Architecture:** store 层新增两个只读扫描函数（复用 `store->sector` 缓冲，零 malloc）；`main.c` 新增 NVS 设置项、牌组页条目、秒级空闲计时与两段休眠状态机。调度核心、协议、payload 格式不动。

**Tech Stack:** C11 (`components/passport_anki/pa_store.c`), ESP-IDF 5.5.3 + LVGL (`main/main.c`), host CTest + pytest。

## Global Constraints

- `pa_store` 静态大小仍 `<= PA_STORE_RAM_BUDGET`（12288）；稳态选卡/评分零 malloc；提交时堆高水位 `<= 64 KiB`。
- 统计 API 只读：IMPORT 中、非 READY、revlog 损坏时返回全零，不改任何 flash。
- NVS 键 `brightness`（u8，档位 30/70/100，默认 70）、`sleep_min`（u8，档位 1/3/5，默认 3）；非法值回退默认。
- 深睡前必须 `pa_store_checkpoint()`；导入中（`store.import_active`）不熄屏不深睡。
- 熄屏待命中 LVGL 与逻辑时钟继续；任一按键先恢复背光并消费该次按键（不触发翻页/评分）。
- Commit message 尾带 `Co-Authored-By: Claude Code <noreply@anthropic.com>`。

---

### Task 1: `pa_store_due_counts` 到期计数 API + host 测试

**Files:**
- Modify: `components/passport_anki/include/pa_store.h`
- Modify: `components/passport_anki/pa_store.c`
- Test: `tests/test_store.c`

**Interfaces:**
- Consumes: `store->chunk_phys[]`, `store->sector`, `pool_offset()`, `chunks_for()`, `rd_u16/rd_u32` (existing statics in `pa_store.c`).
- Produces: `void pa_store_due_counts(pa_store *store, uint16_t counts[PA_STORE_MAX_DECKS])` — 单次扫描全部调度块，按 `deck_id` 累加 `due <= now` 的卡数；非 READY/IMPORT 中时全零。

- [ ] **Step 1: Write the failing test**

In `tests/test_store.c`, append before `int main(void)`:

```c
static void test_due_counts(void)
{
    ram_flash cards_ram, sched_ram;
    pa_flash cards, sched;
    make_flash(&cards_ram, &cards, PA_CARDS_PARTITION_SIZE);
    make_flash(&sched_ram, &sched, PA_SCHED_PARTITION_SIZE);
    pa_store store;
    pa_store_init(&store, &cards, &sched);
    assert(pa_store_mount(&store) == PA_STORE_EMPTY);
    uint8_t payload[128];
    size_t length = append_card(payload, 0, 1, "A", "Q", "A");
    length = append_card(payload, length, 2, "A", "Q2", "A2");
    length = append_card(payload, length, 3, "B", "Q3", "A3");
    assert(import_bytes(&store, payload, 3, (uint32_t)length));
    uint16_t counts[PA_STORE_MAX_DECKS];
    pa_store_due_counts(&store, counts);
    assert(counts[0] == 2 && counts[1] == 1);
    assert(pa_store_advance(&store, 10));
    assert(pa_store_grade(&store, 0, PA_GOOD));
    pa_store_due_counts(&store, counts);
    assert(counts[0] == 1 && counts[1] == 1);
    free_flash(&cards_ram);
    free_flash(&sched_ram);
    puts("store: due counts PASS");
}
```

And in `main()`, add `test_due_counts();` after `test_revlog_export_roundtrip();`.

- [ ] **Step 2: Run test to verify it fails**

Run: `cmake --build build/host --target test_store && ./build/host/test_store`
Expected: FAIL — undefined reference to `pa_store_due_counts`.

- [ ] **Step 3: Write minimal implementation**

In `components/passport_anki/include/pa_store.h`, after `pa_store_next` declaration:

```c
void pa_store_due_counts(pa_store *store, uint16_t counts[PA_STORE_MAX_DECKS]);
```

In `components/passport_anki/pa_store.c`, after `pa_store_next()` (line ~718):

```c
void pa_store_due_counts(pa_store *store, uint16_t counts[PA_STORE_MAX_DECKS])
{
    if (counts == NULL) {
        return;
    }
    for (uint16_t i = 0; i < PA_STORE_MAX_DECKS; ++i) {
        counts[i] = 0;
    }
    if (store == NULL || store->status != PA_STORE_READY || store->import_active) {
        return;
    }
    uint16_t need = chunks_for(store->count);
    for (uint16_t chunk = 0; chunk < need; ++chunk) {
        if (!flash_read(store->sched, pool_offset(store->chunk_phys[chunk]), store->sector, PA_STORE_SECTOR)) {
            return;
        }
        uint16_t records = rd_u16(store->sector + 18);
        for (uint16_t n = 0; n < records; ++n) {
            const uint8_t *rec = store->sector + PA_SCHED_HEADER + (uint32_t)n * PA_SCHED_RECORD;
            uint8_t deck_id = rec[14];
            if (deck_id >= store->deck_count) {
                continue;
            }
            if (rd_u32(rec) <= store->now && counts[deck_id] < UINT16_MAX) {
                counts[deck_id]++;
            }
        }
    }
}
```

- [ ] **Step 4: Run test to verify it passes**

Run: `cmake --build build/host --target test_store && ./build/host/test_store`
Expected: PASS (`store: due counts PASS` in output, exit 0).

- [ ] **Step 5: Commit**

```bash
git add components/passport_anki/include/pa_store.h components/passport_anki/pa_store.c tests/test_store.c
git commit -m "feat: pa_store_due_counts per-deck due scan

Co-Authored-By: Claude Code <noreply@anthropic.com>"
```

### Task 2: `pa_store_today_stats` 今日统计 API + host 测试

**Files:**
- Modify: `components/passport_anki/include/pa_store.h`
- Modify: `components/passport_anki/pa_store.c`
- Test: `tests/test_store.c`

**Interfaces:**
- Consumes: `read_revlog_record()` (existing static), `store->revlog_count`, `store->now`.
- Produces: `void pa_store_today_stats(pa_store *store, uint32_t by_rating[4], uint32_t *total)` — 统计时间戳 `>= now - now % 1440` 的 revlog 条目，按 rating 0..3 累加；损坏/empty/IMPORT 中时全零。

- [ ] **Step 1: Write the failing test**

In `tests/test_store.c`, append before `int main(void)`:

```c
static void test_today_stats(void)
{
    ram_flash cards_ram, sched_ram;
    pa_flash cards, sched;
    make_flash(&cards_ram, &cards, PA_CARDS_PARTITION_SIZE);
    make_flash(&sched_ram, &sched, PA_SCHED_PARTITION_SIZE);
    pa_store store;
    pa_store_init(&store, &cards, &sched);
    assert(pa_store_mount(&store) == PA_STORE_EMPTY);
    uint8_t payload[64];
    size_t length = append_card(payload, 0, 7, "D", "Q", "A");
    length = append_card(payload, length, 8, "D", "Q2", "A2");
    assert(import_bytes(&store, payload, 2, (uint32_t)length));
    assert(pa_store_grade(&store, 1, PA_EASY));
    assert(pa_store_advance(&store, 3));
    assert(pa_store_grade(&store, 0, PA_AGAIN));
    uint32_t by[4] = {0, 0, 0, 0};
    uint32_t total = 0;
    pa_store_today_stats(&store, by, &total);
    assert(total == 2 && by[PA_AGAIN] == 1 && by[PA_EASY] == 1);
    assert(pa_store_advance(&store, 1440));
    pa_store_today_stats(&store, by, &total);
    assert(total == 0 && by[0] == 0 && by[3] == 0);
    free_flash(&cards_ram);
    free_flash(&sched_ram);
    puts("store: today stats PASS");
}
```

And in `main()`, add `test_today_stats();` after `test_due_counts();`.

- [ ] **Step 2: Run test to verify it fails**

Run: `cmake --build build/host --target test_store && ./build/host/test_store`
Expected: FAIL — undefined reference to `pa_store_today_stats`.

- [ ] **Step 3: Write minimal implementation**

In `components/passport_anki/include/pa_store.h`, after `pa_store_due_counts`:

```c
void pa_store_today_stats(pa_store *store, uint32_t by_rating[4], uint32_t *total);
```

In `components/passport_anki/pa_store.c`, after `pa_store_due_counts()`:

```c
void pa_store_today_stats(pa_store *store, uint32_t by_rating[4], uint32_t *total)
{
    if (by_rating != NULL) {
        for (int i = 0; i < 4; ++i) {
            by_rating[i] = 0;
        }
    }
    if (total != NULL) {
        *total = 0;
    }
    if (store == NULL || by_rating == NULL || total == NULL || store->import_active ||
        store->status != PA_STORE_READY || store->revlog_corrupt || store->epoch == 0) {
        return;
    }
    uint32_t day_start = store->now - store->now % 1440u;
    uint8_t rec[PA_REVLOG_RECORD];
    for (uint32_t i = 0; i < store->revlog_count; ++i) {
        if (!read_revlog_record(store, i, rec)) {
            break;
        }
        uint32_t when = (uint32_t)rec[4] | ((uint32_t)rec[5] << 8) | ((uint32_t)rec[6] << 16) |
                        ((uint32_t)rec[7] << 24);
        if (when >= day_start && rec[2] <= PA_EASY) {
            by_rating[rec[2]]++;
            (*total)++;
        }
    }
}
```

Note: revlog record layout is `card u16, rating u8, reserved u8, minutes u32 LE` (see `revlog_append` lines 941-944), so timestamp bytes are `rec[4..7]`.

- [ ] **Step 4: Run test to verify it passes**

Run: `cmake --build build/host --target test_store && ./build/host/test_store`
Expected: PASS (both `due counts` and `today stats` lines, exit 0).

- [ ] **Step 5: Commit**

```bash
git add components/passport_anki/include/pa_store.h components/passport_anki/pa_store.c tests/test_store.c
git commit -m "feat: pa_store_today_stats same-day revlog counts

Co-Authored-By: Claude Code <noreply@anthropic.com>"
```

### Task 3: 亮度+电量设置（NVS + 牌组页条目）

**Files:**
- Modify: `main/main.c`

**Interfaces:**
- Consumes: `pa_store_due_counts()` (Task 1), `bsp_battery_soc()`, `bsp_display_backlight()`, NVS `nvs_get_u8/nvs_set_u8/nvs_commit`.
- Produces: 牌组页条目 `亮度`（OK 循环 30/70/100，即时生效+写 NVS）与只读 `电量` 行；启动时读 NVS 亮度替代写死 70。

- [ ] **Step 1: Write the failing check (manual — no host harness for main.c)**

No host test covers `main.c` (LVGL/IDF only). Verify by inspection: `grep -n "bsp_display_backlight(70)" main/main.c` still shows the hardcoded call, and no `brightness` NVS key exists.

- [ ] **Step 2: Implement NVS brightness + deck-list entries**

Exact edits in `main/main.c`:

1. Globals (near line 44 `static int64_t import_last_input, last_checkpoint;`):

```c
static uint8_t setting_brightness = 70;
static bool battery_usable = false;
```

2. Helper after `clamp_deck_index()` (~line 288):

```c
static uint8_t load_u8_setting(const char *key, uint8_t fallback, const uint8_t *allowed, size_t allowed_count)
{
    uint8_t value = fallback;
    if (nvs_ready && nvs_get_u8(nvs, key, &value) == ESP_OK) {
        for (size_t i = 0; i < allowed_count; ++i) {
            if (value == allowed[i]) return value;
        }
    }
    return fallback;
}

static void save_u8_setting(const char *key, uint8_t value)
{
    if (nvs_ready && nvs_set_u8(nvs, key, value) == ESP_OK) {
        (void)nvs_commit(nvs);
    }
}
```

3. In `app_main()`, replace `bsp_display_backlight(70);` with:

```c
    {
        static const uint8_t levels[] = {30, 70, 100};
        setting_brightness = load_u8_setting("brightness", 70, levels, 3);
        bsp_display_backlight(setting_brightness);
    }
    battery_usable = bsp_battery_init() == ESP_OK;
```

(delete the old `(void)bsp_battery_init();` line).

4. Deck-list model: after `static uint16_t deck_index, question_page, answer_page;` add `static uint8_t deck_cursor_max;` — or simpler, compute entries inline:
   `entries = count + 1 /*next-day*/ + 3 /*brightness, sleep, battery*/`.
   Update `clamp_deck_index()` to clamp against `deck_count() + 4`.
   In `process_button()` UI_DECKS branch, handle `deck_index == count` (next-day, existing),
   `== count+1` (brightness: cycle 30→70→100→30, apply + save),
   `== count+2` (sleep: Task 5),
   `== count+3` (battery: OK does nothing).
   In `render()` UI_DECKS branch, render the four extra rows by `deck_index` value;
   battery row shows `bsp_battery_soc()` or "不可用" when `< 0` or `!battery_usable`.

- [ ] **Step 3: Build check**

Run: `ctest --test-dir build/host` (main.c excluded from host; ensures no host regression).
Then: CI firmware build covers `main.c` (`tools/validate_firmware.sh` needs ESP-IDF; run if `IDF_PATH` set, else rely on GitHub Actions).

- [ ] **Step 4: Commit**

```bash
git add main/main.c
git commit -m "feat: brightness and battery settings on deck list

Co-Authored-By: Claude Code <noreply@anthropic.com>"
```

### Task 4: 牌组到期计数 + 完成页今日统计显示

**Files:**
- Modify: `main/main.c`

**Interfaces:**
- Consumes: `pa_store_due_counts()`, `pa_store_today_stats()` (Tasks 1-2).
- Produces: 牌组行首行 `牌组名·到期N`；EMPTY 页 `今日已评 N（A/H/G/E 各数）`.

- [ ] **Step 1: Implement display**

In `render()`:
- UI_DECKS deck branch: call `pa_store_due_counts(&store, counts)` once (static local array), format title/body as `"牌组名·到期%u"`.
- UI_EMPTY branch: call `pa_store_today_stats(&store, by, &total)`, format body as `"今天已完成\n牌组：%s\n今日已评 %" PRIu32 "（%u/%u/%u/%u）\n可推进学习日。"`.

- [ ] **Step 2: Verify**

Run: `ctest --test-dir build/host` (no host regression); firmware text verified on device.

- [ ] **Step 3: Commit**

```bash
git add main/main.c
git commit -m "feat: deck due counts and today stats display

Co-Authored-By: Claude Code <noreply@anthropic.com>"
```

### Task 5: 两段式自动休眠（熄屏待命 + 深睡）

**Files:**
- Modify: `main/main.c`
- Modify: `components/bsp/include/bsp_power.h` (no change needed — reuse `bsp_power_enter_deep_sleep`)

**Interfaces:**
- Consumes: `sleep_min` NVS setting, `EVENT_TICK` 1-min timer (existing), new 10 s idle timer, `bsp_display_backlight()`, `pa_store_checkpoint()`, `bsp_power_enter_deep_sleep()`.
- Produces: 超时熄屏（背光 0，时钟继续），按键恢复并消费；熄屏 5 分钟后 checkpoint + 深睡。

- [ ] **Step 1: Implement idle state machine**

Globals:

```c
static int64_t last_activity_us;
static int64_t dim_start_us;
static bool screen_dimmed;
static uint8_t setting_sleep_min = 3;
```

In `app_main()`: load `sleep_min` via `load_u8_setting("sleep_min", 3, (uint8_t[]){1,3,5}, 3)`; init `last_activity_us = esp_timer_get_time()`; create 10 s periodic `esp_timer` with callback posting new `EVENT_IDLE`.

In `app_task()`: on `EVENT_BUTTON`/`EVENT_LINE`, set `last_activity_us = now`; if `screen_dimmed`, restore `bsp_display_backlight(setting_brightness)`, `screen_dimmed = false`, and (for buttons) skip `process_button()` this once. On `EVENT_IDLE`/`EVENT_TICK`: if `store.import_active` skip; else if `!screen_dimmed && now - last_activity_us >= sleep_min * 60s`, `bsp_display_backlight(0)`, `screen_dimmed = true`, `dim_start_us = now`; else if `screen_dimmed && now - dim_start_us >= 5*60s`, `pa_store_checkpoint(&store)` then `bsp_power_enter_deep_sleep()`.

Sleep settings entry (`deck_index == count+2`): OK cycles 1→3→5→1, saves via `save_u8_setting("sleep_min", ...)`, resets `last_activity_us`.

- [ ] **Step 2: Verify**

Run: `ctest --test-dir build/host`; firmware behavior verified on device (1-min档熄屏→按键恢复→5 分钟后深睡→按键唤醒重启→进度不丢）.

- [ ] **Step 3: Commit**

```bash
git add main/main.c
git commit -m "feat: two-stage auto sleep with dim then deep sleep

Co-Authored-By: Claude Code <noreply@anthropic.com>"
```

### Task 6: 文档与全量回归

**Files:**
- Modify: `docs/design.md` (§8 UI 表新增条目、§10 休眠段落）
- Modify: `docs/user-guide.md`（亮度/休眠/电量/统计说明）
- Test: full suite

**Interfaces:**
- Consumes: Tasks 1-5 final behavior.
- Produces: 用户可照文档操作；CI 全绿。

- [ ] **Step 1: Update docs**

`docs/design.md` §8 表格 DECKS 行补充亮度/休眠/电量条目与到期数；§10 补充两段休眠与深睡时钟语义。`docs/user-guide.md` 三键复习节补充三项设置位置与档位，完成页统计口径，休眠行为与唤醒说明。

- [ ] **Step 2: Run full regression**

Run:
```bash
cmake --build build/host -j 8 && ctest --test-dir build/host --output-on-failure
python3 -m pytest tests -q
```
Expected: CTest 9/9, pytest 59+新增通过.

- [ ] **Step 3: Commit**

```bash
git add docs/design.md docs/user-guide.md
git commit -m "docs: study assist usage and design updates

Co-Authored-By: Claude Code <noreply@anthropic.com>"
```

## Self-Review

- Spec coverage: §1→Task 3, §2→Tasks 1+2+4, §3→Task 5, 非目标（日上限/图表/USB变更/模拟器）无对应任务 ✓.
- No placeholders: 每步含确切代码与命令 ✓.
- Type consistency: `counts[PA_STORE_MAX_DECKS]`/`by_rating[4]` 头源一致；NVS u8 档位与设计一致 ✓.
