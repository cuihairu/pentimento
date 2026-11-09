# 存储与版本设计

## 源码分层

2026-11-07 分析归纳出七个职责层。层代表运行时职责，而不是目录：头文件与实现都在 `src/` 下；真正隔离无 Widgets 核心代码与桌面外壳的是 CMake target 边界。

### L1 应用入口与桌面外壳

`src/main.cpp` 安装翻译、解析 `--library` 或最近库/默认根目录、构造 `Library` 与 `MainWindow`，并提供截图冒烟入口。`MainWindow` 拥有工具栏、素材/筛选/预览/时间线面板、对话框、用户确认、动作门控和应用定时器；`CompareCanvas` 与 `PreviewCanvas` 是其图像展示控件。VitePress 站点是文档外壳；中英文导航由 `docs/.vitepress/config.mts` 生成。

- 实现：`src/main.cpp:13`、`src/mainwindow.cpp:50`、`src/compare_canvas.cpp:142`、`src/preview_canvas.cpp:83`。
- 配置 schema：命令行参数在 `src/main.cpp:23` 解析；QSettings 与最近库输入在 `src/i18n.cpp:10`、`src/recent.cpp:10`；UI 快照周期种子在 `src/mainwindow.cpp:346`。
- 专属协议文件：无；Qt 编译翻译契约在 `translations/*.ts` 与 `.qm`，CMake 部署在 `CMakeLists.txt:62`。
- 文案与术语：用户动作标签是 `src/mainwindow.cpp:50` 的 `tr()`；稳定功能文案在 `docs/zh/index.md:16`、`docs/index.md:16`、`docs/zh/quickstart.md:23`、`docs/quickstart.md:23`；术语表为 `docs/zh/glossary.md:1` 与 `docs/glossary.md:1`。

### L2 素材库领域与持久化

`Library` 是唯一且不可复制的领域聚合。它校验六类打开状态、写入并迁移 SQLite schema，管理素材、版本、标签、评分/备注、出处、文件夹身份、固定版本、墓碑、快照和恢复收敛。对象与目录同库存放在内容寻址 `objects/`；工作副本在 `working/`；`library.lock` 限制单进程。恢复遵循保护/日志/文件/索引四阶段协议。

- 实现：生命周期/版本核心 `src/library.cpp`；对象存储 `src/library_objects.cpp`；目录元数据 `src/library_meta.cpp`；快照 `src/library_snapshot.cpp`；恢复 `src/library_restore.cpp`；备份与审计 `src/library_backup.cpp`；外部目录索引 `src/library_external.cpp`（2026-11-09 LAY-002 拆分）。
- 配置 schema：库身份与 `schema_version` 持久化在 `meta`由 `Library::openAndInitialize` 统一落库（`src/library.cpp:104`），DDL schema 与向前兼容列迁移同在该函数。
- 专属协议文件：无；恢复阶段契约在源码 `src/library_restore.cpp:100`，备份清单在 `docs/zh/architecture.md:29` 说明。
- 文案与术语：`Library` 用户消息在 `src/library.h:147` 声明可翻译；目录术语见 `docs/zh/architecture.md:3`、`docs/architecture.md:3` 及上方术语表。

### L3 导入、来源与备份服务

该层管理跨文件系统边界的受控内容移动。导入器扫描并生成计划，但只导入用户确认的文件；`SourceConnection` 只读探测已挂载来源，且不会把不可达解释为删除；备份目标探测单独判断目的地是否可写；备份包先校验内容，最后才写 `COMPLETE`。

- 实现：`src/importer.cpp:57`、`src/importer.cpp:91`、`src/importer.cpp:108`、`src/source_connection.cpp:36`、`src/storage.cpp:21`、`src/storage.cpp:64`、`src/library_snapshot.cpp:104`。
- 配置 schema：用户忽略规则由 `src/importer.cpp:22` 解析；来源路径和备份路径是 `src/mainwindow.cpp:1982` 渲染的对话框输入；CMake 翻译目录是 `src/i18n.cpp:23` 的输入。
- 专属协议文件：无；`BackupTargetInfo` 与 `SourceInfo` 是 `src/storage.h:23`、`src/storage.h:48` 的服务契约；JSON 备份契约由 `src/library_backup.cpp:36` 生成。
- 文案与术语：来源与目标状态在 `src/source_connection.cpp:20`、`src/storage.cpp:64` 翻译；重扫/导入文案在 `src/mainwindow.cpp:2145`，公开能力说明在 `docs/zh/architecture.md:45` 与 `docs/architecture.md:45`。

