#include "mainwindow.h"
#include "compare_canvas.h"
#include "preview.h"
#include "preview_canvas.h"
#include "sentinel.h"
#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDesktopServices>
#include <QFileDialog>
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
    auto *brand = new QLabel(QStringLiteral("  pentimento   "));
    brand->setStyleSheet("font-size:20px;font-weight:600;padding:8px;");
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
    // P-213: batch tags apply to the visible (filtered) set explicitly, with a
    // count preview before anything changes.
    toolbar->addAction(tr("为筛选结果添加标签…"), this, &MainWindow::batchAddTag);
    toolbar->addAction(tr("从筛选结果移除标签…"), this, &MainWindow::batchRemoveTag);
    // Anti-ransomware Layer 3: one-click rollback to a pre-infection anchor.
    toolbar->addAction(tr("恢复到锚点"), this, &MainWindow::rollbackToSnapshot);
    // P-312: read-only point-in-time directory browse of a snapshot manifest.
    toolbar->addAction(tr("浏览快照…"), this, &MainWindow::browseSnapshot);
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
    authorInput_ = new QLineEdit;
    authorInput_->setPlaceholderText(tr("作者（可留空）"));
    licenseInput_ = new QLineEdit;
    licenseInput_->setPlaceholderText(tr("许可（如实填写；应用不代为判断）"));
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
    splitter->addWidget(left); splitter->addWidget(center); splitter->addWidget(right);
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
                const auto bytes = library_.readObject(revisions.first().hash);
                const QImage thumb = thumbnailFor(thumbnails_, bytes, revisions.first().hash, QSize(80, 80),
                                                  QFileInfo(asset.name).suffix().toLower());
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

void MainWindow::reloadHistory() {
    const auto asset = selected();
    // P-308: the timeline is ordered by the database's monotonic revision id
    // (revisions() returns newest id first) — local time is display-only, so a
    // clock change or timezone shift never reorders or mixes history.
    revisions_ = asset.id.isEmpty() ? QVector<Revision>{} : library_.revisions(asset.id);
    // Working-copy content markers: which revision matches the current content
    // ("当前内容"), and whether newer unrecorded changes sit in the working copy.
    QString currentHash;
    {
        QFile file(asset.path);
        if (file.exists() && file.open(QIODevice::ReadOnly)) {
            QCryptographicHash hash(QCryptographicHash::Sha256);
            constexpr qint64 kChunk = 1 << 20;
            while (!file.atEnd()) hash.addData(file.read(kChunk));
            currentHash = QString::fromLatin1(hash.result().toHex());
        }
    }
    qint64 currentId = 0; // newest revision whose content equals the working copy
    for (const auto &revision : revisions_) {
        if (!currentHash.isEmpty() && revision.hash == currentHash) { currentId = revision.id; break; }
    }
    const bool unrecorded = !currentHash.isEmpty()
        && (revisions_.isEmpty() || revisions_.constFirst().hash != currentHash);
    historyList_->blockSignals(true);
    historyList_->clear();
    for (int i = 0; i < revisions_.size(); ++i) {
        const auto &revision = revisions_.at(i);
        const auto date = QDateTime::fromString(revision.created, Qt::ISODateWithMs).toLocalTime();
        QByteArray bytes;
        bool corrupt = false;
        try {
            bytes = library_.readObject(revision.hash);
        } catch (const std::exception &) {
            corrupt = true; // object missing or hash mismatch: never hidden
        }
        // P-307/P-308: per-row markers. A restore-formed record and a restore's
        // automatic safety save are marked by their system-written notes; every
        // older revision is plainly "历史".
        QStringList markers;
        if (i == 0 && unrecorded) markers << tr("未记录修改");
        if (revision.pinned) markers << tr("已固定");
        // P-310: a tombstone revision marks the in-app deletion point. It keeps
        // the last content hash, so a healthy object still renders; a missing
        // object just shows the standard corrupt warning. The asset stays
        // reachable here even though it is no longer listed as live.
        if (revision.tombstone) markers << tr("墓碑");
        if (!corrupt && revision.id == currentId) markers << tr("当前内容");
        else if (revision.note.startsWith(QStringLiteral("恢复历史版本 #"))) markers << tr("恢复");
        else if (revision.note == QStringLiteral("恢复前自动保存")) markers << tr("自动保存");
        else if (i != 0) markers << tr("历史");
        if (corrupt) markers << tr("损坏对象");
        const QString prefix = markers.isEmpty() ? QString()
            : markers.join(QStringLiteral(" · ")) + QStringLiteral(" · ");
        auto *item = new QListWidgetItem(QStringLiteral("%1%2\n%3\n%4")
            .arg(prefix, date.toString("MM-dd HH:mm:ss"), revision.note,
                 QStringLiteral("%1 KiB").arg(revision.size / 1024.0, 0, 'f', 1)), historyList_);
        if (revision.pinned) item->setToolTip(tr("固定原因：%1").arg(revision.pinReason));
        if (corrupt) {
            item->setIcon(style()->standardIcon(QStyle::SP_MessageBoxWarning));
        } else {
            const QImage thumb = thumbnailFor(thumbnails_, bytes, revision.hash, QSize(80, 80),
                                              QFileInfo(selected().name).suffix().toLower());
            if (!thumb.isNull()) item->setIcon(QPixmap::fromImage(thumb));
        }
    }
    historyList_->setCurrentRow(revisions_.isEmpty() ? -1 : 0);
    historyList_->blockSignals(false);
    showRevision();
    reloadTags();
    reloadRatingNote();
    reloadProvenance();
}

