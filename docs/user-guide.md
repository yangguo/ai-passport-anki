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

## 从本机 Anki 取牌组

设备不上网，也不看到 AnkiWeb 的用户名、密码或 hkey。先在官方 Anki Desktop 里登录并同步，让牌组落到本机集合。本工具只读这台电脑上的 Anki，再经 USB 推到设备。

Anki 开着时优先用 AnkiConnect（默认 `http://127.0.0.1:8765`，需要本机已安装该插件）。列出牌组，再按名字取出一个或多个。子牌组不会跟着父牌组一起取出，要单独写名字。

```sh
python3 tools/deck.py anki-decks
python3 tools/deck.py anki-pull --deck "JLPT N5" --deck "JLPT N4" \
  --output decks/local.tsv --map decks/local.map.json
python3 tools/deck.py upload decks/local.tsv --port /dev/cu.usbmodem1101
```

`--upload` 可以接在 `anki-pull` 后面，仍然要给 `--port`。映射 JSON 把设备上的 u32 id 对应到 Anki 卡片 id（字符串）。再次 pull 同一份映射时会尽量复用这些设备 id。这个文件和旁边的回写收据都是本地数据，不要提交进仓库。

AnkiConnect 连不上时，可以只读集合文件。工具先把 `collection.anki2` 或 `collection.anki21`（以及旁边的 `-wal`、`-shm`）复制到临时目录，再以只读方式打开副本，不写原文件。`collection.anki21b` 会拒绝。Anki 进程还在时会警告：副本可能看不到尚未写盘的复习。更稳妥的做法是先退出 Anki。

```sh
python3 tools/deck.py anki-decks --collection ~/.local/share/Anki2/User\ 1/collection.anki2
python3 tools/deck.py anki-pull --deck "JLPT N5" \
  --collection ~/.local/share/Anki2/User\ 1/collection.anki2 \
  --output decks/local.tsv --map decks/local.map.json
```

同时给了 `--url` 和 `--collection` 时，先试 AnkiConnect，失败再读副本。只给 `--collection` 时不访问网络。字段规则与 APKG 相同：Basic 的前两个文本字段，或带 `VocabKanji` / `VocabFurigana` / `VocabDefSC` 的 JLPT 牌组。Cloze、其它模板、超过 12,000 张、超长字段都会报错并带上数量，不会静默截断。`--api-key` 只传给本机 AnkiConnect，不是 AnkiWeb 密码。

## 把复习写回本机 Anki

在设备上评完分之后、下一次导入之前，把评分交回本机 Anki。下一次 `COMMIT` 会换掉整组卡片并清掉设备上的复习记录。

```sh
python3 tools/deck.py sync-back --map decks/local.map.json \
  --port /dev/cu.usbmodem1101 --dry-run
python3 tools/deck.py sync-back --map decks/local.map.json \
  --port /dev/cu.usbmodem1101
```

先看 dry-run 打印的计划。确认后再去掉 `--dry-run`。工具从设备分块读出复习记录（`EXPORT` / `XCHUNK` / `XRESUME`，整包 CRC），用映射找到 Anki 卡片 id，再逐张调用 AnkiConnect 的 `answerCards`。设备评分 0..3 对应 Anki 的 Again/Hard/Good/Easy（ease 1..4）。间隔、ease 和到期日由 Anki 自己计算。本工具不改 `cards.ivl`、`factor`、`due`，也不手写 revlog。

作答时间是执行回写时的电脑时钟，不是设备上的逻辑分钟。逻辑分钟只出现在打印里，当作不可靠的提示。完成后在官方 Anki 里同步到 AnkiWeb。本工具不连接 `sync.ankiweb.net`。

再跑一次同一条命令不会把已经记下的评分再答一遍。收据默认写在映射文件旁边（`local.map.json.receipt.json`），也可以用 `--receipt` 另指定。进程如果在 `answerCards` 返回前退出，下次会用 Anki 的 `getReviewsOfCards` 核对同一次作答，对上了就补记收据，不再答一次。AnkiConnect 没有这个接口、而又有一条未完成的收据时，工具会停下来，避免重复作答。

牌组在取出之后又在 Anki 里被改过（卡片 `mod` 更晚，或出现一条不是本工具写的更新复习），这张卡会列成冲突并跳过。dry-run 也会做这项检查，但不会调用 `answerCards`，也不会写收据。设备复习日志写满或追加失败时，评分仍然留在设备调度里，但导出会带上 `dropped`。`dropped` 不为 0 或仍有冲突时，命令以退出码 2 结束。回写只认 AnkiConnect，不走只读集合文件。

## 三键复习

在牌组页，用UP/DOWN选择牌组，OK进入。在问题或答案页，UP/DOWN翻正文页；OK从问题翻到答案，再从答案进入四档评分。评分页UP/DOWN选择Again/Hard/Good/Easy，OK保存。OK长按返回上层；未保存评分不会修改卡片进度。

Again 约一分钟后重新出现；学习步骤为1分钟和10分钟。到期卡优先于新卡。所有到期卡完成后显示完成状态，不会自动跳到下一天。

## 学习日

这是离线学习时钟，不是实际日历日期。设备运行时按分钟计时，关机时间不会自动计入。菜单中选择“下一学习日”并确认推进；也可在导入工具中指定 `--day N` 将学习日期向前推进。禁止回退。

成功评分会立即保存；开机空闲计时约每5分钟保存一次，因此未评分的空闲时间可能在断电时丢失少量分钟。

## 固件与设备验收

按 docs/build.md 编译或下载 GitHub Actions 固件产物。固件是独立 factory 应用，分区布局与 XiaoZhi、消消乐等已有固件可能不同，不能直接沿用其它项目的 app-only 地址。

本项目开发没有替用户刷写设备。真机检查结果见 docs/acceptance.md；编译通过不代表按键、屏幕、断电恢复或USB导入已在设备上通过验收。
