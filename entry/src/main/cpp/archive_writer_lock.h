#pragma once
#include <fcntl.h>
#include <mutex>
#include <sys/file.h>
#include <unistd.h>
#include <cerrno>

namespace ipcam {
// Same mutex for both recorder opens, pinning and the final name-check/unlinkat.
// The fd lock stays with the writer through muxer finalization and close.
inline std::mutex& ArchiveMutationMutex() { static std::mutex mu; return mu; }
inline int OpenRecordingExclusive(const char* path) {
  std::lock_guard<std::mutex> lock(ArchiveMutationMutex());
  int fd = open(path, O_CREAT | O_RDWR | O_EXCL, 0600);
  if (fd >= 0 && flock(fd, LOCK_EX | LOCK_NB) != 0) {
    int error = errno;
    close(fd); // Do not remove, replace or reopen the name on failure.
    errno = error;
    return -1;
  }
  return fd;
}
} // namespace ipcam
