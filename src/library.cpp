#include "library.h"

#include "platform_immutable.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLockFile>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSqlError>
#include <QSqlQuery>
#include <QStorageInfo>
#include <QThread>
#include <QUuid>
#include <stdexcept>

namespace {
constexpr qint64 kStreamChunk = 1LL << 20; // 1 MiB streaming window
[[noreturn]] void fail(const QString &message) { throw std::runtime_error(message.toStdString()); }
QString digest(const QByteArray &bytes) {
    return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}
// P-309 tags serialize as a JSON array so any tag text round-trips exactly.
QString tagsToStore(const QStringList &tags) {
    QJsonArray array;
    for (const auto &tag : tags) array.append(tag);
    return QString::fromUtf8(QJsonDocument(array).toJson(QJsonDocument::Compact));
}
QStringList tagsFromStore(const QString &stored) {
    if (stored.isEmpty()) return {};
    const auto array = QJsonDocument::fromJson(stored.toUtf8()).array();
    QStringList tags;
    for (const auto &value : array) tags << value.toString();
    return tags;
}
// P-302 streaming SHA-256: hashes a file in fixed-size windows instead of
// loading it all into memory, so arbitrarily large assets can be fingerprinted.
QString streamDigest(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) fail(QStringLiteral("无法读取文件：%1").arg(path));
    QCryptographicHash hasher(QCryptographicHash::Sha256);
    QByteArray window(kStreamChunk, Qt::Uninitialized);
    while (true) {
        const qint64 read = file.read(window.data(), window.size());
        if (read < 0) fail(file.errorString());
        if (read == 0) break;
        hasher.addData(window.constData(), read);
    }
    if (file.error() != QFileDevice::NoError) fail(file.errorString());
    return QString::fromLatin1(hasher.result().toHex());
}
// Stream-copies `source` to `destination`. On any failure the partial
// destination is removed and an error is thrown. Returns the bytes copied.
qint64 streamCopy(const QString &source, const QString &destination) {
    QFile in(source);
    if (!in.open(QIODevice::ReadOnly)) fail(QStringLiteral("无法读取文件：%1").arg(source));
    QFile out(destination);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate))
        fail(QStringLiteral("无法写入临时对象：%1；%2").arg(destination, out.errorString()));
    QByteArray window(kStreamChunk, Qt::Uninitialized);
    qint64 total = 0;
    while (true) {
        const qint64 read = in.read(window.data(), window.size());
        if (read < 0) { out.close(); QFile::remove(destination); fail(in.errorString()); }
        if (read == 0) break;
        if (out.write(window.constData(), read) != read) {
            const QString error = out.errorString();
            out.close(); QFile::remove(destination);
            fail(QStringLiteral("无法写入临时对象：%1；%2").arg(destination, error));
        }
        total += read;
    }
    if (in.error() != QFileDevice::NoError || !out.flush()) {
        const QString error = in.error() != QFileDevice::NoError ? in.errorString() : out.errorString();
        out.close(); QFile::remove(destination);
        fail(QStringLiteral("无法写入临时对象：%1；%2").arg(destination, error));
    }
    out.close();
    return total;
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
    if (!QDir().mkpath(root_ + "/objects") || !QDir().mkpath(root_ + "/objects/.tmp") || !QDir().mkpath(root_ + "/working"))
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
             QStringLiteral("CREATE TABLE IF NOT EXISTS assets(id TEXT PRIMARY KEY,name TEXT NOT NULL,path TEXT NOT NULL,tags TEXT NOT NULL DEFAULT '',rating INTEGER,note TEXT NOT NULL DEFAULT '',source TEXT NOT NULL DEFAULT '',deleted INTEGER NOT NULL DEFAULT 0)"),
             QStringLiteral("CREATE TABLE IF NOT EXISTS revisions(id INTEGER PRIMARY KEY AUTOINCREMENT,asset_id TEXT NOT NULL REFERENCES assets(id),hash TEXT NOT NULL,created TEXT NOT NULL,note TEXT NOT NULL,size INTEGER NOT NULL,pinned INTEGER NOT NULL DEFAULT 0,pin_reason TEXT NOT NULL DEFAULT '',tombstone INTEGER NOT NULL DEFAULT 0)"),
             QStringLiteral("CREATE INDEX IF NOT EXISTS revisions_asset ON revisions(asset_id,id)"),
             QStringLiteral("CREATE TABLE IF NOT EXISTS snapshots(id INTEGER PRIMARY KEY AUTOINCREMENT,created TEXT NOT NULL,note TEXT NOT NULL,boundary INTEGER NOT NULL DEFAULT 0)"),
             QStringLiteral("CREATE TABLE IF NOT EXISTS snapshot_assets(snapshot_id INTEGER NOT NULL REFERENCES snapshots(id),asset_id TEXT NOT NULL REFERENCES assets(id),revision_id INTEGER NOT NULL,hash TEXT NOT NULL DEFAULT '',path TEXT NOT NULL DEFAULT '',state TEXT NOT NULL DEFAULT 'present',PRIMARY KEY(snapshot_id,asset_id))"),
             QStringLiteral("CREATE TABLE IF NOT EXISTS exclusion_rules(id INTEGER PRIMARY KEY AUTOINCREMENT,kind TEXT NOT NULL,pattern TEXT NOT NULL,reason TEXT NOT NULL,created TEXT NOT NULL)"),
             QStringLiteral("CREATE TABLE IF NOT EXISTS snapshot_exclusions(snapshot_id INTEGER NOT NULL REFERENCES snapshots(id),asset_id TEXT NOT NULL REFERENCES assets(id),reason TEXT NOT NULL,path TEXT NOT NULL DEFAULT '',PRIMARY KEY(snapshot_id,asset_id))"),
             QStringLiteral("CREATE TABLE IF NOT EXISTS metadata_changes(id INTEGER PRIMARY KEY AUTOINCREMENT,asset_id TEXT NOT NULL REFERENCES assets(id),revision_id INTEGER NOT NULL,field TEXT NOT NULL,old_value TEXT NOT NULL,new_value TEXT NOT NULL,changed TEXT NOT NULL)")}) {
        if (!q.exec(sql)) fail(q.lastError().text());
    }
    // Schema evolution: libraries created before P-307 lack the pin columns.
    // CREATE TABLE IF NOT EXISTS does not alter an existing table, so add the
    // columns idempotently (PRAGMA table_info lists existing columns only).
    {
        QSqlQuery columns(db_);
        columns.prepare("PRAGMA table_info(revisions)");
        execute(columns);
        QStringList names;
        while (columns.next()) names << columns.value(1).toString();
        if (!names.contains(QStringLiteral("pinned"))) {
            QSqlQuery alter(db_);
            if (!alter.exec(QStringLiteral(
                    "ALTER TABLE revisions ADD COLUMN pinned INTEGER NOT NULL DEFAULT 0")))
                fail(alter.lastError().text());
        }
        if (!names.contains(QStringLiteral("pin_reason"))) {
            QSqlQuery alter(db_);
            if (!alter.exec(QStringLiteral(
                    "ALTER TABLE revisions ADD COLUMN pin_reason TEXT NOT NULL DEFAULT ''")))
                fail(alter.lastError().text());
        }
        if (!names.contains(QStringLiteral("tombstone"))) {
            QSqlQuery alter(db_);
            if (!alter.exec(QStringLiteral(
                    "ALTER TABLE revisions ADD COLUMN tombstone INTEGER NOT NULL DEFAULT 0")))
                fail(alter.lastError().text());
        }
    }
    // P-309: libraries created before metadata carry no metadata columns.
    // Tags are stored as a JSON array text; rating is NULL when "no rating".
    {
        QSqlQuery columns(db_);
        columns.prepare("PRAGMA table_info(assets)");
        execute(columns);
        QStringList names;
        while (columns.next()) names << columns.value(1).toString();
        const QList<QPair<QString, QString>> additions = {
            {QStringLiteral("tags"), QStringLiteral("TEXT NOT NULL DEFAULT ''")},
            {QStringLiteral("rating"), QStringLiteral("INTEGER")},
            {QStringLiteral("note"), QStringLiteral("TEXT NOT NULL DEFAULT ''")},
            {QStringLiteral("source"), QStringLiteral("TEXT NOT NULL DEFAULT ''")}};
        for (const auto &column : additions) {
            if (names.contains(column.first)) continue;
            QSqlQuery alter(db_);
            if (!alter.exec(QStringLiteral("ALTER TABLE assets ADD COLUMN %1 %2")
                                .arg(column.first, column.second)))
                fail(alter.lastError().text());
        }
        if (!names.contains(QStringLiteral("deleted"))) {
            QSqlQuery alter(db_);
            if (!alter.exec(QStringLiteral(
                    "ALTER TABLE assets ADD COLUMN deleted INTEGER NOT NULL DEFAULT 0")))
                fail(alter.lastError().text());
        }
    }
    // P-311: snapshot manifest columns (sequence point + per-entry path/state).
    {
        QSqlQuery snapColumns(db_);
        snapColumns.prepare("PRAGMA table_info(snapshots)");
        execute(snapColumns);
        QStringList snapNames;
        while (snapColumns.next()) snapNames << snapColumns.value(1).toString();
        if (!snapNames.contains(QStringLiteral("boundary"))) {
            QSqlQuery alter(db_);
            if (!alter.exec(QStringLiteral(
                    "ALTER TABLE snapshots ADD COLUMN boundary INTEGER NOT NULL DEFAULT 0")))
                fail(alter.lastError().text());
        }
        QSqlQuery entryColumns(db_);
        entryColumns.prepare("PRAGMA table_info(snapshot_assets)");
        execute(entryColumns);
        QStringList entryNames;
        while (entryColumns.next()) entryNames << entryColumns.value(1).toString();
        const QList<QPair<QString, QString>> entryAdditions = {
            {QStringLiteral("hash"), QStringLiteral("TEXT NOT NULL DEFAULT ''")},
            {QStringLiteral("path"), QStringLiteral("TEXT NOT NULL DEFAULT ''")},
            {QStringLiteral("state"), QStringLiteral("TEXT NOT NULL DEFAULT 'present'")}};
        for (const auto &column : entryAdditions) {
            if (entryNames.contains(column.first)) continue;
            QSqlQuery alter(db_);
            if (!alter.exec(QStringLiteral("ALTER TABLE snapshot_assets ADD COLUMN %1 %2")
                                .arg(column.first, column.second)))
                fail(alter.lastError().text());
        }
        // P-312: record the excluded asset's path as of the snapshot, so the
        // time-point browsing surface shows a faithful 当时路径 (never the
        // current index) for excluded assets too.
        QSqlQuery exColumns(db_);
        exColumns.prepare("PRAGMA table_info(snapshot_exclusions)");
        execute(exColumns);
        QStringList exNames;
        while (exColumns.next()) exNames << exColumns.value(1).toString();
        if (!exNames.contains(QStringLiteral("path"))) {
            QSqlQuery alter(db_);
            if (!alter.exec(QStringLiteral(
                    "ALTER TABLE snapshot_exclusions ADD COLUMN path TEXT NOT NULL DEFAULT ''")))
                fail(alter.lastError().text());
        }
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
    q.prepare("SELECT id,name,path,tags,rating,note,source,deleted FROM assets ORDER BY name,id");
    execute(q);
    QVector<Asset> result;
    while (q.next()) {
        Asset asset;
        asset.id = q.value(0).toString();
        asset.name = q.value(1).toString();
        asset.path = q.value(2).toString();
        asset.tags = tagsFromStore(q.value(3).toString());
        asset.rating = q.isNull(4) ? -1 : q.value(4).toInt();
        asset.note = q.value(5).toString();
        asset.source = q.value(6).toString();
        asset.deleted = q.value(7).toBool();
        result.push_back(asset);
    }
    return result;
}