void MainWindow::reloadTags() {
    tagsList_->blockSignals(true);
    tagsList_->clear();
    for (const QString &tag : library_.tags(selected().id))
        tagsList_->addItem(tag);
    tagsList_->blockSignals(false);
}

// P-214: rating/note block reflects the selected asset. The rating combo maps
// -1 (no rating) to index 0, then 0..5 stars; the note editor holds the stored
// note verbatim. Loading blocks signals so a reload never triggers a save.
void MainWindow::reloadRatingNote() {
    const auto asset = selected();
    ratingCombo_->blockSignals(true);
    if (asset.id.isEmpty()) {
        ratingCombo_->setCurrentIndex(0);
    } else {
        const int value = library_.rating(asset.id);
        const int index = ratingCombo_->findData(value);
        ratingCombo_->setCurrentIndex(index >= 0 ? index : 0);
    }
    ratingCombo_->blockSignals(false);
    noteEdit_->blockSignals(true);
    noteEdit_->setPlainText(asset.id.isEmpty() ? QString() : library_.note(asset.id));
    noteEdit_->blockSignals(false);
    noteAssetId_ = asset.id;
}

void MainWindow::applyRating(int index) {
    const auto asset = selected();
    if (asset.id.isEmpty()) return;
    const int value = ratingCombo_->itemData(index).toInt();
    try {
        library_.setRating(asset.id, value);
        statusBar()->showMessage(value < 0 ? tr("已清除评分") : tr("已保存评分：%1 星").arg(value));
        // P-309: the change appended a metadata-only revision; refresh the timeline.
        reloadHistory();
    } catch (const std::exception &error) { report(error); }
}

