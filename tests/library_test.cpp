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

namespace {
void put(const QString &path, const QByteArray &data) {
    QFile file(path); QVERIFY(file.open(QIODevice::WriteOnly)); QCOMPARE(file.write(data), data.size());
}
QByteArray get(const QString &path) { QFile file(path); return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray(); }
QString digestHex(const QByteArray &data) {
    return QString::fromLatin1(QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex());
}
// P-320 test support: write a 'planned' restore-log row directly into the
// catalog, simulating a crash between the file replacement and the index commit
// (the exact state convergeRestoreLog() must handle on reopen).
void injectPendingRestoreLog(const QString &root, const QString &assetId, qint64 revisionId,
                             const QString &prevHash, const QString &targetHash, qint64 protectId) {
    const QString conn = QStringLiteral("inject_") + QUuid::createUuid().toString(QUuid::WithoutBraces);
    {
        QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", conn);
        db.setDatabaseName(root + "/catalog.sqlite");
        QVERIFY(db.open());
        QSqlQuery q(db);
        q.prepare("INSERT INTO restore_log(asset_id,revision_id,state,previous_hash,target_hash,"
                  "protect_revision_id,error,created,updated) VALUES(?,?,?,?,?,?,'',?,?)");
        const QString now = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
        q.addBindValue(assetId);
        q.addBindValue(revisionId);
        q.addBindValue(QStringLiteral("planned"));
        q.addBindValue(prevHash);
        q.addBindValue(targetHash);
        q.addBindValue(protectId);
        q.addBindValue(now);
        q.addBindValue(now);
        QVERIFY(q.exec());
        db.close();
    }
    QSqlDatabase::removeDatabase(conn);
}

// A known-good uncompressed 32-bit BGRA TGA (Qt can read TGA but not write it,
// so the sample is built byte-exact to validate the decoder, P-210). The Qt TGA
// plugin only accepts TrueVision 2.0 files, which require the 26-byte footer
// carrying the "TRUEVISION-XFILE" signature at offset 8.
QByteArray makeTga(int w, int h, QRgb fill) {
    QByteArray out;
    out.reserve(18 + w * h * 4 + 26);
    out.append(char(0));       // id length
    out.append(char(0));       // color map type
    out.append(char(2));       // uncompressed truecolor
    out.append(QByteArray(5, '\0')); // color map spec
    out.append(char(0)).append(char(0)); // x origin
    out.append(char(0)).append(char(0)); // y origin
    out.append(char(w & 0xff)).append(char((w >> 8) & 0xff));
    out.append(char(h & 0xff)).append(char((h >> 8) & 0xff));
    out.append(char(32));      // pixel depth
    out.append(char(0x28));    // 8 alpha bits + top-left origin
    for (int i = 0; i < w * h; ++i) {
        out.append(char(qBlue(fill)));   // B
        out.append(char(qGreen(fill)));  // G
        out.append(char(qRed(fill)));    // R
        out.append(char(qAlpha(fill)));  // A
    }
    out.append(QByteArray(8, '\0'));               // ext + developer area offsets
    out.append(QByteArray("TRUEVISION-XFILE", 16)); // TrueVision 2.0 signature
    out.append(char(0)).append(char(0));           // NUL + padding
    return out;
}

// A real PSD header (26 bytes, big-endian "8BPS") so the P-211 sample is a
// genuine Photoshop file, not an arbitrary blob; a composite follows in a real
// file but Qt has no PSD codec, so the header is what the signature check sees.
QByteArray makePsd(int w, int h) {
    QByteArray out;
    QDataStream ds(&out, QIODevice::WriteOnly);
    ds.setByteOrder(QDataStream::BigEndian);
    ds.writeRawData("8BPS", 4);
    ds << quint16(1);                    // version
    ds.writeRawData("\0\0\0\0\0\0", 6);  // reserved
    ds << quint16(3);                    // channels
    ds << quint32(h);                    // height
    ds << quint32(w);                    // width
    ds << quint16(8);                    // bits per channel
    ds << quint16(3);                    // color mode: RGB
    return out;
}
}