QVector<Revision> Library::revisions(const QString &assetId) const {
    QSqlQuery q(db_);
    q.prepare("SELECT id,hash,created,note,size,pinned,pin_reason,tombstone FROM revisions WHERE asset_id=? ORDER BY id DESC");
    q.addBindValue(assetId);
    execute(q);
    QVector<Revision> result;
    while (q.next()) result.push_back({q.value(0).toLongLong(), q.value(1).toString(),
        q.value(2).toString(), q.value(3).toString(), q.value(4).toLongLong(),
        q.value(5).toBool(), q.value(6).toString(), q.value(7).toBool()});
    return result;
}

AssetTimeline Library::assetTimeline(const QString &assetId) const {
    AssetTimeline timeline;
    // Current working content, if present and readable. "Current" is derived
    // from content (not from the last selected history item): the newest
    // revision whose hash matches the working copy is marked current.
    QString currentHash;
    {
        QSqlQuery asset(db_);
        asset.prepare("SELECT path FROM assets WHERE id=?");
        asset.addBindValue(assetId);
        execute(asset);
        if (asset.next()) {
            const QString path = asset.value(0).toString();
            if (QFileInfo(path).exists()) {
                try { currentHash = streamDigest(path); }
                catch (...) { currentHash.clear(); }
            }
        }
    }
    const QRegularExpression restoreNote(QStringLiteral("^恢复历史版本 #\\d+$"));
    bool currentMarked = false;
    for (const auto &rev : revisions(assetId)) {
        TimelineEntry entry;
        entry.revisionId = rev.id;
        entry.hash = rev.hash;
        entry.created = rev.created;
        entry.note = rev.note;
        entry.size = rev.size;
        entry.pinned = rev.pinned;
        entry.pinReason = rev.pinReason;
        if (!currentMarked && !currentHash.isEmpty() && rev.hash == currentHash) {
            entry.current = true;
            currentMarked = true;
        }
        entry.restore = restoreNote.match(rev.note).hasMatch();
        try { if (streamDigest(objectPath(rev.hash)) != rev.hash) entry.corrupt = true; }
        catch (...) { entry.corrupt = true; }
        timeline.entries.push_back(entry);
    }
    timeline.unrecorded = !currentHash.isEmpty() && !currentMarked;
    return timeline;
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

StoredObject Library::storeObject(const QString &source) {
    const QString temp = newTempObjectPath();
    qint64 size = 0;
    try {
        size = streamCopy(source, temp);
    } catch (...) {
        QFile::remove(temp); // 磁盘满/读失败：清理临时，不发布
        throw;
    }
    return publishTempObject(temp, size);
}

QString Library::newTempObjectPath() const {
    if (!QDir().mkpath(root_ + "/objects/.tmp")) fail(QStringLiteral("无法创建临时对象目录"));
    return root_ + "/objects/.tmp/" + QUuid::createUuid().toString(QUuid::WithoutBraces);
}

StoredObject Library::storeBytes(const QByteArray &bytes) {
    const QString temp = newTempObjectPath();
    QFile file(temp);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate) || file.write(bytes) != bytes.size() || !file.flush()) {
        const QString error = file.errorString();
        file.close();
        QFile::remove(temp); // 写失败：清理临时，不发布
        fail(QStringLiteral("无法写入临时对象；%1").arg(error));
    }
    file.close();
    return publishTempObject(temp, bytes.size());
}

