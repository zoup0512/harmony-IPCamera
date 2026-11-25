#pragma once
#include "archive_writer_lock.h"
#include "safe_archive_dir.h"
#include <regex>

namespace ipcam {
// Capability for ONE exact archive entry, not a path-based delete API. Only the recorder
// and this cleanup path mutate recorder basenames in these app-private directories.
class CloudArchivePin {
  struct Fd {
    int n = -1;
    ~Fd() { if (n >= 0) close(n); }
  } root_, dir_, file_;
  std::string name_;
  const char* folder_ = nullptr;
  struct stat original_{};
  bool used_ = false;
  static bool same(const struct stat& a, const struct stat& b) {
    return a.st_dev == b.st_dev && a.st_ino == b.st_ino;
  }
  static bool privateDir(const struct stat& s) {
    return S_ISDIR(s.st_mode) && s.st_uid == getuid() && !(s.st_mode & 0022);
  }
  // Do not rely on lookup alone: a case-folded directory may resolve a different spelling.
  static bool hasExactName(int dir, const std::string& name) {
    int fd = openat(dir,".",O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return false;
    std::unique_ptr<DIR,decltype(&closedir)> entries(fdopendir(fd),closedir);
    if (!entries) { close(fd); return false; }
    while (const dirent* ent = readdir(entries.get())) if (name == ent->d_name) return true;
    return false;
  }
  bool unchanged(const struct stat& s) const {
    const auto& a = original_;
    return S_ISREG(s.st_mode) && s.st_uid == getuid() && !(s.st_mode & 0022) && s.st_nlink == 1 &&
      same(a,s) && s.st_size > 0 && s.st_size == a.st_size &&
      s.st_mtim.tv_sec == a.st_mtim.tv_sec && s.st_mtim.tv_nsec == a.st_mtim.tv_nsec &&
      s.st_ctim.tv_sec == a.st_ctim.tv_sec && s.st_ctim.tv_nsec == a.st_ctim.tv_nsec;
  }
  bool valid(int currentRoot) const {
    struct stat r{}, current{}, d{}, namedDir{}, s{}, named{};
    return !used_ && file_.n >= 0 && fstat(root_.n,&r) == 0 && privateDir(r) &&
      fstat(currentRoot,&current) == 0 && same(r,current) && fstat(dir_.n,&d) == 0 && privateDir(d) &&
      fstatat(root_.n,folder_,&namedDir,AT_SYMLINK_NOFOLLOW) == 0 && same(d,namedDir) &&
      (folder_[0] == '.' || hasExactName(root_.n,folder_)) && hasExactName(dir_.n,name_) &&
      fstat(file_.n,&s) == 0 && unchanged(s) &&
      fstatat(dir_.n,name_.c_str(),&named,AT_SYMLINK_NOFOLLOW) == 0 && unchanged(named);
  }
  CloudArchivePin() = default;
public:
  CloudArchivePin(const CloudArchivePin&) = delete;
  // Takes ownership of an already trusted root. uploadFd is borrowed and MUST be the
  // reader used for both the upload and the SHA256 pass (not an ArkTS inode number).
  static std::unique_ptr<CloudArchivePin> Pin(int root, const std::string& name, int uploadFd) {
    std::unique_ptr<CloudArchivePin> p(new CloudArchivePin);
    p->root_.n = root;
    static const std::regex exact(R"((ipc_[0-9]{6}|(ipc_|IPS_)[0-9]{4}-[0-9]{2}-[0-9]{2}\.[0-9]{2}\.[0-9]{2}\.[0-9]{2}\.[0-9]{4})(_[1-9][0-9]*)?\.mp4)");
    if (root < 0 || name.size() > 255 || !std::regex_match(name,exact)) return nullptr;
    std::lock_guard<std::mutex> lk(ArchiveMutationMutex());
    p->folder_ = name.compare(0,4,"IPS_") == 0 ? "MD" : ".";
    p->name_ = name;
    p->dir_.n = openat(root,p->folder_,O_DIRECTORY | O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    struct stat named{};
    if (p->dir_.n < 0 || fstatat(p->dir_.n,name.c_str(),&named,AT_SYMLINK_NOFOLLOW) || !S_ISREG(named.st_mode)) return nullptr;
    p->file_.n = openat(p->dir_.n,name.c_str(),O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
    if (p->file_.n < 0 || flock(p->file_.n,LOCK_EX | LOCK_NB) || fstat(uploadFd,&p->original_) || !p->valid(root)) return nullptr;
    return p;
  }
  // Takes ownership of a freshly resolved trusted root, including on failure. No unlink
  // by an unpinned path, retry, rename, copy, overwrite or other recovery operation.
  bool Check(int currentRoot, bool remove) {
    Fd current{currentRoot};
    std::lock_guard<std::mutex> lk(ArchiveMutationMutex());
    const bool ok = valid(current.n);
    if (!ok || remove) used_ = true;
    return ok && (!remove || unlinkat(dir_.n,name_.c_str(),0) == 0);
  }
};
} // namespace ipcam