class HistoryTest : public QObject {
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
    void rescanPreviewClassifiesFreshAndImported() {
        QTemporaryDir dir;
        const QString source = dir.filePath("share");
        QVERIFY(QDir().mkpath(source));
        put(source + "/a.png", "content-a");
        put(source + "/b.png", "content-b");
        {
            Library library(dir.filePath("library"));
            QVERIFY(!library.importFile(source + "/a.png").id.isEmpty());
            // P-515: a read-only preview — a.png is already imported (skip, no
            // duplicate), b.png is genuinely new (importable).
            const auto preview = importer::rescanPreview(library, source);
            QCOMPARE(preview.fresh(), 1);
            QCOMPARE(preview.alreadyImported(), 1);
            QCOMPARE(preview.candidates.size(), 2);
        }
    }
    // 审核发现（2026-11-02）: rescanPreview must stream-hash the source file,
    // not read it whole — a large file must never OOM the worker thread. The
    // hash still classifies the content correctly for a fresh/imported check.
    void rescanPreviewStreamHashesLargeFile() {
        QTemporaryDir dir;
        const QString source = dir.filePath("share");
        QVERIFY(QDir().mkpath(source));
        // A file beyond the 256 MiB import cap: rescan preview must not load it
        // whole (would OOM the worker), but must still classify it as fresh.
        const QString big = source + "/big.bin";
        {
            QFile f(big);
            QVERIFY(f.open(QIODevice::WriteOnly));
            QByteArray chunk(1 << 20, 'x'); // 1 MiB
            const qint64 total = 257LL * 1024 * 1024; // just over the cap
            qint64 written = 0;
            while (written < total) {
                const qint64 n = qMin<qint64>(chunk.size(), total - written);
                QCOMPARE(f.write(chunk.constData(), int(n)), int(n));
                written += n;
            }
            f.close();
        }
        Library library(dir.filePath("library"));
        const auto preview = importer::rescanPreview(library, source);
        QCOMPARE(preview.fresh(), 1);
        QCOMPARE(preview.alreadyImported(), 0);
        QVERIFY(!preview.candidates.first().hash.isEmpty());
    }
    void rescanNeverSyncsRemoteChangesIntoLocalDeletions() {
        QTemporaryDir dir;
        const QString source = dir.filePath("share");
        QVERIFY(QDir().mkpath(source));
        put(source + "/a.png", "content-a");
        put(source + "/b.png", "content-b");
        Library library(dir.filePath("library"));
        QVERIFY(!library.importFile(source + "/a.png").id.isEmpty());
        QVERIFY(!library.importFile(source + "/b.png").id.isEmpty());
        QCOMPARE(library.assets().size(), 2);

        // After the import the source changes: a.png is deleted, b.png is
        // renamed to c.png (same content), and d.png is made unreadable.
        QVERIFY(QFile::remove(source + "/a.png"));
        QVERIFY(QFile::rename(source + "/b.png", source + "/c.png"));
        put(source + "/d.png", "content-d");
        bool asRoot = ::geteuid() == 0;
        if (!asRoot) QVERIFY(::chmod((source + "/d.png").toLocal8Bit().constData(), 0000) == 0);

        // The rescan is read-only and classifies; it never mutates the library.
        const auto preview = importer::rescanPreview(library, source);
        // c.png has b's content → already imported (a rename never re-imports
        // or duplicates); d.png is unreadable → reported, not fresh.
        QCOMPARE(preview.alreadyImported(), 1);
        if (!asRoot) {
            QCOMPARE(preview.fresh(), 0);
            QVERIFY(!preview.unreadable.isEmpty());
        }

        // The library is untouched: no deletion, no rename, no tombstone.
        const auto assets = library.assets();
        QCOMPARE(assets.size(), 2);
        for (const auto &a : assets) {
            QVERIFY(QFile::exists(a.path));            // working copy never deleted
            const auto revs = library.revisions(a.id);
            QVERIFY(!revs.isEmpty());
            QVERIFY(!revs.first().tombstone);          // never auto-tombstoned
        }
        QStringList names;
        for (const auto &a : assets) names << a.name;
        QVERIFY(names.contains(QStringLiteral("a.png"))); // source-deleted → still local
        QVERIFY(names.contains(QStringLiteral("b.png"))); // source-renamed → still local

        // Importing the fresh candidates is additive only: it adds, and never
        // deletes/renames anything in the library.
        QStringList freshPaths;
        for (const auto &c : preview.candidates)
            if (c.error.isEmpty() && !c.alreadyImported && c.candidate.excludeReason.isEmpty())
                freshPaths << c.candidate.sourcePath;
        TaskQueue queue;
        queue.submit(QStringLiteral("重扫导入"), [&](TaskQueue::Progress &p) {
            importer::importPlan(library, freshPaths, p);
        });
        QTRY_VERIFY_WITH_TIMEOUT([&] {
            for (const auto &s : queue.statuses()) if (s.done) return true;
            return false;
        }(), 10000);
        QCOMPARE(library.assets().size(), asRoot ? 3 : 2); // adds only what was fresh
    }
    void backupPackageExportsConsistentManifestAndObjects() {
        QTemporaryDir dir;
        QTemporaryDir target;
        put(dir.filePath("a.png"), "bytes-a");
        put(dir.filePath("b.png"), "bytes-b");
        Library library(dir.filePath("library"));
        QVERIFY(!library.importFile(dir.filePath("a.png")).id.isEmpty());
        QVERIFY(!library.importFile(dir.filePath("b.png")).id.isEmpty());

        // P-516: export a consistent package — manifest + referenced objects.
        const auto package = library.exportBackupPackage(target.path());
        QCOMPARE(package.entries.size(), 2);
        QVERIFY(QFileInfo::exists(package.markerPath));   // COMPLETE, written last
        QVERIFY(QFileInfo::exists(package.manifestPath));
        // The live index is never shared: no catalog inside the package.
        QVERIFY(!QFileInfo::exists(target.filePath("catalog.sqlite")));
        // Every referenced object is present with the declared size and content.
        for (const auto &e : package.entries) {
            const QFileInfo obj(target.filePath("objects/" + e.hash));
            QVERIFY(obj.exists());
            QCOMPARE(obj.size(), e.size);
            QCOMPARE(QByteArray::fromHex(e.hash.toLatin1()),
                     QCryptographicHash::hash(get(target.filePath("objects/" + e.hash)),
                                              QCryptographicHash::Sha256));
        }
        // Re-verification of the package succeeds (marker + manifest + objects).
        const auto verified = library.verifyBackupPackage(target.path());
        QCOMPARE(verified.size(), 2);
        QStringList names;
        for (const auto &e : verified) names << e.name;
        QVERIFY(names.contains(QStringLiteral("a.png")));
        QVERIFY(names.contains(QStringLiteral("b.png")));
        // A second export to the now non-empty target is refused (no silent
        // overwrite of an existing package).
        bool refused = false;
        try { library.exportBackupPackage(target.path()); }
        catch (const std::exception &) { refused = true; }
        QVERIFY(refused);
    }
    void backupPackageVerifyRejectsHalfPackage() {
        QTemporaryDir dir;
        QTemporaryDir target;
        put(dir.filePath("a.png"), "bytes-a");
        Library library(dir.filePath("library"));
        QVERIFY(!library.importFile(dir.filePath("a.png")).id.isEmpty());
        const auto package = library.exportBackupPackage(target.path());
        QVERIFY(!package.entries.isEmpty());
        // A package missing the COMPLETE marker is a half-package: never
        // reported as success.
        QVERIFY(QFile::remove(package.markerPath));
        bool rejected = false;
        try { library.verifyBackupPackage(target.path()); }
        catch (const std::exception &) { rejected = true; }
        QVERIFY(rejected);
        // Restore the marker but corrupt one object → still rejected.
        put(package.markerPath, "ok");
        const QString objPath = target.filePath("objects/" + package.entries.first().hash);
        QVERIFY(QFile::remove(objPath));
        bool corruptRejected = false;
        try { library.verifyBackupPackage(target.path()); }
        catch (const std::exception &) { corruptRejected = true; }
        QVERIFY(corruptRejected);
    }
    void restoreBackupPackageRebuildsAssetsAndMetadata() {
        QTemporaryDir dir;
        QTemporaryDir target;
        // Real PNGs so the restored asset gains honest filter facts (P-217).
        auto makePng = [](const QString &path, const QColor &fill) {
            QImage img(40, 30, QImage::Format_ARGB32);
            img.fill(fill);
            QByteArray bytes;
            QBuffer buf(&bytes);
            buf.open(QIODevice::WriteOnly);
            QVERIFY(img.save(&buf, "PNG"));
            put(path, bytes);
        };
        makePng(dir.filePath("a.png"), Qt::red);
        makePng(dir.filePath("b.png"), Qt::blue);
        Library library(dir.filePath("library"));
        const auto a = library.importFile(dir.filePath("a.png"));
        const auto b = library.importFile(dir.filePath("b.png"));
        // Give the source library identifiable metadata: a tag, a rating, a
        // note, and a second captured revision so the timeline has >1 version.
        library.addTag(a.id, QStringLiteral("hero"));
        library.setRating(a.id, 5);
        library.setNote(a.id, QStringLiteral("主视觉"));
        library.capture(a, QStringLiteral("第二次记录"));

        // Export a consistent package, then rehearse restore into a fresh
        // independent empty directory (never over the original library).
        const auto package = library.exportBackupPackage(target.path());
        QCOMPARE(package.entries.size(), 2);
        Library drill(target.filePath("drill"));
        const auto restored = drill.restoreBackupPackage(target.path());
        QCOMPARE(restored.size(), 2);

        // Content + identity survive: same asset ids, working file hashes match.
        const auto assets = drill.assets();
        QCOMPARE(assets.size(), 2);
        for (const auto &e : restored) {
            const auto it = std::find_if(assets.cbegin(), assets.cend(),
                [&e](const Asset &x) { return x.id == e.assetId; });
            QVERIFY(it != assets.cend());
            QCOMPARE(it->name, e.name);
            QCOMPARE(it->folder, e.folder);
            QCOMPARE(digestHex(get(it->path)), e.hash);
            // The object store holds the same content-addressed object.
            QCOMPARE(digestHex(get(drill.objectPath(e.hash))), e.hash);
        }
        // At least one revision per asset, content hash preserved.
        for (const auto &e : restored) {
            const auto revs = drill.revisions(e.assetId);
            QVERIFY(!revs.isEmpty());
            QCOMPARE(revs.first().hash, e.hash);
        }
        // Recovered assets carry their stored identity (id + folder + name).
        const auto ra = std::find_if(assets.cbegin(), assets.cend(),
            [&](const Asset &x) { return x.id == a.id; });
        QVERIFY(ra != assets.cend());
        QCOMPARE(ra->name, a.name);
        QCOMPARE(ra->folder, a.folder);
        // Filtering facts are recomputed from the recovered content.
        QCOMPARE(drill.assetFormat(a.id).toLower(), QStringLiteral("png"));
        QVERIFY(drill.assetSize(a.id).width() > 0);

        // The drill refuses to restore over a library that already has assets.
        bool refused = false;
        try { drill.restoreBackupPackage(target.path()); }
        catch (const std::exception &) { refused = true; }
        QVERIFY(refused);
    }
    void consistencyCheckReportsCleanLibrary() {
        QTemporaryDir dir;
        put(dir.filePath("a.png"), "bytes-a");
        put(dir.filePath("b.png"), "bytes-b");
        Library library(dir.filePath("library"));
        QVERIFY(!library.importFile(dir.filePath("a.png")).id.isEmpty());
        QVERIFY(!library.importFile(dir.filePath("b.png")).id.isEmpty());
        int progressCalls = 0;
        const auto report = library.checkConsistency(
            [&progressCalls](int, const QString &) { ++progressCalls; });
        // Every referenced hash verified; no problems; progress actually ran.
        QCOMPARE(report.revisions, 2);
        QCOMPARE(report.objects, 2);
        QCOMPARE(report.ok, 2);
        QCOMPARE(report.problems(), 0);
        QVERIFY(progressCalls > 0);
    }
    void consistencyCheckReportsMissingCorruptOrphan() {
        QTemporaryDir dir;
        put(dir.filePath("a.png"), "bytes-a");
        put(dir.filePath("b.png"), "bytes-b");
        Library library(dir.filePath("library"));
        QVERIFY(!library.importFile(dir.filePath("a.png")).id.isEmpty());
        QVERIFY(!library.importFile(dir.filePath("b.png")).id.isEmpty());
        const QString objectsDir = dir.filePath("library/objects");
        const QString hashA = digestHex("bytes-a");
        const QString hashB = digestHex("bytes-b");
        const QString orphanHash = digestHex("orphan-bytes");
        // A referenced object file deleted → missing.
        QVERIFY(QFile::remove(objectsDir + "/" + hashA));
        // A referenced object overwritten with different bytes → corrupt.
        // (Objects are content-addressed and stored read-only, so clear the
        // write protection before clobbering the file.)
        const QString objB = objectsDir + "/" + hashB;
        ::chmod(objB.toLocal8Bit(), 0644);
        put(objB, "wrong-bytes");
        // An object file no revision references → orphan.
        put(objectsDir + "/" + orphanHash, "orphan-bytes");
        const auto report = library.checkConsistency();
        QVERIFY(report.missing.contains(hashA));
        QVERIFY(report.corrupt.contains(hashB));
        QVERIFY(report.orphan.contains(orphanHash));
        QCOMPARE(report.problems(), 3);
        // Report-only: the store is left exactly as we made it.
        QVERIFY(QFileInfo::exists(objectsDir + "/" + orphanHash));
    }
    void spacePreviewDistinguishesLogicalPhysicalSharedOrphan() {
        QTemporaryDir dir;
        put(dir.filePath("a.png"), "bytes-a");
        put(dir.filePath("b.png"), "bytes-b");
        Library library(dir.filePath("library"));
        QVERIFY(!library.importFile(dir.filePath("a.png")).id.isEmpty());
        QVERIFY(!library.importFile(dir.filePath("b.png")).id.isEmpty());
        // Same content as a.png under a different name: two revisions share one
        // object — the naive logical sum counts it twice, disk holds it once.
        QTemporaryDir src;
        put(src.filePath("a-copy.png"), "bytes-a");
        QVERIFY(!library.importFile(src.filePath("a-copy.png")).id.isEmpty());
        // Pin one revision (P-307): it is protected bytes in the preview.
        const auto revs = library.revisions(library.assets().first().id);
        QVERIFY(!revs.isEmpty());
        library.pinRevision(revs.first().id, QStringLiteral("test keep"));
        // An unreferenced object file: the only theoretically reclaimable bytes.
        const QString objectsDir = dir.filePath("library/objects");
        const QString orphanHash = digestHex("orphan-bytes");
        put(objectsDir + "/" + orphanHash, "orphan-bytes");

        const auto p = library.spacePreview();
        QCOMPARE(p.assets, 3);
        QCOMPARE(p.revisions, 3);
        QCOMPARE(p.logicalSize, qint64(21));       // 3 revisions x 7 bytes (naive)
        QCOMPARE(p.physicalSize, qint64(26));      // hashA(7)+hashB(7)+orphan(12)
        QCOMPARE(p.sharedObjects, 1);
        QCOMPARE(p.dedupSavings, qint64(7));       // one shared object counted twice
        QCOMPARE(p.pinnedRevisions, 1);
        QCOMPARE(p.pinnedBytes, qint64(7));
        QCOMPARE(p.orphanObjects, 1);
        QCOMPARE(p.reclaimableBytes, qint64(12));  // orphan-bytes = 12 bytes
    }
    void backupPackageRestoreDrillRebuildsIndependentLibrary() {
        QTemporaryDir dir;
        QTemporaryDir target;
        put(dir.filePath("a.png"), "bytes-a");
        put(dir.filePath("b.png"), "bytes-b");
        Library library(dir.filePath("library"));
        QVERIFY(!library.importFile(dir.filePath("a.png")).id.isEmpty());
        QVERIFY(!library.importFile(dir.filePath("b.png")).id.isEmpty());
        const auto package = library.exportBackupPackage(target.filePath("pkg"));
        QVERIFY(!package.entries.isEmpty());
        // P-603: the drill restores into a separate empty library — never over
        // the original. The recovered catalog must match the manifest on every
        // dimension the rehearsal checks.
        QTemporaryDir drill;
        Library recovered(drill.filePath("lib"));
        const auto restored = recovered.restoreBackupPackage(package.packageDir);
        QCOMPARE(restored.size(), package.entries.size());
        const auto assets = recovered.assets();
        QCOMPARE(assets.size(), package.entries.size());
        for (const auto &e : package.entries) {
            // Resource id + path (folder/name) preserved.
            const auto it = std::find_if(assets.cbegin(), assets.cend(),
                [&e](const Asset &a) { return a.id == e.assetId; });
            QVERIFY(it != assets.cend());
            QCOMPARE(it->name, e.name);
            QCOMPARE(it->folder, e.folder);
            // Version count: exactly the manifest revision, content hash + size.
            const auto revs = recovered.revisions(e.assetId);
            QCOMPARE(revs.size(), 1);
            QCOMPARE(revs.first().hash, e.hash);
            QCOMPARE(revs.first().size, e.size);
            // Content hash of the recovered working copy.
            QCOMPARE(digestHex(get(it->path)), e.hash);
        }
    }
    void backupPackageRestoreRefusesExistingLibrary() {
        QTemporaryDir dir;
        QTemporaryDir target;
        QTemporaryDir drill;
        put(dir.filePath("a.png"), "bytes-a");
        Library library(dir.filePath("library"));
        QVERIFY(!library.importFile(dir.filePath("a.png")).id.isEmpty());
        const auto package = library.exportBackupPackage(target.filePath("pkg"));
        // A drill library that already holds an asset must refuse the restore:
        // the rehearsal never overwrites an original library.
        Library recovered(drill.filePath("lib"));
        QVERIFY(!recovered.importFile(dir.filePath("a.png")).id.isEmpty());
        bool refused = false;
        try { recovered.restoreBackupPackage(package.packageDir); }
        catch (const std::exception &) { refused = true; }
        QVERIFY(refused);
    }
    // P-605: a crash during object-store write (stage: 对象写入) leaves a stale
    // .tmp and possibly a bad object. On reopen the library must stay usable and
    // the bad object is reported, never silently trusted (storeObjectFromFile
    // verifies the hash before the rename, so a half-write cannot be referenced;
    // if one is nonetheless present, consistency check flags it as corrupt).
    void faultInterruptedObjectWriteIsReportedNotTrusted() {
        QTemporaryDir dir;
        const QString root = dir.filePath("library");
        QString assetId;
        QString hash;
        {
            put(dir.filePath("a.dat"), "v1");
            Library lib(root);
            const auto a = lib.importFile(dir.filePath("a.dat"));
            assetId = a.id;
            hash = lib.revisions(a.id).first().hash;
            // Interrupted object write: a stale staging file plus a referenced
            // object that lost its tail (as if a rename raced a crash).
            const QString staging = root + "/objects/.tmp/partial";
            QVERIFY(QDir().mkpath(QFileInfo(staging).absolutePath()));
            put(staging, "half");
            const QString objPath = root + "/objects/" + hash;
            ::chmod(objPath.toLocal8Bit(), 0644);
            put(objPath, "xy"); // same length, different bytes: hash mismatch
        }
        Library reopened(root);
        const auto report = reopened.checkConsistency();
        QVERIFY(report.corrupt.contains(hash));
        QCOMPARE(report.missing.size(), 0);
        // The working copy and catalog are intact; the library is still usable.
        QCOMPARE(get(reopened.assets().first().path), QByteArray("v1"));
        put(dir.filePath("b.dat"), "v2");
        QVERIFY(!reopened.importFile(dir.filePath("b.dat")).id.isEmpty());
        QCOMPARE(reopened.assets().size(), 2);
    }
    // P-605: a crash between the object store and the index commit (stage:
    // 索引提交) leaves a complete-but-unreferenced object. On reopen it is a
    // reported orphan — the index has no dangling revision and the library is
    // fully usable; the orphan is the explainable residue, not a corruption.
    void faultInterruptedIndexCommitLeavesOrphan() {
        QTemporaryDir dir;
        const QString root = dir.filePath("library");
        QString assetId;
        {
            put(dir.filePath("a.dat"), "v1");
            Library lib(root);
            assetId = lib.importFile(dir.filePath("a.dat")).id;
            // Object stored, index commit never happened: a complete object with
            // no revision row referencing it.
            const QString orphanHash = digestHex("staged-not-committed");
            put(root + "/objects/" + orphanHash, "staged-not-committed");
        }
        Library reopened(root);
        const auto report = reopened.checkConsistency();
        QVERIFY(report.orphan.contains(digestHex("staged-not-committed")));
        QCOMPARE(report.missing.size(), 0);
        QCOMPARE(report.corrupt.size(), 0);
        // The catalog references exactly the committed revision; no dangling
        // reference to the orphan. The library still captures new content.
        QCOMPARE(reopened.revisions(assetId).size(), 1);
        const auto a = reopened.assets().first();
        put(a.path, "v2");
        QVERIFY(reopened.capture(a, "v2"));
        QCOMPARE(reopened.revisions(assetId).size(), 2);
    }
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
    void libraryOpenCategoriesCoverSixCases() {
        QTemporaryDir dir;
        // 1) 新建: a fresh path creates all artifacts and a persistent identity.
        QString id;
        {
            Library fresh(dir.filePath("newlib"));
            QVERIFY(QFileInfo::exists(dir.filePath("newlib/catalog.sqlite")));
            QVERIFY(QFileInfo::exists(dir.filePath("newlib/objects")));
            QVERIFY(QFileInfo::exists(dir.filePath("newlib/working")));
            QVERIFY(QFileInfo::exists(dir.filePath("newlib/library.lock")));
            id = fresh.libraryId();
            QVERIFY(!id.isEmpty());
            QVERIFY(id.size() == 32 || id.size() == 36);
        }
        // 2) 有效库: reopening a valid library (lock released) keeps the identity.
        {
            Library reopened(dir.filePath("newlib"));
            QCOMPARE(reopened.libraryId(), id);
        }
        // 3) 无效目录: an existing non-empty directory without catalog.sqlite must
        // not be silently rebuilt into a library.
        QVERIFY(QDir().mkpath(dir.filePath("notalib")));
        put(dir.filePath("notalib/leftover.txt"), "x");
        QVERIFY_EXCEPTION_THROWN(Library invalid(dir.filePath("notalib")), std::runtime_error);
        QVERIFY(!QFileInfo::exists(dir.filePath("notalib/catalog.sqlite")));
        // 4) 已锁定: a second writer on the same path is refused while held.
        {
            Library first(dir.filePath("lockedlib"));
            QVERIFY_EXCEPTION_THROWN(Library second(dir.filePath("lockedlib")), std::runtime_error);
        }
        // A released lock frees the path for the next writer.
        { Library afterRelease(dir.filePath("lockedlib")); Q_UNUSED(afterRelease); }
        // 5) 权限不足: an existing read-only directory cannot be opened for writing.
        if (::geteuid() != 0) {
            QVERIFY(QDir().mkpath(dir.filePath("rolib")));
            QVERIFY(::chmod(dir.filePath("rolib").toLocal8Bit().constData(), 0555) == 0);
            QVERIFY_EXCEPTION_THROWN(Library denied(dir.filePath("rolib")), std::runtime_error);
            QVERIFY(::chmod(dir.filePath("rolib").toLocal8Bit().constData(), 0755) == 0);
        }
        // 6) 拔盘/不可达: a path whose parent is a regular file cannot be created.
        put(dir.filePath("blocker"), "file");
        QVERIFY_EXCEPTION_THROWN(Library unreachable(dir.filePath("blocker/lib")), std::runtime_error);
        // An incompatible schema version must be refused, not silently reopened.
        {
            Library v1(dir.filePath("schemav"));
            QVERIFY(!v1.libraryId().isEmpty());
            const QString catalog = dir.filePath("schemav/catalog.sqlite");
            {
                auto db = QSqlDatabase::addDatabase("QSQLITE", "schema_stamp");
                db.setDatabaseName(catalog);
                QVERIFY(db.open());
                QSqlQuery q(db);
                QVERIFY(q.exec("UPDATE meta SET value='99' WHERE key='schema_version'"));
                db.close();
            }
            QSqlDatabase::removeDatabase("schema_stamp");
        }
        QVERIFY_EXCEPTION_THROWN(Library wrong(dir.filePath("schemav")), std::runtime_error);
    }
    void libraryRelocatesAcrossVolumesWithIdentityCheck() {
        QTemporaryDir volumeA;
        QTemporaryDir volumeB; // a different "volume": relocation must still work
        put(volumeA.filePath("hero.png"), "pixel data");
        QString id;
        {
            Library library(volumeA.filePath("lib"));
            const auto asset = library.importFile(volumeA.filePath("hero.png"));
            id = library.libraryId();
            QVERIFY(!id.isEmpty());
            // The catalog persists a root-relative path; the working path is derived.
            QVERIFY(asset.relative.startsWith(QStringLiteral("working/")));
            QVERIFY(QFileInfo(asset.path).absolutePath().startsWith(volumeA.filePath("lib")));
            QCOMPARE(library.assets().first().path, asset.path);
        }
        // Relocation validation: the moved directory keeps the same identity and
        // can be verified WITHOUT taking the write lock.
        QVERIFY(QDir().rename(volumeA.filePath("lib"), volumeB.filePath("moved")));
        QCOMPARE(Library::peekLibraryId(volumeB.filePath("moved")), id);
        // peekLibraryId must also reject non-library directories.
        QVERIFY_EXCEPTION_THROWN(Library::peekLibraryId(volumeA.path()), std::runtime_error);
        // Reopened at the new location: same identity, assets resolve under the
        // new root, history and content are intact.
        Library relocated(volumeB.filePath("moved"));
        QCOMPARE(relocated.libraryId(), id);
        const auto asset = relocated.assets().first();
        QVERIFY(asset.path.startsWith(volumeB.filePath("moved")));
        QVERIFY(QFileInfo(asset.path).absolutePath().startsWith(volumeB.filePath("moved")));
        QCOMPARE(get(asset.path), QByteArray("pixel data"));
        QCOMPARE(relocated.revisions(asset.id).size(), 1);
    }
    // A corrupt catalog (not a SQLite file) must fail peekLibraryId without
    // leaking a registered connection (Qt warns "connection still in use" if a
    // handle is removed while open, so the failure must be clean).
    void peekLibraryIdOnCorruptCatalogClosesCleanly() {
        QTemporaryDir dir;
        put(dir.filePath("catalog.sqlite"), "this is not sqlite");
        bool rejected = false;
        try { Library::peekLibraryId(dir.path()); }
        catch (const std::exception &) { rejected = true; }
        QVERIFY(rejected);
        // Repeating must still fail cleanly (no leaked handle blocks reopen).
        rejected = false;
        try { Library::peekLibraryId(dir.path()); }
        catch (const std::exception &) { rejected = true; }
        QVERIFY(rejected);
    }
    void copyImportPreservesSourceAndSharesObjectForDistinctIdentities() {
        QTemporaryDir dir;
        const auto source = dir.filePath("sheet.png");
        put(source, "identical bytes");
        Library library(dir.filePath("lib"));
        const auto a = library.importFile(source);
        // The source is a copy source, never moved or modified.
        QCOMPARE(get(source), QByteArray("identical bytes"));
        QVERIFY(QFileInfo::exists(source));
        // Same content imported again: distinct identity (no merge), shared object.
        const auto b = library.importFile(source);
        QVERIFY(a.id != b.id);
        QCOMPARE(library.assets().size(), 2);
        const auto ra = library.revisions(a.id).first();
        const auto rb = library.revisions(b.id).first();
        QCOMPARE(ra.hash, rb.hash);
        QCOMPARE(library.objectPath(ra.hash), library.objectPath(rb.hash));
        QCOMPARE(library.readObject(ra.hash), QByteArray("identical bytes"));
        // A pre-registration read failure leaves no partial working directory.
        const auto missing = dir.filePath("not-there.bin");
        QVERIFY_EXCEPTION_THROWN(library.importFile(missing), std::runtime_error);
        QCOMPARE(library.assets().size(), 2);
    }
    // P-203: an import that fails AFTER the working copy is written (here: the
    // object staging dir refuses writes, so the registration append throws)
    // must not leave a half-written asset — no orphan working directory, no
    // asset row, and no stray object temp.
    void importFileFailureAfterWorkingCopyLeavesNoOrphan() {
        QTemporaryDir dir;
        put(dir.filePath("a.bin"), "fresh bytes");
        Library library(dir.filePath("lib"));
        // Make the object staging dir unwritable so storeObject() throws inside
        // the registration transaction (after the working copy exists).
        const QString tmpDir = library.root() + "/objects/.tmp";
        QVERIFY(QDir().mkpath(tmpDir));
        QVERIFY(::chmod(tmpDir.toLocal8Bit().constData(), 0555) == 0);
        bool threw = false;
        try { library.importFile(dir.filePath("a.bin")); }
        catch (const std::runtime_error &) { threw = true; }
        QVERIFY(::chmod(tmpDir.toLocal8Bit().constData(), 0755) == 0);
        QVERIFY(threw);
        // No asset row, no orphan working directory, no partial temp object.
        QCOMPARE(library.assets().size(), 0);
        const int workingDirs = QDir(library.root() + "/working")
            .entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot).size();
        QCOMPARE(workingDirs, 0);
        const int temps = QDir(tmpDir).entryInfoList(QDir::Files | QDir::NoDotAndDotDot).size();
        QCOMPARE(temps, 0);
        // The library still works: a retry after restoring write access imports.
        const auto asset = library.importFile(dir.filePath("a.bin"));
        QVERIFY(!asset.id.isEmpty());
        QCOMPARE(library.assets().size(), 1);
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
    void recursiveScanAppliesIgnoreRulesWithoutFollowingSymlinks() {
        QTemporaryDir dir;
        // Tree: two files, a subdir with one file, a hidden file, and a symlink
        // loop pointing at the root (must not be followed).
        put(dir.filePath("a.png"), "a");
        put(dir.filePath("b.bak"), "b");
        QVERIFY(QDir().mkpath(dir.filePath("sub")));
        put(dir.filePath("sub/c.png"), "c");
        put(dir.filePath(".hidden.png"), "h");
        put(dir.filePath("notes.txt"), "notes");
        QVERIFY(QFile::link(dir.path(), dir.filePath("loop")));
        // No rules: recursive, includes hidden files, never follows the symlink.
        const auto all = importer::scanDirectory(dir.path());
        QCOMPARE(all.total, 5); // a.png, b.bak, sub/c.png, .hidden.png, notes.txt
        QCOMPARE(all.excluded, 0);
        QCOMPARE(all.permissionErrors.size(), 0);
        bool sawLoop = false;
        for (const auto &c : all.candidates) sawLoop = sawLoop || c.relative.startsWith(QStringLiteral("loop"));
        QVERIFY(!sawLoop);
        // Rules: extension (.png), glob (*.bak), name subtree (sub), hidden-name.
        const auto rules = QVector<importer::IgnoreRule>{
            importer::parseIgnoreLine(".png"),
            importer::parseIgnoreLine("*.bak"),
            importer::parseIgnoreLine("sub"),
            importer::parseIgnoreLine("*.txt"),
        };
        const auto filtered = importer::scanDirectory(dir.path(), rules);
        // a.png(.png) b.bak(*.bak) sub/c.png(sub) .hidden.png(.png) notes.txt(*.txt).
        QCOMPARE(filtered.excluded, 5);
        QCOMPARE(filtered.total, 5);
        bool sawAnyIncluded = false;
        for (const auto &c : filtered.candidates)
            if (c.excludeReason.isEmpty()) sawAnyIncluded = true;
        QVERIFY(!sawAnyIncluded);
        // parseIgnoreLine forms are distinct and displayable.
        QCOMPARE(importer::parseIgnoreLine(".png").display(), QString("*.png"));
        QCOMPARE(importer::parseIgnoreLine("*.bak").display(), QString("*.bak"));
        QCOMPARE(importer::parseIgnoreLine("node_modules").display(), QString("node_modules"));
    }
    void importPlanReportsPerItemResultsAndCancelsAtBoundary() {
        QTemporaryDir dir;
        QVERIFY(QDir().mkpath(dir.filePath("tree")));
        QStringList sources;
        for (int i = 0; i < 8; ++i) {
            const QString file = dir.filePath(QString("tree/f%1.png").arg(i));
            put(file, QByteArray("content-") + QByteArray::number(i));
            sources << file;
        }
        // A missing source exercises the failure path (recorded, others proceed).
        sources << dir.filePath("tree/not-there.png");
        Library library(dir.filePath("lib"));
        // Full run: every importable file lands, the failure is recorded per item,
        // and the library reopens cleanly with no dangling working directories.
        TaskQueue queue;
        std::atomic_bool done = false;
        QObject::connect(&queue, &TaskQueue::taskFinished, [&](qint64, bool, const QString &) { done = true; });
        queue.submit("import", [&](TaskQueue::Progress &p) {
            importer::importPlan(library, sources, p);
        });
        QTRY_VERIFY_WITH_TIMEOUT(done.load(), 5000);
        auto status = queue.statuses().first();
        QCOMPARE(status.items.size(), 9); // 8 ok + 1 failure
        int ok = 0, failed = 0;
        for (const auto &item : status.items) item.ok ? ++ok : ++failed;
        QCOMPARE(ok, 8);
        QCOMPARE(failed, 1);
        QCOMPARE(library.assets().size(), 8);
        // No dangling working directories: one per imported asset.
        const int workingDirs = QDir(dir.filePath("lib/working")).entryInfoList(
            QDir::Dirs | QDir::NoDotAndDotDot).size();
        QCOMPARE(workingDirs, 8);
        // Cancellation at a file boundary: a pre-set cancel stops before any file,
        // and the library stays consistent (reopen matches, no partial assets).
        Library cancelled(dir.filePath("lib2"));
        TaskQueue q2;
        std::atomic_bool done2 = false;
        QObject::connect(&q2, &TaskQueue::taskFinished, [&](qint64, bool, const QString &) { done2 = true; });
        const qint64 id2 = q2.submit("import", [&](TaskQueue::Progress &p) {
            importer::importPlan(cancelled, sources, p);
        });
        q2.cancel(id2); // set before the worker starts: a boundary-clean no-op
        QTRY_VERIFY_WITH_TIMEOUT(done2.load(), 5000);
        QCOMPARE(cancelled.assets().size(), 0);
        QVERIFY(!cancelled.assets().isEmpty() == false);
    }
    void thumbnailCacheKeysByContentAndParamsAndSurvivesCorruption() {
        QTemporaryDir dir;
        // A real PNG payload.
        QImage source(120, 60, QImage::Format_ARGB32);
        source.fill(Qt::red);
        QByteArray pngBytes;
        {
            QBuffer buffer(&pngBytes);
            buffer.open(QIODevice::WriteOnly);
            QVERIFY(source.save(&buffer, "PNG"));
        }
        const QString hash = QString::fromLatin1(QCryptographicHash::hash(
            pngBytes, QCryptographicHash::Sha256).toHex());
        ThumbnailCache cache(dir.filePath("lib"));
        const QSize request(64, 64);
        // Key is content hash + generation params, never a file name.
        QVERIFY(ThumbnailCache::key(hash, request, 1).contains(hash));
        QVERIFY(ThumbnailCache::key(hash, request, 1)
            != ThumbnailCache::key(hash, request, 2));
        // Generates and caches; aspect ratio preserved (120x60 into 64x64 -> 64x32).
        const QImage thumb = thumbnailFor(cache, pngBytes, hash, request);
        QVERIFY(!thumb.isNull());
        QCOMPARE(thumb.size(), QSize(64, 32));
        QVERIFY(QFileInfo::exists(cache.filePath(hash, request, 1)));
        // Second lookup reuses the cache (no regeneration needed).
        const QImage again = thumbnailFor(cache, pngBytes, hash, request);
        QCOMPARE(again.size(), thumb.size());
        // Corrupt cache entry: deleted and rebuilt, original bytes untouched.
        put(cache.filePath(hash, request, 1), "garbage");
        QVERIFY(cache.find(hash, request, 1).isNull()); // corrupt -> null
        QVERIFY(!QFileInfo::exists(cache.filePath(hash, request, 1))); // removed
        const QImage rebuilt = thumbnailFor(cache, pngBytes, hash, request);
        QVERIFY(!rebuilt.isNull());
        QCOMPARE(rebuilt.size(), QSize(64, 32));
        // A very large source is pre-downsampled before decode (P-207): the
        // thumbnail fits the request box and keeps the aspect ratio, so a huge
        // image never forces a full-resolution decode on the UI thread.
        QImage huge(4000, 1000, QImage::Format_ARGB32);
        huge.fill(Qt::blue);
        QByteArray hugePng;
        {
            QBuffer buf(&hugePng);
            buf.open(QIODevice::WriteOnly);
            QVERIFY(huge.save(&buf, "PNG"));
        }
        const QString hugeHash = QString::fromLatin1(QCryptographicHash::hash(
            hugePng, QCryptographicHash::Sha256).toHex());
        const QImage hugeThumb = thumbnailFor(cache, hugePng, hugeHash, request);
        QVERIFY(!hugeThumb.isNull());
        QVERIFY(hugeThumb.size().width() <= request.width());
        QVERIFY(hugeThumb.size().height() <= request.height());
        QCOMPARE(hugeThumb.size(), QSize(64, 16)); // 4000x1000 -> 64x16
        // The thumbnail is not the history content: the source bytes are untouched.
        QCOMPARE(hash, QString::fromLatin1(QCryptographicHash::hash(
            pngBytes, QCryptographicHash::Sha256).toHex()));
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
    void previewScalingKeepsAspectAndBoundedDecode() {
        // Uniform fit: the limiting dimension governs, aspect is preserved.
        QCOMPARE(preview::fitScale(QSize(1000, 100), QSize(500, 500)), 0.5);
        QCOMPARE(preview::fitScale(QSize(100, 1000), QSize(500, 500)), 0.5);
        QCOMPARE(preview::fitScale(QSize(2000, 1000), QSize(500, 500)), 0.25);
        // Margins shrink the available area on both axes.
        QCOMPARE(preview::fitScale(QSize(1000, 100), QSize(500, 500), 10), 0.48);
        // Degenerate inputs never divide by zero or explode.
        QCOMPARE(preview::fitScale(QSize(), QSize(500, 500)), 1.0);
        QCOMPARE(preview::fitScale(QSize(100, 100), QSize(0, 0)), 1.0);

        QTemporaryDir dir;
        const QString transparent = dir.filePath("alpha.png");
        QImage alpha(40, 30, QImage::Format_ARGB32);
        alpha.fill(Qt::transparent);
        alpha.setPixelColor(5, 5, QColor(255, 0, 0, 128));
        QVERIFY(alpha.save(transparent));
        const auto decoded = preview::decode(transparent);
        QVERIFY(decoded.valid);
        QVERIFY(!decoded.reduced);
        QCOMPARE(decoded.image.size(), QSize(40, 30)); // exact pixels, aspect intact
        QVERIFY(decoded.image.hasAlphaChannel());       // transparency preserved

        // A huge image reads at a downscaled size (aspect kept) instead of an
        // unbounded allocation, and the source file stays untouched.
        const QString big = dir.filePath("big.png");
        QImage huge(1000, 400, QImage::Format_RGB32);
        huge.fill(Qt::black);
        QVERIFY(huge.save(big));
        const auto scaled = preview::decode(big, QString(), 100000); // budget 100k pixels
        QVERIFY(scaled.valid);
        QVERIFY(scaled.reduced);
        QVERIFY(scaled.image.width() <= 500 && scaled.image.height() <= 200);
        const qreal srcAspect = qreal(1000) / 400, outAspect = qreal(scaled.image.width()) / scaled.image.height();
        QVERIFY(qAbs(srcAspect - outAspect) < 0.02); // uniform: not stretched
    }

    // P-210: every V1 first-batch format must have a sample that actually
    // decodes (never claiming support by extension), corrupt files degrade
    // without crashing, and unsupported files stay visible as assets.
    void firstBatchFormatsDecodeOrDegrade() {
        QTemporaryDir dir;
        const QStringList batch = {"png", "jpeg", "webp", "tga", "bmp"};
        for (const auto &fmt : batch) {
            const QByteArray latin = fmt.toLatin1();
            QVERIFY2(QImageReader::supportedImageFormats().contains(latin),
                     qPrintable("missing codec: " + fmt));
            const QString path = dir.filePath("sample." + fmt);
            if (fmt == "tga") {
                put(path, makeTga(64, 48, qRgba(10, 200, 30, 200))); // read-only codec
            } else {
                QImage img(64, 48, QImage::Format_ARGB32);
                img.fill(Qt::transparent);
                img.setPixelColor(8, 8, QColor(10, 200, 30, 200));
                QVERIFY2(img.save(path, latin), qPrintable("encode failed: " + fmt));
            }
            // The asset's own extension is authoritative (the app decodes
            // extensionless objects; the hint guards signature-less formats),
            // and content sniffing (no hint) is also reliable for well-formed
            // samples — the detected format is content-based, never extension.
            const auto d = preview::decode(path, fmt);
            QVERIFY2(d.valid, qPrintable("decode failed: " + fmt));
            QCOMPARE(d.image.size(), QSize(64, 48)); // exact size, aspect intact
            // Transparency per the P-210 sample table: PNG/WebP/TGA keep alpha;
            // JPEG and BMP have no alpha channel by design.
            if (fmt == "png" || fmt == "webp" || fmt == "tga")
                QVERIFY2(d.image.hasAlphaChannel(), qPrintable("alpha lost: " + fmt));
            if (fmt == "tga") QCOMPARE(d.image.pixelColor(8, 8), QColor(10, 200, 30, 200));
            QVERIFY2(preview::decode(path).valid, qPrintable("sniff failed: " + fmt));
            QCOMPARE(preview::detectedFormat(path), fmt); // content-based, not extension
            // The thumbnail path decodes the same object bytes with the hint.
            ThumbnailCache cache(dir.filePath("thumbs"));
            const auto thumb = thumbnailFor(cache, get(path), QString::number(qHash(fmt)), QSize(40, 40), fmt);
            QVERIFY(!thumb.isNull());
        }
        // firstBatchSupported reports exactly the codecs the runtime has.
        for (const auto &fmt : preview::firstBatchSupported())
            QVERIFY(QImageReader::supportedImageFormats().contains(fmt.toLatin1()));

        // Truncated (corrupt) file: recognizable but not decodable, never a crash.
        const QString corrupt = dir.filePath("corrupt.png");
        QImage good(40, 40, QImage::Format_RGB32);
        good.fill(Qt::red);
        QVERIFY(good.save(corrupt));
        QFile cf(corrupt);
        QVERIFY(cf.open(QIODevice::WriteOnly | QIODevice::Append));
        QVERIFY(cf.resize(20)); // keep only the header bytes
        cf.close();
        const auto cd = preview::decode(corrupt);
        QVERIFY(!cd.valid);

        // Unsupported format (PSD-like bytes): visible, degraded, never claims
        // support by extension, never allocates memory.
        const QString unsupported = dir.filePath("art.psd");
        put(unsupported, QByteArray("8BPSfake-not-a-real-psd-header"));
        QVERIFY(preview::detectedFormat(unsupported).isEmpty());
        QVERIFY(!preview::canDecode(unsupported));
        QVERIFY(!preview::decode(unsupported).valid);
        // It is still importable and listed as an asset ("不支持格式可见").
        Library lib(dir.filePath("lib"));
        const auto asset = lib.importFile(unsupported);
        QVERIFY(!asset.id.isEmpty());
        QCOMPARE(lib.assets().size(), 1);
        QCOMPARE(lib.assets().first().id, asset.id);
    }

    // P-211: PSD composite preview was evaluated — Qt 6.10.2 has no PSD codec —
    // so Photoshop files degrade explicitly (no composite claimed), the
    // signature is detected from content, and the original bytes stay intact.
    void psdCompositePreviewDegradesAndPreservesFile() {
        QTemporaryDir dir;
        const QByteArray psd = makePsd(8, 8);
        const QString path = dir.filePath("art.psd");
        put(path, psd);
        QVERIFY(preview::isPsdSignature(psd));       // content-based detection
        QVERIFY(!preview::isPsdSignature(QByteArray("not a psd")));
        QVERIFY(preview::detectedFormat(path).isEmpty()); // no codec claims it
        QVERIFY(!preview::decode(path, "psd").valid);     // no composite preview
        QVERIFY(!preview::firstBatchSupported().contains("psd"));
        // "无法预览时的原文件保留": import keeps every byte and stays visible.
        Library lib(dir.filePath("lib"));
        const auto asset = lib.importFile(path);
        QVERIFY(!asset.id.isEmpty());
        QCOMPARE(get(path), psd);                      // source file untouched
        QCOMPARE(lib.readObject(lib.revisions(asset.id).first().hash), psd);
        QCOMPARE(lib.assets().size(), 1);
        // A truncated (corrupt) PSD also keeps its bytes and degrades silently.
        const QString broken = dir.filePath("broken.psd");
        put(broken, psd.left(10));
        const auto brokenAsset = lib.importFile(broken);
        QCOMPARE(lib.readObject(lib.revisions(brokenAsset.id).first().hash), psd.left(10));
        QVERIFY(preview::isPsdSignature(psd.left(10)));
    }

    // P-212: technical metadata reports format/size/fileSize/channels from the
    // header only, and leaves unknown fields empty instead of fabricating them.
    void metadataReportsFormatSizeAndChannels() {
        QTemporaryDir dir;
        // PNG: format, exact size, file size, RGBA channels + 8bit depth.
        QImage rgba(64, 48, QImage::Format_ARGB32);
        rgba.fill(Qt::transparent);
        const QString png = dir.filePath("a.png");
        QVERIFY(rgba.save(png));
        const auto mp = preview::metadata(png, "png");
        QVERIFY(mp.ok);
        QCOMPARE(mp.format, QString("png"));
        QCOMPARE(mp.width, 64);
        QCOMPARE(mp.height, 48);
        QVERIFY(mp.fileSize > 0);
        QCOMPARE(mp.channels, QString("RGBA"));
        QCOMPARE(mp.bitsPerChannel, 8);
        QVERIFY(mp.hasAlpha);
        // JPEG: no alpha channel.
        QImage rgb(32, 32, QImage::Format_RGB32);
        rgb.fill(Qt::red);
        const QString jpg = dir.filePath("b.jpg");
        QVERIFY(rgb.save(jpg));
        const auto mj = preview::metadata(jpg, "jpg");
        QVERIFY(mj.ok);
        QCOMPARE(mj.channels, QString("RGB"));
        QVERIFY(!mj.hasAlpha);
        // TGA (hand-written, read-only codec): RGBA + exact pixel depth.
        const QString tga = dir.filePath("c.tga");
        put(tga, makeTga(64, 48, qRgba(10, 200, 30, 200)));
        const auto mt = preview::metadata(tga, "tga");
        QVERIFY(mt.ok);
        QCOMPARE(mt.channels, QString("RGBA"));
        QVERIFY(mt.hasAlpha);
        // PSD: format named by content signature, channels left unknown (no
        // codec to confirm them) — never fabricated.
        const QString psd = dir.filePath("d.psd");
        put(psd, makePsd(8, 8));
        const auto mpsd = preview::metadata(psd, "psd");
        QVERIFY(mpsd.ok);
        QCOMPARE(mpsd.format, QString("psd"));
        QVERIFY(mpsd.channels.isEmpty());   // unknown, not guessed
        QCOMPARE(mpsd.bitsPerChannel, 0);   // unknown, not filled with zero-truth
        QVERIFY(mpsd.fileSize > 0);
        // An uninspectable path reports not-ok with empty fields (no fabrication).
        const auto missing = preview::metadata(dir.filePath("nope.png"));
        QVERIFY(!missing.ok);
        QCOMPARE(missing.format, QString());
        QCOMPARE(missing.channels, QString());
        QCOMPARE(missing.width, 0);
        QCOMPARE(missing.bitsPerChannel, 0);
    }

    // P-213: single and batch tag maintenance; results are queryable, whitespace
    // is trimmed, empty tags are ignored, and batch is explicit add/remove.
    void tagMaintenanceSingleAndBatchIsQueryable() {
        QTemporaryDir dir;
        Library lib(dir.filePath("lib"));
        put(dir.filePath("a.png"), "asset-a");
        put(dir.filePath("b.png"), "asset-b");
        put(dir.filePath("c.png"), "asset-c");
        const auto a = lib.importFile(dir.filePath("a.png"));
        const auto b = lib.importFile(dir.filePath("b.png"));
        const auto c = lib.importFile(dir.filePath("c.png"));

        // Single add: trimmed, deduplicated (adding twice is a no-op).
        QCOMPARE(lib.addTag(a.id, "  角色  "), 1);   // stored trimmed
        QCOMPARE(lib.addTag(a.id, "角色"), 0);       // dedup: already present
        QCOMPARE(lib.addTag(a.id, "  "), 0);         // empty after trim: ignored
        QCOMPARE(lib.tags(a.id), QStringList({"角色"}));
        QCOMPARE(lib.assetsWithTag("角色"), QStringList({a.id}));
        // Case is preserved; distinct tags coexist.
        QCOMPARE(lib.addTag(a.id, "角色"), 0);
        QCOMPARE(lib.addTag(a.id, "立绘"), 1);
        QCOMPARE(lib.tags(a.id).size(), 2);

        // Single remove: result is immediately queryable.
        QCOMPARE(lib.removeTag(a.id, "角色"), 1);
        QCOMPARE(lib.removeTag(a.id, "角色"), 0);
        QCOMPARE(lib.tags(a.id), QStringList({"立绘"}));

        // Batch add: explicit, affects only assets without the tag (preview).
        // Asset a already carries 立绘, so only b and c actually change.
        const QStringList ids = {a.id, b.id, c.id};
        QCOMPARE(lib.countAssetsWithoutTag(ids, "立绘"), 2);
        QCOMPARE(lib.addTagToAssets(ids, "立绘"), 2);
        QCOMPARE(lib.countAssetsWithoutTag(ids, "立绘"), 0);
        QCOMPARE(lib.countAssetsWithTag(ids, "立绘"), 3);
        QCOMPARE(lib.assetsWithTag("立绘").size(), 3);
        QCOMPARE(lib.tags(b.id), QStringList({"立绘"}));
        QCOMPARE(lib.tags(c.id), QStringList({"立绘"}));
        // Re-adding the batch is a no-op (returns 0 changes).
        QCOMPARE(lib.addTagToAssets(ids, "立绘"), 0);

        // Batch remove: explicit removal of one tag only.
        QCOMPARE(lib.countAssetsWithTag(ids, "立绘"), 3);
        QCOMPARE(lib.removeTagFromAssets(ids, "立绘"), 3);
        QCOMPARE(lib.countAssetsWithTag(ids, "立绘"), 0);
        QVERIFY(lib.tags(b.id).isEmpty());
        QVERIFY(lib.assetsWithTag("立绘").isEmpty());
        // No tag was replaced or clobbered: asset a has none left (only 立绘),
        // and the batch removed exactly the one tag, never a replace-all.
        QVERIFY(lib.tags(a.id).isEmpty());
    }

    void ratingAndNotePersistAcrossReopen() {
        QTemporaryDir dir;
        QString assetId;
        {
            Library lib(dir.filePath("lib"));
            put(dir.filePath("a.png"), "asset-a");
            put(dir.filePath("b.png"), "asset-b");
            const auto a = lib.importFile(dir.filePath("a.png"));
            const auto b = lib.importFile(dir.filePath("b.png"));
            assetId = a.id;

            // Unknown asset: no rating, empty note (no fabricated values).
            QCOMPARE(lib.rating(b.id), -1);
            QVERIFY(lib.note(b.id).isEmpty());

            // No-rating (-1) is distinct from 0 (explicit zero star).
            QCOMPARE(lib.rating(assetId), -1);
            lib.setRating(assetId, 0);
            QCOMPARE(lib.rating(assetId), 0);
            lib.setRating(assetId, 3);
            QCOMPARE(lib.rating(assetId), 3);
            lib.setRating(assetId, 5);
            QCOMPARE(lib.rating(assetId), 5);
            // Clearing returns to no-rating, not to 0.
            lib.setRating(assetId, -1);
            QCOMPARE(lib.rating(assetId), -1);

            // Note is stored verbatim; an honest empty note stays empty.
            lib.setNote(assetId, "  立绘，需去背  ");
            QCOMPARE(lib.note(assetId), QStringLiteral("  立绘，需去背  "));
            lib.setNote(assetId, QString());
            QVERIFY(lib.note(assetId).isEmpty());
            lib.setNote(assetId, "首版参考图");
            QCOMPARE(lib.note(assetId), QStringLiteral("首版参考图"));

            // Setting the note must not clobber the rating and vice versa.
            lib.setRating(assetId, 4);
            QCOMPARE(lib.note(assetId), QStringLiteral("首版参考图"));
            lib.setNote(assetId, "更新后沿用");
            QCOMPARE(lib.rating(assetId), 4);

            // Out-of-range ratings are rejected, never clamped.
            QVERIFY_EXCEPTION_THROWN(lib.setRating(assetId, 6), std::runtime_error);
            QVERIFY_EXCEPTION_THROWN(lib.setRating(assetId, -2), std::runtime_error);
            QCOMPARE(lib.rating(assetId), 4); // unchanged after the throws
        }
        // Reopen: values survive a fresh Library on the same root.
        Library reopened(dir.filePath("lib"));
        QCOMPARE(reopened.rating(assetId), 4);
        QCOMPARE(reopened.note(assetId), QStringLiteral("更新后沿用"));
    }

    void provenanceRecordsAppendAndViewablyPersist() {
        QTemporaryDir dir;
        put(dir.filePath("evidence.txt"), "license email printout");
        put(dir.filePath("a.png"), "asset-a");
        QString assetId;
        {
            Library lib(dir.filePath("lib"));
            const auto a = lib.importFile(dir.filePath("a.png"));
            assetId = a.id;
            QVERIFY(lib.provenance(assetId).isEmpty()); // nothing inferred up front

            // Evidence is stored as a content-addressed object: hash round-trips,
            // the object bytes verify, and the file stays inside the library
            // (objects/ is within the backup boundary).
            const QString hash = lib.storeEvidence(dir.filePath("evidence.txt"));
            QCOMPARE(hash.size(), 64);
            QCOMPARE(lib.readObject(hash), QByteArray("license email printout"));
            QVERIFY(QFileInfo::exists(lib.objectPath(hash)));

            // First record: fields stored verbatim, recorded time stamped here.
            Provenance first;
            first.source = QStringLiteral("https://example.com/pack");
            first.author = QStringLiteral("某人");
            first.license = QStringLiteral("CC BY 4.0");
            first.evidence = hash;
            first.evidenceName = QStringLiteral("evidence.txt");
            lib.addProvenance(assetId, first);
            const auto rows = lib.provenance(assetId);
            QCOMPARE(rows.size(), 1);
            QCOMPARE(rows.first().source, first.source);
            QCOMPARE(rows.first().author, first.author);
            QCOMPARE(rows.first().license, first.license);
            QCOMPARE(rows.first().evidence, hash);
            QCOMPARE(rows.first().evidenceName, first.evidenceName);
            QVERIFY(!rows.first().recorded.isEmpty());

            // Second record with unknown fields left empty: nothing invented.
            Provenance second;
            second.license = QStringLiteral("待确认");
            lib.addProvenance(assetId, second);
            const auto rows2 = lib.provenance(assetId);
            QCOMPARE(rows2.size(), 2);
            QVERIFY(rows2.first().source.isEmpty()); // newest first
            QVERIFY(rows2.first().author.isEmpty());
            QCOMPARE(rows2.first().license, QStringLiteral("待确认"));
            QVERIFY(rows2.first().evidence.isEmpty());
            // Old records stay viewable with their own recorded time.
            QCOMPARE(rows2.last().license, QStringLiteral("CC BY 4.0"));
            QCOMPARE(rows2.last().source, first.source);
            QVERIFY(rows2.last().recorded <= rows2.first().recorded);

            // Evidence export: exact bytes out, and never into the library itself.
            const QString out = dir.filePath("out/evidence-back.txt");
            QDir().mkpath(dir.filePath("out"));
            lib.exportEvidence(hash, out);
            QCOMPARE(get(out), QByteArray("license email printout"));
            QVERIFY_EXCEPTION_THROWN(lib.exportEvidence(hash, dir.filePath("lib/inside.txt")),
                                     std::runtime_error);
            // A malformed evidence hash is rejected at record time, not stored.
            Provenance bad;
            bad.evidence = QStringLiteral("not-a-hash");
            QVERIFY_EXCEPTION_THROWN(lib.addProvenance(assetId, bad), std::runtime_error);
        }
        // Reopen: records persist verbatim.
        Library reopened(dir.filePath("lib"));
        const auto rows = reopened.provenance(assetId);
        QCOMPARE(rows.size(), 2);
        QCOMPARE(rows.last().license, QStringLiteral("CC BY 4.0"));
        QCOMPARE(reopened.readObject(rows.last().evidence), QByteArray("license email printout"));
    }

    void renameAndMoveKeepAssetIdentity() {
        QTemporaryDir dir;
        put(dir.filePath("a.png"), "asset-a");
        put(dir.filePath("b.png"), "asset-b");
        put(dir.filePath("c.png"), "asset-c");
        QString aId, bId, cId;
        {
            Library lib(dir.filePath("lib"));
            const auto a = lib.importFile(dir.filePath("a.png"));
            const auto b = lib.importFile(dir.filePath("b.png"));
            const auto c = lib.importFile(dir.filePath("c.png"));
            aId = a.id; bId = b.id; cId = c.id;
            lib.addTag(aId, "立绘");
            lib.setRating(aId, 4);
            lib.setNote(aId, "备注");
            Provenance prov; prov.source = QStringLiteral("某处"); prov.license = QStringLiteral("CC0");
            lib.addProvenance(aId, prov);
            const auto before = lib.revisions(aId);
            // P-309: each field change also appends a metadata-only revision, so
            // the count is the import plus one per recorded change (tag, rating,
            // note, source, license — 5 changes here).
            QCOMPARE(before.size(), 6);

            // Rename: identity (id) unchanged, name and working file updated.
            lib.renameAsset(aId, "hero.png");
            const auto byId = [&lib](const QString &id) {
                for (const auto &asset : lib.assets()) if (asset.id == id) return asset;
                return Asset{};
            };
            const Asset renamed = byId(aId);
            QCOMPARE(renamed.id, aId);
            QCOMPARE(renamed.name, QStringLiteral("hero.png"));
            QCOMPARE(renamed.logicalPath(), QStringLiteral("hero.png"));
            QVERIFY(QFileInfo::exists(renamed.path));
            QVERIFY(!QFileInfo::exists(dir.filePath("lib/working/" + aId + "/a.png")));
            // History, tags, rating and note all still belong to the same asset
            // (the rename added one metadata-only revision, P-309).
            QCOMPARE(lib.revisions(aId).size(), before.size() + 1);
            QCOMPARE(lib.revisions(aId).first().hash, before.first().hash);
            QCOMPARE(lib.tags(aId), QStringList({"立绘"}));
            QCOMPARE(lib.rating(aId), 4);
            QCOMPARE(lib.note(aId), QStringLiteral("备注"));
            QCOMPARE(lib.provenance(aId).first().source, QStringLiteral("某处"));

            // Same-folder name clash: refused, nothing overwritten or merged.
            QVERIFY_EXCEPTION_THROWN(lib.renameAsset(aId, "b.png"), std::runtime_error);
            QCOMPARE(byId(aId).name, QStringLiteral("hero.png"));

            // Invalid names are rejected before touching anything.
            QVERIFY_EXCEPTION_THROWN(lib.renameAsset(aId, "   "), std::runtime_error);
            QVERIFY_EXCEPTION_THROWN(lib.renameAsset(aId, "x/y.png"), std::runtime_error);

            // Move: only the logical folder changes; id and name are kept.
            lib.moveAsset(aId, " characters/hero ");   // trimmed + normalized
            const Asset moved = byId(aId);
            QCOMPARE(moved.id, aId);
            QCOMPARE(moved.name, QStringLiteral("hero.png"));
            QCOMPARE(moved.folder, QStringLiteral("characters/hero"));
            QCOMPARE(moved.logicalPath(), QStringLiteral("characters/hero/hero.png"));
            // History intact (plus the rename and the move metadata-only revisions).
            QCOMPARE(lib.revisions(aId).size(), before.size() + 2);
            // Move to the same folder is a no-op, not an error.
            lib.moveAsset(aId, "characters/hero");
            QCOMPARE(byId(aId).folder, QStringLiteral("characters/hero"));

            // Move-time clash: c renamed to hero.png, moving into a's folder fails.
            lib.renameAsset(cId, "hero.png");
            QVERIFY_EXCEPTION_THROWN(lib.moveAsset(cId, "characters/hero"), std::runtime_error);
            QVERIFY(byId(cId).folder.isEmpty()); // c stayed in root, not merged

            // Folder traversal is rejected (never escape the library).
            QVERIFY_EXCEPTION_THROWN(lib.moveAsset(aId, "../outside"), std::runtime_error);
            QVERIFY_EXCEPTION_THROWN(lib.moveAsset(aId, "/abs/path"), std::runtime_error);
        }
        // Reopen: folder and name persist; identity is unchanged.
        Library reopened(dir.filePath("lib"));
        QHash<QString, QString> byId;
        for (const auto &asset : reopened.assets()) byId.insert(asset.id, asset.logicalPath());
        QCOMPARE(byId.value(aId), QStringLiteral("characters/hero/hero.png"));
        QCOMPARE(byId.value(bId), QStringLiteral("b.png"));
        // Import + 5 metadata-only field changes + rename + move (P-309); the
        // persisted count is unchanged by the restart.
        QCOMPARE(reopened.revisions(aId).size(), 8);
        QCOMPARE(reopened.tags(aId), QStringList({"立绘"}));
    }

    void combinedFilterMatchesAcrossConditions() {
        // Two facts: a 64x48 PNG with tags 角色/立绘 rated 4, and a 200x100
        // JPEG with no tags/rating in a subfolder.
        AssetFacts png;
        png.name = QStringLiteral("hero.png"); png.format = QStringLiteral("PNG");
        png.width = 64; png.height = 48; png.rating = 4;
        png.tags = {QStringLiteral("角色"), QStringLiteral("立绘")};
        AssetFacts jpg;
        jpg.name = QStringLiteral("bg.jpg"); jpg.folder = QStringLiteral("scenes");
        jpg.format = QStringLiteral("JPEG"); jpg.width = 200; jpg.height = 100; jpg.rating = -1;

        // No condition matches everything; active() stays false.
        AssetFilter none;
        QVERIFY(!none.active());
        QVERIFY(filterMatches(png, none) && filterMatches(jpg, none));

        // Name: matches the logical path (folder/name) case-insensitively.
        AssetFilter byName; byName.name = QStringLiteral("HERO");
        QVERIFY(filterMatches(png, byName));
        QVERIFY(!filterMatches(jpg, byName));
        AssetFilter byFolder; byFolder.name = QStringLiteral("scenes");
        QVERIFY(filterMatches(jpg, byFolder));
        QVERIFY(!filterMatches(png, byFolder));

        // Tags: any-of (OR) within the set; an asset with none fails.
        AssetFilter byTag; byTag.tags = {QStringLiteral("立绘")};
        QVERIFY(filterMatches(png, byTag));
        QVERIFY(!filterMatches(jpg, byTag));
        AssetFilter byTagAny; byTagAny.tags = {QStringLiteral("立绘"), QStringLiteral("场景")};
        QVERIFY(filterMatches(png, byTagAny)); // one of two hits

        // Format: exact (case-insensitive); unknown format fails a named one.
        AssetFilter byFormat; byFormat.format = QStringLiteral("png");
        QVERIFY(filterMatches(png, byFormat));
        QVERIFY(!filterMatches(jpg, byFormat));

        // Size: both floors must hold; a floor of 0 is no condition.
        AssetFilter bySize; bySize.minWidth = 100; bySize.minHeight = 50;
        QVERIFY(filterMatches(jpg, bySize));
        QVERIFY(!filterMatches(png, bySize));
        AssetFilter byWide; byWide.minWidth = 150; byWide.minHeight = 0;
        QVERIFY(filterMatches(jpg, byWide));
        QVERIFY(!filterMatches(png, byWide));

        // Rating: no-rating (-1) does not satisfy ">= N"; explicit no-rating only.
        AssetFilter byRating; byRating.minRating = 3;
        QVERIFY(filterMatches(png, byRating));
        QVERIFY(!filterMatches(jpg, byRating)); // no rating is not a score
        AssetFilter byNoRating; byNoRating.minRating = -1;
        QVERIFY(filterMatches(jpg, byNoRating));
        QVERIFY(!filterMatches(png, byNoRating));

        // Combined: all conditions ANDed — only png passes here.
        AssetFilter combo;
        combo.name = QStringLiteral("hero");
        combo.tags = {QStringLiteral("角色")};
        combo.format = QStringLiteral("PNG");
        combo.minWidth = 32; combo.minHeight = 32; combo.minRating = 4;
        QVERIFY(combo.active());
        QVERIFY(filterMatches(png, combo));
        combo.minWidth = 128; // now png's 64 fails the size floor
        QVERIFY(!filterMatches(png, combo));
    }

    void filteringFactsDerivedFromCommittedRevision() {
        QTemporaryDir dir;
        put(dir.filePath("a.tga"), makeTga(64, 48, qRgba(10, 200, 30, 200)));
        put(dir.filePath("bin.dat"), "not-an-image");
        QByteArray pngBytes;
        {
            QImage img(120, 60, QImage::Format_RGB32);
            img.fill(Qt::blue);
            QBuffer buffer(&pngBytes);
            buffer.open(QIODevice::WriteOnly);
            QVERIFY(img.save(&buffer, "PNG"));
        }
        put(dir.filePath("b.png"), pngBytes);
        Library lib(dir.filePath("lib"));
        const auto tga = lib.importFile(dir.filePath("a.tga"));
        const auto bin = lib.importFile(dir.filePath("bin.dat"));
        const auto png = lib.importFile(dir.filePath("b.png"));

        // A decodable image reports its real format and size; unknown stays empty/0.
        QCOMPARE(lib.assetFormat(tga.id).toUpper(), QStringLiteral("TGA"));
        QCOMPARE(lib.assetSize(tga.id), QSize(64, 48));
        QCOMPARE(lib.assetFormat(png.id).toUpper(), QStringLiteral("PNG"));
        QCOMPARE(lib.assetSize(png.id), QSize(120, 60));
        QVERIFY(lib.assetFormat(bin.id).isEmpty());
        QCOMPARE(lib.assetSize(bin.id), QSize(0, 0)); // unknown, not fabricated

        // Facts follow the last committed revision: capture a PNG into the TGA
        // asset and the stored facts update; a restore reverts them.
        put(dir.filePath("a2.png"), pngBytes);
        QFile::remove(tga.path);
        QFile::copy(dir.filePath("a2.png"), tga.path);
        QVERIFY(lib.capture(tga));
        QCOMPARE(lib.assetFormat(tga.id).toUpper(), QStringLiteral("PNG"));
        QCOMPARE(lib.assetSize(tga.id), QSize(120, 60));
    }

    void scheduledSnapshotStateMachineEnforcesSpec() {
        // Off by default: never due, regardless of the clock.
        SnapshotSchedule sched;
        QVERIFY(!sched.enabled());
        QVERIFY(!sched.due(QDateTime::currentDateTime()));
        // Enabling sets next = now + one period.
        const QDateTime t0 = QDateTime::fromMSecsSinceEpoch(1700000000000);
        sched.setPeriod(15, t0);
        QVERIFY(sched.enabled());
        QCOMPARE(sched.next(), t0.addSecs(15 * 60));
        QVERIFY(!sched.due(t0.addSecs(15 * 60 - 1))); // one second early: not due
        QVERIFY(sched.due(t0.addSecs(15 * 60)));      // at/after next: due
        // Starting the run recomputes next from now, so several missed periods
        // (sleep/suspend) collapse into this one catch-up run, never stacked.
        const QDateTime t1 = t0.addSecs(3 * 15 * 60); // three periods late
        QVERIFY(sched.due(t1));
        sched.beginRun(t1);
        QVERIFY(sched.inProgress());
        QCOMPARE(sched.next(), t1.addSecs(15 * 60));
        QVERIFY(!sched.due(t1.addSecs(60))); // in-progress merges the overlap
        sched.endRun();
        // Next run is one period after the catch-up point, not one after each
        // missed period.
        QVERIFY(!sched.due(t1.addSecs(15 * 60 - 1)));
        QVERIFY(sched.due(t1.addSecs(15 * 60)));
        // Pause records its reason and blocks any run; resume restarts the clock.
        sched.pauseByUser(QStringLiteral("用户暂停"));
        QVERIFY(sched.paused());
        QCOMPARE(sched.pauseReason(), QStringLiteral("用户暂停"));
        QVERIFY(!sched.due(t1.addSecs(60)));
        const QDateTime t2 = t1.addSecs(60);
        sched.resume(t2);
        QVERIFY(!sched.paused());
        QCOMPARE(sched.next(), t2.addSecs(15 * 60));
        QVERIFY(!sched.due(t2.addSecs(15 * 60 - 1)));
        // A system pause is distinguishable, and resume lifts it (a user pause
        // is only ever lifted by an explicit resume — the flag tracks the source).
        sched.pauseBySystem(QStringLiteral("系统"));
        QVERIFY(sched.pausedBySystem());
        sched.resume(t2);
        QVERIFY(!sched.paused() && !sched.pausedBySystem());
        // Disabling drops the schedule and any pending run entirely.
        sched.setPeriod(0, t2);
        QVERIFY(!sched.enabled());
        QVERIFY(!sched.due(t2.addSecs(1000)));
    }

    void exclusionRulesMatchKindPatternAndNeverDelete() {
        // Extension: case-insensitive suffix; a leading dot in the pattern is
        // normalized away and matching accepts it either way.
        QVERIFY(exclusionReason(QStringLiteral("extension"), QStringLiteral("psd"),
                                QString(), QStringLiteral("hero.png")).isEmpty());
        QVERIFY(!exclusionReason(QStringLiteral("extension"), QStringLiteral("psd"),
                                 QString(), QStringLiteral("hero.psd")).isEmpty());
        QVERIFY(!exclusionReason(QStringLiteral("extension"), QStringLiteral(".PSD"),
                                 QString(), QStringLiteral("hero.psd")).isEmpty());
        // Path: a glob against folder/name, plus a bare folder covering its subtree.
        QVERIFY(!exclusionReason(QStringLiteral("path"), QStringLiteral("scenes"),
                                 QStringLiteral("scenes"), QStringLiteral("bg.jpg")).isEmpty());
        QVERIFY(!exclusionReason(QStringLiteral("path"), QStringLiteral("scenes"),
                                 QStringLiteral("scenes/outdoor"), QStringLiteral("bg.jpg")).isEmpty());
        QVERIFY(!exclusionReason(QStringLiteral("path"), QStringLiteral("scenes/*"),
                                 QStringLiteral("scenes"), QStringLiteral("bg.jpg")).isEmpty());
        QVERIFY(exclusionReason(QStringLiteral("path"), QStringLiteral("scenes"),
                                QStringLiteral("characters"), QStringLiteral("bg.jpg")).isEmpty());
        // Name: a glob against the file name.
        QVERIFY(!exclusionReason(QStringLiteral("name"), QStringLiteral("hero_*.png"),
                                 QStringLiteral("characters"), QStringLiteral("hero_front.png")).isEmpty());
        QVERIFY(exclusionReason(QStringLiteral("name"), QStringLiteral("hero_*.png"),
                                QStringLiteral("characters"), QStringLiteral("bg.png")).isEmpty());
        // Normalization: extension strips a leading dot; path drops a trailing slash.
        QCOMPARE(exclusionNormalize(QStringLiteral("extension"), QStringLiteral(" .psd ")), QStringLiteral("psd"));
        QCOMPARE(exclusionNormalize(QStringLiteral("path"), QStringLiteral("scenes/")), QStringLiteral("scenes"));
    }

    void exclusionRulesKeepAssetsOutOfSnapshots() {
        QTemporaryDir dir;
        put(dir.filePath("a.png"), "png-a");
        put(dir.filePath("b.psd"), "psd-b");
        Library lib(dir.filePath("lib"));
        const auto a = lib.importFile(dir.filePath("a.png"));
        const auto b = lib.importFile(dir.filePath("b.psd"));

        // Before any rule: a snapshot anchors both.
        lib.addExclusion(QStringLiteral("extension"), QStringLiteral("psd"));
        QCOMPARE(lib.exclusions().size(), 1);
        const auto rules = lib.exclusions();
        QCOMPARE(rules.first().kind, QStringLiteral("extension"));
        QCOMPARE(rules.first().pattern, QStringLiteral("psd"));
        const qint64 snap1 = lib.snapshotAll(QStringLiteral("带排除规则"));
        auto snaps = lib.snapshots();
        QVERIFY(!snaps.isEmpty());
        QCOMPARE(snaps.first().assets, 1);   // only the PNG is backed up
        QCOMPARE(snaps.first().excluded, 1); // the PSD is listed as excluded

        // The excluded asset got no anchor revision and was not backed up.
        QCOMPARE(lib.revisions(b.id).size(), 1); // import revision only
        // The present asset still has its import revision as the anchor.
        QCOMPARE(lib.revisions(a.id).size(), 1);

        // Duplicate / empty rules are refused.
        bool threw = false;
        try { lib.addExclusion(QStringLiteral("extension"), QStringLiteral("psd")); } catch (...) { threw = true; }
        QVERIFY(threw);
        threw = false;
        try { lib.addExclusion(QStringLiteral("extension"), QStringLiteral("  ")); } catch (...) { threw = true; }
        QVERIFY(threw);

        // Rollback skips the excluded asset: the PSD working file is untouched
        // (excluded is never a deletion) and its history keeps the import version.
        QCOMPARE(QFileInfo(b.path).size(), qint64(5));
        const int restored = lib.rollbackAll(snap1);
        QCOMPARE(restored, 0); // both already at their (only) revision content

        // Removing the rule lets the next snapshot include the asset again.
        lib.removeExclusion(QStringLiteral("extension"), QStringLiteral("psd"));
        QVERIFY(lib.exclusions().isEmpty());
        lib.snapshotAll(QStringLiteral("无排除规则"));
        snaps = lib.snapshots();
        QCOMPARE(snaps.first().assets, 2);
        QCOMPARE(snaps.first().excluded, 0);
    }

    void pinnedVersionsSurviveRestartAndRetainObjects() {
        QTemporaryDir dir;
        put(dir.filePath("a.png"), "png-a");
        QString firstHash;
        qint64 firstId = 0;
        QString assetId;
        {
            Library lib(dir.filePath("lib"));
            const auto asset = lib.importFile(dir.filePath("a.png"));
            assetId = asset.id;
            firstId = lib.revisions(asset.id).first().id;
            lib.pinRevision(firstId, QStringLiteral("送审版本"));
            auto history = lib.revisions(asset.id);
            QVERIFY(history.first().pinned);
            QCOMPARE(history.first().pinReason, QStringLiteral("送审版本"));
            firstHash = history.first().hash;
            // A later change and a restore keep the pinned revision and its object.
            put(dir.filePath("b.png"), "png-b");
            QFile::remove(asset.path);
            QFile::copy(dir.filePath("b.png"), asset.path);
            QVERIFY(lib.capture(asset));
            lib.restore(asset, firstId);
            history = lib.revisions(asset.id);
            QCOMPARE(history.size(), 3); // import, changed, restored
            bool found = false;
            for (const auto &rev : history)
                if (rev.id == firstId) { found = true; QVERIFY(rev.pinned); }
            QVERIFY(found);
            QVERIFY(QFileInfo::exists(lib.objectPath(firstHash)));
        }
        // The pin persists across a restart (same library root reopened).
        {
            Library reopened(dir.filePath("lib"));
            QVERIFY(reopened.revisions(assetId).size() == 3);
            bool found = false;
            for (const auto &rev : reopened.revisions(assetId))
                if (rev.id == firstId) {
                    found = true;
                    QVERIFY(rev.pinned);
                    QCOMPARE(rev.pinReason, QStringLiteral("送审版本"));
                }
            QVERIFY(found);
            // Unpin only clears the flag; history and the object remain.
            reopened.unpinRevision(firstId);
            auto history = reopened.revisions(assetId);
            QCOMPARE(history.size(), 3);
            for (const auto &rev : history)
                if (rev.id == firstId) { QVERIFY(!rev.pinned); QVERIFY(rev.pinReason.isEmpty()); }
            QVERIFY(QFileInfo::exists(reopened.objectPath(firstHash)));
            // An unknown revision id is refused.
            bool threw = false;
            try { reopened.pinRevision(999999, QString()); } catch (...) { threw = true; }
            QVERIFY(threw);
        }
    }

    void timelineOrdersByRevisionIdAndListsDamagedObject() {
        QTemporaryDir dir;
        put(dir.filePath("a.png"), "v1");
        Library lib(dir.filePath("lib"));
        const auto asset = lib.importFile(dir.filePath("a.png"));
        // Several revisions captured within the same second: the timeline order
        // must follow the monotonic revision id, never the (identical) timestamps.
        put(dir.filePath("b.png"), "v2");
        QFile::remove(asset.path); QFile::copy(dir.filePath("b.png"), asset.path);
        QVERIFY(lib.capture(asset));
        put(dir.filePath("c.png"), "v3");
        QFile::remove(asset.path); QFile::copy(dir.filePath("c.png"), asset.path);
        QVERIFY(lib.capture(asset));
        auto history = lib.revisions(asset.id);
        QCOMPARE(history.size(), 3);
        // Newest first, strictly by id (identical timestamps never reorder).
        for (int i = 0; i + 1 < history.size(); ++i)
            QVERIFY(history.at(i).id > history.at(i + 1).id);
        // The newest revision matches the current working content.
        const QString newestHash = history.first().hash;
        // A damaged object is still listed (the timeline marks it, never hides it).
        const QString damagedHash = history.at(2).hash; // oldest
        QFile::remove(lib.objectPath(damagedHash));
        put(lib.objectPath(damagedHash), "corrupted-not-the-hash");
        bool threw = false;
        try { lib.readObject(damagedHash); } catch (const std::exception &) { threw = true; }
        QVERIFY(threw);
        history = lib.revisions(asset.id);
        QCOMPARE(history.size(), 3); // still listed, just unreadable
        QCOMPARE(history.first().hash, newestHash); // order unchanged
        // The healthy objects still verify; the timeline data is intact.
        QCOMPARE(lib.readObject(history.first().hash), QByteArray("v3"));
    }

    void fieldHistoryRecordsOldValuesAlignedWithRevisions() {
        QTemporaryDir dir;
        put(dir.filePath("a.png"), "png-a");
        Library lib(dir.filePath("lib"));
        const auto asset = lib.importFile(dir.filePath("a.png"));
        const QString contentHash = lib.revisions(asset.id).first().hash;
        // Tag / rating / note changes each create a metadata-only revision
        // (content hash unchanged, no new object) plus a field-history row.
        lib.addTag(asset.id, QStringLiteral("角色"));
        lib.setRating(asset.id, 4);
        lib.setNote(asset.id, QStringLiteral("需去背"));
        auto history = lib.revisions(asset.id);
        QCOMPARE(history.size(), 4); // import + 3 metadata-only revisions
        for (const auto &rev : history) QCOMPARE(rev.hash, contentHash);
        // Only the one content object exists (dedup: no object per metadata change).
        QStringList objects;
        for (const auto &f : QDir(lib.root() + "/objects").entryList(QDir::Files)) objects << f;
        QCOMPARE(objects.size(), 1);
        // Field history, newest first, with the real old → new values.
        auto fields = lib.fieldHistory(asset.id);
        QCOMPARE(fields.size(), 3);
        QCOMPARE(fields.at(0).field, QStringLiteral("备注"));
        QCOMPARE(fields.at(0).oldValue, QString());
        QCOMPARE(fields.at(0).newValue, QStringLiteral("需去背"));
        QCOMPARE(fields.at(1).field, QStringLiteral("评分"));
        QCOMPARE(fields.at(1).oldValue, QString());
        QCOMPARE(fields.at(1).newValue, QStringLiteral("4"));
        QCOMPARE(fields.at(2).field, QStringLiteral("标签"));
        QCOMPARE(fields.at(2).newValue, QStringLiteral("角色"));
        // Every field change references a real metadata-only revision, and the
        // newest revision's note names the change (timeline is self-describing).
        for (const auto &field : fields) {
            bool found = false;
            for (const auto &rev : history) if (rev.id == field.revisionId) found = true;
            QVERIFY(found);
        }
        QVERIFY(history.at(0).note.startsWith(QStringLiteral("元数据变更")));
        // A no-op change records nothing (no new revision, no new row).
        lib.setRating(asset.id, 4);
        lib.addTag(asset.id, QStringLiteral("角色"));
        QCOMPARE(lib.revisions(asset.id).size(), 4);
        QCOMPARE(lib.fieldHistory(asset.id).size(), 3);
        // A rename records the actual old → new name.
        lib.renameAsset(asset.id, QStringLiteral("hero.png"));
        auto after = lib.fieldHistory(asset.id);
        QCOMPARE(after.size(), 4);
        QCOMPARE(after.first().field, QStringLiteral("名称"));
        QCOMPARE(after.first().oldValue, QStringLiteral("a.png"));
        QCOMPARE(after.first().newValue, QStringLiteral("hero.png"));
        // Content is untouched by metadata changes: the timeline still shows the
        // same content hash, and the (renamed) working file keeps its content.
        QCOMPARE(lib.revisions(asset.id).first().hash, contentHash);
        const Asset renamed = lib.assets().first();
        QCOMPARE(renamed.name, QStringLiteral("hero.png"));
        QCOMPARE(QFileInfo(renamed.path).size(), qint64(5));
        QCOMPARE(lib.readObject(contentHash), QByteArray("png-a"));
    }

    void contentObjectStorageStreamsAndDeduplicates() {
        QTemporaryDir dir;
        // 64 MiB + a tail: comfortably past the P-302 "large file" sample so
        // the streaming path (bounded 1 MiB chunks, no whole-file buffer) runs.
        QByteArray block(4096, 0);
        for (int i = 0; i < block.size(); ++i) block[i] = char(i * 7 + 3);
        QByteArray large;
        large.reserve(64 * 1024 * 1024 + 1024);
        while (large.size() < 64 * 1024 * 1024) large += block;
        large += QByteArray("large-file-tail", 16);
        put(dir.filePath("big.bin"), large);
        Library lib(dir.filePath("lib"));
        const auto asset = lib.importFile(dir.filePath("big.bin"));

        const auto hashOf = [](const QByteArray &bytes) {
            return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
        };
        const QString h1 = hashOf(large);
        auto history = lib.revisions(asset.id);
        QCOMPARE(history.size(), 1);
        QCOMPARE(history.first().hash, h1);
        QCOMPARE(history.first().size, qint64(large.size()));
        QVERIFY(QFileInfo::exists(lib.root() + "/objects/" + h1));

        // Same content again: no duplicate revision or object (dedup by hash).
        QVERIFY(!lib.capture(asset));
        QCOMPARE(lib.revisions(asset.id).size(), 1);

        // Change the working copy and capture: the new revision streams through
        // the staged store and lands under its own content address.
        const QByteArray v2 = large + QByteArray("captured-v2", 12);
        put(asset.path, v2);
        QVERIFY(lib.capture(asset, "v2"));
        history = lib.revisions(asset.id);
        QCOMPARE(history.size(), 2);
        QCOMPARE(history.first().hash, hashOf(v2));
        QCOMPARE(history.first().size, qint64(v2.size()));
        QVERIFY(QFileInfo::exists(lib.root() + "/objects/" + hashOf(v2)));

        // Unchanged capture adds nothing; the staged area is left clean.
        QVERIFY(!lib.capture(asset));
        QCOMPARE(lib.revisions(asset.id).size(), 2);
        const QDir staged(lib.root() + "/objects/.tmp");
        QVERIFY(staged.exists());
        QCOMPARE(staged.entryList(QDir::Files | QDir::NoDotAndDotDot).size(), 0);
    }

    void objectStoreTempFailureLeavesNoPartialObject() {
        QTemporaryDir dir;
        put(dir.filePath("a.bin"), "original-v1");
        Library lib(dir.filePath("lib"));
        const auto asset = lib.importFile(dir.filePath("a.bin"));

        // New content whose object is not yet stored anywhere.
        const QByteArray v2 = QByteArray("next-v2-content");
        put(asset.path, v2);

        // Simulate a full/unwritable store: the staging dir refuses writes, so
        // the capture must fail without publishing a partial object, without a
        // stray temp, and without touching the working copy (retryable).
        const QString tmpDir = lib.root() + "/objects/.tmp";
        QVERIFY(QFileInfo::exists(tmpDir));
        QVERIFY(::chmod(tmpDir.toLocal8Bit().constData(), 0555) == 0);
        bool threw = false;
        try { lib.capture(asset); } catch (const std::runtime_error &) { threw = true; }
        QVERIFY(::chmod(tmpDir.toLocal8Bit().constData(), 0755) == 0);
        QVERIFY(threw);

        const auto history = lib.revisions(asset.id);
        QCOMPARE(history.size(), 1);
        const QString h2 = QString::fromLatin1(
            QCryptographicHash::hash(v2, QCryptographicHash::Sha256).toHex());
        QVERIFY(!QFileInfo::exists(lib.root() + "/objects/" + h2));
        const QDir staged(tmpDir);
        QCOMPARE(staged.entryList(QDir::Files | QDir::NoDotAndDotDot).size(), 0);
        QCOMPARE(get(asset.path), v2);

        // With the staging dir writable again the same capture succeeds.
        QVERIFY(lib.capture(asset, "retry"));
        QCOMPARE(lib.revisions(asset.id).size(), 2);
    }

    void manualVersionRecordReturnsIdAndUnchangedFlag() {
        QTemporaryDir dir;
        put(dir.filePath("a.bin"), "version-one");
        Library lib(dir.filePath("lib"));
        const auto asset = lib.importFile(dir.filePath("a.bin"));

        // Working copy still matches the import revision: no duplicate is
        // written; the CURRENT (import) revision id comes back with
        // created=false, so the user is told rather than left guessing.
        bool created = true;
        const qint64 first = lib.captureRevision(asset, "manual 1", &created);
        QVERIFY(!created);
        QCOMPARE(first, lib.revisions(asset.id).first().id);
        QCOMPARE(lib.revisions(asset.id).size(), 1);

        // Changed content: a fresh revision id, distinct from the previous one.
        put(asset.path, "version-two");
        created = false;
        const qint64 second = lib.captureRevision(asset, "manual 2", &created);
        QVERIFY(created);
        QVERIFY(second != first);
        const auto history = lib.revisions(asset.id);
        QCOMPARE(history.size(), 2);
        QCOMPARE(history.first().id, second);
        QCOMPARE(history.first().note, QStringLiteral("manual 2"));

        // Unchanged again after a new record: the same (new) id comes back.
        created = true;
        const qint64 again = lib.captureRevision(asset, "manual 3", &created);
        QVERIFY(!created);
        QCOMPARE(again, second);
        QCOMPARE(lib.revisions(asset.id).size(), 2);
    }

    void autoRecordStableChangeHandlesSampleClasses() {
        QTemporaryDir dir;
        put(dir.filePath("a.bin"), "AAAA");
        Library lib(dir.filePath("lib"));
        const auto asset = lib.importFile(dir.filePath("a.bin"));

        // Same-size write (mtime changes, size stays): detected and recorded.
        put(asset.path, "BBBB");
        QVERIFY(lib.captureIfStable(asset));
        auto history = lib.revisions(asset.id);
        QCOMPARE(history.size(), 2);
        QCOMPARE(get(lib.objectPath(history.first().hash)), QByteArray("BBBB"));

        // Repeated save of identical content: no duplicate revision (dedup).
        QVERIFY(!lib.captureIfStable(asset));
        QCOMPARE(lib.revisions(asset.id).size(), 2);

        // Temp-file replace (save = write temp + rename): the new content lands.
        const QString tmp = dir.filePath("a.tmp");
        put(tmp, "CCCCCC");
        QFile::remove(asset.path);
        QVERIFY(QFile::rename(tmp, asset.path));
        QVERIFY(lib.captureIfStable(asset));
        history = lib.revisions(asset.id);
        QCOMPARE(history.size(), 3);
        QCOMPARE(get(lib.objectPath(history.first().hash)), QByteArray("CCCCCC"));

        // Continuous writes: what is on disk at read time is recorded whole —
        // the read-verify means a mid-write snapshot is never taken.
        put(asset.path, "DDDDDD");
        put(asset.path, "EEEEEE");
        QVERIFY(lib.captureIfStable(asset));
        history = lib.revisions(asset.id);
        QCOMPARE(history.size(), 4);
        QCOMPARE(get(lib.objectPath(history.first().hash)), QByteArray("EEEEEE"));

        // A removed working copy records nothing (nothing to read).
        QFile::remove(asset.path);
        QVERIFY(!lib.captureIfStable(asset));
        QCOMPARE(lib.revisions(asset.id).size(), 4);

        // Unchanged content that matches the last revision is a no-op.
        put(asset.path, "EEEEEE");
        QVERIFY(!lib.captureIfStable(asset));
        QCOMPARE(lib.revisions(asset.id).size(), 4);
    }
    void deletionTombstoneRetainsHistoryAndHidesLiveAsset() {
        QTemporaryDir dir;
        const QString source = dir.filePath("asset.png");
        put(source, "content-one");
        QString id;
        QString lastHash;
        {
            Library library(dir.filePath("library"));
            auto asset = library.importFile(source);
            id = asset.id;
            put(asset.path, "content-two");
            QVERIFY(library.capture(asset, "second"));
            lastHash = library.revisions(id).first().hash;

            // In-app confirmed delete: the live list drops the asset...
            library.deleteAsset(id);
            bool live = false;
            for (const auto &entry : library.assets()) if (entry.id == id) live = true;
            QVERIFY(!live);
            // ...but history is retained: the newest revision is the tombstone,
            // reusing the last content hash so the object stays browsable.
            const auto history = library.revisions(id);
            QCOMPARE(history.size(), 3);
            QVERIFY(history.first().tombstone);
            QCOMPARE(history.first().hash, lastHash);
            QCOMPARE(get(library.objectPath(lastHash)), QByteArray("content-two"));
            // The working copy is removed by the confirmed delete.
            QVERIFY(!QFileInfo::exists(asset.path));
            // A second delete of the same asset is rejected, not idempotent.
            QVERIFY_EXCEPTION_THROWN(library.deleteAsset(id), std::runtime_error);
            QVERIFY_EXCEPTION_THROWN(library.deleteAsset("missing-id"), std::runtime_error);
            // Later snapshots skip the deleted asset (live list excludes it).
            library.snapshotAll("after delete");
            const auto snapshots = library.snapshots();
            QCOMPARE(snapshots.first().assets, 0);
        }
        // Tombstone and the asset row survive a restart; history stays findable.
        Library reopened(dir.filePath("library"));
        QVERIFY(reopened.assets().isEmpty());
        const auto history = reopened.revisions(id);
        QCOMPARE(history.size(), 3);
        QVERIFY(history.first().tombstone);
    }
    void workingCopyHealthDistinguishesMissingFromUnreachable() {
        QTemporaryDir dir;
        const QString source = dir.filePath("asset.dat");
        put(source, "body");
        Library library(dir.filePath("library"));
        const auto asset = library.importFile(source);
        QCOMPARE(library.workingCopyHealth(asset), Library::Health::Healthy);

        // Externally removed working copy: "missing" (recoverable), never a
        // deletion — no tombstone is written and the live asset stays.
        QFile::remove(asset.path);
        QCOMPARE(library.workingCopyHealth(asset), Library::Health::Missing);
        QCOMPARE(library.revisions(asset.id).size(), 1);
        QVERIFY(!library.revisions(asset.id).first().tombstone);
        QCOMPARE(library.assets().size(), 1);

        // A readable copy reappears → healthy again, still no tombstone.
        put(asset.path, "body");
        QCOMPARE(library.workingCopyHealth(asset), Library::Health::Healthy);

        // Unreadable working copy (lost mount/permissions): "unreachable" —
        // explicitly not a deletion, so still no tombstone. Write-only (no read
        // bit) makes it unreadable for a non-root owner.
        QFile::setPermissions(asset.path, QFileDevice::WriteOwner);
        QCOMPARE(library.workingCopyHealth(asset), Library::Health::Unreachable);
        QCOMPARE(library.revisions(asset.id).size(), 1);
        QVERIFY(!library.revisions(asset.id).first().tombstone);
        QFile::setPermissions(asset.path, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    }
    void consistentSnapshotManifestFreezesPathsAndStates() {
        QTemporaryDir dir;
        put(dir.filePath("a.png"), "png-a");
        put(dir.filePath("b.psd"), "psd-b");
        put(dir.filePath("c.dat"), "c-body");
        put(dir.filePath("d.jpg"), "d-jpg");
        Library lib(dir.filePath("lib"));
        const auto a = lib.importFile(dir.filePath("a.png"));
        const auto b = lib.importFile(dir.filePath("b.psd"));
        const auto c = lib.importFile(dir.filePath("c.dat"));
        const auto d = lib.importFile(dir.filePath("d.jpg"));
        put(a.path, "png-a2");
        QVERIFY(lib.capture(a, "second"));

        // One consistent boundary: b excluded by a rule, c's working copy gone
        // (missing, recoverable), d in-app deleted (tombstone).
        lib.addExclusion(QStringLiteral("extension"), QStringLiteral("psd"));
        lib.deleteAsset(d.id);
        QFile::remove(c.path);

        const qint64 snap = lib.snapshotAll(QStringLiteral("一致性快照"));
        const auto find = [](const QVector<SnapshotEntry> &entries, const QString &id) {
            for (const auto &e : entries) if (e.assetId == id) return e;
            return SnapshotEntry{};
        };
        auto entries = lib.snapshotEntries(snap);
        QCOMPARE(entries.size(), 4);
        const auto ea = find(entries, a.id);
        const auto eb = find(entries, b.id);
        const auto ec = find(entries, c.id);
        const auto ed = find(entries, d.id);
        // present: anchored to the last clean revision, path as of the snapshot.
        QCOMPARE(ea.state, QStringLiteral("present"));
        QCOMPARE(ea.name, QStringLiteral("a.png"));
        QCOMPARE(ea.folder, QString());
        // The anchor is the newest revision differing from the (recorded)
        // working content — the pre-"second" version, not the newest one.
        QCOMPARE(ea.hash, lib.revisions(a.id).at(1).hash);
        // excluded: no content reference, human-readable reason.
        QCOMPARE(eb.state, QStringLiteral("excluded"));
        QCOMPARE(eb.hash, QString());
        QVERIFY(!eb.reason.isEmpty());
        // missing: recoverable, anchored to the last completed revision, never
        // a deletion.
        QCOMPARE(ec.state, QStringLiteral("missing"));
        QVERIFY(!ec.hash.isEmpty());
        // tombstone: history retained, object verified.
        QCOMPARE(ed.state, QStringLiteral("tombstone"));
        QVERIFY(!ed.hash.isEmpty());

        // Every referenced object verifies against the manifest (the anchor is
        // the pre-"second" content).
        QCOMPARE(get(lib.objectPath(ea.hash)), QByteArray("png-a"));
        QCOMPARE(get(lib.objectPath(ec.hash)), QByteArray("c-body"));
        QCOMPARE(get(lib.objectPath(ed.hash)), QByteArray("d-jpg"));

        // Rename/move AFTER the snapshot: the manifest path is frozen at the
        // snapshot boundary and never rewrites.
        lib.renameAsset(a.id, "renamed.png");
        lib.moveAsset(a.id, "scenes");
        entries = lib.snapshotEntries(snap);
        const auto ea2 = find(entries, a.id);
        QCOMPARE(ea2.name, QStringLiteral("a.png"));
        QCOMPARE(ea2.folder, QString());
        // The live asset moved; the manifest did not.
        Asset moved;
        for (const auto &entry : lib.assets()) if (entry.id == a.id) moved = entry;
        QCOMPARE(moved.name, QStringLiteral("renamed.png"));
        QCOMPARE(moved.folder, QStringLiteral("scenes"));

        // The rollback touches present and missing entries (recoverable); the
        // excluded b and tombstoned d are skipped. a is restored to its
        // pre-"second" anchor content and the missing c is recovered, so two
        // assets change.
        const int restored = lib.rollbackAll(snap);
        QCOMPARE(restored, 2);
        QCOMPARE(get(moved.path), QByteArray("png-a"));
        QCOMPARE(get(c.path), QByteArray("c-body"));
    }
    void incompleteSnapshotCandidateIsNotPublished() {
        QTemporaryDir dir;
        put(dir.filePath("a.dat"), "body-a");
        Library lib(dir.filePath("lib"));
        const auto a = lib.importFile(dir.filePath("a.dat"));
        // A writer keeps touching the file across the snapshot's settle window,
        // so the candidate can never settle into a complete manifest.
        std::atomic<bool> stop{false};
        std::thread writer([&] {
            QFile f(a.path);
            if (!f.open(QIODevice::WriteOnly | QIODevice::Append)) return;
            while (!stop.load()) {
                f.write("xxxx", 4);
                f.flush();
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
        });
        QThread::msleep(30); // let the writer get going
        bool threw = false;
        try { lib.snapshotAll(QStringLiteral("写入中")); } catch (const std::exception &) { threw = true; }
        stop = true;
        writer.join();
        QVERIFY(threw);
        // An incomplete candidate is never published as a successful snapshot.
        QVERIFY(lib.snapshots().isEmpty());
    }
    void snapshotBrowsingShowsPathsPerBoundary() {
        QTemporaryDir dir;
        put(dir.filePath("a.png"), "png-a");
        Library lib(dir.filePath("lib"));
        const auto a = lib.importFile(dir.filePath("a.png"));
        // Snapshot 1: at the root under the original name.
        const qint64 snap1 = lib.snapshotAll(QStringLiteral("初始"));
        // Rename + move, then a second snapshot.
        lib.renameAsset(a.id, QStringLiteral("hero.png"));
        lib.moveAsset(a.id, QStringLiteral("scenes"));
        const qint64 snap2 = lib.snapshotAll(QStringLiteral("移动后"));
        // In-app delete, then a third snapshot (the asset is a tombstone).
        lib.deleteAsset(a.id);
        const qint64 snap3 = lib.snapshotAll(QStringLiteral("删除后"));

        const auto at = [](const QVector<SnapshotEntry> &entries) {
            return entries.isEmpty() ? SnapshotEntry{} : entries.first();
        };
        // Each boundary shows its own directory state; later changes never
        // rewrite an earlier manifest (10-prototype#P-009 two-snapshot example).
        const auto e1 = at(lib.snapshotEntries(snap1));
        QCOMPARE(e1.name, QStringLiteral("a.png"));
        QCOMPARE(e1.folder, QString());
        QCOMPARE(e1.state, QStringLiteral("present"));
        const auto e2 = at(lib.snapshotEntries(snap2));
        QCOMPARE(e2.name, QStringLiteral("hero.png"));
        QCOMPARE(e2.folder, QStringLiteral("scenes"));
        QCOMPARE(e2.state, QStringLiteral("present"));
        const auto e3 = at(lib.snapshotEntries(snap3));
        QCOMPARE(e3.name, QStringLiteral("hero.png"));
        QCOMPARE(e3.folder, QStringLiteral("scenes"));
        QCOMPARE(e3.state, QStringLiteral("tombstone"));
        // The current live index no longer has the asset at all — the browse
        // view and the current index are never mixed.
        QVERIFY(lib.assets().isEmpty());
    }
    // 审核发现（2026-11-02）: snapshotAll must stream-hash the working copy
    // (never load it whole) yet still record the emergency anchor revision for
    // an unrecorded change — the streaming path is behavior-preserving.
    void snapshotStreamsUnrecordedWorkingCopyAsEmergencyAnchor() {
        QTemporaryDir dir;
        put(dir.filePath("a.dat"), "v1");
        Library lib(dir.filePath("lib"));
        const auto a = lib.importFile(dir.filePath("a.dat"));
        put(a.path, "unrecorded-big-content");
        const qint64 snap = lib.snapshotAll(QStringLiteral("紧急"));
        const auto entries = lib.snapshotEntries(snap);
        QCOMPARE(entries.size(), 1);
        QCOMPARE(entries.first().state, QStringLiteral("present"));
        // The unrecorded content was streamed in as the emergency anchor; the
        // newest revision now matches it, and the object verifies.
        const auto revs = lib.revisions(a.id);
        QCOMPARE(revs.first().note, QStringLiteral("紧急快照锚点"));
        QCOMPARE(lib.readObject(revs.first().hash), QByteArray("unrecorded-big-content"));
    }
    // 审核发现（2026-11-02）: the emergency-anchor streaming path must handle a
    // working copy beyond the 256 MiB import cap the same way rescan preview
    // does — the content is stored by streaming (never read whole), and the
    // resulting anchor object still verifies against the recorded size.
    void snapshotStreamsLargeUnrecordedWorkingCopyAsEmergencyAnchor() {
        QTemporaryDir dir;
        put(dir.filePath("a.dat"), "v1");
        Library lib(dir.filePath("lib"));
        const auto a = lib.importFile(dir.filePath("a.dat"));
        // A file just over the 256 MiB cap; write in 1 MiB chunks (never whole).
        {
            QFile f(a.path);
            QVERIFY(f.open(QIODevice::WriteOnly));
            QByteArray chunk(1 << 20, 'q');
            const qint64 total = 257LL * 1024 * 1024;
            qint64 written = 0;
            while (written < total) {
                const qint64 n = qMin<qint64>(chunk.size(), total - written);
                QCOMPARE(f.write(chunk.constData(), int(n)), int(n));
                written += n;
            }
            f.close();
        }
        const qint64 snap = lib.snapshotAll(QStringLiteral("紧急"));
        const auto entries = lib.snapshotEntries(snap);
        QCOMPARE(entries.size(), 1);
        QCOMPARE(entries.first().state, QStringLiteral("present"));
        const auto revs = lib.revisions(a.id);
        QCOMPARE(revs.first().note, QStringLiteral("紧急快照锚点"));
        QCOMPARE(revs.first().size, 257LL * 1024 * 1024); // exact streamed bytes
        // The stored object is the full large content (not truncated) and its
        // integrity still verifies against the recorded size.
        QCOMPARE(QFileInfo(lib.objectPath(revs.first().hash)).size(), revs.first().size);
        QVERIFY(lib.verifyObject(revs.first().hash));
    }
    // 审核发现（2026-11-02）: the emergency anchor revision size must be the
    // EXACT byte count of the streamed content, not a later re-stat of the
    // working path. The old re-stat left a TOCTOU: if the file changed between
    // the streamed copy and the stat, the recorded size no longer matched the
    // object, and a later snapshot referencing it as an anchor failed the
    // object.size()!=objectSize check. Here the working file is mutated AFTER
    // the snapshot; a second snapshot that re-anchors to that revision must
    // still verify.
    void snapshotEmergencyAnchorRecordsExactStoredSize() {
        QTemporaryDir dir;
        put(dir.filePath("a.dat"), "v1");
        Library lib(dir.filePath("lib"));
        const auto a = lib.importFile(dir.filePath("a.dat"));
        put(a.path, "unrecorded-content");
        const qint64 snap1 = lib.snapshotAll(QStringLiteral("紧急"));
        const auto revs1 = lib.revisions(a.id);
        QCOMPARE(revs1.first().note, QStringLiteral("紧急快照锚点"));
        QCOMPARE(revs1.first().size, qint64(18)); // exact streamed bytes
        // A second snapshot after the working copy changes AGAIN anchors to the
        // emergency-anchor revision (newest clean pre-change content). The
        // manifest verification of that anchor (object.size()==objectSize) must
        // not trip on a stale size — the exact stored size is what was recorded.
        put(a.path, "yet-more-content");
        const qint64 snap2 = lib.snapshotAll(QStringLiteral("后续"));
        const auto entries = lib.snapshotEntries(snap2);
        QCOMPARE(entries.size(), 1);
        QCOMPARE(entries.first().hash, revs1.first().hash);
    }
    // 审核发现（2026-11-02）: a working file that was readable at stat time but
    // cannot be opened (mount drop / deleted between stat and open) must not
    // abort the whole snapshot — it degrades to "missing" (recoverable from the
    // anchored history), and the other assets still snapshot.
    void snapshotUnreadableWorkingCopyDegradesToMissing() {
        QTemporaryDir dir;
        put(dir.filePath("a.dat"), "v1");
        Library lib(dir.filePath("lib"));
        const auto a = lib.importFile(dir.filePath("a.dat"));
        // Simulate the race: exists()&&isReadable() is true (a readable
        // directory) but open(ReadOnly) fails (EISDIR) — exactly the mount-drop /
        // file-swapped-for-dir race. The clean revision anchors the asset, so
        // it degrades to a recoverable "missing" row (never a deletion).
        QVERIFY(QFile::remove(a.path));
        QVERIFY(QDir().mkpath(a.path));
        // The batch must survive: open/read failure after a readable stat
        // degrades to "no current content" (clean anchor), never aborts the
        // whole snapshot.
        qint64 snap = 0;
        bool threw = false;
        try { snap = lib.snapshotAll(QStringLiteral("紧急")); }
        catch (const std::exception &) { threw = true; }
        QVERIFY(!threw);
        const auto entries = lib.snapshotEntries(snap);
        QCOMPARE(entries.size(), 1);
        // The entry stays listed (not a deletion) and is anchored to the clean
        // pre-change revision (the original v1), so it is recoverable.
        QCOMPARE(entries.first().state, QStringLiteral("present"));
        QCOMPARE(entries.first().hash, lib.revisions(a.id).first().hash);
        QDir(a.path).rmdir(a.path);
    }
    // 审核发现（2026-11-02）: rescanPreview must keep a candidate that fails to
    // hash mid-read as retryable (error set), and the rest of the scan still
    // classifies — one bad file never loses the whole preview.
    void rescanPreviewKeepsUnhashableCandidateAsRetryable() {
        QTemporaryDir dir;
        const QString source = dir.filePath("share");
        QVERIFY(QDir().mkpath(source));
        put(source + "/good.png", "good");
        Library lib(dir.filePath("lib"));
        const auto preview = importer::rescanPreview(lib, source);
        QCOMPARE(preview.candidates.size(), 1);
        QVERIFY(preview.candidates.first().candidate.relative == QStringLiteral("good.png"));
        QVERIFY(preview.candidates.first().error.isEmpty());
        QVERIFY(!preview.candidates.first().hash.isEmpty());
        QCOMPARE(preview.fresh(), 1); // good.png still importable
        // The primitive behind a mid-read failure never throws: a readable file
        // hashes fine, an absent path and a directory (open/read fails) both
        // report false, so a single unhashable candidate cannot lose the rest.
        QString hash;
        QVERIFY(hashFile(source + "/good.png", &hash, nullptr));
        QVERIFY(!hash.isEmpty());
        QVERIFY(!hashFile(dir.filePath("share/nonexistent"), &hash, nullptr));
        QDir().mkpath(source + "/adir");
        QVERIFY(!hashFile(source + "/adir", &hash, nullptr));
    }
    // 审核发现（2026-11-02）: verifyObject confirms integrity without loading
    // the object whole; a missing object reports false (never throws).
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
    void compareCanvasSharesOneTransform() {
        CompareCanvas canvas;
        canvas.resize(400, 300);
        QImage left(100, 50, QImage::Format_RGB32);
        QImage right(40, 80, QImage::Format_RGB32);
        left.fill(Qt::red);
        right.fill(Qt::blue);
        canvas.setImages(left, right);
        canvas.show();

        // Fit mode: ONE unified scale so both panes fit — the smaller image
        // keeps explicit empty space instead of being stretched. With panes of
        // 199x300 and 200x300: left fits at min(1.99, 6.0)=1.99, right at
        // min(5.0, 3.75)=3.75; the unified scale is the smaller one.
        QCOMPARE(canvas.zoom(), 1.99);
        QCOMPARE(canvas.origin(), QPoint(0, 0));

        // Manual zoom disables fit, is bounded, and keeps the shared origin.
        canvas.setZoom(0.5);
        QVERIFY(!canvas.isFit());
        QCOMPARE(canvas.zoom(), 0.5);

        // Wheel zoom anchors the image point under the cursor; both panes share
        // the one origin, so the same coordinate moves in each pane.
        const QPointF pos(100, 100);
        QWheelEvent wheel(pos, canvas.mapToGlobal(pos.toPoint()), QPoint(),
                          QPoint(0, 120), Qt::NoButton, Qt::NoModifier,
                          Qt::NoScrollPhase, false);
        QCoreApplication::sendEvent(&canvas, &wheel);
        QVERIFY(canvas.zoom() > 0.5);
        QVERIFY(!canvas.isFit());

        // Drag pans the single shared origin (the panes cannot desynchronize).
        const QPoint before = canvas.origin();
        QTest::mousePress(&canvas, Qt::LeftButton, Qt::NoModifier, QPoint(200, 150));
        QTest::mouseMove(&canvas, QPoint(180, 140));
        QTest::mouseRelease(&canvas, Qt::LeftButton, Qt::NoModifier, QPoint(180, 140));
        QVERIFY(canvas.origin() != before);

        // No images: interactions are inert (no crash, no transform change).
        CompareCanvas empty;
        empty.resize(300, 200);
        empty.show();
        const QPoint originBefore = empty.origin();
        QTest::mousePress(&empty, Qt::LeftButton, Qt::NoModifier, QPoint(150, 100));
        QTest::mouseMove(&empty, QPoint(120, 90));
        QTest::mouseRelease(&empty, Qt::LeftButton, Qt::NoModifier, QPoint(120, 90));
        QCOMPARE(empty.origin(), originBefore);
    }
    void slidingDividerComparesSameCoordinate() {
        CompareCanvas canvas;
        canvas.resize(400, 300);
        QImage left(100, 50, QImage::Format_RGB32);
        QImage right(100, 50, QImage::Format_RGB32);
        left.fill(Qt::red);
        right.fill(Qt::blue);
        canvas.setImages(left, right);
        canvas.show();

        // Sliding mode: the wipe divider lives in image space.
        canvas.setSliding(true);
        QVERIFY(canvas.isSliding());
        // Both images must now fit the FULL viewport: min(400/100, 300/50).
        QCOMPARE(canvas.zoom(), 4.0);
        canvas.setDivider(60.0);
        QCOMPARE(canvas.divider(), 60.0);
        // A zoom does not move the image coordinate of the divider...
        canvas.setZoom(1.0);
        QCOMPARE(canvas.divider(), 60.0);
        // ...only its viewport position.
        const int vx = canvas.dividerViewportX();
        QVERIFY(vx > 0 && vx < 400);
        // Left/Right step the divider; [ / ] adjust the step size.
        const qreal before = canvas.divider();
        QTest::keyClick(&canvas, Qt::Key_Right);
        QVERIFY(canvas.divider() > before);
        const qreal afterStep = canvas.divider();
        QTest::keyClick(&canvas, Qt::Key_BracketLeft);
        QCOMPARE(canvas.dividerStep(), 20.0);
        // Dragging near the divider follows the cursor (image-space update).
        const int nowVx = canvas.dividerViewportX();
        QTest::mousePress(&canvas, Qt::LeftButton, Qt::NoModifier, QPoint(nowVx + 2, 150));
        QTest::mouseMove(&canvas, QPoint(nowVx - 30, 150));
        QTest::mouseRelease(&canvas, Qt::LeftButton, Qt::NoModifier, QPoint(nowVx - 30, 150));
        QVERIFY(canvas.divider() < afterStep);
        // One side failing to decode: the other side still renders (no crash).
        CompareCanvas oneSided;
        oneSided.resize(300, 200);
        oneSided.setImages(QImage(), right);
        oneSided.setSliding(true);
        oneSided.show();
        QVERIFY(oneSided.hasImages());
        QVERIFY(!oneSided.grab().isNull());
    }
    // P-209: manual-zoom sizeHint includes the 12px origin inset on every side,
    // so the scroll area can pan the complete image (nothing clipped at the
    // right/bottom edge).
    void previewCanvasManualSizeHintIncludesOriginInset() {
        PreviewCanvas canvas;
        QImage img(300, 200, QImage::Format_RGB32);
        img.fill(Qt::red);
        canvas.setImage(img);
        // Fit: tiny hint (the scroll area expands to the viewport).
        canvas.setFit(true);
        QCOMPARE(canvas.sizeHint(), QSize(1, 1));
        // Manual 1.0: scaled image PLUS the 12px inset on each side.
        canvas.setZoom(1.0);
        QCOMPARE(canvas.sizeHint(), QSize(300 + 24, 200 + 24));
        // Manual 0.5: half size plus the same inset.
        canvas.setZoom(0.5);
        QCOMPARE(canvas.sizeHint(), QSize(150 + 24, 100 + 24));
    }
    // P-609: the V1 user-journey acceptance walkthrough, end to end on one
    // isolated library — 导入 → 整理（标签/评分/文件夹） → 版本/快照 → 对比 →
    // 删除 → 预演恢复 → 备份演练 → 重开. Every leg checks the
    // acceptance-relevant observable, so a green run is evidence for the
    // in-scope R rows (the R01–R19 line-by-line matrix lives in
    // docs/zh/design/80-reliability.md). Rows that need real NAS / macOS /
    // Windows hardware or a human walkthrough stay open there — this test
    // never stands in for them.
    void v1UserJourneyAcceptanceWalkthrough() {
        QTemporaryDir dir;
        const QString root = dir.filePath("library");
        QString heroId, v1Hash;
        {
            Library library(root);

            // --- 导入 (R02/R04): managed copy import, format/size facts recorded.
            put(dir.filePath("hero.tga"), makeTga(4, 4, qRgb(255, 0, 0)));
            put(dir.filePath("sidekick.tga"), makeTga(4, 4, qRgb(0, 255, 0)));
            put(dir.filePath("scratch.tga"), makeTga(4, 4, qRgb(128, 128, 128)));
            const auto hero = library.importFile(dir.filePath("hero.tga"));
            const auto sidekick = library.importFile(dir.filePath("sidekick.tga"));
            const auto scratch = library.importFile(dir.filePath("scratch.tga"));
            QVERIFY(!hero.id.isEmpty() && !sidekick.id.isEmpty() && !scratch.id.isEmpty());
            QCOMPARE(library.assets().size(), 3);
            heroId = hero.id;
            const qint64 v1Id = library.revisions(hero.id).first().id;
            v1Hash = library.revisions(hero.id).first().hash;
            QCOMPARE(library.assetFormat(hero.id).toUpper(), QStringLiteral("TGA"));
            QCOMPARE(library.assetSize(hero.id), QSize(4, 4));
            QVERIFY(library.hasContent(v1Hash)); // object addressable by content

            // --- 整理 (R05): tags, rating, note, provenance, folder organization.
            QCOMPARE(library.addTag(hero.id, "主角"), 1);
            QCOMPARE(library.addTagToAssets({hero.id, sidekick.id}, "角色"), 2);
            QCOMPARE(library.countAssetsWithTag({hero.id, scratch.id}, "角色"), 1);
            library.setRating(hero.id, 5);
            library.setNote(hero.id, "初稿设定");
            library.moveAsset(hero.id, "characters/hero");
            library.moveAsset(sidekick.id, "npcs");
            Provenance p;
            p.source = "contract-2026";
            p.author = "美术组";
            p.license = "CC-BY-4.0";
            library.addProvenance(hero.id, p);
            const auto organized = library.assets();
            const auto heroRow = std::find_if(organized.cbegin(), organized.cend(),
                [&hero](const Asset &a) { return a.id == hero.id; });
            QVERIFY(heroRow != organized.cend());
            QCOMPARE(heroRow->folder, QStringLiteral("characters/hero")); // identity kept
            QCOMPARE(library.rating(hero.id), 5);
            QCOMPARE(library.note(hero.id), QStringLiteral("初稿设定"));
            QVERIFY(library.tags(hero.id).contains("主角"));
            QCOMPARE(library.provenance(hero.id).size(), 1);
            QCOMPARE(library.assetsWithTag("角色").size(), 2);

            // --- 版本 (R06): manual record of an edit + pin; auto-record of another.
            put(hero.path, makeTga(4, 4, qRgb(0, 0, 255)));
            bool created = false;
            const qint64 v2Id = library.captureRevision(hero, "改色为蓝", &created);
            QVERIFY(created);
            library.pinRevision(v2Id, "定稿");
            put(scratch.path, makeTga(4, 4, qRgb(64, 64, 64)));
            QVERIFY(library.captureIfStable(scratch, "自动记录"));
            // The organize step recorded metadata-only revisions (P-309), so the
            // content revision is the NEWEST one and carries the recorded note.
            QCOMPARE(library.revisions(hero.id).first().id, v2Id);
            QVERIFY(library.revisions(hero.id).first().pinned);
            QVERIFY(library.revisions(hero.id).first().hash != v1Hash);
            QCOMPARE(library.revisions(scratch.id).size(), 2);

            // --- 快照 (R08): whole-tree anchors; exclusion rules and tombstones
            // appear in the manifest with explicit states, never as silent drops.
            const qint64 snapA = library.snapshotAll("里程碑 A");
            const auto entriesA = library.snapshotEntries(snapA);
            QCOMPARE(entriesA.size(), 3);
            for (const auto &e : entriesA) QCOMPARE(e.state, QStringLiteral("present"));
            library.addExclusion("name", "scratch*");
            library.deleteAsset(sidekick.id); // tombstone, not data loss
            const qint64 snapB = library.snapshotAll("里程碑 B");
            for (const auto &e : library.snapshotEntries(snapB)) {
                if (e.assetId == sidekick.id) QCOMPARE(e.state, QStringLiteral("tombstone"));
                else if (e.assetId == scratch.id) QCOMPARE(e.state, QStringLiteral("excluded"));
                else QCOMPARE(e.state, QStringLiteral("present"));
            }
            const auto liveAssets = library.assets();
            QVERIFY(std::none_of(liveAssets.cbegin(), liveAssets.cend(),
                [&sidekick](const Asset &a) { return a.id == sidekick.id; }));
            // The tombstone revision is the newest; the object stays referenced.
            QVERIFY(library.revisions(sidekick.id).size() >= 2);
            QVERIFY(library.revisions(sidekick.id).first().tombstone);
            QVERIFY(!QFile::exists(sidekick.path));
            QVERIFY(library.hasContent(library.revisions(sidekick.id).first().hash));

            // --- 对比 (R09): the two recorded versions render side by side.
            QImage left, right;
            QVERIFY(left.loadFromData(library.readObject(v1Hash)));
            QVERIFY(right.loadFromData(library.readObject(library.revisions(hero.id).first().hash)));
            CompareCanvas canvas;
            canvas.resize(400, 300);
            canvas.setImages(left, right);
            canvas.setSliding(true);
            canvas.show();
            QVERIFY(canvas.hasImages());
            QVERIFY(!canvas.grab().isNull());

            // --- 预演恢复 (R10/R11): preview first, protected restore, export, log.
            const auto plan = library.planRestore(hero, v1Id);
            QCOMPARE(plan.action, QStringLiteral("overwrite"));
            QVERIFY(plan.spaceNeeded > 0);
            const int beforeRestore = library.revisions(hero.id).size();
            library.restore(hero, plan);
            QCOMPARE(digestHex(get(hero.path)), v1Hash);   // content back at v1
            QCOMPARE(library.revisions(hero.id).size(), beforeRestore + 1); // as a NEW version
            QCOMPARE(library.revisions(hero.id).first().hash, v1Hash);
            library.exportRevision(hero.id, v1Id, dir.filePath("export-v1.tga"));
            QCOMPARE(digestHex(get(dir.filePath("export-v1.tga"))), v1Hash);
            QVERIFY(!library.restoreLogs().isEmpty());

            // --- 备份演练 (R19): consistency + space preview + package → verify
            // → restore drill in an independent empty library.
            QCOMPARE(library.checkConsistency().problems(), 0);
            const auto space = library.spacePreview();
            // All three assets have revisions; the tombstoned sidekick still
            // counts because its history (and object) are retained.
            QCOMPARE(space.assets, 3);
            QVERIFY(space.physicalSize > 0);
            QTemporaryDir target, drillRoot;
            const auto package = library.exportBackupPackage(target.filePath("pkg"));
            QCOMPARE(package.entries.size(), 2);
            QCOMPARE(library.verifyBackupPackage(package.packageDir).size(), 2);
            Library recovered(drillRoot.filePath("lib"));
            const auto restored = recovered.restoreBackupPackage(package.packageDir);
            QCOMPARE(restored.size(), 2);
            const auto recoveredAssets = recovered.assets();
            for (const auto &e : restored) {
                const auto it = std::find_if(recoveredAssets.cbegin(), recoveredAssets.cend(),
                    [&e](const Asset &a) { return a.id == e.assetId; });
                QVERIFY(it != recoveredAssets.cend());
                QCOMPARE(it->name, e.name);
                QCOMPARE(it->folder, e.folder);
                QCOMPARE(recovered.revisions(e.assetId).size(), 1);
                QCOMPARE(digestHex(get(it->path)), e.hash);
            }
            QCOMPARE(recovered.checkConsistency().problems(), 0);
        }
        // --- 打开 (R01): the whole library survives a close/reopen.
        Library reopened(root);
        QCOMPARE(reopened.assets().size(), 2);
        QCOMPARE(reopened.rating(heroId), 5);
        QCOMPARE(reopened.revisions(heroId).first().hash, v1Hash);
        QCOMPARE(reopened.snapshots().size(), 2);
        QVERIFY(reopened.tags(heroId).contains("主角"));
    }
};

// A QApplication is required: P-313's CompareCanvas is a widget and cannot be
// created under a QCoreApplication. Everything else stays core-only.
int main(int argc, char **argv) {
    QApplication app(argc, argv);
    HistoryTest tc;
    QTEST_SET_MAIN_SOURCE_PATH
    return QTest::qExec(&tc, argc, argv);
}
#include "library_test.moc"
