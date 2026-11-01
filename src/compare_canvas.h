#pragma once

#include <QImage>
#include <QPoint>
#include <QWidget>

// P-313: two versions rendered under ONE shared image-coordinate transform —
// the same zoom scale and the same origin — so a side-by-side (or, later, a
// sliding-divider) comparison never diverges between the two panes. Default is
// top-left aligned (never stretched); the optional "fit window" mode applies a
// single unified scale to both. Pan and zoom always move both panes together,
// so a point in one image corresponds to the same coordinate in the other.
// The empty area left by a smaller image reads as an explicit "no content on
// this side" region, never a stretched fill.
class CompareCanvas : public QWidget {
    Q_OBJECT
public:
    explicit CompareCanvas(QWidget *parent = nullptr);

    void setImages(const QImage &left, const QImage &right);
    void setFit(bool fit);
    bool isFit() const { return fit_; }
    void setZoom(qreal factor); // manual zoom; disables fit mode
    qreal zoom() const { return fit_ ? fitScale() : scale_; }
    // Shared image-space origin of the two panes (inspection/test hook): there
    // is exactly one, which is what keeps pan/zoom synchronized.
    QPoint origin() const { return origin_; }
    bool hasImages() const { return !left_.isNull() || !right_.isNull(); }

signals:
    void zoomChanged(qreal factor);

protected:
    void paintEvent(QPaintEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;

private:
    QImage left_;
    QImage right_;
    qreal scale_ = 1.0;
    // Shared image-space point that sits at the viewport's top-left. One origin
    // for both panes, so panning/zooming never desynchronizes them.
    QPoint origin_;
    bool fit_ = true;
    QPoint dragLast_;
    bool dragging_ = false;

    QRect paneRect(int index) const; // 0 = left, 1 = right
    qreal fitScale() const;          // one unified scale so both panes fit
    void drawPane(QPainter &painter, const QRect &pane, const QImage &image);
    void clampOrigin(const QRect &pane);
    void emitZoom();
};
