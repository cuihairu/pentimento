#include "git_source.h"

#include <QDateTime>
#include <QDir>
#include <QProcess>
#include <QRegularExpression>

namespace {

// Run `git <args>` with the repository working directory set to `repoPath`.
// Returns the exit code; stdout/stderr are captured for the caller. The
// command line is passed as argv (no shell), and the call is bounded so a
// hung git cannot freeze the caller forever.
int runGit(const QString &repoPath, const QStringList &args,
           QByteArray *stdoutData = nullptr, QByteArray *stderrData = nullptr) {
    QProcess git;
    git.setProgram(QStringLiteral("git"));
    git.setArguments(args);
    git.setWorkingDirectory(repoPath);
    git.start();
    if (!git.waitForStarted(5000)) {
        if (stderrData) *stderrData = QByteArrayLiteral("git: could not start");
        return -1;
    }
    if (!git.waitForFinished(30000)) {
        git.kill();
        git.waitForFinished(2000);
        if (stderrData) *stderrData = QByteArrayLiteral("git: timed out");
        return -1;
    }
    if (stdoutData) *stdoutData = git.readAllStandardOutput();
    if (stderrData) *stderrData = git.readAllStandardError();
    return git.exitCode();
}

QByteArray runGitOut(const QString &repoPath, const QStringList &args) {
    QByteArray out;
    runGit(repoPath, args, &out, nullptr);
    return out;
}

QString trimmed(const QByteArray &data) {
    return QString::fromUtf8(data).trimmed();
}

// Error text for a failed git call: git writes errors to stderr, so read
// that first and fall back to stdout (some commands report on stdout).
QString gitErrorText(const QString &repoPath, const QStringList &args) {
    QByteArray out, err;
    runGit(repoPath, args, &out, &err);
    QString text = trimmed(err);
    if (text.isEmpty()) text = trimmed(out);
    return text;
}

// .git/lfs/objects/aa/bb/<oid> — git-lfs stores each object under two
// two-hex-char directories with the FULL oid as the file name.
QString lfsObjectPath(const QString &repoPath, const QString &oid) {
    if (oid.size() < 4) return QString();
    return QDir(repoPath).filePath(QStringLiteral(".git/lfs/objects/%1/%2/%3")
        .arg(oid.left(2), oid.mid(2, 2), oid));
}

// "oid sha256:<64 hex>" from a git-lfs pointer stanza, empty when absent.
QString lfsOidFromPointer(const QByteArray &bytes) {
    const QRegularExpression re(QStringLiteral("^oid sha256:([0-9a-f]{64})$"),
                                QRegularExpression::MultilineOption);
    const auto m = re.match(QString::fromUtf8(bytes));
    return m.hasMatch() ? m.captured(1) : QString();
}

} // namespace

