#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <functional>
#include <cmath>
#include <array>
#include <string>
#include <vector>
#include <set>
#include <ctime>
#include <cstdint>
#include <sys/stat.h>
#include <cerrno>
#include <dirent.h>
#include <memory>
#include "isrgrootx1.pem.h"

#include "cJSON.h"
#include "app_state.h"
#include "app_services.h"
#include "web_ui.h"
#include "http_handlers.h"
#include "driver/gpio.h"
#include "driver/spi_common.h"
#include "driver/ledc.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_task_wdt.h"
#include "driver/i2c_master.h"
#include "esp_wifi.h"
#include "esp_sntp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include "freertos/semphr.h"
#include "ltc2440.h"
#include "nvs_flash.h"
#include "mbedtls/md.h"
#include "mbedtls/sha256.h"
#include "mqtt_bridge.h"
#include "nvs.h"
#include "sdmmc_cmd.h"
#include "esp_vfs_fat.h"
#include "tinyusb.h"
#include "tusb.h"
#include "class/msc/msc.h"
#include "tusb_msc_storage.h"
#include "esp_rom_sys.h"
#include "onewire_m1820.h"
#include <dirent.h>
#include <sys/stat.h>

#include "hw_pins.h"
#if 0  // legacy addresses kept for reference
[[maybe_unused]] constexpr uint64_t TEMP_ADDR_RAD = 0x77062223A096AD28ULL;
[[maybe_unused]] constexpr uint64_t TEMP_ADDR_LOAD = 0xE80000105FF4E228ULL;
#endif

constexpr float VREF = 4.096f;  // ±Vref/2 range, matches original sketch
constexpr float ADC_SCALE = (VREF / 2.0f) / static_cast<float>(1 << 23);

constexpr char HOSTNAME[] = "miap-device";
constexpr bool USE_CUSTOM_MAC = true;
uint8_t CUSTOM_MAC[6] = {0x10, 0x00, 0x3B, 0x6E, 0x83, 0x70};
constexpr char MSC_BASE_PATH[] = "/usb_msc";
// INA219 constants (32V, 2A, Rshunt=0.1 ohm)
constexpr uint8_t INA219_ADDR = 0x40;
constexpr uint16_t INA219_CONFIG = 0x399F;           // 32V range, gain /8 (320mV), 12-bit, continuous
constexpr uint16_t INA219_CALIBRATION = 4096;        // current_LSB=100uA for 0.1R, power_LSB=2mW
constexpr float INA219_CURRENT_LSB = 0.0001f;        // 100 uA
constexpr float INA219_POWER_LSB = INA219_CURRENT_LSB * 20.0f;
constexpr float INA219_BUS_LSB = 0.004f;             // 4 mV
// Wi-Fi helpers
constexpr int WIFI_CONNECTED_BIT = BIT0;
constexpr int WIFI_FAIL_BIT = BIT1;
EventGroupHandle_t wifi_event_group = nullptr;
int retry_count = 0;
bool time_synced = false;
bool wifi_inited = false;
esp_netif_t* wifi_netif_sta = nullptr;
esp_netif_t* wifi_netif_ap = nullptr;

volatile uint32_t fan1_pulse_count = 0;
volatile uint32_t fan2_pulse_count = 0;

i2c_master_bus_handle_t i2c_bus = nullptr;
i2c_master_dev_handle_t ina219_dev = nullptr;
TimerHandle_t error_blink_timer = nullptr;

void ErrorBlinkTimerCb(TimerHandle_t) {
  static bool on = false;
  on = !on;
  gpio_set_level(STATUS_LED_RED, on ? 1 : 0);
}

void StartErrorBlink() {
  gpio_set_level(STATUS_LED_GREEN, 0);
  if (!error_blink_timer) {
    error_blink_timer =
        xTimerCreate("err_led", pdMS_TO_TICKS(250), pdTRUE, nullptr, reinterpret_cast<TimerCallbackFunction_t>(ErrorBlinkTimerCb));
  }
  if (error_blink_timer) {
    xTimerStart(error_blink_timer, 0);
  }
}

void SetStatusLeds(bool ok) {
  if (ok) {
    if (error_blink_timer) {
      xTimerStop(error_blink_timer, 0);
    }
    gpio_set_level(STATUS_LED_RED, 0);
    gpio_set_level(STATUS_LED_GREEN, 1);
  } else {
    StartErrorBlink();
  }
}

static void IRAM_ATTR FanTachIsr(void* arg) {
  uint32_t gpio = (uint32_t)arg;
  if (gpio == static_cast<uint32_t>(FAN1_TACH)) {
    __atomic_fetch_add(&fan1_pulse_count, 1, __ATOMIC_RELAXED);
  } else if (gpio == static_cast<uint32_t>(FAN2_TACH)) {
    __atomic_fetch_add(&fan2_pulse_count, 1, __ATOMIC_RELAXED);
  }
}

bool IsHallTriggered() {
  return gpio_get_level(MT_HALL_SEN) == 0;
}

bool SanitizeFilename(const std::string& name, std::string* out_full) {
  if (name.empty() || name.size() > 64) return false;
  for (char c : name) {
    if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-' || c == '.')) {
      return false;
    }
  }
  std::string full = std::string(CONFIG_MOUNT_POINT) + "/" + name;
  if (out_full) *out_full = full;
  return true;
}

bool SanitizePath(const std::string& rel_path_raw, std::string* out_full) {
  if (rel_path_raw.empty() || rel_path_raw.size() > 256) return false;
  std::string rel_path = rel_path_raw;
  if (!rel_path.empty() && rel_path[0] == '/') rel_path.erase(rel_path.begin());
  if (rel_path.empty() || rel_path.size() > 256) return false;
  if (rel_path.find("..") != std::string::npos) return false;
  if (rel_path.find("//") != std::string::npos) return false;
  for (char c : rel_path) {
    if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-' || c == '.' || c == '/' || c == ' ' || c == '(' || c == ')')) {
      return false;
    }
  }
  std::string full = std::string(CONFIG_MOUNT_POINT) + "/" + rel_path;
  if (out_full) *out_full = full;
  return true;
}

std::string SanitizePostfix(const std::string& raw) {
  // Keep postfix within overall filename limit (SanitizeFilename caps at 64 chars).
  // Base pattern: data_YYYYMMDD_HHMMSS_ + postfix + .txt -> base length 25, so allow up to 39.
  constexpr size_t kMaxPostfixLen = 39;
  std::string out;
  out.reserve(raw.size());
  for (char c : raw) {
    if (std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-') {
      out.push_back(c);
    } else if (std::isspace(static_cast<unsigned char>(c))) {
      out.push_back('_');
    }
    if (out.size() >= kMaxPostfixLen) break;
  }
  // Trim trailing underscores from whitespace-only input
  while (!out.empty() && out.back() == '_') {
    out.pop_back();
  }
  return out;
}

bool EnsureTimeSynced(int timeout_ms);

std::string BuildLogFilename(const std::string& postfix_raw) {
  const std::string postfix = SanitizePostfix(postfix_raw);

  EnsureTimeSynced(1000);
  time_t now = time(nullptr);
  if (now <= 0) {
    now = static_cast<time_t>(esp_timer_get_time() / 1000000ULL);  // fallback if SNTP not yet synced
  }
  struct tm tm_info;
  gmtime_r(&now, &tm_info);
  char ts[32];
  strftime(ts, sizeof(ts), "%Y%m%d_%H%M%S", &tm_info);

  std::string name = "data_";
  name += ts;
  if (!postfix.empty()) {
    name += "_";
    name += postfix;
  }
  name += ".txt";
  return name;
}

// Devices
LTC2440 adc1(ADC_CS1, ADC_MISO);
LTC2440 adc2(ADC_CS2, ADC_MISO);
LTC2440 adc3(ADC_CS3, ADC_MISO);

std::string IsoUtcNow() {
  time_t now = time(nullptr);
  if (now <= 0) {
    now = static_cast<time_t>(esp_timer_get_time() / 1'000'000ULL);
  }
  struct tm tm_utc {};
  gmtime_r(&now, &tm_utc);
  char buf[32];
  strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm_utc);
  return std::string(buf);
}

bool EnsureDirExists(const char* path) {
  if (!path) return false;
  struct stat st {};
  if (stat(path, &st) == 0) {
    return S_ISDIR(st.st_mode);
  }
  if (mkdir(path, 0775) == 0) {
    return true;
  }
  ESP_LOGE(TAG, "mkdir %s failed: %d", path, errno);
  return false;
}

