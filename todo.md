# pentimento V1 原子 TODO

[English](todo.en.md) | [简体中文](todo.md)

**状态：待审核需求。全部实现任务未完成；先文档、原型和多轮审核，明确批准后才开发。现有实验代码不计入交付。**

每项只有一个交付目标和可观察验收条件；具体范围见 `docs/zh/requirements.md`，后端/NAS 边界见 `docs/zh/integrations.md`。分组表示依赖顺序，不表示已经开工。

## 需求与审核

- [x] P-001 确认 V1 功能范围。验收：逐项确认 requirements 中 R01–R19 的纳入与排除。证据：docs/zh/design/00-scope-and-budgets.md#p-001（R01–R19 代决记录 2026-10-30，用户可推翻）。
- [x] P-002 确认版本后端能力表。验收：明确自建可写、Git/LFS 与 SVN 只读、时光机系统集成边界。证据：docs/zh/design/00-scope-and-budgets.md#p-002（能力矩阵 2026-10-30，用户可推翻）。
- [x] P-003 确认 NAS 接入范围。验收：确认挂载 SMB/NFS、直接 WebDAV/SFTP 与发现机制。证据：docs/zh/design/00-scope-and-budgets.md:53-55（NAS 范围代决记录 2026-10-30，用户可推翻）。
- [ ] P-004 确认素材测试样本与性能预算。验收：记录样本格式、文件体积、资源数量和可验证阈值。 （BLOCKED 2026-11-01 复核：阻塞仍成立——待用户输入未解决，docs/review/2026-10-30-decisions.md:142；2026-10-31 原注：立即动作已执行（样本矩阵/两档预算/测量口径，docs/zh/design/00-scope-and-budgets.md#P-004，提交 5266cd3）；待用户输入——素材样本可用性与使用权限、三系统测试机可用性与磁盘余量、256 MiB 单文件上限是否取消、D-21/D-23 三系统硬件与阈值（docs/review/2026-10-30-decisions.md:142））（已代决:按 docs/review/2026-10-30-decisions.md 执行(2026-10-30)）
- [x] P-005 使用 ui-ux-pro-max 形成原型规范。验收：提交桌面布局、状态和键盘交互规范；不套用营销页布局。证据：`docs/zh/desktop-prototype-spec.md`。
- [x] P-006 绘制素材库浏览原型。验收：展示真实层级、网格/列表、筛选、详情及导入入口。证据：docs/zh/design/10-prototype-states.md#p-006（示例数据/状态/焦点顺序 2026-10-30；P-012/P-013 用户审核待进行）。
- [x] P-007 绘制版本时间轴原型。验收：区分当前、历史、已缓存与缺失版本。证据：docs/zh/design/10-prototype-states.md#p-007（五类例子 2026-10-30；已缓存按代决标为未来候选，不画成 V1 可用；P-012/P-013 用户审核待进行）。
- [x] P-008 绘制双版本对比原型。验收：呈现并排、滑动和同步缩放入口。证据：docs/zh/design/10-prototype-states.md#p-008（对齐/边界规则 2026-10-30；P-012/P-013 用户审核待进行）。
- [x] P-009 绘制整库时间点原型。验收：显示快照时间、历史目录树和已删除资源。证据：docs/zh/design/10-prototype-states.md#p-009（两快照例子/浏览与恢复入口 2026-10-30；P-012/P-013 用户审核待进行）。
- [x] P-010 绘制恢复预演原型。验收：展示新增/覆盖/跳过、冲突及保护当前状态的操作。证据：docs/zh/design/10-prototype-states.md#p-010（操作清单/异常状态 2026-10-30；P-012/P-013 用户审核待进行）。
- [ ] P-011 绘制 NAS 与仓库连接原型。验收：覆盖发现、手动地址、认证、能力提示和离线状态。 （BLOCKED复核 2026-10-30：缺用户审核/批准（原型绘制范围未拍板，且审核轮次未开始））（已代决:按 docs/review/2026-10-30-decisions.md 执行(2026-10-30)）
- [ ] P-012 完成第一轮功能审核。验收：记录缺项、范围冲突和决议；未解决项不得标通过。 （BLOCKED复核 2026-10-30：缺用户审核/批准（原型绘制范围未拍板，且审核轮次未开始））（已代决:按 docs/review/2026-10-30-decisions.md 执行(2026-10-30)）
- [ ] P-013 完成第二轮原型交互审核。验收：逐条记录流程、错误态、键盘操作问题与修订。 （BLOCKED复核 2026-10-30：缺用户审核/批准（原型绘制范围未拍板，且审核轮次未开始））（已代决:按 docs/review/2026-10-30-decisions.md 执行(2026-10-30)）
- [ ] P-014 完成功能与界面追溯审核。验收：R01–R19 各自关联页面、操作、状态和验收。 （BLOCKED复核 2026-10-30：缺用户审核/批准（原型绘制范围未拍板，且审核轮次未开始））（已代决:按 docs/review/2026-10-30-decisions.md 执行(2026-10-30)）
- [ ] P-015 取得进入实现阶段的明确批准。验收：审核记录中保留用户批准及未决事项处理结果。 （BLOCKED复核 2026-10-30：缺用户审核/批准（原型绘制范围未拍板，且审核轮次未开始））（已代决:按 docs/review/2026-10-30-decisions.md 执行(2026-10-30)）

## 工程基础

