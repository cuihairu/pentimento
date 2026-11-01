#pragma once

#include "backend.h"

#include <QCoreApplication>
#include <QRecursiveMutex>
#include <QSqlDatabase>
#include <QString>
#include <QVector>
#include <memory>
#include <mutex>
#include <stdexcept>

class QLockFile;

struct Asset {
    QString id;
    QString name;
    // Absolute working-file path under the current library root. Recomputed from
    // the stored relative path on every query, so a relocated library (moved to
    // another location or volume) resolves assets against its new root (P-202).
    QString path;
    // Path relative to the library root, as persisted in the catalog.
    QString relative;
    // Logical folder within the library (P-216): a library-relative path such as
    // "characters/hero", or "" for the root. The working file physically stays in
    // a per-asset directory; the folder is the organizational view.
    QString folder;
    // User-visible location of the asset: "<folder>/<name>" (or just name).
    QString logicalPath() const {
        return folder.isEmpty() ? name : folder + QStringLiteral("/") + name;
    }
};

struct Revision {
    qint64 id = 0;
    QString hash;
    QString created;
    QString note;
    qint64 size = 0;
    // P-307: a pinned revision carries a reason and is excluded from any future
    // retention cleanup — its object is never cleaned or overwritten. Unpinning
    // only clears the flag; history is never deleted by unpinning.
    bool pinned = false;
    QString pinReason;
    // P-310: a tombstone marks an in-app confirmed deletion. The revision keeps
    // the last content hash/size so history and objects stay intact, but the
    // asset is no longer listed as live. Tombstones are written only by
    // deleteAsset — never automatically for a missing or unreachable working
    // copy (those are recoverable states, not deletions).
    bool tombstone = false;
};

// One whole-tree emergency anchor: each asset pinned to the revision that
// represented it when the snapshot was taken.
struct Snapshot {
    qint64 id = 0;
    QString created;
    QString note;
    int assets = 0;      // present (backed-up) entries
    int excluded = 0;    // entries skipped by an exclusion rule (P-306)
};

// P-311: one manifest row of a consistent whole-tree snapshot. The path and
// content hash are denormalized as of the snapshot (later renames/moves/
// deletions never rewrite it), and state is the P-311 vocabulary
// present|tombstone|excluded|missing. `hash` is '' only for excluded entries
// (they have no content reference). P-312 browses exactly these rows.
struct SnapshotEntry {
    QString assetId;
    QString folder;     // logical folder at snapshot time
    QString name;       // file name at snapshot time
    QString hash;       // content hash of the referenced revision
    QString state;      // present | tombstone | excluded | missing
    QString reason;     // exclusion reason / tombstone note, '' otherwise
    qint64 revisionId = 0;
};

// One manual source/author/license entry, exactly as the user typed it (P-215).
// Empty fields mean unknown; nothing is inferred. Evidence is an attached file
// kept as an immutable content-addressed object inside the library.
struct Provenance {
    qint64 id = 0;
    QString source;      // where the asset came from (URL, store), empty = unknown
    QString author;      // who made it, empty = unknown
    QString license;     // license terms as typed by the user, empty = unknown
    QString evidence;    // object hash of the attached evidence file, empty = none
    QString evidenceName; // original file name of the evidence attachment
    QString recorded;    // UTC ISO timestamp stamped by the library on append
};

// P-306: one version-exclusion rule. Kind is extension|path|name; the pattern
// is stored normalized. A rule only affects snapshots taken after it exists.
struct Exclusion {
    QString kind;       // "extension" | "path" | "name"
    QString pattern;    // normalized pattern text
    QString created;    // UTC ISO timestamp stamped by the library on add
};

// P-309: one field-level metadata change (name/folder/tag/rating/note/
// source/author/license), kept for read-only review — V1 has no per-field
// revert and never invents old values. The change also joins the revision
// sequence as a metadata-only revision (same content hash), so field history
// and the content timeline stay aligned.
struct FieldChange {
    qint64 id = 0;
    QString assetId;
    qint64 revisionId = 0; // the metadata-only revision this change created
    QString field;         // human-readable field name, e.g. 标签
    QString oldValue;      // "" means the field was empty/absent
    QString newValue;      // "" means the field became empty/absent
    QString changed;       // UTC ISO timestamp
};