bool EnsureUploadDirs() {
  return EnsureDirExists(TO_UPLOAD_DIR) && EnsureDirExists(UPLOADED_DIR);
}

static std::string Basename(const std::string& path) {
  const size_t pos = path.find_last_of('/');
  if (pos == std::string::npos) return path;
  return path.substr(pos + 1);
}

bool MoveFileToDir(const std::string& src_path, const char* dest_dir, std::string* out_new_path) {
  if (src_path.empty() || !dest_dir) return false;
  if (!EnsureDirExists(dest_dir)) return false;
  std::string dest = std::string(dest_dir) + "/" + Basename(src_path);
  if (rename(src_path.c_str(), dest.c_str()) != 0) {
    ESP_LOGE(TAG, "Failed to move %s -> %s (errno %d)", src_path.c_str(), dest.c_str(), errno);
    return false;
  }
  if (out_new_path) {
    *out_new_path = dest;
  }
  return true;
}

bool QueueCurrentLogForUpload() {
  SdLockGuard guard;
  if (!guard.locked()) {
    ESP_LOGW(TAG, "SD mutex unavailable, cannot queue log");
    return false;
  }
  if (!MountLogSd()) {
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
  if (!MoveFileToDir(current_log_path, TO_UPLOAD_DIR, &new_path)) {
    return false;
  }
  current_log_path.clear();
  ESP_LOGI(TAG, "Queued log for upload: %s", new_path.c_str());
  return true;
}

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
    ESP_LOGE(TAG, "Failed to open %s for hashing", path.c_str());
    return false;
  }
  mbedtls_sha256_context ctx;
  mbedtls_sha256_init(&ctx);
  mbedtls_sha256_starts(&ctx, 0);
  std::unique_ptr<uint8_t[]> buf(new (std::nothrow) uint8_t[1024]);
  if (!buf) {
    mbedtls_sha256_free(&ctx);
    fclose(f);
    ESP_LOGE(TAG, "No memory for hash buffer");
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

static bool UploadFileToMinio(const std::string& path) {
  if (!app_config.minio_enabled) {
    ESP_LOGI(TAG, "MinIO disabled, skip upload");
    return false;
  }
  if (app_config.minio_endpoint.empty() || app_config.minio_access_key.empty() || app_config.minio_secret_key.empty() ||
      app_config.minio_bucket.empty()) {
    ESP_LOGW(TAG, "MinIO config incomplete, skip upload");
    return false;
  }
  const std::string device = SanitizeId(app_config.device_id);
  const std::string filename = Basename(path);
  const std::string object_key = device + "/" + filename;
  const std::string endpoint = TrimTrailingSlash(app_config.minio_endpoint);
  const std::string host = ExtractHost(endpoint);
  const bool use_https = endpoint.rfind("https://", 0) == 0;
  if (host.empty()) {
    ESP_LOGE(TAG, "Invalid MinIO endpoint: %s", endpoint.c_str());
    return false;
  }
  const std::string url = endpoint + "/" + app_config.minio_bucket + "/" + object_key;

  std::string payload_hash;
  size_t file_size = 0;
  if (!Sha256File(path, &payload_hash, &file_size)) {
    ESP_LOGE(TAG, "Failed to hash %s", path.c_str());
    return false;
  }
  // Use device_id as bucket if not set
  if (app_config.minio_bucket.empty()) {
    app_config.minio_bucket = SanitizeId(app_config.device_id);
  }

  auto put_bucket_if_needed = [&]() {
    // Sign empty payload
    const std::string payload_hash = "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";
    time_t now = time(nullptr);
    if (now <= 0) now = static_cast<time_t>(esp_timer_get_time() / 1'000'000ULL);
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
  (void)put_bucket_if_needed();

  time_t now = time(nullptr);
  if (now <= 0) now = static_cast<time_t>(esp_timer_get_time() / 1'000'000ULL);
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
    ESP_LOGE(TAG, "Failed to hash canonical request");
    return false;
  }

  const std::string credential_scope = std::string(date_buf) + "/" + region + "/s3/aws4_request";
  const std::string string_to_sign = std::string("AWS4-HMAC-SHA256\n") + amz_date + "\n" + credential_scope + "\n" + canonical_hash_hex;

  uint8_t signing_key[32];
  if (!DeriveS3SigningKey(app_config.minio_secret_key, date_buf, region, signing_key)) {
    ESP_LOGE(TAG, "Failed to derive signing key");
    return false;
  }
  uint8_t signature_bin[32];
  if (!HmacSha256(signing_key, sizeof(signing_key),
                  reinterpret_cast<const uint8_t*>(string_to_sign.data()), string_to_sign.size(), signature_bin)) {
    ESP_LOGE(TAG, "Failed to sign string");
    return false;
  }
  const std::string signature_hex = HexEncode(signature_bin, sizeof(signature_bin));
  const std::string authorization = "AWS4-HMAC-SHA256 Credential=" + app_config.minio_access_key + "/" + credential_scope +
                                    ", SignedHeaders=" + signed_headers + ", Signature=" + signature_hex;

  esp_http_client_config_t cfg_http = {};
  cfg_http.url = url.c_str();
  cfg_http.method = HTTP_METHOD_PUT;
  cfg_http.timeout_ms = 10000;  // avoid long blocking if network is down
  cfg_http.transport_type = use_https ? HTTP_TRANSPORT_OVER_SSL : HTTP_TRANSPORT_OVER_TCP;
  cfg_http.disable_auto_redirect = true;
  // Use LetsEncrypt ISRG root
  cfg_http.cert_pem = reinterpret_cast<const char*>(isrgrootx1_pem_start);
  cfg_http.cert_len = isrgrootx1_pem_end - isrgrootx1_pem_start;

  esp_http_client_handle_t client = esp_http_client_init(&cfg_http);
  if (!client) {
    ESP_LOGE(TAG, "esp_http_client_init failed");
    return false;
  }
  char len_buf[32];
  snprintf(len_buf, sizeof(len_buf), "%u", static_cast<unsigned>(file_size));
  esp_http_client_set_header(client, "Host", host.c_str());
  esp_http_client_set_header(client, "Content-Length", len_buf);
  esp_http_client_set_header(client, "x-amz-content-sha256", payload_hash.c_str());
  esp_http_client_set_header(client, "x-amz-date", amz_date);
  esp_http_client_set_header(client, "Authorization", authorization.c_str());

  if (esp_http_client_open(client, file_size) != ESP_OK) {
    ESP_LOGE(TAG, "Failed to open HTTP connection to %s", url.c_str());
    esp_http_client_cleanup(client);
    return false;
  }

  FILE* f = fopen(path.c_str(), "rb");
  if (!f) {
    ESP_LOGE(TAG, "Cannot reopen file %s for upload", path.c_str());
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return false;
  }
  char buf[2048];
  size_t n = 0;
  bool ok = true;
  while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
    int written = esp_http_client_write(client, buf, n);
    if (written < 0) {
      ESP_LOGE(TAG, "HTTP write failed");
      ok = false;
      break;
    }
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
  if (status >= 200 && status < 300) {
    ESP_LOGI(TAG, "Uploaded %s to MinIO (%d)", path.c_str(), status);
    return true;
  }
  ESP_LOGE(TAG, "MinIO upload failed, status %d", status);
  return false;
}

static bool UploadPendingOnce() {
  std::vector<std::string> files;
  {
    SdLockGuard guard(pdMS_TO_TICKS(50));
    if (!guard.locked()) {
      ESP_LOGW(TAG, "SD mutex busy, skip upload cycle");
      return false;
    }
    if (!MountLogSd()) {
      return false;
    }
    if (!EnsureUploadDirs()) {
      return false;
    }
    DIR* dir = opendir(TO_UPLOAD_DIR);
    if (!dir) {
      ESP_LOGI(TAG, "No upload dir, nothing to sync");
      return false;
    }
    struct dirent* ent = nullptr;
    while ((ent = readdir(dir)) != nullptr) {
      if (ent->d_name[0] == '.') continue;
      if (ent->d_type != DT_REG && ent->d_type != DT_UNKNOWN) continue;
      std::string full = std::string(TO_UPLOAD_DIR) + "/" + ent->d_name;
      files.push_back(full);
    }
    closedir(dir);
  }

  int uploaded = 0;
  for (const auto& f : files) {
    if (UploadFileToMinio(f)) {
      SdLockGuard guard(pdMS_TO_TICKS(200));
      if (!guard.locked() || !MountLogSd()) {
        ESP_LOGW(TAG, "SD busy, cannot move uploaded file %s", f.c_str());
        continue;
      }
      if (MoveFileToDir(f, UPLOADED_DIR, nullptr)) {
        uploaded++;
      }
    }
  }
  if (uploaded > 0) {
    ESP_LOGI(TAG, "Uploaded %d file(s) to MinIO", uploaded);
    return true;
  }
  return false;
}

void UploadTask(void*) {
  const TickType_t interval = pdMS_TO_TICKS(60 * 60 * 1000);  // hourly
  while (true) {
    UploadPendingOnce();
    vTaskDelay(interval);
  }
}
bool FlushLogFile() {
  if (!log_file) return false;
  fflush(log_file);
  int fd = fileno(log_file);
  if (fd >= 0) {
    fsync(fd);
  }
  return true;
}

bool OpenLogFileWithPostfix(const std::string& postfix) {
  SdLockGuard guard;
  if (!guard.locked()) {
    ESP_LOGW(TAG, "SD mutex unavailable, cannot open log file");
    return false;
  }
  if (!MountLogSd()) {
    return false;
  }
  if (log_file) {
    fclose(log_file);
    log_file = nullptr;
  }

  const std::string filename = BuildLogFilename(postfix);
  std::string full_path;
  if (!SanitizeFilename(filename, &full_path)) {
    ESP_LOGW(TAG, "Bad filename for logging: %s", filename.c_str());
    return false;
  }
  log_file = fopen(full_path.c_str(), "w");
  if (!log_file) {
    ESP_LOGE(TAG, "Failed to open log file %s", full_path.c_str());
    return false;
  }

  SharedState snapshot = CopyState();
  log_config.file_start_us = esp_timer_get_time();
  fprintf(log_file, "timestamp_iso,timestamp_ms,adc1,adc2,adc3");
  for (int i = 0; i < snapshot.temp_sensor_count && i < MAX_TEMP_SENSORS; ++i) {
    const std::string& label = snapshot.temp_labels[i];
    if (!label.empty()) {
      fprintf(log_file, ",%s", label.c_str());
    } else {
      fprintf(log_file, ",temp%d", i + 1);
    }
  }
  fprintf(log_file, ",bus_v,bus_i,bus_p");
  if (log_config.use_motor) {
    fprintf(log_file, ",adc1_cal,adc2_cal,adc3_cal");
  }
  fprintf(log_file, "\n");
  FlushLogFile();

  UpdateState([&](SharedState& s) {
    s.logging = true;
    s.log_filename = filename;
  });
  current_log_path = full_path;
  return true;
}

// TinyUSB MSC descriptors (single-interface device)
constexpr uint8_t EPNUM_MSC_OUT = 0x01;
constexpr uint8_t EPNUM_MSC_IN = 0x81;
constexpr uint8_t ITF_MSC = 0;

tusb_desc_device_t kMscDeviceDescriptor = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
    .bDeviceClass = TUSB_CLASS_MISC,
    .bDeviceSubClass = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = 0x303A,  // Espressif VID
    .idProduct = 0x4002,
    .bcdDevice = 0x0100,
    .iManufacturer = 0x01,
    .iProduct = 0x02,
    .iSerialNumber = 0x03,
    .bNumConfigurations = 0x01,
};

