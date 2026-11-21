// MainWindow shell: construction and the full UI layout, asset filtering
// and view modes, the asset list reload, zoom controls and the shared
// error reporter (LAY-003 split; versions, metadata, snapshots, backup
// and mounted-source flows live in their own mainwindow_*.cpp files).
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


MainWindow::MainWindow(Library &library) : library_(library), thumbnails_(library.root()) {
    setWindowTitle(tr("pentimento · 画迹"));
    resize(1240, 790);
    setMinimumSize(860, 540);
    auto *toolbar = addToolBar(tr("素材操作"));
    toolbar->setMovable(false);
    auto *brand = new QLabel(QStringLiteral("pentimento"));
    brand->setStyleSheet("font-weight:600;padding:8px;");
    toolbar->addWidget(brand);
    auto *importAction = toolbar->addAction(tr("导入素材"), this, &MainWindow::importFiles);
    importAction->setShortcut(QKeySequence("Ctrl+O"));
    toolbar->addAction(tr("记录版本"), this, &MainWindow::capture)->setShortcut(QKeySequence("Ctrl+S"));
    toolbar->addAction(tr("编辑工作副本"), this, [this] {
        const auto asset = selected();
        if (!asset.id.isEmpty() && !QDesktopServices::openUrl(QUrl::fromLocalFile(asset.path)))
            statusBar()->showMessage(tr("无法打开默认编辑器；可通过素材库目录打开工作副本"));
    });
    toolbar->addAction(tr("打开素材库"), this, [this] {
        QDesktopServices::openUrl(QUrl::fromLocalFile(library_.root()));
    });
    // P-208: grid/list are two render modes of one shared model; switching keeps
    // the stable-id selection and scroll position.
    auto *viewGroup = new QActionGroup(this);
    auto *gridAction = new QAction(tr("网格视图"), this);
    gridAction->setCheckable(true); gridAction->setChecked(true);
    auto *listAction = new QAction(tr("列表视图"), this);
    listAction->setCheckable(true);
    viewGroup->addAction(gridAction); viewGroup->addAction(listAction);
    toolbar->addSeparator();
    toolbar->addAction(gridAction);
    toolbar->addAction(listAction);
    connect(gridAction, &QAction::triggered, this, [this] { setAssetViewMode(true); });
    connect(listAction, &QAction::triggered, this, [this] { setAssetViewMode(false); });
    // Anti-ransomware Layer 3: one-click rollback to a pre-infection anchor.
    toolbar->addAction(tr("恢复到锚点"), this, &MainWindow::rollbackToSnapshot);
    // P-312: read-only point-in-time directory browse of a snapshot manifest.
    toolbar->addAction(tr("浏览快照…"), this, &MainWindow::browseSnapshot);
    // P-513: mounted-source reconnection state (connect/online/offline/failed)
    // with a user-paced retry; the probe always runs off the UI thread.
    toolbar->addAction(tr("挂载来源状态…"), this, &MainWindow::showSourceState);
    // UI audit (2026-11-03): at the documented minimum window the 16-action
    // toolbar overflowed by ~613px, hiding 9 actions (incl. the core security
    // "恢复到锚点") behind the "≫" button. The four infrequent maintenance
    // actions move into a "更多" menu so the always-visible actions (import,
    // record, edit, open, grid/list, batch tags, restore, snapshot browse,
    // source state) fit and stay reachable without a second click.
    auto *moreMenu = new QMenu(tr("更多"), this);
    // P-213: batch tags apply to the visible (filtered) set explicitly, with a
    // count preview before anything changes.
    moreMenu->addAction(tr("为筛选结果添加标签…"), this, &MainWindow::batchAddTag);
    moreMenu->addAction(tr("从筛选结果移除标签…"), this, &MainWindow::batchRemoveTag);
    moreMenu->addAction(tr("索引外部目录…"), this, &MainWindow::indexExternalDirectory);
    moreMenu->addAction(tr("导出备份包…"), this, &MainWindow::exportBackup);
    moreMenu->addAction(tr("一致性检查…"), this, &MainWindow::checkConsistency);
    moreMenu->addAction(tr("空间预演…"), this, &MainWindow::showSpacePreview);
    moreMenu->addAction(tr("恢复演练…"), this, &MainWindow::restoreBackupDrill);
    auto *moreAction = toolbar->addAction(QIcon(), tr("更多"));
    moreAction->setMenu(moreMenu);
    auto *splitter = new QSplitter;
    auto *left = new QWidget;
    auto *leftLayout = new QVBoxLayout(left);
    auto *libraryTitle = new QLabel(tr("素材库"));
    libraryTitle->setStyleSheet("font-size:16px;font-weight:600;");
    search_ = new QLineEdit;
    search_->setPlaceholderText(tr("搜索素材名称"));
    search_->setAccessibleName(tr("搜索素材"));
    assetsList_ = new QListWidget;
    assetsList_->setAccessibleName(tr("素材列表"));
    assetsList_->setIconSize(QSize(48, 48));
    assetsList_->setSpacing(5);
    setAssetViewMode(true); // default: grid (one shared model, two render modes)
    leftLayout->addWidget(libraryTitle);
    leftLayout->addWidget(search_);
    // P-217: combined filter — tags (any-of), format, minimum size, rating —
    // ANDed with the name search. Conditions are always visible in a summary;
    // one click clears everything back to the full list.
    auto *filterTitle = new QLabel(tr("筛选"));
    filterTitle->setStyleSheet("font-size:14px;font-weight:600;");
    tagFilterEdit_ = new QLineEdit;
    tagFilterEdit_->setPlaceholderText(tr("标签（逗号分隔，任一命中）"));
    tagFilterEdit_->setAccessibleName(tr("标签筛选"));
    formatFilterCombo_ = new QComboBox;
    formatFilterCombo_->setAccessibleName(tr("格式筛选"));
    formatFilterCombo_->addItem(tr("全部格式"), QString());
    for (const QString &format : {QStringLiteral("PNG"), QStringLiteral("JPEG"), QStringLiteral("WebP"),
                                  QStringLiteral("TGA"), QStringLiteral("BMP"), QStringLiteral("PSD")})
        formatFilterCombo_->addItem(format, format);
    auto *sizeRow = new QHBoxLayout;
    minWidthSpin_ = new QSpinBox;
    minWidthSpin_->setAccessibleName(tr("最小宽度"));
    minWidthSpin_->setRange(0, 100000);
    minWidthSpin_->setSpecialValueText(tr("宽度不限"));
    minHeightSpin_ = new QSpinBox;
    minHeightSpin_->setAccessibleName(tr("最小高度"));
    minHeightSpin_->setRange(0, 100000);
    minHeightSpin_->setSpecialValueText(tr("高度不限"));
    sizeRow->addWidget(minWidthSpin_);
    sizeRow->addWidget(new QLabel("×"));
    sizeRow->addWidget(minHeightSpin_);
    ratingFilterCombo_ = new QComboBox;
    ratingFilterCombo_->setAccessibleName(tr("评分筛选"));
    ratingFilterCombo_->addItem(tr("全部评分"), -2);
    ratingFilterCombo_->addItem(tr("无评分"), -1);
    for (int stars = 1; stars <= 5; ++stars)
        ratingFilterCombo_->addItem(tr("≥ %1 星").arg(stars), stars);
    clearFilterBtn_ = new QPushButton(tr("清除条件"));
    filterSummary_ = new QLabel;
    filterSummary_->setStyleSheet("color:#888;");
    filterSummary_->setWordWrap(true);
    leftLayout->addWidget(filterTitle);
    leftLayout->addWidget(tagFilterEdit_);
    leftLayout->addWidget(formatFilterCombo_);
    leftLayout->addLayout(sizeRow);
    leftLayout->addWidget(ratingFilterCombo_);
    leftLayout->addWidget(clearFilterBtn_);
    leftLayout->addWidget(filterSummary_);
    leftLayout->addWidget(assetsList_);
    // P-213: tags of the selected asset, add/remove one at a time; the result is
    // immediately queryable in this list (backed by library.tags()).
    auto *tagsTitle = new QLabel(tr("标签"));
    tagsTitle->setStyleSheet("font-size:14px;font-weight:600;");
    tagsList_ = new QListWidget;
    tagsList_->setAccessibleName(tr("标签列表"));
    tagsList_->setMaximumHeight(110);
    tagsList_->setViewMode(QListView::ListMode);
    auto *tagRow = new QHBoxLayout;
    tagInput_ = new QLineEdit;
    tagInput_->setPlaceholderText(tr("新标签"));
    tagInput_->setAccessibleName(tr("新标签"));
    tagAddBtn_ = new QPushButton(tr("添加"));
    tagRemoveBtn_ = new QPushButton(tr("移除"));
    tagRow->addWidget(tagInput_, 1);
    tagRow->addWidget(tagAddBtn_);
    tagRow->addWidget(tagRemoveBtn_);
    leftLayout->addWidget(tagsTitle);
    leftLayout->addWidget(tagsList_);
    leftLayout->addLayout(tagRow);
    // P-214: rating (0-5 stars with a distinct "no rating") and a free-form
    // note. The note saves on an explicit button or on focus loss; every save
    // shows a clear "saved" message so it is never assumed silently kept.
    auto *ratingTitle = new QLabel(tr("评分"));
    ratingTitle->setStyleSheet("font-size:14px;font-weight:600;");
    ratingCombo_ = new QComboBox;
    ratingCombo_->setAccessibleName(tr("评分"));
    ratingCombo_->addItem(tr("无评分"), -1);
    for (int stars = 0; stars <= 5; ++stars)
        ratingCombo_->addItem(tr("%1 星").arg(stars), stars);
    auto *noteTitle = new QLabel(tr("备注"));
    noteTitle->setStyleSheet("font-size:14px;font-weight:600;");
    noteEdit_ = new QPlainTextEdit;
    noteEdit_->setAccessibleName(tr("备注"));
    noteEdit_->setPlaceholderText(tr("备注将在失焦或点击保存时写入"));
    noteEdit_->setMaximumHeight(72);
    noteSaveBtn_ = new QPushButton(tr("保存备注"));
    auto *noteRow = new QHBoxLayout;
    noteRow->addStretch(1);
    noteRow->addWidget(noteSaveBtn_);
    leftLayout->addWidget(ratingTitle);
    leftLayout->addWidget(ratingCombo_);
    leftLayout->addWidget(noteTitle);
    leftLayout->addWidget(noteEdit_);
    leftLayout->addLayout(noteRow);
    auto *center = new QWidget;
    auto *centerLayout = new QVBoxLayout(center);
    details_ = new QLabel(tr("导入第一份素材，开始保留创作历史。"));
    details_->setWordWrap(true);
    details_->setTextFormat(Qt::PlainText);
    // P-209: preview = control bar (fit/100%/zoom/transparency) + a canvas in a
    // scroll area; the message label swaps in for corrupt/unsupported formats.
    auto *previewBox = new QWidget;
    auto *previewLayout = new QVBoxLayout(previewBox);
    previewLayout->setContentsMargins(0, 0, 0, 0);
    auto *controls = new QHBoxLayout;
    fitBtn_ = new QToolButton;
    fitBtn_->setText(tr("适应窗口"));
    fitBtn_->setCheckable(true);
    fitBtn_->setChecked(true);
    hundredBtn_ = new QToolButton;
    hundredBtn_->setText(tr("100%"));
    hundredBtn_->setCheckable(true);
    zoomOutBtn_ = new QToolButton;
    zoomOutBtn_->setText(QStringLiteral("−"));
    zoomInBtn_ = new QToolButton;
    zoomInBtn_->setText(QStringLiteral("＋"));
    zoomLabel_ = new QLabel;
    checkerCheck_ = new QCheckBox(tr("透明底"));
    checkerCheck_->setChecked(true);
    controls->addWidget(fitBtn_);
    controls->addWidget(hundredBtn_);
    controls->addSpacing(8);
    controls->addWidget(zoomOutBtn_);
    controls->addWidget(zoomInBtn_);
    controls->addWidget(zoomLabel_);
    controls->addSpacing(8);
    controls->addWidget(checkerCheck_);
    controls->addStretch(1);
    previewLayout->addLayout(controls);
    canvas_ = new PreviewCanvas;
    previewScroll_ = new QScrollArea;
    previewScroll_->setWidgetResizable(true);
    previewScroll_->setFrameShape(QFrame::NoFrame);
    previewScroll_->setWidget(canvas_);
    preview_ = new QLabel(tr("素材预览\n\n支持 PNG、JPEG、WebP 等已安装的 Qt 图像格式\n其他文件保留原始内容，暂不提供画面预览"));
    preview_->setAlignment(Qt::AlignCenter);
    preview_->setMinimumSize(240, 200);
    preview_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
    preview_->setStyleSheet("background:#e5e5e2;color:#444845;border-radius:4px;padding:16px;");
    previewStack_ = new QStackedWidget;
    previewStack_->addWidget(previewScroll_); // index 0: image canvas
    previewStack_->addWidget(preview_);       // index 1: message
    previewLayout->addWidget(previewStack_, 1);
    centerLayout->addWidget(details_);
    centerLayout->addWidget(previewBox, 1);
    connect(fitBtn_, &QToolButton::clicked, this, [this] {
        canvas_->setFit(true);
        syncZoomButtons(true);
    });
    connect(hundredBtn_, &QToolButton::clicked, this, [this] {
        canvas_->setZoom(1.0);
        syncZoomButtons(false);
    });
    connect(zoomInBtn_, &QToolButton::clicked, this, [this] {
        canvas_->setZoom(canvas_->effectiveZoom() * 1.25);
        syncZoomButtons(false);
    });
    connect(zoomOutBtn_, &QToolButton::clicked, this, [this] {
        canvas_->setZoom(canvas_->effectiveZoom() / 1.25);
        syncZoomButtons(false);
    });
    connect(checkerCheck_, &QCheckBox::toggled, canvas_, &PreviewCanvas::setCheckerboard);
    connect(canvas_, &PreviewCanvas::zoomChanged, this, [this](qreal) { updateZoomLabel(); });
    auto *right = new QWidget;
    auto *rightLayout = new QVBoxLayout(right);
    auto *historyTitle = new QLabel(tr("版本时间轴"));
    historyTitle->setStyleSheet("font-size:16px;font-weight:600;");
    auto *hint = new QLabel(tr("选择历史版本预览\n恢复会新增版本，原有历史始终保留。"));
    hint->setWordWrap(true);
    historyList_ = new QListWidget;
    historyList_->setAccessibleName(tr("历史版本"));
    historyList_->setIconSize(QSize(48, 48));
    historyList_->setSpacing(5);
    // UI audit (2026-11-03): at the documented minimum window (860x540) the
    // right column was compressed to a ~15px list and ~14px buttons (below
    // their 24px minimumSizeHint) because the vertical layout had no scroll
    // release. A floor on the timeline keeps the version list readable; the
    // enclosing scroll area releases the overflow instead of clipping.
    historyList_->setMinimumHeight(120);
    restore_ = new QPushButton(tr("恢复此版本"));
    export_ = new QPushButton(tr("导出此版本…"));
    // P-313: side-by-side comparison of the selected history revision against
    // the newest (current) version, under one shared zoom/pan transform.
    compareBtn_ = new QPushButton(tr("对比版本…"));
    connect(compareBtn_, &QPushButton::clicked, this, &MainWindow::compareVersions);
    rightLayout->addWidget(historyTitle);
    rightLayout->addWidget(hint);
    rightLayout->addWidget(historyList_, 1);
    rightLayout->addWidget(restore_);
    rightLayout->addWidget(export_);
    rightLayout->addWidget(compareBtn_);
    // P-215: manual source/author/license records. Entries are appended (never
    // overwritten) and each keeps its recorded time; old entries stay viewable.
    // The license is recorded exactly as typed — the app never asserts that a
    // URL or an attached file means a license was granted.
    auto *provTitle = new QLabel(tr("来源与授权"));
    provTitle->setStyleSheet("font-size:14px;font-weight:600;");
    sourceInput_ = new QLineEdit;
    sourceInput_->setPlaceholderText(tr("来源（网址或出处，可留空）"));
    sourceInput_->setAccessibleName(tr("来源"));
    authorInput_ = new QLineEdit;
    authorInput_->setPlaceholderText(tr("作者（可留空）"));
    authorInput_->setAccessibleName(tr("作者"));
    licenseInput_ = new QLineEdit;
    licenseInput_->setPlaceholderText(tr("许可（如实填写；应用不代为判断）"));
    licenseInput_->setAccessibleName(tr("许可"));
    attachEvidenceBtn_ = new QPushButton(tr("附加授权证据…"));
    recordProvBtn_ = new QPushButton(tr("记录"));
    evidenceLabel_ = new QLabel(tr("未附加证据"));
    evidenceLabel_->setStyleSheet("color:#888;");
    auto *provBtnRow = new QHBoxLayout;
    provBtnRow->addWidget(attachEvidenceBtn_, 1);
    provBtnRow->addWidget(recordProvBtn_);
    auto *provListTitle = new QLabel(tr("记录（旧记录可回看）"));
    provListTitle->setStyleSheet("color:#888;");
    provenanceList_ = new QListWidget;
    provenanceList_->setMinimumHeight(60);
    provenanceList_->setMaximumHeight(96);
    provenanceList_->setWordWrap(true); // a record line stays fully readable
    exportEvidenceBtn_ = new QPushButton(tr("导出证据…"));
    exportEvidenceBtn_->setEnabled(false);
    rightLayout->addWidget(provTitle);
    rightLayout->addWidget(sourceInput_);
    rightLayout->addWidget(authorInput_);
    rightLayout->addWidget(licenseInput_);
    rightLayout->addLayout(provBtnRow);
    rightLayout->addWidget(evidenceLabel_);
    rightLayout->addWidget(provListTitle);
    rightLayout->addWidget(provenanceList_);
    rightLayout->addWidget(exportEvidenceBtn_);
    // P-305: scheduled whole-tree snapshots. Off by default; a period of
    // 15/30/60 minutes schedules the next run from the enable point; pause
    // records its reason (user or system) and stops auto snapshots; resuming
    // restarts the clock. Runtime-only: leaving the app is never counted as
    // continuous backup, so the note says so explicitly.
    auto *snapTitle = new QLabel(tr("定时快照"));
    snapTitle->setStyleSheet("font-size:14px;font-weight:600;");
    snapshotPeriodCombo_ = new QComboBox;
    snapshotPeriodCombo_->setAccessibleName(tr("定时快照周期"));
    snapshotPeriodCombo_->addItem(tr("关闭"), 0);
    snapshotPeriodCombo_->addItem(tr("每 15 分钟"), 15);
    snapshotPeriodCombo_->addItem(tr("每 30 分钟"), 30);
    snapshotPeriodCombo_->addItem(tr("每 60 分钟"), 60);
    snapshotStatus_ = new QLabel(tr("定时快照未启用"));
    snapshotStatus_->setWordWrap(true);
    snapshotStatus_->setStyleSheet("color:#888;");
    snapshotPauseBtn_ = new QPushButton(tr("暂停"));
    snapshotPauseBtn_->setEnabled(false);
    auto *snapNote = new QLabel(tr("仅应用运行时生效；退出应用期间不算持续备份。"));
    snapNote->setWordWrap(true);
    snapNote->setStyleSheet("color:#888;");
    // P-306: exclusion rules keep matching assets out of snapshots taken after
    // the rule exists; history is kept and an exclusion is never a deletion.
    exclusionBtn_ = new QPushButton(tr("排除规则…"));
    // P-309: read-only per-field metadata history (old values, read-only).
    fieldHistoryBtn_ = new QPushButton(tr("字段历史…"));
    rightLayout->addWidget(snapTitle);
    rightLayout->addWidget(snapshotPeriodCombo_);
    rightLayout->addWidget(snapshotStatus_);
    rightLayout->addWidget(snapshotPauseBtn_);
    rightLayout->addWidget(snapNote);
    rightLayout->addWidget(exclusionBtn_);
    rightLayout->addWidget(fieldHistoryBtn_);
    // UI audit (2026-11-03): the right column (timeline + provenance + snapshot
    // controls) is wrapped in a scroll area so a small window scrolls the
    // column instead of crushing every control below its minimum height.
    auto *rightScroll = new QScrollArea;
    rightScroll->setWidgetResizable(true);
    rightScroll->setWidget(right);
    rightScroll->setFrameShape(QFrame::NoFrame);
    splitter->addWidget(left); splitter->addWidget(center); splitter->addWidget(rightScroll);
    splitter->setSizes({260, 660, 300});
    setCentralWidget(splitter);
    // P-217: every filter widget (name included) re-applies the combined filter.
    connect(search_, &QLineEdit::textChanged, this, &MainWindow::applyFilter);
    connect(tagFilterEdit_, &QLineEdit::textChanged, this, &MainWindow::applyFilter);
    connect(formatFilterCombo_, &QComboBox::currentIndexChanged, this, &MainWindow::applyFilter);
    connect(minWidthSpin_, &QSpinBox::valueChanged, this, &MainWindow::applyFilter);
    connect(minHeightSpin_, &QSpinBox::valueChanged, this, &MainWindow::applyFilter);
    connect(ratingFilterCombo_, &QComboBox::currentIndexChanged, this, &MainWindow::applyFilter);
    connect(clearFilterBtn_, &QPushButton::clicked, this, &MainWindow::clearFilter);
    connect(assetsList_, &QListWidget::currentRowChanged, this, &MainWindow::reloadHistory);
    // P-216: rename / in-library move on the selected asset (identity preserved).
    assetsList_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(assetsList_, &QListWidget::customContextMenuRequested, this, [this](const QPoint &pos) {
        if (selected().id.isEmpty()) return;
        QMenu menu;
        menu.addAction(tr("重命名…"), this, &MainWindow::renameSelected);
        menu.addAction(tr("移动到文件夹…"), this, &MainWindow::moveSelected);
        menu.addSeparator();
        // P-310: in-app confirmed deletion (preview → confirm). Writes a
        // tombstone revision — history and objects are retained — and removes
        // the working copy.
        menu.addAction(tr("删除素材…"), this, &MainWindow::deleteSelected);
        menu.exec(assetsList_->viewport()->mapToGlobal(pos));
    });
    connect(historyList_, &QListWidget::currentRowChanged, this, &MainWindow::showRevision);
    // P-307: pin a key version (with a reason) or unpin it from the timeline's
    // context menu. A pin persists across restarts and exempts the version from
    // any future retention cleanup; unpinning never deletes history.
    historyList_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(historyList_, &QListWidget::customContextMenuRequested, this, [this](const QPoint &pos) {
        const int row = historyList_->currentRow();
        if (row < 0 || row >= revisions_.size()) return;
        const auto revision = revisions_.at(row);
        QMenu menu;
        if (revision.pinned) {
            menu.addAction(tr("取消固定"), this, [this, revision] {
                try {
                    library_.unpinRevision(revision.id);
                    statusBar()->showMessage(tr("已取消固定版本 #%1（历史保留）").arg(revision.id));
                    reloadHistory();
                } catch (const std::exception &e) { report(e); }
            });
        } else {
            menu.addAction(tr("固定此版本…"), this, [this, revision] {
                bool ok = false;
                const QString reason = QInputDialog::getText(this, tr("固定关键版本"),
                    tr("固定原因（固定版本不会被保留策略清理）"), QLineEdit::Normal, QString(), &ok);
                if (!ok) return;
                try {
                    library_.pinRevision(revision.id, reason);
                    statusBar()->showMessage(tr("已固定版本 #%1").arg(revision.id));
                    reloadHistory();
                } catch (const std::exception &e) { report(e); }
            });
        }
        menu.exec(historyList_->viewport()->mapToGlobal(pos));
    });
    connect(tagAddBtn_, &QPushButton::clicked, this, &MainWindow::addTagToSelected);
    connect(tagInput_, &QLineEdit::returnPressed, this, &MainWindow::addTagToSelected);
    connect(tagRemoveBtn_, &QPushButton::clicked, this, &MainWindow::removeSelectedTag);
    // P-214: rating saves on change; the note saves on the button or on focus
    // loss (the editor leaving focus flushes pending edits).
    connect(ratingCombo_, &QComboBox::currentIndexChanged, this, &MainWindow::applyRating);
    connect(noteSaveBtn_, &QPushButton::clicked, this, &MainWindow::saveNote);
    connect(qApp, &QApplication::focusChanged, this, [this](QWidget *oldFocus, QWidget *) {
        if (oldFocus == noteEdit_) saveNote();
    });
    // P-215: record the manual entry, attach an evidence file, or export the
    // evidence of the selected record.
    connect(recordProvBtn_, &QPushButton::clicked, this, &MainWindow::recordProvenance);
    connect(attachEvidenceBtn_, &QPushButton::clicked, this, &MainWindow::attachEvidence);
    connect(exportEvidenceBtn_, &QPushButton::clicked, this, &MainWindow::exportEvidenceFile);
    connect(provenanceList_, &QListWidget::currentRowChanged, this, [this](int row) {
        exportEvidenceBtn_->setEnabled(row >= 0 && !provenanceList_->item(row)->data(Qt::UserRole).toString().isEmpty());
    });
    connect(restore_, &QPushButton::clicked, this, &MainWindow::restore);
    connect(export_, &QPushButton::clicked, this, &MainWindow::exportFile);
    // P-305: period selection (default off), pause/resume with a recorded
    // reason, and a system pause while the app is out of the foreground —
    // resuming on return only lifts a system pause, never a user pause.
    connect(snapshotPeriodCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &MainWindow::setSnapshotPeriod);
    connect(snapshotPauseBtn_, &QPushButton::clicked, this, &MainWindow::toggleSnapshotPause);
    connect(exclusionBtn_, &QPushButton::clicked, this, &MainWindow::manageExclusions);
    connect(fieldHistoryBtn_, &QPushButton::clicked, this, &MainWindow::showFieldHistory);
    connect(qApp, &QApplication::applicationStateChanged, this, [this](Qt::ApplicationState state) {
        if (!snapshotSchedule_.enabled()) return;
        if (state != Qt::ApplicationActive) {
            if (!snapshotSchedule_.paused()) {
                snapshotSchedule_.pauseBySystem(tr("系统（应用退到后台）"));
                snapshotPauseBtn_->setText(tr("恢复"));
                updateSnapshotStatus();
            }
        } else if (snapshotSchedule_.paused() && snapshotSchedule_.pausedBySystem()) {
            snapshotSchedule_.resume(QDateTime::currentDateTime());
            snapshotPauseBtn_->setText(tr("暂停"));
            updateSnapshotStatus();
        }
    });
    // P-304: the stable-change capture runs on a serial background worker so a
    // large file never blocks the UI thread. The Library serializes its own
    // access (one internal mutex), so the worker's captureIfStable is safe
    // alongside the UI thread's own library calls.
    captureQueue_ = std::make_unique<TaskQueue>(this);
    // Audit (2026-11-04): history integrity verification runs on its own serial
    // worker so a UI refresh over many large revisions never hashes every
    // object on the UI thread. Results are cached by content hash; a second
    // refresh over the same history costs no re-verify.
    integrityQueue_ = std::make_unique<TaskQueue>(this);
    connect(integrityQueue_.get(), &TaskQueue::taskFinished, this,
            [this](qint64, bool, const QString &) {
                integrityInFlight_ = false;
                // A later selection may have queued more revisions while this
                // batch ran; verify them too (one batch at a time).
                requestIntegrity(revisions_);
                rebuildHistory();
            });
    // Audit (2026-11-04): the backup-target probe is a stat + QStorageInfo
    // query that can block on a dead/NFS/CIFS mount, so it runs on its own
    // background worker. Keeping it as a MainWindow member means closing the
    // source-state dialog never destroys the queue (and never waits on a
    // worker that may still be blocked in statvfs). blockOnShutdown=false
    // detaches the worker at MainWindow destruction, so a probe still blocked
    // in statvfs never freezes the UI thread even at application exit.
    targetQueue_ = std::make_unique<TaskQueue>(this, /*blockOnShutdown=*/false);
    auto *timer = new QTimer(this);
    connect(timer, &QTimer::timeout, this, &MainWindow::scanChanges);
    timer->start(2000);
    // Anti-ransomware Layer 2: watch the workspace for abnormal batch writes.
    sentinel_ = std::make_unique<Sentinel>(library_.root() + QStringLiteral("/working"));
    sentinel_->resetBaseline();
    auto *sentinelTimer = new QTimer(this);
    connect(sentinelTimer, &QTimer::timeout, this, &MainWindow::pollSentinel);
    sentinelTimer->start(5000);
    reloadAssets();
    statusBar()->showMessage(tr("本地素材库：%1 · 编辑工作副本后自动记录稳定变化").arg(library_.root()));
}

