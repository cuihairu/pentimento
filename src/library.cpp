#include "library.h"

#include "exclusion.h"
#include "platform_immutable.h"
#include "preview.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QLockFile>
#include <QSaveFile>
#include <QSqlError>
#include <QSqlQuery>
#include <QUuid>
#include <stdexcept>

namespace {
[[noreturn]] void fail(const QString &message) { throw std::runtime_error(message.toStdString()); }
QString digest(const QByteArray &bytes) {
    return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}
QByteArray readFile(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) fail(Library::tr("无法读取文件：%1").arg(path));
    // Bounded in-memory first release; do not attempt huge PSD/model imports.
    if (file.size() > 256LL * 1024 * 1024) fail(Library::tr("首版单文件上限为 256 MiB：%1").arg(path));
    const auto bytes = file.readAll();
    if (file.error() != QFileDevice::NoError) fail(file.errorString());
    return bytes;
}
void writeFile(const QString &path, const QByteArray &bytes) {
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit())
        fail(Library::tr("无法安全写入文件：%1；%2").arg(path, file.errorString()));
}
void execute(QSqlQuery &query) {
    if (!query.exec()) fail(query.lastError().text());
}
// P-302 streaming helpers: a large file is hashed and copied in bounded chunks
// instead of being read whole into memory.
QString digestFile(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) fail(Library::tr("无法读取文件：%1").arg(path));
    QCryptographicHash hash(QCryptographicHash::Sha256);
    constexpr qint64 kChunk = 1 << 20; // 1 MiB
    while (!file.atEnd()) {
        const auto chunk = file.read(kChunk);
        if (chunk.isEmpty() && file.error() != QFileDevice::NoError)
            fail(Library::tr("无法读取文件：%1").arg(path));
        hash.addData(chunk);
    }
    return QString::fromLatin1(hash.result().toHex());
}
void copyFileStreaming(const QString &from, const QString &to) {
    QFile in(from), out(to);
    if (!in.open(QIODevice::ReadOnly)) fail(Library::tr("无法读取文件：%1").arg(from));
    if (!out.open(QIODevice::WriteOnly)) fail(Library::tr("无法写入暂存对象：%1").arg(to));
    constexpr qint64 kChunk = 1 << 20; // 1 MiB
    while (!in.atEnd()) {
        const auto chunk = in.read(kChunk);
        if (chunk.isEmpty() && in.error() != QFileDevice::NoError)
            fail(Library::tr("无法读取文件：%1").arg(from));
        if (out.write(chunk) != chunk.size())
            fail(Library::tr("无法写入暂存对象：%1").arg(to));
    }
    out.close();
    if (out.error() != QFileDevice::NoError)
        fail(Library::tr("无法写入暂存对象：%1").arg(to));
}
}

Library::Library(const QString &root)
    : root_(QDir(root).absolutePath()), connection_(QUuid::createUuid().toString()) {
    // P-201 six-category open validation. Order matters: a non-library directory
    // must never be silently rebuilt into a library, and an unreachable volume
    // must be told apart from a permission problem.
    const QString catalog = root_ + "/catalog.sqlite";
    if (QFileInfo::exists(root_)) {
        const QDir dir(root_);
        const bool nonEmpty = dir.entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot).size() > 0;
        if (nonEmpty && !QFileInfo::exists(catalog))
            fail(Library::tr("该目录不是有效的素材库（未找到 catalog.sqlite），不会静默重建"));
        if (QFileInfo::exists(catalog) && !QFileInfo(catalog).isReadable())
            fail(Library::tr("素材库索引不可读（权限不足），请检查目录权限"));
    }
    if (!QDir().mkpath(root_ + "/objects") || !QDir().mkpath(root_ + "/working"))
        fail(Library::tr("素材库目录不可达或无法创建（可能是卷未挂载或权限不足）：%1").arg(root_));
    lock_ = std::make_unique<QLockFile>(root_ + "/library.lock");
    if (!lock_->tryLock(0)) fail(Library::tr("素材库正在被另一个进程使用"));
    try {
        openAndInitialize(catalog);
    } catch (...) {
        // Exception-safe construction: never leak a registered, open SQLite
        // connection (or a held lock) when the constructor fails partway.
        db_ = QSqlDatabase();
        QSqlDatabase::removeDatabase(connection_);
        lock_.reset();
        throw;
    }
    // Opening an existing library must re-apply immutability to any object that
    // a tamperer left writable (idempotent, best-effort). Documented promise in
    // 85-antiransom.md: hardenObjects() is for hardening existing objects on open.
    hardenObjects();
}

