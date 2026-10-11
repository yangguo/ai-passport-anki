# 验收记录

所有结果必须由本次运行证据更新。设计提交：`b9166fa`（先于业务实现）。编码模型：Luna，high reasoning。

## Host 与构建

| 项目 | 当前状态 | 证据 |
|---|---|---|
| RED 测试基线 | VERIFIED | core/codec 均在 pa_add 接口桩失败 |
| C11 sanitizer tests | PASS | 本次 CMake/CTest ASan+UBSan，core/codec/layout/protocol/import 5/5 |
| Python 导入与上传测试 | PASS | 本次 pytest 20/20；上传传输使用 fake serial，未实机导入 |
| 分页/输入协议 host 检查 | PASS | CTest layout/protocol/import；真实 LVGL 渲染仍需设备检查 |
| 中文字库生成/覆盖 | PASS | Noto固定版本，21,509字形；缺字按manifest拒绝；生成重跑字节一致 |
| ESP32-C3 编译/size/镜像 | PASS | ESP-IDF 5.5.3 Actions；app/full/分区产物下载与内容校验通过 |

## 远端构建与镜像核对

- 构建源码提交：`d5d3311122f80f292cde35752e96c1e5bad1f240`。
- [Firmware SUCCESS](https://github.com/yangguo/ai-passport-anki/actions/runs/38131196721)：编译、size、分区检查、merge-bin、上传全部成功。
- [Host tests SUCCESS](https://github.com/yangguo/ai-passport-anki/actions/runs/38131196776)：ASan/UBSan CTest 5/5 + pytest 20/20。
- 下载后核对 `source-revision.txt` 与源码提交完全一致；应用头为 ESP32-C3 / 8 MiB flash。
- 应用镜像 2,144,528 字节，factory分区容量 8,060,928 字节；完整镜像 2,472,208 字节。
- 完整镜像的 `0x50000` 应用内容和 `0x8000` 分区表内容与独立产物逐字节相同。
- `app.bin` SHA-256：`b31fce4b2f80a162bc9d00ae2eb09b39d5be2d55e4cea41967cdd1cfdd6d1c90`。
- `full.bin` SHA-256：`2b376e2c99738ab76728fa40fb8f08ffa3a0724f253d994aa5604495cabb19f9`。
- 链接期 DRAM：224,234 / 321,296字节，剩余97,062字节；这是链接统计，运行时heap/栈余量仍为NOT RUN。
- 字体生成依赖更新至 fonttools 4.60.2，重新生成字体字节一致；GitHub依赖告警已记录为fixed。

## 真机验收

| 项目 | 状态 |
|---|---|
| 开机、显示、圆角、安全区 | NOT RUN |
| ADC 三键及长短按互斥 | NOT RUN |
| 中英文换行/分页/字体 | NOT RUN |
| 四评分和一分钟重学 | NOT RUN |
| 评分后断电恢复 | NOT RUN |
| 64 张电脑 USB 导入 | NOT RUN |
| 导入中断/错误恢复/原数据保留 | NOT RUN |
| 损坏存档报错、不擦其它 NVS | NOT RUN |
| 最小空闲 heap、任务栈水位 | NOT RUN |
| 30 分钟学习稳定性及按键响应 | NOT RUN |

未执行刷写、擦除或设备修改。固件编译或主机测试通过不能替代真机验收。

## M6 桌面模拟器本地验收（2026-10-11）

- 工作目录：`/Users/vyang/Desktop/spaces/ai-passport-anki`；编码由 Luna high 完成。
- `cmake -S . -B build/host -DANKI_HOST=ON -DANKI_SANITIZERS=ON` 与构建通过，CTest 5/5；`.venv/bin/python -m pytest -q tests`：30 passed。
- 真实子进程验证四档评分到期时间和重启恢复、翻面限制、损坏快照保护、保存失败后会话保留、时间溢出、NUL/内部回车拒绝与超长输入后的恢复。
- 实际运行 `tools/deck.py commands decks/demo.tsv`，8张卡导入模拟器，显示中文牌组“苹果/apple”并完成 Good 评分与存档。
- 这些证据只覆盖桌面进程；未运行 LVGL，未执行真机刷写或设备验收。原固件构建记录仍对应上方的源码提交。
