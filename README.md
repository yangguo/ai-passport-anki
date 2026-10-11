# ai-passport-anki

FoloToy AI Passport 离线文字闪卡复习器：牌组选择 → 翻面 → Again/Hard/Good/Easy 评分 → 间隔复习 → 保存进度。

参考 [AnkiDroid](https://github.com/ankidroid/Anki-Android) 的学习流程和 [消消乐实现分支](https://github.com/yangguo/ai-passport-xiaoxiaole/tree/cursor/xiaoxiaole-v1-impl-19f9) 的 FoloToy BSP。ESP32-C3 / 8 MiB flash / 无 PSRAM / 240×320屏 / 三个按键。

先提交[详细开发设计](docs/design.md)，再由 **Luna（high）** 实现；架构、协议、数据格式和验收标准均在设计中定义。

## 首版范围

- 最多64张、多牌组、中英文纯文本；每面最多255 UTF-8字节。
- 四档评分与1分钟/10分钟学习步骤；简化间隔复习调度。
- NVS保存复习进度；有版本、CRC校验的便携快照。
- 电脑UTF-8 TSV导入；支持有限旧式 Basic APKG文本转换。
- 三键分页、学习日推进；中文字体实际覆盖21,509个码点，缺字明确拒绝。

完整 Anki 模板/FSRS/AnkiWeb/图片/音频不在首版范围。离线学习时钟不计入关机时间，日期由菜单或电脑命令推进。导入会整体替换卡片并重置复习历史。

## 电脑导入

```sh
python3 tools/deck.py inspect decks/demo.tsv
python3 tools/deck.py commands decks/demo.tsv --output demo.commands
python3 -m pip install -r requirements.txt
python3 tools/deck.py upload decks/demo.tsv --port /dev/cu.usbmodem1101
```

端口改为实际设备路径；只有设备返回 `OK COMMIT` 才报告成功。详见[使用说明](docs/user-guide.md)。

## 主机测试

```sh
cmake -S . -B build/host -DANKI_HOST=ON -DANKI_SANITIZERS=ON
cmake --build build/host
ctest --test-dir build/host --output-on-failure
python3 -m pip install -r requirements-dev.txt
python3 -m pytest tests/test_deck.py -q
```

## 固件构建

```sh
. /path/to/esp-idf-v5.5.3/export.sh
bash tools/validate_firmware.sh
```

ESP-IDF锁5.5.3，LVGL锁9.5系列；构建产物位于 `build/artifacts/`，包含app、bootloader、partition-table和完整factory镜像。也可查看 [GitHub Actions](https://github.com/yangguo/ai-passport-anki/actions) 构建并下载产物。

[构建/刷写说明](docs/build.md)、[项目状态](docs/status.md)、[验收证据](docs/acceptance.md)。真机验收单独记录；本次开发未刷写设备。

## 来源与许可

自主实现部分MIT；BSP保留FoloToy MIT；Noto Sans CJK字体保留SIL OFL。未复制AnkiDroid Android代码、Anki后端或商标图标。[来源记录](docs/sources.md)，[字体生成记录](assets/fonts/README.md)。
