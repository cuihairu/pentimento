# 防勒索病毒安全防线（2026-10-31 新增核心需求）

> 状态：**防线一已实现（Linux 验证环境）；防线二/三随任务推进**。依据用户 2026-10-31 令：本地素材库被勒索软件加密/篡改时，产品要能**防住、发现、并快速恢复**。范围外不做杀毒引擎、不做病毒库——只做素材库自身的防线：不可变、发现、恢复。
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
