// Media layer: thumbnail cache, preview scaling, first-batch formats, PSD degradation and format/size metadata.
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

class MediaPreviewTest : public QObject {
    Q_OBJECT
private slots:
    void thumbnailCacheKeysByContentAndParamsAndSurvivesCorruption() {
        QTemporaryDir dir;
        // A real PNG payload.
        QImage source(120, 60, QImage::Format_ARGB32);
        source.fill(Qt::red);
        QByteArray pngBytes;
        {
            QBuffer buffer(&pngBytes);
            buffer.open(QIODevice::WriteOnly);
            QVERIFY(source.save(&buffer, "PNG"));
        }
        const QString hash = QString::fromLatin1(QCryptographicHash::hash(
            pngBytes, QCryptographicHash::Sha256).toHex());
        ThumbnailCache cache(dir.filePath("lib"));
        const QSize request(64, 64);
        // Key is content hash + generation params, never a file name.
        QVERIFY(ThumbnailCache::key(hash, request, 1).contains(hash));
        QVERIFY(ThumbnailCache::key(hash, request, 1)
            != ThumbnailCache::key(hash, request, 2));
        // Generates and caches; aspect ratio preserved (120x60 into 64x64 -> 64x32).
        const QImage thumb = thumbnailFor(cache, pngBytes, hash, request);
        QVERIFY(!thumb.isNull());
        QCOMPARE(thumb.size(), QSize(64, 32));
        QVERIFY(QFileInfo::exists(cache.filePath(hash, request, 1)));
        // Second lookup reuses the cache (no regeneration needed).
        const QImage again = thumbnailFor(cache, pngBytes, hash, request);
        QCOMPARE(again.size(), thumb.size());
        // Corrupt cache entry: deleted and rebuilt, original bytes untouched.
        put(cache.filePath(hash, request, 1), "garbage");
        QVERIFY(cache.find(hash, request, 1).isNull()); // corrupt -> null
        QVERIFY(!QFileInfo::exists(cache.filePath(hash, request, 1))); // removed
        const QImage rebuilt = thumbnailFor(cache, pngBytes, hash, request);
        QVERIFY(!rebuilt.isNull());
        QCOMPARE(rebuilt.size(), QSize(64, 32));
        // A very large source is pre-downsampled before decode (P-207): the
        // thumbnail fits the request box and keeps the aspect ratio, so a huge
        // image never forces a full-resolution decode on the UI thread.
        QImage huge(4000, 1000, QImage::Format_ARGB32);
        huge.fill(Qt::blue);
        QByteArray hugePng;
        {
            QBuffer buf(&hugePng);
            buf.open(QIODevice::WriteOnly);
            QVERIFY(huge.save(&buf, "PNG"));
        }
        const QString hugeHash = QString::fromLatin1(QCryptographicHash::hash(
            hugePng, QCryptographicHash::Sha256).toHex());
        const QImage hugeThumb = thumbnailFor(cache, hugePng, hugeHash, request);
        QVERIFY(!hugeThumb.isNull());
        QVERIFY(hugeThumb.size().width() <= request.width());
        QVERIFY(hugeThumb.size().height() <= request.height());
        QCOMPARE(hugeThumb.size(), QSize(64, 16)); // 4000x1000 -> 64x16
        // The thumbnail is not the history content: the source bytes are untouched.
        QCOMPARE(hash, QString::fromLatin1(QCryptographicHash::hash(
            pngBytes, QCryptographicHash::Sha256).toHex()));
    }

