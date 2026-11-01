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

HistoryActions historyActionsFor(const BackendInfo &info, bool selectionValid) {
    HistoryActions actions;
    // Restore writes a historical version back as a new version — only offered
    // when the backend supports it. A read-only backend must never show the
    // write-back button (P-409); the reason is surfaced instead.
    actions.restore = selectionValid && info.supports(Capability::Restore);
    if (selectionValid && !info.supports(Capability::Restore))
        actions.restoreReason = info.unsupportedReason(Capability::Restore);
    // Extract reads a historical copy out of the library — gated by History.
    actions.extract = selectionValid && info.supports(Capability::History);
    if (selectionValid && !info.supports(Capability::History))
        actions.extractReason = info.unsupportedReason(Capability::History);
    return actions;
}
