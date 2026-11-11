// Restore: preview/plan and bound execution (P-315/P-316), the durable
// restore log and crash convergence (P-320), selective snapshot restore
// (P-318), tombstoned deletion, working-copy health and historical export
// (P-319) (LAY-002 split).
#include "library_internal.h"

#include "library.h"

#include "exclusion.h"
#include "platform_immutable.h"
#include "preview.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QSaveFile>
#include <QSqlError>
#include <QThread>
#include <QSqlQuery>
#include <QUuid>
#include <stdexcept>


using namespace library_internal;

// P-315: the preview phase. Builds the exact operation list for a restore —
// overwrite / create / skip / conflict plus the space requirement — WITHOUT
// writing any file (the preview never modifies working files). The plan is
// bound to the target revision and the working copy's (size, mtime); execution
// re-checks that binding and refuses a stale plan.
Library::RestorePlan Library::planRestore(const Asset &asset, qint64 revisionId) const {
    const std::lock_guard<QRecursiveMutex> guard(mutex_);
    RestorePlan plan;
    plan.revisionId = revisionId;
    QString targetHash;
    bool found = false;
    for (const auto &revision : revisions(asset.id)) {
        if (revision.id != revisionId) continue;
        found = true;
        plan.targetBytes = revision.size;
        targetHash = revision.hash;
        // The target object must be present and complete — surfaced as a
        // conflict before any write, never as a silent partial restore.
        const QFileInfo object(objectPath(revision.hash));
        if (!object.exists() || object.size() != revision.size) {
            plan.action = QStringLiteral("conflict");
            plan.conflictReason = Library::tr("目标版本内容缺失或损坏，无法恢复");
            return plan;
        }
        break;
    }
    if (!found) fail(Library::tr("找不到属于当前素材的历史版本"));
    const QFileInfo info(asset.path);
    if (!info.exists()) {
        // Missing working copy: nothing to protect, but a file will be created —
        // the UI asks the user to confirm this (it is never silent).
        plan.action = QStringLiteral("create");
        plan.plannedSize = -1;
        plan.spaceNeeded = plan.targetBytes;
        return plan;
    }
    plan.plannedSize = info.size();
    plan.plannedMtime = info.lastModified().toMSecsSinceEpoch();
    if (!info.isReadable()) {
        // P-316: unreachable storage — the current content cannot be read, so it
        // cannot be protected. A conflict, never an unprotected overwrite.
        plan.action = QStringLiteral("conflict");
        plan.conflictReason = Library::tr("工作副本不可读，无法保护当前内容");
        return plan;
    }
    const QByteArray current = readFile(asset.path);
    if (digest(current) == targetHash) {
        plan.action = QStringLiteral("skip"); // already at the target content
        return plan;
    }
    plan.action = QStringLiteral("overwrite");
    plan.protectBytes = current.size();
    plan.spaceNeeded = plan.protectBytes + plan.targetBytes;
    return plan;
}

void Library::restore(const Asset &asset, qint64 revisionId) {
    restore(asset, planRestore(asset, revisionId));
}

