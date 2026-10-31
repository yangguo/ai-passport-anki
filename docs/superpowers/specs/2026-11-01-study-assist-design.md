# 学习辅助功能包 — Design

**Goal:** 对照 AnkiDroid 的对应功能，为 AI Passport 补齐三组学习辅助能力：
亮度+电量、牌组到期计数+今日统计、两段式自动休眠。

**Status:** 三段均经用户批准（2026-11-01）：轻量包+休眠、牌组到期+今日数、
两段式、熄屏三档（1/3/5 分钟）+深睡固定熄屏后 5 分钟、亮度三档（30/70/100）。

## 0. 参考与裁剪

- AnkiDroid：牌组列表到期计数、复习统计、屏幕常亮/自动锁屏、夜间模式。
  本设备是 240×320 三键 ESP32-C3，无触屏、无数字键盘、无 RTC，
  因此计数只做纯文本行内数字，配置只做档位循环切换，不做图表和数字输入。
- 现有约束不变：`pa_store` 静态预算 12 KiB、稳态零 malloc、
  深睡唤醒=重启（`bsp_power_enter_deep_sleep`）、逻辑时钟不计关机时间
  （设计 §10）、导入中不做状态变更。

## 1. 亮度 + 电量（firmware，`main/main.c` + NVS）

- NVS 键 `brightness`（u8，默认 70），启动时读取并替代写死的
  `bsp_display_backlight(70)`；非法值回退 70。
- 牌组页（UI_DECKS）新增条目"亮度"，OK 在 30/70/100 间循环，
  即时调用 `bsp_display_backlight()` 并写 NVS，重启保持。
- 牌组页新增只读条目"电量"，显示 `bsp_battery_soc()` 百分比；
  `bsp_battery_init()` 失败或读回 -1 时显示"不可用"，不挡学习。
- 三键占用：DECKS 页 UP/DOWN 已是上下移动，OK 已是进入；
  新条目只是列表项，无新增手势；UP 长按回启动器（cover 门控）不变。

## 2. 牌组到期计数 + 今日统计（store API + firmware）

对照 AnkiDroid 主列表的牌组计数，裁到纯文本：

- 新增 `void pa_store_due_counts(pa_store *store, uint16_t counts[PA_STORE_MAX_DECKS])`：
  单次扫描全部调度块，按 `deck_id` 累加 `due <= now` 的卡数。
  不分配内存，只用 `store->sector` 缓冲；IMPORT 中/非 READY 时全零。
- 新增 `void pa_store_today_stats(pa_store *store, uint32_t by_rating[4], uint32_t *total)`：
  遍历复习日志（`read_revlog_record`），统计时间戳 `>= day_start`
  （`day_start = now - now % 1440`）的条目，按 rating 0..3 累加。
  `revlog_corrupt` 或 epoch==0 时全零。
- 牌组页（UI_DECKS 一次只显示当前选中的一个牌组）把正文首行改为
  `牌组名·到期N`；新增的亮度/休眠/电量条目接在"下一学习日"条目之后，
  同样经 UP/DOWN 移动、OK 进入/切换。完成页（UI_EMPTY）显示
  `今日已评 N（A/H/G/E 各数）`。不改调度核心，只读统计。

## 3. 两段式自动休眠（firmware）

- NVS 键 `sleep_min`（u8，档位 1/3/5，默认 3）；牌组页新增"休眠"条目，
  OK 循环切档，即时写 NVS。
- 计时：新增 10 秒 `esp_timer` 投递 `EVENT_IDLE`；任一按键/USB 行输入
  更新 `last_activity_us`。`store.import_active` 时不休眠。
- 第一段：超时 → `bsp_display_backlight(0)` 熄屏待命，LVGL 与时钟继续；
  任一按键先恢复 NVS 亮度并清零计时，该次按键被消费（不触发翻页/评分），
  避免误触。
- 第二段：熄屏后再过 5 分钟无操作 → `pa_store_checkpoint()` 落盘 →
  `bsp_power_enter_deep_sleep()`。唤醒=重启，`app_main` 照常挂载，
  `bsp_power_wait_for_wake_release()` 已处理按键释放。
- 深睡后逻辑时钟停走与现有"不计关机时间"一致，不补时间。

## 非目标

- 每日新卡/复习上限（复习计划管理第二阶段，需改调度核心，本次不做）。
- 图表、历史曲线、AnkiDroid 式数字输入。
- 熄屏后时钟继续（第一段时钟继续，第二段深睡停走，见上）。
- USB 协议变更；模拟器 LVGL/电源行为（`passport_sim` 不模拟背光与深睡）。

## 测试

- `tests/test_store.c`：due_counts 与 today_stats 的 host 测试
  （构造多牌组、调 due、评分若干次，断言计数； corruption/empty 边界）。
- 现有 CTest 全过；`main.c` 改动需 ESP-IDF 构建验证（本机无 IDF，
  由 CI/`validate_firmware.sh` 覆盖）。
- 真机：亮度切换即时生效且重启保持；电量显示；牌组到期数与完成页统计；
  1 分钟档熄屏→按键恢复→5 分钟后深睡→按键唤醒重启→进度不丢。
