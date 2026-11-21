// Snapshot and safety flows: the snapshot schedule state machine,
// exclusion rules, the sentinel poll with its emergency anchor,
// snapshot browsing and one-click rollback (LAY-003 split).
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


// P-305: period combo changed. Enabling computes the next run from now plus
// one period; disabling drops the schedule and any pause state entirely.
void MainWindow::setSnapshotPeriod(int index) {
    const int minutes = snapshotPeriodCombo_->itemData(index).toInt();
    snapshotSchedule_.setPeriod(minutes, QDateTime::currentDateTime());
    snapshotPauseBtn_->setText(tr("暂停"));
    snapshotPauseBtn_->setEnabled(minutes > 0);
    updateSnapshotStatus();
}

// P-305: pause asks for a reason (recorded, user-initiated); resume restarts
// the clock from now regardless of who paused.
void MainWindow::toggleSnapshotPause() {
    if (!snapshotSchedule_.enabled()) return;
    if (snapshotSchedule_.paused()) {
        snapshotSchedule_.resume(QDateTime::currentDateTime());
        snapshotPauseBtn_->setText(tr("暂停"));
    } else {
        bool ok = false;
        const QString reason = QInputDialog::getText(this, tr("暂停定时快照"), tr("暂停原因"),
                                                     QLineEdit::Normal, tr("用户暂停"), &ok);
        if (!ok) return;
        snapshotSchedule_.pauseByUser(reason.isEmpty() ? tr("用户暂停") : reason);
        snapshotPauseBtn_->setText(tr("恢复"));
    }
    updateSnapshotStatus();
}

void MainWindow::updateSnapshotStatus() {
    if (!snapshotSchedule_.enabled()) {
        snapshotStatus_->setText(tr("定时快照未启用"));
    } else if (snapshotSchedule_.paused()) {
        snapshotStatus_->setText(tr("已暂停：%1").arg(snapshotSchedule_.pauseReason()));
    } else if (snapshotSchedule_.inProgress()) {
        snapshotStatus_->setText(tr("快照进行中…"));
    } else {
        snapshotStatus_->setText(tr("下次快照：%1")
                                     .arg(snapshotSchedule_.next().toString(QStringLiteral("MM-dd HH:mm"))));
    }
}

// P-305: called on every 2 s housekeeping tick. A run is due only when the
// schedule is active, not paused, and no snapshot is in progress — an overlap
// is merged (skipped), never stacked. When several periods were missed (e.g.
// the machine slept) the single due run catches up once and the next run is
// recomputed from now, so missed periods never pile up tasks.
void MainWindow::checkSnapshotSchedule() {
    const auto now = QDateTime::currentDateTime();
    if (!snapshotSchedule_.due(now)) return;
    snapshotSchedule_.beginRun(now);
    updateSnapshotStatus();
    submitScheduledSnapshot();
}

// P-305: take the snapshot on the background worker (the same serial queue as
// auto-record, so writes never overlap) and hand the outcome back for the next
// scan tick to drain.
void MainWindow::submitScheduledSnapshot() {
    captureQueue_->submit(QStringLiteral("定时快照"), [this](TaskQueue::Progress &) {
        QString error;
        try {
            library_.snapshotAll(QStringLiteral("定时快照"));
        } catch (const std::exception &e) {
            error = QString::fromUtf8(e.what());
        }
        QMutexLocker lock(&captureResultsMutex_);
        snapshotAsyncError_ = error;
        snapshotAsyncDone_ = true;
    });
}

void MainWindow::drainSnapshotResult() {
    bool done = false;
    QString error;
    {
        QMutexLocker lock(&captureResultsMutex_);
        done = snapshotAsyncDone_;
        error = snapshotAsyncError_;
        snapshotAsyncDone_ = false;
    }
    if (!done) return;
    snapshotSchedule_.endRun();
    if (!error.isEmpty()) statusBar()->showMessage(tr("定时快照失败：%1").arg(error));
    updateSnapshotStatus();
}

