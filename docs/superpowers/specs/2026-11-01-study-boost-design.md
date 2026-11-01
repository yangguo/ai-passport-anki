# 每日新卡限额 / 同级打乱 / 剩余显示 / 全自动播放设计

日期：2026-11-01。范围：设备端学习流程增强，不改 flash 格式、不改调度算法、不改 USB 协议。

## 1. 每日新卡限额（只限 NEW）

- 只限制 NEW 卡：LEARNING、REVIEW 到期卡不限，照常出现。
- NVS 新键：`new_limit`（0=不限/10/20/50，默认 20，多档菜单循环）、`new_done`（今日已学新卡数）、`new_day`（日期 id = now/1440）。
- `main` 在评分页对 NEW 卡评分成功后 `new_done+1` 并 commit；跨学习日（`now/1440 != new_day`）清零。grade 前用只读接口 `pa_store_is_new(index)` 判断 stage==PA_NEW（store 新增，不改 flash）。
- `store` 内部保存 `new_limit`/`new_done`（setter 注入，不进 flash）：`pa_store_next` 在 NEW 已达上限时跳过 NEW，只出 LEARNING/REVIEW。
- 新卡限额耗尽且无其他到期时，进完成页并显示"今日新卡已学完"。
- 全局限额（全部牌组共用），与单牌组/全部学习范围无关。

## 2. 同级内随机打乱

- 优先级不变：LEARNING → REVIEW → NEW；LEARNING 步骤与到期过滤不变。
- 同级内不再按 due/id 取最小，改为单遍水库采样均匀随机挑一张（流式扫描兼容 12k，不加 RAM）。
- NVS 新键 `shuffle`（0=关/1=开，默认关，菜单可切，全局）。
- 随机数用 xorshift：设备侧以 `esp_random` 播种；单测可注入种子保证可复现。
- flash 格式不动，不增加磨损。

## 3. 剩余显示（两处）

- 学习画面 QUESTION/ANSWER/RATING 页脚加"剩X·新Y"：X=当前范围今日剩余（LEARNING+REVIEW 到期 + NEW 限额内剩余），Y=NEW 限额内剩余；范围跟随当前学习（全部/单牌组）。
- 牌组页统计汇总行扩展为"待学X（含新Y）"；另加只读预报行"明A/后B/大后C"（全局，全部牌组未来 3 天到期分布；单牌组行仍只显示本组到期 N）。
- `store` 新增只读预报接口 `pa_store_forecast`：按 due 落在 `(now,now+1440k]`（k=1,2,3）分桶计数，不改 flash，饱和到 UINT16_MAX。

## 4. 全自动播放（全程不操作播完今日全部）

- 作用域跟随当前学习（全部/单牌组），遵守新卡限额与打乱。
- 流程：QUESTION 按 `autopage_sec`（关则按 3 秒）自动下翻 → 末页停留同样时长自动翻面 → ANSWER 同样翻页 → 末页停留后自动记 Good → 下一张，直到今日无到期（含新卡限额耗尽）进完成页。
- Again 一分钟这类短到期会停留等待（显示"等 X 秒"，最长等 10 分钟），更久则直接结束。
- 任一按键退出自动播、停在当前卡转手动（该退出按键不评分、不翻页）。
- 播放中抑制熄屏/深睡（与导入同级，`import_active` 同类抑制条件）。

## 5. 菜单与存储（flash 不动）

- 菜单在现有行后加 4 行：新卡限额（OK 循环 不限/10/20/50，默认 20）、打乱（OK 切 开/关，默认关）、未来 3 天预报（只读一行 明A/后B/大后C）、自动播放（OK 开/关当前范围的播放；`autoplay` NVS 存开关持久化，作用域=开启时的当前学习范围，重启后播放关闭）。
- NVS 新键 `new_limit/new_done/new_day/shuffle/autoplay`：经 `load_u8_setting` 白名单校验 + commit，非法值回默认。
- `store` 新增 `pa_store_set_new_limit/pa_store_set_new_done/pa_store_set_shuffle/pa_store_forecast/pa_store_is_new`（只读扫描，饱和计数），`pa_store_next` 内部实现限额 + 水库采样。
- Cover Arts compact 槽（上限 256）同样可用。
- 测试：host 单测覆盖限额跳过 NEW、水库采样分布、预报分桶、跨日清零；`tools/validate_firmware.sh` 保持通过。
