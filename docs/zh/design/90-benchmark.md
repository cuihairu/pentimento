# 对标产品调研（2026-10-31 落库）

> 状态：**调研落库**；「可借鉴做法」16 条复核见文末第六节，14 条功能缺口逐条复核登记于 todo.md「对标缺口复核」节。调研对象：六家产品 + 系统级备份。所有定价均为调研时公开页可见内容，**以官网为准**。无法取证的内容明确标注「未找到公开资料（待核）」。
>
> pentimento 基线（来自 PRODUCT.md / docs/zh/requirements.md R01–R19）：素材库浏览（网格/列表、搜索筛选）、管理式复制导入、内容寻址 append-only 版本库（SHA-256）、单素材时间轴、恢复形成新版本不销毁历史、双版对比（并排/滑动）、防勒索三防线、Git/SVN 只读后端、NAS 挂载/WebDAV/SFTP 只读、备份一致性检查。

---

## 一、逐家功能点详表

### 1. Alienbrain（alienbrain.com）— 游戏美术 VCS 老牌，在营（当前可下载版本 Alienbrain 2022）

定位：**集中式 client/server 美术版本控制系统**，服务端跑在 Windows Server，客户端 Windows 为主（另有 Mac/Linux 跨平台客户端）。面向团队。

| 功能点 | 有无 | 说明 |
|---|---|---|
| 浏览 | 有 | 文件以高质量缩略图与预览呈现；数百种文件格式可预览（含 UE、AI 格式，无需安装对应软件） |
| 导入 | 有 | 客户端/资源管理器 Shell 集成，add 文件入库；无素材库式「收集」语义（是 VCS 而非素材库） |
| 标签搜索 | 部分 | 快速搜索 + 高级搜索（通配符、正则）；可搜版本历史与 change set。**无 Eagle 式用户标签体系** |
| 预览缩略图 | 有 | 版本历史本身也可按缩略图浏览（"browse a file's version history using thumbnails"） |
| 版本历史 | 有 | 每文件完整 revision 树，check-in/check-out 提交注释 |
| 回滚 | 有 | 单文件或**整文件夹递归回滚**到任意历史版本；**回滚非破坏**：回滚版本成为 revision 树新 tip，旧版本仍可访问 |
| 对比 | 有 | 支持格式的图像文件**多版本并排可视化对比**；程序员端集成 Araxis Merge |
| 协作锁 | 有 | 集中式排他 checkout/lock，"only one user can work on a file at a time"；多 checkout 可配置；管理员可禁止删除文件 |
| 集成 | 有 | 3ds Max / Maya / Photoshop / Unreal（4.27+ 插件：编辑器内 checkout、自动建版本）/ Visual Studio |
| 自动化 | 部分 | 无公开脚本 API 细节（待核）；有 change set 与状态工作流（WIP→Sign off→Approved） |
| 安全防篡改 | 部分 | 集中存储+权限管理（组权限可禁删除）；**无防勒索检测**相关公开资料（待核） |
| 备份恢复 | 部分 | 服务器存储集中管理；无内建备份调度公开资料（待核） |
| 审阅协作 | 有（2019 起） | 非破坏标注层、任务分配、进度跟踪、集成消息（仅 Advanced 档） |

**版本历史机制**：集中服务器保存主副本；check-in 形成新 revision，revision 树式组织；非内容寻址公开说明（待核，业界普遍认为按 revision 存储大文件副本）。回滚不销毁历史。

**定价**（https://alienbrain.com/pricing.php ，订阅制按用户/年）：
- Coders：$150/用户/年（版本控制、VS 集成、分支、change set）
- Artists：$250/用户/年（+缩略图、图像预览、UE/PS/Max/Maya 集成）
- Advanced：$400/用户/年（+任务、进度、报表）
- Reader：随订阅免费（只读）
- 亦提供永久授权；无免费版。**面向团队**（个人单机无意义，需服务器）。

来源：https://alienbrain.com ；https://alienbrain.com/features ；https://alienbrain.com/pricing.php ；https://cgchannel.com/2019/08/alienbrain-ships-alienbrain-2019 ；https://gamedeveloper.com/programming/product-review-nxn-s-alienbrain-5 （回滚非破坏、并排对比原文出处）

---

### 2. Anchorpoint（anchorpoint.app）— 艺术家友好 Git LFS 客户端，在营

定位：**给非程序员（美术/设计师）用的 Git + LFS 桌面客户端**，外加云元数据层（锁、标签），Windows/macOS。面向小团队与个人。

