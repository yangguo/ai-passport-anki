# FoloToy AI Passport Anki：详细开发设计 v1

日期：2026-10-11。目标仓库：`yangguo/ai-passport-anki`。用户已选择：离线文字卡片、间隔复习、电脑导入牌组；先完成设计，再使用 Luna high 编码。

## 1. 产品目标与移植边界

在 AI Passport 上完成独立的离线学习闭环：选择牌组 → 显示问题 → 翻面 → 查看答案 → 四档评分 → 保存调度结果 → 下一张到期卡。电脑负责编辑与导出牌组，设备负责复习。首版必须包含实际固件、可运行主机测试、导入工具和 CI，不能仅交付 README。

AnkiDroid 是 Android/Kotlin 应用，其 Android 页面、WebView、SQLite/Anki 后端不能直接搬到 ESP32-C3。采用功能移植：独立 C11 调度/数据核心，ESP-IDF/LVGL 重新实现三键 UI。此产品不是官方 AnkiDroid，也不宣称调度与 Anki/FSRS 完全一致。

| 功能 | v1 | 边界 |
|---|---|---|
| 多牌组、正反面纯文本 | 实现 | 最多 12,000 张，单面 UTF-8 最多 511 字节，牌组名最多 47 字节 |
| Again/Hard/Good/Easy | 实现 | 本文定义的简化调度；不是 FSRS |
| 断电恢复 | 实现 | 评分先把调度块写到新扇区，超级块最后发布；导入在超级块写入前不影响旧牌组 |
| 中文显示、长文本分页 | 实现 | 覆盖中文基本区及规定字符范围；不支持 emoji/富文本 |
| 电脑 USB 导入 | 实现 | TSV；旧式 Basic APKG；JLPT 字段牌组。分块上传，CRC 校验后整组替换并重置复习记录 |
| 完整 APKG/Anki 集合兼容 | 不包含 | 不运行 Anki 模板引擎；不提取媒体；anki21b 拒绝。Anki 的 i64 卡片 ID 只留在电脑映射里 |
| 图片、音频、TTS、手写、数学公式 | 不包含 | 后续资源包设计 |
| AnkiWeb、Wi-Fi、账号、FSRS | 不包含 | 后续独立里程碑 |
| 自动 RTC/关机计时 | 不包含 | 离线学习时钟，用户明确推进日期 |
| 自动深睡眠/OTA/多应用启动器 | 不包含 | 首版独立 factory 固件；先验证稳定性 |

## 2. 方案比较与选型

1. **独立嵌入式复习终端（采用）**：保留学习体验；内存有界、离线可用、易测试。牺牲原始 Anki 调度/同步兼容。
2. 移植完整 Anki 后端：需要 SQLite、模板/HTML、压缩和更大的运行环境；不适合本机无 PSRAM 的约束。
3. 设备作为电脑/手机的远程显示器：兼容性高，但学习依赖外部连接，偏离离线需求。

## 3. 参考来源与许可

- AnkiDroid：<https://github.com/ankidroid/Anki-Android>。参考牌组、翻面、四档评分和间隔复习功能；不复制 Android 或 Anki 后端代码。上游 README 区分 GPL-3.0 主程序、AGPL 后端、LGPL API。
- 消消乐：<https://github.com/yangguo/ai-passport-xiaoxiaole>。实际固件参考分支 `cursor/xiaoxiaole-v1-impl-19f9`，完整提交 `97924dcaa76bd458a3f2a529230b5a4878e79476`；main 当前为设计文档。
- BSP 原始来源 FoloToy/ai-passport，沿用消消乐快照中的 MIT 文件和全部通知，保留 IMPORT.md；新增本项目导入记录，不能假称此前 BSP 从未修改。
- 本项目自主编写部分采用 MIT。中文字体采用 Noto Sans SC，保留 SIL OFL；不复制 Anki 商标/图标，不把参考项目 GPL 文件换成 MIT。

## 4. 硬件与工具链

