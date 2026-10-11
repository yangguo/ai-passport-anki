# FoloToy AI Passport Anki：详细开发设计 v1

日期：2026-10-11。目标仓库：`yangguo/ai-passport-anki`。用户已选择：离线文字卡片、间隔复习、电脑导入牌组；先完成设计，再使用 Luna high 编码。

## 1. 产品目标与移植边界

在 AI Passport 上完成独立的离线学习闭环：选择牌组 → 显示问题 → 翻面 → 查看答案 → 四档评分 → 保存调度结果 → 下一张到期卡。电脑负责编辑与导出牌组，设备负责复习。首版必须包含实际固件、可运行主机测试、导入工具和 CI，不能仅交付 README。

AnkiDroid 是 Android/Kotlin 应用，其 Android 页面、WebView、SQLite/Anki 后端不能直接搬到 ESP32-C3。采用功能移植：独立 C11 调度/数据核心，ESP-IDF/LVGL 重新实现三键 UI。此产品不是官方 AnkiDroid，也不宣称调度与 Anki/FSRS 完全一致。

| 功能 | v1 | 边界 |
|---|---|---|
| 多牌组、正反面纯文本 | 实现 | 每次导入最多 64 张，单面 UTF-8 最多 255 字节 |
| Again/Hard/Good/Easy | 实现 | 本文定义的简化调度；不是 FSRS |
| 断电恢复 | 实现 | 成功评分必须已完成 NVS commit |
| 中文显示、长文本分页 | 实现 | 覆盖中文基本区及规定字符范围；不支持 emoji/富文本 |
| 电脑 USB 导入 | 实现 | TSV；旧式 APKG 转换；整体替换，并明确重置复习记录 |
| 完整 APKG/Anki 集合兼容 | 不包含 | 只转换旧式 SQLite Basic 文本，不运行 Anki 模板引擎 |
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

分区沿用参考独立固件方案：NVS `0x9000/0x6000` 太小，必须扩大以容纳一次完整牌组及 NVS blob 更新：NVS `0x9000/0x40000`，phy `0x49000/0x1000`，factory `0x50000/0x7b0000`。分区总边界为 8 MiB。不沿用旧固件 app-only 地址；文档强调只有核对实际分区表才能做保留配置的更新。

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

`pa_collection`: `now:u32`（逻辑分钟），`count:u16`，固定数组 64 张。

`pa_card`: `id:u32`、`due:u32`、`interval:u16`（天）、`ease:u16`（1000 倍比例）、`reviews:u16`、`lapses:u16`、`stage:u8`、`step:u8`、`deck[48]`、`front[256]`、`back[256]`。

- ID 非零、全 collection 唯一。相同牌组名表示同一牌组；最多 64 个不同牌组。
- deck 最多 47 UTF-8 字节；front/back 最多 255 字节，均必须非空且 NUL 结尾。
- 验证 UTF-8：拒绝截断、多余 continuation、overlong、surrogate、> U+10FFFF、文本中的内嵌 NUL。允许正文换行，拒绝非换行控制字符。
- 空字段、越界、重复 ID、超出卡数必须给出行号/错误原因，不静默截断。
- 新卡默认 ease=2500、interval=0、due=now、stage=NEW、step=0、计数为零。
- 计数饱和到 UINT16_MAX，不回绕；interval 上限 3650 天；now/due 加法溢出返回失败且不修改原卡。
- 导入整个 collection 替换；v1 不做按 ID 合并、不保留复习历史。工具在 upload 之前明确提示这种效果（用户发起上传即授权该操作）。

## 7. 调度算法（精确定义）

时间单位分钟。一天=1440分钟。学习步骤为 1 分钟和 10 分钟。学习时钟 now 只由明确输入和当前开机时间推进。

| 原状态 | Again | Hard | Good | Easy |
|---|---|---|---|---|
| NEW | LEARNING step0，+1min | LEARNING step0，+6min | LEARNING step1，+10min | REVIEW，4天 |
| LEARNING step0 | step0，+1min | step0，+6min | step1，+10min | REVIEW，4天 |
| LEARNING step1 | step0，+1min | step1，+6min | REVIEW，1天 | REVIEW，4天 |
| REVIEW interval=I | LEARNING step0，+1min；lapses+1；ease−200 | REVIEW，max(I+1,ceil(1.2I))天；ease−150 | REVIEW，max(I+1,round(I×ease/1000))天 | REVIEW，max(Good+1,round(I×ease×1.3/1000))天；ease+150 |

