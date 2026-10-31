#include "comparedialog.h"
#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QHBoxLayout>
#include <QImageReader>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QResizeEvent>
#include <QVBoxLayout>
#include <QWheelEvent>

ComparePane::ComparePane(compareview::View *view, std::function<void()> onViewChanged, QWidget *parent)
    : QWidget(parent), view_(view), onViewChanged_(std::move(onViewChanged)) {
    setMinimumSize(240, 200);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
}

void ComparePane::setImage(const QImage &image) {
    image_ = image;
    update();
}

void ComparePane::paintEvent(QPaintEvent *) {
    QPainter painter(this);
    painter.fillRect(rect(), QColor(45, 45, 45));
    if (image_.isNull()) {
        painter.setPen(QColor(160, 160, 160));
        painter.drawText(rect(), Qt::AlignCenter,
                         QStringLiteral("此格式暂不支持画面预览\n原始文件与历史版本完整保留"));
        return;
    }
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    const auto target = compareview::targetRect(image_.size(), size(), view_->zoom, view_->origin);
    // The widget clips to its bounds, so this draws exactly the visible source
    // region at the shared zoom; the empty area beyond a smaller image stays
    // explicit (dark background) rather than being stretched.
    painter.drawImage(target, image_);
}

void ComparePane::wheelEvent(QWheelEvent *event) {
    if (image_.isNull()) return;
    const QPointF cursor = event->position();
    const qreal oldZoom = view_->zoom;
    const QPointF imgPt = QPointF(view_->origin) + cursor / oldZoom; // image pixel under cursor
    const qreal factor = event->angleDelta().y() > 0 ? 1.25 : 0.8;
    const qreal newZoom = qBound<qreal>(0.05, oldZoom * factor, 16.0);
    view_->zoom = newZoom;
    view_->origin = compareview::clampOrigin(image_.size(), size(), newZoom,
                                             imgPt - cursor / newZoom); // keep that pixel anchored
    event->accept();
    if (onViewChanged_) onViewChanged_();
}

void ComparePane::mousePressEvent(QMouseEvent *event) {
    if (event->button() != Qt::LeftButton) return;
    dragging_ = true;
    lastDrag_ = event->position().toPoint();
    setCursor(Qt::ClosedHandCursor);
    event->accept();
}

void ComparePane::mouseMoveEvent(QMouseEvent *event) {
    if (!dragging_) return;
    const QPoint delta = event->position().toPoint() - lastDrag_;
    lastDrag_ = event->position().toPoint();
    view_->origin = compareview::clampOrigin(image_.size(), size(), view_->zoom,
                                             view_->origin + QPointF(-delta.x(), -delta.y()) / view_->zoom);
    if (onViewChanged_) onViewChanged_();
    event->accept();
}

void ComparePane::mouseReleaseEvent(QMouseEvent *event) {
    if (event->button() != Qt::LeftButton) return;
    dragging_ = false;
    unsetCursor();
    event->accept();
}