uint8_t const kMscConfigDescriptor[] = {
    // Config number, interface count, string index, total length, attribute, power in mA
    TUD_CONFIG_DESCRIPTOR(1, 1, 0, TUD_CONFIG_DESC_LEN + TUD_MSC_DESC_LEN, TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100),
    // Interface number, string index, EP Out & EP In address, EP size
    TUD_MSC_DESCRIPTOR(ITF_MSC, 0, EPNUM_MSC_OUT, EPNUM_MSC_IN, 64),
};

const char* kMscStringDescriptor[] = {
    (const char[]) {0x09, 0x04},  // English (0x0409)
    "Espressif",                  // Manufacturer
    "SD Mass Storage",            // Product
    "123456",                     // Serial
    "MSC",                        // MSC interface
};

#if (TUD_OPT_HIGH_SPEED)
const tusb_desc_device_qualifier_t kDeviceQualifier = {
    .bLength = sizeof(tusb_desc_device_qualifier_t),
    .bDescriptorType = TUSB_DESC_DEVICE_QUALIFIER,
    .bcdUSB = 0x0200,
    .bDeviceClass = TUSB_CLASS_MISC,
    .bDeviceSubClass = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .bNumConfigurations = 0x01,
    .bReserved = 0,
};
#endif

// HTML UI (kept close to original Arduino page)
std::string Trim(const std::string& str) {
  size_t start = 0;
  while (start < str.size() && std::isspace(static_cast<unsigned char>(str[start]))) {
    ++start;
  }
  size_t end = str.size();
  while (end > start && std::isspace(static_cast<unsigned char>(str[end - 1]))) {
    --end;
  }
  return str.substr(start, end - start);
}

bool ParseBool(const std::string& value, bool* out) {
  if (!out) {
    return false;
  }
  std::string lower;
  lower.reserve(value.size());
  for (char c : value) {
    lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
  }
  if (lower == "true" || lower == "1" || lower == "yes" || lower == "on") {
    *out = true;
    return true;
  }
  if (lower == "false" || lower == "0" || lower == "no" || lower == "off") {
    *out = false;
    return true;
  }
  return false;
}

