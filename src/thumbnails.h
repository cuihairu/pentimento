#pragma once

#include <QImage>
#include <QSize>
#include <QString>

// Thumbnail cache (P-207). Keyed by content hash + generation parameters (size,
// decoder version), never by filename: the same content reused across versions
// or renames shows one thumbnail, and a version change shows no stale image.
// Entries live inside the library (relocatable) under `thumbnails/`. A corrupt
// cache entry is deleted and regenerated; it never touches the original object.
class ThumbnailCache {
public:
    explicit ThumbnailCache(const QString &libraryRoot);

    // Bump when image codecs upgrade so stale thumbnails rebuild (P-207).
    static int decoderVersion();

    // Stable cache key: contentHash + size + decoder version.
    static QString key(const QString &contentHash, const QSize &size, int decoderVersion);

    // Null when absent or corrupt (a corrupt entry is removed for rebuild).
    QImage find(const QString &contentHash, const QSize &size, int decoderVersion) const;

    // Atomic write: temp file -> verify -> rename; never partial on crash.
    void store(const QString &contentHash, const QSize &size, int decoderVersion,
               const QImage &image);

    QString filePath(const QString &contentHash, const QSize &size, int decoderVersion) const;

private:
    QString root_;
};

// Find a thumbnail for content bytes; if absent or corrupt, decode from the
// bytes (never from a file path, so the object hash is authoritative) and cache
// it. `formatHint` (the asset's file extension) is authoritative for formats
// content sniffing mislabels (TGA). Scaled to fit `size` preserving aspect.
QImage thumbnailFor(ThumbnailCache &cache, const QByteArray &contentBytes,
                    const QString &contentHash, const QSize &size,
                    const QString &formatHint = QString());