| 项目 | 数值/处理 |
|---|---|
| MCU | ESP32-C3，8 MiB flash，无 PSRAM |
| 屏幕 | ST7789P3 240×320，RGB565，BSP 圆角遮罩半径 30 |
| LCD SPI | MOSI GPIO9，SCLK GPIO8，CS GPIO1，DC GPIO20；BL GPIO21 |
| 按键 | GPIO0 ADC 分压 UP/DOWN/OK，使用 BSP 校准/窗口 |
| 输入 | 单击事件和 OK 长按；不同时处理 PRESS、CLICK 导致双动作 |
| 电源/电量 | BSP wake-release，电量初始化失败可以降级；不自动擦 NVS |
| 音频 | 硬件 ES8311，首版不启动音频任务 |
| SDK | 锁 ESP-IDF 5.5.3，与参考 BSP 匹配；不混用 XiaoZhi IDF 6.x |
| GUI | LVGL 9.5.x，esp_lvgl_port 2.9.0 |
| USB | USB Serial/JTAG 控制台；不启用 UART0，避免 GPIO21 背光冲突 |

8 MiB 分区（`partitions.csv`）在保留原 NVS 起点的前提下为卡片正文和调度留出空间。NVS 仍是 `0x9000/0x40000`（256 KiB），这样已有设备上的 PAK1 `snapshot` 键还能读到并迁移。应用分区保持 64 KiB 对齐，起点仍是 `0x50000`，长度改为 `0x280000`（2.5 MiB）。`0x4A000..0x50000` 的空隙是应用对齐留下的，与旧表相同。

| 分区 | 偏移 | 大小 | 用途 |
|---|---:|---:|---|
| nvs | 0x9000 | 0x40000 | 时钟无关的遗留 PAK1；迁移成功后只删 `snapshot` 键 |
| phy_init | 0x49000 | 0x1000 | PHY |
| factory | 0x50000 | 0x280000 | 应用，含字体 |
| cards | 0x2D0000 | 0x460000 | 两份正文槽，每槽 0x230000 |
| sched | 0x730000 | 0xD0000 | 超级块、导入进度、调度块池 |

表尾是 `0x800000`。这不是其它项目的 app-only 地址；更新必须整表刷写，不能只覆盖旧 factory。cards 子类型 `0x40`，sched 子类型 `0x41`，固件按名字查找。

## 5. 架构与目录

```text
电脑 TSV / 旧式 APKG
  → Python 规范化/限制校验 → 有界 USB 行协议
  → console reader → FreeRTOS 消息队列
  → 单一 app owner → import staging → portable snapshot → NVS commit
                                    ↓ 成功后替换 live
按键 callback → 同一队列 → UI/调度 → snapshot commit → 新画面
                               ↓
                        LVGL（持 BSP lock）
```

| 模块 | 责任 |
|---|---|
| components/passport_anki | 纯 C11 collection、调度、UTF-8 校验、快照编解码，无 heap/时钟/ESP 依赖 |
| components/bsp | 引脚、显示、按键、电源、电量；不与业务耦合 |
| main | 单 owner 事件循环、LVGL、串口、NVS、逻辑时钟 |
| tools/deck.py | TSV/APKG → 严格规范化牌组 → inspect/export/upload |
| tools/font | 可重现中文字体生成与覆盖清单 |
| tests | core、codec、导入格式、协议/事务边界 |
| docs | 设计、开发计划、来源、构建、验收、状态 |
| .github/workflows | host sanitizer/Python tests、ESP-IDF build、固件产物 |

业务状态只由 app task 修改。按键回调只做非阻塞队列入队；console reader 不直接操作 NVS/LVGL/collection。LVGL 操作加 BSP lock，获取失败时稍后重试，不能无锁调用。

## 6. 数据模型与限制

设备上的正文和调度不再放进一个 RAM 数组。`pa_collection` / `PA_MAX_CARDS 64` / PAK1 编解码仍保留，只用于主机上的旧快照和迁移，不能把这个结构扩到 12,000 张。

运行时上限：

| 项 | 值 |
|---|---|
| 卡片 | 12,000（`PA_STORE_MAX_CARDS`）。目标牌组 10,641 张，留有余量 |
| 牌组 | 64。牌组名最多 47 UTF-8 字节，使用完整牌组路径，避免叶子名相撞 |
| 单面 | 511 UTF-8 字节，缓冲 512 含 NUL。超出即拒绝，不截断 |
| 设备 ID | 非零 u32，一次导入内唯一。电脑按 `cards.id` 顺序编成 1..n；Anki 的 i64 只写在可选 `--map` JSON，不进设备 |
| 正文镜像 | 每个槽的 payload 上限是 `0x200000`（2,097,152 字节）。主机必须在放不下时失败 |

