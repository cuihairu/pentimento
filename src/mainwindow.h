#pragma once
#include "library.h"
#include "sentinel.h"
#include "thumbnails.h"
#include <QMainWindow>
#include <QMap>
#include <memory>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPlainTextEdit;
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
    // P-213: tag maintenance for the selected asset (add/remove, result shown
    // in a list) plus batch add/remove over the filtered set (explicit, never
    // a blind "replace all tags").
    QListWidget *tagsList_;
    QLineEdit *tagInput_;
    QPushButton *tagAddBtn_;
    QPushButton *tagRemoveBtn_;
    // P-214: per-asset rating (0-5 stars plus a distinct "no rating") and a
    // free-form note. The note saves on an explicit button or on focus loss,
    // always with a clear "saved" message so it never looks silently kept.
    QComboBox *ratingCombo_;
    QPlainTextEdit *noteEdit_;
    QPushButton *noteSaveBtn_;
    // Asset whose note/rating the editor currently represents. Focus-loss saves
    // go here (not to selected()) so switching assets mid-edit never writes the
    // old note into the newly selected asset.
    QString noteAssetId_;
    // P-215: manual source/author/license records (append-only, old entries
    // stay viewable). Evidence files are stored as library objects; a pending
    // attachment is held until the entry is recorded.
    QLineEdit *sourceInput_;
    QLineEdit *authorInput_;
    QLineEdit *licenseInput_;
    QPushButton *attachEvidenceBtn_;
    QPushButton *recordProvBtn_;
    QPushButton *exportEvidenceBtn_;
    QLabel *evidenceLabel_;
    QListWidget *provenanceList_;
    QString pendingEvidenceHash_;
    QString pendingEvidenceName_;
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
    void reloadTags();
    void addTagToSelected();
    void removeSelectedTag();
    void batchAddTag();
    void batchRemoveTag();
    void reloadRatingNote();
    void applyRating(int index);
    void saveNote();
    void reloadProvenance();
    void attachEvidence();
    void recordProvenance();
    void exportEvidenceFile();
    void renameSelected();
    void moveSelected();
    QStringList visibleAssetIds() const;
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
