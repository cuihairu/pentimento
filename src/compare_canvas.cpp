#include "compare_canvas.h"

#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPaintEvent>
#include <QWheelEvent>
#include <QtMath>
#include <cmath>

namespace {
constexpr int kDivider = 1;     // px gap between the two panes
constexpr qreal kMinZoom = 0.05;
constexpr qreal kMaxZoom = 16.0;
constexpr int kChecker = 8;     // transparency backdrop tile

QSize scaledTo(const QSize &size, qreal factor) {
    return (QSizeF(size) * factor).toSize().expandedTo(QSize(1, 1));
}

QPixmap checkerTile() {
    QPixmap tile(kChecker * 2, kChecker * 2);
    tile.fill(QColor(0xcc, 0xcc, 0xcc));
    QPainter p(&tile);
    p.fillRect(0, 0, kChecker, kChecker, QColor(0xff, 0xff, 0xff));
    p.fillRect(kChecker, kChecker, kChecker, kChecker, QColor(0xff, 0xff, 0xff));
    return tile;
}
} // namespace

CompareCanvas::CompareCanvas(QWidget *parent) : QWidget(parent) {
    setMinimumSize(160, 160);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    setFocusPolicy(Qt::StrongFocus); // P-314: Left/Right step, [ ] adjust step
}

void CompareCanvas::setImages(const QImage &left, const QImage &right) {
    left_ = left;
    right_ = right;
    update();
}

void CompareCanvas::setSliding(bool on) {
    if (sliding_ == on) return;
    sliding_ = on;
    if (on) dividerX_ = qMax(0, origin_.x() + qRound(viewport().width() / (2.0 * zoom())));
    update();
}

void CompareCanvas::setDivider(qreal imageX) {
    dividerX_ = imageX;
    update();
}

void CompareCanvas::stepDivider(int direction) {
    const qreal step = dividerStepPx_ / (fit_ ? fitScale() : scale_);
    dividerX_ += direction * step;
    update();
}

void CompareCanvas::setDividerStep(qreal px) {
    dividerStepPx_ = qBound(4.0, px, 400.0);
}

int CompareCanvas::dividerViewportX() const {
    const qreal factor = fit_ ? fitScale() : scale_;
    const QRect pane = viewport();
    return pane.left() + qRound((dividerX_ - origin_.x()) * factor);
}

void CompareCanvas::setFit(bool fit) {
    if (fit_ == fit) return;
    fit_ = fit;
    emitZoom();
    update();
}

void CompareCanvas::setZoom(qreal factor) {
    fit_ = false;
    scale_ = qBound(kMinZoom, factor, kMaxZoom);
    clampOrigin(paneRect(0));
    emitZoom();
    update();
}

QRect CompareCanvas::paneRect(int index) const {
    const int half = (width() - kDivider) / 2;
    if (index == 0) return QRect(0, 0, half, height());
    return QRect(half + kDivider, 0, width() - half - kDivider, height());
}

QRect CompareCanvas::viewport() const {
    return sliding_ ? rect() : paneRect(0);
}

qreal CompareCanvas::fitScale() const {
    // One unified scale so BOTH panes fit their viewport; the smaller image
    // simply leaves explicit empty space on its side (never stretched). In
    // sliding mode both share the full viewport, so both must fit it.
    qreal best = 1.0;
    const QList<QPair<QImage, QRect>> panes = sliding_
        ? QList<QPair<QImage, QRect>>{{left_, viewport()}, {right_, viewport()}}
        : QList<QPair<QImage, QRect>>{{left_, paneRect(0)}, {right_, paneRect(1)}};
    bool any = false;
    for (const auto &pair : panes) {
        if (pair.first.isNull()) continue;
        const qreal sx = pair.second.width() / qreal(pair.first.width());
        const qreal sy = pair.second.height() / qreal(pair.first.height());
        const qreal f = qMin(sx, sy);
        best = any ? qMin(best, f) : f;
        any = true;
    }
    return any ? qBound(kMinZoom, best, kMaxZoom) : 1.0;
}

