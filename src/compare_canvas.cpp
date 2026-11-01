#include "compare_canvas.h"

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
}

void CompareCanvas::setImages(const QImage &left, const QImage &right) {
    left_ = left;
    right_ = right;
    update();
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

qreal CompareCanvas::fitScale() const {
    // One unified scale so BOTH panes fit their viewport; the smaller image
    // simply leaves explicit empty space on its side (never stretched).
    qreal best = 1.0;
    const QList<QPair<QImage, QRect>> panes{{left_, paneRect(0)}, {right_, paneRect(1)}};
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
    painter.save();
    painter.setClipRect(pane);
    painter.fillRect(pane, QColor(0x33, 0x33, 0x35));
    if (image.isNull()) {
        painter.setPen(QColor(0xbb, 0xbb, 0xbb));
        painter.drawText(pane, Qt::AlignCenter, tr("此版本不可预览"));
        painter.restore();
        return;
    }
    // Shared transform: one scale and one origin for both panes. Default and
    // fit modes anchor the image origin (0,0) at the pane's top-left; manual
    // zoom/pan keeps the shared origin.
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
    painter.restore();
}

void CompareCanvas::paintEvent(QPaintEvent *) {
    QPainter painter(this);
    painter.fillRect(rect(), palette().window());
    drawPane(painter, paneRect(0), left_);
    drawPane(painter, paneRect(1), right_);
    // The divider is drawn after the panes so it never vanishes under content.
    const int half = paneRect(0).width();
    painter.fillRect(half, 0, kDivider, height(), QColor(0x88, 0x88, 0x8a));
}

void CompareCanvas::wheelEvent(QWheelEvent *event) {
    if (!hasImages()) return;
    const qreal factor = std::pow(1.15, event->angleDelta().y() / 120.0);
    const qreal old = fit_ ? fitScale() : scale_;
    const qreal next = qBound(kMinZoom, old * factor, kMaxZoom);
    // Zoom about the cursor's image point, keeping it under the cursor. The
    // cursor lies in one pane; the shared origin moves both panes together.
    const QRect pane = paneRect(0).contains(event->position().toPoint())
        ? paneRect(0) : paneRect(1);
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
    if (event->button() == Qt::LeftButton && hasImages()) {
        dragging_ = true;
        dragLast_ = event->pos();
    }
}

void CompareCanvas::mouseMoveEvent(QMouseEvent *event) {
    if (!dragging_) return;
    if (fit_) return; // fit mode is not panned; the shared origin stays (0,0)
    const QPoint delta = event->pos() - dragLast_;
    dragLast_ = event->pos();
    origin_ -= QPoint(qRound(delta.x() / scale_), qRound(delta.y() / scale_));
    clampOrigin(paneRect(0));
    update();
}

void CompareCanvas::mouseReleaseEvent(QMouseEvent *event) {
    if (event->button() == Qt::LeftButton) dragging_ = false;
}

void CompareCanvas::clampOrigin(const QRect &pane) {
    // Keep at least a sliver of the (scaled) image visible in its pane.
    const qreal factor = fit_ ? fitScale() : scale_;
    const int margin = 40;
    origin_.setX(qBound(-qRound(pane.width() / factor) + margin, origin_.x(), margin));
    origin_.setY(qBound(-qRound(pane.height() / factor) + margin, origin_.y(), margin));
}

void CompareCanvas::emitZoom() { emit zoomChanged(fit_ ? fitScale() : scale_); }