// P-306: manage version-exclusion rules in a modal dialog (kept off the main
// panel so it does not crowd the versioning column). A rule is added with a
// kind (extension/path/name) and a pattern, or removed by selecting it.
void MainWindow::manageExclusions() {
    QDialog dialog(this);
    dialog.setWindowTitle(tr("版本排除规则"));
    dialog.resize(440, 320);
    auto *layout = new QVBoxLayout(&dialog);
    auto *hint = new QLabel(tr("命中规则的对象不进入后续快照；历史保留，被排除 ≠ 已删除，恢复时不会触碰其当前文件。"));
    hint->setWordWrap(true);
    layout->addWidget(hint);
    auto *kindCombo = new QComboBox;
    kindCombo->addItem(tr("扩展名"), QStringLiteral("extension"));
    kindCombo->addItem(tr("路径"), QStringLiteral("path"));
    kindCombo->addItem(tr("名称"), QStringLiteral("name"));
    auto *patternEdit = new QLineEdit;
    patternEdit->setPlaceholderText(tr("如：psd、scenes、hero_*.png"));
    auto *addBtn = new QPushButton(tr("添加"));
    auto *row = new QHBoxLayout;
    row->addWidget(kindCombo);
    row->addWidget(patternEdit, 1);
    row->addWidget(addBtn);
    layout->addLayout(row);
    auto *list = new QListWidget;
    layout->addWidget(list, 1);
    auto *removeBtn = new QPushButton(tr("删除选中"));
    layout->addWidget(removeBtn);
    const auto kindLabel = [this](const QString &kind) {
        return kind == QLatin1String("extension") ? tr("扩展名")
               : kind == QLatin1String("path") ? tr("路径") : tr("名称");
    };
    const auto reload = [&] {
        list->clear();
        for (const auto &rule : library_.exclusions()) {
            auto *item = new QListWidgetItem(QStringLiteral("%1 · %2").arg(kindLabel(rule.kind), rule.pattern));
            item->setData(Qt::UserRole, rule.kind);
            item->setData(Qt::UserRole + 1, rule.pattern);
            list->addItem(item);
        }
    };
    reload();
    connect(addBtn, &QPushButton::clicked, &dialog, [&] {
        try {
            library_.addExclusion(kindCombo->currentData().toString(), patternEdit->text());
            patternEdit->clear();
            reload();
        } catch (const std::exception &e) {
            QMessageBox::warning(&dialog, tr("无法添加排除规则"), QString::fromUtf8(e.what()));
        }
    });
    connect(removeBtn, &QPushButton::clicked, &dialog, [&] {
        const auto item = list->currentItem();
        if (!item) return;
        try {
            library_.removeExclusion(item->data(Qt::UserRole).toString(), item->data(Qt::UserRole + 1).toString());
            reload();
        } catch (const std::exception &e) {
            QMessageBox::warning(&dialog, tr("无法删除排除规则"), QString::fromUtf8(e.what()));
        }
    });
    dialog.exec();
}

void MainWindow::pollSentinel() {
    if (!sentinel_) return;
    const auto alert = sentinel_->check();
    if (!alert.triggered) return;
    // Ransomware-style batch write detected: anchor the current state, then tell
    // the user. The anchor keeps the pre-infection revisions restorable even if
    // the workspace is later rolled back or overwritten.
    try { library_.snapshotAll(QStringLiteral("紧急快照：疑似勒索批量写入")); }
    catch (const std::exception &error) {
        statusBar()->showMessage(tr("紧急快照失败：%1").arg(QString::fromUtf8(error.what())));
    }
    QMessageBox::warning(this, tr("疑似勒索批量写入"),
                         tr("工作区检测到异常批量写入，已自动建立紧急快照锚点。\n\n%1\n\n可先关闭外部编辑器，再通过「恢复历史版本」或「恢复到锚点」回退。").arg(alert.reason));
}

