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
| ESP32-C3 编译/size/镜像 | PENDING | 本机无 IDF；由 GitHub Actions 构建 |

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
