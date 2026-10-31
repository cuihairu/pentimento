#include "library.h"

#include "platform_immutable.h"

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
    if (!file.open(QIODevice::ReadOnly)) fail(QStringLiteral("无法读取文件：%1").arg(path));
    // Bounded in-memory first release; do not attempt huge PSD/model imports.
    if (file.size() > 256LL * 1024 * 1024) fail(QStringLiteral("首版单文件上限为 256 MiB：%1").arg(path));
    const auto bytes = file.readAll();
    if (file.error() != QFileDevice::NoError) fail(file.errorString());
    return bytes;
}
void writeFile(const QString &path, const QByteArray &bytes) {
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit())
        fail(QStringLiteral("无法安全写入文件：%1；%2").arg(path, file.errorString()));
}
void execute(QSqlQuery &query) {
    if (!query.exec()) fail(query.lastError().text());
}
}

Library::Library(const QString &root)
    : root_(QDir(root).absolutePath()), connection_(QUuid::createUuid().toString()) {
    if (!QDir().mkpath(root_ + "/objects") || !QDir().mkpath(root_ + "/working"))
        fail(QStringLiteral("无法创建素材库目录"));
    lock_ = std::make_unique<QLockFile>(root_ + "/library.lock");
    if (!lock_->tryLock(0)) fail(QStringLiteral("素材库正在被另一个进程使用"));
    db_ = QSqlDatabase::addDatabase("QSQLITE", connection_);
    db_.setDatabaseName(root_ + "/catalog.sqlite");
    if (!db_.open()) {
        const QString error = db_.lastError().text();
        db_ = QSqlDatabase();
        QSqlDatabase::removeDatabase(connection_);
        fail(error);
    }
    QSqlQuery q(db_);
    for (const QString &sql : {
             QStringLiteral("PRAGMA foreign_keys=ON"),
             QStringLiteral("CREATE TABLE IF NOT EXISTS assets(id TEXT PRIMARY KEY,name TEXT NOT NULL,path TEXT NOT NULL)"),
             QStringLiteral("CREATE TABLE IF NOT EXISTS revisions(id INTEGER PRIMARY KEY AUTOINCREMENT,asset_id TEXT NOT NULL REFERENCES assets(id),hash TEXT NOT NULL,created TEXT NOT NULL,note TEXT NOT NULL,size INTEGER NOT NULL)"),
             QStringLiteral("CREATE INDEX IF NOT EXISTS revisions_asset ON revisions(asset_id,id)"),
             QStringLiteral("CREATE TABLE IF NOT EXISTS snapshots(id INTEGER PRIMARY KEY AUTOINCREMENT,created TEXT NOT NULL,note TEXT NOT NULL)"),
             QStringLiteral("CREATE TABLE IF NOT EXISTS snapshot_assets(snapshot_id INTEGER NOT NULL REFERENCES snapshots(id),asset_id TEXT NOT NULL REFERENCES assets(id),revision_id INTEGER NOT NULL,PRIMARY KEY(snapshot_id,asset_id))")}) {
        if (!q.exec(sql)) fail(q.lastError().text());
    }
    // Opening an existing library must re-apply immutability to any object that
    // a tamperer left writable (idempotent, best-effort). Documented promise in
    // 85-antiransom.md: hardenObjects() is for hardening existing objects on open.
    hardenObjects();
}

Library::~Library() {
    db_.close();
    db_ = QSqlDatabase();
    QSqlDatabase::removeDatabase(connection_);
}

QVector<Asset> Library::assets() const {
    QSqlQuery q(db_);
    q.prepare("SELECT id,name,path FROM assets ORDER BY name,id");
    execute(q);
    QVector<Asset> result;
    while (q.next()) result.push_back({q.value(0).toString(), q.value(1).toString(), q.value(2).toString()});
    return result;
}

QVector<Revision> Library::revisions(const QString &assetId) const {
    QSqlQuery q(db_);
    q.prepare("SELECT id,hash,created,note,size FROM revisions WHERE asset_id=? ORDER BY id DESC");
    q.addBindValue(assetId);
    execute(q);
    QVector<Revision> result;
    while (q.next()) result.push_back({q.value(0).toLongLong(), q.value(1).toString(),
        q.value(2).toString(), q.value(3).toString(), q.value(4).toLongLong()});
    return result;
}

QString Library::objectPath(const QString &hash) const {
    if (hash.size() != 64) fail(QStringLiteral("无效的对象哈希"));
    for (const auto c : hash)
        if (!QStringLiteral("0123456789abcdef").contains(c)) fail(QStringLiteral("无效的对象哈希"));
    return root_ + "/objects/" + hash;
}