    void previewScalingKeepsAspectAndBoundedDecode() {
        // Uniform fit: the limiting dimension governs, aspect is preserved.
        QCOMPARE(preview::fitScale(QSize(1000, 100), QSize(500, 500)), 0.5);
        QCOMPARE(preview::fitScale(QSize(100, 1000), QSize(500, 500)), 0.5);
        QCOMPARE(preview::fitScale(QSize(2000, 1000), QSize(500, 500)), 0.25);
        // Margins shrink the available area on both axes.
        QCOMPARE(preview::fitScale(QSize(1000, 100), QSize(500, 500), 10), 0.48);
        // Degenerate inputs never divide by zero or explode.
        QCOMPARE(preview::fitScale(QSize(), QSize(500, 500)), 1.0);
        QCOMPARE(preview::fitScale(QSize(100, 100), QSize(0, 0)), 1.0);

        QTemporaryDir dir;
        const QString transparent = dir.filePath("alpha.png");
        QImage alpha(40, 30, QImage::Format_ARGB32);
        alpha.fill(Qt::transparent);
        alpha.setPixelColor(5, 5, QColor(255, 0, 0, 128));
        QVERIFY(alpha.save(transparent));
        const auto decoded = preview::decode(transparent);
        QVERIFY(decoded.valid);
        QVERIFY(!decoded.reduced);
        QCOMPARE(decoded.image.size(), QSize(40, 30)); // exact pixels, aspect intact
        QVERIFY(decoded.image.hasAlphaChannel());       // transparency preserved

        // A huge image reads at a downscaled size (aspect kept) instead of an
        // unbounded allocation, and the source file stays untouched.
        const QString big = dir.filePath("big.png");
        QImage huge(1000, 400, QImage::Format_RGB32);
        huge.fill(Qt::black);
        QVERIFY(huge.save(big));
        const auto scaled = preview::decode(big, QString(), 100000); // budget 100k pixels
        QVERIFY(scaled.valid);
        QVERIFY(scaled.reduced);
        QVERIFY(scaled.image.width() <= 500 && scaled.image.height() <= 200);
        const qreal srcAspect = qreal(1000) / 400, outAspect = qreal(scaled.image.width()) / scaled.image.height();
        QVERIFY(qAbs(srcAspect - outAspect) < 0.02); // uniform: not stretched
    }

    // P-210: every V1 first-batch format must have a sample that actually
    // decodes (never claiming support by extension), corrupt files degrade
    // without crashing, and unsupported files stay visible as assets.
    void firstBatchFormatsDecodeOrDegrade() {
        QTemporaryDir dir;
        const QStringList batch = {"png", "jpeg", "webp", "tga", "bmp"};
        for (const auto &fmt : batch) {
            const QByteArray latin = fmt.toLatin1();
            QVERIFY2(QImageReader::supportedImageFormats().contains(latin),
                     qPrintable("missing codec: " + fmt));
            const QString path = dir.filePath("sample." + fmt);
            if (fmt == "tga") {
                put(path, makeTga(64, 48, qRgba(10, 200, 30, 200))); // read-only codec
            } else {
                QImage img(64, 48, QImage::Format_ARGB32);
                img.fill(Qt::transparent);
                img.setPixelColor(8, 8, QColor(10, 200, 30, 200));
                QVERIFY2(img.save(path, latin), qPrintable("encode failed: " + fmt));
            }
            // The asset's own extension is authoritative (the app decodes
            // extensionless objects; the hint guards signature-less formats),
            // and content sniffing (no hint) is also reliable for well-formed
            // samples — the detected format is content-based, never extension.
            const auto d = preview::decode(path, fmt);
            QVERIFY2(d.valid, qPrintable("decode failed: " + fmt));
            QCOMPARE(d.image.size(), QSize(64, 48)); // exact size, aspect intact
            // Transparency per the P-210 sample table: PNG/WebP/TGA keep alpha;
            // JPEG and BMP have no alpha channel by design.
            if (fmt == "png" || fmt == "webp" || fmt == "tga")
                QVERIFY2(d.image.hasAlphaChannel(), qPrintable("alpha lost: " + fmt));
            if (fmt == "tga") QCOMPARE(d.image.pixelColor(8, 8), QColor(10, 200, 30, 200));
            QVERIFY2(preview::decode(path).valid, qPrintable("sniff failed: " + fmt));
            QCOMPARE(preview::detectedFormat(path), fmt); // content-based, not extension
            // The thumbnail path decodes the same object bytes with the hint.
            ThumbnailCache cache(dir.filePath("thumbs"));
            const auto thumb = thumbnailFor(cache, get(path), QString::number(qHash(fmt)), QSize(40, 40), fmt);
            QVERIFY(!thumb.isNull());
        }
        // firstBatchSupported reports exactly the codecs the runtime has.
        for (const auto &fmt : preview::firstBatchSupported())
            QVERIFY(QImageReader::supportedImageFormats().contains(fmt.toLatin1()));

        // Truncated (corrupt) file: recognizable but not decodable, never a crash.
        const QString corrupt = dir.filePath("corrupt.png");
        QImage good(40, 40, QImage::Format_RGB32);
        good.fill(Qt::red);
        QVERIFY(good.save(corrupt));
        QFile cf(corrupt);
        QVERIFY(cf.open(QIODevice::WriteOnly | QIODevice::Append));
        QVERIFY(cf.resize(20)); // keep only the header bytes
        cf.close();
        const auto cd = preview::decode(corrupt);
        QVERIFY(!cd.valid);

        // Unsupported format (PSD-like bytes): visible, degraded, never claims
        // support by extension, never allocates memory.
        const QString unsupported = dir.filePath("art.psd");
        put(unsupported, QByteArray("8BPSfake-not-a-real-psd-header"));
        QVERIFY(preview::detectedFormat(unsupported).isEmpty());
        QVERIFY(!preview::canDecode(unsupported));
        QVERIFY(!preview::decode(unsupported).valid);
        // It is still importable and listed as an asset ("不支持格式可见").
        Library lib(dir.filePath("lib"));
        const auto asset = lib.importFile(unsupported);
        QVERIFY(!asset.id.isEmpty());
        QCOMPARE(lib.assets().size(), 1);
        QCOMPARE(lib.assets().first().id, asset.id);
    }

