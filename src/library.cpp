// Library lifecycle and version core: construction/open/schema stamping,
// per-thread connections, catalog listing, content dedup query, append and
// the manual/auto version record paths (LAY-002 split; object storage,
// metadata, snapshots, restore, backup and external indexing live in their
// own library_*.cpp files).
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

// P-302 streaming helpers: a large file is hashed and copied in bounded chunks
// instead of being read whole into memory. `hashFile` is the sole SHA-256
// streaming implementation (shared across TUs via library.h); it reports the
// byte count actually consumed so a caller can record the exact size of the
// content it hashed instead of re-stat'ing the (possibly changed) path. It
// returns false on an open/read failure instead of throwing, so a caller that
// wants graceful degradation (a mount drop, a file deleted between stat and
// open) can fall back without aborting a whole batch.
bool hashFile(const QString &path, QString *hash, qint64 *size) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return false;
    QCryptographicHash h(QCryptographicHash::Sha256);
    constexpr qint64 kChunk = 1 << 20; // 1 MiB
    qint64 total = 0;
    while (!file.atEnd()) {
        const auto chunk = file.read(kChunk);
        if (chunk.isEmpty() && file.error() != QFileDevice::NoError) return false;
        h.addData(chunk);
        total += chunk.size();
    }
    if (file.error() != QFileDevice::NoError) return false;
    if (hash) *hash = QString::fromLatin1(h.result().toHex());
    if (size) *size = total;
    return true;
}
QString digestFileBounded(const QString &path) {
    QString hash;
    if (!hashFile(path, &hash, nullptr))
        fail(Library::tr("无法读取文件：%1").arg(path));
    return hash;
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
    // P-320: an interrupted restore (power loss / kill between the file write
    // and the index commit) is converged here — completed or rolled back — so a
    // half-restored state is never silently left behind.
    convergeRestoreLog();
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
             QStringLiteral("CREATE TABLE IF NOT EXISTS field_history(id INTEGER PRIMARY KEY AUTOINCREMENT,asset_id TEXT NOT NULL REFERENCES assets(id),revision_id INTEGER NOT NULL DEFAULT 0,field TEXT NOT NULL,old_value TEXT NOT NULL DEFAULT '',new_value TEXT NOT NULL DEFAULT '',changed TEXT NOT NULL)"),
             // P-320: durable restore log. A restore is split into stages
             // (protect -> log -> replace file -> commit index); the log row is
             // committed BEFORE the working file is replaced, so an interrupted
             // restore (power loss / kill) is detected and converged on reopen.
             // state: planned | done | failed | aborted | conflict.
             QStringLiteral("CREATE TABLE IF NOT EXISTS restore_log(id INTEGER PRIMARY KEY AUTOINCREMENT,asset_id TEXT NOT NULL,revision_id INTEGER NOT NULL,state TEXT NOT NULL,previous_hash TEXT NOT NULL DEFAULT '',target_hash TEXT NOT NULL DEFAULT '',protect_revision_id INTEGER NOT NULL DEFAULT 0,error TEXT NOT NULL DEFAULT '',created TEXT NOT NULL,updated TEXT NOT NULL)"),
             // P-204: external directories indexed in place. Rows describe
             // files OUTSIDE the library; the indexed tree is only read,
             // never copied or written. Identity is (root, rel_path); state:
             // present | missing | unreadable.
             QStringLiteral("CREATE TABLE IF NOT EXISTS external_roots(id INTEGER PRIMARY KEY AUTOINCREMENT,path TEXT NOT NULL UNIQUE,added TEXT NOT NULL,pass INTEGER NOT NULL DEFAULT 0)"),
             QStringLiteral("CREATE TABLE IF NOT EXISTS external_assets(root_id INTEGER NOT NULL REFERENCES external_roots(id),rel_path TEXT NOT NULL,hash TEXT NOT NULL DEFAULT '',size INTEGER NOT NULL DEFAULT 0,mtime INTEGER NOT NULL DEFAULT 0,state TEXT NOT NULL DEFAULT 'present',last_seen INTEGER NOT NULL DEFAULT 0,PRIMARY KEY(root_id,rel_path))")}) {
        if (!q.exec(sql)) fail(q.lastError().text());
    }
    // P-216/P-217: assets gain a logical folder column plus filtering facts
    // (detected format, pixel size). CREATE IF NOT EXISTS does not alter an
    // existing table, so add each column when it is missing (existing rows keep
    // the root folder "" and unknown format/size).
    const auto ensureColumn = [&](const QString &name, const QString &ddl) {
        QSqlQuery check(db());
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
        QSqlQuery check(db());
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
    // P-311: the manifest denormalizes the path and content hash as of the
    // snapshot, so a later rename/move/delete never rewrites what a snapshot
    // shows (P-312 reads exactly these). Older snapshots default to '' and fall
    // back to the asset's current recorded location.
    ensureSnapshotColumn(QStringLiteral("folder"), QStringLiteral("folder TEXT NOT NULL DEFAULT ''"));
    ensureSnapshotColumn(QStringLiteral("name"), QStringLiteral("name TEXT NOT NULL DEFAULT ''"));
    ensureSnapshotColumn(QStringLiteral("hash"), QStringLiteral("hash TEXT NOT NULL DEFAULT ''"));
    // P-307: a revision can be pinned (with a reason) so a future retention
    // policy never cleans or overwrites its object. Old libraries lack the
    // columns; every existing revision is simply unpinned by default.
    const auto ensureRevisionColumn = [&](const QString &name, const QString &ddl) {
        QSqlQuery check(db());
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
        if (meta.value(0).toString() != QStringLiteral("1")) {
            // Close the already-open handle before the exception propagates to
            // the constructor catch (which only resets + removeDatabase), so an
            // incompatible catalog never leaks an open SQLite connection.
            db_.close();
            fail(Library::tr("素材库 schema 版本不兼容（%1），请使用匹配版本打开").arg(meta.value(0).toString()));
        }
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

QSqlDatabase &Library::db() const {
    if (QThread::currentThread() == QCoreApplication::instance()->thread())
        return db_;
    // A worker thread: get (or lazily create) its own connection, stored in
    // QThreadStorage so it is cleaned up when that worker exits. The library is
    // opened and validated by the owner before any worker runs; a worker only
    // ever sees an already-initialized catalog.
    if (!threadDbs_.hasLocalData()) {
        // Unique name per thread; never reuses a thread id, so a recycled id
        // (a later worker) cannot collide with a dead thread's connection.
        const QString name = connection_ + QStringLiteral("_w%1").arg(
            QUuid::createUuid().toString(QUuid::WithoutBraces));
        QSqlDatabase worker = QSqlDatabase::addDatabase("QSQLITE", name);
        worker.setDatabaseName(root_ + "/catalog.sqlite");
        if (!worker.open()) {
            const QString error = worker.lastError().text();
            QSqlDatabase::removeDatabase(name);
            fail(Library::tr("后台连接无法打开：%1").arg(error));
        }
        QSqlQuery pragma(worker);
        if (!pragma.exec("PRAGMA foreign_keys=ON"))
            fail(Library::tr("后台连接初始化失败：%1").arg(pragma.lastError().text()));
        WorkerDb entry;
        entry.db = worker;
        entry.name = name;
        threadDbs_.setLocalData(entry);
        // Close + unregister when this worker thread exits, so a recycled
        // thread id never reuses a dead thread's connection and no name leaks.
        QThread *self = QThread::currentThread();
        QObject::connect(self, &QThread::finished, self, [this, name]() {
            if (QSqlDatabase::contains(name)) {
                QSqlDatabase db = QSqlDatabase::database(name, false);
                if (db.isOpen()) db.close();
                QSqlDatabase::removeDatabase(name);
            }
        });
    }
    return threadDbs_.localData().db;
}

BackendInfo Library::backendInfo() const {
    const std::lock_guard<QRecursiveMutex> guard(mutex_);
    // V1 only ever hosts the self-built backend. Git/SVN (read-only) and Time
    // Machine are deferred to V1.x; they must not be assumed writable here.
    return selfBackendInfo();
}

QString Library::libraryId() const {
    const std::lock_guard<QRecursiveMutex> guard(mutex_);
    QSqlQuery q(db());
    q.prepare("SELECT value FROM meta WHERE key='library_id'");
    execute(q);
    if (q.next()) return q.value(0).toString();
    fail(Library::tr("素材库缺少身份标识（library_id），库可能已损坏"));
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
        }
        // Close on BOTH paths: a successful open must release the handle before
        // removeDatabase, and a failed open may still have registered a handle
        // that Qt warns about if removed while "in use".
        if (db.isOpen()) db.close();
    }
    QSqlDatabase::removeDatabase(connection);
    if (id.isEmpty()) fail(Library::tr("该素材库缺少身份标识（library_id），库可能已损坏"));
    return id;
}

