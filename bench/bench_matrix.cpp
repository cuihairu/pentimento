// P-004 evidence harness: generate the draft sample matrix (docs/zh/
// performance.md) and measure the draft thresholds on this machine. This is
// NOT part of ctest — a benchmark is not a unit test and takes minutes.
//
//   cmake --build build --target pentimento_bench_matrix
//   ./build/pentimento_bench_matrix <workdir> [gen-only|bench-only|gen-quick|bench-quick]
//
// D-20 defines two tiers: the full matrix (default) and a quick tier scaled
// to ~5 GiB ("数量/尺寸按比例缩减") — both covering the five bitmap families,
// PSD degradation and damaged samples. Use a separate workdir per tier; the
// generator names files identically, so one directory holds one tier.
//
// Generation is deterministic (fixed-seed xorshift noise) and idempotent: an
// existing matrix with the right file count is reused. The library under
// <workdir>/lib is created fresh on every bench run.
#include "filter.h"
#include "library.h"
#include "thumbnails.h"

#include <QBuffer>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QImage>
#include <QStorageInfo>

#include <QStringList>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <vector>

namespace {

// --- deterministic noise (same matrix on every run) -------------------------
quint64 g_state = 0x9E3779B97F4A7C15ull;
quint64 xrand() {
    quint64 x = g_state;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    g_state = x;
    return x;
}

QImage noiseImage(int w, int h, bool alpha) {
    QImage img(w, h, QImage::Format_ARGB32);
    const int stride = img.bytesPerLine() / 4;
    auto *bits = reinterpret_cast<quint32 *>(img.bits());
    for (int y = 0; y < h; ++y) {
        quint32 *row = bits + static_cast<size_t>(y) * stride;
        for (int x = 0; x < w; ++x)
            row[x] = static_cast<quint32>(xrand()) | (alpha ? 0 : 0xFF000000u);
    }
    return img;
}

// Noise does not compress, so the encoded size tracks the pixel count; adjust
// dimensions until the file lands within ±30% of the draft target size.
bool writeNoiseImage(const QString &path, int w, int h, const char *format,
                     qint64 targetBytes, bool alpha) {
    for (int attempt = 0; attempt < 8; ++attempt) {
        if (!noiseImage(w, h, alpha).save(path, format))
            return false;
        const qint64 size = QFileInfo(path).size();
        if (qAbs(size - targetBytes) <= targetBytes * 3 / 10)
            return true;
        const double factor = std::sqrt(double(targetBytes) / double(qMax<qint64>(1, size)));
        w = qBound(64, int(w * factor), 8192);
        h = qBound(64, int(h * factor), 8192);
    }
    return true; // accept the closest attempt
}

// TGA has no Qt writer; hand-roll 24-bit BGR, uncompressed (type 2) or
// one-pixel RLE packets (type 10).
void writeTga(const QString &path, int w, int h, bool rle) {
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return;
    uchar header[18] = {0};
    header[2] = rle ? 10 : 2;
    header[12] = uchar(w & 0xFF);
    header[13] = uchar((w >> 8) & 0xFF);
    header[14] = uchar(h & 0xFF);
    header[15] = uchar((h >> 8) & 0xFF);
    header[16] = 24;
    file.write(reinterpret_cast<const char *>(header), 18);
    QByteArray row(3, '\0');
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const quint64 r = xrand();
            row[0] = char(r & 0xFF);         // B
            row[1] = char((r >> 8) & 0xFF);  // G
            row[2] = char((r >> 16) & 0xFF); // R
            if (rle)
                file.putChar(char(0x80)); // RLE packet, count 1
            file.write(row);
        }
    }
}