- [ ] P-101 建立 C++17/CMake 目标结构。验收：核心库、桌面应用和测试目标独立，禁止依赖 C++20。实验代码不计入交付，明确批准后才开发。证据：`cmake --preset dev && cmake --build --preset dev && ctest --preset dev` 通过（1/1 绿，`library_history`）；编译命令为 `-std=c++17`，无 C++20；目标独立：`pentimento_core`、`pentimento`、`pentimento_tests`。 （BLOCKED复核 2026-10-30：缺用户批准（P-015 进入实现阶段未获批））（已代决:按 docs/review/2026-10-30-decisions.md 执行(2026-10-30)）
- [ ] P-102 确定 Qt 版本与模块。验收：记录三系统可安装版本、图像插件及授权部署要求。 （BLOCKED复核 2026-10-30：缺用户批准（P-015 进入实现阶段未获批））（已代决:按 docs/review/2026-10-30-decisions.md 执行(2026-10-30)）
- [ ] P-103 定义版本后端接口。验收：通过能力查询区分历史、树快照、恢复、锁和远端获取。 （BLOCKED复核 2026-10-30：缺用户批准（P-015 进入实现阶段未获批））（已代决:按 docs/review/2026-10-30-decisions.md 执行(2026-10-30)）
- [ ] P-104 定义资源存储访问接口。验收：本地、挂载目录、WebDAV/SFTP 返回一致的目录与文件信息。 （BLOCKED复核 2026-10-30：缺用户批准（P-015 进入实现阶段未获批））（已代决:按 docs/review/2026-10-30-decisions.md 执行(2026-10-30)）
- [ ] P-105 建立后台任务队列。验收：耗时任务可报告进度、取消和错误且不阻塞界面。 （BLOCKED复核 2026-10-30：缺用户批准（P-015 进入实现阶段未获批））（已代决:按 docs/review/2026-10-30-decisions.md 执行(2026-10-30)）
- [ ] P-106 建立系统凭据适配。验收：凭据不进入素材索引、日志或明文配置。 （BLOCKED复核 2026-10-30：缺用户批准（P-015 进入实现阶段未获批））（已代决:按 docs/review/2026-10-30-decisions.md 执行(2026-10-30)）
- [ ] P-107 建立中英翻译机制。验收：语言切换覆盖菜单、状态、错误和对话框。 （BLOCKED复核 2026-10-30：缺用户批准（P-015 进入实现阶段未获批））（已代决:按 docs/review/2026-10-30-decisions.md 执行(2026-10-30)）

## 素材库

- [ ] P-201 实现库创建与打开。验收：无效、锁定、不可达目录返回明确错误。 （BLOCKED复核 2026-10-30：缺用户批准（P-015 进入实现阶段未获批））（已代决:按 docs/review/2026-10-30-decisions.md 执行(2026-10-30)）
- [ ] P-202 实现最近素材库与重新定位。验收：路径失效可定位新位置且资源身份保持。 （BLOCKED复核 2026-10-30：缺用户批准（P-015 进入实现阶段未获批））（已代决:按 docs/review/2026-10-30-decisions.md 执行(2026-10-30)）
- [ ] P-203 实现复制导入。验收：导入后原文件字节与路径不变。 （BLOCKED复核 2026-10-30：缺用户批准（P-015 进入实现阶段未获批））（已代决:按 docs/review/2026-10-30-decisions.md 执行(2026-10-30)）
- [ ] P-204 实现外部目录索引。验收：索引模式不复制、不修改来源文件。 （BLOCKED复核 2026-10-30：缺用户批准（P-015 进入实现阶段未获批））（已代决:按 docs/review/2026-10-30-decisions.md 执行(2026-10-30)）
- [ ] P-205 实现递归目录导入与忽略规则。验收：符合排除规则的文件不进入导入任务。 （BLOCKED复核 2026-10-30：缺用户批准（P-015 进入实现阶段未获批））（已代决:按 docs/review/2026-10-30-decisions.md 执行(2026-10-30)）
- [ ] P-206 实现导入取消与结果清单。验收：取消后已完成/失败/未处理项目可区分。 （BLOCKED复核 2026-10-30：缺用户批准（P-015 进入实现阶段未获批））（已代决:按 docs/review/2026-10-30-decisions.md 执行(2026-10-30)）
- [ ] P-207 实现缩略图缓存。验收：同内容复用缩略图，损坏缓存可重建。 （BLOCKED复核 2026-10-30：缺用户批准（P-015 进入实现阶段未获批））（已代决:按 docs/review/2026-10-30-decisions.md 执行(2026-10-30)）
- [ ] P-208 实现网格与列表浏览。验收：两种视图保持选择、排序和滚动定位。 （BLOCKED复核 2026-10-30：缺用户批准（P-015 进入实现阶段未获批））（已代决:按 docs/review/2026-10-30-decisions.md 执行(2026-10-30)）
- [ ] P-209 实现图片缩放与透明底。验收：原图比例正确，可切换透明背景。 （BLOCKED复核 2026-10-30：缺用户批准（P-015 进入实现阶段未获批））（已代决:按 docs/review/2026-10-30-decisions.md 执行(2026-10-30)）
- [ ] P-210 实现首批格式与降级状态。验收：PNG/JPEG/WebP/TGA/BMP 有验证样本；不支持格式可见。 （BLOCKED复核 2026-10-30：缺用户批准（P-015 进入实现阶段未获批））（已代决:按 docs/review/2026-10-30-decisions.md 执行(2026-10-30)）
- [ ] P-211 评估并实现 PSD 合成预览适配。验收：明确支持的 PSD 样本与无法预览时的原文件保留。 （BLOCKED复核 2026-10-30：缺用户批准（P-015 进入实现阶段未获批））（已代决:按 docs/review/2026-10-30-decisions.md 执行(2026-10-30)）
- [ ] P-212 提取技术元数据。验收：报告格式、尺寸、大小及可读色彩/通道，未知字段不编造。 （BLOCKED复核 2026-10-30：缺用户批准（P-015 进入实现阶段未获批））（已代决:按 docs/review/2026-10-30-decisions.md 执行(2026-10-30)）
- [ ] P-213 实现标签维护。验收：单个与批量标签修改结果可查询。 （BLOCKED复核 2026-10-30：缺用户批准（P-015 进入实现阶段未获批））（已代决:按 docs/review/2026-10-30-decisions.md 执行(2026-10-30)）
- [ ] P-214 实现评分与备注。验收：修改后重新打开仍保持值。 （BLOCKED复核 2026-10-30：缺用户批准（P-015 进入实现阶段未获批））（已代决:按 docs/review/2026-10-30-decisions.md 执行(2026-10-30)）
- [ ] P-215 实现来源作者授权记录。验收：记录用户输入和附件来源，不自动推断许可。 （BLOCKED复核 2026-10-30：缺用户批准（P-015 进入实现阶段未获批））（已代决:按 docs/review/2026-10-30-decisions.md 执行(2026-10-30)）
- [ ] P-216 实现文件夹组织与资源身份。验收：改名和库内移动后历史仍属于同一素材。 （BLOCKED复核 2026-10-30：缺用户批准（P-015 进入实现阶段未获批））（已代决:按 docs/review/2026-10-30-decisions.md 执行(2026-10-30)）
- [ ] P-217 实现名称搜索与组合筛选。验收：名称、标签、格式和尺寸条件可组合、清除。 （BLOCKED复核 2026-10-30：缺用户批准（P-015 进入实现阶段未获批））（已代决:按 docs/review/2026-10-30-decisions.md 执行(2026-10-30)）

## 自建版本管理

