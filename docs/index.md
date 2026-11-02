---
layout: home

hero:
  name: pentimento
  text: Version history for your game art assets
  tagline: A local desktop asset manager centered on version-history browsing and restore. C++17, Qt 6 Widgets, SQLite — no WebView.
  actions:
    - theme: brand
      text: Quickstart
      link: /quickstart
    - theme: alt
      text: Architecture
      link: /architecture

features:
  - icon: 🗂️
    title: Import without touching originals
    details: Create local asset libraries and batch-import files as managed copies; the original files are never modified.
  - icon: 🕰️
    title: Automatic version recording
    details: While the app runs, working copies are checked every two seconds; once size and mtime stay stable for two consecutive checks, a change is recorded. Identical content never produces a duplicate version.
  - icon: ⏪
    title: Restore adds, never destroys
    details: Browse and export historical versions. Restoring saves any not-yet-recorded current content first, then adds a new version — existing history is preserved.
  - icon: 🔒
    title: Anti-ransomware defenses
    details: An append-only, read-once object store, a batch-modification sentinel that snapshots before infection spreads, and one-click whole-tree rollback to the last clean anchor.
  - icon: 🧬
    title: SHA-256 content addressing
    details: Identical file content reuses objects. SQLite keeps the index, QSaveFile gives atomic writes, and a library-level lock restricts access to one process.
  - icon: 🧪
    title: Verified by Qt Test
    details: Storage and restore behavior is covered by tests through CTest; CMake 3.24 presets are the only build entry.
