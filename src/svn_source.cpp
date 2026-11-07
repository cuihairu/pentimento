#include "svn_source.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QXmlStreamReader>

namespace {

// Run `svn <args>` with a usable working directory. Every invocation is
// --non-interactive (no auth prompt hang) and --no-auth-cache (no writes to
// ~/.subversion). Returns the exit code; stdout/stderr are captured.
int runSvn(const QString &cwd, const QStringList &args,
           QByteArray *stdoutData = nullptr, QByteArray *stderrData = nullptr) {
    QProcess svn;
    svn.setProgram(QStringLiteral("svn"));
    QStringList full{QStringLiteral("--non-interactive"),
                     QStringLiteral("--no-auth-cache")};
    full += args;
    svn.setArguments(full);
    svn.setWorkingDirectory(cwd);
    svn.start();
    if (!svn.waitForStarted(5000)) {
        if (stderrData) *stderrData = QByteArrayLiteral("svn: could not start");
        return -1;
    }
    if (!svn.waitForFinished(30000)) {
        svn.kill();
        svn.waitForFinished(2000);
        if (stderrData) *stderrData = QByteArrayLiteral("svn: timed out");
        return -1;
    }
    if (stdoutData) *stdoutData = svn.readAllStandardOutput();
    if (stderrData) *stderrData = svn.readAllStandardError();
    return svn.exitCode();
}

QString errorText(const QByteArray &err, const QByteArray &out) {
    QString text = QString::fromUtf8(err).trimmed();
    if (text.isEmpty()) text = QString::fromUtf8(out).trimmed();
    return text;
}

// A directory path is used as cwd directly; a file path uses its parent; a
// URL (not a local path) falls back to the current directory.
QString cwdFor(const QString &path) {
    const QFileInfo fi(path);
    if (fi.isDir()) return path;
    if (fi.exists()) return fi.absolutePath();
    return QDir::currentPath();
}

QString attr(const QXmlStreamAttributes &a, const char *name) {
    return a.value(QLatin1String(name)).toString();
}

qint64 parseIsoMs(const QString &iso) {
    const QDateTime dt = QDateTime::fromString(iso, Qt::ISODateWithMs);
    return dt.isValid() ? dt.toMSecsSinceEpoch() : 0;
}

// Parse the revision attribute of the <entry> of `svn info --xml`.
qint64 entryRevision(const QByteArray &xmlBytes) {
    QXmlStreamReader xml(xmlBytes);
    while (!xml.atEnd()) {
        xml.readNext();
        if (xml.isStartElement() && xml.name() == QLatin1String("entry"))
            return attr(xml.attributes(), "revision").toLongLong();
    }
    return -1;
}

} // namespace

