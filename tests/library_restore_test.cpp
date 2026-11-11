// Restore, recovery and object immutability: restore preview/plan/commit, interrupted-restore convergence, export evidence, tamper resistance.
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

class RestoreRecoveryTest : public QObject {
    Q_OBJECT
private slots:
    void restorePreservesUnsavedWorkAndSurvivesRestart() {
        QTemporaryDir dir;
        const QString source = dir.filePath("character.png");
        put(source, "first");
        QString id, working;
        {
            Library library(dir.filePath("library"));
            const auto asset = library.importFile(source);
            id = asset.id; working = asset.path;
            const qint64 first = library.revisions(id).first().id;
            QVERIFY(!library.capture(asset));
            put(working, "second");
            QVERIFY(library.capture(asset, "draft 2"));
            put(working, "unsaved third");
            library.restore(asset, first);
            QCOMPARE(get(source), QByteArray("first"));
            QCOMPARE(get(working), QByteArray("first"));
            QCOMPARE(library.revisions(id).size(), 4);
            const auto preserved = library.revisions(id).at(1);
            library.exportRevision(id, preserved.id, dir.filePath("export.png"));
            QCOMPARE(get(dir.filePath("export.png")), QByteArray("unsaved third"));
            QVERIFY_EXCEPTION_THROWN(library.exportRevision(id, first, library.objectPath(preserved.hash)), std::runtime_error);
        }
        Library reopened(dir.filePath("library"));
        QCOMPARE(reopened.assets().first().id, id);
        QCOMPARE(reopened.revisions(id).size(), 4);
        QCOMPARE(get(working), QByteArray("first"));
    }
    void detectsCorruptionAndEnforcesRevisionOwnership() {
        QTemporaryDir dir;
        const auto source = dir.filePath("image.dat");
        put(source, "original");
        Library library(dir.filePath("library"));
        const auto a = library.importFile(source);
        const auto b = library.importFile(source);
        const auto revision = library.revisions(a.id).first();
        QCOMPARE(library.revisions(b.id).first().hash, revision.hash);
        QVERIFY_EXCEPTION_THROWN(library.restore(b, revision.id), std::runtime_error);
        // Objects are append-only/immutable; a successful tamperer clears the
        // guard first (best-effort defense), which is what this test simulates.
        QVERIFY(platform::makeMutable(library.objectPath(revision.hash)));
        put(library.objectPath(revision.hash), "corrupt");
        put(a.path, "current");
        QVERIFY_EXCEPTION_THROWN(library.restore(a, revision.id), std::runtime_error);
        QCOMPARE(get(a.path), QByteArray("current"));
    }
    void restorePreservationIsTransactionalWithTargetAppend() {
        QTemporaryDir dir;
        const auto source = dir.filePath("asset.dat");
        put(source, "v1");
        Library library(dir.filePath("library"));
        const auto asset = library.importFile(source);
        const qint64 first = library.revisions(asset.id).first().id;
        put(asset.path, "unrecorded");
        library.restore(asset, first);
        // Both the pre-save ("恢复前自动保存") and the restored version are appended.
        QCOMPARE(library.revisions(asset.id).size(), 3);
        // The restored working file matches the target revision content.
        QCOMPARE(get(asset.path), QByteArray("v1"));
        // The pre-saved unrecorded content is recoverable from history.
        const auto preserved = library.revisions(asset.id).at(1);
        library.exportRevision(asset.id, preserved.id, dir.filePath("preserved.dat"));
        QCOMPARE(get(dir.filePath("preserved.dat")), QByteArray("unrecorded"));
    }
    // P-320: a restore that fails at the file-replacement stage leaves the
    // protect revision recorded (a real version of the content that was there)
    // and the working file unchanged — no half-restored state, no lost content.
    void restoreRollsBackPreservationOnTransactionFailure() {
        if (::geteuid() == 0) QSKIP("root bypasses directory write permissions");
        QTemporaryDir dir;
        const auto source = dir.filePath("asset.dat");
        put(source, "v1");
        Library library(dir.filePath("library"));
        const auto asset = library.importFile(source);
        const qint64 first = library.revisions(asset.id).first().id;
        put(asset.path, "unrecorded");
        // Make the working-file directory unwritable so the restored working-file write fails
        // after the protect ("恢复前自动保存") revision was already committed.
        const QString workingDir = QFileInfo(asset.path).absolutePath();
        struct stat st;
        QVERIFY(::stat(workingDir.toLocal8Bit().constData(), &st) == 0);
        const mode_t original = st.st_mode & 07777;
        QVERIFY(::chmod(workingDir.toLocal8Bit().constData(), 0555) == 0);
        QVERIFY_EXCEPTION_THROWN(library.restore(asset, first), std::runtime_error);
        QVERIFY(::chmod(workingDir.toLocal8Bit().constData(), original) == 0);
        // The protect revision stays (the unrecorded content is recorded as a
        // real version); the restore revision was never appended.
        QCOMPARE(library.revisions(asset.id).size(), 2);
        QCOMPARE(library.revisions(asset.id).first().note, QStringLiteral("恢复前自动保存"));
        const auto preserved = library.revisions(asset.id).first();
        library.exportRevision(asset.id, preserved.id, dir.filePath("preserved.dat"));
        QCOMPARE(get(dir.filePath("preserved.dat")), QByteArray("unrecorded"));
        // The working file is untouched because the write never succeeded, and
        // no restore note exists in the history (nothing half-restored).
        QCOMPARE(get(asset.path), QByteArray("unrecorded"));
    }
    // P1 audit (2026-11-12): a stage-4 failure must propagate its own error —
    // a failed log update at the end of the rollback path must not swallow it.
    // Both the append and the log update are made to fail; the exception that
    // escapes must still be the stage-4 cause.
    void restoreStage4FailurePropagatesOriginalErrorOverLogFailure() {
        QTemporaryDir dir;
        put(dir.filePath("asset.dat"), "v1");
        Library library(dir.filePath("library"));
        const auto asset = library.importFile(dir.filePath("asset.dat"));
        const qint64 first = library.revisions(asset.id).first().id;
        // Unrecorded change to the working copy: the protect (stage 1) records
        // it BEFORE the log row exists, so the trigger stays silent for the
        // protect and only fires for the stage-4 append.
        put(asset.path, "v2");
        const QString conn = QStringLiteral("restore_stage4_")
                             + QUuid::createUuid().toString(QUuid::WithoutBraces);
        {
            QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), conn);
            db.setDatabaseName(dir.filePath(QStringLiteral("library/catalog.sqlite")));
            QVERIFY(db.open());
            QSqlQuery q(db);
            QVERIFY(q.exec(QStringLiteral(
                "CREATE TRIGGER fail_stage4_append BEFORE INSERT ON revisions "
                "BEGIN SELECT RAISE(ABORT, 'injected stage4 append') "
                "WHERE (SELECT COUNT(*) FROM restore_log) > 0; END")));
            QVERIFY(q.exec(QStringLiteral(
                "CREATE TRIGGER fail_stage4_log BEFORE UPDATE ON restore_log "
                "BEGIN SELECT RAISE(ABORT, 'injected stage4 log'); END")));
        }
        QSqlDatabase::removeDatabase(conn);
        bool threw = false;
        QString message;
        try {
            library.restore(asset, first);
        } catch (const std::exception &e) {
            threw = true;
            message = QString::fromUtf8(e.what());
        }
        QVERIFY(threw);
        // The stage-4 cause propagates; the failed log update must not replace it.
        QVERIFY(message.contains(QStringLiteral("injected stage4 append")));
        QVERIFY(!message.contains(QStringLiteral("injected stage4 log")));
        // The log update failed too, so the row stays planned for the
        // convergence on reopen; the working file was rolled back to "v2".
        const auto logs = library.restoreLogs();
        QCOMPARE(logs.size(), 1);
        QCOMPARE(logs.first().state, QStringLiteral("planned"));
        QCOMPARE(get(asset.path), QByteArray("v2"));
        QCOMPARE(library.revisions(asset.id).size(), 2);
        QCOMPARE(library.revisions(asset.id).first().note, QStringLiteral("恢复前自动保存"));
    }
    // P1 audit (2026-11-12): if the stage-4 rollback write itself fails, BOTH
    // causes — the stage error and the rollback-write error — must be joined
    // into the log's error field, and the stage error must still propagate.
    void restoreJoinsErrorsWhenRollbackWriteFails() {
        if (::geteuid() == 0) QSKIP("root bypasses directory write permissions");
        QTemporaryDir dir;
        // A large asset stretches stage 4 (it re-verifies the restored object)
        // into a window wide enough to make the directory unwritable AFTER the
        // stage-3 file replacement committed, without a race on the fast path.
        const QString source = dir.filePath("big.dat");
        {
            QFile f(source);
            QVERIFY(f.open(QIODevice::WriteOnly));
            const QByteArray zeros(256 * 1024, '\0');
            for (int i = 0; i < 128; ++i) QVERIFY(f.write(zeros) == zeros.size()); // 32 MiB
            f.close();
        }
        Library library(dir.filePath("library"));
        const auto asset = library.importFile(source);
        const qint64 first = library.revisions(asset.id).first().id;
        // Unrecorded change to the working copy: the protect (stage 1) records
        // it BEFORE the log row exists, so the trigger must stay silent for the
        // protect and only fire for the stage-4 append (the log row exists).
        put(asset.path, "v2");
        const QString conn = QStringLiteral("restore_join_")
                             + QUuid::createUuid().toString(QUuid::WithoutBraces);
        {
            QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), conn);
            db.setDatabaseName(dir.filePath(QStringLiteral("library/catalog.sqlite")));
            QVERIFY(db.open());
            QSqlQuery q(db);
            QVERIFY(q.exec(QStringLiteral(
                "CREATE TRIGGER fail_stage4_append BEFORE INSERT ON revisions "
                "BEGIN SELECT RAISE(ABORT, 'injected stage4 append') "
                "WHERE (SELECT COUNT(*) FROM restore_log) > 0; END")));
        }
        QSqlDatabase::removeDatabase(conn);
        const QString workingDir = QFileInfo(asset.path).absolutePath();
        const qint64 baselineMtime = QFileInfo(asset.path).lastModified().toMSecsSinceEpoch();
        std::thread watcher([&] {
            // Wait for the stage-3 replacement to commit (the target's mtime
            // moves on the atomic rename), then make the directory unwritable
            // so the catch's rollback write fails.
            const auto deadline = std::chrono::steady_clock::now()
                                  + std::chrono::seconds(60);
            while (std::chrono::steady_clock::now() < deadline) {
                if (QFileInfo(asset.path).lastModified().toMSecsSinceEpoch()
                    != baselineMtime) {
                    ::chmod(workingDir.toLocal8Bit().constData(), 0555);
                    return;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        });
        bool threw = false;
        QString message;
        try {
            library.restore(asset, first);
        } catch (const std::exception &e) {
            threw = true;
            message = QString::fromUtf8(e.what());
        }
        watcher.join();
        QVERIFY(::chmod(workingDir.toLocal8Bit().constData(), 0755) == 0);
        QVERIFY(threw);
        qWarning() << "ACTUAL MESSAGE:" << message;
        QVERIFY(message.contains(QStringLiteral("injected stage4 append")));
        const auto logs = library.restoreLogs();
        QCOMPARE(logs.size(), 1);
        QCOMPARE(logs.first().state, QStringLiteral("failed"));
        // BOTH errors are recorded, neither is lost.
        QVERIFY(logs.first().error.contains(QStringLiteral("injected stage4 append")));
        QVERIFY(logs.first().error.contains(QStringLiteral("回滚工作副本也失败")));
        // The rollback write failed, so the working file still holds the
        // restored content (the 32 MiB of zeros, not "v2"); the protect
        // revision keeps "v2" recoverable and convergence on reopen takes it
        // from the log row.
        {
            QFile working(asset.path);
            QVERIFY(working.open(QIODevice::ReadOnly));
            QCOMPARE(working.size(), qint64(32) * 1024 * 1024);
            QCOMPARE(working.read(4), QByteArray(4, '\0'));
        }
        const auto history = library.revisions(asset.id);
        QCOMPARE(history.size(), 2);
        QCOMPARE(history.first().note, QStringLiteral("恢复前自动保存"));
        library.exportRevision(asset.id, history.first().id,
                               dir.filePath("preserved.dat"));
        QCOMPARE(get(dir.filePath("preserved.dat")), QByteArray("v2"));
    }
    // P-315: the preview lists exactly what a restore would do without writing.
    void restorePreviewListsActionsWithoutWriting() {
        QTemporaryDir dir;
        const auto source = dir.filePath("asset.dat");
        put(source, "v1");
        Library library(dir.filePath("library"));
        const auto asset = library.importFile(source);
        const qint64 rev1 = library.revisions(asset.id).first().id;
        put(asset.path, "v2");
        QVERIFY(library.capture(asset, "second"));
        const qint64 rev2 = library.revisions(asset.id).first().id;
        put(asset.path, "unrecorded");

        // Overwrite: unrecorded content would be protected, target written.
        auto plan = library.planRestore(asset, rev1);
        QCOMPARE(plan.action, QStringLiteral("overwrite"));
        QCOMPARE(plan.protectBytes, qint64(10));   // "unrecorded"
        QCOMPARE(plan.targetBytes, qint64(2));     // "v1"
        QCOMPARE(plan.spaceNeeded, qint64(12));
        QCOMPARE(plan.plannedSize, qint64(10));
        // The preview itself never writes: working copy and history untouched.
        QCOMPARE(get(asset.path), QByteArray("unrecorded"));
        QCOMPARE(library.revisions(asset.id).size(), 2);

        // Skip: working copy already at the target content.
        put(asset.path, "v2");
        plan = library.planRestore(asset, rev2);
        QCOMPARE(plan.action, QStringLiteral("skip"));
        QCOMPARE(plan.spaceNeeded, qint64(0));

        // Create: a missing working copy means a new file (nothing to protect).
        QVERIFY(QFile::remove(asset.path));
        plan = library.planRestore(asset, rev1);
        QCOMPARE(plan.action, QStringLiteral("create"));
        QCOMPARE(plan.plannedSize, qint64(-1));
        QCOMPARE(plan.targetBytes, qint64(2));
        QCOMPARE(plan.spaceNeeded, qint64(2));
        QVERIFY(!QFileInfo::exists(asset.path)); // still nothing written

        // Conflict: a corrupt/missing target object is surfaced, never a partial write.
        put(asset.path, "unrecorded");
        QVERIFY(platform::makeMutable(library.objectPath(library.revisions(asset.id).at(1).hash)));
        QVERIFY(QFile::remove(library.objectPath(library.revisions(asset.id).at(1).hash)));
        plan = library.planRestore(asset, rev1);
        QCOMPARE(plan.action, QStringLiteral("conflict"));
        QVERIFY(!plan.conflictReason.isEmpty());
    }
    // P-315: executing a stale plan (working copy changed since the preview) is refused.
    void staleRestorePlanIsRejected() {
        QTemporaryDir dir;
        const auto source = dir.filePath("asset.dat");
        put(source, "v1");
        Library library(dir.filePath("library"));
        const auto asset = library.importFile(source);
        const qint64 rev1 = library.revisions(asset.id).first().id;
        put(asset.path, "v2");
        QVERIFY(library.capture(asset, "second"));

        // Plan made, then the working copy changed -> stale.
        put(asset.path, "unrecorded");
        auto plan = library.planRestore(asset, rev1);
        put(asset.path, "rewritten after preview");
        QVERIFY_EXCEPTION_THROWN(library.restore(asset, plan), std::runtime_error);
        QCOMPARE(get(asset.path), QByteArray("rewritten after preview"));

        // Plan when missing, then the file appeared -> stale.
        QVERIFY(QFile::remove(asset.path));
        plan = library.planRestore(asset, rev1);
        QCOMPARE(plan.action, QStringLiteral("create"));
        put(asset.path, "sneaked in");
        QVERIFY_EXCEPTION_THROWN(library.restore(asset, plan), std::runtime_error);

        // Plan when present, then the file disappeared -> stale.
        QVERIFY(QFile::remove(asset.path));
        put(asset.path, "v2");
        plan = library.planRestore(asset, rev1);
        QVERIFY(QFile::remove(asset.path));
        QVERIFY_EXCEPTION_THROWN(library.restore(asset, plan), std::runtime_error);
    }
    // P-316: executing a valid plan protects the current content before overwriting.
    void plannedRestoreProtectsCurrentContentBeforeOverwrite() {
        QTemporaryDir dir;
        const auto source = dir.filePath("asset.dat");
        put(source, "v1");
        Library library(dir.filePath("library"));
        const auto asset = library.importFile(source);
        const qint64 rev1 = library.revisions(asset.id).first().id;
        put(asset.path, "v2");
        QVERIFY(library.capture(asset, "second"));
        put(asset.path, "unrecorded");

        const auto plan = library.planRestore(asset, rev1);
        QCOMPARE(plan.action, QStringLiteral("overwrite"));
        library.restore(asset, plan);
        QCOMPARE(get(asset.path), QByteArray("v1"));
        // The unrecorded content became a protected version before the overwrite.
        QCOMPARE(library.revisions(asset.id).size(), 4);
        const auto preserved = library.revisions(asset.id).at(1);
        library.exportRevision(asset.id, preserved.id, dir.filePath("preserved.dat"));
        QCOMPARE(get(dir.filePath("preserved.dat")), QByteArray("unrecorded"));
    }
    // P-317: a restore forms a NEW revision and keeps every prior revision — the
    // restored content is queryable again, and the replaced version stays browsable.
    void restoreCreatesNewRevisionPreservingAllHistory() {
        QTemporaryDir dir;
        const auto source = dir.filePath("asset.dat");
        put(source, "A");
        Library library(dir.filePath("library"));
        const auto asset = library.importFile(source);
        const qint64 revA = library.revisions(asset.id).first().id;
        put(asset.path, "B");
        QVERIFY(library.capture(asset, "stage B"));
        const qint64 revB = library.revisions(asset.id).first().id;

        // A -> B -> restore A produces a new revision C; history is not truncated.
        library.restore(asset, revA);
        QCOMPARE(library.revisions(asset.id).size(), 3);
        QCOMPARE(get(asset.path), QByteArray("A"));
        const auto revisions = library.revisions(asset.id); // newest first: C, B, A
        QCOMPARE(revisions.at(1).id, revB); // B preserved, still browsable
        QCOMPARE(revisions.at(2).id, revA);

        // B remains exportable; the restored result is queryable again.
        library.exportRevision(asset.id, revB, dir.filePath("b.dat"));
        QCOMPARE(get(dir.filePath("b.dat")), QByteArray("B"));
        library.exportRevision(asset.id, revisions.at(0).id, dir.filePath("c.dat"));
        QCOMPARE(get(dir.filePath("c.dat")), QByteArray("A"));
        QCOMPARE(library.revisions(asset.id).at(0).note, QStringLiteral("恢复历史版本 #%1").arg(revA));
    }
    // P-318: selective snapshot restore touches ONLY the reviewed assets, one
    // outcome each; assets created after the snapshot and tombstoned entries are
    // skipped with a reason, never silently restored or removed.
    void snapshotSelectiveRestoreTouchesOnlyReviewedItems() {
        QTemporaryDir dir;
        Library library(dir.filePath("library"));
        put(dir.filePath("a.dat"), "a1");
        put(dir.filePath("b.dat"), "b1");
        put(dir.filePath("c.dat"), "c1");
        const auto a = library.importFile(dir.filePath("a.dat"));
        const auto b = library.importFile(dir.filePath("b.dat"));
        const auto c = library.importFile(dir.filePath("c.dat"));
        const qint64 anchor = library.snapshotAll("anchor");
        // Evolve all three, then create an asset that is NOT in the snapshot.
        put(a.path, "a2"); QVERIFY(library.capture(a, "a2"));
        put(b.path, "b2"); QVERIFY(library.capture(b, "b2"));
        put(c.path, "c2"); QVERIFY(library.capture(c, "c2"));
        put(dir.filePath("d.dat"), "d1");
        const auto d = library.importFile(dir.filePath("d.dat"));

        const auto items = library.planSnapshotRestore(anchor, {a.id, c.id, d.id});
        QCOMPARE(items.size(), 3);
        QCOMPARE(items.at(0).plan.action, QStringLiteral("overwrite"));
        QCOMPARE(items.at(2).plan.action, QStringLiteral("skip")); // d: not in snapshot
        const auto outcomes = library.restoreFromSnapshot(anchor, items);
        QCOMPARE(outcomes.size(), 3);
        QCOMPARE(outcomes.at(0).outcome, QStringLiteral("success"));
        QCOMPARE(outcomes.at(1).outcome, QStringLiteral("success"));
        QCOMPARE(outcomes.at(2).outcome, QStringLiteral("skipped"));
        // Only a and c were restored; b and the post-snapshot d are untouched.
        QCOMPARE(get(a.path), QByteArray("a1"));
        QCOMPARE(get(c.path), QByteArray("c1"));
        QCOMPARE(get(b.path), QByteArray("b2"));
        QCOMPARE(get(d.path), QByteArray("d1"));

        // A snapshot taken after a deletion records the entry as tombstone; a
        // selective restore of it is skipped (history stays, no auto-revival).
        put(dir.filePath("e.dat"), "e1");
        const auto e = library.importFile(dir.filePath("e.dat"));
        library.deleteAsset(e.id);
        const qint64 afterDelete = library.snapshotAll("after delete");
        const auto tombstoneItems = library.planSnapshotRestore(afterDelete, {e.id});
        QCOMPARE(tombstoneItems.size(), 1);
        QCOMPARE(tombstoneItems.at(0).plan.action, QStringLiteral("skip"));
        const auto tombstoneOutcomes = library.restoreFromSnapshot(afterDelete, tombstoneItems);
        QCOMPARE(tombstoneOutcomes.at(0).outcome, QStringLiteral("skipped"));
    }
    // P-319: exporting a historical copy validates the path (outside the library,
    // no symlink, no unconfirmed overwrite), verifies the object hash, and never
    // mutates the version backend or the working area.
    void exportRevisionValidatesPathAndRefusesUnconfirmedOverwrite() {
        QTemporaryDir dir;
        put(dir.filePath("asset.dat"), "v1");
        Library library(dir.filePath("library"));
        const auto asset = library.importFile(dir.filePath("asset.dat"));
        put(asset.path, "v2");
        QVERIFY(library.capture(asset, "v2"));
        const qint64 revisionsBefore = library.revisions(asset.id).size();
        const auto rev2 = library.revisions(asset.id).first();

        // Fresh destination: exports the content and verifies the object hash.
        const QString out = dir.filePath("export.dat");
        library.exportRevision(asset.id, rev2.id, out);
        QCOMPARE(get(out), QByteArray("v2"));
        // The backend and working area are untouched.
        QCOMPARE(library.revisions(asset.id).size(), revisionsBefore);
        QCOMPARE(get(asset.path), QByteArray("v2"));

        // An existing destination is refused without overwrite, allowed with it.
        QVERIFY_EXCEPTION_THROWN(library.exportRevision(asset.id, rev2.id, out), std::runtime_error);
        library.exportRevision(asset.id, rev2.id, out, /*overwrite=*/true);
        QCOMPARE(get(out), QByteArray("v2"));

        // A destination resolving into the library is refused (parent check).
        const QString inLib = library.objectPath(rev2.hash);
        QVERIFY_EXCEPTION_THROWN(library.exportRevision(asset.id, rev2.id, inLib), std::runtime_error);

        // A symlink destination is refused even when its parent is outside the
        // library (it could resolve back into objects/ or a working file).
        const QString link = dir.filePath("link.dat");
        QVERIFY(QFile::link(inLib, link));
        QVERIFY_EXCEPTION_THROWN(library.exportRevision(asset.id, rev2.id, link), std::runtime_error);
    }
    // 审核发现（2026-11-02）: exportEvidence must enforce the same symlink
    // guard as exportRevision — the "never write into the library" invariant
    // is identical on both export paths.
    void exportEvidenceRefusesSymlinkDestination() {
        QTemporaryDir dir;
        put(dir.filePath("asset.dat"), "evidence");
        Library library(dir.filePath("library"));
        const auto asset = library.importFile(dir.filePath("asset.dat"));
        const auto hash = library.revisions(asset.id).first().hash;
        const QString out = dir.filePath("out/evidence.txt");
        QDir().mkpath(dir.filePath("out"));
        library.exportEvidence(hash, out);
        QCOMPARE(get(out), QByteArray("evidence"));
        // A symlink destination is refused just like exportRevision, even
        // though its parent directory is outside the library.
        const QString link = dir.filePath("out/link.txt");
        QVERIFY(QFile::link(library.objectPath(hash), link));
        QVERIFY_EXCEPTION_THROWN(library.exportEvidence(hash, link), std::runtime_error);
    }
    // P-320: a crash between the file replacement and the index commit (file =
    // target, log row still 'planned') is converged FORWARD on reopen — the
    // restore revision is completed, the log row becomes 'done'.
    void interruptedRestoreConvergesForwardOnReopen() {
        QTemporaryDir dir;
        const QString root = dir.filePath("library");
        QString assetId;
        qint64 rev1Id = 0;
        qint64 protectId = 0;
        QString rev1Hash;
        {
            put(dir.filePath("a.dat"), "v1");
            Library lib(root);
            const auto a = lib.importFile(dir.filePath("a.dat"));
            assetId = a.id;
            rev1Id = lib.revisions(a.id).first().id;
            rev1Hash = lib.revisions(a.id).first().hash;
            put(a.path, "v2");
            QVERIFY(lib.capture(a, "v2"));
            put(a.path, "unrecorded");
            QVERIFY(lib.capture(a, QStringLiteral("恢复前自动保存"))); // stage 1 protect, committed
            protectId = lib.revisions(a.id).first().id;
            // Crash state: the file was replaced with the target (stage 3), the
            // index was never committed (stage 4) — the log row stays 'planned'.
            put(a.path, "v1");
            injectPendingRestoreLog(root, a.id, rev1Id, digestHex("unrecorded"), rev1Hash, protectId);
        }
        Library reopened(root);
        // Forward convergence: the restore revision was completed and logged done.
        QCOMPARE(reopened.restoreLogs().first().state, QStringLiteral("done"));
        QCOMPARE(reopened.restoreLogs().first().revisionId, rev1Id);
        const auto a = reopened.assets().first();
        QCOMPARE(a.id, assetId);
        QCOMPARE(get(a.path), QByteArray("v1"));
        QCOMPARE(reopened.revisions(assetId).first().note, QStringLiteral("恢复历史版本 #%1").arg(rev1Id));
        // The protect revision is still present as a real version.
        QCOMPARE(reopened.revisions(assetId).size(), 4);
    }
    // P-320: a crash before the file replacement (file = original, log row still
    // 'planned') is converged BACKWARD on reopen — nothing restored, the row is
    // marked 'aborted', and the working file is untouched.
    void interruptedRestoreConvergesBackwardOnReopen() {
        QTemporaryDir dir;
        const QString root = dir.filePath("library");
        QString assetId;
        qint64 rev1Id = 0;
        qint64 protectId = 0;
        QString rev1Hash;
        {
            put(dir.filePath("a.dat"), "v1");
            Library lib(root);
            const auto a = lib.importFile(dir.filePath("a.dat"));
            assetId = a.id;
            rev1Id = lib.revisions(a.id).first().id;
            rev1Hash = lib.revisions(a.id).first().hash;
            put(a.path, "v2");
            QVERIFY(lib.capture(a, "v2"));
            put(a.path, "unrecorded");
            QVERIFY(lib.capture(a, QStringLiteral("恢复前自动保存")));
            protectId = lib.revisions(a.id).first().id;
            // Crash before stage 3: the file is still the original content.
            injectPendingRestoreLog(root, a.id, rev1Id, digestHex("unrecorded"), rev1Hash, protectId);
        }
        Library reopened(root);
        QCOMPARE(reopened.restoreLogs().first().state, QStringLiteral("aborted"));
        const auto a = reopened.assets().first();
        QCOMPARE(get(a.path), QByteArray("unrecorded")); // untouched
        // No restore revision appended: import, v2, protect only.
        QCOMPARE(reopened.revisions(assetId).size(), 3);
    }
    void readObjectIsPublicAndVerifiesHash() {
        QTemporaryDir dir;
        const auto source = dir.filePath("asset.dat");
        put(source, "content");
        Library library(dir.filePath("library"));
        const auto asset = library.importFile(source);
        const auto revision = library.revisions(asset.id).first();
        QCOMPARE(library.readObject(revision.hash), QByteArray("content"));
        // A successful tamperer clears the immutability guard first (best-effort
        // defense); the hash check must still catch the corrupted content.
        QVERIFY(platform::makeMutable(library.objectPath(revision.hash)));
        put(library.objectPath(revision.hash), "tampered");
        QVERIFY_EXCEPTION_THROWN(library.readObject(revision.hash), std::runtime_error);
    }
    void deletedWorkingCopyCanBeRecoveredAndWriterIsExclusive() {
        QTemporaryDir dir;
        const auto root = dir.filePath("library");
        put(dir.filePath("asset"), "recover me");
        Library library(root);
        QVERIFY_EXCEPTION_THROWN(Library other(root), std::runtime_error);
        const auto asset = library.importFile(dir.filePath("asset"));
        const auto revision = library.revisions(asset.id).first();
        QVERIFY(QFile::remove(asset.path));
        library.restore(asset, revision.id);
        QCOMPARE(get(asset.path), QByteArray("recover me"));
        QCOMPARE(library.revisions(asset.id).size(), 2);
    }
    void objectFilesAreImmutableAndResistTamper() {
        QTemporaryDir dir;
        const auto source = dir.filePath("asset.dat");
        put(source, "v1");
        Library library(dir.filePath("library"));
        const auto asset = library.importFile(source);
        const auto revision = library.revisions(asset.id).first();
        const QString object = library.objectPath(revision.hash);
        QVERIFY(QFileInfo::exists(object));
        QVERIFY(platform::isImmutable(object));
        if (::geteuid() != 0) {
            // A direct write to the object file must fail while it stays immutable.
            QFile attempt(object);
            QVERIFY(!attempt.open(QIODevice::WriteOnly));
        }
        QCOMPARE(library.readObject(revision.hash), QByteArray("v1"));
        // hardenObjects re-applies the guard to any object left writable.
        QVERIFY(platform::makeMutable(object));
        QVERIFY(!platform::isImmutable(object));
        QCOMPARE(library.hardenObjects(), 1);
        QVERIFY(platform::isImmutable(object));
    }
    void openingExistingLibraryHardensObjects() {
        QTemporaryDir dir;
        const auto source = dir.filePath("asset.dat");
        put(source, "v1");
        {
            Library library(dir.filePath("library"));
            const auto asset = library.importFile(source);
            const auto revision = library.revisions(asset.id).first();
            // Simulate a tamperer that cleared the immutability guard.
            QVERIFY(platform::makeMutable(library.objectPath(revision.hash)));
            QVERIFY(!platform::isImmutable(library.objectPath(revision.hash)));
        }
        // Reopening an existing library must re-apply immutability (idempotent).
        Library reopened(dir.filePath("library"));
        const auto revision = reopened.revisions(reopened.assets().first().id).first();
        QVERIFY(platform::isImmutable(reopened.objectPath(revision.hash)));
    }

    void verifyObjectStreamsWithoutFullRead() {
        QTemporaryDir dir;
        put(dir.filePath("a.dat"), "v1");
        Library lib(dir.filePath("lib"));
        const auto a = lib.importFile(dir.filePath("a.dat"));
        const auto revs = lib.revisions(a.id);
        QVERIFY(!revs.isEmpty());
        QVERIFY(lib.verifyObject(revs.first().hash));
        QVERIFY(!lib.verifyObject(QString(64, '0'))); // absent -> false
    }
    // A revision id from another asset (or a bogus one) never restores,
    // previews, or exports — the ownership check fires before any write.
    void foreignOrBogusRevisionsAreRefusedEverywhere() {
        QTemporaryDir dir;
        put(dir.filePath("a.dat"), "a");
        put(dir.filePath("b.dat"), "b");
        Library library(dir.filePath("library"));
        const auto a = library.importFile(dir.filePath("a.dat"));
        const auto b = library.importFile(dir.filePath("b.dat"));
        const qint64 bRev = library.revisions(b.id).first().id;
        QVERIFY_EXCEPTION_THROWN(library.planRestore(a, bRev), std::runtime_error);
        QVERIFY_EXCEPTION_THROWN(library.planRestore(a, 424242), std::runtime_error);
        QVERIFY_EXCEPTION_THROWN(library.restore(a, bRev), std::runtime_error);
        QVERIFY_EXCEPTION_THROWN(library.exportRevision(a.id, bRev, dir.filePath("out.dat")), std::runtime_error);
        // Nothing was written by any of the refused calls.
        QCOMPARE(library.revisions(a.id).size(), 1);
        QCOMPARE(get(a.path), QByteArray("a"));
    }
    // P-316: an unreadable working copy cannot be protected, so the preview
    // reports a conflict and executing that plan is refused before any write.
    void unreadableWorkingCopyPlansConflictAndExecutionRefuses() {
        if (::geteuid() == 0) QSKIP("root bypasses file read permissions");
        QTemporaryDir dir;
        put(dir.filePath("a.dat"), "v1");
        Library library(dir.filePath("library"));
        const auto asset = library.importFile(dir.filePath("a.dat"));
        const qint64 rev1 = library.revisions(asset.id).first().id;
        put(asset.path, "unrecorded");
        QVERIFY(::chmod(asset.path.toLocal8Bit().constData(), 0000) == 0);
        const auto plan = library.planRestore(asset, rev1);
        QCOMPARE(plan.action, QStringLiteral("conflict"));
        QVERIFY(plan.conflictReason.contains(QStringLiteral("不可读")));
        QVERIFY_EXCEPTION_THROWN(library.restore(asset, plan), std::runtime_error);
        QVERIFY(::chmod(asset.path.toLocal8Bit().constData(), 0644) == 0);
        // Nothing was written and no version was appended by the refused plan.
        QCOMPARE(get(asset.path), QByteArray("unrecorded"));
        QCOMPARE(library.revisions(asset.id).size(), 1);
        QCOMPARE(library.restoreLogs().size(), 0);
    }
    // P-320 convergence edges: a forward convergence whose target object has
    // vanished is recorded failed (never a silent completion), and a working
    // file that an external editor changed after the interruption is flagged
    // as a conflict and left alone.
    void convergeRecordsCorruptTargetAndExternalEdit() {
        QTemporaryDir dir;
        const QString root = dir.filePath("library");
        {
            put(dir.filePath("a.dat"), "v1");
            Library lib(root);
            const auto a = lib.importFile(dir.filePath("a.dat"));
            const qint64 rev1Id = lib.revisions(a.id).first().id;
            const QString rev1Hash = lib.revisions(a.id).first().hash;
            put(a.path, "v2");
            QVERIFY(lib.capture(a, "v2"));
            // Crash state, then an external editor rewrote the working file to
            // a third content (neither the previous nor the target content).
            put(a.path, "edited by someone else");
            injectPendingRestoreLog(root, a.id, rev1Id, digestHex("v2"), rev1Hash, 0);
        }
        Library reopened(root);
        QCOMPARE(reopened.restoreLogs().first().state, QStringLiteral("conflict"));
        QVERIFY(reopened.restoreLogs().first().error.contains(QStringLiteral("外部修改")));
        const auto a = reopened.assets().first();
        QCOMPARE(get(a.path), QByteArray("edited by someone else")); // left alone
        QCOMPARE(reopened.revisions(a.id).size(), 2); // nothing invented
    }
    // Audit (2026-11-12, P0): convergeRestoreLog hashed the working copy with
    // readFile(), which caps at 256 MiB and throws. A working copy over the cap
    // made the Library constructor throw inside convergeRestoreLog (with the
    // lock already held), leaving the whole library unopenable. It now streams
    // via the non-throwing hashFile and degrades an unhashable/oversize copy to
    // a conflict, never a constructor failure.
    void convergeOversizeWorkingCopyDegradesToConflict() {
        QTemporaryDir dir;
        const QString root = dir.filePath("library");
        QString assetId;
        qint64 rev1Id = 0;
        QString rev1Hash;
        {
            put(dir.filePath("a.dat"), "v1");
            Library lib(root);
            const auto a = lib.importFile(dir.filePath("a.dat"));
            assetId = a.id;
            rev1Id = lib.revisions(a.id).first().id;
            rev1Hash = lib.revisions(a.id).first().hash;
            // A working copy just over the old 256 MiB readFile cap. The
            // streaming hash handles it; the old readFile threw here, so the
            // library could never be reopened.
            QFile huge(a.path);
            QVERIFY(huge.open(QIODevice::WriteOnly));
            QVERIFY(huge.resize(257LL * 1024 * 1024));
            huge.close();
            // Third content (neither prev nor target): must converge to conflict.
            injectPendingRestoreLog(root, a.id, rev1Id, digestHex("v2"), rev1Hash, 0);
        }
        // Reopening succeeds (no throw from convergeRestoreLog) and the row is
        // flagged conflict; the over-limit working copy is left untouched and
        // no restore revision was invented.
        Library reopened(root);
        QCOMPARE(reopened.restoreLogs().first().state, QStringLiteral("conflict"));
        QVERIFY(reopened.restoreLogs().first().error.contains(QStringLiteral("未自动处理")));
        QCOMPARE(reopened.assets().first().id, assetId);
        QCOMPARE(QFileInfo(reopened.assets().first().path).size(), qint64(257LL * 1024 * 1024));
        QCOMPARE(reopened.revisions(assetId).size(), 1);
    }
    // The forward-convergence path with a vanished target object must mark the
    // row failed with a concrete reason instead of claiming completion.
    void convergeForwardWithMissingObjectMarksFailed() {
        QTemporaryDir dir;
        const QString root = dir.filePath("library");
        QString assetId;
        qint64 rev1Id = 0;
        QString rev1Hash;
        {
            put(dir.filePath("a.dat"), "v1");
            Library lib(root);
            const auto a = lib.importFile(dir.filePath("a.dat"));
            assetId = a.id;
            rev1Id = lib.revisions(a.id).first().id;
            rev1Hash = lib.revisions(a.id).first().hash;
            put(a.path, "v2");
            QVERIFY(lib.capture(a, "v2"));
            put(a.path, "v1"); // file = target: forward convergence will be attempted
            injectPendingRestoreLog(root, a.id, rev1Id, digestHex("v2"), rev1Hash, 0);
            // A successful tamperer clears the immutability guard before deleting.
            QVERIFY(platform::makeMutable(lib.objectPath(rev1Hash)));
            QVERIFY(QFile::remove(lib.objectPath(rev1Hash)));
        }
        Library reopened(root);
        QCOMPARE(reopened.restoreLogs().first().state, QStringLiteral("failed"));
        QVERIFY(reopened.restoreLogs().first().error.contains(QStringLiteral("损坏"))
                || reopened.restoreLogs().first().error.contains(QStringLiteral("无法完成")));
        QCOMPARE(reopened.revisions(assetId).size(), 2); // no restore revision invented
    }
    // P-306/P-318: excluded snapshot entries and assets deleted after the
    // snapshot are skipped with a reason, never restored.
    void snapshotPlanSkipsExcludedAndVanishedAssets() {
        QTemporaryDir dir;
        Library library(dir.filePath("library"));
        put(dir.filePath("keep.dat"), "k1");
        put(dir.filePath("skip.tmp"), "s1");
        const auto keep = library.importFile(dir.filePath("keep.dat"));
        const auto skip = library.importFile(dir.filePath("skip.tmp"));
        library.addExclusion(QStringLiteral("extension"), QStringLiteral("tmp"));
        const qint64 anchor = library.snapshotAll("with exclusion");
        const auto summary = std::find_if(library.snapshots().begin(), library.snapshots().end(),
                                          [&](const Snapshot &s) { return s.id == anchor; });
        QVERIFY(summary != library.snapshots().end());
        QCOMPARE(summary->excluded, 1);
        // The excluded entry plans as a skip and never restores.
        const auto items = library.planSnapshotRestore(anchor, {skip.id});
        QCOMPARE(items.size(), 1);
        QCOMPARE(items.at(0).plan.action, QStringLiteral("skip"));
        QVERIFY(items.at(0).plan.conflictReason.contains(QStringLiteral("排除")));
        const auto outcomes = library.restoreFromSnapshot(anchor, items);
        QCOMPARE(outcomes.at(0).outcome, QStringLiteral("skipped"));
        QCOMPARE(get(skip.path), QByteArray("s1"));
        // An asset deleted AFTER the snapshot vanished from the live list but
        // its snapshot entry is still 'present': skip, no auto-revival.
        const qint64 beforeDelete = library.snapshotAll("before delete");
        library.deleteAsset(keep.id);
        const auto vanished = library.planSnapshotRestore(beforeDelete, {keep.id});
        QCOMPARE(vanished.size(), 1);
        QCOMPARE(vanished.at(0).plan.action, QStringLiteral("skip"));
        QVERIFY(vanished.at(0).plan.conflictReason.contains(QStringLiteral("不存在")));
    }
    // P-318 execution: a plan for an asset that no longer exists fails its own
    // item; a stale plan fails that item too — the batch never aborts.
    void snapshotRestoreReportsPerItemFailures() {
        QTemporaryDir dir;
        put(dir.filePath("a.dat"), "a1");
        put(dir.filePath("b.dat"), "b1");
        Library library(dir.filePath("library"));
        const auto a = library.importFile(dir.filePath("a.dat"));
        const auto b = library.importFile(dir.filePath("b.dat"));
        const qint64 anchor = library.snapshotAll("anchor");
        put(a.path, "a2"); QVERIFY(library.capture(a, "a2"));
        put(b.path, "b2"); QVERIFY(library.capture(b, "b2"));
        auto items = library.planSnapshotRestore(anchor, {a.id, b.id});
        QCOMPARE(items.at(0).plan.action, QStringLiteral("overwrite"));
        QCOMPARE(items.at(1).plan.action, QStringLiteral("overwrite"));
        // The working copy changes after the preview: that item's plan is stale.
        put(a.path, "externally rewritten");
        const auto outcomes = library.restoreFromSnapshot(anchor, items);
        QCOMPARE(outcomes.size(), 2);
        QCOMPARE(outcomes.at(0).outcome, QStringLiteral("failed"));
        QVERIFY(!outcomes.at(0).error.isEmpty());
        QCOMPARE(get(a.path), QByteArray("externally rewritten")); // untouched
        QCOMPARE(outcomes.at(1).outcome, QStringLiteral("success"));
        QCOMPARE(get(b.path), QByteArray("b1"));
        // A hand-built plan for an asset that is not in the library at all
        // fails as its own item (never throws across the batch).
        Library::SnapshotRestoreItem ghost;
        ghost.assetId = QStringLiteral("ghost");
        ghost.name = QStringLiteral("ghost.dat");
        ghost.plan.action = QStringLiteral("overwrite");
        ghost.plan.revisionId = 1;
        const auto ghostOutcomes = library.restoreFromSnapshot(anchor, {ghost});
        QCOMPARE(ghostOutcomes.size(), 1);
        QCOMPARE(ghostOutcomes.at(0).outcome, QStringLiteral("failed"));
        QVERIFY(ghostOutcomes.at(0).error.contains(QStringLiteral("不存在")));
    }
    // Delete confirms intent on the working copy: an unremovable file blocks
    // the delete (never a half-deleted asset), a removable one tombstones.
    void deleteAssetBlocksOnUnremovableWorkingCopy() {
        if (::geteuid() == 0) QSKIP("root bypasses directory write permissions");
        QTemporaryDir dir;
        put(dir.filePath("a.dat"), "v1");
        Library library(dir.filePath("library"));
        const auto asset = library.importFile(dir.filePath("a.dat"));
        const QString workingDir = QFileInfo(asset.path).absolutePath();
        struct stat st;
        QVERIFY(::stat(workingDir.toLocal8Bit().constData(), &st) == 0);
        const mode_t original = st.st_mode & 07777;
        QVERIFY(::chmod(workingDir.toLocal8Bit().constData(), 0500) == 0);
        QVERIFY_EXCEPTION_THROWN(library.deleteAsset(asset.id), std::runtime_error);
        QVERIFY(::chmod(workingDir.toLocal8Bit().constData(), original) == 0);
        // The blocked delete tombstoned nothing.
        QCOMPARE(library.revisions(asset.id).size(), 1);
        QVERIFY(library.assets().size() == 1);
        // With the file removable the delete records the tombstone revision,
        // removes the working copy, and keeps history browsable.
        library.deleteAsset(asset.id);
        QVERIFY(!QFileInfo::exists(asset.path));
        QVERIFY(library.assets().isEmpty());
        const auto history = library.revisions(asset.id);
        QCOMPARE(history.size(), 2);
        QCOMPARE(history.first().note, QStringLiteral("删除"));
        QVERIFY(history.first().tombstone);
        QVERIFY_EXCEPTION_THROWN(library.deleteAsset(asset.id), std::runtime_error); // already deleted
    }

};

// A QApplication is required: CompareCanvas/PreviewCanvas are widgets
// and cannot be created under a QCoreApplication (canvas/journey).
int main(int argc, char **argv) {
    QApplication app(argc, argv);
    RestoreRecoveryTest tc;
    QTEST_SET_MAIN_SOURCE_PATH
    return QTest::qExec(&tc, argc, argv);
}
#include "library_restore_test.moc"
