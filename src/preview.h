#pragma once

#include <QImage>
#include <QSize>
#include <QString>

// Preview helpers (P-209). Aspect-preserving zoom math and bounded decoding so
// huge images degrade to a scaled read instead of exhausting memory. All scaling
// is uniform (one factor for both axes): a displayed image is never stretched,
// so the on-screen aspect always equals the source aspect.
namespace preview {

struct Decoded {
    QImage image;        // null when the format cannot be read
    bool reduced = false; // true: read at a downsampled size (memory guard)
    bool valid = false;   // true: a decodable image was produced
};

// Uniform scale fitting `imageSize` inside `viewportSize` minus `margin` on each
// side, preserving aspect ratio; clamped to [1/32, 8] so pathological ratios
// still land in a usable range. Returns 1 when either size is empty.
qreal fitScale(const QSize &imageSize, const QSize &viewportSize, int margin = 0);

// Decode `path` for preview. When the RGBA decode would exceed `pixelBudget`
// pixels, the image is read at a uniformly downscaled size (aspect preserved)
// and flagged `reduced`; the source file is never modified. Never allocates
// unbounded memory.
Decoded decode(const QString &path, qint64 pixelBudget = 64LL * 1024 * 1024);

} // namespace preview
