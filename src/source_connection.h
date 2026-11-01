#pragma once

#include "storage.h"
#include "taskqueue.h"

#include <QMap>
#include <QObject>
#include <QString>

#include <functional>
#include <memory>

// P-513: mounted-source reconnection state machine.
//
// V1 consumes a user-mounted SMB/NFS directory as an import source. A mount can
// drop at any time (network loss, server sleep, unmount), and a stat on a dead
// mount can block for a long time — so the probe always runs on a background
// worker and the UI thread never touches the source path.
//
// Connection states (visible as 连接中 / 可达 / 不可达 / 失败):
//   Connecting  — a probe is in flight (also while queued behind a still-
//                 running probe: the serial queue is the V1 backoff — one
//                 probe at a time, user-paced, no automatic retry loop).
//   Reachable   — the path exists and is listable. A read-only mount is still
//                 connected; writability is a separate backup-target concern
//                 (P-510), never a connection state.
//   Unreachable — the path is missing right now (network drop / unmount). This
//                 is never interpreted as a remote deletion, and nothing is
//                 written back: the only source-side call is a read-only stat,
//                 so a retry cannot overwrite remote content (P-515 keeps the
//                 later rescan read-only too).
//   Failed      — the path exists but cannot be read (permissions / mount
//                 error), or the probe itself failed.
//
// V1 never auto-remounts and never retries on a timer; retry() is user-paced
// and ignored while a probe is in flight.
enum class ConnectionState { Connecting, Reachable, Unreachable, Failed };
Q_DECLARE_METATYPE(ConnectionState)

// Map a storage-level probe (statSource) to a connection state.
ConnectionState connectionStateFor(SourceState state);

// UI-facing state word: 连接中 / 可达 / 不可达 / 失败.
QString connectionStateText(ConnectionState state);

class SourceConnection : public QObject {
    Q_OBJECT
public:
    using ProbeFn = std::function<SourceInfo(const QString &path)>;

    explicit SourceConnection(QObject *parent = nullptr);
    // The probe worker drains before the members die (like TaskQueue): a probe
    // blocked by a dead hard mount delays teardown — V1 accepts this, and the
    // UI thread never blocks on a probe regardless (soft-failing mounts return
    // promptly).
    ~SourceConnection() override;

    QString path() const { return path_; }
    ConnectionState state() const { return state_; }
    QString reason() const { return reason_; } // non-empty unless Reachable
    bool probing() const { return probing_; }

    // Start watching a mounted path: transitions to Connecting and probes off
    // the UI thread. Changing the path while a probe is in flight queues the
    // new probe behind it; only results for the current path are applied.
    void open(const QString &path);
    // User retry: re-probe the current path. Ignored while a probe is in flight
    // (one probe at a time — the V1 backoff rule; no fast retry loop).
    void retry();

    // Test seam: replace the probe (default is a plain statSource).
    void setProbeFn(ProbeFn fn);

signals:
    void stateChanged(ConnectionState state, const QString &reason);

private:
    struct Pending {
        QString path;
        std::shared_ptr<SourceInfo> result;
    };
    void startProbe();
    void applyResult(SourceState probed);

    QString path_;
    ConnectionState state_ = ConnectionState::Unreachable;
    QString reason_;
    bool probing_ = false;
    ProbeFn probe_;
    QMap<qint64, Pending> pending_; // task id → probe in flight
    TaskQueue queue_;               // declared last: its worker drains first
};
