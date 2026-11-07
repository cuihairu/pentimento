#include "preview.h"

#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QtGlobal>
#include <QtMath>

namespace {
const QByteArray kPsdSignature = QByteArrayLiteral("8BPS");
/* TrueVision 2.0 footer signature: the only content marker TGA carries
 * (no magic in the header), so content-based sniffing can fall back to it
 * when the runtime's qtga plugin declines the heuristic-only canRead. */
const QByteArray kTgaFooterSignature = QByteArrayLiteral("TRUEVISION-XFILE");

bool hasTgaFooter(const QString &path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return false;
    if (f.size() < 18 + 26) return false; // header + footer minimum
    if (!f.seek(f.size() - 26)) return false;
    return f.read(26).contains(kTgaFooterSignature);
}
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
    // Resolve the format: the hint is authoritative (signature-less TGA);
    // otherwise sniff the content, falling back to the TrueVision footer
    // signature when the runtime's qtga canRead heuristic declines real TGA
    // content (heuristic behavior varies across Qt builds).
    auto tryRead = [&](const QString &fmt) -> Decoded {
        Decoded r;
        QImageReader reader(path);
        if (!fmt.isEmpty() &&
            QImageReader::supportedImageFormats().contains(fmt.toLatin1())) {
            reader.setFormat(fmt.toLatin1());
        } else {
            reader.setDecideFormatFromContent(true); // sniff the content
        }
        reader.setAutoTransform(true);
        const QSize size = reader.size();
        if (!size.isValid()) return r;
        const qint64 pixels = qint64(size.width()) * size.height();
        if (pixels > pixelBudget && pixels > 0) {
            // Uniform downscale to fit the budget; aspect ratio is preserved and
            // the source file is untouched, so a huge image degrades gracefully.
            const qreal factor = qSqrt(qreal(pixelBudget) / qreal(pixels));
            reader.setScaledSize(QSize(qMax(1, int(size.width() * factor)),
                                       qMax(1, int(size.height() * factor))));
            r.reduced = true;
        }
        r.image = reader.read();
        r.valid = !r.image.isNull();
        return r;
    };
    Decoded out = tryRead(formatHint);
    if (!out.valid && formatHint.isEmpty() && hasTgaFooter(path))
        out = tryRead(QStringLiteral("tga")); // footer-confirmed content
    return out;
}

QString detectedFormat(const QString &path) {
    QImageReader reader(path);
    reader.setDecideFormatFromContent(true);
    const QString fmt = reader.format(); // recognized from content, never from the extension
    if (!fmt.isEmpty()) return fmt;
    return hasTgaFooter(path) ? QStringLiteral("tga") : QString();
}

bool canDecode(const QString &path) {
    QImageReader reader(path);
    reader.setDecideFormatFromContent(true);
    reader.setAutoTransform(true);
    if (reader.canRead()) return true;
    return hasTgaFooter(path);
}

bool isPsdSignature(const QByteArray &content) {
    return content.startsWith(kPsdSignature);
}

Metadata metadata(const QString &path, const QString &formatHint) {
    Metadata m;
    const QFileInfo info(path);
    if (!info.exists() || !info.isFile()) return m; // nothing to inspect
    m.ok = true;
    m.fileSize = info.size();
    QImageReader reader(path);
    if (!formatHint.isEmpty() &&
        QImageReader::supportedImageFormats().contains(formatHint.toLatin1())) {
        reader.setFormat(formatHint.toLatin1());
    } else {
        reader.setDecideFormatFromContent(true);
    }
    const QSize size = reader.size();
    if (!size.isValid()) {
        // Header unreadable (e.g. no PSD codec): still name it by content
        // signature; size/channels stay unknown rather than fabricated.
        QFile file(path);
        if (file.open(QIODevice::ReadOnly) && file.read(4) == kPsdSignature)
            m.format = QStringLiteral("psd");
        return m;
    }
    m.width = size.width();
    m.height = size.height();
    m.format = reader.format().toLower();
    // Channels and per-channel depth come from the header format, never guessed.
    switch (reader.imageFormat()) {
    case QImage::Format_ARGB32:
    case QImage::Format_ARGB32_Premultiplied:
    case QImage::Format_RGBA8888:
    case QImage::Format_RGBA64:
    case QImage::Format_RGBA64_Premultiplied:
    case QImage::Format_ARGB8565_Premultiplied:
    case QImage::Format_ARGB6666_Premultiplied:
    case QImage::Format_ARGB4444_Premultiplied:
        m.channels = QStringLiteral("RGBA");
        m.hasAlpha = true;
        break;
    case QImage::Format_RGB32:
    case QImage::Format_RGB888:
    case QImage::Format_RGBX64:
        m.channels = QStringLiteral("RGB");
        break;
    case QImage::Format_Grayscale8:
    case QImage::Format_Grayscale16:
        m.channels = QStringLiteral("灰度");
        break;
    case QImage::Format_Indexed8:
        m.channels = QStringLiteral("调色板");
        break;
    case QImage::Format_Alpha8:
        m.channels = QStringLiteral("Alpha");
        m.hasAlpha = true;
        break;
    default:
        break; // unknown (incl. CMYK, which Qt 6 dropped): leave channels empty
    }
    switch (reader.imageFormat()) {
    case QImage::Format_RGBA64:
    case QImage::Format_RGBA64_Premultiplied:
    case QImage::Format_RGBX64:
    case QImage::Format_Grayscale16:
        m.bitsPerChannel = 16;
        break;
    case QImage::Format_ARGB32:
    case QImage::Format_ARGB32_Premultiplied:
    case QImage::Format_RGB32:
    case QImage::Format_RGB888:
    case QImage::Format_RGBA8888:
    case QImage::Format_Indexed8:
    case QImage::Format_Alpha8:
        m.bitsPerChannel = 8;
        break;
    default:
        break; // unknown depth: 0, never filled with a guess
    }
    // PSD has no Qt codec: name it by content signature, keep channels unknown.
    if (m.format.isEmpty()) {
        QFile file(path);
        if (file.open(QIODevice::ReadOnly) && file.read(4) == kPsdSignature)
            m.format = QStringLiteral("psd");
    }
    return m;
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
