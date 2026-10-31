# pentimento V1 原子 TODO

[English](todo.en.md) | [简体中文](todo.md)

**状态：待审核需求。全部实现任务未完成；先文档、原型和多轮审核，明确批准后才开发。现有实验代码不计入交付。**

每项只有一个交付目标和可观察验收条件；具体范围见 `docs/zh/requirements.md`，后端/NAS 边界见 `docs/zh/integrations.md`。分组表示依赖顺序，不表示已经开工。

## 需求与审核

- [x] P-001 确认 V1 功能范围。验收：逐项确认 requirements 中 R01–R19 的纳入与排除。证据：docs/zh/design/00-scope-and-budgets.md#p-001（R01–R19 代决记录 2026-10-30，用户可推翻）。
- [x] P-002 确认版本后端能力表。验收：明确自建可写、Git/LFS 与 SVN 只读、时光机系统集成边界。证据：docs/zh/design/00-scope-and-budgets.md#p-002（能力矩阵 2026-10-30，用户可推翻）。
- [x] P-003 确认 NAS 接入范围。验收：确认挂载 SMB/NFS、直接 WebDAV/SFTP 与发现机制。证据：docs/zh/design/00-scope-and-budgets.md:53-55（NAS 范围代决记录 2026-10-30，用户可推翻）。
- [ ] P-004 确认素材测试样本与性能预算。验收：记录样本格式、文件体积、资源数量和可验证阈值。 （BLOCKED 2026-10-31：立即动作已执行（样本矩阵/两档预算/测量口径，docs/zh/design/00-scope-and-budgets.md#P-004，提交 5266cd3）；待用户输入——素材样本可用性与使用权限、三系统测试机可用性与磁盘余量、256 MiB 单文件上限是否取消、D-21/D-23 三系统硬件与阈值（docs/review/2026-10-30-decisions.md:142））（已代决:按 docs/review/2026-10-30-decisions.md 执行(2026-10-30)）
- [x] P-005 使用 ui-ux-pro-max 形成原型规范。验收：提交桌面布局、状态和键盘交互规范；不套用营销页布局。证据：`docs/zh/desktop-prototype-spec.md`。
- [x] P-006 绘制素材库浏览原型。验收：展示真实层级、网格/列表、筛选、详情及导入入口。证据：docs/zh/design/10-prototype-states.md#p-006（示例数据/状态/焦点顺序 2026-10-30；P-012/P-013 用户审核待进行）。
- [x] P-007 绘制版本时间轴原型。验收：区分当前、历史、已缓存与缺失版本。证据：docs/zh/design/10-prototype-states.md#p-007（五类例子 2026-10-30；已缓存按代决标为未来候选，不画成 V1 可用；P-012/P-013 用户审核待进行）。
- [x] P-008 绘制双版本对比原型。验收：呈现并排、滑动和同步缩放入口。证据：docs/zh/design/10-prototype-states.md#p-008（对齐/边界规则 2026-10-30；P-012/P-013 用户审核待进行）。
- [x] P-009 绘制整库时间点原型。验收：显示快照时间、历史目录树和已删除资源。证据：docs/zh/design/10-prototype-states.md#p-009（两快照例子/浏览与恢复入口 2026-10-30；P-012/P-013 用户审核待进行）。
- [x] P-010 绘制恢复预演原型。验收：展示新增/覆盖/跳过、冲突及保护当前状态的操作。证据：docs/zh/design/10-prototype-states.md#p-010（操作清单/异常状态 2026-10-30；P-012/P-013 用户审核待进行）。
- [ ] P-011 绘制 NAS 与仓库连接原型。验收：覆盖发现、手动地址、认证、能力提示和离线状态。 （BLOCKED 2026-11-01：立即动作已执行——原验收差异/缩减范围状态表/焦点顺序/键盘与安全已补齐（docs/zh/design/10-prototype-states.md#p-011，提交 1e1acf0）；待用户批准缩减验收后勾选——决议明确「缩小后不能直接标为已通过」）（已代决:按 docs/review/2026-10-30-decisions.md 执行(2026-10-30)）
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
- [ ] P-302 实现内容对象存储。验收：流式哈希、去重和对象校验通过大文件样本。 （BLOCKED复核 2026-10-30：缺用户批准（P-015 进入实现阶段未获批））（已代决:按 docs/review/2026-10-30-decisions.md 执行(2026-10-30)）
- [ ] P-303 实现手动记录版本。验收：一次操作创建可查询的版本 ID 与说明。 （BLOCKED复核 2026-10-30：缺用户批准（P-015 进入实现阶段未获批））（已代决:按 docs/review/2026-10-30-decisions.md 执行(2026-10-30)）
- [ ] P-304 实现稳定变化自动记录。验收：半写入与连续保存样本按已定策略处理。 （BLOCKED复核 2026-10-30：缺用户批准（P-015 进入实现阶段未获批））（已代决:按 docs/review/2026-10-30-decisions.md 执行(2026-10-30)）
- [ ] P-305 实现定时快照与暂停。验收：调度状态可见，暂停后不产生自动快照。 （BLOCKED复核 2026-10-30：缺用户批准（P-015 进入实现阶段未获批））（已代决:按 docs/review/2026-10-30-decisions.md 执行(2026-10-30)）
- [ ] P-306 实现版本排除规则。验收：排除对象不进入后续快照。 （BLOCKED复核 2026-10-30：缺用户批准（P-015 进入实现阶段未获批））（已代决:按 docs/review/2026-10-30-decisions.md 执行(2026-10-30)）
- [ ] P-307 实现关键版本固定。验收：固定版本不被保留策略清理。 （BLOCKED复核 2026-10-30：缺用户批准（P-015 进入实现阶段未获批））（已代决:按 docs/review/2026-10-30-decisions.md 执行(2026-10-30)）
- [ ] P-308 实现单素材时间轴。验收：顺序、时间、说明与当前/历史标识准确。 （BLOCKED复核 2026-10-30：缺用户批准（P-015 进入实现阶段未获批））（已代决:按 docs/review/2026-10-30-decisions.md 执行(2026-10-30)）
- [ ] P-309 实现元数据历史。验收：可查看旧名称、标签、备注与来源记录。 （BLOCKED复核 2026-10-30：缺用户批准（P-015 进入实现阶段未获批））（已代决:按 docs/review/2026-10-30-decisions.md 执行(2026-10-30)）
- [ ] P-310 实现删除墓碑。验收：删除后历史查询仍可找到原素材。 （BLOCKED复核 2026-10-30：缺用户批准（P-015 进入实现阶段未获批））（已代决:按 docs/review/2026-10-30-decisions.md 执行(2026-10-30)）
- [ ] P-311 实现整库一致快照。验收：快照目录树与内容引用属于同一完整清单。 （BLOCKED复核 2026-10-30：缺用户批准（P-015 进入实现阶段未获批））（已代决:按 docs/review/2026-10-30-decisions.md 执行(2026-10-30)）
- [ ] P-312 实现时间点目录浏览。验收：切换快照能看到当时路径和删除状态。 （BLOCKED复核 2026-10-30：缺用户批准（P-015 进入实现阶段未获批））（已代决:按 docs/review/2026-10-30-decisions.md 执行(2026-10-30)）
- [ ] P-313 实现双版并排与同步缩放。验收：同一位置和比例可重复比较。 （BLOCKED复核 2026-10-30：缺用户批准（P-015 进入实现阶段未获批））（已代决:按 docs/review/2026-10-30-decisions.md 执行(2026-10-30)）
- [ ] P-314 实现滑动对比。验收：拖动分界线能检查两版同坐标区域。 （BLOCKED复核 2026-10-30：缺用户批准（P-015 进入实现阶段未获批））（已代决:按 docs/review/2026-10-30-decisions.md 执行(2026-10-30)）
- [ ] P-315 实现恢复预演。验收：列出新增、替换、跳过和冲突，不先写文件。 （BLOCKED复核 2026-10-30：缺用户批准（P-015 进入实现阶段未获批））（已代决:按 docs/review/2026-10-30-decisions.md 执行(2026-10-30)）
- [ ] P-316 实现恢复前保护当前状态。验收：未记录内容先保存，失败则停止恢复。 （BLOCKED复核 2026-10-30：缺用户批准（P-015 进入实现阶段未获批））（已代决:按 docs/review/2026-10-30-decisions.md 执行(2026-10-30)）
- [ ] P-317 实现单素材恢复为新版本。验收：原历史保留，恢复结果可再次查到。 （BLOCKED复核 2026-10-30：缺用户批准（P-015 进入实现阶段未获批））（已代决:按 docs/review/2026-10-30-decisions.md 执行(2026-10-30)）
- [ ] P-318 实现快照内选中项恢复。验收：只处理预演确认的资源与元数据。 （BLOCKED复核 2026-10-30：缺用户批准（P-015 进入实现阶段未获批））（已代决:按 docs/review/2026-10-30-decisions.md 执行(2026-10-30)）
- [ ] P-319 实现历史副本导出。验收：导出不改变版本后端或当前工作区。 （BLOCKED复核 2026-10-30：缺用户批准（P-015 进入实现阶段未获批））（已代决:按 docs/review/2026-10-30-decisions.md 执行(2026-10-30)）
- [ ] P-320 实现恢复日志与重启收敛。验收：中断后明确完成或回滚，不隐藏半恢复状态。 （BLOCKED复核 2026-10-30：缺用户批准（P-015 进入实现阶段未获批））（已代决:按 docs/review/2026-10-30-decisions.md 执行(2026-10-30)）

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
- [x] 审核发现：`snapshotAll` 的恢复锚点取 `history.first().id`（src/library.cpp:226），而 `revisions` 按 `id DESC` 排序（src/library.cpp:85），即取**最新（最大 id）**修订作「最近一次干净修订（感染前内容）」。但当勒索已改写工作区后，`scanChanges`（2s 周期，src/mainwindow.cpp:102）与 `pollSentinel`（5s 周期，src/mainwindow.cpp:108）竞态：若自动记录先于哨兵告警把被加密内容捕获为「恢复前自动保存」新修订，则该修订成为 `history.first()`，锚点实际指向**已感染内容**而非感染前，回滚将恢复到被加密版本，违背 85-antiransom.md:36「锚点始终指向可回滚的感染前状态」（src/library.cpp:226,85）。**注意：本发现仅指「未记录工作内容先被自动记录再告警」的窗口；若哨兵先触发则锚点仍为感染前。**（src/library.cpp:226）。**已核实属实并修复**：`snapshotAll`（src/library.cpp:224-245）锚点取「最新一个内容与当前工作文件不同的修订」——自动记录抢跑捕获的感染修订因哈希等于当前工作字节被跳过，锚点落在感染前干净修订；若全部修订都等于当前内容（未改动素材/误报），保守取最新修订且当前内容保留为可恢复修订；回归测试 `snapshotAnchorSkipsInfectedAutoRecord`（tests/security_test.cpp）；ctest `security_sentinel` 通过（1/1 绿）；测试函数 9/9 绿。提交 687bc87 + 9278b43
- [x] 审核发现：`Library::hardenObjects` 定义为公开 API 且文档声明「用于打开既有库时加固」（src/library.h:50；85-antiransom.md:12），但本 diff 中无任何生产调用点（仅在 tests/library_test.cpp:153 被测试调用），打开既有库时并不会实际加固旧对象，与「幂等加固既有对象」的文档承诺不符（src/library.cpp:199,204；85-antiransom.md:12）。**若属有意未接线，应删除该公开 API 或补生产调用，避免文档与实现不一致。**（src/library.cpp:199）。**已核实属实并修复**：`Library` 构造函数末尾补生产调用 `hardenObjects()`（src/library.cpp:68-71），打开既有库即对可写对象重新加只读（幂等）；回归测试 `openingExistingLibraryHardensObjects`（tests/library_test.cpp，模拟篡改清除只读后重开，断言对象恢复只读）；ctest `library_history` 通过（1/1 绿）；测试函数 9/9 绿。提交 687bc87 + 9278b43
- [x] 审核发现：`rollbackAll` 中 `anchorBytes.isEmpty()` 作为「锚点修订消失」的防御判断不可靠——若锚点修订的内容本身为**空字节**（合法对象），`readObject` 返回空 `QByteArray`，会被误判为「锚点消失」而 `continue` 跳过该素材，导致静默不回滚（src/library.cpp:277-280）。空内容虽罕见（导入空文件即产生空对象），但 append 对空内容同样可生成对象，故该防御以内容非空判存在存在逻辑缺口（src/library.cpp:280）。**已核实属实并修复**：`rollbackAll`（src/library.cpp:289-297）改以**修订 id 存在性**（`anchorFound`）而非内容非空判断锚点是否消失——空内容锚点照常回滚；回归测试 `rollbackHandlesEmptyAnchorObject`（tests/security_test.cpp，导入空文件→快照→改内容→回滚→断言恢复空内容）；ctest `security_sentinel` 通过（1/1 绿）；测试函数 9/9 绿。提交 687bc87 + 9278b43
- [x] 审核发现：`MainWindow::rollbackToSnapshot` 的标签构造 `QStringLiteral("#%1 %2 · %3 个素材").arg(snapshot.id).arg(snapshot.created, snapshot.assets)` 中，`QString::arg(const QString&, int)` 二参重载把 `snapshot.created` 当作字符串、`snapshot.assets` 当作 `fieldWidth` 而非替换 `%3`，导致 `%3` 未被替换而原样显示为字面量；已实测确认（输出 `#5 2026-10-31T00:00:00 · %3 个素材`），快照列表标签缺失素材数量（src/mainwindow.cpp:286）。**已核实属实并修复**：`src/mainwindow.cpp:286-289` 改为三个独立 `.arg()` 链式调用（`.arg(snapshot.id).arg(snapshot.created).arg(snapshot.assets)`），`%3` 正确替换为素材数量；构建通过、ctest 全绿（library_history + security_sentinel 2/2）。提交 687bc87 + 9278b43
- [x] 审核发现：`snapshotAll` 的锚点若为「空内容对象」时，`append` 内 `readObject(hash) != bytes` 分支中 `readObject` 返回空 `QByteArray` 与 `bytes`（空）相等，不触发失败，但 `hardenObjects`/`makeImmutable` 对空对象照常执行；此路径本身无错，但与 `rollbackAll` 的 `anchorBytes.isEmpty()` 误判共同构成「空对象无法回滚」的组合缺陷（见上条），建议以显式存在性/修订 id 校验替代内容非空判断（src/library.cpp:280）。**已核实属实并修复**：`rollbackAll`（src/library.cpp:289-297）以修订 id 存在性替代内容非空判断，空内容锚点可正常回滚；组合缺陷消除——`snapshotAll` 空对象锚点 + `rollbackAll` 修订 id 校验均正常；回归测试 `rollbackHandlesEmptyAnchorObject`（tests/security_test.cpp）；ctest `security_sentinel` 通过（1/1 绿）；测试函数 9/9 绿。提交 687bc87 + 9278b43

