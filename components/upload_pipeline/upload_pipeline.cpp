#include "upload_pipeline.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <memory>
#include <string>
#include <sys/stat.h>
#include <vector>

#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/md.h"
#include "mbedtls/sha256.h"
#include "sdkconfig.h"

#include "app_state.h"
#include "app_utils.h"
#include "data_logger.h"
#include "error_manager.h"
#include "gps_module.h"
#include "isrgrootx1.pem.h"
#include "sd_maintenance.h"
#include "storage_manager.h"

#if CONFIG_SPIRAM
#include "esp_psram.h"
#endif

static constexpr char kTag[] = "UPLOAD";

static TaskHandle_t s_uploaded_clear_task = nullptr;
static int s_uploaded_clear_max_files = 1000;

// ---------- filename helpers ----------

static bool IsDataLogFilename(const char* name) {
  if (!name) return false;
  return std::strncmp(name, "data_", 5) == 0;
}

static int CountFilesInDir(const char* dir_path, bool only_data_logs) {
  if (!dir_path) return 0;
  DIR* dir = opendir(dir_path);
  if (!dir) return 0;
  int count = 0;
  struct dirent* ent = nullptr;
  while ((ent = readdir(dir)) != nullptr) {
    if (ent->d_name[0] == '.') continue;
    if (ent->d_type != DT_REG && ent->d_type != DT_UNKNOWN) continue;
    if (only_data_logs && !IsDataLogFilename(ent->d_name)) continue;
    count++;
  }
  closedir(dir);
  return count;
}

// ---------- file move helpers ----------

static int MoveRootDataFilesToUploadLocked(const std::string& active_path) {
  if (!EnsureUploadDirs()) return 0;
  const char* mount_point = ActiveStorageMountPoint();
  const std::string to_upload = ActiveToUploadDir();
  DIR* dir = opendir(mount_point);
  if (!dir) return 0;
  const std::string active_name = Basename(active_path);
  int moved = 0;
  struct dirent* ent = nullptr;
  while ((ent = readdir(dir)) != nullptr) {
    if (ent->d_name[0] == '.') continue;
    if (ent->d_type != DT_REG && ent->d_type != DT_UNKNOWN) continue;
    if (!IsDataLogFilename(ent->d_name)) continue;
    if (!active_name.empty() && active_name == ent->d_name) continue;
    const std::string src = std::string(mount_point) + "/" + ent->d_name;
    if (MoveFileToDir(src, to_upload.c_str(), nullptr)) {
      moved++;
    }
  }
  closedir(dir);
  return moved;
}

static int MoveRootMeteoFilesToUploadLocked(const std::string& active_path) {
  if (!EnsureUploadDirs()) return 0;
  const char* mount_point = ActiveStorageMountPoint();
  const std::string to_upload = ActiveToUploadDir();
  DIR* dir = opendir(mount_point);
  if (!dir) return 0;
  const std::string active_name = Basename(active_path);
  int moved = 0;
  struct dirent* ent = nullptr;
  while ((ent = readdir(dir)) != nullptr) {
    if (ent->d_name[0] == '.') continue;
    if (ent->d_type != DT_REG && ent->d_type != DT_UNKNOWN) continue;
    if (!IsMeteoLogFilename(ent->d_name)) continue;
    if (!active_name.empty() && active_name == ent->d_name) continue;
    const std::string src = std::string(mount_point) + "/" + ent->d_name;
    if (MoveFileToDir(src, to_upload.c_str(), nullptr)) {
      moved++;
    }
  }
  closedir(dir);
  return moved;
}

// ---------- SD stats ----------

void UpdateSdStatsLocked() {
  uint64_t total = 0;
  uint64_t used = 0;
  FsOps ops = DefaultFsOps();
  struct statvfs stats {};
  if (ops.statvfs_fn(CONFIG_MOUNT_POINT, &stats) == 0 && stats.f_blocks > 0) {
    total = static_cast<uint64_t>(stats.f_blocks) * stats.f_frsize;
    const uint64_t avail = static_cast<uint64_t>(stats.f_bavail) * stats.f_frsize;
    used = total > avail ? (total - avail) : 0;
  }
  const int root_files = CountFilesInDir(CONFIG_MOUNT_POINT, true);
  const int to_upload_files = CountFilesInDir(TO_UPLOAD_DIR, false);
  const int uploaded_files = CountFilesInDir(UPLOADED_DIR, false);
  UpdateState([&](SharedState& s) {
    s.sd_total_bytes = total;
    s.sd_used_bytes = used;
    s.sd_data_root_files = root_files;
    s.sd_to_upload_files = to_upload_files;
    s.sd_uploaded_files = uploaded_files;
  });
}

// ---------- uploaded-files cleanup ----------

