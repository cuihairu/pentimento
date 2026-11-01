#include "recent.h"

#include <QSettings>

namespace {
constexpr int kMaxEntries = 10;
// Test seam: when non-empty, QSettings uses this file instead of the user scope.
QString g_storagePath;

QSettings settings() {
    if (!g_storagePath.isEmpty()) return QSettings(g_storagePath, QSettings::IniFormat);
    return QSettings(QStringLiteral("pentimento"), QStringLiteral("pentimento"));
}

QString encode(const recent::Entry &e) { return e.id + QLatin1Char('|') + e.path; }
recent::Entry decode(const QString &line) {
    const int split = line.indexOf(QLatin1Char('|'));
    if (split <= 0) return {};
    return {line.left(split), line.mid(split + 1)};
}
} // namespace

namespace recent {

QVector<Entry> libraries() {
    QVector<Entry> result;
    for (const QString &line : settings().value(QStringLiteral("library/recent")).toStringList()) {
        const Entry entry = decode(line);
        if (entry.id.isEmpty()) continue;
        bool seen = false;
        for (const auto &existing : result)
            if (existing.id == entry.id) { seen = true; break; }
        if (!seen) result.push_back(entry);
    }
    return result;
}

void remember(const QString &id, const QString &path) {
    if (id.isEmpty()) return;
    forget(id);
    QVector<Entry> list = libraries();
    Entry entry{id, path};
    QVector<Entry> merged{entry};
    for (const auto &existing : list) {
        if (merged.size() >= kMaxEntries) break;
        if (existing.id != id) merged.push_back(existing);
    }
    QStringList lines;
    for (const auto &e : merged) lines << encode(e);
    settings().setValue(QStringLiteral("library/recent"), lines);
}

void forget(const QString &id) {
    QStringList kept;
    for (const QString &line : settings().value(QStringLiteral("library/recent")).toStringList()) {
        const Entry entry = decode(line);
        if (entry.id != id) kept << line;
    }
    settings().setValue(QStringLiteral("library/recent"), kept);
}

void clear() { settings().remove(QStringLiteral("library/recent")); }

void setStoragePath(const QString &path) { g_storagePath = path; }

} // namespace recent