UTF-8 规则不变：拒绝截断、多余 continuation、overlong、surrogate、大于 U+10FFFF、内嵌 NUL。正文允许换行，拒绝其它控制字符。空字段、重复 ID、超限都报错。

新卡默认 ease=2500、interval=0、due=now、stage=NEW、step=0。计数饱和到 UINT16_MAX。interval 上限 3650 天。now/due 加法溢出则失败且不改原记录。USB 导入替换整组并清零复习历史；从 PAK1 迁移时按原调度字段搬入。工具在 upload 前说明替换效果。

### 6.1 正文分区

`cards` 分成两个等大槽。槽 0 的头在分区起点。每个槽：

- 4 KiB 头：magic `PCT1`，version u16=1，count，epoch，payload 偏移/长度/CRC，索引 CRC，牌组数，头 32 字节的 CRC32（IEEE，与 `pa_crc32` 相同）。
- 紧随其后的牌组名，每名 48 字节含 NUL。
- 索引从 4 KiB 对齐的 `PA_INDEX_OFF` 开始。每条 16 字节：id、front 偏移、front 长度、back 长度、back 偏移。12000 条向上取整到扇区后，payload 从 `0x30000` 开始。
- payload 是主机发来的规范记录流，不在设备上再打包。记录为小端：`id:u32, deck_len:u8, front_len:u16, back_len:u16, bytes`。

读一张卡只读 16 字节索引和两侧正文，不把牌组载入 RAM。挂载时按扇区分段核对索引 CRC 和 payload CRC。

### 6.2 调度分区

`sched` 前 4 个扇区是元数据，其余是池（本表 204 个扇区，代码上限 240）。

- 扇区 0 和 1：超级块 `PSS2`，version=2。双槽轮换，读 generation 较大且 CRC 通过的一份。字段：count、now、epoch、活动槽、牌组数。
- 扇区 2 和 3：导入进度 `PIM1`。记录已落到 flash 的 payload 偏移（满扇区，或最后一块不足扇区但已写完）。RAM 里未刷出的尾部在重启后丢弃。
- 池内每块 4 KiB，magic `PSCH`。头 32 字节含 generation、擦除计数、epoch、块号、记录数、记录区 CRC。每条调度记录 20 字节，一块 203 条。12,000 张需要 60 块。

到期队列按块流式扫描：先 LEARNING，再 REVIEW，再 NEW，然后 due 升序、id 升序。不把 12,000 条抄进 RAM。

评分把改过的那一块写到擦除次数最低的空闲扇区，块头 CRC 最后写入。成功后这块的 generation 更高；断电时旧块仍在，挂载会选 generation 最大的有效块。超级块的 now 更新是尽力而为，块写入才是评分的提交点。多次评分同一张卡会在空闲扇区之间轮换，不会反复擦同一扇区。导入时池里要同时放下旧 epoch 和新 epoch 的块（最多 120）以及余量。

导入发布顺序：写索引、写新 epoch 的调度块、写槽头、最后写超级块，然后清进度。超级块写入之前崩溃，旧牌组仍是活动牌组。显式导入可以覆盖 `PA_STORE_CORRUPT`；不会因为挂载失败去擦 NVS。

### 6.3 从 PAK1 迁移

仅当新存储挂载为 `EMPTY` 时，读取 NVS namespace `pa_anki`、key `snapshot`。用现有 `pa_decode` 校验 magic `PAK1`、version 1 和 CRC。空快照只发布超级块。非空快照经 `pa_store_import_collection` 写入，并沿用原 due/ease/interval/stage。成功后 `nvs_erase_key("snapshot")` 并 commit。解码失败则保留该键，不调用 `nvs_flash_erase`。已经是 CORRUPT 的 v2 存储不会被自动迁移覆盖，只能由用户重新导入。模拟器遇到 PAK1 文件同样迁移成 PAK2。

## 7. 调度算法（精确定义）

时间单位分钟。一天=1440分钟。学习步骤为 1 分钟和 10 分钟。学习时钟 now 只由明确输入和当前开机时间推进。