QVector<Asset> Library::assets() const {
    const std::lock_guard<QRecursiveMutex> guard(mutex_);
    QSqlQuery q(db());
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
    QSqlQuery q(db());
    q.prepare("SELECT id,hash,created,note,size,pinned,pin_reason,tombstone FROM revisions WHERE asset_id=? ORDER BY id DESC");
    q.addBindValue(assetId);
    execute(q);
    QVector<Revision> result;
    while (q.next()) result.push_back({q.value(0).toLongLong(), q.value(1).toString(),
        q.value(2).toString(), q.value(3).toString(), q.value(4).toLongLong(),
        q.value(5).toInt() != 0, q.value(6).toString(), q.value(7).toInt() != 0});
    return result;
}

bool Library::hasContent(const QString &hexHash) const {
    const std::lock_guard<QRecursiveMutex> guard(mutex_);
    QSqlQuery q(db());
    q.prepare("SELECT COUNT(*) FROM revisions WHERE hash=?");
    q.addBindValue(hexHash);
    execute(q);
    return q.next() && q.value(0).toInt() > 0;
}


void Library::append(const Asset &asset, const QByteArray &bytes, const QString &note) {
    const QString hash = storeObject(bytes);
    recordRevision(asset, hash, bytes.size(), note);
}


qint64 Library::recordRevision(const Asset &asset, const QString &hash, qint64 size, const QString &note) {
    QSqlQuery q(db());
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
    QSqlQuery facts(db());
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
    if (!db().transaction()) fail(db().lastError().text());
    try {
        QSqlQuery q(db());
        q.prepare("INSERT INTO assets(id,name,path,folder) VALUES(?,?,?,?)");
        q.addBindValue(asset.id); q.addBindValue(asset.name); q.addBindValue(asset.relative);
        // A null QString binds as SQL NULL; the root folder is stored as ''.
        q.addBindValue(asset.folder.isNull() ? QStringLiteral("") : asset.folder);
        execute(q);
        append(asset, bytes, QStringLiteral("首次导入"));
        if (!db().commit()) fail(db().lastError().text());
    } catch (...) {
        db().rollback();
        // P-203: a pre-registration failure must not leave a half-written asset.
        // The working copy is removed (and its now-empty directory) so no orphan
        // with an assets row is left behind; the content-addressed object is
        // shared and would be reported by the consistency check, not leaked here.
        QFile::remove(asset.path);
        QDir().rmdir(workingDir);
        throw;
    }
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

