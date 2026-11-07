#pragma once

#include <QString>

// P-509: manual address entry. Parses a user-typed host spec into a host +
// port + protocol triple, accepting hostname / IPv4 / bracketed IPv6,
// an optional :port, and an optional scheme that selects the protocol.
// Pure logic, no I/O — used by the "add address" form.
namespace remote_address {

struct Address {
    QString host;      // hostname or IP literal, brackets stripped
    quint16 port = 0;  // 0 = protocol default (caller applies it)
    QString protocol;  // "webdav" | "sftp" | "smb" | "nfs" | "unknown"
    QString error;     // non-empty = input is not a valid address
};

// `defaultProtocol` is used when the input has no scheme prefix. Known
// schemes: https/webdav -> webdav, sftp/ssh -> sftp, smb -> smb, nfs -> nfs.
Address parse(const QString &input, const QString &defaultProtocol = QString());

bool looksLikeHostname(const QString &s);
bool looksLikeIpv4(const QString &s);
bool looksLikeIpv6(const QString &s);

} // namespace remote_address