Asset MainWindow::selected() const {
    const int row = assetsList_->currentRow();
    return row >= 0 && row < assets_.size() ? assets_.at(row) : Asset{};
}

// P-217: read the filter widgets into an AssetFilter. Tags are comma-separated
// (any-of); format is the combo's stored value; rating maps the combo (-2 none,
// -1 no-rating, 1..5 >= N); size is the two spin floors (0 = no condition).
AssetFilter MainWindow::currentFilter() const {
    AssetFilter filter;
    filter.name = search_->text().trimmed();
    const QStringList tags = tagFilterEdit_->text().split(',', Qt::SkipEmptyParts);
    for (const QString &tag : tags) filter.tags << tag.trimmed();
    filter.format = formatFilterCombo_->currentData().toString();
    filter.minWidth = minWidthSpin_->value();
    filter.minHeight = minHeightSpin_->value();
    filter.minRating = ratingFilterCombo_->currentData().toInt();
    return filter;
}

void MainWindow::updateFilterSummary(const AssetFilter &filter) {
    QStringList parts;
    if (!filter.name.isEmpty()) parts << tr("名称:%1").arg(filter.name);
    if (!filter.tags.isEmpty()) parts << tr("标签:%1").arg(filter.tags.join(','));
    if (!filter.format.isEmpty()) parts << tr("格式:%1").arg(filter.format);
    if (filter.minWidth > 0 || filter.minHeight > 0)
        parts << tr("尺寸:≥%1×%2").arg(filter.minWidth).arg(filter.minHeight);
    if (filter.minRating == -1) parts << tr("评分:无评分");
    else if (filter.minRating >= 1) parts << tr("评分:≥%1 星").arg(filter.minRating);
    filterSummary_->setText(parts.isEmpty() ? tr("无生效条件") : parts.join(" · "));
}

