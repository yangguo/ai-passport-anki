# Study Boost 实现计划（每日新卡限额 / 同级打乱 / 剩余显示 / 全自动播放）

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 在不改 flash 格式、不改调度算法、不改 USB 协议的前提下，实现 spec（`docs/superpowers/specs/2026-11-01-study-boost-design.md`）的四个功能：每日新卡限额、同级内随机打乱、剩余显示、全自动播放。

**Architecture:** store 层加只读查询 + RAM-only 策略状态（limit/done/shuffle/seed），`pa_store_next` 内部实现限额跳过与水库采样；main.c 加 NVS 设置、菜单行、页脚剩余、EVENT_IDLE 自动播放引擎。flash 布局、调度表、USB 协议一律不动。

**Tech Stack:** C11（`components/passport_anki/pa_store.c`、`include/pa_store.h`）、ESP-IDF main.c（LVGL 菜单/事件循环）、host 单测（ctest + ASan/UBSan）、pytest（回归）。

## Global Constraints

- `sizeof(pa_store) <= PA_STORE_RAM_BUDGET`（12288 字节）必须成立，`tests/test_store.c` 已有 `assert`，新增字段后保持通过。
- flash 格式不动：不加 superblock 字段、不做版本迁移；`new_limit/new_done/shuffle/autoplay` 只存 NVS，`rng_state` 纯 RAM。
- NVS 默认值（白名单校验，非法回默认）：`new_limit=20`（允许 0/10/20/50，0=不限）、`shuffle=0`、`autoplay=0`、`new_done=0`、`new_day=0`。
- 菜单顺序（`settings_base` 起）：下一学习日(+0)、亮度(+1)、休眠(+2)、自动翻页(+3)、电量(+4 只读)、新卡限额(+5)、打乱(+6)、未来3天预报(+7 只读)、自动播放(+8)；`DECK_EXTRA_ROWS` 由 5 改为 9。
- 自动播放：QUESTION/ANSWER 按 `autopage_sec`（关则按 3 秒）翻页→翻面→自动记 Good；Again 短等待只等 ≤600 秒并显示秒数；任一按键退出自动播且该键被消费（不翻页不评分）；播放中抑制熄屏/深睡（与导入同级）；重启后 `autoplay_active=false`。
- 限额只 gate NEW：LEARNING/REVIEW 到期不限；`new_done` 只在 NEW 卡评分成功后 +1。
- Host 验证命令（以仓库根目录为准）：
  - `cmake -S . -B build/host -DANKI_HOST=ON -DANKI_SANITIZERS=ON`
  - `cmake --build build/host`
  - `ctest --test-dir build/host --output-on-failure`
  - `python3 -m pytest -q tests/test_deck.py tests/test_anki_sync.py tests/test_simulator.py tests/test_image_pad.py`
- main.c 是固件代码，host 编译不了；main.c 改动靠仔细对照本计划的代码块 + CI 固件构建验证（`tools/validate_firmware.sh`，跑在有 IDF 的机器 / CI 上）。
- 不提交 `n3.apkg`、不提交 `--map` JSON；`upload` 仍是破坏性操作，本计划不涉及 upload。

---

### Task 1: store 策略状态 + `pa_store_is_new`

**Files:**
- Modify: `components/passport_anki/include/pa_store.h`
- Modify: `components/passport_anki/pa_store.c`
- Test: `tests/test_store.c`

**Interfaces:**
- Consumes: 现有 `pa_store` 结构、`read_record`（pa_store.c 内部静态函数）、`PA_NEW`。
- Produces: `void pa_store_set_limits(pa_store *store, uint8_t new_limit, uint16_t new_done)`；`void pa_store_set_shuffle(pa_store *store, bool shuffle_on, uint32_t seed)`；`bool pa_store_is_new(pa_store *store, uint16_t index)`；struct 新增 `new_limit/shuffle_on/new_done/rng_state` 四个字段。

- [ ] **Step 1: 声明新 API**

在 `components/passport_anki/include/pa_store.h` 中，找到：
```c
void pa_store_due_counts(pa_store *store, uint16_t counts[PA_STORE_MAX_DECKS]);
void pa_store_today_stats(pa_store *store, uint32_t by_rating[4], uint32_t *total);
```
在其后追加：
```c
/* Daily NEW-card policy (RAM only, persisted by the caller in NVS).
 * new_limit 0 means unlimited. new_done counts NEW grades today. */
void pa_store_set_limits(pa_store *store, uint8_t new_limit, uint16_t new_done);
void pa_store_set_shuffle(pa_store *store, bool shuffle_on, uint32_t seed);
bool pa_store_is_new(pa_store *store, uint16_t index);
void pa_store_new_due_counts(pa_store *store, uint16_t counts[PA_STORE_MAX_DECKS]);
void pa_store_forecast(pa_store *store, uint16_t buckets[3]);
bool pa_store_earliest_due(pa_store *store, const char *deck, uint32_t *due_out);
```

- [ ] **Step 2: struct 加四个字段**

在 `include/pa_store.h` 的 `struct pa_store` 中，找到：
```c
    uint32_t revlog_count;
    uint32_t revlog_dropped;
```
在其后插入：
```c
    uint8_t new_limit;
    uint8_t shuffle_on;
    uint16_t new_done;
    uint32_t rng_state;
```

- [ ] **Step 3: `pa_store_init` 初始化四个字段**

Read `components/passport_anki/pa_store.c` 的 `pa_store_init` 函数，在其字段初始化处追加：
```c
    store->new_limit = 0;
    store->shuffle_on = 0;
    store->new_done = 0;
    store->rng_state = 0x9E3779B9u;
```

- [ ] **Step 4: 实现 setter + `pa_store_is_new`**

在 `pa_store.c` 中 `pa_store_due_counts` 附近追加：
```c
void pa_store_set_limits(pa_store *store, uint8_t new_limit, uint16_t new_done)
{
    if (store == NULL) {
        return;
    }
    store->new_limit = new_limit;
    store->new_done = new_done;
}

void pa_store_set_shuffle(pa_store *store, bool shuffle_on, uint32_t seed)
{
    if (store == NULL) {
        return;
    }
    store->shuffle_on = shuffle_on ? 1u : 0u;
    if (seed != 0) {
        store->rng_state = seed;
    }
}

bool pa_store_is_new(pa_store *store, uint16_t index)
{
    if (store == NULL || store->status != PA_STORE_READY || index >= store->count) {
        return false;
    }
    uint8_t rec[PA_SCHED_RECORD];
    if (!read_record(store, index, rec)) {
        return false;
    }
    return rec[12] == PA_NEW;
}
```