static int DeleteUploadedFilesBatchLocked(int max_files, ClearUploadedFilesResult* result) {
  if (!MountActiveStorage() || !EnsureUploadDirs()) {
    result->mount_failed = true;
    return -1;
  }

  const std::string uploaded = ActiveUploadedDir();
  DIR* dir = opendir(uploaded.c_str());
  if (!dir) {
    if (app_config.storage_backend == StorageBackend::kSd) {
      UpdateSdStatsLocked();
    }
    return 0;
  }

  int scanned_in_batch = 0;
  struct dirent* ent = nullptr;
  while ((ent = readdir(dir)) != nullptr && scanned_in_batch < max_files) {
    if (ent->d_name[0] == '.') continue;
    if (ent->d_type != DT_REG && ent->d_type != DT_UNKNOWN) continue;
    const std::string full = uploaded + "/" + ent->d_name;
    struct stat st {};
    if (stat(full.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) {
      continue;
    }
    result->scanned++;
    scanned_in_batch++;
    if (remove(full.c_str()) == 0) {
      result->deleted++;
    } else {
      result->failed++;
      ESP_LOGW(kTag, "Failed to delete uploaded file %s (errno %d)", full.c_str(), errno);
    }
  }
  closedir(dir);
  if (app_config.storage_backend == StorageBackend::kSd) {
    UpdateSdStatsLocked();
  }
  return scanned_in_batch;
}

static void UploadedClearTask(void*) {
  constexpr int kBatchFiles = 8;
  constexpr int kMaxBusyRetries = 30;
  ClearUploadedFilesResult result{};
  const int limit = s_uploaded_clear_max_files;
  int busy_retries = 0;

  while (result.scanned < limit) {
    int batch_scanned = 0;
    {
      SdLockGuard guard(pdMS_TO_TICKS(100));
      if (!guard.locked()) {
        if (++busy_retries >= kMaxBusyRetries) {
          result.sd_busy = true;
          break;
        }
        vTaskDelay(pdMS_TO_TICKS(200));
        continue;
      }
      busy_retries = 0;
      const int batch_limit = std::min(kBatchFiles, limit - result.scanned);
      batch_scanned = DeleteUploadedFilesBatchLocked(batch_limit, &result);
    }

    if (batch_scanned <= 0) {
      break;
    }
    vTaskDelay(pdMS_TO_TICKS(50));
  }

  ESP_LOGI(kTag,
           "Manual uploaded cleanup done: scanned=%d deleted=%d failed=%d limit=%d%s%s",
           result.scanned, result.deleted, result.failed, limit,
           result.sd_busy ? " sd_busy" : "",
           result.mount_failed ? " mount_failed" : "");
  s_uploaded_clear_task = nullptr;
  vTaskDelete(nullptr);
}

bool StartUploadedClearTask(int max_files, std::string* out_status) {
  const int limit = std::clamp(max_files > 0 ? max_files : 1000, 1, 1000);
  if (s_uploaded_clear_task != nullptr) {
    if (out_status) *out_status = "already_running";
    return true;
  }

  s_uploaded_clear_max_files = limit;
  const BaseType_t ok = xTaskCreatePinnedToCore(&UploadedClearTask, "uploaded_clear", 4096,
                                                nullptr, 1, &s_uploaded_clear_task, 0);
  if (ok != pdPASS) {
    s_uploaded_clear_task = nullptr;
    if (out_status) *out_status = "start_failed";
    return false;
  }
  if (out_status) *out_status = "started";
  return true;
}

bool QueueCurrentLogForUpload() {
  SdLockGuard guard;
  if (!guard.locked()) {
    ESP_LOGW(kTag, "Storage mutex unavailable, cannot queue log");
    return false;
  }
  if (!MountActiveStorage()) {
    return false;
  }
  if (log_file) {
    FlushLogFile();
    fclose(log_file);
    log_file = nullptr;
  }
  if (current_log_path.empty()) {
    return false;
  }
  std::string new_path;
  const std::string to_upload = ActiveToUploadDir();
  if (!MoveFileToDir(current_log_path, to_upload.c_str(), &new_path)) {
    return false;
  }
  current_log_path.clear();
  (void)MoveRootDataFilesToUploadLocked("");
  if (app_config.storage_backend == StorageBackend::kSd) {
    UpdateSdStatsLocked();
  }
  ESP_LOGI(kTag, "Queued log for upload: %s", new_path.c_str());
  return true;
}

// ---------- crypto/hash helpers ----------

static std::string HexEncode(const uint8_t* data, size_t len) {
  static const char* kHex = "0123456789abcdef";
  std::string out;
  out.reserve(len * 2);
  for (size_t i = 0; i < len; ++i) {
    out.push_back(kHex[data[i] >> 4]);
    out.push_back(kHex[data[i] & 0x0F]);
  }
  return out;
}

static bool Sha256Bytes(const uint8_t* data, size_t len, uint8_t out[32]) {
  if (!out) return false;
  mbedtls_sha256_context ctx;
  mbedtls_sha256_init(&ctx);
  mbedtls_sha256_starts(&ctx, 0);
  mbedtls_sha256_update(&ctx, data, len);
  mbedtls_sha256_finish(&ctx, out);
  mbedtls_sha256_free(&ctx);
  return true;
}

static bool Sha256String(const std::string& input, std::string* out_hex) {
  uint8_t hash[32];
  if (!Sha256Bytes(reinterpret_cast<const uint8_t*>(input.data()), input.size(), hash)) {
    return false;
  }
  if (out_hex) {
    *out_hex = HexEncode(hash, sizeof(hash));
  }
  return true;
}

static bool Sha256File(const std::string& path, std::string* out_hex, size_t* out_size) {
  if (out_size) *out_size = 0;
  FILE* f = fopen(path.c_str(), "rb");
  if (!f) {
    ESP_LOGE(kTag, "Failed to open %s for hashing", path.c_str());
    return false;
  }
  mbedtls_sha256_context ctx;
  mbedtls_sha256_init(&ctx);
  mbedtls_sha256_starts(&ctx, 0);
  std::unique_ptr<uint8_t[]> buf(new (std::nothrow) uint8_t[1024]);
  if (!buf) {
    mbedtls_sha256_free(&ctx);
    fclose(f);
    ESP_LOGE(kTag, "No memory for hash buffer");
    return false;
  }
  size_t total = 0;
  size_t n = 0;
  while ((n = fread(buf.get(), 1, 1024, f)) > 0) {
    total += n;
    mbedtls_sha256_update(&ctx, buf.get(), n);
  }
  fclose(f);
  if (out_size) *out_size = total;
  uint8_t hash[32];
  mbedtls_sha256_finish(&ctx, hash);
  mbedtls_sha256_free(&ctx);
  if (out_hex) {
    *out_hex = HexEncode(hash, sizeof(hash));
  }
  return true;
}

// ---------- SigV4 ----------

static bool HmacSha256(const uint8_t* key, size_t key_len, const uint8_t* data, size_t data_len, uint8_t out[32]) {
  if (!key || !out) return false;
  const mbedtls_md_info_t* info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  if (!info) return false;
  if (mbedtls_md_hmac(info, key, key_len, data, data_len, out) != 0) {
    return false;
  }
  return true;
}

static bool DeriveS3SigningKey(const std::string& secret, const std::string& date, const std::string& region, uint8_t out[32]) {
  if (secret.empty() || date.size() != 8 || region.empty() || !out) return false;
  std::string key = "AWS4" + secret;
  uint8_t k_date[32];
  uint8_t k_region[32];
  uint8_t k_service[32];
  if (!HmacSha256(reinterpret_cast<const uint8_t*>(key.data()), key.size(),
                  reinterpret_cast<const uint8_t*>(date.data()), date.size(), k_date)) {
    return false;
  }
  if (!HmacSha256(k_date, sizeof(k_date), reinterpret_cast<const uint8_t*>(region.data()), region.size(), k_region)) {
    return false;
  }
  static const char kService[] = "s3";
  if (!HmacSha256(k_region, sizeof(k_region), reinterpret_cast<const uint8_t*>(kService), strlen(kService), k_service)) {
    return false;
  }
  static const char kTerm[] = "aws4_request";
  if (!HmacSha256(k_service, sizeof(k_service), reinterpret_cast<const uint8_t*>(kTerm), strlen(kTerm), out)) {
    return false;
  }
  return true;
}

// ---------- upload helpers ----------

static std::string TrimTrailingSlash(const std::string& in) {
  std::string out = in;
  while (!out.empty() && out.back() == '/') {
    out.pop_back();
  }
  return out;
}

static std::string ExtractHost(const std::string& endpoint) {
  std::string host = endpoint;
  size_t scheme = host.find("://");
  if (scheme != std::string::npos) {
    host = host.substr(scheme + 3);
  }
  size_t slash = host.find('/');
  if (slash != std::string::npos) {
    host = host.substr(0, slash);
  }
  return host;
}

static std::string UploadHeapDiag() {
  char buf[192];
  std::snprintf(buf, sizeof(buf),
                "heap=%u min=%u largest=%u internal=%u internal_largest=%u psram=%u psram_largest=%u",
                static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_8BIT)),
                static_cast<unsigned>(heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT)),
                static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)),
                static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
                static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
                static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)),
                static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)));
  return std::string(buf);
}