// Re-apply the filter over the cached facts: hides non-matching items without
// touching the database. Keeps the selection on a visible item (P-208).
void MainWindow::applyFilter() {
    const AssetFilter filter = currentFilter();
    const QString selectedId = selected().id;
    for (int i = 0; i < assetsList_->count() && i < assets_.size(); ++i) {
        const auto &asset = assets_.at(i);
        assetsList_->item(i)->setHidden(!filterMatches(facts_.value(asset.id), filter));
    }
    updateFilterSummary(filter);
    if (!selectedId.isEmpty()) {
        bool found = false;
        for (int i = 0; i < assetsList_->count(); ++i)
            if (!assetsList_->item(i)->isHidden() && assets_.value(i).id == selectedId) {
                assetsList_->setCurrentRow(i); found = true; break;
            }
        if (!found) keepSelectionVisible();
    } else keepSelectionVisible();
}

void MainWindow::clearFilter() {
    search_->clear();
    tagFilterEdit_->clear();
    formatFilterCombo_->setCurrentIndex(0);
    minWidthSpin_->setValue(0);
    minHeightSpin_->setValue(0);
    ratingFilterCombo_->setCurrentIndex(0);
    applyFilter(); // reset widgets to defaults, then re-show everything
}

void MainWindow::report(const std::exception &error) {
    QMessageBox::warning(this, tr("操作未完成"), QString::fromUtf8(error.what()));
}