bool ParseConfigFile(FILE* file, AppConfig* config) {
  if (!file || !config) {
    return false;
  }

  char line[256];
  bool ssid_set = false;
  bool pass_set = false;
  bool usb_set = false;
  bool usb_value = false;
  bool wifi_ap_mode_set = false;
  bool wifi_ap_mode_val = false;
  bool log_active_set = false;
  bool log_active_val = false;
  bool log_postfix_set = false;
  std::string log_postfix;
  bool log_use_motor_set = false;
  bool log_use_motor_val = false;
  bool log_duration_set = false;
  float log_duration_val = log_config.duration_s;
  bool stepper_speed_set = false;
  int stepper_speed_val = config->stepper_speed_us;
  bool pid_enabled_set = false;
  bool pid_enabled_val = false;
  bool pid_kp_set = false, pid_ki_set = false, pid_kd_set = false, pid_sp_set = false, pid_sensor_set = false;
  float pid_kp = pid_config.kp;
  float pid_ki = pid_config.ki;
  float pid_kd = pid_config.kd;
  float pid_sp = pid_config.setpoint;
  int pid_sensor = pid_config.sensor_index;
  std::string ssid;
  std::string password;
  std::string device_id = config->device_id;
  bool device_id_set = false;
  std::string minio_endpoint = config->minio_endpoint;
  std::string minio_access = config->minio_access_key;
  std::string minio_secret = config->minio_secret_key;
  std::string minio_bucket = config->minio_bucket;
  bool minio_endpoint_set = false;
  bool minio_access_set = false;
  bool minio_secret_set = false;
  bool minio_bucket_set = false;
  bool minio_enabled_val = config->minio_enabled;
  bool minio_enabled_set = false;
  std::string mqtt_uri = config->mqtt_uri;
  std::string mqtt_user = config->mqtt_user;
  std::string mqtt_password = config->mqtt_password;
  bool mqtt_uri_set = false;
  bool mqtt_user_set = false;
  bool mqtt_password_set = false;
  bool mqtt_enabled_val = config->mqtt_enabled;
  bool mqtt_enabled_set = false;

  while (fgets(line, sizeof(line), file)) {
    std::string raw(line);
    std::string trimmed = Trim(raw);
    if (trimmed.empty() || trimmed[0] == '#') {
      continue;
    }

    const size_t eq = trimmed.find('=');
    if (eq == std::string::npos) {
      continue;
    }

    std::string key = Trim(trimmed.substr(0, eq));
    std::string value = Trim(trimmed.substr(eq + 1));

    if (key == "wifi_ssid") {
      if (!value.empty() && value.size() < WIFI_SSID_MAX_LEN) {
        ssid = value;
        ssid_set = true;
      } else {
        ESP_LOGW(TAG, "Invalid wifi_ssid in config.txt");
      }
    } else if (key == "wifi_password") {
      if (value.size() >= 8 && value.size() < WIFI_PASSWORD_MAX_LEN) {
        password = value;
        pass_set = true;
      } else {
        ESP_LOGW(TAG, "Invalid wifi_password in config.txt");
      }
    } else if (key == "wifi_ap_mode") {
      if (ParseBool(value, &wifi_ap_mode_val)) {
        wifi_ap_mode_set = true;
      }
    } else if (key == "usb_mass_storage") {
      if (ParseBool(value, &usb_value)) {
        usb_set = true;
      } else {
        ESP_LOGW(TAG, "Invalid usb_mass_storage value in config.txt");
      }
    } else if (key == "pid_kp") {
      pid_kp = std::strtof(value.c_str(), nullptr);
      pid_kp_set = true;
    } else if (key == "pid_ki") {
      pid_ki = std::strtof(value.c_str(), nullptr);
      pid_ki_set = true;
    } else if (key == "pid_kd") {
      pid_kd = std::strtof(value.c_str(), nullptr);
      pid_kd_set = true;
    } else if (key == "pid_setpoint") {
      pid_sp = std::strtof(value.c_str(), nullptr);
      pid_sp_set = true;
    } else if (key == "pid_sensor") {
      pid_sensor = std::atoi(value.c_str());
      pid_sensor_set = true;
    } else if (key == "pid_enabled") {
      if (ParseBool(value, &pid_enabled_val)) {
        pid_enabled_set = true;
      }
    } else if (key == "logging_active") {
      if (ParseBool(value, &log_active_val)) {
        log_active_set = true;
      }
    } else if (key == "logging_postfix") {
      log_postfix = value;
      log_postfix_set = true;
    } else if (key == "logging_use_motor") {
      if (ParseBool(value, &log_use_motor_val)) {
        log_use_motor_set = true;
      }
    } else if (key == "logging_duration_s") {
      log_duration_val = std::strtof(value.c_str(), nullptr);
      log_duration_set = (log_duration_val > 0.0f);
    } else if (key == "stepper_speed_us") {
      stepper_speed_val = std::atoi(value.c_str());
      if (stepper_speed_val > 0) {
        stepper_speed_set = true;
      } else {
        ESP_LOGW(TAG, "Invalid stepper_speed_us in config.txt");
      }
    } else if (key == "device_id") {
      if (!value.empty()) {
        device_id = value;
        device_id_set = true;
      }
    } else if (key == "minio_endpoint") {
      minio_endpoint = value;
      minio_endpoint_set = true;
    } else if (key == "minio_access_key") {
      minio_access = value;
      minio_access_set = true;
    } else if (key == "minio_secret_key") {
      minio_secret = value;
      minio_secret_set = true;
    } else if (key == "minio_bucket") {
      minio_bucket = value;
      minio_bucket_set = true;
    } else if (key == "minio_enabled") {
      if (ParseBool(value, &minio_enabled_val)) {
        minio_enabled_set = true;
      }
    } else if (key == "mqtt_uri") {
      mqtt_uri = value;
      mqtt_uri_set = true;
    } else if (key == "mqtt_user") {
      mqtt_user = value;
      mqtt_user_set = true;
    } else if (key == "mqtt_password") {
      mqtt_password = value;
      mqtt_password_set = true;
    } else if (key == "mqtt_enabled") {
      if (ParseBool(value, &mqtt_enabled_val)) {
        mqtt_enabled_set = true;
      }
    }
  }

  if (ssid_set && pass_set) {
    config->wifi_ssid = ssid;
    config->wifi_password = password;
    config->wifi_from_file = true;
  }
  if (wifi_ap_mode_set) {
    config->wifi_ap_mode = wifi_ap_mode_val;
    config->wifi_from_file = true;
  }
  if (usb_set) {
    config->usb_mass_storage = usb_value;
    config->usb_mass_storage_from_file = true;
  }
  if (log_active_set) {
    config->logging_active = log_active_val;
  }
  if (log_postfix_set) {
    config->logging_postfix = log_postfix;
  }
  if (log_use_motor_set) {
    config->logging_use_motor = log_use_motor_val;
    log_config.use_motor = log_use_motor_val;
  }
  if (log_duration_set) {
    config->logging_duration_s = log_duration_val;
    log_config.duration_s = log_duration_val;
  }
  if (stepper_speed_set) {
    config->stepper_speed_us = stepper_speed_val;
    UpdateState([&](SharedState& s) { s.stepper_speed_us = stepper_speed_val; });
  }
  if (device_id_set) {
    config->device_id = device_id;
  }
  if (minio_endpoint_set) {
    config->minio_endpoint = minio_endpoint;
  }
  if (minio_access_set) {
    config->minio_access_key = minio_access;
  }
  if (minio_secret_set) {
    config->minio_secret_key = minio_secret;
  }
  if (minio_bucket_set) {
    config->minio_bucket = minio_bucket;
  }
  if (minio_enabled_set) {
    config->minio_enabled = minio_enabled_val;
  }
  if (mqtt_uri_set) {
    config->mqtt_uri = mqtt_uri;
  }
  if (mqtt_user_set) {
    config->mqtt_user = mqtt_user;
  }
  if (mqtt_password_set) {
    config->mqtt_password = mqtt_password;
  }
  if (mqtt_enabled_set) {
    config->mqtt_enabled = mqtt_enabled_val;
  }
  if (pid_kp_set || pid_ki_set || pid_kd_set || pid_sp_set || pid_sensor_set) {
    pid_config.kp = pid_kp;
    pid_config.ki = pid_ki;
    pid_config.kd = pid_kd;
    pid_config.setpoint = pid_sp;
    pid_config.sensor_index = pid_sensor;
    pid_config.from_file = true;
  }
  if (pid_enabled_set) {
    UpdateState([&](SharedState& s) { s.pid_enabled = pid_enabled_val; });
    pid_config.from_file = true;
  }
  return config->wifi_from_file || config->usb_mass_storage_from_file || log_active_set || log_postfix_set || log_use_motor_set ||
         log_duration_set || stepper_speed_set || device_id_set || minio_endpoint_set || minio_access_set || minio_secret_set ||
         minio_bucket_set || minio_enabled_set || mqtt_uri_set || mqtt_user_set || mqtt_password_set || mqtt_enabled_set ||
         pid_config.from_file;
}

void LoadConfigFromSdCard(AppConfig* config) {
  if (!config) {
    return;
  }

  SdLockGuard guard;
  if (!guard.locked()) {
    ESP_LOGW(TAG, "SD mutex unavailable, skip config load");
    return;
  }

  sdmmc_card_t* card = nullptr;
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
  mount_config.max_files = 8;
  mount_config.allocation_unit_size = 0;

  esp_err_t ret = esp_vfs_fat_sdmmc_mount(CONFIG_MOUNT_POINT, &host, &slot_config, &mount_config, &card);
  if (ret != ESP_OK) {
    ESP_LOGW(TAG, "SD mount failed for config.txt: %s", esp_err_to_name(ret));
    return;
  }

  FILE* file = fopen(CONFIG_FILE_PATH, "r");
  if (!file) {
    ESP_LOGW(TAG, "Config file not found at %s", CONFIG_FILE_PATH);
    esp_vfs_fat_sdcard_unmount(CONFIG_MOUNT_POINT, card);
    return;
  }

  const bool parsed = ParseConfigFile(file, config);
  fclose(file);
  esp_vfs_fat_sdcard_unmount(CONFIG_MOUNT_POINT, card);

  if (parsed) {
    if (config->wifi_from_file) {
      ESP_LOGI(TAG, "Wi-Fi config loaded from config.txt (SSID: %s)", config->wifi_ssid.c_str());
    } else {
      // Keep defaults if Wi-Fi not found/invalid in file.
      config->wifi_ssid = DEFAULT_WIFI_SSID;
      config->wifi_password = DEFAULT_WIFI_PASS;
    }
    if (config->usb_mass_storage_from_file) {
      ESP_LOGI(TAG, "usb_mass_storage=%s (config.txt)", config->usb_mass_storage ? "true" : "false");
    }
  } else {
    ESP_LOGW(TAG, "config.txt present but values are missing/invalid, using defaults");
    config->wifi_ssid = DEFAULT_WIFI_SSID;
    config->wifi_password = DEFAULT_WIFI_PASS;
  }
}

void WifiEventHandler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
  if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
    esp_wifi_connect();
  } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
    if (retry_count < 5) {
      esp_wifi_connect();
      retry_count++;
      ESP_LOGW(TAG, "Retry Wi-Fi connection (%d)", retry_count);
    } else {
      xEventGroupSetBits(wifi_event_group, WIFI_FAIL_BIT);
    }
  } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
    retry_count = 0;
    auto* event = static_cast<ip_event_got_ip_t*>(event_data);
    ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
    xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
  }
}

