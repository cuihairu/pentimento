#include "sentinel.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QSet>
#include <QStringList>
#include <cmath>

namespace {
constexpr int kSampleBytes = 64 * 1024;
QString suffixOf(const QString &name) {
    const int dot = name.lastIndexOf('.');
    if (dot < 0 || dot == name.size() - 1) return QString();
    return name.mid(dot + 1).toLower();
}
QByteArray samplePrefix(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return QByteArray();
    return file.read(kSampleBytes);
}
}

Sentinel::Sentinel(const QString &workspaceRoot, const SentinelConfig &config)
    : root_(workspaceRoot), config_(config) {}

void Sentinel::resetBaseline() { baseline_ = scan(); }

QHash<QString, Sentinel::FileState> Sentinel::scan() const {
    QHash<QString, FileState> result;
    QDirIterator it(root_, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        it.next();
        const QFileInfo info = it.fileInfo();
        FileState state;
        state.suffix = suffixOf(info.fileName());
        state.size = info.size();
        state.mtimeMs = info.lastModified().toMSecsSinceEpoch();
        result.insert(QDir(root_).relativeFilePath(it.filePath()), state);
    }
    return result;
}

double Sentinel::contentEntropy(const QByteArray &sample) {
    const int n = qMin(sample.size(), kSampleBytes);
    if (n <= 0) return 0.0;
    int counts[256] = {0};
    for (int i = 0; i < n; ++i) ++counts[static_cast<unsigned char>(sample.at(i))];
    double entropy = 0.0;
    const double size = n;
    for (const int count : counts) {
        if (count == 0) continue;
        const double p = count / size;
        entropy -= p * std::log2(p);
    }
    return entropy;
}

SentinelAlert Sentinel::check() {
    const auto current = scan();
    QSet<QString> knownSuffixes;
    for (auto it = baseline_.constBegin(); it != baseline_.constEnd(); ++it)
        knownSuffixes.insert(it->suffix);

    SentinelAlert alert;
    QStringList fired;
    int changed = 0, novel = 0, highEntropy = 0;
    double maxEntropy = 0.0;
    for (auto it = current.constBegin(); it != current.constEnd(); ++it) {
        const auto old = baseline_.constFind(it.key());
        const bool isNew = old == baseline_.constEnd();
        const bool changedContent =
            isNew || old->size != it->size || old->mtimeMs != it->mtimeMs;
        if (changedContent) ++changed;
        // A path that appears with a suffix never seen in the baseline is the
        // signature of a mass rename to a locked extension (.png -> .enc).
        if (isNew && !knownSuffixes.contains(it->suffix)) ++novel;
        if (changedContent) {
            const double entropy =
                contentEntropy(samplePrefix(root_ + '/' + it.key()));
            if (entropy > maxEntropy) maxEntropy = entropy;
            if (entropy > config_.entropyLimit) ++highEntropy;
        }
    }
    alert.changedFiles = changed;
    alert.novelExtensionFiles = novel;
    alert.highEntropyFiles = highEntropy;
    alert.maxEntropy = maxEntropy;
    if (changed >= config_.batchChangeThreshold)
        fired << QStringLiteral("批量写入(%1 文件)").arg(changed);
    if (novel >= config_.extensionChurnThreshold)
        fired << QStringLiteral("扩展名批量变更(%1 文件)").arg(novel);
    if (highEntropy >= config_.highEntropyThreshold)
        fired << QStringLiteral("内容熵值异常(%1 文件)").arg(highEntropy);
    alert.reason = fired.join(QStringLiteral("；"));
    alert.triggered = !fired.isEmpty();
    baseline_ = current;
    return alert;
}