void MainWindow::browseSnapshot() {
    const auto snapshots = library_.snapshots();
    if (snapshots.isEmpty()) {
        statusBar()->showMessage(tr("还没有快照"));
        return;
    }
    QStringList labels;
    for (const auto &snapshot : snapshots) {
        const QString excluded = snapshot.excluded > 0
            ? tr("（%1 个被排除）").arg(snapshot.excluded) : QString();
        labels << tr("#%1 %2 · %3 个素材%4")
                      .arg(snapshot.id).arg(snapshot.created).arg(snapshot.assets).arg(excluded);
    }
    bool ok = false;
    const auto choice = QInputDialog::getItem(this, tr("浏览快照"),
                                              tr("选择要浏览的快照（只读，不修改任何内容）"),
                                              labels, 0, false, &ok);
    if (!ok) return;
    const int index = labels.indexOf(choice);
    if (index < 0) return;
    const auto snapshot = snapshots.at(index);
    const auto entries = library_.snapshotEntries(snapshot.id);

    // P-312: read-only point-in-time directory listing built from the snapshot
    // manifest (path and states frozen at the snapshot boundary — never mixed
    // with the current index). No write-back, no completeness claim.
    QDialog dialog(this);
    dialog.setWindowTitle(tr("时间点目录浏览（只读）"));
    dialog.resize(560, 420);
    auto *layout = new QVBoxLayout(&dialog);
    auto *hint = new QLabel(tr("#%1 · %2 · %3 个素材（只读）")
        .arg(snapshot.id).arg(snapshot.created).arg(snapshot.assets));
    hint->setStyleSheet("color:#888;");
    layout->addWidget(hint);
    auto *list = new QListWidget(&dialog);
    for (const auto &entry : entries) {
        // Manifest path as of the snapshot (folder/name frozen at the boundary).
        const QString path = entry.folder.isEmpty() ? entry.name
                                                    : entry.folder + QLatin1Char('/') + entry.name;
        QStringList parts{path};
        if (entry.state == QLatin1String("tombstone")) parts << tr("已删除（历史保留）");
        else if (entry.state == QLatin1String("missing")) parts << tr("工作副本缺失（可恢复）");
        else if (entry.state == QLatin1String("excluded")) parts << tr("已排除：%1").arg(entry.reason);
        auto *item = new QListWidgetItem(parts.join(QStringLiteral("  ·  ")), list);
        if (entry.state == QLatin1String("tombstone"))
            item->setIcon(style()->standardIcon(QStyle::SP_DialogDiscardButton));
        else if (entry.state == QLatin1String("missing"))
            item->setIcon(style()->standardIcon(QStyle::SP_MessageBoxWarning));
        else if (entry.state == QLatin1String("excluded"))
            item->setIcon(style()->standardIcon(QStyle::SP_MessageBoxInformation));
    }
    if (entries.isEmpty()) {
        auto *empty = new QListWidgetItem(tr("（此快照没有条目）"), list);
        empty->setFlags(empty->flags() & ~Qt::ItemIsEnabled);
    }
    layout->addWidget(list, 1);
    // P-318: selective restore. Only the assets the user reviewed in the preview
    // are touched (excluded/tombstone entries are skipped with a reason), each
    // item reports its own outcome, and the batch is never claimed atomic.
    auto *restoreBtn = new QPushButton(tr("恢复选中项…"), &dialog);
    restoreBtn->setEnabled(false);
    connect(list, &QListWidget::itemSelectionChanged, restoreBtn, [restoreBtn, list]() {
        restoreBtn->setEnabled(!list->selectedItems().isEmpty());
    });
    connect(restoreBtn, &QPushButton::clicked, &dialog,
            [&dialog, &snapshot, &entries, &list, this]() {
        QStringList assetIds;
        for (auto *sel : list->selectedItems()) {
            const int idx = list->row(sel);
            if (idx < 0 || idx >= entries.size()) continue;
            const auto &entry = entries.at(idx);
            if (entry.state != QLatin1String("excluded") && entry.state != QLatin1String("tombstone"))
                assetIds << entry.assetId;
        }
        if (assetIds.isEmpty()) {
            QMessageBox::information(&dialog, tr("恢复选中项"),
                                     tr("请选择快照中存在的素材（已排除或已删除的条目不可恢复）。"));
            return;
        }
        QVector<Library::SnapshotRestoreItem> items;
        try { items = library_.planSnapshotRestore(snapshot.id, assetIds); }
        catch (const std::exception &error) { report(error); return; }
        QStringList lines;
        qint64 space = 0;
        for (const auto &item : items) {
            QString action;
            if (item.plan.action == QStringLiteral("overwrite")) action = tr("覆盖");
            else if (item.plan.action == QStringLiteral("create")) action = tr("新增");
            else if (item.plan.action == QStringLiteral("skip")) action = tr("跳过：%1").arg(item.plan.conflictReason);
            else action = tr("冲突：%1").arg(item.plan.conflictReason);
            space += item.plan.spaceNeeded;
            lines << tr("· %1 —— %2").arg(item.name).arg(action);
        }
        if (space > 0) lines << tr("空间需求：约 %1 KiB").arg(space / 1024.0, 0, 'f', 1);
        QMessageBox box(&dialog);
        box.setWindowTitle(tr("恢复预演"));
        box.setIcon(QMessageBox::Question);
        box.setText(tr("将恢复以下选中项（写入前预演）：\n%1\n\n保护的是磁盘已保存内容；外部编辑器未写盘内容不受保护。")
                        .arg(lines.join(QLatin1Char('\n'))));
        QAbstractButton *confirm = box.addButton(tr("确认恢复"), QMessageBox::YesRole);
        box.addButton(tr("取消"), QMessageBox::NoRole);
        box.exec();
        if (box.clickedButton() != confirm) return;
        const auto outcomes = library_.restoreFromSnapshot(snapshot.id, items);
        QStringList result;
        for (const auto &outcome : outcomes) {
            if (outcome.outcome == QStringLiteral("success"))
                result << tr("· %1 —— 成功").arg(outcome.name);
            else if (outcome.outcome == QStringLiteral("skipped"))
                result << tr("· %1 —— 跳过：%2").arg(outcome.name, outcome.error);
            else
                result << tr("· %1 —— 失败：%2").arg(outcome.name, outcome.error);
        }
        QMessageBox::information(&dialog, tr("恢复结果"), result.join(QLatin1Char('\n')));
        reloadAssets();
    });
    layout->addWidget(restoreBtn);
    auto *closeBtn = new QPushButton(tr("关闭"), &dialog);
    connect(closeBtn, &QPushButton::clicked, &dialog, &QDialog::accept);
    layout->addWidget(closeBtn);
    dialog.exec();
}

