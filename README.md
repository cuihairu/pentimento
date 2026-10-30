# pentimento · 画迹

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