## 对标缺口复核

> 对标调研（[docs/zh/design/90-benchmark.md](docs/zh/design/90-benchmark.md) 第四节）共 14 条功能缺口逐条复核：属于 V1 范围（含待拍板决策点 D-xx）的登记为 open 项，随对应决策点拍板；不属于的勾掉并注明排除理由。逐家详表/矩阵/来源见 90-benchmark.md。

- [ ] B-001 智能文件夹/保存的搜索。Eagle/Billfish/Bridge 智能集合均有；pentimento 仅一次性组合筛选。**属 V1**：对应 R03 组合筛选，D-03 待拍板时一并确认「另存为智能文件夹」（做法 9）。 （BLOCKED 2026-10-31：缺用户决策——D-03 待拍板，确认「另存为智能文件夹」是否纳入）
- [ ] B-002 颜色搜索/按颜色筛选。Eagle/Billfish 有；pentimento 已提取色彩元数据。**属 V1**：对应 R03/R05 检索，补颜色检索入口（做法 11）。 （BLOCKED 2026-10-31：缺用户决策——D-03 检索范围待拍板；实现须待 P-015 批准）
- [ ] B-003 导入去重的用户流程。Eagle/Billfish 有；pentimento 有内容寻址去重但未暴露。**属 V1**：对应 R02 导入，补「导入时重复询问 + 事后重复扫描合并」（做法 10）。 （BLOCKED 2026-10-31：缺用户决策——R02 导入流程范围待确认；实现须待 P-015 批准）
- [x] B-004 浏览器扩展/剪贴板采集。**排除**：R02 范围为文件复制导入，浏览器扩展/剪贴板需扩展基础设施，超出 V1 范围；列为 V2 候选——但「参考图收集」场景是 Eagle 体验核心，★ 建议进入需求评审（90-benchmark.md 第六节做法）。
- [x] B-005 图上标注/批注。**排除**：R05 范围为标签/评分/备注/来源授权文字记录，图上批注超出；列为 V2 候选。
- [ ] B-006 库内文件夹组织与改名/移动后历史归属保持。Eagle/Billfish 天然支持；pentimento P-216/D-05 明确未决。**属 V1 决策**：D-05 待拍板（库内移动/改名后路径语义与历史归属）。 （BLOCKED 2026-10-31：缺用户决策——D-05 待拍板）
- [x] B-007 格式广度。**排除**：R04 已定 V1 首批格式（PNG/JPEG/WebP/TGA/BMP），PSD 合成预览仅评估（D-04）；更大广度（字体免安装预览/音频悬停/视频逐帧等）为 V2 候选。
- [ ] B-008 Windows 上基于 USN change journal 的变更监听。Windows 文件历史/Anchorpoint Auto Fetch 有事件驱动先例。**属 V1 决策**：对应 D-06（启发式 vs 文件监视器），USN journal 为现成答案（做法 7）。 （BLOCKED 2026-10-31：缺用户决策——D-06 待拍板）
- [x] B-009 版本历史的缩略图条。**排除**：V1 时间轴已实现缩略图维度（带缩略图的版本时间轴，见 README「当前可用」）；Alienbrain 式「整条缩略图浏览历史」强化为 V2 候选，非 V1 缺口。
- [x] B-010 编辑器/DCC 集成。**排除（定位外）**：单机素材库无此定位（调研原文即标注「定位外缺口，不建议 V1 做」）；R01–R19 无 DCC 集成需求。
- [x] B-011 协作锁。**排除（定位外）**：单机定位外（矩阵已标注「R 需求未含」）；做法 12 的单机「锁定防误改」已列为 V2 候选（90-benchmark.md 第六节）。
- [x] B-012 NAS/外部目标的自动化定期备份。**排除**：R19 范围为备份包导出+校验+一致性检查+保留策略预演，无自动调度语义；自动定期备份需后台调度服务，超出 V1 范围，列为 V2 候选（★ 建议进入需求评审）。
- [ ] B-013 保留策略的显式设置界面。Time Machine/文件历史有可视化保留规则；pentimento 因「无历史删除」约定只能做「仅预演」。**属 V1 决策**：对应 R19/D-19（预演 vs 实际清理），界面形态可抄前两者。 （BLOCKED 2026-10-31：缺用户决策——D-19 待拍板）
- [ ] B-014 断连目标的友好降级。Time Machine 断连自动续备+本地快照兜底、文件历史低优先级 I/O 可断可续。**属 V1**：对应 R18 断线重连状态机（D-18 待拍板），本地锚点兜底可抄做法 6（防勒索紧急锚点）。 （BLOCKED 2026-10-31：缺用户决策——D-18 待拍板）

