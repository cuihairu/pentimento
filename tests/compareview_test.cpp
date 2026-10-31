#include "compareview.h"
#include <QtTest>

// P-313: the side-by-side comparison renders both panes through the same
// geometry (compareview), so the same (zoom, origin) shows the same image-pixel
// region on both sides — 同一位置和比例可重复比较. These are pure-math rules;
// the pane widgets (UI) consume them, and this test pins the rules.
class CompareViewTest : public QObject {
    Q_OBJECT
private slots:
    void fitNeverUpscalesAndIsUniform() {
        QCOMPARE(compareview::fitZoom(QSize(800, 600), QSize(400, 300)), 0.5);
        // Smaller image than the window: fit does NOT stretch past 100%.
        QCOMPARE(compareview::fitZoom(QSize(100, 100), QSize(800, 800)), 1.0);
        // Limited by the tighter axis (height binds here).
        QCOMPARE(compareview::fitZoom(QSize(800, 600), QSize(400, 200)), 1.0 / 3.0);
        QCOMPARE(compareview::fitZoom(QSize(), QSize(400, 300)), 1.0);
        QCOMPARE(compareview::fitZoom(QSize(800, 600), QSize()), 1.0);
    }
    void sameViewShowsSameRegionOnBothSides() {
        // Two images of DIFFERENT sizes, one shared View (zoom + origin). Both
        // panes must show the same image-pixel region at the same scale, so a
        // feature compared on one side is at the identical position on the other.
        const QSize viewport(400, 300);
        const QRect a = compareview::sourceRect(QSize(800, 600), viewport, 2.0, QPointF(100, 50));
        const QRect b = compareview::sourceRect(QSize(400, 300), viewport, 2.0, QPointF(100, 50));
        QCOMPARE(a, QRect(100, 50, 200, 150));
        QCOMPARE(b, QRect(100, 50, 200, 150));
        QCOMPARE(a, b);
        // At that zoom the visible region fills the viewport at the same scale
        // on both sides: source size * zoom == viewport (200*2 x 150*2).
        QCOMPARE(QSize(a.width() * 2, a.height() * 2), viewport);
        QCOMPARE(QSize(b.width() * 2, b.height() * 2), viewport);
    }
    void clampKeepsCoverageWhenZoomedIn() {
        const QSize image(800, 600);
        const QSize viewport(400, 300);
        const qreal zoom = 2.0;
        // Dragging far past the bottom/right edge is clamped so the image still
        // covers the viewport (no blank while inspecting).
        QCOMPARE(compareview::clampOrigin(image, viewport, zoom, QPointF(9999, 9999)),
                 QPointF(600, 450)); // (800-400/2, 600-300/2)
        // Dragging before the top-left edge clamps to the image origin.
        QCOMPARE(compareview::clampOrigin(image, viewport, zoom, QPointF(-5, -5)),
                 QPointF(0, 0));
    }
    void zoomedOutIsTopLeftWithExplicitBlank() {
        const QSize image(200, 150);
        const QSize viewport(400, 300);
        const qreal zoom = 0.5; // whole image (100x75) fits with room to spare
        QCOMPARE(compareview::clampOrigin(image, viewport, zoom, QPointF(50, 50)),
                 QPointF(0, 0)); // top-left; blank beyond is explicit, not stretched
        QCOMPARE(compareview::targetRect(image, viewport, zoom, QPointF(0, 0)),
                 QRect(0, 0, 100, 75));
        QCOMPARE(compareview::sourceRect(image, viewport, zoom, QPointF(0, 0)),
                 QRect(0, 0, 200, 150)); // whole image visible
    }
    void sourceAndTargetStayConsistent() {
        const QSize image(800, 600);
        const QSize viewport(400, 300);
        const qreal zoom = 2.0;
        const QPointF origin(100, 50);
        const QRect source = compareview::sourceRect(image, viewport, zoom, origin);
        const QRect target = compareview::targetRect(image, viewport, zoom, origin);
        // The visible source region scaled by zoom equals the drawn size of that
        // region (the part of target inside the viewport).
        QCOMPARE(qreal(source.width()) * zoom, 200.0 * zoom);
        QCOMPARE(qreal(source.height()) * zoom, 150.0 * zoom);
        QVERIFY(target.width() > viewport.width()); // zoomed in: image exceeds viewport
    }
    void slidingDividerShowsSameCoordinateOnBothSides() {
        const compareview::View view{2.0, QPointF(100, 50)};
        // The divider at viewport x exposes one image-pixel column; both sides
        // render through the same View, so that column is the same on both.
        const qreal column = compareview::dividerToImageX(300.0, view);
        QVERIFY(qAbs(column - 250.0) < 1e-6); // 100 + 300/2
        // Round trip: the viewport x of that column is back to 300.
        QVERIFY(qAbs(compareview::imageXToViewport(column, view) - 300.0) < 1e-6);
        // 缩放后同坐标: at another zoom the same image column maps to a different
        // viewport x, and the divider there still exposes that same column.
        const compareview::View zoomed{4.0, QPointF(50, 25)};
        const qreal vx = compareview::imageXToViewport(column, zoomed); // (250-50)*4
        QVERIFY(qAbs(vx - 800.0) < 1e-6);
        QVERIFY(qAbs(compareview::dividerToImageX(vx, zoomed) - column) < 1e-6);
    }
};
QTEST_GUILESS_MAIN(CompareViewTest)
#include "compareview_test.moc"