| 功能点 | 有无 | 说明 |
|---|---|---|
| 浏览 | 有 | 文件树 + 缩略图（PSD/FBX/GLTF/OBJ/EXR/BLEND/UASSET/视频音频等）；psd/fbx 视觉缩略图是与 GitHub Desktop 对比的核心卖点 |
| 导入 | 有 | 本地文件夹建仓、模板系统；支持从浏览器/聊天工具拖图 |
| 标签搜索 | 有 | 标签、属性（attributes）、AI 辅助打标签与注释 |
| 预览缩略图 | 有 | 大量美术格式；大仓按需加载缩略图（selective checkout） |
| 版本历史 | 有 | Git 提交历史；每文件内建 history 视图；Unreal 插件内可看单文件历史、Blueprint diff |
| 回滚 | 有 | revert / 版本恢复（Git 语义，历史不销毁） |
| 对比 | 部分 | Unreal 内 Blueprint diff；**图像双版并排对比未见公开资料（待核）** |
| 协作锁 | 有 | **特色功能**：云元数据层实现即时锁（≤1000 文件 <1s），被锁文件置只读，绕开 Git LFS 锁的局限；UE/Unity 插件内可见锁状态 |
| 集成 | 有 | Unreal（官方 Revision Control 插件）、Unity、Godot；GitHub/GitLab/Azure DevOps/Gitea/自建 |
| 自动化 | 有 | Python API 与 actions（发布流、图片序列转视频等）；Auto Fetch 每分钟检查远端 |
| 安全防篡改 | 无 | 未找到公开资料（待核）；云元数据不碰生产数据 |
| 备份恢复 | 部分 | Git 远端即异地备份；本地仓可用；无防勒索语义（待核） |
| 特色 | — | 增量文件版本（NAS/共享盘上 v001/v002 + 元数据管理，不需要完整 Git）；审阅（评论、审批流）；TB 级大仓优化、自动 LFS 配置与缓存清理 |

**版本历史机制**：Git 内容寻址 + LFS 大文件；提交 = 快照锚点；revert 不销毁历史。锁/标签是云元数据，**不进 Git**。

**定价**（来源 https://fab.com/listings/c37f3c5e-70c6-4ea0-bb18-b1b367ec443c ；官网免费层说明 https://www.anchorpoint.app ）：免费版可推拉 GitHub + 本地版本控制；Pro €20/用户/月；独立开发者 5 折。**以官网为准**。

来源：https://anchorpoint.app/compare/alternative-to-github-desktop ；https://anchorpoint.app/policies/facts ；https://docs.anchorpoint.app/docs/asset-management/incremental-file-versioning/ ；https://forums.unrealengine.com/t/2567882

---

### 3. Perforce Helix Core / P4（perforce.com）— 行业标准团队 VCS

定位：**集中式超大仓版本控制**（19/20 AAA 工作室），配合 Helix DAM（美术资产库）与 Helix Plan/Swarm。面向团队（免费层可用到 5 人）。

| 功能点 | 有无 | 说明 |
|---|---|---|
| 浏览 | 部分 | P4V 可视化客户端浏览 depot；**美术向浏览靠 Helix DAM**（视觉资产库） |
| 导入 | 有 | add/sync 工作区模型；P4 One 本地版本化 |
| 标签搜索 | 部分 | Helix DAM 有 AI 搜索与标签；Core 本身是 depot 路径模型，无素材标签 |
| 预览缩略图 | 有（DAM） | DAM 支持 2D/3D 预览、可旋转 3D 模型 |
| 版本历史 | 有 | 全资产不可变历史；P4V 图形化变更历史；完整审计日志 |
| 回滚 | 有 | 回退到任意 revision；历史不可变、回退不销毁 |
| 对比 | 有 | P4V / P4 Merge **文本与二进制可视化 diff** |
| 协作锁 | 有 | **行业标杆**：排他 checkout、全局锁可见性、文件级粒度权限 |
| 集成 | 有 | UE/Unity/Photoshop/3ds Max/Maya/Jira/IDE/CI；大量插件与 API |
| 自动化 | 有 | CLI、API、云部署模板、并行 sync/submit |
| 安全防篡改 | 有（安全向） | 文件级+IP 级权限、SAML/OIDC SSO、端到端加密、审计；**无防勒索检测语义**（待核） |
| 备份恢复 | 部分 | 云托管版自动更新与备份（Helix Core Cloud）；S3 冷归档；自建需自行备份 |

**版本历史机制**：集中服务器、按 revision 存储 + 智能增量传输（delta sync/submit）；changelist 原子提交；历史不可变（"securely stores and tracks every iteration, with immutable history"）。回滚不销毁历史。

**定价**（https://www.perforce.com/products/p4/pricing ）：**5 用户 + 20 工作区以内免费**（全功能、自托管、存储无限制；超限限 1000 文件）；Helix Core Cloud $39/用户/月（≤100 人，64GiB 含）；Scale/Platform 档报价制；Helix DAM 为附加购买。**以官网为准**。