void CompareCanvas::drawPane(QPainter &painter, const QRect &pane, const QImage &image) {
    // Shared transform: one scale and one origin for both panes. Default and
    // fit modes anchor the image origin (0,0) at the pane's top-left; manual
    // zoom/pan keeps the shared origin. The caller owns the clip (so the
    // sliding wipe can clip to a region); the image is drawn without touching
    // it. A null image draws the "cannot preview" message into the pane.
    if (image.isNull()) {
        painter.setPen(QColor(0xbb, 0xbb, 0xbb));
        painter.drawText(pane, Qt::AlignCenter, tr("此版本不可预览"));
        return;
    }
    const qreal factor = fit_ ? fitScale() : scale_;
    const QPoint topLeft = fit_
        ? pane.topLeft()
        : pane.topLeft() - QPoint(qRound(origin_.x() * factor), qRound(origin_.y() * factor));
    const QSize scaled = scaledTo(image.size(), factor);
    const QRect target(topLeft, scaled);
    if (image.hasAlphaChannel())
        painter.fillRect(target, QBrush(checkerTile()));
    else
        painter.fillRect(target, QColor(0xe5, 0xe5, 0xe2));
    const bool fractional = qAbs(factor - qRound(factor)) > 0.001;
    painter.setRenderHint(QPainter::SmoothPixmapTransform, factor < 1.0 || fractional);
    painter.drawImage(target, image);
}

void CompareCanvas::paintEvent(QPaintEvent *) {
    QPainter painter(this);
    painter.fillRect(rect(), palette().window());
    if (!sliding_) {
        // Each pane owns its clip and background; the image content is drawn by
        // drawPane under the shared transform.
        const QRect leftPane = paneRect(0);
        const QRect rightPane = paneRect(1);
        painter.setClipRect(leftPane);
        painter.fillRect(leftPane, QColor(0x33, 0x33, 0x35));
        drawPane(painter, leftPane, left_);
        painter.setClipRect(rightPane);
        painter.fillRect(rightPane, QColor(0x33, 0x33, 0x35));
        drawPane(painter, rightPane, right_);
        painter.setClipping(false);
        // The divider is drawn after the panes so it never vanishes under content.
        const int half = leftPane.width();
        painter.fillRect(half, 0, kDivider, height(), QColor(0x88, 0x88, 0x8a));
        return;
    }
    // P-314 sliding mode: one viewport, the divider wiping between the two
    // versions at the SAME image coordinate. Left of the divider shows the left
    // image, right of it the right image — the shared transform makes the wipe
    // line correspond to the same content position in both, and differing
    // sizes stay honest (empty where one side has no content, never stretched).
    const int divX = dividerViewportX();
    const QRect pane = viewport();
    painter.setClipRect(pane);
    painter.fillRect(pane, QColor(0x33, 0x33, 0x35));
    if (left_.isNull() && right_.isNull()) drawPane(painter, pane, QImage());
    const QRect leftRegion(pane.left(), pane.top(), qMax(0, divX - pane.left()), pane.height());
    const QRect rightRegion(divX, pane.top(), qMax(0, pane.right() - divX), pane.height());
    if (leftRegion.width() > 0) {
        painter.setClipRect(leftRegion);
        drawPane(painter, pane, left_);
    }
    if (rightRegion.width() > 0) {
        painter.setClipRect(rightRegion);
        drawPane(painter, pane, right_);
    }
    painter.setClipping(false);
    painter.fillRect(divX - kDivider, 0, kDivider * 2 + 1, height(), QColor(0xff, 0xff, 0xff));
    painter.fillRect(divX, 0, kDivider, height(), QColor(0x1a1a6e));
    // Grip: three ticks centered on the divider line.
    painter.setPen(QColor(0xff, 0xff, 0xff));
    const int cy = height() / 2;
    for (int i = -1; i <= 1; ++i)
        painter.drawLine(divX - 6, cy + i * 5, divX + 6, cy + i * 5);
}