| 原状态 | Again | Hard | Good | Easy |
|---|---|---|---|---|
| NEW | LEARNING step0，+1min | LEARNING step0，+6min | LEARNING step1，+10min | REVIEW，4天 |
| LEARNING step0 | step0，+1min | step0，+6min | step1，+10min | REVIEW，4天 |
| LEARNING step1 | step0，+1min | step1，+6min | REVIEW，1天 | REVIEW，4天 |
| REVIEW interval=I | LEARNING step0，+1min；lapses+1；ease−200 | REVIEW，max(I+1,ceil(1.2I))天；ease−150 | REVIEW，max(I+1,round(I×ease/1000))天 | REVIEW，max(Good+1,round(I×ease×1.3/1000))天；ease+150 |

计算使用整数和足够宽的中间值。ease 范围 1300..3500。间隔上限 3650 天，达到上限时 Hard/Good/Easy 可以相同，不伪称严格递增。Again 后重新完成学习时按1天毕业（简化遗忘策略）。评分结果先在候选 card 上计算，验证成功再赋值。

选卡规则不变：`due <= now`；LEARNING 优先，其次 REVIEW，最后 NEW；同一类别按 due 升序、id 升序。没有匹配返回 `PA_NO_CARD`。没有每日新卡限额；12,000 张是总量上限。闪存实现按调度块扫描，比较规则与 `pa_next` 相同。评分先在 `pa_sched` 候选上计算，通过 `pa_sched_valid` 后才写入新块。

会话 `pa_session` 包含 current index/revealed。评分必须当前卡有效、已翻面、rating 0..3；成功后关闭 revealed，不能连按 OK 重复给同一卡评分。

## 8. 三键 UI 与排版

统一安全内容矩形 x=18..221、y=30..289。背景深色，正文16px，标题和页脚较小；避免圆角裁切。主状态：DECKS、QUESTION、ANSWER、RATING、EMPTY、NEXT_DAY_CONFIRM、IMPORTING、ERROR。

| 状态 | UP/DOWN 短按 | OK 短按 | OK 长按 |
|---|---|---|---|
| DECKS | 切换牌组/“下一学习日” | 进入牌组/日期确认 | 回到牌组首页 |
| QUESTION | 上/下正文页 | 翻面，答案从第一页开始 | 回牌组 |
| ANSWER | 上/下答案页 | 进入评分选择（默认 Good） | 回牌组，当前卡不评分 |
| RATING | 四档循环选择 | 保存评分，成功后下一张 | 回答案 |
| EMPTY | 可查看提示 | 重新检查到期队列 | 回牌组 |
| NEXT_DAY_CONFIRM | 取消/确认选择 | 确认则逻辑日+1并保存 | 取消 |
| IMPORTING | 忽略 | 忽略 | 中止事务 |
| ERROR | 无业务变更 | 重试/返回 | 返回安全首页 |

每屏显示牌组、逻辑 day、当前卡/总数、Q/A、页码、操作提示。答案不能靠 UP/DOWN 无意泄露。长文本必须按 UTF-8 字符边界和字体宽度自动换行后分页；不能把 CJK 字节硬切。正文最大 511 字节。分页按 UTF-8 边界和字体宽度换行；中文、假名、英文长单词和换行都要测。每屏最多 4 行，页数上限 64，511 字节能放下。

没有到期卡显示“本次已完成”及等待/推进学习日入口，不自动推进一天以凑下一张。Again 的一分钟到期要由开机计时器触发刷新，无需离开页面。

## 9. 字体与字符覆盖

生成 Noto Sans SC 16px、2bpp、const flash 字体。请求范围在原有 ASCII、拉丁扩展、中文基本区和全角之外，增加希腊字母 U+0370..03FF、通用标点 U+2000..206F、带圈数字 U+2460..24FF、假名区 U+3000..30FF 和片假名扩展 U+31F0..31FF，用来显示 JLPT 牌组里的假名、声调圈号、省略号和 α。换行无 glyph。上游字体没有的码点不生成。清单记录请求 22,447、实际 21,957、缺失 490。`tools/deck.py` 只按这份清单拒绝缺字，不再用一份更窄的硬编码区间。设备在 COMMIT 时再用字体度量复检，缺字整组失败。

不把字体加载到 RAM。首版可先用内置 ASCII 字体通过编译，但交付中文能力必须实际生成并构建中文字体。若完整字库超过 flash 预算，改用明确的子集并由导入工具拒绝缺字，不能以方框显示伪装中文支持。字体生成失败不得阻塞全部 host 开发，但必须标为验收缺项。

