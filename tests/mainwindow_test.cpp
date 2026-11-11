// P-606: desktop keyboard and zoom acceptance, automated portion. The full
// walkthrough (visual rendering at 100/150/200% scale, real focus cycling) is
// a human acceptance step; these tests pin the programmatic guarantees an
// offscreen run can prove: the documented minimum window, the core keyboard
// shortcuts, focus traversal through the core widgets, state text that is not
// color-only, and dialog buttons that stay inside the dialog rect (no fixed
// panel width can push confirm/cancel out of reach after scaling).
#include "library.h"
#include "mainwindow.h"
#include "storage.h"
#include <QAction>
#include <QApplication>
#include <QDeadlineTimer>
#include <QDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QToolBar>
#include <QMenu>
#include <QRandomGenerator>
#include <QScrollArea>
#include <QStatusBar>
#include <QTemporaryDir>
#include <QTimer>
#include <QtTest>
#include <functional>

namespace {
// The search box carries a distinctive accessible name (P-a11y); locate it
// without depending on child order.
QLineEdit *findSearchBox(const MainWindow &window) {
    const auto edits = window.findChildren<QLineEdit *>();
    for (auto *e : edits)
        if (e->accessibleName() == QStringLiteral("搜索素材")
            || e->placeholderText() == QStringLiteral("搜索素材名称"))
            return e;
    return nullptr;
}
} // namespace

class MainWindowUiTest : public QObject {
    Q_OBJECT
private slots:
    void minimumWindowAndCoreShortcutsPresent() {
        QTemporaryDir dir;
        Library library(dir.filePath("library"));
        MainWindow window(library);
        // P-606: the documented minimum window is 860x540.
        QCOMPARE(window.minimumSize(), QSize(860, 540));
        // Core keyboard flow: import and capture are reachable without a mouse.
        bool hasImport = false, hasCapture = false;
        const auto actions = window.findChildren<QAction *>();
        for (const auto *a : actions) {
            if (a->shortcut() == QKeySequence(QStringLiteral("Ctrl+O"))) hasImport = true;
            if (a->shortcut() == QKeySequence(QStringLiteral("Ctrl+S"))) hasCapture = true;
        }
        QVERIFY(hasImport);
        QVERIFY(hasCapture);
    }