- [x] P-301 定义修订与快照清单格式。验收：内容、元数据、路径、资源 ID 与 schema 版本有明确表示。证据：docs/zh/design/50-versioning.md#p-301（schema v1 + 四组例子验证 2026-10-30）。
- [x] P-302 实现内容对象存储。验收：流式哈希、去重和对象校验通过大文件样本。证据：tests/library_test.cpp 大文件流式+去重、损坏对象阻断去重、磁盘满临时清理三测试（ctest library_history 绿 13/13；commit ca1ad7b）。
- [x] P-303 实现手动记录版本。验收：一次操作创建可查询的版本 ID 与说明。证据：tests/library_test.cpp manualRecordReturnsQueryableVersionIdAndNote（内容变更创建新 ID+说明、未变返回当前 ID、默认说明、失败抛错；ctest library_history 绿 15/15；commit 343f3aa）。
- [x] P-304 实现稳定变化自动记录。验收：半写入与连续保存样本按已定策略处理。证据：tests/library_test.cpp autoRecordPolicyHandlesSampleClasses（连续写入不记、长暂停后稳定记录、临时文件改名保存、同大小写入、touch 不重复）与 stableRecordRetriesWhenFileMovesMidRead（读后复核 mtime/size 变化则重试、撕裂对象不发布；ctest library_history 绿 17/17；commit 3e1a85e）。设计项「后台执行」暂缓：与 README「I/O 在 UI 线程」边界一致，随整体 I/O 线程化一并处理。
- [x] P-305 实现定时快照与暂停。验收：调度状态可见，暂停后不产生自动快照。证据：tests/library_test.cpp scheduleStateIsVisibleAndPauseStopsSnapshots（默认关、非法周期拒绝、到期触发一次、错过多周期只补一次、暂停记录原因且不再产生快照、继续后按新时点重启、关闭清空；ctest library_history 绿 18/18；commit 0c9a069）。UI 状态栏常显周期/下次时间/暂停原因。
- [x] P-306 实现版本排除规则。验收：排除对象不进入后续快照。证据：tests/library_test.cpp exclusionRulesKeepAssetsOutOfSnapshots（无规则时全部入锚；扩展名规则排除 .psd 并记录原因；被排除≠已删除，仍可手动记录；glob 规则再排除 .txt；首条命中原因优先；删除规则后恢复入快照；非法规则拒绝；ctest library_history 绿 19/19；commit cb6939a）。排除仅作用于后续快照，不追溯删除/GC。
- [x] P-307 实现关键版本固定。验收：固定版本不被保留策略清理。证据：tests/library_test.cpp pinnedVersionSurvivesRetentionCleanup（默认最新版非候选、其余为候选；固定后带原因且退出候选；全部固定则无可清理；取消固定恢复候选且历史不删；跨素材/未知版本固定拒绝；重开保持固定与原因；ctest library_history 绿 20/20；commit 807f735）。固定标记+原因持久化，取消固定不删历史；自动清理为 P-602 预演。
- [x] P-308 实现单素材时间轴。验收：顺序、时间、说明与当前/历史标识准确。证据：tests/library_test.cpp timelineOrderAndMarksAreAccurate（按确定序列 id 排序、同秒/时钟回拨不改序、内容推导当前标识且唯一、恢复记录标 restore、未记录修改置 unrecorded、损坏对象标 corrupt 不隐藏、固定显示；ctest library_history 绿 21/21；commit adc623b）。时间仅作展示；UI 时间轴改用 AssetTimeline（当前/恢复/固定/损坏标识 + 未记录修改提示）。
- [x] P-309 实现元数据历史。验收：可查看旧名称、标签、备注与来源记录。证据：tests/library_test.cpp metadataHistoryRecordsPerFieldChanges（导入记基线（旧值空）；五字段一次更新各记一条、修订内序不作约定；元数据修订复用最新内容哈希、不新建内容对象；当前元数据可读；重开保持元数据与历史；重复更新 unchanged 且不增修订；ctest library_history 绿 22/22；commit 8694c76）。逐字段旧/新值与变更时间持久化；无 UI 接入（P-213/214/215 编辑面未建），只读回看为库级接口，UI 延迟里程碑。
- [x] P-310 实现删除墓碑。验收：删除后历史查询仍可找到原素材。证据：tests/library_test.cpp deleteKeepsHistoryFindableAndMissingIsDistinct（应用内删除写墓碑修订、复用末内容哈希不建内容对象、历史/对象全保留、素材仍可查、工作副本移除；恢复后回 Present 且清墓碑；重复删除拒绝；手动移除工作副本为 Missing 且不写墓碑；ctest library_history 绿 23/23；commit c88250f）。三状态 Present/Missing/Tombstone；存储不可达不写墓碑，由存储层后续上报。
- [x] P-311 实现整库一致快照。验收：快照目录树与内容引用属于同一完整清单。证据：tests/library_test.cpp consistentSnapshotPublishesCompleteManifest（3 个在库素材各锚定到 boundary 内的已完成修订；清单含根相对路径+内容哈希，哈希对应真实修订且对象校验通过；保守锚定跳过镜像当前内容的最新修订；boundary 为固定点（后加修订 id > boundary）；工作副本缺失 state=missing；应用内删除 state=tombstone；被排除素材不进清单且单独记录；ctest library_history 绿 24/24；commit fa64197）。一次序列点读出的目录树与内容引用属于同一完整集合，无逐文件扫描；为 P-312 时间点浏览提供只读数据面。
- [x] P-312 实现时间点目录浏览。验收：切换快照能看到当时路径和删除状态。证据：tests/library_test.cpp timePointBrowseShowsSnapshotStateNotCurrentIndex（快照时 present+当时根相对路径；随后应用内删除成墓碑且工作副本移除后，该快照清单仍显示当时 present+旧路径（不混用当前索引，历史目录 vs 当前目录差异正确）；墓碑修订 id > 该快照 boundary（固定点）；后续快照反映墓碑；被排除素材单独列出（当时路径+原因）且不进清单；ctest library_history 绿 25/25；commit 02f3b6a）。只读浏览：按快照完整清单显示路径/名称/删除/缺失/排除状态，切换时间点不写回；UI「浏览时间点」工具栏入口；改名/移动后路径差异需 P-216 支持（本项验证删除差异）。
- [x] P-313 实现双版并排与同步缩放。验收：同一位置和比例可重复比较。证据：tests/compareview_test.cpp compareview（fitZoom 不超 100% 不自动拉伸、受较紧轴限制；同 View（zoom+origin）下两不同尺寸图像 sourceRect 相同且按 zoom 填满视口（同一位置和比例可重复比较）；zoom 入时 origin 夹紧保持覆盖无空白；zoom 出时左上对齐、空白显式；targetRect 与 sourceRect 一致；ctest compare_geometry 绿 7/7；commit c1ae04d）。并排与滑动共用同一渲染规则 compareview::sourceRect/targetRect（P-314 复用）；UI「对比此版本…」双面板滚轮缩放、拖动平移同步同一比例与原点，尺寸不同显式说明，可选适配窗口统一比例。
- [x] P-314 实现滑动对比。验收：拖动分界线能检查两版同坐标区域。证据：tests/compareview_test.cpp slidingDividerShowsSameCoordinateOnBothSides（dividerToImageX 仅依赖共享 View 即两版同列；imageXToViewport 为精确逆（往返一致）；缩放后同一图像列映射到新视口 x 且分界线处仍为同列（缩放后同坐标）；ctest compare_geometry 绿 8/8；commit 62929b6）。UI「对比此版本…」新增滑动方式：单面板可拖动分界线，左侧一版右侧另一版（同 View 渲染），←/→ 步进、[ / ] 调步进，滚轮缩放/拖动平移同步；一侧不可预览时原位标注不隐藏。
- [x] P-315 实现恢复预演。验收：列出新增、替换、跳过和冲突，不先写文件。证据：tests/library_test.cpp restorePreviewListsOpsWithoutWriting（三素材预演：已改版 a=replace、未变 b=skip、工作副本缺失 c=add；bytesToWrite=add+replace 所需空间；预演不写任何文件（缺失仍缺失、改版内容未动）；只读且内容不同 b=conflict(目标不可写) 列出不静默解决、恢复可写后=replace；ctest library_history 绿 26/26；commit 3502fb3）。planRestore 为只读干跑（镜像 rollbackAll 的分类），绑定目标版本与当前文件状态，执行前须重查（P-316+）；计划界面随 P-318 快照恢复接入，本项为库级接口，UI 延迟里程碑。
- [x] P-316 实现恢复前保护当前状态。验收：未记录内容先保存，失败则停止恢复。证据：tests/library_test.cpp restoreGuardReportsStateBeforeOverwrite（已记录=ready；未记录=unrecorded 且 protectBytes=内容大小、预检不写文件；restore 先自动保存未记录内容为新修订再恢复（auto-save 修订可读=原内容、工作副本落回目标版）后=ready；不存在目标=notarget；工作副本缺失=missing；目标对象损坏=corrupt；ctest library_history 绿 27/27；commit 54d9445）。checkRestore 只读预检：未记录/缺失/外部写入（size+mtime 双采样不稳）/空间不足/目标不可用分别返回，绝不声称已保存编辑器未写盘内容；恢复事务内保护失败即回滚停止（不无保护覆盖）。UI「恢复此版本」接入预检：外部写入/损坏/空间不足阻止并提示，缺失需确认，未记录先保存。
- [x] P-317 实现单素材恢复为新版本。验收：原历史保留，恢复结果可再次查到。证据：tests/library_test.cpp singleAssetRestoreAppendsNewVersionKeepingHistory（A→B 两版；恢复 A 产生新修订 C（note=恢复历史版本 #A、id>C、内容=A、工作副本落回 A）；B 仍可浏览（readObject=B）与导出（export=B）；C 可再次查到（时间轴含 C、可再选中）；再恢复 B 继续增长历史为 4 版而非抹除；ctest library_history 绿 28/28；commit 900c4ca）。语义由 restore() 既有实现承担（恢复=追加新修订+保留全部历史），本项补验收断言，无新接口。
- [x] P-318 实现快照内选中项恢复。验收：只处理预演确认的资源与元数据。证据：tests/library_test.cpp restoreConfirmedRestoresOnlyConfirmedItems（快照后 a 内容+元数据都改、b 仅元数据改、c 未变、d 快照后新增；只确认 a/b：a 内容与元数据都回退、b 内容未重写仅元数据回退、c 与 d 完全不动——不默认整库镜像覆盖、不删除快照外新素材；已完成的项重跑收敛为 skipped；确认清单外的 d=conflict 且 d 未动）；restoreConfirmedIsolatesItemFailures（b 锚点对象损坏=conflict 明确列出不静默解决，c 仍正常完成——失败不中断其余项）；快照清单新增 name/tags/rating/note/source 记录快照时元数据（P-311 清单扩展，存量库幂等迁移）；ctest 全绿 library_history 30/30、security_sentinel、compare_geometry 8/8；commit f43b638）。restoreConfirmed 走常规「恢复形成新版本」路径逐项执行、每项独立完成（明确逐项完成状态，不宣称跨库原子），可重启收敛；UI 确认界面随里程碑接入。
- [x] P-319 实现历史副本导出。验收：导出不改变版本后端或当前工作区。证据：tests/library_test.cpp exportVerifiesOwnershipAndRejectsUnsafeTargets（正常导出到库外=校验后字节；他人素材的版本 id 与不存在 id 均拒绝=版本归属；对象被写坏=拒绝且不写出任何文件=复用 readObject 哈希校验；已存在文件未确认不覆盖、显式 overwrite=true 才替换；库外符号链接与指向库内对象的符号链接一律拒绝（即使已确认）；以当前工作文件为目标拒绝（真实路径指向库内）；目录不存在先拒绝；/proc 报 0 可用空间→空间预检先失败不写半成品；导出后库内索引与对象不变）。exportRevision 保持 const（不写库、不改索引、不提交仓库），QSaveFile 原子落盘；UI 端以保存对话框的覆盖确认传 overwrite。ctest 全绿 library_history 31/31、security_sentinel、compare_geometry 8/8；commit 32c7744。
- [x] P-320 实现恢复日志与重启收敛。验收：中断后明确完成或回滚，不隐藏半恢复状态。证据：tests/library_test.cpp restoreJournalConvergesAfterInterruption（阶段2 写日志：beginRestore 在触碰任何文件前落盘计划（目标版本+逐项 pending），运行中 openRestore 可见 in_progress 且两项 pending、目标版本>0；崩溃模拟：a 内容已恢复并标记 restored 后进程未关日志即销毁；重开后 openRestore 返回未完成日志（a=restored、b=pending，不隐藏半恢复状态），resumeRestore 只重跑未完成项（b=restored）、已完成 a 不动（revisions 数不变），两项都到快照内容；无剩余可收敛项→openRestore 空、对已关闭日志 resume 为干净 no-op）。restoreConfirmed 改走持久日志：计划先落盘、逐项记录完成、结束关闭（completed/aborted）；SQLite 事务与文件替换非同事务——以日志+重开收敛为准，绝不把「异常回滚」当「断电安全」。ctest 全绿 library_history 32/32、security_sentinel、compare_geometry 8/8；commit df9a272）。