- [ ] **Step 5: 写 failing 测试**

在 `tests/test_store.c` 末尾（`test_today_stats` 之后、`main` 之前）追加：
```c
static void test_limits_and_is_new(void)
{
    ram_flash cards_ram, sched_ram;
    pa_flash cards, sched;
    make_flash(&cards_ram, &cards, PA_CARDS_PARTITION_SIZE);
    make_flash(&sched_ram, &sched, PA_SCHED_PARTITION_SIZE);
    pa_store store;
    pa_store_init(&store, &cards, &sched);
    assert(pa_store_mount(&store) == PA_STORE_EMPTY);
    uint8_t payload[64];
    size_t length = append_card(payload, 0, 1, "D", "Q", "A");
    length = append_card(payload, length, 2, "D", "Q2", "A2");
    assert(import_bytes(&store, payload, 2, (uint32_t)length));
    assert(!pa_store_is_new(NULL, 0));
    assert(!pa_store_is_new(&store, 99));
    assert(pa_store_is_new(&store, 0));
    assert(pa_store_is_new(&store, 1));
    pa_store_set_limits(NULL, 1, 1);
    pa_store_set_shuffle(NULL, true, 42);
    pa_store_set_limits(&store, 20, 0);
    assert(pa_store_grade(&store, 0, PA_GOOD));
    assert(!pa_store_is_new(&store, 0));
    assert(pa_store_is_new(&store, 1));
    free_flash(&cards_ram);
    free_flash(&sched_ram);
    puts("store: limits setters and is_new PASS");
}
```
并在 `main` 中 `test_today_stats();` 之后加一行 `test_limits_and_is_new();`。

- [ ] **Step 6: 运行测试验证失败**

Run: `cmake -S . -B build/host -DANKI_HOST=ON -DANKI_SANITIZERS=ON && cmake --build build/host && ./build/host/test_store`
Expected: FAIL（`pa_store_set_limits` 等未定义，链接失败）。

- [ ] **Step 7: 实现后运行测试验证通过**

Run: 同上命令。
Expected: PASS，且输出含 `store: limits setters and is_new PASS`；`sizeof(pa_store)` 断言仍通过。

- [ ] **Step 8: Commit**

```bash
git add components/passport_anki/include/pa_store.h components/passport_anki/pa_store.c tests/test_store.c
git commit -m "feat(store): NEW-limit policy state and is_new query

Co-Authored-By: Claude Code <noreply@anthropic.com>"
```

### Task 2: NEW 限额选卡 + `pa_store_new_due_counts`

**Files:**
- Modify: `components/passport_anki/pa_store.c`
- Test: `tests/test_store.c`

**Interfaces:**
- Consumes: Task 1 的 `new_limit/new_done` 字段。
- Produces: `pa_store_next` 在限额耗尽时跳过 NEW；`pa_store_new_due_counts`（只统计 `stage==PA_NEW` 且 `due<=now`）。

- [ ] **Step 1: `pa_store_next` 非打乱路径加限额跳过**

Read `pa_store.c` 的 `pa_store_next`，在现有候选过滤处（`due > store->now` 的 `continue` 之后）加 NEW 限额判断。完整改后循环体关键部分为：
```c
        for (uint16_t n = 0; n < records; ++n) {
            const uint8_t *rec = store->sector + PA_SCHED_HEADER + (uint32_t)n * PA_SCHED_RECORD;
            if (rec[14] != (uint8_t)deck_id) {
                continue;
            }
            uint32_t due = rd_u32(rec);
            if (due > store->now) {
                continue;
            }
            if (rec[12] == PA_NEW && store->new_limit != 0 && store->new_done >= store->new_limit) {
                continue;
            }
            unsigned priority = rec[12] == PA_LEARNING ? 0u : rec[12] == PA_REVIEW ? 1u : 2u;
            ...
```
（`...` 保持原有 best 比较逻辑不变。）

- [ ] **Step 2: 实现 `pa_store_new_due_counts`**

在 `pa_store_due_counts` 实现之后追加（镜像其结构，多一个 `rec[12] == PA_NEW` 条件）：
```c
void pa_store_new_due_counts(pa_store *store, uint16_t counts[PA_STORE_MAX_DECKS])
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
        if (!flash_read(store->sched, pool_offset(store->chunk_phys[chunk]), store->sector,
                        PA_STORE_SECTOR)) {
            return;
        }
        uint16_t records = rd_u16(store->sector + 18);
        for (uint16_t n = 0; n < records; ++n) {
            const uint8_t *rec = store->sector + PA_SCHED_HEADER + (uint32_t)n * PA_SCHED_RECORD;
            uint8_t deck_id = rec[14];
            if (deck_id >= store->deck_count) {
                continue;
            }
            if (rec[12] == PA_NEW && rd_u32(rec) <= store->now && counts[deck_id] < UINT16_MAX) {
                counts[deck_id]++;
            }
        }
    }
}
```

- [ ] **Step 3: 写 failing 测试**

