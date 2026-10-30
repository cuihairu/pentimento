# 工程基础（代决执行，2026-10-30）

> 状态：**已代决**。依据 `docs/review/2026-10-30-decisions.md` 执行 P-102–P-107 的立即动作。含 Qt 版本/模块与插件验证证据、版本后端接口与资源存储访问接口定义、后台任务队列规则、凭据安全清单与翻译键清单。**接口为设计定义（含草案），代码实现须待 P-015 批准。**

## P-102 Qt 版本与模块（Linux 验证记录）

沿用 C++17/Qt 6 Widgets；候选版本以本机 Qt 6.10.2 为开发基线，三系统可安装/部署证据到位后固定版本。

**本机验证（2026-10-30，Linux，`/usr/lib/x86_64-linux-gnu`）**：

| 项 | 结果 |
|---|---|
| Qt 版本 | 6.10.2（`qmake6` / `Qt6::*` CMake 目标，`qt6-base-dev 6.10.2+dfsg-7`） |
| CMake 组件 | Core、Gui、Widgets、Sql、Test（`find_package(Qt6 6.4 REQUIRED ...)` 通过） |
| SQLite 驱动 | `libqsqlite.so` 存在；`sqlite3` CLI 可用 |
| 图像插件 | imageformats：`libqgif / libqico / libqjpeg / libqsvg` |
| 支持的图像格式 | `QImageReader::supportedImageFormats()` = bmp, cur, gif, ico, jfif, jpeg, jpg, pbm, pgm, png, ppm, svg, svgz, xbm, xpm |
| 构建/测试 | `cmake --preset dev && cmake --build --preset dev && ctest --preset dev` 通过（1/1 绿，`library_history`） |

**关键仓库事实（影响 P-210）**：本机 Qt 6.10.2 **不提供 WebP/TGA 解码**（无 `libqwebp`/`libqtga` 插件，且非 built-in）；PNG/JPEG/BMP 可预览。V1 五格式中的 WebP/TGA 在本环境需补充插件（Debian/Ubuntu `qt6-image-formats-plugins` 含 webp/tga/icns 等）或按「降级：暂无预览，原字节保留」处理。记录此限制，不作为「已支持」结论。

**待用户输入**：macOS/Windows 可安装版本、图像插件与打包要求（缺平台证据则不宣称该平台交付）。

**许可/部署**：项目 Apache-2.0；Qt 及依赖保留各自许可证，分发须遵守实际使用的 Qt 授权条件（静态链接、动态链接与商业授权差异须按所选打包方式确认）。

## P-103 版本后端接口（能力查询定义）

按代决：V1 仅自建后端；接口按能力独立查询，避免「支持历史」单布尔掩盖差异。**定义以下独立能力**，每个能力返回 支持/不支持/未实现 与 不支持错误码：

- `Capability::History` — 是否有版本历史列表；
- `Capability::SnapshotTree` — 是否有整库快照/提交树（自建=整库一致快照；Git=提交树；SVN=revision 树；TM=未实现）；
- `Capability::Restore` — 是否可生成新版本恢复（自建=是；Git/SVN/TM=否，只读）；
- `Capability::Lock` — 是否暴露锁信息（自建=单进程写锁；SVN=读锁；Git=无；TM=无）；
- `Capability::RemoteFetch` — 是否需按需获取远端对象（Git LFS=按需须同意；SVN=按需；自建=无）。

**接口设计草案（设计级，不实现）**：

```cpp
// design only — not compiled until P-015 approval.
enum class Capability { History, SnapshotTree, Restore, Lock, RemoteFetch };
struct BackendInfo {
    QString name;                  // "self", "git", "svn", "time-machine"
    bool writable;                 // V1: self only
    QHash<Capability, bool> capabilities; // per-capability support
    QString unsupportedReason(Capability c) const; // human-readable, stable error
};
```

**不支持错误**：对未实现能力返回稳定的 `CapabilityUnsupported` 错误（含能力名与原因），界面据此隐藏对应操作（见 P-409）。V1 不实现 Git/SVN，接口不得假定所有后端可写。

## P-104 资源存储访问接口（定义 + 原验收缩减记录）

统一文件信息结构；V1 实现本地/挂载来源；直连适配（WebDAV/SFTP）延后。**定义**：

- 路径：库内相对路径 + 稳定资源 ID；库外来源以挂载路径（可断线/只读）。
- 文件身份：内容哈希（SHA-256）+ 资源 ID；不以绝对路径为身份。
- 大小、修改时间（UTC 毫秒，界面本地时区显示）。
- 权限/可达性：可读、可写、可达、只读、失联（`SourceUnreachable`）、权限不足（`PermissionDenied`）。
- 不可达错误：`SourceUnreachable`/`PermissionDenied` 与本地 `DiskFull`/`CorruptObject` 区分；挂载路径不得一律按本地健康磁盘处理。

**原验收缩减记录**：原验收「本地、挂载目录、WebDAV/SFTP 返回一致的目录与文件信息」缩减为「本地与已挂载目录返回一致信息；WebDAV/SFTP 直连延后（不把占位类记为已支持）」。

## P-105 后台任务队列（规则定义）

大文件与网络候选功能的前置基础。**定义**：

- 任务：进度（0–100 + 当前项）、取消点（仅安全阶段）、结果清单（成功/失败/未处理）、错误（可读）。
- **单写者规则**：写操作（导入/快照/恢复/备份）串行管理，同一库同一时刻至多一个写任务；读任务（浏览/预览）可并行但须避免读未完成写。
- 取消语义：按文件边界安全生效；已完成项保留，不静默丢失结果清单；取消不撤销已完成导入（见 P-206）。
- 界面线程不处理大文件；不允许在不可回滚阶段强杀写任务（强杀须经日志收敛，见 P-320）。

**先接入**：导入与哈希（待 P-015 实现）。

## P-106 系统凭据适配（安全验收清单）

V1 不收集 NAS/仓库密码；凭据系统随直连后端延后。列为协议上线**前置条件**，形成禁止密码进入敏感位置的**验收清单**：

- [ ] 凭据不进入素材索引、不进入日志（含调试/崩溃日志）、不进入明文配置（`catalog.sqlite`、`*.conf`、`*.ini`、环境转储）。
- [ ] 密码字符串不序列化到持久化文件；内存使用后清零（`QByteArray::fill(0)`/`OPENSSL_cleanse` 等价）。
- [ ] 应用内无密码输入假界面；系统挂载凭据不复制进应用。
- [ ] 错误信息不含密码；重试/取消不泄漏。
- [ ] 未来直连后端上线时逐项通过（P-511/P-512 随协议绑定，不可省工）。

## P-107 中英翻译机制（键清单与切换时机）

保留中英翻译机制；首批覆盖关键流程与错误；不扩展更多语言。**整理键类别**：菜单、状态栏、对话框、错误消息、危险操作提示。**语言切换时机**：应用重启生效（Qt 翻译文件重载成本低且避免运行时局部混用），或设置页即时生效二选一 —— 待用户走查后定；当前建议：切换后重启生效，界面提示「重启后生效」。

**首批键清单（草案，供走查）**：`menu.file/import/record/edit/open`、`status.library.path/selection/connection`、`dlg.restore.preview/protect`、`error.source.unreachable/perm.denied/disk.full/corrupt`、`version.current/history/deleted/corrupted`、`format.no-preview`、`task.import.progress/cancel`。

**硬编码禁用**：动态拼句、硬编码快捷键文本是漏译/歧义/平台冲突来源，走查时逐处核查；不只翻译主页面而漏掉危险操作提示。
