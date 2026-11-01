#include "mainwindow.h"
#include "preview.h"
#include "preview_canvas.h"
#include "sentinel.h"
#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
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
    rightLayout->addWidget(historyTitle);
    rightLayout->addWidget(hint);
    rightLayout->addWidget(historyList_, 1);
    rightLayout->addWidget(restore_);
    rightLayout->addWidget(export_);
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
        menu.exec(assetsList_->viewport()->mapToGlobal(pos));
    });
    connect(historyList_, &QListWidget::currentRowChanged, this, &MainWindow::showRevision);
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
    revisions_ = asset.id.isEmpty() ? QVector<Revision>{} : library_.revisions(asset.id);
    historyList_->blockSignals(true);
    historyList_->clear();
    for (int i = 0; i < revisions_.size(); ++i) {
        const auto &revision = revisions_.at(i);
        const auto date = QDateTime::fromString(revision.created, Qt::ISODateWithMs).toLocalTime();
        auto *item = new QListWidgetItem(QStringLiteral("%1%2\n%3\n%4")
            .arg(i == 0 ? tr("当前 · ") : QString(), date.toString("MM-dd HH:mm:ss"), revision.note,
                 QStringLiteral("%1 KiB").arg(revision.size / 1024.0, 0, 'f', 1)), historyList_);
        QIcon icon;
        try {
            const auto bytes = library_.readObject(revision.hash);
            const QImage thumb = thumbnailFor(thumbnails_, bytes, revision.hash, QSize(80, 80),
                                              QFileInfo(selected().name).suffix().toLower());
            if (!thumb.isNull()) icon = QPixmap::fromImage(thumb);
        } catch (const std::exception &) {
            icon = style()->standardIcon(QStyle::SP_MessageBoxWarning);
        }
        if (!icon.isNull()) item->setIcon(icon);
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
    restore_->setEnabled(valid); export_->setEnabled(valid);
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
    details_->setText(QStringLiteral("%1\n%2 · %3 · #%4%5")
        .arg(selected().name)
        .arg(row == 0 ? tr("当前版本") : tr("历史版本"))
        .arg(QDateTime::fromString(revision.created, Qt::ISODateWithMs).toLocalTime().toString("yyyy-MM-dd HH:mm:ss"))
        .arg(revision.id)
        .arg(facts.isEmpty() ? QString() : QStringLiteral(" · ") + facts.join(QStringLiteral(" · "))));
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
    if (QMessageBox::question(this, tr("恢复历史版本"), tr("将历史内容恢复为当前工作副本，并新增一个版本。\n未记录的当前内容会先保存。请先在外部编辑器中关闭此文件。")) != QMessageBox::Yes) return;
    try { library_.restore(selected(), revisions_.at(row).id); reloadAssets(selected().id); }
    catch (const std::exception &error) { report(error); }
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

void MainWindow::scanChanges() {
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

void MainWindow::rollbackToSnapshot() {
    const auto snapshots = library_.snapshots();
    if (snapshots.isEmpty()) {
        statusBar()->showMessage(tr("还没有紧急快照锚点"));
        return;
    }
    QStringList labels;
    for (const auto &snapshot : snapshots)
        labels << tr("#%1 %2 · %3 个素材")
                      .arg(snapshot.id)
                      .arg(snapshot.created)
                      .arg(snapshot.assets);
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