namespace svn_source {

RepoInfo identify(const QString &path) {
    RepoInfo info;
    const QString cwd = cwdFor(path);
    QByteArray out, err;
    if (runSvn(cwd, {QStringLiteral("info"), QStringLiteral("--xml"), path},
               &out, &err) != 0) {
        info.error = errorText(err, out);
        return info;
    }
    QXmlStreamReader xml(out);
    while (!xml.atEnd()) {
        xml.readNext();
        if (!xml.isStartElement()) continue;
        const QStringView name = xml.name();
        if (name == QLatin1String("entry")) {
            info.kind = attr(xml.attributes(), "kind");
            info.revision = attr(xml.attributes(), "revision").toLongLong();
        } else if (name == QLatin1String("url")) {
            info.url = xml.readElementText();
        } else if (name == QLatin1String("root")) {
            info.repoRoot = xml.readElementText();
        } else if (name == QLatin1String("wc-info")) {
            info.workingCopy = true;
        }
    }
    // Repository HEAD revision, for a stable "what is current" answer.
    QByteArray headOut, headErr;
    if (runSvn(cwd, {QStringLiteral("info"), QStringLiteral("-r"),
                     QStringLiteral("HEAD"), QStringLiteral("--xml"), path},
               &headOut, &headErr) == 0)
        info.headRevision = entryRevision(headOut);
    return info;
}

QVector<LogEntry> log(const QString &path, int limit, QString *error) {
    QVector<LogEntry> result;
    if (limit < 1) limit = 1;
    // Repo-wide history: log the repository root, not the working-copy node
    // (the root dir node itself has no commits, so logging `path` directly
    // would come back empty).
    const RepoInfo info = identify(path);
    if (!info.error.isEmpty()) {
        if (error) *error = info.error;
        return result;
    }
    QByteArray out, err;
    if (runSvn(cwdFor(path), {QStringLiteral("log"), QStringLiteral("--xml"),
                              QStringLiteral("-l"), QString::number(limit),
                              info.repoRoot},
               &out, &err) != 0) {
        if (error) *error = errorText(err, out);
        return result;
    }
    QXmlStreamReader xml(out);
    while (!xml.atEnd()) {
        xml.readNext();
        if (!xml.isStartElement()) continue;
        if (xml.name() != QLatin1String("logentry")) continue;
        LogEntry entry;
        entry.revision = attr(xml.attributes(), "revision").toLongLong();
        // <author>, <date>, <msg> are children of logentry.
        while (xml.readNext() && !(xml.isEndElement() &&
                                   xml.name() == QLatin1String("logentry"))) {
            if (!xml.isStartElement()) continue;
            const QStringView n = xml.name();
            if (n == QLatin1String("author"))
                entry.author = xml.readElementText();
            else if (n == QLatin1String("date"))
                entry.whenMs = parseIsoMs(xml.readElementText());
            else if (n == QLatin1String("msg"))
                entry.message = xml.readElementText();
        }
        result.push_back(entry);
    }
    if (error) error->clear();
    return result;
}

QVector<TreeEntry> tree(const QString &path, qint64 revision,
                        const QString &subdir, QString *error) {
    QVector<TreeEntry> result;
    const RepoInfo info = identify(path);
    if (!info.error.isEmpty()) {
        if (error) *error = info.error;
        return result;
    }
    const QString rev = revision > 0 ? QString::number(revision)
                                     : QStringLiteral("HEAD");
    QString target = info.repoRoot;
    if (!subdir.isEmpty()) target += QLatin1Char('/') + subdir;
    QByteArray out, err;
    if (runSvn(cwdFor(path), {QStringLiteral("ls"), QStringLiteral("-R"),
                              QStringLiteral("--xml"), QStringLiteral("-r"), rev,
                              target},
               &out, &err) != 0) {
        if (error) *error = errorText(err, out);
        return result;
    }
    QXmlStreamReader xml(out);
    while (!xml.atEnd()) {
        xml.readNext();
        if (!xml.isStartElement() || xml.name() != QLatin1String("entry")) continue;
        TreeEntry entry;
        entry.kind = attr(xml.attributes(), "kind");
        while (xml.readNext() && !(xml.isEndElement() &&
                                   xml.name() == QLatin1String("entry"))) {
            if (!xml.isStartElement()) continue;
            const QStringView n = xml.name();
            if (n == QLatin1String("name")) {
                entry.path = xml.readElementText();
                // svn ls names are relative to the target URL, so when a
                // subdir narrows the listing, re-prefix to stay repo-relative.
                if (!subdir.isEmpty())
                    entry.path = subdir + QLatin1Char('/') + entry.path;
            } else if (n == QLatin1String("size")) {
                entry.size = xml.readElementText().toLongLong();
            } else if (n == QLatin1String("commit")) {
                entry.rev = attr(xml.attributes(), "revision").toLongLong();
                // <author> inside <commit>.
                while (xml.readNext() && !(xml.isEndElement() &&
                                           xml.name() == QLatin1String("commit"))) {
                    if (xml.isStartElement() &&
                        xml.name() == QLatin1String("author"))
                        entry.author = xml.readElementText();
                }
            }
        }
        result.push_back(entry);
    }
    if (error) error->clear();
    return result;
}

bool extract(const QString &path, qint64 revision, const QString &filePath,
             QByteArray *bytes, QString *error) {
    const RepoInfo info = identify(path);
    if (!info.error.isEmpty()) {
        if (error) *error = info.error;
        return false;
    }
    // Resolve against the repository root so a file deleted in the working
    // copy is still readable at any historical revision.
    const QString url = info.repoRoot + QLatin1Char('/') + filePath;
    QByteArray out, err;
    if (runSvn(cwdFor(path), {QStringLiteral("cat"), QStringLiteral("-r"),
                              QString::number(revision), url},
               &out, &err) != 0) {
        if (error) *error = errorText(err, out);
        return false;
    }
    *bytes = out;
    if (error) error->clear();
    return true;
}

LockInfo lockInfo(const QString &path, QString *error) {
    LockInfo lock;
    QByteArray out, err;
    if (runSvn(cwdFor(path), {QStringLiteral("info"), QStringLiteral("--xml"), path},
               &out, &err) != 0) {
        if (error) *error = errorText(err, out);
        return lock;
    }
    QXmlStreamReader xml(out);
    while (!xml.atEnd()) {
        xml.readNext();
        if (!xml.isStartElement() || xml.name() != QLatin1String("lock")) continue;
        lock.locked = true;
        while (xml.readNext() && !(xml.isEndElement() &&
                                   xml.name() == QLatin1String("lock"))) {
            if (!xml.isStartElement()) continue;
            const QStringView n = xml.name();
            if (n == QLatin1String("token"))
                lock.token = xml.readElementText();
            else if (n == QLatin1String("owner"))
                lock.owner = xml.readElementText();
            else if (n == QLatin1String("comment"))
                lock.comment = xml.readElementText();
            else if (n == QLatin1String("created"))
                lock.createdMs = parseIsoMs(xml.readElementText());
        }
    }
    if (error) error->clear();
    return lock;
}

} // namespace svn_source
