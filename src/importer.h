#pragma once

#include "library.h"
#include "taskqueue.h"

#include <QString>
#include <QStringList>
#include <QVector>

// Directory import pipeline (P-205 recursive scan + ignore rules, P-206 cancel
// + result list). Scanning and import are separate: the scan produces a
// previewable plan (candidates + per-item exclude reason + permission errors),
// the user confirms, then the plan imports in the background with cancellation
// at file boundaries and a per-item result list.
namespace importer {

enum class IgnoreType { Extension, Name, Glob };

struct IgnoreRule {
    IgnoreType type = IgnoreType::Name;
    QString pattern; // "png" for Extension, ".git" for Name, "*.bak" for Glob
    // Human-readable rule, e.g. ".png", "__pycache__", "*.bak".
    QString display() const;
};

// Parse a user ignore line: ".png"/"png" (extension), "*.bak" (glob),
// ".git"/"__pycache__"/"node_modules" (name/subtree). Blank lines are ignored.
IgnoreRule parseIgnoreLine(const QString &line);

struct Candidate {
    QString sourcePath;  // absolute path on disk
    QString relative;    // path relative to the scan root (display, stable)
    bool isDirectory = false;
    QString excludeReason; // empty = included
};

struct ScanResult {
    QVector<Candidate> candidates; // files to import (included), in stable order
    QStringList permissionErrors;  // unreadable directories/files, not swallowed
    int excluded = 0;              // files matched by an ignore rule
    int total = 0;                 // files seen (included + excluded)
};

// Recursively scan `root` for importable files. Directory symlinks are never
// followed (loop protection); hidden files/dirs are subject to the rules and
// otherwise included. Unreadable directories are reported, not silently skipped.
ScanResult scanDirectory(const QString &root, const QVector<IgnoreRule> &rules = {});

// Run a confirmed import plan. Cancellation takes effect at file boundaries;
// already-imported items and their shared objects are kept (P-206). Errors are
// recorded per item so the user can retry only the unfinished ones.
void importPlan(Library &library, const QVector<QString> &sourcePaths,
                TaskQueue::Progress &progress);

} // namespace importer