在 `tests/test_store.c` 追加：
```c
static void test_new_limit_skip(void)
{
    ram_flash cards_ram, sched_ram;
    pa_flash cards, sched;
    make_flash(&cards_ram, &cards, PA_CARDS_PARTITION_SIZE);
    make_flash(&sched_ram, &sched, PA_SCHED_PARTITION_SIZE);
    pa_store store;
    pa_store_init(&store, &cards, &sched);
    assert(pa_store_mount(&store) == PA_STORE_EMPTY);
    uint8_t payload[64];
    size_t length = append_card(payload, 0, 1, "D", "Q", "A");
    length = append_card(payload, length, 2, "D", "Q2", "A2");
    assert(import_bytes(&store, payload, 2, (uint32_t)length));
    /* Card 0: NEW -> Good (LEARNING s1, due=10) -> advance -> Good
     * (REVIEW I=1, due=now+1440) -> advance -> REVIEW due. */
    assert(pa_store_grade(&store, 0, PA_GOOD));
    assert(pa_store_advance(&store, 10));
    assert(pa_store_grade(&store, 0, PA_GOOD));
    assert(pa_store_advance(&store, 1440));
    /* Card 1 is still NEW due (due=0 <= now). Limit reached: only REVIEW surfaces. */
    pa_store_set_limits(&store, 1, 1);
    assert(pa_store_next(&store, "D") == 0);
    uint16_t counts[PA_STORE_MAX_DECKS];
    pa_store_new_due_counts(&store, counts);
    assert(counts[0] == 1);
    /* Budget left: NEW surfaces again. */
    pa_store_set_limits(&store, 1, 0);
    assert(pa_store_next(&store, "D") == 1);
    /* Limit 0 means unlimited even with huge done. */
    pa_store_set_limits(&store, 0, 999);
    assert(pa_store_next(&store, "D") == 1);
    pa_store_new_due_counts(NULL, counts);
    free_flash(&cards_ram);
    free_flash(&sched_ram);
    puts("store: new limit skip PASS");
}
```
`main` 中加 `test_new_limit_skip();`。注意：`pa_store_next` 返回 NEW 卡时按 due/id 排序，两张卡 due 不同（card0 due=1450+...=远期？不——card0 已 advance 到 due，due=1450=now；card1 due=0 past）——在 `set_limits(1,0)` 下两者都 due，优先级 REVIEW(0) < NEW(1)，`pa_store_next` 应返回 0 而不是 1！修正：第二段断言改为先把 card0 评掉或接受返回 0。正确做法：`pa_store_set_limits(&store, 1, 0);` 后 NEW 不再被跳过，但优先级 REVIEW 仍优先，所以返回的还是 0。要验证 NEW 可达，需把 card0 推进到未来：`assert(pa_store_grade(&store, 0, PA_GOOD));`（REVIEW I=1 +Good → I=3，due=now+4320，未来）→ 再 `assert(pa_store_next(&store, "D") == 1);`。按此修正写测试：
```c
    pa_store_set_limits(&store, 1, 0);
    assert(pa_store_grade(&store, 0, PA_GOOD));
    assert(pa_store_next(&store, "D") == 1);
```
（REVIEW I=1 +Good = max(2, round(1*2500/1000)) = max(2, round(2.5)) = 3 天，due 远期，只剩 NEW card1。）

- [ ] **Step 4: 运行测试验证失败**

Run: `cmake --build build/host && ./build/host/test_store`
Expected: FAIL（`pa_store_new_due_counts` 未定义，链接失败）。

- [ ] **Step 5: 实现后运行测试验证通过**

Run: 同上。
Expected: PASS，含 `store: new limit skip PASS`。

- [ ] **Step 6: Commit**

```bash
git add components/passport_anki/pa_store.c tests/test_store.c
git commit -m "feat(store): gate NEW picks on daily limit, add new-due counts

Co-Authored-By: Claude Code <noreply@anthropic.com>"
```

### Task 3: 同级内水库采样打乱

**Files:**
- Modify: `components/passport_anki/pa_store.c`
- Test: `tests/test_store.c`

**Interfaces:**
- Consumes: Task 1 的 `shuffle_on/rng_state`。
- Produces: `pa_store_next` 在 `shuffle_on` 时同级内均匀随机（优先级与到期过滤不变）。

- [ ] **Step 1: 加 xorshift RNG（静态）**

在 `pa_store.c` 的 `pa_store_next` 之前追加：
```c
static uint32_t pa_rng_next(pa_store *store)
{
    if (store->rng_state == 0) {
        store->rng_state = 0x9E3779B9u;
    }
    uint32_t x = store->rng_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    store->rng_state = x;
    return x;
}
```

- [ ] **Step 2: `pa_store_next` 加打乱分支**

将 `pa_store_next` 改为：保留现有守卫（NULL/status/deck），计算 `limited`，若 `!store->shuffle_on` 走原有 best 逻辑（含 Task 2 的 NEW 跳过）；否则走水库采样：
```c
uint16_t pa_store_next(pa_store *store, const char *deck)
{
    if (store == NULL || store->status != PA_STORE_READY || store->import_active) {
        return PA_NO_CARD;
    }
    int deck_id = deck_index(store, deck);
    if (deck_id < 0) {
        return PA_NO_CARD;
    }
    bool limited = store->new_limit != 0 && store->new_done >= store->new_limit;
    uint16_t need = chunks_for(store->count);
    if (!store->shuffle_on) {
        /* Legacy path: lowest (priority, due, id). NEW skipped when limited. */
        uint16_t best = PA_NO_CARD;
        unsigned best_priority = 3;
        uint32_t best_due = 0;
        uint32_t best_id = 0;
        for (uint16_t chunk = 0; chunk < need; ++chunk) {
            if (!flash_read(store->sched, pool_offset(store->chunk_phys[chunk]), store->sector, PA_STORE_SECTOR)) {
                return PA_NO_CARD;
            }
            uint16_t records = rd_u16(store->sector + 18);
            for (uint16_t n = 0; n < records; ++n) {
                const uint8_t *rec = store->sector + PA_SCHED_HEADER + (uint32_t)n * PA_SCHED_RECORD;
                if (rec[14] != (uint8_t)deck_id) {
                    continue;
                }
                uint32_t due = rd_u32(rec);
                if (due > store->now) {
                    continue;
                }
                if (rec[12] == PA_NEW && limited) {
                    continue;
                }
                unsigned priority = rec[12] == PA_LEARNING ? 0u : rec[12] == PA_REVIEW ? 1u : 2u;
                uint32_t id = rd_u32(rec + 16);
                uint16_t index = (uint16_t)(chunk * PA_SCHED_PER_CHUNK + n);
                if (best == PA_NO_CARD || priority < best_priority ||
                    (priority == best_priority && (due < best_due || (due == best_due && id < best_id)))) {
                    best = index;
                    best_priority = priority;
                    best_due = due;
                    best_id = id;
                }
            }
        }
        return best;
    }
    /* Shuffled path: reservoir sampling within each priority; lowest
     * non-empty priority wins. Due filter and NEW limit still apply. */
    uint16_t res[3] = {PA_NO_CARD, PA_NO_CARD, PA_NO_CARD};
    uint32_t seen[3] = {0, 0, 0};
    for (uint16_t chunk = 0; chunk < need; ++chunk) {
        if (!flash_read(store->sched, pool_offset(store->chunk_phys[chunk]), store->sector, PA_STORE_SECTOR)) {
            return PA_NO_CARD;
        }
        uint16_t records = rd_u16(store->sector + 18);
        for (uint16_t n = 0; n < records; ++n) {
            const uint8_t *rec = store->sector + PA_SCHED_HEADER + (uint32_t)n * PA_SCHED_RECORD;
            if (rec[14] != (uint8_t)deck_id) {
                continue;
            }
            uint32_t due = rd_u32(rec);
            if (due > store->now) {
                continue;
            }
            if (rec[12] == PA_NEW && limited) {
                continue;
            }
            unsigned priority = rec[12] == PA_LEARNING ? 0u : rec[12] == PA_REVIEW ? 1u : 2u;
            uint16_t index = (uint16_t)(chunk * PA_SCHED_PER_CHUNK + n);
            seen[priority]++;
            if (pa_rng_next(store) % seen[priority] == 0) {
                res[priority] = index;
            }
        }
    }
    for (unsigned priority = 0; priority < 3; ++priority) {
        if (seen[priority] > 0) {
            return res[priority];
        }
    }
    return PA_NO_CARD;
}
```
注意：原函数体整个被替换，`best_due/best_id` 初始化保持原样（原代码 `best_due=0, best_id=0` 只在 best 有效后比较，无需改动）。