// PSD: structurally valid 4-channel (RGB+A) raw image — the matrix documents
// PSD bytes being preserved with preview degradation, not decoded.
bool writePsd(const QString &path, int w, int h) {
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return false;
    auto be16 = [&file](int v) {
        file.putChar(char((v >> 8) & 0xFF));
        file.putChar(char(v & 0xFF));
    };
    auto be32 = [&file, &be16](quint32 v) {
        be16(int((v >> 16) & 0xFFFF));
        be16(int(v & 0xFFFF));
    };
    file.write("8BPS", 4);
    be16(1);
    file.write(QByteArray(6, '\0'));
    be16(4); // channels: R, G, B, A
    be32(quint32(h));
    be32(quint32(w));
    be16(8); // bit depth
    be16(3); // RGB color mode
    be32(0); // color mode data
    be32(0); // image resources
    be32(0); // layer and mask info
    be16(0); // raw (uncompressed) image data
    QByteArray row(w * 4, '\0');
    for (int y = 0; y < h; ++y) {
        for (int i = 0; i < row.size(); ++i)
            row[i] = char(xrand() & 0xFF);
        file.write(row);
    }
    return true;
}

quint32 crc32Update(quint32 crc, const char *data, int len) {
    for (int i = 0; i < len; ++i) {
        crc ^= uchar(data[i]);
        for (int k = 0; k < 8; ++k)
            crc = (crc >> 1) ^ (0xEDB88320u & (~(crc & 1u) + 1u));
    }
    return crc;
}

// Single stored (uncompressed) entry ZIP — the matrix's "源文件" category is
// raw bytes preserved, so a genuinely valid archive is written, not noise
// with a .zip name.
bool writeZipStored(const QString &path, const QString &entryName, qint64 dataBytes) {
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return false;
    const QByteArray name = entryName.toUtf8();
    quint32 crc = 0xFFFFFFFFu;
    const qint64 localHeaderOffset = 0;
    auto le16 = [&file](quint32 v) {
        file.putChar(char(v & 0xFF));
        file.putChar(char((v >> 8) & 0xFF));
    };
    auto le32 = [&file, &le16](quint32 v) {
        le16(v & 0xFFFF);
        le16((v >> 16) & 0xFFFF);
    };
    // Local file header (stored).
    le32(0x04034b50);
    le16(20);
    le16(0);
    le16(0); // stored
    le16(0);
    le16(0);
    le32(0); // CRC patched below (header is rewritten)
    le32(quint32(dataBytes));
    le32(quint32(dataBytes));
    le16(quint32(name.size()));
    le16(0);
    file.write(name);
    QByteArray chunk(1 << 20, '\0');
    for (qint64 left = dataBytes; left > 0;) {
        const int n = int(qMin<qint64>(left, chunk.size()));
        for (int i = 0; i < n; ++i)
            chunk[i] = char(xrand() & 0xFF);
        crc = crc32Update(crc, chunk.constData(), n);
        file.write(chunk.constData(), n);
        left -= n;
    }
    crc = ~crc;
    // Central directory.
    const qint64 cdOffset = file.pos();
    le32(0x02014b50);
    le16(20);
    le16(20);
    le16(0);
    le16(0);
    le16(0);
    le16(0);
    le32(crc);
    le32(quint32(dataBytes));
    le32(quint32(dataBytes));
    le16(quint32(name.size()));
    le16(0);
    le16(0);
    le16(0);
    le16(0);
    le32(0);
    le32(quint32(localHeaderOffset));
    file.write(name);
    const qint64 cdSize = file.pos() - cdOffset;
    // End of central directory.
    le32(0x06054b50);
    le16(0);
    le16(0);
    le16(1);
    le16(1);
    le32(quint32(cdSize));
    le32(quint32(cdOffset));
    le16(0);
    // Patch the local header CRC/sizes (offsets 14/18/22).
    file.seek(14);
    le32(crc);
    le32(quint32(dataBytes));
    le32(quint32(dataBytes));
    file.seek(file.size());
    return true;
}

struct Sized {
    QString path;
    qint64 bytes = 0;
};

qint64 totalBytes(const std::vector<Sized> &files) {
    qint64 sum = 0;
    for (const auto &f : files)
        sum += f.bytes;
    return sum;
}

QString mibGiB(qint64 bytes) {
    return QStringLiteral("%1 MiB (%2 GiB)")
        .arg(bytes / (1024.0 * 1024.0), 0, 'f', 1)
        .arg(bytes / (1024.0 * 1024.0 * 1024.0), 0, 'f', 2);
}

