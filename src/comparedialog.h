#pragma once
#include "compareview.h"
#include "library.h"
#include <QDialog>
#include <QImage>
#include <functional>

class QCheckBox;
class QComboBox;
class QLabel;

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

// P-313 dual-version side-by-side comparison with synchronized zoom and pan:
// one shared zoom ratio AND one shared origin, so the same position and scale
// can be compared repeatedly on both sides. Default top-left aligned; an
// optional fit-to-window shows each whole image at a uniform (never stretched)
// ratio; a differing image size is labeled rather than silently stretched.
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
    compareview::View view_;
    QVector<TimelineEntry> revisions_;
    Library &library_;
    QComboBox *leftCombo_ = nullptr;
    QComboBox *rightCombo_ = nullptr;
    ComparePane *leftPane_ = nullptr;
    ComparePane *rightPane_ = nullptr;
    QLabel *zoomLabel_ = nullptr;
    QLabel *sizeNote_ = nullptr;
    QCheckBox *fitCheck_ = nullptr;
    QImage leftImage_;
    QImage rightImage_;
};