void MainWindow::reloadAssets(const QString &selectId) {
    assets_ = library_.assets();
    // P-217: cache the filter facts once per reload (tags/format/size/rating),
    // so re-applying the filter on any widget change is pure and DB-free.
    facts_.clear();
    for (const auto &asset : assets_) {
        AssetFacts facts;
        facts.name = asset.name;
        facts.folder = asset.folder;
        facts.tags = library_.tags(asset.id);
        facts.format = library_.assetFormat(asset.id);
        const QSize size = library_.assetSize(asset.id);
        facts.width = size.width();
        facts.height = size.height();
        facts.rating = library_.rating(asset.id);
        facts_.insert(asset.id, facts);
    }
    assetsList_->blockSignals(true);
    assetsList_->clear();
    // Selection is restored by stable asset id (P-208), never by row or filename.
    int selection = -1;
    for (int i = 0; i < assets_.size(); ++i) {
        const auto &asset = assets_.at(i);
        // P-216: the label is the user-visible location (folder/name), so an
        // in-library move is immediately visible; the tooltip keeps the physical
        // working-copy path for inspection.
        auto *item = new QListWidgetItem(asset.logicalPath(), assetsList_);
        item->setToolTip(asset.logicalPath() + QStringLiteral("\n") + asset.path);
        item->setData(Qt::UserRole, asset.id);
        QIcon icon;
        try {
            const auto revisions = library_.revisions(asset.id);
            if (!revisions.isEmpty()) {
                const QImage thumb = cachedThumbnail(revisions.first().hash, asset.name, QSize(80, 80));
                if (!thumb.isNull()) icon = QPixmap::fromImage(thumb);
            }
        } catch (const std::exception &) { /* corrupt object: keep placeholder */ }
        if (!icon.isNull()) item->setIcon(icon);
        item->setHidden(!filterMatches(facts_.value(asset.id), currentFilter()));
        if (!asset.id.isEmpty() && asset.id == selectId) selection = i;
    }
    // If the requested id (or any item) is filtered out, focus the nearest visible item.
    if (selection >= 0 && !assetsList_->item(selection)->isHidden())
        assetsList_->setCurrentRow(selection);
    else keepSelectionVisible();
    assetsList_->blockSignals(false);
    updateFilterSummary(currentFilter());
    reloadHistory();
}

