# 自建版本管理设计与验收（代决执行，2026-10-30）

> 状态：**已代决**。依据 `docs/review/2026-10-30-decisions.md` 执行 P-301–P-320 的立即动作。含修订/快照清单格式定义（含四组例子验证）、对象存储流程、恢复与快照语义、故障收敛阶段。**实现须待 P-015 批准**；本节定义格式与语义，不构成实现完成。

## P-301 修订与快照清单格式（定义）

固定带 schema 版本的修订/快照清单，区分资源 ID、路径、内容哈希；不把绝对路径当永久身份。**格式草案**：

```json
{ "schema": 1,
  "revision": { "id": 1001, "assetId": "A-7f3e", "path": "characters/hero/hero-banner.webp",
                "contentHash": "sha256:9f2c...", "size": 20480, "note": "首版",
                "createdUtcMs": 1769760000000, "pinned": false, "tombstone": false,
                "excluded": false, "metadata": { "name": "...", "tags": [], "rating": null, "note": "", "source": "" } },
  "snapshot": { "id": "S-20261030", "takenUtcMs": 1769760000000,
                "entries": [ {"assetId": "A-7f3e", "path": "...", "contentHash": "sha256:9f2c...", "state": "present"} ] } }
```

**四组例子验证清单可完整表达**：

| 例子 | 表达 |
|---|---|
| 改名 | 同 assetId、`path` 变化；历史条目保留旧 `path`（新修订记录新 path） |
| 删除 | 新修订 `tombstone:true`；对象/历史保留，可找回 |
| 排除 | 条目 `excluded:true`（命中排除规则），不进后续快照 |
| 元数据修改 | 新修订仅 `metadata` 变化、`contentHash` 不变（不生成内容对象） |

**规则**：schema 版本化（升级可迁移，避免旧库读错）；不在格式未定前大量生成历史。

## P-302 内容对象存储（流程）

- 流式哈希（SHA-256，不整文件驻内存）；不可变对象（按哈希命名，写后不改）；同内容去重（相同哈希共享对象）。
- **顺序**：临时对象（`objects/.tmp/`）→ 完整校验（读取哈希）→ 发布（改名到 `objects/<hash>`，若已存在则校验后丢弃临时）→ 索引登记。
- 验证：大文件（64 MiB+）、磁盘满（临时写失败 → 清理临时，不发布）。
- 风险：中途断电/复用损坏对象使多版本不可读 → 每次读取校验（见 P-601）；校验失败阻断使用。
- 不做：覆盖已存在不同内容对象；历史对象 GC（意外对象占空间，不误清理）。
- 实现（2026-11-01）：内容对象存储统一走暂存→校验→发布顺序——storeObject(bytes)/storeObjectFromFile(path,hash) 先在 objects/.tmp/ 写暂存（字节直接写 / 流式 1 MiB 分块拷贝），再重读校验暂存内容哈希=目标地址，改名发布到 objects/<hash>（若并发已出现同名对象则校验后丢弃暂存，绝不覆盖），成功后 makeImmutable；任何失败删除暂存并抛出，绝不发布部分对象、不留孤儿暂存。哈希：bytes 整块 / 文件流式 SHA-256（digestFile 1 MiB 分块，不整文件驻内存）；去重：objects/<hash> 已存在则校验后直接复用（不重写）；capture() 改为从工作副本流式入库（只算一次哈希，复用做未变检测与暂存校验），大文件不再整块进内存；每次读取仍校验（readObject，P-601）。测试 contentObjectStorageStreamsAndDeduplicates（64 MiB+ 导入+capture 流式入库、同内容不重复版本/对象、改内容再入库、暂存目录无残留）与 objectStoreTempFailureLeavesNoPartialObject（暂存目录只读模拟磁盘满：capture 抛错、无部分对象/无孤儿暂存/工作副本未动，恢复后同 capture 可重试成功）。ctest 2/2 绿；offscreen 冒烟正常。

## P-303 手动记录版本（语义）