- [ ] **Step 3: 写 failing 测试**

在 `tests/test_store.c` 追加：
```c
static void test_shuffle_within_priority(void)
{
    ram_flash cards_ram, sched_ram;
    pa_flash cards, sched;
    make_flash(&cards_ram, &cards, PA_CARDS_PARTITION_SIZE);
    make_flash(&sched_ram, &sched, PA_SCHED_PARTITION_SIZE);
    pa_store store;
    pa_store_init(&store, &cards, &sched);
    assert(pa_store_mount(&store) == PA_STORE_EMPTY);
    uint8_t payload[128];
    size_t length = 0;
    for (uint32_t i = 1; i <= 5; ++i) {
        length = append_card(payload, length, i, "D", "Q", "A");
    }
    assert(import_bytes(&store, payload, 5, (uint32_t)length));
    /* One LEARNING-due card beats NEW cards even when shuffled. */
    assert(pa_store_grade(&store, 0, PA_AGAIN));
    assert(pa_store_advance(&store, 1));
    pa_store_set_shuffle(&store, true, 1234);
    assert(pa_store_next(&store, "D") == 0);
    /* Deterministic: same seed, same pick. */
    pa_store_set_shuffle(&store, true, 777);
    uint16_t first = pa_store_next(&store, "D");
    pa_store_set_shuffle(&store, true, 777);
    assert(pa_store_next(&store, "D") == first);
    free_flash(&cards_ram);
    free_flash(&sched_ram);

    /* All-NEW deck: shuffled picks spread across indices. */
    make_flash(&cards_ram, &cards, PA_CARDS_PARTITION_SIZE);
    make_flash(&sched_ram, &sched, PA_SCHED_PARTITION_SIZE);
    pa_store_init(&store, &cards, &sched);
    assert(pa_store_mount(&store) == PA_STORE_EMPTY);
    length = 0;
    for (uint32_t i = 1; i <= 5; ++i) {
        length = append_card(payload, length, i, "E", "Q", "A");
    }
    assert(import_bytes(&store, payload, 5, (uint32_t)length));
    bool seen[5] = {false, false, false, false, false};
    for (uint32_t seed = 1; seed <= 100; ++seed) {
        pa_store_set_shuffle(&store, true, seed);
        uint16_t got = pa_store_next(&store, "E");
        assert(got < 5);
        seen[got] = true;
    }
    int distinct = 0;
    for (int i = 0; i < 5; ++i) {
        distinct += seen[i] ? 1 : 0;
    }
    assert(distinct >= 3);
    /* Shuffle off restores due/id order: all NEW due=0, lowest id wins. */
    pa_store_set_shuffle(&store, false, 0);
    assert(pa_store_next(&store, "E") == 0);
    free_flash(&cards_ram);
    free_flash(&sched_ram);
    puts("store: shuffle within priority PASS");
}
```
`main` 中加 `test_shuffle_within_priority();`。说明：第一段中 card0 Again 后 LEARNING s0 due=now+1，advance(1) 后到期；LEARNING 优先级 0，唯一候选，水库采样必中（与种子无关，稳定不断言 golden 值）。

- [ ] **Step 4: 运行测试验证失败**

Run: `cmake --build build/host && ./build/host/test_store`
Expected: FAIL（`pa_store_set_shuffle` 行为缺失——链接失败；若 Task 1 已合入则是采样逻辑缺失导致断言失败）。

- [ ] **Step 5: 实现后运行测试验证通过**

Run: 同上。
Expected: PASS，含 `store: shuffle within priority PASS`。

- [ ] **Step 6: Commit**

```bash
git add components/passport_anki/pa_store.c tests/test_store.c
git commit -m "feat(store): reservoir-sample shuffle within priority class

Co-Authored-By: Claude Code <noreply@anthropic.com>"
```

### Task 4: 未来3天预报 + `pa_store_earliest_due`

**Files:**
- Modify: `components/passport_anki/pa_store.c`
- Test: `tests/test_store.c`

**Interfaces:**
- Consumes: 现有 chunk 扫描模式。
- Produces: `pa_store_forecast`（全局三桶计数）；`pa_store_earliest_due`（某牌组大于 now 的最小 due）。

- [ ] **Step 1: 实现 `pa_store_forecast`**

在 `pa_store.c` 中 `pa_store_new_due_counts` 之后追加：
```c
void pa_store_forecast(pa_store *store, uint16_t buckets[3])
{
    if (buckets != NULL) {
        buckets[0] = 0;
        buckets[1] = 0;
        buckets[2] = 0;
    }
    if (store == NULL || store->status != PA_STORE_READY || store->import_active ||
        buckets == NULL) {
        return;
    }
    uint16_t need = chunks_for(store->count);
    for (uint16_t chunk = 0; chunk < need; ++chunk) {
        if (!flash_read(store->sched, pool_offset(store->chunk_phys[chunk]), store->sector,
                        PA_STORE_SECTOR)) {
            return;
        }
        uint16_t records = rd_u16(store->sector + 18);
        for (uint16_t n = 0; n < records; ++n) {
            const uint8_t *rec = store->sector + PA_SCHED_HEADER + (uint32_t)n * PA_SCHED_RECORD;
            if (rec[14] >= store->deck_count) {
                continue;
            }
            uint32_t due = rd_u32(rec);
            if (due <= store->now) {
                continue;
            }
            uint32_t delta = due - store->now;
            unsigned bucket = delta <= 1440u ? 0u : delta <= 2880u ? 1u : delta <= 4320u ? 2u : 3u;
            if (bucket < 3 && buckets[bucket] < UINT16_MAX) {
                buckets[bucket]++;
            }
        }
    }
}
```

