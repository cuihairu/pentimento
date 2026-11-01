#pragma once
#include "filter.h"
#include "library.h"
#include "sentinel.h"
#include "snapshot_schedule.h"
#include "taskqueue.h"
#include "thumbnails.h"
#include <QHash>
#include <QMainWindow>
#include <QMap>
#include <QMutex>
#include <QSet>
#include <memory>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPlainTextEdit;
class QSpinBox;
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
    // P-217: combined filter (name, tags any-of, format, min size, rating),
    // all ANDed. Facts are cached per reload so a filter change re-applies
    // without touching the database; the summary never hides active conditions.
    QLineEdit *tagFilterEdit_;
    QComboBox *formatFilterCombo_;
    QSpinBox *minWidthSpin_;
    QSpinBox *minHeightSpin_;
    QComboBox *ratingFilterCombo_;
    QPushButton *clearFilterBtn_;
    QLabel *filterSummary_;
    QHash<QString, AssetFacts> facts_;
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
    // P-305: whole-tree scheduled snapshots. Off by default; periods 15/30/60
    // minutes; runtime-only (leaving the app pauses the schedule, since exiting
    // is never counted as continuous backup). The next run is enable-time plus
    // one period; a pause (user or system) records its reason; an overlap never
    // stacks (in-progress skips); missed periods collapse to one catch-up run.
    // The pure state machine lives in snapshot_schedule.h; the widgets and the
    // background result channel stay here.
    QComboBox *snapshotPeriodCombo_;
    QPushButton *snapshotPauseBtn_;
    QLabel *snapshotStatus_;
    SnapshotSchedule snapshotSchedule_;
    bool snapshotAsyncDone_ = false;    // worker result, drained by the scan tick
    QString snapshotAsyncError_;
    QPushButton *restore_;
    QPushButton *export_;
    QMap<QString, QPair<qint64, qint64>> observations_;
    QMap<QString, QPair<qint64, qint64>> recorded_;
    // P-304: the stable-change capture runs on a background TaskQueue worker so
    // a large file never blocks the UI thread. The worker writes its result
    // under a mutex; the scan timer drains the results on the UI thread (at
    // most one 2 s tick later). pendingCapture_ gates one capture per asset.
    // Declaration order matters for teardown: captureQueue_ is declared last so
    // it is destroyed first — its worker drains before the result buffers die.
    QMutex captureResultsMutex_;
    QHash<QString, QPair<bool, QString>> captureResults_;
    QSet<QString> pendingCapture_;
    std::unique_ptr<TaskQueue> captureQueue_;
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
    void applyFilter();
    void clearFilter();
    AssetFilter currentFilter() const;
    void updateFilterSummary(const AssetFilter &filter);
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
    // P-305: scheduled-snapshot state machine (period, pause, next run).
    void setSnapshotPeriod(int index);
    void toggleSnapshotPause();
    void updateSnapshotStatus();
    void checkSnapshotSchedule();
    void drainSnapshotResult();
    void submitScheduledSnapshot();
    void rollbackToSnapshot();
    void report(const std::exception &error);
};