- 始终可用；返回版本 ID、说明、是否内容未变。
- 相同内容：不生成重复版本，返回「内容未变」+ 当前版本 ID（明确反馈，防误以为已保护）。
- 失败提示：写盘/登记失败明确错误，可重试；不静默。
- 完成后时间轴定位：新版本置顶并选中；不承诺记录编辑器内未写盘内容。
- 实现（2026-11-01）：库 API 增 captureRevision(asset,note,created) 返回「代表当前工作副本的版本 ID」——新记录版本 ID，或内容未变时当前（最新）版本 ID（created=false 明确告知，绝不静默）；capture() 委托 captureRevision（bool=created，兼容既有调用）；工具栏「记录版本」(Ctrl+S) 用其 ID 显示「已记录新版本 #N」/「内容未变化（当前版本 #N），无需重复保存」，reloadHistory 置顶并选中新版本（时间轴定位）。测试 manualVersionRecordReturnsIdAndUnchangedFlag：导入后内容未变→created=false 返回当前版本 ID 且不新增版本；改内容→created=true 返回新 ID（≠旧）；再次未变→返回同一新 ID 且不重复。

## P-304 稳定变化自动记录（启发式与样本）

- 启发式：连续两次（2 秒间隔）大小+mtime 稳定后读取；**增加读取前后状态复核**（读后复查 mtime/size，变化则重试）与**后台执行**。
- 样本：连续写入、长暂停（>窗口）、临时文件替换（保存=写临时+改名）、同大小写入（mtime 变 size 不变）四类；记录每类处理结果。
- 漏记边界：不改变磁盘签名的写入可能漏掉；写入中暂停超窗口可能误记 → 记录局限，**保留手动入口**（P-303）。
- 不承诺捕获每次保存；文件监视器只作未来加速，不替代。
- 实现（2026-11-01）：自动记录捕获移到后台 TaskQueue 工作线程（大文件不再阻塞 UI 线程），并新增读取前后状态复核——Library::captureIfStable 在读取前记录 (size, mtime)，流式读完后再复查签名，读取期间文件变了则本轮不记录（防半写入撕裂快照），等下轮扫描重试；库公开方法全部以内部 QRecursiveMutex 串行化（后台捕获与 UI 线程自身调用安全并存，递归锁兼容公开方法互调）。UI 侧：2 秒扫描计时器发现「连续两次稳定」后提交后台捕获（每素材一轮 pendingCapture_ 防重复提交），工作线程写结果缓冲（互斥锁），下一轮扫描在 UI 线程排空并按需 reloadHistory/提示错误；同签名只捕获一次（recorded_），内容再变出新高即重捕获。测试 autoRecordStableChangeHandlesSampleClasses：同大小写入（mtime 变 size 不变）→记录、重复保存→去重不重复版本、临时替换（写临时+改名）→新内容落库、连续写入→记录读取时整份内容、工作副本被删→不记录、恢复后未变→无操作。offscreen 端到端冒烟：改工作副本→9 秒内自动记录版本，对象以哈希落库并只读。

## P-305 定时快照与暂停（调度）

- 默认**关闭**；可选周期 15/30/60 分钟；应用运行时生效（非系统常驻服务）。
- 下次时间：按启用时点 + 周期计算；暂停原因记录（用户/系统）；重叠合并：同库上一快照未完成则不叠加。
- 唤醒后只补一次：错过多个周期只补一次（不堆积任务占空间）。
- 退出应用期间不算持续备份（明确说明）。
- 实现（2026-11-01）：调度状态机抽成纯逻辑 src/snapshot_schedule.h（无 UI 依赖，可单测）——默认关闭；setPeriod 下次=启用时点+周期并清暂停；pauseByUser/pauseBySystem 记录原因（系统暂停在应用回到前台时自动恢复，用户暂停只由用户恢复）；due() 仅在启用且未暂停且无进行中且到点；beginRun 置进行中门并**从当前时点重算下次**（错过多个周期只补一次，绝不堆积）；resume 从当前时点重算。MainWindow 右侧面板加「定时快照」块（周期下拉 关闭/15/30/60 分钟、状态标签 未启用/下次 HH:mm/已暂停（原因）/快照进行中、暂停/恢复按钮带原因输入、说明「仅应用运行时生效；退出应用期间不算持续备份。」）；应用退到后台（applicationStateChanged≠Active）自动系统暂停、回前台自动恢复；2 秒扫描 tick 调 checkSnapshotSchedule（due→beginRun→captureQueue_ 后台 snapshotAll("定时快照")→结果经 captureResultsMutex_ 回传，下一 tick 排空→endRun/提示错误），与自动记录共用同一串行队列保证写入不重叠。测试 scheduledSnapshotStateMachineEnforcesSpec：默认关闭永不触发、启用下次=now+周期、到点前不触发/到点触发、开始后置进行中门且下次从当前重算（三周期错过只补一次）、重叠合并不叠加、暂停记录原因且不再触发、恢复从当前重算、系统暂停可区分并恢复、禁用清空。ctest 2/2 绿（37 项）；offscreen 冒烟：调度块正常显示（关闭/未启用/暂停禁用/说明）。