const char* MbedtlsAllocModeName() {
#if CONFIG_MBEDTLS_INTERNAL_MEM_ALLOC
  return "internal";
#elif CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC
  return "external";
#elif CONFIG_MBEDTLS_DEFAULT_MEM_ALLOC
  return "default";
#elif CONFIG_MBEDTLS_CUSTOM_MEM_ALLOC
  return "custom";
#elif CONFIG_MBEDTLS_IRAM_8BIT_MEM_ALLOC
  return "iram8";
#else
  return "unknown";
#endif
}

void LogMemoryStatus(const char* label) {
  ESP_LOGI(kTag, "Memory %s: %s", label ? label : "-", UploadHeapDiag().c_str());
#if CONFIG_SPIRAM
  ESP_LOGI(kTag, "PSRAM %s: initialized=%s size=%u bytes",
           label ? label : "-",
           esp_psram_is_initialized() ? "yes" : "no",
           static_cast<unsigned>(esp_psram_is_initialized() ? esp_psram_get_size() : 0));
#else
  ESP_LOGI(kTag, "PSRAM %s: disabled in sdkconfig", label ? label : "-");
#endif
}

static bool HasHeapForTlsUpload() {
  const uint32_t free_heap = static_cast<uint32_t>(heap_caps_get_free_size(MALLOC_CAP_8BIT));
  const uint32_t largest = static_cast<uint32_t>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
  const uint32_t internal_free = static_cast<uint32_t>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  const uint32_t internal_largest =
      static_cast<uint32_t>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
#if CONFIG_MBEDTLS_INTERNAL_MEM_ALLOC
  constexpr uint32_t min_largest = 32 * 1024;
  constexpr uint32_t min_free = 60 * 1024;
  constexpr uint32_t min_internal_largest = 32 * 1024;
  constexpr uint32_t min_internal_free = 60 * 1024;
#else
  constexpr uint32_t min_largest = 32 * 1024;
  constexpr uint32_t min_free = 60 * 1024;
  constexpr uint32_t min_internal_largest = 10 * 1024;
  constexpr uint32_t min_internal_free = 24 * 1024;
#endif
  if (largest >= min_largest && free_heap >= min_free &&
      internal_largest >= min_internal_largest && internal_free >= min_internal_free) {
    return true;
  }
  char msg[224];
  std::snprintf(msg, sizeof(msg),
                "TLS upload deferred: heap=%u largest=%u internal=%u internal_largest=%u need heap>=%u largest>=%u internal>=%u internal_largest>=%u",
                static_cast<unsigned>(free_heap),
                static_cast<unsigned>(largest),
                static_cast<unsigned>(internal_free),
                static_cast<unsigned>(internal_largest),
                static_cast<unsigned>(min_free),
                static_cast<unsigned>(min_largest),
                static_cast<unsigned>(min_internal_free),
                static_cast<unsigned>(min_internal_largest));
  ErrorManagerSet(ErrorCode::kMinioUpload, ErrorSeverity::kWarning, msg);
  ESP_LOGW(kTag, "%s", msg);
  return false;
}

