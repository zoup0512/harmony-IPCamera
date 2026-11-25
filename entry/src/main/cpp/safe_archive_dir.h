#pragma once

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <memory>
#include <string>

namespace ipcam {

// Both HTTP callbacks share this pinned directory for one server registration.
class SafeArchiveDir {
  static constexpr int kDirectory = O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC;

  struct Fd {
    explicit Fd(int n) : value(n) {}
    ~Fd() { if (value >= 0) ::close(value); }
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    int release() { int n = value; value = -1; return n; }
    int value;
  };
  const Fd root_;

  static bool ValidBasename(const std::string& file, bool motion) {
    // Keep older ipc_*.mp4 names as well as timestamps and rotated segments.
    if (file.size() <= 8 || file.compare(0, 4, motion ? "IPS_" : "ipc_") != 0 ||
        file.compare(file.size() - 4, 4, ".mp4") != 0 || file.find("..") != file.npos) return false;
    for (unsigned char c : file) if (c < 32 || c == 127 || c == '/' || c == '\\') return false;
    return true;
  }

  int OpenDirectory(bool motion) const {
    // Even for the root, open a new file description: dup would share readdir's offset.
    return ::openat(root_.value, motion ? "MD" : ".", kDirectory);
  }

public:
  // Returns an owned fd only for an exact, component-wise no-follow sandbox root.
  static int OpenAppRoot(const std::string& filesDir) {
    // Select only the entry module's sandbox paths, not an arbitrary HTTP option.
    if (filesDir != "/data/storage/el2/base/files" &&
        filesDir != "/data/storage/el2/base/haps/entry/files") return -1;
    // Sandbox ancestors need only search permission, not permission to enumerate them.
    constexpr int anchor = O_PATH | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC;
    Fd parent(::open("/", anchor));
    for (size_t start = 1; start < filesDir.size();) {
      const size_t end = filesDir.find('/', start);
      const std::string component = filesDir.substr(start, end - start);
      if (parent.value < 0) return -1;
      Fd child(::openat(parent.value, component.c_str(), anchor));
      ::close(parent.release());
      parent.value = child.release();
      if (end == filesDir.npos) break;
      start = end + 1;
    }
    return parent.release();
  }

public:
  explicit SafeArchiveDir(const std::string& filesDir) : root_(OpenAppRoot(filesDir)) {}
  // Alternative for callers already holding a trusted directory fd; takes ownership.
  explicit SafeArchiveDir(int root) : root_(root) {}

  std::string List() const {
    std::string out;
    for (bool motion : {false, true}) {
      Fd dir(OpenDirectory(motion));
      if (dir.value < 0) continue;
      std::unique_ptr<DIR, int(*)(DIR*)> stream(::fdopendir(dir.value), ::closedir);
      if (!stream) continue;
      dir.release();  // now owned by closedir
      while (const dirent* ent = ::readdir(stream.get())) {
        const std::string file(ent->d_name);
        struct stat st{};
        if (ValidBasename(file, motion) &&
            ::fstatat(::dirfd(stream.get()), file.c_str(), &st, AT_SYMLINK_NOFOLLOW) == 0 && S_ISREG(st.st_mode)) {
          if (motion) out += "MD/";
          out += file + " " + std::to_string(st.st_size) + "\n";
        }
      }
    }
    return out;
  }

  // The legacy router folds case; the browser router folds just the MD/IPS_ prefix.
  // Return an owned regular-file fd; the HTTP layer never resolves this name again.
  int OpenFile(const std::string& key) const {
    const bool motion = key.compare(0, 7, "md/ips_") == 0;
    const std::string file = motion ? "IPS_" + key.substr(7) : key;
    if (!ValidBasename(file, motion)) return -1;
    Fd dir(OpenDirectory(motion));
    struct stat st{};
    if (dir.value < 0 || ::fstatat(dir.value, file.c_str(), &st, AT_SYMLINK_NOFOLLOW) != 0 || !S_ISREG(st.st_mode)) return -1;
    Fd fd(::openat(dir.value, file.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK));
    return fd.value >= 0 && ::fstat(fd.value, &st) == 0 && S_ISREG(st.st_mode) ? fd.release() : -1;
  }
};

}  // namespace ipcam
