#pragma once

#include <QHash>
#include <QString>

// Version-backend capability model (P-103). Each independent capability is
// queried separately so a single "supports history" boolean cannot mask
// differences in tree snapshots, locking, or remote fetch. The self-built
// backend is the only V1 backend; Git/SVN (read-only) and Time Machine are
// deferred to V1.x and must never be assumed writable through this interface.
enum class Capability { History, SnapshotTree, Restore, Lock, RemoteFetch };

struct BackendInfo {
    QString name;                 // "self" (V1); future: "git", "svn", "time-machine"
    bool writable = false;        // V1: self only
    QHash<Capability, bool> capabilities;
    bool supports(Capability c) const { return capabilities.value(c, false); }
    // Stable, human-readable reason for an unsupported capability. The UI uses
    // it (P-409) to hide the corresponding action instead of offering a broken
    // path; the text is part of the stable error contract.
    QString unsupportedReason(Capability c) const;
};

// V1 self-built backend capability report. The self backend is writable and
// exposes history, whole-tree snapshots, restore, and a single-process write
// lock; it has no remote objects to fetch.
BackendInfo selfBackendInfo();
