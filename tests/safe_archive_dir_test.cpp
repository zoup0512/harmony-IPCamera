// Standalone POSIX/C++17 filesystem regression. No HTTP, sockets, devices or media.
#include "../entry/src/main/cpp/safe_archive_dir.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <sstream>
#include <stdexcept>

namespace fs = std::filesystem;

static void Check(bool ok, const char* message) {
  if (!ok) throw std::runtime_error(message);
}

struct Fixture {
  char path[64] = "/tmp/ipcam-safe-archive-XXXXXX";
  Fixture() { Check(::mkdtemp(path) != nullptr, "temporary fixture"); }
  ~Fixture() { std::error_code ec; fs::remove_all(path, ec); }
};

static void Write(const fs::path& path, const char* text) {
  std::ofstream out(path);
  out << text;
  out.close();
  Check(out.good(), "fixture write");
}

static std::set<std::string> Rows(const ipcam::SafeArchiveDir& archives) {
  std::istringstream in(archives.List());
  std::set<std::string> rows;
  for (std::string s; std::getline(in, s);) Check(rows.insert(s).second, "duplicate listing row");
  return rows;
}

static void Read(const ipcam::SafeArchiveDir& archives, const std::string& key, const std::string& expected) {
  const int fd = archives.OpenFile(key);
  if (expected.empty()) {
    if (fd >= 0) ::close(fd);
    Check(fd < 0, "unexpected archive descriptor");
    return;
  }
  Check(fd >= 0, "missing archive descriptor");
  char buf[32];
  const ssize_t count = ::read(fd, buf, sizeof(buf));
  ::close(fd);
  Check(count >= 0 && std::string(buf, static_cast<size_t>(count)) == expected, "unexpected synthetic content");
}

static void Test() {
  Fixture temp;
  const fs::path work(temp.path), root = work / "files", md = root / "MD", sibling = work / "other";
  fs::create_directories(md);
  fs::create_directory(sibling);
  const std::string old = "ipc_232304_2.mp4", manual = "ipc_2026-11-24.12.00.00.0001.mp4";
  const std::string motion = "IPS_2026-11-24.12.00.00.0001_2.mp4", key = "md/ips_" + motion.substr(4);
  Write(root / old, "old"); Write(root / manual, "manual"); Write(md / motion, "motion");
  Write(sibling / motion, "other");
  Write(root / "ipc_note.mp4.bak", "backup"); Write(root / "ipc_bad\nrow.mp4", "bad");
  Write(root / motion, "wrong location");
  fs::create_directory(root / "ipc_dir.mp4");
  Check(::mkfifo((md / "IPS_pipe.mp4").c_str(), 0600) == 0, "fixture fifo");
  fs::create_symlink(sibling / motion, root / "ipc_link.mp4");
  fs::create_symlink(sibling / motion, md / "IPS_link.mp4");

  // A caller-supplied path is not an application root. Inject only our fixture fd.
  ipcam::SafeArchiveDir untrusted(root.string());
  Check(untrusted.List().empty(), "unrecognized application root");
  Read(untrusted, old, "");
  const int fd = ::open(root.c_str(), O_PATH | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  Check(fd >= 0, "open synthetic root");
  ipcam::SafeArchiveDir archives(fd);
  const std::set<std::string> manualRows{old + " 3", manual + " 6"};
  auto allRows = manualRows;
  allRows.insert("MD/" + motion + " 6");
  Check(Rows(archives) == allRows && Rows(archives) == allRows, "listing or shared offset");
  Read(archives, old, "old"); Read(archives, manual, "manual"); Read(archives, key, "motion");
  for (const char* bad : {"ipc_link.mp4", "md/ips_link.mp4", "ipc_dir.mp4", "md/ips_pipe.mp4",
                          "ipc_note.mp4.bak", "ipc_bad\nrow.mp4", "ipc_sub/file.mp4", "ipc_a..b.mp4"})
    Read(archives, bad, "");
  Read(archives, old + std::string(1, '\0') + ".mp4", "");
  Read(archives, motion, "");

  // Replace MD after listing; opening must re-check the actual directory entry.
  fs::rename(md, root / "saved-md");
  fs::create_directory_symlink(sibling, md);
  Check(Rows(archives) == manualRows, "linked MD was listed");
  Read(archives, key, "");
  fs::remove(md);
  fs::rename(root / "saved-md", md);
  // Replacing the root pathname must not retarget either callback.
  fs::rename(root, work / "original");
  fs::create_directory_symlink(sibling, root);
  Check(Rows(archives) == allRows, "root was not pinned");
  Read(archives, key, "motion");
}

int main() {
  try { Test(); }
  catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
  std::cout << "safe_archive_dir: passed\n";
}