- [ ] **Step 2: 实现 `pa_store_earliest_due`**

追加：
```c
bool pa_store_earliest_due(pa_store *store, const char *deck, uint32_t *due_out)
{
    if (due_out != NULL) {
        *due_out = 0;
    }
    if (store == NULL || store->status != PA_STORE_READY || store->import_active ||
        deck == NULL || due_out == NULL) {
        return false;
    }
    int deck_id = deck_index(store, deck);
    if (deck_id < 0) {
        return false;
    }
    bool found = false;
    uint32_t best = 0;
    uint16_t need = chunks_for(store->count);
    for (uint16_t chunk = 0; chunk < need; ++chunk) {
        if (!flash_read(store->sched, pool_offset(store->chunk_phys[chunk]), store->sector,
                        PA_STORE_SECTOR)) {
            return false;
        }
        uint16_t records = rd_u16(store->sector + 18);
        for (uint16_t n = 0; n < records; ++n) {
            const uint8_t *rec = store->sector + PA_SCHED_HEADER + (uint32_t)n * PA_SCHED_RECORD;
            if (rec[14] != (uint8_t)deck_id) {
                continue;
            }
            uint32_t due = rd_u32(rec);
            if (due > store->now && (!found || due < best)) {
                best = due;
                found = true;
            }
        }
    }
    if (found) {
        *due_out = best;
    }
    return found;
}
```

- [ ] **Step 3: 写 failing 测试**

在 `tests/test_store.c` 追加（时间线：now=0 导入两张 D 牌）：
```c
static void test_forecast_and_earliest(void)
{
    ram_flash cards_ram, sched_ram;
    pa_flash cards, sched;
    make_flash(&cards_ram, &cards, PA_CARDS_PARTITION_SIZE);
    make_flash(&sched_ram, &sched, PA_SCHED_PARTITION_SIZE);
    pa_store store;
    pa_store_init(&store, &cards, &sched);
    assert(pa_store_mount(&store) == PA_STORE_EMPTY);
    uint8_t payload[64];
    size_t length = append_card(payload, 0, 1, "D", "Q", "A");
    length = append_card(payload, length, 2, "D", "Q2", "A2");
    assert(import_bytes(&store, payload, 2, (uint32_t)length));
    /* idx0 NEW+Good -> LEARNING s1 due=10. idx1 NEW+Good -> LEARNING s1 due=10. */
    assert(pa_store_grade(&store, 0, PA_GOOD));
    assert(pa_store_grade(&store, 1, PA_GOOD));
    /* advance(10): now=10. idx1 +Good -> REVIEW I=1 due=1450. */
    assert(pa_store_advance(&store, 10));
    assert(pa_store_grade(&store, 1, PA_GOOD));
    /* advance(1440): now=1450. idx1 +Hard -> I=max(2,ceil(1.2))=2, due=4330. */
    assert(pa_store_advance(&store, 1440));
    assert(pa_store_grade(&store, 1, PA_HARD));
    /* idx0 +Good at now=1450: LEARNING s1 -> REVIEW I=1, due=2890. */
    assert(pa_store_grade(&store, 0, PA_GOOD));
    uint16_t buckets[3] = {9, 9, 9};
    pa_store_forecast(&store, buckets);
    /* now=1450: idx0 due=2890 -> bucket0; idx1 due=4330 -> bucket1. */
    assert(buckets[0] == 1 && buckets[1] == 1 && buckets[2] == 0);
    uint32_t earliest = 0;
    assert(pa_store_earliest_due(&store, "D", &earliest) && earliest == 2890);
    assert(!pa_store_earliest_due(&store, "missing", &earliest));
    assert(!pa_store_earliest_due(&store, "D", NULL));
    assert(!pa_store_earliest_due(NULL, "D", &earliest));
    pa_store_forecast(NULL, buckets);
    assert(buckets[0] == 1 && buckets[1] == 1 && buckets[2] == 0);
    free_flash(&cards_ram);
    free_flash(&sched_ram);
    puts("store: forecast and earliest due PASS");
}
```
`main` 中加 `test_forecast_and_earliest();`。

- [ ] **Step 4: 运行测试验证失败**

Run: `cmake --build build/host && ./build/host/test_store`
Expected: FAIL（链接失败）。

- [ ] **Step 5: 实现后运行测试验证通过**

Run: 同上。
Expected: PASS，含 `store: forecast and earliest due PASS`。

- [ ] **Step 6: Commit**

```bash
git add components/passport_anki/pa_store.c tests/test_store.c
git commit -m "feat(store): three-day forecast and earliest-due query

Co-Authored-By: Claude Code <noreply@anthropic.com>"
```

### Task 5: host 全套测试绿

**Files:** 无改动（验证任务）。

- [ ] **Step 1: 全量构建 + ctest**

Run:
```bash
cmake -S . -B build/host -DANKI_HOST=ON -DANKI_SANITIZERS=ON
cmake --build build/host
ctest --test-dir build/host --output-on-failure
```
Expected: 全部通过（含 store 新增的 4 个测试输出）。

- [ ] **Step 2: pytest 回归**

Run: `python3 -m pytest -q tests/test_deck.py tests/test_anki_sync.py tests/test_simulator.py tests/test_image_pad.py`
Expected: PASS（deck.py 未动，纯回归）。

- [ ] **Step 3: Commit（无改动则跳过）**

本任务不产生改动，无需 commit。若发现失败，按失败信息修（修 store 问题就地改并补 commit）。

### Task 6: main.c 设置项 + 新卡日计数

**Files:**
- Modify: `main/main.c`

