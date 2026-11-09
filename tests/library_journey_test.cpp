// P-609 end-to-end V1 user journey acceptance walkthrough.
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

class V1JourneyTest : public QObject {
    Q_OBJECT
private slots:
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
            // Content sniffing of extension-less TGA objects is not portable
            // across Qt builds (some qico builds claim the header); the asset's
            // recorded format is what the app decodes with, so pass it here too.
            QVERIFY(left.loadFromData(library.readObject(v1Hash), "TGA"));
            QVERIFY(right.loadFromData(library.readObject(library.revisions(hero.id).first().hash), "TGA"));
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
    // P-204: external directory indexing. The acceptance is that index mode
    // never copies and never modifies the source: the walk reads (stat +
    // streaming hash), records (root, rel_path, size, mtime, hash) rows in
    // the catalog, and refreshes flip vanished files to missing instead of
    // dropping rows. Identity is the path — a rename is a new entry.

};

// A QApplication is required: CompareCanvas/PreviewCanvas are widgets
// and cannot be created under a QCoreApplication (canvas/journey).
int main(int argc, char **argv) {
    QApplication app(argc, argv);
    V1JourneyTest tc;
    QTEST_SET_MAIN_SOURCE_PATH
    return QTest::qExec(&tc, argc, argv);
}
#include "library_journey_test.moc"