来源：https://www.perforce.com/products/p4/pricing ；https://www.perforce.com/solutions/game-development ；https://www.perforce.com/blog/p4/p4-cloud-managed-hosted-version-control ；https://www.perforce.com/sites/default/files/pdfs/pf-core-product-brief-web.pdf

---

### 4. Eagle（cn.eagle.cool）— pentimento 的 UX 参考基准

定位：**个人本地素材库**（灵感收集/整理/检索），Windows/macOS。**无版本历史、无协作**。

| 功能点 | 有无 | 说明 |
|---|---|---|
| 浏览 | **有（行业最佳）** | 网格/列表、多层级文件夹、智能文件夹（按名称/标签/颜色/格式等条件自动聚合）、随机排序、置顶；文件夹即时计数与多选统计 |
| 导入 | **有（行业最佳）** | 拖放、浏览器扩展（YouTube/Pinterest/ArtStation 等网页采集）、剪贴板粘贴、截图；重复导入自动识别拦截询问 |
| 标签搜索 | **有（行业最佳）** | 标签（批量建/管理/排序）、自动标签（文件夹级）、评分、标注（图上矩形批注）、颜色搜索、横竖图/尺寸筛选、0.5 秒检索、AI 搜索（4.x）与 AI Action/Eagle MCP（5.0 预告） |
| 预览缩略图 | **有** | 官方称 Windows 100+ / macOS 109 种格式；视频逐帧浏览、音频悬停播放、字体免安装预览、GIF 播放器、空格全屏预览 |
| 版本历史 | **无** | 素材无版本历史（删除进回收站）；编辑类操作（裁切）可「另存新文件」不覆盖原文件 |
| 回滚 | **无** | — |
| 对比 | **无** | 未找到双版图像对比功能公开资料（待核） |
| 协作锁 | **无** | 无团队功能；官方建议配第三方同步网盘 |
| 集成 | 部分 | 浏览器扩展、eagle:// 链接协议；无 DCC/引擎集成 |
| 自动化 | 部分 | 「动作」自定义整理流程；MCP/AI 插件生态（社区+官方 Beta） |
| 安全防篡改 | **无** | 库文件夹可设密码（隐藏级别）；**无防勒索** |
| 备份恢复 | 弱 | 无内建备份；.eaglepack 导出素材包可作人工备份载体；用户实践为手动整库拷贝 |

**版本历史机制**：无。库为「管理式复制导入」的文件夹结构（.library 元数据），与 pentimento 当前实现同构。

**定价**（来源 https://en.eagle.cool/blog/post/digital-asset-management-for-video ："$29.95 for a lifetime license"；5.0 预告 https://cn.eagle.cool/blog/post/eagle5-teaser 明确「终身买断、已购免费升级」）：一次性买断约 $29.95，30 天全功能试用，已购免费升级。**以官网为准**。**面向个人**。

来源：https://cn.eagle.cool/ ；https://eagle.cool/support/desktop/organize ；https://cn.eagle.cool/blog/post/eagle-3.0 ；https://cn.eagle.cool/blog/post/adobe-bridge-and-lightroom-alternative （重复拦截、eaglepack 备份实践）

---

### 5. Billfish（billfish.cn）— 国产免费素材库

定位：**对标 Eagle 的免费素材管理器**（苏州长嘴鱼软件），Windows/macOS。库结构与电脑文件夹对应（与 Eagle 的差异点）。面向个人（个人免费承诺），企业服务另算。

| 功能点 | 有无 | 说明 |
|---|---|---|
| 浏览 | 有 | 文件夹树+标签+评分+备注；瀑布流预览；颜色值提取与颜色筛选；鸟瞰图；界面与 Eagle 高度相似 |
| 导入 | 有 | 导入文件/文件夹、拖放、资源管理器复制后 Ctrl+V、浏览器扩展；**可直接导入 Eagle 资源库**；61+ 格式 |
| 标签搜索 | 有 | 标签（层级≤5、总量≤2000）、备注、评分、形状/导入日期/类型/尺寸筛选、以图搜图、图片内容搜索 |
| 预览缩略图 | 有 | 常见图片/视频/音频；长图裁剪预览策略（高宽比>8 截断）等细节；特殊格式（AE 模板、字体预览）弱于 Eagle |
| 版本历史 | **无** | 未找到版本历史功能公开资料（待核） |
| 回滚 | **无** | — |
| 对比 | **无** | — |
| 协作锁 | **无** | 无 |
| 集成 | 部分 | 浏览器扩展；无 DCC/引擎集成 |
| 自动化 | 部分 | 插件版本持续迭代；智能识图规划中（官网路线图，2022 年资料） |
| 安全防篡改 | **无** | — |
| 备份恢复 | 部分 | 可选云资源库：加密备份、多设备访问、密码保护分享链接；本地库无内建版本备份 |

