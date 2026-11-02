# 快速开始

## 环境要求

- CMake 3.24+
- Ninja
- 支持 C++17 的编译器
- Qt 6.4+（Core、Gui、Widgets、Sql、Test），运行时需要 Qt SQLite 驱动

已在本地 Qt 6.10.2 / Linux 构建验证；macOS 与 Windows 尚待真机构建验证。

## 构建与运行

```sh
cmake --preset dev
cmake --build --preset dev
ctest --preset dev
./build/dev/pentimento --library /path/to/my-library
```

macOS 的 bundle 入口为 `build/dev/pentimento.app/Contents/MacOS/pentimento`。Qt 安装目录不在默认搜索路径时，配置命令添加 `-DCMAKE_PREFIX_PATH=/path/to/Qt/6.x/platform`。Qt Creator 也可直接打开 `CMakeLists.txt`。

## 应用内第一步

1. `--library` 指定素材库目录。不传时优先回退到最近使用且仍存在的库，否则在 Qt `AppLocalDataLocation` 下创建 `library/`。
2. 素材库内有 `catalog.sqlite`、`objects/`、`working/`。
3. 批量导入文件——文件被复制进库，原文件不动。
4. 通过「编辑工作副本」打开库内文件，在外部编辑器中修改后变化会被自动记录（或点击「记录版本」）。

## 当前可用

- 素材列表、名称搜索、图片预览和带缩略图的版本时间轴。
- 手动记录版本说明；相同内容不重复生成版本。
- 浏览、导出历史版本；恢复保留全部既有历史。
- 恢复/导出校验历史对象哈希；误删工作副本后可从历史恢复。

## 首版边界

这是可运行的早期原型，不是完整素材管理器替代品，也不是系统级备份工具。自动记录仅在应用运行时生效；PSD、模型等文件按原始字节保留历史，不解析图层、不渲染模型；标签、批量文件夹导入与 AI 识别尚未提供。同盘历史不能替代异盘备份——备份时先关闭应用，再完整复制整个素材库。恢复相关的边界见[配置](/zh/configuration)。
