#include "importer.h"

#include "storage.h"

#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>

namespace importer {

QString IgnoreRule::display() const {
    switch (type) {
        case IgnoreType::Extension: return QStringLiteral("*.%1").arg(pattern);
        case IgnoreType::Name: return pattern;
        case IgnoreType::Glob: return pattern;
    }
    return pattern;
}

IgnoreRule parseIgnoreLine(const QString &line) {
    const QString trimmed = line.trimmed();
    if (trimmed.contains(QLatin1Char('*')) || trimmed.contains(QLatin1Char('?')))
        return {IgnoreType::Glob, trimmed};
    if (trimmed.startsWith(QLatin1Char('.')) && trimmed.size() > 1
        && !trimmed.contains(QLatin1Char('/')))
        return {IgnoreType::Extension, trimmed.mid(1)};
    return {IgnoreType::Name, trimmed};
}

namespace {
bool ruleMatches(const IgnoreRule &rule, const QString &relative) {
    switch (rule.type) {
        case IgnoreType::Extension: return relative.endsWith(
            QStringLiteral(".%1").arg(rule.pattern), Qt::CaseInsensitive);
        case IgnoreType::Name: {
            // Match any path segment: excludes that whole subtree.
            const QStringList segments = relative.split(QLatin1Char('/'));
            return segments.contains(rule.pattern, Qt::CaseInsensitive);
        }
        case IgnoreType::Glob:
            return QDir::match(rule.pattern, relative)
                || QDir::match(rule.pattern, QFileInfo(relative).fileName());
    }
    return false;
}

QString excludeReason(const QString &relative, const QVector<IgnoreRule> &rules) {
    for (const auto &rule : rules)
        if (ruleMatches(rule, relative))
            return QObject::tr("忽略规则 %1").arg(rule.display());
    return QString();
}
} // namespace

ScanResult scanDirectory(const QString &root, const QVector<IgnoreRule> &rules) {
    ScanResult result;
    const QFileInfo rootInfo(root);
    if (!rootInfo.exists() || !rootInfo.isDir()) {
        result.permissionErrors << QObject::tr("根目录不可读：%1").arg(root);
        return result;
    }
    // QDirIterator never follows directory symlinks by default when
    // NoSymLinks is requested: loop protection (P-205).
    QDirIterator it(root, QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot
        | QDir::NoSymLinks | QDir::Hidden, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString path = it.next();
        const QFileInfo info = it.fileInfo();
        const QString relative = QDir(root).relativeFilePath(path);
        // Never report or traverse above the source root.
        if (relative.startsWith(QLatin1String("../"))) continue;
        if (!info.isReadable()) {
            result.permissionErrors << QObject::tr("不可读（跳过，可重试）：%1").arg(path);
            continue;
        }
        if (info.isDir()) continue; // directories themselves are not imported
        ++result.total;
        const QString reason = excludeReason(relative, rules);
        if (!reason.isEmpty()) {
            ++result.excluded;
            result.candidates.push_back({path, relative, false, reason});
        } else {
            result.candidates.push_back({path, relative, false, QString()});
        }
    }
    return result;
}

void importPlan(Library &library, const QVector<QString> &sourcePaths,
                TaskQueue::Progress &progress) {
    for (int i = 0; i < sourcePaths.size(); ++i) {
        if (progress.cancelled()) break; // file-boundary cancel; items stay
        const QString &source = sourcePaths.at(i);
        progress.setCurrent(source);
        try {
            const Asset asset = library.importFile(source);
            progress.addItem({source, true, asset.id});
        } catch (const std::exception &error) {
            progress.addItem({source, false, QString::fromUtf8(error.what())});
        }
        progress.setPercent(static_cast<int>(
            (static_cast<qint64>(i + 1) * 100) / qMax<qsizetype>(1, sourcePaths.size())));
    }
}

RescanPreview rescanPreview(Library &library, const QString &root,
                            const QVector<IgnoreRule> &rules) {
    const ScanResult scan = scanDirectory(root, rules);
    RescanPreview preview;
    preview.unreadable = scan.permissionErrors;
    for (const auto &candidate : scan.candidates) {
        RescanCandidate rc;
        rc.candidate = candidate;
        // Streaming hash: a large source file (beyond the 256 MiB single-file
        // import cap) is never loaded whole, so a mounted source cannot OOM the
        // worker thread during a rescan preview. A mid-read failure (mount drop,
        // file deleted between the scan and this hash) must NOT escape the whole
        // function: the candidate is kept as retryable (with an error), and the
        // rest of the preview still completes. Only the "already imported"
        // comparison needs the successful hash.
        QString hash;
        if (!hashFile(candidate.sourcePath, &hash, nullptr)) {
            rc.error = QObject::tr("读取失败（可重试）：%1").arg(candidate.sourcePath);
            preview.candidates.push_back(rc);
            continue;
        }
        rc.hash = hash;
        rc.alreadyImported = library.hasContent(rc.hash);
        preview.candidates.push_back(rc);
    }
    return preview;
}

int RescanPreview::fresh() const {
    int n = 0;
    for (const auto &c : candidates)
        if (c.error.isEmpty() && !c.alreadyImported && c.candidate.excludeReason.isEmpty())
            ++n;
    return n;
}

int RescanPreview::alreadyImported() const {
    int n = 0;
    for (const auto &c : candidates)
        if (c.alreadyImported) ++n;
    return n;
}

} // namespace importer
