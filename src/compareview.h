#pragma once
#include <QPointF>
#include <QRect>
#include <QRectF>
#include <QSize>
#include <QtMath>

// P-313 shared comparison rendering geometry. Both panes of the side-by-side
// view (and the sliding compare, P-314) render through these rules, so a
// position compared on one side is the identical image pixel on the other at the
// same scale — 同一位置和比例可重复比较. Pure math (Qt Core types only), so the
// rules are unit-testable without a GUI.
namespace compareview {

// One pane's shared view state. Both panes hold the SAME View: zoom is the
// uniform scale (1.0 = image pixels), origin is the image pixel that appears at
// the viewport's top-left. A pan or zoom updates the shared View once and both
// panes repaint from it, so the compared position never drifts between sides.
struct View {
    qreal zoom = 1.0;
    QPointF origin; // (0,0) = top-left aligned
};

// Uniform fit-to-window ratio: the whole image visible, never stretched past
// 100% (no auto-upscale). Returns 1.0 for an empty image or viewport.
inline qreal fitZoom(const QSize &image, const QSize &viewport) {
    if (image.isEmpty() || viewport.isEmpty()) return 1.0;
    return qMin<qreal>(1.0, qMin<qreal>(qreal(viewport.width()) / image.width(),
                                        qreal(viewport.height()) / image.height()));
}

// Clamp the origin per axis. Zoomed in: the image keeps covering the viewport
// (no blank while inspecting). Zoomed out (image smaller than the viewport):
// top-left aligned at 0, so the empty area beyond the image is explicit rather
// than stretched.
inline QPointF clampOrigin(const QSize &image, const QSize &viewport, qreal zoom, QPointF origin) {
    const auto axis = [zoom](qreal o, int imagePx, int viewportPx) -> qreal {
        if (imagePx * zoom <= viewportPx) return 0.0; // whole image visible, top-left
        return qBound<qreal>(0.0, o, imagePx - viewportPx / zoom);
    };
    return {axis(origin.x(), image.width(), viewport.width()),
            axis(origin.y(), image.height(), viewport.height())};
}

// The image pixel rect currently visible in the viewport at `zoom` with the
// viewport top-left at `origin`. Both panes call this with the SAME View, so
// the region compared is identical in image coordinates on both sides.
inline QRect sourceRect(const QSize &image, const QSize &viewport, qreal zoom, QPointF origin) {
    if (image.isEmpty() || viewport.isEmpty()) return {};
    const QPointF clamped = clampOrigin(image, viewport, zoom, origin);
    const qreal w = qMin<qreal>(image.width(), qreal(viewport.width()) / zoom);
    const qreal h = qMin<qreal>(image.height(), qreal(viewport.height()) / zoom);
    return QRectF(clamped.x(), clamped.y(), w, h)
        .intersected(QRectF(QPointF(0, 0), QSizeF(image))).toAlignedRect();
}

// Where the image lands in viewport coordinates (for painting), given the same
// View. The same clamped origin/zoom gives both panes the same geometry.
inline QRect targetRect(const QSize &image, const QSize &viewport, qreal zoom, QPointF origin) {
    if (image.isEmpty() || viewport.isEmpty()) return {};
    const QPointF clamped = clampOrigin(image, viewport, zoom, origin);
    return QRectF(QPointF(-clamped.x() * zoom, -clamped.y() * zoom),
                  QSizeF(image.width() * zoom, image.height() * zoom)).toAlignedRect();
}

// P-314 sliding divider. The divider is a viewport x; both versions render
// through the SAME View, so the image-pixel column it exposes is identical on
// both sides — 拖动分界线检查的是两版同坐标区域, and that holds at any zoom
// (缩放后同坐标, because the mapping depends only on the shared View).
inline qreal dividerToImageX(qreal viewportX, const View &view) {
    return view.origin.x() + viewportX / view.zoom;
}
// Inverse: the viewport x at which an image-pixel column appears.
inline qreal imageXToViewport(qreal imageX, const View &view) {
    return (imageX - view.origin.x()) * view.zoom;
}

} // namespace compareview
