// P-401..P-405: read-only Git source access. Every test builds a throwaway
// repository with the real `git` binary and asserts that the module only
// reads: no worktree change, no .git/HEAD/ref change, and the LFS consent
// gate (P-405) keeps objects absent until an explicit fetch.
#include "git_source.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QProcess>
#include <QTemporaryDir>
#include <QtTest>

namespace {

bool run(const QString &dir, const QStringList &args, QByteArray *out = nullptr) {
    QProcess git;
    git.setProgram(QStringLiteral("git"));
    git.setArguments(args);
    git.setWorkingDirectory(dir);
    git.start();
    if (!git.waitForStarted(5000)) return false;
    if (!git.waitForFinished(30000)) { git.kill(); git.waitForFinished(2000); return false; }
    if (out) *out = git.readAllStandardOutput();
    return git.exitCode() == 0;
}

bool runEnv(const QString &dir, const QStringList &args,
            const QProcessEnvironment &env, QByteArray *out = nullptr) {
    QProcess git;
    git.setProgram(QStringLiteral("git"));
    git.setArguments(args);
    git.setWorkingDirectory(dir);
    git.setProcessEnvironment(env);
    git.start();
    if (!git.waitForStarted(5000)) return false;
    if (!git.waitForFinished(60000)) { git.kill(); git.waitForFinished(2000); return false; }
    if (out) *out = git.readAllStandardOutput();
    return git.exitCode() == 0;
}

// Build a repository with one commit on `main` holding `assets/hero.png`.
bool makeRepo(const QString &dir, QString *head = nullptr) {
    if (!run(dir, {QStringLiteral("init"), QStringLiteral("-b"), QStringLiteral("main")}))
        return false;
    if (!run(dir, {QStringLiteral("config"), QStringLiteral("user.email"), QStringLiteral("t@example.com")}))
        return false;
    if (!run(dir, {QStringLiteral("config"), QStringLiteral("user.name"), QStringLiteral("Test")}))
        return false;
    QDir(dir).mkpath(QStringLiteral("assets"));
    QFile hero(dir + "/assets/hero.png");
    if (!hero.open(QIODevice::WriteOnly)) return false;
    hero.write("hero-v1");
    hero.close();
    if (!run(dir, {QStringLiteral("add"), QStringLiteral("assets/hero.png")})) return false;
    if (!run(dir, {QStringLiteral("commit"), QStringLiteral("-m"), QStringLiteral("add hero")}))
        return false;
    if (head) {
        QByteArray out;
        run(dir, {QStringLiteral("rev-parse"), QStringLiteral("HEAD")}, &out);
        *head = QString::fromUtf8(out).trimmed();
    }
    return true;
}

// Like makeRepo but tracks *.png through git-lfs: the commit holds a pointer
// and the object is stored under .git/lfs/objects (real bytes in the worktree).
bool makeLfsRepo(const QString &dir, QString *head = nullptr) {
    if (!run(dir, {QStringLiteral("init"), QStringLiteral("-b"), QStringLiteral("main")}))
        return false;
    if (!run(dir, {QStringLiteral("config"), QStringLiteral("user.email"), QStringLiteral("t@example.com")}))
        return false;
    if (!run(dir, {QStringLiteral("config"), QStringLiteral("user.name"), QStringLiteral("Test")}))
        return false;
    if (!run(dir, {QStringLiteral("lfs"), QStringLiteral("install"), QStringLiteral("--local")}))
        return false;
    if (!run(dir, {QStringLiteral("lfs"), QStringLiteral("track"), QStringLiteral("*.png")}))
        return false;
    QDir(dir).mkpath(QStringLiteral("assets"));
    QFile big(dir + "/assets/big.png");
    if (!big.open(QIODevice::WriteOnly)) return false;
    big.write("PNG-big-bytes");
    big.close();
    if (!run(dir, {QStringLiteral("add"), QStringLiteral("assets/big.png")})) return false;
    if (!run(dir, {QStringLiteral("commit"), QStringLiteral("-m"), QStringLiteral("add big")}))
        return false;
    if (head) {
        QByteArray out;
        run(dir, {QStringLiteral("rev-parse"), QStringLiteral("HEAD")}, &out);
        *head = QString::fromUtf8(out).trimmed();
    }
    return true;
}

// Snapshot of everything a "read-only" call must not change: HEAD bytes,
// refs, index presence and mtime, and the worktree file list.
QByteArray repoFingerprint(const QString &dir) {
    QByteArray fp;
    QFile head(dir + "/.git/HEAD");
    if (head.open(QIODevice::ReadOnly)) fp += "HEAD:" + head.readAll() + "\n";
    QDir refs(dir + "/.git/refs/heads");
    for (const auto &e : refs.entryList(QDir::Files)) {
        QFile f(refs.filePath(e));
        if (f.open(QIODevice::ReadOnly)) fp += "ref:" + e.toUtf8() + ":" + f.readAll() + "\n";
    }
    QFile index(dir + "/.git/index");
    if (index.exists()) fp += "INDEX_MTIME:" + QByteArray::number(index.fileTime(QFileDevice::FileModificationTime).toMSecsSinceEpoch()) + "\n";
    fp += "WORKTREE:" + QDir(dir).entryList(QDir::AllEntries | QDir::NoDotAndDotDot).join('\n').toUtf8() + "\n";
    return fp;
}

QByteArray digest(const QByteArray &data) {
    return QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex();
}

} // namespace