namespace git_source {

RepoInfo identify(const QString &path) {
    RepoInfo info;
    QByteArray out;
    QByteArray err;
    // git writes errors to stderr, so the failure path must read stderr —
    // otherwise a non-repo path would report success with an empty error.
    if (runGit(path, {QStringLiteral("rev-parse"), QStringLiteral("--show-toplevel")},
               &out, &err) != 0) {
        info.error = trimmed(err);
        if (info.error.isEmpty()) info.error = trimmed(out);
        return info;
    }
    info.topLevel = trimmed(out);
    const QByteArray bare = runGitOut(path, {QStringLiteral("rev-parse"),
                                             QStringLiteral("--is-bare-repository")});
    info.bare = (trimmed(bare) == QLatin1String("true"));
    const QByteArray branch = runGitOut(path, {QStringLiteral("symbolic-ref"),
                                               QStringLiteral("--short"), QStringLiteral("HEAD")});
    if (branch.isEmpty()) {
        info.detached = true;
    } else {
        info.currentBranch = trimmed(branch);
    }
    info.headCommit = trimmed(runGitOut(path, {QStringLiteral("rev-parse"), QStringLiteral("HEAD")}));
    return info;
}

QVector<Branch> branches(const QString &path, QString *error) {
    QVector<Branch> result;
    QByteArray out;
    if (runGit(path, {QStringLiteral("for-each-ref"),
                      QStringLiteral("--format=%(refname:short)%00%(objectname)%00%(HEAD)"),
                      QStringLiteral("refs/heads")},
               &out, nullptr) != 0) {
        if (error) *error = gitErrorText(path, {QStringLiteral("for-each-ref")});
        return result;
    }
    const QString current = identify(path).currentBranch;
    // for-each-ref separates records with newlines, and each record is
    // "name<NUL>sha<NUL>HEAD" — so split per line first, then on NUL.
    for (const QByteArray &record : out.split('\n')) {
        const QStringList fields = QString::fromUtf8(record).split(QChar('\0'));
        if (fields.size() < 1 || fields.at(0).isEmpty()) continue;
        const QString name = fields.at(0);
        Branch b;
        b.name = name;
        b.head = fields.value(1);
        b.isCurrent = (name == current);
        result.push_back(b);
    }
    if (error) error->clear();
    return result;
}

QVector<StatusEntry> status(const QString &path, QString *error) {
    QVector<StatusEntry> result;
    QByteArray out;
    // --no-optional-locks: git status normally refreshes the index stat
    // cache, which would write to .git/ — "read-only" must mean truly
    // read-only, so the index is left untouched.
    if (runGit(path, {QStringLiteral("-c"), QStringLiteral("core.quotepath=false"),
                      QStringLiteral("--no-optional-locks"),
                      QStringLiteral("status"), QStringLiteral("--porcelain")},
               &out, nullptr) != 0) {
        if (error) *error = gitErrorText(path, {QStringLiteral("status")});
        return result;
    }
    for (const QByteArray &line : out.split('\n')) {
        const QString text = QString::fromUtf8(line);
        if (text.size() < 4) continue;
        StatusEntry entry;
        entry.code = text.left(2);
        entry.path = text.mid(3);
        result.push_back(entry);
    }
    if (error) error->clear();
    return result;
}

QVector<LogEntry> log(const QString &path, int limit, QString *error) {
    QVector<LogEntry> result;
    if (limit < 1) limit = 1;
    QByteArray out;
    // %x1f separates fields, %x1e separates records: subject/author may
    // contain spaces but almost never RS/US, so this parses unambiguously.
    // %at is committer time (unix seconds).
    if (runGit(path, {QStringLiteral("log"),
                      QStringLiteral("--format=%H%x1f%an%x1f%ae%x1f%at%x1f%s%x1e"),
                      QStringLiteral("--max-count=%1").arg(limit)},
               &out, nullptr) != 0) {
        if (error) *error = gitErrorText(path, {QStringLiteral("log")});
        return result;
    }
    for (const QByteArray &record : out.split(char(0x1e))) {
        const QStringList fields = QString::fromUtf8(record).split(QChar(0x1f));
        if (fields.size() < 5 || fields.at(0).isEmpty()) continue;
        LogEntry entry;
        entry.commit = fields.at(0);
        entry.author = fields.at(1);
        entry.email = fields.at(2);
        bool ok = false;
        const qint64 secs = fields.at(3).toLongLong(&ok);
        entry.whenMs = ok ? secs * 1000 : 0;
        entry.subject = fields.at(4);
        result.push_back(entry);
    }
    if (error) error->clear();
    return result;
}

QVector<TreeEntry> tree(const QString &path, const QString &commit,
                        const QString &subdir, QString *error) {
    QVector<TreeEntry> result;
    const QString rev = commit.isEmpty() ? QStringLiteral("HEAD") : commit;
    QStringList args{QStringLiteral("-c"), QStringLiteral("core.quotepath=false"),
                     QStringLiteral("ls-tree"), QStringLiteral("-r"),
                     QStringLiteral("--long"), QStringLiteral("--full-tree"), rev};
    if (!subdir.isEmpty()) {
        args << QStringLiteral("--") << subdir;
    }
    QByteArray out;
    if (runGit(path, args, &out, nullptr) != 0) {
        if (error) *error = gitErrorText(path, {QStringLiteral("ls-tree"), rev});
        return result;
    }
    // Each line: "<mode> <type> <sha> <size>\t<path>".
    for (const QByteArray &line : out.split('\n')) {
        const int tab = line.indexOf('\t');
        if (tab < 0) continue;
        const QString meta = QString::fromUtf8(line.left(tab));
        const QString entryPath = QString::fromUtf8(line.mid(tab + 1));
        // --long right-aligns the blob size with spaces, so skip empties.
        const QStringList parts = meta.split(QRegularExpression(QStringLiteral("\\s+")),
                                             Qt::SkipEmptyParts);
        if (parts.size() < 3) continue;
        TreeEntry entry;
        entry.mode = parts.at(0);
        entry.type = parts.at(1);
        entry.hash = parts.at(2);
        if (entry.type == QLatin1String("blob") && parts.size() >= 4) {
            bool ok = false;
            const qint64 size = parts.at(3).toLongLong(&ok);
            entry.size = ok ? size : -1;
        }
        entry.path = entryPath;
        result.push_back(entry);
    }
    if (error) error->clear();
    return result;
}

bool isLfsPointer(const QByteArray &bytes) {
    const QString text = QString::fromUtf8(bytes);
    return text.startsWith(QLatin1String("version https://git-lfs.github.com/spec/v1"))
        && text.contains(QLatin1String("oid sha256:"));
}

bool extract(const QString &path, const QString &commit, const QString &filePath,
             QByteArray *bytes, QString *error) {
    const QString rev = commit.isEmpty() ? QStringLiteral("HEAD") : commit;
    QByteArray out, err;
    // <rev>:<path> resolves in the object DB, not the worktree — read-only.
    if (runGit(path, {QStringLiteral("show"), rev + QLatin1Char(':') + filePath},
               &out, &err) != 0) {
        if (error) {
            *error = trimmed(err);
            if (error->isEmpty()) *error = trimmed(out);
        }
        return false;
    }
    *bytes = out;
    // A git-lfs pointer is returned as the pointer text (never silently the
    // object); when the object is already stored locally, hand back the real
    // bytes from .git/lfs/objects without touching the worktree.
    if (isLfsPointer(out)) {
        const QString oid = lfsOidFromPointer(out);
        const QString objPath = lfsObjectPath(path, oid);
        if (!oid.isEmpty() && QFile::exists(objPath)) {
            QFile obj(objPath);
            if (obj.open(QIODevice::ReadOnly)) *bytes = obj.readAll();
        }
    }
    if (error) error->clear();
    return true;
}

bool objectPresent(const QString &path, const QString &filePath,
                   const QString &commit, QString *error) {
    QByteArray bytes;
    if (!extract(path, commit, filePath, &bytes, error)) return false;
    // extract() returns the pointer text when the LFS object is not stored
    // locally and the real bytes when it is — so a pointer result means the
    // object is absent, and any non-pointer result means it is present.
    if (isLfsPointer(bytes)) return false;
    if (error) error->clear();
    return true;
}

bool fetchLfs(const QString &path, const QString &filePath,
              const QString &commit, QString *error) {
    const QString rev = commit.isEmpty() ? QStringLiteral("HEAD") : commit;
    // git lfs fetch takes [options] [remote [ref]] — the first positional is
    // the remote NAME (not a ref), so detect it instead of assuming origin.
    QString remote = trimmed(runGitOut(path, {QStringLiteral("remote")}));
    const int nl = remote.indexOf(QLatin1Char('\n'));
    if (nl >= 0) remote = remote.left(nl);
    QStringList args{QStringLiteral("lfs"), QStringLiteral("fetch"),
                     QStringLiteral("--include=") + filePath};
    if (!remote.isEmpty()) args << remote;
    args << rev;
    QByteArray out, err;
    // git lfs fetch downloads objects into .git/lfs/objects only — the
    // worktree is never touched. The include glob narrows it to one file.
    if (runGit(path, args, &out, &err) != 0) {
        if (error) {
            *error = trimmed(err);
            if (error->isEmpty()) *error = trimmed(out);
        }
        return false;
    }
    if (error) error->clear();
    return true;
}

} // namespace git_source