### L4 运行时服务与安全

`TaskQueue` 把耗时工作串行移出 UI 线程，并支持 join 或 detach 关闭。`SnapshotSchedule` 是仅运行时生效的快照调度，带暂停语义。`Sentinel` 将工作区扫描窗口与基线比较。`platform` 尽力加固历史对象；后端能力模型阻止不支持的恢复动作。定时快照与紧急快照复用同一捕获队列，因此写入不会重叠。

- 实现：`src/taskqueue.cpp:68`、`src/snapshot_schedule.h:13`、`src/sentinel.cpp:25`、`src/platform_immutable.cpp:66`、`src/backend.cpp:14`；UI 接线和紧急响应是 `src/mainwindow.cpp:455`、`src/mainwindow.cpp:1420`、`src/mainwindow.cpp:1558`。
- 配置 schema：阈值是 `src/sentinel.h:12` 的 `SentinelConfig` 默认值；快照周期是 `src/mainwindow.cpp:346` 附加在下拉项上的 data；队列关闭策略是 `src/taskqueue.h:42` 的显式构造参数。
- 专属协议文件：无；`Capability`/`BackendInfo` 是 `src/backend.h:11` 的后端契约，`SentinelAlert` 是 `src/sentinel.h:20` 的安全信号。
- 文案与术语：触发原因与状态在 `src/sentinel.cpp:90`、`src/source_connection.cpp:20` 生成；公开安全架构文案是 `docs/zh/architecture.md:23` 与 `docs/architecture.md:23`，阈值文档在 `docs/zh/configuration.md:26`、`docs/configuration.md:26`。

### L5 媒体智能与预览

该层把存储字节转换为诚实的技术事实和受限视觉结果。文件头检查不虚构未知元数据；解码使用像素预算；缩略图在完整解码前预缩放，并以内容和解码器版本为键；对比画布共用一个变换，保证平移、缩放和分界线空间一致。

- 实现：`src/preview.cpp:25`、`src/thumbnails.cpp:15`、`src/preview_canvas.cpp:35`、`src/compare_canvas.cpp:31`；UI 消费是 `src/mainwindow.cpp:1061`、`src/mainwindow.cpp:1250`。
- 配置 schema：无文件 schema；像素预算是 `src/preview.h:45` 的函数默认值，缩略图解码器版本在 `src/thumbnails.cpp:12`，展示尺寸来自构造/UI 默认值。
- 专属协议文件：无；内容嗅探与 TGA 回退行为由 `src/preview.cpp:16`、`src/preview.cpp:48` 表达。
- 文案与术语：降级和尺寸消息是 `src/mainwindow.cpp:1076`、`src/mainwindow.cpp:1122`、`src/compare_canvas.cpp:116` 的 `tr()`；公开预览行为概括在 `docs/zh/quickstart.md:30` 与 `docs/quickstart.md:30`。

### L6 进程、文件系统与平台边界

基础边界把持久化操作显式化：受限 SHA-256 流式哈希、`QSaveFile` 原子写入、SQLite 事务，以及平台相关的只读/不可变操作。它刻意保持薄，并被更高层复用。

- 实现：`src/library_internal.h` 的辅助函数；Linux 不可变操作在 `src/platform_immutable.cpp:27`；挂载来源事实在 `src/storage.cpp:10`。
- 配置 schema：无；编译期 OS 分支和 Qt 路径就是 schema，由 `src/platform_immutable.cpp:7` 选择、`CMakeLists.txt:23` 发现。
- 专属协议文件：无；平台保证在 `src/platform_immutable.h:5` 说明，并在 `docs/zh/architecture.md:27` 镜像。
- 文案与术语：错误文案在操作边界生成，如 `src/library.cpp:66`；不可变库架构文案是 `docs/zh/architecture.md:27` 与 `docs/architecture.md:27`。

### L7 验证层

领域层按域拆为十个套件（恢复/版本/备份/服务/来源/导入/媒体/目录/画布/旅程），共固定 90 个场景；安全测试覆盖哨兵与回滚回路；UI 测试在 offscreen 平台下覆盖快捷键、后台完整性和对话框。

