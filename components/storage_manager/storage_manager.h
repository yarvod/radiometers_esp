#pragma once

#include <string>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "sdmmc_cmd.h"

// Initialize StorageManager: creates the SD mutex.
// Call once at startup before any storage operations.
void StorageManagerInit();

// RAII mutex guard for SD card / storage operations.
class SdLockGuard {
 public:
  explicit SdLockGuard(TickType_t timeout_ticks = pdMS_TO_TICKS(2000));
  ~SdLockGuard();
  bool locked() const { return locked_; }
 private:
  bool locked_ = false;
};

// Mount/unmount SD card (SDMMC FAT at CONFIG_MOUNT_POINT).
bool MountLogSd();
void UnmountLogSd();

// Mount/unmount internal flash FAT partition (wear-levelling).
bool MountInternalFlashFs();
void UnmountInternalFlashFs();

// Mount whichever backend is active in app_config.storage_backend.
bool MountActiveStorage();

// Return the VFS mount point for the active backend.
const char* ActiveStorageMountPoint();
std::string ActiveToUploadDir();
std::string ActiveUploadedDir();

// Active meteo root file registry. Callers must hold SdLockGuard while reading or
// replacing the path so upload sweeps cannot race with the meteo writer.
std::string ActiveMeteoLogPathLocked();
void SetActiveMeteoLogPathLocked(const std::string& path);

// Resolve a filename or relative path against the active mount point.
bool BuildActiveStorageFilenamePath(const std::string& name, std::string* out_full);
bool BuildActiveStorageRelativePath(const std::string& rel_path, std::string* out_full);

// Directory creation helpers.
bool EnsureDirExists(const char* path);
bool EnsureUploadDirs();

// State accessors (avoids exposing internal statics).
bool IsLogSdMounted();
bool IsInternalFlashMounted();
