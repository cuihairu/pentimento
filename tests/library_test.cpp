#include "library.h"
#include "platform_immutable.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QtTest>
#include <sys/stat.h>
#include <unistd.h>

namespace {
void put(const QString &path, const QByteArray &data) {
    QFile file(path); QVERIFY(file.open(QIODevice::WriteOnly)); QCOMPARE(file.write(data), data.size());
}
QByteArray get(const QString &path) { QFile file(path); return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray(); }
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
    void largeContentStreamsThroughObjectStoreWithDedup() {
        QTemporaryDir dir;
        const QString source = dir.filePath("big.psd");
        // 64 MiB + 17 bytes: crosses the 1 MiB streaming windows and leaves a
        // partial tail chunk, so the large-file sample catches boundary bugs.
        const qint64 total = 64LL * 1024 * 1024 + 17;
        {
            QFile file(source);
            QVERIFY(file.open(QIODevice::WriteOnly));
            QByteArray block(1 << 20, Qt::Uninitialized);
            for (qsizetype i = 0; i < block.size(); ++i) block[i] = char(i % 251);
            for (qint64 i = 0; i < total / block.size(); ++i) {
                block[0] = char(i % 251); // vary blocks so content is not uniform
                QCOMPARE(file.write(block), block.size());
            }
            const QByteArray tail(total % block.size(), '\a');
            QCOMPARE(file.write(tail), tail.size());
            QCOMPARE(file.size(), total);
        }
        Library library(dir.filePath("library"));
        const auto asset = library.importFile(source);
        const auto revision = library.revisions(asset.id).first();
        QCOMPARE(revision.size, total);
        // Independent full-content reference hash: the streamed store must agree.
        const auto bytes = get(source);
        const QString expected = QString::fromLatin1(
            QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
        QCOMPARE(revision.hash, expected);
        // One published object, exact size, immutable, and verified on read.
        QCOMPARE(QDir(library.root() + "/objects").entryInfoList(QDir::Files).size(), 1);
        QCOMPARE(QFileInfo(library.objectPath(expected)).size(), total);
        QVERIFY(platform::isImmutable(library.objectPath(expected)));
        QCOMPARE(library.readObject(expected), bytes);
        // The temporary staging area is empty after publishing.
        QCOMPARE(QDir(library.root() + "/objects/.tmp").entryInfoList(QDir::Files).size(), 0);
        // Dedup across assets: same content shares one object, no second copy.
        const auto twin = library.importFile(source);
        QCOMPARE(library.revisions(twin.id).first().hash, expected);
        QCOMPARE(QDir(library.root() + "/objects").entryInfoList(QDir::Files).size(), 1);
        // Recording the unchanged 64 MiB+ working copy is a streaming no-op.
        QVERIFY(!library.capture(asset));
        QCOMPARE(library.revisions(asset.id).size(), 1);
        // A same-size edit is caught by the streaming hash and recorded.
        QFile touch(asset.path);
        QVERIFY(touch.open(QIODevice::ReadWrite));
        QVERIFY(touch.seek(total / 2));
        const char flip = '\xfe';
        QCOMPARE(touch.write(&flip, 1), 1);
        touch.close();
        QVERIFY(library.capture(asset, "touched"));
        QCOMPARE(library.revisions(asset.id).size(), 2);
        QVERIFY(library.revisions(asset.id).first().hash != expected);
        QCOMPARE(QDir(library.root() + "/objects").entryInfoList(QDir::Files).size(), 2);
    }
    void corruptedExistingObjectBlocksDedup() {
        QTemporaryDir dir;
        const auto source = dir.filePath("a.dat");
        put(source, "payload");
        Library library(dir.filePath("library"));
        const auto stored = library.storeObject(source);
        QCOMPARE(stored.hash, QString::fromLatin1(
            QCryptographicHash::hash(QByteArray("payload"), QCryptographicHash::Sha256).toHex()));
        QCOMPARE(stored.size, 7);
        // Corrupt the stored object (a successful tamperer clears the guard first).
        QVERIFY(platform::makeMutable(library.objectPath(stored.hash)));
        put(library.objectPath(stored.hash), "corrupt");
        // Re-storing the same content must verify the existing object and refuse
        // to bless the corrupted copy — never overwrite an existing object.
        QVERIFY_EXCEPTION_THROWN(library.storeObject(source), std::runtime_error);
        QCOMPARE(get(library.objectPath(stored.hash)), QByteArray("corrupt"));
        // The temp staging area stays clean: no half-published leftovers.
        QCOMPARE(QDir(library.root() + "/objects/.tmp").entryInfoList(QDir::Files).size(), 0);
    }
    void failedTempObjectWritePublishesNothing() {
        if (::geteuid() == 0) QSKIP("root bypasses directory write permissions");
        QTemporaryDir dir;
        const auto source = dir.filePath("work.dat");
        put(source, "unsaved work");
        Library library(dir.filePath("library"));
        // Simulate disk exhaustion where the temp object lives: the temporary
        // write fails, the temp is cleaned up, and nothing is published.
        const QString tempDir = library.root() + "/objects/.tmp";
        QVERIFY(::chmod(tempDir.toLocal8Bit().constData(), 0555) == 0);
        QVERIFY_EXCEPTION_THROWN(library.storeObject(source), std::runtime_error);
        QVERIFY(::chmod(tempDir.toLocal8Bit().constData(), 0755) == 0);
        QCOMPARE(QDir(library.root() + "/objects").entryInfoList(QDir::Files).size(), 0);
        QCOMPARE(QDir(tempDir).entryInfoList(QDir::Files).size(), 0);
        // The store recovers: a retry succeeds and reports the content hash.
        const auto stored = library.storeObject(source);
        QCOMPARE(stored.hash, QString::fromLatin1(
            QCryptographicHash::hash(QByteArray("unsaved work"), QCryptographicHash::Sha256).toHex()));
        QCOMPARE(stored.size, 12);
    }
    void manualRecordReturnsQueryableVersionIdAndNote() {
        QTemporaryDir dir;
        const auto source = dir.filePath("art.png");
        put(source, "v1");
        Library library(dir.filePath("library"));
        const auto asset = library.importFile(source);
        // Recording changed content creates a new version with its id + note.
        put(asset.path, "v2");
        const auto first = library.recordVersion(asset, QStringLiteral("定稿"));
        QVERIFY(!first.unchanged);
        QCOMPARE(first.note, QStringLiteral("定稿"));
        const auto stored = library.revisions(asset.id).first();
        QCOMPARE(first.revisionId, stored.id);
        QCOMPARE(stored.note, QStringLiteral("定稿"));
        // Same content again: no duplicate version; the current version id is
        // returned so the user can see what is already protected.
        put(asset.path, "v3");
        const auto second = library.recordVersion(asset, QStringLiteral("v3 note"));
        const auto again = library.recordVersion(asset, QStringLiteral("v3 note"));
        QVERIFY(!second.unchanged);
        QVERIFY(again.unchanged);
        QCOMPARE(again.revisionId, second.revisionId);
        QCOMPARE(again.note, QStringLiteral("v3 note"));
        QCOMPARE(library.revisions(asset.id).size(), 3);
        // An empty note falls back to the documented default.
        put(asset.path, "v4");
        const auto fourth = library.recordVersion(asset, QString());
        QCOMPARE(fourth.note, QStringLiteral("手动快照"));
        QCOMPARE(library.revisions(asset.id).first().note, QStringLiteral("手动快照"));
        // Failures are loud, never silent: recording a missing working copy throws.
        QVERIFY(QFile::remove(asset.path));
        QVERIFY_EXCEPTION_THROWN(library.recordVersion(asset, QStringLiteral("gone")), std::runtime_error);
    }
};
QTEST_GUILESS_MAIN(HistoryTest)
#include "library_test.moc"