// ---------- MinIO upload ----------

static bool UploadFileToMinio(const std::string& path) {
  if (!app_config.minio_enabled) {
    ErrorManagerClear(ErrorCode::kMinioUpload);
    ESP_LOGI(kTag, "MinIO disabled, skip upload");
    return false;
  }
  if (app_config.minio_endpoint.empty() || app_config.minio_access_key.empty() || app_config.minio_secret_key.empty() ||
      app_config.minio_bucket.empty()) {
    ErrorManagerSet(ErrorCode::kMinioUpload, ErrorSeverity::kWarning, "MinIO config incomplete");
    ESP_LOGW(kTag, "MinIO config incomplete, skip upload");
    return false;
  }
  const std::string filename = Basename(path);
  std::string object_prefix = "radiometers";
  if (IsGpsLogFilename(filename.c_str())) {
    object_prefix = "gps";
  } else if (IsMeteoLogFilename(filename.c_str())) {
    object_prefix = "meteo";
  } else if (IsDataLogFilename(filename.c_str())) {
    object_prefix = "radiometers";
  }
  const std::string object_key = object_prefix + "/" + filename;
  const std::string endpoint = TrimTrailingSlash(app_config.minio_endpoint);
  const std::string host = ExtractHost(endpoint);
  const bool use_https = endpoint.rfind("https://", 0) == 0;
  if (host.empty()) {
    ErrorManagerSet(ErrorCode::kMinioUpload, ErrorSeverity::kWarning, "Invalid MinIO endpoint");
    ESP_LOGE(kTag, "Invalid MinIO endpoint: %s", endpoint.c_str());
    return false;
  }
  if (use_https && !HasHeapForTlsUpload()) {
    return false;
  }
  const std::string url = endpoint + "/" + app_config.minio_bucket + "/" + object_key;

  std::string payload_hash;
  size_t file_size = 0;
  if (!Sha256File(path, &payload_hash, &file_size)) {
    ErrorManagerSet(ErrorCode::kMinioUpload, ErrorSeverity::kWarning, "Failed to hash upload file");
    ESP_LOGE(kTag, "Failed to hash %s", path.c_str());
    return false;
  }
  ESP_LOGI(kTag,
           "MinIO upload prepare: file=%s size=%u tls=%s tls_in=%d tls_out=%d dynamic_buffer=%s mbedtls_alloc=%s %s",
           path.c_str(),
           static_cast<unsigned>(file_size),
           use_https ? "yes" : "no",
           CONFIG_MBEDTLS_SSL_IN_CONTENT_LEN,
           CONFIG_MBEDTLS_SSL_OUT_CONTENT_LEN,
#if CONFIG_MBEDTLS_DYNAMIC_BUFFER
           "yes",
#else
           "no",
#endif
           MbedtlsAllocModeName(),
           UploadHeapDiag().c_str());
  if (app_config.minio_bucket.empty()) {
    app_config.minio_bucket = SanitizeId(app_config.device_id);
  }

  [[maybe_unused]] auto put_bucket_if_needed = [&]() {
    const std::string payload_hash = "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";
    const UtcTimeSnapshot now_snapshot = GetBestUtcTimeForData();
    time_t now = now_snapshot.unix_time;
    struct tm tm_utc {};
    gmtime_r(&now, &tm_utc);
    char date_buf[9];
    char amz_date[17];
    strftime(date_buf, sizeof(date_buf), "%Y%m%d", &tm_utc);
    strftime(amz_date, sizeof(amz_date), "%Y%m%dT%H%M%SZ", &tm_utc);
    const char* region = "us-east-1";
    const std::string canonical_uri = "/" + app_config.minio_bucket;
    const std::string canonical_headers = "host:" + host + "\n" +
                                          "x-amz-content-sha256:" + payload_hash + "\n" +
                                          "x-amz-date:" + amz_date + "\n";
    const std::string signed_headers = "host;x-amz-content-sha256;x-amz-date";
    const std::string canonical_request =
        "PUT\n" + canonical_uri + "\n\n" + canonical_headers + "\n" + signed_headers + "\n" + payload_hash;
    std::string canonical_hash_hex;
    if (!Sha256String(canonical_request, &canonical_hash_hex)) {
      return false;
    }
    const std::string credential_scope = std::string(date_buf) + "/" + region + "/s3/aws4_request";
    const std::string string_to_sign = std::string("AWS4-HMAC-SHA256\n") + amz_date + "\n" + credential_scope + "\n" + canonical_hash_hex;
    uint8_t signing_key[32];
    if (!DeriveS3SigningKey(app_config.minio_secret_key, date_buf, region, signing_key)) {
      return false;
    }
    uint8_t signature_bin[32];
    if (!HmacSha256(signing_key, sizeof(signing_key),
                    reinterpret_cast<const uint8_t*>(string_to_sign.data()), string_to_sign.size(), signature_bin)) {
      return false;
    }
    const std::string signature_hex = HexEncode(signature_bin, sizeof(signature_bin));
    const std::string authorization = "AWS4-HMAC-SHA256 Credential=" + app_config.minio_access_key + "/" + credential_scope +
                                      ", SignedHeaders=" + signed_headers + ", Signature=" + signature_hex;
    const std::string bucket_url = endpoint + "/" + app_config.minio_bucket;

    esp_http_client_config_t cfg = {};
    cfg.url = bucket_url.c_str();
    cfg.method = HTTP_METHOD_PUT;
    cfg.transport_type = use_https ? HTTP_TRANSPORT_OVER_SSL : HTTP_TRANSPORT_OVER_TCP;
    cfg.disable_auto_redirect = true;
    cfg.timeout_ms = 8000;
    cfg.buffer_size = 1024;
    cfg.buffer_size_tx = 1024;
    cfg.cert_pem = reinterpret_cast<const char*>(isrgrootx1_pem_start);
    cfg.cert_len = isrgrootx1_pem_end - isrgrootx1_pem_start;
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return false;
    esp_http_client_set_header(client, "Host", host.c_str());
    esp_http_client_set_header(client, "Content-Length", "0");
    esp_http_client_set_header(client, "x-amz-content-sha256", payload_hash.c_str());
    esp_http_client_set_header(client, "x-amz-date", amz_date);
    esp_http_client_set_header(client, "Authorization", authorization.c_str());
    bool ok = false;
    if (esp_http_client_open(client, 0) == ESP_OK) {
      esp_http_client_fetch_headers(client);
      int status = esp_http_client_get_status_code(client);
      ok = (status >= 200 && status < 300);
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return ok;
  };

  const UtcTimeSnapshot now_snapshot = GetBestUtcTimeForData();
  time_t now = now_snapshot.unix_time;
  struct tm tm_utc {};
  gmtime_r(&now, &tm_utc);
  char date_buf[9];
  char amz_date[17];
  strftime(date_buf, sizeof(date_buf), "%Y%m%d", &tm_utc);
  strftime(amz_date, sizeof(amz_date), "%Y%m%dT%H%M%SZ", &tm_utc);

  const char* region = "us-east-1";
  const std::string canonical_uri = "/" + app_config.minio_bucket + "/" + object_key;
  const std::string canonical_headers = "host:" + host + "\n" +
                                        "x-amz-content-sha256:" + payload_hash + "\n" +
                                        "x-amz-date:" + amz_date + "\n";
  const std::string signed_headers = "host;x-amz-content-sha256;x-amz-date";
  const std::string canonical_request =
      "PUT\n" + canonical_uri + "\n\n" + canonical_headers + "\n" + signed_headers + "\n" + payload_hash;

  std::string canonical_hash_hex;
  if (!Sha256String(canonical_request, &canonical_hash_hex)) {
    ErrorManagerSet(ErrorCode::kMinioUpload, ErrorSeverity::kWarning, "Failed to hash canonical request");
    ESP_LOGE(kTag, "Failed to hash canonical request");
    return false;
  }

  const std::string credential_scope = std::string(date_buf) + "/" + region + "/s3/aws4_request";
  const std::string string_to_sign = std::string("AWS4-HMAC-SHA256\n") + amz_date + "\n" + credential_scope + "\n" + canonical_hash_hex;

  uint8_t signing_key[32];
  if (!DeriveS3SigningKey(app_config.minio_secret_key, date_buf, region, signing_key)) {
    ErrorManagerSet(ErrorCode::kMinioUpload, ErrorSeverity::kWarning, "Failed to derive signing key");
    ESP_LOGE(kTag, "Failed to derive signing key");
    return false;
  }
  uint8_t signature_bin[32];
  if (!HmacSha256(signing_key, sizeof(signing_key),
                  reinterpret_cast<const uint8_t*>(string_to_sign.data()), string_to_sign.size(), signature_bin)) {
    ErrorManagerSet(ErrorCode::kMinioUpload, ErrorSeverity::kWarning, "Failed to sign request");
    ESP_LOGE(kTag, "Failed to sign string");
    return false;
  }
  const std::string signature_hex = HexEncode(signature_bin, sizeof(signature_bin));
  const std::string authorization = "AWS4-HMAC-SHA256 Credential=" + app_config.minio_access_key + "/" + credential_scope +
                                    ", SignedHeaders=" + signed_headers + ", Signature=" + signature_hex;

  esp_http_client_config_t cfg_http = {};
  cfg_http.url = url.c_str();
  cfg_http.method = HTTP_METHOD_PUT;
  cfg_http.timeout_ms = 10000;
  cfg_http.buffer_size = 1024;
  cfg_http.buffer_size_tx = 1024;
  cfg_http.transport_type = use_https ? HTTP_TRANSPORT_OVER_SSL : HTTP_TRANSPORT_OVER_TCP;
  cfg_http.disable_auto_redirect = true;
  cfg_http.cert_pem = reinterpret_cast<const char*>(isrgrootx1_pem_start);
  cfg_http.cert_len = isrgrootx1_pem_end - isrgrootx1_pem_start;

  esp_http_client_handle_t client = esp_http_client_init(&cfg_http);
  if (!client) {
    ErrorManagerSet(ErrorCode::kMinioUpload, ErrorSeverity::kWarning, "esp_http_client_init failed");
    ESP_LOGE(kTag, "esp_http_client_init failed");
    return false;
  }
  char len_buf[32];
  snprintf(len_buf, sizeof(len_buf), "%u", static_cast<unsigned>(file_size));
  esp_http_client_set_header(client, "Host", host.c_str());
  esp_http_client_set_header(client, "Content-Length", len_buf);
  esp_http_client_set_header(client, "x-amz-content-sha256", payload_hash.c_str());
  esp_http_client_set_header(client, "x-amz-date", amz_date);
  esp_http_client_set_header(client, "Authorization", authorization.c_str());

  ESP_LOGI(kTag, "MinIO HTTP open begin: %s", UploadHeapDiag().c_str());
  const esp_err_t open_err = esp_http_client_open(client, file_size);
  if (open_err != ESP_OK) {
    const std::string heap_diag = UploadHeapDiag();
    const std::string diag = std::string("HTTP open failed: ") + esp_err_to_name(open_err) + " " + heap_diag;
    ErrorManagerSet(ErrorCode::kMinioUpload, ErrorSeverity::kWarning, diag);
    ESP_LOGE(kTag, "Failed to open HTTP connection to %s: %s (%s)",
             url.c_str(), esp_err_to_name(open_err), heap_diag.c_str());
    esp_http_client_cleanup(client);
    return false;
  }
  ESP_LOGI(kTag, "MinIO HTTP open ok: %s", UploadHeapDiag().c_str());

  FILE* f = fopen(path.c_str(), "rb");
  if (!f) {
    ErrorManagerSet(ErrorCode::kMinioUpload, ErrorSeverity::kWarning, "Failed to open upload file");
    ESP_LOGE(kTag, "Cannot reopen file %s for upload", path.c_str());
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return false;
  }
  char buf[1024];
  size_t n = 0;
  bool ok = true;
  while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
    size_t offset = 0;
    while (offset < n) {
      const int written = esp_http_client_write(client, buf + offset, n - offset);
      if (written <= 0) {
        ErrorManagerSet(ErrorCode::kMinioUpload, ErrorSeverity::kWarning, "HTTP write failed or incomplete");
        ESP_LOGE(kTag, "HTTP write failed at %u/%u bytes", static_cast<unsigned>(offset), static_cast<unsigned>(n));
        ok = false;
        break;
      }
      offset += static_cast<size_t>(written);
    }
    if (!ok) break;
  }
  if (ferror(f)) {
    ErrorManagerSet(ErrorCode::kMinioUpload, ErrorSeverity::kWarning, "Upload file read failed");
    ESP_LOGE(kTag, "Failed while reading upload file %s", path.c_str());
    ok = false;
  }
  fclose(f);
  if (!ok) {
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return false;
  }
  esp_http_client_fetch_headers(client);
  int status = esp_http_client_get_status_code(client);
  esp_http_client_close(client);
  esp_http_client_cleanup(client);
  ESP_LOGI(kTag, "MinIO HTTP cleanup done: status=%d %s", status, UploadHeapDiag().c_str());
  if (status >= 200 && status < 300) {
    ErrorManagerClear(ErrorCode::kMinioUpload);
    ESP_LOGI(kTag, "Uploaded %s to MinIO (%d)", path.c_str(), status);
    return true;
  }
  ErrorManagerSet(ErrorCode::kMinioUpload, ErrorSeverity::kWarning, "MinIO upload failed");
  ESP_LOGE(kTag, "MinIO upload failed, status %d", status);
  return false;
}

