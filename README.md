[English](README.md) | [中文](README.zh.md)

<h1 align="center">pentimento · 画迹</h1>

<p align="center"><a href="https://cuihairu.github.io/pentimento/"><img src="docs/public/badges/docs.svg" alt="docs" /></a> <img src="docs/public/badges/cpp17.svg" alt="language: C++17" /> <img src="docs/public/badges/qt6.svg" alt="framework: Qt 6" /> <img src="docs/public/badges/cmake.svg" alt="build: CMake 3.24+" /> <img src="docs/public/badges/license.svg" alt="license: Apache-2.0" /> <img src="docs/public/badges/platform.svg" alt="platform: Linux | Windows | macOS" /> <img src="docs/public/badges/status.svg" alt="status: prototype stage" /></p>

**A local desktop asset manager for game art assets, centered on version-history browsing and restore.** It takes inspiration from Eagle's asset-organizing workflow and macOS Time Machine's version rollback experience, with the code and UI implemented from scratch.

**Tech stack: C++17, Qt 6 Widgets, Qt SQL/SQLite, managed by CMake.** No WebView.

## What works today

- Create/open local asset libraries and batch-import files (copy-in import; original files are never modified).
- Asset list, name search, image previews, and a version timeline with thumbnails.
- Manual version notes; while the app runs, working copies are checked every two seconds, and a change is recorded once size/mtime stays stable for two consecutive checks — identical content does not produce a duplicate version.
- Browse and export historical versions; restoring adds a new version, saving any not-yet-recorded current content first, so existing history is preserved.
- SHA-256 content addressing: identical file content reuses objects; SQLite keeps the index, QSaveFile gives atomic writes, and a library-level lock restricts access to one process.
- Restore/export verifies historical object hashes; if a working copy is accidentally deleted it can be recovered from history.

## Build & run

Requires CMake 3.24+, Ninja, a C++17-capable compiler, and Qt 6.4+ (Core, Gui, Widgets, Sql, Test); the Qt SQLite driver is needed at runtime. Verified with a local Qt 6.10.2/Linux build; macOS/Windows builds are not yet verified on real machines.

```sh
cmake --preset dev
cmake --build --preset dev
ctest --preset dev
./build/dev/pentimento --library /path/to/my-library
```

On macOS the bundle entry point is `build/dev/pentimento.app/Contents/MacOS/pentimento`. If your Qt install is not on the default search path, add `-DCMAKE_PREFIX_PATH=/path/to/Qt/6.x/platform` to the configure command. Qt Creator can also open `CMakeLists.txt` directly.

Without `--library`, the library lives under Qt's AppLocalDataLocation as `library/`. A library contains `catalog.sqlite`, `objects/`, and `working/`; open a file in the library with "Edit working copy", and after editing it is recorded automatically or via "Record version". The original file imported into the library is never updated by edits.

## Security defense (anti-ransomware)

When a local asset library is encrypted or tampered with by ransomware, the product must **prevent, detect, and quickly recover** (new core requirement added 2026-10-31). Only the library's own defenses are built — no antivirus engine, no virus database.

- **Layer 1: immutable version store** (implemented) — history objects are content-addressed and append-only, then set read-only after write (Linux `chattr +i` when permitted, otherwise write bits are cleared; Windows/macOS read-only attributes are abstracted to the platform layer, with real-machine verification still TODO). The version store is separate from the workspace, and restore is one-way store→workspace; encrypting the workspace leaves history objects readable and restorable.
- **Layer 2: batch-modification sentinel** (implemented) — watches the workspace for anomalous batch writes; any of three signals (many files changed / extension churn .png→.enc / abnormally rising content entropy) tripping its threshold raises an alert and automatically takes an emergency snapshot anchor. Normal small edits stay quiet (threshold-tested).
- **Layer 3: whole-tree rollback** (implemented) — "Restore to anchor" restores the workspace to the pre-infection anchor in one action, using the existing "restore forms a new version" semantics; later history (including encrypted versions) is not destroyed.

## First-release boundaries

This is a runnable early prototype — not a full Eagle replacement, and not a system-level backup tool.

- Automatic recording only works while the app is running; file-stability detection is heuristic and does not guarantee capture of every editor save, and content not written to disk in the editor cannot be recorded. Close external editors before restoring so they don't overwrite the restore result.
- Image previews depend on the local Qt image plugins. PSDs, models, and similar files keep their history as raw bytes, but layers are not parsed and models are not rendered. Single-file limit is 256 MiB; I/O and thumbnails currently run on the UI thread — small libraries are the priority for trial use.
- Currently single-asset version history; whole-library point-in-time restore, metadata history, side-by-side/slider comparison, tags, batch folder import, and AI recognition are not yet available.
- This release uses managed copy-in import; rename/move tracking, external directory indexing, library migration, history cleanup, and remote backup are not implemented. Working-file paths are currently absolute, so a library cannot be moved wholesale and continue editing.
- Objects and the index are not in a single cross-file transaction; if the process crashes between restoring to disk and committing the database, the current working copy must be recorded once after reopening. Orphaned objects are not cleaned up for now, to avoid deleting history by mistake.
- Same-disk history is not a substitute for off-disk backup; to back up, close the app first, then copy the entire library.

Product conventions: [PRODUCT.md](PRODUCT.md). Storage and restore boundaries: [docs/zh/architecture.md](docs/zh/architecture.md). Upcoming tasks: [todo.en.md](todo.en.md).

## License

Apache-2.0. Qt and other dependencies retain their own licenses; distributing the app must comply with the licensing terms of the Qt edition actually used.
