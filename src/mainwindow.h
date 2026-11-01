#pragma once
#include "library.h"
#include "sentinel.h"
#include "thumbnails.h"
#include <QMainWindow>
#include <QMap>
#include <memory>

class QCheckBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QScrollArea;
class QStackedWidget;
class QToolButton;
class PreviewCanvas;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(Library &library);
    void saveScreenshot(const QString &path);
private:
    Library &library_;
    QVector<Asset> assets_;
    QVector<Revision> revisions_;
    QListWidget *assetsList_;
    QListWidget *historyList_;
    QLabel *preview_;          // message panel (corrupt / unsupported format)
    QLabel *details_;
    QLineEdit *search_;
    // P-209: the preview is a canvas in a scroll area behind a control bar; the
    // message label and the canvas swap via a stack. Zoom is uniform (never
    // stretches), and a checkerboard toggles the transparency backdrop.
    QStackedWidget *previewStack_;
    QScrollArea *previewScroll_;
    PreviewCanvas *canvas_;
    QToolButton *fitBtn_;
    QToolButton *hundredBtn_;
    QToolButton *zoomOutBtn_;
    QToolButton *zoomInBtn_;
    QLabel *zoomLabel_;
    QCheckBox *checkerCheck_;
    QPushButton *restore_;
    QPushButton *export_;
    QMap<QString, QPair<qint64, qint64>> observations_;
    QMap<QString, QPair<qint64, qint64>> recorded_;
    std::unique_ptr<Sentinel> sentinel_;
    ThumbnailCache thumbnails_;
    Asset selected() const;
    void pollSentinel();
    void reloadAssets(const QString &selectId = QString());
    void reloadHistory();
    // P-208: one shared model, two render modes (grid/list); selection is held by
    // stable asset id so a filter/sort/view change never selects another asset.
    void setAssetViewMode(bool grid);
    void keepSelectionVisible();
    void showRevision();
    void updateZoomLabel();
    void syncZoomButtons(bool fit);
    void importFiles();
    void capture();
    void restore();
    void exportFile();
    void scanChanges();
    void rollbackToSnapshot();
    void report(const std::exception &error);
};
