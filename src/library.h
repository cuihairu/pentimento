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
    // P-309 current metadata (display name, tags, rating, note, source).
    // rating is 0..5, or -1 for "no rating" (no rating != 0 stars).
    QStringList tags;
    int rating = -1;
    QString note;
    QString source;
    // P-310 in-app confirmed delete: the asset is tombstoned (deleted=1) but
    // its history and objects are retained so it stays findable/recoverable.
    bool deleted = false;
};

// P-310 asset availability state. Present = working copy exists; Tombstone =
// user confirmed delete in-app (history retained); Missing = the working copy
// is absent without a tombstone (accidental/external removal or a mount that
// is not reachable) — NOT treated as a user deletion, no tombstone is written.
enum class AssetState { Present, Missing, Tombstone };

// P-309 one metadata snapshot. Rating -1 means "no rating" (a distinct state
// from 0 stars). Tags serialize as a JSON array (robust for any tag text).
struct Metadata {
    QString name;
    QStringList tags;
    int rating = -1;
    QString note;
    QString source;
};

// P-309 one per-field metadata change, viewable read-only. `field` is one of
// "name"/"tags"/"rating"/"note"/"source"; values are recorded as actually
// stored (never fabricated); `revisionId` is the revision that carried the
// change (same contentHash as the prior revision — no new content object).
struct MetadataChange {
    qint64 revisionId = 0;
    QString field;
    QString oldValue;
    QString newValue;
    QString changed;
};

struct Revision {
    qint64 id = 0;
    QString hash;
    QString created;
    QString note;
    qint64 size = 0;
    // P-307 critical-version pin. A pinned revision is never a retention-cleanup
    // candidate (see retentionCandidates); `pinReason` records why it was pinned.
    bool pinned = false;
    QString pinReason;
    // P-310 deletion tombstone: this revision marks the in-app delete event
    // (reuses the last content hash — no new content object, history retained).
    bool tombstone = false;
};

// One whole-tree emergency anchor: each asset pinned to the revision that
// represented it when the snapshot was taken. `boundary` is the sequence point
// (max revision id) the snapshot was fixed at; every manifest entry references
// a revision at or before it.
struct Snapshot {
    qint64 id = 0;
    QString created;
    QString note;
    int assets = 0;
    qint64 boundary = 0;
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

// P-308 one entry on a single asset's timeline. Ordering is by the deterministic
// revision id (never by the display timestamp), so a clock rollback or timezone
// change cannot reorder history. `current` marks the revision whose content IS
// the current working copy (derived from content, not from UI selection);
// `restore` marks a revision produced by a restore ("恢复历史版本 #N");
// `corrupt` marks a revision whose object failed verification.
struct TimelineEntry {
    qint64 revisionId = 0;
    QString hash;
    QString created;
    QString note;
    qint64 size = 0;
    bool pinned = false;
    QString pinReason;
    bool current = false;
    bool restore = false;
    bool corrupt = false;
};

// The whole timeline for one asset, newest first. `unrecorded` is true when the
// working copy exists but its content matches no recorded revision (an unsaved
// modification) — no entry is then marked current.
struct AssetTimeline {
    QVector<TimelineEntry> entries;
    bool unrecorded = false;
};

// P-311 one entry of a consistent whole-library snapshot manifest. `path` is
// root-relative as of the snapshot; `contentHash` is the anchored revision's
// hash; `state` is "present" | "tombstone" | "missing" at the snapshot moment.
// All entries reference revisions at or before the snapshot's sequence point,
// so the listing is one complete, consistent set (never a per-file scan).
struct SnapshotEntry {
    QString assetId;
    QString path;
    QString contentHash;
    QString state;
    qint64 revisionId = 0;
};

// P-312 one exclusion recorded against a snapshot: the asset was left out of
// that snapshot by a P-306 exclusion rule, and `reason` says why. `path` is the
// root-relative path as of the snapshot (never the current index). Read-only;
// shown by the time-point browsing surface so an excluded asset is not silently
// absent from the directory (it existed then but was never a rollback anchor).
struct SnapshotExclusion {
    QString assetId;
    QString reason;
    QString path;
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
    // P-308 single-asset timeline: deterministic order (revision id), display
    // timestamps, notes, and accurate current/history/restore/corrupt marks
    // plus the unrecorded-modification flag. See AssetTimeline.
    AssetTimeline assetTimeline(const QString &assetId) const;
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
    // P-307 critical-version pinning. A pinned revision is kept out of retention
    // cleanup and its object is never treated as replaceable. Pinning/unpinning
    // is persistent and survives reopen; unpinning does NOT delete history.
    void pinRevision(const QString &assetId, qint64 revisionId, const QString &reason);
    void unpinRevision(const QString &assetId, qint64 revisionId);
    // The set of revision ids a retention policy may remove: non-pinned, and not
    // the newest (current) revision. This is the seam the future V1 retention
    // policy (P-602) consumes; pinned versions are always excluded (P-307).
    QVector<qint64> retentionCandidates(const QString &assetId) const;
    // P-309 metadata history. Current metadata is readable; updates record
    // per-field old/new values with a change timestamp and append a revision
    // that reuses the newest content hash (no new content object). History is
    // read-only; no fabrication of values, no auto-merge, no auto-revert.
    Metadata metadata(const QString &assetId) const;
    QVector<MetadataChange> metadataHistory(const QString &assetId) const;
    // Records a metadata change and returns the revision that carries it
    // (unchanged=true when no field actually changed — the current revision id
    // is returned so the caller can report "already up to date").
    RecordResult updateMetadata(const QString &assetId, const Metadata &newMeta,
                                const QString &note = QString());
    // P-310 deletion tombstone. In-app confirmed delete writes a tombstone
    // revision (history and objects retained — the asset stays findable and
    // recoverable via restore) and marks the asset deleted. Missing working
    // copies are never tombstoned (that is NOT a user deletion).
    void deleteAsset(const Asset &asset, const QString &note = QString());
    // The asset's availability state (Present / Missing / Tombstone).
    AssetState assetState(const QString &assetId) const;
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
    // P-311 the snapshot's manifest: every accounted asset (present/tombstone/
    // missing; excluded assets are recorded separately via snapshotExclusionReason)
    // with its root-relative path and anchored content hash, all from one
    // sequence point. Read-only; drives time-point browsing (P-312).
    QVector<SnapshotEntry> snapshotManifest(qint64 snapshotId) const;
    // P-312 the assets a snapshot excluded (rule-matched, never a rollback
    // anchor), with the recorded reason each. Read-only, never mixed with the
    // current index; the browsing surface shows these so the directory tree of
    // that time point is complete and honest.
    QVector<SnapshotExclusion> snapshotExclusions(qint64 snapshotId) const;
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
    qint64 append(const Asset &asset, const QByteArray &bytes, const QString &note);
    qint64 appendFromFile(const Asset &asset, const QString &source, const QString &note);
    qint64 insertRevision(const Asset &asset, const QString &hash, qint64 size, const QString &note);
    QString newTempObjectPath() const;
    StoredObject storeBytes(const QByteArray &bytes);
    StoredObject publishTempObject(const QString &temp, qint64 size);
};
