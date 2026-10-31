# pentimento · 画迹

![C++17](https://img.shields.io/badge/C%2B%2B-17-00599C?logo=cplusplus&logoColor=white)
![Qt 6](https://img.shields.io/badge/Qt-6-41CD52?logo=qt&logoColor=white)
![CMake](https://img.shields.io/badge/CMake-3.24+-064F8C?logo=cmake&logoColor=white)
![License](https://img.shields.io/badge/License-Apache--2.0-blue)
![Platform](https://img.shields.io/badge/Platform-Linux%20%7C%20Windows%20%7C%20macOS-lightgrey)
![Status](https://img.shields.io/badge/Status-%E5%8E%9F%E5%9E%8B%E9%98%B6%E6%AE%B5-orange)

面向游戏美术素材的本地桌面资源管理器，以历史版本浏览与恢复为核心。参考 Eagle 的素材整理习惯和 macOS 时光机的版本回溯体验，自行实现代码与界面。

**技术栈：C++17、Qt 6 Widgets、Qt SQL/SQLite，CMake 管理。** 不使用 WebView。

## 当前可用

- 创建/打开本地素材库，批量导入文件（复制导入，不修改原始文件）。
- 素材列表、名称搜索、图片预览和带缩略图的版本时间轴。
- 手动记录版本说明；应用运行时，每两秒检查工作副本，连续两次大小/修改时间稳定后记录变化，相同内容不重复生成版本。
- 浏览、导出历史版本；恢复时新增版本，先保存尚未记录的当前内容，保留原历史。
- SHA-256 内容寻址，相同文件内容复用对象；SQLite 保存索引，QSaveFile 原子写入，库级锁限制单进程访问。
- 恢复/导出校验历史对象哈希；误删工作副本后可从历史恢复。

## 构建与运行

需要 CMake 3.21+、Ninja、支持 C++17 的编译器及 Qt 6.4+（Core、Gui、Widgets、Sql、Test），运行时需要 Qt SQLite 驱动。已在本地 Qt 6.10.2/Linux 构建验证；macOS/Windows 尚待实际构建验证。

```sh
cmake --preset dev
cmake --build --preset dev
ctest --preset dev
./build/dev/pentimento --library /path/to/my-library
```

macOS 的 bundle 入口为 `build/dev/pentimento.app/Contents/MacOS/pentimento`。Qt 安装目录不在默认搜索路径时，配置命令添加 `-DCMAKE_PREFIX_PATH=/path/to/Qt/6.x/platform`。Qt Creator 也可直接打开 `CMakeLists.txt`。

不传 `--library` 时使用 Qt AppLocalDataLocation 下的 `library/`。素材库内有 `catalog.sqlite`、`objects/`、`working/`；通过“编辑工作副本”打开库内文件，编辑完成后自动记录或点击“记录版本”。编辑导入前的原文件不会更新库内副本。

## 安全防线（防勒索病毒）

本地素材库被勒索软件加密/篡改时，产品要能防住、发现、并快速恢复（用户 2026-10-31 新增核心需求）。只做素材库自身防线，不做杀毒引擎/病毒库。

- **防线一：不可变版本库**（已实现）——历史对象按内容寻址 append-only，落盘后置只读（Linux `chattr +i` 可用则用，否则清除写权限位；Windows/macOS 只读属性抽象到平台层，真机验证 TODO）。版本库与工作区分离，恢复单向 库→工作区；加密工作区后历史对象仍可读可恢复。
- **防线二：批量修改哨兵**（已实现）——监测工作区异常批量写入，三个信号（大量文件被改 / 被改扩展名 .png→.enc / 内容熵值异常升高）任一达阈值即告警并自动紧急快照锚点；正常小编辑不触发（阈值测试）。
- **防线三：整树回滚**（已实现）——「恢复到锚点」一键恢复工作区到感染前锚点，走「恢复形成新版本」口径，不销毁后续历史（被加密版本仍保留）。

### Security defense (anti-ransomware)

When a local asset library is encrypted or tampered by ransomware, the product must **prevent, detect, and quickly recover** (new core requirement, 2026-10-31). Only the library's own defense is built — no antivirus engine, no virus database.

- **Layer 1: Immutable version store** (implemented) — history objects are content-addressed and append-only, then set read-only after write (Linux `chattr +i` when permitted, else clear write bits; Windows/macOS read-only attribute abstracted to the platform layer, real-machine verification TODO). Version store is separate from the workspace; restore is one-way store→workspace. Encrypting the workspace leaves history objects readable and restorable.
- **Layer 2: Batch-modification sentinel** (implemented) — watches the workspace for anomalous batch writes; three signals (many files changed / extension churn .png→.enc / abnormally high content entropy) trip an alert at threshold, and an emergency snapshot anchor is taken automatically. Normal small edits stay quiet (threshold test).
- **Layer 3: Whole-tree rollback** (implemented) — "Restore to anchor" restores the workspace to the pre-infection anchor in one action, using the existing "restore forms a new version" semantics; later history (including encrypted versions) is not destroyed.

## 对标调研

对齐 7 款对标产品（Alienbrain、Anchorpoint、Perforce、Eagle、Billfish、Adobe Bridge+Lr、Time Machine/File History）的调研已落库：[docs/zh/design/90-benchmark.md](docs/zh/design/90-benchmark.md)（16 条可借鉴做法逐条复核 V1采纳/V2候选/排除，14 条功能缺口逐条复核登记至 [todo.md](todo.md)「对标缺口复核」）。

## 首版边界

这是可运行的早期原型，不是 Eagle 的完整替代品，也不是系统级备份工具。

- 自动记录仅在应用运行时生效；文件稳定判断是启发式，不保证捕获每次编辑器保存，编辑器中未写盘的内容无法记录。恢复前先关闭外部编辑器，避免它覆盖恢复结果。
- 图片预览取决于本机 Qt 图像插件。PSD、模型等文件可按原始字节保留历史，但当前不解析图层、不渲染模型。单文件限 256 MiB；I/O 与缩略图目前在 UI 线程，小型素材库试用优先。
- 当前为单素材版本历史，尚无整库时间点恢复、元数据历史、并排/滑动对比、标签、批量文件夹导入与 AI 识别。
- 本版使用管理式复制导入；改名/移动追踪、外部目录索引、库迁移、历史清理和远端备份尚未实现。工作文件路径当前为绝对路径，不能直接移动整个库后继续编辑。
- 对象与索引不在同一个跨文件事务中，进程在恢复写盘和数据库提交之间崩溃时，重开后需记录一次当前工作副本。孤立对象暂不清理，避免误删历史。
- 同盘历史不能替代异盘备份；备份时先关闭应用，再完整复制整个素材库。

产品约定见 [PRODUCT.md](PRODUCT.md)，存储与恢复边界见 [docs/architecture.md](docs/architecture.md)，后续任务见 [todo.md](todo.md)。

## 许可

Apache-2.0。Qt 及其他依赖保留各自许可证；应用分发需遵守实际使用的 Qt 授权条件。
