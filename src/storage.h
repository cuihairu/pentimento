#pragma once

#include <QByteArray>
#include <QString>
#include <QVector>

// Uniform access to resource sources (P-104). Local directories and already
// mounted SMB/NFS directories share one implementation (a mounted path is an
// ordinary local path) but must never be treated as a healthy local disk when
// unreachable or read-only. Direct WebDAV/SFTP adapters are deferred to V1.x
// per the delegated plan and deliberately have no representation here — do not
// add placeholder classes claiming support.
enum class SourceState { Reachable, ReadOnly, Unreachable, PermissionDenied };

struct SourceInfo {
    QString path;             // as given (display); canonical when resolved
    QByteArray contentHash;   // SHA-256 for files; empty for directories
    qint64 size = 0;          // bytes; -1 for directories
    qint64 mtimeMs = 0;       // UTC ms since epoch
    bool isDir = false;
    SourceState state = SourceState::Reachable;
};

// Stat a single file or directory. Never throws; unreachable/permission states
// are reported in the returned SourceInfo instead of failing.
SourceInfo statSource(const QString &path);

// List entries of a directory. On unreachable/permission failure returns empty
// and records the state via the directory's own SourceInfo (returned through
// `directoryState`, may be null). Entries carry size/mtime and, for files,
// state Reachable/ReadOnly; content hashes are filled only for files (identity
// is resolved by the importer, not by listing).
QVector<SourceInfo> listDirectory(const QString &path, SourceInfo *directoryState = nullptr);
