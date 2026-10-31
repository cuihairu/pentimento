#include "mainwindow.h"
#include "sentinel.h"
#include <QComboBox>
#include <QDateTime>
#include <QDesktopServices>
#include <QDialog>
#include <QFileDialog>
#include <QHeaderView>
#include <QTableWidget>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QImageReader>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollArea>
#include <QSplitter>
#include <QStatusBar>
#include <QStyle>
#include <QTimer>
#include <QToolBar>
#include <QUrl>

MainWindow::MainWindow(Library &library) : library_(library) {
    setWindowTitle(QStringLiteral("pentimento · 画迹"));
    resize(1240, 790);
    setMinimumSize(860, 540);
    auto *toolbar = addToolBar(QStringLiteral("素材操作"));
    toolbar->setMovable(false);
    auto *brand = new QLabel(QStringLiteral("  pentimento   "));
    brand->setStyleSheet("font-size:20px;font-weight:600;padding:8px;");
    toolbar->addWidget(brand);
    auto *importAction = toolbar->addAction(QStringLiteral("导入素材"), this, &MainWindow::importFiles);
    importAction->setShortcut(QKeySequence("Ctrl+O"));
    toolbar->addAction(QStringLiteral("记录版本"), this, &MainWindow::capture)->setShortcut(QKeySequence("Ctrl+S"));
    toolbar->addAction(QStringLiteral("编辑工作副本"), this, [this] {
        const auto asset = selected();
        if (!asset.id.isEmpty() && !QDesktopServices::openUrl(QUrl::fromLocalFile(asset.path)))
            statusBar()->showMessage(QStringLiteral("无法打开默认编辑器；可通过素材库目录打开工作副本"));
    });
    toolbar->addAction(QStringLiteral("打开素材库"), this, [this] {
        QDesktopServices::openUrl(QUrl::fromLocalFile(library_.root()));
    });
    // Anti-ransomware Layer 3: one-click rollback to a pre-infection anchor.
    toolbar->addAction(QStringLiteral("恢复到锚点"), this, &MainWindow::rollbackToSnapshot);
    // P-312 read-only time-point browsing: switch snapshots and see the
    // directory tree and deletion/exclusion state as of that moment.
    toolbar->addAction(QStringLiteral("浏览时间点"), this, &MainWindow::browseTimePoint);
    // P-305 scheduled snapshots: default OFF, 15/30/60-minute periods, pausable.
    auto *scheduleMenu = new QMenu(this);
    const QList<int> periods = {15, 30, 60};
    for (const int minutes : periods) {
        auto *action = scheduleMenu->addAction(QStringLiteral("每 %1 分钟").arg(minutes), this, [this, minutes] {
            try { library_.enableSchedule(minutes); updateScheduleStatus(); }
            catch (const std::exception &error) { report(error); }
        });
        action->setCheckable(true);
    }
    scheduleMenu->addSeparator();
    pauseAction_ = scheduleMenu->addAction(QStringLiteral("暂停定时快照"), this, [this] {
        if (library_.scheduleState().paused) library_.resumeSchedule();
        else library_.pauseSchedule(QStringLiteral("user"));
        updateScheduleStatus();
    });
    scheduleMenu->addAction(QStringLiteral("关闭定时快照"), this, [this] {
        library_.disableSchedule(); updateScheduleStatus();
    });
    auto *scheduleAction = toolbar->addAction(QStringLiteral("定时快照"));
    scheduleAction->setMenu(scheduleMenu);
    scheduleLabel_ = new QLabel;
    statusBar()->addPermanentWidget(scheduleLabel_);
    auto *splitter = new QSplitter;
    auto *left = new QWidget;
    auto *leftLayout = new QVBoxLayout(left);
    auto *libraryTitle = new QLabel(QStringLiteral("素材库"));
    libraryTitle->setStyleSheet("font-size:16px;font-weight:600;");
    search_ = new QLineEdit;
    search_->setPlaceholderText(QStringLiteral("搜索素材名称"));
    search_->setAccessibleName(QStringLiteral("搜索素材"));
    assetsList_ = new QListWidget;
    assetsList_->setAccessibleName(QStringLiteral("素材列表"));
    assetsList_->setIconSize(QSize(48, 48));
    assetsList_->setSpacing(5);
    leftLayout->addWidget(libraryTitle);
    leftLayout->addWidget(search_);
    leftLayout->addWidget(assetsList_);
    auto *center = new QWidget;
    auto *centerLayout = new QVBoxLayout(center);
    details_ = new QLabel(QStringLiteral("导入第一份素材，开始保留创作历史。"));
    details_->setWordWrap(true);
    details_->setTextFormat(Qt::PlainText);
    preview_ = new QLabel(QStringLiteral("素材预览\n\n支持 PNG、JPEG、WebP 等已安装的 Qt 图像格式\n其他文件保留原始内容，暂不提供画面预览"));
    preview_->setAlignment(Qt::AlignCenter);
    preview_->setMinimumSize(240, 200);
    preview_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
    preview_->setStyleSheet("background:#e5e5e2;color:#444845;border-radius:4px;padding:16px;");
    centerLayout->addWidget(details_);
    centerLayout->addWidget(preview_, 1);
    auto *right = new QWidget;
    auto *rightLayout = new QVBoxLayout(right);
    auto *historyTitle = new QLabel(QStringLiteral("版本时间轴"));
    historyTitle->setStyleSheet("font-size:16px;font-weight:600;");
    hint_ = new QLabel(QStringLiteral("选择历史版本预览\n恢复会新增版本，原有历史始终保留。"));
    hint_->setWordWrap(true);
    historyList_ = new QListWidget;
    historyList_->setAccessibleName(QStringLiteral("历史版本"));
    historyList_->setIconSize(QSize(48, 48));
    historyList_->setSpacing(5);
    restore_ = new QPushButton(QStringLiteral("恢复此版本"));
    export_ = new QPushButton(QStringLiteral("导出此版本…"));
    rightLayout->addWidget(historyTitle);
    rightLayout->addWidget(hint_);
    rightLayout->addWidget(historyList_, 1);
    rightLayout->addWidget(restore_);
    rightLayout->addWidget(export_);
    splitter->addWidget(left); splitter->addWidget(center); splitter->addWidget(right);
    splitter->setSizes({260, 660, 300});
    setCentralWidget(splitter);
    connect(search_, &QLineEdit::textChanged, this, [this] {
        for (int i = 0; i < assetsList_->count(); ++i)
            assetsList_->item(i)->setHidden(!assetsList_->item(i)->text().contains(search_->text(), Qt::CaseInsensitive));
    });
    connect(assetsList_, &QListWidget::currentRowChanged, this, &MainWindow::reloadHistory);
    connect(historyList_, &QListWidget::currentRowChanged, this, &MainWindow::showRevision);
    connect(restore_, &QPushButton::clicked, this, &MainWindow::restore);
    connect(export_, &QPushButton::clicked, this, &MainWindow::exportFile);
    auto *timer = new QTimer(this);
    connect(timer, &QTimer::timeout, this, &MainWindow::scanChanges);
    timer->start(2000);
    // Anti-ransomware Layer 2: watch the workspace for abnormal batch writes.
    sentinel_ = std::make_unique<Sentinel>(library_.root() + QStringLiteral("/working"));
    sentinel_->resetBaseline();
    auto *sentinelTimer = new QTimer(this);
    connect(sentinelTimer, &QTimer::timeout, this, &MainWindow::pollSentinel);
    sentinelTimer->start(5000);
    // P-305: pump the schedule once a minute; missed periods collapse to one.
    auto *scheduleTimer = new QTimer(this);
    connect(scheduleTimer, &QTimer::timeout, this, [this] {
        try {
            const qint64 snapshotId = library_.pumpSchedule(QDateTime::currentMSecsSinceEpoch());
            if (snapshotId > 0) statusBar()->showMessage(QStringLiteral("定时快照 #%1 已记录").arg(snapshotId), 10000);
        } catch (const std::exception &error) { report(error); }
        updateScheduleStatus();
    });
    scheduleTimer->start(60000);
    reloadAssets();
    updateScheduleStatus();
    statusBar()->showMessage(QStringLiteral("本地素材库：%1 · 编辑工作副本后自动记录稳定变化").arg(library_.root()));
}

