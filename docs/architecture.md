# Storage and version design

## Structure

`pentimento_core` is a Qt Core/Sql library with no Widgets dependency; the desktop layer calls into the Library, and Qt Test covers storage and restore behavior. CMake is the only build entry; all targets use ISO C++17 with compiler extensions disabled.

```text
library/
  catalog.sqlite   assets + revisions
  objects/         SHA-256 named immutable content objects
  working/<uuid>/  working copies edited in external editors
  library.lock     single-process write lock
```

Copy import: read the source → write a working copy → insert the asset and its first revision index. The original file is untouched. Identical content can share objects, but different imports remain independent assets; identities are never merged automatically.

Recording a version: read the working file → compare content hashes against the latest version → write a new object → insert the revision. Identical content does not produce a duplicate version. History is ordered by the database's monotonic id; timestamps are stored as UTC milliseconds and displayed in the local timezone.

Restore: query versions by asset ownership → verify the target hash → save any unrecorded current working content → append a restore revision in a transaction → atomically replace the working file → commit the database. Ordinary exceptions roll back the index and attempt to restore the working file; a process crash is not an atomic cross-SQLite/filesystem transaction and needs a later journal-recovery mechanism. There is no history deletion or GC: accidentally orphaned objects are allowed to occupy space, and aggressive cleanup is not permitted.

Export: likewise verifies revision ownership and the object hash, and refuses to target a path inside the library. Overwriting external files is confirmed by the save dialog; the core interface only performs atomic writes.

## Security defenses (anti-ransomware)

A core requirement added 2026-10-31: when the local asset library is encrypted or tampered with by ransomware, the product must prevent it, detect it, and recover quickly. Only the library's own defenses are built — no antivirus engine, no virus database.

### Layer 1: immutable version store (implemented)

- Objects are append-only (content-addressed, reusable) and set **read-only** once written: `src/platform_immutable.{h,cpp}` abstracts the platform differences — Linux prefers `chattr +i` (the `FS_IMMUTABLE_FL` ioctl, requires privileges), falling back to clearing the write permission bits when refused; Windows uses `FILE_ATTRIBUTE_READONLY`; macOS uses `UF_IMMUTABLE`. Real-machine verification on Windows/macOS is TODO. `Library::append` hardens objects right after writing them, and `Library::hardenObjects` hardens existing writable objects (idempotent).
- The version store (`objects/`, `catalog.sqlite`) is separate from the workspace (`working/`); the restore path is one-way store→workspace — `restore` only ever writes `working/` and never rewrites `objects/`.
- Known boundary: read-only is **best effort** (a same-user process can chmod the write bits back; only `chattr +i` is a strong guarantee and it needs privileges) and makes no claim against privileged attackers. Encrypting the workspace does not affect `objects/`; reading an object still verifies its hash.

### Layer 2: batch-modification sentinel (implemented)

- `src/sentinel.{h,cpp}` scans `working/` incrementally; any of three signals (batch writes / mass extension churn / abnormal content entropy, Shannon entropy > 7.2) reaching its threshold raises a `SentinelAlert`. Thresholds are configurable (production defaults 25/10/10/7.2); normal small edits stay quiet (threshold-tested).
- On trigger, an emergency snapshot anchor is taken automatically: `snapshotAll` writes `snapshots`/`snapshot_assets` in a transaction, anchoring each asset to its **most recent clean revision (pre-infection content)**. If the current working content has already drifted (encrypted/tampered), it is additionally stored as a `紧急快照锚点` revision for forensics but is **not used as the restore anchor**; a renamed/missing working file likewise anchors to the most recent readable (clean) revision.
- Known boundary: the sentinel is **after-the-fact detection** (not real-time interception); it leans toward false alarms over misses, legitimate large changes may trigger and require user confirmation, and the extension signal counts only "extensions not present in the baseline" so legitimate new formats are not mistaken for ransomware.

### Layer 3: whole-tree rollback (implemented)

- `Library::rollbackAll(snapshotId)` restores each asset to its anchored revision through the existing `restore` path (save unrecorded current content → append a "restored historical version" → atomically replace the working file) and **does not destroy later history** (encrypted versions are retained). Assets already at their anchored content are skipped.
- UI entry: `MainWindow::rollbackToSnapshot` (toolbar "Restore to anchor"), listing `Library::snapshots` for selection; after rollback, assets are reloaded and the sentinel baseline is re-armed.
- Known boundary: rollback can only target recorded historical revisions (unrecorded edits are unrecoverable); close external editors before rolling back.

## Automatic recording

A UI timer checks file size and mtime; a read happens once they have stayed the same twice in a row (2-second intervals), and an already-processed signature is not fully hashed again. Writes that do not change the on-disk signature may be missed, and a write that pauses mid-write beyond the window may be recorded. Editor awareness and file watchers are future work. The stable-read double check (`src/library.cpp`) and a background task queue (`src/taskqueue.{h,cpp}`, used for capture/integrity/probe workers) have landed since this section was first written.

## Versioning and backup

Per-asset content history, whole-library snapshot anchors (`snapshotAll`/`snapshots`), batch point-in-time restore (`rollbackAll`, the "Restore to anchor" path), scheduled snapshots (off by default), snapshot exclusion rules, per-field metadata history (P-309, read-only review), and one-click backup-package export (`exportBackupPackage`) have landed. Deletion tombstones and retention policies have not. The object store must be backed up together with SQLite; only a cold backup taken while the app is closed is supported. A library cannot be moved to a new path directly (working paths are currently absolute).