// P-315/P-316: the execution phase. Re-checks the plan's binding first — a
// working copy that appeared, disappeared or changed since the preview means
// the plan is stale and the caller must refresh it (no stale plan ever runs).
// The current on-disk content is protected as a version BEFORE any overwrite;
// a failed protect (or a detected concurrent external write) stops the restore
// with nothing overwritten unprotected.
void Library::restore(const Asset &asset, const RestorePlan &plan) {
    const std::lock_guard<QRecursiveMutex> guard(mutex_);
    const QFileInfo current(asset.path);
    if (plan.plannedSize < 0) {
        if (current.exists())
            fail(Library::tr("恢复计划已失效：工作副本已出现，请刷新预演"));
    } else if (!current.exists()) {
        fail(Library::tr("恢复计划已失效：工作副本已消失，请刷新预演"));
    } else if (current.size() != plan.plannedSize
               || current.lastModified().toMSecsSinceEpoch() != plan.plannedMtime) {
        fail(Library::tr("恢复计划已失效：工作副本已变化，请刷新预演"));
    }
    if (plan.action == QStringLiteral("skip")) return;
    if (plan.action == QStringLiteral("conflict"))
        fail(plan.conflictReason.isEmpty() ? Library::tr("无法恢复：冲突未解决") : plan.conflictReason);
    Revision target;
    bool found = false;
    for (const auto &revision : revisions(asset.id)) {
        if (revision.id != plan.revisionId) continue;
        target = revision;
        found = true;
        break;
    }
    if (!found) fail(Library::tr("找不到属于当前素材的历史版本"));
    const auto bytes = readObject(target.hash); // Verify before touching the working file.
    const bool existed = current.exists();
    QByteArray previous;
    qint64 readSize = -1;
    qint64 readMtime = 0;
    if (existed) {
        previous = readFile(asset.path);
        const QFileInfo afterRead(asset.path);
        readSize = afterRead.size();
        readMtime = afterRead.lastModified().toMSecsSinceEpoch();
    }
    // P-320: the restore runs in durable stages. The protect version (stage 1)
    // and the restore-log row (stage 2) are committed BEFORE the working file
    // is replaced, so a crash or power loss between the file write (stage 3)
    // and the index commit (stage 4) leaves a log row that convergeRestoreLog()
    // completes or rolls back on reopen — a half-restored state is never left
    // silently behind. A SQLite transaction alone is not crash-safe across the
    // file write; the log is.
    const QString previousHash = existed ? digest(previous) : QStringLiteral("");
    qint64 protectId = 0;
    const auto currentHistory = revisions(asset.id);
    if (existed && (currentHistory.isEmpty() || currentHistory.first().hash != previousHash)) {
        // Stage 1: protect. The unrecorded on-disk content becomes a real
        // version BEFORE anything else. Committed on its own: if the restore is
        // abandoned later, the protect stays (a legit version of the content
        // that was there) and nothing is half-restored.
        if (!db().transaction()) fail(db().lastError().text());
        try {
            append(asset, previous, QStringLiteral("恢复前自动保存"));
            if (!db().commit()) fail(db().lastError().text());
            protectId = revisions(asset.id).first().id;
        } catch (...) {
            db().rollback();
            throw;
        }
    }
    // Stage 2: write the durable log row (committed before any file change).
    const qint64 logId = writeRestoreLog(asset.id, plan.revisionId, previousHash,
                                         target.hash, protectId);
    // Stage 3: replace the working file (QSaveFile is atomic per file).
    try {
        if (!QDir().mkpath(QFileInfo(asset.path).absolutePath())) fail(Library::tr("无法创建恢复目录"));
        // P-316: external-edit concurrency. An outside process writing the file
        // between our read and our write would be silently destroyed by the
        // overwrite — block instead and ask the user to close the editor.
        if (existed) {
            const QFileInfo beforeWrite(asset.path);
            if (beforeWrite.size() != readSize
                || beforeWrite.lastModified().toMSecsSinceEpoch() != readMtime)
                fail(Library::tr("检测到外部编辑正在写入，请关闭外部编辑器后重试"));
        }
        writeFile(asset.path, bytes);
    } catch (const std::exception &error) {
        // The file was not replaced (QSaveFile keeps the original on failure);
        // record the failure so the log never claims a finished restore.
        markRestoreLog(logId, QStringLiteral("failed"), QString::fromUtf8(error.what()));
        throw;
    }
    // Stage 4: append the restore revision and mark the log done (one commit).
    if (!db().transaction()) {
        markRestoreLog(logId, QStringLiteral("failed"), db().lastError().text());
        fail(db().lastError().text());
    }
    try {
        append(asset, bytes, QStringLiteral("恢复历史版本 #%1").arg(plan.revisionId));
        markRestoreLog(logId, QStringLiteral("done"));
        if (!db().commit()) fail(db().lastError().text());
    } catch (...) {
        // The file WAS replaced but the index did not commit: restore the
        // previous content so file and index agree, and record the failure in
        // the log (in-process path; a power loss here is handled by convergence
        // on reopen instead).
        db().rollback();
        if (existed) writeFile(asset.path, previous);
        else QFile::remove(asset.path);
        markRestoreLog(logId, QStringLiteral("failed"), db().lastError().text());
        throw;
    }
}