void MainWindow::updateScheduleStatus() {
    // The schedule state stays visible: period + next fire time (or pause reason).
    const auto state = library_.scheduleState();
    QString text = QStringLiteral("定时快照：");
    if (!state.enabled) text += QStringLiteral("关闭");
    else if (state.paused) text += QStringLiteral("已暂停（%1）").arg(state.pauseReason);
    else text += QStringLiteral("每 %1 分钟 · 下次 %2").arg(state.periodMinutes)
                    .arg(QDateTime::fromMSecsSinceEpoch(state.nextUtcMs).toString(QStringLiteral("MM-dd HH:mm")));
    scheduleLabel_->setText(QStringLiteral("  %1  ").arg(text));
    if (pauseAction_) pauseAction_->setText(state.paused ? QStringLiteral("继续定时快照") : QStringLiteral("暂停定时快照"));
}

Asset MainWindow::selected() const {
    const int row = assetsList_->currentRow();
    return row >= 0 && row < assets_.size() ? assets_.at(row) : Asset{};
}

void MainWindow::report(const std::exception &error) {
    QMessageBox::warning(this, QStringLiteral("操作未完成"), QString::fromUtf8(error.what()));
}

void MainWindow::reloadAssets(const QString &selectId) {
    assets_ = library_.assets();
    assetsList_->blockSignals(true);
    assetsList_->clear();
    int selection = -1;
    for (const auto &asset : assets_) {
        auto *item = new QListWidgetItem(asset.name, assetsList_);
        item->setToolTip(asset.path);
        QImageReader reader(asset.path);
        reader.setAutoTransform(true);
        reader.setScaledSize(QSize(80, 80));
        item->setIcon(QPixmap::fromImage(reader.read()));
        item->setHidden(!asset.name.contains(search_->text(), Qt::CaseInsensitive));
        if (asset.id == selectId) selection = assetsList_->count() - 1;
    }
    assetsList_->setCurrentRow(selection >= 0 ? selection : (assets_.isEmpty() ? -1 : 0));
    assetsList_->blockSignals(false);
    reloadHistory();
}

