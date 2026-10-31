#include "comparedialog.h"
#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QHBoxLayout>
#include <QImageReader>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QResizeEvent>
#include <QStackedWidget>
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

SlidingComparePane::SlidingComparePane(compareview::View *view, std::function<void()> onViewChanged, QWidget *parent)
    : QWidget(parent), view_(view), onViewChanged_(std::move(onViewChanged)) {
    setMinimumSize(240, 200);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    setFocusPolicy(Qt::StrongFocus);
    setToolTip(QStringLiteral("拖动分界线比较两版同坐标区域；滚轮缩放、拖动平移同步同一比例与原点；"
                              "←/→ 步进分界线，[ / ] 调整步进"));
}

void SlidingComparePane::setImages(const QImage &left, const QImage &right) {
    left_ = left;
    right_ = right;
    update();
}

void SlidingComparePane::setDividerFraction(qreal fraction) {
    divider_ = qBound(0.0, fraction, 1.0);
    update();
}

int SlidingComparePane::dividerX() const {
    return qRound(divider_ * width());
}

void SlidingComparePane::paintEvent(QPaintEvent *) {
    QPainter painter(this);
    painter.fillRect(rect(), QColor(45, 45, 45));
    const int dx = dividerX();
    // Right version first, then the left version clipped to the divider, both
    // drawn at the SAME targetRect (shared zoom+origin). The divider therefore
    // exposes the identical image-pixel column on both sides.
    const auto draw = [&](const QImage &image, const QRect &clip) {
        if (image.isNull()) return;
        painter.save();
        painter.setClipRect(clip);
        painter.setRenderHint(QPainter::SmoothPixmapTransform);
        painter.drawImage(compareview::targetRect(image.size(), size(), view_->zoom, view_->origin), image);
        painter.restore();
    };
    draw(right_, rect());
    draw(left_, QRect(0, 0, dx, height()));
    painter.setPen(QPen(QColor(220, 220, 220), 2));
    painter.drawLine(dx, 0, dx, height());
    if (left_.isNull()) {
        painter.setPen(QColor(160, 160, 160));
        painter.drawText(QRect(0, 0, dx, height()), Qt::AlignCenter, QStringLiteral("左侧不可预览"));
    }
    if (right_.isNull()) {
        painter.setPen(QColor(160, 160, 160));
        painter.drawText(QRect(dx, 0, width() - dx, height()), Qt::AlignCenter, QStringLiteral("右侧不可预览"));
    }
}

void SlidingComparePane::wheelEvent(QWheelEvent *event) {
    const QImage &image = !left_.isNull() ? left_ : right_;
    if (image.isNull()) return;
    const QPointF cursor = event->position();
    const qreal oldZoom = view_->zoom;
    const QPointF imgPt = QPointF(view_->origin) + cursor / oldZoom;
    const qreal factor = event->angleDelta().y() > 0 ? 1.25 : 0.8;
    const qreal newZoom = qBound<qreal>(0.05, oldZoom * factor, 16.0);
    view_->zoom = newZoom;
    view_->origin = compareview::clampOrigin(image.size(), size(), newZoom, imgPt - cursor / newZoom);
    event->accept();
    if (onViewChanged_) onViewChanged_();
}

void SlidingComparePane::mousePressEvent(QMouseEvent *event) {
    if (event->button() != Qt::LeftButton) return;
    if (qAbs(event->position().x() - dividerX()) <= 6) {
        draggingDivider_ = true;
        setCursor(Qt::SplitHCursor);
    } else {
        draggingPan_ = true;
        lastDrag_ = event->position().toPoint();
        setCursor(Qt::ClosedHandCursor);
    }
    event->accept();
}

void SlidingComparePane::mouseMoveEvent(QMouseEvent *event) {
    if (draggingDivider_) {
        divider_ = qBound<qreal>(0.0, qreal(event->position().x()) / width(), 1.0);
        update();
    } else if (draggingPan_) {
        const QPoint delta = event->position().toPoint() - lastDrag_;
        lastDrag_ = event->position().toPoint();
        const QImage &image = !left_.isNull() ? left_ : right_;
        if (!image.isNull())
            view_->origin = compareview::clampOrigin(image.size(), size(), view_->zoom,
                                                     view_->origin + QPointF(-delta.x(), -delta.y()) / view_->zoom);
        if (onViewChanged_) onViewChanged_();
    }
    event->accept();
}

void SlidingComparePane::mouseReleaseEvent(QMouseEvent *event) {
    if (event->button() != Qt::LeftButton) return;
    draggingDivider_ = draggingPan_ = false;
    unsetCursor();
    event->accept();
}

