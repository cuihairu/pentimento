// External directory indexing (P-204): read-only index of a tree the user
// owns elsewhere — nothing is copied or modified at the source
// (LAY-002 split).
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

// --- External directory indexing (P-204) -------------------------------------

namespace {
QString externalStateName(Library::ExternalAssetState state) {
    switch (state) {
    case Library::ExternalAssetState::Present: return QStringLiteral("present");
    case Library::ExternalAssetState::Missing: return QStringLiteral("missing");
    case Library::ExternalAssetState::Unreadable: return QStringLiteral("unreadable");
    }
    return QStringLiteral("present");
}

Library::ExternalAssetState externalStateFromName(const QString &name) {
    if (name == QLatin1String("missing")) return Library::ExternalAssetState::Missing;
    if (name == QLatin1String("unreadable")) return Library::ExternalAssetState::Unreadable;
    return Library::ExternalAssetState::Present;
}
} // namespace

qint64 Library::addExternalRoot(const QString &path) {
    const std::lock_guard<QRecursiveMutex> guard(mutex_);
    const QFileInfo info(path);
    if (!info.exists() || !info.isDir() || !info.isReadable())
        fail(tr("外部目录不可读：%1").arg(path));
    const QString canonical = QDir(path).canonicalPath();
    // The index must describe a tree OUTSIDE the library: a root inside the
    // library would double-count content the catalog already owns and blur
    // the import/index boundary.
    if (canonical == root_ || canonical.startsWith(root_ + QLatin1Char('/')))
        fail(tr("外部目录不能位于素材库内部：%1").arg(path));
    QSqlQuery check(db());
    check.prepare(QStringLiteral("SELECT id FROM external_roots WHERE path=?"));
    check.addBindValue(canonical);
    if (!check.exec()) fail(check.lastError().text());
    if (check.next())
        fail(tr("该目录已在索引中：%1").arg(path));
    QSqlQuery ins(db());
    ins.prepare(QStringLiteral("INSERT INTO external_roots(path,added) VALUES(?,?)"));
    ins.addBindValue(canonical);
    ins.addBindValue(QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
    if (!ins.exec()) fail(ins.lastError().text());
    return ins.lastInsertId().toLongLong();
}

void Library::removeExternalRoot(qint64 rootId) {
    const std::lock_guard<QRecursiveMutex> guard(mutex_);
    QSqlQuery del(db());
    del.prepare(QStringLiteral("DELETE FROM external_assets WHERE root_id=?"));
    del.addBindValue(rootId);
    if (!del.exec()) fail(del.lastError().text());
    del.prepare(QStringLiteral("DELETE FROM external_roots WHERE id=?"));
    del.addBindValue(rootId);
    if (!del.exec()) fail(del.lastError().text());
    if (del.numRowsAffected() != 1)
        fail(tr("未知的索引目录 id：%1").arg(rootId));
}

QVector<Library::ExternalRoot> Library::externalRoots() const {
    const std::lock_guard<QRecursiveMutex> guard(mutex_);
    QSqlQuery q(db());
    if (!q.exec(QStringLiteral("SELECT id,path,added FROM external_roots ORDER BY id")))
        fail(q.lastError().text());
    QVector<ExternalRoot> roots;
    while (q.next())
        roots.push_back({q.value(0).toLongLong(), q.value(1).toString(), q.value(2).toString()});
    return roots;
}

int Library::indexExternalRoot(qint64 rootId) {
    const std::lock_guard<QRecursiveMutex> guard(mutex_);
    QSqlQuery q(db());
    q.prepare(QStringLiteral("SELECT path,pass FROM external_roots WHERE id=?"));
    q.addBindValue(rootId);
    if (!q.exec()) fail(q.lastError().text());
    if (!q.next())
        fail(tr("未知的索引目录 id：%1").arg(rootId));
    const QString rootPath = q.value(0).toString();
    // Each pass gets a monotonic marker: two refreshes can land inside the
    // same wall-clock millisecond, so a timestamp cannot tell "seen this
    // pass" from "seen the previous one". A per-root counter always differs.
    const qint64 pass = q.value(1).toLongLong() + 1;
    QSqlQuery bump(db());
    bump.prepare(QStringLiteral("UPDATE external_roots SET pass=? WHERE id=?"));
    bump.addBindValue(pass);
    bump.addBindValue(rootId);
    if (!bump.exec()) fail(bump.lastError().text());

    // The walk reads only: stat for size/mtime, a streaming SHA-256 for the
    // hash. NoSymlinks keeps the index inside the root (P-205 loop rule).
    int indexed = 0;
    QDirIterator it(rootPath, QDir::Files | QDir::NoDotAndDotDot
        | QDir::NoSymLinks | QDir::Hidden, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString filePath = it.next();
        const QString relative = QDir(rootPath).relativeFilePath(filePath);
        if (relative.startsWith(QLatin1String("../"))) continue; // never record above the root
        const QFileInfo info = it.fileInfo();
        QString hashHex;
        qint64 size = 0;
        const bool readable = info.isReadable() && hashFile(filePath, &hashHex, &size);
        QSqlQuery upsert(db());
        if (readable) {
            upsert.prepare(QStringLiteral(
                "INSERT INTO external_assets(root_id,rel_path,hash,size,mtime,state,last_seen) "
                "VALUES(?,?,?,?,?,'present',?) "
                "ON CONFLICT(root_id,rel_path) DO UPDATE SET hash=excluded.hash,size=excluded.size,"
                "mtime=excluded.mtime,state='present',last_seen=excluded.last_seen"));
            upsert.addBindValue(rootId);
            upsert.addBindValue(relative);
            upsert.addBindValue(hashHex);
            upsert.addBindValue(size);
            upsert.addBindValue(info.lastModified().toMSecsSinceEpoch());
            upsert.addBindValue(pass);
            ++indexed;
        } else {
            // Unreadable this pass: keep the last good hash/size, flip state,
            // stamp the pass so it is not mistaken for a vanished file.
            upsert.prepare(QStringLiteral(
                "INSERT INTO external_assets(root_id,rel_path,hash,size,mtime,state,last_seen) "
                "VALUES(?,?,'',?,?,'unreadable',?) "
                "ON CONFLICT(root_id,rel_path) DO UPDATE SET state='unreadable',last_seen=excluded.last_seen"));
            upsert.addBindValue(rootId);
            upsert.addBindValue(relative);
            upsert.addBindValue(info.size());
            upsert.addBindValue(info.lastModified().toMSecsSinceEpoch());
            upsert.addBindValue(pass);
        }
        if (!upsert.exec()) fail(upsert.lastError().text());
    }
    // Entries whose file was not seen this pass keep their row (identity
    // history stays visible) and flip to missing.
    QSqlQuery stale(db());
    stale.prepare(QStringLiteral(
        "UPDATE external_assets SET state='missing' WHERE root_id=? AND last_seen<>? AND state<>'missing'"));
    stale.addBindValue(rootId);
    stale.addBindValue(pass);
    if (!stale.exec()) fail(stale.lastError().text());
    return indexed;
}

QVector<Library::ExternalAsset> Library::externalAssets(qint64 rootId) const {
    const std::lock_guard<QRecursiveMutex> guard(mutex_);
    QSqlQuery q(db());
    q.prepare(QStringLiteral(
        "SELECT rel_path,hash,size,mtime,state FROM external_assets WHERE root_id=? ORDER BY rel_path"));
    q.addBindValue(rootId);
    if (!q.exec()) fail(q.lastError().text());
    QVector<ExternalAsset> assets;
    while (q.next())
        assets.push_back({q.value(0).toString(), q.value(1).toString(), q.value(2).toLongLong(),
                          q.value(3).toLongLong(), externalStateFromName(q.value(4).toString())});
    return assets;
}

