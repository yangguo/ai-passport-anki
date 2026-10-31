# 状态

2026-10-31：同一份应用可以装进 Cover Arts 槽，也可以作为单 factory 整机刷写。槽内卡片空间是镜像后的 60 KiB（结构上限 256 张、4096 字节正文），不是 12,000 张。独立 factory 尾部仍是原来的 12,000 张窗口。学习画面改为六行正文和一行评分。真机验收仍为 NOT RUN。详情见 [build.md](build.md) 和 [user-guide.md](user-guide.md)。

2026-10-11：离线首版已实现并通过主机测试与ESP32-C3固件构建；真机验收仍为 NOT RUN。

先提交详细设计（`b9166fa`），再由 Luna high 编码实现；后续修正、字体工具升级也由 Luna high 完成。

| 里程碑 | 状态 | 证据 |
|---|---|---|
| M0 设计与公共仓库 | 完成 | docs/design.md，独立设计提交先于业务实现 |
| M1 调度/UTF-8/CRC | PASS | CTest core/codec，ASan/UBSan |
| M2 电脑导入/协议 | PASS | pytest20/20；协议、导入事务host测试；传输fake serial |
| M3 固件/中文/存档/UI | 构建PASS | ESP-IDF5.5.3；实机显示、输入和存档验收未执行 |
| M4 CI镜像与使用文档 | PASS | app/full产物已下载，来源、分区、内容与SHA256核对 |
| M5 真机验收 | NOT RUN | 未刷写设备 |

构建源码提交：`d5d3311122f80f292cde35752e96c1e5bad1f240`。

[成功固件构建](https://github.com/yangguo/ai-passport-anki/actions/runs/38131196721)、[成功主机测试](https://github.com/yangguo/ai-passport-anki/actions/runs/38131196776)。具体记录见 [acceptance.md](acceptance.md)。

范围：最多 12,000 张多牌组纯文本、三键分页、四评分简化调度、分区存档、USB 分块导入。导入替换整组并清零复习历史；从旧 PAK1 迁移时保留复习状态。学习时钟不计关机时间。电脑可以读取本机 Anki 并把评分经 AnkiConnect 交回，由用户在官方 Anki 里同步 AnkiWeb。设备不上网，不保存凭据。不包含 FSRS、设备端 AnkiWeb、图片/音频、完整 Anki 模板或 anki21b。

## 万张容量（2026-10-11）

正文改到 `cards` 分区的双槽里按需读取，调度记录放在 `sched` 分区的 4 KiB 块中，到期队列按块扫描。稳态 RAM 是一份 `pa_store`（不超过 12 KiB）加上当前卡的两面缓冲。提交时的临时堆有 64 KiB 上限，选卡和评分不再分配。协议为 version 2：`BEGIN count crc length`、`CHUNK`、`RESUME`、`COMMIT`。

用 `eggrolls-JLPT10k-v3.5` 的 `collection.anki21`（10,641 张、11 个牌组）在电脑上转换：正面最长 52 字节，背面最长 479 字节，其中 105 张背面超过 255 字节，没有一面超过 511 字节。规范 payload 1,944,239 字节，放进 2,097,152 字节的槽 payload。转换后的字符都在更新后的 Noto Sans SC 字体里（请求 22,447，生成 21,957）。字体补了假名、带圈声调号、省略号和 α。

主机测试覆盖 10,000 张导入、选卡和评分时堆占用为 0、评分扇区写失败后旧卡仍在、超级块未发布时旧牌组仍在、PAK1 迁移保留 Again 的到期时间。`pa_store` 静态大小 7,840 字节。提交 10,000 张时的临时堆低于 64 KiB，返回后为 0。

本地 `tools/validate_firmware.sh`（ESP-IDF 5.5.3）通过。应用镜像 2,176,832 / 2,621,440 字节，factory 余量 444,608 字节。链接后 DRAM 115,842 字节，剩余 205,454 / 321,296。Flash 只读数据约 1.62 MiB，其中大部分是字体。cards 分区 4,587,520 字节（两槽各 2,293,760，payload 上限 2,097,152），sched 分区 851,968 字节。这些是编译和主机测试数字。真机刷写、USB 传输和掉电仍是 NOT RUN。

## M6 桌面学习模拟器（2026-10-11）

Luna high 新增 `passport_sim`，复用固件 C 核心、快照和导入事务。支持命令行导入、翻面、四评分、学习时钟推进和重启恢复；不运行 LVGL，也不模拟硬件。

本地 ASan/UBSan 构建与 CTest 5/5、pytest 30/30 均通过；使用真实 `tools/deck.py` 生成 demo.tsv 的8张卡导入命令后，中文牌组复习及存档通过。详情见 [simulator.md](simulator.md)。真机验收仍为 NOT RUN。

源码提交：`4d7e63cfb7797eab056704e50d712044a71b2a8c`；[Host CI SUCCESS](https://github.com/yangguo/ai-passport-anki/actions/runs/38132744938)（CTest 5/5、pytest 30/30）；[Firmware CI SUCCESS](https://github.com/yangguo/ai-passport-anki/actions/runs/38132744957)（编译、分区/size检查、合并镜像及上传产物）。本次未另行下载镜像校验，也未执行真机验收。

## 本机 Anki 路径（2026-10-12）

`sched` 分区表未改。末尾 48 个扇区（192 KiB）改成复习日志，每条 8 字节，最多 24,384 次评分。调度池从 204 个扇区减到 156 个，导入仍能同时放下新旧 epoch 的 120 块，并留下 36 个空闲扇区。`pa_store` 静态大小从 7,840 字节增到 7,904 字节（+64）。协议仍是 version 2，新增只读 `EXPORT` / `XCHUNK` / `XRESUME`。主机 `anki-decks`、`anki-pull`、`sync-back` 见 [user-guide.md](user-guide.md) 和 [ankiweb-sync.md](ankiweb-sync.md) 第 13 节。

主机：`CC=gcc` 的 ASan/UBSan CTest 6/6 通过。`test_store` 打印 `pa_store` 7,904 字节、复习日志上限 24,384。pytest `tests/test_deck.py`、`tests/test_anki_sync.py`、`tests/test_simulator.py` 共 43 项通过，覆盖 AnkiConnect 假服务器、schema 11/18 只读副本、导出续传与 CRC、回写幂等和 dry-run。

本地 `IDF_PATH` 指向 ESP-IDF 5.5.3 时，`tools/validate_firmware.sh` 通过。相对上一节的万张容量构建：

| 项目 | 万张容量 | 本次 | 差值 |
|---|---:|---:|---:|
| 应用镜像 | 2,176,832 | 2,180,048 | +3,216 |
| factory 余量（上限 2,621,440） | 444,608 | 441,392 | −3,216 |
| DRAM 已用（共 321,296） | 115,842 | 115,906 | +64 |
| DRAM 剩余 | 205,454 | 205,390 | −64 |
| `pa_store` 静态大小 | 7,840 | 7,904 | +64 |

应用镜像 2,180,048 / 2,621,440 字节，余量 441,392 字节（17%）。链接后 Flash 数据 1,619,132 字节，Flash 代码 484,718 字节。DRAM 增加的 64 字节与 `pa_store` 增加的导出和日志字段一致。分区表未改。这些是编译和主机测试数字。真机刷写、USB 传输、真实 Anki Desktop 和 AnkiWeb 同步仍是 NOT RUN。
