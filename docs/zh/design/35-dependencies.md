# 依赖管理策略

> 状态：**现行约定（2026-10-31）**。CMake 是依赖管理的单一来源：所有外部依赖在根 `CMakeLists.txt` 的「依赖管理」节声明，其余任何地方（CI 脚本、README、文档）不得另立安装口径。

## 1. 当前依赖清单

| 依赖 | 版本下限 | 组件 | 发现方式 | 用途 |
|---|---|---|---|---|
| Qt 6 | 6.4 | Core、Gui、Widgets、Sql、Test | `find_package` | 框架：界面、QSqlite 持久层、QTemporaryDir/QSaveFile、QTest |
| CMake | 3.24 | — | — | 构建与依赖声明 |

无其他三方库。SQLite 经 Qt SQL 驱动（`libqt6sql6-sqlite`）使用，不直接链接 sqlite3。

## 2. 各平台安装 Qt（仅此一项需要系统安装）

```bash
# Linux (Debian/Ubuntu)
sudo apt install qt6-base-dev qt6-base-dev-tools libqt6sql6-sqlite ninja-build
# macOS
brew install qt
# Windows: Qt 官方在线安装器（选 MSVC 2022 组件）或 vcpkg install qtbase:x64-windows
```

CI（.github/workflows/nightly.yml）只允许出现 Qt 相关的包安装，外加构建工具 `ninja-build`（Ninja 生成器所需）；缺驱动类问题（如 QSQLITE not loaded）优先怀疑发行版把组件拆成了独立包。

## 3. 新增三方 C++ 库的规则

- 一律走 `CMakeLists.txt` 的 `fetch_dep(<名字> GIT_REPOSITORY ... GIT_TAG <锁定的 tag>)`（FetchContent），**禁止**手工 vendor 源码进仓、禁止在 CI 或文档里要求用户散装 `apt install <三方库>`。
- 锁定具体 tag（不用分支名），升级依赖=改 tag + 全测试绿，单独一个 commit。
- 同名依赖只取第一次声明（`fetch_dep` 已内置，按 `fetch_dep(<名字>)` 的调用名去重：首次声明后即设置 GLOBAL 属性 `FETCH_DEP_<名字>_DONE`，该属性跨函数/子目录作用域可见，不依赖依赖自身是否提供同名 CMake target，纯头文件/别名/不同名 target 依赖同样按 `<名字>` 去重），防止传递依赖重复拉取。
- 新增时在本文件第 1 节表格登记一行，并在该 commit 的说明里写选型理由（为什么不用 std/Qt 自带能力）。

## 4. 明确不做

- 不引入 vcpkg/conan 全局清单（当前规模 FetchContent 足够；若三方依赖超过 5 个再评估）。
- 不做 Qt 的源码级 FetchContent（框架级依赖，系统/官方安装器是正确路径）。