**版本历史机制**：无。库内文件与磁盘文件夹结构一一对应（这是与 Eagle「管理式复制」的关键差异，见少数派对比文）。

**定价**：个人用户完全免费（官方承诺不强制收费）；收入来自企业与定制服务（来源 https://titikey.com/en/article/tivpbf-kpq28 、https://sspai.com/post/76222 ）。**以官网为准**。

来源：https://www.billfish.cn/ ；https://billfish.cn/help/shiyongjianyijipeizhi （库容量/标签限额参数）；https://sspai.com/post/76222 ；https://edgedrop.app/blog/eagle-billfish-and-asset-managers-vs-clip-history

---

### 6. Adobe Bridge + Lightroom（adobe.com）

定位：Bridge = 免费文件浏览器（无数据库）；Lightroom Classic = 目录（catalog）式照片管理+非破坏编辑。均面向个人。

**Bridge**：

| 功能点 | 有无 | 说明 |
|---|---|---|
| 浏览 | 有 | 缩略图/列表/幻灯；文件夹书签；缩略图缓存加速 |
| 导入 | 有 | 直接浏览磁盘文件夹（非复制导入；**无库概念**） |
| 标签搜索 | 有 | 星级、颜色标签、关键字、XMP/IPTC 元数据编辑与模板、批量重命名、堆栈（stacks）、集合（collections，含智能集合） |
| 预览缩略图 | 有 | PSD/AI/INDD/PDF/raw/视频/音频免打开预览 |
| 版本历史 | **无** | 自身无版本历史（历史仅存在于 Version Cue 遗留场景与 Camera Raw 设置） |
| 回滚 | **无** | — |
| 对比 | **无** | — |
| 协作锁 | **无** | — |
| 集成 | 有 | Photoshop/Illustrator/InDesign/Camera Raw 无缝交接 |
| 自动化 | 部分 | 批量重命名、元数据模板、输出 contact sheet |
| 安全防篡改 | **无** | — |
| 备份恢复 | **无** | 缓存可清即丢；无库无备份；断连磁盘不可浏览 |

**已知弱点（对 pentimento 有警示价值）**：无数据库→离线盘不可浏览搜索；NAS/网络卷性能差，大文件夹缩略图缓存需定期清理（来源 https://softwr.com/software/adobe-bridge ）。

**Lightroom Classic**：
- **非破坏编辑**：所有调整存 catalog 不动原文件字节（来源 https://dpmag.com/how-to/tip-of-the-week/bridge-or-lightroom-which-one-should-i-use ）。
- **编辑级历史**：History 面板逐步记录每次调整，可跳回任意步骤；快照（snapshots）与虚拟副本（virtual copies）派生多版本——**这是「编辑历史」而非「文件字节版本历史」**。
- **Catalog 定时备份**：可配置退出时自动备份 catalog（仅元数据库，不含原图）。
- 定价：Lightroom 1TB 计划 $11.99/月；Photography 计划（Lr + LrC + PS，1TB）$19.99/月（来源 https://josephnilo.com/blog/adobe-bridge-vs-lightroom-for-organizing-photos ，**以 adobe.com 官网为准**）。

来源：https://en.wikipedia.org/wiki/Adobe_Bridge ；https://shotkit.com/adobe-bridge ；https://softwr.com/software/adobe-bridge

---

### 7. 系统级：macOS Time Machine 与 Windows 文件历史

定位：操作系统内建、面向个人、**无人值守自动备份**。素材库软件都不是它们的对手，但它们的**自动化、保留策略、恢复体验**是 pentimento「时光机」体验的直接原型（PRODUCT.md 明言参考时光机）。

**macOS Time Machine**：

| 功能点 | 有无 | 说明 |
|---|---|---|
| 自动记录 | 有 | 每小时自动备份；FSEvents 事件驱动增量（不轮询全盘）；首次全量、之后只拷变化块（APFS 块级 delta-copy） |
| 时间点浏览 | 有 | 时光机界面按时间轴穿梭浏览整库任意历史时点；整卷恢复 |
| 本地快照（应急层） | 有 | 备份盘不在时，每小时在本盘建 APFS 本地快照，保留 24 小时——**备份盘失联也有 24h 回滚窗口** |
| 保留策略 | 有（自动清理） | 24 小时内每小时、月内每天、更早每周；磁盘满删最旧（age-based + space-based thinning）。**注意：历史会被自动删除，与 pentimento「无历史删除」约定相反** |
| 目标 | 有 | 外置盘或网络位置（NAS/SMB）；多备份盘轮换；断连自动续备 |
| 排除规则 | 有 | 排除列表 |
| 防篡改 | 弱 | 快照可被 root 删除；自动瘦身意味着勒索软件潜伏超过保留期后历史丢失——**pentimento 的不可变库在这点上严格更强** |

