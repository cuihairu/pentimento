#pragma once

#include <QSqlDatabase>
#include <QString>
#include <QVector>
#include <memory>
#include <stdexcept>

class QLockFile;

struct Asset {
    QString id;
    QString name;
    QString path;
};

struct Revision {
    qint64 id = 0;
    QString hash;
    QString created;
    QString note;
    qint64 size = 0;
};

// One whole-tree emergency anchor: each asset pinned to the revision that
// represented it when the snapshot was taken.
struct Snapshot {
    qint64 id = 0;
    QString created;
    QString note;
    int assets = 0;
};

// One object stored in the content-addressed store. `hash` is the SHA-256 of
// the stored bytes (lowercase hex); `size` is the byte count that was stored.
struct StoredObject {
    QString hash;
    qint64 size = 0;
};

// One library, one writer. Original imports are never modified.
class Library {
public:
    explicit Library(const QString &root);
    ~Library();
    Library(const Library &) = delete;
    Library &operator=(const Library &) = delete;
    QVector<Asset> assets() const;
    QVector<Revision> revisions(const QString &assetId) const;
    Asset importFile(const QString &source);
    bool capture(const Asset &asset, const QString &note = QString());
    void restore(const Asset &asset, qint64 revisionId);
    void exportRevision(const QString &assetId, qint64 revisionId, const QString &destination) const;
    QString objectPath(const QString &hash) const;
    QByteArray readObject(const QString &hash) const;
    // P-302 content object store. Streams the file at `source` through a
    // temporary object (objects/.tmp/), hashes the finished temp, then publishes
    // it under its content address. Identical content reuses the existing object
    // (dedup: the existing object is verified before it is trusted). A failed
    // temp write removes the temp and publishes nothing. Never loads the whole
    // file into memory.
    StoredObject storeObject(const QString &source);
    // Anti-ransomware: re-apply immutability to any object left writable (idempotent).
    int hardenObjects();
    // Anti-ransomware: freeze current working state of every asset as one anchor.
    qint64 snapshotAll(const QString &note);
    QVector<Snapshot> snapshots() const;
    // Anti-ransomware: one-click whole-tree rollback to a snapshot anchor. Each
    // asset is restored via the normal "restore forms a new version" path, so
    // later history is preserved rather than destroyed. Returns how many assets
    // actually changed. Assets already at the anchor content are skipped.
    int rollbackAll(qint64 snapshotId);
    QString root() const { return root_; }
private:
    QString root_;
    QString connection_;
    QSqlDatabase db_;
    std::unique_ptr<QLockFile> lock_;
    void append(const Asset &asset, const QByteArray &bytes, const QString &note);
    void appendFromFile(const Asset &asset, const QString &source, const QString &note);
    void insertRevision(const Asset &asset, const QString &hash, qint64 size, const QString &note);
    QString newTempObjectPath() const;
    StoredObject storeBytes(const QByteArray &bytes);
    StoredObject publishTempObject(const QString &temp, qint64 size);
};
