# 来源与许可

截至 2026-10-11，本项目以自主 C11/ESP-IDF 实现移植闪卡学习流程。

| 来源 | 快照 | 使用方式 | 许可 |
|---|---|---|---|
| [AnkiDroid](https://github.com/ankidroid/Anki-Android) | main `1a1dab329812a96b4cbc439e9b5f6e1415c2a76b` | 参考牌组/翻面/四评分体验；未复制 Android、Anki 后端代码或资源 | 上游 GPL-3.0/AGPL-3.0/LGPL-3.0 分模块 |
| [ai-passport-xiaoxiaole](https://github.com/yangguo/ai-passport-xiaoxiaole) | `cursor/xiaoxiaole-v1-impl-19f9` @ `97924dcaa76bd458a3f2a529230b5a4878e79476` | 导入 BSP、初始 SDK/依赖配置，参考项目布局 | MIT |
| [FoloToy/ai-passport](https://github.com/FoloToy/ai-passport) | BSP 继承记录 `33d3d1d93a1125b356b47b6d83a7a60121be801e` | 通过消消乐快照间接复用硬件驱动；修改历史见 components/bsp/IMPORT.md | MIT，保留 BSP LICENSE |
| Noto Sans SC | 字体目录中的 SOURCE/生成记录 | 中文 const 字体 | SIL OFL，保留字体目录许可 |

本项目核心/适配/工具采用仓库 MIT LICENSE。不宣称官方 AnkiDroid 身份；没有 FSRS 或 AnkiWeb 协议兼容保证。后续复制任何上游代码时应单独记录其来源及原许可，不能用本仓库 MIT 替换上游许可。