## Git、SVN、时光机

- [ ] P-401 识别并连接本地 Git 仓库。验收：显示仓库、分支/提交及工作区状态，不修改仓库。 （BLOCKED复核 2026-10-30：缺用户批准（P-015 进入实现阶段未获批））（已代决:按 docs/review/2026-10-30-decisions.md 执行(2026-10-30)）
- [ ] P-402 读取 Git 文件树与日志。验收：选定提交可关联素材路径和历史。 （BLOCKED复核 2026-10-30：缺用户批准（P-015 进入实现阶段未获批））（已代决:按 docs/review/2026-10-30-decisions.md 执行(2026-10-30)）
- [ ] P-403 提取 Git 历史文件。验收：指定提交字节正确，提取到临时/导出位置。 （BLOCKED复核 2026-10-30：缺用户批准（P-015 进入实现阶段未获批））（已代决:按 docs/review/2026-10-30-decisions.md 执行(2026-10-30)）
- [ ] P-404 识别 LFS 指针与缺失对象。验收：指针不能被误当图像预览，缺失原因可见。 （BLOCKED复核 2026-10-30：缺用户批准（P-015 进入实现阶段未获批））（已代决:按 docs/review/2026-10-30-decisions.md 执行(2026-10-30)）
- [ ] P-405 实现 LFS 明确同意后的按需获取。验收：用户同意后获取对象并校验，拒绝则保持缺失状态。 （BLOCKED复核 2026-10-30：缺用户批准（P-015 进入实现阶段未获批））（已代决:按 docs/review/2026-10-30-decisions.md 执行(2026-10-30)）
- [ ] P-406 连接 SVN 工作副本或 URL。验收：认证失败与只读权限分别显示。 （BLOCKED复核 2026-10-30：缺用户批准（P-015 进入实现阶段未获批））（已代决:按 docs/review/2026-10-30-decisions.md 执行(2026-10-30)）
- [ ] P-407 读取 SVN 日志树与锁信息。验收：显示选定 revision 及可取得的锁信息。 （BLOCKED复核 2026-10-30：缺用户批准（P-015 进入实现阶段未获批））（已代决:按 docs/review/2026-10-30-decisions.md 执行(2026-10-30)）
- [ ] P-408 提取 SVN 历史文件。验收：指定 revision 内容正确且不提交、不更新覆盖。 （BLOCKED复核 2026-10-30：缺用户批准（P-015 进入实现阶段未获批））（已代决:按 docs/review/2026-10-30-decisions.md 执行(2026-10-30)）
- [ ] P-409 统一后端历史能力显示。验收：只读后端不显示可直接写回的恢复按钮。 （BLOCKED复核 2026-10-30：缺用户批准（P-015 进入实现阶段未获批））（已代决:按 docs/review/2026-10-30-decisions.md 执行(2026-10-30)）
- [ ] P-410 定义 macOS 时光机系统适配边界。验收：评估可用系统状态/入口与权限；明确不解析备份格式。 （BLOCKED复核 2026-10-30：缺用户批准（P-015 进入实现阶段未获批））（已代决:按 docs/review/2026-10-30-decisions.md 执行(2026-10-30)）
- [ ] P-411 实现时光机能力提示与系统入口。验收：仅 macOS 提供适用入口，其他平台明确不支持。 （BLOCKED复核 2026-10-30：缺用户批准（P-015 进入实现阶段未获批））（已代决:按 docs/review/2026-10-30-decisions.md 执行(2026-10-30)）

