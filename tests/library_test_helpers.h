// Shared samples and catalog helpers for the library_* test suites
// (LAY-001: extracted verbatim from the former single-file suite so each
// domain suite stays self-contained). Internal linkage: one copy per TU.
#include <QByteArray>
#include <QCryptographicHash>
#include <QDataStream>
#include <QDateTime>
#include <QFile>
#include <QImage>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QUuid>
#include <QtTest>

namespace {
void put(const QString &path, const QByteArray &data) {
    QFile file(path); QVERIFY(file.open(QIODevice::WriteOnly)); QCOMPARE(file.write(data), data.size());
}
QByteArray get(const QString &path) { QFile file(path); return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray(); }
QString digestHex(const QByteArray &data) {
    return QString::fromLatin1(QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex());
}
// P-320 test support: write a 'planned' restore-log row directly into the
// catalog, simulating a crash between the file replacement and the index commit
// (the exact state convergeRestoreLog() must handle on reopen).
void injectPendingRestoreLog(const QString &root, const QString &assetId, qint64 revisionId,
                             const QString &prevHash, const QString &targetHash, qint64 protectId) {
    const QString conn = QStringLiteral("inject_") + QUuid::createUuid().toString(QUuid::WithoutBraces);
    {
        QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", conn);
        db.setDatabaseName(root + "/catalog.sqlite");
        QVERIFY(db.open());
        QSqlQuery q(db);
        q.prepare("INSERT INTO restore_log(asset_id,revision_id,state,previous_hash,target_hash,"
                  "protect_revision_id,error,created,updated) VALUES(?,?,?,?,?,?,'',?,?)");
        const QString now = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
        q.addBindValue(assetId);
        q.addBindValue(revisionId);
        q.addBindValue(QStringLiteral("planned"));
        q.addBindValue(prevHash);
        q.addBindValue(targetHash);
        q.addBindValue(protectId);
        q.addBindValue(now);
        q.addBindValue(now);
        QVERIFY(q.exec());
        db.close();
    }
    QSqlDatabase::removeDatabase(conn);
}

// A known-good uncompressed 32-bit BGRA TGA (Qt can read TGA but not write it,
// so the sample is built byte-exact to validate the decoder, P-210). The Qt TGA
// plugin only accepts TrueVision 2.0 files, which require the 26-byte footer
// carrying the "TRUEVISION-XFILE" signature at offset 8.
QByteArray makeTga(int w, int h, QRgb fill) {
    QByteArray out;
    out.reserve(18 + w * h * 4 + 26);
    out.append(char(0));       // id length
    out.append(char(0));       // color map type
    out.append(char(2));       // uncompressed truecolor
    out.append(QByteArray(5, '\0')); // color map spec
    out.append(char(0)).append(char(0)); // x origin
    out.append(char(0)).append(char(0)); // y origin
    out.append(char(w & 0xff)).append(char((w >> 8) & 0xff));
    out.append(char(h & 0xff)).append(char((h >> 8) & 0xff));
    out.append(char(32));      // pixel depth
    out.append(char(0x28));    // 8 alpha bits + top-left origin
    for (int i = 0; i < w * h; ++i) {
        out.append(char(qBlue(fill)));   // B
        out.append(char(qGreen(fill)));  // G
        out.append(char(qRed(fill)));    // R
        out.append(char(qAlpha(fill)));  // A
    }
    out.append(QByteArray(8, '\0'));               // ext + developer area offsets
    out.append(QByteArray("TRUEVISION-XFILE", 16)); // TrueVision 2.0 signature
    out.append(char(0)).append(char(0));           // NUL + padding
    return out;
}

// A real PSD header (26 bytes, big-endian "8BPS") so the P-211 sample is a
// genuine Photoshop file, not an arbitrary blob; a composite follows in a real
// file but Qt has no PSD codec, so the header is what the signature check sees.
QByteArray makePsd(int w, int h) {
    QByteArray out;
    QDataStream ds(&out, QIODevice::WriteOnly);
    ds.setByteOrder(QDataStream::BigEndian);
    ds.writeRawData("8BPS", 4);
    ds << quint16(1);                    // version
    ds.writeRawData("\0\0\0\0\0\0", 6);  // reserved
    ds << quint16(3);                    // channels
    ds << quint32(h);                    // height
    ds << quint32(w);                    // width
    ds << quint16(8);                    // bits per channel
    ds << quint16(3);                    // color mode: RGB
    return out;
}
}


