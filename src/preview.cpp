#include "preview.h"

#include <QImageReader>
#include <QtGlobal>
#include <QtMath>

namespace preview {

qreal fitScale(const QSize &imageSize, const QSize &viewportSize, int margin) {
    const int availW = viewportSize.width() - 2 * margin;
    const int availH = viewportSize.height() - 2 * margin;
    if (imageSize.isEmpty() || availW <= 0 || availH <= 0) return 1.0;
    const qreal scale = qMin(qreal(availW) / imageSize.width(),
                             qreal(availH) / imageSize.height());
    return qBound(1.0 / 32.0, scale, 8.0);
}

Decoded decode(const QString &path, qint64 pixelBudget) {
    Decoded out;
    QImageReader reader(path);
    reader.setDecideFormatFromContent(true);
    reader.setAutoTransform(true);
    const QSize size = reader.size();
    if (!size.isValid()) return out;
    const qint64 pixels = qint64(size.width()) * size.height();
    if (pixels > pixelBudget && pixels > 0) {
        // Uniform downscale to fit the budget; aspect ratio is preserved and the
        // source file is untouched, so a huge image degrades gracefully.
        const qreal factor = qSqrt(qreal(pixelBudget) / qreal(pixels));
        reader.setScaledSize(QSize(qMax(1, int(size.width() * factor)),
                                   qMax(1, int(size.height() * factor))));
        out.reduced = true;
    }
    out.image = reader.read();
    out.valid = !out.image.isNull();
    return out;
}

} // namespace preview
