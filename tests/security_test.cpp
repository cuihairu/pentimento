#include "library.h"
#include "sentinel.h"
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>

namespace {
void put(const QString &path, const QByteArray &data) {
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    QCOMPARE(file.write(data), data.size());
}
QByteArray get(const QString &path) { QFile file(path); return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray(); }
// Deterministic pseudo-random bytes (xorshift), reproducible across runs.
QByteArray randomBytes(int count) {
    QByteArray data(count, 0);
    unsigned int state = 0x12345678u;
    for (int i = 0; i < count; ++i) {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        data[i] = static_cast<char>(state & 0xff);
    }
    return data;
}
SentinelConfig isolating(int batch, int churn, int entropy) {
    SentinelConfig cfg;
    cfg.batchChangeThreshold = batch;
    cfg.extensionChurnThreshold = churn;
    cfg.highEntropyThreshold = entropy;
    cfg.entropyLimit = 7.0;
    return cfg;
}
}

class SecurityTest : public QObject {
    Q_OBJECT
private slots:
    void entropyIsHighForRandomAndLowForText() {
        QVERIFY(Sentinel::contentEntropy(randomBytes(4096)) > 7.5);
        const QByteArray text = QByteArray("the quick brown fox jumps over the lazy dog ").repeated(64);
        QVERIFY(Sentinel::contentEntropy(text) < 5.0);
    }
    void detectsBatchRewrite() {
        QTemporaryDir dir;
        SentinelConfig cfg = isolating(3, 1000, 1000);
        Sentinel sentinel(dir.path(), cfg);
        for (int i = 0; i < 5; ++i) put(dir.filePath(QString("f%1.png").arg(i)), "v1 content");
        sentinel.resetBaseline();
        for (int i = 0; i < 4; ++i) put(dir.filePath(QString("f%1.png").arg(i)), randomBytes(512));
        const auto alert = sentinel.check();
        QVERIFY(alert.triggered);
        QVERIFY(alert.reason.contains(QStringLiteral("批量写入")));
        QVERIFY(alert.changedFiles >= 3);
    }
    void detectsExtensionChurn() {
        QTemporaryDir dir;
        SentinelConfig cfg = isolating(1000, 3, 1000);
        Sentinel sentinel(dir.path(), cfg);
        for (int i = 0; i < 5; ++i) put(dir.filePath(QString("f%1.png").arg(i)), "v1 content");
        sentinel.resetBaseline();
        for (int i = 0; i < 5; ++i) {
            QFile::rename(dir.filePath(QString("f%1.png").arg(i)),
                          dir.filePath(QString("f%1.png.enc").arg(i)));
        }
        const auto alert = sentinel.check();
        QVERIFY(alert.triggered);
        QVERIFY(alert.reason.contains(QStringLiteral("扩展名批量变更")));
        QVERIFY(alert.novelExtensionFiles >= 3);
    }
    void detectsHighEntropyBurst() {
        QTemporaryDir dir;
        SentinelConfig cfg = isolating(1000, 1000, 3);
        Sentinel sentinel(dir.path(), cfg);
        for (int i = 0; i < 5; ++i) put(dir.filePath(QString("f%1.png").arg(i)), "plain text content");
        sentinel.resetBaseline();
        // Same filenames, but content is now high-entropy (encryption signature).
        for (int i = 0; i < 5; ++i) put(dir.filePath(QString("f%1.png").arg(i)), randomBytes(2048));
        const auto alert = sentinel.check();
        QVERIFY(alert.triggered);
        QVERIFY(alert.reason.contains(QStringLiteral("内容熵值异常")));
        QVERIFY(alert.highEntropyFiles >= 3);
    }
    void staysQuietOnNormalSmallEdits() {
        QTemporaryDir dir;
        SentinelConfig cfg = isolating(3, 3, 3);
        Sentinel sentinel(dir.path(), cfg);
        for (int i = 0; i < 5; ++i) put(dir.filePath(QString("f%1.png").arg(i)), "stable content");
        sentinel.resetBaseline();
        // A normal, small edit: one file, low entropy, no rename.
        put(dir.filePath("f0.png"), "stable content, tiny edit");
        const auto alert = sentinel.check();
        QVERIFY(!alert.triggered);
        QCOMPARE(alert.changedFiles, 1);
        QCOMPARE(alert.novelExtensionFiles, 0);
        QCOMPARE(alert.highEntropyFiles, 0);
    }
    void sentinelAlertsAndLibraryTakesEmergencySnapshot() {
        QTemporaryDir dir;
        put(dir.filePath("a.png"), "alpha");
        put(dir.filePath("b.png"), "beta");
        Library library(dir.filePath("library"));
        library.importFile(dir.filePath("a.png"));
        library.importFile(dir.filePath("b.png"));
        SentinelConfig cfg = isolating(2, 2, 2);
        Sentinel sentinel(library.root() + "/working", cfg);
        sentinel.resetBaseline();
        // Simulated ransomware over the workspace: batch overwrite + rename.
        for (int i = 0; i < 2; ++i) {
            const QString base = library.assets().at(i).path;
            put(base, randomBytes(1024));
            QVERIFY(QFile::rename(base, base + ".enc"));
        }
        const auto alert = sentinel.check();
        QVERIFY(alert.triggered);
        // Emergency snapshot freezes the current state as an anchor.
        const qint64 snap = library.snapshotAll(QStringLiteral("紧急快照：疑似勒索批量写入"));
        QVERIFY(snap > 0);
        QCOMPARE(library.snapshots().size(), 1);
        QCOMPARE(library.snapshots().first().assets, 2);
        QVERIFY(library.snapshots().first().note.contains(QStringLiteral("勒索")));
    }
    void rollbackRestoresWorkspaceToPreInfectionAnchor() {
        QTemporaryDir dir;
        put(dir.filePath("a.png"), "alpha");
        put(dir.filePath("b.png"), "beta");
        Library library(dir.filePath("library"));
        library.importFile(dir.filePath("a.png"));
        library.importFile(dir.filePath("b.png"));
        const auto assets = library.assets();
        QCOMPARE(assets.size(), 2);
        // Pre-infection working content (the recovery target).
        QFile beforeA(assets.at(0).path), beforeB(assets.at(1).path);
        QVERIFY(beforeA.open(QIODevice::ReadOnly) && beforeB.open(QIODevice::ReadOnly));
        const QByteArray aBefore = beforeA.readAll();
        const QByteArray bBefore = beforeB.readAll();
        QVERIFY(!aBefore.isEmpty() && !bBefore.isEmpty());
        SentinelConfig cfg = isolating(2, 2, 2);
        Sentinel sentinel(library.root() + "/working", cfg);
        sentinel.resetBaseline();
        // Plant simulated ransomware: batch overwrite + rename (the Layer 2 fixture).
        for (int i = 0; i < 2; ++i) {
            const QString base = assets.at(i).path;
            put(base, randomBytes(1024));
            QVERIFY(QFile::rename(base, base + ".enc"));
        }
        QVERIFY(sentinel.check().triggered);
        // Select the emergency snapshot as the rollback anchor.
        const qint64 snap = library.snapshotAll(QStringLiteral("紧急快照：疑似勒索批量写入"));
        QVERIFY(snap > 0);
        // One-click rollback restores the whole workspace to pre-infection content.
        QCOMPARE(library.rollbackAll(snap), 2);
        QFile afterA(assets.at(0).path), afterB(assets.at(1).path);
        QVERIFY(afterA.open(QIODevice::ReadOnly) && afterB.open(QIODevice::ReadOnly));
        QCOMPARE(afterA.readAll(), aBefore); // sampled hashes match pre-infection
        QCOMPARE(afterB.readAll(), bBefore);
        // Rollback forms a new version and does not destroy later history: the
        // infected .enc files remain on disk, and each asset gained a restore version.
        QVERIFY(QFile::exists(assets.at(0).path + ".enc"));
        QVERIFY(QFile::exists(assets.at(1).path + ".enc"));
        QCOMPARE(library.revisions(assets.at(0).id).size(), 2);
        QCOMPARE(library.revisions(assets.at(1).id).size(), 2);
    }
    void snapshotAnchorSkipsInfectedAutoRecord() {
        QTemporaryDir dir;
        const auto source = dir.filePath("asset.dat");
        put(source, "clean v1");
        Library library(dir.filePath("library"));
        const auto asset = library.importFile(source);
        const qint64 cleanRev = library.revisions(asset.id).first().id;
        // Ransomware overwrites the working file; before the sentinel fires the
        // 2s auto-record captures the infected content as the newest revision.
        put(asset.path, "infected content");
        QVERIFY(library.capture(asset, QStringLiteral("自动记录")));
        QCOMPARE(library.revisions(asset.id).first().id != cleanRev, true);
        // Emergency snapshot must anchor to the pre-infection clean revision,
        // not to the newest revision that merely mirrors the infected content.
        const qint64 snap = library.snapshotAll(QStringLiteral("紧急快照：疑似勒索批量写入"));
        QVERIFY(snap > 0);
        // Roll back: the workspace returns to clean pre-infection content.
        QCOMPARE(library.rollbackAll(snap), 1);
        QCOMPARE(get(asset.path), QByteArray("clean v1"));
    }
    void rollbackHandlesEmptyAnchorObject() {
        QTemporaryDir dir;
        const auto source = dir.filePath("asset.dat");
        put(source, ""); // legal empty object
        Library library(dir.filePath("library"));
        const auto asset = library.importFile(source);
        const qint64 snap = library.snapshotAll(QStringLiteral("紧急快照"));
        QVERIFY(snap > 0);
        // Empty content is a valid anchor; content emptiness must not be read as
        // "anchor revision vanished".
        put(asset.path, "later content");
        QCOMPARE(library.rollbackAll(snap), 1);
        QCOMPARE(get(asset.path), QByteArray(""));
    }
};
QTEST_GUILESS_MAIN(SecurityTest)
#include "security_test.moc"