void InitWifi(const std::string& ssid, const std::string& password, bool ap_mode) {
  if (!wifi_event_group) {
    wifi_event_group = xEventGroupCreate();
  } else {
    xEventGroupClearBits(wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);
  }

  if (!wifi_inited) {
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    wifi_netif_sta = esp_netif_create_default_wifi_sta();
    wifi_netif_ap = esp_netif_create_default_wifi_ap();
    if (wifi_netif_sta && strlen(HOSTNAME) > 0) {
      esp_netif_set_hostname(wifi_netif_sta, HOSTNAME);
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &WifiEventHandler, nullptr));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &WifiEventHandler, nullptr));
    wifi_inited = true;
  } else {
    esp_wifi_stop();
  }

  retry_count = 0;

  wifi_config_t wifi_config = {};
  std::strncpy(reinterpret_cast<char*>(wifi_config.sta.ssid), ssid.c_str(), sizeof(wifi_config.sta.ssid) - 1);
  std::strncpy(reinterpret_cast<char*>(wifi_config.sta.password), password.c_str(), sizeof(wifi_config.sta.password) - 1);
  wifi_config.sta.ssid[sizeof(wifi_config.sta.ssid) - 1] = '\0';
  wifi_config.sta.password[sizeof(wifi_config.sta.password) - 1] = '\0';
  wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
  wifi_config.sta.pmf_cfg = {.capable = true, .required = false};

  wifi_config_t ap_config = {};
  const char* ap_ssid = ap_mode ? ssid.c_str() : "esp";
  const char* ap_pass = ap_mode ? password.c_str() : "12345678";
  std::strncpy(reinterpret_cast<char*>(ap_config.ap.ssid), ap_ssid, sizeof(ap_config.ap.ssid) - 1);
  std::strncpy(reinterpret_cast<char*>(ap_config.ap.password), ap_pass, sizeof(ap_config.ap.password) - 1);
  ap_config.ap.ssid_len = strlen(reinterpret_cast<char*>(ap_config.ap.ssid));
  ap_config.ap.channel = 1;
  ap_config.ap.authmode = (strlen(ap_pass) >= 8) ? WIFI_AUTH_WPA_WPA2_PSK : WIFI_AUTH_OPEN;
  ap_config.ap.max_connection = 4;

  wifi_mode_t mode = ap_mode ? WIFI_MODE_APSTA : WIFI_MODE_STA;
  ESP_ERROR_CHECK(esp_wifi_set_mode(mode));
  if (USE_CUSTOM_MAC) {
    ESP_ERROR_CHECK(esp_wifi_set_mac(WIFI_IF_STA, CUSTOM_MAC));
  }
  ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
  if (mode == WIFI_MODE_APSTA) {
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
  }
  ESP_ERROR_CHECK(esp_wifi_start());
  ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

  EventBits_t bits = xEventGroupWaitBits(
      wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT, pdFALSE, pdFALSE, pdMS_TO_TICKS(15'000));

  if (bits & WIFI_CONNECTED_BIT) {
    ESP_LOGI(TAG, "Connected to SSID:%s", ssid.c_str());
  } else {
    ESP_LOGW(TAG, "Failed to connect to SSID:%s, starting AP fallback", ssid.c_str());
    // Fallback to AP-only with default creds
    wifi_config_t fallback_ap = {};
    std::strncpy(reinterpret_cast<char*>(fallback_ap.ap.ssid), "esp", sizeof(fallback_ap.ap.ssid) - 1);
    std::strncpy(reinterpret_cast<char*>(fallback_ap.ap.password), "12345678", sizeof(fallback_ap.ap.password) - 1);
    fallback_ap.ap.ssid_len = strlen(reinterpret_cast<char*>(fallback_ap.ap.ssid));
    fallback_ap.ap.channel = 1;
    fallback_ap.ap.authmode = WIFI_AUTH_WPA_WPA2_PSK;
    fallback_ap.ap.max_connection = 4;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &fallback_ap));
  }
}

void StartSntp() {
  esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
  esp_sntp_setservername(0, "pool.ntp.org");
  esp_sntp_init();
}

bool WaitForTimeSyncMs(int timeout_ms) {
  const int64_t deadline_us = esp_timer_get_time() + static_cast<int64_t>(timeout_ms) * 1000;
  while (esp_timer_get_time() < deadline_us) {
    time_t now = 0;
    time(&now);
    if (now > 1'700'000'000) {  // ~2023-11-14
      time_synced = true;
      return true;
    }
    if (sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED) {
      time(&now);
      if (now > 1'700'000'000) {
        time_synced = true;
        return true;
      }
    }
    vTaskDelay(pdMS_TO_TICKS(200));
  }
  return false;
}

bool EnsureTimeSynced(int timeout_ms) {
  if (time_synced) return true;
  return WaitForTimeSyncMs(timeout_ms);
}

esp_err_t InitSpiBus() {
  spi_bus_config_t buscfg = {};
  buscfg.mosi_io_num = ADC_MOSI;
  buscfg.miso_io_num = ADC_MISO;
  buscfg.sclk_io_num = ADC_SCK;
  buscfg.quadwp_io_num = -1;
  buscfg.quadhd_io_num = -1;
  buscfg.max_transfer_sz = 4;
  return spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO);
}

struct TempMeta {
  std::array<std::string, MAX_TEMP_SENSORS> labels{};
  std::array<std::string, MAX_TEMP_SENSORS> addresses{};
};

std::string FormatOneWireAddress(uint64_t address) {
  char buf[20];
  std::snprintf(buf, sizeof(buf), "0x%016llX", static_cast<unsigned long long>(address));
  return std::string(buf);
}

TempMeta BuildTempMeta(int count) {
  TempMeta meta{};
  uint64_t addrs[MAX_TEMP_SENSORS] = {};
  const int addr_count = M1820GetAddresses(addrs, MAX_TEMP_SENSORS);
  const int capped_count = std::min(count, MAX_TEMP_SENSORS);

  for (int i = 0; i < capped_count; ++i) {
    meta.labels[i] = "t" + std::to_string(i + 1);
  }
  for (int i = 0; i < std::min(addr_count, MAX_TEMP_SENSORS); ++i) {
    if (addrs[i] != 0) {
      meta.addresses[i] = FormatOneWireAddress(addrs[i]);
    }
  }
  return meta;
}

void InitGpios() {
  gpio_config_t io_conf = {};
  io_conf.intr_type = GPIO_INTR_DISABLE;
  io_conf.mode = GPIO_MODE_OUTPUT;
  io_conf.pin_bit_mask =
      (1ULL << RELAY_PIN) | (1ULL << STEPPER_EN) | (1ULL << STEPPER_DIR) | (1ULL << STEPPER_STEP) |
      (1ULL << HEATER_PWM) | (1ULL << FAN_PWM) | (1ULL << STATUS_LED_RED) | (1ULL << STATUS_LED_GREEN);
  io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
  io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
  ESP_ERROR_CHECK(gpio_config(&io_conf));

  gpio_set_level(RELAY_PIN, 0);
  gpio_set_level(STEPPER_EN, 1);   // disable motor by default
  gpio_set_level(STEPPER_DIR, 0);
  gpio_set_level(STEPPER_STEP, 0);
  gpio_set_level(HEATER_PWM, 0);
  gpio_set_level(STATUS_LED_RED, 0);
  gpio_set_level(STATUS_LED_GREEN, 0);

  // Hall sensor input
  gpio_config_t hall_conf = {};
  hall_conf.intr_type = GPIO_INTR_DISABLE;
  hall_conf.mode = GPIO_MODE_INPUT;
  hall_conf.pin_bit_mask = (1ULL << MT_HALL_SEN);
  hall_conf.pull_up_en = GPIO_PULLUP_ENABLE;
  hall_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
  ESP_ERROR_CHECK(gpio_config(&hall_conf));

  // Tachometer inputs with interrupts
  gpio_config_t tach_conf = {};
  tach_conf.intr_type = GPIO_INTR_POSEDGE;
  tach_conf.mode = GPIO_MODE_INPUT;
  tach_conf.pin_bit_mask = (1ULL << FAN1_TACH) | (1ULL << FAN2_TACH);
  tach_conf.pull_up_en = GPIO_PULLUP_ENABLE;
  tach_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
  ESP_ERROR_CHECK(gpio_config(&tach_conf));

  esp_err_t isr_err = gpio_install_isr_service(0);
  if (isr_err != ESP_ERR_INVALID_STATE) {
    ESP_ERROR_CHECK(isr_err);
  }
  ESP_ERROR_CHECK(gpio_isr_handler_add(FAN1_TACH, FanTachIsr, (void*)FAN1_TACH));
  ESP_ERROR_CHECK(gpio_isr_handler_add(FAN2_TACH, FanTachIsr, (void*)FAN2_TACH));
}

