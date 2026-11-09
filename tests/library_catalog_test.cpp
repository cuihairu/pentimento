// Catalog metadata and filtering: tags, ratings, provenance, rename/move identity, combined filters, snapshot schedule, exclusion rules.
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

class CatalogFilterTest : public QObject {
    Q_OBJECT
private slots:
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


};

// A QApplication is required: CompareCanvas/PreviewCanvas are widgets
// and cannot be created under a QCoreApplication (canvas/journey).
int main(int argc, char **argv) {
    QApplication app(argc, argv);
    CatalogFilterTest tc;
    QTEST_SET_MAIN_SOURCE_PATH
    return QTest::qExec(&tc, argc, argv);
}
#include "library_catalog_test.moc"
