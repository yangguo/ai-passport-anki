# AnkiWeb 登录与牌组同步：可行性与设计

2026-10-12：容量已是 12,000 张，第 13 节的主机路径已经实现。设备仍然离线，不保存 AnkiWeb 凭据。用法见 [user-guide.md](user-guide.md)。

日期：2026-10-11。第 1–12 节是当时的可行性分析，卡片上限还写着 64 张，协议还没有导出命令。这些数字保留作分析记录，不以它们为当前规格。对照对象是 Anki 后端 `rslib/src/sync`（`ankitects/anki` `main` `d64812b680426c6dd55f9de7004070e560361d4b`，2026-10-09）和 AnkiDroid（`ankidroid/Anki-Android` `main` `1e1db761d8562f317a0a9630b97a916690a81fbc`，2026-10-11）。本仓库约束见 [design.md](design.md)、[status.md](status.md)、[acceptance.md](acceptance.md)。

## 1. 结论

采用主机侧转换，设备保持离线。

1. 用户用官方 Anki / AnkiDroid / AnkiMobile 自己完成 AnkiWeb 同步。
2. 电脑上的 `tools/deck.py` 读取**已经同步到本机的集合**（优先 AnkiConnect，其次在 Anki 关闭后只读 SQLite），筛出最多 64 张纯文本卡，写成现有 TSV，经现有 USB 协议推到设备。
3. 复习结果若要回到 Anki，由电脑导出设备快照，再交给**本机官方 Anki 的评分函数**逐张作答。然后仍由官方客户端去同步 AnkiWeb。
4. 设备不保存 Anki 密码或 hkey，不打开 Wi-Fi，不实现同步协议。

直接让设备或 `deck.py` 登录 AnkiWeb 不在推荐范围内：AnkiWeb 条款只允许批准的客户端访问；ESP32-C3 的 RAM、flash 和没有 RTC 也撑不起这条协议。协议本身没有「只下载一个牌组」的接口，一次同步的对象是整份 SQLite 集合。

## 2. 本机约束

| 项目 | 数值 | 出处 |
|---|---|---|
| MCU | ESP32-C3，400 KiB SRAM，无 PSRAM | 数据手册；[design.md](design.md) 第 4 节 |
| 链接期 DRAM | 已用 224,234 / 可用 321,296，余 97,062 字节 | [acceptance.md](acceptance.md)；运行时 heap 仍为 NOT RUN |
| 无线电 | 蓝牙关闭；注释写明 v1 不用 Wi-Fi。`main/CMakeLists.txt` 未依赖 `esp_wifi` | `sdkconfig.defaults` |
| 屏幕 / 输入 | 240×320，GPIO0 上三个 ADC 按键 | [design.md](design.md) |
| NVS | `0x9000/0x40000`（256 KiB），快照约 37 KiB | `partitions.csv`，[design.md](design.md) 第 11 节 |
| factory | `0x50000/0x7b0000`（8,060,928 字节） | `partitions.csv` |
| 当前 app | 2,144,528 字节；字体位图 1,162,067 字节 | [acceptance.md](acceptance.md)，`assets/fonts/README.md` |
| 静态集合预算 | live 约 37 KiB + staging 约 37 KiB + codec 40 KiB；LVGL 池 24 KiB | [design.md](design.md) 第 13 节 |
| 卡片 | 最多 64 张；`id` 为 u32；牌组名 ≤47 字节；正反面 ≤255 字节 | `tools/deck.py`，`passport_anki` |
| 时钟 | 无 RTC。逻辑分钟只在开机时推进，关机时间不计 | [design.md](design.md) 第 10 节 |
| 调度 | 本文档定义的四档简化间隔，不是 SM-2，也不是 FSRS | [design.md](design.md) 第 7 节 |
| 导入 | `BEGIN/CARD/COMMIT` 整集合替换，并清零设备上的复习历史 | `tools/deck.py` `upload()` |

这 97 KiB 链接余量是**当前不联网固件**的数字。Wi-Fi 和 mbedTLS 还没链进来。ESP-IDF 的 Wi-Fi 缓冲和 TLS 记录缓冲（默认单方向约 16 KiB）要从这个余量和运行时 heap 里出。

## 3. AnkiDroid 怎样登录

AnkiDroid 自己不组 HTTP 包。登录界面把用户名、密码和可选的自定义 endpoint 交给 Anki Rust 后端，后端返回 hkey，应用只把用户名和 hkey 存下来。

调用链：

