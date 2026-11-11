// Catalog metadata: tags (P-213), rating and note (P-214), provenance and
// evidence (P-215), per-field history (P-309), rename/move identity (P-216)
// and the P-217 filtering facts (LAY-002 split).
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

// --- Tag maintenance (P-213) --------------------------------------------------
// A tag is trimmed; an empty tag is a no-op rather than an error. Mutators
// report how many rows actually changed so the caller can show real results.

QStringList Library::tags(const QString &assetId) const {
    const std::lock_guard<QRecursiveMutex> guard(mutex_);
    QSqlQuery q(db());
    q.prepare("SELECT tag FROM tags WHERE asset_id=? ORDER BY tag");
    q.addBindValue(assetId);
    execute(q);
    QStringList result;
    while (q.next()) result << q.value(0).toString();
    return result;
}

QStringList Library::assetsWithTag(const QString &tag) const {
    const std::lock_guard<QRecursiveMutex> guard(mutex_);
    QSqlQuery q(db());
    q.prepare("SELECT asset_id FROM tags WHERE tag=? ORDER BY asset_id");
    q.addBindValue(tag.trimmed());
    execute(q);
    QStringList result;
    while (q.next()) result << q.value(0).toString();
    return result;
}

int Library::addTag(const QString &assetId, const QString &tag) {
    const std::lock_guard<QRecursiveMutex> guard(mutex_);
    const QString value = tag.trimmed();
    if (value.isEmpty()) return 0;
    QSqlQuery q(db());
    q.prepare("INSERT OR IGNORE INTO tags(asset_id,tag) VALUES(?,?)");
    q.addBindValue(assetId);
    q.addBindValue(value);
    if (!q.exec()) fail(q.lastError().text());
    if (q.numRowsAffected() > 0) recordFieldChange(assetId, QStringLiteral("标签"), QString(), value);
    return q.numRowsAffected();
}

int Library::removeTag(const QString &assetId, const QString &tag) {
    const std::lock_guard<QRecursiveMutex> guard(mutex_);
    const QString value = tag.trimmed();
    if (value.isEmpty()) return 0;
    QSqlQuery q(db());
    q.prepare("DELETE FROM tags WHERE asset_id=? AND tag=?");
    q.addBindValue(assetId);
    q.addBindValue(value);
    execute(q);
    if (q.numRowsAffected() > 0) recordFieldChange(assetId, QStringLiteral("标签"), value, QString());
    return q.numRowsAffected();
}

int Library::addTagToAssets(const QStringList &assetIds, const QString &tag) {
    const std::lock_guard<QRecursiveMutex> guard(mutex_);
    const QString value = tag.trimmed();
    if (value.isEmpty() || assetIds.isEmpty()) return 0;
    int changed = 0;
    // Audit (2026-11-12, P0): a metadata change also writes revisions +
    // field_history inside the same transaction. Any statement failure (the
    // tag INSERT or recordFieldChange) must roll back the WHOLE transaction,
    // or the leftover transaction chains every later write into an
    // uncommitted state that is lost at process exit. The transaction and
    // commit returns were previously ignored, so a failed commit was reported
    // as success.
    if (!db().transaction())
        fail(tr("无法开启事务：%1").arg(db().lastError().text()));
    try {
        for (const QString &id : assetIds) {
            QSqlQuery q(db());
            q.prepare("INSERT OR IGNORE INTO tags(asset_id,tag) VALUES(?,?)");
            q.addBindValue(id);
            q.addBindValue(value);
            if (!q.exec()) fail(q.lastError().text());
            if (q.numRowsAffected() > 0) recordFieldChange(id, QStringLiteral("标签"), QString(), value);
            changed += q.numRowsAffected();
        }
    } catch (...) {
        db().rollback();
        throw;
    }
    if (!db().commit())
        fail(tr("提交事务失败：%1").arg(db().lastError().text()));
    return changed;
}

int Library::removeTagFromAssets(const QStringList &assetIds, const QString &tag) {
    const std::lock_guard<QRecursiveMutex> guard(mutex_);
    const QString value = tag.trimmed();
    if (value.isEmpty() || assetIds.isEmpty()) return 0;
    int changed = 0;
    if (!db().transaction())
        fail(tr("无法开启事务：%1").arg(db().lastError().text()));
    try {
        for (const QString &id : assetIds) {
            QSqlQuery q(db());
            q.prepare("DELETE FROM tags WHERE asset_id=? AND tag=?");
            q.addBindValue(id);
            q.addBindValue(value);
            if (!q.exec()) fail(q.lastError().text());
            if (q.numRowsAffected() > 0) recordFieldChange(id, QStringLiteral("标签"), value, QString());
            changed += q.numRowsAffected();
        }
    } catch (...) {
        db().rollback();
        throw;
    }
    if (!db().commit())
        fail(tr("提交事务失败：%1").arg(db().lastError().text()));
    return changed;
}

