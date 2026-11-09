// Runtime services: storage interface, background task queue, recent libraries, translations and workspace-encrypted history objects.
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

class RuntimeServicesTest : public QObject {
    Q_OBJECT
private slots:
    void storageInterfaceReportsConsistentLocalInfo() {
        QTemporaryDir dir;
        put(dir.filePath("a.png"), "image-bytes");
        QVERIFY(QDir().mkpath(dir.filePath("sub")));
        // A file: size, mtime, SHA-256 identity, reachable.
        const SourceInfo file = statSource(dir.filePath("a.png"));
        QCOMPARE(file.path, QFileInfo(dir.filePath("a.png")).absoluteFilePath());
        QVERIFY(!file.isDir);
        QCOMPARE(file.size, qint64(11));
        QVERIFY(file.mtimeMs > 0);
        QCOMPARE(file.contentHash.toHex(), QString(QCryptographicHash::hash(
            QByteArray("image-bytes"), QCryptographicHash::Sha256).toHex()));
        QCOMPARE(file.state, SourceState::Reachable);
        // A directory listing returns children with consistent size/mtime.
        SourceInfo dirState;
        const auto entries = listDirectory(dir.path(), &dirState);
        QCOMPARE(dirState.state, SourceState::Reachable);
        QVERIFY(dirState.isDir);
        QCOMPARE(entries.size(), 2);
        bool sawFile = false, sawSub = false;
        for (const auto &e : entries) {
            if (e.isDir) { sawSub = e.path.endsWith("/sub") || e.path.endsWith("sub"); }
            else { sawFile = e.path.endsWith("a.png"); QCOMPARE(e.size, qint64(11)); }
        }
        QVERIFY(sawFile && sawSub);
        // Missing path is unreachable, not a healthy disk.
        QCOMPARE(statSource(dir.filePath("missing")).state, SourceState::Unreachable);
        // A read-only file reports ReadOnly (not silently healthy).
        if (::geteuid() != 0) {
            put(dir.filePath("ro.png"), "x");
            QVERIFY(::chmod(dir.filePath("ro.png").toLocal8Bit().constData(), 0444) == 0);
            QCOMPARE(statSource(dir.filePath("ro.png")).state, SourceState::ReadOnly);
        }
    }
    void taskQueueRunsOffThreadAndReportsProgressCancelAndResults() {
        TaskQueue queue;
        // A long task: reports progress, accumulates per-item results, and stops
        // cooperatively when cancelled at a safe item boundary.
        std::atomic_int started = 0;
        std::atomic_int finished = 0;
        QObject::connect(&queue, &TaskQueue::taskFinished, [&](qint64, bool ok, const QString &err) {
            Q_UNUSED(ok); Q_UNUSED(err);
            ++finished;
        });
        const qint64 id = queue.submit("import", [&](TaskQueue::Progress &p) {
            ++started;
            for (int i = 0; i < 100; ++i) {
                if (p.cancelled()) break;
                p.setPercent(i);
                p.setCurrent(QString("file %1").arg(i));
                p.addItem({QString("file %1").arg(i), true, QString()});
                QThread::msleep(1);
            }
            p.setPercent(100);
        });
        QTRY_VERIFY_WITH_TIMEOUT(started.load() > 0, 5000);
        queue.cancel(id);
        QTRY_COMPARE_WITH_TIMEOUT(finished.load(), 1, 5000);
        // The queue reports a single result with the items accumulated before cancel.
        const auto statuses = queue.statuses();
        QCOMPARE(statuses.size(), 1);
        QCOMPARE(statuses.first().id, id);
        QVERIFY(statuses.first().done);
        QVERIFY(statuses.first().items.size() < 100);
        // A second (non-cancelled) task completes fully with a clean result list.
        std::atomic_int done = 0;
        const qint64 id2 = queue.submit("hash", [&](TaskQueue::Progress &p) {
            for (int i = 0; i < 3; ++i) { p.addItem({QString("h%1").arg(i), true, QString()}); }
            p.setPercent(100);
            ++done;
        }, /*write=*/false);
        QTRY_COMPARE_WITH_TIMEOUT(done.load(), 1, 5000);
        const auto all = queue.statuses();
        QCOMPARE(all.size(), 2);
        const auto s2 = std::find_if(all.begin(), all.end(), [&](const TaskQueue::Status &s) { return s.id == id2; });
        QVERIFY(s2 != all.end());
        QCOMPARE(s2->items.size(), 3);
        QVERIFY(s2->done);
    }
    // 审核发现（2026-11-02）: the queue never pruned completed tasks, so a
    // long session (auto-record submits one task per change) accumulated every
    // lambda + result list. The completed tail is now bounded; running/pending
    // tasks are never dropped.
    void taskQueuePrunesCompletedTailKeepsRunning() {
        TaskQueue queue;
        std::atomic_int finished = 0;
        QObject::connect(&queue, &TaskQueue::taskFinished,
                         [&](qint64, bool, const QString &) { ++finished; });
        // Submit more than the retention cap of quick tasks, each with a result.
        const int many = 150;
        for (int i = 0; i < many; ++i)
            queue.submit(QStringLiteral("t%1").arg(i), [i](TaskQueue::Progress &p) {
                p.addItem({QStringLiteral("r%1").arg(i), true, QString()});
            }, /*write=*/false);
        QTRY_COMPARE_WITH_TIMEOUT(finished.load(), many, 30000);
        const auto statuses = queue.statuses();
        // The retained tail is bounded; a long session never grows without limit.
        QVERIFY(statuses.size() <= 64);
        QVERIFY(statuses.size() >= 1);
        // The retained tasks are the MOST RECENT ones (ids many-64..many-1) —
        // pruning must never drop the newest completed task (audit 2026-11-02).
        QVERIFY(statuses.size() == 64);
        for (int i = 0; i < statuses.size(); ++i)
            QCOMPARE(statuses.at(i).id, qint64(many - statuses.size() + i + 1));
        QCOMPARE(statuses.last().label, QStringLiteral("t%1").arg(many - 1));
        QCOMPARE(statuses.last().items.size(), 1);
        // A subsequent task still runs and reports (the queue is usable).
        std::atomic_int done2 = 0;
        const qint64 id2 = queue.submit("after-prune", [&](TaskQueue::Progress &p) {
            p.addItem({QStringLiteral("ok"), true, QString()});
            ++done2;
        }, /*write=*/false);
        QTRY_COMPARE_WITH_TIMEOUT(done2.load(), 1, 5000);
        const auto all = queue.statuses();
        const auto it = std::find_if(all.begin(), all.end(),
                                     [&](const TaskQueue::Status &s) { return s.id == id2; });
        QVERIFY(it != all.end());
        QVERIFY(it->done);
        QCOMPARE(it->items.size(), 1);
    }
    // 审核发现（0126b9443a27..060a40f）: a TaskQueue created with
    // blockOnShutdown=false must not free the teardown thread when a task is
    // still blocked in a long syscall (the backup-target probe on a dead mount).
    // The destructor detaches the worker, so it returns promptly; the detached
    // worker keeps the shared state alive and never emits into the dead queue.
    void taskQueueDetachedShutdownNeverBlocksTeardown() {
        std::atomic_int started = 0;
        {
            TaskQueue queue(nullptr, /*blockOnShutdown=*/false);
            queue.submit(QStringLiteral("blocked"), [&](TaskQueue::Progress &) {
                ++started;
                // Simulate a statvfs that hangs on a dropped mount: block until
                // the process would otherwise be stuck. The destructor must not
                // wait for this.
                std::this_thread::sleep_for(std::chrono::seconds(30));
            });
            QTRY_VERIFY_WITH_TIMEOUT(started.load() > 0, 5000);
            // The queue object is destroyed here; with blockOnShutdown=false the
            // destructor detaches the still-running worker and returns without
            // waiting for the 30 s sleep. If it waited, this test would hang for
            // 30 s (and the CI would time out).
        }
        // Reaching here within the timeout proves the destructor did not block.
        QVERIFY(started.load() > 0);
    }
    void translationMechanismLoadsAndTranslates() {
        // English: a known Library-context source string must translate.
        QTranslator en;
        QVERIFY(i18n::loadTranslator(en, "en"));
        QCOMPARE(en.translate("Library", "无法读取文件：%1"), QString("Cannot read file: %1"));
        QCOMPARE(en.translate("MainWindow", "导入素材"), QString("Import assets"));
        // Chinese is the source language: identity.
        QTranslator zh;
        QVERIFY(i18n::loadTranslator(zh, "zh_CN"));
        QCOMPARE(zh.translate("Library", "无法读取文件：%1"), QString("无法读取文件：%1"));
        // Language preference persists; Chinese is the default.
        i18n::setLanguagePreference("en");
        QCOMPARE(i18n::languagePreference(), QString("en"));
        i18n::setLanguagePreference("zh_CN");
        QCOMPARE(i18n::languagePreference(), QString("zh_CN"));
    }
    // P-608: the English UI must be genuinely English — no Chinese source text
    // left as its own translation (identity) for the feature strings added by
    // the reliability work (P-601/P-602/P-603). A Chinese-as-English string
    // would make the English interface unreadable.
    void bilingualTranslationsAreNotIdentityChinese() {
        QTranslator en;
        QVERIFY(i18n::loadTranslator(en, "en"));
        const auto expect = [&](const char *ctx, const char *src, const char *enStr) {
            QCOMPARE(en.translate(ctx, src), QString::fromUtf8(enStr));
        };
        expect("MainWindow", "一致性检查…", "Consistency check…");
        expect("MainWindow", "空间预演…", "Space preview…");
        expect("MainWindow", "恢复演练…", "Restore drill…");
        expect("MainWindow", "一致性检查", "Consistency check");
        expect("MainWindow", "空间预演", "Space preview");
        expect("Library", "恢复演练目标库非空（不覆盖既有素材库）",
               "Drill target library is not empty (an existing library is never overwritten)");
        expect("Library", "备份包清单为空：%1", "Backup package manifest is empty: %1");
        expect("MainWindow", "恢复演练失败：%1\n（演练目录未通过，备份不可视为可用）",
               "Restore drill failed: %1\n(the drill directory did not verify; the backup must not be treated as usable)");
        // Structural guard on the source .ts: every message whose source is
        // Chinese must have a translation that is not Chinese (identity would
        // leak the Chinese string into the English UI).
        const QString tsPath =
            QStringLiteral(PENTIMENTO_SOURCE_TRANSLATIONS_DIR "/pentimento_en.ts");
        QFile f(tsPath);
        QVERIFY(f.open(QIODevice::ReadOnly));
        const QString ts = QString::fromUtf8(f.readAll());
        static const QRegularExpression msgRe(
            QStringLiteral("<message>(?:(?!<message>).)*?</message>"),
            QRegularExpression::DotMatchesEverythingOption);
        static const QRegularExpression srcRe(
            QStringLiteral("<source>(.*?)</source>"),
            QRegularExpression::DotMatchesEverythingOption);
        static const QRegularExpression trRe(
            QStringLiteral("<translation>(.*?)</translation>"),
            QRegularExpression::DotMatchesEverythingOption);
        auto isCjk = [](const QString &s) {
            for (const QChar c : s)
                if (c.unicode() >= 0x4e00 && c.unicode() <= 0x9fff) return true;
            return false;
        };
        auto it = msgRe.globalMatch(ts);
        int identityCjk = 0;
        while (it.hasNext()) {
            const QString block = it.next().captured(0);
            const auto mSrc = srcRe.match(block);
            const auto mTr = trRe.match(block);
            if (!mSrc.hasMatch() || !mTr.hasMatch()) continue;
            const QString src = mSrc.captured(1);
            const QString tr = mTr.captured(1);
            if (isCjk(src) && src == tr) ++identityCjk;
        }
        QCOMPARE(identityCjk, 0);
    }

