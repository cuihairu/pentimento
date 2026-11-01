#include "preview.h"

#include <QImageReader>
#include <QtGlobal>
#include <QtMath>

namespace {
const QByteArray kPsdSignature = QByteArrayLiteral("8BPS");
}

namespace preview {

qreal fitScale(const QSize &imageSize, const QSize &viewportSize, int margin) {
    const int availW = viewportSize.width() - 2 * margin;
    const int availH = viewportSize.height() - 2 * margin;
    if (imageSize.isEmpty() || availW <= 0 || availH <= 0) return 1.0;
    const qreal scale = qMin(qreal(availW) / imageSize.width(),
                             qreal(availH) / imageSize.height());
    return qBound(1.0 / 32.0, scale, 8.0);
}

Decoded decode(const QString &path, const QString &formatHint, qint64 pixelBudget) {
    Decoded out;
    QImageReader reader(path);
    if (!formatHint.isEmpty() &&
        QImageReader::supportedImageFormats().contains(formatHint.toLatin1())) {
        reader.setFormat(formatHint.toLatin1()); // authoritative (signature-less TGA)
    } else {
        reader.setDecideFormatFromContent(true); // fallback: sniff the content
    }
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

QString detectedFormat(const QString &path) {
    QImageReader reader(path);
    reader.setDecideFormatFromContent(true);
    return reader.format(); // recognized from content, never from the extension
}

bool canDecode(const QString &path) {
    QImageReader reader(path);
    reader.setDecideFormatFromContent(true);
    reader.setAutoTransform(true);
    return reader.canRead();
}

bool isPsdSignature(const QByteArray &content) {
    return content.startsWith(kPsdSignature);
}

QStringList firstBatchSupported() {
    const auto all = QImageReader::supportedImageFormats();
    const QStringList batch = {QStringLiteral("png"), QStringLiteral("jpeg"),
                               QStringLiteral("webp"), QStringLiteral("tga"),
                               QStringLiteral("bmp")};
    QStringList out;
    for (const auto &format : batch)
        if (all.contains(format)) out << format;
    return out;
}

} // namespace preview