- 实现：`tests/library_{restore,versions,backup,services,sources,import,media,catalog,canvas,journey}_test.cpp`、`tests/security_test.cpp:36`、`tests/mainwindow_test.cpp:41`（2026-11-09 LAY-001 拆分）。
- 配置 schema：目标组成和 offscreen 测试环境由 `CMakeLists.txt` 声明。
- 专属协议文件：无；测试调用公开 API 和 Qt 对象属性，而非外部 fixture 协议。
- 文案与术语：测试名是可执行证据词表；公开声明只限 `docs/zh/index.md:32`、`docs/index.md:32` 中测试实际覆盖的范围。

### 为什么这样拆分

1. target 边界是真实约束，不是装饰：`pentimento_core` 排除 Widgets 并把领域/服务暴露给独立可执行程序（`CMakeLists.txt:53`、`CMakeLists.txt:57`）。存储、来源、快照和安全因此可不实例化主窗口测试。
2. `Library` 必须保持唯一写者，因为目录事务、不可变对象和工作文件替换必须跨恢复阶段一致；持久恢复日志正是为了弥补 SQLite 事务不能覆盖文件写入（`src/library_restore.cpp:100`）。
3. 导入刻意采用计划/确认/执行，而不是自动同步：扫描结果可预览，取消时保留已完成项，重扫只做增量分类，不会把来源变化转换成本地删除（`src/importer.h:10`、`src/importer.cpp:91`、`src/importer.cpp:108`）。
4. 来源可达与备份可写是两个问题。只读挂载来源对导入可达，但必须被备份目标判定拒绝，因此两者状态词表和探测不能合并（`src/source_connection.h:20`、`src/storage.h:43`）。
5. 运行时服务独立存在是因为生命周期不同：串行捕获/完整性队列要排空，而目标探测在死挂载阻塞关闭时可 detach（`src/taskqueue.h:42`、`src/mainwindow.cpp:505`）；快照调度仅运行时生效，明确不是系统服务（`src/snapshot_schedule.h:5`）。
6. 安全拆成预防、发现、响应，因为单一机制无法三者兼得：对象先加固（`src/library_objects.cpp:127`），哨兵发现异常窗口（`src/sentinel.cpp:60`），回滚复用只增恢复以保留感染历史（`src/library_snapshot.cpp:337`）。
7. 媒体处理需要独立的内存控制层，因为同一资产可能是 256 MiB 对象：流式哈希被共享（`src/library.h:33`），预览用受限预算（`src/preview.h:39`），缩略图在完整解码前预缩放（`src/thumbnails.cpp:72`）。
8. 展示与数据获取分离，使损坏或不支持的内容不会被含糊表示：头部事实不可用时保持未知（`src/preview.cpp:99`），对比窗格共用一个变换（`src/compare_canvas.cpp:96`），UI 明确切换到降级消息（`src/mainwindow.cpp:1111`）。
9. 验证是架构层而非附属：核心、安全、UI 套件刻意覆盖不同入口和生命周期，包括后台完整性与 detach 探测关闭（`CMakeLists.txt:72`、`CMakeLists.txt:79`、`CMakeLists.txt:85`）。
## 结构

`pentimento_core` 是 Qt Core/Sql 库，不依赖 Widgets；桌面层调用 Library，Qt Test 覆盖存储与恢复行为。CMake 为唯一构建入口，所有目标采用 ISO C++17、禁编译器扩展。

```text
library/
  catalog.sqlite   assets + revisions
  objects/         SHA-256 命名的不可变内容对象
  working/<uuid>/  用户在外部编辑器中编辑的工作副本
  library.lock     单进程写锁
```

复制导入：读取来源 → 写工作副本 → 插入资源及第一版索引。原文件不变。同内容可共享对象，但不同导入仍是独立素材，不自动合并身份。

记录版本：读取工作文件 → 与最新版本内容哈希比较 → 写入新对象 → 插入版本。相同内容不生成重复版本。历史按数据库单调 id 排序，时间以 UTC 毫秒保存、界面按本地时区显示。

恢复：按素材归属查询版本 → 校验目标哈希 → 保存未记录的当前工作内容 → 事务追加恢复版本 → 原子替换工作文件 → 提交数据库。普通异常回滚索引并尝试恢复工作文件；进程断电不构成跨 SQLite/文件系统的原子事务，需后续日志恢复机制。无历史删除/GC，意外留下的对象允许占空间，不允许误清理。