## 10. 时钟与日期

无 RTC/网络校时，不能推测关机多久。初始逻辑 day=0；开机后 esp_timer 经过每完整分钟推进 now。每5分钟做一次 checkpoint（并在评分、日期操作、导入时持久化）。断电可能丢失最多5分钟空闲计时，但已经确认成功的评分不会丢失。

菜单“下一学习日”将 day推进1，并保持日内分钟；电脑 `DAY N` 将逻辑日期推进到 N×1440，拒绝倒退和溢出。明确显示学习日而非真实日历日期。重启不自动加一天。自动深睡眠暂不实现，避免时钟、按键唤醒、NVS持久化混为未经验证的功能。

## 11. 存档与断电一致性

卡片正文和调度在 `cards` / `sched` 分区，不放进一个 NVS blob。NVS 只承接遗留 PAK1。`nvs_flash_init` 失败时记录错误，不调用 `nvs_flash_erase`，新分区仍可导入。

PAK1（magic `PAK1`，version 1，末尾 CRC32）仍是迁移输入。解码失败时目标数据不变。满载旧快照约 37 KiB，放得进保留的 256 KiB NVS。迁移细节见 6.3。

评分：读出当前调度块，在候选 `pa_sched` 上计算，写到新扇区。写失败则 RAM 映射不变，界面保留答案和选择。写成功才进入下一张。超级块更新失败不把这次评分判成失败，因为新块已经带有更高的 generation，重新挂载仍能看到它。

导入：数据先进入非活动槽。进度扇区只记录已经写完的扇区。COMMIT 重读 payload、核对 CRC、拒绝重复 ID，再发布。发布前断电保留旧牌组。CRC 或文本不合法则不写超级块。损坏的 v2 存储显示错误，只接受明确的新导入。

模拟器把两份分区镜像存成 PAK2（magic、version 2、两段长度、两段拼接的 CRC32），临时文件 fsync 后 rename。垃圾文件直接拒绝，不覆盖。保存失败时把进程内镜像恢复到写入前并重新挂载。

## 12. 电脑导入格式与协议

规范 TSV：UTF-8（允许 BOM），标题 `id\tdeck\tfront\tback`。使用 Python csv delimiter=tab，支持引用字段内的真实换行；ID 十进制1..UINT32_MAX。可支持 Anki文本导出（前/后两列，#分隔符/HTML注释），规范化由工具完成。

旧式 APKG：只读 zip 内的 SQLite。同时存在 `collection.anki21` 和较小的 `collection.anki2` 时用 anki21（JLPT 包里的 anki2 是空壳）。`collection.anki21b` 直接拒绝。不解压媒体，不执行 SQL 扩展或模板脚本。Basic 模型取前两个文本字段。字段名含 `VocabKanji`、`VocabFurigana`、`VocabDefSC` 的牌组按 JLPT 规则转换：正面是词条，若注音不同再加一行；背面依次放声调、词性、简体释义、补充、第一条例句注音和简体例句释义。先去掉 `[sound:...]` 和图片标签，HTML 转成纯文本并保留 `<br>` 换行，再把 `汉字[假名]` 收成 `汉字（假名）`。背面超过 511 字节时先丢掉补充，再丢掉例句释义，仍然超出就报错，不截断。Cloze 和其它模型拒绝。一台笔记多张卡拒绝。

行协议 ASCII，正文 hex编码UTF-8；最大行1200字节，串口输入超过上限必须丢弃整个行并报错。reader遇到断线/半行不能重复上一命令。

协议 version 2。正文不再一行一张卡，而是规范记录的二进制分块。每块最多 480 字节，hex 后仍低于 1200 字节行长。`zlib.crc32` 与设备 `pa_crc32` 对同一 payload 结果相同。

```text
INFO                              -> OK INFO version=2 cards=N day=D cap=12000
BEGIN count crc length            -> OK BEGIN resume=N
CHUNK offset hex                  -> OK CHUNK accepted
RESUME                            -> OK RESUME offset=N count=N crc=N length=N
COMMIT                            -> OK COMMIT
ABORT                             -> OK ABORT
DAY N                             -> OK DAY
```

