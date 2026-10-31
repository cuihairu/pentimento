#pragma once
#include "library.h"
#include "sentinel.h"
#include <QMainWindow>
#include <memory>

class QListWidget;
class QLabel;
class QLineEdit;
class QPushButton;
class QAction;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(Library &library);
    void saveScreenshot(const QString &path);
private:
    Library &library_;
    QVector<Asset> assets_;
    // P-308: the timeline is the library's AssetTimeline (deterministic id
    // order, content-derived current mark, restore/corrupt marks).
    QVector<TimelineEntry> revisions_;
    bool unrecorded_ = false;
    QListWidget *assetsList_;
    QListWidget *historyList_;
    QLabel *preview_;
    QLabel *details_;
    QLabel *hint_;
    QLineEdit *search_;
    QPushButton *restore_;
    QPushButton *export_;
    QPushButton *compare_;
    QLabel *scheduleLabel_;
    QAction *pauseAction_;
    std::unique_ptr<Sentinel> sentinel_;
    Asset selected() const;
    void pollSentinel();
    void reloadAssets(const QString &selectId = QString());
    void reloadHistory();
    void showRevision();
    void importFiles();
    void capture();
    void restore();
    void exportFile();
    // P-313 dual-version side-by-side comparison from the selected revision.
    void compareVersions();
    void scanChanges();
    void rollbackToSnapshot();
    // P-312 read-only time-point directory browsing: pick a snapshot, see the
    // directory tree and deletion/exclusion state as of that moment, from the
    // snapshot manifest only (never the current index).
    void browseTimePoint();
    void updateScheduleStatus();
    void report(const std::exception &error);
protected:
    void resizeEvent(QResizeEvent *event) override;
};
