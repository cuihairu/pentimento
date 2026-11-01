#pragma once
#include "library.h"
#include "sentinel.h"
#include "thumbnails.h"
#include <QMainWindow>
#include <QMap>
#include <memory>

class QListWidget;
class QLabel;
class QLineEdit;
class QPushButton;

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
    QLabel *preview_;
    QLabel *details_;
    QLineEdit *search_;
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
    void importFiles();
    void capture();
    void restore();
    void exportFile();
    void scanChanges();
    void rollbackToSnapshot();
    void report(const std::exception &error);
protected:
    void resizeEvent(QResizeEvent *event) override;
};