class GitSourceTest : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() {
        QProcess p;
        p.setProgram(QStringLiteral("git"));
        p.setArguments({QStringLiteral("--version")});
        p.start();
        if (!p.waitForStarted(5000) || !p.waitForFinished(5000))
            QSKIP("git not on PATH");
    }
    void identifyReportsRepoAndHead() {
        QTemporaryDir dir;
        QString head;
        QVERIFY(makeRepo(dir.path(), &head));
        const auto info = git_source::identify(dir.path());
        QVERIFY(info.error.isEmpty());
        QCOMPARE(info.topLevel, QDir(dir.path()).canonicalPath());
        QCOMPARE(info.currentBranch, QStringLiteral("main"));
        QCOMPARE(info.headCommit, head);
        QVERIFY(!info.detached);
        QVERIFY(!info.bare);
        // A path outside any repository is reported, not thrown.
        QTemporaryDir nowhere;
        const auto bad = git_source::identify(nowhere.path());
        QVERIFY(!bad.error.isEmpty());
        QCOMPARE(bad.topLevel, QString());
    }
    void branchesListCurrentAndHeads() {
        QTemporaryDir dir;
        QString head;
        QVERIFY(makeRepo(dir.path(), &head));
        QString error;
        const auto list = git_source::branches(dir.path(), &error);
        QVERIFY(error.isEmpty());
        QCOMPARE(list.size(), 1);
        QCOMPARE(list.first().name, QStringLiteral("main"));
        QCOMPARE(list.first().head, head);
        QVERIFY(list.first().isCurrent);
    }
    // Read-API failure paths: a non-repository path reports a message through
    // the error out-param with an empty result (and fetchLfs refuses), never a
    // throw or fabricated data. A detached HEAD is flagged as such.
    void errorPathsAndDetachedHead() {
        QTemporaryDir notRepo;
        QString error;
        QVERIFY(git_source::branches(notRepo.path(), &error).isEmpty());
        QVERIFY(!error.isEmpty());
        QVERIFY(git_source::status(notRepo.path(), &error).isEmpty());
        QVERIFY(!error.isEmpty());
        QVERIFY(git_source::log(notRepo.path(), 10, &error).isEmpty());
        QVERIFY(!error.isEmpty());
        QVERIFY(git_source::tree(notRepo.path(), QStringLiteral("HEAD"),
                                 QString(), &error).isEmpty());
        QVERIFY(!error.isEmpty());
        // The LFS fetch is network/write consent-gated: off a repository it
        // fails closed with a message and nothing fetched.
        QVERIFY(!git_source::fetchLfs(notRepo.path(),
                                      QStringLiteral("assets/big.png"),
                                      QString(), &error));
        QVERIFY(!error.isEmpty());
        // Detached HEAD: no current branch, the flag set, commits intact.
        QTemporaryDir dir;
        QString head;
        QVERIFY(makeRepo(dir.path(), &head));
        QProcess git;
        git.setProgram(QStringLiteral("git"));
        git.setWorkingDirectory(dir.path());
        git.setArguments({QStringLiteral("checkout"), QStringLiteral("--detach"), head});
        git.start();
        QVERIFY(git.waitForFinished(10000));
        const auto info = git_source::identify(dir.path());
        QVERIFY(info.error.isEmpty());
        QVERIFY(info.detached);
        QVERIFY(info.currentBranch.isEmpty());
        QCOMPARE(info.headCommit, head);
        // Branch listing still works; on a detached HEAD no branch is current.
        const auto list = git_source::branches(dir.path(), &error);
        QVERIFY(error.isEmpty());
        QVERIFY(!list.isEmpty());
        for (const auto &b : list)
            QVERIFY(!b.isCurrent);
    }
    void statusReflectsWorktreeWithoutWriting() {
        QTemporaryDir dir;
        QVERIFY(makeRepo(dir.path()));
        QString error;
        // Clean repo: no entries, and the repository is untouched.
        const QByteArray before = repoFingerprint(dir.path());
        const auto clean = git_source::status(dir.path(), &error);
        QVERIFY(error.isEmpty());
        QVERIFY(clean.isEmpty());
        QCOMPARE(repoFingerprint(dir.path()), before);
        // An untracked file shows up as "??", still without writing.
        QFile note(dir.path() + "/note.txt");
        QVERIFY(note.open(QIODevice::WriteOnly));
        note.write("draft");
        note.close();
        const QByteArray before2 = repoFingerprint(dir.path());
        const auto dirty = git_source::status(dir.path(), &error);
        QVERIFY(error.isEmpty());
        QCOMPARE(dirty.size(), 1);
        QCOMPARE(dirty.first().path, QStringLiteral("note.txt"));
        QCOMPARE(dirty.first().code, QStringLiteral("??"));
        QCOMPARE(repoFingerprint(dir.path()), before2);
    }
    void logListsCommitsNewestFirst() {
        QTemporaryDir dir;
        QString head;
        QVERIFY(makeRepo(dir.path(), &head));
        QString error;
        const auto entries = git_source::log(dir.path(), 10, &error);
        QVERIFY(error.isEmpty());
        QCOMPARE(entries.size(), 1);
        QCOMPARE(entries.first().commit, head);
        QCOMPARE(entries.first().subject, QStringLiteral("add hero"));
        QCOMPARE(entries.first().author, QStringLiteral("Test"));
        QCOMPARE(entries.first().email, QStringLiteral("t@example.com"));
        QVERIFY(entries.first().whenMs > 0);
    }
    // P1 audit (2026-11-12) claimed a pipe deadlock: runGit reads stdout only
    // after waitForFinished, so output beyond the OS pipe buffer would block
    // git and the bounded wait would kill it. Empirically false on Qt 6 —
    // QProcess's waitForFinished services both read channels internally, so a
    // 170 KiB log comes back complete. This test locks that behavior: a large
    // repo's log must return every record, untruncated, newest first.
    void logDrainsOutputLargerThanThePipeBuffer() {
        QTemporaryDir dir;
        QString head;
        QVERIFY(makeRepo(dir.path(), &head));
        // ~200 x ~830 bytes of log output ≈ 170 KiB, well past the 64 KiB
        // pipe buffer; each commit needs a distinct content blob.
        const int commits = 200;
        for (int i = 0; i < commits; ++i) {
            QFile f(dir.filePath(QStringLiteral("blob%1.txt").arg(i)));
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write(QByteArray::number(i));
            f.close();
            QVERIFY(run(dir.path(), {QStringLiteral("add"), QStringLiteral(".")}));
            const QString subject = QStringLiteral("c%1 ").arg(i)
                                   + QString(790, QChar('x'));
            QVERIFY(run(dir.path(), {QStringLiteral("commit"), QStringLiteral("-m"),
                                     subject}));
        }
        QString error;
        const auto entries = git_source::log(dir.path(), commits + 1, &error);
        QCOMPARE(error, QString());
        QCOMPARE(entries.size(), commits + 1);
        QByteArray headNow;
        QVERIFY(run(dir.path(), {QStringLiteral("rev-parse"), QStringLiteral("HEAD")}, &headNow));
        // Newest first: the tip is the last commit made, the oldest is
        // makeRepo's original head. All records arrived complete — no
        // truncation at the pipe buffer.
        QCOMPARE(entries.first().commit, QString::fromLatin1(headNow.trimmed()));
        QVERIFY(entries.first().subject.startsWith(QStringLiteral("c%1 ").arg(commits - 1)));
        QCOMPARE(entries.last().commit, head);
        QVERIFY(entries.last().subject == QStringLiteral("add hero"));
    }
    void treeListsCommitFilesWithSubdir() {
        QTemporaryDir dir;
        QString head;
        QVERIFY(makeRepo(dir.path(), &head));
        const QByteArray before = repoFingerprint(dir.path());
        QString error;
        const auto all = git_source::tree(dir.path(), head, QString(), &error);
        QVERIFY(error.isEmpty());
        QCOMPARE(all.size(), 1);
        QCOMPARE(all.first().path, QStringLiteral("assets/hero.png"));
        QCOMPARE(all.first().mode, QStringLiteral("100644"));
        QCOMPARE(all.first().type, QStringLiteral("blob"));
        QCOMPARE(all.first().size, qint64(7));
        // Narrowing to a subdir still yields the same repo-relative path.
        const auto assets = git_source::tree(dir.path(), head,
                                             QStringLiteral("assets"), &error);
        QVERIFY(error.isEmpty());
        QCOMPARE(assets.size(), 1);
        QCOMPARE(assets.first().path, QStringLiteral("assets/hero.png"));
        // A bad commit is reported, and nothing was written.
        const auto bad = git_source::tree(dir.path(), QStringLiteral("nope"), QString(), &error);
        QVERIFY(!error.isEmpty());
        QVERIFY(bad.isEmpty());
        QCOMPARE(repoFingerprint(dir.path()), before);
    }
    void extractReturnsCommitBytes() {
        QTemporaryDir dir;
        QString head;
        QVERIFY(makeRepo(dir.path(), &head));
        const QByteArray before = repoFingerprint(dir.path());
        QByteArray bytes;
        QString error;
        QVERIFY(git_source::extract(dir.path(), head,
                                    QStringLiteral("assets/hero.png"), &bytes, &error));
        QVERIFY(error.isEmpty());
        QCOMPARE(bytes, QByteArrayLiteral("hero-v1"));
        QCOMPARE(repoFingerprint(dir.path()), before);
        // The worktree file is not the source of truth: rewrite it and the
        // commit bytes must be unchanged (read from the object DB).
        QFile hero(dir.path() + "/assets/hero.png");
        QVERIFY(hero.open(QIODevice::WriteOnly));
        hero.write("worktree-only");
        hero.close();
        QVERIFY(git_source::extract(dir.path(), head,
                                    QStringLiteral("assets/hero.png"), &bytes, &error));
        QVERIFY(error.isEmpty());
        QCOMPARE(bytes, QByteArrayLiteral("hero-v1"));
    }
    void extractReportsBadPath() {
        QTemporaryDir dir;
        QString head;
        QVERIFY(makeRepo(dir.path(), &head));
        QByteArray bytes;
        QString error;
        QVERIFY(!git_source::extract(dir.path(), head,
                                     QStringLiteral("assets/nope.png"), &bytes, &error));
        QVERIFY(!error.isEmpty());
        QVERIFY(bytes.isEmpty());
    }
    void lfsPointerAndMissingObject() {
        QProcess lfs;
        lfs.setProgram(QStringLiteral("git"));
        lfs.setArguments({QStringLiteral("lfs"), QStringLiteral("version")});
        lfs.start();
        if (!lfs.waitForStarted(5000) || !lfs.waitForFinished(5000) || lfs.exitCode() != 0)
            QSKIP("git-lfs not on PATH");
        QTemporaryDir dir;
        QString head;
        QVERIFY(makeLfsRepo(dir.path(), &head));
        const QByteArray before = repoFingerprint(dir.path());
        QString error;
        // Committed under LFS: the object is stored locally, so the pointer
        // is not treated as an image and the real bytes are extracted.
        QByteArray bytes;
        QVERIFY(git_source::extract(dir.path(), head,
                                    QStringLiteral("assets/big.png"), &bytes, &error));
        QVERIFY(error.isEmpty());
        QCOMPARE(bytes, QByteArrayLiteral("PNG-big-bytes"));
        QVERIFY(git_source::objectPresent(dir.path(),
                                          QStringLiteral("assets/big.png"), head, &error));
        QVERIFY(error.isEmpty());
        QVERIFY(!git_source::isLfsPointer(bytes));
        // Remove the object store: the pointer is now "missing" and extract
        // must return the pointer text, never an empty or wrong buffer.
        QVERIFY(QDir(dir.path() + "/.git/lfs/objects").removeRecursively());
        QVERIFY(!git_source::objectPresent(dir.path(),
                                           QStringLiteral("assets/big.png"), head, &error));
        QVERIFY(git_source::extract(dir.path(), head,
                                    QStringLiteral("assets/big.png"), &bytes, &error));
        QVERIFY(error.isEmpty());
        QVERIFY(git_source::isLfsPointer(bytes));
        QVERIFY(bytes.startsWith("version https://git-lfs.github.com/spec/v1"));
        // A plain blob is not a pointer, and nothing was written anywhere.
        QVERIFY(!git_source::isLfsPointer(QByteArrayLiteral("not-a-pointer")));
        QCOMPARE(repoFingerprint(dir.path()), before);
    }
    void fetchLfsWithConsent() {
        QProcess lfs;
        lfs.setProgram(QStringLiteral("git"));
        lfs.setArguments({QStringLiteral("lfs"), QStringLiteral("version")});
        lfs.start();
        if (!lfs.waitForStarted(5000) || !lfs.waitForFinished(5000) || lfs.exitCode() != 0)
            QSKIP("git-lfs not on PATH");
        // A source repo with an LFS object, pushed to a bare remote.
        QTemporaryDir originDir;
        QString head;
        QVERIFY(makeLfsRepo(originDir.path(), &head));
        const QString remote = originDir.path() + "/origin.git";
        QVERIFY(run(originDir.path(), {QStringLiteral("init"), QStringLiteral("-q"),
                                       QStringLiteral("-b"), QStringLiteral("main"),
                                       QStringLiteral("--bare"), remote}));
        QVERIFY(run(originDir.path(), {QStringLiteral("remote"), QStringLiteral("add"),
                                       QStringLiteral("origin"), remote}));
        QVERIFY(run(originDir.path(), {QStringLiteral("push"), QStringLiteral("-q"),
                                       QStringLiteral("origin"), QStringLiteral("main")}));
        // Clone WITHOUT smudging: the commit has the pointer, the object is absent.
        QTemporaryDir cloneDir;
        const QString clone = cloneDir.path() + "/clone";
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.insert(QStringLiteral("GIT_LFS_SKIP_SMUDGE"), QStringLiteral("1"));
        QVERIFY(runEnv(cloneDir.path(), {QStringLiteral("clone"), QStringLiteral("-q"),
                                         remote, clone}, env));
        QVERIFY(run(clone, {QStringLiteral("lfs"), QStringLiteral("install"),
                            QStringLiteral("--local")}));
        QString error;
        QVERIFY(!git_source::objectPresent(clone, QStringLiteral("assets/big.png"),
                                           QStringLiteral("HEAD"), &error));
        // The worktree holds the pointer before fetch, and is left untouched.
        QFile worktree(clone + "/assets/big.png");
        QVERIFY(worktree.open(QIODevice::ReadOnly));
        const QByteArray pointerBefore = worktree.readAll();
        worktree.close();
        QVERIFY(git_source::isLfsPointer(pointerBefore));
        // Consent-gated fetch (the test IS the caller's consent): object appears.
        QVERIFY(git_source::fetchLfs(clone, QStringLiteral("assets/big.png"),
                                     QStringLiteral("HEAD"), &error));
        QVERIFY(error.isEmpty());
        QVERIFY(git_source::objectPresent(clone, QStringLiteral("assets/big.png"),
                                          QStringLiteral("HEAD"), &error));
        QVERIFY(error.isEmpty());
        QByteArray bytes;
        QVERIFY(git_source::extract(clone, QStringLiteral("HEAD"),
                                    QStringLiteral("assets/big.png"), &bytes, &error));
        QVERIFY(error.isEmpty());
        QCOMPARE(bytes, QByteArrayLiteral("PNG-big-bytes"));
        // Only .git/lfs was written: the worktree file is still the pointer.
        QFile after(clone + "/assets/big.png");
        QVERIFY(after.open(QIODevice::ReadOnly));
        QCOMPARE(after.readAll(), pointerBefore);
        after.close();
    }
};

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    GitSourceTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "git_source_test.moc"