来源：https://support.apple.com/en-us/102154 （本地快照 24h）；https://support.apple.com/en-us/104984 （保留策略）；https://eclecticlight.co/2023/03/01/how-does-time-machine-make-a-backup/ ；https://eclecticlight.co/2021/03/11/time-machine-to-apfs-understanding-backups/ （块级 delta、合成快照机制）

**Windows 文件历史（File History）**：

| 功能点 | 有无 | 说明 |
|---|---|---|
| 自动记录 | 有 | 默认每小时（可调 10 分钟～每天）；**用 NTFS USN change journal 检测变更而非扫描卷**；低优先级 I/O 后台运行、可中断可续 |
| 版本机制 | 有 | 变更文件**整份复制**到外部盘/网络位置，文件名附时间戳；无去重、无 delta（待核——未找到去重公开资料） |
| 时间点浏览/恢复 | 有 | 控制面板时间轴界面左右箭头穿梭；资源管理器右键「还原以前的版本」；**恢复到原位（覆盖且不可撤销，有明确警告）或「还原到…」其他位置** |
| 范围 | 有 | 库、桌面、收藏夹、联系人；排除文件夹；用户级免管理员 |
| 保留策略 | 有 | 按时长或按空间（空间不足删旧） |
| 防篡改 | 弱 | 备份在另一介质，加密原文件后历史可读——天然具备一定防勒索恢复能力，但无检测、保留策略同样可滚掉旧历史 |

来源：https://learn.microsoft.com/en-us/previous-versions/technet-magazine/dn448546(v=msdn.10) （USN journal 机制、设计目标）；https://learn.microsoft.com/en-us/windows/win32/fileio/change-journals ；https://support.microsoft.com/zh-cn/windows/experience/backup-recovery/backup-and-restore-with-file-history （还原流程、覆盖警告、还原到其他位置）

---

## 二、六家 × pentimento 核心能力功能矩阵

图例：✅ 强 / 🟡 部分或弱 / ❌ 无。「系统级」列 = Time Machine + Windows 文件历史综合。

| 核心能力 | Alienbrain | Anchorpoint | Perforce (Core+DAM) | Eagle | Billfish | Adobe Bridge+Lr | 系统级备份 | **pentimento 现状** |
|---|---|---|---|---|---|---|---|---|
| 素材库浏览（网格+标签+搜索） | 🟡 缩略图浏览强，无用户标签体系 | ✅ 树+标签+缩略图 | 🟡 靠 DAM | ✅ **基准** | ✅ | 🟡 Bridge 浏览强、无库 | ❌ | ✅ 三栏+网格/列表（R03） |
| 导入体验（拖放/扩展/剪贴板/去重拦截） | 🟡 VCS 语义 | 🟡 建仓语义 | 🟡 工作区语义 | ✅ **基准** | ✅ | 🟡 直接浏览 | ❌ | ✅ 管理式复制（R02），缺浏览器扩展/剪贴板 |
| 版本历史 | ✅ revision 树+缩略图历史 | ✅ Git | ✅ 不可变+审计 | ❌ | ❌ | 🟡 仅编辑历史 | ✅ 时间戳副本 | ✅ 内容寻址+时间轴（R06/R07） |
| 时间点恢复（整库/单件） | ✅ 单件+文件夹递归 | ✅ Git revert | ✅ 任意 revision | ❌ | ❌ | 🟡 编辑回退 | ✅ **最强**（整卷任意时点） | 🟡 单件已实现，整库时间点待实现（R08） |
| 双版对比 | ✅ 图像多版并排 | 🟡 Blueprint diff | ✅ 文本+二进制可视化 | ❌ | ❌ | 🟡 Camera Raw 前后 | ❌ | ✅ 并排/同步缩放/滑动（R09） |
| 回滚不销毁历史 | ✅ **（16 年前就有）** | ✅ | ✅ | — | — | ✅ | ✅（但保留期后自动删） | ✅ 核心约定 |
| NAS/远端 | 🟡 服务器+VPN 式远程 | 🟡 Git 远端 | ✅ 多站点/代理/云 | ❌ | 🟡 云资源库 | 🟡 NAS 性能差 | ✅ 备份目标 | ✅ 挂载/WebDAV/SFTP 只读（R15–R18，规划中） |
| 防勒索（不可变+发现+恢复） | ❌ 未找到 | ❌ 未找到 | 🟡 权限/审计/加密，无检测 | ❌ | ❌ | ❌ | 🟡 异介质副本可救但无检测+保留期会滚掉 | ✅ **三防线独家**（85-antiransom.md） |
| 协作锁 | ✅ | ✅ 云元数据锁 | ✅ 行业标杆 | ❌ | ❌ | ❌ | ❌ | ❌（单机定位外，R 需求未含） |
| 备份导出/校验 | 🟡 待核 | 🟡 依赖 Git 远端 | ✅ 云托管自动备份 | 🟡 eaglepack 人工 | 🟡 云加密备份 | 🟡 LrC 仅目录备份 | ✅ | 🟡 R19 规划中（备份包+校验） |

