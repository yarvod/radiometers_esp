#pragma once

#include <ctime>
#include <dirent.h>
#include <string>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <vector>

struct UploadedFileInfo {
  std::string path;
  time_t mtime;
};

struct FsOps {
  int (*statvfs_fn)(const char* path, struct statvfs* out);
  DIR* (*opendir_fn)(const char* path);
  struct dirent* (*readdir_fn)(DIR* dir);
  int (*closedir_fn)(DIR* dir);
  int (*stat_fn)(const char* path, struct stat* out);
  int (*unlink_fn)(const char* path);
};

FsOps DefaultFsOps();

int PurgeUploadedFiles(const char* mount_point, const char* uploaded_dir, int max_percent);
int PurgeUploadedFiles(const char* mount_point, const char* uploaded_dir, int max_percent, const FsOps& ops);