StoredObject Library::publishTempObject(const QString &temp, qint64 size) {
    // 完整校验：re-hash the finished temporary object before it is trusted.
    const QString hash = streamDigest(temp);
    const QString path = objectPath(hash);
    bool created = false;
    if (QFileInfo::exists(path)) {
        // Dedup: identical content is already stored. The existing object must
        // actually match; a mismatch is corruption and must block the store
        // (never overwrite an existing object with different content).
        if (streamDigest(path) != hash) {
            QFile::remove(temp);
            fail(QStringLiteral("历史对象内容不一致"));
        }
        QFile::remove(temp);
    } else if (!QFile::rename(temp, path)) {
        QFile::remove(temp);
        fail(QStringLiteral("无法发布历史对象"));
    } else {
        created = true;
    }
    // The version store is append-only: once an object lands it must resist
    // later tampering by a same-user ransomware process. Best-effort + idempotent
    // (chattr +i where the OS allows it, read-only mode bits otherwise).
    platform::makeImmutable(path);
    return {hash, size, created};
}

qint64 Library::insertRevision(const Asset &asset, const QString &hash, qint64 size, const QString &note) {
    QSqlQuery q(db_);
    q.prepare("INSERT INTO revisions(asset_id,hash,created,note,size) VALUES(?,?,?,?,?)");
    q.addBindValue(asset.id);
    q.addBindValue(hash);
    q.addBindValue(QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
    q.addBindValue(note.isEmpty() ? QStringLiteral("手动快照") : note);
    q.addBindValue(size);
    execute(q);
    return q.lastInsertId().toLongLong();
}

qint64 Library::append(const Asset &asset, const QByteArray &bytes, const QString &note) {
    const auto stored = storeBytes(bytes);
    return insertRevision(asset, stored.hash, stored.size, note);
}

qint64 Library::appendFromFile(const Asset &asset, const QString &source, const QString &note) {
    const auto stored = storeObject(source);
    return insertRevision(asset, stored.hash, stored.size, note);
}

RecordResult Library::recordVersion(const Asset &asset, const QString &note) {
    const QString hash = streamDigest(asset.path);
    const auto history = revisions(asset.id);
    if (!history.isEmpty() && history.first().hash == hash)
        return {history.first().id, history.first().note, true};
    const QString effective = note.isEmpty() ? QStringLiteral("手动快照") : note;
    return {appendFromFile(asset, asset.path, note), effective, false};
}

namespace {
bool signatureMatches(const Asset &asset, qint64 size, qint64 mtimeMs) {
    const QFileInfo info(asset.path);
    return info.exists() && info.size() == size && info.lastModified().toMSecsSinceEpoch() == mtimeMs;
}
}

StableTick Library::recordStableVersion(const Asset &asset, qint64 size, qint64 mtimeMs, const QString &note) {
    // Read-before/after复核, first half: a working copy that changed between
    // the stability observation and this read is a moving target — retry.
    const QString hash = streamDigest(asset.path);
    if (!signatureMatches(asset, size, mtimeMs)) return {false, false, true, 0};
    const auto history = revisions(asset.id);
    if (!history.isEmpty() && history.first().hash == hash) return {false, true, false, history.first().id};
    // Second read pass (store) can still race a write; the object it publishes
    // is internally consistent (temp-verified) but may be a torn snapshot.
    const auto stored = storeObject(asset.path);
    if (!signatureMatches(asset, size, mtimeMs)) {
        // The file moved while it was copied: discard the fresh object so a torn
        // mid-write snapshot never enters history, then let the caller retry.
        if (stored.created) {
            const QString path = objectPath(stored.hash);
            platform::makeMutable(path);
            QFile::remove(path);
        }
        return {false, false, true, 0};
    }
    return {true, false, false, insertRevision(asset, stored.hash, stored.size, note)};
}

StableTick Library::autoRecordTick(const Asset &asset, const QString &note) {
    const QString effective = note.isEmpty() ? QStringLiteral("自动记录") : note;
    const QFileInfo info(asset.path);
    if (!info.exists()) {
        stablePending_.remove(asset.id);
        stableRecorded_.remove(asset.id);
        return {};
    }
    const auto sig = qMakePair(info.size(), info.lastModified().toMSecsSinceEpoch());
    // Two consecutive ticks with the same size+mtime signature = the working
    // copy has stopped being written (half-written / continuous-write files
    // keep changing and never reach this point).
    const auto pending = stablePending_.constFind(asset.id);
    if (pending == stablePending_.constEnd() || pending.value() != sig) {
        stablePending_.insert(asset.id, sig);
        return {};
    }
    if (stableRecorded_.value(asset.id) == sig) return {}; // this state was already recorded
    StableTick result;
    try {
        result = recordStableVersion(asset, sig.first, sig.second, effective);
    } catch (...) {
        stablePending_.remove(asset.id); // loud failure: re-observe from scratch
        throw;
    }
    if (result.retry) {
        stablePending_.remove(asset.id); // file moved during read: observe again
        return result;
    }
    if (result.recorded || result.unchanged) stableRecorded_.insert(asset.id, sig);
    return result;
}

ScheduleState Library::scheduleState() const { return schedule_; }

void Library::enableSchedule(int periodMinutes) {
    if (periodMinutes != 15 && periodMinutes != 30 && periodMinutes != 60)
        fail(QStringLiteral("定时快照周期须为 15/30/60 分钟"));
    schedule_.enabled = true;
    schedule_.periodMinutes = periodMinutes;
    schedule_.paused = false;
    schedule_.pauseReason.clear();
    schedule_.nextUtcMs = QDateTime::currentMSecsSinceEpoch() + periodMinutes * 60000LL;
}

void Library::disableSchedule() { schedule_ = ScheduleState{}; }

void Library::pauseSchedule(const QString &reason) {
    if (!schedule_.enabled) return;
    schedule_.paused = true;
    schedule_.pauseReason = reason;
}

void Library::resumeSchedule() {
    if (!schedule_.enabled) return;
    schedule_.paused = false;
    schedule_.pauseReason.clear();
    // Resuming restarts the interval from now: no surprise snapshot right after
    // a pause, consistent with "next time = enabling moment + period".
    schedule_.nextUtcMs = QDateTime::currentMSecsSinceEpoch() + schedule_.periodMinutes * 60000LL;
}

qint64 Library::pumpSchedule(qint64 nowUtcMs) {
    if (!schedule_.enabled || schedule_.paused || nowUtcMs < schedule_.nextUtcMs) return 0;
    // Several missed periods collapse into this single catch-up snapshot; the
    // next window starts from now (no stacking of overdue work). snapshotAll is
    // synchronous, so a still-running previous snapshot cannot overlap by design.
    const qint64 id = snapshotAll(QStringLiteral("定时快照"));
    schedule_.nextUtcMs = nowUtcMs + schedule_.periodMinutes * 60000LL;
    return id;
}

QVector<ExclusionRule> Library::exclusionRules() const {
    QSqlQuery q(db_);
    q.prepare("SELECT id,kind,pattern,reason FROM exclusion_rules ORDER BY id");
    execute(q);
    QVector<ExclusionRule> result;
    while (q.next()) {
        result.push_back({q.value(0).toLongLong(), q.value(1).toString(),
                          q.value(2).toString(), q.value(3).toString()});
    }
    return result;
}

qint64 Library::addExclusionRule(const QString &kind, const QString &pattern, const QString &reason) {
    if (kind != QStringLiteral("extension") && kind != QStringLiteral("glob"))
        fail(QStringLiteral("排除规则类型须为 extension 或 glob"));
    if (pattern.isEmpty()) fail(QStringLiteral("排除规则模式不能为空"));
    QSqlQuery q(db_);
    q.prepare("INSERT INTO exclusion_rules(kind,pattern,reason,created) VALUES(?,?,?,?)");
    q.addBindValue(kind);
    q.addBindValue(pattern);
    q.addBindValue(reason);
    q.addBindValue(QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
    execute(q);
    return q.lastInsertId().toLongLong();
}

void Library::removeExclusionRule(qint64 id) {
    QSqlQuery q(db_);
    q.prepare("DELETE FROM exclusion_rules WHERE id=?");
    q.addBindValue(id);
    execute(q);
}

bool Library::isExcluded(const Asset &asset, QString *reason) const {
    QSqlQuery q(db_);
    q.prepare("SELECT id,kind,pattern,reason FROM exclusion_rules ORDER BY id");
    execute(q);
    while (q.next()) {
        const QString kind = q.value(1).toString();
        const QString pattern = q.value(2).toString();
        bool hit = false;
        if (kind == QStringLiteral("extension")) {
            QString p = pattern.toLower();
            if (p.startsWith(QLatin1Char('.'))) p.remove(0, 1);
            const QString ext = QFileInfo(asset.name).suffix().toLower();
            hit = !ext.isEmpty() && ext == p;
        } else if (kind == QStringLiteral("glob")) {
            const QRegularExpression re(QRegularExpression::wildcardToRegularExpression(pattern),
                                        QRegularExpression::CaseInsensitiveOption);
            const QString rel = QDir(root_).relativeFilePath(asset.path);
            hit = re.match(asset.name).hasMatch() || re.match(rel).hasMatch();
        }
        if (hit) {
            if (reason) *reason = q.value(3).toString();
            return true;
        }
    }
    return false;
}

QString Library::snapshotExclusionReason(qint64 snapshotId, const QString &assetId) const {
    QSqlQuery q(db_);
    q.prepare("SELECT reason FROM snapshot_exclusions WHERE snapshot_id=? AND asset_id=?");
    q.addBindValue(snapshotId);
    q.addBindValue(assetId);
    execute(q);
    return q.next() ? q.value(0).toString() : QString();
}

void Library::pinRevision(const QString &assetId, qint64 revisionId, const QString &reason) {
    QSqlQuery owns(db_);
    owns.prepare("SELECT 1 FROM revisions WHERE id=? AND asset_id=?");
    owns.addBindValue(revisionId);
    owns.addBindValue(assetId);
    execute(owns);
    if (!owns.next()) fail(QStringLiteral("找不到属于当前素材的历史版本"));
    QSqlQuery q(db_);
    q.prepare("UPDATE revisions SET pinned=1,pin_reason=? WHERE id=?");
    q.addBindValue(reason);
    q.addBindValue(revisionId);
    execute(q);
}

void Library::unpinRevision(const QString &assetId, qint64 revisionId) {
    QSqlQuery owns(db_);
    owns.prepare("SELECT 1 FROM revisions WHERE id=? AND asset_id=?");
    owns.addBindValue(revisionId);
    owns.addBindValue(assetId);
    execute(owns);
    if (!owns.next()) fail(QStringLiteral("找不到属于当前素材的历史版本"));
    QSqlQuery q(db_);
    q.prepare("UPDATE revisions SET pinned=0,pin_reason='' WHERE id=?");
    q.addBindValue(revisionId);
    execute(q);
}

QVector<qint64> Library::retentionCandidates(const QString &assetId) const {
    // The seam for the future V1 retention policy: everything a cleanup may
    // remove. The newest (current) revision is never a candidate, and a pinned
    // revision is always excluded (P-307) — so pinning is what keeps a version.
    QSqlQuery q(db_);
    q.prepare("SELECT id FROM revisions WHERE asset_id=? AND pinned=0 "
              "AND id<>(SELECT MAX(id) FROM revisions WHERE asset_id=?) ORDER BY id DESC");
    q.addBindValue(assetId);
    q.addBindValue(assetId);
    execute(q);
    QVector<qint64> result;
    while (q.next()) result.push_back(q.value(0).toLongLong());
    return result;
}

Metadata Library::metadata(const QString &assetId) const {
    QSqlQuery q(db_);
    q.prepare("SELECT name,tags,rating,note,source FROM assets WHERE id=?");
    q.addBindValue(assetId);
    execute(q);
    if (!q.next()) fail(QStringLiteral("找不到素材"));
    Metadata m;
    m.name = q.value(0).toString();
    m.tags = tagsFromStore(q.value(1).toString());
    m.rating = q.isNull(2) ? -1 : q.value(2).toInt();
    m.note = q.value(3).toString();
    m.source = q.value(4).toString();
    return m;
}

QVector<MetadataChange> Library::metadataHistory(const QString &assetId) const {
    QSqlQuery q(db_);
    q.prepare("SELECT revision_id,field,old_value,new_value,changed FROM metadata_changes "
              "WHERE asset_id=? ORDER BY id DESC");
    q.addBindValue(assetId);
    execute(q);
    QVector<MetadataChange> result;
    while (q.next()) result.push_back({q.value(0).toLongLong(), q.value(1).toString(),
        q.value(2).toString(), q.value(3).toString(), q.value(4).toString()});
    return result;
}

RecordResult Library::updateMetadata(const QString &assetId, const Metadata &newMeta,
                                     const QString &note) {
    const auto current = metadata(assetId);
    const auto history = revisions(assetId);
    if (history.isEmpty()) fail(QStringLiteral("找不到属于当前素材的历史版本"));
    // Normalize null strings to empty so NOT NULL columns bind '' not NULL.
    Metadata m = newMeta;
    if (m.name.isNull()) m.name = QStringLiteral("");
    if (m.note.isNull()) m.note = QStringLiteral("");
    if (m.source.isNull()) m.source = QStringLiteral("");
    // Per-field diffs; values are recorded as actually stored, never fabricated.
    struct Change { QString field; QString oldValue; QString newValue; };
    QVector<Change> changes;
    const QString newTags = tagsToStore(m.tags);
    const QString oldTags = tagsToStore(current.tags);
    if (m.name != current.name)
        changes.push_back(Change{QStringLiteral("name"), current.name, m.name});
    if (newTags != oldTags) changes.push_back(Change{QStringLiteral("tags"), oldTags, newTags});
    if (m.rating != current.rating)
        changes.push_back(Change{QStringLiteral("rating"), QString::number(current.rating),
                                 QString::number(m.rating)});
    if (m.note != current.note)
        changes.push_back(Change{QStringLiteral("note"), current.note, m.note});
    if (m.source != current.source)
        changes.push_back(Change{QStringLiteral("source"), current.source, m.source});
    const QString effective = note.isEmpty() ? QStringLiteral("元数据更新") : note;
    if (changes.isEmpty()) return {history.first().id, effective, true};
    if (!db_.transaction()) fail(db_.lastError().text());
    try {
        QSqlQuery up(db_);
        up.prepare("UPDATE assets SET name=?,tags=?,rating=?,note=?,source=? WHERE id=?");
        up.addBindValue(m.name);
        up.addBindValue(newTags);
        up.addBindValue(m.rating < 0 ? QVariant() : QVariant(m.rating));
        up.addBindValue(m.note);
        up.addBindValue(m.source);
        up.addBindValue(assetId);
        execute(up);
        // A metadata-only revision reuses the newest content hash (design
        // P-309): contentHash unchanged, no new content object is created.
        Asset asset;
        asset.id = assetId;
        asset.name = m.name;
        const qint64 revisionId = insertRevision(asset, history.first().hash,
                                                 history.first().size, effective);
        const QString changed = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
        for (const auto &change : changes) {
            QSqlQuery ins(db_);
            ins.prepare("INSERT INTO metadata_changes(asset_id,revision_id,field,old_value,new_value,changed) VALUES(?,?,?,?,?,?)");
            ins.addBindValue(assetId); ins.addBindValue(revisionId); ins.addBindValue(change.field);
            ins.addBindValue(change.oldValue); ins.addBindValue(change.newValue); ins.addBindValue(changed);
            execute(ins);
        }
        if (!db_.commit()) fail(db_.lastError().text());
        return {revisionId, effective, false};
    } catch (...) { db_.rollback(); throw; }
}

Asset Library::importFile(const QString &source) {
    const auto bytes = readFile(source);
    const QFileInfo info(source);
    const QString id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    Asset asset;
    asset.id = id;
    asset.name = info.fileName();
    asset.path = root_ + "/working/" + id + "/" + info.fileName();
    asset.tags = QStringList(); // non-null, so NOT NULL columns bind as '' not NULL
    asset.rating = -1;
    asset.note = QStringLiteral("");
    asset.source = QFileInfo(source).absoluteFilePath(); // P-309 source record
    if (!QDir().mkpath(QFileInfo(asset.path).absolutePath())) fail(QStringLiteral("无法创建工作目录"));
    writeFile(asset.path, bytes);
    if (!db_.transaction()) fail(db_.lastError().text());
    try {
        QSqlQuery q(db_);
        q.prepare("INSERT INTO assets(id,name,path,tags,rating,note,source) VALUES(?,?,?,?,?,?,?)");
        q.addBindValue(asset.id); q.addBindValue(asset.name); q.addBindValue(asset.path);
        q.addBindValue(tagsToStore(asset.tags));
        q.addBindValue(asset.rating < 0 ? QVariant() : QVariant(asset.rating));
        q.addBindValue(asset.note); q.addBindValue(asset.source);
        execute(q);
        const qint64 firstId = append(asset, bytes, QStringLiteral("首次导入"));
        // P-309 baseline: the initial metadata is itself a recorded change
        // (old value empty), so history is viewable from the very first record.
        for (const auto &field : {QStringLiteral("name"), QStringLiteral("source")}) {
            const QString value = field == QStringLiteral("name") ? asset.name : asset.source;
            if (value.isEmpty()) continue;
            QSqlQuery change(db_);
            change.prepare("INSERT INTO metadata_changes(asset_id,revision_id,field,old_value,new_value,changed) VALUES(?,?,?,?,?,?)");
            change.addBindValue(asset.id); change.addBindValue(firstId); change.addBindValue(field);
            change.addBindValue(QStringLiteral("")); change.addBindValue(value);
            change.addBindValue(QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
            execute(change);
        }
        if (!db_.commit()) fail(db_.lastError().text());
    } catch (...) { db_.rollback(); throw; }
    return asset;
}

bool Library::capture(const Asset &asset, const QString &note) {
    // Recording never holds the working copy in memory (streaming, see P-302).
    return !recordVersion(asset, note).unchanged;
}

void Library::deleteAsset(const Asset &asset, const QString &note) {
    if (assetState(asset.id) == AssetState::Tombstone)
        fail(QStringLiteral("素材已删除"));
    const auto history = revisions(asset.id);
    if (history.isEmpty()) fail(QStringLiteral("找不到属于当前素材的历史版本"));
    const auto newest = history.first();
    // Tombstone revision reuses the last content hash — no new content object;
    // history and objects are retained so the asset stays findable/recoverable.
    if (!db_.transaction()) fail(db_.lastError().text());
    try {
        QSqlQuery ins(db_);
        ins.prepare("INSERT INTO revisions(asset_id,hash,created,note,size,tombstone) VALUES(?,?,?,?,?,1)");
        ins.addBindValue(asset.id);
        ins.addBindValue(newest.hash);
        ins.addBindValue(QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
        ins.addBindValue(note.isEmpty() ? QStringLiteral("删除") : note);
        ins.addBindValue(newest.size);
        execute(ins);
        QSqlQuery up(db_);
        up.prepare("UPDATE assets SET deleted=1 WHERE id=?");
        up.addBindValue(asset.id);
        execute(up);
        if (!db_.commit()) fail(db_.lastError().text());
    } catch (...) { db_.rollback(); throw; }
    // Best-effort removal of the managed working copy after the tombstone is
    // recorded; if removal fails the asset is still tombstoned and recoverable.
    if (QFileInfo::exists(asset.path)) QFile::remove(asset.path);
}

AssetState Library::assetState(const QString &assetId) const {
    QSqlQuery q(db_);
    q.prepare("SELECT deleted,path FROM assets WHERE id=?");
    q.addBindValue(assetId);
    execute(q);
    if (!q.next()) fail(QStringLiteral("找不到素材"));
    if (q.value(0).toBool()) return AssetState::Tombstone;
    return QFileInfo(q.value(1).toString()).exists() ? AssetState::Present : AssetState::Missing;
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
            // P-310: recovering a tombstoned/missing asset brings it back to
            // Present (the restore itself is the resurrection record).
            QSqlQuery clear(db_);
            clear.prepare("UPDATE assets SET deleted=0 WHERE id=?");
            clear.addBindValue(asset.id);
            execute(clear);
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
        // P-311: one consistent manifest. Current working content is first
        // recorded as a completed revision (anti-ransom preservation), then a
        // sequence point (boundary = max revision id) is fixed; every anchor is
        // a completed revision at or before it, so the listing is a single
        // consistent whole-library set — never a per-file scan.
        struct Entry { QString assetId; QString path; QString hash; QString state; qint64 revisionId; };
        QVector<Entry> entries;
        const auto all = assets();
        for (const auto &asset : all) {
            // P-306: excluded assets keep the emergency append (history kept)
            // but are never a rollback anchor; the reason is recorded instead.
            QString excludeReason;
            if (isExcluded(asset, &excludeReason)) {
                QSqlQuery ex(db_);
                ex.prepare("INSERT OR REPLACE INTO snapshot_exclusions(snapshot_id,asset_id,reason,path) VALUES(?,?,?,?)");
                ex.addBindValue(snapshotId);
                ex.addBindValue(asset.id);
                ex.addBindValue(excludeReason);
                ex.addBindValue(QDir(root_).relativeFilePath(asset.path));
                execute(ex);
                continue;
            }
            const bool hasWorking = QFileInfo::exists(asset.path);
            const auto history = revisions(asset.id);
            QString currentHash;
            if (hasWorking) {
                try { currentHash = streamDigest(asset.path); }
                catch (...) { currentHash.clear(); } // unreadable ≈ not present
            }
            // The recovery anchor must be the last CLEAN revision (pre-infection).
            // Race: scanChanges (2s) can auto-record infected bytes just before
            // pollSentinel (5s) fires, so history.first() may simply mirror the
            // current (suspected-infected) working content. Skip any newest
            // revision whose content equals the current working bytes, so the
            // anchor stays on the newest clean (pre-infection) revision. In a
            // false-positive alert where the current content is genuinely clean
            // and already recorded, this conservatively anchors to the prior
            // revision (consistent with "宁可误报、不可漏报").
            qint64 anchor = 0;
            QString anchorHash;
            for (const auto &rev : history) { // newest first
                if (currentHash.isEmpty() || rev.hash != currentHash) { anchor = rev.id; anchorHash = rev.hash; break; }
            }
            if (!anchor && !history.isEmpty()) { anchor = history.first().id; anchorHash = history.first().hash; }
            if (!currentHash.isEmpty() && (history.isEmpty() || history.first().hash != currentHash))
                appendFromFile(asset, asset.path, QStringLiteral("紧急快照锚点"));
            if (!anchor) {
                // A present asset with no completed revision cannot be anchored:
                // the candidate is incomplete, so it is NOT published as a
                // successful snapshot.
                fail(QStringLiteral("快照不完整：素材「%1」没有可锚定版本，未发布").arg(asset.name));
            }
            // Objects referenced by the manifest are verified (content reference
            // == manifest consistency); a bad object blocks the snapshot.
            if (streamDigest(objectPath(anchorHash)) != anchorHash)
                fail(QStringLiteral("快照不完整：素材「%1」对象校验失败，未发布").arg(asset.name));
            QString state = QStringLiteral("present");
            if (asset.deleted) state = QStringLiteral("tombstone");
            else if (currentHash.isEmpty()) state = QStringLiteral("missing");
            entries.push_back({asset.id, QDir(root_).relativeFilePath(asset.path),
                               anchorHash, state, anchor});
        }
        // The sequence point: everything above is a completed revision <= this.
        qint64 boundary = 0;
        {
            QSqlQuery b(db_);
            b.prepare("SELECT COALESCE(MAX(id),0) FROM revisions");
            execute(b);
            if (b.next()) boundary = b.value(0).toLongLong();
        }
        QSqlQuery ub(db_);
        ub.prepare("UPDATE snapshots SET boundary=? WHERE id=?");
        ub.addBindValue(boundary);
        ub.addBindValue(snapshotId);
        execute(ub);
        for (const auto &entry : entries) {
            if (entry.revisionId > boundary)
                fail(QStringLiteral("快照不完整：锚点超出序列点，未发布"));
            QSqlQuery ins(db_);
            ins.prepare("INSERT OR REPLACE INTO snapshot_assets(snapshot_id,asset_id,revision_id,hash,path,state) VALUES(?,?,?,?,?,?)");
            ins.addBindValue(snapshotId);
            ins.addBindValue(entry.assetId);
            ins.addBindValue(entry.revisionId);
            ins.addBindValue(entry.hash);
            ins.addBindValue(entry.path);
            ins.addBindValue(entry.state);
            execute(ins);
        }
        if (!db_.commit()) fail(db_.lastError().text());
    } catch (...) {
        db_.rollback();
        throw;
    }
    return snapshotId;
}

QVector<SnapshotEntry> Library::snapshotManifest(qint64 snapshotId) const {
    QSqlQuery q(db_);
    q.prepare("SELECT asset_id,path,hash,state,revision_id FROM snapshot_assets "
              "WHERE snapshot_id=? ORDER BY path,asset_id");
    q.addBindValue(snapshotId);
    execute(q);
    QVector<SnapshotEntry> result;
    while (q.next()) {
        result.push_back({q.value(0).toString(), q.value(1).toString(),
                          q.value(2).toString(), q.value(3).toString(),
                          q.value(4).toLongLong()});
    }
    return result;
}

QVector<SnapshotExclusion> Library::snapshotExclusions(qint64 snapshotId) const {
    QSqlQuery q(db_);
    q.prepare("SELECT asset_id,reason,path FROM snapshot_exclusions WHERE snapshot_id=? ORDER BY path,asset_id");
    q.addBindValue(snapshotId);
    execute(q);
    QVector<SnapshotExclusion> result;
    while (q.next()) result.push_back({q.value(0).toString(), q.value(1).toString(), q.value(2).toString()});
    return result;
}

QVector<Snapshot> Library::snapshots() const {
    QSqlQuery q(db_);
    q.prepare("SELECT s.id,s.created,s.note,COUNT(sa.asset_id),s.boundary FROM snapshots s "
              "LEFT JOIN snapshot_assets sa ON sa.snapshot_id=s.id GROUP BY s.id ORDER BY s.id DESC");
    execute(q);
    QVector<Snapshot> result;
    while (q.next()) {
        result.push_back({q.value(0).toLongLong(), q.value(1).toString(),
                          q.value(2).toString(), q.value(3).toInt(),
                          q.value(4).toLongLong()});
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

RestorePlan Library::planRestore(qint64 snapshotId) const {
    // P-315: a dry-run of rollbackAll. It reads only — no file is written — and
    // classifies each manifest entry as add / replace / skip / conflict plus the
    // space the writes would need. The plan binds the target version (anchor
    // revision) and the file state as of now; the executor rechecks before
    // writing (P-316+), since a stale plan must be refreshed, never trusted.
    RestorePlan plan;
    QHash<QString, Asset> byId;
    for (const auto &asset : assets()) byId.insert(asset.id, asset);
    QSqlQuery q(db_);
    q.prepare("SELECT asset_id, revision_id, hash FROM snapshot_assets "
              "WHERE snapshot_id=? ORDER BY path, asset_id");
    q.addBindValue(snapshotId);
    execute(q);
    while (q.next()) {
        const QString assetId = q.value(0).toString();
        const qint64 anchorId = q.value(1).toLongLong();
        const QString anchorHash = q.value(2).toString();
        RestoreOp op;
        op.assetId = assetId;
        const auto it = byId.constFind(assetId);
        if (it == byId.constEnd()) {
            op.kind = QStringLiteral("conflict");
            op.reason = QStringLiteral("素材已不存在于当前库");
            plan.ops.push_back(op);
            continue;
        }
        const Asset &asset = it.value();
        op.path = asset.path;
        // The recovery anchor must exist and verify, else the op cannot be
        // carried out — listed as a conflict, never silently skipped.
        bool anchorFound = false;
        for (const auto &rev : revisions(assetId)) {
            if (rev.id != anchorId) continue;
            op.size = rev.size;
            try {
                anchorFound = rev.hash == anchorHash
                    && streamDigest(objectPath(anchorHash)) == anchorHash;
            } catch (...) { anchorFound = false; }
            break;
        }
        if (!anchorFound) {
            op.kind = QStringLiteral("conflict");
            op.reason = QStringLiteral("锚点版本对象缺失或校验失败");
            plan.ops.push_back(op);
            continue;
        }
        if (!QFileInfo::exists(asset.path)) {
            op.kind = QStringLiteral("add"); // target absent, will create
        } else if (readFile(asset.path) == readObject(anchorHash)) {
            op.kind = QStringLiteral("skip"); // already at the recovery content
            op.size = 0;
        } else if (!QFileInfo(asset.path).isWritable()) {
            op.kind = QStringLiteral("conflict");
            op.reason = QStringLiteral("目标不可写");
        } else {
            op.kind = QStringLiteral("replace");
        }
        if (op.kind == QStringLiteral("add") || op.kind == QStringLiteral("replace"))
            plan.bytesToWrite += op.size;
        plan.ops.push_back(op);
    }
    return plan;
}

RestoreGuard Library::checkRestore(const Asset &asset, qint64 revisionId) const {
    // P-316: read-only pre-flight. It classifies the current working-copy state
    // relative to a restore of `revisionId` so the caller can decide whether a
    // restore may proceed, and reports the space the protection would need. It
    // never writes and never claims editor-unsaved content is saved.
    RestoreGuard guard;
    guard.state = QStringLiteral("ready");
    bool targetFound = false;
    for (const auto &rev : revisions(asset.id)) {
        if (rev.id != revisionId) continue;
        guard.restoreBytes = rev.size;
        try {
            if (streamDigest(objectPath(rev.hash)) != rev.hash) {
                guard.state = QStringLiteral("corrupt");
                return guard;
            }
        } catch (...) {
            guard.state = QStringLiteral("corrupt");
            return guard;
        }
        targetFound = true;
        break;
    }
    if (!targetFound) {
        guard.state = QStringLiteral("notarget");
        return guard;
    }
    if (!QFileInfo::exists(asset.path)) {
        guard.state = QStringLiteral("missing");
        return guard;
    }
    // Concurrent external write: the size+mtime signature is unstable across a
    // short double sample, so the file is being written right now — block.
    const auto signature = [&] {
        const QFileInfo info(asset.path);
        return QPair<qint64, qint64>(info.size(), info.lastModified().toMSecsSinceEpoch());
    };
    const auto first = signature();
    QThread::msleep(50);
    if (signature() != first) {
        guard.state = QStringLiteral("externalwrite");
        return guard;
    }
    // Unrecorded current content: the newest recorded revision differs from the
    // working bytes, so the restore will first protect them as a new revision.
    const auto current = revisions(asset.id);
    QString workingHash;
    try { workingHash = streamDigest(asset.path); } catch (...) { workingHash.clear(); }
    if (current.isEmpty() || current.first().hash != workingHash) {
        guard.state = QStringLiteral("unrecorded");
        guard.protectBytes = QFileInfo(asset.path).size();
    }
    // Space estimate: the protection (a new object of the unrecorded content)
    // plus the restored working-copy write must fit the free space.
    guard.freeBytes = QStorageInfo(root_).bytesAvailable();
    if (guard.freeBytes < guard.protectBytes + guard.restoreBytes)
        guard.state = QStringLiteral("nospace");
    return guard;
}
