// Whole-tree snapshots and their guards: exclusion rules (P-306), revision
// pins (P-307), the emergency anchor snapshot (P-304), manifest browsing
// (P-311/P-312) and one-click rollback (LAY-002 split).
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

// --- Version-exclusion rules (P-306) -------------------------------------------
// A rule (path / pattern / extension) keeps the matching asset out of snapshots
// taken after the rule exists. It never removes history and is never a deletion.

void Library::addExclusion(const QString &kind, const QString &pattern) {
    const std::lock_guard<QRecursiveMutex> guard(mutex_);
    const QString normalized = exclusionNormalize(kind, pattern);
    if (normalized.isEmpty()) fail(Library::tr("排除规则不能为空"));
    if (kind != QLatin1String("extension") && kind != QLatin1String("path") && kind != QLatin1String("name"))
        fail(Library::tr("未知的排除规则类型"));
    QSqlQuery find(db());
    find.prepare("SELECT COUNT(*) FROM exclusions WHERE kind=? AND pattern=?");
    find.addBindValue(kind);
    find.addBindValue(normalized);
    execute(find);
    if (find.next() && find.value(0).toInt() > 0) fail(Library::tr("该排除规则已存在"));
    QSqlQuery ins(db());
    ins.prepare("INSERT INTO exclusions(kind,pattern,created) VALUES(?,?,?)");
    ins.addBindValue(kind);
    ins.addBindValue(normalized);
    ins.addBindValue(QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
    execute(ins);
}

void Library::removeExclusion(const QString &kind, const QString &pattern) {
    const std::lock_guard<QRecursiveMutex> guard(mutex_);
    QSqlQuery del(db());
    del.prepare("DELETE FROM exclusions WHERE kind=? AND pattern=?");
    del.addBindValue(kind);
    del.addBindValue(exclusionNormalize(kind, pattern));
    execute(del);
}

QVector<Exclusion> Library::exclusions() const {
    const std::lock_guard<QRecursiveMutex> guard(mutex_);
    QSqlQuery q(db());
    q.prepare("SELECT kind,pattern,created FROM exclusions ORDER BY kind,pattern");
    execute(q);
    QVector<Exclusion> result;
    while (q.next()) {
        result.push_back({q.value(0).toString(), q.value(1).toString(), q.value(2).toString()});
    }
    return result;
}

// --- Key-version pinning (P-307) ------------------------------------------------
// A pin marks a version as untouchable by any future retention policy and is
// persisted (survives a restart). Unpinning never deletes history.

void Library::pinRevision(qint64 revisionId, const QString &reason) {
    const std::lock_guard<QRecursiveMutex> guard(mutex_);
    QSqlQuery find(db());
    find.prepare("SELECT COUNT(*) FROM revisions WHERE id=?");
    find.addBindValue(revisionId);
    execute(find);
    if (find.next() && find.value(0).toInt() == 0) fail(Library::tr("版本不存在"));
    QSqlQuery upd(db());
    upd.prepare("UPDATE revisions SET pinned=1,pin_reason=? WHERE id=?");
    upd.addBindValue(reason.trimmed().isEmpty() ? Library::tr("手动固定") : reason.trimmed());
    upd.addBindValue(revisionId);
    execute(upd);
}

void Library::unpinRevision(qint64 revisionId) {
    const std::lock_guard<QRecursiveMutex> guard(mutex_);
    QSqlQuery upd(db());
    upd.prepare("UPDATE revisions SET pinned=0,pin_reason='' WHERE id=?");
    upd.addBindValue(revisionId);
    execute(upd);
}

qint64 Library::snapshotAll(const QString &note) {
    const std::lock_guard<QRecursiveMutex> guard(mutex_);
    // P-311: the manifest is published once, built from completed revisions —
    // the snapshot boundary is a sequence point in the revision sequence, not
    // whatever a live file scan happens to see. Before the transaction, wait
    // (bounded retries) for any file still being written; a file that never
    // settles makes the candidate incomplete, and an incomplete candidate is
    // never published as a successful snapshot: the whole call fails instead.
    const auto fileSig = [](const QString &path) {
        const QFileInfo info(path);
        return qMakePair(info.size(), info.lastModified().toMSecsSinceEpoch());
    };
    QStringList writing;
    for (const auto &asset : assets()) {
        const QFileInfo info(asset.path);
        if (!info.exists()) continue; // missing: manifest lists the last revision
        // Only a very recently modified file can be mid-write; older files are
        // treated as settled (one stat, no wait). The transaction's torn-read
        // guard is the authoritative check regardless of this fast path.
        const qint64 ageMs = QDateTime::currentMSecsSinceEpoch()
                           - info.lastModified().toMSecsSinceEpoch();
        if (ageMs > 3000) continue;
        auto observed = fileSig(asset.path);
        bool settled = false;
        for (int attempt = 0; attempt < 3 && !settled; ++attempt) {
            QThread::msleep(25); // bounded wait for the external writer to finish
            const auto again = fileSig(asset.path);
            settled = again == observed;
            observed = again; // keep observing the latest writer state
        }
        if (!settled) writing << asset.logicalPath();
    }
    if (!writing.isEmpty())
        fail(Library::tr("无法发布一致快照，以下文件仍在写入：%1").arg(writing.join(QStringLiteral("、"))));
    if (!db().transaction()) fail(db().lastError().text());
    qint64 snapshotId = 0;
    try {
        QSqlQuery q(db());
        q.prepare("INSERT INTO snapshots(created,note) VALUES(?,?)");
        q.addBindValue(QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
        q.addBindValue(note.isEmpty() ? QStringLiteral("紧急快照") : note);
        execute(q);
        snapshotId = q.lastInsertId().toLongLong();
        const auto rules = exclusions();
        // One manifest row per entry: assetId, path as of the snapshot, the
        // referenced revision's content hash, and a state
        // (present|tombstone|excluded|missing). Every referenced object is
        // verified (present + size) before the manifest is committed.
        const auto insertEntry = [&](const QString &assetId, const QString &folder,
                                     const QString &name, const QString &hash,
                                     const QString &state, const QString &reason,
                                     qint64 revisionId, qint64 objectSize) {
            if (!hash.isEmpty()) {
                const QFileInfo object(objectPath(hash));
                if (!object.exists() || object.size() != objectSize)
                    fail(Library::tr("无法发布一致快照，对象校验失败：%1").arg(name));
            }
            QSqlQuery ins(db());
            ins.prepare("INSERT OR REPLACE INTO snapshot_assets"
                        "(snapshot_id,asset_id,revision_id,state,reason,folder,name,hash) VALUES(?,?,?,?,?,?,?,?)");
            ins.addBindValue(snapshotId);
            ins.addBindValue(assetId);
            ins.addBindValue(revisionId);
            ins.addBindValue(state);
            ins.addBindValue(reason);
            ins.addBindValue(folder);
            ins.addBindValue(name);
            ins.addBindValue(hash);
            execute(ins);
        };
        for (const auto &asset : assets()) {
            // P-306: an asset hitting an exclusion rule is listed in this
            // snapshot as excluded but gets no anchor and is never backed up —
            // it simply stays out of subsequent snapshots, and "excluded" is
            // never a deletion, so a rollback will not touch it.
            QString hit;
            for (const auto &rule : rules) {
                hit = exclusionReason(rule.kind, rule.pattern, asset.folder, asset.name);
                if (!hit.isEmpty()) break;
            }
            if (!hit.isEmpty()) {
                insertEntry(asset.id, asset.folder, asset.name, QString(),
                            QStringLiteral("excluded"), hit, 0, 0);
                continue;
            }
            const auto history = revisions(asset.id);
            QFileInfo info(asset.path);
            // P-302 audit (2026-11-02): the working copy is stream-hashed, never
            // loaded whole — a large file (beyond the 256 MiB import cap) must
            // not OOM the worker thread. A change needing an emergency anchor is
            // stored by streaming the file straight into the object store, so
            // no full in-memory copy is ever allocated on this path.
            QString currentHash;
            if (info.exists() && info.isReadable()) {
                const auto pre = fileSig(asset.path);
                // An open/read failure after a readable stat (mount drop, file
                // deleted between stat and open, EISDIR, …) degrades to
                // "missing"; one bad file must not abort the whole batch.
                if (hashFile(asset.path, &currentHash, nullptr)) {
                    // Torn-read guard: the manifest may only reference settled
                    // content, so a writer that touched the file during the read
                    // makes this candidate incomplete — fail it, publish nothing.
                    if (fileSig(asset.path) != pre)
                        fail(Library::tr("无法发布一致快照，文件写入未稳定：%1").arg(asset.name));
                } else {
                    currentHash.clear();
                }
            }
            // The recovery anchor must be the last CLEAN revision (pre-infection).
            // Race: scanChanges (2s) can auto-record suspected-infected bytes
            // just before pollSentinel (5s) fires, so history.first() may merely
            // mirror the current (infected) working content. The anchor is the
            // newest revision whose content differs from the current working
            // content — an auto-recorded infected mirror (hash == current) is
            // skipped, so the anchor stays on the newest clean (pre-infection)
            // revision. If every revision equals the current content, the current
            // state is genuinely clean (e.g. an untouched asset), so the newest
            // revision is kept as the anchor rather than dropping the asset.
            qint64 anchor = 0;
            QString anchorHash;
            qint64 anchorSize = 0;
            for (const auto &rev : history) { // newest first
                if (currentHash.isEmpty() || rev.hash != currentHash) {
                    anchor = rev.id; anchorHash = rev.hash; anchorSize = rev.size; break;
                }
            }
            if (!anchor && !history.isEmpty()) {
                anchor = history.first().id;
                anchorHash = history.first().hash;
                anchorSize = history.first().size;
            }
            if (!currentHash.isEmpty() && (history.isEmpty() || history.first().hash != currentHash)) {
                // Stream the working copy into the object store (never whole in
                // memory), then record the emergency anchor revision.
                const qint64 storedSize = storeObjectFromFile(asset.path, currentHash);
                recordRevision(asset, currentHash, storedSize,
                               QStringLiteral("紧急快照锚点"));
            }
            if (!anchor) continue; // no clean history to recover to
            // P-310/P-311: a working copy that is gone (external removal) is
            // "missing" — recoverable from the anchored revision, never a
            // deletion, and never silently dropped from the manifest.
            const QString state = info.exists() ? QStringLiteral("present")
                                                : QStringLiteral("missing");
            insertEntry(asset.id, asset.folder, asset.name, anchorHash, state, QString(),
                        anchor, anchorSize);
        }
        // P-310/P-311: in-app deleted assets belong to the whole-tree manifest
        // as tombstones — the library state at this boundary includes "deleted
        // here, history retained". The newest revision of a deleted asset is
        // the tombstone deleteAsset wrote; its object is verified like any
        // other reference.
        QSqlQuery dead(db());
        dead.prepare("SELECT id,name,folder FROM assets WHERE deleted=1 ORDER BY name,id");
        execute(dead);
        while (dead.next()) {
            const auto history = revisions(dead.value(0).toString());
            if (history.isEmpty()) continue;
            insertEntry(dead.value(0).toString(), dead.value(2).toString(),
                        dead.value(1).toString(), history.first().hash,
                        QStringLiteral("tombstone"), QString(),
                        history.first().id, history.first().size);
        }
        if (!db().commit()) fail(db().lastError().text());
    } catch (...) {
        db().rollback();
        throw;
    }
    return snapshotId;
}

QVector<Snapshot> Library::snapshots() const {
    const std::lock_guard<QRecursiveMutex> guard(mutex_);
    QSqlQuery q(db());
    q.prepare("SELECT s.id,s.created,s.note,"
              // P-311: counts are by exact manifest state. present and missing
              // both carry a recoverable anchor, so both count as anchored
              // assets (a missing working copy is recoverable, not deleted).
              // An empty snapshot (no matching rows) yields NULL from the LEFT
              // JOIN, which compares false against every state, so it is 0.
              "SUM(CASE WHEN sa.state='present' OR sa.state='missing' THEN 1 ELSE 0 END),"
              "SUM(CASE WHEN sa.state='excluded' THEN 1 ELSE 0 END) "
              "FROM snapshots s LEFT JOIN snapshot_assets sa ON sa.snapshot_id=s.id "
              "GROUP BY s.id ORDER BY s.id DESC");
    execute(q);
    QVector<Snapshot> result;
    while (q.next()) {
        result.push_back({q.value(0).toLongLong(), q.value(1).toString(), q.value(2).toString(),
                          q.value(3).toInt(), q.value(4).toInt()});
    }
    return result;
}

QVector<SnapshotEntry> Library::snapshotEntries(qint64 snapshotId) const {
    const std::lock_guard<QRecursiveMutex> guard(mutex_);
    QSqlQuery q(db());
    q.prepare("SELECT asset_id,folder,name,hash,state,reason,revision_id "
              "FROM snapshot_assets WHERE snapshot_id=? ORDER BY folder,name,asset_id");
    q.addBindValue(snapshotId);
    execute(q);
    QVector<SnapshotEntry> result;
    while (q.next()) {
        SnapshotEntry entry;
        entry.assetId = q.value(0).toString();
        entry.folder = q.value(1).toString();
        entry.name = q.value(2).toString();
        entry.hash = q.value(3).toString();
        entry.state = q.value(4).toString();
        entry.reason = q.value(5).toString();
        entry.revisionId = q.value(6).toLongLong();
        // P-311: snapshots written before the manifest carried path/hash fall
        // back to the asset's current recorded location (deleted assets still
        // have their row, so this stays correct for tombstones too). The trigger
        // is an empty name only — a root-level entry legitimately has folder=''.
        if (entry.name.isEmpty()) {
            QSqlQuery cur(db());
            cur.prepare("SELECT folder,name FROM assets WHERE id=?");
            cur.addBindValue(entry.assetId);
            execute(cur);
            if (cur.next()) {
                entry.folder = cur.value(0).toString();
                entry.name = cur.value(1).toString();
            }
        }
        result.push_back(entry);
    }
    return result;
}

// P-516: export a consistent full backup package. The mutex is held for the
// whole export — that IS the brief write pause (the app runs one writer on a
// serial queue, so no import/capture/restore can interleave).

int Library::rollbackAll(qint64 snapshotId) {
    const std::lock_guard<QRecursiveMutex> guard(mutex_);
    QHash<QString, Asset> byId;
    for (const auto &asset : assets()) byId.insert(asset.id, asset);
    QSqlQuery q(db());
    // P-306: an excluded entry has no anchor (revision_id 0) and is never a
    // deletion — rollback only touches present and missing entries, so a
    // restore can never damage an excluded asset's current working file.
    // P-311: a missing working copy is recoverable from its anchor, so it is
    // restored exactly like a present one.
    q.prepare("SELECT asset_id, revision_id FROM snapshot_assets "
              "WHERE snapshot_id=? AND (state='present' OR state='missing')");
    q.addBindValue(snapshotId);
    execute(q);
    int restored = 0;
    while (q.next()) {
        const auto assetId = q.value(0).toString();
        const auto anchorId = q.value(1).toLongLong();
        const auto it = byId.constFind(assetId);
        if (it == byId.constEnd()) continue;
        const Asset &asset = it.value();
        QByteArray anchorBytes;
        bool anchorFound = false;
        for (const auto &rev : revisions(assetId)) {
            if (rev.id == anchorId) { anchorBytes = readObject(rev.hash); anchorFound = true; break; }
        }
        // Distinguish "anchor revision vanished" from a valid anchor whose
        // content is itself empty (an empty object is legal). Presence of the
        // revision id is the existence check; content emptiness is not.
        if (!anchorFound) continue; // anchor revision vanished (defensive)
        // Already at the recovery target: skip (a no-op restore would only churn).
        if (QFileInfo::exists(asset.path) && readFile(asset.path) == anchorBytes) continue;
        // One-click rollback goes through the normal restore path: it appends the
        // anchor content as a new version and rewrites the working file, so later
        // history stays intact and the infection's own versions remain recoverable.
        restore(asset, anchorId);
        ++restored;
    }
    return restored;
}