// P-215: list the provenance records of the selected asset (newest first) and
// clear any pending (unrecorded) attachment. A record line omits empty fields,
// so "unknown" never renders as a value that was never entered.
void MainWindow::reloadProvenance() {
    const auto asset = selected();
    pendingEvidenceHash_.clear();
    pendingEvidenceName_.clear();
    evidenceLabel_->setText(tr("未附加证据"));
    provenanceList_->clear();
    for (const auto &entry : library_.provenance(asset.id)) {
        const auto date = QDateTime::fromString(entry.recorded, Qt::ISODateWithMs).toLocalTime();
        QStringList parts;
        parts << date.toString("MM-dd HH:mm:ss");
        if (!entry.source.isEmpty()) parts << entry.source;
        if (!entry.author.isEmpty()) parts << tr("作者：%1").arg(entry.author);
        if (!entry.license.isEmpty()) parts << tr("许可：%1").arg(entry.license);
        if (!entry.evidenceName.isEmpty()) parts << tr("证据：%1").arg(entry.evidenceName);
        auto *item = new QListWidgetItem(parts.join(" · "), provenanceList_);
        item->setData(Qt::UserRole, entry.evidence);
    }
    exportEvidenceBtn_->setEnabled(false);
}

void MainWindow::attachEvidence() {
    const QString path = QFileDialog::getOpenFileName(this, tr("选择授权证据文件"));
    if (path.isEmpty()) return;
    try {
        pendingEvidenceHash_ = library_.storeEvidence(path);
        pendingEvidenceName_ = QFileInfo(path).fileName();
        evidenceLabel_->setText(tr("待记录证据：%1").arg(pendingEvidenceName_));
        evidenceLabel_->setStyleSheet(QString());
    } catch (const std::exception &error) { report(error); }
}

void MainWindow::recordProvenance() {
    const auto asset = selected();
    if (asset.id.isEmpty()) return;
    try {
        Provenance entry;
        entry.source = sourceInput_->text().trimmed();
        entry.author = authorInput_->text().trimmed();
        entry.license = licenseInput_->text().trimmed();
        entry.evidence = pendingEvidenceHash_;
        entry.evidenceName = pendingEvidenceName_;
        library_.addProvenance(asset.id, entry);
        sourceInput_->clear();
        authorInput_->clear();
        licenseInput_->clear();
        statusBar()->showMessage(tr("已记录来源与授权"));
        reloadProvenance();
        reloadHistory(); // P-309: the change appended metadata-only revisions
    } catch (const std::exception &error) { report(error); }
}

void MainWindow::renameSelected() {
    const auto asset = selected();
    if (asset.id.isEmpty()) return;
    bool ok = false;
    const QString name = QInputDialog::getText(this, tr("重命名"), tr("新名称"), QLineEdit::Normal, asset.name, &ok).trimmed();
    if (!ok || name.isEmpty()) return;
    try {
        library_.renameAsset(asset.id, name);
        statusBar()->showMessage(tr("已重命名：%1").arg(name));
        reloadAssets(asset.id);
    } catch (const std::exception &error) { report(error); }
}

void MainWindow::moveSelected() {
    const auto asset = selected();
    if (asset.id.isEmpty()) return;
    bool ok = false;
    const QString folder = QInputDialog::getText(this, tr("移动到文件夹"),
                                                 tr("文件夹（库内相对路径，空为根目录）"),
                                                 QLineEdit::Normal, asset.folder, &ok);
    if (!ok) return;
    try {
        library_.moveAsset(asset.id, folder);
        const QString cleaned = folder.trimmed();
        statusBar()->showMessage(cleaned.isEmpty() ? tr("已移到根目录") : tr("已移动到：%1").arg(cleaned));
        reloadAssets(asset.id);
    } catch (const std::exception &error) { report(error); }
}

void MainWindow::deleteSelected() {
    const auto asset = selected();
    if (asset.id.isEmpty()) return;
    // P-310: preview → confirm, exactly as the spec's three-state table demands
    // for an in-app delete. The preview makes explicit that this is not the
    // "missing working copy" or "storage unreachable" state.
    const auto revisions = library_.revisions(asset.id);
    const QStringList lines{
        tr("名称：%1").arg(asset.name),
        tr("路径：%1").arg(asset.logicalPath()),
        tr("版本：%1 个").arg(revisions.size()),
        tr("工作副本将被移除；历史与对象保留，可从时间线恢复。"),
    };
    QMessageBox box(QMessageBox::Warning, tr("删除素材"), lines.join(QLatin1Char('\n')), QMessageBox::NoButton, this);
    auto *confirmBtn = box.addButton(tr("确认删除"), QMessageBox::YesRole);
    box.addButton(tr("取消"), QMessageBox::NoRole);
    box.exec();
    if (box.clickedButton() != confirmBtn) return;
    try {
        library_.deleteAsset(asset.id);
        statusBar()->showMessage(tr("已删除：%1（历史与对象保留，可从时间线恢复）").arg(asset.name));
        reloadAssets();
        historyList_->clear();
        details_->clear();
    } catch (const std::exception &error) { report(error); }
}

