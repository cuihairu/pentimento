#include "storage.h"

#include <QCryptographicHash>
#include <QStorageInfo>
#include <QDir>
#include <QFile>
#include <QFileInfo>

namespace {
SourceState stateFor(const QFileInfo &info) {
    if (!info.exists()) return SourceState::Unreachable;
    if (!info.isReadable()) return SourceState::PermissionDenied;
    // Readable from the process's perspective. isWritable() applies to files AND
    // directories: a read-only mounted share (or a directory without write
    // permission) reports ReadOnly, which must not be treated as a healthy disk
    // or as unreachable — a read-only source is still listable/importable.
    return info.isWritable() ? SourceState::Reachable : SourceState::ReadOnly;
}
}

SourceInfo statSource(const QString &path) {
    const QFileInfo info(path);
    SourceInfo out;
    out.path = info.absoluteFilePath();
    out.isDir = info.isDir();
    out.state = stateFor(info);
    if (out.state == SourceState::Reachable || out.state == SourceState::ReadOnly) {
        out.size = info.isDir() ? -1 : info.size();
        out.mtimeMs = info.lastModified().toMSecsSinceEpoch();
        if (!info.isDir()) {
            QFile file(info.absoluteFilePath());
            if (file.open(QIODevice::ReadOnly))
                out.contentHash = QCryptographicHash::hash(file.readAll(), QCryptographicHash::Sha256);
        }
    }
    return out;
}

QVector<SourceInfo> listDirectory(const QString &path, SourceInfo *directoryState) {
    const QFileInfo info(path);
    const SourceInfo dir = statSource(path);
    if (directoryState) *directoryState = dir;
    if (!dir.isDir || dir.state == SourceState::Unreachable || dir.state == SourceState::PermissionDenied)
        return {};
    QVector<SourceInfo> entries;
    const QDir directory(info.absoluteFilePath());
    const auto children = directory.entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot,
                                                  QDir::DirsFirst | QDir::Name);
    for (const auto &child : children) {
        SourceInfo entry;
        entry.path = child.absoluteFilePath();
        entry.isDir = child.isDir();
        entry.state = stateFor(child);
        entry.size = child.isDir() ? -1 : child.size();
        entry.mtimeMs = child.lastModified().toMSecsSinceEpoch();
        entries.push_back(entry);
    }
    return entries;
}

// P-510: backup-target write acceptance. `statSource` answers the source-read
// question; this answers the backup-target-write question separately, so a
// read-only share is never mistaken for a valid backup destination.
BackupTargetInfo probeBackupTarget(const QString &path) {
    BackupTargetInfo out;
    const QString trimmed = path.trimmed();
    if (trimmed.isEmpty()) {
        out.reason = QObject::tr("备份目标目录不能为空");
        return out;
    }
    const QFileInfo info(trimmed);
    // The target may not exist yet: the parent must be writable so mkpath can
    // create it. A missing target with no writable parent is refused here.
    const QFileInfo probe = info.exists() ? info : QFileInfo(info.absolutePath());
    out.exists = info.exists();
    if (!probe.exists()) {
        out.reason = QObject::tr("备份目标不可达：%1").arg(trimmed);
        return out;
    }
    if (info.exists() && !info.isDir()) {
        out.reason = QObject::tr("备份目标必须是目录：%1").arg(trimmed);
        return out;
    }
    if (!probe.isReadable()) {
        out.reason = QObject::tr("备份目标不可读：%1").arg(trimmed);
        return out;
    }
    if (!probe.isWritable()) {
        out.readOnly = true;
        out.reason = QObject::tr("备份目标只读（无写权限或只读挂载）：%1").arg(trimmed);
        return out;
    }
    // A writable directory we can actually create a file in is the real
    // acceptance; mode bits alone can be misleading on a mounted share.
    out.writable = true;
    QStorageInfo storage(probe.absoluteFilePath());
    if (storage.isValid() && storage.isReadOnly()) {
        out.readOnly = true;
        out.writable = false;
        out.reason = QObject::tr("备份目标所在卷只读：%1").arg(trimmed);
        return out;
    }
    out.freeBytes = storage.isValid() ? storage.bytesAvailable() : 0;
    return out;
}