CompareDialog::CompareDialog(const QString &assetName, const QVector<TimelineEntry> &revisions,
                             int defaultLeft, Library &library, QWidget *parent)
    : QDialog(parent), revisions_(revisions), library_(library) {
    setWindowTitle(QStringLiteral("对比版本 · %1").arg(assetName));
    resize(1120, 640);
    auto *layout = new QVBoxLayout(this);
    auto *title = new QLabel(QStringLiteral("双版并排对比（同步缩放/平移）"));
    title->setStyleSheet("font-size:16px;font-weight:600;");
    layout->addWidget(title);
    auto *pickers = new QHBoxLayout;
    leftCombo_ = new QComboBox;
    rightCombo_ = new QComboBox;
    for (int i = 0; i < revisions_.size(); ++i) {
        const auto &r = revisions_.at(i);
        const auto date = QDateTime::fromString(r.created, Qt::ISODateWithMs).toLocalTime();
        const QString label = QStringLiteral("#%1 %2 · %3").arg(r.revisionId).arg(date.toString("MM-dd HH:mm:ss"), r.note);
        leftCombo_->addItem(label, i);
        rightCombo_->addItem(label, i);
    }
    const int left = qBound(0, defaultLeft, revisions_.size() - 1);
    const int right = (left == 0 && revisions_.size() > 1) ? 1 : 0;
    leftCombo_->setCurrentIndex(left);
    rightCombo_->setCurrentIndex(right);
    pickers->addWidget(new QLabel(QStringLiteral("左侧版本")));
    pickers->addWidget(leftCombo_, 1);
    pickers->addSpacing(16);
    pickers->addWidget(new QLabel(QStringLiteral("右侧版本")));
    pickers->addWidget(rightCombo_, 1);
    layout->addLayout(pickers);
    auto *panes = new QHBoxLayout;
    auto onChanged = [this] {
        leftPane_->update();
        rightPane_->update();
        updateZoomLabel();
    };
    leftPane_ = new ComparePane(&view_, onChanged);
    rightPane_ = new ComparePane(&view_, onChanged);
    panes->addWidget(leftPane_, 1);
    panes->addWidget(rightPane_, 1);
    layout->addLayout(panes, 1);
    auto *controls = new QHBoxLayout;
    fitCheck_ = new QCheckBox(QStringLiteral("适配窗口（统一比例）"));
    auto *resetButton = new QPushButton(QStringLiteral("重置视图"));
    zoomLabel_ = new QLabel;
    sizeNote_ = new QLabel;
    sizeNote_->setWordWrap(true);
    controls->addWidget(fitCheck_);
    controls->addWidget(resetButton);
    controls->addWidget(zoomLabel_);
    controls->addStretch(1);
    layout->addLayout(controls);
    layout->addWidget(sizeNote_);
    const auto load = [this](int side) { loadRevision(side); };
    connect(leftCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, load);
    connect(rightCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, load);
    connect(fitCheck_, &QCheckBox::toggled, this, &CompareDialog::applyFit);
    connect(resetButton, &QPushButton::clicked, this, &CompareDialog::resetView);
    loadRevision(0);
    loadRevision(1);
    updateZoomLabel();
}

void CompareDialog::loadRevision(int side) {
    QComboBox *combo = side == 0 ? leftCombo_ : rightCombo_;
    ComparePane *pane = side == 0 ? leftPane_ : rightPane_;
    const int index = combo->currentData().toInt();
    if (index < 0 || index >= revisions_.size()) return;
    const auto &revision = revisions_.at(index);
    QImage image;
    if (!revision.corrupt) {
        QImageReader reader(library_.objectPath(revision.hash));
        reader.setDecideFormatFromContent(true);
        reader.setAutoTransform(true);
        image = reader.read();
    }
    pane->setImage(image);
    if (side == 0) leftImage_ = image; else rightImage_ = image;
    const bool differ = !leftImage_.isNull() && !rightImage_.isNull() && leftImage_.size() != rightImage_.size();
    sizeNote_->setText(differ
        ? QStringLiteral("两图尺寸不同（左 %1×%2，右 %3×%4）：默认左上对齐，不自动拉伸；"
                         "缩放/平移同步同一比例与原点，空白区域表示该侧无内容。")
              .arg(leftImage_.width()).arg(leftImage_.height())
              .arg(rightImage_.width()).arg(rightImage_.height())
        : QString());
    if (fitCheck_->isChecked()) applyFit(true);
}

void CompareDialog::applyFit(bool fit) {
    if (!fit) return;
    // Uniform ratio for both panes: the smallest fit so both whole images are
    // visible (never stretched past 100%). Blank beyond a smaller image is
    // explicit, not a deformation hiding the size difference.
    qreal zoom = 1.0;
    bool any = false;
    for (const auto &image : {&leftImage_, &rightImage_}) {
        if (image->isNull()) continue;
        const QSize paneSize = (image == &leftImage_) ? leftPane_->size() : rightPane_->size();
        zoom = qMin(zoom, compareview::fitZoom(image->size(), paneSize));
        any = true;
    }
    if (any) {
        view_.zoom = zoom;
        view_.origin = QPointF(0, 0);
        leftPane_->update();
        rightPane_->update();
        updateZoomLabel();
    }
}

void CompareDialog::resetView() {
    view_.zoom = 1.0;
    view_.origin = QPointF(0, 0);
    fitCheck_->setChecked(false);
    leftPane_->update();
    rightPane_->update();
    updateZoomLabel();
}

void CompareDialog::updateZoomLabel() {
    zoomLabel_->setText(QStringLiteral("缩放 %1%").arg(qRound(view_.zoom * 100)));
}

void CompareDialog::resizeEvent(QResizeEvent *event) {
    QDialog::resizeEvent(event);
    if (fitCheck_->isChecked()) applyFit(true);
}