**Interfaces:**
- Consumes: Task 1 的 store setter；现有 `load_u8_setting/save_u8_setting`。
- Produces: NVS 新键读写、开机装配、`study_day_sync/study_day_reset`、`do_grade` 计 NEW 数。

- [ ] **Step 1: 设置变量与 label**

找到 `static uint8_t setting_autopage_sec = 0;`（约 76 行），在其后插入：
```c
static uint8_t setting_new_limit = 20;
static uint8_t setting_shuffle = 0;
static uint8_t setting_autoplay = 0;
static uint16_t setting_new_done = 0;
static uint32_t setting_new_day = 0;
static bool autoplay_active = false;
static int64_t autoplay_empty_since_us = 0;
```
找到 `autopage_label` 函数，在其后插入：
```c
static const char *new_limit_label(uint8_t limit)
{
    switch (limit) {
    case 0: return "不限";
    case 10: return "10张";
    case 50: return "50张";
    default: return "20张";
    }
}
```

- [ ] **Step 2: u32 设置 helper**

找到 `save_u8_setting` 函数，在其后插入：
```c
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
```

- [ ] **Step 3: 日期同步 helper（放在 `do_grade` 之前）**

在 `do_grade` 定义之前插入：
```c
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
```

- [ ] **Step 4: `do_grade` 计 NEW 数**

将 `do_grade` 改为：
```c
static void do_grade(void) {
    if (session.current == PA_NO_CARD || !session.revealed) return;
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
```

- [ ] **Step 5: 开机装配**

找到 `app_main` 中的设置加载块：
```c
        setting_brightness = load_u8_setting("brightness", 70, brightness_levels, 3);
        setting_sleep_min = load_u8_setting("sleep_min", 3, sleep_levels, 3);
        setting_autopage_sec = load_u8_setting("autopage_sec", 0, autopage_levels, 4);
```
在其后插入：
```c
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
```
（`esp_random` 由已 include 的 `esp_system.h` 提供。）

- [ ] **Step 6: 接入推进日与导入提交**

`UI_CONFIRM_DAY` 成功分支（`confirming_day = false; ui = UI_DECKS;` 处）改为成功后加 `study_day_sync();`：
```c
    } else { confirming_day = false; ui = UI_DECKS; study_day_sync(); }
```
`PA_PROTOCOL_COMMIT` 成功路径（`clamp_deck_index(); ui = UI_DECKS;` 附近，`printf("OK COMMIT\n")` 之前）加：
```c
        study_day_reset();
        autoplay_active = false;
        autoplay_empty_since_us = 0;
```
`PA_PROTOCOL_BEGIN` 成功路径（`ui = UI_IMPORTING;` 处）加：
```c
        autoplay_active = false;
        autoplay_empty_since_us = 0;
```
`abort_import` 函数内 `if (ui == UI_IMPORTING) ui = UI_DECKS;` 后加同样两行（中止也停播）。

- [ ] **Step 7: 长按 OK 统一停播**

在 `process_button` 长按分支开头（`if (long_press && event->button == BSP_BTN_OK) {` 内第一行）加：
```c
    autoplay_active = false;
    autoplay_empty_since_us = 0;
```

- [ ] **Step 8: 验证**

Run: `ctest --test-dir build/host --output-on-failure`（store 未动，应仍绿）；`python3 - <<'EOF'
import re
src = open('main/main.c', encoding='utf-8').read()
for token in ['setting_new_limit', 'study_day_sync', 'study_day_reset', 'pa_store_is_new', 'autoplay_active']:
    assert token in src, token
print('main wiring tokens present')
EOF`
Expected: PASS。说明：main.c 无法 host 编译，靠 token 自检 + 人工对照 + CI 固件构建。

- [ ] **Step 9: Commit**

```bash
git add main/main.c
git commit -m "feat(fw): daily NEW budget settings and day rollover

Co-Authored-By: Claude Code <noreply@anthropic.com>"
```

### Task 7: 菜单四行（限额/打乱/预报/播放）

**Files:**
- Modify: `main/main.c`

**Interfaces:**
- Consumes: Task 6 的设置变量。
- Produces: `DECK_EXTRA_ROWS=9`，render + process_button 新分支。

- [ ] **Step 1: 行数 5→9**

找到：
```c
/* Deck list: stats summary, decks..., then "next day", brightness, sleep,
 * auto-page, battery rows. */
#define DECK_EXTRA_ROWS 5u
```
改为：
```c
/* Deck list: stats summary, decks..., then "next day", brightness, sleep,
 * auto-page, battery, new-card limit, shuffle, forecast, autoplay rows. */
#define DECK_EXTRA_ROWS 9u
```
找到 process_button 注释 `* at settings_base (next day, brightness, sleep, auto-page, battery). */`，改为 `* at settings_base (next day, brightness, sleep, auto-page, battery, new limit, shuffle, forecast, autoplay). */`。

- [ ] **Step 2: render 新分支**

找到 battery 分支：
```c
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
```
在其后（仍在 `} else {` 链内、`}` 闭合前）插入：
```c
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
```

- [ ] **Step 3: process_button 新分支 + 入学即播**

找到 battery 输入分支：
```c
        } else if (deck_index == settings_base + 4u) {
            /* Battery row is read-only; OK gives visible feedback by
             * returning to the stats summary instead of doing nothing. */
            deck_index = 0;
        }
```
在其后插入：
```c
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
```
找到两个入学分支：
```c
        else if (count > 0 && deck_index == 0) { study_all = true; study_deck = 0; begin_study(); }
        else if (count > 0 && deck_index >= 1u && deck_index <= count) {
            study_all = false;
            study_deck = (uint16_t)(deck_index - 1u);
            begin_study();
        }
```
改为入学后按开关 arm：
```c
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
```

- [ ] **Step 4: 验证**

Run: token 自检（`DECK_EXTRA_ROWS 9u`、`extra == 8u`、`settings_base + 8u` 存在）；`ctest --test-dir build/host --output-on-failure` 仍绿。
Expected: PASS。

- [ ] **Step 5: Commit**

```bash
git add main/main.c
git commit -m "feat(fw): menu rows for new limit, shuffle, forecast, autoplay

Co-Authored-By: Claude Code <noreply@anthropic.com>"
```

### Task 8: 剩余显示（统计行 + 学习页脚）

**Files:**
- Modify: `main/main.c`