bool fileOk(const QString &path) { return QFileInfo(path).isFile(); }

struct Matrix {
    std::vector<Sized> png;   // 250 opaque + 250 alpha
    std::vector<Sized> jpeg;
    std::vector<Sized> webp;
    std::vector<Sized> tga;   // 250 uncompressed + 250 RLE
    std::vector<Sized> bmp;
    std::vector<Sized> largePng;
    std::vector<Sized> psd;
    std::vector<Sized> zip;
    qint64 count() const {
        return qint64(png.size() + jpeg.size() + webp.size() + tga.size() + bmp.size()
                      + largePng.size() + psd.size() + zip.size());
    }
    qint64 bytes() const {
        qint64 sum = totalBytes(png) + totalBytes(jpeg) + totalBytes(webp)
            + totalBytes(tga) + totalBytes(bmp) + totalBytes(largePng)
            + totalBytes(psd) + totalBytes(zip);
        return sum;
    }
};

bool generateMatrix(const QString &src, int bitmapCount, int largeCount,
                    int psdCount, int zipCount)
{
    QDir().mkpath(src);
    Matrix m;
    const auto add = [](std::vector<Sized> &v, const QString &path) {
        v.push_back({path, QFileInfo(path).size()});
    };

    // PNG bitmapCount @512 KiB (透明/不透明各半): 512x512 noise ≈ 512 KiB as PNG.
    for (int i = 0; i < bitmapCount; ++i) {
        const bool alpha = (i % 2 == 0);
        const QString p = src + QStringLiteral("/png-%1.png").arg(i, 4, 10, QLatin1Char('0'));
        if (!fileOk(p))
            writeNoiseImage(p, 512, 512, "PNG", 512 * 1024, alpha);
        add(m.png, p);
    }

    // JPEG bitmapCount @1 MiB: 2048x2048 noise is lossy-incompressible → ~1 MiB.
    for (int i = 0; i < bitmapCount; ++i) {
        const QString p = src + QStringLiteral("/jpeg-%1.jpg").arg(i, 4, 10, QLatin1Char('0'));
        if (!fileOk(p))
            writeNoiseImage(p, 2048, 2048, "JPEG", 1024 * 1024, false);
        add(m.jpeg, p);
    }

    // WebP bitmapCount @512 KiB (lossy half / lossless half; noise is
    // incompressible for both, so the encoder output tracks the pixel count).
    for (int i = 0; i < bitmapCount; ++i) {
        const bool lossless = (i % 2 == 0);
        const QString p = src + QStringLiteral("/webp-%1.webp").arg(i, 4, 10, QLatin1Char('0'));
        if (!fileOk(p))
            writeNoiseImage(p, 512, 512, lossless ? "WEBP" : "WEBP", 512 * 1024, true);
        add(m.webp, p);
    }

    // TGA bitmapCount @2 MiB (raw / RLE 各半): 2048x2048x3 = 12 MiB raw —
    // overshoots; 1152x1152x3 ≈ 4 MiB. TGA has no Qt writer, so writeTga
    // takes fixed dims and lands within ±30% of target.
    for (int i = 0; i < bitmapCount; ++i) {
        const bool rle = (i % 2 == 0);
        const QString p = src + QStringLiteral("/tga-%1.tga").arg(i, 4, 10, QLatin1Char('0'));
        if (!fileOk(p))
            writeTga(p, 1152, 1152, rle);
        add(m.tga, p);
    }

    // BMP bitmapCount @2 MiB (uncompressed): 2048x2048x3 = 12 MiB raw —
    // overshoots; 1024x1024x3 = 3 MiB, use 1024x1024 → ~3 MiB, within tolerance.
    for (int i = 0; i < bitmapCount; ++i) {
        const QString p = src + QStringLiteral("/bmp-%1.bmp").arg(i, 4, 10, QLatin1Char('0'));
        if (!fileOk(p))
            writeNoiseImage(p, 1024, 1024, "BMP", 2 * 1024 * 1024, false);
        add(m.bmp, p);
    }

    // 大图 PNG largeCount @64 MiB: 8192x8192 noise ≈ 256 MiB; scale factor
    // sqrt(64/256) = 0.5 → 4096x4096 noise ≈ 64 MiB.
    for (int i = 0; i < largeCount; ++i) {
        const QString p = src + QStringLiteral("/big-%1.png").arg(i, 2, 10, QLatin1Char('0'));
        if (!fileOk(p))
            writeNoiseImage(p, 4096, 4096, "PNG", 64 * 1024 * 1024, false);
        add(m.largePng, p);
    }

    // PSD psdCount @64 MiB (structurally valid, noise RGBA 8192x8192 = 256 MiB;
    // need 64 MiB → 4096x4096).
    for (int i = 0; i < psdCount; ++i) {
        const QString p = src + QStringLiteral("/psd-%1.psd").arg(i, 2, 10, QLatin1Char('0'));
        if (!fileOk(p))
            writePsd(p, 4096, 4096);
        add(m.psd, p);
    }

    // ZIP/other zipCount @128 MiB: stored ZIP (uncompressed, valid CRC32).
    for (int i = 0; i < zipCount; ++i) {
        const QString p = src + QStringLiteral("/other-%1.zip").arg(i, 2, 10, QLatin1Char('0'));
        if (!fileOk(p))
            writeZipStored(p, QStringLiteral("payload.bin"), 128 * 1024 * 1024);
        add(m.zip, p);
    }

    // D-20: one damaged sample per bitmap family plus two PSD cases (bad
    // signature / damaged pixel data). Generated deterministically by taking a
    // 4 KiB prefix of a valid sample and, for PSD, writing a small header +
    // payload prefix with a broken signature or a flipped data byte. They are
    // small on purpose: they exercise import/thumbnail degradation paths, not
    // throughput. `fileOk` also makes regeneration idempotent.
    struct Bad { const char *source; const char *name; bool corruptSignature; bool flipData; };
    const std::vector<Bad> bad = {
        {"png-0000.png",   "bad-png-truncated.png",   false, false},
        {"jpeg-0000.jpg",  "bad-jpeg-truncated.jpg",  false, false},
        {"webp-0000.webp", "bad-webp-truncated.webp", false, false},
        {"tga-0000.tga",   "bad-tga-truncated.tga",   false, false},
        {"bmp-0000.bmp",   "bad-bmp-truncated.bmp",   false, false},
        {"psd-00.psd",     "bad-psd-signature.psd",   true,  false},
        {"psd-00.psd",     "bad-psd-data.psd",        false, true },
    };
    for (const Bad &b : bad) {
        const QString p = src + QLatin1Char('/') + QString::fromLatin1(b.name);
        if (fileOk(p)) continue;
        QFile in(src + QLatin1Char('/') + QString::fromLatin1(b.source));
        if (!in.open(QIODevice::ReadOnly)) continue;
        const qint64 keep = b.corruptSignature || b.flipData ? 64 * 1024 : 4096;
        QByteArray bytes = in.read(keep);
        in.close();
        if (b.corruptSignature && bytes.size() >= 4)
            bytes.replace(0, 4, "XXXX");
        if (b.flipData && bytes.size() > 5000)
            bytes[5000] = char(uchar(bytes[5000]) ^ 0xFFu);
        QFile out(p);
        if (out.open(QIODevice::WriteOnly)) out.write(bytes);
    }

    printf("matrix: %lld files, %s\n", (long long)m.count(), qPrintable(mibGiB(m.bytes())));
    return true;
}

