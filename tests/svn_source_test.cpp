// P-406..P-408: read-only SVN source access. Tests build a throwaway
// file:// repository with the real svn/svnadmin binaries and assert the
// module only reads: no commit, no update, no working-copy overwrite, and
// lock state is reported without acquiring one.
#include "svn_source.h"

#include <algorithm>

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QProcess>
#include <QTemporaryDir>
#include <QtTest>

namespace {

bool run(const QString &program, const QString &dir, const QStringList &args,
         QByteArray *out = nullptr) {
    QProcess p;
    p.setProgram(program);
    p.setArguments(args);
    p.setWorkingDirectory(dir);
    p.start();
    if (!p.waitForStarted(5000)) return false;
    if (!p.waitForFinished(60000)) { p.kill(); p.waitForFinished(2000); return false; }
    if (out) *out = p.readAllStandardOutput();
    return p.exitCode() == 0;
}

bool toolAvailable(const QString &program) {
    QProcess p;
    p.setProgram(program);
    p.setArguments({QStringLiteral("--version")});
    p.start();
    return p.waitForStarted(5000) && p.waitForFinished(5000);
}

// Create repo + working copy with one commit (r1) holding assets/hero.png.
bool makeRepo(const QString &base, QString *wcOut) {
    const QString repo = base + "/repo";
    const QString wc = base + "/wc";
    if (!run(QStringLiteral("svnadmin"), base,
             {QStringLiteral("create"), repo}))
        return false;
    const QString url = QStringLiteral("file://") + QDir(repo).absolutePath();
    if (!run(QStringLiteral("svn"), base,
             {QStringLiteral("checkout"), QStringLiteral("-q"), url, wc}))
        return false;
    QDir(wc).mkpath(QStringLiteral("assets"));
    QFile hero(wc + "/assets/hero.png");
    if (!hero.open(QIODevice::WriteOnly)) return false;
    hero.write("hero-v1");
    hero.close();
    QFile readme(wc + "/readme.txt");
    if (!readme.open(QIODevice::WriteOnly)) return false;
    readme.write("notes");
    readme.close();
    if (!run(QStringLiteral("svn"), wc,
             {QStringLiteral("add"), QStringLiteral("-q"),
              QStringLiteral("assets"), QStringLiteral("readme.txt")}))
        return false;
    if (!run(QStringLiteral("svn"), wc,
             {QStringLiteral("commit"), QStringLiteral("-q"),
              QStringLiteral("-m"), QStringLiteral("add hero")}))
        return false;
    if (wcOut) *wcOut = wc;
    return true;
}

} // namespace