void MainWindow::reloadHistory() {
    const auto asset = selected();
    // P-308: the timeline comes from the library's content-derived marks, so
    // ordering is the deterministic id, and current/restore/corrupt/unrecorded
    // are accurate regardless of clock or selection.
    if (asset.id.isEmpty()) {
        revisions_.clear();
        unrecorded_ = false;
    } else {
        const auto timeline = library_.assetTimeline(asset.id);
        revisions_ = timeline.entries;
        unrecorded_ = timeline.unrecorded;
    }
    hint_->setText(unrecorded_
        ? QStringLiteral("工作副本有未记录修改，尚未进入时间轴。\n恢复会新增版本，原有历史始终保留。")
        : QStringLiteral("选择历史版本预览\n恢复会新增版本，原有历史始终保留。"));
    historyList_->blockSignals(true);
    historyList_->clear();
    for (int i = 0; i < revisions_.size(); ++i) {
        const auto &entry = revisions_.at(i);
        const auto date = QDateTime::fromString(entry.created, Qt::ISODateWithMs).toLocalTime();
        QString state;
        if (entry.current) state = QStringLiteral("当前 · ");
        else if (entry.restore) state = QStringLiteral("恢复 · ");
        else if (entry.pinned) state = QStringLiteral("固定 · ");
        auto *item = new QListWidgetItem(QStringLiteral("%1%2\n%3\n%4")
            .arg(state, date.toString("MM-dd HH:mm:ss"), entry.note,
                 QStringLiteral("%1 KiB").arg(entry.size / 1024.0, 0, 'f', 1)), historyList_);
        if (entry.pinned)
            item->setToolTip(entry.pinReason.isEmpty() ? QStringLiteral("固定版本")
                                                       : QStringLiteral("固定：%1").arg(entry.pinReason));
        QIcon icon;
        if (entry.corrupt) {
            icon = style()->standardIcon(QStyle::SP_MessageBoxWarning);
        } else {
            QImageReader reader(library_.objectPath(entry.hash));
            reader.setDecideFormatFromContent(true);
            reader.setScaledSize(QSize(80, 80));
            icon = QPixmap::fromImage(reader.read());
        }
        if (!icon.isNull()) item->setIcon(icon);
    }
    historyList_->setCurrentRow(revisions_.isEmpty() ? -1 : 0);
    historyList_->blockSignals(false);
    showRevision();
}

void MainWindow::showRevision() {
    const int row = historyList_->currentRow();
    const bool valid = row >= 0 && row < revisions_.size();
    restore_->setEnabled(valid); export_->setEnabled(valid);
    if (!valid) return;
    const auto &revision = revisions_.at(row);
    details_->setText(QStringLiteral("%1\n%2 · %3 · #%4")
        .arg(selected().name, revision.current ? QStringLiteral("当前版本") : QStringLiteral("历史版本"),
             QDateTime::fromString(revision.created, Qt::ISODateWithMs).toLocalTime().toString("yyyy-MM-dd HH:mm:ss"))
        .arg(revision.revisionId));
    if (revision.corrupt) {
        preview_->setPixmap(QPixmap());
        preview_->setText(QStringLiteral("此版本对象已损坏或校验失败\n历史记录保留，可导出对象文件排查。"));
        return;
    }
    QImageReader reader(library_.objectPath(revision.hash));
    reader.setDecideFormatFromContent(true);
    reader.setAutoTransform(true);
    const auto size = reader.size();
    if (size.isValid()) reader.setScaledSize(size.scaled(preview_->size() - QSize(32,32), Qt::KeepAspectRatio));
    const QImage image = reader.read();
    if (image.isNull()) {
        preview_->setPixmap(QPixmap());
        preview_->setText(QStringLiteral("此格式暂不支持画面预览\n\n原始文件和历史版本已完整保留，可以导出查看。"));
    } else preview_->setPixmap(QPixmap::fromImage(image));
}