## 审核发现（7233ecc7..HEAD 只审）

- [x] 审核发现：README 徽章声明 CMake 3.24+，但「构建与运行」仍写「需要 CMake 3.21+」，与 `CMakeLists.txt:1`（`cmake_minimum_required(VERSION 3.24)`）不一致（README.md:25）。 （已修：README.md:25 改为 CMake 3.24+，0bc1f09）
- [x] 审核发现：`docs/zh/design/90-benchmark.md:273` 称「pentimento 时间轴 R07 目前未提缩略图维度」，但 `todo.md:168` 的 B-009 判定「V1 时间轴已实现缩略图维度」，两处对同一现状描述矛盾（README「当前可用」亦确认时间轴带缩略图）。 （已修：docs/zh/design/90-benchmark.md:273 改为「已实现缩略图维度」，7877cf0）
- [x] 审核发现：`fetch_dep` 的 `if(TARGET ${name} OR DEFINED FETCH_DEP_${name}_DONE)` 用 `DEFINED FETCH_DEP_${name}_DONE` 作「已声明」判据，但 `set(FETCH_DEP_${name}_DONE ON)` 在 `FetchContent_Declare` 之前设置，若同名依赖的 `FetchContent_Declare` 本身失败（如参数错误），`DEFINED` 已为真，后续调用会静默跳过声明，错误被掩盖；且 `set(... DONE ON)` 未加 `PARENT_SCOPE`，在 `fetch_dep` 函数作用域内设置对父作用域不可见，`DEFINED FETCH_DEP_${name}_DONE` 在下次调用时恒为假，该去重判据实际不生效（CMakeLists.txt:33-36）。 （已修：CMakeLists.txt 的 fetch_dep 将 DONE 标志移至 FetchContent_Declare 之后并加 PARENT_SCOPE，4a79ed4）
- [x] 审核发现：`fetch_dep` 仅检查 `TARGET ${name}`，若某依赖未提供同名 target（仅提供库别名或不同名 target，或纯头文件依赖），则无法触发去重；去重依赖 target 名与依赖名相同这一隐含约定未在文档中说明（CMakeLists.txt:33）。 （已修：CMakeLists.txt 注释与 35-dependencies.md 第 3 节补充 target 同名去重约定，015d8ca）
- [x] 审核发现：`docs/zh/design/35-dependencies.md:24` 声明「CI（.github/workflows/nightly.yml）只允许出现 Qt 相关的包安装」，但 `nightly.yml:20` 同时安装 `ninja-build`（构建工具，非 Qt 包），与「只允许 Qt 相关包安装」口径矛盾（35-dependencies.md:24；.github/workflows/nightly.yml:20）。 （已修：35-dependencies.md:24 口径补记 ninja-build 为构建工具例外，df7f210）
- [x] 审核发现：`fetch_dep` 去重注释与 35-dependencies.md 第 3 节将判据表述为「依赖须提供与 <名字> 同名的 CMake target；纯头文件/别名 target 需额外用 FetchContent_GetProperties 显式处理」，但 4a79ed4 已在 `FetchContent_Declare` 之后无条件 `set(FETCH_DEP_<名字>_DONE ON PARENT_SCOPE)`，去重实际仅以 <名字> 为键、不依赖同名 target，文档所述约定与实现不符，易误导后续新增依赖者（CMakeLists.txt:32-33；docs/zh/design/35-dependencies.md:30）。 （已修：CMakeLists.txt:31-33 与 35-dependencies.md:30 改为「以 <名字> 为键去重、不依赖同名 target」口径，6cf20d8）
