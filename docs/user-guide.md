# 使用与兼容范围

## 卡片准备

电脑准备 UTF-8 TSV，第一行为四列：`id`、`deck`、`front`、`back`。每张卡 ID 唯一非零，多个相同 deck 名的卡归为同一牌组。最多 12,000 张、最多 64 个牌组、牌组名最多 47 UTF-8 字节、每面最多 511 UTF-8 字节。中文通常每字 3 字节，假名也是 3 字节。标准 TSV 引用字段可以包含换行。超出长度会报错，不会截断。

从 Anki 导出文字时，优先采用 UTF-8 文本。旧式 Basic APKG 可转换。字段里带 `VocabKanji`、`VocabFurigana`、`VocabDefSC` 的牌组（例如 [anki-jlpt-decks](https://github.com/5mdld/anki-jlpt-decks) 的 `eggrolls-JLPT10k-v3.5.apkg`）会取词条、假名、声调、词性和简体释义，并带上第一条例句；音频和图片丢弃，`汉字[假名]` 写成 `汉字（假名）`。Cloze、复杂模板不转换；`collection.anki21b`（zstd 压缩）已支持，多成员包优先用它。`--map out.json` 把设备上的 u32 ID 对应到 Anki 卡片 ID（JSON 里用字符串），这个文件是本地数据，不要提交进仓库。设备不上网，也不保存 Anki 的 64 位 ID。嵌套子牌组取末级名（如 `1-高频`），同名末级会合并。

```sh
python3 tools/deck.py inspect decks/demo.tsv
python3 tools/deck.py commands decks/demo.tsv --output demo.commands
python3 -m pip install -r requirements.txt
python3 tools/deck.py upload decks/demo.tsv --port /dev/cu.usbmodem1101
```

```sh
python3 tools/deck.py inspect n3.apkg
python3 tools/deck.py upload n3.apkg --port /dev/cu.usbmodem1101
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

AnkiConnect 连不上时，可以只读集合文件。工具把 `collection.anki2`、`collection.anki21` 复制到临时目录（含旁边的 `-wal`、`-shm`）；压缩的 `collection.anki21b` 先解压为临时 SQLite 再只读打开，不复制 `-wal`/`-shm`。副本都不写回原文件。直接传 `.apkg` 文件也能导入（含 `anki21b` 的新包，如 `n3.apkg`）。Anki 进程还在时会警告：副本可能看不到尚未写盘的复习。更稳妥的做法是先退出 Anki。

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

在牌组页，用 UP/DOWN 选择牌组，OK 进入。在问题或答案页，UP/DOWN 翻正文页；OK 从问题翻到答案，再从答案进入四档评分。评分页 UP/DOWN 选择 Again/Hard/Good/Easy，OK 保存。OK 长按返回上层；未保存评分不会修改卡片进度。

牌组页是开机后的第一屏。按住 UP 约 0.5 秒（与 BSP 长按相同，不是 1.5 秒）会尝试回到 Cover Arts 启动器。其它画面的 UP 长按没有这个作用。屏幕上不另写返回提示。启动器缺失、已经在 factory 上运行、或校验失败时，留在本应用。复位或断电仍是回到启动器的通用办法。

学习画面把正文放在标题和底栏之间，一页六行。问题用浅色字，答案用偏冷的字色。评分底栏是一行 `Again Hard [Good] Easy`，方括号标出当前档，颜色随档位变化。没有触摸操作。

Again 约一分钟后重新出现；学习步骤为1分钟和10分钟。到期卡优先于新卡。所有到期卡完成后显示完成状态，不会自动跳到下一天。

## 学习日

这是离线学习时钟，不是实际日历日期。设备运行时按分钟计时，关机时间不会自动计入。菜单中选择“下一学习日”并确认推进；也可在导入工具中指定 `--day N` 将学习日期向前推进。禁止回退。

成功评分会立即保存；开机空闲计时约每5分钟保存一次，因此未评分的空闲时间可能在断电时丢失少量分钟。

## 固件与设备验收

按 [build.md](build.md) 编译，或下载 GitHub Actions 产物。分区表是 FoloToy 的单 factory 布局（`nvs` 24 KiB，`factory` 从 `0x10000` 到 8 MiB 末尾）。同一份 `app.bin` 有两种安装方式：

- **Cover Arts**：在 https://calm.yishan.app/ 把 `app.bin` 装进玩法槽。安装器也可以从合并镜像里抽出应用。不要把 `full.bin` 刷到 `0x0`，那会改掉启动器的引导程序和分区表，并清掉已经装好的玩法。
- **独立整机**：只在准备覆盖这台设备时，把 `full.bin` 刷到 `0x0`。factory 尾部仍放下原来的 12,000 张窗口（正文 `0x460000`，调度 `0xD0000`）。

卡片和复习记录不在独立分区里，也不进 NVS。它们写在当前应用镜像校验通过之后的尾部。Cover Arts 槽的大小是镜像按 64 KiB 对齐后再加 64 KiB 边车；本仓库把镜像长度补到模 65536 余 4096，从而在边车前留下 60 KiB 空白。重装玩法会重写整个槽，包括这段空白。NVS 只有 24 KiB，并且和启动器共用，旧的约 37 KiB PAK1 快照可能放不下。迁不走就保持原样，固件不会擦除 NVS。

槽内的结构上限是 256 张、每槽 4096 字节正文、复习日志 1 个扇区（508 条）。JLPT 这种大约 180 字节一张的牌，大致只能放二十张出头；一面写满 511 字节时更少。`INFO` 的 `cap` 是当前设备的张数上限，`room` 是正文字节上限。上传工具在 `BEGIN` 之前拒绝超过 `cap` 的牌组，也拒绝超过 `room` 字节的 payload。设备在 `BEGIN` 时还会再查一次。独立 factory 尾部的 `cap` 仍是 12000。万张 JLPT 牌组不能装进一个玩法槽。

本项目开发没有替用户刷写设备。真机检查结果见 docs/acceptance.md；编译通过不代表按键、屏幕、断电恢复或 USB 导入已在设备上通过验收。
