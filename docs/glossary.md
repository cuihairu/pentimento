# Glossary

Terms below are derived from the 2026-11-07 source-layer analysis and use the
responsibility layers documented in
[Architecture](./architecture.md). Equivalent Chinese terms are in the
[Chinese glossary](./zh/glossary.md).

| Term | English | Definition | Evidence |
| --- | --- | --- | --- |
| 素材库 | Asset Library | One validated, relocatable root that owns a SQLite catalog, immutable objects, working copies, and a process lock; noncopyable by design. | `src/library.h:145` |
| 工作副本 | Working Copy | The editor-owned live file under `working/`; a missing or unreadable file is recoverable state, not a deletion. | `src/library.cpp:1580` |
| 历史对象 | History Object | Content-addressed `objects/<sha256>` payload, verified by hash and hardened after publish. | `src/library.cpp:918` |
| 版本 | Revision | A content revision row and timeline entry; a restore appends a revision rather than replacing history. | `src/library.h:65` |
| 墓碑 | Tombstone | An explicit in-app deletion revision plus `assets.deleted=1`; history and objects remain browsable. | `src/library.cpp:1530` |
| 快照 | Snapshot | A whole-tree anchor whose manifest freezes path, hash, state, and anchor revision at one boundary. | `src/library.cpp:1631` |
| 快照清单 | Snapshot Manifest | The frozen `snapshot_assets` rows whose state vocabulary is present, tombstone, excluded, or missing. | `src/library.h:94` |
| 恢复计划 | Restore Plan | A no-write preview bound to target revision and working-copy `(size, mtime)`; execution rejects a stale binding. | `src/library.h:177` |
| 不可变加固 | Immutability Hardening | The best-effort Linux/Windows/macOS operation that makes a stored object read-only or immutable. | `src/platform_immutable.h:17` |
| 内容寻址 | Content Addressing | SHA-256 is both the object identity and deduplication key; every read verifies the address. | `src/library.cpp:926` |
| 来源连接 | Source Connection | A read-only reachability state machine for an already-mounted local/SMB/NFS source. | `src/source_connection.h:13` |
| 备份目标探测 | Backup Target Probe | A separate read-only writability and free-space check for a backup destination. | `src/storage.h:43` |
| 重扫预览 | Rescan Preview | An additive classification of fresh, already-imported, excluded, and unreadable source files; it never syncs deletions. | `src/importer.h:65` |
| 稳定自动记录 | Stable Auto-Record | The post-read `(size, mtime)` re-check that prevents torn writes from becoming versions. | `src/library.cpp:1112` |
| 定时快照 | Snapshot Schedule | Runtime-only 15/30/60-minute state machine with user/system pause, one run gate, and collapsed catch-up. | `src/snapshot_schedule.h:5` |
| 哨兵 | Sentinel | A working-tree baseline scanner that detects batch writes, new extensions, and high entropy, then triggers an emergency snapshot. | `src/sentinel.h:6` |
| 后台队列 | Task Queue | A serial worker with cooperative cancellation, item-boundary progress, and join/detach shutdown modes. | `src/taskqueue.h:10` |
| 缩略图缓存 | Thumbnail Cache | Content-hash plus decoder-version cache entries under `thumbnails/`; corrupt entries are removed and regenerated. | `src/thumbnails.h:7` |
| 受限解码 | Bounded Decode | Header-based metadata and downsampled image reads that keep huge files from exhausting memory. | `src/preview.h:8` |
| 统一对比变换 | Shared Comparison Transform | One scale and one image-space origin synchronized across compare panes and the sliding divider. | `src/compare_canvas.h:7` |
| 筛选事实 | Filter Facts | Catalog-derived format, pixel size, tags, rating, and logical path consumed by a pure AND/OR filter matcher. | `src/filter.h:6` |
| 出处记录 | Provenance | Append-only, user-entered source, author, license, and evidence-object records. | `src/library.h:109` |
| 字段历史 | Field History | Read-only old/new field values aligned to metadata-only revisions. | `src/library.h:130` |
| 一致性报告 | Consistency Report | A streaming audit that classifies references as OK, missing, corrupt, I/O-error, or orphan. | `src/library.cpp:2047` |
| 备份包 | Backup Package | The verified `manifest.json`, deduplicated objects, and last-written `COMPLETE` marker. | `src/library.cpp:1860` |
| 能力模型 | Capability Model | Independent backend flags (`History`, `SnapshotTree`, `Restore`, `Lock`, `RemoteFetch`) mapped to safe UI actions. | `src/backend.h:6` |
| 最近库 | Recent Library | A QSettings list keyed by stable library ID so a moved library can be re-validated. | `src/recent.h:6` |
| 应用配置 | Application Settings | User-scope QSettings keys such as recent libraries and `ui/language`. | `src/i18n.cpp:10` |
| 用户界面壳层 | UI Shell | The main window and dialogs that own widgets, timers, action gating, and user confirmation. | `src/mainwindow.h:28` |
| 核心库 | Core Library | The widget-free Qt Core/Gui/Sql domain and service components consumed by the executable. | `CMakeLists.txt:53` |
