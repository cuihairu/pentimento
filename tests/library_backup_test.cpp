// Backup packages, consistency audit, space preview and the fault matrix (interrupted writes and index commits).
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
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
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

class BackupConsistencyTest : public QObject {
    Q_OBJECT
private slots:
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

    // Export refuses unusable destinations with concrete reasons before any
    // file is written: empty path, unwritable parent, unwritable target dir.
    void backupExportRefusesUnusableDestinations() {
        if (::geteuid() == 0) QSKIP("root bypasses directory write permissions");
        QTemporaryDir dir;
        put(dir.filePath("a.png"), "bytes-a");
        Library library(dir.filePath("library"));
        QVERIFY(!library.importFile(dir.filePath("a.png")).id.isEmpty());
        bool refused = false;
        try { library.exportBackupPackage(QStringLiteral("   ")); }
        catch (const std::exception &) { refused = true; }
        QVERIFY(refused);
        // A target under an unwritable parent cannot be created.
        const QString lockedParent = dir.filePath("locked-parent");
        QVERIFY(QDir().mkpath(lockedParent));
        QVERIFY(::chmod(lockedParent.toLocal8Bit().constData(), 0555) == 0);
        refused = false;
        try { library.exportBackupPackage(lockedParent + "/pkg"); }
        catch (const std::exception &) { refused = true; }
        QVERIFY(refused);
        QVERIFY(!QFileInfo::exists(lockedParent + "/pkg"));
        // An existing empty-but-unwritable target cannot host objects/.
        const QString lockedTarget = dir.filePath("locked-target");
        QVERIFY(QDir().mkpath(lockedTarget));
        QVERIFY(::chmod(lockedTarget.toLocal8Bit().constData(), 0555) == 0);
        refused = false;
        try { library.exportBackupPackage(lockedTarget); }
        catch (const std::exception &) { refused = true; }
        QVERIFY(refused);
        QVERIFY(!QFileInfo::exists(lockedTarget + "/objects"));
        QVERIFY(::chmod(lockedParent.toLocal8Bit().constData(), 0755) == 0);
        QVERIFY(::chmod(lockedTarget.toLocal8Bit().constData(), 0755) == 0);
    }
    // The export verifies every live object (declared size) and refuses a
    // package whose index is malformed or whose object vanished — never a
    // half package marked COMPLETE.
    void backupExportVerifiesObjectsBeforeTrusting() {
        QTemporaryDir dir;
        put(dir.filePath("a.png"), "bytes-a");
        Library library(dir.filePath("library"));
        const auto asset = library.importFile(dir.filePath("a.png"));
        const auto hash = library.revisions(asset.id).first().hash;
        // Malformed hash in the index: refused before touching the filesystem.
        {
            const QString conn = QStringLiteral("bk_malformed_") + QUuid::createUuid().toString(QUuid::WithoutBraces);
            QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", conn);
            db.setDatabaseName(dir.filePath("library/catalog.sqlite"));
            QVERIFY(db.open());
            {
                QSqlQuery q(db);
                q.prepare("UPDATE revisions SET hash='zz' WHERE asset_id=?");
                q.addBindValue(asset.id);
                QVERIFY(q.exec());
            }
            db.close();
            QSqlDatabase::removeDatabase(conn);
        }
        bool refused = false;
        try { QTemporaryDir t; library.exportBackupPackage(t.path()); }
        catch (const std::exception &) { refused = true; }
        QVERIFY(refused);
        // Restore the hash but remove the object: still refused.
        {
            const QString conn = QStringLiteral("bk_restore_") + QUuid::createUuid().toString(QUuid::WithoutBraces);
            QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", conn);
            db.setDatabaseName(dir.filePath("library/catalog.sqlite"));
            QVERIFY(db.open());
            {
                QSqlQuery q(db);
                q.prepare("UPDATE revisions SET hash=? WHERE asset_id=?");
                q.addBindValue(hash);
                q.addBindValue(asset.id);
                QVERIFY(q.exec());
            }
            db.close();
            QSqlDatabase::removeDatabase(conn);
        }
        QVERIFY(platform::makeMutable(library.objectPath(hash)));
        QVERIFY(QFile::remove(library.objectPath(hash)));
        refused = false;
        try { QTemporaryDir t; library.exportBackupPackage(t.path()); }
        catch (const std::exception &) { refused = true; }
        QVERIFY(refused);
    }
    // Verification rejects a package with an unreadable/unrecognized manifest
    // or a tampered object — each with a concrete reason, never silent.
    void verifyBackupPackageRejectsBadManifestAndTamperedObjects() {
        QTemporaryDir dir;
        QTemporaryDir target;
        put(dir.filePath("a.png"), "bytes-a");
        Library library(dir.filePath("library"));
        QVERIFY(!library.importFile(dir.filePath("a.png")).id.isEmpty());
        const auto package = library.exportBackupPackage(target.path());
        QVERIFY(!package.entries.isEmpty());
        // COMPLETE present but manifest gone.
        QVERIFY(QFile::remove(package.manifestPath));
        bool rejected = false;
        try { library.verifyBackupPackage(target.path()); }
        catch (const std::exception &) { rejected = true; }
        QVERIFY(rejected);
        // Manifest present but not the v1 format.
        put(package.manifestPath, QByteArray("{\"format\": \"something-else\", \"entries\": []}"));
        rejected = false;
        try { library.verifyBackupPackage(target.path()); }
        catch (const std::exception &) { rejected = true; }
        QVERIFY(rejected);
        // Restore the correct manifest, then tamper the packaged object
        // content: the hash check refuses the package.
        const auto &e = package.entries.first();
        QJsonObject manifest;
        manifest.insert(QStringLiteral("format"), QStringLiteral("pentimento-backup-v1"));
        manifest.insert(QStringLiteral("exported"), QStringLiteral("x"));
        manifest.insert(QStringLiteral("entries"), QJsonArray{
            QJsonObject{{"assetId", e.assetId}, {"name", e.name}, {"folder", QString()},
                        {"hash", e.hash}, {"size", static_cast<double>(e.size)},
                        {"revisionId", static_cast<double>(e.revisionId)}}});
        put(package.manifestPath, QJsonDocument(manifest).toJson(QJsonDocument::Indented));
        const auto verified = library.verifyBackupPackage(target.path());
        QCOMPARE(verified.size(), 1);
        const QString objPath = target.filePath("objects/" + e.hash);
        QVERIFY(platform::makeMutable(objPath));
        put(objPath, "tampered");
        rejected = false;
        try { library.verifyBackupPackage(target.path()); }
        catch (const std::exception &) { rejected = true; }
        QVERIFY(rejected);
    }
    // The drill refuses an empty manifest outright, and a package whose
    // manifest lists one asset twice rolls the whole drill back — the target
    // library stays empty (no half-imported drill state).
    void restoreDrillRefusesEmptyManifestAndRollsBackDuplicates() {
        QTemporaryDir dir;
        QTemporaryDir target;
        put(dir.filePath("a.png"), "bytes-a");
        Library library(dir.filePath("library"));
        QVERIFY(!library.importFile(dir.filePath("a.png")).id.isEmpty());
        const auto package = library.exportBackupPackage(target.path());
        QVERIFY(!package.entries.isEmpty());
        // An empty manifest is a complete-but-empty package: refused.
        put(package.manifestPath,
            QByteArray("{\"format\": \"pentimento-backup-v1\", \"exported\": \"x\", \"entries\": []}"));
        {
            Library drill(dir.filePath("drill"));
            bool refused = false;
            try { drill.restoreBackupPackage(target.path()); }
            catch (const std::exception &) { refused = true; }
            QVERIFY(refused);
            QVERIFY(drill.assets().isEmpty());
        }
        // Duplicate asset id in the manifest: the second insert violates the
        // primary key mid-transaction, the drill rolls back, nothing persists.
        const auto &e = package.entries.first();
        const QJsonObject entryJson{
            {"assetId", e.assetId}, {"name", e.name}, {"folder", QString()},
            {"hash", e.hash}, {"size", static_cast<double>(e.size)},
            {"revisionId", static_cast<double>(e.revisionId)}};
        QJsonObject manifest;
        manifest.insert(QStringLiteral("format"), QStringLiteral("pentimento-backup-v1"));
        manifest.insert(QStringLiteral("exported"), QStringLiteral("x"));
        manifest.insert(QStringLiteral("entries"), QJsonArray{entryJson, entryJson});
        put(package.manifestPath, QJsonDocument(manifest).toJson(QJsonDocument::Indented));
        Library drill(dir.filePath("drill2"));
        bool rolledBack = false;
        try { drill.restoreBackupPackage(target.path()); }
        catch (const std::exception &) { rolledBack = true; }
        QVERIFY(rolledBack);
        QVERIFY(drill.assets().isEmpty());
        QVERIFY(drill.revisions(e.assetId).isEmpty());
    }
    // The consistency audit reports unreadable objects as I/O errors (not
    // corruption, not missing) and streams progress percent to the callback.
    void consistencyCheckReportsIoErrorsAndProgress() {
        if (::geteuid() == 0) QSKIP("root bypasses file read permissions");
        QTemporaryDir dir;
        put(dir.filePath("a.png"), "bytes-a");
        Library library(dir.filePath("library"));
        const auto asset = library.importFile(dir.filePath("a.png"));
        const auto hash = library.revisions(asset.id).first().hash;
        int calls = 0;
        int lastPercent = -1;
        const auto report = library.checkConsistency(
            [&](int percent, const QString &current) {
                ++calls;
                lastPercent = percent;
                QVERIFY(!current.isEmpty());
            });
        QCOMPARE(report.ok, 1);
        QVERIFY(report.missing.isEmpty() && report.corrupt.isEmpty() && report.ioErrors.isEmpty());
        // One callback per referenced object, fired before it is inspected
        // (percent counts objects already done).
        QCOMPARE(calls, 1);
        QCOMPARE(lastPercent, 0);
        // An unreadable object is an I/O error: present, right size, unopenable.
        QVERIFY(::chmod(library.objectPath(hash).toLocal8Bit().constData(), 0000) == 0);
        const auto io = library.checkConsistency({});
        QCOMPARE(io.ioErrors.size(), 1);
        QCOMPARE(io.ioErrors.first(), hash);
        QCOMPARE(io.ok, 0);
        QVERIFY(::chmod(library.objectPath(hash).toLocal8Bit().constData(), 0644) == 0);
    }

};

// A QApplication is required: CompareCanvas/PreviewCanvas are widgets
// and cannot be created under a QCoreApplication (canvas/journey).
int main(int argc, char **argv) {
    QApplication app(argc, argv);
    BackupConsistencyTest tc;
    QTEST_SET_MAIN_SOURCE_PATH
    return QTest::qExec(&tc, argc, argv);
}
#include "library_backup_test.moc"
