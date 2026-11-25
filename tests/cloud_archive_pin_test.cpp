// POSIX standalone: C++17, no accounts, sockets or devices. Every name is relative to mkdtemp.
#include "../entry/src/main/cpp/cloud_archive_pin.h"
#include <cassert>
#include <cstdlib>
#include <cstdio>
#include <filesystem>
using ipcam::CloudArchivePin;
int main() {
  char temp[] = "/tmp/cloud-pin-XXXXXX";
  assert(mkdtemp(temp));
  assert(chdir(temp) == 0);
  int root = open(".",O_RDONLY|O_DIRECTORY);
  const char* name = "ipc_010203.mp4";
  auto create = [](const char* n) { int fd = ipcam::OpenRecordingExclusive(n); assert(fd >= 0 && write(fd,"abc",3)==3); return fd; };
  int writer = create(name), reader = open(name,O_RDONLY);
  auto pin = [&]() { return CloudArchivePin::Pin(dup(root),name,reader); };
  assert(!pin()); // even with nonempty payload, the active writer's fd is exclusive
  close(writer);
  assert(ipcam::SafeArchiveDir::OpenAppRoot(temp)<0);
  for (auto n : {"../ipc_010203.mp4","IPC_010203.mp4","ipc_010203.mp4\n","ipc_bad.mp4"})
    assert(!CloudArchivePin::Pin(dup(root),n,reader));
  auto p = pin(); assert(p && p->Check(dup(root),false));
  assert(!pin() && ipcam::OpenRecordingExclusive(name)<0);
  assert(rename(name,"old")==0); close(create(name));
  assert(!p->Check(dup(root),true) && access(name,F_OK)==0 && access("old",F_OK)==0);
  p.reset(); assert(!pin()); // new name, old upload fd
  close(reader); reader=open(name,O_RDONLY); p=pin(); assert(p);
  struct stat before{}; assert(fstat(reader,&before)==0);
  int changed=open(name,O_WRONLY); assert(changed>=0 && pwrite(changed,"xyz",3,0)==3); close(changed);
  struct timespec stamp[]{before.st_atim,before.st_mtim}; usleep(2000);
  assert(utimensat(root,name,stamp,0)==0 && !p->Check(dup(root),true)); // restored mtime + same size, changed ctime
  p.reset(); assert(link(name,"alias")==0 && !pin()); assert(unlink("alias")==0);
  p=pin(); assert(p); assert(unlink(name)==0 && symlink("old",name)==0);
  assert(!p->Check(dup(root),true) && !pin() && access("old",F_OK)==0);
  p.reset(); close(reader);
  assert(mkdir("MD",0700)==0);
  const char* motion="IPS_2026-11-24.01.02.03.0004_2.mp4";
  std::string path=std::string("MD/")+motion;
  close(create(path.c_str())); reader=open(path.c_str(),O_RDONLY);
  auto motionPin=[&](){return CloudArchivePin::Pin(dup(root),motion,reader);};
  p=motionPin(); assert(p);
  assert(rename("MD","saved")==0 && symlink("saved","MD")==0);
  assert(!p->Check(dup(root),true) && !motionPin() && access(path.c_str(),F_OK)==0);
  p.reset(); assert(unlink("MD")==0 && rename("saved","MD")==0);
  p=motionPin(); assert(p && !p->Check(open("MD",O_DIRECTORY),true)); // root changed
  p.reset(); p=motionPin(); assert(p && p->Check(dup(root),true) && !p->Check(dup(root),true));
  assert(access(path.c_str(),F_OK)<0 && access("old",F_OK)==0);
  p.reset(); close(reader); close(root); assert(chdir("/")==0); std::filesystem::remove_all(temp);
  puts("cloud archive pin: all synthetic cases passed");
}