## P-306 版本排除规则

- 规则：路径/模式/扩展名；命中原因记录；清单中排除标记（`excluded:true`）。
- 只影响后续快照，历史保留；**「被排除」≠「已删除」**（防恢复时误删当前文件）。
- 不做：追溯删除旧版本；排除对象默认为删除墓碑。
- 实现（2026-11-01）：匹配抽成纯逻辑 src/exclusion.h（UI 无关可单测）——kind=extension（名字以 .+模式 结尾，大小写不敏感，模式前导点去除）、name（QDir::match 通配文件名）、path（QDir::match 逻辑路径 folder/name，另有裸文件夹前缀匹配以覆盖其整棵子树）；exclusionReason 返回人类可读命中原因，exclusionNormalize 规范化存储。库：exclusions 表（kind,pattern,created，PK(kind,pattern)）+ snapshot_assets 增列 state（present|excluded，未来 P-310/P-311 扩展 tombstone|missing）与 reason；addExclusion（空/重复抛错）/removeExclusion/exclusions；snapshotAll 命中规则的对象登记为 excluded 行（无锚点、不备份、记录命中原因），未命中登记 present；rollbackAll 只回滚 state='present' 行——被排除资产的工作副本绝不触碰（被排除≠已删除）；Snapshot 增 excluded 计数，rollback 界面显示「（N 个被排除）」。UI：右侧面板「排除规则…」按钮开模态对话框（类型下拉 扩展名/路径/名称 + 模式输入 + 添加 + 规则列表 + 删除选中，说明「命中规则的对象不进入后续快照；历史保留，被排除 ≠ 已删除」）。测试 exclusionRulesMatchKindPatternAndNeverDelete（三类匹配+大小写+前导点+裸文件夹子树+不命中+规范化）与 exclusionRulesKeepAssetsOutOfSnapshots（排除对象不入快照且无锚点、present 资产正常锚定、重复/空规则拒绝、回滚跳过被排除资产且其文件未动、删除规则后下一快照重新包含）。ctest 2/2 绿；offscreen 冒烟正常。

## P-307 关键版本固定

- 固定标记 + 原因字段，纳入持久化、备份（随快照/备份包）与恢复预演排除（固定版本不作清理/覆盖对象）。
- 验证重开后保持。
- 不做：取消固定立即删除历史；自动历史清理（V1 保留策略仅预演，见 P-602）。
- 实现（2026-11-01）：revisions 表增列 pinned（INTEGER 默认 0）与 pin_reason（TEXT 默认 ''，老库迁移后旧版本默认未固定）；Revision 结构增 pinned/pinReason；pinRevision(id,reason)（空原因给默认「手动固定」，版本不存在抛错）/unpinRevision(id)（只清标志，绝不删历史）；revisions() 读回标志与原因——固定状态持久化、重开保持。UI：时间轴条目标记「已固定」（当前/已固定前缀），tooltip 显示固定原因；历史列表右键菜单「固定此版本…」（QInputDialog 记录原因）／「取消固定」（提示历史保留）；固定版本随历史/对象保留（恢复形成新版本时对象不被清理/覆盖，且 V1 无自动清理，固定是未来保留策略的豁免标记）。测试 pinnedVersionsSurviveRestartAndRetainObjects：固定后标志+原因正确；改内容再恢复后固定版本与其对象仍在；重开库固定保持；取消固定只清标志、历史与对象不变；未知版本 id 拒绝。ctest 2/2 绿；offscreen 冒烟正常。

## P-308 单素材时间轴（排序与标识）

- 排序：按**确定序列**（数据库单调 id），本地时间只负责展示 → 时钟回拨/时区变化不混乱。
- 标识：当前内容、历史说明、恢复产生的新记录、未记录修改、损坏对象分别标识。
- 验证：同秒多版本、时区变化、未记录内容、损坏对象。
- 不做：把最近选中的历史当当前工作版本；隐藏失败版本状态。
- 实现（2026-11-01）：时间轴保持 revisions() 的 ORDER BY id DESC（数据库单调 id 为确定序列，本地时间仅展示，同秒多版本/时钟回拨不重排）。reloadHistory 计算工作副本内容哈希（1 MiB 分块），与各版本哈希比对得「当前内容」版本 id（内容等于工作副本的最新版本）；工作副本内容 ≠ 最新版本时标「未记录修改」；每行标识：当前内容 / 历史（其余旧版本）/ 恢复（note 前缀「恢复历史版本 #」，restore 系统写入）/ 自动保存（note=「恢复前自动保存」）/ 已固定（P-307）/ 损坏对象（readObject 失败：警告图标 + 标识，绝不隐藏）；说明仍显示 note 文本。不做：把最近选中的历史当当前工作版本（当前内容按内容哈希判定，不按选中）。测试 timelineOrdersByRevisionIdAndListsDamagedObject：同秒多版本按 id 严格降序、最新版本=当前工作内容、损坏对象仍列在时间轴（readObject 抛错但 revisions 完整、顺序不变、健康对象仍可校验）。ctest 2/2 绿；offscreen 冒烟正常。

