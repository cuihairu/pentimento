#include "backend.h"

QString BackendInfo::unsupportedReason(Capability c) const {
    switch (c) {
    case Capability::History: return QStringLiteral("此后端不提供版本历史");
    case Capability::SnapshotTree: return QStringLiteral("此后端不提供整库快照/提交树");
    case Capability::Restore: return QStringLiteral("此后端只读，不能生成新版本恢复");
    case Capability::Lock: return QStringLiteral("此后端不暴露锁信息");
    case Capability::RemoteFetch: return QStringLiteral("此后端无远端对象按需获取");
    }
    return QStringLiteral("此能力未实现");
}

BackendInfo selfBackendInfo() {
    BackendInfo info;
    info.name = QStringLiteral("self");
    info.writable = true;
    info.capabilities.insert(Capability::History, true);
    info.capabilities.insert(Capability::SnapshotTree, true);
    info.capabilities.insert(Capability::Restore, true);
    info.capabilities.insert(Capability::Lock, true);       // single-process write lock (library.lock)
    info.capabilities.insert(Capability::RemoteFetch, false);
    return info;
}
