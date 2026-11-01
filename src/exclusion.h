#pragma once
#include <QDir>
#include <QString>

// P-306: one version-exclusion rule (path / pattern / extension). A rule only
// affects snapshots taken after it exists — history is kept, and "excluded" is
// never "deleted" (a restore must not touch an excluded object's current file).
// Kinds:
//  - extension: the file name ends with "."+pattern (case-insensitive; a
//               leading dot in the pattern is stripped).
//  - path:      a glob against the logical path folder/name, plus a plain
//               folder-prefix match so a folder pattern covers its subtree.
//  - name:      a glob against the file name.
// Returns a human reason describing the hit, or an empty string when no rule
// matches. Pure logic, UI-independent, so the semantics are unit-testable.
inline QString exclusionReason(const QString &kind, const QString &pattern,
                               const QString &folder, const QString &name) {
    const QString logicalPath = folder.isEmpty() ? name : folder + QLatin1Char('/') + name;
    if (kind == QLatin1String("extension")) {
        const QString ext = pattern.startsWith(QLatin1Char('.')) ? pattern.mid(1) : pattern;
        if (name.endsWith(QLatin1Char('.') + ext, Qt::CaseInsensitive))
            return QStringLiteral("扩展名 .%1").arg(ext);
    } else if (kind == QLatin1String("path")) {
        if (QDir::match(pattern, logicalPath))
            return QStringLiteral("路径 %1").arg(pattern);
        const QString prefix = pattern.endsWith(QLatin1Char('/')) ? pattern : pattern + QLatin1Char('/');
        if (logicalPath.startsWith(prefix, Qt::CaseInsensitive))
            return QStringLiteral("路径 %1（含子目录）").arg(pattern);
    } else if (kind == QLatin1String("name")) {
        if (QDir::match(pattern, name))
            return QStringLiteral("名称 %1").arg(pattern);
    }
    return {};
}

// Normalize a rule before storing: trim whitespace, strip a leading dot from an
// extension pattern, and drop a trailing slash from a path pattern (matching
// already treats a bare folder as covering its subtree).
inline QString exclusionNormalize(const QString &kind, const QString &pattern) {
    QString p = pattern.trimmed();
    if (kind == QLatin1String("extension")) {
        if (p.startsWith(QLatin1Char('.'))) p.remove(0, 1);
        p.remove(QLatin1Char(' '));
    } else if (kind == QLatin1String("path")) {
        while (p.endsWith(QLatin1Char('/'))) p.chop(1);
    }
    return p;
}