计算使用整数和足够宽的中间值。ease 范围 1300..3500。间隔上限 3650 天，达到上限时 Hard/Good/Easy 可以相同，不伪称严格递增。Again 后重新完成学习时按1天毕业（简化遗忘策略）。评分结果先在候选 card 上计算，验证成功再赋值。

`pa_next(collection, deck)`：选 `due <= now` 的匹配卡；LEARNING 优先，其次 REVIEW，最后 NEW；同一类别按 due 升序、id 升序。没有匹配返回 `PA_NO_CARD`。首版无每日新卡限额；64张上限控制总量。

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

每屏显示牌组、逻辑 day、当前卡/总数、Q/A、页码、操作提示。答案不能靠 UP/DOWN 无意泄露。长文本必须按 UTF-8 字符边界和字体宽度自动换行后分页；不能把 CJK 字节硬切。正文最大255字节，固定页索引数组足够；中文、英文长单词、换行都要测试。

没有到期卡显示“本次已完成”及等待/推进学习日入口，不自动推进一天以凑下一张。Again 的一分钟到期要由开机计时器触发刷新，无需离开页面。

## 9. 字体与字符覆盖

生成 Noto Sans SC 16px、2bpp、const flash 字体。目标范围为 ASCII U+0020..007E、拉丁扩展 U+00A0..024F、中文标点 U+3000..303F、中文基本区 U+4E00..9FFF、全角/半角 U+FF00..FFEF；换行无 glyph。实际字形覆盖以 assets/fonts/pa_font_16.manifest.json 为准：上游字体本身缺少的码点不生成，导入工具和设备拒绝缺字。输出 C 文件和覆盖清单；保留上游 OFL 与固定字体来源提交。

不把字体加载到 RAM。首版可先用内置 ASCII 字体通过编译，但交付中文能力必须实际生成并构建中文字体。若完整字库超过 flash 预算，改用明确的子集并由导入工具拒绝缺字，不能以方框显示伪装中文支持。字体生成失败不得阻塞全部 host 开发，但必须标为验收缺项。

## 10. 时钟与日期

无 RTC/网络校时，不能推测关机多久。初始逻辑 day=0；开机后 esp_timer 经过每完整分钟推进 now。每5分钟做一次 checkpoint（并在评分、日期操作、导入时持久化）。断电可能丢失最多5分钟空闲计时，但已经确认成功的评分不会丢失。

菜单“下一学习日”将 day推进1，并保持日内分钟；电脑 `DAY N` 将逻辑日期推进到 N×1440，拒绝倒退和溢出。明确显示学习日而非真实日历日期。重启不自动加一天。自动深睡眠暂不实现，避免时钟、按键唤醒、NVS持久化混为未经验证的功能。

## 11. 存档与断电一致性

namespace `pa_anki`，key `snapshot`，blob 由 portable codec 编码。NVS init 失败显示错误，不调用 nvs_flash_erase。

快照采用小端：magic `PAK1`，version u16=1，count u16，now u32；随后每张卡按本文数据字段顺序编码，字符串按 u16字节长度+UTF-8内容（不含NUL）；末尾 CRC32（IEEE）。不把原生C struct直接写入 flash。

解码先检查总长度、magic/version/count、CRC和每个字段，再检查重复ID、stage/step/ease/interval等不变量。失败时目标 collection 完全不变。避免36KiB局部变量压栈：采用两遍校验再提交，或明确调用者提供静态 staging。

评分：备份旧 card → 计算候选 → encode & nvs_set_blob & nvs_commit → 成功才显示已评分/进入下一张；失败恢复旧 card，保留答案和选择，可重试。导入：staging校验 → persist staging → swap live，任何失败继续使用原 collection。启动 corrupt blob 不自动覆盖；显示“存档损坏”并只允许明确导入替换或保留错误状态。

NVS空间需求：最大序列化约37KiB，更新期间旧/新 blob 可能共存，256KiB NVS提供页/索引余量。若 nvs_set_blob/commit 返回空间错误，报告失败，不擦除别的 namespace。

## 12. 电脑导入格式与协议

规范 TSV：UTF-8（允许 BOM），标题 `id\tdeck\tfront\tback`。使用 Python csv delimiter=tab，支持引用字段内的真实换行；ID 十进制1..UINT32_MAX。可支持 Anki文本导出（前/后两列，#分隔符/HTML注释），规范化由工具完成。

