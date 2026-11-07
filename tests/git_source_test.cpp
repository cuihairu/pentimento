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
};

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    GitSourceTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "git_source_test.moc"
