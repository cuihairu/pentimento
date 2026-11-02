#include "storage.h"

#include <QCryptographicHash>
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