void Library::openAndInitialize(const QString &catalog) {
    db_ = QSqlDatabase::addDatabase("QSQLITE", connection_);
    db_.setDatabaseName(catalog);
    if (!db_.open()) {
        const QString error = db_.lastError().text();
        db_ = QSqlDatabase();
        QSqlDatabase::removeDatabase(connection_);
        fail(Library::tr("素材库损坏或索引无法打开：%1").arg(error));
    }
    QSqlQuery q(db_);
    for (const QString &sql : {
             QStringLiteral("PRAGMA foreign_keys=ON"),
             QStringLiteral("CREATE TABLE IF NOT EXISTS meta(key TEXT PRIMARY KEY,value TEXT NOT NULL)"),
             QStringLiteral("CREATE TABLE IF NOT EXISTS assets(id TEXT PRIMARY KEY,name TEXT NOT NULL,path TEXT NOT NULL)"),
             QStringLiteral("CREATE TABLE IF NOT EXISTS revisions(id INTEGER PRIMARY KEY AUTOINCREMENT,asset_id TEXT NOT NULL REFERENCES assets(id),hash TEXT NOT NULL,created TEXT NOT NULL,note TEXT NOT NULL,size INTEGER NOT NULL)"),
             QStringLiteral("CREATE INDEX IF NOT EXISTS revisions_asset ON revisions(asset_id,id)"),
             QStringLiteral("CREATE TABLE IF NOT EXISTS tags(asset_id TEXT NOT NULL REFERENCES assets(id),tag TEXT NOT NULL,PRIMARY KEY(asset_id,tag))"),
             // P-214: one optional row per asset. rating is NULL for "no rating"
             // (distinct from the explicit 0 star); note defaults to empty.
             QStringLiteral("CREATE TABLE IF NOT EXISTS asset_meta(asset_id TEXT PRIMARY KEY REFERENCES assets(id),rating INTEGER,note TEXT NOT NULL DEFAULT '')"),
             // P-215: append-only manual source/author/license records. Each
             // entry keeps its own recorded timestamp, so old entries stay
             // viewable. evidence is an objects/ content hash (empty = none).
             QStringLiteral("CREATE TABLE IF NOT EXISTS provenance(id INTEGER PRIMARY KEY AUTOINCREMENT,asset_id TEXT NOT NULL REFERENCES assets(id),source TEXT NOT NULL,author TEXT NOT NULL,license TEXT NOT NULL,evidence TEXT NOT NULL,evidence_name TEXT NOT NULL,recorded TEXT NOT NULL)"),
             QStringLiteral("CREATE TABLE IF NOT EXISTS snapshots(id INTEGER PRIMARY KEY AUTOINCREMENT,created TEXT NOT NULL,note TEXT NOT NULL)"),
             QStringLiteral("CREATE TABLE IF NOT EXISTS snapshot_assets(snapshot_id INTEGER NOT NULL REFERENCES snapshots(id),asset_id TEXT NOT NULL REFERENCES assets(id),revision_id INTEGER NOT NULL,PRIMARY KEY(snapshot_id,asset_id))"),
             // P-306: version-exclusion rules (kind extension|path|name). A rule
             // only affects snapshots taken after it exists; history is never
             // retroactively removed, and an exclusion is not a deletion.
             QStringLiteral("CREATE TABLE IF NOT EXISTS exclusions(kind TEXT NOT NULL,pattern TEXT NOT NULL,created TEXT NOT NULL,PRIMARY KEY(kind,pattern))"),
             // P-309: append-only per-field metadata history. Each change is
             // linked to the metadata-only revision it created, keeping field
             // history and the revision sequence aligned.
             QStringLiteral("CREATE TABLE IF NOT EXISTS field_history(id INTEGER PRIMARY KEY AUTOINCREMENT,asset_id TEXT NOT NULL REFERENCES assets(id),revision_id INTEGER NOT NULL DEFAULT 0,field TEXT NOT NULL,old_value TEXT NOT NULL DEFAULT '',new_value TEXT NOT NULL DEFAULT '',changed TEXT NOT NULL)")}) {
        if (!q.exec(sql)) fail(q.lastError().text());
    }
    // P-216/P-217: assets gain a logical folder column plus filtering facts
    // (detected format, pixel size). CREATE IF NOT EXISTS does not alter an
    // existing table, so add each column when it is missing (existing rows keep
    // the root folder "" and unknown format/size).
    const auto ensureColumn = [&](const QString &name, const QString &ddl) {
        QSqlQuery check(db_);
        check.prepare("SELECT COUNT(*) FROM pragma_table_info('assets') WHERE name=?");
        check.addBindValue(name);
        execute(check);
        if (check.next() && check.value(0).toInt() == 0) {
            if (!q.exec("ALTER TABLE assets ADD COLUMN " + ddl))
                fail(q.lastError().text());
        }
    };
    ensureColumn(QStringLiteral("folder"), QStringLiteral("folder TEXT NOT NULL DEFAULT ''"));
    ensureColumn(QStringLiteral("format"), QStringLiteral("format TEXT NOT NULL DEFAULT ''"));
    ensureColumn(QStringLiteral("width"), QStringLiteral("width INTEGER NOT NULL DEFAULT 0"));
    ensureColumn(QStringLiteral("height"), QStringLiteral("height INTEGER NOT NULL DEFAULT 0"));
    // P-310: an in-app confirmed deletion sets deleted=1. The asset row (and
    // its tags/meta/provenance) stays for history queries; a tombstone revision
    // marks the deletion point in the timeline.
    ensureColumn(QStringLiteral("deleted"), QStringLiteral("deleted INTEGER NOT NULL DEFAULT 0"));
    // P-306: snapshot entries carry a state (present|excluded; tombstone|missing
    // arrive with P-310/P-311) plus the exclusion reason that produced it.
    const auto ensureSnapshotColumn = [&](const QString &name, const QString &ddl) {
        QSqlQuery check(db_);
        check.prepare("SELECT COUNT(*) FROM pragma_table_info('snapshot_assets') WHERE name=?");
        check.addBindValue(name);
        execute(check);
        if (check.next() && check.value(0).toInt() == 0) {
            if (!q.exec("ALTER TABLE snapshot_assets ADD COLUMN " + ddl))
                fail(q.lastError().text());
        }
    };
    ensureSnapshotColumn(QStringLiteral("state"), QStringLiteral("state TEXT NOT NULL DEFAULT 'present'"));
    ensureSnapshotColumn(QStringLiteral("reason"), QStringLiteral("reason TEXT NOT NULL DEFAULT ''"));
    // P-307: a revision can be pinned (with a reason) so a future retention
    // policy never cleans or overwrites its object. Old libraries lack the
    // columns; every existing revision is simply unpinned by default.
    const auto ensureRevisionColumn = [&](const QString &name, const QString &ddl) {
        QSqlQuery check(db_);
        check.prepare("SELECT COUNT(*) FROM pragma_table_info('revisions') WHERE name=?");
        check.addBindValue(name);
        execute(check);
        if (check.next() && check.value(0).toInt() == 0) {
            if (!q.exec("ALTER TABLE revisions ADD COLUMN " + ddl))
                fail(q.lastError().text());
        }
    };
    ensureRevisionColumn(QStringLiteral("pinned"), QStringLiteral("pinned INTEGER NOT NULL DEFAULT 0"));
    ensureRevisionColumn(QStringLiteral("pin_reason"), QStringLiteral("pin_reason TEXT NOT NULL DEFAULT ''"));
    // P-310: a tombstone revision marks an in-app confirmed deletion. Only
    // deleteAsset writes one; a missing or unreachable working copy never does.
    ensureRevisionColumn(QStringLiteral("tombstone"), QStringLiteral("tombstone INTEGER NOT NULL DEFAULT 0"));
    // Schema/identity metadata. A library predating the meta table, or with an
    // unknown schema version, must not be silently treated as current (P-201).
    QSqlQuery meta(db_);
    meta.prepare("SELECT value FROM meta WHERE key='schema_version'");
    execute(meta);
    if (meta.next()) {
        if (meta.value(0).toString() != QStringLiteral("1"))
            fail(Library::tr("素材库 schema 版本不兼容（%1），请使用匹配版本打开").arg(meta.value(0).toString()));
    } else {
        // Fresh library: stamp identity + version (P-202 foundation).
        QSqlQuery stamp(db_);
        stamp.prepare("INSERT OR REPLACE INTO meta(key,value) VALUES('schema_version','1'),('library_id',?)");
        stamp.addBindValue(QUuid::createUuid().toString(QUuid::WithoutBraces));
        execute(stamp);
    }
}

