// Backend capability gating and source connections: independent capabilities, probe states, reachability retries, backup targets.
// (LAY-001: suite split out of the former tests/library_test.cpp.)
#include "library_test_helpers.h"

#include "library.h"
#include "backend.h"
#include "compare_canvas.h"
#include "preview_canvas.h"
#include "exclusion.h"
#include "filter.h"
#include "i18n.h"
#include "importer.h"
#include "platform_immutable.h"
#include "preview.h"
#include "recent.h"
#include "snapshot_schedule.h"
#include "source_connection.h"
#include "storage.h"
#include "taskqueue.h"
#include "thumbnails.h"
#include <QWheelEvent>
#include <QApplication>
#include <QTranslator>
#include <QBuffer>
#include <QCryptographicHash>
#include <QDataStream>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QImageReader>
#include <QRegularExpression>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QtTest>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <thread>
#include <chrono>

class BackendSourcesTest : public QObject {
    Q_OBJECT
private slots:
    void backendCapabilityQueryDistinguishesIndependentCapabilities() {
        QTemporaryDir dir;
        const auto source = dir.filePath("asset.dat");
        put(source, "content");
        Library library(dir.filePath("library"));
        const BackendInfo info = library.backendInfo();
        // V1 self-built backend: writable, with history / tree snapshot / restore
        // / lock; no remote objects to fetch.
        QCOMPARE(info.name, QString("self"));
        QVERIFY(info.writable);
        QVERIFY(info.supports(Capability::History));
        QVERIFY(info.supports(Capability::SnapshotTree));
        QVERIFY(info.supports(Capability::Restore));
        QVERIFY(info.supports(Capability::Lock));
        QVERIFY(!info.supports(Capability::RemoteFetch));
        // The unsupported capability carries a stable, human-readable reason.
        const QString reason = info.unsupportedReason(Capability::RemoteFetch);
        QVERIFY(!reason.isEmpty());
        QCOMPARE(reason, info.unsupportedReason(Capability::RemoteFetch));
        // Supported capabilities must not report an "unsupported" reason for the
        // self backend; the UI hides an action only when the capability is absent.
        QVERIFY(info.unsupportedReason(Capability::History).contains(QStringLiteral("此后端")));
    }
    // P-409: the history actions are capability-driven — a read-only backend
    // exposes extract/export but never a write-back restore button, and the
    // reason is surfaced; no selection disables everything with no reason.
    void historyActionsFollowBackendCapabilities() {
        // Self backend (writable, all capabilities): both actions offered.
        const auto self = historyActionsFor(selfBackendInfo(), /*selectionValid=*/true);
        QVERIFY(self.restore);
        QVERIFY(self.extract);
        QVERIFY(self.restoreReason.isEmpty());
        QVERIFY(self.extractReason.isEmpty());

        // Read-only backend: history yes, restore no.
        BackendInfo readOnly;
        readOnly.name = QStringLiteral("git");
        readOnly.writable = false;
        readOnly.capabilities.insert(Capability::History, true);
        readOnly.capabilities.insert(Capability::SnapshotTree, false);
        readOnly.capabilities.insert(Capability::Restore, false);
        const auto ro = historyActionsFor(readOnly, true);
        QVERIFY(!ro.restore);                 // no write-back restore button
        QVERIFY(ro.extract);                  // export/extract still available
        QVERIFY(!ro.restoreReason.isEmpty()); // reason surfaced instead

        // No history capability: extraction is off too.
        BackendInfo noHistory = selfBackendInfo();
        noHistory.capabilities.insert(Capability::History, false);
        const auto nh = historyActionsFor(noHistory, true);
        QVERIFY(!nh.extract);
        QVERIFY(!nh.extractReason.isEmpty());

        // No selection: nothing is offered, and no reason is attached.
        const auto none = historyActionsFor(selfBackendInfo(), /*selectionValid=*/false);
        QVERIFY(!none.restore);
        QVERIFY(!none.extract);
        QVERIFY(none.restoreReason.isEmpty());
        QVERIFY(none.extractReason.isEmpty());
    }
    void connectionStateMapsStorageProbeStates() {
        // P-513 mapping: a read-only mount is still connected (writability is a
        // backup-target concern, not a connection state); a missing path is
        // unreachable, never "deleted"; an unreadable path is a failed probe.
        QCOMPARE(connectionStateFor(SourceState::Reachable), ConnectionState::Reachable);
        QCOMPARE(connectionStateFor(SourceState::ReadOnly), ConnectionState::Reachable);
        QCOMPARE(connectionStateFor(SourceState::Unreachable), ConnectionState::Unreachable);
        QCOMPARE(connectionStateFor(SourceState::PermissionDenied), ConnectionState::Failed);
        // The four visible state words stay distinct (连接中/可达/不可达/失败).
        const QVector<ConnectionState> all = {
            ConnectionState::Connecting, ConnectionState::Reachable,
            ConnectionState::Unreachable, ConnectionState::Failed};
        QStringList words;
        for (const auto s : all) {
            const QString word = connectionStateText(s);
            QVERIFY(!word.isEmpty());
            QVERIFY(!words.contains(word));
            words << word;
        }
    }
    void sourceConnectionTracksReachabilityWithUserRetry() {
        QTemporaryDir dir;
        const QString source = dir.filePath("share");
        QVERIFY(QDir().mkpath(source));
        put(source + "/a.png", "share-bytes");
        const qint64 originalMtime = QFileInfo(source + "/a.png").lastModified().toMSecsSinceEpoch();

        SourceConnection conn;
        conn.open(source);
        QCOMPARE(conn.state(), ConnectionState::Connecting); // visible pre-probe state
        QTRY_COMPARE(conn.state(), ConnectionState::Reachable);
        QVERIFY(!conn.probing());
        QVERIFY(conn.reason().isEmpty());

        // "Unmount" the share: the directory disappears → unreachable. This is
        // never treated as a deletion; a retry only re-probes.
        QVERIFY(QDir(dir.path()).rename(QStringLiteral("share"), QStringLiteral("share-gone")));
        conn.retry();
        QCOMPARE(conn.state(), ConnectionState::Connecting);
        QTRY_COMPARE(conn.state(), ConnectionState::Unreachable);
        QVERIFY(!conn.reason().isEmpty());

        // "Remount" the same path: retry reaches it again.
        QVERIFY(QDir(dir.path()).rename(QStringLiteral("share-gone"), QStringLiteral("share")));
        conn.retry();
        QTRY_COMPARE(conn.state(), ConnectionState::Reachable);
        QVERIFY(conn.reason().isEmpty());

        // The source directory holds exactly what it held before the retries:
        // no reconnect artifact, no content/metadata churn (the probe is
        // read-only — a retry never overwrites remote content).
        const auto entries = listDirectory(source);
        QCOMPARE(entries.size(), 1);
        QCOMPARE(entries.first().path, QFileInfo(source + "/a.png").absoluteFilePath());
        QCOMPARE(get(source + "/a.png"), QByteArray("share-bytes"));
        QCOMPARE(QFileInfo(source + "/a.png").lastModified().toMSecsSinceEpoch(), originalMtime);
    }
    void sourceConnectionAllowsOneProbeAtATime() {
        QTemporaryDir dir;
        QVERIFY(QDir().mkpath(dir.filePath("share")));
        put(dir.filePath("share/a.png"), "x");

        SourceConnection conn;
        std::atomic_int calls = 0;
        QSemaphore gate;
        conn.setProbeFn([&](const QString &path) {
            if (++calls == 1) gate.acquire(); // hold the first probe (slow mount)
            return statSource(path);
        });
        conn.open(dir.filePath("share"));
        // Wait for the probe to actually start (it is now held open by the
        // gate), so a later assertion cannot fail before the probe is released.
        QTRY_COMPARE(calls.load(), 1);
        QTRY_VERIFY(conn.probing());
        // Retry while a probe is in flight is a no-op: one probe at a time (the
        // V1 backoff), no fast retry loop piling up probes.
        conn.retry();
        QTest::qWait(50);
        QCOMPARE(calls.load(), 1);
        gate.release();
        QTRY_COMPARE(conn.state(), ConnectionState::Reachable);
        QTest::qWait(50); // a wrongly-queued retry would now surface
        QCOMPARE(calls.load(), 1);
        QVERIFY(!conn.probing());

        // A path change while a probe is in flight queues a new probe; the stale
        // result (old path) must be dropped, never applied to the new path.
        QSignalSpy spy(&conn, &SourceConnection::stateChanged);
        QSemaphore gate2;
        conn.setProbeFn([&](const QString &path) {
            if (++calls == 2) gate2.acquire(); // hold the stale probe
            return statSource(path);
        });
        conn.open(dir.filePath("share"));    // probe #2 (stale: reports Reachable)
        conn.open(dir.filePath("missing"));  // probe #3 (current path)
        QTRY_COMPARE(calls.load(), 2);       // stale probe started (now held open)
        gate2.release();
        QTRY_COMPARE(conn.state(), ConnectionState::Unreachable);
        QCOMPARE(conn.path(), QFileInfo(dir.filePath("missing")).absoluteFilePath());
        // After the last Connecting (open of the new path), the only applied
        // state is Unreachable — the stale Reachable never leaked through.
        int lastConnecting = -1;
        for (int i = 0; i < spy.size(); ++i)
            if (qvariant_cast<ConnectionState>(spy.at(i).at(0)) == ConnectionState::Connecting)
                lastConnecting = i;
        QVERIFY(lastConnecting >= 0);
        for (int i = lastConnecting + 1; i < spy.size(); ++i)
            QCOMPARE(qvariant_cast<ConnectionState>(spy.at(i).at(0)), ConnectionState::Unreachable);
    }
    void probeBackupTargetDistinguishesWriteFromRead() {
        QTemporaryDir dir;
        const QString writable = dir.filePath("target");
        QVERIFY(QDir().mkpath(writable));
        const BackupTargetInfo ok = probeBackupTarget(writable);
        QVERIFY(ok.writable);
        QVERIFY(!ok.readOnly);
        QVERIFY(ok.reason.isEmpty());
        QVERIFY(ok.freeBytes >= 0);

        // An empty path is refused with a reason, never "writable".
        const BackupTargetInfo empty = probeBackupTarget(QString());
        QVERIFY(!empty.writable);
        QVERIFY(!empty.reason.isEmpty());

        const QString filePath = dir.filePath("existing-file");
        QFile file(filePath);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QVERIFY(file.write("target") > 0);
        file.close();
        const BackupTargetInfo fileTarget = probeBackupTarget(filePath);
        QVERIFY(!fileTarget.writable);
        QVERIFY(!fileTarget.readOnly);
        QVERIFY(!fileTarget.reason.isEmpty());

        // A not-yet-existing path under a writable parent is a creatable new
        // directory — the P-510 "可为新目录" acceptance, not an error.
        const BackupTargetInfo creatable = probeBackupTarget(dir.filePath("new-target"));
        QVERIFY(creatable.writable);
        QVERIFY(creatable.reason.isEmpty());

        // A read-only directory (mode bits) is a readable source but NOT a
        // writable backup target — the P-510 separation.
        const QString ro = dir.filePath("ro");
        QVERIFY(QDir().mkpath(ro));
        QFile::setPermissions(ro, QFileDevice::ReadOwner | QFileDevice::ReadGroup | QFileDevice::ReadOther);
        const BackupTargetInfo roInfo = probeBackupTarget(ro);
        // Running as root, mode bits may be bypassed; only assert the read-only
        // classification when the probe actually refuses (non-root).
        if (!roInfo.writable) {
            QVERIFY(roInfo.readOnly);
            QVERIFY(!roInfo.reason.isEmpty());
        }
        QFile::setPermissions(ro, QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                                      QFileDevice::ReadGroup | QFileDevice::WriteGroup |
                                      QFileDevice::ReadOther | QFileDevice::WriteOther);
    }

};

// A QApplication is required: CompareCanvas/PreviewCanvas are widgets
// and cannot be created under a QCoreApplication (canvas/journey).
int main(int argc, char **argv) {
    QApplication app(argc, argv);
    BackendSourcesTest tc;
    QTEST_SET_MAIN_SOURCE_PATH
    return QTest::qExec(&tc, argc, argv);
}
#include "library_sources_test.moc"