## P-309 元数据历史（逐字段只读回看）

- 逐字段保留旧名称、标签、评分、备注、来源；标注变更时间；先只读回看。
- 统一字段历史与内容版本的时间关系：字段变更记录与修订序列一致（新修订含 metadata 变化，`contentHash` 不变）。
- **明确取消 V1 承诺**：多人合并、单字段任意自动回退（不做）；整资源恢复另按预演处理（P-315/P-317）。
- 不做：编造旧值。
- 实现（2026-11-01）：field_history 表（id,asset_id,revision_id,field,old_value,new_value,changed，追加只读）+ FieldChange 结构；recordFieldChange(assetId,field,old,new)（old==new 时 no-op）——每次字段变更追加一个**仅元数据修订**（复用当前内容修订的 hash/size，不写新对象，note=「元数据变更：字段」，使字段历史与修订序列对齐）并插入 field_history 行（revision_id=该修订 id）；fieldHistory(assetId) 按 id 降序只读返回。接入所有字段变更点：renameAsset（名称）、moveAsset（目录）、addTag/removeTag 及批量（标签，仅实际变更）、setRating（评分，先读旧值）、setNote（备注，先读旧值）、addProvenance（来源/作者/许可，旧值=上一条记录）。UI：右侧「字段历史…」按钮 → 模态只读对话框（时间 · 字段：旧 → 新 · 版本 #N，空值显示（空），说明「字段变更与版本序列一致，内容不变」）；评分/备注/标签/来源保存后 reloadHistory 使新修订即时入时间轴。不做：无逐字段回退 API；不编造旧值。测试 fieldHistoryRecordsOldValuesAlignedWithRevisions：标签/评分/备注各生成一个仅元数据修订（hash 不变、对象数=1）、field_history 行 old→new 正确、revision_id 指向真实修订、note 以「元数据变更」开头、no-op 不记录、重命名记录真实旧名新名、内容未被元数据变更改动。既有 renameAndMoveKeepAssetIdentity 修订计数按新语义更新（import+字段变更修订数）。ctest 2/2 绿；offscreen 冒烟正常。

## P-310 删除墓碑（三状态）

| 状态 | 触发 | 处理 |
|---|---|---|
| 应用内确认删除 | 用户在应用删除（预演→确认） | 写墓碑修订 `tombstone:true`，历史/对象保留 |
| 工作副本缺失 | 库内文件不存在（误删/外部移除） | 标记「工作副本缺失」，可恢复；**不视为用户删除** |
| 存储不可达 | 库卷/挂载源不可达 | 明确「不可达」，不写墓碑；拔盘/断网 ≠ 删除 |

实现（2026-11-01）：

