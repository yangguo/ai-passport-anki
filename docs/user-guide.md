# 使用与兼容范围

## 卡片准备

电脑准备 UTF-8 TSV，第一行为四列：`id`、`deck`、`front`、`back`。每张卡 ID 唯一非零，多个相同 deck 名的卡归为同一牌组。最多64张、牌组名最多47 UTF-8字节、每面最多255 UTF-8字节；中文通常每字3字节。使用标准TSV引用字段可以包含换行。

从 Anki 导出文字时，优先采用 UTF-8 文本，再转换为本项目规范。旧式 Basic APKG 可转换；Cloze、复杂模板、collection.anki21b、媒体资源不在首版范围。转换失败时工具会明确报错。

```sh
python3 tools/deck.py inspect decks/demo.tsv
python3 tools/deck.py commands decks/demo.tsv --output demo.commands
python3 -m pip install -r requirements.txt
python3 tools/deck.py upload decks/demo.tsv --port /dev/cu.usbmodem1101
```

端口需使用自己电脑上实际设备路径。导入会整体替换设备卡片并重置复习历史，只有设备返回 `OK COMMIT` 才算成功。导入失败不应显示“成功”；错误/超时后工具尝试 ABORT，设备也有事务超时保护。

## 三键复习

在牌组页，用UP/DOWN选择牌组，OK进入。在问题或答案页，UP/DOWN翻正文页；OK从问题翻到答案，再从答案进入四档评分。评分页UP/DOWN选择Again/Hard/Good/Easy，OK保存。OK长按返回上层；未保存评分不会修改卡片进度。

Again 约一分钟后重新出现；学习步骤为1分钟和10分钟。到期卡优先于新卡。所有到期卡完成后显示完成状态，不会自动跳到下一天。

## 学习日

这是离线学习时钟，不是实际日历日期。设备运行时按分钟计时，关机时间不会自动计入。菜单中选择“下一学习日”并确认推进；也可在导入工具中指定 `--day N` 将学习日期向前推进。禁止回退。

成功评分会立即保存；开机空闲计时约每5分钟保存一次，因此未评分的空闲时间可能在断电时丢失少量分钟。

## 固件与设备验收

按 docs/build.md 编译或下载 GitHub Actions 固件产物。固件是独立 factory 应用，分区布局与 XiaoZhi、消消乐等已有固件可能不同，不能直接沿用其它项目的 app-only 地址。

本项目开发没有替用户刷写设备。真机检查结果见 docs/acceptance.md；编译通过不代表按键、屏幕、断电恢复或USB导入已在设备上通过验收。
