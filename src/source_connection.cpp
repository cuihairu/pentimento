#include "source_connection.h"

#include <QObject>

ConnectionState connectionStateFor(SourceState state) {
    switch (state) {
    case SourceState::Reachable:
    case SourceState::ReadOnly:
        // A read-only mount is still connected; writability is a backup-target
        // concern (P-510), not a connection state.
        return ConnectionState::Reachable;
    case SourceState::Unreachable:
        return ConnectionState::Unreachable;
    case SourceState::PermissionDenied:
        return ConnectionState::Failed;
    }
    return ConnectionState::Failed;
}

QString connectionStateText(ConnectionState state) {
    switch (state) {
    case ConnectionState::Connecting: return QObject::tr("连接中");
    case ConnectionState::Reachable: return QObject::tr("可达");
    case ConnectionState::Unreachable: return QObject::tr("不可达");
    case ConnectionState::Failed: return QObject::tr("失败");
    }
    return QObject::tr("失败");
}

namespace {
QString connectingReason() {
    return QObject::tr("正在探测来源连接；长时间无响应说明挂载可能已卡住。");
}
}

SourceConnection::SourceConnection(QObject *parent)
    : QObject(parent)
    , probe_([](const QString &path) { return statSource(path); }) {
    connect(&queue_, &TaskQueue::taskFinished, this, [this](qint64 id, bool, const QString &) {
        const auto it = pending_.constFind(id);
        if (it == pending_.constEnd()) return;
        const Pending pending = *it; // by value: never hold a reference into the map
        pending_.erase(it);
        // A stale result (the path changed while this probe was in flight) is
        // dropped, never applied to the new path.
        if (pending.path != path_) return;
        probing_ = !pending_.isEmpty();
        applyResult(pending.result->state);
    });
}

SourceConnection::~SourceConnection() {
    // Stop callbacks before the members die: the TaskQueue worker drains the
    // remaining tasks during its own destruction, and with this disconnected no
    // result reaches the half-destroyed connection.
    disconnect(&queue_, nullptr, this, nullptr);
}

void SourceConnection::open(const QString &path) {
    const QString trimmed = path.trimmed();
    if (trimmed.isEmpty()) return;
    path_ = trimmed;
    probing_ = true;
    state_ = ConnectionState::Connecting;
    reason_ = connectingReason();
    emit stateChanged(state_, reason_);
    startProbe();
}

void SourceConnection::retry() {
    // One probe at a time, user-paced: while a probe is in flight (or queued
    // behind one) a retry is a no-op — the V1 backoff rule. No timer ever
    // re-probes, so there is no automatic retry loop to overwrite anything.
    if (path_.isEmpty() || probing_) return;
    probing_ = true;
    state_ = ConnectionState::Connecting;
    reason_ = connectingReason();
    emit stateChanged(state_, reason_);
    startProbe();
}

void SourceConnection::setProbeFn(ProbeFn fn) {
    if (fn) probe_ = std::move(fn);
}

void SourceConnection::startProbe() {
    Pending pending;
    pending.path = path_;
    pending.result = std::make_shared<SourceInfo>();
    const qint64 id = queue_.submit(QObject::tr("来源探测"),
        [this, path = pending.path, result = pending.result](TaskQueue::Progress &) {
            // Read-only: statSource never creates, writes, or removes anything
            // on the source; a retry therefore cannot overwrite remote content.
            try {
                *result = probe_(path);
            } catch (...) {
                // A throwing probe is a failed probe, not "the source is gone".
                result->state = SourceState::PermissionDenied;
            }
        });
    pending_.insert(id, pending);
}

void SourceConnection::applyResult(SourceState probed) {
    state_ = connectionStateFor(probed);
    switch (state_) {
    case ConnectionState::Reachable:
        reason_.clear();
        break;
    case ConnectionState::Unreachable:
        reason_ = QObject::tr("来源当前不可达（可能断网或挂载已卸载）；不会被视为删除，"
                              "重试只重新探测、不改动来源内容。");
        break;
    case ConnectionState::Failed:
        reason_ = QObject::tr("来源存在但当前不可读（权限不足或挂载异常）。");
        break;
    case ConnectionState::Connecting:
        break; // never produced by a probe result
    }
    emit stateChanged(state_, reason_);
}