void MainWindow::rollbackToSnapshot() {
    const auto snapshots = library_.snapshots();
    if (snapshots.isEmpty()) {
        statusBar()->showMessage(tr("还没有紧急快照锚点"));
        return;
    }
    QStringList labels;
    for (const auto &snapshot : snapshots) {
        // P-306: an excluded entry is listed separately; rollback never touches it.
        const QString excluded = snapshot.excluded > 0
            ? tr("（%1 个被排除）").arg(snapshot.excluded) : QString();
        labels << tr("#%1 %2 · %3 个素材%4")
                      .arg(snapshot.id)
                      .arg(snapshot.created)
                      .arg(snapshot.assets)
                      .arg(excluded);
    }
    bool ok = false;
    const auto choice = QInputDialog::getItem(this, tr("恢复到锚点"),
                                              tr("选择感染前锚点（回滚形成新版本，不销毁后续历史）"), labels, 0, false, &ok);
    if (!ok) return;
    const int index = labels.indexOf(choice);
    if (index < 0) return;
    const qint64 snapshotId = snapshots.at(index).id;
    if (QMessageBox::question(this, tr("恢复到锚点"),
                              tr("将整个工作区恢复到该锚点。\n未记录的当前内容会先保存，后续历史（含被加密版本）保留。\n请先关闭所有外部编辑器。")) != QMessageBox::Yes) return;
    try {
        const int restored = library_.rollbackAll(snapshotId);
        reloadAssets();
        // Re-arm the sentinel: rollback is an app-initiated write, not ransomware.
        if (sentinel_) sentinel_->resetBaseline();
        statusBar()->showMessage(tr("已恢复到锚点 #%1（%2 个素材回滚）").arg(snapshotId).arg(restored));
    } catch (const std::exception &error) { report(error); }
}