class SvnSourceTest : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() {
        if (!toolAvailable(QStringLiteral("svn")) ||
            !toolAvailable(QStringLiteral("svnadmin")))
            QSKIP("svn/svnadmin not on PATH");
    }
    void identifyReportsUrlAndHead() {
        QTemporaryDir dir;
        QString wc;
        QVERIFY(makeRepo(dir.path(), &wc));
        const auto info = svn_source::identify(wc);
        QVERIFY(info.error.isEmpty());
        QVERIFY(info.workingCopy);
        QCOMPARE(info.kind, QStringLiteral("dir"));
        QVERIFY(info.repoRoot.startsWith(QLatin1String("file://")));
        QVERIFY(info.repoRoot.endsWith(QLatin1String("/repo")));
        QVERIFY(info.headRevision >= 1);
        // A path outside any repository is reported, not thrown.
        QTemporaryDir nowhere;
        const auto bad = svn_source::identify(nowhere.path());
        QVERIFY(!bad.error.isEmpty());
    }
    void logListsRevisions() {
        QTemporaryDir dir;
        QString wc;
        QVERIFY(makeRepo(dir.path(), &wc));
        QString error;
        const auto entries = svn_source::log(wc, 10, &error);
        QVERIFY(error.isEmpty());
        QVERIFY(entries.size() >= 1);
        QCOMPARE(entries.first().message, QStringLiteral("add hero"));
        QVERIFY(entries.first().revision >= 1);
        QVERIFY(!entries.first().author.isEmpty());
        QVERIFY(entries.first().whenMs > 0);
    }
    void treeListsRevisionFiles() {
        QTemporaryDir dir;
        QString wc;
        QVERIFY(makeRepo(dir.path(), &wc));
        const auto info = svn_source::identify(wc);
        QString error;
        const auto all = svn_source::tree(wc, info.headRevision, QString(), &error);
        QVERIFY(error.isEmpty());
        // Whole-repo listing includes the assets dir entry plus two files.
        QCOMPARE(all.size(), 3);
        // Find the hero file among entries.
        const auto hero = std::find_if(all.cbegin(), all.cend(), [](const svn_source::TreeEntry &e) {
            return e.path == QLatin1String("assets/hero.png");
        });
        QVERIFY(hero != all.cend());
        QCOMPARE(hero->kind, QStringLiteral("file"));
        QCOMPARE(hero->size, qint64(7));
        QVERIFY(hero->rev >= 1);
        // Narrowing to a subdir still yields the repo-relative path.
        const auto assets = svn_source::tree(wc, info.headRevision,
                                             QStringLiteral("assets"), &error);
        QVERIFY(error.isEmpty());
        QCOMPARE(assets.size(), 1);
        QCOMPARE(assets.first().path, QStringLiteral("assets/hero.png"));
    }
    void extractRevisionBytes() {
        QTemporaryDir dir;
        QString wc;
        QVERIFY(makeRepo(dir.path(), &wc));
        const auto info = svn_source::identify(wc);
        QByteArray bytes;
        QString error;
        QVERIFY(svn_source::extract(wc, info.headRevision,
                                    QStringLiteral("assets/hero.png"), &bytes, &error));
        QVERIFY(error.isEmpty());
        QCOMPARE(bytes, QByteArrayLiteral("hero-v1"));
        // The working copy is not the source of truth: overwrite it and the
        // revision bytes must be unchanged (read from the repository).
        QFile hero(wc + "/assets/hero.png");
        QVERIFY(hero.open(QIODevice::WriteOnly));
        hero.write("worktree-only");
        hero.close();
        QVERIFY(svn_source::extract(wc, info.headRevision,
                                    QStringLiteral("assets/hero.png"), &bytes, &error));
        QVERIFY(error.isEmpty());
        QCOMPARE(bytes, QByteArrayLiteral("hero-v1"));
        // A bad path is reported.
        QVERIFY(!svn_source::extract(wc, info.headRevision,
                                     QStringLiteral("assets/nope.png"), &bytes, &error));
        QVERIFY(!error.isEmpty());
    }
    void lockInfoReportsState() {
        QTemporaryDir dir;
        QString wc;
        QVERIFY(makeRepo(dir.path(), &wc));
        QString error;
        // Unlocked initially: no lock is a valid state, not an error.
        auto lock = svn_source::lockInfo(wc + "/assets/hero.png", &error);
        QVERIFY(error.isEmpty());
        QVERIFY(!lock.locked);
        // Acquire a lock in the throwaway repo, then read it back.
        QVERIFY(run(QStringLiteral("svn"), wc,
                    {QStringLiteral("lock"), QStringLiteral("-q"),
                     QStringLiteral("assets/hero.png"), QStringLiteral("-m"),
                     QStringLiteral("test lock")}));
        lock = svn_source::lockInfo(wc + "/assets/hero.png", &error);
        QVERIFY(error.isEmpty());
        QVERIFY(lock.locked);
        QVERIFY(lock.token.startsWith(QLatin1String("opaquelocktoken:")));
        QVERIFY(!lock.owner.isEmpty());
        QCOMPARE(lock.comment, QStringLiteral("test lock"));
        QVERIFY(lock.createdMs > 0);
        // Release so the temp dir can be cleaned up.
        QVERIFY(run(QStringLiteral("svn"), wc,
                    {QStringLiteral("unlock"), QStringLiteral("-q"),
                     QStringLiteral("assets/hero.png")}));
    }
    // Read-API failure paths: a path outside any working copy (and a URL that
    // cannot be contacted) reports a message through the error out-param with
    // an empty result — never a throw and never fabricated data.
    void errorPathsReportMessages() {
        QTemporaryDir notWc;
        QString error;
        QVERIFY(svn_source::log(notWc.path(), 10, &error).isEmpty());
        QVERIFY(!error.isEmpty());
        QVERIFY(svn_source::tree(notWc.path(), 0, QString(), &error).isEmpty());
        QVERIFY(!error.isEmpty());
        QByteArray bytes;
        QVERIFY(!svn_source::extract(notWc.path(), 1, QStringLiteral("f.png"), &bytes, &error));
        QVERIFY(!error.isEmpty());
        const auto lock = svn_source::lockInfo(notWc.path(), &error);
        QVERIFY(!error.isEmpty());
        QVERIFY(!lock.locked);
        // A URL for a closed port is an immediate refusal, not an empty
        // success (no 30 s network timeout: the connection is rejected).
        const QString deadUrl = QStringLiteral("svn://127.0.0.1:1/repo");
        const auto bad = svn_source::identify(deadUrl);
        QVERIFY(!bad.error.isEmpty());
        QVERIFY(svn_source::log(deadUrl, 10, &error).isEmpty());
        QVERIFY(!error.isEmpty());
        QVERIFY(!svn_source::extract(deadUrl, 1, QStringLiteral("f.png"), &bytes, &error));
        QVERIFY(!error.isEmpty());
    }
};

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    SvnSourceTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "svn_source_test.moc"
