#pragma once

// Internal helpers shared by the library_* translation units (LAY-002: the
// former single 2.4k-line library.cpp is split into per-domain files). The
// Library class API in library.h is unchanged; these helpers stay private to
// the library implementation and are inline so every TU sees the same bodies.
#include "library.h"

#include <QCryptographicHash>
#include <QFile>
#include <QSaveFile>
#include <QSqlError>
#include <QSqlQuery>
#include <stdexcept>

namespace library_internal {

// Every domain error surfaces as std::runtime_error with a user-readable,
// translated message (the UI catches and shows it; tests assert on it).
[[noreturn]] inline void fail(const QString &message) { throw std::runtime_error(message.toStdString()); }

inline QString digest(const QByteArray &bytes) {
    return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}

inline QByteArray readFile(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) fail(Library::tr("无法读取文件：%1").arg(path));
    // Bounded in-memory first release; do not attempt huge PSD/model imports.
    if (file.size() > kMaxObjectBytes) fail(Library::tr("首版单文件上限为 256 MiB：%1").arg(path));
    const auto bytes = file.readAll();
    if (file.error() != QFileDevice::NoError) fail(file.errorString());
    return bytes;
}

inline void writeFile(const QString &path, const QByteArray &bytes) {
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit())
        fail(Library::tr("无法安全写入文件：%1；%2").arg(path, file.errorString()));
}

inline void execute(QSqlQuery &query) {
    if (!query.exec()) fail(query.lastError().text());
}

inline QString digestFile(const QString &path) { return digestFileBounded(path); }
inline qint64 copyFileStreaming(const QString &from, const QString &to) {
    QFile in(from), out(to);
    if (!in.open(QIODevice::ReadOnly)) fail(Library::tr("无法读取文件：%1").arg(from));
    if (!out.open(QIODevice::WriteOnly)) fail(Library::tr("无法写入暂存对象：%1").arg(to));
    constexpr qint64 kChunk = 1 << 20; // 1 MiB
    qint64 total = 0;
    while (!in.atEnd()) {
        const auto chunk = in.read(kChunk);
        if (chunk.isEmpty() && in.error() != QFileDevice::NoError)
            fail(Library::tr("无法读取文件：%1").arg(from));
        if (out.write(chunk) != chunk.size())
            fail(Library::tr("无法写入暂存对象：%1").arg(to));
        total += chunk.size();
    }
    out.close();
    if (out.error() != QFileDevice::NoError)
        fail(Library::tr("无法写入暂存对象：%1").arg(to));
    return total;
}

} // namespace library_internal