void InitHeaterPwm() {
  ledc_timer_config_t t = {};
  t.speed_mode = LEDC_LOW_SPEED_MODE;
  t.duty_resolution = LEDC_TIMER_10_BIT;
  t.timer_num = LEDC_TIMER_0;
  t.freq_hz = 1000;
  t.clk_cfg = LEDC_AUTO_CLK;
  ESP_ERROR_CHECK(ledc_timer_config(&t));

  ledc_channel_config_t c = {};
  c.gpio_num = HEATER_PWM;
  c.speed_mode = LEDC_LOW_SPEED_MODE;
  c.channel = LEDC_CHANNEL_0;
  c.timer_sel = LEDC_TIMER_0;
  c.duty = 0;
  ESP_ERROR_CHECK(ledc_channel_config(&c));
}

void HeaterSetPowerPercent(float p) {
  if (p < 0.0f) p = 0.0f;
  if (p > 100.0f) p = 100.0f;
  const uint32_t duty = static_cast<uint32_t>(p * 1023.0f / 100.0f);  // 10-bit scale
  ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty);
  ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
  UpdateState([&](SharedState& s) { s.heater_power = p; });
}

[[maybe_unused]] void HeaterOn() { HeaterSetPowerPercent(100.0f); }
[[maybe_unused]] void HeaterOff() { HeaterSetPowerPercent(0.0f); }

void FanSetPowerPercent(float p) {
  if (p < 0.0f) p = 0.0f;
  if (p > 100.0f) p = 100.0f;
  const uint32_t duty = static_cast<uint32_t>(p * 1023.0f / 100.0f);
  ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1, duty);
  ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1);
  UpdateState([&](SharedState& s) { s.fan_power = p; });
}

void InitFanPwm() {
  ledc_timer_config_t t = {};
  t.speed_mode = LEDC_LOW_SPEED_MODE;
  t.duty_resolution = LEDC_TIMER_10_BIT;
  t.timer_num = LEDC_TIMER_1;
  t.freq_hz = 25000;
  t.clk_cfg = LEDC_AUTO_CLK;
  ESP_ERROR_CHECK(ledc_timer_config(&t));

  ledc_channel_config_t c = {};
  c.gpio_num = FAN_PWM;
  c.speed_mode = LEDC_LOW_SPEED_MODE;
  c.channel = LEDC_CHANNEL_1;
  c.timer_sel = LEDC_TIMER_1;
  c.duty = 0;
  ESP_ERROR_CHECK(ledc_channel_config(&c));

  // Default to full power
  FanSetPowerPercent(100.0f);
}

esp_err_t InitIna219() {
  if (!i2c_bus) {
    i2c_master_bus_config_t bus_cfg = {};
    bus_cfg.i2c_port = I2C_NUM_0;
    bus_cfg.sda_io_num = INA_SDA;
    bus_cfg.scl_io_num = INA_SCL;
    bus_cfg.clk_source = I2C_CLK_SRC_DEFAULT;
    bus_cfg.glitch_ignore_cnt = 0;
    bus_cfg.intr_priority = 0;
    bus_cfg.trans_queue_depth = 0;
    bus_cfg.flags.enable_internal_pullup = true;
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_cfg, &i2c_bus), TAG, "I2C bus init failed");
  }

  if (!ina219_dev) {
    i2c_device_config_t dev_cfg = {};
    dev_cfg.device_address = INA219_ADDR;
    dev_cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dev_cfg.scl_speed_hz = 400000;
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(i2c_bus, &dev_cfg, &ina219_dev), TAG, "INA219 attach failed");
  }

  auto write_reg = [](uint8_t reg, uint16_t value) -> esp_err_t {
    uint8_t payload[3] = {
        reg,
        static_cast<uint8_t>((value >> 8) & 0xFF),
        static_cast<uint8_t>(value & 0xFF),
    };
    return i2c_master_transmit(ina219_dev, payload, sizeof(payload), pdMS_TO_TICKS(100));
  };

  ESP_RETURN_ON_ERROR(write_reg(0x00, INA219_CONFIG), TAG, "INA219 config failed");
  ESP_RETURN_ON_ERROR(write_reg(0x05, INA219_CALIBRATION), TAG, "INA219 calibration failed");
  ESP_LOGI(TAG, "INA219 initialized");
  return ESP_OK;
}

esp_err_t ReadIna219() {
  auto read_reg = [](uint8_t reg, uint16_t* value) -> esp_err_t {
    if (!value) {
      return ESP_ERR_INVALID_ARG;
    }
    uint8_t reg_addr = reg;
    uint8_t rx[2] = {};
    esp_err_t res =
        i2c_master_transmit_receive(ina219_dev, &reg_addr, sizeof(reg_addr), rx, sizeof(rx), pdMS_TO_TICKS(100));
    if (res == ESP_OK) {
      *value = static_cast<uint16_t>(static_cast<uint16_t>(rx[0]) << 8 | static_cast<uint16_t>(rx[1]));
    }
    return res;
  };

  uint16_t bus_raw = 0;
  uint16_t current_raw = 0;
  uint16_t power_raw = 0;

  ESP_RETURN_ON_ERROR(read_reg(0x02, &bus_raw), TAG, "INA219 read bus failed");
  ESP_RETURN_ON_ERROR(read_reg(0x04, &current_raw), TAG, "INA219 read current failed");
  ESP_RETURN_ON_ERROR(read_reg(0x03, &power_raw), TAG, "INA219 read power failed");

  // Bus voltage: bits 3..15, LSB = 4 mV
  const float bus_v = static_cast<float>((bus_raw >> 3) & 0x1FFF) * INA219_BUS_LSB;
  const float current_a = static_cast<int16_t>(current_raw) * INA219_CURRENT_LSB;
  const float power_w = static_cast<uint16_t>(power_raw) * INA219_POWER_LSB;

  UpdateState([&](SharedState& s) {
    s.ina_bus_voltage = bus_v;
    s.ina_current = current_a;
    s.ina_power = power_w;
  });
  return ESP_OK;
}

esp_err_t InitSdCardForMsc(sdmmc_card_t** out_card) {
  bool host_init = false;
  sdmmc_card_t* card = static_cast<sdmmc_card_t*>(malloc(sizeof(sdmmc_card_t)));
  ESP_RETURN_ON_FALSE(card, ESP_ERR_NO_MEM, TAG, "No mem for sdmmc_card_t");

  sdmmc_host_t host = SDMMC_HOST_DEFAULT();
  host.flags |= SDMMC_HOST_FLAG_1BIT;

  sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
  slot_config.width = 1;
  slot_config.clk = SD_CLK;
  slot_config.cmd = SD_CMD;
  slot_config.d0 = SD_D0;
  slot_config.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

  ESP_RETURN_ON_ERROR((*host.init)(), TAG, "Host init failed");
  host_init = true;

  esp_err_t err = sdmmc_host_init_slot(host.slot, &slot_config);
  if (err != ESP_OK) {
    if (host_init) {
      if (host.flags & SDMMC_HOST_FLAG_DEINIT_ARG) {
        host.deinit_p(host.slot);
      } else {
        (*host.deinit)();
      }
    }
    free(card);
    return err;
  }

  // Retry until card present
  while (sdmmc_card_init(&host, card) != ESP_OK) {
    ESP_LOGW(TAG, "Insert SD card");
    vTaskDelay(pdMS_TO_TICKS(1000));
  }

  sdmmc_card_print_info(stdout, card);
  *out_card = card;
  return ESP_OK;
}

esp_err_t StartUsbMsc(sdmmc_card_t* card) {
  tinyusb_msc_sdmmc_config_t config_sdmmc = {
      .card = card,
      .callback_mount_changed = nullptr,
      .callback_premount_changed = nullptr,
      .mount_config =
          {
              .format_if_mount_failed = false,
              .max_files = 4,
              .allocation_unit_size = 0,
              .disk_status_check_enable = false,
              .use_one_fat = false,
          },
  };
  ESP_RETURN_ON_ERROR(tinyusb_msc_storage_init_sdmmc(&config_sdmmc), TAG, "Init MSC storage failed");
  ESP_RETURN_ON_ERROR(tinyusb_msc_storage_mount(MSC_BASE_PATH), TAG, "Mount MSC storage failed");

  tinyusb_config_t tusb_cfg = {};
  tusb_cfg.device_descriptor = &kMscDeviceDescriptor;
  tusb_cfg.string_descriptor = kMscStringDescriptor;
  tusb_cfg.string_descriptor_count = sizeof(kMscStringDescriptor) / sizeof(kMscStringDescriptor[0]);
  tusb_cfg.external_phy = false;
#if (TUD_OPT_HIGH_SPEED)
  tusb_cfg.fs_configuration_descriptor = kMscConfigDescriptor;
  tusb_cfg.hs_configuration_descriptor = kMscConfigDescriptor;
  tusb_cfg.qualifier_descriptor = &kDeviceQualifier;
#else
  tusb_cfg.configuration_descriptor = kMscConfigDescriptor;
#endif
  tusb_cfg.self_powered = false;
  tusb_cfg.vbus_monitor_io = -1;
  ESP_RETURN_ON_ERROR(tinyusb_driver_install(&tusb_cfg), TAG, "TinyUSB install failed");
  ESP_LOGI(TAG, "USB in MSC mode");
  return ESP_OK;
}