int Library::countAssetsWithoutTag(const QStringList &assetIds, const QString &tag) const {
    const std::lock_guard<QRecursiveMutex> guard(mutex_);
    const QString value = tag.trimmed();
    if (value.isEmpty()) return 0;
    int count = 0;
    for (const QString &id : assetIds) {
        QSqlQuery q(db());
        q.prepare("SELECT 1 FROM tags WHERE asset_id=? AND tag=?");
        q.addBindValue(id);
        q.addBindValue(value);
        execute(q);
        if (!q.next()) ++count;
    }
    return count;
}

int Library::countAssetsWithTag(const QStringList &assetIds, const QString &tag) const {
    const std::lock_guard<QRecursiveMutex> guard(mutex_);
    const QString value = tag.trimmed();
    if (value.isEmpty()) return 0;
    int count = 0;
    for (const QString &id : assetIds) {
        QSqlQuery q(db());
        q.prepare("SELECT 1 FROM tags WHERE asset_id=? AND tag=?");
        q.addBindValue(id);
        q.addBindValue(value);
        execute(q);
        if (q.next()) ++count;
    }
    return count;
}

// --- Rating and note (P-214) ---------------------------------------------------
// One optional asset_meta row per asset. "No rating" is NULL (-1 to callers) and
// is deliberately separate from 0 (explicit zero star). The note is stored as
// typed; it is never trimmed or auto-filled, so an honest empty note stays empty.

int Library::rating(const QString &assetId) const {
    const std::lock_guard<QRecursiveMutex> guard(mutex_);
    QSqlQuery q(db());
    q.prepare("SELECT rating FROM asset_meta WHERE asset_id=?");
    q.addBindValue(assetId);
    execute(q);
    if (!q.next() || q.value(0).isNull()) return -1;
    return q.value(0).toInt();
}

void Library::setRating(const QString &assetId, int rating) {
    const std::lock_guard<QRecursiveMutex> guard(mutex_);
    if (rating < -1 || rating > 5)
        fail(Library::tr("无效的评分：%1").arg(rating));
    // P-309: the old rating is read before the change so the field history can
    // record exactly old → new ("" means no rating).
    QSqlQuery old(db());
    old.prepare("SELECT rating FROM asset_meta WHERE asset_id=?");
    old.addBindValue(assetId);
    execute(old);
    const QString oldValue = old.next() && !old.value(0).isNull()
        ? QString::number(old.value(0).toInt()) : QString();
    QSqlQuery q(db());
    // Upsert preserves an existing note; a fresh row starts with an empty note.
    if (rating < 0) {
        q.prepare("INSERT INTO asset_meta(asset_id,rating,note) VALUES(?,NULL,'') "
                  "ON CONFLICT(asset_id) DO UPDATE SET rating=NULL");
    } else {
        q.prepare("INSERT INTO asset_meta(asset_id,rating,note) VALUES(?,?, '') "
                  "ON CONFLICT(asset_id) DO UPDATE SET rating=excluded.rating");
    }
    q.addBindValue(assetId);
    if (rating >= 0) q.addBindValue(rating);
    execute(q);
    recordFieldChange(assetId, QStringLiteral("评分"), oldValue,
                      rating < 0 ? QString() : QString::number(rating));
}

QString Library::note(const QString &assetId) const {
    const std::lock_guard<QRecursiveMutex> guard(mutex_);
    QSqlQuery q(db());
    q.prepare("SELECT note FROM asset_meta WHERE asset_id=?");
    q.addBindValue(assetId);
    execute(q);
    return q.next() ? q.value(0).toString() : QString();
}

