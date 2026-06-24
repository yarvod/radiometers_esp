#include "sd_maintenance.h"

#include <algorithm>
#include <cerrno>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef ESP_PLATFORM
#include "esp_vfs_fat.h"
#endif

#include "esp_log.h"

namespace {

constexpr char TAG_SD[] = "SD_MAINT";

bool GetFsUsagePercent(const char* path, const FsOps& ops, int* out_percent) {
  if (!path || !out_percent) return false;
  struct statvfs stats {};
  if (ops.statvfs_fn(path, &stats) != 0 || stats.f_blocks == 0) {
    return false;
  }
  const uint64_t total = static_cast<uint64_t>(stats.f_blocks) * stats.f_frsize;
  const uint64_t avail = static_cast<uint64_t>(stats.f_bavail) * stats.f_frsize;
  const uint64_t used = total > avail ? (total - avail) : 0;
  *out_percent = static_cast<int>((used * 100ULL) / total);
  return true;
}

void CollectUploadedFiles(const char* uploaded_dir, const FsOps& ops, std::vector<UploadedFileInfo>* out) {
  if (!uploaded_dir || !out) return;
  DIR* dir = ops.opendir_fn(uploaded_dir);
  if (!dir) return;
  struct dirent* ent = nullptr;
  while ((ent = ops.readdir_fn(dir)) != nullptr) {
    if (ent->d_name[0] == '.') continue;
    std::string full = std::string(uploaded_dir) + "/" + ent->d_name;
    struct stat st {};
    if (ops.stat_fn(full.c_str(), &st) != 0) continue;
    if (!S_ISREG(st.st_mode)) continue;
    out->push_back({full, st.st_mtime});
  }
  ops.closedir_fn(dir);
}

}  // namespace

FsOps DefaultFsOps() {
  FsOps ops{};
#ifdef ESP_PLATFORM
  ops.statvfs_fn = [](const char* path, struct statvfs* out) -> int {
    if (!path || !out) return -1;
    uint64_t total = 0;
    uint64_t free = 0;
    if (esp_vfs_fat_info(path, &total, &free) != ESP_OK || total == 0) {
      return -1;
    }
    out->f_frsize = 1;
    out->f_blocks = total;
    out->f_bavail = free;
    return 0;
  };
#else
  ops.statvfs_fn = &statvfs;
#endif
  ops.opendir_fn = &opendir;
  ops.readdir_fn = &readdir;
  ops.closedir_fn = &closedir;
  ops.stat_fn = &stat;
  ops.unlink_fn = &unlink;
  return ops;
}

int PurgeUploadedFiles(const char* mount_point, const char* uploaded_dir, int max_percent) {
  return PurgeUploadedFiles(mount_point, uploaded_dir, max_percent, DefaultFsOps());
}

int PurgeUploadedFiles(const char* mount_point, const char* uploaded_dir, int max_percent, const FsOps& ops) {
  int usage = 0;
  if (!GetFsUsagePercent(mount_point, ops, &usage)) {
    return -1;
  }
  if (usage <= max_percent) {
    return 0;
  }
  std::vector<UploadedFileInfo> files;
  CollectUploadedFiles(uploaded_dir, ops, &files);
  if (files.empty()) {
    return 0;
  }
  std::sort(files.begin(), files.end(), [](const UploadedFileInfo& a, const UploadedFileInfo& b) {
    return a.mtime < b.mtime;
  });
  int deleted = 0;
  for (const auto& file : files) {
    if (usage <= max_percent) break;
    if (ops.unlink_fn(file.path.c_str()) == 0) {
      deleted++;
      if (!GetFsUsagePercent(mount_point, ops, &usage)) {
        break;
      }
    } else {
      ESP_LOGW(TAG_SD, "Failed to delete uploaded file %s (errno %d)", file.path.c_str(), errno);
    }
  }
  return deleted;
}
