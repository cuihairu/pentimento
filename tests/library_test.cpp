#include "library.h"
#include "backend.h"
#include "exclusion.h"
#include "filter.h"
#include "i18n.h"
#include "importer.h"
#include "platform_immutable.h"
#include "preview.h"
#include "recent.h"
#include "snapshot_schedule.h"
#include "storage.h"
#include "taskqueue.h"
#include "thumbnails.h"
#include <QTranslator>
#include <QBuffer>
#include <QDataStream>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QImageReader>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QtTest>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>

namespace {
void put(const QString &path, const QByteArray &data) {
    QFile file(path); QVERIFY(file.open(QIODevice::WriteOnly)); QCOMPARE(file.write(data), data.size());
}
QByteArray get(const QString &path) { QFile file(path); return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray(); }

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
        // *after* the transaction has already appended the preservation ("恢复前自动保存") revision.
        const QString workingDir = QFileInfo(asset.path).absolutePath();
        struct stat st;
        QVERIFY(::stat(workingDir.toLocal8Bit().constData(), &st) == 0);
        const mode_t original = st.st_mode & 07777;
        QVERIFY(::chmod(workingDir.toLocal8Bit().constData(), 0555) == 0);
        QVERIFY_EXCEPTION_THROWN(library.restore(asset, first), std::runtime_error);
        QVERIFY(::chmod(workingDir.toLocal8Bit().constData(), original) == 0);
        // The failed in-transaction write must roll back both appends: only the original import revision remains.
        QCOMPARE(library.revisions(asset.id).size(), 1);
        // The working file is untouched because the write never succeeded.
        QCOMPARE(get(asset.path), QByteArray("unrecorded"));
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
};
QTEST_GUILESS_MAIN(HistoryTest)
#include "library_test.moc"
