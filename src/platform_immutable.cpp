#include "platform_immutable.h"

#include <QFile>
#include <QFileInfo>
#include <sys/stat.h>

#if defined(Q_OS_LINUX)
#include <fcntl.h>
#include <linux/fs.h>
#include <sys/ioctl.h>
#include <unistd.h>
#elif defined(Q_OS_WIN)
#include <windows.h>
#elif defined(Q_OS_MACOS)
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace platform {

namespace {

#if defined(Q_OS_LINUX)
// Try to set/clear the immutable flag via chattr's ioctl. Returns:
//   applied=true  -> the +i flag is now in the requested state.
//   applied=false -> the filesystem/OS refused (EPERM/EACCES or unsupported).
bool tryImmutableFlag(const QString &path, bool set, bool &applied) {
    applied = false;
    const QByteArray encoded = QFile::encodeName(path);
    const int fd = ::open(encoded.constData(), O_RDONLY | O_NONBLOCK);
    if (fd < 0) return false;
    int flags = 0;
    const bool got = ::ioctl(fd, FS_IOC_GETFLAGS, &flags) == 0;
    if (got) {
        const bool isSet = (flags & FS_IMMUTABLE_FL) != 0;
        if (isSet == set) {
            applied = true;
        } else {
            const int next = set ? (flags | FS_IMMUTABLE_FL) : (flags & ~FS_IMMUTABLE_FL);
            applied = ::ioctl(fd, FS_IOC_SETFLAGS, &next) == 0;
        }
    }
    ::close(fd);
    return got;
}

bool clearWriteBits(const QString &path) {
    const QByteArray encoded = QFile::encodeName(path);
    struct stat st;
    if (::stat(encoded.constData(), &st) != 0) return false;
    const mode_t mode = st.st_mode & 07777;
    return ::chmod(encoded.constData(), mode & ~(S_IWUSR | S_IWGRP | S_IWOTH)) == 0;
}

bool setWriteBits(const QString &path) {
    const QByteArray encoded = QFile::encodeName(path);
    struct stat st;
    if (::stat(encoded.constData(), &st) != 0) return false;
    const mode_t mode = st.st_mode & 07777;
    return ::chmod(encoded.constData(), mode | S_IWUSR) == 0;
}
#endif

} // namespace

bool makeImmutable(const QString &path) {
    if (!QFileInfo::exists(path)) return false;
#if defined(Q_OS_LINUX)
    // chattr +i is the strong guarantee (needs CAP_LINUX_IMMUTABLE, i.e. root).
    // When the OS refuses it, fall back to clearing the write permission bits,
    // which still stops a same-user process writing directly to the file.
    bool applied = false;
    if (tryImmutableFlag(path, true, applied) && applied) return true;
    return clearWriteBits(path);
#elif defined(Q_OS_WIN)
    // TODO(real-machine): verify on Windows. Set the read-only attribute.
    return ::SetFileAttributesW(reinterpret_cast<const wchar_t *>(path.utf16()),
                                FILE_ATTRIBUTE_READONLY) != 0;
#elif defined(Q_OS_MACOS)
    // TODO(real-machine): verify on macOS. UF_IMMUTABLE needs the file owner.
    const QByteArray encoded = QFile::encodeName(path);
    return ::chflags(encoded.constData(), UF_IMMUTABLE) == 0;
#else
    return false;
#endif
}

bool makeMutable(const QString &path) {
    if (!QFileInfo::exists(path)) return false;
#if defined(Q_OS_LINUX)
    // Clear the +i flag if it is set (best-effort; needs CAP_LINUX_IMMUTABLE),
    // then always restore the owner write bit. The flag alone is not the whole
    // guard: the read-only mode bits must be lifted too.
    bool applied = false;
    tryImmutableFlag(path, false, applied);
    return setWriteBits(path);
#elif defined(Q_OS_WIN)
    return ::SetFileAttributesW(reinterpret_cast<const wchar_t *>(path.utf16()),
                                FILE_ATTRIBUTE_NORMAL) != 0;
#elif defined(Q_OS_MACOS)
    const QByteArray encoded = QFile::encodeName(path);
    return ::chflags(encoded.constData(), 0) == 0;
#else
    return false;
#endif
}

bool isImmutable(const QString &path) {
    if (!QFileInfo::exists(path)) return false;
#if defined(Q_OS_LINUX)
    bool applied = false;
    if (tryImmutableFlag(path, true, applied) && applied) return true;
    struct stat st;
    const QByteArray encoded = QFile::encodeName(path);
    if (::stat(encoded.constData(), &st) != 0) return false;
    return (st.st_mode & (S_IWUSR | S_IWGRP | S_IWOTH)) == 0;
#elif defined(Q_OS_WIN)
    return (::GetFileAttributesW(reinterpret_cast<const wchar_t *>(path.utf16()))
            & FILE_ATTRIBUTE_READONLY) != 0;
#elif defined(Q_OS_MACOS)
    struct stat st;
    const QByteArray encoded = QFile::encodeName(path);
    if (::stat(encoded.constData(), &st) != 0) return false;
    return (st.st_flags & UF_IMMUTABLE) != 0;
#else
    return false;
#endif
}

} // namespace platform