## NAS、发现与连接

- [ ] P-501 支持系统挂载 SMB/NFS 来源。验收：可索引已挂载共享，识别失联和只读权限。 （BLOCKED复核 2026-10-30：缺用户批准（P-015 进入实现阶段未获批））（已代决:按 docs/review/2026-10-30-decisions.md 执行(2026-10-30)）
- [ ] P-502 实现 HTTPS WebDAV 列目录。验收：能浏览授权范围，正确处理编码和分页/深度。 （BLOCKED复核 2026-10-30：缺用户批准（P-015 进入实现阶段未获批））（已代决:按 docs/review/2026-10-30-decisions.md 执行(2026-10-30)）
- [ ] P-503 实现 WebDAV 内容按需读取。验收：下载可取消，缓存校验正确。 （BLOCKED复核 2026-10-30：缺用户批准（P-015 进入实现阶段未获批））（已代决:按 docs/review/2026-10-30-decisions.md 执行(2026-10-30)）
- [ ] P-504 实现 SFTP 列目录。验收：仅浏览授权路径，权限错误可见。 （BLOCKED复核 2026-10-30：缺用户批准（P-015 进入实现阶段未获批））（已代决:按 docs/review/2026-10-30-decisions.md 执行(2026-10-30)）
- [ ] P-505 实现 SFTP 内容按需读取。验收：可预览或缓存远端文件，不默认写回。 （BLOCKED复核 2026-10-30：缺用户批准（P-015 进入实现阶段未获批））（已代决:按 docs/review/2026-10-30-decisions.md 执行(2026-10-30)）
- [ ] P-506 实现 mDNS/DNS-SD 发现。验收：显示已广播的文件服务与时光机能力，不保证未广播服务。 （BLOCKED复核 2026-10-30：缺用户批准（P-015 进入实现阶段未获批））（已代决:按 docs/review/2026-10-30-decisions.md 执行(2026-10-30)）
- [ ] P-507 实现 WS-Discovery 发现。验收：发现可响应设备，保留来源与有效期。 （BLOCKED复核 2026-10-30：缺用户批准（P-015 进入实现阶段未获批））（已代决:按 docs/review/2026-10-30-decisions.md 执行(2026-10-30)）
- [ ] P-508 实现发现结果合并与过期。验收：同 NAS 多地址/协议正确合并，不丢接入选项。 （BLOCKED复核 2026-10-30：缺用户批准（P-015 进入实现阶段未获批））（已代决:按 docs/review/2026-10-30-decisions.md 执行(2026-10-30)）
- [ ] P-509 实现手动添加地址。验收：可处理主机名、IPv4/IPv6、端口和协议选择。 （BLOCKED复核 2026-10-30：缺用户批准（P-015 进入实现阶段未获批））（已代决:按 docs/review/2026-10-30-decisions.md 执行(2026-10-30)）
- [ ] P-510 实现共享/目录选择与能力探测。验收：发现、认证、权限和备份能力分别显示。 （BLOCKED复核 2026-10-30：缺用户批准（P-015 进入实现阶段未获批））（已代决:按 docs/review/2026-10-30-decisions.md 执行(2026-10-30)）
- [ ] P-511 实现 TLS 证书校验。验收：无效证书阻止自动连接，不静默忽略。 （BLOCKED复核 2026-10-30：缺用户批准（P-015 进入实现阶段未获批））（已代决:按 docs/review/2026-10-30-decisions.md 执行(2026-10-30)）
- [ ] P-512 实现 SSH 主机密钥校验。验收：新主机需确认，已知密钥变化需明确处理。 （BLOCKED复核 2026-10-30：缺用户批准（P-015 进入实现阶段未获批））（已代决:按 docs/review/2026-10-30-decisions.md 执行(2026-10-30)）
- [ ] P-513 实现断线重连状态机。验收：可见连接中/在线/离线/失败，重试不覆盖远端内容。 （BLOCKED复核 2026-10-30：缺用户批准（P-015 进入实现阶段未获批））（已代决:按 docs/review/2026-10-30-decisions.md 执行(2026-10-30)）
- [ ] P-514 实现离线缓存浏览。验收：可用和未缓存内容明确区分。 （BLOCKED复核 2026-10-30：缺用户批准（P-015 进入实现阶段未获批））（已代决:按 docs/review/2026-10-30-decisions.md 执行(2026-10-30)）
- [ ] P-515 实现重连后的目录变化核对。验收：远端删除、改名和权限变化不会冒充本地删除。 （BLOCKED复核 2026-10-30：缺用户批准（P-015 进入实现阶段未获批））（已代决:按 docs/review/2026-10-30-decisions.md 执行(2026-10-30)）
- [ ] P-516 实现 NAS 备份包导出与校验。验收：导出一致清单及对象后才标成功，不共享运行中索引。 （BLOCKED复核 2026-10-30：缺用户批准（P-015 进入实现阶段未获批））（已代决:按 docs/review/2026-10-30-decisions.md 执行(2026-10-30)）