void EnableStepper() {
  gpio_set_level(STEPPER_EN, 0);
  UpdateState([](SharedState& s) {
    s.stepper_enabled = true;
  });
}

void DisableStepper() {
  gpio_set_level(STEPPER_EN, 1);
  UpdateState([](SharedState& s) {
    s.stepper_enabled = false;
    s.stepper_moving = false;
  });
}

void StopStepper() {
  UpdateState([](SharedState& s) {
    s.stepper_moving = false;
    s.stepper_abort = true;
    s.homing = false;
  });
}

void StartStepperMove(int steps, bool forward, int speed_us) {
  const int clamped_speed = std::max(speed_us, 1);
  UpdateState([=](SharedState& s) {
    if (!s.stepper_enabled) {
      return;
    }
    s.stepper_abort = false;
    s.stepper_direction_forward = forward;
    s.stepper_speed_us = clamped_speed;
    s.stepper_target = s.stepper_position + (forward ? steps : -steps);
    s.stepper_moving = true;
    s.last_step_timestamp_us = esp_timer_get_time();
  });
  gpio_set_level(STEPPER_DIR, forward ? 1 : 0);
}

void StepperTask(void*) {
  const TickType_t idle_delay_active = pdMS_TO_TICKS(1);
  const TickType_t idle_delay_idle = pdMS_TO_TICKS(5);
  while (true) {
    SharedState snapshot = CopyState();
    if (snapshot.homing && !snapshot.stepper_abort) {
      vTaskDelay(idle_delay_idle);
      continue;
    }
    if (snapshot.stepper_enabled && snapshot.stepper_moving && !snapshot.stepper_abort) {
      // Reassert direction each loop to avoid spurious flips from noise
      gpio_set_level(STEPPER_DIR, snapshot.stepper_direction_forward ? 1 : 0);

      const int64_t now = esp_timer_get_time();
      if (now - snapshot.last_step_timestamp_us >= snapshot.stepper_speed_us) {
        gpio_set_level(STEPPER_STEP, 1);
        esp_rom_delay_us(4);
        gpio_set_level(STEPPER_STEP, 0);

        UpdateState([&](SharedState& s) {
          if (!s.stepper_moving || !s.stepper_enabled) {
            return;
          }
          s.stepper_position += s.stepper_direction_forward ? 1 : -1;
          s.last_step_timestamp_us = now;
          if ((s.stepper_direction_forward && s.stepper_position >= s.stepper_target) ||
              (!s.stepper_direction_forward && s.stepper_position <= s.stepper_target)) {
            s.stepper_moving = false;
          }
        });
        // Yield after step to let lower-priority/idle run
        vTaskDelay(idle_delay_active);
      }
    } else if (snapshot.stepper_abort) {
      UpdateState([](SharedState& s) {
        s.stepper_moving = false;
      });
    }
    vTaskDelay(snapshot.stepper_moving ? idle_delay_active : idle_delay_idle);
  }
}

esp_err_t ReadAllAdc(float* v1, float* v2, float* v3) {
  int32_t raw1 = 0, raw2 = 0, raw3 = 0;
  esp_err_t err = adc1.Read(&raw1);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "ADC1 read failed");
    StartErrorBlink();
    return err;
  }
  err = adc2.Read(&raw2);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "ADC2 read failed");
    StartErrorBlink();
    return err;
  }
  err = adc3.Read(&raw3);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "ADC3 read failed");
    StartErrorBlink();
    return err;
  }

  *v1 = static_cast<float>(raw1) * ADC_SCALE;
  *v2 = static_cast<float>(raw2) * ADC_SCALE;
  *v3 = static_cast<float>(raw3) * ADC_SCALE;
  return ESP_OK;
}

void AdcTask(void*) {
  while (true) {
    float v1 = 0.0f, v2 = 0.0f, v3 = 0.0f;
    if (ReadAllAdc(&v1, &v2, &v3) == ESP_OK) {
      const uint64_t now_ms = esp_timer_get_time() / 1000ULL;
      const float raw1 = v1;
      const float raw2 = v2;
      const float raw3 = v3;
      UpdateState([&](SharedState& s) {
        s.voltage1 = raw1 - s.offset1;
        s.voltage2 = raw2 - s.offset2;
        s.voltage3 = raw3 - s.offset3;
        s.voltage1_cal = raw1;
        s.voltage2_cal = raw2;
        s.voltage3_cal = raw3;
        s.last_update_ms = now_ms;
      });
      ESP_LOGD(TAG, "ADC: %.6f %.6f %.6f", v1, v2, v3);
    }
    vTaskDelay(pdMS_TO_TICKS(200));
  }
}

void Ina219Task(void*) {
  while (true) {
    esp_err_t err = ReadIna219();
    if (err != ESP_OK) {
      ESP_LOGW(TAG, "INA219 read failed: %s", esp_err_to_name(err));
    }
    vTaskDelay(pdMS_TO_TICKS(1000));
  }
}

void FanTachTask(void*) {
  const uint32_t pulses_per_rev = 2;
  while (true) {
    uint32_t c1 = fan1_pulse_count;
    uint32_t c2 = fan2_pulse_count;
    fan1_pulse_count = 0;
    fan2_pulse_count = 0;

    const uint32_t rpm1 = (c1 * 60U) / pulses_per_rev;
    const uint32_t rpm2 = (c2 * 60U) / pulses_per_rev;

    UpdateState([&](SharedState& s) {
      s.fan1_rpm = rpm1;
      s.fan2_rpm = rpm2;
    });

    vTaskDelay(pdMS_TO_TICKS(1000));
  }
}
void TempTask(void*) {
  std::array<float, MAX_TEMP_SENSORS> temps{};
  while (true) {
    int count = 0;
    if (M1820ReadTemperatures(temps.data(), MAX_TEMP_SENSORS, &count)) {
      const auto meta = BuildTempMeta(count);
      UpdateState([&](SharedState& s) {
        s.temp_sensor_count = count;
        s.temps_c = temps;
        s.temp_labels = meta.labels;
        s.temp_addresses = meta.addresses;
        if (count > 0 && (s.pid_sensor_index >= count || s.pid_sensor_index < 0)) {
          s.pid_sensor_index = 0;
        }
      });
      if (count > 0) {
        ESP_LOGI(TAG, "Temps (%d):", count);
        for (int i = 0; i < count; ++i) {
          ESP_LOGI(TAG, "  Sensor %d: %.2f C", i + 1, temps[i]);
        }
      }
    } else {
      ESP_LOGW(TAG, "M1820ReadTemperatures failed");
      StartErrorBlink();
    }
    vTaskDelay(pdMS_TO_TICKS(2000));
  }
}

void PidTask(void*) {
  float integral = 0.0f;
  float prev_error = 0.0f;
  const float dt = 1.0f;
  // Auto-enable PID if it was enabled in config
  SharedState initial = CopyState();
  if (pid_config.from_file && initial.pid_enabled) {
    UpdateState([](SharedState& s) { s.pid_enabled = true; });
  }
  while (true) {
    SharedState snapshot = CopyState();
    if (!snapshot.pid_enabled) {
      integral = 0.0f;
      prev_error = 0.0f;
      vTaskDelay(pdMS_TO_TICKS(1000));
      continue;
    }
    if (snapshot.pid_sensor_index < 0 || snapshot.pid_sensor_index >= snapshot.temp_sensor_count) {
      vTaskDelay(pdMS_TO_TICKS(1000));
      continue;
    }
    float temp = snapshot.temps_c[snapshot.pid_sensor_index];
    if (!std::isfinite(temp)) {
      vTaskDelay(pdMS_TO_TICKS(1000));
      continue;
    }
    float error = snapshot.pid_setpoint - temp;
    integral += error * dt;
    integral = std::clamp(integral, -200.0f, 200.0f);
    float derivative = (error - prev_error) / dt;
    float output = snapshot.pid_kp * error + snapshot.pid_ki * integral + snapshot.pid_kd * derivative;
    output = std::clamp(output, 0.0f, 100.0f);
    HeaterSetPowerPercent(output);
    UpdateState([&](SharedState& s) {
      s.pid_output = output;
    });
    prev_error = error;
    vTaskDelay(pdMS_TO_TICKS(1000));
  }
}

