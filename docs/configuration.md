# Configuration

pentimento is configured at launch and in code; there are no settings files yet.

## Command-line options

| Flag | Effect |
| --- | --- |
| `--library <path>` | Open the asset library at `<path>`. |
| `--screenshot <path>` | Save a UI screenshot and exit (smoke check). |
| `--help`, `--version` | Qt standard options. |

Without `--library`, the app falls back to the most recently used library if it still exists on disk, and otherwise creates one under Qt's `AppLocalDataLocation` as `library/`.

## Library layout

A library directory holds:

```text
catalog.sqlite   index (assets + revisions)
objects/         SHA-256 content-addressed immutable objects
working/         working copies edited in external editors
library.lock     single-process write lock
```

## Sentinel thresholds (anti-ransomware)

The batch-modification sentinel compares each scan window against a baseline and raises an alert when any signal passes its threshold. Thresholds are defined in `SentinelConfig` (`src/sentinel.h`) and can be tuned in code before embedding the library layer:

| Signal | Default | Meaning |
| --- | --- | --- |
| Batch change | 25 files | Files changed in one scan window |
| Extension churn | 10 files | Files that gained a brand-new extension |
| High entropy | 10 files | Changed files above the entropy limit |
| Entropy limit | 7.2 bits/byte | Encrypted content is close to the 8.0 maximum |

The design leans toward false positives: normal small edits stay quiet, legitimate large changes may trigger and require user confirmation.

## Build configuration

- Build presets live in `CMakePresets.json` (`dev` configure/build/test presets); CMake is the only build entry, all targets use ISO C++17 with compiler extensions disabled.
- If Qt is not on the default search path, pass `-DCMAKE_PREFIX_PATH=/path/to/Qt/6.x/platform`.
- Language preference is stored in QSettings (`ui/language`, default `zh_CN`) and applied at startup; there is no in-app switcher yet (`setLanguagePreference` has no UI caller), so switching means editing the setting outside the app.
- Runtime snapshot periods (off/15/30/60 minutes) are UI data, not persisted settings; they are attached to combo-box rows in `src/mainwindow.cpp:346` and applied to `SnapshotSchedule` in `src/mainwindow.cpp:1376`.
- Sentinel thresholds are code defaults in `SentinelConfig` (`src/sentinel.h:12`); see [Architecture](./architecture.md) for the responsibility layers and [Glossary](./glossary.md) for terminology.