矩阵结论：**「版本历史 × 素材库浏览」正交组合没有一家做全**——素材库（Eagle/Billfish）全部无版本；VCS（Alienbrain/Anchorpoint/Perforce）全部无素材库级整理体验；系统备份有版本但无素材管理。pentimento 的定位恰好落在这个空档，防勒索三防线则无人做。

---

## 三、「pentimento 应该抄的做法」清单

1. **回滚成为新 tip、永不销毁历史**——Alienbrain 2005 年（v5 评测）就已明确此语义，验证 pentimento 核心约定的行业合法性。来源：https://gamedeveloper.com/programming/product-review-nxn-s-alienbrain-5
2. **版本历史按缩略图浏览**——用户用「看图」而非「看注释+哈希」找版本，美术工具的硬要求。抄 Alienbrain 的缩略图历史条。来源：https://alienbrain.com/features
3. **整文件夹递归回滚**——Alienbrain 支持单文件之外的整目录递归回滚，对应 pentimento R08 整库时间点/R10 快照内选中项恢复的 UI 命名与范围预期。来源：https://alienbrain.com/features
4. **「恢复到其他位置」选项**——Windows 文件历史的「还原到…」避免覆盖当前版，配合明确警告「覆盖不可撤销」；pentimento 的「恢复前保护当前状态」（P-316）应再加一条同款出口。来源：https://support.microsoft.com/zh-cn/windows/experience/backup-recovery/backup-and-restore-with-file-history
5. **时间轴穿梭式恢复 UI（时光机原型）**——Time Machine 与文件历史都用「左右箭头 + 整库状态随时间切换」而不是逐文件选版本；pentimento 整库时间点（R08）界面应以此为原型的显式致敬点（PRODUCT.md 已引时光机，落地为「时间点目录浏览」P-312）。来源：https://support.microsoft.com/zh-cn/windows/experience/backup-recovery/backup-and-restore-with-file-history
6. **本地应急快照层（Time Machine local snapshots）**——备份目标不可达时仍在源盘保 24h 快照应急。pentimento 的防勒索紧急锚点（snapshotAll）在语义上就是它，可把「离线时也保留本地锚点、明确 24h 类过期语义」写成文案。来源：https://support.apple.com/en-us/102154
7. **用文件系统事件而非轮询做变更检测**——Time Machine 用 FSEvents、Windows 文件历史用 NTFS USN change journal、Anchorpoint 每分钟 Auto Fetch；对应 pentimento D-06 决策点（启发式 vs 文件监视器）：Windows 上 USN journal 是现成答案。来源：https://learn.microsoft.com/en-us/previous-versions/technet-magazine/dn448546(v=msdn.10) ；https://eclecticlight.co/2023/03/01/how-does-time-machine-make-a-backup/
8. **自动备份频率可配置 + 排除规则 + 空间不足降级策略**——文件历史（10 分钟～每天）与 Time Machine（age/space thinning）都把「快照节奏」做成显式设置；pentimento R06 定时快照策略照此暴露给用户。来源：https://learn.microsoft.com/en-us/previous-versions/technet-magazine/dn448546(v=msdn.10) ；https://eclecticlight.co/2023/11/01/time-machine-in-sonoma-how-we-got-here/
9. **智能文件夹 = 保存的查询**——Eagle 智能文件夹是筛选条件的持久化，不复制文件；pentimento R03 组合筛选应支持「另存为智能文件夹」而不只是一次性筛选。来源：https://cn.eagle.cool/support/article/smart-folders
10. **导入时重复拦截**——Eagle 在复制导入时自动识别相同文件并询问；pentimento 已有 SHA-256 去重，把「导入时重复询问 + 事后重复扫描合并」补成用户流程。来源：https://cn.eagle.cool/blog/post/adobe-bridge-and-lightroom-alternative
11. **颜色搜索**——Eagle/Billfish 都按颜色值检索，游戏美术找「同色系贴图」高频；pentimento 技术元数据已提取色彩信息，顺路可做。来源：https://cn.eagle.cool/blog/post/how-to-search-for-files-in-eagle ；https://www.billfish.cn/
12. **锁 = 置只读 + 全局可见**——Anchorpoint 锁文件时直接把文件置只读、锁状态人人可见；pentimento 即使单机，「锁定素材防误改」也可用同一交互（与防线一只读机制天然同构）。来源：https://anchorpoint.app/compare/alternative-to-github-desktop
13. **大文件同步走 delta**——Perforce delta sync/submit 与 APFS 块级 delta-copy 证明 NAS/远端场景按块增量传输是必须项；pentimento R16/R19 的按需读取与备份包应对大 PSD 做分块。来源：https://www.perforce.com/sites/default/files/pdfs/pf-core-product-brief-web.pdf ；https://eclecticlight.co/2021/03/11/time-machine-to-apfs-understanding-backups/
14. **免费层获客**——Perforce（5 人免费全功能）、Anchorpoint（免费本地版）、Billfish（个人免费）三家都用免费层起量；pentimento 个人版定价可参考「单机免费/付费 Pro 功能」分层。来源：https://www.perforce.com/products/p4/pricing ；https://fab.com/listings/c37f3c5e-70c6-4ea0-bb18-b1b367ec443c
15. **按角色分档而非按功能堆叠**——Alienbrain 三档按 Coders/Artists/Advanced 划分且 Reader 只读免费，是对「个人→团队」扩展路径的成熟切片。来源：https://alienbrain.com/pricing.php
16. **共享盘上的增量文件版本（v001/v002）托管**——Anchorpoint 把美术圈的手工增量命名做成受管版本流（不需 Git）；pentimento 对「外部目录追踪」（D-02/R02 暂缓项）可借鉴其元数据+文件名解析做法。来源：https://docs.anchorpoint.app/docs/asset-management/incremental-file-versioning/