// One library, one writer. Original imports are never modified.
class Library {
    Q_DECLARE_TR_FUNCTIONS(Library)
public:
    explicit Library(const QString &root);
    ~Library();
    Library(const Library &) = delete;
    Library &operator=(const Library &) = delete;
    QVector<Asset> assets() const;
    QVector<Revision> revisions(const QString &assetId) const;
    Asset importFile(const QString &source);
    bool capture(const Asset &asset, const QString &note = QString());
    // P-303 manual version record: always returns the version id that now
    // represents the working copy — the newly recorded revision, or the current
    // (latest) revision id when the content is unchanged so no duplicate is
    // written. `created` reports which happened; a caller can surface the id to
    // give the user an explicit "content unchanged" answer instead of silence.
    qint64 captureRevision(const Asset &asset, const QString &note = QString(), bool *created = nullptr);
    // P-304 auto-record: like captureRevision, but first re-checks (size, mtime)
    // after reading the working copy — if the file changed during the read the
    // snapshot would be torn, so nothing is recorded and the caller retries on a
    // later scan. Returns true only when a new revision was recorded.
    bool captureIfStable(const Asset &asset, const QString &note = QString());
    void restore(const Asset &asset, qint64 revisionId);
    // P-310: in-app confirmed deletion. Writes a tombstone revision (history
    // and objects stay intact — the timeline still finds the asset) and removes
    // the working copy. Throws when the asset is unknown or already deleted.
    void deleteAsset(const QString &assetId);
    // P-310: working-copy health, distinct from deletion. "missing" = the file
    // is gone (externally removed — recoverable from history, never treated as
    // a user deletion); "unreachable" = the file or its directory exists but
    // cannot be read (unplugged volume / lost mount / permissions — no tombstone
    // is ever written for this); "" = healthy.
    enum class Health { Healthy, Missing, Unreachable };
    Health workingCopyHealth(const Asset &asset) const;
    void exportRevision(const QString &assetId, qint64 revisionId, const QString &destination) const;
    QString objectPath(const QString &hash) const;
    QByteArray readObject(const QString &hash) const;
    // Anti-ransomware: re-apply immutability to any object left writable (idempotent).
    int hardenObjects();
    // Anti-ransomware: freeze current working state of every asset as one anchor.
    qint64 snapshotAll(const QString &note);
    QVector<Snapshot> snapshots() const;
    // P-311: the manifest of a consistent snapshot — assetId, path-as-of-
    // snapshot, content hash and state per entry. Older snapshots (written
    // before the manifest carried path/hash) fall back to the asset's current
    // recorded location; the hash column stays '' for those entries.
    QVector<SnapshotEntry> snapshotEntries(qint64 snapshotId) const;
    // Anti-ransomware: one-click whole-tree rollback to a snapshot anchor. Each
    // asset is restored via the normal "restore forms a new version" path, so
    // later history is preserved rather than destroyed. Returns how many assets
    // actually changed. Assets already at the anchor content are skipped.
    // Entries excluded by a rule (P-306) are skipped — "excluded" is never a
    // deletion, so a rollback must not touch an excluded asset's working file.
    int rollbackAll(qint64 snapshotId);
    // P-306: version-exclusion rules. addExclusion throws on an empty pattern or
    // a duplicate; removeExclusion is a no-op when absent. Rules only affect
    // snapshots taken after they exist; history is never retroactively removed.
    void addExclusion(const QString &kind, const QString &pattern);
    void removeExclusion(const QString &kind, const QString &pattern);
    QVector<Exclusion> exclusions() const;
    // P-307: pin a revision (with a recorded reason) so a future retention
    // policy never cleans or overwrites its object; the pin persists across
    // restarts. Unpinning only clears the flag — it never deletes history.
    // Throws when the revision does not exist.
    void pinRevision(qint64 revisionId, const QString &reason);
    void unpinRevision(qint64 revisionId);
    // P-309: record a field change (no-op when old == new). The change is also
    // appended as a metadata-only revision (same content hash and size as the
    // current content), so field history aligns with the revision sequence.
    // Called by the metadata mutators; exposed for tests and review.
    void recordFieldChange(const QString &assetId, const QString &field,
                           const QString &oldValue, const QString &newValue);
    // P-309: read-only per-field history for one asset, newest first.
    QVector<FieldChange> fieldHistory(const QString &assetId) const;
    // Version-backend capability report (P-103): the self-built backend.
    BackendInfo backendInfo() const;
    QString root() const { return root_; }
    // Library identity (P-202): a random UUID stamped at creation and persisted
    // in the meta table; used to validate a relocated library before reopening.
    QString libraryId() const;
    // --- Tag maintenance (P-213) -------------------------------------------------
    // Tags are stored as (asset_id, tag) rows; a tag is trimmed, and empty tags
    // are ignored. All mutators return how many rows changed; the batch variants
    // are explicit add/remove (never a blind "replace all tags"). Metadata
    // history wiring happens under P-309.
    QStringList tags(const QString &assetId) const;
    QStringList assetsWithTag(const QString &tag) const;
    int addTag(const QString &assetId, const QString &tag);
    int removeTag(const QString &assetId, const QString &tag);
    int addTagToAssets(const QStringList &assetIds, const QString &tag);
    int removeTagFromAssets(const QStringList &assetIds, const QString &tag);
    // Batch previews (P-213): how many of the given assets would actually change.
    int countAssetsWithoutTag(const QStringList &assetIds, const QString &tag) const;
    int countAssetsWithTag(const QStringList &assetIds, const QString &tag) const;
    // --- Rating and note (P-214) -------------------------------------------------
    // Per-asset rating and free-form note, persisted in asset_meta so they
    // survive a reopen. Rating is -1 for "no rating", which is distinct from 0
    // (the explicit zero-star state); setRating(id, -1) clears it back to no
    // rating. setRating with any other out-of-range value throws. The note is
    // stored verbatim (never trimmed or inferred).
    int rating(const QString &assetId) const;
    void setRating(const QString &assetId, int rating);
    QString note(const QString &assetId) const;
    void setNote(const QString &assetId, const QString &note);
    // --- Source/author/license records (P-215) ----------------------------------
    // Append-only: every manual entry keeps its own library-stamped timestamp,
    // so old records stay viewable with when they were recorded. Fields may be
    // empty (unknown); the app never infers or asserts a license — a URL or an
    // attached evidence file is input the user provided, not validation.
    // Evidence files land in objects/ (immutable, deduped, inside the backup
    // boundary); storeEvidence returns their content hash.
    QVector<Provenance> provenance(const QString &assetId) const; // newest first
    void addProvenance(const QString &assetId, const Provenance &entry);
    QString storeEvidence(const QString &filePath);
    void exportEvidence(const QString &hash, const QString &destination) const;
    // --- Folder organization and asset identity (P-216) --------------------------
    // Rename keeps the asset id (history stays with the same asset) and renames
    // the working copy file to match. Move changes only the logical folder
    // (library-relative path, "" = root), keeping id and name. Both refuse a
    // name/folder clash with another asset: never a silent overwrite, never an
    // automatic merge of two distinct assets. Out-of-library folder targets
    // ("..", absolute paths) are rejected.
    void renameAsset(const QString &assetId, const QString &newName);
    void moveAsset(const QString &assetId, const QString &newFolder);
    // --- Filtering facts (P-217) ------------------------------------------------
    // Detected format and pixel size of the asset's last committed revision
    // (updated at import/capture/restore). format is empty and size 0 when the
    // content is not a decodable image — never fabricated.
    QString assetFormat(const QString &assetId) const;
    QSize assetSize(const QString &assetId) const;
    // Read a library's identity from its catalog WITHOUT taking the write lock.
    // Used when re-locating a missing library: verify this is the same library
    // (same id) before opening it at the new path. Throws when the directory is
    // not a valid library.
    static QString peekLibraryId(const QString &root);
private:
    QString root_;
    QString connection_;
    QSqlDatabase db_;
    std::unique_ptr<QLockFile> lock_;
    // P-304: every public method serializes on this, so the auto-record capture
    // can run on the background TaskQueue worker without racing the UI thread's
    // own library calls. Recursive because public methods call other public
    // methods (capture -> captureRevision -> revisions, restore -> append).
    mutable QRecursiveMutex mutex_;
    // Exception-safe construction helper (P-201): open the catalog, stamp the
    // schema/identity metadata, and validate the schema version.
    void openAndInitialize(const QString &catalog);
    void append(const Asset &asset, const QByteArray &bytes, const QString &note);
    // P-302 object storage: content is addressed by its SHA-256 and stored
    // immutably in objects/<hash>. Both paths follow the same order — stage in
    // objects/.tmp/, fully verify the staged bytes hash to the address, publish
    // by rename (or, if the object already appeared, verify and drop the
    // temp) — so a failed store never leaves a partial or duplicate object and
    // a dedup never rewrites existing content. `hash` is supplied by the
    // streaming callers so a large file is hashed once, not twice.
    QString storeObject(const QByteArray &bytes);
    QString storeObjectFromFile(const QString &sourcePath, const QString &hash);
    void publishObject(const QString &tempPath, const QString &hash);
    QString tempObjectPath() const;
    // Record one revision row and refresh the asset's P-217 facts; returns the
    // new revision id.
    qint64 recordRevision(const Asset &asset, const QString &hash, qint64 size, const QString &note);
};
