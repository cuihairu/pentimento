#pragma once

#include <QString>
#include <QVector>

// Most-recently-used libraries (P-202). Persisted under QSettings
// ("pentimento","pentimento") so the set survives restarts. The identity (id)
// is what lets a relocated library be re-located: the recorded path may go
// stale, but the id lets the UI validate a user-picked replacement path.
namespace recent {

struct Entry {
    QString id;
    QString path;
};

// Ordered most-recent first, deduplicated by id, capped at a small size.
QVector<Entry> libraries();
// Remember an opened library; moves it to the front, deduplicates by id.
void remember(const QString &id, const QString &path);
// Forget a library (e.g. after the user removes it from the list).
void forget(const QString &id);
// Clear the whole list (used by tests to reset state).
void clear();
// Test hook: override the QSettings backend with an explicit storage file.
void setStoragePath(const QString &path);

} // namespace recent
