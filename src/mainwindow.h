#pragma once
#include "library.h"
#include <QMainWindow>
#include <QMap>

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
    Asset selected() const;
    void reloadAssets(const QString &selectId = QString());
    void reloadHistory();
    void showRevision();
    void importFiles();
    void capture();
    void restore();
    void exportFile();
    void scanChanges();
    void report(const std::exception &error);
protected:
    void resizeEvent(QResizeEvent *event) override;
};