qint64 Library::writeRestoreLog(const QString &assetId, qint64 revisionId,
                                const QString &previousHash, const QString &targetHash,
                                qint64 protectId) {
    if (!db().transaction()) fail(db().lastError().text());
    qint64 logId = 0;
    try {
        QSqlQuery ins(db());
        ins.prepare("INSERT INTO restore_log(asset_id,revision_id,state,previous_hash,target_hash,"
                    "protect_revision_id,error,created,updated) VALUES(?,?,?,?,?,?,'',?,?)");
        const QString now = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
        ins.addBindValue(assetId);
        ins.addBindValue(revisionId);
        ins.addBindValue(QStringLiteral("planned"));
        ins.addBindValue(previousHash);
        ins.addBindValue(targetHash);
        ins.addBindValue(protectId);
        ins.addBindValue(now);
        ins.addBindValue(now);
        execute(ins);
        logId = ins.lastInsertId().toLongLong();
        if (!db().commit()) fail(db().lastError().text());
    } catch (...) {
        db().rollback();
        throw;
    }
    return logId;
}

QVector<Library::RestoreLogEntry> Library::restoreLogs() const {
    const std::lock_guard<QRecursiveMutex> guard(mutex_);
    QVector<RestoreLogEntry> entries;
    QSqlQuery q(db());
    q.prepare("SELECT id,asset_id,revision_id,state,error,created,updated "
              "FROM restore_log ORDER BY id DESC");
    execute(q);
    while (q.next()) {
        RestoreLogEntry entry;
        entry.id = q.value(0).toLongLong();
        entry.assetId = q.value(1).toString();
        entry.revisionId = q.value(2).toLongLong();
        entry.state = q.value(3).toString();
        entry.error = q.value(4).toString();
        entry.created = q.value(5).toString();
        entry.updated = q.value(6).toString();
        entries.append(entry);
    }
    return entries;
}