void Library::setNote(const QString &assetId, const QString &note) {
    const std::lock_guard<QRecursiveMutex> guard(mutex_);
    // P-309: the old note is read before the change (the field history records
    // the actual old value, never an invented one).
    QSqlQuery old(db());
    old.prepare("SELECT note FROM asset_meta WHERE asset_id=?");
    old.addBindValue(assetId);
    execute(old);
    const QString oldValue = old.next() ? old.value(0).toString() : QString();
    QSqlQuery q(db());
    // Upsert preserves an existing rating; a fresh row starts with no rating.
    // A null QString binds as SQL NULL, which the NOT NULL note column rejects;
    // an "empty" note is stored as '' (readers see empty either way).
    const QString value = note.isNull() ? QStringLiteral("") : note;
    q.prepare("INSERT INTO asset_meta(asset_id,rating,note) VALUES(?,NULL,?) "
              "ON CONFLICT(asset_id) DO UPDATE SET note=excluded.note");
    q.addBindValue(assetId);
    q.addBindValue(value);
    execute(q);
    recordFieldChange(assetId, QStringLiteral("备注"), oldValue, value);
}

// --- Source/author/license records (P-215) -------------------------------------
// Append-only manual records: addProvenance never updates an existing row, so
// every entry keeps the exact text the user typed plus the time it was entered.
// The recorded timestamp is stamped here, not taken from the caller. Nothing is
// inferred: an empty field stays empty (unknown), and attaching a file or URL
// is recorded as user input only — never as a license assertion.

QVector<Provenance> Library::provenance(const QString &assetId) const {
    const std::lock_guard<QRecursiveMutex> guard(mutex_);
    QSqlQuery q(db());
    q.prepare("SELECT id,source,author,license,evidence,evidence_name,recorded "
              "FROM provenance WHERE asset_id=? ORDER BY id DESC");
    q.addBindValue(assetId);
    execute(q);
    QVector<Provenance> result;
    while (q.next()) {
        Provenance entry;
        entry.id = q.value(0).toLongLong();
        entry.source = q.value(1).toString();
        entry.author = q.value(2).toString();
        entry.license = q.value(3).toString();
        entry.evidence = q.value(4).toString();
        entry.evidenceName = q.value(5).toString();
        entry.recorded = q.value(6).toString();
        result.push_back(entry);
    }
    return result;
}

