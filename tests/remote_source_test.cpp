// P-509: manual remote address parsing — hostname/IPv4/IPv6/port/protocol.
#include "remote_address.h"

#include <QtTest>

class RemoteAddressTest : public QObject {
    Q_OBJECT
private slots:
    void parsesIpv4() {
        const auto a = remote_address::parse(QStringLiteral("192.168.1.5"));
        QVERIFY(a.error.isEmpty());
        QCOMPARE(a.host, QStringLiteral("192.168.1.5"));
        QCOMPARE(a.port, quint16(0));
    }
    void parsesHostnameAndPort() {
        const auto a = remote_address::parse(QStringLiteral("nas.local:8443"));
        QVERIFY(a.error.isEmpty());
        QCOMPARE(a.host, QStringLiteral("nas.local"));
        QCOMPARE(a.port, quint16(8443));
    }
    void parsesBracketedIpv6() {
        const auto a = remote_address::parse(QStringLiteral("[::1]:443"));
        QVERIFY(a.error.isEmpty());
        QCOMPARE(a.host, QStringLiteral("::1"));
        QCOMPARE(a.port, quint16(443));
    }
    void parsesBareIpv6() {
        const auto a = remote_address::parse(QStringLiteral("::1"));
        QVERIFY(a.error.isEmpty());
        QCOMPARE(a.host, QStringLiteral("::1"));
        QCOMPARE(a.port, quint16(0));
    }
    void parsesSchemeProtocols() {
        QCOMPARE(remote_address::parse(QStringLiteral("https://nas.local")).protocol,
                 QStringLiteral("webdav"));
        QCOMPARE(remote_address::parse(QStringLiteral("sftp://nas.local")).protocol,
                 QStringLiteral("sftp"));
        QCOMPARE(remote_address::parse(QStringLiteral("smb://nas")).protocol,
                 QStringLiteral("smb"));
        QCOMPARE(remote_address::parse(QStringLiteral("nfs://nas")).protocol,
                 QStringLiteral("nfs"));
    }
    void appliesDefaultProtocol() {
        const auto a = remote_address::parse(QStringLiteral("nas.local"),
                                             QStringLiteral("sftp"));
        QVERIFY(a.error.isEmpty());
        QCOMPARE(a.protocol, QStringLiteral("sftp"));
    }
    void stripsUserinfo() {
        const auto a = remote_address::parse(QStringLiteral("user@nas.local"));
        QVERIFY(a.error.isEmpty());
        QCOMPARE(a.host, QStringLiteral("nas.local"));
    }
    void rejectsInvalid() {
        QVERIFY(!remote_address::parse(QStringLiteral("")).error.isEmpty());
        QVERIFY(!remote_address::parse(QStringLiteral("nas.local:99999")).error.isEmpty());
        QVERIFY(!remote_address::parse(QStringLiteral("nope://nas.local")).error.isEmpty());
        QVERIFY(!remote_address::parse(QStringLiteral("bad..host")).error.isEmpty());
        QVERIFY(!remote_address::parse(QStringLiteral("[::1")).error.isEmpty());
    }
    void treatsAllNumericIpAsHostname() {
        // 300.1.1.1 is not a valid IPv4 but is a syntactically valid hostname
        // label set; the address parser accepts it and connect-time DNS does
        // the final decision.
        const auto a = remote_address::parse(QStringLiteral("300.1.1.1"));
        QVERIFY(a.error.isEmpty());
        QCOMPARE(a.host, QStringLiteral("300.1.1.1"));
    }
};

QTEST_APPLESS_MAIN(RemoteAddressTest)
#include "remote_source_test.moc"