QByteArray Library::readObject(const QString &hash) const {
    const auto bytes = readFile(objectPath(hash));
    if (digest(bytes) != hash) fail(QStringLiteral("历史对象校验失败；已停止操作"));
    return bytes;
}

void Library::append(const Asset &asset, const QByteArray &bytes, const QString &note) {
    const QString hash = digest(bytes);
    const QString path = objectPath(hash);
    if (!QFileInfo::exists(path)) {
        writeFile(path, bytes);
    } else if (readObject(hash) != bytes) {
        fail(QStringLiteral("历史对象内容不一致"));
    }
    // The version store is append-only: once an object lands it must resist
    // later tampering by a same-user ransomware process. Best-effort + idempotent
    // (chattr +i where the OS allows it, read-only mode bits otherwise).
    platform::makeImmutable(path);
    QSqlQuery q(db_);
    q.prepare("INSERT INTO revisions(asset_id,hash,created,note,size) VALUES(?,?,?,?,?)");
    q.addBindValue(asset.id);
    q.addBindValue(hash);
    q.addBindValue(QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
    q.addBindValue(note.isEmpty() ? QStringLiteral("手动快照") : note);
    q.addBindValue(bytes.size());
    execute(q);
}

Asset Library::importFile(const QString &source) {
    const auto bytes = readFile(source);
    const QFileInfo info(source);
    const QString id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    Asset asset{id, info.fileName(), root_ + "/working/" + id + "/" + info.fileName()};
    if (!QDir().mkpath(QFileInfo(asset.path).absolutePath())) fail(QStringLiteral("无法创建工作目录"));
    writeFile(asset.path, bytes);
    if (!db_.transaction()) fail(db_.lastError().text());
    try {
        QSqlQuery q(db_);
        q.prepare("INSERT INTO assets VALUES(?,?,?)");
        q.addBindValue(asset.id); q.addBindValue(asset.name); q.addBindValue(asset.path);
        execute(q);
        append(asset, bytes, QStringLiteral("首次导入"));
        if (!db_.commit()) fail(db_.lastError().text());
    } catch (...) { db_.rollback(); throw; }
    return asset;
}

bool Library::capture(const Asset &asset, const QString &note) {
    const auto bytes = readFile(asset.path);
    const auto history = revisions(asset.id);
    if (!history.isEmpty() && history.first().hash == digest(bytes)) return false;
    append(asset, bytes, note);
    return true;
}

void Library::restore(const Asset &asset, qint64 revisionId) {
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
            if (!QDir().mkpath(QFileInfo(asset.path).absolutePath())) fail(QStringLiteral("无法创建恢复目录"));
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
    fail(QStringLiteral("找不到属于当前素材的历史版本"));
}

void Library::exportRevision(const QString &assetId, qint64 revisionId, const QString &destination) const {
    // Never export into the managed library (would permit overwriting immutable objects).
    const QString dest = QFileInfo(destination).absoluteFilePath();
    const QString parent = QFileInfo(dest).dir().canonicalPath();
    const QString canonicalRoot = QDir(root_).canonicalPath();
    if (parent.isEmpty() || parent == canonicalRoot || parent.startsWith(canonicalRoot + '/'))
        fail(QStringLiteral("请导出到素材库之外的现有目录"));
    for (const auto &revision : revisions(assetId)) {
        if (revision.id == revisionId) { writeFile(dest, readObject(revision.hash)); return; }
    }
    fail(QStringLiteral("找不到历史版本"));
}

int Library::hardenObjects() {
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
        for (const auto &asset : all) {
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
            ins.prepare("INSERT OR REPLACE INTO snapshot_assets(snapshot_id,asset_id,revision_id) VALUES(?,?,?)");
            ins.addBindValue(snapshotId);
            ins.addBindValue(asset.id);
            ins.addBindValue(anchor);
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
    QSqlQuery q(db_);
    q.prepare("SELECT s.id,s.created,s.note,COUNT(sa.asset_id) FROM snapshots s "
              "LEFT JOIN snapshot_assets sa ON sa.snapshot_id=s.id GROUP BY s.id ORDER BY s.id DESC");
    execute(q);
    QVector<Snapshot> result;
    while (q.next()) {
        result.push_back({q.value(0).toLongLong(), q.value(1).toString(),
                          q.value(2).toString(), q.value(3).toInt()});
    }
    return result;
}

int Library::rollbackAll(qint64 snapshotId) {
    QHash<QString, Asset> byId;
    for (const auto &asset : assets()) byId.insert(asset.id, asset);
    QSqlQuery q(db_);
    q.prepare("SELECT asset_id, revision_id FROM snapshot_assets WHERE snapshot_id=?");
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