**Interfaces:**
- Consumes: Task 2 的 `pa_store_new_due_counts`、Task 4 的 `pa_store_forecast`。
- Produces: 统计汇总行 `待学X（含新Y)` + 预报行；QUESTION/ANSWER/RATING 页脚 `剩X·新Y`。

- [ ] **Step 1: `study_remaining` helper（放在 render 之前）**

在 `render` 函数之前插入：
```c
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
    uint32_t x = total - ntotal + left;
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
```

- [ ] **Step 2: 统计汇总行扩展**

找到统计行 snprintf：
```c
            snprintf(body, sizeof(body), "今日已评 %" PRIu32 "（%u/%u/%u/%u）\n待学 %" PRIu32 " · %u 牌组\n学习日 %" PRIu32,
                     today_total, (unsigned)by_rating[0], (unsigned)by_rating[1],
                     (unsigned)by_rating[2], (unsigned)by_rating[3],
                     total_due, count, day);
```
改为：
```c
            uint16_t remain_x = 0, remain_y = 0, forecast[3] = {0, 0, 0};
            study_remaining(true, 0, &remain_x, &remain_y);
            pa_store_forecast(&store, forecast);
            snprintf(body, sizeof(body), "今日已评 %" PRIu32 "（%u/%u/%u/%u）\n待学 %u（含新%u） · %u 牌组\n明%u/后%u/大后%u\n学习日 %" PRIu32,
                     today_total, (unsigned)by_rating[0], (unsigned)by_rating[1],
                     (unsigned)by_rating[2], (unsigned)by_rating[3],
                     remain_x, remain_y, count,
                     (unsigned)forecast[0], (unsigned)forecast[1], (unsigned)forecast[2], day);
```
（`total_due` 变量保留与否不影响编译；若编译器报 unused 任由实现者删除该累加循环——这是 firmware 代码，以 CI 构建为准，此处先保留。）
注意：原 `total_due` 累加循环保留，避免改动面扩大；`pa_store_due_counts(&store, due); for...total_due` 保持不动。

- [ ] **Step 3: 学习页脚加剩余**

找到：
```c
        snprintf(title, sizeof(title), "%s", card_deck);
        if (ui == UI_QUESTION) {
            snprintf(footer, sizeof(footer), "%u/%u  OK 翻面", page + 1, (unsigned)pages_count);
        } else if (ui == UI_ANSWER) {
            body_color = 0xE7F0FA;
            snprintf(footer, sizeof(footer), "%u/%u  OK 评分", page + 1, (unsigned)pages_count);
        } else if (grade_save_failed) {
```
改为：
```c
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
```
找到 RATING 正常分支：
```c
        } else {
            body_color = 0xE7F0FA;
            footer_color = 0xC8D0DA;
            snprintf(footer, sizeof(footer), "上下选择  OK 确认");
        }
```
改为：
```c
        } else {
            body_color = 0xE7F0FA;
            footer_color = 0xC8D0DA;
            snprintf(footer, sizeof(footer), "剩%u·新%u 上下选择 OK", remain_x, remain_y);
        }
```

- [ ] **Step 4: 验证**

Run: token 自检（`study_remaining`、`剩%u·新%u`、`含新%u` 存在）；`ctest --test-dir build/host --output-on-failure` 仍绿。
Expected: PASS。

- [ ] **Step 5: Commit**

```bash
git add main/main.c
git commit -m "feat(fw): remaining counts on stats row and study footers

Co-Authored-By: Claude Code <noreply@anthropic.com>"
```

### Task 9: 全自动播放引擎

**Files:**
- Modify: `main/main.c`

**Interfaces:**
- Consumes: Task 6/7 的 `autoplay_active`、Task 4 的 `pa_store_earliest_due`、现有 `EVENT_IDLE` 1 秒 tick、`check_idle_sleep`、`do_grade/begin_study/study_has_due`。
- Produces: `autoplay_step/autoplay_wait_tick/autoplay_scope_earliest`；IDLE 分支改写；按键退出；睡眠抑制；EMPTY 等待显示。

- [ ] **Step 1: scope 最早到期 helper（放在 `study_has_due` 之后）**

插入：
```c
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
```

- [ ] **Step 2: `autoplay_step` + `autoplay_wait_tick`（放在 `do_grade` 之后）**

插入：
```c
#define AUTOPLAY_MAX_WAIT_S (10u * 60u)

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
```

- [ ] **Step 3: EMPTY 等待显示**

找到 UI_EMPTY 分支：
```c
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
```
改为：
```c
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
                snprintf(body, sizeof(body), "今天已完成\n牌组：%s\n今日已评 %" PRIu32 "（%u/%u/%u/%u）\n可推进学习日。",
                         study_name != NULL ? study_name : "全部", today_total,
                         (unsigned)by_rating[0], (unsigned)by_rating[1],
                         (unsigned)by_rating[2], (unsigned)by_rating[3]);
            }
        }
        snprintf(footer, sizeof(footer), autoplay_active ? "按任意键转手动" : "OK 重新检查");
```

- [ ] **Step 4: EVENT_IDLE 改写**

找到：
```c
        } else if (event.type == EVENT_IDLE) {
            int64_t now = esp_timer_get_time();
            check_idle_sleep(now);
            if (!screen_dimmed && setting_autopage_sec > 0 &&
                (ui == UI_QUESTION || ui == UI_ANSWER)) {
```
改为：
```c
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
```
（后续原有 autopage 块保持不变。）

- [ ] **Step 5: 按键退出 + 睡眠抑制**

在 `app_task` 的 `EVENT_BUTTON` 分支中，找到 dim 唤醒块之后：
```c
            if (screen_dimmed) {
                /* Woke from dim: consume the press so it cannot flip or grade. */
                screen_dimmed = false;
                bsp_display_backlight(setting_brightness);
                render();
            } else if (pa_cover_requests_launcher((int)ui, (int)event.button, (int)event.button_event)) {
```
改为插入自动播退出（dim 唤醒与自动播退出之间）：
```c
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
```
在 `check_idle_sleep` 的 USB 分支之后、dim 检查之前插入播放抑制：
```c
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
```

- [ ] **Step 6: 验证**

Run: token 自检（`autoplay_step`、`autoplay_wait_tick`、`autoplay_scope_earliest`、`AUTOPLAY_MAX_WAIT_S` 存在）；`ctest --test-dir build/host --output-on-failure` 仍绿。
Expected: PASS。

