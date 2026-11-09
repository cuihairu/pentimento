// Backup packages and read-only audits: export/verify/restore rehearsal
// (P-516/P-603), the consistency check (P-601) and the space preview
// (P-602) (LAY-002 split).
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

// P-516: export a consistent full backup package. The mutex is held for the
// whole export — that IS the brief write pause (the app runs one writer on a
// serial queue, so no import/capture/restore can interleave).
Library::BackupPackage Library::exportBackupPackage(const QString &destinationDir) const {
    const std::lock_guard<QRecursiveMutex> guard(mutex_);
    if (destinationDir.trimmed().isEmpty())
        fail(Library::tr("备份目标目录不能为空"));
    const QDir dest(destinationDir);
    // No silent overwrite: an existing target is refused; the caller picks a
    // fresh directory (or clears the old package themselves).
    if (dest.exists() && !QDir(destinationDir).entryList(QDir::AllEntries | QDir::NoDotAndDotDot).isEmpty())
        fail(Library::tr("备份目标已存在且不为空：%1（不静默覆盖既有备份，请选择新目录）").arg(destinationDir));
    if (!dest.mkpath(QStringLiteral(".")))
        fail(Library::tr("无法创建备份目标目录：%1").arg(destinationDir));
    const QString objectsDir = dest.filePath(QStringLiteral("objects"));
    if (!QDir().mkpath(objectsDir))
        fail(Library::tr("无法创建备份对象目录：%1").arg(objectsDir));

    BackupPackage out;
    out.packageDir = dest.absolutePath();
    // The manifest is built from completed revisions (the same consistency
    // model as snapshotAll): latest revision per live asset, object verified
    // before it is trusted. Deleted assets are not part of a live backup.
    QSet<QString> uniqueHashes;
    for (const auto &asset : assets()) {
        const auto history = revisions(asset.id);
        if (history.isEmpty()) continue;
        const auto &latest = history.first(); // newest-first
        if (latest.hash.size() != 64)
            fail(Library::tr("备份对象校验失败：素材 %1 的版本内容缺失").arg(asset.logicalPath()));
        const QFileInfo object(objectPath(latest.hash));
        if (!object.exists() || object.size() != latest.size)
            fail(Library::tr("备份对象校验失败：%1").arg(asset.logicalPath()));
        BackupEntry entry{asset.id, asset.name, asset.folder, latest.hash, latest.size, latest.id};
        out.entries.push_back(entry);
        if (uniqueHashes.contains(latest.hash)) continue; // content dedup: copy once
        uniqueHashes.insert(latest.hash);
        const QString target = objectsDir + QLatin1Char('/') + latest.hash;
        // Copy then verify the COPY (size + content hash) before trusting it:
        // a half-copied object must never end up in a package marked complete.
        copyFileStreaming(object.absoluteFilePath(), target);
        if (QFileInfo(target).size() != latest.size || digestFile(target) != latest.hash)
            fail(Library::tr("备份对象校验失败：%1").arg(asset.logicalPath()));
        ++out.objects;
        out.bytes += latest.size;
    }

    QJsonArray entries;
    for (const auto &e : out.entries)
        entries.append(QJsonObject{{QStringLiteral("assetId"), e.assetId},
                                   {QStringLiteral("name"), e.name},
                                   {QStringLiteral("folder"), e.folder},
                                   {QStringLiteral("hash"), e.hash},
                                   {QStringLiteral("size"), static_cast<double>(e.size)},
                                   {QStringLiteral("revisionId"), static_cast<double>(e.revisionId)}});
    QJsonObject manifest;
    manifest.insert(QStringLiteral("format"), QStringLiteral("pentimento-backup-v1"));
    const QString exported = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
    manifest.insert(QStringLiteral("exported"), exported);
    manifest.insert(QStringLiteral("entries"), entries);
    const QString manifestPath = dest.filePath(QStringLiteral("manifest.json"));
    writeFile(manifestPath, QJsonDocument(manifest).toJson(QJsonDocument::Indented));
    out.manifestPath = manifestPath;
    // Completion marker LAST: a package without it is a half-package and is
    // never reported as success (verifyBackupPackage refuses it).
    const QString markerPath = dest.filePath(QStringLiteral("COMPLETE"));
    writeFile(markerPath, exported.toUtf8());
    out.markerPath = markerPath;
    return out;
}

