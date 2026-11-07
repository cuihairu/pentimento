#pragma once

#include <QByteArray>
#include <QString>
#include <QStringList>
#include <QVector>

// P-406..P-408: read-only SVN repository access through the `svn` binary.
// The command set is deliberately limited to info / log / ls / cat / status
// (all read-only) and every invocation passes --non-interactive so a network
// repo never hangs on an auth prompt, plus --no-auth-cache so nothing is
// written to the user's ~/.subversion. No commit, no update, no lock/unlock.
//
// Every function is synchronous, never throws, and reports failure through
// an output `error` (QString*) — empty on success. The caller decides the
// thread. Auth failure vs read-only permission are surfaced by passing the
// svn error text through unchanged.
namespace svn_source {

struct RepoInfo {
    QString url;          // item URL (file:// or svn:// or https://)
    QString repoRoot;     // repository root URL
    qint64 revision = -1; // working-copy revision of the item
    qint64 headRevision = -1; // repository HEAD revision
    QString kind;         // "file" or "dir"
    bool workingCopy = false; // true when `path` is a checkout
    QString error;        // non-empty = not a connectable repo at that path
};

struct LogEntry {
    qint64 revision = -1;
    QString author;
    QString message;      // full log message
    qint64 whenMs = 0;    // commit time, ms UTC
};

struct TreeEntry {
    QString path;         // repo-relative
    QString kind;         // "file" or "dir"
    qint64 size = -1;     // file size; -1 for dirs
    qint64 rev = -1;      // last-changed revision
    QString author;
};

struct LockInfo {
    bool locked = false;  // false = no lock (≠ "no permission to read lock")
    QString token;
    QString owner;
    QString comment;
    qint64 createdMs = 0;
};

// P-406: identify a working copy or URL. Verifies it is a reachable SVN
// repository and reads URL / HEAD revision. Read-only.
RepoInfo identify(const QString &path);

// P-407: repository log (newest first), limited to `limit` entries.
QVector<LogEntry> log(const QString &path, int limit = 50, QString *error = nullptr);

// P-407: recursive listing of one revision. `subdir` narrows the listing
// ("" = whole tree). Paths are repo-relative and stable across revisions.
QVector<TreeEntry> tree(const QString &path, qint64 revision = -1,
                        const QString &subdir = QString(), QString *error = nullptr);

// P-408: bytes of `filePath` (repo-relative) at `revision`, read from the
// repository — the working copy is never touched or overwritten.
bool extract(const QString &path, qint64 revision, const QString &filePath,
             QByteArray *bytes, QString *error = nullptr);

// P-407: lock state of an item. An absent <lock> block means "no lock".
LockInfo lockInfo(const QString &path, QString *error = nullptr);

} // namespace svn_source