    void recentLibraryListPersistsDeduplicatesAndCaps() {
        QTemporaryDir dir;
        recent::setStoragePath(dir.filePath("settings.ini"));
        recent::clear();
        recent::remember("id-1", "/vol/a/lib");
        recent::remember("id-2", "/vol/b/lib");
        // Re-opening the same library (possibly at a new path) moves it to the
        // front and replaces the stale path instead of duplicating the entry.
        recent::remember("id-1", "/vol/c/relocated");
        const auto list = recent::libraries();
        QCOMPARE(list.size(), 2);
        QCOMPARE(list.first().id, QString("id-1"));
        QCOMPARE(list.first().path, QString("/vol/c/relocated"));
        QCOMPARE(list.at(1).id, QString("id-2"));
        // Forgetting removes exactly that library.
        recent::forget("id-2");
        QCOMPARE(recent::libraries().size(), 1);
        // The list is capped so it cannot grow without bound.
        for (int i = 0; i < 20; ++i) recent::remember(QString("id-%1").arg(i), QString("/p/%1").arg(i));
        QVERIFY(recent::libraries().size() <= 10);
        recent::clear();
        QVERIFY(recent::libraries().isEmpty());
        recent::setStoragePath(QString());
    }

    void historyObjectsSurviveWorkspaceEncryption() {
        QTemporaryDir dir;
        const auto source = dir.filePath("character.png");
        put(source, "v1 content");
        Library library(dir.filePath("library"));
        const auto asset = library.importFile(source);
        const auto v1 = library.revisions(asset.id).first();
        put(asset.path, "v2 content");
        QVERIFY(library.capture(asset, "draft 2"));
        const auto v2 = library.revisions(asset.id).first();
        // Simulated ransomware: overwrite the working file with garbage and rename
        // it to a locked extension. The managed objects must stay untouched.
        put(asset.path, QByteArray(2048, '\x7f'));
        QVERIFY(QFile::rename(asset.path, asset.path + ".locked"));
        QVERIFY(!QFileInfo::exists(asset.path));
        QCOMPARE(library.readObject(v1.hash), QByteArray("v1 content"));
        QCOMPARE(library.readObject(v2.hash), QByteArray("v2 content"));
        QVERIFY(platform::isImmutable(library.objectPath(v1.hash)));
        QVERIFY(platform::isImmutable(library.objectPath(v2.hash)));
        // Restore still works and the working file comes back with original content.
        library.restore(asset, v1.id);
        QCOMPARE(get(asset.path), QByteArray("v1 content"));
    }

    // P-209: fit scale is uniform (one factor for both axes, so a wide image is
    // never stretched), respects margins, and decode keeps aspect/alpha while a
    // huge image degrades to a bounded, proportionally-scaled read.

};

// A QApplication is required: CompareCanvas/PreviewCanvas are widgets
// and cannot be created under a QCoreApplication (canvas/journey).
int main(int argc, char **argv) {
    QApplication app(argc, argv);
    RuntimeServicesTest tc;
    QTEST_SET_MAIN_SOURCE_PATH
    return QTest::qExec(&tc, argc, argv);
}
#include "library_services_test.moc"
