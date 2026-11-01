#include "preview_canvas.h"
#include "preview.h"

#include <QPainter>
#include <QPaintEvent>
#include <QPalette>
#include <QResizeEvent>
#include <QtGlobal>
#include <QtMath>

namespace {
constexpr int kCheckerSize = 8; // squares of the transparency backdrop

// One 2x2 checkerboard tile painted under the image; transparency reads as a
// neutral, familiar backdrop instead of an invisible hole (P-209).
QPixmap checkerTile() {
    QPixmap tile(kCheckerSize * 2, kCheckerSize * 2);
    tile.fill(QColor(0xcc, 0xcc, 0xcc));
    QPainter p(&tile);
    p.fillRect(0, 0, kCheckerSize, kCheckerSize, QColor(0xff, 0xff, 0xff));
    p.fillRect(kCheckerSize, kCheckerSize, kCheckerSize, kCheckerSize, QColor(0xff, 0xff, 0xff));
    return tile;
}

QSize scaledTo(const QSize &size, qreal factor) {
    return (QSizeF(size) * factor).toSize().expandedTo(QSize(1, 1));
}
} // namespace

PreviewCanvas::PreviewCanvas(QWidget *parent) : QWidget(parent) {
    setMinimumSize(80, 80);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
}

void PreviewCanvas::setImage(const QImage &image) {
    image_ = image;
    emitZoom();
    updateGeometry();
    update();
}

void PreviewCanvas::setFit(bool fit) {
    if (fit_ == fit) return;
    fit_ = fit;
    emitZoom();
    updateGeometry();
    update();
}

void PreviewCanvas::setZoom(qreal factor) {
    fit_ = false;
    zoom_ = qBound(0.05, factor, 16.0);
    emitZoom();
    updateGeometry();
    update();
}

void PreviewCanvas::setCheckerboard(bool on) {
    if (checker_ == on) return;
    checker_ = on;
    update();
}

qreal PreviewCanvas::effectiveZoom() const {
    if (fit_) return preview::fitScale(image_.size(), size());
    return zoom_;
}

QSize PreviewCanvas::sizeHint() const {
    if (image_.isNull()) return QSize(0, 0);
    // Fit: a tiny hint lets the scroll area expand us to the full viewport.
    // Manual: exact scaled size, so scrollbars pan across the image.
    return fit_ ? QSize(1, 1) : scaledTo(image_.size(), zoom_);
}

void PreviewCanvas::paintEvent(QPaintEvent *) {
    if (image_.isNull()) return;
    const qreal factor = effectiveZoom();
    const QSize scaled = scaledTo(image_.size(), factor);
    const QRect target = fit_
        ? QRect(QPoint((width() - scaled.width()) / 2, (height() - scaled.height()) / 2), scaled)
        : QRect(QPoint(12, 12), scaled); // manual zoom: fixed origin, scroll pans
    QPainter p(this);
    p.fillRect(rect(), palette().window());
    if (checker_ && image_.hasAlphaChannel())
        p.fillRect(target, QBrush(checkerTile()));
    else
        p.fillRect(target, QColor(0xe5, 0xe5, 0xe2));
    // Smooth only when downscaling or at a fractional factor: integer upscale
    // stays crisp so pixel-level inspection is honest.
    const bool fractional = qAbs(factor - qRound(factor)) > 0.001;
    p.setRenderHint(QPainter::SmoothPixmapTransform, factor < 1.0 || fractional);
    p.drawImage(target, image_);
}

void PreviewCanvas::resizeEvent(QResizeEvent *) {
    if (fit_) emitZoom(); // fit zoom tracks the viewport size
}

void PreviewCanvas::emitZoom() { emit zoomChanged(effectiveZoom()); }