void MainWindow::importFiles() {
    const auto files = QFileDialog::getOpenFileNames(this, QStringLiteral("导入素材（保留原文件，创建工作副本）"));
    QString last;
    for (const auto &file : files) {
        try { last = library_.importFile(file).id; }
        catch (const std::exception &error) { report(error); break; }
    }
    reloadAssets(last);
    // Re-arm the sentinel: the app's own import is legitimate, not ransomware.
    if (sentinel_) sentinel_->resetBaseline();
}

void MainWindow::capture() {
    const auto asset = selected();
    if (asset.id.isEmpty()) return;
    bool ok = false;
    const auto note = QInputDialog::getText(this, QStringLiteral("记录版本"), QStringLiteral("版本说明"), QLineEdit::Normal, QStringLiteral("手动快照"), &ok);
    if (!ok) return;
    try {
        const auto result = library_.recordVersion(asset, note);
        // Explicit feedback so the user never mistakes an unchanged capture for
        // a successful new version (防误以为已保护).
        statusBar()->showMessage(result.unchanged
            ? QStringLiteral("内容未变化，当前已是版本 #%1").arg(result.revisionId)
            : QStringLiteral("已记录版本 #%1").arg(result.revisionId));
        reloadHistory();
    } catch (const std::exception &error) { report(error); }
}

void MainWindow::restore() {
    const int row = historyList_->currentRow();
    if (row < 0) return;
    if (QMessageBox::question(this, QStringLiteral("恢复历史版本"), QStringLiteral("将历史内容恢复为当前工作副本，并新增一个版本。\n未记录的当前内容会先保存。请先在外部编辑器中关闭此文件。")) != QMessageBox::Yes) return;
    try { library_.restore(selected(), revisions_.at(row).revisionId); reloadAssets(selected().id); }
    catch (const std::exception &error) { report(error); }
    // Re-arm the sentinel: restore is an app-initiated write, not ransomware.
    if (sentinel_) sentinel_->resetBaseline();
}

void MainWindow::exportFile() {
    const int row = historyList_->currentRow();
    if (row < 0) return;
    const auto path = QFileDialog::getSaveFileName(this, QStringLiteral("导出历史版本"), selected().name);
    if (path.isEmpty()) return;
    try { library_.exportRevision(selected().id, revisions_.at(row).revisionId, path); }
    catch (const std::exception &error) { report(error); }
}

void MainWindow::scanChanges() {
    for (const auto &asset : assets_) {
        try {
            // P-304 stable-change policy: two stable ticks + read-before/after
            // recheck, so a half-written or continuously-written file is never
            // recorded as a version (retry re-observes on a later tick).
            const auto tick = library_.autoRecordTick(asset);
            if (tick.recorded && selected().id == asset.id) reloadHistory();
        } catch (const std::exception &error) {
            statusBar()->showMessage(QStringLiteral("自动记录失败：%1").arg(QString::fromUtf8(error.what())));
        }
    }
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
        statusBar()->showMessage(QStringLiteral("紧急快照失败：%1").arg(QString::fromUtf8(error.what())));
    }
    QMessageBox::warning(this, QStringLiteral("疑似勒索批量写入"),
                         QStringLiteral("工作区检测到异常批量写入，已自动建立紧急快照锚点。\n\n%1\n\n可先关闭外部编辑器，再通过「恢复历史版本」或「恢复到锚点」回退。").arg(alert.reason));
}

