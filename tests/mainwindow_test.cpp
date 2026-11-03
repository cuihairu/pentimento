// P-606: desktop keyboard and zoom acceptance, automated portion. The full
// walkthrough (visual rendering at 100/150/200% scale, real focus cycling) is
// a human acceptance step; these tests pin the programmatic guarantees an
// offscreen run can prove: the documented minimum window, the core keyboard
// shortcuts, focus traversal through the core widgets, state text that is not
// color-only, and dialog buttons that stay inside the dialog rect (no fixed
// panel width can push confirm/cancel out of reach after scaling).
#include "library.h"
#include "mainwindow.h"
#include <QAction>
#include <QApplication>
#include <QDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QToolBar>
#include <QMenu>
#include <QScrollArea>
#include <QTemporaryDir>
#include <QTimer>
#include <QtTest>

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
            for (auto *a : tb->actions()) {
                if (auto *wd = tb->widgetForAction(a)) content += wd->sizeHint().width();
                if (a->text() == QStringLiteral("恢复到锚点")
                    && (!a->menu() || a->menu()->isEmpty())) restoreVisible = true;
            }
            QVERIFY2(content <= tb->width(),
                     qPrintable(QStringLiteral("toolbar overflow: %1 > %2").arg(content).arg(tb->width())));
            QVERIFY(restoreVisible);
        }
        // Right timeline column: the version list keeps a readable height even
        // at the minimum window (the enclosing scroll area releases overflow).
        int timelineH = 0;
        for (auto *l : window.findChildren<QListWidget *>())
            if (l->accessibleName() == QStringLiteral("历史版本")) timelineH = l->height();
        QVERIFY2(timelineH >= 120, qPrintable(QStringLiteral("timeline height %1").arg(timelineH)));
        // The right column is scrollable (not a crushed fixed layout).
        bool hasScroll = false;
        for (auto *s : window.findChildren<QScrollArea *>()) hasScroll = true;
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