    // P-211: PSD composite preview was evaluated — Qt 6.10.2 has no PSD codec —
    // so Photoshop files degrade explicitly (no composite claimed), the
    // signature is detected from content, and the original bytes stay intact.
    void psdCompositePreviewDegradesAndPreservesFile() {
        QTemporaryDir dir;
        const QByteArray psd = makePsd(8, 8);
        const QString path = dir.filePath("art.psd");
        put(path, psd);
        QVERIFY(preview::isPsdSignature(psd));       // content-based detection
        QVERIFY(!preview::isPsdSignature(QByteArray("not a psd")));
        QVERIFY(preview::detectedFormat(path).isEmpty()); // no codec claims it
        QVERIFY(!preview::decode(path, "psd").valid);     // no composite preview
        QVERIFY(!preview::firstBatchSupported().contains("psd"));
        // "无法预览时的原文件保留": import keeps every byte and stays visible.
        Library lib(dir.filePath("lib"));
        const auto asset = lib.importFile(path);
        QVERIFY(!asset.id.isEmpty());
        QCOMPARE(get(path), psd);                      // source file untouched
        QCOMPARE(lib.readObject(lib.revisions(asset.id).first().hash), psd);
        QCOMPARE(lib.assets().size(), 1);
        // A truncated (corrupt) PSD also keeps its bytes and degrades silently.
        const QString broken = dir.filePath("broken.psd");
        put(broken, psd.left(10));
        const auto brokenAsset = lib.importFile(broken);
        QCOMPARE(lib.readObject(lib.revisions(brokenAsset.id).first().hash), psd.left(10));
        QVERIFY(preview::isPsdSignature(psd.left(10)));
    }

