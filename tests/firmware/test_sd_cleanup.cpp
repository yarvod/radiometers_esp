#include <dirent.h>
#include <sys/stat.h>
#include <sys/statvfs.h>

#include <algorithm>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#include "sd_maintenance.h"

namespace {

struct FakeFile {
  std::string name;
  time_t mtime;
  size_t size;
};

struct FakeDir {
  std::vector<std::string> names;
  size_t index = 0;
  dirent entry{};
};

std::string mount_point;
std::string uploaded_dir;
std::vector<FakeFile> files;
size_t total_bytes = 0;

int FakeStatvfs(const char*, struct statvfs* out) {
  if (!out || total_bytes == 0) return -1;
  size_t used = 0;
  for (const auto& f : files) {
    used += f.size;
  }
  const size_t block_size = 512;
  out->f_frsize = block_size;
  out->f_blocks = static_cast<fsblkcnt_t>(total_bytes / block_size);
  const size_t avail = total_bytes > used ? (total_bytes - used) : 0;
  out->f_bavail = static_cast<fsblkcnt_t>(avail / block_size);
  return 0;
}

DIR* FakeOpendir(const char* path) {
  if (!path || uploaded_dir != path) return nullptr;
  auto* dir = new FakeDir();
  for (const auto& f : files) {
    dir->names.push_back(f.name);
  }
  return reinterpret_cast<DIR*>(dir);
}

struct dirent* FakeReaddir(DIR* dir) {
  if (!dir) return nullptr;
  auto* fake = reinterpret_cast<FakeDir*>(dir);
  if (fake->index >= fake->names.size()) return nullptr;
  const std::string& name = fake->names[fake->index++];
  std::memset(&fake->entry, 0, sizeof(fake->entry));
  std::snprintf(fake->entry.d_name, sizeof(fake->entry.d_name), "%s", name.c_str());
  fake->entry.d_type = DT_REG;
  return &fake->entry;
}

int FakeClosedir(DIR* dir) {
  delete reinterpret_cast<FakeDir*>(dir);
  return 0;
}

int FakeStat(const char* path, struct stat* out) {
  if (!path || !out) return -1;
  std::string name = path;
  if (name.rfind(uploaded_dir + "/", 0) != 0) return -1;
  name = name.substr(uploaded_dir.size() + 1);
  for (const auto& f : files) {
    if (f.name == name) {
      std::memset(out, 0, sizeof(*out));
      out->st_mode = S_IFREG;
      out->st_mtime = f.mtime;
      return 0;
    }
  }
  return -1;
}

int FakeUnlink(const char* path) {
  if (!path) return -1;
  std::string name = path;
  if (name.rfind(uploaded_dir + "/", 0) != 0) return -1;
  name = name.substr(uploaded_dir.size() + 1);
  auto it = std::find_if(files.begin(), files.end(), [&](const FakeFile& f) { return f.name == name; });
  if (it == files.end()) return -1;
  files.erase(it);
  return 0;
}

void ResetFs() {
  mount_point = "/sdcard";
  uploaded_dir = "/sdcard/uploaded";
  files.clear();
  total_bytes = 10 * 512;
}

FsOps FakeOps() {
  FsOps ops{};
  ops.statvfs_fn = &FakeStatvfs;
  ops.opendir_fn = &FakeOpendir;
  ops.readdir_fn = &FakeReaddir;
  ops.closedir_fn = &FakeClosedir;
  ops.stat_fn = &FakeStat;
  ops.unlink_fn = &FakeUnlink;
  return ops;
}

int failures = 0;

void Check(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << "\n";
    failures++;
  }
}

void TestNoCleanupBelowThreshold() {
  ResetFs();
  files.push_back({"a.txt", 10, 512});
  const int deleted = PurgeUploadedFiles(mount_point.c_str(), uploaded_dir.c_str(), 60, FakeOps());
  Check(deleted == 0, "no deletion below threshold");
  Check(files.size() == 1, "files untouched");
}

void TestCleanupDeletesOldest() {
  ResetFs();
  files.push_back({"old.txt", 10, 5 * 512});
  files.push_back({"new.txt", 20, 4 * 512});
  const int deleted = PurgeUploadedFiles(mount_point.c_str(), uploaded_dir.c_str(), 60, FakeOps());
  Check(deleted >= 1, "deleted at least one file");
  Check(files.size() == 1, "one file remains");
  Check(files.front().name == "new.txt", "oldest removed first");
}

void TestCleanupStopsWhenEnough() {
  ResetFs();
  total_bytes = 20 * 512;
  files.push_back({"old.txt", 10, 6 * 512});
  files.push_back({"mid.txt", 20, 6 * 512});
  files.push_back({"new.txt", 30, 6 * 512});
  const int deleted = PurgeUploadedFiles(mount_point.c_str(), uploaded_dir.c_str(), 60, FakeOps());
  Check(deleted == 1, "only one file deleted to reach threshold");
  Check(files.size() == 2, "two files remain");
}

void TestStatvfsFailure() {
  ResetFs();
  total_bytes = 0;
  const int deleted = PurgeUploadedFiles(mount_point.c_str(), uploaded_dir.c_str(), 60, FakeOps());
  Check(deleted == -1, "statvfs failure returns -1");
}

}  // namespace

int main() {
  TestNoCleanupBelowThreshold();
  TestCleanupDeletesOldest();
  TestCleanupStopsWhenEnough();
  TestStatvfsFailure();

  if (failures == 0) {
    std::cout << "OK: all sd cleanup tests passed\n";
    return 0;
  }
  std::cerr << failures << " test(s) failed\n";
  return 1;
}