1. `LoginViewModel.handleLogin` 调用 `Collection.syncLogin(username, password, endpoint)`。
2. 该调用进入 `anki.backend.GeneratedBackend.syncLogin`，也就是 `proto/anki/sync.proto` 的 `SyncLogin`。
3. rslib `sync_login()`（`rslib/src/sync/login.rs`）向 `{endpoint}/sync/hostKey` 发登录请求。endpoint 为空时，客户端用 `https://sync.ankiweb.net/`（`rslib/src/sync/http_client/mod.rs` 的 `HttpSyncClient::new`）。
4. 成功时响应 JSON 为 `{"key":"<hkey>"}`。`LoginViewModel.updateLogin` 只写 `Prefs.username` 和 `Prefs.hkey`。
5. 认证失败（后端把 HTTP 403 映成 `AuthFailed`）时，同一函数把用户名和 hkey 写成空字符串。
6. 之后 `Sync.syncAuth()` 从 `Prefs.hkey` 组装 `SyncAuth`。hkey 为空就视为未登录（`isLoggedIn()`）。
7. 普通同步走 `Collection.syncCollection`；冲突时用户选择 `fullUploadOrDownload`。媒体由 `SyncMediaWorker` 另做，不塞进同一次集合 RPC。
8. 同步中再次认证失败会登出。服务器若返回新 endpoint，写入 `Prefs.currentSyncUri`。自定义服务器和自定义证书也在偏好里，默认仍是 AnkiWeb。
9. AnkiDroid 把网络超时放进 `SyncAuth.io_timeout_secs`，偏好默认 60 秒。rslib 在该字段缺失时用 30 秒。

`Prefs.hkey` 是普通 `stringPref`，最终 `putString` 到应用 SharedPreferences。这段代码没有再做一层应用级加密。密码不写入 `Prefs`。hkey 等价于后续所有同步请求的凭据：拿到它就能下载集合。官方客户端把它放在应用私有存储里，这不表示一块没有应用沙箱的闪存分区也可以照做。

AnkiDroid 源码：

- `AnkiDroid/src/main/java/com/ichi2/anki/account/LoginViewModel.kt`
- `AnkiDroid/src/main/java/com/ichi2/anki/Sync.kt`
- `AnkiDroid/src/main/java/com/ichi2/anki/settings/Prefs.kt`
- `anki-common/src/main/kotlin/com/ichi2/anki/sync/SyncAuth.kt`

`rslib/src/sync/version.rs` 里关于「AnkiDroid 仍走旧 graves」的句子写的是 2022 年底的旧 schema 路径。上面这份 2026-10 的 `Sync.kt` 已经改走后端 `syncCollection` / `fullUploadOrDownload`。

## 4. 线上协议

rslib 同步代码版权头是 GNU AGPL v3 或更新版本。下面只描述行为，供判断可行性。实现时不要把这些文件抄进本仓库。

### 4.1 端点

默认基址 `https://sync.ankiweb.net/`。集合方法拼在 `sync/` 下，媒体方法拼在 `msync/` 下（`rslib/src/sync/collection/protocol.rs`、`rslib/src/sync/media/protocol.rs`）。服务器用 `POST /{method}` 分发（`rslib/src/sync/http_server/routes.rs`）。

| 集合方法 | 作用 |
|---|---|
| `hostKey` | 用户名密码换 hkey |
| `meta` | 比较修改时间、schema 时间、USN；决定要不要同步 |
| `start` | 打开有状态会话，交换删除墓碑（graves） |
| `applyGraves` | 分块提交本机删除 |
| `applyChanges` | 一次提交笔记类型、牌组、牌组配置、标签；本机较新时还带配置和创建时间 |
| `chunk` / `applyChunk` | 按最多 250 条交换 revlog、卡片、笔记 |
| `sanityCheck2` | 比对两边对象数量，失败则下次必须全量同步 |
| `finish` | 提交事务，对齐 USN 和服务器修改时间 |
| `abort` | 丢弃服务器上的未完成会话 |
| `upload` / `download` | 整份 `collection.anki2` 上传或下载 |

媒体方法：`begin`、`mediaChanges`、`uploadChanges`、`downloadFiles`、`mediaSanity`。`begin` 的旧客户端用 GET query `k`/`v`；新客户端用 POST。`msync/begin` 返回的 `sk` 现在直接回填 hkey，不再单独发媒体会话密钥（`rslib/src/sync/media/begin.rs`）。

没有按牌组、笔记类型或「到期队列」过滤的方法。所谓单向牌组下载，只能是把整份集合拿下来再在本地丢弃。

### 4.2 登录与 hkey

`HostKeyRequest` 的线上字段名是短名：`{"u":"<username>","p":"<password>"}`（`login.rs` 的测试锁了这个形状）。成功体是 `{"key":"<hkey>"}`。之后每条请求的 `anki-sync` 头里 `k` 带这把 key，密码不再发送。

HTTP 403 映射为认证失败，500 映射为服务器错误，连接被掐掉映射为网络错误。开源自建服务器把 hkey 派生为 `用户名:密码` 的 SHA1，源码注明「这不是 AnkiWeb 的做法」（`rslib/src/sync/http_server/mod.rs` 的 `derive_hkey`）。AnkiWeb 如何作废 hkey 没有写在这份服务器里。可以确定的是：hkey 放在之后每条请求的 `k` 字段里，AnkiDroid 一旦同步返回认证失败就把本地 hkey 清空。泄漏 hkey 就等于泄漏同步权限。

### 4.3 v11 请求和压缩

当前客户端 `IntoSyncRequest` 使用 `SyncVersion::latest()`，也就是 11。一条 v11 请求：