QVector<Library::BackupEntry> Library::verifyBackupPackage(const QString &packageDir) const {
    const QDir dir(packageDir);
    // The completion marker decides "success": a half-package (interrupted
    // export, disk-full) has no marker and is reported incomplete, never
    // silently accepted.
    if (!QFileInfo::exists(dir.filePath(QStringLiteral("COMPLETE"))))
        fail(Library::tr("备份包不完整（缺少完成标记）：%1").arg(packageDir));
    QFile manifestFile(dir.filePath(QStringLiteral("manifest.json")));
    if (!manifestFile.open(QIODevice::ReadOnly))
        fail(Library::tr("备份包缺少清单：%1").arg(packageDir));
    const QJsonDocument doc = QJsonDocument::fromJson(manifestFile.readAll());
    if (!doc.isObject() || doc.object().value(QStringLiteral("format")).toString()
                                != QStringLiteral("pentimento-backup-v1"))
        fail(Library::tr("备份包清单无法识别：%1").arg(packageDir));
    const auto arr = doc.object().value(QStringLiteral("entries")).toArray();
    QVector<BackupEntry> entries;
    QSet<QString> verified;
    for (const auto v : arr) {
        const QJsonObject o = v.toObject();
        BackupEntry entry;
        entry.assetId = o.value(QStringLiteral("assetId")).toString();
        entry.name = o.value(QStringLiteral("name")).toString();
        entry.folder = o.value(QStringLiteral("folder")).toString();
        entry.hash = o.value(QStringLiteral("hash")).toString();
        entry.size = static_cast<qint64>(o.value(QStringLiteral("size")).toDouble());
        entry.revisionId = static_cast<qint64>(o.value(QStringLiteral("revisionId")).toDouble());
        // A package object must exist with the declared size and the exact
        // content hash — anything else is a corrupt/half package, reported, not
        // accepted.
        if (!verified.contains(entry.hash)) {
            verified.insert(entry.hash);
            const QString objectPath = dir.filePath(QStringLiteral("objects/") + entry.hash);
            const QFileInfo object(objectPath);
            if (!object.exists() || object.size() != entry.size)
                fail(Library::tr("备份包对象缺失或尺寸不符：%1").arg(entry.name));
            if (digestFile(objectPath) != entry.hash)
                fail(Library::tr("备份包对象校验失败：%1").arg(entry.name));
        }
        entries.push_back(entry);
    }
    return entries;
}