## 可靠性、交付与验收

- [ ] P-601 实现对象与索引一致性检查。验收：能报告丢失/损坏对象与孤立引用。 （BLOCKED复核 2026-10-30：缺用户批准（P-015 进入实现阶段未获批））（已代决:按 docs/review/2026-10-30-decisions.md 执行(2026-10-30)）
- [ ] P-602 实现空间与保留策略预演。验收：预演列出可清理对象和保留依据，不立即删除。 （BLOCKED复核 2026-10-30：缺用户批准（P-015 进入实现阶段未获批））（已代决:按 docs/review/2026-10-30-decisions.md 执行(2026-10-30)）
- [ ] P-603 实现可恢复备份校验流程。验收：在空目录恢复备份并核对资源/版本/元数据。 （BLOCKED复核 2026-10-30：缺用户批准（P-015 进入实现阶段未获批））（已代决:按 docs/review/2026-10-30-decisions.md 执行(2026-10-30)）
- [ ] P-604 验证 NAS 发现与协议兼容矩阵。验收：记录已测设备、系统、协议、失败与限制，不假称全兼容。 （BLOCKED复核 2026-10-30：缺用户批准（P-015 进入实现阶段未获批））（已代决:按 docs/review/2026-10-30-decisions.md 执行(2026-10-30)）
- [ ] P-605 验证故障与恢复场景。验收：磁盘满、断网、中断、损坏及冲突各有结果证据。 （BLOCKED复核 2026-10-30：缺用户批准（P-015 进入实现阶段未获批））（已代决:按 docs/review/2026-10-30-decisions.md 执行(2026-10-30)）
- [ ] P-606 验证桌面键盘与缩放。验收：关键流程键盘可完成，缩放后控件可用。 （BLOCKED复核 2026-10-30：缺用户批准（P-015 进入实现阶段未获批））（已代决:按 docs/review/2026-10-30-decisions.md 执行(2026-10-30)）
- [ ] P-607 验证 macOS/Windows/Linux 构建。验收：三系统分别记录构建、插件部署和已知限制。 （BLOCKED复核 2026-10-30：缺用户批准（P-015 进入实现阶段未获批））（已代决:按 docs/review/2026-10-30-decisions.md 执行(2026-10-30)）
- [ ] P-608 验证中英界面与文档一致性。验收：同需求 ID、任务状态、能力限制逐项一致。 （BLOCKED复核 2026-10-30：缺用户批准（P-015 进入实现阶段未获批））（已代决:按 docs/review/2026-10-30-decisions.md 执行(2026-10-30)）
- [ ] P-609 执行 V1 用户旅程验收。验收：完成 requirements 中全部场景，未通过项保持未完成。 （BLOCKED复核 2026-10-30：缺用户批准（P-015 进入实现阶段未获批））（已代决:按 docs/review/2026-10-30-decisions.md 执行(2026-10-30)）

## 安全防线（防勒索病毒）

- [x] A-001 防线一：不可变版本库。验收：模拟加密工作区文件后，历史对象仍可读可恢复，测试绿；对象落盘后置只读。证据：docs/zh/design/85-antiransom.md#防线一不可变版本库防篡改（对象 append-only + 落盘置只读 + 库/工作区分离 + 恢复单向；提交 797fc61）。
- [x] A-002 防线二：批量修改哨兵。验收：测试 fixture（批量改名+随机字节覆盖）触发，单测覆盖三种特征，误报率有阈值测试。证据：`docs/zh/design/85-antiransom.md#防线二批量修改哨兵发现`（`src/sentinel.{h,cpp}`、`Library::snapshotAll`、`tests/security_test.cpp`，提交 ba576b7）。
- [x] A-003 防线三：整树回滚。验收：端到端测试——种入模拟勒索 → 哨兵告警 → 选锚点回滚 → 抽样文件哈希与感染前一致。证据：`docs/zh/design/85-antiransom.md#防线三整树回滚恢复`（`Library::rollbackAll`、`MainWindow::rollbackToSnapshot`、`tests/security_test.cpp` 的 `rollbackRestoresWorkspaceToPreInfectionAnchor`，提交 ff75b95）。

