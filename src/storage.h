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

// ReadOnly means "the path exists, is readable, but the process cannot write
// it" (mode bits / read-only mount). It applies to BOTH files and directories:
// a read-only mounted SMB/NFS source is still listable and importable, so it
// maps to a connected source (see source_connection) — writability is a
// backup-target question (P-510), never a "disk is healthy" verdict. Callers
// must not treat ReadOnly as Unreachable/PermissionDenied, and must not assume
// a Reachable directory is writable.

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

// P-510: backup-target write acceptance. The source-read acceptance (reachable
// / readable) is the SourceConnection state machine; a backup target must be
// separately verified as writable with usable free space — a source that is
// readable but read-only is NOT a valid backup target, and "can be listed" is
// never a backup-capability verdict.
struct BackupTargetInfo {
    bool exists = false;        // the path exists (or can be created?)
    bool writable = false;      // the process can write into it
    bool readOnly = false;      // explicit read-only mount / mode bits
    qint64 freeBytes = 0;       // bytes available to this user on the volume
    QString reason;             // non-empty when not writable
};

// Probe a backup-target directory for writability and free space. Read-only:
// never creates or modifies anything on the target (a probe must not mutate the
// very volume it is checking).
BackupTargetInfo probeBackupTarget(const QString &path);