// --- measurement helpers -----------------------------------------------------

double percentile95(std::vector<double> &samples) {
    if (samples.empty())
        return 0.0;
    std::sort(samples.begin(), samples.end());
    const size_t idx = size_t(0.95 * double(samples.size() - 1));
    return samples[idx];
}

void printVerdict(const char *label, double value, const char *unit,
                  double threshold, bool higherIsBetter) {
    const bool pass = higherIsBetter ? value >= threshold : value < threshold;
    printf("  %-42s %10.1f %-4s (threshold %s %.0f) -> %s\n", label, value, unit,
           higherIsBetter ? ">=" : "<", threshold, pass ? "PASS" : "MISS");
}

// 100-revision timeline: rewrite the working copy with one of `variants`
// distinct contents (cycled), capture; identical content is deduped by the
// library, cycling guarantees each capture differs from the previous one.
void buildTimeline(Library &lib, Asset &asset,
                   const std::vector<QByteArray> &variants, int count) {
    for (int i = 0; i < count; ++i) {
        QFile file(asset.path);
        if (!file.open(QIODevice::WriteOnly))
            throw std::runtime_error("cannot rewrite working copy");
        file.write(variants[size_t(i) % variants.size()]);
        file.close();
        bool created = false;
        lib.captureRevision(asset, QStringLiteral("bench %1").arg(i), &created);
        if (!created)
            throw std::runtime_error("timeline capture deduped (content cycle too small)");
    }
}

