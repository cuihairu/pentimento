#include "remote_address.h"

#include <QHostAddress>

namespace remote_address {

bool looksLikeHostname(const QString &s) {
    if (s.isEmpty() || s.size() > 253) return false;
    // Hostnames: labels of [A-Za-z0-9-], separated by dots, no empty label,
    // no leading/trailing hyphen in a label. IP literals are handled by the
    // IPv4/IPv6 checks, so this is the "word" fallback.
    for (const QString &label : s.split(QLatin1Char('.'))) {
        if (label.isEmpty() || label.size() > 63) return false;
        if (label.front() == QLatin1Char('-') || label.back() == QLatin1Char('-'))
            return false;
        for (const QChar c : label) {
            if (!c.isLetterOrNumber() && c != QLatin1Char('-')) return false;
        }
    }
    return true;
}

bool looksLikeIpv4(const QString &s) {
    QHostAddress addr;
    return addr.setAddress(s) && addr.protocol() == QAbstractSocket::IPv4Protocol;
}

bool looksLikeIpv6(const QString &s) {
    QHostAddress addr;
    return addr.setAddress(s) && addr.protocol() == QAbstractSocket::IPv6Protocol;
}

Address parse(const QString &input, const QString &defaultProtocol) {
    Address out;
    QString work = input.trimmed();
    if (work.isEmpty()) {
        out.error = QStringLiteral("empty address");
        return out;
    }

    // Scheme prefix: https:// / webdav:// -> webdav, sftp:// / ssh:// -> sftp,
    // smb:// -> smb, nfs:// -> nfs. Anything else is rejected as unknown.
    const int scheme = work.indexOf(QLatin1String("://"));
    if (scheme > 0) {
        const QString proto = work.left(scheme).toLower();
        if (proto == QLatin1String("https") || proto == QLatin1String("webdav") ||
            proto == QLatin1String("http")) {
            out.protocol = QStringLiteral("webdav");
        } else if (proto == QLatin1String("sftp") || proto == QLatin1String("ssh")) {
            out.protocol = QStringLiteral("sftp");
        } else if (proto == QLatin1String("smb")) {
            out.protocol = QStringLiteral("smb");
        } else if (proto == QLatin1String("nfs")) {
            out.protocol = QStringLiteral("nfs");
        } else {
            out.error = QStringLiteral("unknown protocol: ") + proto;
            return out;
        }
        work = work.mid(scheme + 3);
    } else {
        out.protocol = defaultProtocol.isEmpty() ? QStringLiteral("unknown")
                                                 : defaultProtocol;
    }

    // Strip a userinfo prefix (user@host) — credentials are never parsed
    // here (P-106 keeps them out of the index); the host part is all we need.
    const int at = work.lastIndexOf(QLatin1Char('@'));
    if (at >= 0) work = work.mid(at + 1);

    // Optional :port — but an unbracketed IPv6 literal contains colons, so
    // bracketed IPv6 "[...]:port" and a single trailing ":port" are handled
    // explicitly; a bare IPv6 literal is kept whole.
    QString portText;
    if (work.startsWith(QLatin1Char('['))) {
        const int close = work.indexOf(QLatin1Char(']'));
        if (close < 0) {
            out.error = QStringLiteral("unterminated IPv6 bracket");
            return out;
        }
        const QString host6 = work.mid(1, close - 1);
        if (!looksLikeIpv6(host6)) {
            out.error = QStringLiteral("invalid IPv6 address");
            return out;
        }
        out.host = host6;
        const QString rest = work.mid(close + 1);
        if (!rest.isEmpty()) {
            if (!rest.startsWith(QLatin1Char(':'))) {
                out.error = QStringLiteral("unexpected text after IPv6 address");
                return out;
            }
            portText = rest.mid(1);
        }
    } else if (looksLikeIpv6(work)) {
        // A bare IPv6 literal is a host with no port — its colons are part
        // of the address, not a host:port separator.
        out.host = work;
    } else {
        // Split at the last colon only when everything after it is digits
        // (so "host:443" splits but a bare IPv6 literal does not).
        const int colon = work.lastIndexOf(QLatin1Char(':'));
        if (colon >= 0) {
            const QString tail = work.mid(colon + 1);
            const QString head = work.left(colon);
            bool digitsOnly = !tail.isEmpty();
            for (const QChar c : tail) if (!c.isDigit()) { digitsOnly = false; break; }
            if (digitsOnly) {
                portText = tail;
                work = head;
            }
        }
        out.host = work;
    }

    if (out.host.isEmpty()) {
        out.error = QStringLiteral("missing host");
        return out;
    }
    if (!looksLikeIpv4(out.host) && !looksLikeIpv6(out.host) &&
        !looksLikeHostname(out.host)) {
        out.error = QStringLiteral("not a valid hostname or IP address");
        return out;
    }

    if (!portText.isEmpty()) {
        bool ok = false;
        const uint port = portText.toUInt(&ok);
        if (!ok || port == 0 || port > 65535) {
            out.error = QStringLiteral("invalid port");
            return out;
        }
        out.port = quint16(port);
    }
    return out;
}

} // namespace remote_address