void Library::addProvenance(const QString &assetId, const Provenance &entry) {
    const std::lock_guard<QRecursiveMutex> guard(mutex_);
    if (!entry.evidence.isEmpty()) objectPath(entry.evidence); // validates hash format
    // A null QString binds as SQL NULL and the NOT NULL columns reject it;
    // "unknown" is stored as an empty string, never NULL.
    const auto text = [](const QString &value) {
        return value.isNull() ? QStringLiteral("") : value;
    };
    // P-309: the previous record's values are the "old" values of this change,
    // so the field history reflects the actual progression (never an invented
    // value). Append-only records stay viewable either way (P-215).
    const auto previous = provenance(assetId);
    const Provenance prior = previous.isEmpty() ? Provenance{} : previous.first();
    QSqlQuery q(db());
    q.prepare("INSERT INTO provenance(asset_id,source,author,license,evidence,evidence_name,recorded) "
              "VALUES(?,?,?,?,?,?,?)");
    q.addBindValue(assetId);
    q.addBindValue(text(entry.source));
    q.addBindValue(text(entry.author));
    q.addBindValue(text(entry.license));
    q.addBindValue(text(entry.evidence));
    q.addBindValue(text(entry.evidenceName));
    q.addBindValue(QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
    execute(q);
    recordFieldChange(assetId, QStringLiteral("来源"), prior.source, text(entry.source));
    recordFieldChange(assetId, QStringLiteral("作者"), prior.author, text(entry.author));
    recordFieldChange(assetId, QStringLiteral("许可"), prior.license, text(entry.license));
}

QString Library::storeEvidence(const QString &filePath) {
    const std::lock_guard<QRecursiveMutex> guard(mutex_);
    const auto bytes = readFile(filePath);
    const QString hash = digest(bytes);
    const QString path = objectPath(hash);
    if (!QFileInfo::exists(path)) {
        writeFile(path, bytes);
        platform::makeImmutable(path);
    } else if (readObject(hash) != bytes) {
        fail(Library::tr("历史对象内容不一致"));
    }
    return hash;
}

void Library::exportEvidence(const QString &hash, const QString &destination) const {
    const std::lock_guard<QRecursiveMutex> guard(mutex_);
    // Never export into the managed library (would permit overwriting immutable objects).
    const QString dest = QFileInfo(destination).absoluteFilePath();
    const QString parent = QFileInfo(dest).dir().canonicalPath();
    const QString canonicalRoot = QDir(root_).canonicalPath();
    if (parent.isEmpty() || parent == canonicalRoot || parent.startsWith(canonicalRoot + '/'))
        fail(Library::tr("请导出到素材库之外的现有目录"));
    // P-319 symmetry with exportRevision: a symlink destination could resolve
    // back into the library (objects/ or a working file) — following it would
    // overwrite library-internal data. Refuse rather than QSaveFile replacing
    // the link itself (which would make the two exports diverge).
    if (QFileInfo(dest).isSymLink())
        fail(Library::tr("导出目标是指向其他位置的链接，请选择一个真实文件路径"));
    writeFile(dest, readObject(hash));
}

// --- Per-field metadata history (P-309) -----------------------------------------
// A metadata change is recorded read-only and appended as a metadata-only
// revision (same content hash and size), so the timeline and the field history
// agree on when each field changed. V1 never reverts a field and never invents
// an old value.

void Library::recordFieldChange(const QString &assetId, const QString &field,
                                const QString &oldValue, const QString &newValue) {
    const std::lock_guard<QRecursiveMutex> guard(mutex_);
    // A null QString binds as SQL NULL, which the NOT NULL columns reject;
    // empty is stored as '' (readers see empty either way).
    const QString oldText = oldValue.isNull() ? QStringLiteral("") : oldValue;
    const QString newText = newValue.isNull() ? QStringLiteral("") : newValue;
    if (oldText == newText) return; // no change: nothing to record
    qint64 revisionId = 0;
    const auto history = revisions(assetId);
    if (!history.isEmpty()) {
        // Metadata-only revision: the content hash and size are copied from the
        // current content, so no new object is written and the content timeline
        // is untouched; only the note carries the field change.
        QSqlQuery ins(db());
        ins.prepare("INSERT INTO revisions(asset_id,hash,created,note,size,pinned,pin_reason) VALUES(?,?,?,?,?,0,'')");
        ins.addBindValue(assetId);
        ins.addBindValue(history.first().hash);
        ins.addBindValue(QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
        ins.addBindValue(QStringLiteral("元数据变更：%1").arg(field));
        ins.addBindValue(history.first().size);
        execute(ins);
        revisionId = ins.lastInsertId().toLongLong();
    }
    QSqlQuery rec(db());
    rec.prepare("INSERT INTO field_history(asset_id,revision_id,field,old_value,new_value,changed) VALUES(?,?,?,?,?,?)");
    rec.addBindValue(assetId);
    rec.addBindValue(revisionId);
    rec.addBindValue(field);
    rec.addBindValue(oldText);
    rec.addBindValue(newText);
    rec.addBindValue(QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
    execute(rec);
}

QVector<FieldChange> Library::fieldHistory(const QString &assetId) const {
    const std::lock_guard<QRecursiveMutex> guard(mutex_);
    QSqlQuery q(db());
    q.prepare("SELECT id,asset_id,revision_id,field,old_value,new_value,changed "
              "FROM field_history WHERE asset_id=? ORDER BY id DESC");
    q.addBindValue(assetId);
    execute(q);
    QVector<FieldChange> result;
    while (q.next()) {
        result.push_back({q.value(0).toLongLong(), q.value(1).toString(), q.value(2).toLongLong(),
                          q.value(3).toString(), q.value(4).toString(), q.value(5).toString(),
                          q.value(6).toString()});
    }
    return result;
}

// --- Folder organization and asset identity (P-216) ----------------------------
// Identity is the asset id: rename and move only update the catalog row (and,
// for a rename, the working-copy file name), so every revision, tag, rating and
// provenance record stays attached to the same asset. A clash with another
// asset's (folder, name) is refused — never a silent overwrite and never an
// automatic merge of two distinct assets.

void Library::renameAsset(const QString &assetId, const QString &newName) {
    const std::lock_guard<QRecursiveMutex> guard(mutex_);
    const QString name = newName.trimmed();
    if (name.isEmpty()) fail(Library::tr("名称不能为空"));
    if (name.contains('/') || name.contains('\\'))
        fail(Library::tr("名称不能包含路径分隔符"));
    QSqlQuery find(db());
    find.prepare("SELECT name,folder,path FROM assets WHERE id=?");
    find.addBindValue(assetId);
    execute(find);
    if (!find.next()) fail(Library::tr("找不到素材"));
    const QString folder = find.value(1).toString();
    // Same-folder name clash: refuse and say so; two assets never merge.
    QSqlQuery clash(db());
    clash.prepare("SELECT id FROM assets WHERE folder=? AND name=? AND id<>?");
    clash.addBindValue(folder);
    clash.addBindValue(name);
    clash.addBindValue(assetId);
    execute(clash);
    if (clash.next())
        fail(Library::tr("同文件夹已有同名素材：%1，不会覆盖").arg(name));
    const QString oldStored = find.value(2).toString();
    // P-202 onward stores root-relative paths; a pre-P-202 library may still hold
    // absolute ones. Resolve the working file either way, then keep its name in
    // sync with the catalog (the per-asset directory itself is untouched).
    const QString oldAbsolute = QFileInfo(oldStored).isAbsolute() ? oldStored : root_ + "/" + oldStored;
    const QString newAbsolute = QFileInfo(oldAbsolute).path() + "/" + name;
    const QString newStored = QFileInfo(oldStored).isAbsolute()
        ? newAbsolute
        : QFileInfo(oldStored).path() + "/" + name;
    if (QFileInfo::exists(oldAbsolute) && oldAbsolute != newAbsolute) {
        if (!QFile::rename(oldAbsolute, newAbsolute))
            fail(Library::tr("无法重命名工作副本"));
    }
    QSqlQuery update(db());
    update.prepare("UPDATE assets SET name=?,path=? WHERE id=?");
    update.addBindValue(name);
    update.addBindValue(newStored);
    update.addBindValue(assetId);
    execute(update);
    recordFieldChange(assetId, QStringLiteral("名称"), find.value(0).toString(), name);
}

void Library::moveAsset(const QString &assetId, const QString &newFolder) {
    const std::lock_guard<QRecursiveMutex> guard(mutex_);
    // A folder is always library-relative; an absolute target is refused rather
    // than silently relativized (the user's intent would be misread).
    if (newFolder.startsWith('/') || newFolder.startsWith('\\'))
        fail(Library::tr("文件夹必须是库内相对路径"));
    // Normalize: trim, drop redundant separators, forbid escaping the library.
    QStringList parts;
    for (const QString &part : newFolder.split('/', Qt::KeepEmptyParts)) {
        const QString piece = part.trimmed();
        if (piece.isEmpty() || piece == ".") continue;
        if (piece == "..") fail(Library::tr("文件夹不能超出素材库"));
        parts << piece;
    }
    const QString folder = parts.join('/');
    QSqlQuery find(db());
    find.prepare("SELECT name,folder FROM assets WHERE id=?");
    find.addBindValue(assetId);
    execute(find);
    if (!find.next()) fail(Library::tr("找不到素材"));
    if (find.value(1).toString() == folder) return; // already there: no-op
    // Same-folder name clash after the move: refuse, never merge identities.
    QSqlQuery clash(db());
    clash.prepare("SELECT id FROM assets WHERE folder=? AND name=? AND id<>?");
    clash.addBindValue(folder);
    clash.addBindValue(find.value(0).toString());
    clash.addBindValue(assetId);
    execute(clash);
    if (clash.next())
        fail(Library::tr("目标文件夹已有同名素材：%1，不会合并").arg(find.value(0).toString()));
    QSqlQuery update(db());
    update.prepare("UPDATE assets SET folder=? WHERE id=?");
    update.addBindValue(folder);
    update.addBindValue(assetId);
    execute(update);
    recordFieldChange(assetId, QStringLiteral("目录"), find.value(1).toString(), folder);
}

// --- Filtering facts (P-217) ----------------------------------------------------
// The detected format and pixel size of the asset's last committed revision.
// Unknown stays unknown: an undecodable content yields an empty format and a
// 0x0 size, never a guessed value.

QString Library::assetFormat(const QString &assetId) const {
    const std::lock_guard<QRecursiveMutex> guard(mutex_);
    QSqlQuery q(db());
    q.prepare("SELECT format FROM assets WHERE id=?");
    q.addBindValue(assetId);
    execute(q);
    return q.next() ? q.value(0).toString() : QString();
}

QSize Library::assetSize(const QString &assetId) const {
    const std::lock_guard<QRecursiveMutex> guard(mutex_);
    QSqlQuery q(db());
    q.prepare("SELECT width,height FROM assets WHERE id=?");
    q.addBindValue(assetId);
    execute(q);
    if (!q.next()) return QSize();
    return QSize(q.value(0).toInt(), q.value(1).toInt());
}
