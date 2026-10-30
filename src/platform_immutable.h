#pragma once

#include <QString>

// Platform abstraction for making history objects resist tampering.
//
// The guarantee is best-effort and intentionally layered:
//   Linux:  chattr +i (ioctl FS_IMMUTABLE_FL) when the OS permits it
//           (requires CAP_LINUX_IMMUTABLE / root); otherwise falls back to
//           clearing the write permission bits (owner may chmod back).
//   Windows: FILE_ATTRIBUTE_READONLY (real-machine verification TODO).
//   macOS:   UF_IMMUTABLE (real-machine verification TODO).
//
// This is not an antivirus engine: it reduces the surface a same-user
// ransomware process can silently modify, it does not stop a privileged
// attacker. The verification environment is Linux.
namespace platform {

// Best-effort: make `path` not writable through normal file APIs.
// Returns true if the file is now read-only/immutable by the strongest
// available mechanism on this platform.
bool makeImmutable(const QString &path);

// Reverse of makeImmutable (maintenance or tests). Best-effort.
bool makeMutable(const QString &path);

// True if the file currently reports as read-only/immutable.
bool isImmutable(const QString &path);

} // namespace platform
