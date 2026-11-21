# 分层：核心与 UI 代码归属

> 状态：**UI-3 产物（2026-11-22）**。用户硬要求「核心功能不能和 UI 的代码放在一起」。
> 本文件是移动前的归属盘点：`src/` 平铺里每个文件的「核心 or UI」判定、歧义项理由、
> 目标 CMake 依赖图与验收证据。**先盘点再动**，本表是 `src/ui/` 目录聚合与
> `pentimento_ui` 库目标的依据。

## 1. 判定口径

- **UI 层**：直接构建/持有 Qt Widgets 控件、实现窗口/画布/交互的代码。特征是
  `#include <QWidget>` 及子类、`QMainWindow`/`QApplication`。
- **核心层**：领域数据、存储、来源访问、任务队列、纯逻辑。允许 Qt Core/Gui/Sql/Network，
  **不允许任何 Qt Widgets 头、不允许 include 任何 UI 头**。
- 歧义项（数据缓存、解码助手）单独判定，理由写入第 3 节。

## 2. 归属表（移动前盘点）

### 2.1 归 UI → 移入 `src/ui/`

| 文件 | 判定 | 依据 |
|---|---|---|
| `main.cpp` | UI（exe 入口） | `QApplication`，include `mainwindow.h`；保留在 `src/` 根作为 `pentimento` 可执行入口 |
| `mainwindow.h` | UI | `class MainWindow : public QMainWindow` |
| `mainwindow.cpp` | UI | 主窗口实现，`QMainWindow` |
| `mainwindow_versions.cpp` | UI | MainWindow 版本对比分片 |
| `mainwindow_metadata.cpp` | UI | MainWindow 元数据历史分片 |
| `mainwindow_snapshots.cpp` | UI | MainWindow 快照分片 |
| `mainwindow_backup.cpp` | UI | MainWindow 备份分片 |
| `mainwindow_sources.cpp` | UI | MainWindow 来源/连接分片 |
| `compare_canvas.h/.cpp` | UI | `class CompareCanvas : public QWidget`（版本对比画布） |
| `preview_canvas.h/.cpp` | UI | `class PreviewCanvas : public QWidget`（预览画布） |

### 2.2 留核心（`src/` 根不动）

`backend.*`、`discovery.*`、`filter.h`、`git_source.*`、`i18n.*`、`importer.*`、
`library.*`、`library_internal.h`、`library_backup.cpp`、`library_external.cpp`、
`library_meta.cpp`、`library_objects.cpp`、`library_restore.cpp`、`library_snapshot.cpp`、
`platform_immutable.*`、`preview.*`、`recent.*`、`remote_address.*`、`sentinel.*`、
`source_connection.*`、`storage.*`、`svn_source.*`、`taskqueue.*`、`thumbnails.*`、
`exclusion.h`、`snapshot_schedule.h`。

以上全部：Qt Widgets 头零引用（逐文件核验），且被 UI 件 include 但自身不反向依赖 UI。

## 3. 歧义项判定

| 文件 | 判定 | 理由 |
|---|---|---|
| `thumbnails.h/.cpp` | **核心** | `ThumbnailCache` 是纯数据/存储缓存：`QImage` 属 Qt Gui（core 已依赖 Gui），无任何 Widgets 头；只被 `thumbnails.cpp` 与 `mainwindow.h` 使用，不承担控件职责。留核心，不因「图片」而误判 UI。 |
| `preview.h/.cpp` | **核心** | 解码/元数据助手（`preview` 命名 = 图像解码与尺寸/格式信息），被多个 `library_*.cpp` 使用；无 Widgets 头。属核心数据服务。 |
| `exclusion.h` | **核心** | 纯逻辑：`exclusionReason` 判定，头文件实现，无 Qt 依赖。 |
| `snapshot_schedule.h` | **核心** | 纯逻辑：`SnapshotSchedule` 运行时机计算，无 Qt 依赖。 |
| `main.cpp` | UI | 虽只含 `QApplication`/命令行解析，但它是 `pentimento` 可执行入口且 include `mainwindow.h`，属 UI 层入口；不进入 `pentimento_ui` 库，留在 exe。 |

## 4. 目标 CMake 依赖图

```
pentimento (exe, src/main.cpp)
    └─ pentimento_ui   (src/ui/…, links Qt6::Widgets)
           └─ pentimento_core (src/…, links Qt6::Core/Gui/Sql/Network, 无 Widgets)
```

硬约束：

1. 依赖方向单向：`pentimento → pentimento_ui → pentimento_core`；core 是叶子。
2. `pentimento_core` **不依赖 Qt::Widgets、不含任何 UI 头**（现 CMake 已满足：只链
   Core/Gui/Sql/Network）。
3. `pentimento_ui` 以 `PUBLIC` 暴露 `src/ui/` 与 `pentimento_core`，使 exe 能
   `#include "mainwindow.h"`（其公开头里包含 core 头与 `QMainWindow`）。

## 5. 测试目标适配（最小侵入）

- `pentimento_ui_tests`（desktop_keyboard_zoom）：仍需**直接编译** mainwindow 源，以保留
  `PENTIMENTO_TESTING` 定义（mainwindow.h / mainwindow_sources.cpp 里的测试钩子）。源路径
  改为 `src/ui/…`，include 路径加 `src/ui`。
- `library_canvas`/`library_journey`：追加的 canvas 源路径改为 `src/ui/…`。
- 全部 10 个 `library_*` 套件 `#include "compare_canvas.h"/"preview_canvas.h"`（其中 8 个
  仅是 LAY-001 拆分时的残留 include，实际只用于注释说明「需 QApplication」），统一在套件
  include 路径加 `src/ui`。
- 后续项（2026-11-22 完成）：8 个非 canvas 套件的残留 include 已清除（连带移除共享
  注释里的 canvas 措辞），`src/ui` include 路径收敛到 `library_canvas`/
  `library_journey` 两套件。至此核心测试套件不再头文件级依赖 UI 头；tests/ 中引用
  UI 头的仅剩 `library_canvas`/`library_journey`（直接测画布）与
  `mainwindow_test.cpp`（desktop_keyboard_zoom）。

## 6. 验收证据（本批）

- [x] CMake 目标图满足 §4 依赖方向（`grep -E "target_link_libraries"` 核验）。
- [x] `grep -rE "#include.*(mainwindow|_canvas)" src/*.h src/*.cpp`（排除 `src/ui/`）零命中。
- [x] 本机 Linux ctest 全绿（16/16）。
- [x] 本归属表完整覆盖 `src/` 平铺全部文件。