void MainWindow::rollbackToSnapshot() {
    const auto snapshots = library_.snapshots();
    if (snapshots.isEmpty()) {
        statusBar()->showMessage(QStringLiteral("还没有紧急快照锚点"));
        return;
    }
    QStringList labels;
    for (const auto &snapshot : snapshots)
        labels << QStringLiteral("#%1 %2 · %3 个素材")
                      .arg(snapshot.id)
                      .arg(snapshot.created)
                      .arg(snapshot.assets);
    bool ok = false;
    const auto choice = QInputDialog::getItem(this, QStringLiteral("恢复到锚点"),
                                              QStringLiteral("选择感染前锚点（回滚形成新版本，不销毁后续历史）"), labels, 0, false, &ok);
    if (!ok) return;
    const int index = labels.indexOf(choice);
    if (index < 0) return;
    const qint64 snapshotId = snapshots.at(index).id;
    if (QMessageBox::question(this, QStringLiteral("恢复到锚点"),
                              QStringLiteral("将整个工作区恢复到该锚点。\n未记录的当前内容会先保存，后续历史（含被加密版本）保留。\n请先关闭所有外部编辑器。")) != QMessageBox::Yes) return;
    try {
        const int restored = library_.rollbackAll(snapshotId);
        reloadAssets();
        // Re-arm the sentinel: rollback is an app-initiated write, not ransomware.
        if (sentinel_) sentinel_->resetBaseline();
        statusBar()->showMessage(QStringLiteral("已恢复到锚点 #%1（%2 个素材回滚）").arg(snapshotId).arg(restored));
    } catch (const std::exception &error) { report(error); }
}

void MainWindow::browseTimePoint() {
    const auto snapshots = library_.snapshots();
    if (snapshots.isEmpty()) {
        statusBar()->showMessage(QStringLiteral("还没有快照可浏览"));
        return;
    }
    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("浏览时间点（只读）"));
    dialog.resize(760, 520);
    auto *layout = new QVBoxLayout(&dialog);
    auto *title = new QLabel(QStringLiteral("时间点目录浏览"));
    title->setStyleSheet("font-size:16px;font-weight:600;");
    auto *note = new QLabel(QStringLiteral("只读：切换时间点不写回。目录树按该快照的完整清单显示，不混用当前索引；"
                                          "删除/缺失/排除状态以当时为准。"));
    note->setWordWrap(true);
    auto *combo = new QComboBox;
    for (const auto &snapshot : snapshots)
        combo->addItem(QStringLiteral("#%1 %2 · %3 个素材 · %4")
                           .arg(snapshot.id).arg(snapshot.created)
                           .arg(snapshot.assets).arg(snapshot.note), snapshot.id);
    auto *table = new QTableWidget(0, 4);
    table->setHorizontalHeaderLabels({QStringLiteral("路径"), QStringLiteral("名称"),
                                      QStringLiteral("状态"), QStringLiteral("备注")});
    table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    table->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    layout->addWidget(title);
    layout->addWidget(note);
    layout->addWidget(combo);
    layout->addWidget(table, 1);
    const auto fill = [&](int index) {
        if (index < 0 || index >= snapshots.size()) return;
        const qint64 snapshotId = snapshots.at(index).id;
        const auto manifest = library_.snapshotManifest(snapshotId);
        const auto exclusions = library_.snapshotExclusions(snapshotId);
        table->setRowCount(manifest.size() + exclusions.size());
        int row = 0;
        for (const auto &entry : manifest) {
            QString state;
            if (entry.state == QStringLiteral("present")) state = QStringLiteral("存在");
            else if (entry.state == QStringLiteral("tombstone")) state = QStringLiteral("已删除（墓碑保留历史）");
            else if (entry.state == QStringLiteral("missing")) state = QStringLiteral("缺失（工作副本不在）");
            else state = entry.state;
            table->setItem(row, 0, new QTableWidgetItem(entry.path));
            table->setItem(row, 1, new QTableWidgetItem(QFileInfo(entry.path).fileName()));
            table->setItem(row, 2, new QTableWidgetItem(state));
            table->setItem(row, 3, new QTableWidgetItem(QStringLiteral("版本 #%1").arg(entry.revisionId)));
            table->item(row, 0)->setToolTip(entry.assetId);
            ++row;
        }
        for (const auto &exclusion : exclusions) {
            table->setItem(row, 0, new QTableWidgetItem(exclusion.path));
            table->setItem(row, 1, new QTableWidgetItem(QFileInfo(exclusion.path).fileName()));
            table->setItem(row, 2, new QTableWidgetItem(QStringLiteral("已排除")));
            table->setItem(row, 3, new QTableWidgetItem(exclusion.reason));
            table->item(row, 0)->setToolTip(exclusion.assetId);
            ++row;
        }
    };
    connect(combo, QOverload<int>::of(&QComboBox::currentIndexChanged), &dialog, fill);
    fill(combo->currentIndex());
    dialog.exec();
}

void MainWindow::resizeEvent(QResizeEvent *event) { QMainWindow::resizeEvent(event); showRevision(); }
void MainWindow::saveScreenshot(const QString &path) { grab().save(path); }
