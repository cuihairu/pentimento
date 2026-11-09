// Mounted-source and external-index flows: the rescan action, the
// mounted-source state dialog (P-513) and external directory indexing
// (P-204) (LAY-003 split).
#include "mainwindow.h"
#include "compare_canvas.h"
#include "importer.h"
#include "preview.h"
#include "preview_canvas.h"
#include "sentinel.h"
#include "source_connection.h"
#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDesktopServices>
#include <QDialog>
#include <QFileDialog>
#include <QVBoxLayout>
#include <QFileInfo>
#include <QFrame>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QListView>
#include <QMenu>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QSplitter>
#include <QSpinBox>
#include <QStackedWidget>
#include <QStatusBar>
#include <QStyle>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QUrl>


void MainWindow::scanChanges() {
    // P-305: drain a finished scheduled snapshot first, so a completed run
    // frees the in-progress gate before the schedule check looks at it.
    drainSnapshotResult();
    checkSnapshotSchedule();
    // P-304: drain the background captures finished since the last tick (the
    // worker wrote each result under the mutex). This runs on the UI thread.
    QHash<QString, QPair<bool, QString>> results;
    {
        QMutexLocker lock(&captureResultsMutex_);
        results = std::move(captureResults_);
    }
    for (auto it = results.begin(); it != results.end(); ++it) {
        const QString &id = it.key();
        pendingCapture_.remove(id);
        const bool changed = it.value().first;
        const QString error = it.value().second;
        if (!error.isEmpty()) {
            statusBar()->showMessage(tr("自动记录失败：%1").arg(error));
        } else if (changed && selected().id == id) {
            reloadHistory();
        }
    }
    for (const auto &asset : assets_) {
        const QFileInfo info(asset.path);
        if (!info.exists()) { observations_.remove(asset.id); continue; }
        const auto signature = qMakePair(info.size(), info.lastModified().toMSecsSinceEpoch());
        if (!observations_.contains(asset.id) || observations_.value(asset.id) != signature) {
            observations_[asset.id] = signature;
            continue;
        }
        if (recorded_.contains(asset.id) && recorded_.value(asset.id) == signature) continue;
        if (pendingCapture_.contains(asset.id)) continue; // one capture in flight per asset
        // Stable change: capture off the UI thread. The working copy is re-read
        // and re-verified inside captureIfStable, so a mid-write snapshot is
        // never recorded; the worker reports the outcome back for the next tick.
        recorded_[asset.id] = signature; // capture once per signature
        pendingCapture_.insert(asset.id);
        captureQueue_->submit(QStringLiteral("自动记录"),
            [this, asset, signature](TaskQueue::Progress &) {
                bool changed = false;
                QString error;
                try {
                    changed = library_.captureIfStable(asset, QStringLiteral("自动记录"));
                } catch (const std::exception &e) {
                    error = QString::fromUtf8(e.what());
                }
                QMutexLocker lock(&captureResultsMutex_);
                captureResults_.insert(asset.id, {changed, error});
            });
    }
}