static void CleanupUploadedDirIfNeeded(int max_percent);

static bool UploadPendingOnce() {
  constexpr int kMaxSdUsagePercent = 60;
  constexpr int kMaxUploadsPerCycle = 1;
  constexpr int kMaxUploadAttemptsPerCycle = 1;
  UpdateState([](SharedState& s) {
    s.heap_free_bytes = static_cast<uint32_t>(heap_caps_get_free_size(MALLOC_CAP_8BIT));
    s.heap_min_free_bytes = static_cast<uint32_t>(heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT));
    s.heap_largest_free_block_bytes = static_cast<uint32_t>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    s.heap_internal_free_bytes = static_cast<uint32_t>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    s.heap_internal_largest_free_block_bytes =
        static_cast<uint32_t>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    s.heap_psram_free_bytes = static_cast<uint32_t>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    s.heap_psram_largest_free_block_bytes =
        static_cast<uint32_t>(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  });
  if (!app_config.minio_enabled) {
    ErrorManagerClear(ErrorCode::kMinioUpload);
    return false;
  }
  std::vector<std::string> files;
  {
    SdLockGuard guard(pdMS_TO_TICKS(50));
    if (!guard.locked()) {
      ESP_LOGW(kTag, "Storage mutex busy, skip upload cycle");
      return false;
    }
    if (!MountActiveStorage()) {
      return false;
    }
    if (!EnsureUploadDirs()) {
      return false;
    }
    const std::string to_upload = ActiveToUploadDir();
    DIR* dir = opendir(to_upload.c_str());
    if (!dir) {
      ESP_LOGI(kTag, "No upload dir, nothing to sync");
      return false;
    }
    struct dirent* ent = nullptr;
    while ((ent = readdir(dir)) != nullptr) {
      if (ent->d_name[0] == '.') continue;
      if (ent->d_type != DT_REG && ent->d_type != DT_UNKNOWN) continue;
      std::string full = to_upload + "/" + ent->d_name;
      files.push_back(full);
    }
    closedir(dir);
  }

  int uploaded = 0;
  int attempts = 0;
  for (const auto& f : files) {
    if (uploaded >= kMaxUploadsPerCycle || attempts >= kMaxUploadAttemptsPerCycle) {
      break;
    }
    attempts++;
    const uint64_t attempt_ms = esp_timer_get_time() / 1000ULL;
    const bool upload_succeeded = UploadFileToMinio(f);
    bool archived = false;
    if (upload_succeeded) {
      SdLockGuard guard(pdMS_TO_TICKS(200));
      if (!guard.locked() || !MountActiveStorage()) {
        ESP_LOGW(kTag, "Storage busy, cannot move uploaded file %s", f.c_str());
        ErrorManagerSet(ErrorCode::kMinioUpload, ErrorSeverity::kWarning,
                        "MinIO PUT succeeded but local file archive is busy");
      } else {
        const std::string uploaded_dir = ActiveUploadedDir();
        if (MoveFileToDir(f, uploaded_dir.c_str(), nullptr)) {
          uploaded++;
          archived = true;
        } else {
          ESP_LOGW(kTag, "MinIO PUT succeeded but failed to move %s to uploaded", f.c_str());
          ErrorManagerSet(ErrorCode::kMinioUpload, ErrorSeverity::kWarning,
                          "MinIO PUT succeeded but local file archive failed");
        }
      }
    }
    const uint64_t result_ms = esp_timer_get_time() / 1000ULL;
    UpdateStateBlocking([attempt_ms, upload_succeeded, archived, result_ms](SharedState& s) {
      if (s.minio_upload_attempts < UINT32_MAX) s.minio_upload_attempts++;
      s.minio_last_attempt_ms = attempt_ms;
      if (upload_succeeded) {
        if (s.minio_upload_successes < UINT32_MAX) s.minio_upload_successes++;
        s.minio_last_success_ms = result_ms;
        if (!archived && s.minio_archive_failures < UINT32_MAX) s.minio_archive_failures++;
      } else {
        if (s.minio_upload_failures < UINT32_MAX) s.minio_upload_failures++;
        s.minio_last_failure_ms = result_ms;
      }
    });
    vTaskDelay(pdMS_TO_TICKS(1000));
  }
  if (uploaded > 0) {
    ESP_LOGI(kTag, "Uploaded %d file(s) to MinIO", uploaded);
  }
  CleanupUploadedDirIfNeeded(kMaxSdUsagePercent);
  {
    SdLockGuard guard(pdMS_TO_TICKS(50));
    if (guard.locked() && app_config.storage_backend == StorageBackend::kSd && MountLogSd()) {
      UpdateSdStatsLocked();
    }
  }
  if (uploaded > 0) {
    return true;
  }
  return false;
}

