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
};
QTEST_GUILESS_MAIN(HistoryTest)
#include "library_test.moc"
