#pragma once

#include <QImage>
#include <QSize>
#include <QString>
#include <QStringList>

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

// Decode `path` for preview. `formatHint` (from the asset's own file extension,
// which the asset stores) is authoritative for signature-less formats such as
// TGA that content sniffing mislabels; when absent or unsupported, content
// sniffing decides. When the RGBA decode would exceed `pixelBudget` pixels, the
// image is read at a uniformly downscaled size (aspect preserved) and flagged
// `reduced`; the source file is never modified. Never allocates unbounded memory.
Decoded decode(const QString &path, const QString &formatHint = QString(),
               qint64 pixelBudget = 64LL * 1024 * 1024);

// Content-sniffed format of `path` ("png", "jpeg", ...), empty when the content
// is unrecognized. Never trusts the file extension (P-210).
QString detectedFormat(const QString &path);

// True when the running Qt can actually decode `path` (content-based). Distinct
// from a successful decode: a truncated file is recognizable but not decodable.
bool canDecode(const QString &path);

// True when `content` begins with the PSD signature ("8BPS"); content-based, so
// the degradation message for Photoshop files never relies on the extension
// (P-211: composite preview is evaluated, not assumed).
bool isPsdSignature(const QByteArray &content);

// The V1 first-batch formats (PNG/JPEG/WebP/TGA/BMP) that this Qt build can
// decode; reports capability honestly instead of claiming support by extension.
QStringList firstBatchSupported();

} // namespace preview