static void CleanupUploadedDirIfNeeded(int max_percent) {
  SdLockGuard guard(pdMS_TO_TICKS(200));
  if (!guard.locked()) {
    ESP_LOGW(kTag, "Storage mutex unavailable, skip cleanup");
    return;
  }
  if (!MountActiveStorage()) {
    return;
  }
  const std::string uploaded = ActiveUploadedDir();
  int deleted = PurgeUploadedFiles(ActiveStorageMountPoint(), uploaded.c_str(), max_percent);
  if (deleted > 0) {
    ESP_LOGI(kTag, "Deleted %d uploaded file(s) to free space", deleted);
  }
}

void UpdateSdStats() {
  if (app_config.storage_backend == StorageBackend::kInternalFlash) {
    UpdateState([](SharedState& s) {
      s.sd_total_bytes = 0;
      s.sd_used_bytes = 0;
      s.sd_data_root_files = 0;
      s.sd_to_upload_files = 0;
      s.sd_uploaded_files = 0;
    });
    return;
  }
  SdLockGuard guard(pdMS_TO_TICKS(200));
  if (!guard.locked()) {
    return;
  }
  const bool already_mounted = IsLogSdMounted();
  if (!already_mounted && !MountLogSd()) {
    UpdateState([](SharedState& s) {
      s.sd_total_bytes = 0;
      s.sd_used_bytes = 0;
      s.sd_data_root_files = 0;
      s.sd_to_upload_files = 0;
      s.sd_uploaded_files = 0;
    });
    return;
  }
  UpdateSdStatsLocked();
  if (!already_mounted && !log_file) {
    UnmountLogSd();
  }
}

void SdStatsTask(void*) {
  const TickType_t interval = pdMS_TO_TICKS(10000);
  while (true) {
    UpdateSdStats();
    vTaskDelay(interval);
  }
}

void UploadTask(void*) {
  const TickType_t interval = pdMS_TO_TICKS(10 * 60 * 1000);
  vTaskDelay(pdMS_TO_TICKS(2 * 60 * 1000));
  {
    SdLockGuard guard(pdMS_TO_TICKS(1000));
    if (guard.locked() && MountActiveStorage()) {
      MoveRootMeteoFilesToUploadLocked(ActiveMeteoLogPathLocked());
      if (app_config.storage_backend == StorageBackend::kSd && !log_file) {
        UnmountLogSd();
      }
    }
  }
  while (true) {
    UploadPendingOnce();
    vTaskDelay(interval);
  }
}
