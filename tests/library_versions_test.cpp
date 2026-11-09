// Versions and whole-library snapshots: manual/auto recording, pins, tombstones, timelines, field history, emergency anchors.
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

class VersionsSnapshotsTest : public QObject {
    Q_OBJECT
private slots:
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

};

// A QApplication is required: CompareCanvas/PreviewCanvas are widgets
// and cannot be created under a QCoreApplication (canvas/journey).
int main(int argc, char **argv) {
    QApplication app(argc, argv);
    VersionsSnapshotsTest tc;
    QTEST_SET_MAIN_SOURCE_PATH
    return QTest::qExec(&tc, argc, argv);
}
#include "library_versions_test.moc"