void Library::markRestoreLog(qint64 logId, const QString &state, const QString &error) {
    if (logId <= 0) return;
    QSqlQuery up(db());
    up.prepare("UPDATE restore_log SET state=?,error=?,updated=? WHERE id=?");
    up.addBindValue(state);
    up.addBindValue(error.isNull() ? QStringLiteral("") : error); // NOT NULL column: bind '' not null
    up.addBindValue(QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
    up.addBindValue(logId);
    execute(up);
}

// P-320: on open, finish or roll back any restore that was interrupted between
// the file replacement and the index commit. A log row left in 'planned' means
// the file write may or may not have happened; comparing the current working
// file against the logged previous/target hashes decides which way to converge
// — forward (the file is the target, complete the index) or backward (the file
// is still the original, record the abort). Any other file state is an external
// edit after the interruption: left untouched and flagged, never overwritten.
void Library::convergeRestoreLog() {
    QSqlQuery q(db());
    q.prepare("SELECT id,asset_id,revision_id,previous_hash,target_hash,protect_revision_id "
              "FROM restore_log WHERE state='planned'");
    execute(q);
    struct Pending { qint64 id; QString assetId; qint64 revisionId; QString prev; QString target; };
    QVector<Pending> pending;
    while (q.next())
        pending.append({q.value(0).toLongLong(), q.value(1).toString(), q.value(2).toLongLong(),
                        q.value(3).toString(), q.value(4).toString()});
    for (const auto &row : pending) {
        // Locate the asset row (even if deleted — the working path is still needed).
        QSqlQuery asset(db());
        asset.prepare("SELECT name,path,deleted FROM assets WHERE id=?");
        asset.addBindValue(row.assetId);
        execute(asset);
        if (!asset.next()) { markRestoreLog(row.id, QStringLiteral("aborted"), Library::tr("素材记录不存在")); continue; }
        const bool deleted = asset.value(2).toInt() != 0;
        if (deleted) { markRestoreLog(row.id, QStringLiteral("aborted"), Library::tr("素材已删除，不自动复活")); continue; }
        const QString stored = asset.value(1).toString();
        const QString workingPath = QFileInfo(stored).isAbsolute() ? stored : root_ + "/" + stored;
        // The target object must still exist for a forward convergence.
        QSqlQuery rev(db());
        rev.prepare("SELECT hash FROM revisions WHERE asset_id=? AND id=?");
        rev.addBindValue(row.assetId);
        rev.addBindValue(row.revisionId);
        execute(rev);
        if (!rev.next()) { markRestoreLog(row.id, QStringLiteral("aborted"), Library::tr("目标版本不存在")); continue; }
        const QString targetHash = rev.value(0).toString();
        const QFileInfo info(workingPath);
        // Audit (2026-11-12, P0): the working copy is hashed with the streaming,
        // non-throwing hashFile — the old readFile() caps at 256 MiB and throws,
        // so an over-limit (or mid-read-failing) copy would make the Library
        // constructor throw in convergeRestoreLog and leave the whole library
        // unopenable (with the lock already held). A copy that cannot be hashed
        // never matches a hash: it is flagged conflict and left untouched.
        QString fileHash;
        const bool hashed = info.exists() && info.isReadable()
            && hashFile(workingPath, &fileHash, nullptr);
        if (!hashed) {
            markRestoreLog(row.id, QStringLiteral("conflict"),
                           Library::tr("工作副本不可读或无法校验，未自动处理"));
            continue;
        }
        if (fileHash == targetHash) {
            // The file WAS replaced; the index never committed. Complete it now.
            QByteArray bytes;
            try { bytes = readObject(targetHash); }
            catch (const std::exception &error) {
                markRestoreLog(row.id, QStringLiteral("failed"),
                               Library::tr("目标内容损坏，无法完成恢复：%1").arg(QString::fromUtf8(error.what())));
                continue;
            }
            Asset a;
            a.id = row.assetId;
            a.name = asset.value(0).toString();
            a.path = workingPath;
            if (!db().transaction()) { markRestoreLog(row.id, QStringLiteral("failed"), db().lastError().text()); continue; }
            try {
                append(a, bytes, QStringLiteral("恢复历史版本 #%1").arg(row.revisionId));
                markRestoreLog(row.id, QStringLiteral("done"));
                if (!db().commit()) fail(db().lastError().text());
            } catch (...) {
                db().rollback();
                markRestoreLog(row.id, QStringLiteral("failed"), db().lastError().text());
            }
        } else if (fileHash == row.prev) {
            // The file was never replaced (or was already rolled back). Nothing
            // to undo; the protect revision (if any) is a legit version. The
            // restore simply did not complete — record the abort.
            markRestoreLog(row.id, QStringLiteral("aborted"), Library::tr("恢复未完成，已取消"));
        } else {
            // The working file changed after the interruption for another reason:
            // never overwrite it. Flag the row and leave the file alone.
            markRestoreLog(row.id, QStringLiteral("conflict"), Library::tr("工作副本在中断后被外部修改，未自动处理"));
        }
    }
}
// anchor. Excluded / tombstone / unknown entries are reported as skip with a

QVector<Library::SnapshotRestoreItem> Library::planSnapshotRestore(
    qint64 snapshotId, const QStringList &assetIds) const {
    const std::lock_guard<QRecursiveMutex> guard(mutex_);
    QVector<SnapshotRestoreItem> result;
    QHash<QString, Asset> byId;
    for (const auto &asset : assets()) byId.insert(asset.id, asset);
    for (const QString &assetId : assetIds) {
        SnapshotRestoreItem item;
        item.assetId = assetId;
        QSqlQuery entry(db());
        entry.prepare("SELECT name,state,revision_id FROM snapshot_assets "
                      "WHERE snapshot_id=? AND asset_id=?");
        entry.addBindValue(snapshotId);
        entry.addBindValue(assetId);
        execute(entry);
        const auto it = byId.constFind(assetId);
        if (!entry.next()) {
            item.name = assetId;
            item.plan.action = QStringLiteral("skip");
            item.plan.revisionId = 0;
            item.plan.conflictReason = Library::tr("不在该快照中，跳过");
            result.append(item);
            continue;
        }
        item.name = entry.value(0).toString();
        const QString state = entry.value(1).toString();
        const qint64 anchorId = entry.value(2).toLongLong();
        if (state == QLatin1String("excluded")) {
            item.plan.action = QStringLiteral("skip");
            item.plan.revisionId = 0;
            item.plan.conflictReason = Library::tr("快照中该素材被排除，跳过");
            result.append(item);
            continue;
        }
        if (state == QLatin1String("tombstone")) {
            item.plan.action = QStringLiteral("skip");
            item.plan.revisionId = 0;
            item.plan.conflictReason = Library::tr("该素材已在快照后删除（历史保留，不自动复活）");
            result.append(item);
            continue;
        }
        if (it == byId.constEnd()) {
            item.plan.action = QStringLiteral("skip");
            item.plan.revisionId = 0;
            item.plan.conflictReason = Library::tr("素材不存在，跳过");
            result.append(item);
            continue;
        }
        // present or missing: plan against the anchor revision. A missing
        // working copy is recoverable (P-310) — the plan is "create".
        item.plan = planRestore(it.value(), anchorId);
        result.append(item);
    }
    return result;
}

// P-318 execution: applies ONLY the reviewed items, one outcome each. A failed
// item records "failed" and the batch continues — no cross-item atomic claim.
QVector<Library::RestoreOutcome> Library::restoreFromSnapshot(
    qint64 snapshotId, const QVector<SnapshotRestoreItem> &items) {
    const std::lock_guard<QRecursiveMutex> guard(mutex_);
    QVector<RestoreOutcome> outcomes;
    QHash<QString, Asset> byId;
    for (const auto &asset : assets()) byId.insert(asset.id, asset);
    for (const auto &item : items) {
        RestoreOutcome outcome;
        outcome.assetId = item.assetId;
        outcome.name = item.name;
        if (item.plan.action == QStringLiteral("skip")) {
            outcome.outcome = QStringLiteral("skipped");
            outcome.error = item.plan.conflictReason;
            outcomes.append(outcome);
            continue;
        }
        const auto it = byId.constFind(item.assetId);
        if (it == byId.constEnd()) {
            outcome.outcome = QStringLiteral("failed");
            outcome.error = Library::tr("素材不存在");
            outcomes.append(outcome);
            continue;
        }
        try {
            restore(it.value(), item.plan);
            outcome.outcome = QStringLiteral("success");
        } catch (const std::exception &error) {
            outcome.outcome = QStringLiteral("failed");
            outcome.error = QString::fromUtf8(error.what());
        }
        outcomes.append(outcome);
    }
    return outcomes;
}

void Library::deleteAsset(const QString &assetId) {
    const std::lock_guard<QRecursiveMutex> guard(mutex_);
    QSqlQuery find(db());
    find.prepare("SELECT name,deleted,path FROM assets WHERE id=?");
    find.addBindValue(assetId);
    execute(find);
    if (!find.next()) fail(Library::tr("素材不存在"));
    const QString name = find.value(0).toString();
    const bool alreadyDeleted = find.value(1).toInt() != 0;
    if (alreadyDeleted) fail(Library::tr("素材已删除"));
    const QString stored = find.value(2).toString();
    const QString workingPath = QFileInfo(stored).isAbsolute() ? stored : root_ + "/" + stored;
    // The working copy is removed only for a confirmed in-app delete. A file
    // that is already gone (external removal) still gets the tombstone — the
    // user's intent is recorded and history stays complete. An unremovable file
    // (locked, unplugged mount, permissions) blocks the delete rather than
    // half-deleting; that is the "storage unreachable" state, never a deletion.
    if (QFileInfo::exists(workingPath)) {
        QFile f(workingPath);
        if (!f.remove() && QFileInfo::exists(workingPath))
            fail(Library::tr("无法删除工作副本：%1（请关闭占用该文件的程序）").arg(name));
    }
    // Tombstone revision: history and objects stay intact. The revision reuses
    // the last content hash and size, so the timeline remains fully browsable
    // and restorable after the asset is gone from the live list.
    QString hash;
    qint64 size = 0;
    const auto history = revisions(assetId);
    if (!history.isEmpty()) { hash = history.first().hash; size = history.first().size; }
    if (!db().transaction()) fail(db().lastError().text());
    try {
        QSqlQuery ins(db());
        ins.prepare("INSERT INTO revisions(asset_id,hash,created,note,size,pinned,pin_reason,tombstone) VALUES(?,?,?,?,?,0,'',1)");
        ins.addBindValue(assetId);
        ins.addBindValue(hash);
        ins.addBindValue(QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
        ins.addBindValue(QStringLiteral("删除"));
        ins.addBindValue(size);
        execute(ins);
        QSqlQuery up(db());
        up.prepare("UPDATE assets SET deleted=1 WHERE id=?");
        up.addBindValue(assetId);
        execute(up);
        if (!db().commit()) fail(db().lastError().text());
    } catch (...) {
        db().rollback();
        throw;
    }
}

Library::Health Library::workingCopyHealth(const Asset &asset) const {
    const std::lock_guard<QRecursiveMutex> guard(mutex_);
    const QFileInfo info(asset.path);
    const QFileInfo dirInfo(info.absolutePath());
    // A file in an unreadable/unstat-able directory is "unreachable" (lost
    // mount, dropped network share, permissions) rather than "missing" —
    // unplugging or a network drop is never a deletion, so no tombstone is ever
    // written for it. Only deleteAsset writes a tombstone.
    if (dirInfo.exists() && !dirInfo.isReadable()) return Health::Unreachable;
    if (!info.exists()) return Health::Missing;
    if (!info.isReadable()) return Health::Unreachable;
    QFile f(asset.path);
    if (!f.open(QIODevice::ReadOnly)) return Health::Unreachable;
    return Health::Healthy;
}

void Library::exportRevision(const QString &assetId, qint64 revisionId,
                             const QString &destination, bool overwrite) const {
    const std::lock_guard<QRecursiveMutex> guard(mutex_);
    // Never export into the managed library (would permit overwriting immutable objects).
    const QString dest = QFileInfo(destination).absoluteFilePath();
    const QString parent = QFileInfo(dest).dir().canonicalPath();
    const QString canonicalRoot = QDir(root_).canonicalPath();
    if (parent.isEmpty() || parent == canonicalRoot || parent.startsWith(canonicalRoot + '/'))
        fail(Library::tr("请导出到素材库之外的现有目录"));
    // P-319: a symlink destination could resolve back into the library (objects/
    // or a working file) — following it would overwrite library-internal data.
    if (QFileInfo(dest).isSymLink())
        fail(Library::tr("导出目标是指向其他位置的链接，请选择一个真实文件路径"));
    // P-319: an existing file is only overwritten with explicit confirmation —
    // the caller passes overwrite=true after asking.
    if (!overwrite && QFileInfo::exists(dest))
        fail(Library::tr("目标文件已存在；覆盖需要明确确认"));
    for (const auto &revision : revisions(assetId)) {
        if (revision.id == revisionId) { writeFile(dest, readObject(revision.hash)); return; }
    }
    fail(Library::tr("找不到历史版本"));
}