void MainWindow::exportEvidenceFile() {
    const int row = provenanceList_->currentRow();
    if (row < 0) return;
    const QString hash = provenanceList_->item(row)->data(Qt::UserRole).toString();
    if (hash.isEmpty()) return;
    const QString dest = QFileDialog::getSaveFileName(this, tr("导出授权证据"));
    if (dest.isEmpty()) return;
    try {
        library_.exportEvidence(hash, dest);
        statusBar()->showMessage(tr("授权证据已导出：%1").arg(QFileInfo(dest).fileName()));
    } catch (const std::exception &error) { report(error); }
}

void MainWindow::saveNote() {
    // Save to the asset the editor represents, not to selected(): switching
    // assets while the editor is focused must not carry the old note over.
    if (noteAssetId_.isEmpty()) return;
    try {
        library_.setNote(noteAssetId_, noteEdit_->toPlainText());
        statusBar()->showMessage(tr("备注已保存"));
        // P-309: the change appended a metadata-only revision; refresh the timeline.
        reloadHistory();
    } catch (const std::exception &error) { report(error); }
}

QStringList MainWindow::visibleAssetIds() const {
    QStringList ids;
    for (int i = 0; i < assetsList_->count(); ++i)
        if (!assetsList_->item(i)->isHidden())
            ids << assetsList_->item(i)->data(Qt::UserRole).toString();
    return ids;
}

void MainWindow::addTagToSelected() {
    const auto asset = selected();
    if (asset.id.isEmpty()) return;
    const QString tag = tagInput_->text().trimmed();
    if (tag.isEmpty()) { statusBar()->showMessage(tr("标签不能为空")); return; }
    try {
        const int changed = library_.addTag(asset.id, tag);
        statusBar()->showMessage(changed ? tr("已添加标签：%1").arg(tag) : tr("标签已存在：%1").arg(tag));
        tagInput_->clear();
        reloadTags();
        if (changed) reloadHistory(); // P-309: the change appended a metadata-only revision
    } catch (const std::exception &error) { report(error); }
}

void MainWindow::removeSelectedTag() {
    const auto asset = selected();
    if (asset.id.isEmpty()) return;
    const int row = tagsList_->currentRow();
    if (row < 0) { statusBar()->showMessage(tr("请先选择一个标签")); return; }
    const QString tag = tagsList_->item(row)->text();
    try {
        library_.removeTag(asset.id, tag);
        statusBar()->showMessage(tr("已移除标签：%1").arg(tag));
        reloadTags();
        reloadHistory(); // P-309: the change appended a metadata-only revision
    } catch (const std::exception &error) { report(error); }
}

void MainWindow::batchAddTag() {
    bool ok = false;
    const QString tag = QInputDialog::getText(this, tr("为筛选结果添加标签"), tr("标签"), QLineEdit::Normal, QString(), &ok).trimmed();
    if (!ok || tag.isEmpty()) return;
    const QStringList ids = visibleAssetIds();
    const int affected = library_.countAssetsWithoutTag(ids, tag);
    if (affected == 0) { statusBar()->showMessage(tr("筛选结果中的素材都已带此标签")); return; }
    if (QMessageBox::question(this, tr("批量添加标签"),
                              tr("将给 %1 个素材添加标签「%2」（已有该标签的不变）。\n不会覆盖其他标签。").arg(affected).arg(tag)) != QMessageBox::Yes) return;
    try {
        const int changed = library_.addTagToAssets(ids, tag);
        statusBar()->showMessage(tr("已为 %1 个素材添加标签：%2").arg(changed).arg(tag));
        reloadTags();
    } catch (const std::exception &error) { report(error); }
}