- **数据**：`assets.deleted`（0/1）标记应用内确认删除；`revisions.tombstone`（0/1）标记墓碑修订。老库缺列自动补（默认 0）。
- **deleteAsset(id)**：未知 id / 已删除 → 抛错；工作副本存在但无法删除（占用/只读挂载）→ 抛错不半删；工作副本已不存在（外部移除）仍写墓碑。墓碑修订复用最后内容的 hash/size（note=“删除”），因此时间线仍可浏览、对象仍可读、历史可恢复。随后 `UPDATE assets SET deleted=1`。只由用户确认触发，**永不自动写**。
- **assets()**：`WHERE deleted=0` —— 删除的素材不再出现在实时清单；其行、标签、元数据、来源、版本历史全部保留（历史查询 `revisions(id)` 仍可找到原素材）。
- **workingCopyHealth(asset)**：区分三种工作副本状态 —— `Healthy`（可读）；`Missing`（文件不存在=外部移除，可恢复，不视为删除）；`Unreachable`（目录不可读或文件不可读=拔盘/断网/权限，明确「不可达」，不写墓碑）。只读探测，绝不改写目录。
- **UI**：素材列表右键「删除素材…」→ 预演对话框（名称/路径/版本数/工作副本将被移除、历史与对象保留）→「确认删除/取消」。删除后实时清单移除、详情与时间线清空、状态栏提示「历史与对象保留，可从时间线恢复」。
- **时间线标记**：墓碑修订显示「墓碑」标记；工作副本缺失/不可达在详情面板显示对应状态行（区别于删除，且明确“不算删除”）。
- **快照**：`assets()` 过滤删除素材后，后续快照自然不含已删除素材；已删除素材的旧快照条目保留（引用仍在）。
- **修复**：`snapshots()` 的 present 计数改为 NULL 安全（`sa.state IS NOT NULL AND sa.state!='excluded'`）——空快照（LEFT JOIN 无匹配行）曾把 NULL 行误计为 1 个 present。
- **测试**：`deletionTombstoneRetainsHistoryAndHidesLiveAsset`（删除→实时列表移除/历史 3 版且最新为墓碑/对象可读/工作副本移除/重复删除抛错/重启后墓碑持久/后续快照 0 素材）；`workingCopyHealthDistinguishesMissingFromUnreachable`（Healthy→外部移除=Missing 无墓碑→恢复可读=Healthy→只写无读=Unreachable 无墓碑）。ctest 2/2 绿；offscreen 冒烟通过；翻译 225/225。

## P-311 整库一致快照（V1 门槛）

- 基于已完成修订一次发布完整清单：快照边界=某序列点；等待正在写入文件（重试/跳过+标记）或失败策略；**不完整候选不发布为成功快照**。
- 内容引用与清单一致性：清单含 assetId/path/contentHash/state（present|tombstone|excluded|missing）；对象按清单校验。
- 不做：逐文件扫完就叫一致；直接复制运行中的数据库当快照；宣称外部编辑器同时冻结。

实现（2026-11-01）：

- **清单数据**：`snapshot_assets` 增加 `folder`/`name`（快照时刻路径，后续改名/移动/删除不回写）与 `hash`（引用修订的内容哈希）。老行默认空串，读取时回退到素材当前记录位置（`snapshotEntries`）。
- **发布语义**：快照边界=修订序列点。发布前先对每个现有工作副本做有界等待（仅最近 3 秒内修改过的文件进入等待循环，≤3×25ms）；仍未稳定→**候选不完整，整体失败**，绝不发布为成功快照。事务内读取后再 stat 一次（torn-read 守卫），写入未稳定→同样失败回滚。因此清单只引用**已完成修订**。
- **清单状态**：`present`（锚定最近干净修订）、`tombstone`（应用内删除，对象保留）、`excluded`（规则命中，无内容引用）、`missing`（工作副本缺失，可恢复，不视为删除）。
- **对象校验**：每条含 hash 的清单项在提交前校验对象存在且大小匹配；失败→候选不完整，回滚不发布。
- **已删除素材**：整树清单同样收录为 tombstone 项（库状态含“此处删除、历史保留”）；`assets()` 过滤后实时清单不含它们，但旧快照与历史仍可查到。
- **rollbackAll**：恢复 `present` **和** `missing`（缺失工作副本可从锚点恢复，正是“可恢复”语义）；`excluded`/`tombstone` 跳过。
- **snapshots() 计数**：`assets`=present+missing（锚定且可恢复），`excluded`=excluded，按精确状态求和（LEFT JOIN 空快照不误计）。
- **修复**：present 计数曾把 `missing` 计入（因 NULL-safe 通配）——现按状态精确求和。
- **测试**：`consistentSnapshotManifestFreezesPathsAndStates`（present/excluded/missing/tombstone 四态、路径冻结于快照时刻、改名/移动后清单不变、对象按清单校验、rollback 恢复 present+missing）；`incompleteSnapshotCandidateIsNotPublished`（并发写入→快照抛错且不发布）。ctest 2/2 绿；offscreen 冒烟通过；翻译 228/228。

## P-312 时间点目录浏览（只读）

- 按快照清单只读显示当时目录、名称、删除与排除状态；不混用当前索引。
- 验证：改名、移动、删除后的同一资源，历史目录 vs 当前目录差异正确（见 10-prototype#P-009 两快照例子）。
- 不做：浏览时写回；显示不存在的完整性承诺。

实现（2026-11-01）：