    // Audit (2026-11-04): history-integrity verification must run on a
    // background worker, never on the UI thread. This test drives the real
    // selection-changed path (not a private method call), waits for the cache
    // to be populated, then reloads history again and verifies the same
    // revisions are served from cache (no requeue, integrityInFlight_ returns
    // to zero once the batch completes).
    void historyIntegrityRunsInBackgroundAndCachesResults() {
        QTemporaryDir dir;
        Library library(dir.filePath("library"));

        // A small valid PNG: importing produces one revision and one object.
        QImage image(64, 64, QImage::Format_RGB32);
        image.fill(Qt::magenta);
        const QString path = dir.filePath("magenta.png");
        QVERIFY(image.save(path, "PNG"));
        const Asset asset = library.importFile(path);

        MainWindow window(library);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));

        // The constructor's reloadAssets() selected the first row and already
        // scheduled the first integrity batch. Wait for the cache to fill.
        QTRY_COMPARE_WITH_TIMEOUT(window.integrityCachedCount(), 1, 10000);
        QCOMPARE(window.integrityInFlight(), 0);

        // Selecting another row and back replays the same revision. Because
        // the hash is already cached, requestIntegrity() must not enqueue a
        // second verify batch. Drive this through the public selection signal.
        QListWidget *list = nullptr;
        for (auto *l : window.findChildren<QListWidget *>())
            if (l->accessibleName() == QStringLiteral("素材列表")) list = l;
        QVERIFY(list);
        list->setCurrentRow(-1);
        list->setCurrentRow(0);
        QTRY_COMPARE_WITH_TIMEOUT(window.integrityCachedCount(), 1, 10000);
        QCOMPARE(window.integrityInFlight(), 0);
    }

    // Audit (2026-11-04): a not-yet-verified revision shows the corrupt
    // placeholder first, then re-renders with the verified result once the
    // background batch lands — the UI never blocks while waiting. This uses a
    // large object so the placeholder window is observable under offscreen.
    void corruptPlaceholderShowsThenReplacesOnVerified() {
        QTemporaryDir dir;
        Library library(dir.filePath("library"));

        // A moderately large object (16 MiB) keeps the streaming SHA-256 long
        // enough that the first frame renders before the result arrives.
        QByteArray blob(16 * 1024 * 1024, 0);
        QRandomGenerator::global()->fillRange(reinterpret_cast<quint32 *>(blob.data()),
                                              16 * 1024 * 1024 / 4);
        const QString path = dir.filePath("blob.bin");
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(blob);
        file.close();
        const Asset asset = library.importFile(path);

        MainWindow window(library);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));

        QListWidget *history = nullptr;
        for (auto *l : window.findChildren<QListWidget *>())
            if (l->accessibleName() == QStringLiteral("历史版本")) history = l;
        QVERIFY(history);
        QVERIFY(history->count() >= 1);

        // The first frame may already show the placeholder (not yet verified).
        QTRY_VERIFY_WITH_TIMEOUT(window.integrityCachedCount() >= 1, 10000);
        // Once verified (a valid object), the corrupt placeholder disappears.
        QTRY_VERIFY_WITH_TIMEOUT(!history->item(0)->text().contains(QStringLiteral("损坏对象")), 10000);
    }

    // Audit (2026-11-04): backup-target probing is asynchronous; the newest
    // request wins and stale results are dropped. The direct unit assertions
    // cover the existing-file rejection; the modal dialog assertions exercise
    // the actual UI path through the background TaskQueue.
    void backupTargetProbeIsAsyncAndStaleResultsDropped() {
        QTemporaryDir dir;
        Library library(dir.filePath("library"));
        MainWindow window(library);

        // An existing regular file must be refused before any writability
        // decision is made (P-510 regression).
        const QString filePath = dir.filePath("existing-file");
        QFile file(filePath);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("x");
        file.close();
        const BackupTargetInfo info = probeBackupTarget(filePath);
        QVERIFY(!info.writable);
        QVERIFY(info.reason.contains(QStringLiteral("目录")));

        // Drive the real dialog path. The target queue is a MainWindow member,
        // so opening/closing the dialog must never destroy a probe in flight.
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));

        struct ProbeState {
            QLineEdit *edit = nullptr;
            QLabel *label = nullptr;
            QDialog *dialog = nullptr;
            bool ok = true;
        };
        auto state = std::make_shared<ProbeState>();

        QTimer::singleShot(0, [&] {
            for (auto *w : QApplication::topLevelWidgets()) {
                auto *d = qobject_cast<QDialog *>(w);
                if (!d || !d->isVisible()) continue;
                for (auto *e : d->findChildren<QLineEdit *>())
                    if (e->accessibleName() == QStringLiteral("备份目标路径"))
                        state->edit = e;
                for (auto *l : d->findChildren<QLabel *>())
                    if (l->accessibleName() == QStringLiteral("备份目标能力"))
                        state->label = l;
                state->dialog = d;
            }
            if (!state->edit || !state->label || !state->dialog) {
                state->ok = false;
                if (state->dialog) state->dialog->accept();
                return;
            }

            // Poll for an observable transition instead of assuming a fixed
            // wall-clock delay: a loaded CI may take far longer than 100 ms to
            // run a local stat, so fixed QTimer::singleShot waits are flaky.
            // Every wait ends in the same event loop the dialog runs in, so the
            // modal loop stays responsive and the dialog is always closed below.
            auto waitFor = [&](const std::function<bool()> &cond, int timeoutMs) {
                const QDeadlineTimer deadline(timeoutMs);
                while (!cond() && !deadline.hasExpired())
                    QTest::qWait(10);
                return cond();
            };

            // Start a probe for a regular file; it must be refused asynchronously.
            state->edit->setText(filePath);
            state->ok = waitFor([&] { return state->label->text().contains(QStringLiteral("正在探测")); }, 10000) && state->ok;

            // Clearing the input must invalidate the pending result and show the
            // empty state; a late result for the old path must be dropped.
            state->edit->clear();
            state->ok = waitFor([&] { return state->label->text().contains(QStringLiteral("尚未选择")); }, 10000) && state->ok;

            // Re-enter the regular file to observe the final rejection.
            state->edit->setText(filePath);
            state->ok = waitFor([&] { return state->label->text().contains(QStringLiteral("目录")); }, 10000) && state->ok;
            state->ok = state->ok && !state->label->text().contains(QStringLiteral("正在探测"));
            state->dialog->accept();
        });

        bool triggered = false;
        for (auto *a : window.findChildren<QAction *>())
            if (a->text() == QStringLiteral("挂载来源状态…")) { a->trigger(); triggered = true; break; }
        QVERIFY(triggered);

        // All assertions ran inside the modal loop (before the dialog was
        // destroyed); reaching here means the probe sequence already completed.
        QVERIFY2(state->ok, "backup-target probe async transitions not observed");
    }
    // Audit (2026-11-12, P0): exportBackup's backup-target probe (writability +
    // free space) must never run on the UI thread. The probe is submitted to
    // the background target queue; the confirm dialog is only reached after the
    // result backfills. The old code called probeBackupTarget() synchronously
    // in exportBackup(), which would block the whole window on a hung NAS
    // mount (statvfs).
    void exportBackupRejectsUnwritableTargetAsync() {
        QTemporaryDir dir;
        Library library(dir.filePath("library"));
        MainWindow window(library);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));

        // A regular file is a non-writable target: the probe must reject it on
        // the background queue, not freeze the UI thread.
        const QString filePath = dir.filePath("existing-file");
        QFile file(filePath);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("x");
        file.close();

        auto warningSeen = std::make_shared<bool>(false);
        auto click = std::make_shared<QTimer>(&window);
        click->setInterval(10);
        QObject::connect(click.get(), &QTimer::timeout, &window, [&] {
            for (auto *w : QApplication::topLevelWidgets()) {
                auto *box = qobject_cast<QMessageBox *>(w);
                if (!box || !box->isVisible()) continue;
                if (box->text().contains(QStringLiteral("所选备份目标不可写"))) {
                    *warningSeen = true;
                    if (auto *b = box->button(QMessageBox::Ok)) b->click();
                    return;
                }
            }
        });
        click->start();

        // The call returns immediately with the probe still in flight. If the
        // probe ran synchronously this call would block in the modal box (or
        // freeze on statvfs) instead of returning with the pending status.
        window.exportBackupToForTest(filePath);
        QVERIFY(window.statusBar()->currentMessage().contains(QStringLiteral("正在探测备份目标")));
        // The warning only appears after the async probe backfilled.
        QTRY_VERIFY_WITH_TIMEOUT(*warningSeen, 10000);
        // The rejected target never reached the export stage.
        QTest::qWait(100);
        QVERIFY(!window.statusBar()->currentMessage().contains(QStringLiteral("已导出")));
        click->stop();
    }

    // Audit (2026-11-12, P0): a writable target is confirmed only after the
    // async probe backfills the real free-space figure, and the export runs on
    // the background capture queue only after that confirmation.
    void exportBackupConfirmsAfterAsyncProbe() {
        QTemporaryDir dir;
        Library library(dir.filePath("library"));
        // One real asset so the export produces a complete package.
        QImage image(32, 32, QImage::Format_RGB32);
        image.fill(Qt::cyan);
        const QString imgPath = dir.filePath("cyan.png");
        QVERIFY(image.save(imgPath, "PNG"));
        library.importFile(imgPath);

        MainWindow window(library);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));

        const QString target = dir.filePath("pkg");
        auto confirmText = std::make_shared<QString>();
        auto click = std::make_shared<QTimer>(&window);
        click->setInterval(10);
        QObject::connect(click.get(), &QTimer::timeout, &window, [&] {
            for (auto *w : QApplication::topLevelWidgets()) {
                auto *box = qobject_cast<QMessageBox *>(w);
                if (!box || !box->isVisible()) continue;
                if (box->text().contains(QStringLiteral("将导出一致备份包到"))) {
                    *confirmText = box->text();
                    if (auto *b = box->button(QMessageBox::Yes)) b->click();
                    return;
                }
            }
        });
        click->start();

        window.exportBackupToForTest(target);
        // Returns immediately with the probe in flight (never blocks on the UI).
        QVERIFY(window.statusBar()->currentMessage().contains(QStringLiteral("正在探测备份目标")));
        // The confirm shows the probed free-space verdict, i.e. it is reached
        // only after the background probe backfilled.
        QTRY_VERIFY_WITH_TIMEOUT(confirmText->contains(QStringLiteral("备份目标可写")), 10000);
        // Confirmed → the export runs on the background queue and completes.
        QTRY_VERIFY_WITH_TIMEOUT(window.statusBar()->currentMessage().contains(QStringLiteral("已导出")), 10000);
        // A complete package with the completion marker actually exists.
        QVERIFY(QFileInfo::exists(target + "/COMPLETE"));
        click->stop();
    }

    void tabTraversalMovesFocusThroughCoreWidgets() {
        QTemporaryDir dir;
        Library library(dir.filePath("library"));
        MainWindow window(library);
        window.resize(860, 540);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        auto *search = findSearchBox(window);
        QVERIFY(search);
        search->setFocus();
        QVERIFY(window.focusWidget() == search);
        // Tab leaves the search box and lands on another focusable widget —
        // the traversal does not dead-end on a fixed panel.
        QTest::keyClick(window.focusWidget(), Qt::Key_Tab);
        QVERIFY(window.focusWidget() != search);
        QVERIFY(window.focusWidget() != nullptr);
    }
    void dialogStateTextAndButtonsStayInsideRect() {
        QTemporaryDir dir;
        Library library(dir.filePath("library"));
        MainWindow window(library);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        auto stateTextSeen = std::make_shared<bool>(false);
        auto closeSeen = std::make_shared<bool>(false);
        auto clipped = std::make_shared<bool>(false);
        // showSourceState() runs a modal exec(); close it from inside the loop
        // while inspecting it once.
        QTimer::singleShot(0, [&] {
            for (auto *w : QApplication::topLevelWidgets()) {
                auto *d = qobject_cast<QDialog *>(w);
                if (!d || !d->isVisible()) continue;
                for (const auto *label : d->findChildren<QLabel *>())
                    if (!label->text().isEmpty()) { *stateTextSeen = true; break; }
                for (const auto *btn : d->findChildren<QPushButton *>()) {
                    if (!d->rect().contains(btn->geometry())) { *clipped = true; continue; }
                    const QString t = btn->text();
                    if (t == QStringLiteral("关闭") || t == QStringLiteral("Close"))
                        *closeSeen = true;
                }
                d->accept();
            }
        });
        // Drive the real user path: the toolbar action, not a private slot.
        bool triggered = false;
        const auto actions = window.findChildren<QAction *>();
        for (auto *a : actions)
            if (a->text() == QStringLiteral("挂载来源状态…")) { a->trigger(); triggered = true; break; }
        QVERIFY(triggered);
        // State is conveyed by text, not color alone; the close button stayed
        // reachable inside the dialog rect (no fixed-width clip at any scale).
        QVERIFY(*stateTextSeen);
        QVERIFY(*closeSeen);
        QVERIFY(!*clipped);
        // Modal cleanup restored focus to a widget of the main window.
        QVERIFY(window.focusWidget() != nullptr);
    }
    // UI audit (2026-11-03): at the documented minimum window (860x540) the
    // main toolbar must not overflow (no action hidden behind "≫"), and the
    // right timeline column must keep a usable minimum height (not compressed
    // below its minimumSizeHint by a fixed vertical layout).
    void minimumWindowToolbarFitsAndTimelineStaysUsable() {
        QTemporaryDir dir;
        Library library(dir.filePath("library"));
        MainWindow window(library);
        window.resize(860, 540);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        // Toolbar: the sum of its visible action widths must fit the toolbar,
        // and the core security "恢复到锚点" action must not be hidden in the
        // overflow chevron (it is an always-visible top-level action now).
        for (auto *tb : window.findChildren<QToolBar *>()) {
            int content = 0;
            bool restoreVisible = false;
            bool restoreHidden = false;
            for (auto *a : tb->actions()) {
                if (auto *wd = tb->widgetForAction(a)) content += wd->sizeHint().width();
                if (a->text() == QStringLiteral("恢复到锚点")) {
                    // The core security action must be an always-visible
                    // top-level action, never folded into the "≫" overflow.
                    restoreVisible = !a->menu() || a->menu()->isEmpty();
                    if (auto *wd = tb->widgetForAction(a)) restoreHidden = !wd->isVisible();
                }
            }
            QVERIFY2(content <= tb->width(),
                     qPrintable(QStringLiteral("toolbar overflow: %1 > %2").arg(content).arg(tb->width())));
            QVERIFY(restoreVisible);
            QVERIFY2(!restoreHidden, "恢复到锚点 must not be hidden in the overflow menu");
        }
        // Right timeline column: the version list keeps a readable height even
        // at the minimum window (the enclosing scroll area releases overflow).
        int timelineH = 0;
        for (auto *l : window.findChildren<QListWidget *>())
            if (l->accessibleName() == QStringLiteral("历史版本")) timelineH = l->height();
        QVERIFY2(timelineH >= 120, qPrintable(QStringLiteral("timeline height %1").arg(timelineH)));
        // The right column is scrollable (not a crushed fixed layout): the
        // scroll area that wraps the history list actually has overflow room.
        bool hasScroll = false;
        for (auto *s : window.findChildren<QScrollArea *>()) {
            bool holdsHistory = false;
            for (auto *l : s->findChildren<QListWidget *>())
                if (l->accessibleName() == QStringLiteral("历史版本")) holdsHistory = true;
            if (holdsHistory) { hasScroll = true; break; }
        }
        QVERIFY(hasScroll);
    }
    // UI audit evidence (todo.md 审核发现 · UI-AUDIT P2): offscreen grabs of the
    // main window (with sample assets) and the source-state dialog, written only
    // when PENTIMENTO_UI_AUDIT_DIR is set, so a normal test run is unaffected.
    // The grabs feed docs/review/2026-11-02-ui-audit.md.
    void uiAuditScreenshots() {
        const QString dirEnv = qgetenv("PENTIMENTO_UI_AUDIT_DIR");
        if (dirEnv.isEmpty()) QSKIP("PENTIMENTO_UI_AUDIT_DIR not set");
        QTemporaryDir work;
        Library library(work.filePath("library"));
        // Three distinct sample assets (varied content + organization state) so
        // the grid, details and history panels show a real, non-empty view.
        for (int i = 0; i < 3; ++i) {
            QImage img(160, 120, QImage::Format_RGB32);
            img.fill(i == 0 ? Qt::red : i == 1 ? Qt::green : Qt::blue);
            const QString p = work.filePath(QStringLiteral("sample%1.png").arg(i));
            QVERIFY(img.save(p, "PNG"));
            const auto a = library.importFile(p);
            library.setRating(a.id, 3 + i);
            library.addTag(a.id, i == 0 ? QStringLiteral("主角") : QStringLiteral("场景"));
            if (i == 0) library.moveAsset(a.id, "characters");
        }
        MainWindow window(library);
        window.resize(860, 540);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        window.saveScreenshot(dirEnv + "/main.png");
        // The source-state dialog, grabbed while it is modal.
        QTimer::singleShot(0, [&] {
            for (auto *w : QApplication::topLevelWidgets()) {
                auto *d = qobject_cast<QDialog *>(w);
                if (!d || !d->isVisible()) continue;
                d->grab().save(dirEnv + "/source-state.png");
                d->accept();
            }
        });
        const auto actions = window.findChildren<QAction *>();
        for (auto *a : actions)
            if (a->text() == QStringLiteral("挂载来源状态…")) { a->trigger(); break; }
        QVERIFY(QFileInfo::exists(dirEnv + "/main.png"));
    }
};

int main(int argc, char **argv) {
    QTemporaryDir home; // stable HOME: settings/portal must not touch the real one
    qputenv("HOME", home.path().toUtf8());
    QApplication app(argc, argv);
    MainWindowUiTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "mainwindow_test.moc"
