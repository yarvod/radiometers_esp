#include "storage_manager.h"

#include <cctype>
#include <cerrno>
#include <string>
#include <sys/stat.h>

#include "app_state.h"
#include "error_manager.h"
#include "hw_pins.h"

#include "driver/sdmmc_host.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "wear_levelling.h"

static constexpr char kTag[] = "STOR";

// ---------- module-private state ----------

static SemaphoreHandle_t s_sd_mutex = nullptr;
static sdmmc_card_t*     s_log_sd_card = nullptr;
static bool              s_log_sd_mounted = false;
static bool              s_flash_mounted = false;
static wl_handle_t       s_flash_wl = WL_INVALID_HANDLE;

// ---------- init ----------

void StorageManagerInit() {
  s_sd_mutex = xSemaphoreCreateMutex();
}

// ---------- SdLockGuard ----------

SdLockGuard::SdLockGuard(TickType_t timeout_ticks) {
  locked_ = (s_sd_mutex && xSemaphoreTake(s_sd_mutex, timeout_ticks) == pdTRUE);
}

SdLockGuard::~SdLockGuard() {
  if (locked_ && s_sd_mutex) {
    xSemaphoreGive(s_sd_mutex);
  }
}

// ---------- SD card ----------

bool MountLogSd() {
  if (s_log_sd_mounted) return true;

  sdmmc_host_t host = SDMMC_HOST_DEFAULT();
  host.flags |= SDMMC_HOST_FLAG_1BIT;

  sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
  slot_config.width = 1;
  slot_config.clk = SD_CLK;
  slot_config.cmd = SD_CMD;
  slot_config.d0 = SD_D0;
  slot_config.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

  esp_vfs_fat_sdmmc_mount_config_t mount_config = {};
  mount_config.format_if_mount_failed = false;
  mount_config.max_files = 4;
  mount_config.allocation_unit_size = 0;

  esp_err_t ret = esp_vfs_fat_sdmmc_mount(CONFIG_MOUNT_POINT, &host, &slot_config,
                                           &mount_config, &s_log_sd_card);
  if (ret != ESP_OK) {
    ESP_LOGE(kTag, "SD mount failed: %s", esp_err_to_name(ret));
    ErrorManagerSet(ErrorCode::kSdMount, ErrorSeverity::kError,
                    std::string("SD mount failed: ") + esp_err_to_name(ret));
    return false;
  }
  s_log_sd_mounted = true;
  ErrorManagerClear(ErrorCode::kSdMount);
  return true;
}

void UnmountLogSd() {
  if (!s_log_sd_mounted) return;
  esp_vfs_fat_sdcard_unmount(CONFIG_MOUNT_POINT, s_log_sd_card);
  s_log_sd_mounted = false;
  s_log_sd_card = nullptr;
}

// ---------- internal flash ----------

bool MountInternalFlashFs() {
  if (s_flash_mounted) return true;

  const esp_partition_t* part = esp_partition_find_first(
      ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_FAT,
      INTERNAL_FLASH_PARTITION_LABEL);
  if (!part) {
    ESP_LOGE(kTag, "Flash partition '%s' not found", INTERNAL_FLASH_PARTITION_LABEL);
    return false;
  }

  esp_vfs_fat_mount_config_t mount_config = {};
  mount_config.max_files = 3;
  mount_config.format_if_mount_failed = true;
  mount_config.allocation_unit_size = 32768;

  esp_err_t ret = esp_vfs_fat_spiflash_mount_rw_wl(INTERNAL_FLASH_MOUNT_POINT,
                                                    INTERNAL_FLASH_PARTITION_LABEL,
                                                    &mount_config, &s_flash_wl);
  if (ret != ESP_OK) {
    s_flash_wl = WL_INVALID_HANDLE;
    ESP_LOGE(kTag, "Flash FS mount failed: %s (heap=%u largest=%u)",
             esp_err_to_name(ret),
             static_cast<unsigned>(esp_get_free_heap_size()),
             static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)));
    return false;
  }
  s_flash_mounted = true;
  ESP_LOGI(kTag, "Flash FS mounted at %s", INTERNAL_FLASH_MOUNT_POINT);
  return true;
}

void UnmountInternalFlashFs() {
  if (!s_flash_mounted || s_flash_wl == WL_INVALID_HANDLE) return;
  esp_err_t ret = esp_vfs_fat_spiflash_unmount_rw_wl(INTERNAL_FLASH_MOUNT_POINT, s_flash_wl);
  if (ret != ESP_OK) {
    ESP_LOGW(kTag, "Flash FS unmount failed: %s", esp_err_to_name(ret));
    return;
  }
  s_flash_wl = WL_INVALID_HANDLE;
  s_flash_mounted = false;
  ESP_LOGI(kTag, "Flash FS unmounted");
}

// ---------- active storage ----------

bool MountActiveStorage() {
  if (app_config.storage_backend == StorageBackend::kInternalFlash) {
    return MountInternalFlashFs();
  }
  return MountLogSd();
}

const char* ActiveStorageMountPoint() {
  return app_config.storage_backend == StorageBackend::kInternalFlash
             ? INTERNAL_FLASH_MOUNT_POINT
             : CONFIG_MOUNT_POINT;
}

std::string ActiveToUploadDir() {
  return std::string(ActiveStorageMountPoint()) + "/to_upload";
}

std::string ActiveUploadedDir() {
  return std::string(ActiveStorageMountPoint()) + "/uploaded";
}

// ---------- path helpers ----------

static bool ValidateStorageFilename(const std::string& name) {
  if (name.empty() || name.size() > 255) return false;
  for (char c : name) {
    if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-' || c == '.')) {
      return false;
    }
  }
  return true;
}

bool BuildActiveStorageFilenamePath(const std::string& name, std::string* out_full) {
  if (!ValidateStorageFilename(name)) return false;
  if (out_full) *out_full = std::string(ActiveStorageMountPoint()) + "/" + name;
  return true;
}

bool BuildActiveStorageRelativePath(const std::string& rel_path_raw, std::string* out_full) {
  if (rel_path_raw.empty() || rel_path_raw.size() > 256) return false;
  std::string rel = rel_path_raw;
  if (!rel.empty() && rel[0] == '/') rel.erase(rel.begin());
  if (rel.empty() || rel.size() > 256) return false;
  if (rel.find("..") != std::string::npos) return false;
  if (rel.find("//") != std::string::npos) return false;
  for (char c : rel) {
    if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-' ||
          c == '.' || c == '/' || c == ' ' || c == '(' || c == ')')) {
      return false;
    }
  }
  if (out_full) *out_full = std::string(ActiveStorageMountPoint()) + "/" + rel;
  return true;
}

// ---------- directory helpers ----------

bool EnsureDirExists(const char* path) {
  if (!path) return false;
  struct stat st {};
  if (stat(path, &st) == 0) return S_ISDIR(st.st_mode);
  if (mkdir(path, 0775) == 0) return true;
  ESP_LOGE(kTag, "mkdir %s failed: %d", path, errno);
  return false;
}

bool EnsureUploadDirs() {
  return EnsureDirExists(ActiveToUploadDir().c_str()) &&
         EnsureDirExists(ActiveUploadedDir().c_str());
}

// ---------- status accessors ----------

bool IsLogSdMounted() { return s_log_sd_mounted; }
bool IsInternalFlashMounted() { return s_flash_mounted; }