---

## 四、pentimento 未覆盖但对标普遍有的功能缺口清单

> 「普遍有」= 至少两家对标具备；pentimento 现状对照 PRODUCT.md 与 R01–R19。其中标注 ★ 的建议进入需求评审（对应 todo 决策点）。逐条复核（V1 open / 排除）登记于 todo.md「对标缺口复核」节。

1. ★ **智能文件夹/保存的搜索**（Eagle、Billfish、Bridge 智能集合均有；pentimento 仅一次性组合筛选，R03 决策点 D-03 可一并拍板）。
2. ★ **颜色搜索/按颜色筛选**（Eagle、Billfish；pentimento 已提取色彩元数据，差一层检索入口）。
3. ★ **导入去重的用户流程**：导入时重复询问 + 事后重复扫描合并（Eagle、Billfish；pentimento 有内容寻址去重但未暴露该流程）。
4. ★ **浏览器扩展/剪贴板采集**（Eagle、Billfish、Eagle 的网页拖入；pentimento 仅文件导入——对「参考图收集」场景是 Eagle 体验的核心组成）。
5. **图上标注/批注**（Eagle 图片标注、Alienbrain 非破坏标注层、Anchorpoint 视觉批注；pentimento 仅有备注文字）。
6. ★ **库内文件夹组织与改名/移动后历史归属保持**（Eagle/Billfish 天然支持；pentimento P-216/D-05 明确未决，属已知缺口而非遗漏）。
7. **格式广度**：Eagle 100+/109 格式、字体免安装预览、音频悬停播放、视频逐帧；pentimento V1 仅 PNG/JPEG/WebP/TGA/BMP（R04），游戏美术贴图源文件（PSD/SAI/Procreate 等）预览差距大。
8. ★ **Windows 上基于 USN change journal 的变更监听**（Windows 文件历史、Anchorpoint Auto Fetch 有事件驱动先例；pentimento D-06 决策点）。
9. **版本历史的缩略图条**（Alienbrain；pentimento 时间轴 R07 已实现缩略图维度，见 README「当前可用」与 src/mainwindow.cpp 的 reloadHistory()）。
10. **编辑器/DCC 集成**（Alienbrain PS/Max/Maya/UE、Anchorpoint UE/Unity、Perforce 全家桶；pentimento 无此定位——单机素材库，标注为**定位外缺口**，不建议 V1 做）。
11. **协作锁**（Alienbrain/Anchorpoint/Perforce 有；pentimento 单机定位外，同上标注）。
12. ★ **NAS/外部目标的自动化定期备份**（Time Machine 每小时、文件历史默认每小时自动到 NAS/外置盘；pentimento R19 备份包导出尚无「调度」语义）。
13. **保留策略的显式设置界面**（Time Machine/文件历史都有可视化保留规则；pentimento 因「无历史删除」约定只能做「仅预演」，R19/D-19 已识别，但界面形态可抄前两者）。
14. **断连目标的友好降级**（Time Machine 断连自动续备+本地快照兜底、文件历史低优先级 I/O 可断可续；pentimento R18 断线重连状态机规划中，本地锚点兜底可抄做法 6）。

---

## 五、总备注