void MainWindow::batchRemoveTag() {
    bool ok = false;
    const QString tag = QInputDialog::getText(this, tr("从筛选结果移除标签"), tr("标签"), QLineEdit::Normal, QString(), &ok).trimmed();
    if (!ok || tag.isEmpty()) return;
    const QStringList ids = visibleAssetIds();
    const int affected = library_.countAssetsWithTag(ids, tag);
    if (affected == 0) { statusBar()->showMessage(tr("筛选结果中没有素材带此标签")); return; }
    if (QMessageBox::question(this, tr("批量移除标签"),
                              tr("将从 %1 个素材移除标签「%2」。\n只移除这一个标签，不影响其他标签。").arg(affected).arg(tag)) != QMessageBox::Yes) return;
    try {
        const int changed = library_.removeTagFromAssets(ids, tag);
        statusBar()->showMessage(tr("已从 %1 个素材移除标签：%2").arg(changed).arg(tag));
        reloadTags();
    } catch (const std::exception &error) { report(error); }
}

void MainWindow::showRevision() {
    const int row = historyList_->currentRow();
    const bool valid = row >= 0 && row < revisions_.size();
    restore_->setEnabled(valid); export_->setEnabled(valid); compareBtn_->setEnabled(valid);
    if (!valid) return;
    const auto &revision = revisions_.at(row);
    // P-212: technical metadata (format/size/fileSize/channels) from the header;
    // unknown fields are omitted, never invented.
    const auto meta = preview::metadata(library_.objectPath(revision.hash),
                                        QFileInfo(selected().name).suffix().toLower());
    QStringList facts;
    if (!meta.format.isEmpty()) facts << meta.format.toUpper();
    if (meta.width > 0 && meta.height > 0)
        facts << QStringLiteral("%1×%2").arg(meta.width).arg(meta.height);
    if (meta.fileSize > 0)
        facts << tr("%1 MiB").arg(meta.fileSize / 1048576.0, 0, 'f', 1);
    if (!meta.channels.isEmpty()) {
        QString ch = meta.channels;
        if (meta.bitsPerChannel > 0) ch += QStringLiteral(" %1bit").arg(meta.bitsPerChannel);
        facts << ch;
    }
    // P-310: working-copy health is shown alongside the version facts. Missing
    // (externally removed) and unreachable (unplugged mount / dropped share /
    // permissions) are recoverable states, never deletions — no tombstone is
    // written for either, and the message says so explicitly.
    QString healthLine;
    const auto health = library_.workingCopyHealth(selected());
    if (health == Library::Health::Missing)
        healthLine = tr("\n工作副本缺失（可从历史恢复，不视为删除）");
    else if (health == Library::Health::Unreachable)
        healthLine = tr("\n存储不可达（拔盘或断网不算删除；恢复挂载后自动恢复）");
    details_->setText(QStringLiteral("%1\n%2 · %3 · #%4%5%6")
        .arg(selected().name)
        .arg(row == 0 ? tr("当前版本") : tr("历史版本"))
        .arg(QDateTime::fromString(revision.created, Qt::ISODateWithMs).toLocalTime().toString("yyyy-MM-dd HH:mm:ss"))
        .arg(revision.id)
        .arg(facts.isEmpty() ? QString() : QStringLiteral(" · ") + facts.join(QStringLiteral(" · ")))
        .arg(healthLine));
    QByteArray content;
    try {
        content = library_.readObject(revision.hash);
    } catch (const std::exception &error) {
        preview_->setText(tr("内容损坏或校验失败\n\n%1\n\n原始对象已保留，可导出排查。")
            .arg(QString::fromUtf8(error.what())));
        previewStack_->setCurrentIndex(1);
        return;
    }
    // P-209/P-210: decode is bounded (huge images downscale on read, never
    // stretching), the canvas zoom stays uniform, and the asset's own extension
    // is authoritative for signature-less formats (TGA) that sniffing mislabels.
    const auto decoded = preview::decode(library_.objectPath(revision.hash),
                                         QFileInfo(selected().name).suffix().toLower());
    if (!decoded.valid) {
        // P-211: PSD composite preview was evaluated (no PSD codec in Qt), so
        // Photoshop files degrade explicitly: no composite view is claimed, and
        // the original bytes stay fully preserved and exportable.
        preview_->setText(preview::isPsdSignature(content)
            ? tr("原文件已保留，暂无合成预览（PSD 解析未支持）\n\nPhotoshop 合成图解析不在当前版本范围内（Qt 6.10.2 无 PSD 解码插件）。\n原始文件和历史版本已完整保留，可以导出查看。")
            : tr("此格式暂不支持画面预览\n\n原始文件和历史版本已完整保留，可以导出查看。"));
        previewStack_->setCurrentIndex(1);
        return;
    }
    canvas_->setImage(decoded.image);
    previewStack_->setCurrentIndex(0);
    syncZoomButtons(canvas_->isFit());
    updateZoomLabel();
    if (decoded.reduced)
        statusBar()->showMessage(tr("预览已降采样显示（解码尺寸超出内存预算），原始文件未修改"));
}