- **数据层**：P-311 的 `snapshotEntries(snapshotId)` 已冻结快照时刻的 folder/name/hash/state，直接作为浏览数据源（无需新列）。
- **UI**：工具栏「浏览快照…」→ 选择快照 → 只读对话框，按清单（folder/name）排序列出每条：`present` 正常；`tombstone` 显示「已删除（历史保留）」；`missing` 显示「工作副本缺失（可恢复）」；`excluded` 显示「已排除：<原因>」。空清单显示「（此快照没有条目）」。纯只读，无任何写回；不宣称完整性承诺。
- **不混用当前索引**：路径与状态完全取自清单；实时素材列表（改名/移动/删除后）不影响历史视图。
- **测试**：`snapshotBrowsingShowsPathsPerBoundary`（P-009 两快照例子）——初始快照=根目录/a.png；改名+移动后快照=scenes/hero.png；删除后快照=tombstone（路径仍为删除前的 scenes/hero.png）；当前实时列表为空，历史与当前不混用。ctest 2/2 绿；offscreen 冒烟通过；翻译 238/238。

## P-313 双版并排与同步缩放（对齐规则）

- 统一图像坐标；并排与滑动共用同一渲染规则。
- 尺寸不同：默认左上对齐 + 显式说明；不自动拉伸；可选「适配窗口」统一比例。
- 同步平移：同步缩放比例**且**同步原点；空白区域含义明确（一侧无内容区域）。
- 不做：自动拉伸两图到同尺寸；文本/源文件差异。

实现（2026-11-01）：

- **CompareCanvas（src/compare_canvas.h/.cpp）**：双窗格共用**一个**图像坐标变换——同一缩放比例 + 同一原点（`origin_` 单值），平移/缩放永远同时驱动两侧，不可能失同步。并排与后续滑动（P-314）共用该渲染规则。
- **默认**：scale=1、origin=(0,0)——两图左上角对齐，绝不拉伸；适配模式计算**统一**比例（两侧各自适配比例取最小），小图一侧留下明确空白（无内容区域）。
- **缩放**：滚轮以光标下的图像点为锚缩放（两侧同原点联动），范围 0.05–16×；拖拽平移同一原点；平移钳制保证不把图像拖出视野。
- **UI**：右栏时间轴下方「对比版本…」按钮——左侧=最新版本（当前内容），右侧=所选历史版本；栏含缩放百分比、「适配窗口（统一比例）」开关；两版尺寸不同时显示显式说明（尺寸、左上对齐、不拉伸、空白含义）；某侧解码失败显示「此版本不可预览」（另一侧正常）。
- **不做**：不自动拉伸到同尺寸；不做文本/源文件差异。
- **测试**：`compareCanvasSharesOneTransform`（统一适配比例=两窗格取小、原点初始 (0,0)、手动缩放禁用适配且有界、滚轮锚点缩放改变缩放并保持共享原点、拖拽平移单一共享原点、无图像时交互惰性）。测试主入口改为 QApplication（CompareCanvas 是 widget）。ctest 2/2 绿；offscreen 冒烟通过；翻译 245/245。

## P-314 滑动对比（交互验收）

- 分界线：鼠标拖动、左右键步进（`[ / ]` 调步进）、缩放后同坐标。
- 验证：一侧加载失败（显示该侧不可预览，另一侧正常）；不叠加变形隐藏尺寸差。
- 不做：闪烁/图层差异模式。

实现（2026-11-01）：

- **CompareCanvas.setSliding(true)**：单视口分界线擦拭模式，复用 P-313 的共享变换（同缩放 + 同原点）——擦拭线在两侧对应**同一图像坐标**，尺寸差如实显示（一侧无内容即空白，不拉伸、不叠加、不隐藏）。
- **分界线**：存为**图像坐标**（`dividerX_`），缩放后仍标记同一内容位置；视图坐标 = pane.left + (dividerX-originX)×scale。鼠标在分界线 ±6px 内按下即拖动，否则平移；`Left/Right` 步进（步长默认 40px 视口），`[ / ]` 缩放步长（4–400px）。
- **验证**：某侧解码失败 → 该侧显示「此版本不可预览」，另一侧正常；两图尺寸差 → 不叠加变形、空白含义明确。
- **UI**：对比对话框加「滑动对比」开关（与并排共用变换）与键盘提示行。
- **不做**：闪烁/图层差异模式。
- **测试**：`slidingDividerComparesSameCoordinate`（滑动模式统一适配=全视口适配、分界线图像坐标在缩放后不变而视图坐标变、Left/Right 步进、`[` 减半步长、分界线附近拖拽跟光标、一侧空图仍可渲染不崩溃）。ctest 2/2 绿；offscreen 冒烟通过；翻译 247/247。

