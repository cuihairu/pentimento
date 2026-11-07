#pragma once

#include <QByteArray>
#include <QString>
#include <QStringList>
#include <QVector>

// P-401..P-405: read-only Git repository access. This module talks to a
// local working tree through the `git` binary and NEVER issues a mutating
// command: the command set is deliberately limited to rev-parse / log /
// ls-tree / show / cat-file / status --porcelain / config --get (all
// read-only). LFS object fetching (P-405) is the single exception and is
// only reachable through fetchLfs(), which the caller must gate behind
// explicit user consent — the method itself never runs implicitly.
//
// Every function is synchronous, never throws, and reports failure through
// an output `error` (QString*) — empty on success. The caller decides the
// thread (the UI runs these off the main thread; see mainwindow).
namespace git_source {

struct RepoInfo {
    QString topLevel;      // canonical working-tree root (P-401)
    QString currentBranch; // short branch name, empty when detached
    QString headCommit;    // full HEAD sha
    bool detached = false;
    bool bare = false;
    QString error;         // non-empty = not a connectable repo at that path
};

struct Branch {
    QString name;      // short branch name
    QString head;      // commit sha
    bool isCurrent = false;
};

struct LogEntry {
    QString commit;    // full sha
    QString subject;   // one-line subject
    QString author;    // author name
    QString email;
    qint64 whenMs = 0; // committer time, ms UTC
};

struct StatusEntry {
    QString path;      // repo-relative
    QString code;      // porcelain XY, e.g. " M", "??", "MM"
};

struct TreeEntry {
    QString path;      // repo-relative
    QString mode;      // "100644" file, "100755" executable, "120000" symlink
    QString type;      // "blob", "tree", "commit" (gitlink)
    QString hash;      // object sha
    qint64 size = -1;  // blob size; -1 when not a blob
};

// P-401: identify a local repository. Verifies the directory is inside a
// git work tree and reads HEAD state. Read-only: no refs, no worktree
// change.
RepoInfo identify(const QString &path);

// P-401: local branches with their head commits, current branch flagged.
QVector<Branch> branches(const QString &path, QString *error = nullptr);

// P-401: worktree status (porcelain). Only reads the index and worktree.
QVector<StatusEntry> status(const QString &path, QString *error = nullptr);

// P-402: commit log (newest first), limited to `limit` entries.
QVector<LogEntry> log(const QString &path, int limit = 50, QString *error = nullptr);

// P-402: recursive file tree of one commit. `subdir` narrows the listing
// ("" = whole tree). Paths are repo-relative and stable across commits,
// which is what associates an asset path with its history.
QVector<TreeEntry> tree(const QString &path, const QString &commit,
                        const QString &subdir = QString(), QString *error = nullptr);

// P-403: bytes of `filePath` (repo-relative) at `commit`. A file that is a
// git-lfs pointer returns the pointer text (never silently the object);
// when the LFS object is already present locally the real bytes are read
// from .git/lfs/objects without touching the worktree.
bool extract(const QString &path, const QString &commit, const QString &filePath,
             QByteArray *bytes, QString *error = nullptr);

// P-404: a git-lfs pointer is a small text stanza, never image bytes.
bool isLfsPointer(const QByteArray &bytes);

// P-404: whether an object is present locally (cat-file -e, or the LFS
// object under .git/lfs/objects for a pointer file).
bool objectPresent(const QString &path, const QString &filePath, const QString &commit,
                   QString *error = nullptr);

// P-405: fetch a single LFS object. Callers MUST obtain explicit user
// consent before calling; the method only enforces that nothing in the
// working tree is written (git lfs fetch touches .git/lfs only). Returns
// false and keeps the object absent on failure.
bool fetchLfs(const QString &path, const QString &filePath, const QString &commit,
              QString *error = nullptr);

} // namespace git_source