// P-513: mounted-source reconnection state machine, made visible. The user
// enters (or browses) a mounted SMB/NFS path and watches the live state
// 连接中 / 可达 / 不可达 / 失败; a retry re-probes off the UI thread (a dead
// mount can block a stat, so the dialog itself never hangs). Retrying never
// touches the source content — it only re-probes reachability, so a reconnect
// cannot overwrite anything on the remote (P-515 keeps the rescan read-only).
void MainWindow::showSourceState() {
    QDialog dialog(this);
    dialog.setWindowTitle(tr("挂载来源状态"));
    dialog.setMinimumWidth(620);
    auto *layout = new QVBoxLayout(&dialog);

    auto *pathRow = new QHBoxLayout;
    auto *pathEdit = new QLineEdit(&dialog);
    pathEdit->setPlaceholderText(tr("已挂载的来源目录（SMB/NFS 挂载点）"));
    pathEdit->setAccessibleName(tr("挂载来源路径"));
    auto *browseBtn = new QPushButton(tr("浏览…"), &dialog);
    pathRow->addWidget(pathEdit, 1);
    pathRow->addWidget(browseBtn);
    layout->addLayout(pathRow);

    auto *stateLabel = new QLabel(tr("未选择来源"), &dialog);
    stateLabel->setWordWrap(true);
    stateLabel->setAccessibleName(tr("来源连接状态"));
    layout->addWidget(stateLabel);
    auto *hint = new QLabel(tr("来源只读探测；重试只重新检查可达性，不写入、不覆盖远端内容。"), &dialog);
    hint->setStyleSheet("color:#888;");
    hint->setWordWrap(true);
    layout->addWidget(hint);

    // P-515: a reconnect rescan is a read-only preview plus an explicit,
    // additive import — never a sync. A source-side delete, rename, or
    // permission change is reported or skipped, never turned into a local
    // deletion or rename (no tombstones, no double-import, no write-back).
    auto *rescanBtn = new QPushButton(tr("重扫预览…"), &dialog);
    rescanBtn->setEnabled(false);
    rescanBtn->setToolTip(tr("扫描来源并与库对照：已导入内容跳过，仅新增内容可导入；来源变化不影响本地素材。"));
    auto *importBtn = new QPushButton(tr("导入新增"), &dialog);
    importBtn->setEnabled(false);
    auto *previewLabel = new QLabel(&dialog);
    previewLabel->setWordWrap(true);
    previewLabel->setStyleSheet("color:#444;");
    layout->addWidget(rescanBtn);
    layout->addWidget(previewLabel);
    layout->addWidget(importBtn);

    // P-510: the backup-target write acceptance is shown separately from the
    // source-read state — a readable source is not automatically a writable
    // backup destination (a read-only share can still be a valid import source).
    auto *targetLabel = new QLabel(tr("备份目标能力：尚未选择"), &dialog);
    targetLabel->setWordWrap(true);
    targetLabel->setAccessibleName(tr("备份目标能力"));
    layout->addWidget(targetLabel);
    auto *targetRow = new QHBoxLayout;
    auto *targetEdit = new QLineEdit(&dialog);
    targetEdit->setPlaceholderText(tr("可选的备份目标目录（可为新目录）"));
    targetEdit->setAccessibleName(tr("备份目标路径"));
    auto *targetBrowse = new QPushButton(tr("选择目标…"), &dialog);
    targetRow->addWidget(targetEdit, 1);
    targetRow->addWidget(targetBrowse);
    layout->addLayout(targetRow);

    auto *buttonRow = new QHBoxLayout;
    auto *retryBtn = new QPushButton(tr("重试"), &dialog);
    retryBtn->setEnabled(false);
    auto *closeBtn = new QPushButton(tr("关闭"), &dialog);
    buttonRow->addWidget(retryBtn);
    buttonRow->addStretch();
    buttonRow->addWidget(closeBtn);
    layout->addLayout(buttonRow);

    SourceConnection connection; // lives on the UI thread; probes run in the background
    // P-510 + audit (2026-11-04): the backup-target probe is a stat + storage
    // query that can block on a dead/NFS/CIFS mount (the dialog exists precisely
    // to handle a dropped mount), so it must never run on the UI thread. A
    // background TaskQueue serializes the probes (one at a time); the newest
    // request wins and stale results are dropped. It never writes to the target.
    qint64 lastTargetTask = -1;
    auto targetResult = std::make_shared<BackupTargetInfo>();
    // The queue is a MainWindow member (targetQueue_), so the dialog's
    // destruction never waits on a worker still probing a dropped mount.
    QObject::connect(targetQueue_.get(), &TaskQueue::taskFinished, &dialog,
        [&](qint64 finished, bool, const QString &) {
            // A stale result (a newer request was queued after this one) is
            // dropped, never applied to the current input.
            if (finished != lastTargetTask) return;
            const BackupTargetInfo &t = *targetResult;
            if (t.writable)
                targetLabel->setText(tr("备份目标能力：可写，可用空间约 %1 MiB")
                                         .arg(t.freeBytes / (1024.0 * 1024.0), 0, 'f', 1));
            else
                targetLabel->setText(tr("备份目标能力：%1").arg(t.reason));
            targetLabel->setStyleSheet(t.writable ? "color:#2e7d32;" : "color:#c62828;");
        });
    auto renderTarget = [&](const QString &path) {
        const QString trimmed = path.trimmed();
        if (trimmed.isEmpty()) {
            targetLabel->setText(tr("备份目标能力：尚未选择"));
            targetLabel->setStyleSheet("color:#888;");
            // Audit (2026-11-04): invalidate any in-flight result so a stale
            // probe for a previously entered path is never applied after the
            // input was cleared (finished == lastTargetTask would otherwise
            // still match and overwrite the "尚未选择" state).
            lastTargetTask = -1;
            return;
        }
        targetLabel->setText(tr("备份目标能力：正在探测…"));
        targetLabel->setStyleSheet("color:#666;");
#ifdef PENTIMENTO_TESTING
        ++targetProbeSubmissions_;
#endif
        lastTargetTask = targetQueue_->submit(QStringLiteral("备份目标探测"),
            [result = targetResult, trimmed](TaskQueue::Progress &) {
                *result = probeBackupTarget(trimmed);
            });
    };
    // P-515 preview result, produced on the worker and read on taskFinished.
    auto preview = std::make_shared<importer::RescanPreview>();
    bool previewDone = false;
    auto render = [&](ConnectionState state, const QString &reason) {
        switch (state) {
        case ConnectionState::Connecting:
            stateLabel->setText(tr("连接中：%1").arg(reason));
            stateLabel->setStyleSheet("color:#666;");
            retryBtn->setEnabled(false);
            rescanBtn->setEnabled(false);
            break;
        case ConnectionState::Reachable:
            stateLabel->setText(tr("在线：来源可达%1")
                                    .arg(reason.isEmpty() ? QString() : tr("（%1）").arg(reason)));
            stateLabel->setStyleSheet("color:#2e7d32;");
            retryBtn->setEnabled(false);
            rescanBtn->setEnabled(true);
            break;
        case ConnectionState::Unreachable:
            stateLabel->setText(tr("离线：来源不可达%1")
                                    .arg(reason.isEmpty() ? QString() : tr("（%1）").arg(reason)));
            stateLabel->setStyleSheet("color:#b26a00;");
            retryBtn->setEnabled(true);
            rescanBtn->setEnabled(false);
            break;
        case ConnectionState::Failed:
            stateLabel->setText(tr("失败：来源探测失败%1")
                                    .arg(reason.isEmpty() ? QString() : tr("（%1）").arg(reason)));
            stateLabel->setStyleSheet("color:#c62828;");
            retryBtn->setEnabled(true);
            rescanBtn->setEnabled(false);
            break;
        }
    };
    QObject::connect(&connection, &SourceConnection::stateChanged, &dialog, render);
    QObject::connect(pathEdit, &QLineEdit::returnPressed, &dialog, [&] {
        if (!pathEdit->text().trimmed().isEmpty()) connection.open(pathEdit->text());
    });
    QObject::connect(browseBtn, &QPushButton::clicked, &dialog, [&] {
        const QString path = QFileDialog::getExistingDirectory(&dialog, tr("选择已挂载来源目录"));
        if (path.isEmpty()) return;
        pathEdit->setText(path);
        connection.open(path);
    });
    QObject::connect(retryBtn, &QPushButton::clicked, &connection, &SourceConnection::retry);
    // P-510: target-write acceptance probe — text entry or browse re-runs the
    // read-only probe (never writes to the target).
    QObject::connect(targetEdit, &QLineEdit::textChanged, &dialog, renderTarget);
    QObject::connect(targetBrowse, &QPushButton::clicked, &dialog, [&] {
        const QString path = QFileDialog::getExistingDirectory(&dialog, tr("选择备份目标目录"));
        if (path.isEmpty()) return;
        targetEdit->setText(path);
    });
    // P-515: run the rescan on the background writer queue (a mounted-source
    // scan can be slow and may hang, never on the UI thread).
    QObject::connect(rescanBtn, &QPushButton::clicked, &dialog, [&] {
        const QString path = pathEdit->text().trimmed();
        if (path.isEmpty()) return;
        rescanBtn->setEnabled(false);
        previewLabel->setText(tr("正在扫描来源并与库对照…"));
        const qint64 id = captureQueue_->submit(QStringLiteral("来源重扫预览"),
            [this, path, preview](TaskQueue::Progress &) { *preview = importer::rescanPreview(library_, path); });
        QObject::connect(captureQueue_.get(), &TaskQueue::taskFinished, &dialog,
            [&, id](qint64 finished, bool, const QString &) {
                if (finished != id) return;
                rescanBtn->setEnabled(connection.state() == ConnectionState::Reachable);
                importBtn->setEnabled(preview->fresh() > 0);
                previewLabel->setText(tr("重扫预览：新增 %1（可导入）、%2 已在库中、%3 不可读（不视为删除）")
                                          .arg(preview->fresh())
                                          .arg(preview->alreadyImported())
                                          .arg(preview->unreadable.size()));
                QStringList lines;
                for (const auto &c : preview->candidates) {
                    if (c.candidate.excludeReason.isEmpty()) continue;
                    lines << tr("· 已排除：%1（%2）").arg(c.candidate.relative, c.candidate.excludeReason);
                }
                for (const auto &u : preview->unreadable) lines << tr("· 不可读：%1").arg(u);
                if (!lines.isEmpty()) previewLabel->setText(previewLabel->text() + "\n" + lines.join('\n'));
            });
    });
    // P-515: importing is additive only — the fresh candidates are imported,
    // never anything deleted, renamed, or tombstoned.
    QObject::connect(importBtn, &QPushButton::clicked, &dialog, [&] {
        QStringList paths;
        for (const auto &c : preview->candidates)
            if (c.error.isEmpty() && !c.alreadyImported && c.candidate.excludeReason.isEmpty())
                paths << c.candidate.sourcePath;
        if (paths.isEmpty()) return;
        importBtn->setEnabled(false);
        previewLabel->setText(tr("正在导入 %1 个新增内容…").arg(paths.size()));
        const qint64 id = captureQueue_->submit(QStringLiteral("来源重扫导入"),
            [this, paths](TaskQueue::Progress &p) { importer::importPlan(library_, paths, p); });
        QObject::connect(captureQueue_.get(), &TaskQueue::taskFinished, &dialog,
            [&, id](qint64 finished, bool ok, const QString &error) {
                if (finished != id) return;
                if (!ok) {
                    previewLabel->setText(tr("导入未完成：%1").arg(error));
                    return;
                }
                QStringList done;
                for (const auto &s : captureQueue_->statuses())
                    if (s.id == id)
                        for (const auto &item : s.items)
                            done << (item.ok ? tr("· 已导入：%1").arg(item.label)
                                             : tr("· 失败：%1（%2）").arg(item.label, item.error));
                previewLabel->setText(done.isEmpty() ? tr("没有新增内容可导入")
                                                     : tr("导入完成：\n%1").arg(done.join('\n')));
                importBtn->setEnabled(false);
                reloadAssets();
                if (sentinel_) sentinel_->resetBaseline();
            });
    });
    QObject::connect(closeBtn, &QPushButton::clicked, &dialog, &QDialog::accept);
    dialog.exec();
}