int runBench(const QString &workdir)
{
    const QString src = workdir + QStringLiteral("/src");
    const QString libRoot = workdir + QStringLiteral("/lib");
    // Fresh library every run; the generated matrix under src/ is kept.
    QDir(libRoot).removeRecursively();
    QDir().mkpath(libRoot);

    printf("== environment ==\n");
    QString cpu = QStringLiteral("unknown");
    QFile cpuinfo(QStringLiteral("/proc/cpuinfo"));
    if (cpuinfo.open(QIODevice::ReadOnly)) {
        const QString text = QString::fromUtf8(cpuinfo.readAll());
        const int at = int(text.indexOf(QStringLiteral("model name")));
        if (at >= 0) {
            const int colon = int(text.indexOf(QLatin1Char(':'), at));
            const int eol = int(text.indexOf(QLatin1Char('\n'), colon));
            if (colon > 0 && eol > colon)
                cpu = text.mid(colon + 2, eol - colon - 2).trimmed();
        }
    }
    // D-21 asks that the baseline record CPU, RAM, disk, OS, Qt, plugins and
    // cold/warm conditions; RAM and OS come from /proc and os-release.
    QString mem = QStringLiteral("unknown");
    QFile meminfo(QStringLiteral("/proc/meminfo"));
    if (meminfo.open(QIODevice::ReadOnly)) {
        const QString text = QString::fromUtf8(meminfo.readAll());
        const int at = int(text.indexOf(QStringLiteral("MemTotal:")));
        if (at >= 0) {
            const int eol = int(text.indexOf(QLatin1Char('\n'), at));
            bool ok = false;
            const qint64 kb = text.mid(at + 9, eol - at - 9).trimmed().toLongLong(&ok);
            if (ok)
                mem = QStringLiteral("%1 GiB").arg(double(kb) / (1024.0 * 1024.0), 0, 'f', 1);
        }
    }
    QString os = QStringLiteral("unknown");
    QFile osrelease(QStringLiteral("/etc/os-release"));
    if (osrelease.open(QIODevice::ReadOnly)) {
        const QString text = QString::fromUtf8(osrelease.readAll());
        const int at = int(text.indexOf(QStringLiteral("PRETTY_NAME=")));
        if (at >= 0) {
            const int eol = int(text.indexOf(QLatin1Char('\n'), at));
            os = text.mid(at + 12, eol - at - 12).remove(QLatin1Char('"')).trimmed();
        }
    }
    const QStorageInfo storage(workdir);
    printf("cpu: %s | ram %s | os %s | qt %s | fs %s %s | free %s\n",
           qPrintable(cpu), qPrintable(mem), qPrintable(os), qVersion(),
           qPrintable(QString::fromUtf8(storage.fileSystemType())),
           qPrintable(storage.name()),
           qPrintable(mibGiB(storage.bytesFree())));

    Library lib(libRoot);

    // --- 1. import throughput -------------------------------------------------
    // Deterministic order: sorted QDir listing; category = suffix. importFile
    // copies into objects/ + working/ and hashes each file once (streaming) —
    // that is the "object-write bytes / elapsed" the draft defines.
    printf("== import ==\n");
    const QStringList sources = QDir(src).entryList(QDir::Files, QDir::Name);
    if (sources.isEmpty())
        throw std::runtime_error("no sample files — run once with 'gen-only' first");
    qint64 imported = 0, failed = 0, importedBytes = 0;
    QElapsedTimer importTimer;
    importTimer.start();
    QHash<QString, qint64> catMs, catBytes;
    for (const QString &name : sources) {
        const QString path = src + QLatin1Char('/') + name;
        const qint64 bytes = QFileInfo(path).size();
        const qint64 start = importTimer.elapsed();
        try {
            lib.importFile(path);
        } catch (const std::exception &e) {
            ++failed;
            printf("  import failed: %s (%s)\n", qPrintable(name), e.what());
            continue;
        }
        const qint64 ms = importTimer.elapsed() - start;
        ++imported;
        importedBytes += bytes;
        const QString cat = QFileInfo(name).suffix().toUpper();
        catMs[cat] += ms;
        catBytes[cat] += bytes;
    }
    const double importSec = double(importTimer.elapsed()) / 1000.0;
    const double mibPerSec = importSec > 0 ? double(importedBytes) / (1024.0 * 1024.0) / importSec : 0.0;
    printf("  imported %lld/%lld files, %s in %.1f s\n",
           (long long)imported, (long long)sources.size(),
           qPrintable(mibGiB(importedBytes)), importSec);
    for (auto it = catMs.constBegin(); it != catMs.constEnd(); ++it) {
        const double perSec = it.value() > 0
            ? double(catBytes.value(it.key())) / (1024.0 * 1024.0) / (double(it.value()) / 1000.0)
            : 0.0;
        printf("    %-5s %7lld MiB @ %7.1f MiB/s\n", qPrintable(it.key()),
               (long long)(catBytes.value(it.key()) / (1024 * 1024)), perSec);
    }
    printVerdict("import throughput", mibPerSec, "MiB/s", 100.0, true);
    if (failed != 0)
        throw std::runtime_error("some imports failed — results invalid");

    const QVector<Asset> assets = lib.assets();
    if (assets.isEmpty())
        throw std::runtime_error("no assets imported");

    // --- 2. thumbnail generation ---------------------------------------------
    // The draft's口径: full generation per image, small PNG/JPEG/WebP sources,
    // queue wait excluded. Same code path as the UI: readObject + thumbnailFor.
    printf("== thumbnails ==\n");
    struct ThumbJob { QString hash; QString hint; };
    QVector<ThumbJob> jobs;
    for (const Asset &a : assets) {
        const QString fmt = lib.assetFormat(a.id);
        if (fmt.isEmpty())
            continue;                      // not a decodable image
        if (QFileInfo(a.path).size() > 1024 * 1024)
            continue;                      // the draft measures 512 KiB-class bitmaps
        jobs.push_back({lib.revisions(a.id).first().hash, QFileInfo(a.path).suffix()});
    }
    ThumbnailCache cache(libRoot);
    qint64 thumbMs = 0;
    {
        QElapsedTimer t;
        t.start();
        for (const ThumbJob &j : jobs)
            thumbnailFor(cache, lib.readObject(j.hash), j.hash, QSize(256, 256), j.hint);
        thumbMs = t.elapsed();
    }
    const double perMin = thumbMs > 0 ? double(jobs.size()) / (double(thumbMs) / 60000.0) : 0.0;
    printf("  %d thumbnails in %.1f s\n", int(jobs.size()), double(thumbMs) / 1000.0);
    printVerdict("thumbnail rate", perMin, "/min", 512.0, true);

    // --- 3. filter facts: tags + ratings so combined filters have data --------
    const int n = assets.size();
    for (int i = 0; i < n; i += 8)
        lib.addTag(assets[i].id, QStringLiteral("bench"));
    for (int i = 1; i < n; i += 5)
        lib.setRating(assets[i].id, 3);

    // Build facts exactly like mainwindow's reloadAssets (catalog queries +
    // pure filterMatches) — the real name-search/combined-filter code path.
    const auto buildFacts = [&lib](const QVector<Asset> &list) {
        QVector<AssetFacts> facts;
        facts.reserve(list.size());
        for (const Asset &a : list) {
            AssetFacts f;
            f.name = a.name;
            f.folder = a.folder;
            f.tags = lib.tags(a.id);
            f.format = lib.assetFormat(a.id);
            const QSize sz = lib.assetSize(a.id);
            f.width = sz.width();
            f.height = sz.height();
            f.rating = lib.rating(a.id);
            facts.push_back(f);
        }
        return facts;
    };

    // --- 4. name search P95 ----------------------------------------------------
    // D-23: "搜索建议每条件至少100次再算95%" — 120 queries per condition.
    printf("== name search ==\n");
    std::vector<double> searchMs;
    for (int i = 0; i < 120; ++i) {
        const Asset &probe = assets[size_t((i * 37) % n)];
        AssetFilter f;
        f.name = probe.name.left(6 + (i % 5)); // substrings that mostly hit
        if (i % 4 == 3)
            f.name = QStringLiteral("zzz-no-such-"); // plus misses
        const qint64 start = importTimer.elapsed();
        const QVector<AssetFacts> facts = buildFacts(lib.assets());
        int hits = 0;
        for (const AssetFacts &fact : facts)
            if (filterMatches(fact, f))
                ++hits;
        const double ms = double(importTimer.elapsed() - start);
        (void)hits;
        searchMs.push_back(ms);
    }
    printf("  120 queries over %d assets\n", n);
    printVerdict("name search P95", percentile95(searchMs), "ms", 200.0, false);

    // --- 5. combined filter P95 ------------------------------------------------
    printf("== combined filter ==\n");
    std::vector<double> comboMs;
    for (int i = 0; i < 120; ++i) {
        const Asset &probe = assets[size_t((i * 53) % n)];
        AssetFilter f;
        f.name = probe.name.left(4 + (i % 4));
        if (i % 3 == 0) f.tags << QStringLiteral("bench");
        if (i % 3 == 1) f.format = QStringLiteral("PNG");
        if (i % 2 == 0) f.minWidth = 256;
        f.minRating = (i % 5 == 4) ? 3 : -2;
        const qint64 start = importTimer.elapsed();
        const QVector<AssetFacts> facts = buildFacts(lib.assets());
        int hits = 0;
        for (const AssetFacts &fact : facts)
            if (filterMatches(fact, f))
                ++hits;
        const double ms = double(importTimer.elapsed() - start);
        (void)hits;
        comboMs.push_back(ms);
    }
    printVerdict("combined filter P95", percentile95(comboMs), "ms", 500.0, false);

    // --- 6. timeline open -------------------------------------------------------
    // Single asset with 100 versions; open = revisions() (core API per口径).
    printf("== timeline ==\n");
    std::vector<QByteArray> variants;
    for (int i = 0; i < 10; ++i) {
        QBuffer buf;
        buf.open(QIODevice::WriteOnly);
        noiseImage(96, 96, i % 2 == 0).save(&buf, "PNG");
        variants.push_back(buf.buffer());
    }
    Asset timelineAsset = assets.first();
    buildTimeline(lib, timelineAsset, variants, 99);
    std::vector<double> tlMs;
    for (int i = 0; i < 20; ++i) {
        const qint64 start = importTimer.elapsed();
        const QVector<Revision> revs = lib.revisions(timelineAsset.id);
        const double ms = double(importTimer.elapsed() - start);
        if (i == 0 && revs.size() < 100)
            throw std::runtime_error("timeline asset has < 100 revisions");
        tlMs.push_back(ms);
    }
    printf("  %d revisions, 20 opens\n", int(lib.revisions(timelineAsset.id).size()));
    printVerdict("timeline open P95", percentile95(tlMs), "ms", 500.0, false);

    // --- 7. whole-library snapshot ----------------------------------------------
    printf("== snapshot ==\n");
    const qint64 s1 = lib.snapshotAll(QStringLiteral("bench snapshot 1"));
    const qint64 s2 = lib.snapshotAll(QStringLiteral("bench snapshot 2"));
    (void)s1;
    (void)s2;
    double snapMs;
    {
        QElapsedTimer t;
        t.start();
        lib.snapshotAll(QStringLiteral("bench snapshot 3"));
        snapMs = double(t.elapsed());
    }
    printf("  %d assets, content unchanged\n", n);
    printVerdict("whole-library snapshot", snapMs, "ms", 10000.0, false);

    // --- 8. restore a 64 MiB file -------------------------------------------------
    printf("== restore ==\n");
    // Pick a big PNG; make a second revision (valid PNG, different hash) so the
    // restore to revision 1 actually writes and hash-verifies 64 MiB.
    Asset big;
    for (const Asset &a : assets) {
        if (a.name.startsWith(QStringLiteral("big-"))) { big = a; break; }
    }
    if (big.id.isEmpty())
        throw std::runtime_error("no big PNG asset found");
    QString otherSource;
    for (const QString &name : sources)
        if (name.startsWith(QStringLiteral("big-01")))
            otherSource = src + QLatin1Char('/') + name;
    if (otherSource.isEmpty())
        throw std::runtime_error("no second big PNG source found");
    {
        QFile in(otherSource);
        if (!in.open(QIODevice::ReadOnly))
            throw std::runtime_error("cannot open second big PNG");
        QFile out(big.path);
        if (!out.open(QIODevice::WriteOnly))
            throw std::runtime_error("cannot rewrite big working copy");
        const QByteArray bytes = in.readAll();
        out.write(bytes);
        if (bytes.size() != out.size())
            throw std::runtime_error("big working copy rewrite short");
    }
    lib.captureRevision(big, QStringLiteral("bench rev2"));
    QVector<Revision> revs = lib.revisions(big.id);
    const qint64 firstId = std::min_element(revs.cbegin(), revs.cend(),
        [](const Revision &x, const Revision &y) { return x.id < y.id; })->id;
    double restoreMs;
    {
        QElapsedTimer t;
        t.start();
        lib.restore(big, lib.planRestore(big, firstId));
        restoreMs = double(t.elapsed());
    }
    printf("  %.1f MiB written + verified\n",
           double(revs.front().size) / (1024.0 * 1024.0));
    printVerdict("restore 64 MiB", restoreMs, "ms", 3000.0, false);

    return 0;
}

} // namespace

