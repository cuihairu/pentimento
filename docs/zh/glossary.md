# 术语表

以下条目来自 2026-11-07 源码分层分析，按 [架构](./architecture.md) 中的职责层归纳。英文术语见 [English glossary](../glossary.md)。

| 术语 | 英文 | 定义 | 证据 |
| --- | --- | --- | --- |
| 素材库 | Asset Library | 一个校验后打开、可整体重定位的根目录，拥有 SQLite 目录、不可变对象、工作副本与单进程锁；设计上禁止复制。 | `src/library.h:145` |
| 工作副本 | Working Copy | 位于 `working/`、交给外部编辑器处理的实时文件；缺失或不可读是可恢复状态，不是删除。 | `src/library.cpp:1580` |
| 历史对象 | History Object | `objects/<sha256>` 内容寻址负载，读取时校验哈希，发布后尽力加固。 | `src/library.cpp:918` |
| 版本 | Revision | 内容修订及其时间线记录；恢复会新增版本，不会覆盖既有历史。 | `src/library.h:65` |
| 墓碑 | Tombstone | 应用内确认删除产生的版本和 `assets.deleted=1`；历史与对象仍可浏览、恢复。 | `src/library.cpp:1530` |
| 快照 | Snapshot | 整树锚点，在单个边界冻结素材路径、内容哈希、状态与锚定版本。 | `src/library.cpp:1631` |
| 快照清单 | Snapshot Manifest | 冻结的 `snapshot_assets` 行，状态词表为 present、tombstone、excluded、missing。 | `src/library.h:94` |
| 恢复计划 | Restore Plan | 不写入的目标恢复预演，绑定目标版本和工作副本 `(size, mtime)`；执行时拒绝失效计划。 | `src/library.h:177` |
| 不可变加固 | Immutability Hardening | Linux、Windows、macOS 平台的尽力而为操作，使历史对象只读或不可变。 | `src/platform_immutable.h:17` |
| 内容寻址 | Content Addressing | SHA-256 同时是对象身份与去重键，读取时校验内容与地址一致。 | `src/library.cpp:926` |
| 来源连接 | Source Connection | 针对已挂载本地/SMB/NFS 来源的只读可达性状态机。 | `src/source_connection.h:13` |
| 备份目标探测 | Backup Target Probe | 独立判断备份目的地可写性与剩余空间的只读探测。 | `src/storage.h:43` |
| 重扫预览 | Rescan Preview | 将来源分类为新增、已入库、已排除、不可读的只读预演；永不把远端变化同步为删除。 | `src/importer.h:65` |
| 稳定自动记录 | Stable Auto-Record | 读取后再核对 `(size, mtime)`，避免把撕裂写入记录成版本。 | `src/library.cpp:1112` |
| 定时快照 | Snapshot Schedule | 仅应用运行时生效的 15/30/60 分钟状态机，区分用户/系统暂停，合并错过的周期。 | `src/snapshot_schedule.h:5` |
| 哨兵 | Sentinel | 工作区基线扫描器，检测批量写入、新扩展名和高熵内容，并触发紧急快照。 | `src/sentinel.h:6` |
| 后台队列 | Task Queue | 串行工作线程，支持协作取消、逐项进度以及 join/detach 两种关闭方式。 | `src/taskqueue.h:10` |
| 缩略图缓存 | Thumbnail Cache | 以内容哈希和解码器版本为键的 `thumbnails/` 缓存；损坏条目删除后重建。 | `src/thumbnails.h:7` |
| 受限解码 | Bounded Decode | 基于文件头读取元数据、按预算降采样解码，避免大图耗尽内存。 | `src/preview.h:8` |
| 统一对比变换 | Shared Comparison Transform | 双版画布共用一个比例与图像坐标原点，滑动分界线也随缩放保持坐标。 | `src/compare_canvas.h:7` |
| 筛选事实 | Filter Facts | 来自目录的格式、像素尺寸、标签、评分与逻辑路径，供纯函数筛选器使用。 | `src/filter.h:6` |
| 出处记录 | Provenance | 追加式记录用户输入的来源、作者、许可和证据对象。 | `src/library.h:109` |
| 字段历史 | Field History | 只读回看新旧字段值，并与元数据版本保持对齐。 | `src/library.h:130` |
| 一致性报告 | Consistency Report | 流式审计修订引用，分类为 OK、缺失、损坏、I/O 错误或孤儿对象。 | `src/library.cpp:2047` |
| 备份包 | Backup Package | 经过校验的 `manifest.json`、去重对象，以及最后写入的 `COMPLETE` 标记。 | `src/library.cpp:1860` |
| 能力模型 | Capability Model | 将后端能力拆成 History、SnapshotTree、Restore、Lock、RemoteFetch，再映射到安全 UI 动作。 | `src/backend.h:6` |
| 最近库 | Recent Library | QSettings 中按稳定库 ID 去重的列表，用于重定位后验证同一库。 | `src/recent.h:6` |
| 应用配置 | Application Settings | 用户作用域 QSettings 键，如最近库和 `ui/language`。 | `src/i18n.cpp:10` |
| 用户界面壳层 | UI Shell | 主窗口与对话框，拥有控件、定时器、动作门控和用户确认。 | `src/mainwindow.h:28` |
| 核心库 | Core Library | 可执行程序消费的无 Widgets Qt Core/Gui/Sql 领域与服务组件。 | `CMakeLists.txt:53` |