QVector<Library::BackupEntry> Library::restoreBackupPackage(const QString &packageDir) {
    const std::lock_guard<QRecursiveMutex> guard(mutex_);
    // P-603: the drill runs in an independent empty library — restoring into one
    // that already holds assets would overwrite an original library, which the
    // rehearsal must never do.
    if (!assets().isEmpty())
        fail(Library::tr("恢复演练目标库非空（不覆盖既有素材库）"));
    // The package itself must be complete and self-consistent before anything
    // is materialized (marker + manifest + objects verified).
    const auto entries = verifyBackupPackage(packageDir);
    if (entries.isEmpty())
        fail(Library::tr("备份包清单为空：%1").arg(packageDir));
    const QDir dir(packageDir);
    QVector<BackupEntry> restored;
    QSet<QString> copiedObjects;
    if (!db().transaction()) fail(db().lastError().text());
    try {
        for (const auto &entry : entries) {
            const QString objectSource = dir.filePath(QStringLiteral("objects/") + entry.hash);
            // Content dedup: one object copy per hash, verified after copy.
            if (!copiedObjects.contains(entry.hash)) {
                copiedObjects.insert(entry.hash);
                copyFileStreaming(objectSource, root_ + QStringLiteral("/objects/") + entry.hash);
                const QString copied = root_ + QStringLiteral("/objects/") + entry.hash;
                if (QFileInfo(copied).size() != entry.size || digestFile(copied) != entry.hash)
                    fail(Library::tr("备份包对象校验失败：%1").arg(entry.name));
            }
            // Reconstruct the asset with its recorded identity: the manifest's
            // asset id, folder/name path, and latest-revision content. Revision
            // ids are new (autoincrement) but the hash/size are preserved.
            const QString relative = QStringLiteral("working/%1/%2").arg(entry.assetId, entry.name);
            const QString workingPath = root_ + QLatin1Char('/') + relative;
            if (!QDir().mkpath(QFileInfo(workingPath).absolutePath()))
                fail(Library::tr("无法创建工作目录"));
            copyFileStreaming(objectSource, workingPath);
            if (digestFile(workingPath) != entry.hash)
                fail(Library::tr("恢复后内容校验失败：%1").arg(entry.name));
            QSqlQuery a(db());
            a.prepare("INSERT INTO assets(id,name,path,folder) VALUES(?,?,?,?)");
            a.addBindValue(entry.assetId);
            a.addBindValue(entry.name);
            a.addBindValue(relative);
            a.addBindValue(entry.folder.isNull() ? QStringLiteral("") : entry.folder);
            execute(a);
            QSqlQuery facts(db());
            facts.prepare("UPDATE assets SET format=?,width=?,height=? WHERE id=?");
            const auto metadata = preview::metadata(objectSource);
            facts.addBindValue(metadata.format.isEmpty()
                                   ? QStringLiteral("")
                                   : metadata.format.toUpper());
            facts.addBindValue(metadata.width);
            facts.addBindValue(metadata.height);
            facts.addBindValue(entry.assetId);
            execute(facts);
            QSqlQuery r(db());
            r.prepare("INSERT INTO revisions(asset_id,hash,created,note,size,pinned,pin_reason) "
                      "VALUES(?,?,?,?,?,0,'')");
            r.addBindValue(entry.assetId);
            r.addBindValue(entry.hash);
            r.addBindValue(QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
            r.addBindValue(QStringLiteral("从备份包恢复演练"));
            r.addBindValue(entry.size);
            execute(r);
            restored.push_back(entry);
        }
        if (!db().commit()) fail(db().lastError().text());
    } catch (...) {
        db().rollback();
        throw;
    }
    return restored;
}

Library::ConsistencyReport Library::checkConsistency(
    const std::function<void(int percent, const QString &current)> &progress) const {
    const std::lock_guard<QRecursiveMutex> guard(mutex_);
    ConsistencyReport report;
    // Reference map: every revision (including history of deleted assets — the
    // timeline still owns those objects) pins its content hash.
    QHash<QString, qint64> hashSize;
    QHash<QString, int> refs;
    {
        QSqlQuery q(db());
        q.prepare("SELECT hash,size FROM revisions");
        execute(q);
        while (q.next()) {
            const QString hash = q.value(0).toString();
            refs[hash]++;
            hashSize[hash] = q.value(1).toLongLong();
            ++report.revisions;
        }
    }
    const QString objectsDir = root_ + QStringLiteral("/objects");
    // Missing / corrupt / I/O: one check per referenced hash.
    const int total = qMax(1, refs.size());
    int done = 0;
    for (auto it = refs.constBegin(); it != refs.constEnd(); ++it, ++done) {
        const QString hash = it.key();
        if (progress) progress(done * 100 / total, hash);
        // Build the path directly: a malformed hash in the index is reported as
        // missing rather than throwing (the check must never die on bad data).
        const QString objectPath = objectsDir + QLatin1Char('/') + hash;
        const QFileInfo object(objectPath);
        if (!object.exists()) {
            report.missing << hash;
            continue;
        }
        if (object.size() != hashSize.value(hash)) {
            report.corrupt << hash;
            continue;
        }
        // Read the object (streaming) and verify its content hash.
        QFile file(objectPath);
        if (!file.open(QIODevice::ReadOnly)) {
            report.ioErrors << hash;
            continue;
        }
        QCryptographicHash verifier(QCryptographicHash::Sha256);
        constexpr qint64 kChunk = 1 << 20;
        bool ioError = false;
        while (!file.atEnd()) {
            const QByteArray chunk = file.read(kChunk);
            if (chunk.isEmpty() && file.error() != QFileDevice::NoError) {
                ioError = true;
                break;
            }
            verifier.addData(chunk);
        }
        if (ioError) {
            report.ioErrors << hash;
            continue;
        }
        if (QString::fromLatin1(verifier.result().toHex()) != hash) {
            report.corrupt << hash;
            continue;
        }
        ++report.ok;
    }
    // Orphan: an object file no revision references. History objects of deleted
    // assets ARE referenced, so only truly unreferenced files are orphans.
    const QDir objects(objectsDir);
    if (objects.exists()) {
        const auto files = objects.entryList(QDir::Files | QDir::NoDotAndDotDot, QDir::Name);
        report.objects = files.size();
        for (const auto &name : files)
            if (!refs.contains(name)) report.orphan << name;
    }
    return report;
}

Library::SpacePreview Library::spacePreview() const {
    const std::lock_guard<QRecursiveMutex> guard(mutex_);
    SpacePreview p;
    QHash<QString, int> refs;        // hash -> number of revisions referencing it
    QHash<QString, qint64> hashSize; // hash -> declared revision size
    QSet<QString> liveAssets;
    {
        QSqlQuery q(db());
        q.prepare("SELECT asset_id,hash,size,pinned FROM revisions");
        execute(q);
        while (q.next()) {
            liveAssets.insert(q.value(0).toString());
            ++p.revisions;
            const QString hash = q.value(1).toString();
            const qint64 size = q.value(2).toLongLong();
            ++refs[hash];
            hashSize[hash] = size;
            // Naive manifest sum: a content hash shared across versions counts
            // once per revision, which is exactly what overstates usage.
            p.logicalSize += size;
            if (q.value(3).toBool()) {  // P-307 pinned: excluded from cleanup
                ++p.pinnedRevisions;
                p.pinnedBytes += size;
            }
        }
    }
    p.assets = liveAssets.size();
    // Physical occupancy and the shared/orphan split come from the object store,
    // keyed by how many revisions actually reference each object file.
    const QDir objects(root_ + QStringLiteral("/objects"));
    if (objects.exists()) {
        const auto files = objects.entryList(QDir::Files | QDir::NoDotAndDotDot, QDir::Name);
        for (const auto &name : files) {
            const qint64 size = hashSize.value(name, QFileInfo(objects.filePath(name)).size());
            p.physicalSize += size;
            const int n = refs.value(name, 0);
            if (n == 0) {
                // Unreferenced object: pure waste today — the only thing a
                // cleanup could actually free without touching any history.
                ++p.orphanObjects;
                p.reclaimableBytes += size;
            } else if (n > 1) {
                // Shared object: counted once on disk but once per reference in
                // the naive manifest sum — that is the dedup it provides.
                ++p.sharedObjects;
                p.dedupSavings += (qint64(n) - 1) * size;
            }
        }
    }
    return p;
}