旧式 APKG：仅打开 zip 内 `collection.anki2` / `collection.anki21` SQLite；不提取任意路径，不执行SQL扩展或模板脚本。Basic模型取前两个文本字段；HTML用标准库解析成纯文本，保留 `<br>` 换行；Cloze/自定义复杂模型明确拒绝或说明有限转换，不能伪造完整卡片展开。压缩 collection.anki21b、媒体与新式包提示不支持，并建议从 Anki 导出 UTF-8 TSV。

行协议 ASCII，正文 hex编码UTF-8；最大行1200字节，串口输入超过上限必须丢弃整个行并报错。reader遇到断线/半行不能重复上一命令。

```text
INFO                  -> OK INFO version=1 cards=N day=N
BEGIN N               -> OK BEGIN       # 1..64；清空 staging；暂停复习
CARD ID DECKHEX FRONTHEX BACKHEX -> OK CARD
COMMIT                -> OK COMMIT      # 收到恰好 N 张且 NVS commit成功
ABORT                 -> OK ABORT
DAY N                 -> OK DAY         # 非事务期间，单调前进
```

错误统一 `ERR <code>`；不在 payload 含日志换行。COMMIT失败不能回 OK；CARD失败使事务失效或明确允许重试，工具遇ERR必须ABORT。BEGIN中再次BEGIN拒绝；缺卡COMMIT拒绝；断线事务60秒无输入自动ABORT；按键长OK允许中止。最大队列 4 条，串口忙需发送ERR BUSY，工具必须逐条等对应ACK，不把BSP/ESP日志误识别为ACK。

导入脚本：`inspect input.tsv` 验证并统计；`commands input.tsv --output deck.commands` 离线导出；`upload input.tsv --port /dev/cu.usbmodem...`（pyserial可选依赖）串行执行，校验所有ACK，只有 OK COMMIT 才报告成功。超时/ERR报告失败，尝试ABORT。先INFO检查协议版本。

## 13. RAM、flash和性能预算

| 项目 | 预算/要求 |
|---|---|
| live collection | 约37KiB，静态 |
| staging collection | 约37KiB，静态，复用解码/导入 |
| codec buffer | 40KiB，静态，禁止大栈数组 |
| 消息队列/串口行 | 约6KiB；消息长度<=1200 |
| app task | 8..12KiB 栈，实测水位后调整 |
| LVGL | pool 24KiB，显示缓冲沿用 BSP |
| 字体 | flash const，目标<=3MiB |
| app flash | <factory 0x7b0000；CI 检查 |
| 核心 | 无 malloc/rand/system clock；一次选卡O(64) |
| 响应 | 目标短按到画面<150ms；需真机测量，不能用编译PASS代替 |

最大配置可能接近C3 RAM上限：固件size和启动heap日志必需；若余量不足优先缩减静态双份临时缓冲/限制卡数，须同步工具、文档、测试，不能悄悄降低规格。

## 14. 验证矩阵与验收

主机：CMake+CTest，ASan/UBSan。覆盖学习步骤、四评分、due排序/牌组隔离、未翻面拒绝、重复评分拒绝、整数溢出、10000次重复复习、最大牌组、UTF-8边界。codec覆盖往返、每个截断长度、每个字节CRC破坏、未知版本、非法状态、目标不变。Python覆盖TSV/BOM/引用换行、重复ID/超限/unsupported APKG、HTML到文本、上传ACK/ERR/超时。

固件：ESP-IDF 5.5.3 build、size、merge-bin、分区边界。CI产物至少 app.bin、bootloader.bin、partition-table.bin、full.bin、构建与size记录，不把full.bin自动刷到用户设备。

真机项全部初始 NOT RUN：开机屏幕/圆角、ADC三键、中文/分页、评分后断电恢复、Again 1min重现、DAY推进、64张USB导入、导入中断原数据保留、坏CRC错误、堆/栈余量、30min稳定复习、电量/电源行为。没有实机证据不能标“移植完全验收”。

## 15. 里程碑与交付

M0：本设计、来源记录、公共仓库；独立设计提交先于业务实现。
M1：C11核心与codec，host sanitizer通过。
M2：Python牌组转换/导入协议测试通过。
M3：LVGL三键固件、NVS持久化、USB导入、中文字体；固件编译通过。
M4：发布可下载CI固件及清晰使用说明；真机测试保持单独状态。
M5：经用户后续授权进行刷写/真机验收，再决定FSRS、媒体或同步。

开发按文件责任分给 Luna（high）；并行工作不能改对方文件。负责人负责接口冻结、集成、review、复跑验收和GitHub远端结果确认。若测试/构建出现问题，修复仍由Luna high完成。
