// P-508: discovery result merge and expiry — same-NAS multi-address merge
// must not drop reachable options; stale records expire by ttl.
#include "discovery.h"

#include <QtTest>

class DiscoveryTest : public QObject {
    Q_OBJECT
private slots:
    void addsNewRecords() {
        const QDateTime now = QDateTime::currentDateTimeUtc();
        discovery::Record r;
        r.key = QStringLiteral("nas.local");
        r.addresses = {QStringLiteral("smb://nas.local")};
        const auto merged = discovery::merge({}, {r}, now);
        QCOMPARE(merged.size(), 1);
        QCOMPARE(merged.first().key, QStringLiteral("nas.local"));
        QCOMPARE(merged.first().addresses.size(), 1);
        QCOMPARE(merged.first().lastSeen, now);
    }
    void mergesSameNasAcrossProtocols() {
        const QDateTime now = QDateTime::currentDateTimeUtc();
        discovery::Record first;
        first.key = QStringLiteral("nas.local");
        first.addresses = {QStringLiteral("smb://nas.local")};
        const auto r1 = discovery::merge({}, {first}, now);
        // Same NAS now also reachable over sftp — one record, both options.
        discovery::Record second;
        second.key = QStringLiteral("nas.local");
        second.addresses = {QStringLiteral("sftp://nas.local")};
        const auto r2 = discovery::merge(r1, {second}, now);
        QCOMPARE(r2.size(), 1);
        QCOMPARE(r2.first().addresses.size(), 2);
        QVERIFY(r2.first().addresses.contains(QStringLiteral("smb://nas.local")));
        QVERIFY(r2.first().addresses.contains(QStringLiteral("sftp://nas.local")));
        QCOMPARE(r2.first().lastSeen, now);
    }
    void keepsUnseenRecordsUntilTtl() {
        const QDateTime t0 = QDateTime::currentDateTimeUtc();
        discovery::Record r;
        r.key = QStringLiteral("nas.local");
        r.addresses = {QStringLiteral("smb://nas.local")};
        r.ttlMs = 60000;
        const auto merged = discovery::merge({}, {r}, t0);
        // Seen at t0, then a refresh round at t0+30s with no mention: kept.
        const auto kept = discovery::merge(merged, {}, t0.addSecs(30));
        QCOMPARE(kept.size(), 1);
        // Past the ttl: dropped.
        const auto expired = discovery::merge(merged, {}, t0.addSecs(61));
        QCOMPARE(expired.size(), 0);
    }
    void neverExpiresWithZeroTtl() {
        const QDateTime t0 = QDateTime::currentDateTimeUtc();
        discovery::Record r;
        r.key = QStringLiteral("nas.local");
        r.addresses = {QStringLiteral("smb://nas.local")};
        r.ttlMs = 0; // never expires
        const auto merged = discovery::merge({}, {r}, t0);
        const auto later = discovery::merge(merged, {}, t0.addDays(30));
        QCOMPARE(later.size(), 1);
    }
    void ignoresUnkeyedRecords() {
        const QDateTime now = QDateTime::currentDateTimeUtc();
        discovery::Record r;
        r.addresses = {QStringLiteral("smb://nas.local")};
        QVERIFY(discovery::merge({}, {r}, now).isEmpty());
    }
};

QTEST_APPLESS_MAIN(DiscoveryTest)
#include "discovery_test.moc"