- [ ] **Step 7: Commit**

```bash
git add main/main.c
git commit -m "feat(fw): hands-free autoplay of today's cards

Co-Authored-By: Claude Code <noreply@anthropic.com>"
```

### Task 10: 文档（design.md + user-guide.md）

**Files:**
- Modify: `docs/design.md`
- Modify: `docs/user-guide.md`

- [ ] **Step 1: design.md §7 选卡与限额**

Read `docs/design.md` §7 先确认行文。用 Edit 做两处替换（anchor 二选一，取文件中实际存在的那句；两句都在：两处都改）：
1. old: `没有每日新卡限额；12,000 张是总量上限。`
   new: `每日新卡限额只 gate NEW（LEARNING/REVIEW 不限），NVS \`new_limit\`（0=不限/10/20/50，默认 20）配 \`new_done/new_day\` 计数，跨学习日清零；12,000 张是总量上限。`
2. old: `同一类别按 due 升序、id 升序。`
   new: `同一类别默认按 due 升序、id 升序，打乱开时同级内均匀随机（水库采样，优先级与到期过滤不变）。`

- [ ] **Step 2: design.md §8 菜单表与行说明**

1. old（§8 表格 DECKS 行内）: `切换统计汇总/牌组/下一学习日/亮度/休眠/自动翻页/电量`
   new: `切换统计汇总/牌组/下一学习日/亮度/休眠/自动翻页/新卡限额/打乱/预报/自动播放/电量`
2. old（§8 段落内）: `手动翻页重置计时，不自动翻面/评分）、电量`
   new: `手动翻页重置计时，不自动翻面/评分）、新卡限额（NVS \`new_limit/new_done/new_day\`）、打乱（NVS \`shuffle\`，xorshift 水库采样，\`esp_random\` 播种）、未来3天预报（只读全局分桶）、自动播放（NVS \`autoplay\` 开关 + RAM 播放态，播完今日全部，任一按键转手动，播放中抑制熄屏/深睡）、电量`
3. old（统计行说明）: `牌组页首行是统计汇总（当日已评分布、全部待学数；OK 进首个到期牌组）`
   new: `牌组页首行是统计汇总（当日已评分布、待学X含新Y、未来3天预报；OK 进首个到期牌组）`
4. old（学习画面页脚）: 在 `每屏显示牌组、逻辑 day、当前卡/总数、Q/A、页码、操作提示。` 后追加一句：`学习画面页脚带今日剩余（剩X·新Y，范围跟随当前学习）。`

- [ ] **Step 3: user-guide.md 学习与菜单说明**

1. old: `手动翻页重置计时）、电量（只读`
   new: `手动翻页重置计时）、新卡限额（OK 在不限/10/20/50 间切换，默认 20，只限 NEW，今日已学计数跨学习日清零）、打乱（OK 开/关，同级内随机）、未来3天（只读 明/后/大后到期分布）、自动播放（OK 开/关并立即按当前范围播放，播完今日全部；任一按键转手动）、电量（只读`
2. old: `到期卡优先于新卡。所有到期卡完成后显示完成状态，不会自动跳到下一天。`
   new: `到期卡优先于新卡。每日新卡默认 20 张（LEARNING/REVIEW 到期不限）。学习画面页脚显示剩X·新Y。自动播放开时，翻页/翻面/记 Good 全自动；Again 一分钟这类短等待（≤10 分钟）会停留并显示等待秒数。所有到期卡完成后显示完成状态，不会自动跳到下一天。`

- [ ] **Step 4: 验证**

Run: `grep -n "新卡限额" docs/design.md docs/user-guide.md | head` 应两文件都有命中；`git diff --stat` 确认只动两文件。
Expected: PASS。

- [ ] **Step 5: Commit**

```bash
git add docs/design.md docs/user-guide.md
git commit -m "docs: study boost behavior (limit, shuffle, remaining, autoplay)

Co-Authored-By: Claude Code <noreply@anthropic.com>"
```

### Task 11: 最终验证

**Files:** 无改动（验证任务）。

- [ ] **Step 1: host 全绿**

Run:
```bash
cmake -S . -B build/host -DANKI_HOST=ON -DANKI_SANITIZERS=ON
cmake --build build/host
ctest --test-dir build/host --output-on-failure
python3 -m pytest -q tests/test_deck.py tests/test_anki_sync.py tests/test_simulator.py tests/test_image_pad.py
```
Expected: 全部 PASS。

- [ ] **Step 2: 工作区干净且无违禁文件**

Run: `git status --porcelain`
Expected: 无 `n3.apkg`、无 `*.map.json` 被暂存或提交；plan/spec/docs 与代码改动文件列表符合预期。

- [ ] **Step 3: 固件构建说明**

main.c 改动无法在本机编译（无 IDF）。在本任务输出中明确记录：需在有 IDF 的机器或 CI 上跑 `tools/validate_firmware.sh`；真机验收项（菜单 9 行 OK 映射、限额/打乱/预报/自动播放、Again 等待、按键退出、播放中不熄屏）待用户刷机验证。不做代码改动。

---

## Self-Review

- Spec 覆盖：①新卡限额→Task 1/2/6/7/8/10；②打乱→Task 1/3/6/7/10；③剩余→Task 2/4/8/10；④自动播放→Task 4/6/7/9/10。每节都有任务。
- Placeholder 扫描：无 TBD/TODO；所有代码块完整；命令含期望输出。
- 类型一致性：`pa_store_set_limits(store, uint8_t, uint16_t)` 各任务一致；`study_remaining(bool, uint16_t, uint16_t*, uint16_t*)`；`pa_store_earliest_due(store, const char*, uint32_t*)`；`forecast` 用 `uint16_t[3]`；`AUTOPLAY_MAX_WAIT_S` 为秒；`wait_s=(due-now)*60u` 分钟转秒。
- 风险点：Task 2 测试依赖 `round(2.5)=3`（glibc round 半值远离零，确定性，无 ambiguity，断言的是 REVIEW 行为而非 round 本身——实际断言只用 `next==1`，不依赖具体 I 值，只依赖“due 远期”；Good 后 I≥2 恒成立，安全）。Task 3 分布断言 `distinct>=3` 保守。Task 8/9 的 main.c 以 CI 构建为准，计划中已声明。