void CompareCanvas::wheelEvent(QWheelEvent *event) {
    if (!hasImages()) return;
    const qreal factor = std::pow(1.15, event->angleDelta().y() / 120.0);
    const qreal old = fit_ ? fitScale() : scale_;
    const qreal next = qBound(kMinZoom, old * factor, kMaxZoom);
    // Zoom about the cursor's image point, keeping it under the cursor. The
    // cursor lies in one pane (or the single viewport in sliding mode); the
    // shared origin moves both panes together.
    const QRect pane = sliding_ ? viewport()
        : (paneRect(0).contains(event->position().toPoint()) ? paneRect(0) : paneRect(1));
    const QPointF local = event->position() - pane.topLeft();
    const QPointF imagePt = QPointF(origin_) + local / old;
    fit_ = false;
    scale_ = next;
    origin_ = QPoint(qRound(imagePt.x() - local.x() / next),
                     qRound(imagePt.y() - local.y() / next));
    clampOrigin(pane);
    emitZoom();
    update();
    event->accept();
}

void CompareCanvas::mousePressEvent(QMouseEvent *event) {
    if (event->button() != Qt::LeftButton || !hasImages()) return;
    if (sliding_ && qAbs(event->pos().x() - dividerViewportX()) <= 6) {
        draggingDivider_ = true;
        return;
    }
    dragging_ = true;
    dragLast_ = event->pos();
}

void CompareCanvas::mouseMoveEvent(QMouseEvent *event) {
    if (draggingDivider_) {
        // Divider follows the cursor, stored in image space so a later zoom
        // keeps it on the SAME content coordinate in both versions.
        const qreal factor = fit_ ? fitScale() : scale_;
        const QRect pane = viewport();
        dividerX_ = origin_.x() + (event->pos().x() - pane.left()) / factor;
        update();
        return;
    }
    if (!dragging_) return;
    if (fit_) return; // fit mode is not panned; the shared origin stays (0,0)
    const QPoint delta = event->pos() - dragLast_;
    dragLast_ = event->pos();
    origin_ -= QPoint(qRound(delta.x() / scale_), qRound(delta.y() / scale_));
    clampOrigin(paneRect(0));
    update();
}

void CompareCanvas::mouseReleaseEvent(QMouseEvent *event) {
    if (event->button() != Qt::LeftButton) return;
    dragging_ = false;
    draggingDivider_ = false;
}

void CompareCanvas::keyPressEvent(QKeyEvent *event) {
    if (!sliding_) { QWidget::keyPressEvent(event); return; }
    // P-314: Left/Right step the divider; [ / ] adjust the step size.
    switch (event->key()) {
    case Qt::Key_Left:  stepDivider(-1); event->accept(); return;
    case Qt::Key_Right: stepDivider(+1); event->accept(); return;
    case Qt::Key_BracketLeft:
        setDividerStep(dividerStepPx_ / 2.0); event->accept(); return;
    case Qt::Key_BracketRight:
        setDividerStep(dividerStepPx_ * 2.0); event->accept(); return;
    default:
        QWidget::keyPressEvent(event);
    }
}

void CompareCanvas::clampOrigin(const QRect &pane) {
    // Keep at least a sliver of the (scaled) image visible in its pane.
    const qreal factor = fit_ ? fitScale() : scale_;
    const int margin = 40;
    origin_.setX(qBound(-qRound(pane.width() / factor) + margin, origin_.x(), margin));
    origin_.setY(qBound(-qRound(pane.height() / factor) + margin, origin_.y(), margin));
}

void CompareCanvas::emitZoom() { emit zoomChanged(fit_ ? fitScale() : scale_); }