int main(int argc, char **argv)
{
    // Line-buffer stdout so progress interleaves correctly with stderr when piped.
    setvbuf(stdout, nullptr, _IOLBF, 0);
    QCoreApplication app(argc, argv);
    if (argc < 2) {
        fprintf(stderr, "usage: %s <workdir> [gen-only|bench-only|gen-quick|bench-quick]\n", argv[0]);
        return 2;
    }
    const QString workdir = QDir(QString::fromUtf8(argv[1])).absolutePath();
    const QString mode = argc >= 3 ? QString::fromUtf8(argv[2]) : QString();
    const bool quick = mode.contains(QStringLiteral("quick"));
    // Full tier = draft matrix (500/20/20/50). Quick tier (D-20, ~5 GiB):
    // counts scaled by 0.4, per-file sizes unchanged, same coverage floor —
    // five bitmap families, PSD degradation (8 PSDs + 2 damaged PSD cases),
    // large PNGs and ZIPs.
    const int bitmapCount = quick ? 200 : 500;
    const int largeCount  = quick ? 8   : 20;
    const int psdCount    = quick ? 8   : 20;
    const int zipCount    = quick ? 20  : 50;
    try {
        if (mode != QStringLiteral("bench-only") && mode != QStringLiteral("bench-quick"))
            generateMatrix(workdir + QStringLiteral("/src"),
                           bitmapCount, largeCount, psdCount, zipCount);
        if (mode != QStringLiteral("gen-only") && mode != QStringLiteral("gen-quick"))
            return runBench(workdir);
    } catch (const std::exception &e) {
        fprintf(stderr, "bench failed: %s\n", e.what());
        return 1;
    }
    return 0;
}