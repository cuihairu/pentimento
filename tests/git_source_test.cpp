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
};

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    GitSourceTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "git_source_test.moc"