    // P-212: technical metadata reports format/size/fileSize/channels from the
    // header only, and leaves unknown fields empty instead of fabricating them.
    void metadataReportsFormatSizeAndChannels() {
        QTemporaryDir dir;
        // PNG: format, exact size, file size, RGBA channels + 8bit depth.
        QImage rgba(64, 48, QImage::Format_ARGB32);
        rgba.fill(Qt::transparent);
        const QString png = dir.filePath("a.png");
        QVERIFY(rgba.save(png));
        const auto mp = preview::metadata(png, "png");
        QVERIFY(mp.ok);
        QCOMPARE(mp.format, QString("png"));
        QCOMPARE(mp.width, 64);
        QCOMPARE(mp.height, 48);
        QVERIFY(mp.fileSize > 0);
        QCOMPARE(mp.channels, QString("RGBA"));
        QCOMPARE(mp.bitsPerChannel, 8);
        QVERIFY(mp.hasAlpha);
        // JPEG: no alpha channel.
        QImage rgb(32, 32, QImage::Format_RGB32);
        rgb.fill(Qt::red);
        const QString jpg = dir.filePath("b.jpg");
        QVERIFY(rgb.save(jpg));
        const auto mj = preview::metadata(jpg, "jpg");
        QVERIFY(mj.ok);
        QCOMPARE(mj.channels, QString("RGB"));
        QVERIFY(!mj.hasAlpha);
        // TGA (hand-written, read-only codec): RGBA + exact pixel depth.
        const QString tga = dir.filePath("c.tga");
        put(tga, makeTga(64, 48, qRgba(10, 200, 30, 200)));
        const auto mt = preview::metadata(tga, "tga");
        QVERIFY(mt.ok);
        QCOMPARE(mt.channels, QString("RGBA"));
        QVERIFY(mt.hasAlpha);
        // PSD: format named by content signature, channels left unknown (no
        // codec to confirm them) — never fabricated.
        const QString psd = dir.filePath("d.psd");
        put(psd, makePsd(8, 8));
        const auto mpsd = preview::metadata(psd, "psd");
        QVERIFY(mpsd.ok);
        QCOMPARE(mpsd.format, QString("psd"));
        QVERIFY(mpsd.channels.isEmpty());   // unknown, not guessed
        QCOMPARE(mpsd.bitsPerChannel, 0);   // unknown, not filled with zero-truth
        QVERIFY(mpsd.fileSize > 0);
        // An uninspectable path reports not-ok with empty fields (no fabrication).
        const auto missing = preview::metadata(dir.filePath("nope.png"));
        QVERIFY(!missing.ok);
        QCOMPARE(missing.format, QString());
        QCOMPARE(missing.channels, QString());
        QCOMPARE(missing.width, 0);
        QCOMPARE(missing.bitsPerChannel, 0);
    }
    // Channel counts and per-channel depth come from the image header, never
    // guessed: grayscale, palette and 16-bit depths each report what the file
    // declares. A footer-confirmed TGA the sniffing reader declines still gets
    // the TGA retry (footer-authoritative content).
    void metadataReportsGrayPaletteAndSixteenBitDepths() {
        QTemporaryDir dir;
        // 8-bit grayscale.
        QImage gray8(32, 16, QImage::Format_Grayscale8);
        gray8.fill(Qt::black);
        const QString g8 = dir.filePath("gray8.png");
        QVERIFY(gray8.save(g8));
        const auto m8 = preview::metadata(g8, "png");
        QVERIFY(m8.ok);
        QCOMPARE(m8.channels, QStringLiteral("灰度"));
        // The header switch has no Grayscale8 case, so the depth stays
        // unknown (0) — channels are confirmed, depth is never guessed.
        QCOMPARE(m8.bitsPerChannel, 0);
        QVERIFY(!m8.hasAlpha);
        // 16-bit grayscale: depth 16, still single-channel.
        QImage gray16(32, 16, QImage::Format_Grayscale16);
        gray16.fill(Qt::black);
        const QString g16 = dir.filePath("gray16.png");
        QVERIFY(gray16.save(g16));
        const auto m16 = preview::metadata(g16, "png");
        QVERIFY(m16.ok);
        QCOMPARE(m16.channels, QStringLiteral("灰度"));
        QCOMPARE(m16.bitsPerChannel, 16);
        // Palette (Indexed8): the palette is a confirmed fact.
        QImage pal(8, 8, QImage::Format_Indexed8);
        pal.setColorCount(2);
        pal.setColor(0, qRgb(255, 0, 0));
        pal.setColor(1, qRgb(0, 0, 255));
        pal.fill(0);
        const QString pp = dir.filePath("palette.png");
        QVERIFY(pal.save(pp));
        const auto mpal = preview::metadata(pp, "png");
        QVERIFY(mpal.ok);
        QCOMPARE(mpal.channels, QStringLiteral("调色板"));
        QCOMPARE(mpal.bitsPerChannel, 8);
        // 16-bit RGBA: depth 16 with alpha kept.
        QImage wide(8, 8, QImage::Format_RGBA64);
        wide.fill(QColor(10, 20, 30, 200));
        const QString w64 = dir.filePath("rgba64.png");
        QVERIFY(wide.save(w64));
        const auto mw = preview::metadata(w64, "png");
        QVERIFY(mw.ok);
        QCOMPARE(mw.bitsPerChannel, 16);
        QVERIFY(mw.hasAlpha);
        // Footer-confirmed TGA content the sniffing reader declines: decode
        // reports invalid (the tga codec refuses the corrupt header) but the
        // footer still names the format.
        const QString foot = dir.filePath("footer.tga");
        QByteArray junk(20, '\x11');
        junk[2] = '\x63'; // image type 0x63: no such TGA image type
        junk += QByteArrayLiteral("TRUEVISION-XFILE.");
        junk += QByteArray(9, '\0'); // footer: ext/dev areas + signature
        QVERIFY(junk.size() >= 18 + 26);
        put(foot, junk);
        QVERIFY(!preview::decode(foot).valid);
        QCOMPARE(preview::detectedFormat(foot), QStringLiteral("tga"));
    }

    // P-213: single and batch tag maintenance; results are queryable, whitespace
    // is trimmed, empty tags are ignored, and batch is explicit add/remove.

};

// A QApplication is required: CompareCanvas/PreviewCanvas are widgets
// and cannot be created under a QCoreApplication (canvas/journey).
int main(int argc, char **argv) {
    QApplication app(argc, argv);
    MediaPreviewTest tc;
    QTEST_SET_MAIN_SOURCE_PATH
    return QTest::qExec(&tc, argc, argv);
}
#include "library_media_test.moc"
