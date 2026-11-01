#pragma once

#include <QImage>
#include <QWidget>

// Preview canvas (P-209). Paints a decoded image at either a fit-to-window zoom
// (aspect preserved) or a manual factor, over an optional checkerboard backdrop
// that reveals transparency. Fit mode fills the viewport (no scrollbars); manual
// zoom sizes the widget so the surrounding scroll area pans. Every zoom is
// uniform (one scale for both axes), so the displayed aspect always equals the
// source and the image is never stretched to the window.
class PreviewCanvas : public QWidget {
    Q_OBJECT
public:
    explicit PreviewCanvas(QWidget *parent = nullptr);

    void setImage(const QImage &image);
    void setFit(bool fit);
    void setZoom(qreal factor); // manual zoom; disables fit mode
    void setCheckerboard(bool on);
    qreal effectiveZoom() const; // display scale (fit recomputed on resize)
    bool isFit() const { return fit_; }

    QSize sizeHint() const override;

signals:
    void zoomChanged(qreal factor);

protected:
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

private:
    QImage image_;
    qreal zoom_ = 1.0;
    bool fit_ = true;
    bool checker_ = true;
    void emitZoom();
};