- 方法：POST。
- 头 `anki-sync`：JSON `{"v":11,"k":"<hkey 或登录时为空>","c":"<客户端版本>","s":"<会话 key>"}`（`rslib/src/sync/request/header_and_stream.rs` 的 `SyncHeader`）。
- 体：业务 JSON 的 zstd 帧。上传/下载时业务体是 SQLite 文件字节，不是 JSON。空请求必须是 `{}`，因为 serde 会把空结构体编成 `null`，协议不接受（`protocol.rs` 的 `EmptyInput`）。
- 响应：zstd 体，另加头 `anki-original-size` 表示解压后长度（`rslib/src/sync/response.rs`）。
- `upload` 的解压后正文是裸字符串 `OK` 或错误文本，不是 JSON。

压缩实现是 `encode_zstd_body()`：`async_compression` 的 `ZstdEncoder::new`，源码没有设置 pledged source size，也没有把 windowLog 调小。zstd 在源大小未知时走大输入参数表。level 3（库默认级别）在大于 256 KiB 那一档的 windowLog 是 21，解码窗口 2 MiB（[zstd `lib/compress/clevels.h`](https://github.com/facebook/zstd/blob/dev/lib/compress/clevels.h) 第一张表）。16 KiB 以下的小输入若能声明源大小，level 3 的 windowLog 才是 14（16 KiB）。当前这段编码器没有声明源大小，所以不能把「登录 JSON 只有几十字节」当成「解码只要几十字节 RAM」。帧头里的窗口以实际压缩库为准；rslib 没有把它降到 ESP32 放得下的大小。

v10 及更早仍被服务器接受：multipart 表单，字段 `c`（是否 gzip）、`k`/`sk`、`s`、`v`、`data`。`data` 用 gzip 而不是 zstd（`rslib/src/sync/request/multipart.rs`）。那是遗留路径。新的 AnkiDroid 不走它。gzip 窗口同样是 32 KiB 量级，但遗留路径只换到 schema 11，而且 AnkiWeb 会拒绝过旧的同步版本。

### 4.4 普通同步和全量上下载

`meta` 请求体是 `{"v":<同步版本>,"cv":"<客户端版本>"}`。服务器返回修改时间 `mod`、schema 时间 `scm`、`usn`、服务器时间 `ts`、可选提示 `msg`、是否继续 `cont`、集合是否为空、媒体 USN。v11 起 `hostNum` 废弃，改地址用 HTTP 308，Location 式的新 endpoint 放在错误上下文里，客户端更新基址再重试 `meta`（`meta.rs` 的 `meta_with_redirect`）。

判定（`SyncMeta::compared_to_remote` 和 `online_sync_status_check`）：

| 条件 | 结果 |
|---|---|
| 两边 `mod` 相同 | 无变更 |
| `scm` 不同 | 全量同步。空集合不能覆盖非空的另一边 |
| 其余 | 普通同步 |
| 服务器 `cont == false` | 中止，并带上服务器消息（例如客户端不支持 v2 调度或新时区） |
| 时钟差的绝对值大于 300 秒 | `ClockIncorrect`，同步直接失败 |
| 集合大于未压缩上限 | 服务器改写 `scm`，迫使走单向全量；客户端在 endpoint 含 `ankiweb` 时也会拒绝过大的上传 |

普通同步在一个 SQLite 事务里做完，失败则回滚并 `abort`（`normal.rs`）：

1. `start`：发送本机上次同步的 USN，以及本机是否较新。服务器返回本机尚未见到的删除。旧客户端还能把 graves 塞在 `start` 里；新客户端改为循环 `applyGraves`。
2. `applyChanges`：笔记类型、牌组、牌组配置、标签一次送完。源码注明这当初被当成廉价操作，大牌组树和笔记类型并没有分块（`changes.rs`）。
3. `chunk` 循环直到 `done`：服务器每块最多 250 条 revlog / 卡片 / 笔记（`CHUNK_SIZE`）。
4. `applyChunk` 把本机变更按同样的块送回去。
5. `sanityCheck2`：比较卡片、笔记、revlog、笔记类型、牌组、牌组配置等计数。graves 计数在 schema 18 之前不可靠，服务器比对时把两边 graves 清零。失败会把本机 schema 标成已修改，下次只能全量同步。
6. `finish`：服务器给出新的修改时间。客户端把 USN 设为服务器 USN 加 1。

会话是有状态的。`s` 必须和服务器 `ServerSyncState.skey` 一致，否则 HTTP 冲突（`http_server/user.rs`）。两台设备同时普通同步会互相打断。

全量下载（`collection/download.rs`）：关闭集合，把解压后的 SQLite **流式写盘**（桌面端用 1 MiB `BufWriter`），做完整性检查，执行 `update col set ls=mod`，再原子改名。注释写明这样是为了让大集合不必整份进内存。全量上传则先把文件读进内存，关闭时降到/升到 schema 18，再整份上传（`collection/upload.rs`）。

体积上限：环境变量 `MAX_SYNC_PAYLOAD_MEGS` 默认 100，单位 MiB，这是压缩后的请求上限；未压缩上限是它的 3 倍，默认 300 MiB（`request/mod.rs`）。自建服务器可以用 `MAX_SYNC_PAYLOAD_MEGS` 放大。AnkiWeb 用默认值。

### 4.5 媒体同步

媒体和集合分开，有自己的 SQLite（记录文件名、mtime、SHA1、USN）。变更按 USN 批量拉取。每个变更是 `(文件名, usn, sha1)`。下载和上传都是 zip：每包最多 25 个文件，目标约 2.5 MiB；单文件上限 100 MiB；文件名长度服务器放宽到 255，客户端目标 120（`rslib/src/sync/media/mod.rs`、`syncer.rs`）。

本设备首版不启动 ES8311，也没有图片控件。媒体同步对当前产品没有落点，而且单是一个 zip 缓冲就超过全部 SRAM。

### 4.6 集合文件长什么样

同步上下载的是 SQLite 文件 `collection.anki2`（服务器按用户目录打开这个名字，见 `http_server/user.rs`）。schema 只有两档：`SchemaVersion::V11` 和 `V18`（`rslib/src/storage/mod.rs`）。v11 同步协议对应 V11 集合；v11 协议（数字碰巧相同）的上传在关闭时写成 V18。V18 把牌组、笔记类型、配置拆成独立表，`graves` 也换过结构（`storage/upgrades/schema18_upgrade.sql`）。

这和 `.apkg` / `.colpkg` 不是同一种字节。包格式由 zip 里的 protobuf `meta` 决定（`rslib/src/import_export/package/meta.rs`）：

| 包版本 | 集合文件名 | schema | 是否 zstd |
|---|---|---|---|
| Legacy1 | `collection.anki2` | V11 | 否 |
| Legacy2 | `collection.anki21` | V11 | 否 |
| Latest | `collection.anki21b` | V18 | 是，整文件 zstd |

`tools/deck.py` 已经拒绝 `collection.anki21b`，只读旧式 Basic 文本。同步下载解出来的是未压缩的 SQLite，不是 `anki21b`。主机工具如果要读新版集合，应读 Anki 配置目录里的 `collection.anki2`，或让官方 Anki 导出，而不是把同步体当成 apkg。

卡片、笔记、revlog 在普通同步里仍按 schema 11 的 JSON 元组传输（`chunks.rs` 的 `CardEntry` / `NoteEntry`）。笔记字段是用 `\x1f` 分隔的 HTML。卡片带 `ctype`、`queue`、`due`、`ivl`、`factor`、`reps`、`lapses`、`left`、`data`。`data` 里有 FSRS 的记忆状态。只改 `ivl`/`factor` 会把 FSRS 状态和间隔拆开。

## 5. 版本和稳定性

`rslib/src/sync/version.rs`：

| 版本 | 时间 | 含义 |
|---|---|---|
| 8 | 2013 | 会话 key。2022 年底只剩约 0.045% 的同步在用 |
| 9 | 2018-01 | 标记客户端支持 v2 调度。协议字段本身没有功能变化 |
| 10 | 2020-03 | 标记 v2 时区。仍是 multipart + gzip，集合按 schema 11 |
| 11 | 2023-01 | 改为头 + zstd，媒体不再单独会话 key，支持 schema 18 上下载，`hostNum` 改为 308 重定向 |

服务器拒绝 `[SYNC_VERSION_MIN, SYNC_VERSION_MAX]` 以外的版本。当前 min=8、max=11。官方文档写明：新客户端可能依赖协议变化，不升级服务器就会同步失败；第三方同步服务器没有测试，也不推荐（<https://docs.ankiweb.net/sync-server.html>）。

对本项目的含义：

- 这不是稳定的公开 API。字段短名、分块、schema 11 形状和「一次送完全部牌组」都是历史包袱，上游可以改。
- 普通同步依赖服务器内存里的会话。嵌入式客户端一旦掉线，必须 `abort` 再来，不能假设可以断点续传。
- 完整性检查失败会强制全量同步。一个写错 USN 的客户端会把用户推进「只能选边覆盖」的状态。
- 时钟必须在 300 秒以内。本设备没有 RTC，开机逻辑日从 0 起算，过不了这道检查。
- Anki 卡片 ID 是毫秒时间戳，i64。2026 年的 ID 约 1.7×10^12，大于 u32 上限 4,294,967,295。现有 `CARD` 命令的 `id` 放不下 Anki 的 card id。

## 6. AnkiWeb 条款和许可

条款页：<https://ankiweb.net/account/terms>。2026-10-11 检索到的正文包括：

- 可以通过浏览器，以及批准客户端里的同步功能访问 AnkiWeb。批准客户端是 Anki、AnkiMobile、AnkiDroid、AnkiUniversal。
- 其他客户端会带来问题，因此 AnkiWeb 目前不允许浏览器扩展或其他第三方客户端访问。请改用 AnkiConnect，在本地通过套接字改集合，从而不影响 AnkiWeb。
- 服务免费提供，运营方可以自行决定暂停或移除访问。

Anki 作者 @dae 在 2022-07-11 回复：服务只提供给上述主客户端，并表示后悔批准过 AnkiUniversal，因为它没有跟上协议和媒体同步：<https://forums.ankiweb.net/t/what-is-the-process-of-getting-new-clients-approved-for-ankiweb-sync/21350>。同一讨论里建议的做法是生成牌组文件，让用户导入官方客户端，再由官方客户端同步。

因此：

- 方案 A（设备登录 AnkiWeb）和「`deck.py` 用用户名密码打 `sync.ankiweb.net`」都落在条款禁止的第三方客户端上。即使用户本人授权，也不能把本项目做成 AnkiWeb 客户端。
- 自建同步服务器（`anki --syncserver` 或 `python -m anki.syncserver`）不受 AnkiWeb 条款约束，因为它不是 AnkiWeb。它默认明文 HTTP，文档要求用户自己解决网络问题，并提醒协议会变。即便如此，设备端仍过不了 RAM 和时钟这两关。主机去实现一套自建服务器客户端，也比读取本机已经同步好的 `collection.anki2` 更脆。
- rslib 同步代码是 AGPL-3.0。AnkiDroid 应用代码是 GPL-3.0，通过后端库使用 AGPL 引擎。本仓库自主代码是 MIT（[sources.md](sources.md)）。不要把 `rslib/src/sync` 或 AnkiDroid 的 Kotlin 复制进来，也不要把 `anki` Python 包当作本仓库的依赖源码树。可选的回写步骤应调用用户本机已经安装的官方 Anki，而不是内嵌一份后端。

## 7. 方案比较

### 方案 A：设备上最小登录，单向下载一个牌组

设备用 Wi-Fi 做 TLS，只实现 `hostKey` + `download`（或普通同步），再把一个牌组收成现有 64 张卡。

做不到，原因叠在一起：

| 障碍 | 估计 |
|---|---|
| 链接余量 | 当前固件剩 97 KiB DRAM，且未链入 Wi-Fi / TLS |
| Wi-Fi + mbedTLS | 初始化后通常要留下数十 KiB heap 才能握手；默认 TLS 记录缓冲 16 KiB×2。这会吃掉上述余量 |
| zstd 窗口 | 当前编码器路径对应的大输入默认窗口是 2 MiB，大于全部 SRAM |
| 集合体积 | 未压缩上限 300 MiB。桌面端靠流式写盘；本机没有这块盘。NVS 只有 256 KiB，还要留给现有快照 |
| flash | factory 约 7.7 MiB，app 已占约 2.1 MiB，字体位图约 1.1 MiB。放下一份真实集合和 TLS 证书后，没有稳定余量 |
| 协议形状 | 最小的「只下载牌组」仍然是整库 SQLite，外加 schema、笔记类型、模板、revlog、FSRS `data` |
| 时钟 | 无 RTC，`meta` 的 300 秒检查会失败 |
| 输入 | 三个按键没有密码输入法。密码若从 USB 送入，电脑已经在旁边，登录就该发生在电脑上 |
| 条款 | 设备成为未批准的 AnkiWeb 客户端 |
| 调度 | 下载到的 `ivl`/`due` 单位（复习卡是天，学习卡是秒级截止）和本设备的逻辑分钟不是同一套。硬套进 `pa_card` 会假装兼容 |

内存上，登录 JSON 本身很小，但解压窗口、TLS 和整库 SQLite 不是小的。把窗口「改小」意味着改 Anki 服务器的压缩参数，本项目做不到。

### 方案 B：主机登录 AnkiWeb，转成 TSV，USB 推送，可选回传

`tools/deck.py` 增加登录和下载，在电脑上过滤，再调用现有 `upload()`。电脑的 RAM 和磁盘够用，zstd 和 SQLite 也不是问题。回传时电脑比设备更适合跑 Anki 调度。

卡住的是条款：用 hkey 访问 `sync.ankiweb.net` 的程序就是第三方客户端，和跑在设备上还是电脑上无关。AnkiConnect 是条款点名的替代品，它连的是本机 Anki，不是 AnkiWeb。

因此 B 的「转换 + USB + 回传」保留，B 的「登录 AnkiWeb」去掉。

### 方案 C：主机读本机集合，设备只走现有 USB（采用）

这是 B 去掉 AnkiWeb 登录之后的做法，也是 A 想要的用户体验（把牌组带到设备上）里真正能落地的部分。

```text
官方 Anki  ←——AnkiWeb 同步（官方客户端，本项目不参与）——→  AnkiWeb
   │
   │  AnkiConnect 127.0.0.1:8765   或   Anki 关闭后只读 collection.anki2
   ▼
tools/deck.py  过滤 Basic 文本、64 张、字体、字节上限
   │  写出 TSV + 主机侧 id 映射（Anki i64 → 设备 u32）
   ▼
现有 USB：INFO / BEGIN / CARD / COMMIT
   ▼
设备离线复习（现有简化调度，NVS 快照）
   │
   │  后续阶段：分块导出快照
   ▼
主机用映射找到 Anki card id，调用本机 Anki 的 answer 路径
   │
   ▼
用户再按一次官方客户端的同步
```

设备侧协议、调度和 NVS 在第一阶段保持不变。AnkiWeb 账号只出现在用户自己的官方客户端里。

自建同步服务器不作为产品路径。它绕开 AnkiWeb 条款，但设备仍然要做完整协议、zstd、SQLite 和真实时钟。主机若已经能运行自建服务器，就一定也能运行官方 Anki，直接读本机集合更短。

## 8. 凭证怎么放

| 秘密 | 放哪里 | 不放哪里 |
|---|---|---|
| AnkiWeb 密码 | 只进官方客户端 | 设备 NVS、串口日志、命令行参数、本仓库配置 |
| hkey | 只留在官方客户端 | 同上。hkey 等于集合的下载权 |
| AnkiConnect | 默认只听 127.0.0.1。沿用 AnkiConnect 自己的 API key / 来源许可 | 不要把 AnkiConnect 暴露到局域网后再让设备去连 |
| 设备快照 | 现有 NVS `pa_anki/snapshot` | 不写入网络凭据字段 |
| id 映射文件 | 用户本机，与 TSV 放在一起，权限与牌组文件相同 | 不提交到 git；映射里含牌组内容对应关系，按用户数据对待 |

USB 协议没有认证。拿着线的人本来就能替换牌组。这条通道只传卡片文本和以后的复习记录，不传密码或 hkey。

若将来有人试验自建服务器客户端，密码用交互输入或文件描述符，避免出现在 `argv` 和 shell 历史里；hkey 只放进程内存；TLS 校验服务器证书。该试验仍然不是本设计的交付物。

## 9. 复习进度怎样回到 Anki

设备评分是 0..3（Again/Hard/Good/Easy）。Anki 的 `Rating` 在写入 revlog 时对应 `ease` / `button_chosen` 的 1..4（`rslib/src/scheduler/answering/mod.rs` 把 `Again/Hard/Good/Easy` 写成 1/2/3/4）。数字能对上，调度状态对不上。

| | 本设备 | Anki |
|---|---|---|
| 时间 | 逻辑分钟，关机不计 | 真实毫秒时间戳；同步要求时钟误差 ≤300 秒 |
| 新卡 / 学习 | `stage` + `step`，步骤 1 分钟和 10 分钟 | `CardType` 0..3，学习队列的 `due` 是秒级截止，`left` 是剩余步骤 |
| 复习间隔 | `interval` 天，上限 3650，ease 1300..3500，公式见设计第 7 节 | SM-2 或 FSRS。FSRS 状态在卡片 `data` 里，不只在 `factor` |
| 身份 | u32，导入时由本工具分配 | 笔记和卡片都是 i64 毫秒 ID |
| 一次导入 | 整集合替换，历史清零 | 普通同步按 USN 合并；全量同步会覆盖另一边 |

回写规则：

1. 保存主机映射 `{device_id, anki_card_id, anki_note_id, deck}`。设备上的 `id` 用 1..n 的 u32，不使用 Anki 的 i64。同一张 Anki 卡再次导出时尽量复用映射里的 device id，便于用户对照；设备上的进度仍会在下次 `COMMIT` 时被清掉，所以回传必须发生在下一次导入之前。
2. 回传的是「这张卡按了哪一档」，加上设备上的逻辑分钟。主机把逻辑分钟相对本次导入时刻，标成一段不可靠的时间提示，供用户看。写进 Anki 的作答时间使用主机在执行回写时的时钟，并在确认界面写明这一点。
3. 调用本机 Anki 的作答入口（AnkiConnect 若提供等价动作，或用户本机 Anki 里的 `answer_card` / 对应的 collection op）。由 Anki 自己写 revlog、`ivl`、`due` 和 FSRS `data`。
4. 禁止为了「对齐」去 UPDATE `cards.ivl`、`factor`、`due` 或手写 revlog。那样下一次官方同步会把损坏的调度上传到 AnkiWeb。
5. 若这张卡在官方 Anki 里的修改时间晚于本次导出，说明用户已经在电脑或手机上复习过。跳过并列出冲突，不要自动再答一次。
6. 设备学习时钟不含关机时间，也不能代表真实日历。回写不能声称间隔与用户在 Anki 里当天复习相同。第一阶段可以只做下载，不做回写；用户的 Anki 进度保持原样，设备进度只存在于设备。

现有协议没有把快照读回来的命令。`INFO` 只返回版本、张数和学习日。回写阶段要增加导出，并且单行上限是 1200 字节（`MAX_LINE_BYTES`），而满快照约 37 KiB，必须分块。这是固件协议版本 2 的内容，不属于本次文档改动。

## 10. 分阶段

规模按要动的模块估计，不按日历。

### 阶段 0（本次）

本文。固件、`deck.py` 行为和测试保持原样。

### 阶段 1：主机选出牌组并走现有 USB

只改 `tools/deck.py`、`tests/test_deck.py` 和用户文档。

- 子命令从 AnkiConnect（`http://127.0.0.1:8765`）或只读打开的 `collection.anki2` 取指定牌组。
- 只接受两个纯文本字段的 Basic。Cloze、多卡片笔记、缺字段、超长、缺字形，沿用现在的 `DeckError`，带牌组名和 Anki 卡片 id，不截断。
- 超过 64 张就失败并打印数量。用户用 `--limit` 之前必须看到将被丢下的张数；默认不静默截断。
- 生成现有四列 TSV，以及旁边的映射 JSON。映射文件不进仓库。
- `upload` 仍打印「将替换设备集合并清空复习历史」，仍以 `OK COMMIT` 为成功。
- SQL 只覆盖打开失败、schema 读不出 `notes`/`cards` 时的明确错误。schema 18 和旧 schema 11 的表位置不同，读失败就停，不猜。
- AnkiConnect 优先：Anki 开着时 SQLite 常被锁。只读 SQLite 路径要求用户先退出 Anki。

验收：

- pytest 覆盖：Basic 两字段、牌组过滤、65 张拒绝、Cloze 拒绝、HTML 变成纯文本、超长字段拒绝、映射里的 device id 在 u32 且 Anki id 以字符串保存以免 JSON 数字精度出问题。
- 不访问 `sync.ankiweb.net`。测试用假的 AnkiConnect HTTP 和临时 SQLite。
- 生成的命令仍能被现有 `upload` 状态机接受。
- 固件二进制不因本阶段改变。

### 阶段 2：设备把快照分块交还主机

固件协议升到版本 2，旧设备继续用版本 1。`INFO` 已有版本检查。

- 新增分块导出，载荷是现有 PAK1 快照字节，每行不超过 1200 字节，带序号和 CRC，主机拼完再校验。
- 导出是只读的，失败不修改 live collection。
- `passport_sim` 和 host 测试覆盖分块、缺块、坏 CRC。
- 真机 USB 导出列入验收，在刷写之前保持 NOT RUN。

验收：模拟器导入 demo、评若干张、导出、主机解析出的 `id/ease/interval/stage` 与评分一致。版本 1 设备收到新命令时返回 `ERR`，工具停下来，不把半份快照当成成功。

### 阶段 3：主机把评分交给本机 Anki

- 读取阶段 2 的快照和阶段 1 的映射。
- 对每张有复习记录的卡，在用户确认后调用本机 Anki 作答。
- 打印将要作答的张数、跳过的冲突、以及「作答时间是回写时刻，不是设备逻辑日」。
- 不调用 `upload`/`download`，不接触 AnkiWeb。用户自己在官方客户端里同步。
- 依赖缺失时退出码非 0，并说明需要本机 Anki / AnkiConnect，而不是自动 `pip install` 一份 AGPL 后端进本仓库。

验收：对一份临时集合，Again/Hard/Good/Easy 各一张，回写后的 revlog 按钮与设备评分一致，FSRS/`data` 由 Anki 自己的函数更新。冲突卡的 `mod` 不变。

## 11. 不做什么

- 不在设备上做 Wi-Fi、TLS、hkey、zstd 或 SQLite。
- 不让 `deck.py` 登录 AnkiWeb，也不缓存 hkey。
- 不把设备间隔写回 Anki 卡片。
- 不同步媒体、模板、Cloze、图片公式。
- 不在设备上实现 FSRS。
- 不把 rslib 或 AnkiDroid 源码拷进本仓库。

## 12. 来源

查阅日期 2026-10-11。协议行为以当次 `main` 源码为准。

| 来源 | 位置 |
|---|---|
| Anki 同步登录、hkey | <https://github.com/ankitects/anki/blob/d64812b680426c6dd55f9de7004070e560361d4b/rslib/src/sync/login.rs> |
| 版本 8–11 | `rslib/src/sync/version.rs`（同一提交） |
| 头、zstd、`anki-original-size` | `rslib/src/sync/request/header_and_stream.rs`、`rslib/src/sync/response.rs`、`rslib/src/sync/request/mod.rs` |
| 旧 multipart + gzip | `rslib/src/sync/request/multipart.rs` |
| 方法名与 URL | `rslib/src/sync/collection/protocol.rs`、`rslib/src/sync/http_server/routes.rs`、`rslib/src/sync/http_client/mod.rs` |
| 普通同步 / 全量 / 时钟 300 秒 | `rslib/src/sync/collection/normal.rs`、`meta.rs`、`status.rs`、`download.rs`、`upload.rs` |
| 块大小 250、graves、sanity | `chunks.rs`、`graves.rs`、`sanity.rs`、`changes.rs`、`finish.rs` |
| 媒体 | `rslib/src/sync/media/mod.rs`、`begin.rs`、`syncer.rs`、`protocol.rs` |
| 后端 RPC | `proto/anki/sync.proto` |
| schema 与 anki21b | `rslib/src/storage/mod.rs`、`rslib/src/import_export/package/meta.rs` |
| 评分 1..4 | `rslib/src/scheduler/answering/mod.rs`、`rslib/src/revlog/mod.rs` |
| zstd 默认窗口 | <https://github.com/facebook/zstd/blob/dev/lib/compress/clevels.h> |
| AnkiDroid 登录与同步 | `LoginViewModel.kt`、`Sync.kt`、`Prefs.kt`、`SyncAuth.kt`（提交 `1e1db761d8562f317a0a9630b97a916690a81fbc`） |
| AnkiWeb 条款 | <https://ankiweb.net/account/terms> |
| 新客户端不获批准 | <https://forums.ankiweb.net/t/what-is-the-process-of-getting-new-clients-approved-for-ankiweb-sync/21350> |
| 自建服务器与协议会变 | <https://docs.ankiweb.net/sync-server.html> |
| 本仓库预算 | [design.md](design.md)、[acceptance.md](acceptance.md)、`partitions.csv`、`tools/deck.py` |

## 13. 已实现的主机路径

设备保持离线。用户在官方 Anki Desktop 登录 AnkiWeb 并同步。`tools/deck.py` 和 `tools/anki_sync.py` 只读本机集合，经现有协议版本 2 的分块 USB 上传，复习后再把档位交给 AnkiConnect 的 `answerCards`。不调用 `sync.ankiweb.net`，不保存 hkey 或密码。

### 取出

```sh
python3 tools/deck.py anki-decks
python3 tools/deck.py anki-pull --deck "牌组名" --output decks/local.tsv --map decks/local.map.json
python3 tools/deck.py upload decks/local.tsv --port /dev/cu.usbmodem1101
```

- `anki-decks` 列出牌组。`anki-pull --deck` 可重复，只取点名的牌组，不含未点名的子牌组。
- 默认 AnkiConnect 是 `http://127.0.0.1:8765`。`--api-key` 放在 JSON 请求体里，不是 AnkiWeb 密码。
- `--collection collection.anki2` 或 `collection.anki21`：先复制到临时目录（含 `-wal`、`-shm`），再以 SQLite URI `mode=ro` 打开副本。检测到 Anki 进程会警告。只给 `--collection` 时不访问网络。两个都给时，AnkiConnect 失败才读副本。
- 字段沿用 Basic 前两栏和 JLPT（`VocabKanji`、`VocabFurigana`、`VocabDefSC`）。schema 11 和 schema 18 共用同一套字段读取。Cloze 和其它模板拒绝。超过 12,000 张失败并打印数量，不截断。
- `--map` 写成 version 1 JSON：`source`、`decks`、`imported_at_ms`、`cards[device_id] = {anki_card_id, anki_note_id, deck}`。Anki id 是字符串。再次 pull 会按 Anki 卡片 id 复用设备 id。旧的扁平映射 `{device_id: anki_id}` 仍能读。

### 设备复习日志

`sched` 分区表不变（`0xD0000`，208 个扇区）。末尾 48 个扇区、每条 8 字节，最多 24,384 次评分。调度池因此是 156 个扇区，仍能在导入时同时放下新旧 epoch 的 120 块。记录是卡片下标、评分 0..3 和逻辑分钟。评分先提交调度块，再追加日志。日志满或追加失败时这次评分仍算成功，超级块里的 `revlog_dropped` 加一。导入提交换 epoch，旧日志不再导出。

导出命令仍属协议版本 2，不改 `INFO` 行：

```text
EXPORT
XCHUNK <offset>
XRESUME
```

镜像 magic `PREV`。CRC 在 OK 行上，覆盖整幅镜像。主机超时后 `XRESUME`，核对 crc、length、epoch、revlog，再从自己已经拼好的偏移继续。导出过程中再评分会使快照失效。`dropped` 不为 0 时主机以退出码 2 结束。

### 写回

```sh
python3 tools/deck.py sync-back --map decks/local.map.json --port /dev/cu.usbmodem1101 --dry-run
python3 tools/deck.py sync-back --map decks/local.map.json --port /dev/cu.usbmodem1101
```

主机只用复习记录里的设备 id 和评分。评分 0..3 变成 Anki ease 1..4（Again/Hard/Good/Easy），一次一张调用 `answerCards`，请求体只有 `cardId` 和 `ease`。设备上的 interval、ease、due 不写入 Anki。作答时间是主机时钟。用户随后在官方 Anki 里同步 AnkiWeb。

幂等：收据默认是映射路径加上 `.receipt.json`，键是 `(epoch, seq)`。已经记下的序号会跳过。调用 `answerCards` 之前先把 pending 行 fsync。进程若在返回前退出，下次用 `getReviewsOfCards` 查找 ease 相同、时间不早于开始前 5 秒、且尚未被别的序号占用的复习；对上了就补记，不再答一次。没有这个接口而又存在 pending 时拒绝重试。dry-run 打印计划并检查冲突，不调用 `answerCards`，不写收据。

冲突：取出之后出现一条不是本工具写的更新复习，或卡片 `mod`（秒）晚于 `imported_at_ms // 1000` 且这张卡还没被本工具答过。冲突只列出并跳过。仍有冲突或 `dropped` 时退出码 2。

阶段 0 到阶段 3 的意图已按上面的命令落地。当时写的「最多 64 张」「协议升到版本 2 才导出」「满快照约 37 KiB」不再是当前规格：上限是 12,000 张，导出加在现有版本 2 上，镜像按调度记录和复习日志现算，不导出旧的 PAK1 快照。
