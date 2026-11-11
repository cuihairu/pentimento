// Canvas widgets: shared compare transform, sliding divider coordinates and preview canvas size hints.
// (LAY-001: suite split out of the former tests/library_test.cpp.)
#include "library_test_helpers.h"

#include "library.h"
#include "backend.h"
#include "compare_canvas.h"
#include "preview_canvas.h"
#include "exclusion.h"
#include "filter.h"
#include "i18n.h"
#include "importer.h"
#include "platform_immutable.h"
#include "preview.h"
#include "recent.h"
#include "snapshot_schedule.h"
#include "source_connection.h"
#include "storage.h"
#include "taskqueue.h"
#include "thumbnails.h"
#include <QWheelEvent>
#include <QApplication>
#include <QTranslator>
#include <QBuffer>
#include <QCryptographicHash>
#include <QDataStream>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QImageReader>
#include <QRegularExpression>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QtTest>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <thread>
#include <chrono>

class CanvasCompareTest : public QObject {
    Q_OBJECT
private slots:
    void compareCanvasSharesOneTransform() {
        CompareCanvas canvas;
        canvas.resize(400, 300);
        QImage left(100, 50, QImage::Format_RGB32);
        QImage right(40, 80, QImage::Format_RGB32);
        left.fill(Qt::red);
        right.fill(Qt::blue);
        canvas.setImages(left, right);
        canvas.show();

        // Fit mode: ONE unified scale so both panes fit — the smaller image
        // keeps explicit empty space instead of being stretched. With panes of
        // 199x300 and 200x300: left fits at min(1.99, 6.0)=1.99, right at
        // min(5.0, 3.75)=3.75; the unified scale is the smaller one.
        QCOMPARE(canvas.zoom(), 1.99);
        QCOMPARE(canvas.origin(), QPoint(0, 0));

        // Manual zoom disables fit, is bounded, and keeps the shared origin.
        canvas.setZoom(0.5);
        QVERIFY(!canvas.isFit());
        QCOMPARE(canvas.zoom(), 0.5);

        // Wheel zoom anchors the image point under the cursor; both panes share
        // the one origin, so the same coordinate moves in each pane.
        const QPointF pos(100, 100);
        QWheelEvent wheel(pos, canvas.mapToGlobal(pos.toPoint()), QPoint(),
                          QPoint(0, 120), Qt::NoButton, Qt::NoModifier,
                          Qt::NoScrollPhase, false);
        QCoreApplication::sendEvent(&canvas, &wheel);
        QVERIFY(canvas.zoom() > 0.5);
        QVERIFY(!canvas.isFit());

        // Drag pans the single shared origin (the panes cannot desynchronize).
        const QPoint before = canvas.origin();
        QTest::mousePress(&canvas, Qt::LeftButton, Qt::NoModifier, QPoint(200, 150));
        QTest::mouseMove(&canvas, QPoint(180, 140));
        QTest::mouseRelease(&canvas, Qt::LeftButton, Qt::NoModifier, QPoint(180, 140));
        QVERIFY(canvas.origin() != before);

        // No images: interactions are inert (no crash, no transform change).
        CompareCanvas empty;
        empty.resize(300, 200);
        empty.show();
        const QPoint originBefore = empty.origin();
        QTest::mousePress(&empty, Qt::LeftButton, Qt::NoModifier, QPoint(150, 100));
        QTest::mouseMove(&empty, QPoint(120, 90));
        QTest::mouseRelease(&empty, Qt::LeftButton, Qt::NoModifier, QPoint(120, 90));
        QCOMPARE(empty.origin(), originBefore);
    }
    void slidingDividerComparesSameCoordinate() {
        CompareCanvas canvas;
        canvas.resize(400, 300);
        QImage left(100, 50, QImage::Format_RGB32);
        QImage right(100, 50, QImage::Format_RGB32);
        left.fill(Qt::red);
        right.fill(Qt::blue);
        canvas.setImages(left, right);
        canvas.show();

        // Sliding mode: the wipe divider lives in image space.
        canvas.setSliding(true);
        QVERIFY(canvas.isSliding());
        // Both images must now fit the FULL viewport: min(400/100, 300/50).
        QCOMPARE(canvas.zoom(), 4.0);
        canvas.setDivider(60.0);
        QCOMPARE(canvas.divider(), 60.0);
        // A zoom does not move the image coordinate of the divider...
        canvas.setZoom(1.0);
        QCOMPARE(canvas.divider(), 60.0);
        // ...only its viewport position.
        const int vx = canvas.dividerViewportX();
        QVERIFY(vx > 0 && vx < 400);
        // Left/Right step the divider; [ / ] adjust the step size.
        const qreal before = canvas.divider();
        QTest::keyClick(&canvas, Qt::Key_Right);
        QVERIFY(canvas.divider() > before);
        const qreal afterStep = canvas.divider();
        QTest::keyClick(&canvas, Qt::Key_BracketLeft);
        QCOMPARE(canvas.dividerStep(), 20.0);
        // Dragging near the divider follows the cursor (image-space update).
        const int nowVx = canvas.dividerViewportX();
        QTest::mousePress(&canvas, Qt::LeftButton, Qt::NoModifier, QPoint(nowVx + 2, 150));
        QTest::mouseMove(&canvas, QPoint(nowVx - 30, 150));
        QTest::mouseRelease(&canvas, Qt::LeftButton, Qt::NoModifier, QPoint(nowVx - 30, 150));
        QVERIFY(canvas.divider() < afterStep);
        // One side failing to decode: the other side still renders (no crash).
        CompareCanvas oneSided;
        oneSided.resize(300, 200);
        oneSided.setImages(QImage(), right);
        oneSided.setSliding(true);
        oneSided.show();
        QVERIFY(oneSided.hasImages());
        QVERIFY(!oneSided.grab().isNull());
    }
    // P-209: manual-zoom sizeHint includes the 12px origin inset on every side,
    // so the scroll area can pan the complete image (nothing clipped at the
    // right/bottom edge).
    void previewCanvasManualSizeHintIncludesOriginInset() {
        PreviewCanvas canvas;
        QImage img(300, 200, QImage::Format_RGB32);
        img.fill(Qt::red);
        canvas.setImage(img);
        // Fit: tiny hint (the scroll area expands to the viewport).
        canvas.setFit(true);
        QCOMPARE(canvas.sizeHint(), QSize(1, 1));
        // Manual 1.0: scaled image PLUS the 12px inset on each side.
        canvas.setZoom(1.0);
        QCOMPARE(canvas.sizeHint(), QSize(300 + 24, 200 + 24));
        // Manual 0.5: half size plus the same inset.
        canvas.setZoom(0.5);
        QCOMPARE(canvas.sizeHint(), QSize(150 + 24, 100 + 24));
    }
    // The checkerboard paints under transparent pixels (P-209) and both
    // toggles (fit, checkerboard) actually flip their state and repaint.
    void previewCanvasPaintsCheckerboardAndToggles() {
        PreviewCanvas canvas;
        canvas.resize(200, 150);
        QImage img(80, 60, QImage::Format_ARGB32);
        img.fill(Qt::transparent);
        canvas.setImage(img);
        // Checkerboard defaults on; disabling it is a state change, a second
        // disable is a no-op.
        canvas.setCheckerboard(false);
        canvas.setCheckerboard(false);
        // Fit toggles: fit→manual runs the flip path; a repeat is a no-op.
        canvas.setFit(false);
        canvas.setFit(false);
        QCOMPARE(canvas.isFit(), false);
        canvas.setFit(true);
        QCOMPARE(canvas.isFit(), true);
        // Checkerboard back on + transparent image: the tile is painted.
        canvas.setCheckerboard(true);
        canvas.show();
        QVERIFY(!canvas.grab().isNull());
        QCOMPARE(canvas.effectiveZoom(), preview::fitScale(img.size(), canvas.size()));
    }
    // Split mode renders both panes with one divider; fit round-trips; in
    // sliding mode Left steps down and ] doubles the step; unbound keys fall
    // through without touching the divider.
    void compareCanvasRendersSplitPanesAndKeys() {
        CompareCanvas canvas;
        canvas.resize(400, 300);
        QImage left(100, 50, QImage::Format_RGB32);
        QImage right(100, 50, QImage::Format_RGB32);
        left.fill(Qt::red);
        right.fill(Qt::blue);
        canvas.setImages(left, right);
        canvas.show();
        // Split (non-sliding) mode: both panes + the divider render.
        QVERIFY(!canvas.isSliding());
        QVERIFY(!canvas.grab().isNull());
        // Manual-zoom split render: panes draw from the shared origin.
        canvas.setZoom(1.0);
        QVERIFY(!canvas.grab().isNull());
        // Fit round-trip (repeat calls are no-ops).
        canvas.setFit(false);
        canvas.setFit(false);
        QVERIFY(!canvas.isFit());
        canvas.setFit(true);
        QVERIFY(canvas.isFit());
        // Sliding-mode keys: Left steps down, ] doubles the step size, an
        // unbound key falls through to the default handler unchanged.
        canvas.setSliding(true);
        canvas.setZoom(1.0);
        const qreal before = canvas.divider();
        QTest::keyClick(&canvas, Qt::Key_Left);
        QVERIFY(canvas.divider() < before);
        const qreal stepped = canvas.divider();
        const qreal step = canvas.dividerStep();
        QTest::keyClick(&canvas, Qt::Key_BracketRight);
        QCOMPARE(canvas.dividerStep(), step * 2.0);
        QTest::keyClick(&canvas, Qt::Key_A);
        QCOMPARE(canvas.divider(), stepped);
        QVERIFY(!canvas.grab().isNull());
    }
    // P-609: the V1 user-journey acceptance walkthrough, end to end on one
    // isolated library — 导入 → 整理（标签/评分/文件夹） → 版本/快照 → 对比 →
    // 删除 → 预演恢复 → 备份演练 → 重开. Every leg checks the
    // acceptance-relevant observable, so a green run is evidence for the
    // in-scope R rows (the R01–R19 line-by-line matrix lives in
    // docs/zh/design/80-reliability.md). Rows that need real NAS / macOS /
    // Windows hardware or a human walkthrough stay open there — this test
    // never stands in for them.

};

// A QApplication is required: CompareCanvas/PreviewCanvas are widgets
// and cannot be created under a QCoreApplication (canvas/journey).
int main(int argc, char **argv) {
    QApplication app(argc, argv);
    CanvasCompareTest tc;
    QTEST_SET_MAIN_SOURCE_PATH
    return QTest::qExec(&tc, argc, argv);
}
#include "library_canvas_test.moc"