- Alienbrain 最新可下载版本为 2022（官网 download 页）；2019 后无新版本公告，**仍在营但迭代缓慢**（判断依据：官网仍开放购买与下载，https://alienbrain.com ）；更细的版本史机制（是否内容寻址）未找到公开资料（待核）。
- Eagle 无任何版本历史能力为本调研中多来源交叉确认（官网功能页+第三方对比），未发现反例；若 Eagle 4.x 隐藏版本功能，待核。
- Billfish 版本历史缺失基于官网功能页与第三方评测 absence 判断，未见官方「无版本」声明（待核口径：标注为「未找到版本历史功能公开资料」）。
- Adobe 产品定价随促销变动，本文只记录调研时可见数字。

---

## 六、可借鉴做法复核（V1 采纳 / V2 候选 / 排除）

> 复核口径：与 R01–R19 范围对照。**V1 采纳** = 对应已纳入（建议纳入）需求或其决策点，属 V1 范围；**V2 候选** = 超出 V1 已定范围但价值明确，列为 V2 候选；**排除** = 非功能范围（定价策略）或定位外，明确不纳入 V1。D-xx 待拍板项以需求文档为准。

| # | 做法 | 复核 | 依据与说明 |
|---|---|---|---|
| 1 | 回滚成为新 tip、永不销毁历史 | **V1 采纳** | 已实现：恢复形成新版本不销毁历史（核心约定）。本做法验证该约定的行业合法性（Alienbrain 2005 起）。 |
| 2 | 版本历史按缩略图浏览 | **V1 采纳** | 已实现：版本时间轴带缩略图。本做法确认「看图找版本」方向正确。 |
| 3 | 整文件夹递归回滚 | **V1 采纳** | 对应 R08 整库时间点 / R10 快照内选中项恢复的 UI 命名与范围预期；D-08/D-10 待拍板。 |
| 4 | 「恢复到其他位置」选项 | **V1 采纳** | 对应 R10 恢复范围：已有「恢复前保护当前状态」（P-316）；补「恢复到其他位置」出口（避免覆盖当前版），D-10 待拍板。 |
| 5 | 时间轴穿梭式恢复 UI（时光机原型） | **V1 采纳** | 对应 R08 整库时间点 → P-312「时间点目录浏览」，以 Time Machine/文件历史「左右箭头 + 整库状态切换」为显式原型；D-08 待拍板。 |
| 6 | 本地应急快照层（TM local snapshots） | **V1 采纳** | 已实现：防勒索紧急锚点 `snapshotAll`（85-antiransom.md 防线二）即此语义。补「离线时也保留本地锚点、明确 24h 类过期语义」文案。 |
| 7 | 用文件系统事件而非轮询做变更检测 | **V1 采纳** | 对应 D-06 决策点（启发式 vs 文件监视器）：Windows 上 NTFS USN change journal 是现成答案（文件历史/Anchorpoint 先例）。 |
| 8 | 自动备份频率可配置 + 排除规则 + 空间不足降级 | **V1 采纳** | 对应 R06 定时快照与排除规则：把「快照节奏」做成显式设置暴露给用户；与 R08 整库时间点一起确认。 |
| 9 | 智能文件夹 = 保存的查询 | **V1 采纳** | 对应 R03 组合筛选：支持「另存为智能文件夹」（筛选条件持久化，不复制文件）；D-03 待拍板。 |
| 10 | 导入时重复拦截 | **V1 采纳** | 对应 R02：内容寻址去重已实现（SHA-256），补「导入时重复询问 + 事后重复扫描合并」用户流程。 |
| 11 | 颜色搜索 | **V1 采纳** | 对应 R03/R05：技术元数据已提取色彩信息，补颜色检索入口（找同色系贴图）。 |
| 12 | 锁 = 置只读 + 全局可见 | **V2 候选** | 协作锁本身定位外（单机）；但单机「锁定素材防误改」交互与防线一只读机制天然同构，可作 V2 候选。 |
| 13 | 大文件同步走 delta | **V2 候选** | R16/R19 按需读取为 V1 范围；按块增量（delta）传输是 NAS/远端与备份包对大 PSD 的优化，列为 V2。 |
| 14 | 免费层获客 | **排除（非功能范围）** | 定价/获客策略，超出功能对标范围；不进 V1 功能清单，仅记录参考（Perforce 5 人免费 / Anchorpoint 免费本地版 / Billfish 个人免费）。 |
| 15 | 按角色分档而非按功能堆叠 | **排除（非功能范围）** | 定价策略（Alienbrain Coders/Artists/Advanced + Reader 只读免费），非功能范围；不进 V1 功能清单。 |
| 16 | 共享盘上的增量文件版本（v001/v002）托管 | **V2 候选** | 对应「外部目录追踪」（D-02/R02 暂缓项）：若 D-02 纳入 V1 则升为 V1 候选，否则 V2；借鉴其元数据+文件名解析做法。 |
