#include "thumbnails.h"

#include <QBuffer>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QSaveFile>
#include <QSize>

namespace {
int kDecoderVersion = 1;
}

ThumbnailCache::ThumbnailCache(const QString &libraryRoot)
    : root_(libraryRoot + "/thumbnails") {
    QDir().mkpath(root_);
}

int ThumbnailCache::decoderVersion() { return kDecoderVersion; }

QString ThumbnailCache::key(const QString &contentHash, const QSize &size, int decoderVersion) {
    return QStringLiteral("%1_%2x%3_v%4").arg(contentHash)
        .arg(size.width()).arg(size.height()).arg(decoderVersion);
}

QString ThumbnailCache::filePath(const QString &contentHash, const QSize &size,
                                 int decoderVersion) const {
    return root_ + "/" + key(contentHash, size, decoderVersion) + ".png";
}

QImage ThumbnailCache::find(const QString &contentHash, const QSize &size,
                            int decoderVersion) const {
    const QString path = filePath(contentHash, size, decoderVersion);
    if (!QFileInfo::exists(path)) return QImage();
    QImage image(path);
    if (image.isNull()) {
        // Corrupt cache entry: delete so the next access regenerates (P-207).
        QFile::remove(path);
        return QImage();
    }
    return image;
}

void ThumbnailCache::store(const QString &contentHash, const QSize &size,
                           int decoderVersion, const QImage &image) {
    if (image.isNull()) return;
    QDir().mkpath(root_);
    QSaveFile file(filePath(contentHash, size, decoderVersion));
    if (!file.open(QIODevice::WriteOnly)) return;
    image.save(&file, "PNG");
    file.commit();
}

QImage thumbnailFor(ThumbnailCache &cache, const QByteArray &contentBytes,
                    const QString &contentHash, const QSize &size,
                    const QString &formatHint) {
    const int version = ThumbnailCache::decoderVersion();
    QImage cached = cache.find(contentHash, size, version);
    if (!cached.isNull()) return cached;
    // Decode from the object bytes, never from a path: the hash is authoritative
    // and a renamed/version-changed file cannot show a stale image. The hint is
    // authoritative for signature-less formats (TGA) that sniffing mislabels.
    QBuffer buffer;
    buffer.setData(contentBytes);
    buffer.open(QIODevice::ReadOnly);
    QImageReader reader(&buffer);
    if (!formatHint.isEmpty() &&
        QImageReader::supportedImageFormats().contains(formatHint.toLatin1()))
        reader.setFormat(formatHint.toLatin1());
    reader.setAutoTransform(true);
    QImage decoded = reader.read();
    if (decoded.isNull()) return QImage();
    const QImage scaled = decoded.scaled(size, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    cache.store(contentHash, size, version, scaled);
    return scaled;
}