## P-315 恢复预演（计划绑定）

- 预演先于任何目标文件写入，生成具体操作清单（新增/覆盖/跳过/冲突 + 空间需求）。
- 计划绑定目标版本和当前文件状态；**执行前重新检查**，变化则要求刷新预演（防旧计划失效）。
- 不做：预演阶段修改工作文件；静默解决冲突。

**实现（2026-11-01）**
- 设计：`Library::RestorePlan`（action = overwrite|create|skip|conflict；protectBytes/targetBytes/spaceNeeded；conflictReason；plannedSize/plannedMtime 为计划时的绑定）。`planRestore(asset, revisionId)` 只读预演，`restore(asset, plan)` 执行（执行前重查绑定）。
- 预演判定：工作副本缺失→create（无内容可保护，UI 需确认）；内容==目标→skip；目标对象缺失/损坏→conflict；工作副本不可读→conflict（P-316 不可无保护覆盖）；其余→overwrite。
- 绑定：plannedSize<0 表示计划时缺失；执行时文件出现/消失/尺寸或 mtime 变化→「恢复计划已失效，请刷新预演」。
- 接线：MainWindow::restore 先 planRestore 弹「恢复预演」清单（动作+空间需求+保护说明），确认后才执行；skip/conflict 只提示不执行。
- 测试：`restorePreviewListsActionsWithoutWriting`、`staleRestorePlanIsRejected`、`plannedRestoreProtectsCurrentContentBeforeOverwrite`。
- 验证：ctest 2/2 绿；离线烟雾运行无崩溃。

## P-316 恢复前保护当前状态（阻断条件）

- 保护当前未记录内容成功后才允许恢复；保护失败即停止（不无保护覆盖）。
- 定义：保护版本（生成新修订保存当前）、空间预估、缺失工作副本（无内容可保护但需确认）、外部编辑并发（检测到外部写则阻断，提示关闭编辑器）。
- 不宣称保存了编辑器未写盘内容。

**实现（2026-11-01）**
- 保护前置：`restore(asset, plan)` 在写目标前先 append「恢复前自动保存」（若当前内容未记录）；保护 append 失败则整个事务回滚、工作文件不写——绝不无保护覆盖。
- 空间预估：preview 的 spaceNeeded = protectBytes + targetBytes，UI 以 KiB 展示。
- 缺失工作副本：plan 动作 create，UI 明确「无内容可保护，请确认」，非静默。
- 外部编辑并发：读取工作副本后、写目标前再次校验 (size, mtime)；变化→「检测到外部编辑正在写入，请关闭外部编辑器后重试」，阻断并回滚。保护仅覆盖磁盘已保存内容，界面文案不宣称保存编辑器未写盘内容。
- 测试：`plannedRestoreProtectsCurrentContentBeforeOverwrite`（保护版本可导出）；外部写并发为写前校验，属防御性检查。
- 验证：ctest 2/2 绿；离线烟雾运行无崩溃。

## P-317 单素材恢复为新版本

- 恢复创建**新修订**，原历史全部保留。
- 验证：A→B→恢复 A 产生新 C；B 仍可浏览/导出；恢复结果可再次查到。
- 不做：删除后续历史；恢复=回滚并抹除记录。

**实现（2026-11-01）**
- 设计：`restore()` 走 P-315 预演后执行——先 append「恢复前自动保存」（若未记录），再 append「恢复历史版本 #id」新修订；绝不删除或改写任何既有修订。
- 验证：A→B→恢复 A 产生新 C；B 保留且可导出；C 可再次查到（note = 恢复历史版本 #A）。测试 `restoreCreatesNewRevisionPreservingAllHistory`。
- 验证：ctest 2/2 绿；离线烟雾运行无崩溃。

## P-318 快照内选中项恢复

- 限定为预演确认的资源及元数据；逐项结果（成功/失败/跳过）、冲突处理、可重启收敛。
- 原子性能力不足 → 明确逐项完成状态（不宣称跨文件系统整体原子操作）。
- 不做：默认全库镜像覆盖；删除快照之外新素材。

