#pragma once

#include <QHash>
#include <QString>

// Anti-ransomware Layer 2: batch-modification sentinel.
//
// Watches the workspace tree for anomalous batch writes that match ransomware
// behaviour: many files changed in one window, mass extension churn, or a burst
// of files whose content entropy is abnormally high (encrypted content is
// ~8 bits/byte). Thresholds are configurable so normal small edits stay quiet.
class SentinelConfig {
public:
    int batchChangeThreshold = 25;      // changed files in one scan window
    int extensionChurnThreshold = 10;   // files that gained a brand-new extension
    int highEntropyThreshold = 10;      // changed files above the entropy limit
    double entropyLimit = 7.2;          // bits/byte; text/images are much lower
};

struct SentinelAlert {
    bool triggered = false;
    QString reason;                     // joined trigger descriptions (empty = quiet)
    int changedFiles = 0;
    int novelExtensionFiles = 0;
    int highEntropyFiles = 0;
    double maxEntropy = 0.0;
};

class Sentinel {
public:
    explicit Sentinel(const QString &workspaceRoot, const SentinelConfig &config = {});
    // Capture the current tree as the clean baseline (never alerts).
    void resetBaseline();
    // Scan the tree, compare to the baseline, and return an alert when a signal
    // fires. The baseline advances to the current scan, so each window is clean.
    SentinelAlert check();
    // Shannon entropy of a byte sample, bits/byte (max 8.0).
    static double contentEntropy(const QByteArray &sample);
private:
    struct FileState {
        QString suffix;
        qint64 size = 0;
        qint64 mtimeMs = 0;
    };
    QHash<QString, FileState> scan() const;
    QString root_;
    SentinelConfig config_;
    QHash<QString, FileState> baseline_;
};
