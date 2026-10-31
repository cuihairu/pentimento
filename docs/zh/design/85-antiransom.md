# 防勒索病毒安全防线（2026-10-31 新增核心需求）

> 状态：**防线一、二、三已实现（Linux 验证环境）**。依据用户 2026-10-31 令：本地素材库被勒索软件加密/篡改时，产品要能**防住、发现、并快速恢复**。范围外不做杀毒引擎、不做病毒库——只做素材库自身的防线：不可变、发现、恢复。
>
> 三层防线：① 不可变版本库（防篡改）② 批量修改哨兵（发现）③ 整树回滚（恢复）。

## 防线一：不可变版本库（防篡改）

- **append-only**：对象按 SHA-256 内容寻址，一旦写入不再改写；相同内容复用对象。
- **落盘后置只读**：新对象写入后立即置只读（`src/platform_immutable.{h,cpp}`）。Linux 优先 `chattr +i`（ioctl FS_IMMUTABLE_FL，需 CAP_LINUX_IMMUTABLE/root），OS 拒绝时回退为清除写权限位（owner 可 chmod 恢复，属尽力而为）；Windows 置 FILE_ATTRIBUTE_READONLY（真机验证 TODO）；macOS 置 UF_IMMUTABLE（真机验证 TODO）。平台差异全部抽象在 `platform_immutable`，不散落业务代码。
- **版本库与工作区分离**：`objects/`（+`catalog.sqlite`）为版本库，`working/` 为工作副本；恢复路径单向 库→工作区（`restore` 只写 `working/`，绝不改写 `objects/`；`exportRevision` 禁止导出进库内）。
- **hardenObjects()**：对已存在但被置回可写的对象重新加只读（幂等），用于打开既有库时加固。
- **验收**：模拟加密/篡改工作区文件后，历史对象仍可读、可恢复，测试绿。

### 实现映射（防线一）

| 件 | 位置 |
|---|---|
| 平台只读抽象 | `src/platform_immutable.{h,cpp}` |
| 对象写后置只读 | `Library::append`（`src/library.cpp`） |
| 加固既有对象 | `Library::hardenObjects`（`src/library.cpp`） |
| 单测 | `objectFilesAreImmutableAndResistTamper`、`historyObjectsSurviveWorkspaceEncryption`（`tests/library_test.cpp`） |

### 已知边界（防线一）

- 只读是**尽力而为**防线：同用户进程可用 `platform::makeMutable` 恢复写权限（owner 可 chmod），`chattr +i` 才是强保证且需要特权。这不冒充「防住特权攻击者」，只降低同权限勒索进程静默改写的便利度。
- 加密工作区（`working/`）不影响 `objects/`：对象校验（读时哈希比对）仍会拦截被改写的内容。

## 防线二：批量修改哨兵（发现）

- **监测**工作区（`working/`）异常批量写入。三个独立信号（`src/sentinel.{h,cpp}`，阈值可配置，默认取生产值）：
  1. **批量写入**：一个扫描窗口内被修改（新增/大小或 mtime 变化）文件数 ≥ `batchChangeThreshold`（默认 25）。
  2. **扩展名批量变更**：出现基线中从未有过的新扩展名的文件数 ≥ `extensionChurnThreshold`（默认 10），对应 `.png → .enc` 式改名。
  3. **内容熵值异常**：被修改文件中 Shannon 熵（bits/byte，采样至 64 KiB）超 `entropyLimit`（默认 7.2，加密内容约 8.0）的文件数 ≥ `highEntropyThreshold`（默认 10）。
- **触发**任一信号 → `SentinelAlert{triggered, reason, 计数, 最大熵}`；**自动做紧急快照锚点**（`Library::snapshotAll`）。
- **快照锚点**：`snapshotAll(note)` 在事务内记录 `snapshots` + `snapshot_assets`，逐素材锚定**最近一次干净修订（感染前内容）**；当前工作内容若已偏离（被加密/篡改），另落一个 `紧急快照锚点` 修订留作取证，但**不作为恢复锚点**；工作文件缺失（被改名）时同样锚定最近可读（干净）修订。这样锚点始终指向可回滚的感染前状态。**抢跑保护**：若自动记录（2s）先于哨兵（5s）把被加密内容捕获为最新修订，则跳过内容与当前工作文件一致的最新修订（它只镜像感染字节），锚点仍落在感染前干净修订。
- **验收**：测试 fixture（批量改名+随机字节覆盖）能触发；单测覆盖三种特征；误报率有阈值测试（正常小编辑不触发）。

### 实现映射（防线二）

| 件 | 位置 |
|---|---|
| 哨兵与阈值 | `src/sentinel.{h,cpp}`（`SentinelConfig`/`SentinelAlert`/`Sentinel`） |
| 紧急快照锚点 | `Library::snapshotAll`、`Library::snapshots`（`src/library.{h,cpp}`） |
| 快照表 | `snapshots`、`snapshot_assets`（`src/library.cpp` 建表） |
| 单测 | `tests/security_test.cpp`：`detectsBatchRewrite`/`detectsExtensionChurn`/`detectsHighEntropyBurst`/`staysQuietOnNormalSmallEdits`/`sentinelAlertsAndLibraryTakesEmergencySnapshot` |

### 已知边界（防线二）

- 哨兵是**事后发现**：勒索已发生后告警，不是实时拦截；检测窗口由调用方（UI 定时器）决定，扫描为增量比对（不每次全量哈希，只对变化文件取熵样本）。
- 误报阈值是经验值：`staysQuietOnNormalSmallEdits` 证明正常小编辑不触发；真实素材库的合法大改（批量整理/批量导入）可能触发，属「宁可误报、不可漏报」倾向，触发后走紧急快照 + 用户确认回滚。
- 扩展名信号按「基线中从未出现的新扩展名」计数，避免把合法新格式（如首次导入 .psd）当勒索——需用户确认。

## 防线三：整树回滚（恢复）

- **一键恢复**：界面「恢复到锚点」列出紧急快照（`Library::snapshots`），选择后 `Library::rollbackAll(snapshotId)` 逐素材恢复到该锚点修订。
- **走「恢复形成新版本」口径**：`rollbackAll` 对每个素材调用现有 `restore` 路径——先保存未记录的当前内容，再追加「恢复历史版本」新版本，最后原子替换工作文件。**不销毁后续历史**：被加密的版本（`紧急快照锚点` 修订与 `.enc` 工作文件）仍保留、可再恢复。
- **锚点指向感染前**：快照锚点记录的是最近一次干净修订（见防线二）；回滚后工作文件内容回到感染前。已在锚点内容的素材跳过（空操作）。
- **验收**：e2e 测试——植入模拟勒索（批量改名+随机字节覆盖）→ 哨兵告警 → 选择锚点回滚 → 采样文件哈希与感染前一致。

### 实现映射（防线三）

| 件 | 位置 |
|---|---|
| 整树回滚 | `Library::rollbackAll`（`src/library.{h,cpp}`） |
| 快照列表 | `Library::snapshots`（`src/library.{h,cpp}`） |
| UI 入口 | `MainWindow::rollbackToSnapshot`（`src/mainwindow.{h,cpp}`），工具栏「恢复到锚点」 |
| 单测 | `tests/security_test.cpp`：`rollbackRestoresWorkspaceToPreInfectionAnchor` |

### 已知边界（防线三）

- 回滚目标只能是**已记录的历史修订**：感染前未手动/自动记录的编辑无法恢复（与既有恢复边界一致）。
- 回滚前须先关闭外部编辑器，避免它覆盖恢复结果（与既有恢复一致）。