QImage MainWindow::cachedThumbnail(const QString &hash, const QString &name, const QSize &size) {
    const int version = ThumbnailCache::decoderVersion();
    // Serve from cache first: no full-object read on every refresh (P-207).
    QImage cached = thumbnails_.find(hash, size, version);
    if (!cached.isNull()) return cached;
    // Cache miss: read and verify the object bytes, then decode/cache.
    const auto bytes = library_.readObject(hash);
    return thumbnailFor(thumbnails_, bytes, hash, size,
                        QFileInfo(name).suffix().toLower());
}

void MainWindow::keepSelectionVisible() {
    if (assets_.isEmpty()) { assetsList_->setCurrentRow(-1); return; }
    // Fall back to the nearest visible item below, then above, so the current
    // asset never silently disappears from focus after filtering (P-208).
    const int current = assetsList_->currentRow();
    int target = -1;
    for (int i = qMax(0, current); i < assetsList_->count(); ++i)
        if (!assetsList_->item(i)->isHidden()) { target = i; break; }
    if (target < 0)
        for (int i = qMin(qMax(0, current), assetsList_->count() - 1); i >= 0; --i)
            if (!assetsList_->item(i)->isHidden()) { target = i; break; }
    assetsList_->setCurrentRow(target);
    // The focused item must stay visible; never let focus rest off-viewport (P-208).
    if (target >= 0) assetsList_->scrollToItem(assetsList_->item(target));
}

