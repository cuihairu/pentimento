#pragma once

#include <QHash>
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
// `created` is true when this call published a new object (dedup hit otherwise).
struct StoredObject {
    QString hash;
    qint64 size = 0;
    bool created = false;
};

// Result of recording a version. `revisionId` is the queryable version id that
// now represents the asset; `note` is the note attached to that version;
// `unchanged` is true when the content was already the newest recorded version
// (no duplicate version was created) — the returned id is then the existing one.
struct RecordResult {
    qint64 revisionId = 0;
    QString note;
    bool unchanged = false;
};

// Result of one auto-record tick. Exactly one of `recorded` / `unchanged` /
// `retry` may be true: a new version was recorded, the content was already the
// newest version, or the file moved during the read (so nothing was recorded and
// the caller should observe again next tick).
struct StableTick {
    bool recorded = false;
    bool unchanged = false;
    bool retry = false;
    qint64 revisionId = 0;
};

// P-305 scheduled-snapshot state, visible to the user. A schedule is OFF by
// default; enabling it at a 15/30/60-minute period computes the next fire time
// from the enable moment. `paused` stops automatic snapshots and keeps the
// reason ("user"/"system") for display.
struct ScheduleState {
    bool enabled = false;
    int periodMinutes = 0;
    qint64 nextUtcMs = 0;
    bool paused = false;
    QString pauseReason;
};

// P-306 exclusion rule. `kind` is "extension" (match the asset name's suffix)
// or "glob" (match the asset name or its path under the library root). `reason`
// records why the match excludes the asset from snapshots.
struct ExclusionRule {
    qint64 id = 0;
    QString kind;
    QString pattern;
    QString reason;
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
    // P-303 manual version record. Always available: records the asset's current
    // working content as a new version and returns its queryable id and note.
    // When the content is already the newest recorded version, no duplicate is
    // created and the existing version id is returned with unchanged=true, so the
    // caller can tell the user what is already protected. Throws a loud error on
    // a failed read/write/index registration instead of silently doing nothing.
    RecordResult recordVersion(const Asset &asset, const QString &note = QString());
    // P-304 stable-change auto-record policy. Call once per timer tick per
    // asset: records the working copy only after TWO consecutive ticks observe
    // the same size+mtime signature (half-written and continuously-written
    // files stay unstable and are not recorded). See recordStableVersion for
    // the read-before/after recheck.
    StableTick autoRecordTick(const Asset &asset, const QString &note = QString());
    // Records the asset's current content, but only if it still matches the
    // observed size+mtime signature BOTH before and after streaming the file.
    // A file that moved during the read (a torn mid-write snapshot) is never
    // recorded: any fresh object it created is discarded and retry=true is
    // returned so the caller observes again.
    StableTick recordStableVersion(const Asset &asset, qint64 size, qint64 mtimeMs,
                                   const QString &note = QString());
    // P-305 scheduled snapshots. The schedule runs only while the app runs (not
    // a resident system service); exit time is not counted as continuous backup.
    ScheduleState scheduleState() const;
    void enableSchedule(int periodMinutes); // 15/30/60, throws on anything else
    void disableSchedule();
    void pauseSchedule(const QString &reason); // "user"/"system"
    void resumeSchedule();
    // Called periodically (e.g. once a minute). Fires at most ONE snapshot when
    // the due time is reached; several missed periods collapse to one. Returns
    // the snapshot id, or 0 when none was taken (not due / disabled / paused).
    qint64 pumpSchedule(qint64 nowUtcMs);
    // P-306 exclusion rules. A matched asset is left out of subsequent snapshots
    // (never a rollback anchor) but stays in the library and keeps its history:
    // being excluded is NOT being deleted. The matching rule's reason is recorded
    // per snapshot.
    QVector<ExclusionRule> exclusionRules() const;
    qint64 addExclusionRule(const QString &kind, const QString &pattern, const QString &reason);
    void removeExclusionRule(qint64 id);
    bool isExcluded(const Asset &asset, QString *reason = nullptr) const;
    // The recorded reason an asset was excluded from a snapshot (empty if not).
    QString snapshotExclusionReason(qint64 snapshotId, const QString &assetId) const;
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
    // P-304 per-asset observation state: signature observed on the previous tick
    // (waiting for a second consecutive match) and the signature already recorded.
    QHash<QString, QPair<qint64, qint64>> stablePending_;
    QHash<QString, QPair<qint64, qint64>> stableRecorded_;
    // P-305 schedule state (runtime only; the schedule is not a resident service).
    ScheduleState schedule_;
    void append(const Asset &asset, const QByteArray &bytes, const QString &note);
    qint64 appendFromFile(const Asset &asset, const QString &source, const QString &note);
    qint64 insertRevision(const Asset &asset, const QString &hash, qint64 size, const QString &note);
    QString newTempObjectPath() const;
    StoredObject storeBytes(const QByteArray &bytes);
    StoredObject publishTempObject(const QString &temp, qint64 size);
};