void MainWindow::updateZoomLabel() {
    zoomLabel_->setText(QStringLiteral("%1%").arg(qRound(canvas_->effectiveZoom() * 100)));
}

void MainWindow::syncZoomButtons(bool fit) {
    fitBtn_->setChecked(fit);
    hundredBtn_->setChecked(!fit && qAbs(canvas_->effectiveZoom() - 1.0) < 0.001);
}

void MainWindow::importFiles() {
    const auto files = QFileDialog::getOpenFileNames(this, tr("导入素材（保留原文件，创建工作副本）"));
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
    const auto note = QInputDialog::getText(this, tr("记录版本"), tr("版本说明"), QLineEdit::Normal, QStringLiteral("手动快照"), &ok);
    if (!ok) return;
    try {
        bool changed = false;
        const qint64 revisionId = library_.captureRevision(asset, note, &changed);
        // P-303: surface the version id either way, so an unchanged capture is
        // an explicit answer ("current version #N") rather than a silent no-op.
        statusBar()->showMessage(changed
            ? tr("已记录新版本 #%1").arg(revisionId)
            : tr("内容未变化（当前版本 #%1），无需重复保存").arg(revisionId));
        reloadHistory();
    } catch (const std::exception &error) { report(error); }
}

void MainWindow::restore() {
    const int row = historyList_->currentRow();
    if (row < 0) return;
    const auto asset = selected();
    const qint64 revisionId = revisions_.at(row).id;
    // P-315: the preview comes first — the exact operation list is computed
    // before any target write, and the preview itself never writes a file.
    Library::RestorePlan plan;
    try { plan = library_.planRestore(asset, revisionId); }
    catch (const std::exception &error) { report(error); return; }
    QString detail;
    bool confirmable = true;
    if (plan.action == QStringLiteral("overwrite")) {
        // P-316: the on-disk unrecorded content is protected as a version before
        // the overwrite. Only disk-saved content is protected — an editor's
        // unsaved buffer is never claimed to be saved.
        detail = tr("将覆盖当前工作副本：未记录的当前内容会先保存为保护版本。\n"
                    "空间需求：约 %1 KiB（保护 %2 KiB + 写入 %3 KiB）。\n"
                    "保护的是磁盘上已保存的内容；外部编辑器未写盘的内容不受保护，请先关闭编辑器。")
            .arg(plan.spaceNeeded / 1024.0, 0, 'f', 1)
            .arg(plan.protectBytes / 1024.0, 0, 'f', 1)
            .arg(plan.targetBytes / 1024.0, 0, 'f', 1);
    } else if (plan.action == QStringLiteral("create")) {
        detail = tr("当前工作副本缺失——将新增该文件（无内容可保护，请确认）。\n"
                    "空间需求：约 %1 KiB。")
            .arg(plan.spaceNeeded / 1024.0, 0, 'f', 1);
    } else if (plan.action == QStringLiteral("skip")) {
        detail = tr("当前内容已与目标一致——无需恢复。");
        confirmable = false;
    } else {
        detail = tr("无法恢复：%1").arg(plan.conflictReason);
        confirmable = false;
    }
    QMessageBox box(this);
    box.setWindowTitle(tr("恢复预演"));
    box.setIcon(confirmable ? QMessageBox::Question : QMessageBox::Warning);
    box.setText(detail);
    if (!confirmable) {
        box.addButton(QMessageBox::Ok);
        box.exec();
        return;
    }
    QAbstractButton *confirm = box.addButton(tr("确认恢复"), QMessageBox::YesRole);
    box.addButton(tr("取消"), QMessageBox::NoRole);
    box.exec();
    if (box.clickedButton() != confirm) return;
    // P-315: execution re-checks the plan binding — a working copy that changed
    // since the preview makes the plan stale, and the user is asked to refresh.
    try { library_.restore(asset, plan); reloadAssets(asset.id); }
    catch (const std::exception &error) { report(error); return; }
    // Re-arm the sentinel: restore is an app-initiated write, not ransomware.
    if (sentinel_) sentinel_->resetBaseline();
}