Library::~Library() {
    db_.close();
    db_ = QSqlDatabase();
    QSqlDatabase::removeDatabase(connection_);
}

BackendInfo Library::backendInfo() const {
    const std::lock_guard<QRecursiveMutex> guard(mutex_);
    // V1 only ever hosts the self-built backend. Git/SVN (read-only) and Time
    // Machine are deferred to V1.x; they must not be assumed writable here.
    return selfBackendInfo();
}

QString Library::libraryId() const {
    const std::lock_guard<QRecursiveMutex> guard(mutex_);
    QSqlQuery q(db_);
    q.prepare("SELECT value FROM meta WHERE key='library_id'");
    execute(q);
    if (q.next()) return q.value(0).toString();
    fail(Library::tr("素材库缺少身份标识（library_id），库可能已损坏"));
}

// --- Tag maintenance (P-213) --------------------------------------------------
// A tag is trimmed; an empty tag is a no-op rather than an error. Mutators
// report how many rows actually changed so the caller can show real results.

QStringList Library::tags(const QString &assetId) const {
    const std::lock_guard<QRecursiveMutex> guard(mutex_);
    QSqlQuery q(db_);
    q.prepare("SELECT tag FROM tags WHERE asset_id=? ORDER BY tag");
    q.addBindValue(assetId);
    execute(q);
    QStringList result;
    while (q.next()) result << q.value(0).toString();
    return result;
}

QStringList Library::assetsWithTag(const QString &tag) const {
    const std::lock_guard<QRecursiveMutex> guard(mutex_);
    QSqlQuery q(db_);
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
    QSqlQuery q(db_);
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
    QSqlQuery q(db_);
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
    db_.transaction();
    for (const QString &id : assetIds) {
        QSqlQuery q(db_);
        q.prepare("INSERT OR IGNORE INTO tags(asset_id,tag) VALUES(?,?)");
        q.addBindValue(id);
        q.addBindValue(value);
        if (!q.exec()) { db_.rollback(); fail(q.lastError().text()); }
        if (q.numRowsAffected() > 0) recordFieldChange(id, QStringLiteral("标签"), QString(), value);
        changed += q.numRowsAffected();
    }
    db_.commit();
    return changed;
}

