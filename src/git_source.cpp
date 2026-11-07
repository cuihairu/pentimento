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

// .git/lfs/objects/aa/bb/<rest> — the content store for one LFS object.
QString lfsObjectPath(const QString &repoPath, const QString &oid) {
    if (oid.size() < 4) return QString();
    return QDir(repoPath).filePath(QStringLiteral(".git/lfs/objects/%1/%2/%3")
        .arg(oid.left(2), oid.mid(2, 2), oid.mid(4)));
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
        if (error) *error = trimmed(runGitOut(path, {QStringLiteral("for-each-ref")}));
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
        if (error) *error = trimmed(runGitOut(path, {QStringLiteral("status")}));
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

} // namespace git_source
