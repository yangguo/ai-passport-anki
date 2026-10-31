# ai-passport-anki

FoloToy AI Passport 离线文字闪卡复习器：牌组选择 → 翻面 → Again/Hard/Good/Easy 评分 → 间隔复习 → 保存进度。

参考 [AnkiDroid](https://github.com/ankidroid/Anki-Android) 的学习流程和 [消消乐实现分支](https://github.com/yangguo/ai-passport-xiaoxiaole/tree/cursor/xiaoxiaole-v1-impl-19f9) 的 FoloToy BSP。ESP32-C3 / 8 MiB flash / 无 PSRAM / 240×320屏 / 三个按键。

先提交[详细开发设计](docs/design.md)，再由 **Luna（high）** 实现；架构、协议、数据格式和验收标准均在设计中定义。

## 首版范围

- 最多 12,000 张、多牌组、中英日纯文本；每面最多 511 UTF-8 字节。
- 四档评分与 1 分钟/10 分钟学习步骤；简化间隔复习调度。
- 正文和调度写在当前应用镜像之后的尾部，按块读写并带 CRC。独立刷写时仍是最多 12,000 张；Cover Arts 槽只有约 60 KiB，见[构建说明](docs/build.md)。旧的 64 张 PAK1 快照在放得进 NVS 时可以迁入。
- 电脑 UTF-8 TSV 导入；旧式 Basic APKG，以及 JLPT 字段牌组（假名展开、丢弃音频图片）。
- 三键分页、学习日推进；字体覆盖清单中的 21,957 个码点，缺字明确拒绝。

完整 Anki 模板、FSRS、图片和音频不在范围内。设备不上网，也不保存 AnkiWeb 凭据。电脑可以读取已经同步到本机的 Anki 集合，并把设备评分经 AnkiConnect 交回本机 Anki，再由用户在官方 Anki 里同步 AnkiWeb。离线学习时钟不计入关机时间，日期由菜单或电脑命令推进。导入会整体替换卡片并重置复习历史。

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
python3 -m pytest tests/test_deck.py tests/test_anki_sync.py tests/test_simulator.py tests/test_image_pad.py -q
```

## 桌面学习模拟器

```sh
./build/host/passport_sim --state ./study.pak
```

模拟器可复用调度、快照和导入协议进行命令行练习，但不模拟 LVGL、实体屏幕/按键或 USB 设备时序。完整命令和边界见[模拟器说明](docs/simulator.md)。

## 固件构建

```sh
. /path/to/esp-idf-v5.5.3/export.sh
bash tools/validate_firmware.sh
```

ESP-IDF锁5.5.3，LVGL锁9.5系列；构建产物位于 `build/artifacts/`。Cover Arts 安装 `app.bin`。`full.bin` 只用于准备整机覆盖的设备，不要刷到已经有启动器的 `0x0`。也可查看 [GitHub Actions](https://github.com/yangguo/ai-passport-anki/actions) 构建并下载产物。

[构建/刷写说明](docs/build.md)、[项目状态](docs/status.md)、[验收证据](docs/acceptance.md)。真机验收单独记录；本次开发未刷写设备。

## 来源与许可

自主实现部分MIT；BSP保留FoloToy MIT；Noto Sans CJK字体保留SIL OFL。未复制AnkiDroid Android代码、Anki后端或商标图标。[来源记录](docs/sources.md)，[字体生成记录](assets/fonts/README.md)。