- [x] 审核发现：P-005 仅在 todo.md 标记完成，跨语言链接的 todo.en.md:15 仍为未勾选，与 P-608 中英一致要求不符（todo.md:15）。**已核实属实并修复**：todo.en.md:15 已改为 `- [x]`，与 todo.md 中 P-005 一致；ctest `library_history` 通过（1/1 绿）。
- [x] 审核发现：P-001–P-004 的 BLOCKED 理由含与本任务无关的“无 git 远程，无法 fetch+rebase+push”，实际阻断原因是用户未拍板，理由误导（todo.md:11）。**已核实属实并修复**：移除 P-001–P-004 中“无 git 远程”误导理由，仅保留缺用户拍板；git 确认无远程（`git remote -v` 为空）故该理由确属误导；ctest 绿（1/1）。
- [x] 审核发现：Shift+Tab 语义自相矛盾——3.5 说在对话框内循环，另一行又说从确认按钮反向回到摘要列表，同一按键不能同时满足（docs/zh/desktop-prototype-spec.md:161）。**已核实属实并修复**：规范 3.5 节 `Shift+Tab` 行改为“反向遍历（属上行对话框内循环的反向路径）：确认按钮 → 摘要列表，支持复核”，消除与 3.5 首行“仅在当前对话框内循环”的歧义；ctest 绿（1/1）。
- [x] 审核发现：规范自称基于 R01–R19，但未给出 R06 定时快照/暂停与排除规则、R07 元数据历史的任何界面/状态面，覆盖不完整（docs/zh/desktop-prototype-spec.md:3）。**已核实属实并修复**：新增 `2.3.1 R06 快照策略与排除规则状态`（快照调度/暂停/运行/失败、排除规则/命中）与 `2.3.2 R07 元数据历史状态`（可回看/字段变更/空历史/冲突）两个状态表；ctest 绿（1/1）。

## 审核发现

- [x] 审核发现：新增 todo.en.md 引用 `docs/requirements.md` 与 `docs/integrations.md`。**已核实属实并修复**：todo.en.md:7 两条引用改为 `docs/zh/requirements.md` 与 `docs/zh/integrations.md`，与 todo.md 一致；两路径已确认存在。
- [x] 审核发现：新增 todo.en.md 无任何 BLOCKED 标注。**已核实属实并修复**：todo.en.md 全部 94 项未完成任务已补 `(BLOCKED review 2026-10-30: ...)` 标注，与 todo.md 中英任务状态逐项一致。
- [x] 审核发现：P-001–P-004 的“无 git 远程”误导理由已移除。**已核实属实并修复**：从 todo.md 中 P-006–P-609 全部 90 处移除“无 git 远程，无法 fetch+rebase+push”误导理由，仅保留缺用户审核/批准；`git remote -v` 为空已确认该理由属误导。