void CalibrateZero() {
  bool already_running = false;
  UpdateState([&](SharedState& s) {
    already_running = s.calibrating;
    if (!s.calibrating) {
      s.calibrating = true;
    }
  });
  if (already_running) {
    ESP_LOGW(TAG, "Calibration already in progress");
    return;
  }

  gpio_set_level(RELAY_PIN, 1);
  vTaskDelay(pdMS_TO_TICKS(1000));

  const int samples = 100;
  const int ignore_samples = 10;
  float sum1 = 0.0f, sum2 = 0.0f, sum3 = 0.0f;
  int valid = 0;

  for (int i = 0; i < samples; ++i) {
    float v1 = 0.0f, v2 = 0.0f, v3 = 0.0f;
    if (ReadAllAdc(&v1, &v2, &v3) == ESP_OK && i >= ignore_samples) {
      sum1 += v1;
      sum2 += v2;
      sum3 += v3;
      valid++;
    }
    vTaskDelay(pdMS_TO_TICKS(200));
  }

  if (valid > 0) {
    const float new_offset1 = sum1 / valid;
    const float new_offset2 = sum2 / valid;
    const float new_offset3 = sum3 / valid;
    UpdateState([&](SharedState& s) {
      s.offset1 = new_offset1;
      s.offset2 = new_offset2;
      s.offset3 = new_offset3;
      s.calibrating = false;
    });
    ESP_LOGI(TAG, "Calibration done: offsets %.6f, %.6f, %.6f", new_offset1, new_offset2, new_offset3);
  } else {
    UpdateState([](SharedState& s) {
      s.calibrating = false;
    });
    ESP_LOGW(TAG, "Calibration collected no samples");
  }

  gpio_set_level(RELAY_PIN, 0);
}

void CalibrationTask(void*) {
  CalibrateZero();
  calibration_task = nullptr;
  vTaskDelete(nullptr);
}

extern "C" void app_main(void) {
  bool init_ok = true;
  bool msc_ok = true;
  esp_err_t ret = nvs_flash_init();
  if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    ESP_ERROR_CHECK(nvs_flash_init());
  } else {
    ESP_ERROR_CHECK(ret);
  }

  // Optionally disable task watchdog to avoid false positives from tight stepper loop
  esp_task_wdt_deinit();

  state_mutex = xSemaphoreCreateMutex();
  sd_mutex = xSemaphoreCreateMutex();

  LoadConfigFromSdCard(&app_config);

  bool usb_mode_found = false;
  usb_mode = LoadUsbModeFromNvs(&usb_mode_found);
  if (app_config.usb_mass_storage_from_file && !usb_mode_found) {
    usb_mode = app_config.usb_mass_storage ? UsbMode::kMsc : UsbMode::kCdc;
    ESP_LOGI(TAG, "USB mode set from config.txt (no NVS value): %s", usb_mode == UsbMode::kMsc ? "MSC" : "CDC");
    SaveUsbModeToNvs(usb_mode);
  }
  UpdateState([&](SharedState& s) { s.usb_msc_mode = (usb_mode == UsbMode::kMsc); });

  if (usb_mode == UsbMode::kMsc) {
    esp_err_t msc_err = InitSdCardForMsc(&sd_card);
    if (msc_err == ESP_OK) {
      msc_err = StartUsbMsc(sd_card);
  }
  if (msc_err != ESP_OK) {
    StartErrorBlink();
    msc_ok = false;
    ESP_LOGE(TAG, "USB MSC init failed, fallback to CDC mode: %s", esp_err_to_name(msc_err));
    usb_mode = UsbMode::kCdc;
    UpdateState([&](SharedState& s) {
      s.usb_msc_mode = false;
        s.usb_error = "MSC init failed: " + std::string(esp_err_to_name(msc_err));
      });
      SaveUsbModeToNvs(usb_mode);
    }
  } else {
    UpdateState([](SharedState& s) { s.usb_error.clear(); });
  }

  InitGpios();
  InitHeaterPwm();
  InitFanPwm();
  bool temp_ok = M1820Init(TEMP_1WIRE);
  if (!temp_ok) {
    ESP_LOGW(TAG, "M1820 init failed or no sensors found");
    StartErrorBlink();
    init_ok = false;
  }
  ESP_ERROR_CHECK(InitSpiBus());
  ESP_ERROR_CHECK(adc1.Init(SPI2_HOST, ADC_SPI_FREQ_HZ));
  ESP_ERROR_CHECK(adc2.Init(SPI2_HOST, ADC_SPI_FREQ_HZ));
  ESP_ERROR_CHECK(adc3.Init(SPI2_HOST, ADC_SPI_FREQ_HZ));
  esp_err_t ina_err = InitIna219();
  if (ina_err != ESP_OK) {
    ESP_LOGE(TAG, "INA219 init failed: %s", esp_err_to_name(ina_err));
    StartErrorBlink();
    init_ok = false;
  }

  if (!app_config.wifi_from_file) {
    ESP_LOGI(TAG, "Using default Wi-Fi config (SSID: %s)", app_config.wifi_ssid.c_str());
  }
  UpdateState([](SharedState& s) {
    s.pid_kp = pid_config.kp;
    s.pid_ki = pid_config.ki;
    s.pid_kd = pid_config.kd;
    s.pid_setpoint = pid_config.setpoint;
    s.pid_sensor_index = pid_config.sensor_index;
    s.stepper_speed_us = app_config.stepper_speed_us;
  });
  InitWifi(app_config.wifi_ssid, app_config.wifi_password, app_config.wifi_ap_mode);
  StartSntp();
  if (WaitForTimeSyncMs(8000)) {
    ESP_LOGI(TAG, "Time synced via NTP");
  } else {
    ESP_LOGW(TAG, "NTP sync timed out, using monotonic timestamp fallback");
  }
  StartHttpServer();

  if (app_config.logging_active && usb_mode == UsbMode::kCdc) {
    const std::string postfix = SanitizePostfix(app_config.logging_postfix);
    log_config.postfix = postfix;
    log_config.use_motor = app_config.logging_use_motor;
    log_config.duration_s = app_config.logging_duration_s;
    if (!StartLoggingToFile(postfix, usb_mode)) {
      ESP_LOGW(TAG, "Auto-start logging failed");
      app_config.logging_active = false;
    } else {
      app_config.logging_active = true;
      app_config.logging_postfix = postfix;
      app_config.logging_use_motor = log_config.use_motor;
      app_config.logging_duration_s = log_config.duration_s;
      UpdateState([&](SharedState& s) {
        s.logging = true;
        s.log_filename = postfix;
        s.log_use_motor = log_config.use_motor;
        s.log_duration_s = log_config.duration_s;
      });
    }
  }

  // Pin tasks to different cores: ADC на core0 (prio 4), шаги на core1 (prio 3) — idle0 свободен для WDT
  xTaskCreatePinnedToCore(&AdcTask, "adc_task", 4096, nullptr, 4, nullptr, 0);
  xTaskCreatePinnedToCore(&StepperTask, "stepper_task", 4096, nullptr, 3, nullptr, 1);
  if (ina_err == ESP_OK) {
    xTaskCreatePinnedToCore(&Ina219Task, "ina219_task", 3072, nullptr, 2, nullptr, 0);
  }
  xTaskCreatePinnedToCore(&FanTachTask, "fan_tach_task", 2048, nullptr, 2, nullptr, 0);
  if (temp_ok) {
    xTaskCreatePinnedToCore(&TempTask, "temp_task", 3072, nullptr, 2, nullptr, 0);
  }
  xTaskCreatePinnedToCore(&PidTask, "pid_task", 4096, nullptr, 2, nullptr, 0);
  if (upload_task == nullptr) {
    // Upload task uses esp_http_client and std::string, needs a bit more stack to avoid overflow.
    xTaskCreatePinnedToCore(&UploadTask, "upload_task", 12288, nullptr, 1, &upload_task, 0);
  }

  init_ok = init_ok && msc_ok && (ina_err == ESP_OK);
  SetStatusLeds(init_ok);
  ESP_LOGI(TAG, "System ready");

  // Start MQTT after init (non-blocking)
  StartMqttBridge();
}
