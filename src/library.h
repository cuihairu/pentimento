#pragma once

#include "backend.h"

#include <QCoreApplication>
#include <QSqlDatabase>
#include <QString>
#include <QVector>
#include <memory>
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
};

// One whole-tree emergency anchor: each asset pinned to the revision that
// represented it when the snapshot was taken.
struct Snapshot {
    qint64 id = 0;
    QString created;
    QString note;
    int assets = 0;
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
    void restore(const Asset &asset, qint64 revisionId);
    void exportRevision(const QString &assetId, qint64 revisionId, const QString &destination) const;
    QString objectPath(const QString &hash) const;
    QByteArray readObject(const QString &hash) const;
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
