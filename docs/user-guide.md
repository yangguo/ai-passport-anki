# 使用与兼容范围

## 卡片准备

电脑准备 UTF-8 TSV，第一行为四列：`id`、`deck`、`front`、`back`。每张卡 ID 唯一非零，多个相同 deck 名的卡归为同一牌组。最多 12,000 张、最多 64 个牌组、牌组名最多 47 UTF-8 字节、每面最多 511 UTF-8 字节。中文通常每字 3 字节，假名也是 3 字节。标准 TSV 引用字段可以包含换行。超出长度会报错，不会截断。

从 Anki 导出文字时，优先采用 UTF-8 文本。旧式 Basic APKG 可转换。字段里带 `VocabKanji`、`VocabFurigana`、`VocabDefSC` 的牌组（例如 [anki-jlpt-decks](https://github.com/5mdld/anki-jlpt-decks) 的 `eggrolls-JLPT10k-v3.5.apkg`）会取词条、假名、声调、词性和简体释义，并带上第一条例句；音频和图片丢弃，`汉字[假名]` 写成 `汉字（假名）`。Cloze、复杂模板、`collection.anki21b` 不转换。`--map out.json` 把设备上的 u32 ID 对应到 Anki 卡片 ID（JSON 里用字符串），这个文件是本地数据，不要提交进仓库。设备不上网，也不保存 Anki 的 64 位 ID。

```sh
python3 tools/deck.py inspect decks/demo.tsv
python3 tools/deck.py commands decks/demo.tsv --output demo.commands
python3 -m pip install -r requirements.txt
python3 tools/deck.py upload decks/demo.tsv --port /dev/cu.usbmodem1101
```

端口需使用自己电脑上实际设备路径。导入会整体替换设备卡片并重置复习历史，只有设备返回 `OK COMMIT` 才算成功。数据按最多 480 字节一块上传。中断后再次 upload 会从设备已经写好的位置继续；对不上时工具才 ABORT。COMMIT 可能要比单块更久，默认等待 120 秒。

## 三键复习

在牌组页，用UP/DOWN选择牌组，OK进入。在问题或答案页，UP/DOWN翻正文页；OK从问题翻到答案，再从答案进入四档评分。评分页UP/DOWN选择Again/Hard/Good/Easy，OK保存。OK长按返回上层；未保存评分不会修改卡片进度。

Again 约一分钟后重新出现；学习步骤为1分钟和10分钟。到期卡优先于新卡。所有到期卡完成后显示完成状态，不会自动跳到下一天。

## 学习日

这是离线学习时钟，不是实际日历日期。设备运行时按分钟计时，关机时间不会自动计入。菜单中选择“下一学习日”并确认推进；也可在导入工具中指定 `--day N` 将学习日期向前推进。禁止回退。

成功评分会立即保存；开机空闲计时约每5分钟保存一次，因此未评分的空闲时间可能在断电时丢失少量分钟。

## 固件与设备验收

按 docs/build.md 编译或下载 GitHub Actions 固件产物。固件是独立 factory 应用，分区布局与 XiaoZhi、消消乐等已有固件可能不同，不能直接沿用其它项目的 app-only 地址。

本项目开发没有替用户刷写设备。真机检查结果见 docs/acceptance.md；编译通过不代表按键、屏幕、断电恢复或USB导入已在设备上通过验收。