`resume` 是已经落盘的偏移；同一进程里若还有未刷出的尾部，RESUME 会把这段也算进去。主机从该偏移继续，不盲目重发旧偏移。BEGIN 在设备重启后可以接上同一 count/crc/length 的进度。块确认丢失时先 RESUME，对不上才 ABORT。COMMIT 在 payload 到齐且 CRC、牌组数、字体都通过后才回答 OK。解析器仍接受旧的 CARD 行，固件和模拟器不把它当作导入路径。

错误仍是 `ERR <code>`。COMMIT 失败不能回 OK。再次 BEGIN 在事务未结束时拒绝。60 秒无输入自动 ABORT。长按 OK 可中止。队列仍是 4 条，忙则 `ERR BUSY`。工具逐条等 ACK，不把启动日志当成 ACK。

导入脚本：`inspect input.tsv` 验证并统计；`commands input.tsv --output deck.commands` 离线导出；`upload input.tsv --port /dev/cu.usbmodem...`（pyserial可选依赖）串行执行，校验所有ACK，只有 OK COMMIT 才报告成功。超时/ERR报告失败，尝试ABORT。先INFO检查协议版本。

## 13. RAM、flash和性能预算

| 项目 | 预算/要求 |
|---|---|
| `pa_store` | 静态一份，含 4 KiB 扇区缓冲和 64 个牌组名。编译期不超过 12 KiB；当前结构 7,840 字节 |
| 当前卡文本 | 正面、背面各 512 字节，牌组名 48 字节 |
| 提交时临时堆 | id 数组加牌组名表，高水位必须 `<= 64 KiB`，返回后为 0 |
| 稳态选卡/评分 | 不再 malloc |
| 消息队列/串口行 | 约 6 KiB；行长 <= 1200 |
| app task | 16 KiB 栈。挂载和读块用 `pa_store` 里的扇区缓冲，不在栈上再放 4 KiB |
| LVGL | pool 24 KiB，显示缓冲沿用 BSP |
| 字体 | flash const。假名和圈号大约增加几百个字形 |
| app flash | 必须小于 factory `0x280000`；CI 检查 |
| 选卡 | 按调度块扫描，块数最多 60，不随卡数把对象放进 RAM |
| 响应 | 目标短按到画面 <150ms；需真机测量，不能用编译 PASS 代替 |

最大配置可能接近C3 RAM上限：固件size和启动heap日志必需；若余量不足优先缩减静态双份临时缓冲/限制卡数，须同步工具、文档、测试，不能悄悄降低规格。

## 14. 验证矩阵与验收

主机：CMake+CTest，ASan/UBSan。覆盖学习步骤、四评分、due排序/牌组隔离、未翻面拒绝、重复评分拒绝、整数溢出、10000次重复复习、最大牌组、UTF-8边界。codec覆盖往返、每个截断长度、每个字节CRC破坏、未知版本、非法状态、目标不变。Python覆盖TSV/BOM/引用换行、重复ID/超限/unsupported APKG、HTML到文本、上传ACK/ERR/超时。

固件：ESP-IDF 5.5.3 build、size、merge-bin、分区边界。CI产物至少 app.bin、bootloader.bin、partition-table.bin、full.bin、构建与size记录，不把full.bin自动刷到用户设备。

真机项保持 NOT RUN：开机屏幕/圆角、ADC 三键、假名与长文分页、评分后断电恢复、Again 1 分钟重现、DAY 推进、万张 USB 导入、导入中断原数据保留、坏 CRC、堆/栈余量、30 分钟稳定复习、电量/电源。主机上的断电测试只模拟扇区写入失败和超级块未发布，不能代替真机掉电。没有实机证据不能标“移植完全验收”。

## 15. 里程碑与交付

M0：本设计、来源记录、公共仓库；独立设计提交先于业务实现。
M1：C11核心与codec，host sanitizer通过。
M2：Python牌组转换/导入协议测试通过。
M3：LVGL三键固件、NVS持久化、USB导入、中文字体；固件编译通过。
M4：发布可下载CI固件及清晰使用说明；真机测试保持单独状态。
M5：经用户后续授权进行刷写/真机验收，再决定FSRS、媒体或同步。

开发按文件责任分给 Luna（high）；并行工作不能改对方文件。负责人负责接口冻结、集成、review、复跑验收和GitHub远端结果确认。若测试/构建出现问题，修复仍由Luna high完成。
