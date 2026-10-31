#pragma once
#include "compareview.h"
#include "library.h"
#include <QDialog>
#include <QImage>
#include <functional>

class QCheckBox;
class QComboBox;
class QLabel;
class QStackedWidget;

// P-313 one side of the side-by-side comparison. Renders a revision's image
// through the shared compareview::View (zoom + origin), and reports every pan or
// zoom through onViewChanged so the other side repaints from the same View. Both
// panes share one View, so the compared position never drifts between sides.
class ComparePane : public QWidget {
    Q_OBJECT
public:
    ComparePane(compareview::View *view, std::function<void()> onViewChanged, QWidget *parent = nullptr);
    void setImage(const QImage &image);
protected:
    void paintEvent(QPaintEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
private:
    compareview::View *view_;
    std::function<void()> onViewChanged_;
    QImage image_;
    QPoint lastDrag_;
    bool dragging_ = false;
};

// P-314 sliding compare: one pane, a draggable vertical divider. Left of the
// divider shows one version, right shows the other, both through the SAME View,
// so the divider inspects the same image-pixel column on both sides at any zoom.
// The divider drags with the mouse and steps with Left/Right arrows (the step is
// adjusted by [ and ]). A failed side is labeled in place, not hidden.
class SlidingComparePane : public QWidget {
    Q_OBJECT
public:
    SlidingComparePane(compareview::View *view, std::function<void()> onViewChanged, QWidget *parent = nullptr);
    void setImages(const QImage &left, const QImage &right);
    void setDividerFraction(qreal fraction); // 0..1 of the pane width
protected:
    void paintEvent(QPaintEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
private:
    int dividerX() const;
    compareview::View *view_;
    std::function<void()> onViewChanged_;
    QImage left_;
    QImage right_;
    qreal divider_ = 0.5;   // fraction of the pane width
    int stepPx_ = 20;       // arrow-key divider step
    bool draggingDivider_ = false;
    bool draggingPan_ = false;
    QPoint lastDrag_;
};

// P-313/P-314 dual-version comparison, side-by-side or sliding, with
// synchronized zoom and pan: one shared zoom ratio AND one shared origin, so the
// same position and scale can be compared repeatedly on both sides. Default
// top-left aligned; an optional fit-to-window shows each whole image at a
// uniform (never stretched) ratio; a differing image size is labeled rather
// than silently stretched.
class CompareDialog : public QDialog {
    Q_OBJECT
public:
    CompareDialog(const QString &assetName, const QVector<TimelineEntry> &revisions,
                  int defaultLeft, Library &library, QWidget *parent = nullptr);
protected:
    void resizeEvent(QResizeEvent *event) override;
private:
    void loadRevision(int side);
    void applyFit(bool fit);
    void resetView();
    void updateZoomLabel();
    void switchMode(int mode);
    compareview::View view_;
    QVector<TimelineEntry> revisions_;
    Library &library_;
    QComboBox *leftCombo_ = nullptr;
    QComboBox *rightCombo_ = nullptr;
    QStackedWidget *stack_ = nullptr;
    ComparePane *leftPane_ = nullptr;
    ComparePane *rightPane_ = nullptr;
    SlidingComparePane *slidingPane_ = nullptr;
    QLabel *zoomLabel_ = nullptr;
    QLabel *sizeNote_ = nullptr;
    QCheckBox *fitCheck_ = nullptr;
    QImage leftImage_;
    QImage rightImage_;
};