导出：同样验证版本归属和对象哈希，禁止将目标设为素材库内路径。外部文件由保存对话框提示覆盖；核心接口只负责原子写入。

## 安全防线（防勒索病毒）

用户 2026-10-31 令新增核心需求：本地素材库被勒索软件加密/篡改时，产品要能防住、发现、并快速恢复。只做素材库自身防线，不做杀毒引擎/病毒库。三层防线：防线一不可变版本库、防线二批量修改哨兵、防线三整树回滚，详见下文。

### 防线一：不可变版本库（已实现）

- 对象 append-only（内容寻址，复用），落盘后**置只读**：`src/platform_immutable.{h,cpp}` 抽象平台差异——Linux 优先 `chattr +i`（ioctl FS_IMMUTABLE_FL，需特权），拒绝时回退清除写权限位；Windows `FILE_ATTRIBUTE_READONLY`；macOS `UF_IMMUTABLE`；Windows/macOS 真机验证 TODO。`Library::append` 写对象后即置只读，`Library::hardenObjects` 对既有可写对象加固（幂等）。
- 版本库（`objects/`、`catalog.sqlite`）与工作区（`working/`）分离；恢复路径单向 库→工作区——`restore` 只写 `working/`，绝不改写 `objects/`。
- 已知边界：只读是**尽力而为**（同用户进程可 chmod 恢复写权限，`chattr +i` 才是强保证且需特权）；不冒充防住特权攻击者。工作区被加密不影响 `objects/`；读对象仍哈希校验。

### 防线二：批量修改哨兵（已实现）

- `src/sentinel.{h,cpp}`：增量扫描 `working/`，三个信号（批量写入 / 扩展名批量变更 / 内容熵值异常，Shannon 熵>7.2）任一达阈值即 `SentinelAlert`；阈值可配置（生产默认 25/10/10/7.2），正常小编辑不触发（有阈值测试）。
- 触发即自动紧急快照锚点：`snapshotAll` 在事务内写 `snapshots`/`snapshot_assets`，逐素材锚定**最近一次干净修订（感染前内容）**；当前工作内容若已偏离（被加密/篡改），另落 `紧急快照锚点` 修订留作取证但**不作为恢复锚点**；工作文件被改名缺失时同样锚定最近可读（干净）修订。
- 已知边界：哨兵为**事后发现**（非实时拦截）；「宁可误报、不可漏报」倾向，合法大改可能触发，走用户确认；扩展名信号按「基线未有的新扩展名」计数，避免把合法新格式当勒索。

### 防线三：整树回滚（已实现）

- `Library::rollbackAll(snapshotId)`：逐素材恢复到锚点修订，走既有 `restore` 路径（先保存未记录当前内容 → 追加「恢复历史版本」→ 原子替换工作文件），**不销毁后续历史**（被加密版本仍保留）。已在锚点内容的素材跳过。
- UI 入口：`MainWindow::rollbackToSnapshot`（工具栏「恢复到锚点」），列出 `Library::snapshots` 供选择，回滚后重载素材并重臂哨兵基线。
- 已知边界：回滚目标只能是已记录的历史修订（未记录编辑不可恢复）；回滚前须先关闭外部编辑器。

## 自动记录

UI 定时器检查文件大小和修改时间，连续两次（2 秒间隔）相同后读取；已经处理的同一签名不再全量哈希。不改变磁盘签名的写入可能被漏掉，写入中暂停超过窗口可能被记录。编辑器感知与文件监视器仍是后续工作；稳定读取二次校验（`src/library.cpp`）与后台任务队列（`src/taskqueue.{h,cpp}`，用于捕获/完整性/探测工作线程）已落地。

## 版本与备份

单资源内容历史、整库快照锚点（`snapshotAll`/`snapshots`）、批量时间点恢复（`rollbackAll`，即「恢复到锚点」）、定时快照（默认关闭）、快照排除规则、逐字段元数据历史（P-309，只读回看）与一键备份包导出（`exportBackupPackage`）已落地；删除墓碑与保留策略未落地。对象库必须和 SQLite 一起备份，当前仅支持关闭应用后的冷备。库不可直接跨路径移动（工作路径暂为绝对路径）。

完整职责映射、术语表和编号理由见上方[分层小节](#源码分层)与[术语表](./glossary.md)。