int Library::removeTagFromAssets(const QStringList &assetIds, const QString &tag) {
    const std::lock_guard<QRecursiveMutex> guard(mutex_);
    const QString value = tag.trimmed();
    if (value.isEmpty() || assetIds.isEmpty()) return 0;
    int changed = 0;
    db_.transaction();
    for (const QString &id : assetIds) {
        QSqlQuery q(db_);
        q.prepare("DELETE FROM tags WHERE asset_id=? AND tag=?");
        q.addBindValue(id);
        q.addBindValue(value);
        if (!q.exec()) { db_.rollback(); fail(q.lastError().text()); }
        if (q.numRowsAffected() > 0) recordFieldChange(id, QStringLiteral("标签"), value, QString());
        changed += q.numRowsAffected();
    }
    db_.commit();
    return changed;
}

int Library::countAssetsWithoutTag(const QStringList &assetIds, const QString &tag) const {
    const std::lock_guard<QRecursiveMutex> guard(mutex_);
    const QString value = tag.trimmed();
    if (value.isEmpty()) return 0;
    int count = 0;
    for (const QString &id : assetIds) {
        QSqlQuery q(db_);
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
        QSqlQuery q(db_);
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
    QSqlQuery q(db_);
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
    QSqlQuery old(db_);
    old.prepare("SELECT rating FROM asset_meta WHERE asset_id=?");
    old.addBindValue(assetId);
    execute(old);
    const QString oldValue = old.next() && !old.value(0).isNull()
        ? QString::number(old.value(0).toInt()) : QString();
    QSqlQuery q(db_);
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
    QSqlQuery q(db_);
    q.prepare("SELECT note FROM asset_meta WHERE asset_id=?");
    q.addBindValue(assetId);
    execute(q);
    return q.next() ? q.value(0).toString() : QString();
}

void Library::setNote(const QString &assetId, const QString &note) {
    const std::lock_guard<QRecursiveMutex> guard(mutex_);
    // P-309: the old note is read before the change (the field history records
    // the actual old value, never an invented one).
    QSqlQuery old(db_);
    old.prepare("SELECT note FROM asset_meta WHERE asset_id=?");
    old.addBindValue(assetId);
    execute(old);
    const QString oldValue = old.next() ? old.value(0).toString() : QString();
    QSqlQuery q(db_);
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
    QSqlQuery q(db_);
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
    QSqlQuery q(db_);
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
    writeFile(dest, readObject(hash));
}

// --- Version-exclusion rules (P-306) -------------------------------------------
// A rule (path / pattern / extension) keeps the matching asset out of snapshots
// taken after the rule exists. It never removes history and is never a deletion.

void Library::addExclusion(const QString &kind, const QString &pattern) {
    const std::lock_guard<QRecursiveMutex> guard(mutex_);
    const QString normalized = exclusionNormalize(kind, pattern);
    if (normalized.isEmpty()) fail(Library::tr("排除规则不能为空"));
    if (kind != QLatin1String("extension") && kind != QLatin1String("path") && kind != QLatin1String("name"))
        fail(Library::tr("未知的排除规则类型"));
    QSqlQuery find(db_);
    find.prepare("SELECT COUNT(*) FROM exclusions WHERE kind=? AND pattern=?");
    find.addBindValue(kind);
    find.addBindValue(normalized);
    execute(find);
    if (find.next() && find.value(0).toInt() > 0) fail(Library::tr("该排除规则已存在"));
    QSqlQuery ins(db_);
    ins.prepare("INSERT INTO exclusions(kind,pattern,created) VALUES(?,?,?)");
    ins.addBindValue(kind);
    ins.addBindValue(normalized);
    ins.addBindValue(QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
    execute(ins);
}

void Library::removeExclusion(const QString &kind, const QString &pattern) {
    const std::lock_guard<QRecursiveMutex> guard(mutex_);
    QSqlQuery del(db_);
    del.prepare("DELETE FROM exclusions WHERE kind=? AND pattern=?");
    del.addBindValue(kind);
    del.addBindValue(exclusionNormalize(kind, pattern));
    execute(del);
}

QVector<Exclusion> Library::exclusions() const {
    const std::lock_guard<QRecursiveMutex> guard(mutex_);
    QSqlQuery q(db_);
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
    QSqlQuery find(db_);
    find.prepare("SELECT COUNT(*) FROM revisions WHERE id=?");
    find.addBindValue(revisionId);
    execute(find);
    if (find.next() && find.value(0).toInt() == 0) fail(Library::tr("版本不存在"));
    QSqlQuery upd(db_);
    upd.prepare("UPDATE revisions SET pinned=1,pin_reason=? WHERE id=?");
    upd.addBindValue(reason.trimmed().isEmpty() ? Library::tr("手动固定") : reason.trimmed());
    upd.addBindValue(revisionId);
    execute(upd);
}

void Library::unpinRevision(qint64 revisionId) {
    const std::lock_guard<QRecursiveMutex> guard(mutex_);
    QSqlQuery upd(db_);
    upd.prepare("UPDATE revisions SET pinned=0,pin_reason='' WHERE id=?");
    upd.addBindValue(revisionId);
    execute(upd);
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
        QSqlQuery ins(db_);
        ins.prepare("INSERT INTO revisions(asset_id,hash,created,note,size,pinned,pin_reason) VALUES(?,?,?,?,?,0,'')");
        ins.addBindValue(assetId);
        ins.addBindValue(history.first().hash);
        ins.addBindValue(QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
        ins.addBindValue(QStringLiteral("元数据变更：%1").arg(field));
        ins.addBindValue(history.first().size);
        execute(ins);
        revisionId = ins.lastInsertId().toLongLong();
    }
    QSqlQuery rec(db_);
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
    QSqlQuery q(db_);
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
    QSqlQuery find(db_);
    find.prepare("SELECT name,folder,path FROM assets WHERE id=?");
    find.addBindValue(assetId);
    execute(find);
    if (!find.next()) fail(Library::tr("找不到素材"));
    const QString folder = find.value(1).toString();
    // Same-folder name clash: refuse and say so; two assets never merge.
    QSqlQuery clash(db_);
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
    QSqlQuery update(db_);
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
    QSqlQuery find(db_);
    find.prepare("SELECT name,folder FROM assets WHERE id=?");
    find.addBindValue(assetId);
    execute(find);
    if (!find.next()) fail(Library::tr("找不到素材"));
    if (find.value(1).toString() == folder) return; // already there: no-op
    // Same-folder name clash after the move: refuse, never merge identities.
    QSqlQuery clash(db_);
    clash.prepare("SELECT id FROM assets WHERE folder=? AND name=? AND id<>?");
    clash.addBindValue(folder);
    clash.addBindValue(find.value(0).toString());
    clash.addBindValue(assetId);
    execute(clash);
    if (clash.next())
        fail(Library::tr("目标文件夹已有同名素材：%1，不会合并").arg(find.value(0).toString()));
    QSqlQuery update(db_);
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
    QSqlQuery q(db_);
    q.prepare("SELECT format FROM assets WHERE id=?");
    q.addBindValue(assetId);
    execute(q);
    return q.next() ? q.value(0).toString() : QString();
}

QSize Library::assetSize(const QString &assetId) const {
    const std::lock_guard<QRecursiveMutex> guard(mutex_);
    QSqlQuery q(db_);
    q.prepare("SELECT width,height FROM assets WHERE id=?");
    q.addBindValue(assetId);
    execute(q);
    if (!q.next()) return QSize();
    return QSize(q.value(0).toInt(), q.value(1).toInt());
}

QString Library::peekLibraryId(const QString &root) {
    const QString catalog = QDir(root).absoluteFilePath("catalog.sqlite");
    if (!QFileInfo::exists(catalog) || !QFileInfo(catalog).isReadable())
        fail(Library::tr("该路径不是有效的素材库（未找到可读的 catalog.sqlite）"));
    // A short-lived read-only connection; deliberately never takes library.lock,
    // so relocation validation cannot block on a running library.
    const QString connection = QStringLiteral("peek_") + QUuid::createUuid().toString(QUuid::WithoutBraces);
    QString id;
    {
        auto db = QSqlDatabase::addDatabase("QSQLITE", connection);
        db.setDatabaseName(catalog);
        if (db.open()) {
            QSqlQuery q(db);
            if (q.exec("SELECT value FROM meta WHERE key='library_id'") && q.next())
                id = q.value(0).toString();
            db.close();
        }
    }
    QSqlDatabase::removeDatabase(connection);
    if (id.isEmpty()) fail(Library::tr("该素材库缺少身份标识（library_id），库可能已损坏"));
    return id;
}

QVector<Asset> Library::assets() const {
    const std::lock_guard<QRecursiveMutex> guard(mutex_);
    QSqlQuery q(db_);
    // P-310: live assets only. A deleted asset keeps its row (and history) for
    // timeline queries but is never listed as a live entry; it stays reachable
    // through the tombstone revision's asset_id.
    q.prepare("SELECT id,name,path,folder FROM assets WHERE deleted=0 ORDER BY name,id");
    execute(q);
    QVector<Asset> result;
    while (q.next()) {
        const QString stored = q.value(2).toString();
        Asset asset{q.value(0).toString(), q.value(1).toString(), QString(), stored, q.value(3).toString()};
        // Relative paths resolve against the current root; a pre-relative-path
        // library (stored absolute paths) keeps working unchanged.
        asset.path = QFileInfo(stored).isAbsolute() ? stored : root_ + "/" + stored;
        result.push_back(asset);
    }
    return result;
}

QVector<Revision> Library::revisions(const QString &assetId) const {
    const std::lock_guard<QRecursiveMutex> guard(mutex_);
    QSqlQuery q(db_);
    q.prepare("SELECT id,hash,created,note,size,pinned,pin_reason,tombstone FROM revisions WHERE asset_id=? ORDER BY id DESC");
    q.addBindValue(assetId);
    execute(q);
    QVector<Revision> result;
    while (q.next()) result.push_back({q.value(0).toLongLong(), q.value(1).toString(),
        q.value(2).toString(), q.value(3).toString(), q.value(4).toLongLong(),
        q.value(5).toInt() != 0, q.value(6).toString(), q.value(7).toInt() != 0});
    return result;
}

QString Library::objectPath(const QString &hash) const {
    const std::lock_guard<QRecursiveMutex> guard(mutex_);
    if (hash.size() != 64) fail(Library::tr("无效的对象哈希"));
    for (const auto c : hash)
        if (!QStringLiteral("0123456789abcdef").contains(c)) fail(Library::tr("无效的对象哈希"));
    return root_ + "/objects/" + hash;
}

QByteArray Library::readObject(const QString &hash) const {
    const std::lock_guard<QRecursiveMutex> guard(mutex_);
    const auto bytes = readFile(objectPath(hash));
    if (digest(bytes) != hash) fail(Library::tr("历史对象校验失败；已停止操作"));
    return bytes;
}

void Library::append(const Asset &asset, const QByteArray &bytes, const QString &note) {
    const QString hash = storeObject(bytes);
    recordRevision(asset, hash, bytes.size(), note);
}

QString Library::storeObject(const QByteArray &bytes) {
    const QString hash = digest(bytes);
    const QString path = objectPath(hash);
    if (QFileInfo::exists(path)) {
        if (digestFile(path) != hash) fail(Library::tr("历史对象校验失败；已停止操作"));
        platform::makeImmutable(path); // best-effort + idempotent
        return hash;
    }
    const QString temp = tempObjectPath();
    try {
        writeFile(temp, bytes);
        publishObject(temp, hash);
    } catch (...) {
        QFile::remove(temp); // never leave a stray temp; never publish partial content
        throw;
    }
    return hash;
}

QString Library::storeObjectFromFile(const QString &sourcePath, const QString &hash) {
    const QString path = objectPath(hash);
    if (QFileInfo::exists(path)) {
        if (digestFile(path) != hash) fail(Library::tr("历史对象校验失败；已停止操作"));
        platform::makeImmutable(path);
        return hash;
    }
    const QString temp = tempObjectPath();
    try {
        copyFileStreaming(sourcePath, temp);
        publishObject(temp, hash);
    } catch (...) {
        QFile::remove(temp);
        throw;
    }
    return hash;
}

void Library::publishObject(const QString &tempPath, const QString &hash) {
    // P-302 order: fully verify the staged bytes hash to the address, then
    // publish by rename. If the object already appeared (another capture raced
    // us), verify it and drop the temp instead of overwriting.
    if (digestFile(tempPath) != hash) fail(Library::tr("历史对象校验失败；已停止操作"));
    const QString path = objectPath(hash);
    if (QFileInfo::exists(path)) {
        if (digestFile(path) != hash) fail(Library::tr("历史对象校验失败；已停止操作"));
        QFile::remove(tempPath);
    } else if (!QFile::rename(tempPath, path)) {
        fail(Library::tr("无法发布历史对象"));
    }
    // Append-only store: once an object lands it must resist later tampering by
    // a same-user ransomware process. Best-effort + idempotent (chattr +i where
    // the OS allows it, read-only mode bits otherwise).
    platform::makeImmutable(path);
}

QString Library::tempObjectPath() const {
    const QString dir = root_ + "/objects/.tmp";
    if (!QDir().mkpath(dir)) fail(Library::tr("无法创建对象暂存目录"));
    return dir + "/" + QUuid::createUuid().toString(QUuid::WithoutBraces);
}

qint64 Library::recordRevision(const Asset &asset, const QString &hash, qint64 size, const QString &note) {
    QSqlQuery q(db_);
    q.prepare("INSERT INTO revisions(asset_id,hash,created,note,size) VALUES(?,?,?,?,?)");
    q.addBindValue(asset.id);
    q.addBindValue(hash);
    q.addBindValue(QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
    q.addBindValue(note.isEmpty() ? QStringLiteral("手动快照") : note);
    q.addBindValue(size);
    execute(q);
    const qint64 revisionId = q.lastInsertId().toLongLong();
    // P-217: refresh the asset's filtering facts from this new revision. Header
    // only; an undecodable content leaves the format empty and size 0 (unknown).
    // Content detection is authoritative (no name-extension hint): the asset
    // name can be stale after a format-changing capture, and the facts must
    // reflect the true committed content.
    const auto meta = preview::metadata(objectPath(hash));
    QSqlQuery facts(db_);
    facts.prepare("UPDATE assets SET format=?,width=?,height=? WHERE id=?");
    // A null QString binds as SQL NULL and the NOT NULL column rejects it.
    facts.addBindValue(meta.format.isEmpty() ? QStringLiteral("") : meta.format.toUpper());
    facts.addBindValue(meta.width);
    facts.addBindValue(meta.height);
    facts.addBindValue(asset.id);
    execute(facts);
    return revisionId;
}

Asset Library::importFile(const QString &source) {
    const std::lock_guard<QRecursiveMutex> guard(mutex_);
    const auto bytes = readFile(source);
    const QFileInfo info(source);
    const QString id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    // Persist a root-relative path so the library stays relocatable (P-202);
    // the absolute working-file path is derived from the current root.
    // New imports start at the root folder (P-216: organization happens later).
    Asset asset{id, info.fileName(), QString(),
                QStringLiteral("working/%1/%2").arg(id, info.fileName()), QString()};
    asset.path = root_ + "/" + asset.relative;
    const QString workingDir = QFileInfo(asset.path).absolutePath();
    if (!QDir().mkpath(workingDir)) fail(Library::tr("无法创建工作目录"));
    // Copy import order (P-203): stable read (bounded, whole-file) -> atomic
    // write of the working copy -> hash check before registration -> original
    // untouched. A pre-registration failure must not leave a half-written asset.
    try {
        writeFile(asset.path, bytes);
    } catch (...) {
        QDir().rmdir(workingDir); // best-effort; empty dir only, never the source
        throw;
    }
    if (!db_.transaction()) fail(db_.lastError().text());
    try {
        QSqlQuery q(db_);
        q.prepare("INSERT INTO assets(id,name,path,folder) VALUES(?,?,?,?)");
        q.addBindValue(asset.id); q.addBindValue(asset.name); q.addBindValue(asset.relative);
        // A null QString binds as SQL NULL; the root folder is stored as ''.
        q.addBindValue(asset.folder.isNull() ? QStringLiteral("") : asset.folder);
        execute(q);
        append(asset, bytes, QStringLiteral("首次导入"));
        if (!db_.commit()) fail(db_.lastError().text());
    } catch (...) { db_.rollback(); throw; }
    return asset;
}

bool Library::capture(const Asset &asset, const QString &note) {
    const std::lock_guard<QRecursiveMutex> guard(mutex_);
    bool created = false;
    captureRevision(asset, note, &created);
    return created;
}

qint64 Library::captureRevision(const Asset &asset, const QString &note, bool *created) {
    const std::lock_guard<QRecursiveMutex> guard(mutex_);
    // P-302: stream the working copy straight into the object store so a large
    // file never sits whole in memory; the content hash is computed once and
    // reused for the dedup check and the staged-object verification.
    const QString hash = digestFile(asset.path);
    const auto history = revisions(asset.id);
    if (!history.isEmpty() && history.first().hash == hash) {
        // Same content (P-303): no duplicate revision; report the current one
        // so the user gets an explicit "content unchanged" answer.
        if (created) *created = false;
        return history.first().id;
    }
    storeObjectFromFile(asset.path, hash);
    const qint64 revisionId = recordRevision(asset, hash, QFileInfo(asset.path).size(), note);
    if (created) *created = true;
    return revisionId;
}

bool Library::captureIfStable(const Asset &asset, const QString &note) {
    const std::lock_guard<QRecursiveMutex> guard(mutex_);
    // P-304: the heuristic already required the (size, mtime) signature to be
    // stable across two scans; this adds the read-time re-check so a save that
    // straddles the read is never recorded as a torn snapshot. The file is
    // re-statted after the read; if it moved, the caller retries next scan.
    const QFileInfo before(asset.path);
    if (!before.exists()) return false;
    const auto sig = [](const QFileInfo &info) {
        return qMakePair(info.size(), info.lastModified().toMSecsSinceEpoch());
    };
    const auto beforeSig = sig(before);
    const QString hash = digestFile(asset.path);
    if (sig(QFileInfo(asset.path)) != beforeSig) return false; // unstable: retry later
    const auto history = revisions(asset.id);
    if (!history.isEmpty() && history.first().hash == hash) return false; // unchanged
    storeObjectFromFile(asset.path, hash);
    recordRevision(asset, hash, QFileInfo(asset.path).size(), note);
    return true;
}

void Library::restore(const Asset &asset, qint64 revisionId) {
    const std::lock_guard<QRecursiveMutex> guard(mutex_);
    const auto history = revisions(asset.id);
    for (const auto &revision : history) {
        if (revision.id != revisionId) continue;
        const auto bytes = readObject(revision.hash); // Verify before touching the working file.
        const bool existed = QFileInfo::exists(asset.path);
        QByteArray previous;
        if (existed) previous = readFile(asset.path);
        if (!db_.transaction()) fail(db_.lastError().text());
        bool replaced = false;
        try {
            const auto currentHistory = revisions(asset.id);
            if (existed && (currentHistory.isEmpty() || currentHistory.first().hash != digest(previous)))
                append(asset, previous, QStringLiteral("恢复前自动保存"));
            append(asset, bytes, QStringLiteral("恢复历史版本 #%1").arg(revisionId));
            if (!QDir().mkpath(QFileInfo(asset.path).absolutePath())) fail(Library::tr("无法创建恢复目录"));
            writeFile(asset.path, bytes);
            replaced = true;
            if (!db_.commit()) fail(db_.lastError().text());
        } catch (...) {
            db_.rollback();
            if (replaced && existed) writeFile(asset.path, previous);
            else if (replaced) QFile::remove(asset.path);
            throw;
        }
        return;
    }
    fail(Library::tr("找不到属于当前素材的历史版本"));
}

void Library::deleteAsset(const QString &assetId) {
    const std::lock_guard<QRecursiveMutex> guard(mutex_);
    QSqlQuery find(db_);
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
    if (!db_.transaction()) fail(db_.lastError().text());
    try {
        QSqlQuery ins(db_);
        ins.prepare("INSERT INTO revisions(asset_id,hash,created,note,size,pinned,pin_reason,tombstone) VALUES(?,?,?,?,?,0,'',1)");
        ins.addBindValue(assetId);
        ins.addBindValue(hash);
        ins.addBindValue(QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
        ins.addBindValue(QStringLiteral("删除"));
        ins.addBindValue(size);
        execute(ins);
        QSqlQuery up(db_);
        up.prepare("UPDATE assets SET deleted=1 WHERE id=?");
        up.addBindValue(assetId);
        execute(up);
        if (!db_.commit()) fail(db_.lastError().text());
    } catch (...) {
        db_.rollback();
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

void Library::exportRevision(const QString &assetId, qint64 revisionId, const QString &destination) const {
    const std::lock_guard<QRecursiveMutex> guard(mutex_);
    // Never export into the managed library (would permit overwriting immutable objects).
    const QString dest = QFileInfo(destination).absoluteFilePath();
    const QString parent = QFileInfo(dest).dir().canonicalPath();
    const QString canonicalRoot = QDir(root_).canonicalPath();
    if (parent.isEmpty() || parent == canonicalRoot || parent.startsWith(canonicalRoot + '/'))
        fail(Library::tr("请导出到素材库之外的现有目录"));
    for (const auto &revision : revisions(assetId)) {
        if (revision.id == revisionId) { writeFile(dest, readObject(revision.hash)); return; }
    }
    fail(Library::tr("找不到历史版本"));
}

int Library::hardenObjects() {
    const std::lock_guard<QRecursiveMutex> guard(mutex_);
    const QDir dir(root_ + "/objects");
    int hardened = 0;
    const auto entries = dir.entryInfoList(QDir::Files | QDir::NoDotAndDotDot, QDir::Name);
    for (const auto &info : entries) {
        if (platform::isImmutable(info.absoluteFilePath())) continue;
        if (platform::makeImmutable(info.absoluteFilePath())) ++hardened;
    }
    return hardened;
}

qint64 Library::snapshotAll(const QString &note) {
    const std::lock_guard<QRecursiveMutex> guard(mutex_);
    if (!db_.transaction()) fail(db_.lastError().text());
    qint64 snapshotId = 0;
    try {
        QSqlQuery q(db_);
        q.prepare("INSERT INTO snapshots(created,note) VALUES(?,?)");
        q.addBindValue(QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
        q.addBindValue(note.isEmpty() ? QStringLiteral("紧急快照") : note);
        execute(q);
        snapshotId = q.lastInsertId().toLongLong();
        const auto all = assets();
        const auto rules = exclusions();
        for (const auto &asset : all) {
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
                QSqlQuery ins(db_);
                ins.prepare("INSERT OR REPLACE INTO snapshot_assets(snapshot_id,asset_id,revision_id,state,reason) VALUES(?,?,?,?,?)");
                ins.addBindValue(snapshotId);
                ins.addBindValue(asset.id);
                ins.addBindValue(0);
                ins.addBindValue(QStringLiteral("excluded"));
                ins.addBindValue(hit);
                execute(ins);
                continue;
            }
            const auto history = revisions(asset.id);
            QFile file(asset.path);
            QByteArray current;
            if (file.exists() && file.open(QIODevice::ReadOnly)) {
                current = file.readAll();
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
            const QString currentHash = current.isEmpty() ? QString() : digest(current);
            qint64 anchor = 0;
            for (const auto &rev : history) { // newest first
                if (current.isEmpty() || rev.hash != currentHash) { anchor = rev.id; break; }
            }
            if (!anchor && !history.isEmpty()) anchor = history.first().id;
            if (!current.isEmpty() && (history.isEmpty() || history.first().hash != currentHash))
                append(asset, current, QStringLiteral("紧急快照锚点"));
            if (!anchor) continue; // no clean history to recover to
            QSqlQuery ins(db_);
            ins.prepare("INSERT OR REPLACE INTO snapshot_assets(snapshot_id,asset_id,revision_id,state,reason) VALUES(?,?,?,?,?)");
            ins.addBindValue(snapshotId);
            ins.addBindValue(asset.id);
            ins.addBindValue(anchor);
            ins.addBindValue(QStringLiteral("present"));
            ins.addBindValue(QString());
            execute(ins);
        }
        if (!db_.commit()) fail(db_.lastError().text());
    } catch (...) {
        db_.rollback();
        throw;
    }
    return snapshotId;
}

QVector<Snapshot> Library::snapshots() const {
    const std::lock_guard<QRecursiveMutex> guard(mutex_);
    QSqlQuery q(db_);
    q.prepare("SELECT s.id,s.created,s.note,"
              // NULL-safe: a LEFT JOIN row for a snapshot with no entries has
              // state=NULL and must not be counted as present (P-310 empty tree).
              "SUM(CASE WHEN sa.state IS NOT NULL AND sa.state!='excluded' THEN 1 ELSE 0 END),"
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

int Library::rollbackAll(qint64 snapshotId) {
    const std::lock_guard<QRecursiveMutex> guard(mutex_);
    QHash<QString, Asset> byId;
    for (const auto &asset : assets()) byId.insert(asset.id, asset);
    QSqlQuery q(db_);
    // P-306: an excluded entry has no anchor (revision_id 0) and is never a
    // deletion — rollback only touches present entries, so a restore can never
    // damage an excluded asset's current working file.
    q.prepare("SELECT asset_id, revision_id FROM snapshot_assets WHERE snapshot_id=? AND state='present'");
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
