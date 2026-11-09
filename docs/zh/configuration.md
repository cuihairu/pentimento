# 配置

pentimento 在启动参数和代码层面配置，目前没有设置文件。

## 命令行参数

| 参数 | 作用 |
| --- | --- |
| `--library <path>` | 打开 `<path>` 处的素材库。 |
| `--screenshot <path>` | 保存一张界面截图后退出（冒烟检查）。 |
| `--help`、`--version` | Qt 标准参数。 |

不传 `--library` 时，优先使用最近一次打开且仍在磁盘上的库，否则在 Qt `AppLocalDataLocation` 下创建 `library/`。

## 素材库结构

一个素材库目录包含：

```text
catalog.sqlite   索引（素材 + 版本）
objects/         SHA-256 内容寻址的不可变对象
working/         在外部编辑器中编辑的工作副本
library.lock     单进程写锁
```

## 哨兵阈值（防勒索）

批量修改哨兵将每个扫描窗口与基线比较，任一信号超过阈值即告警。阈值定义在 `SentinelConfig`（`src/sentinel.h`），嵌入库层前可在代码中调整：

| 信号 | 默认值 | 含义 |
| --- | --- | --- |
| 批量修改 | 25 个文件 | 一个扫描窗口内被修改的文件数 |
| 扩展名变更 | 10 个文件 | 出现基线未有的新扩展名的文件数 |
| 高熵内容 | 10 个文件 | 超过熵上限的被修改文件数 |
| 熵上限 | 7.2 bits/byte | 加密内容接近 8.0 上限 |

设计倾向「宁可误报」：正常小编辑不触发，合法大改可能触发并需用户确认。

## 构建配置

- 构建 preset 在 `CMakePresets.json`（`dev` 配置/构建/测试 preset）；CMake 是唯一构建入口，所有目标 ISO C++17、禁编译器扩展。
- Qt 不在默认搜索路径时传 `-DCMAKE_PREFIX_PATH=/path/to/Qt/6.x/platform`。
- 语言偏好保存在 QSettings（`ui/language`，默认 `zh_CN`），启动时加载；应用内尚无语言切换入口（`setLanguagePreference` 无界面调用方），切换需在应用外修改该设置。
- 运行时快照周期（关闭/15/30/60 分钟）是 UI data，不是持久化设置；周期附加在下拉项上（`src/mainwindow.cpp:354`），并应用到 `SnapshotSchedule`（`src/mainwindow_snapshots.cpp:50`）。
- 哨兵阈值是 `SentinelConfig`（`src/sentinel.h:12`）中的代码默认值；职责分层见[架构](./architecture.md)，术语见[术语表](./glossary.md)。
