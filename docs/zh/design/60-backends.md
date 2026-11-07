# 后端（Git/SVN/时光机）重启条件与未来验收（代决执行，2026-10-30）

> 状态：**已代决**。依据 `docs/zh/review/2026-10-30-decisions.md` 执行 P-401–P-411 的立即动作。P-401–P-405 已于 **2026-11-08 实现**（P-015 闸门放行）；P-406–P-411 仍为**延后 V1.x** 项，本节保留其重启条件、未来验收样本与能力边界。

## P-401 本地 Git 仓库识别（已实现 2026-11-08）

**实现（2026-11-08）**：`git_source::identify/branches/status`（src/git_source.cpp:57-134）经 argv-only `git` 调用（30s 上限、`--no-optional-locks` 保证不写索引），只读识别仓库根/分支/HEAD 与工作区状态。测试 `identifyReportsRepoAndHead/branchesListCurrentAndHeads/statusReflectsWorktreeWithoutWriting`；仓库指纹前后一致证明只读。**不做**：探测外部仓库、clone、checkout、fetch、提交。

**重启条件（保留，真实样本仍手动验收）**：获得实际仓库样本；只读边界获批；支持布局清单获批。

## P-402 Git 文件树与日志读取（已实现 2026-11-08）

**实现（2026-11-08）**：`git_source::log/tree`（src/git_source.cpp:136-216）按明确提交标识读取日志（新→旧，RS/US 分隔）与递归文件树（`ls-tree -r --long --full-tree`，子目录收窄），路径仓库相对、跨提交稳定。测试 `logListsCommitsNewestFirst/treeListsCommitFilesWithSubdir`。**不做**：Git 历史浏览；不只按当前路径猜历史（会把当前分支路径映射到所有提交而漏历史改名）。

**未来验收样本（保留）**：分支、删除、改名、特殊字符路径、大目录。

## P-403 Git 历史文件提取（已实现 2026-11-08）

**实现（2026-11-08）**：`git_source::extract`（src/git_source.cpp:225-253）`git show <rev>:<path>` 读对象库字节到内存，绝不写工作区（改写工作区文件不影响结果，测试验证）。**不做**：检出历史到工作区；未校验临时内容冒充原历史。

**未来验收（保留）**：指定提交和路径**字节正确**，当前工作区前后不变；真实项目样本手动验收。

## P-404 LFS 指针与缺失对象（已实现 2026-11-08，上线强制前置）

**实现（2026-11-08）**：`git_source::isLfsPointer/objectPresent`（src/git_source.cpp:59-69,240-265）——指针小文本段绝不当作图像字节解码；`extract` 在对象缺失时返回指针文本、对象在 `.git/lfs/objects/aa/bb/<完整oid>` 时返回真实字节（对象路径以完整 oid 作文件名，测试验证）。真实 git-lfs 3.7.1 仓库覆盖指针/缺失/可用/非指针四类。**不做**：指针作为图像解码；默认自动下载缺失对象。

**未来样本（保留）**：指针、对象缺失、对象可用、非指针文本四类（区分原因）。

## P-405 LFS 按需获取（已实现 2026-11-08，上线前置）

**实现（2026-11-08）**：`git_source::fetchLfs`（src/git_source.cpp:267-289）按单文件 `--include` 将对象写入 `.git/lfs/objects`（工作区不变，测试断言工作区仍为指针）；remote 名经 `git remote` 检测（`git lfs fetch` 位置参数是 remote 名而非 ref）。**本方法绝不隐式运行**——调用方必须先取得用户独立同意；测试即同意门后的获取/校验。**不做**：把接入 Git 当无限下载授权；后台 fetch 或修改仓库配置。

**未来设计（保留）**：下载体积、目的地、同意/拒绝、失败与哈希验证状态；优先应用缓存；明确缓存位置与实际副作用（只读仓库也可能被缓存/配置写入）。

## P-406 SVN 连接（延后，重启条件）

**重启条件**：有真实 SVN 使用者与测试环境。记录需要的工作副本/URL、服务端版本、匿名与认证样本条件。**V1 不做**：收集用户凭据；把 V1 宣传为支持 SVN。

## P-407 SVN 日志树与锁（未来样本）

随连接延后。未来区分：未知/可读/无锁/有锁及权限错误（「读不到锁」≠「没有锁」）；准备锁定文件样本。**V1 不做**：获取/解除锁；把无权限当无历史。

## P-408 SVN 历史文件提取（未来验收）

未来按指定 revision 导出到库外。验收：指定 revision 内容校验，工作副本状态前后不变。**V1 不做**：提交、update 覆盖、替用户解决工作副本冲突。

## P-409 统一后端历史能力显示（能力→界面映射）

能力驱动的历史操作显示；先覆盖自建和明确未支持态。**映射**：能力 → 界面操作 → 核心校验；未来只读后端仅导出/提取（不显示写回恢复按钮）。**V1 不做**：为未实现后端显示可用标记；入口跳转当历史支持。

**实现（2026-11-01）**
- 设计：`historyActionsFor(info, selectionValid)` 纯映射（backend.h）——restore 动作仅当 `Restore` 能力且选中；extract/导出/对比仅当 `History` 能力且选中；不满足时置空并给出 `unsupportedReason`（工具提示展示）。
- 接线：`showRevision()` 用该映射设置恢复/导出/对比按钮启用与工具提示；核心侧校验（restore/exportRevision 已按版本归属+能力契约校验）不变。
- 只读后端（Git/SVN，V1.x）不显示写回恢复按钮，仅导出——映射已在核心路径强制，未来后端接入即生效。
- 测试：`historyActionsFollowBackendCapabilities`（self 全可用；只读后端 restore 关+原因、extract 开；无 History 关 extract；无选中全关无原因）。
- 验证：ctest 2/2 绿；离线烟雾运行无崩溃。

## P-410 macOS 时光机系统适配边界

只评估官方系统入口与权限边界，不解析备份内容。当前**保持未验证**（无 macOS 环境证据）。**V1 不做**：读取 Time Machine 快照；修改系统备份配置；承诺已备份。

## P-411 时光机能力提示与系统入口

按 P-410 验证结果处理三状态：有入口 / 无入口 / 权限失败。入口仅**已验证 macOS** 提供；其余平台说明不适用。**V1 不做**：Windows/Linux 假冒可用；未做 macOS 验证就标验收通过（三台真机 openclaw/imac/.5 均为 Windows，macOS 项保持跳过）。
