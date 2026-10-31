#include "library.h"
#include "platform_immutable.h"
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QtTest>
#include <cstdio>
#include <sys/stat.h>
#include <unistd.h>

namespace {
void put(const QString &path, const QByteArray &data) {
    QFile file(path); QVERIFY(file.open(QIODevice::WriteOnly)); QCOMPARE(file.write(data), data.size());
}
QByteArray get(const QString &path) { QFile file(path); return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray(); }
// Fix the modification time so the auto-record size+mtime signature is exact.
void setMtime(const QString &path, qint64 ms) {
    QFile file(path); QVERIFY(file.open(QIODevice::ReadWrite));
    QVERIFY(file.setFileTime(QDateTime::fromMSecsSinceEpoch(ms, Qt::UTC), QFileDevice::FileModificationTime));
}
qint64 mtimeOf(const QString &path) { return QFileInfo(path).lastModified().toMSecsSinceEpoch(); }
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
    void autoRecordPolicyHandlesSampleClasses() {
        QTemporaryDir dir;
        const auto source = dir.filePath("sprite.png");
        put(source, "v1");
        Library library(dir.filePath("library"));
        const auto asset = library.importFile(source);
        QCOMPARE(library.revisions(asset.id).size(), 1);

        // Sample: 连续写入 — the file keeps changing, so no two ticks agree and
        // no half-written state is ever recorded.
        for (int i = 0; i < 5; ++i) {
            put(asset.path, QByteArray("writing-") + QByteArray::number(i));
            setMtime(asset.path, 1700000000000LL + i * 1000);
            library.autoRecordTick(asset);
        }
        QCOMPARE(library.revisions(asset.id).size(), 1);

        // Sample: 长暂停（>窗口）后稳定 — recorded once two ticks agree.
        put(asset.path, "final-after-pause");
        setMtime(asset.path, 1700000100000LL);
        QVERIFY(!library.autoRecordTick(asset).recorded); // first observation
        const auto settled = library.autoRecordTick(asset);
        QVERIFY(settled.recorded);
        QCOMPARE(library.revisions(asset.id).size(), 2);
        QCOMPARE(library.revisions(asset.id).first().note, QStringLiteral("自动记录"));
        // Later ticks on the already-recorded state are quiet no-ops.
        QVERIFY(!library.autoRecordTick(asset).recorded);
        QCOMPARE(library.revisions(asset.id).size(), 2);

        // Sample: 临时文件替换保存（写临时+改名）— detected and recorded.
        const QString temp = asset.path + ".tmp-save";
        put(temp, "saved-via-rename");
        setMtime(temp, 1700000200000LL);
        QCOMPARE(::rename(QFile::encodeName(temp).constData(), QFile::encodeName(asset.path).constData()), 0);
        QVERIFY(!library.autoRecordTick(asset).recorded); // observe the new signature
        QVERIFY(library.autoRecordTick(asset).recorded);
        QCOMPARE(library.revisions(asset.id).size(), 3);
        QCOMPARE(library.readObject(library.revisions(asset.id).first().hash), QByteArray("saved-via-rename"));

        // Sample: 同大小写入（mtime 变、size 不变）— caught by the mtime half.
        put(asset.path, "same-len-A");
        setMtime(asset.path, 1700000300000LL);
        library.autoRecordTick(asset);
        QVERIFY(library.autoRecordTick(asset).recorded);
        QCOMPARE(library.revisions(asset.id).size(), 4);
        put(asset.path, "same-len-B");
        setMtime(asset.path, 1700000400000LL);
        library.autoRecordTick(asset);
        QVERIFY(library.autoRecordTick(asset).recorded);
        QCOMPARE(library.revisions(asset.id).size(), 5);
        QCOMPARE(library.readObject(library.revisions(asset.id).first().hash), QByteArray("same-len-B"));

        // Same content with only mtime bumped (touch): unchanged, no duplicate.
        setMtime(asset.path, 1700000500000LL);
        library.autoRecordTick(asset);
        const auto touched = library.autoRecordTick(asset);
        QVERIFY(touched.unchanged);
        QVERIFY(!touched.recorded);
        QCOMPARE(library.revisions(asset.id).size(), 5);
    }
    void stableRecordRetriesWhenFileMovesMidRead() {
        QTemporaryDir dir;
        const auto source = dir.filePath("doc.psd");
        put(source, "v1");
        Library library(dir.filePath("library"));
        const auto asset = library.importFile(source);
        // A write lands between the stability observation and the read: the
        // observed size+mtime no longer matches, so the read was of a moving
        // target. Nothing is recorded and retry is reported.
        const qint64 staleSize = QFileInfo(asset.path).size();
        const qint64 staleMtime = mtimeOf(asset.path);
        put(asset.path, "changed-mid-read");
        setMtime(asset.path, staleMtime + 60000);
        const auto torn = library.recordStableVersion(asset, staleSize, staleMtime);
        QVERIFY(torn.retry);
        QVERIFY(!torn.recorded);
        QCOMPARE(library.revisions(asset.id).size(), 1);
        // No torn object leaked into the store (only the import's object).
        QCOMPARE(QDir(library.root() + "/objects").entryInfoList(QDir::Files).size(), 1);
        QCOMPARE(QDir(library.root() + "/objects/.tmp").entryInfoList(QDir::Files).size(), 0);
        // Once the file settles, a retry with the matching signature records it.
        const auto info = QFileInfo(asset.path);
        const auto settled = library.recordStableVersion(asset, info.size(), info.lastModified().toMSecsSinceEpoch());
        QVERIFY(settled.recorded);
        QCOMPARE(library.revisions(asset.id).size(), 2);
        QCOMPARE(library.readObject(library.revisions(asset.id).first().hash), QByteArray("changed-mid-read"));
    }
    void scheduleStateIsVisibleAndPauseStopsSnapshots() {
        QTemporaryDir dir;
        const auto source = dir.filePath("a.png");
        put(source, "v1");
        Library library(dir.filePath("library"));
        library.importFile(source);
        // Default schedule is OFF and visible.
        QCOMPARE(library.scheduleState().enabled, false);
        QCOMPARE(library.scheduleState().paused, false);
        // Invalid periods are rejected loudly.
        QVERIFY_EXCEPTION_THROWN(library.enableSchedule(7), std::runtime_error);
        // Enable at 15 minutes; next time = enabling moment + 15 min.
        library.enableSchedule(15);
        auto state = library.scheduleState();
        QVERIFY(state.enabled);
        QCOMPARE(state.periodMinutes, 15);
        QVERIFY(!state.paused);
        const qint64 next = state.nextUtcMs;
        QVERIFY(next > QDateTime::currentMSecsSinceEpoch());
        // Not due yet: no snapshot.
        QCOMPARE(library.pumpSchedule(next - 1), 0);
        QCOMPARE(library.snapshots().size(), 0);
        // Due: exactly one snapshot, next window advanced by one period.
        const qint64 first = library.pumpSchedule(next);
        QVERIFY(first > 0);
        QCOMPARE(library.snapshots().size(), 1);
        QCOMPARE(library.scheduleState().nextUtcMs, next + 15 * 60000LL);
        // Several missed periods collapse to a single catch-up snapshot.
        const qint64 late = next + 15 * 60000LL * 4;
        const qint64 catchUp = library.pumpSchedule(late);
        QVERIFY(catchUp > 0);
        QVERIFY(catchUp != first);
        QCOMPARE(library.snapshots().size(), 2);
        QCOMPARE(library.scheduleState().nextUtcMs, late + 15 * 60000LL);
        // Pause records the reason and stops automatic snapshots.
        library.pauseSchedule(QStringLiteral("user"));
        state = library.scheduleState();
        QVERIFY(state.paused);
        QCOMPARE(state.pauseReason, QStringLiteral("user"));
        const qint64 pausedAt = library.scheduleState().nextUtcMs;
        QCOMPARE(library.pumpSchedule(pausedAt + 15 * 60000LL), 0);
        QCOMPARE(library.pumpSchedule(pausedAt + 30 * 60000LL), 0);
        QCOMPARE(library.snapshots().size(), 2); // no snapshots while paused
        // Resume restarts a fresh interval from the resume moment (no surprise
        // snapshot right after a pause); the next fire is one period away.
        library.resumeSchedule();
        state = library.scheduleState();
        QVERIFY(!state.paused);
        QVERIFY(state.pauseReason.isEmpty());
        const qint64 resumedNext = state.nextUtcMs;
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        QVERIFY(resumedNext > now);
        QVERIFY(resumedNext <= now + 15 * 60000LL + 5000);
        QCOMPARE(library.pumpSchedule(resumedNext - 1), 0);
        QVERIFY(library.pumpSchedule(resumedNext) > 0);
        QCOMPARE(library.snapshots().size(), 3);
        // Disabling clears the state back to the default.
        library.disableSchedule();
        state = library.scheduleState();
        QCOMPARE(state.enabled, false);
        QCOMPARE(state.periodMinutes, 0);
        QCOMPARE(library.pumpSchedule(library.scheduleState().nextUtcMs + 15 * 60000LL), 0);
    }
    void exclusionRulesKeepAssetsOutOfSnapshots() {
        QTemporaryDir dir;
        const auto png = dir.filePath("a.png");
        const auto psd = dir.filePath("b.psd");
        const auto txt = dir.filePath("readme.txt");
        put(png, "png1"); put(psd, "psd1"); put(txt, "readme1");
        Library library(dir.filePath("library"));
        const auto a = library.importFile(png);
        const auto b = library.importFile(psd);
        const auto c = library.importFile(txt);
        // No rules yet: every asset is a snapshot anchor.
        const qint64 first = library.snapshotAll(QStringLiteral("基线"));
        QVERIFY(first > 0);
        QCOMPARE(library.snapshots().first().assets, 3);
        // An extension rule excludes a whole kind; the reason is recorded and
        // the excluded asset is NOT deleted (it keeps its history and can still
        // be recorded manually).
        const qint64 psdRule = library.addExclusionRule(QStringLiteral("extension"),
                                                        QStringLiteral(".psd"),
                                                        QStringLiteral("不追踪 PSD 源文件"));
        QVERIFY(psdRule > 0);
        QString reason;
        QVERIFY(library.isExcluded(b, &reason));
        QCOMPARE(reason, QStringLiteral("不追踪 PSD 源文件"));
        QVERIFY(!library.isExcluded(a));
        QVERIFY(!library.isExcluded(c));
        const qint64 second = library.snapshotAll(QStringLiteral("第二次"));
        QVERIFY(second > 0);
        QCOMPARE(library.snapshots().first().assets, 2);
        QCOMPARE(library.snapshotExclusionReason(second, b.id),
                 QStringLiteral("不追踪 PSD 源文件"));
        QCOMPARE(library.assets().size(), 3); // excluded != deleted
        QVERIFY(library.recordVersion(b, QStringLiteral("仍可手动记录")).revisionId > 0);
        // A glob rule also matches; only the first matching rule's reason counts.
        const qint64 txtRule = library.addExclusionRule(QStringLiteral("glob"),
                                                        QStringLiteral("*.txt"),
                                                        QStringLiteral("跳过文本"));
        QVERIFY(txtRule > 0);
        QCOMPARE(library.exclusionRules().size(), 2);
        const qint64 third = library.snapshotAll(QStringLiteral("第三次"));
        QVERIFY(third > 0);
        QCOMPARE(library.snapshots().first().assets, 1);
        QCOMPARE(library.snapshotExclusionReason(third, c.id), QStringLiteral("跳过文本"));
        // Removal restores the asset to snapshots again.
        library.removeExclusionRule(txtRule);
        QCOMPARE(library.exclusionRules().size(), 1);
        const qint64 fourth = library.snapshotAll(QStringLiteral("第四次"));
        QVERIFY(fourth > 0);
        QCOMPARE(library.snapshots().first().assets, 2);
        QCOMPARE(library.snapshotExclusionReason(fourth, c.id), QString());
        // Invalid rules are rejected loudly.
        QVERIFY_EXCEPTION_THROWN(library.addExclusionRule(QStringLiteral("bogus"), QStringLiteral("x"), QString()), std::runtime_error);
        QVERIFY_EXCEPTION_THROWN(library.addExclusionRule(QStringLiteral("extension"), QString(), QString()), std::runtime_error);
    }
    void pinnedVersionSurvivesRetentionCleanup() {
        QTemporaryDir dir;
        const auto source = dir.filePath("a.png");
        put(source, "v1");
        const QString libraryRoot = dir.filePath("library");
        qint64 rev1 = 0, rev2 = 0, rev3 = 0;
        {
            Library library(libraryRoot);
            const auto asset = library.importFile(source);
            put(asset.path, "v2");
            QVERIFY(library.recordVersion(asset, QStringLiteral("第二版")).revisionId > 0);
            put(asset.path, "v3");
            QVERIFY(library.recordVersion(asset, QStringLiteral("第三版")).revisionId > 0);
            const auto history = library.revisions(asset.id);
            QCOMPARE(history.size(), 3);
            rev3 = history.at(0).id; rev2 = history.at(1).id; rev1 = history.at(2).id;
            // By default the newest revision is not a candidate; the rest are.
            QCOMPARE(library.retentionCandidates(asset.id), (QVector<qint64>{rev2, rev1}));
            // Pin the oldest: it disappears from the candidates and carries its reason.
            library.pinRevision(asset.id, rev1, QStringLiteral("客户定稿"));
            auto after = library.revisions(asset.id);
            QCOMPARE(after.at(2).pinned, true);
            QCOMPARE(after.at(2).pinReason, QStringLiteral("客户定稿"));
            QCOMPARE(library.retentionCandidates(asset.id), (QVector<qint64>{rev2}));
            // Pin everything cleanable: nothing may be removed.
            library.pinRevision(asset.id, rev2, QStringLiteral("过审素材"));
            QVERIFY(library.retentionCandidates(asset.id).isEmpty());
            // Unpinning restores candidacy and does NOT delete history.
            library.unpinRevision(asset.id, rev2);
            after = library.revisions(asset.id);
            QCOMPARE(after.size(), 3); // history kept
            QCOMPARE(after.at(1).pinned, false);
            QVERIFY(after.at(1).pinReason.isEmpty());
            QCOMPARE(library.retentionCandidates(asset.id), (QVector<qint64>{rev2}));
            // A revision of another asset (or an unknown id) cannot be pinned.
            QVERIFY_EXCEPTION_THROWN(library.pinRevision(QStringLiteral("no-such-asset"), rev1, QString()), std::runtime_error);
            QVERIFY_EXCEPTION_THROWN(library.pinRevision(asset.id, rev1 + 999, QString()), std::runtime_error);
        }
        // Pinning survives reopen.
        Library reopened(libraryRoot);
        const auto asset = reopened.assets().first();
        const auto history = reopened.revisions(asset.id);
        QCOMPARE(history.at(2).pinned, true);
        QCOMPARE(history.at(2).pinReason, QStringLiteral("客户定稿"));
        QCOMPARE(history.at(1).pinned, false);
        QCOMPARE(reopened.retentionCandidates(asset.id), (QVector<qint64>{rev2}));
    }
    void timelineOrderAndMarksAreAccurate() {
        QTemporaryDir dir;
        const auto source = dir.filePath("a.png");
        put(source, "v1");
        const QString libraryRoot = dir.filePath("library");
        Library library(libraryRoot);
        const auto asset = library.importFile(source);
        put(asset.path, "v2");
        const auto rev2 = library.recordVersion(asset, QStringLiteral("第二版")).revisionId;
        put(asset.path, "v3");
        const auto rev3 = library.recordVersion(asset, QStringLiteral("第三版")).revisionId;
        // Timeline: newest first by deterministic id, exactly one current mark.
        auto tl = library.assetTimeline(asset.id);
        QCOMPARE(tl.entries.size(), 3);
        QVERIFY(tl.entries.at(0).revisionId > tl.entries.at(1).revisionId);
        QVERIFY(tl.entries.at(1).revisionId > tl.entries.at(2).revisionId);
        QCOMPARE(tl.entries.at(0).revisionId, rev3);
        QCOMPARE(tl.entries.at(0).current, true);
        QCOMPARE(tl.entries.at(1).current, false);
        QCOMPARE(tl.entries.at(2).current, false);
        QCOMPARE(tl.unrecorded, false);
        QVERIFY(!tl.entries.at(0).restore);
        QVERIFY(!tl.entries.at(0).corrupt);
        // An unsaved working-copy edit: nothing is current, unrecorded is true.
        put(asset.path, "unrecorded-edit");
        tl = library.assetTimeline(asset.id);
        QCOMPARE(tl.unrecorded, true);
        for (const auto &entry : tl.entries) QCOMPARE(entry.current, false);
        // Restoring the older revision: pre-save + restored append; the new
        // record is marked restore AND is current (its content IS the working
        // copy). The duplicated older content is NOT marked current.
        library.restore(asset, rev2);
        tl = library.assetTimeline(asset.id);
        QCOMPARE(tl.entries.size(), 5);
        QCOMPARE(tl.unrecorded, false);
        QCOMPARE(tl.entries.at(0).restore, true);
        QCOMPARE(tl.entries.at(0).current, true);
        QCOMPARE(tl.entries.at(0).note, QStringLiteral("恢复历史版本 #%1").arg(rev2));
        QCOMPARE(tl.entries.at(1).note, QStringLiteral("恢复前自动保存"));
        QCOMPARE(tl.entries.at(1).restore, false);
        int currentCount = 0;
        for (const auto &entry : tl.entries) if (entry.current) ++currentCount;
        QCOMPARE(currentCount, 1);
        // Pin shows on the timeline.
        const qint64 rev1 = tl.entries.at(4).revisionId;
        library.pinRevision(asset.id, rev1, QStringLiteral("客户定稿"));
        tl = library.assetTimeline(asset.id);
        QCOMPARE(tl.entries.at(4).pinned, true);
        QCOMPARE(tl.entries.at(4).pinReason, QStringLiteral("客户定稿"));
        // A corrupted object is marked corrupt, not hidden (others stay fine).
        QVERIFY(platform::makeMutable(library.objectPath(
            library.revisions(asset.id).at(4).hash)));
        put(library.objectPath(library.revisions(asset.id).at(4).hash), "corrupt");
        tl = library.assetTimeline(asset.id);
        QCOMPARE(tl.entries.at(4).corrupt, true);
        QCOMPARE(tl.entries.at(3).corrupt, false);
        QCOMPARE(tl.entries.at(0).corrupt, false);
        // Clock rollback / same-second versions: timestamps are display-only;
        // order still follows the deterministic revision id.
        {
            QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"),
                                                        QStringLiteral("timeline-test"));
            db.setDatabaseName(libraryRoot + QStringLiteral("/catalog.sqlite"));
            QVERIFY(db.open());
            {
                QSqlQuery q(db);
                QVERIFY(q.exec("UPDATE revisions SET created='2020-01-01T00:00:00.000'"));
            }
            db.close();
            db = QSqlDatabase();
            QSqlDatabase::removeDatabase(QStringLiteral("timeline-test"));
        }
        tl = library.assetTimeline(asset.id);
        QCOMPARE(tl.entries.size(), 5);
        for (int i = 1; i < tl.entries.size(); ++i)
            QVERIFY(tl.entries.at(i - 1).revisionId > tl.entries.at(i).revisionId);
        QCOMPARE(tl.entries.at(0).current, true);
        // Missing working copy: no unrecorded flag, no current mark.
        QFile::remove(asset.path);
        tl = library.assetTimeline(asset.id);
        QCOMPARE(tl.unrecorded, false);
        for (const auto &entry : tl.entries) QCOMPARE(entry.current, false);
    }
    void metadataHistoryRecordsPerFieldChanges() {
        QTemporaryDir dir;
        const auto source = dir.filePath("a.png");
        put(source, "v1");
        const QString libraryRoot = dir.filePath("library");
        qint64 firstId = 0, metadataId = 0;
        {
            Library library(libraryRoot);
            const auto asset = library.importFile(source);
            firstId = library.revisions(asset.id).first().id;
            // Import records a baseline (old value empty) for non-empty fields.
            auto current = library.metadata(asset.id);
            QCOMPARE(current.name, QStringLiteral("a.png"));
            QCOMPARE(current.rating, -1); // no rating, distinct from 0 stars
            QCOMPARE(current.source, QFileInfo(source).absoluteFilePath());
            QCOMPARE(library.metadataHistory(asset.id).size(), 2);
            // The content-hash object count before a metadata-only change.
            const int objectsBefore = QDir(libraryRoot + QStringLiteral("/objects"))
                .entryInfoList(QDir::Files | QDir::NoDotAndDotDot).size();
            // Change every field at once.
            Metadata next;
            next.name = QStringLiteral("英雄.png");
            next.tags = {QStringLiteral("角色"), QStringLiteral("立绘")};
            next.rating = 5;
            next.note = QStringLiteral("定稿");
            next.source = QStringLiteral("https://example.com/a.png");
            const auto result = library.updateMetadata(asset.id, next, QStringLiteral("补齐元数据"));
            QVERIFY(!result.unchanged);
            metadataId = result.revisionId;
            QVERIFY(metadataId > firstId);
            QCOMPARE(result.note, QStringLiteral("补齐元数据"));
            // Current metadata reflects the update.
            current = library.metadata(asset.id);
            QCOMPARE(current.name, QStringLiteral("英雄.png"));
            QCOMPARE(current.tags, (QStringList{QStringLiteral("角色"), QStringLiteral("立绘")}));
            QCOMPARE(current.rating, 5);
            QCOMPARE(current.note, QStringLiteral("定稿"));
            QCOMPARE(current.source, QStringLiteral("https://example.com/a.png"));
            // A metadata-only revision reuses the newest content hash and does
            // NOT create a new content object (contentHash unchanged).
            const auto history = library.revisions(asset.id);
            QCOMPARE(history.size(), 2);
            QCOMPARE(history.at(0).id, metadataId);
            QCOMPARE(history.at(0).hash, history.at(1).hash);
            QCOMPARE(history.at(0).note, QStringLiteral("补齐元数据"));
            const int objectsAfter = QDir(libraryRoot + QStringLiteral("/objects"))
                .entryInfoList(QDir::Files | QDir::NoDotAndDotDot).size();
            QCOMPARE(objectsAfter, objectsBefore);
            // History is read-only viewable, newest revision first; the 5 field
            // rows of the update precede the 2 import baseline rows. Within one
            // revision the field order is not contractual — compare as a set.
            const auto changes = library.metadataHistory(asset.id);
            QCOMPARE(changes.size(), 7);
            QCOMPARE(changes.at(0).revisionId, metadataId);
            QCOMPARE(changes.at(4).revisionId, metadataId);
            QCOMPARE(changes.at(5).revisionId, firstId);
            QCOMPARE(changes.at(6).revisionId, firstId);
            QStringList fields;
            for (int i = 0; i < 5; ++i) fields << changes.at(i).field;
            fields.sort();
            QStringList expectedFields{QStringLiteral("name"), QStringLiteral("tags"),
                                       QStringLiteral("rating"), QStringLiteral("note"),
                                       QStringLiteral("source")};
            expectedFields.sort();
            QCOMPARE(fields, expectedFields);
            // The newest name change (revision metadataId) reads a.png -> 英雄.png.
            QString oldName, newName;
            for (const auto &change : changes) {
                if (change.field == QStringLiteral("name")) {
                    oldName = change.oldValue; newName = change.newValue; break;
                }
            }
            QCOMPARE(oldName, QStringLiteral("a.png"));
            QCOMPARE(newName, QStringLiteral("英雄.png"));
            // The metadata revision also shows on the asset timeline (P-308).
            const auto timeline = library.assetTimeline(asset.id);
            QCOMPARE(timeline.entries.size(), 2);
            QCOMPARE(timeline.entries.at(0).note, QStringLiteral("补齐元数据"));
            QCOMPARE(timeline.entries.at(0).current, true);
            // Re-applying identical metadata changes nothing (no new revision).
            const auto again = library.updateMetadata(asset.id, next, QStringLiteral("重复"));
            QVERIFY(again.unchanged);
            QCOMPARE(again.revisionId, metadataId);
            QCOMPARE(library.revisions(asset.id).size(), 2);
            QCOMPARE(library.metadataHistory(asset.id).size(), 7);
        }
        // Metadata and its history survive reopen.
        Library reopened(libraryRoot);
        const auto asset = reopened.assets().first();
        QCOMPARE(asset.name, QStringLiteral("英雄.png"));
        QCOMPARE(asset.rating, 5);
        QCOMPARE(reopened.metadata(asset.id).tags, (QStringList{QStringLiteral("角色"), QStringLiteral("立绘")}));
        QCOMPARE(reopened.metadataHistory(asset.id).size(), 7);
        QCOMPARE(reopened.revisions(asset.id).first().id, metadataId);
    }
    void deleteKeepsHistoryFindableAndMissingIsDistinct() {
        QTemporaryDir dir;
        const auto source = dir.filePath("a.png");
        put(source, "v1");
        Library library(dir.filePath("library"));
        const auto asset = library.importFile(source);
        put(asset.path, "v2");
        const auto rev2 = library.recordVersion(asset, QStringLiteral("第二版")).revisionId;
        QCOMPARE(library.assetState(asset.id), AssetState::Present);
        const int objectsBefore = QDir(library.root() + QStringLiteral("/objects"))
            .entryInfoList(QDir::Files | QDir::NoDotAndDotDot).size();
        // In-app confirmed delete: tombstone revision, history retained, working
        // copy removed, asset still findable (not erased from the index).
        library.deleteAsset(asset, QStringLiteral("不再需要"));
        QCOMPARE(library.assetState(asset.id), AssetState::Tombstone);
        QVERIFY(!QFile::exists(asset.path));
        const auto history = library.revisions(asset.id);
        QCOMPARE(history.size(), 3); // v1, v2, tombstone — none erased
        QCOMPARE(history.first().tombstone, true);
        QCOMPARE(history.first().hash, history.at(1).hash); // reused last hash
        QCOMPARE(history.first().note, QStringLiteral("不再需要"));
        const auto all = library.assets();
        QCOMPARE(all.size(), 1); // still findable
        QCOMPARE(all.first().deleted, true);
        const int objectsAfter = QDir(library.root() + QStringLiteral("/objects"))
            .entryInfoList(QDir::Files | QDir::NoDotAndDotDot).size();
        QCOMPARE(objectsAfter, objectsBefore); // tombstone adds no content object
        // History query still finds the original content, intact and restorable.
        QCOMPARE(library.readObject(history.at(1).hash), QByteArray("v2"));
        library.restore(asset, rev2);
        QCOMPARE(library.assetState(asset.id), AssetState::Present);
        QCOMPARE(get(asset.path), QByteArray("v2"));
        QCOMPARE(library.assets().first().deleted, false);
        // Re-deleting after recovery works; deleting twice is rejected loudly.
        library.deleteAsset(asset);
        QVERIFY_EXCEPTION_THROWN(library.deleteAsset(asset), std::runtime_error);
        // Recovery restores the asset to Present, then a manual (non-app)
        // working-copy removal is a Missing state — NOT a user deletion, so no
        // tombstone revision is written.
        library.restore(asset, rev2);
        QCOMPARE(library.assetState(asset.id), AssetState::Present);
        const qint64 beforeMissing = library.revisions(asset.id).first().id;
        QVERIFY(QFile::remove(asset.path));
        QCOMPARE(library.assetState(asset.id), AssetState::Missing);
        QCOMPARE(library.revisions(asset.id).first().id, beforeMissing);
        QCOMPARE(library.revisions(asset.id).first().tombstone, false); // no new record
    }
    void consistentSnapshotPublishesCompleteManifest() {
        QTemporaryDir dir;
        const auto png = dir.filePath("a.png");
        const auto psd = dir.filePath("b.psd");
        const auto txt = dir.filePath("readme.txt");
        put(png, "png-v1"); put(psd, "psd-v1"); put(txt, "readme-v1");
        Library library(dir.filePath("library"));
        const auto a = library.importFile(png);
        const auto b = library.importFile(psd);
        const auto c = library.importFile(txt);
        put(a.path, "png-v2");
        QVERIFY(library.capture(a, QStringLiteral("第二版")));
        put(a.path, "png-unrecorded"); // not recorded; snapshot preserves it in history
        const auto byAsset = [&](const QVector<SnapshotEntry> &manifest, const QString &id) -> const SnapshotEntry * {
            for (const auto &entry : manifest) if (entry.assetId == id) return &entry;
            return nullptr;
        };
        // Snapshot 1: three present assets, each anchored to a completed
        // revision at or before the sequence point (one consistent manifest).
        const qint64 snap1 = library.snapshotAll(QStringLiteral("一致快照"));
        QVERIFY(snap1 > 0);
        const auto first = library.snapshots().first();
        QCOMPARE(first.id, snap1);
        QCOMPARE(first.assets, 3);
        QVERIFY(first.boundary > 0);
        auto manifest = library.snapshotManifest(snap1);
        QCOMPARE(manifest.size(), 3);
        for (const auto &entry : manifest) {
            QCOMPARE(entry.state, QStringLiteral("present"));
            QVERIFY(entry.revisionId <= first.boundary); // sequence-point consistency
            QVERIFY(!entry.path.isEmpty());
            // Content reference matches a real revision of the same asset, and
            // the object verifies (manifest == object store).
            bool found = false;
            for (const auto &rev : library.revisions(entry.assetId))
                if (rev.id == entry.revisionId) { QCOMPARE(rev.hash, entry.contentHash); found = true; }
            QVERIFY(found);
            QCOMPARE(library.readObject(entry.contentHash).size() > 0, true);
        }
        const auto *aEntry = byAsset(manifest, a.id);
        QVERIFY(aEntry);
        // The conservative anchor skips the newest revision that mirrors current
        // content: a is anchored to its last CLEAN revision (png-v2), while the
        // unrecorded png-unrecorded is preserved as a later history revision.
        QString aAnchorHash;
        for (const auto &rev : library.revisions(a.id))
            if (rev.id == aEntry->revisionId) aAnchorHash = rev.hash;
        QCOMPARE(library.readObject(aAnchorHash), QByteArray("png-v2"));
        // The unrecorded png-unrecorded is preserved as a history revision (the
        // emergency append), while the anchor stays on the clean png-v2.
        QCOMPARE(library.readObject(library.revisions(a.id).first().hash), QByteArray("png-unrecorded"));
        // The boundary is a fixed point: a revision recorded after the snapshot
        // has a strictly greater id.
        put(a.path, "png-v3");
        QVERIFY(library.capture(a, QStringLiteral("第三版")));
        QVERIFY(library.revisions(a.id).first().id > first.boundary);
        // Snapshot 2: a missing working copy (no tombstone) is state=missing.
        QVERIFY(QFile::remove(c.path));
        const qint64 snap2 = library.snapshotAll(QStringLiteral("缺失快照"));
        QVERIFY(snap2 > 0);
        manifest = library.snapshotManifest(snap2);
        QCOMPARE(manifest.size(), 3);
        const auto *cEntry = byAsset(manifest, c.id);
        QVERIFY(cEntry);
        QCOMPARE(cEntry->state, QStringLiteral("missing"));
        QVERIFY(cEntry->revisionId <= library.snapshots().first().boundary);
        // Snapshot 3: an in-app deleted asset is state=tombstone.
        library.deleteAsset(b);
        const qint64 snap3 = library.snapshotAll(QStringLiteral("墓碑快照"));
        QVERIFY(snap3 > 0);
        manifest = library.snapshotManifest(snap3);
        const auto *bEntry = byAsset(manifest, b.id);
        QVERIFY(bEntry);
        QCOMPARE(bEntry->state, QStringLiteral("tombstone"));
        // Snapshot 4: an excluded asset leaves the manifest and is recorded
        // separately (reason), while present/missing assets remain.
        library.addExclusionRule(QStringLiteral("extension"), QStringLiteral(".psd"),
                                 QStringLiteral("不追踪 PSD"));
        const qint64 snap4 = library.snapshotAll(QStringLiteral("排除快照"));
        QVERIFY(snap4 > 0);
        manifest = library.snapshotManifest(snap4);
        QCOMPARE(manifest.size(), 2);
        QVERIFY(!byAsset(manifest, b.id));
        QCOMPARE(library.snapshotExclusionReason(snap4, b.id), QStringLiteral("不追踪 PSD"));
    }
    void timePointBrowseShowsSnapshotStateNotCurrentIndex() {
        QTemporaryDir dir;
        const auto source = dir.filePath("a.png");
        put(source, "v1");
        Library library(dir.filePath("library"));
        const auto a = library.importFile(source);
        const QString relative = QDir(library.root()).relativeFilePath(a.path);
        QVERIFY(relative.endsWith(QStringLiteral("/a.png")));
        // Snapshot 1: the asset is present at its then-path.
        const qint64 snap1 = library.snapshotAll(QStringLiteral("时间点一"));
        QVERIFY(snap1 > 0);
        const auto s1 = library.snapshots().first();
        auto manifest = library.snapshotManifest(snap1);
        QCOMPARE(manifest.size(), 1);
        QCOMPARE(manifest.first().assetId, a.id);
        QCOMPARE(manifest.first().path, relative); // root-relative as of t1
        QCOMPARE(manifest.first().state, QStringLiteral("present"));
        QVERIFY(manifest.first().revisionId <= s1.boundary);
        // Later: the asset is edited, then in-app deleted (tombstone) after the
        // snapshot. The current index now shows a deleted, file-less asset.
        put(a.path, "v2");
        QVERIFY(library.capture(a, QStringLiteral("第二版")));
        const qint64 tombstoneId = library.revisions(a.id).first().id;
        library.deleteAsset(a, QStringLiteral("清理"));
        QCOMPARE(library.assetState(a.id), AssetState::Tombstone);
        QVERIFY(!QFile::exists(a.path));
        // The browse surface must read the SNAPSHOT manifest, never the current
        // index: at t1 the asset was present at its then-path, even though today
        // it is tombstoned and its working copy is gone. This is the "历史目录 vs
        // 当前目录差异正确" rule — mixing the current index would wrongly show
        // today's path/state for the historical time point.
        manifest = library.snapshotManifest(snap1);
        QCOMPARE(manifest.size(), 1);
        QCOMPARE(manifest.first().state, QStringLiteral("present"));
        QCOMPARE(manifest.first().path, relative);
        // The tombstone happened after the sequence point, so it is NOT part of
        // snapshot 1's manifest (boundary is a fixed point, not re-read later).
        QVERIFY(tombstoneId > s1.boundary);
        // A later snapshot DOES reflect the tombstone state at its own time.
        const qint64 snap2 = library.snapshotAll(QStringLiteral("时间点二"));
        QVERIFY(snap2 > 0);
        manifest = library.snapshotManifest(snap2);
        QCOMPARE(manifest.size(), 1);
        QCOMPARE(manifest.first().state, QStringLiteral("tombstone"));
        QCOMPARE(manifest.first().path, relative);
        // Exclusion: an excluded asset is listed separately (with its then-path
        // and reason) and stays out of the manifest, so the browse surface can
        // show 排除状态 honestly rather than silently omitting it.
        library.addExclusionRule(QStringLiteral("extension"), QStringLiteral(".png"),
                                 QStringLiteral("不追踪 PNG"));
        const qint64 snap3 = library.snapshotAll(QStringLiteral("时间点三"));
        QVERIFY(snap3 > 0);
        const auto exclusions = library.snapshotExclusions(snap3);
        QCOMPARE(exclusions.size(), 1);
        QCOMPARE(exclusions.first().assetId, a.id);
        QCOMPARE(exclusions.first().path, relative);
        QCOMPARE(exclusions.first().reason, QStringLiteral("不追踪 PNG"));
        manifest = library.snapshotManifest(snap3);
        QCOMPARE(manifest.size(), 0); // excluded asset is not a manifest anchor
        // A snapshot that excluded nothing returns an empty exclusion listing.
        QCOMPARE(library.snapshotExclusions(snap1).size(), 0);
    }
    void restorePreviewListsOpsWithoutWriting() {
        QTemporaryDir dir;
        const auto aSrc = dir.filePath("a.png");
        const auto bSrc = dir.filePath("b.png");
        const auto cSrc = dir.filePath("c.png");
        put(aSrc, "a-v1"); put(bSrc, "b-v1"); put(cSrc, "c-v1");
        Library library(dir.filePath("library"));
        const auto a = library.importFile(aSrc);
        const auto b = library.importFile(bSrc);
        const auto c = library.importFile(cSrc);
        const qint64 snap = library.snapshotAll(QStringLiteral("预演锚点"));
        QVERIFY(snap > 0);
        // Divergence: a edited to a new version; c's working copy removed.
        put(a.path, "a-v2");
        QVERIFY(library.capture(a, QStringLiteral("第二版")));
        QVERIFY(QFile::remove(c.path));
        const auto byId = [](const RestorePlan &plan, const QString &id) -> const RestoreOp * {
            for (const auto &op : plan.ops) if (op.assetId == id) return &op;
            return nullptr;
        };
        // The preview lists the full operation set and writes nothing.
        const auto plan = library.planRestore(snap);
        QCOMPARE(plan.ops.size(), 3);
        const auto *aOp = byId(plan, a.id);
        const auto *bOp = byId(plan, b.id);
        const auto *cOp = byId(plan, c.id);
        QVERIFY(aOp && bOp && cOp);
        QCOMPARE(aOp->kind, QStringLiteral("replace")); // exists, differs from anchor
        QCOMPARE(bOp->kind, QStringLiteral("skip"));    // already at anchor content
        QCOMPARE(cOp->kind, QStringLiteral("add"));     // target absent, will create
        QVERIFY(aOp->size > 0);
        QCOMPARE(cOp->size, qint64(4)); // "c-v1"
        QCOMPARE(bOp->size, qint64(0));
        QCOMPARE(plan.bytesToWrite, aOp->size + cOp->size); // space needed
        // No file was written by the preview: the working copies are untouched.
        QVERIFY(!QFile::exists(c.path));
        QCOMPARE(get(a.path), QByteArray("a-v2"));
        // A read-only target whose content differs is a conflict, listed (never
        // silently resolved); once writable again it is a replace.
        put(b.path, "b-tampered"); // differs from the anchor now (writable yet)
        QVERIFY(QFile::setPermissions(b.path, QFile::ReadOwner | QFile::ReadGroup | QFile::ReadOther));
        const auto conflictPlan = library.planRestore(snap);
        const auto *bConflict = byId(conflictPlan, b.id);
        QVERIFY(bConflict);
        QCOMPARE(bConflict->kind, QStringLiteral("conflict"));
        QCOMPARE(bConflict->reason, QStringLiteral("目标不可写"));
        QVERIFY(QFile::setPermissions(b.path, QFile::ReadOwner | QFile::WriteOwner | QFile::ReadGroup | QFile::ReadOther));
        const auto writablePlan = library.planRestore(snap);
        QCOMPARE(byId(writablePlan, b.id)->kind, QStringLiteral("replace"));
    }
    void restoreGuardReportsStateBeforeOverwrite() {
        QTemporaryDir dir;
        const auto source = dir.filePath("a.png");
        put(source, "v1");
        Library library(dir.filePath("library"));
        const auto a = library.importFile(source);
        const qint64 v1 = library.revisions(a.id).first().id;
        // Recorded current content: safe to restore.
        auto guard = library.checkRestore(a, v1);
        QCOMPARE(guard.state, QStringLiteral("ready"));
        QCOMPARE(guard.protectBytes, qint64(0));
        QVERIFY(guard.restoreBytes > 0);
        // Unrecorded current content: it WILL be protected first (new revision),
        // and the guard reports the space that protection needs, without writing.
        put(a.path, "a-unsaved");
        guard = library.checkRestore(a, v1);
        QCOMPARE(guard.state, QStringLiteral("unrecorded"));
        QCOMPARE(guard.protectBytes, qint64(9)); // "a-unsaved"
        QCOMPARE(get(a.path), QByteArray("a-unsaved")); // the check wrote nothing
        // The restore itself protects the unrecorded content BEFORE overwriting:
        // the auto-save revision is recorded and the working copy lands on v1.
        library.restore(a, v1);
        QCOMPARE(get(a.path), QByteArray("v1"));
        const auto after = library.revisions(a.id);
        QCOMPARE(after.size(), 3); // v1, auto-save(a-unsaved), restore(v1)
        QCOMPARE(library.readObject(after.at(1).hash), QByteArray("a-unsaved"));
        QVERIFY(after.first().id > after.at(1).id);
        // After the restore the newest revision matches the working content again.
        QCOMPARE(library.checkRestore(a, v1).state, QStringLiteral("ready"));
        // A target revision that does not exist blocks the restore.
        QCOMPARE(library.checkRestore(a, 999999).state, QStringLiteral("notarget"));
        // A missing working copy has nothing to protect; the caller confirms.
        QVERIFY(QFile::remove(a.path));
        QCOMPARE(library.checkRestore(a, v1).state, QStringLiteral("missing"));
        // A corrupt target object blocks (never restores unverified bytes).
        const auto b = library.importFile(source);
        const qint64 bV1 = library.revisions(b.id).first().id;
        QVERIFY(platform::makeMutable(library.objectPath(library.revisions(b.id).first().hash)));
        put(library.objectPath(library.revisions(b.id).first().hash), "corrupt");
        QCOMPARE(library.checkRestore(b, bV1).state, QStringLiteral("corrupt"));
    }
    void singleAssetRestoreAppendsNewVersionKeepingHistory() {
        QTemporaryDir dir;
        const auto source = dir.filePath("a.png");
        put(source, "A");
        Library library(dir.filePath("library"));
        const auto asset = library.importFile(source);
        const qint64 revA = library.revisions(asset.id).first().id;
        // A -> B (a second recorded version).
        put(asset.path, "B");
        const qint64 revB = library.recordVersion(asset, QStringLiteral("第二版")).revisionId;
        QVERIFY(revB > revA);
        // Restoring A creates a NEW revision C; the working copy lands on A and
        // B stays in history (no rollback-and-erase).
        library.restore(asset, revA);
        QCOMPARE(get(asset.path), QByteArray("A"));
        const auto history = library.revisions(asset.id);
        QCOMPARE(history.size(), 3); // A, B, C(restore of A)
        const qint64 revC = history.first().id;
        QVERIFY(revC > revB);
        QCOMPARE(history.first().note, QStringLiteral("恢复历史版本 #%1").arg(revA));
        // The restore result C is queryable again (same content as A, its own id).
        QCOMPARE(library.readObject(history.first().hash), QByteArray("A"));
        // B is still browsable/exportable and untouched by the restore.
        for (const auto &rev : history) if (rev.id == revB) {
            QCOMPARE(library.readObject(rev.hash), QByteArray("B"));
            library.exportRevision(asset.id, revB, dir.filePath("export-B.png"));
            QCOMPARE(get(dir.filePath("export-B.png")), QByteArray("B"));
        }
        // The restored C content is a distinct revision; restoring it back again
        // keeps growing history rather than erasing it.
        library.restore(asset, revB);
        QCOMPARE(get(asset.path), QByteArray("B"));
        QCOMPARE(library.revisions(asset.id).size(), 4);
    }
};
QTEST_GUILESS_MAIN(HistoryTest)
#include "library_test.moc"