// P-204: index a directory OUTSIDE the library without copying it. The
// indexed tree is only ever read — the catalog records path/size/mtime/hash
// so browsing and consistency views can account for the source, and a
// refresh flips vanished entries to "missing" instead of dropping them.
void MainWindow::indexExternalDirectory() {
    const QString dir = QFileDialog::getExistingDirectory(
        this, tr("选择要索引的外部目录（不复制、不修改文件）"));
    if (dir.isEmpty()) return;
    qint64 rootId = 0;
    try {
        rootId = library_.addExternalRoot(dir);
    } catch (const std::exception &) {
        // Already registered: refresh that root instead of failing.
        const QString canonical = QDir(dir).canonicalPath();
        bool found = false;
        for (const auto &root : library_.externalRoots()) {
            if (root.path == canonical) {
                rootId = root.id;
                found = true;
                break;
            }
        }
        if (!found) {
            QMessageBox::warning(this, tr("无法索引该目录"),
                                 tr("目录不可读或位于素材库内部。"));
            return;
        }
    }
    int missing = 0;
    try {
        const int indexed = library_.indexExternalRoot(rootId);
        const auto assets = library_.externalAssets(rootId);
        for (const auto &asset : assets)
            if (asset.state == Library::ExternalAssetState::Missing) ++missing;
        QMessageBox::information(this, tr("外部目录索引"),
            tr("已索引 %1 个文件（仅读取，未复制、未修改来源）。\n目录：%2\n记录共 %3 条，其中缺失 %4 条。")
                .arg(indexed).arg(dir).arg(assets.size()).arg(missing));
    } catch (const std::exception &error) {
        report(error);
    }
}
