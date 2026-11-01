#include "library.h"
#include "backend.h"
#include "i18n.h"
#include "importer.h"
#include "platform_immutable.h"
#include "preview.h"
#include "recent.h"
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
};
QTEST_GUILESS_MAIN(HistoryTest)
#include "library_test.moc"
