#pragma once

#include <QDateTime>
#include <QString>
#include <QStringList>
#include <QVector>

// P-508: merge discovery results and expire stale ones. A "record" is one
// discovered service; the same NAS may be reachable through several
// addresses/protocols, so records are merged by a stable `key` and the
// addresses are unioned so no reachable option is dropped. Pure logic, no
// I/O — callers supply the discovery results and the current time.
namespace discovery {

struct Record {
    QString key;           // stable identity shared by all addresses of one NAS
    QStringList addresses; // e.g. "smb://nas.local", "sftp://nas.local"
    QDateTime lastSeen;    // refreshed by merge(); set to `now` on first sight
    qint64 ttlMs = 0;      // 0 = never expires
};

// Merge `incoming` into `existing` as of `now`:
//  - records by matching key are refreshed (lastSeen=now) and their address
//    lists are unioned in order, without duplicates;
//  - an incoming record with a new key is added with lastSeen=now;
//  - existing records absent from `incoming` are kept unless their ttl
//    (ttlMs > 0) has elapsed since lastSeen.
QVector<Record> merge(const QVector<Record> &existing,
                      const QVector<Record> &incoming,
                      const QDateTime &now);

} // namespace discovery