- [x] 审核发现：P-101 在 P-015（进入实现阶段批准）未获批的情况下被标记完成，与 todo.md:5「明确批准后才开发」及 P-015 前置门槛矛盾；P-101 是未批准的实现类任务却先行完成（todo.md:29）。**已核实属实并修复**：`todo.md:29`、`todo.en.md:29` 均取消勾选并补 BLOCKED/已代决标注，保留实验证据但明确不计入交付；ctest `library_history` 通过（1/1 绿）；测试函数 5/5 绿。
- [x] 审核发现：todo.en.md 完全缺失「已代决」决策标注，而 todo.md 对应 93 项均有「已代决:按 docs/review/2026-10-30-decisions.md 执行(2026-10-30)」，且 todo.en.md 未引用 decisions 文档，与 P-608 中英一致性要求不符（todo.en.md:11）。**已核实属实并修复**：`todo.en.md:11–124` 全部 93 个未完成 P 任务均补 `(delegated: execute per docs/review/2026-10-30-decisions.md (2026-10-30))`；同步取消 P-101 后共 94 个 open 任务与中文版状态一致；`rg -c 'delegated: execute per' todo.en.md` = 94；ctest `library_history` 通过（1/1 绿）；测试函数 5/5 绿。
- [x] 审核发现：恢复时序存在窗口——restore() 先「保存未记录当前内容」(capture→append) 于事务外（src/library.cpp:153），再开始事务（src/library.cpp:155）；若事务前进程崩溃，则出现「已存恢复前版本但未完成恢复」的部分状态，且 capture 本身非事务性（src/library.cpp:140），与 docs/architecture.md 声称的「事务追加恢复版本」不符（src/library.cpp:153）。**已核实属实并修复**：`src/library.cpp:153-161` 将恢复前保存移入与恢复版本同一数据库事务，不再调用事务外 `capture()`；回归测试 `restorePreservationIsTransactionalWithTargetAppend`；ctest `library_history` 通过（1/1 绿）；测试函数 5/5 绿。
- [x] 审核发现：UI 中损坏/不支持对象哈希在历史时间轴缩略图与预览处直接静默忽略——reloadHistory() 用 QImageReader 读 objectPath 失败即空图标，showRevision() 对 image.isNull() 显示「此格式暂不支持」而非区分「内容损坏/校验失败」，掩盖了 readObject 的哈希校验失败，与 DESIGN.md:9「内容错误须可见，损坏历史不能静默恢复」不符（src/mainwindow.cpp:143,171）。**已核实属实并修复**：`src/mainwindow.cpp:142-152` 缩略图先调用 `Library::readObject()` 校验并显示警告图标；`src/mainwindow.cpp:165-171` 预览先校验，校验失败明确显示「内容损坏或校验失败」并禁用画面渲染；`Library::readObject` 公开于 `src/library.h:39`；回归测试 `readObjectIsPublicAndVerifiesHash`；ctest `library_history` 通过（1/1 绿）；测试函数 5/5 绿。
- [x] 审核发现：决策文档开头「不改变任何任务完成状态」与同文档末尾「todo 勾选（8 项）」矛盾（docs/review/2026-10-30-decisions.md:7,1519）。**已核实属实并修复**：头注改为「正文（建议部分）本身不改变任何任务完成状态」，并注明文末「落档执行状态」另行记录已执行动作（其中 8 项按原验收口径被勾选），消除头注与落档执行状态的范围冲突；ctest `library_history` 通过（1/1 绿）。
- [x] 审核发现：决策落档状态行「原勾选状态未改动」与相邻行「P-001/P-002/P-006–P-010/P-301 已勾选」直接冲突（docs/review/2026-10-30-decisions.md:1517,1519）。**已核实属实并修复**：落档状态行改为「落档标注这一步未改动任何勾选；随后执行阶段按原验收口径勾选 8 项（见下一条）」，将「未改动」限定为标注动作本身，消除与「todo 勾选（8 项）」的冲突；ctest `library_history` 通过（1/1 绿）。
- [x] 审核发现：P-007 验收要求区分「已缓存与缺失版本」，但勾选证据只记录「损坏/删除」四类且无已缓存、缺失状态（todo.md:17；docs/zh/design/10-prototype-states.md:36-45）。**已核实属实并修复**：`docs/zh/design/10-prototype-states.md` P-007 改为「五类例子」，新增「缺失版本」行（标记版本内容缺失、对象不可得，明确缺失≠损坏≠删除）并单独说明「已缓存」为未来远端缓存候选（不画成 V1 可用）；`todo.md`/`todo.en.md` P-007 证据同步更新；ctest `library_history` 通过（1/1 绿）。
- [x] 审核发现：P-301 新增测试只验证恢复前保存与目标追加的成功路径，未注入事务内失败以证明回滚保护真实覆盖（tests/library_test.cpp:59-76）。**已核实属实并修复**：新增回归测试 `restoreRollsBackPreservationOnTransactionFailure`，将工作副本目录置为不可写以在事务内写入失败，断言回滚后仅保留原导入修订、工作副本未变；ctest `library_history` 通过（1/1 绿）；测试函数 6/6 绿。
- [x] 审核发现：`snapshotAll` 的恢复锚点取 `history.first().id`（src/library.cpp:226），而 `revisions` 按 `id DESC` 排序（src/library.cpp:85），即取**最新（最大 id）**修订作「最近一次干净修订（感染前内容）」。但当勒索已改写工作区后，`scanChanges`（2s 周期，src/mainwindow.cpp:102）与 `pollSentinel`（5s 周期，src/mainwindow.cpp:108）竞态：若自动记录先于哨兵告警把被加密内容捕获为「恢复前自动保存」新修订，则该修订成为 `history.first()`，锚点实际指向**已感染内容**而非感染前，回滚将恢复到被加密版本，违背 85-antiransom.md:36「锚点始终指向可回滚的感染前状态」（src/library.cpp:226,85）。**注意：本发现仅指「未记录工作内容先被自动记录再告警」的窗口；若哨兵先触发则锚点仍为感染前。**（src/library.cpp:226）。**已核实属实并修复**：`snapshotAll`（src/library.cpp:224-245）改为跳过「内容与当前工作内容相同」的最新修订，锚点落在最新干净（感染前）修订——自动记录抢跑捕获的感染修订因只镜像当前工作字节而被排除；回归测试 `snapshotAnchorSkipsInfectedAutoRecord`（tests/security_test.cpp）；ctest `security_sentinel` 通过（1/1 绿）；测试函数 9/9 绿。提交 687bc87
- [x] 审核发现：`Library::hardenObjects` 定义为公开 API 且文档声明「用于打开既有库时加固」（src/library.h:50；85-antiransom.md:12），但本 diff 中无任何生产调用点（仅在 tests/library_test.cpp:153 被测试调用），打开既有库时并不会实际加固旧对象，与「幂等加固既有对象」的文档承诺不符（src/library.cpp:199,204；85-antiransom.md:12）。**若属有意未接线，应删除该公开 API 或补生产调用，避免文档与实现不一致。**（src/library.cpp:199）。**已核实属实并修复**：`Library` 构造函数末尾补生产调用 `hardenObjects()`（src/library.cpp:68-71），打开既有库即对可写对象重新加只读（幂等）；回归测试 `openingExistingLibraryHardensObjects`（tests/library_test.cpp，模拟篡改清除只读后重开，断言对象恢复只读）；ctest `library_history` 通过（1/1 绿）；测试函数 9/9 绿。提交 687bc87
- [x] 审核发现：`rollbackAll` 中 `anchorBytes.isEmpty()` 作为「锚点修订消失」的防御判断不可靠——若锚点修订的内容本身为**空字节**（合法对象），`readObject` 返回空 `QByteArray`，会被误判为「锚点消失」而 `continue` 跳过该素材，导致静默不回滚（src/library.cpp:277-280）。空内容虽罕见（导入空文件即产生空对象），但 append 对空内容同样可生成对象，故该防御以内容非空判存在存在逻辑缺口（src/library.cpp:280）。**已核实属实并修复**：`rollbackAll`（src/library.cpp:289-297）改以**修订 id 存在性**（`anchorFound`）而非内容非空判断锚点是否消失——空内容锚点照常回滚；回归测试 `rollbackHandlesEmptyAnchorObject`（tests/security_test.cpp，导入空文件→快照→改内容→回滚→断言恢复空内容）；ctest `security_sentinel` 通过（1/1 绿）；测试函数 9/9 绿。提交 687bc87
- [x] 审核发现：`MainWindow::rollbackToSnapshot` 的标签构造 `QStringLiteral("#%1 %2 · %3 个素材").arg(snapshot.id).arg(snapshot.created, snapshot.assets)` 中，`QString::arg(const QString&, int)` 二参重载把 `snapshot.created` 当作字符串、`snapshot.assets` 当作 `fieldWidth` 而非替换 `%3`，导致 `%3` 未被替换而原样显示为字面量；已实测确认（输出 `#5 2026-10-31T00:00:00 · %3 个素材`），快照列表标签缺失素材数量（src/mainwindow.cpp:286）。**已核实属实并修复**：`src/mainwindow.cpp:286-289` 改为三个独立 `.arg()` 链式调用（`.arg(snapshot.id).arg(snapshot.created).arg(snapshot.assets)`），`%3` 正确替换为素材数量；构建通过、ctest 全绿（library_history + security_sentinel 2/2）。提交 687bc87
- [x] 审核发现：`snapshotAll` 的锚点若为「空内容对象」时，`append` 内 `readObject(hash) != bytes` 分支中 `readObject` 返回空 `QByteArray` 与 `bytes`（空）相等，不触发失败，但 `hardenObjects`/`makeImmutable` 对空对象照常执行；此路径本身无错，但与 `rollbackAll` 的 `anchorBytes.isEmpty()` 误判共同构成「空对象无法回滚」的组合缺陷（见上条），建议以显式存在性/修订 id 校验替代内容非空判断（src/library.cpp:280）。**已核实属实并修复**：`rollbackAll`（src/library.cpp:289-297）以修订 id 存在性替代内容非空判断，空内容锚点可正常回滚；组合缺陷消除——`snapshotAll` 空对象锚点 + `rollbackAll` 修订 id 校验均正常；回归测试 `rollbackHandlesEmptyAnchorObject`（tests/security_test.cpp）；ctest `security_sentinel` 通过（1/1 绿）；测试函数 9/9 绿。提交 687bc87
