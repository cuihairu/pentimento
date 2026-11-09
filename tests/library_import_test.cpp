// Import and sources: open categories, relocation, copy import, recursive scans, import plans, rescan previews, external index.
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

class ImportSourcesTest : public QObject {
    Q_OBJECT
private slots:
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

    void externalIndexReadsSourcesWithoutCopying() {
        QTemporaryDir work;
        Library library(work.filePath("library"));
        QTemporaryDir source;
        const QByteArray a = QByteArrayLiteral("external-alpha-bytes\n");
        const QByteArray b = QByteArrayLiteral("external-beta-bytes\n");
        const QString aPath = source.filePath("alpha.png");
        const QString bPath = source.filePath("sub/beta.dat");
        QVERIFY(QDir(source.path()).mkpath("sub"));
        put(aPath, a);
        put(bPath, b);
        const qint64 aMtime = QFileInfo(aPath).lastModified().toMSecsSinceEpoch();
        const qint64 bMtime = QFileInfo(bPath).lastModified().toMSecsSinceEpoch();

        const qint64 rootId = library.addExternalRoot(source.path());
        QVERIFY(rootId > 0);
        QCOMPARE(library.indexExternalRoot(rootId), 2);
        // No copy: objects/ holds only the catalog's own bookkeeping — the
        // walk must not import a single byte from the indexed tree.
        QCOMPARE(QDir(QDir(work.filePath("library")).filePath("objects"))
                     .entryList(QDir::Files).size(), 0);
        // No modification: bytes and mtimes of both source files unchanged.
        QCOMPARE(get(aPath), a);
        QCOMPARE(get(bPath), b);
        QCOMPARE(QFileInfo(aPath).size(), qint64(a.size()));
        QCOMPARE(QFileInfo(aPath).lastModified().toMSecsSinceEpoch(), aMtime);
        QCOMPARE(QFileInfo(bPath).lastModified().toMSecsSinceEpoch(), bMtime);
        // The index describes the tree faithfully.
        const auto assets = library.externalAssets(rootId);
        QCOMPARE(assets.size(), 2);
        QCOMPARE(assets.at(0).relPath, QStringLiteral("alpha.png"));
        QCOMPARE(assets.at(0).hash, digestHex(a));
        QCOMPARE(assets.at(0).size, qint64(a.size()));
        QCOMPARE(assets.at(0).mtimeMs, aMtime);
        QVERIFY(assets.at(0).state == Library::ExternalAssetState::Present);
        QCOMPARE(assets.at(1).relPath, QStringLiteral("sub/beta.dat"));
        QCOMPARE(assets.at(1).hash, digestHex(b));
        QVERIFY(assets.at(1).state == Library::ExternalAssetState::Present);
        // Refresh is idempotent: no duplicate rows, count still 2.
        QCOMPARE(library.indexExternalRoot(rootId), 2);
        QCOMPARE(library.externalAssets(rootId).size(), 2);
        QCOMPARE(library.externalRoots().size(), 1);
        QCOMPARE(library.externalRoots().first().path, QDir(source.path()).canonicalPath());
    }
    void externalIndexRefreshKeepsIdentityAndMarksMissing() {
        QTemporaryDir work;
        Library library(work.filePath("library"));
        QTemporaryDir source;
        put(source.filePath("keep.png"), "kept");
        put(source.filePath("gone.txt"), "vanishing");
        const qint64 rootId = library.addExternalRoot(source.path());
        library.indexExternalRoot(rootId);
        // Vanish one file: its row survives (state missing), the other stays.
        QVERIFY(QFile::remove(source.filePath("gone.txt")));
        QCOMPARE(library.indexExternalRoot(rootId), 1);
        auto assets = library.externalAssets(rootId);
        QCOMPARE(assets.size(), 2);
        QCOMPARE(assets.at(0).relPath, QStringLiteral("gone.txt"));
        QVERIFY(assets.at(0).state == Library::ExternalAssetState::Missing);
        QCOMPARE(assets.at(0).hash, digestHex("vanishing")); // last good read
        QCOMPARE(assets.at(1).relPath, QStringLiteral("keep.png"));
        QVERIFY(assets.at(1).state == Library::ExternalAssetState::Present);
        // Identity is the path: renaming keep.png creates a new entry; the
        // old name flips to missing and no row is dropped.
        QVERIFY(QDir(source.path()).rename(QStringLiteral("keep.png"), QStringLiteral("renamed.png")));
        library.indexExternalRoot(rootId);
        assets = library.externalAssets(rootId);
        QCOMPARE(assets.size(), 3);
        QVERIFY(assets.at(0).state == Library::ExternalAssetState::Missing);   // gone.txt
        QVERIFY(assets.at(1).state == Library::ExternalAssetState::Missing);   // keep.png
        QCOMPARE(assets.at(2).relPath, QStringLiteral("renamed.png"));
        QVERIFY(assets.at(2).state == Library::ExternalAssetState::Present);
        QCOMPARE(assets.at(2).hash, digestHex("kept"));
    }
    void externalRootValidationAndRemoval() {
        QTemporaryDir work;
        Library library(work.filePath("library"));
        QTemporaryDir source;
        put(source.filePath("f.png"), "x");
        // A non-directory / nonexistent path is rejected.
        QVERIFY_EXCEPTION_THROWN(library.addExternalRoot(source.filePath("f.png")),
                                 std::runtime_error);
        QVERIFY_EXCEPTION_THROWN(library.addExternalRoot(source.filePath("missing")),
                                 std::runtime_error);
        // Roots inside the library are rejected: the index describes trees
        // the catalog does not own.
        QVERIFY_EXCEPTION_THROWN(library.addExternalRoot(work.filePath("library")),
                                 std::runtime_error);
        QVERIFY_EXCEPTION_THROWN(library.addExternalRoot(work.filePath("library/objects")),
                                 std::runtime_error);
        // The same directory cannot be registered twice.
        const qint64 rootId = library.addExternalRoot(source.path());
        QVERIFY_EXCEPTION_THROWN(library.addExternalRoot(source.path()), std::runtime_error);
        library.indexExternalRoot(rootId);
        // Unknown ids fail loudly on every operation.
        QVERIFY_EXCEPTION_THROWN(library.removeExternalRoot(rootId + 123), std::runtime_error);
        QVERIFY_EXCEPTION_THROWN(library.indexExternalRoot(rootId + 123), std::runtime_error);
        // Removal is catalog-side only; the source tree keeps its file.
        library.removeExternalRoot(rootId);
        QVERIFY(library.externalRoots().isEmpty());
        QVERIFY(library.externalAssets(rootId).isEmpty());
        QVERIFY(QFileInfo::exists(source.filePath("f.png")));
    }

};

// A QApplication is required: CompareCanvas/PreviewCanvas are widgets
// and cannot be created under a QCoreApplication (canvas/journey).
int main(int argc, char **argv) {
    QApplication app(argc, argv);
    ImportSourcesTest tc;
    QTEST_SET_MAIN_SOURCE_PATH
    return QTest::qExec(&tc, argc, argv);
}
#include "library_import_test.moc"