**实现（2026-11-01）**
- 设计：`planSnapshotRestore(snapshotId, assetIds)` 逐项预演（每项绑定快照锚点 revision，走 P-315 planRestore）；`restoreFromSnapshot(snapshotId, items)` 只执行预演确认项，逐项 try/catch 产出 success|failed|skipped（单项失败不中断批次，不宣称整体原子）。
- 只处理预演确认项：excluded/tombstone/不在快照/素材不存在 → 逐项 skip 带原因（绝不静默恢复、绝不复活已删除素材、绝不动快照之后新建的素材）。
- 接线：快照浏览对话框（P-312）加「恢复选中项…」：选中 present|missing 条目 → 预演清单（动作+空间需求+保护说明）→ 确认 → 逐项结果对话框。
- 测试：`snapshotSelectiveRestoreTouchesOnlyReviewedItems`（a/c 恢复、b 与快照后新增 d 不动、tombstone 条目 skip）。
- 验证：ctest 2/2 绿；离线烟雾运行无崩溃。

## P-319 历史副本导出

- 校验（版本归属+对象哈希）后输出到库外；复用对象读取校验。
- 检查：目标真实路径（拒绝指向库内对象/当前工作文件）、符号链接、已存在文件（覆盖需明确确认）、磁盘满。
- 不做：导出覆盖库内部数据；修改后端或自动提交仓库。

**实现（2026-11-01）**
- 设计：`exportRevision(assetId, revisionId, destination, overwrite=false)`——版本归属校验（找不到历史版本→拒绝）+ readObject 对象哈希校验；目标父目录 canonicalPath 必须在库外（拒绝 objects/ 与工作副本）；目标为符号链接→拒绝；已存在且 !overwrite→拒绝（UI 先确认）。
- 磁盘满：QSaveFile 原子写，写失败/提交失败→抛错，不半写。
- 后端/工作区零改动：导出为只读路径，无任何修订/对象/工作文件写入。
- 接线：导出对话框：目标已存在→明确「覆盖？」确认后才 overwrite=true。
- 测试：`exportRevisionValidatesPathAndRefusesUnconfirmedOverwrite`（库外导出成功且后端/工作区不变、未确认覆盖拒绝、库内路径拒绝、符号链接拒绝）。
- 验证：ctest 2/2 绿；离线烟雾运行无崩溃。

## P-320 恢复日志与重启收敛（硬门槛）

**阶段列表**（各阶段中断验证再次启动）：

| 阶段 | 动作 | 中断后重开 |
|---|---|---|
| 1 保护 | 写保护版本（当前内容） | 若未完成，重开可重试或放弃（无部分恢复） |
| 2 写日志 | 记录恢复计划（目标版本、逐项状态） | 恢复未开始，可安全取消 |
| 3 替换文件 | 逐项原子替换工作文件 | 已替换项标记完成；未替换继续；日志保证收敛 |
| 4 提交索引 | 数据库提交（事务） | 提交失败则回滚索引并恢复工作文件；断电由日志收敛 |

- SQLite 事务与文件替换非同一事务；普通异常回滚 ≠ 断电安全 → 以日志+重开收敛为准。
- 不发布隐藏半恢复状态的实现；不把「异常回滚」当「断电安全」。

**实现（2026-11-01）**
- 设计：`restore_log` 表（asset_id/revision_id/state/previous_hash/target_hash/protect_revision_id/error/时间戳）。restore 分四个持久阶段：①保护版本独立提交（失败→什么都不写）→ ②日志行「planned」独立提交（先于任何文件改动）→ ③QSaveFile 原子替换工作文件（失败→日志标 failed，文件未动）→ ④append 恢复修订+日志标 done 单事务提交（失败→回滚+恢复原文件+日志标 failed）。
- 断电安全：阶段 3 与 4 之间断电时文件已替换而索引未提交——重开 `Library` 构造尾部 `convergeRestoreLog()`：工作文件哈希==目标哈希→向前收敛（补 append 恢复修订+标 done）；==previous_hash→向后收敛（标 aborted，文件不动）；都不是→外部改动，标 conflict 不覆盖。素材记录/目标版本不存在或已删除→aborted（不自动复活）。
- 不隐藏：`restoreLogs()` 只读返回日志行（planned|done|failed|aborted|conflict），可查每条恢复的最终状态。
- 保护阶段独立提交的语义变化：restore 在文件替换阶段失败时，保护版本（恢复前自动保存）保留为真实版本——内容无丢失、无半恢复；旧「全部回滚」测试已按此语义更新。
- 测试：`interruptedRestoreConvergesForwardOnReopen`（直接写入 planned 日志行模拟断电窗口，重开后补齐恢复修订、日志 done）、`interruptedRestoreConvergesBackwardOnReopen`（文件未替换→aborted、文件不动）、`restoreRollsBackPreservationOnTransactionFailure`（更新：失败恢复保留保护版本、工作文件不变）。
- 验证：ctest 2/2 绿；离线烟雾运行无崩溃。