void MainWindow::exportFile() {
    const int row = historyList_->currentRow();
    if (row < 0) return;
    const auto path = QFileDialog::getSaveFileName(this, tr("导出历史版本"), selected().name);
    if (path.isEmpty()) return;
    try { library_.exportRevision(selected().id, revisions_.at(row).id, path); }
    catch (const std::exception &error) { report(error); }
}

void MainWindow::compareVersions() {
    const int row = historyList_->currentRow();
    if (row < 0 || row >= revisions_.size() || revisions_.isEmpty()) return;
    const auto &current = revisions_.constFirst();   // newest = current content
    const auto &selected = revisions_.at(row);
    const QString suffix = QFileInfo(this->selected().name).suffix().toLower();

    // P-313: one shared image-coordinate transform (same zoom scale, same
    // origin) for both panes. Default is top-left aligned, never stretched;
    // "fit window" applies a single unified scale. Pan and zoom move both
    // panes together, and differing sizes read as explicit empty regions.
    auto decodeRevision = [this, suffix](qint64 revisionId) {
        for (const auto &rev : revisions_)
            if (rev.id == revisionId)
                return preview::decode(library_.objectPath(rev.hash), suffix).image;
        return QImage();
    };
    const QImage leftImage = decodeRevision(current.id);
    const QImage rightImage = decodeRevision(selected.id);

    QDialog dialog(this);
    dialog.setWindowTitle(tr("双版对比"));
    dialog.resize(860, 560);
    auto *layout = new QVBoxLayout(&dialog);
    auto *canvas = new CompareCanvas(&dialog);
    canvas->setImages(leftImage, rightImage);
    auto *bar = new QWidget(&dialog);
    auto *barLayout = new QHBoxLayout(bar);
    barLayout->setContentsMargins(0, 0, 0, 0);
    auto *leftLabel = new QLabel(tr("左：当前版本 #%1").arg(current.id), bar);
    auto *rightLabel = new QLabel(tr("右：所选版本 #%1").arg(selected.id), bar);
    auto *zoomLabel = new QLabel(bar);
    auto *fitCheck = new QCheckBox(tr("适配窗口（统一比例）"), bar);
    fitCheck->setChecked(true);
    connect(fitCheck, &QCheckBox::toggled, canvas, &CompareCanvas::setFit);
    // P-314: sliding-divider wipe over the same shared transform. The divider
    // is dragged with the mouse, stepped with Left/Right, and its step size is
    // adjusted with [ / ].
    auto *slidingCheck = new QCheckBox(tr("滑动对比"), bar);
    connect(slidingCheck, &QCheckBox::toggled, canvas, &CompareCanvas::setSliding);
    connect(canvas, &CompareCanvas::zoomChanged, bar, [zoomLabel](qreal factor) {
        zoomLabel->setText(QStringLiteral("%1%").arg(qRound(factor * 100)));
    });
    zoomLabel->setText(QStringLiteral("%1%").arg(qRound(canvas->zoom() * 100)));
    barLayout->addWidget(leftLabel);
    barLayout->addStretch(1);
    barLayout->addWidget(zoomLabel);
    barLayout->addWidget(fitCheck);
    barLayout->addWidget(slidingCheck);
    barLayout->addStretch(1);
    barLayout->addWidget(rightLabel);
    // P-313: differing sizes are stated explicitly — never auto-stretched.
    if (leftImage.size() != rightImage.size()) {
        auto *sizeNote = new QLabel(tr("两版尺寸不同：%1×%2 与 %3×%4（左上对齐，不拉伸；空白为无内容区域）")
            .arg(leftImage.width()).arg(leftImage.height())
            .arg(rightImage.width()).arg(rightImage.height()), &dialog);
        sizeNote->setStyleSheet("color:#b26a00;");
        layout->addWidget(sizeNote);
    }
    layout->addWidget(canvas, 1);
    layout->addWidget(bar);
    // P-314: divider interaction is discoverable and explicit.
    auto *dividerHint = new QLabel(tr("滑动对比：拖动分界线；左右键步进；[ / ] 调整步进；缩放后分界线保持同一图像坐标。"), &dialog);
    dividerHint->setStyleSheet("color:#888;");
    layout->addWidget(dividerHint);
    auto *closeBtn = new QPushButton(tr("关闭"), &dialog);
    connect(closeBtn, &QPushButton::clicked, &dialog, &QDialog::accept);
    layout->addWidget(closeBtn);
    dialog.exec();
}

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

