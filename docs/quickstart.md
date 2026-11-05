# Quickstart

## Prerequisites

- CMake 3.24+
- Ninja
- A C++17-capable compiler
- Qt 6.4+ with Core, Gui, Widgets, Sql and Test (the Qt SQLite driver is needed at runtime)

Verified with a local Qt 6.10.2 / Linux build; macOS and Windows builds are not yet verified on real machines.

## Build and run

```sh
cmake --preset dev
cmake --build --preset dev
ctest --preset dev
./build/dev/pentimento --library /path/to/my-library
```

On macOS the bundle entry point is `build/dev/pentimento.app/Contents/MacOS/pentimento`. If your Qt install is not on the default search path, add `-DCMAKE_PREFIX_PATH=/path/to/Qt/6.x/platform` to the configure command. Qt Creator can also open `CMakeLists.txt` directly.

## First steps in the app

1. The `--library` flag points the app at a library directory. Without it, the app falls back to the most recently used library, and otherwise creates one under Qt's `AppLocalDataLocation` as `library/`.
2. A library contains `catalog.sqlite`, `objects/`, and `working/`.
3. Batch-import files — they are copied into the library; the originals are never touched.
4. Open a file with **Edit working copy**, edit it in your external editor, and the change is recorded automatically (or via **Record version**).

## What works today

- Asset list, name search, image previews, and a version timeline with thumbnails.
- Manual version notes; duplicate content never produces a duplicate version.
- Side-by-side or sliding-divider version comparison; tags; per-field metadata history (read-only review).
- Browse and export historical versions; restore preserves all existing history.
- Restore/export verify historical object hashes; an accidentally deleted working copy can be recovered from history.

## First-release boundaries

This is a runnable early prototype, not a full asset-manager replacement and not a system-level backup tool. Automatic recording only works while the app runs; PSDs, models, and similar files keep their history as raw bytes without layer parsing; AI recognition is not yet available, and batch folder import has a core scan pipeline but no UI entry yet. Same-disk history is not a substitute for off-disk backup — to back up, close the app first, then copy the entire library. See [Configuration](/configuration) for the boundaries around restore.
