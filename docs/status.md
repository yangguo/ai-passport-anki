# 状态

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

范围：最多64张多牌组纯文本、三键分页、四评分简化调度、NVS持久化、USB导入。导入替换整集合并清零复习历史；学习时钟不计关机时间。不包含FSRS、AnkiWeb、图片/音频、完整Anki模板或新式APKG兼容。

## M6 桌面学习模拟器（2026-10-11）

Luna high 新增 `passport_sim`，复用固件 C 核心、快照和导入事务。支持命令行导入、翻面、四评分、学习时钟推进和重启恢复；不运行 LVGL，也不模拟硬件。

本地 ASan/UBSan 构建与 CTest 5/5、pytest 30/30 均通过；使用真实 `tools/deck.py` 生成 demo.tsv 的8张卡导入命令后，中文牌组复习及存档通过。详情见 [simulator.md](simulator.md)。真机验收仍为 NOT RUN。