void MainWindow::setAssetViewMode(bool grid) {
    // One shared model; only the rendering differs. The stable-id selection and
    // scroll position are preserved by QListWidget itself (P-208).
    QListWidgetItem *anchor = assetsList_->currentItem();
    if (grid) {
        assetsList_->setViewMode(QListView::IconMode);
        assetsList_->setIconSize(QSize(72, 72));
        assetsList_->setGridSize(QSize(104, 110));
        assetsList_->setResizeMode(QListView::Adjust);
        assetsList_->setWrapping(true);
        assetsList_->setWordWrap(true);
        assetsList_->setUniformItemSizes(true);
        assetsList_->setSpacing(8);
    } else {
        assetsList_->setViewMode(QListView::ListMode);
        assetsList_->setIconSize(QSize(48, 48));
        assetsList_->setGridSize(QSize());
        assetsList_->setResizeMode(QListView::Fixed);
        assetsList_->setWrapping(false);
        assetsList_->setWordWrap(false);
        assetsList_->setUniformItemSizes(true);
        assetsList_->setSpacing(5);
    }
    // Keep the selected asset on screen after the layout change so a view switch
    // never loses the item being inspected (P-208: selection, order, position).
    if (anchor) assetsList_->scrollToItem(anchor);
    assetsList_->viewport()->update();
}

void MainWindow::updateZoomLabel() {
    zoomLabel_->setText(QStringLiteral("%1%").arg(qRound(canvas_->effectiveZoom() * 100)));
}

void MainWindow::syncZoomButtons(bool fit) {
    fitBtn_->setChecked(fit);
    hundredBtn_->setChecked(!fit && qAbs(canvas_->effectiveZoom() - 1.0) < 0.001);
}

void MainWindow::saveScreenshot(const QString &path) { grab().save(path); }
