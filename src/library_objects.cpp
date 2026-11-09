// Object storage (P-302): content-addressed, immutable objects under
// objects/<sha256> — staged in objects/.tmp, hash-verified, published by
// rename, plus the read/verify paths and idempotent re-hardening
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

bool Library::verifyObject(const QString &hash) const {
    const std::lock_guard<QRecursiveMutex> guard(mutex_);
    QString actual;
    // Missing/unreadable/hash-mismatch all report false; the object is never
    // loaded whole (streaming), so a UI refresh over history costs no large
    // in-memory read per revision.
    if (!hashFile(objectPath(hash), &actual, nullptr)) return false;
    return actual == hash;
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

// Returns the exact number of bytes staged, so a caller can record the revision
// size of the content it stored (not a later re-stat of a possibly-changed
// working copy — an audit finding flagged that TOCTOU, which would make a
// subsequent snapshot's object.size()!=objectSize check fail).
qint64 Library::storeObjectFromFile(const QString &sourcePath, const QString &hash) {
    const QString path = objectPath(hash);
    if (QFileInfo::exists(path)) {
        if (digestFile(path) != hash) fail(Library::tr("历史对象校验失败；已停止操作"));
        platform::makeImmutable(path);
        return QFileInfo(path).size();
    }
    const QString temp = tempObjectPath();
    qint64 size = 0;
    try {
        size = copyFileStreaming(sourcePath, temp);
        publishObject(temp, hash);
    } catch (...) {
        QFile::remove(temp);
        throw;
    }
    return size;
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