void SlidingComparePane::keyPressEvent(QKeyEvent *event) {
    if (event->key() == Qt::Key_Left) {
        divider_ = qBound<qreal>(0.0, divider_ - qreal(stepPx_) / qMax(1, width()), 1.0);
        update();
        event->accept();
    } else if (event->key() == Qt::Key_Right) {
        divider_ = qBound<qreal>(0.0, divider_ + qreal(stepPx_) / qMax(1, width()), 1.0);
        update();
        event->accept();
    } else if (event->key() == Qt::Key_BracketLeft) {
        stepPx_ = qMax(5, stepPx_ - 10);
        event->accept();
    } else if (event->key() == Qt::Key_BracketRight) {
        stepPx_ = qMin(200, stepPx_ + 10);
        event->accept();
    } else {
        QWidget::keyPressEvent(event);
    }
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
    // P-313 side-by-side and P-314 sliding share one View and one rendering
    // rule; the mode switch just swaps which widget presents them.
    stack_ = new QStackedWidget;
    auto onChanged = [this] {
        leftPane_->update();
        rightPane_->update();
        slidingPane_->update();
        updateZoomLabel();
    };
    auto *sideBySide = new QWidget;
    auto *panes = new QHBoxLayout(sideBySide);
    leftPane_ = new ComparePane(&view_, onChanged);
    rightPane_ = new ComparePane(&view_, onChanged);
    panes->addWidget(leftPane_, 1);
    panes->addWidget(rightPane_, 1);
    panes->setContentsMargins(0, 0, 0, 0);
    slidingPane_ = new SlidingComparePane(&view_, onChanged);
    stack_->addWidget(sideBySide);
    stack_->addWidget(slidingPane_);
    layout->addWidget(stack_, 1);
    auto *controls = new QHBoxLayout;
    auto *modeCombo = new QComboBox;
    modeCombo->addItem(QStringLiteral("并排"));
    modeCombo->addItem(QStringLiteral("滑动"));
    fitCheck_ = new QCheckBox(QStringLiteral("适配窗口（统一比例）"));
    auto *resetButton = new QPushButton(QStringLiteral("重置视图"));
    zoomLabel_ = new QLabel;
    sizeNote_ = new QLabel;
    sizeNote_->setWordWrap(true);
    controls->addWidget(new QLabel(QStringLiteral("对比方式")));
    controls->addWidget(modeCombo);
    controls->addSpacing(16);
    controls->addWidget(fitCheck_);
    controls->addWidget(resetButton);
    controls->addWidget(zoomLabel_);
    controls->addStretch(1);
    layout->addLayout(controls);
    layout->addWidget(sizeNote_);
    const auto load = [this](int side) { loadRevision(side); };
    connect(leftCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, load);
    connect(rightCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, load);
    connect(modeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &CompareDialog::switchMode);
    connect(fitCheck_, &QCheckBox::toggled, this, &CompareDialog::applyFit);
    connect(resetButton, &QPushButton::clicked, this, &CompareDialog::resetView);
    loadRevision(0);
    loadRevision(1);
    updateZoomLabel();
}

void CompareDialog::switchMode(int mode) {
    stack_->setCurrentIndex(mode == 0 ? 0 : 1);
    if (mode == 1) slidingPane_->setFocus();
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
    slidingPane_->setImages(leftImage_, rightImage_);
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
    // Uniform ratio for both sides: the smallest fit so both whole images are
    // visible (never stretched past 100%). Blank beyond a smaller image is
    // explicit, not a deformation hiding the size difference. Uses the viewport
    // of the active mode (side-by-side panes or the single sliding pane).
    const QSize leftViewport = stack_->currentIndex() == 0 ? leftPane_->size() : slidingPane_->size();
    const QSize rightViewport = stack_->currentIndex() == 0 ? rightPane_->size() : slidingPane_->size();
    qreal zoom = 1.0;
    bool any = false;
    const auto narrow = [&](const QImage &image, const QSize &viewport) {
        if (image.isNull()) return;
        zoom = qMin(zoom, compareview::fitZoom(image.size(), viewport));
        any = true;
    };
    narrow(leftImage_, leftViewport);
    narrow(rightImage_, rightViewport);
    if (any) {
        view_.zoom = zoom;
        view_.origin = QPointF(0, 0);
        leftPane_->update();
        rightPane_->update();
        slidingPane_->update();
        updateZoomLabel();
    }
}

void CompareDialog::resetView() {
    view_.zoom = 1.0;
    view_.origin = QPointF(0, 0);
    fitCheck_->setChecked(false);
    leftPane_->update();
    rightPane_->update();
    slidingPane_->update();
    updateZoomLabel();
}

void CompareDialog::updateZoomLabel() {
    zoomLabel_->setText(QStringLiteral("缩放 %1%").arg(qRound(view_.zoom * 100)));
}

void CompareDialog::resizeEvent(QResizeEvent *event) {
    QDialog::resizeEvent(event);
    if (fitCheck_->isChecked()) applyFit(true);
}