// P-309: read-only per-field metadata history for the selected asset. Each
// change shows field, old → new, and the metadata-only revision it created.
// V1 is read-only: no per-field revert, no invented old values.
void MainWindow::showFieldHistory() {
    const auto asset = selected();
    QDialog dialog(this);
    dialog.setWindowTitle(tr("字段历史"));
    dialog.resize(480, 380);
    auto *layout = new QVBoxLayout(&dialog);
    auto *hint = new QLabel(tr("逐字段只读回看（名称/目录/标签/评分/备注/来源/作者/许可）；字段变更与版本序列一致，内容不变。"));
    hint->setWordWrap(true);
    layout->addWidget(hint);
    auto *list = new QListWidget;
    list->setWordWrap(true);
    layout->addWidget(list, 1);
    auto *closeBtn = new QPushButton(tr("关闭"));
    layout->addWidget(closeBtn);
    connect(closeBtn, &QPushButton::clicked, &dialog, &QDialog::accept);
    const auto history = library_.fieldHistory(asset.id);
    if (history.isEmpty()) {
        auto *item = new QListWidgetItem(tr("（无字段变更记录）"));
        item->setForeground(Qt::gray);
        list->addItem(item);
    }
    for (const auto &change : history) {
        const auto time = QDateTime::fromString(change.changed, Qt::ISODateWithMs).toLocalTime();
        const auto value = [](const QString &text) { return text.isEmpty() ? tr("（空）") : text; };
        list->addItem(QStringLiteral("%1\n%2：%3 → %4\n版本 #%5")
                          .arg(time.toString("MM-dd HH:mm:ss"), change.field,
                               value(change.oldValue), value(change.newValue))
                          .arg(change.revisionId));
    }
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

void MainWindow::saveScreenshot(const QString &path) { grab().save(path); }
