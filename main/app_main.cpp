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
#include <sys/time.h>
#include <cerrno>
#include <dirent.h>
#include <memory>
#include "isrgrootx1.pem.h"

#include "cJSON.h"
#include "app_state.h"
#include "app_services.h"
#include "app_utils.h"
#include "web_ui.h"
#include "http_handlers.h"
#include "driver/gpio.h"
#include "driver/spi_common.h"
#include "driver/spi_master.h"
#include "driver/ledc.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_eth.h"
#include "esp_eth_mac.h"
#include "esp_eth_netif_glue.h"
#include "esp_eth_phy.h"
#include "esp_http_server.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_mac.h"
#include "esp_netif.h"
#if CONFIG_SPIRAM
#include "esp_psram.h"
#endif
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
#include "error_manager.h"
#include "sd_maintenance.h"
#include "gps_unicore.h"
#include "nvs.h"
#include "esp_partition.h"
#include "sdmmc_cmd.h"
#include "esp_vfs_fat.h"
#include "tinyusb.h"
#include "tusb.h"
#include "class/msc/msc.h"
#include "tusb_msc_storage.h"
#include "esp_rom_sys.h"
#include "onewire_m1820.h"
#include "sdkconfig.h"
#include <dirent.h>
#include <sys/stat.h>

#include "hw_pins.h"
#include "config_loader.h"
#include "network_manager.h"
#include "upload_pipeline.h"

static constexpr char kTag[] = "APP";

#if 0  // legacy addresses kept for reference
[[maybe_unused]] constexpr uint64_t TEMP_ADDR_RAD = 0x77062223A096AD28ULL;
[[maybe_unused]] constexpr uint64_t TEMP_ADDR_LOAD = 0xE80000105FF4E228ULL;
#endif

constexpr float VREF = 4.096f;  // ±Vref/2 range, matches original sketch
constexpr float ADC_SCALE = (VREF / 2.0f) / static_cast<float>(1 << 23);

constexpr char MSC_BASE_PATH[] = "/usb_msc";
// INA219 constants (32V range, hardware shunt is 0.05 ohm).
constexpr uint8_t INA219_ADDR = 0x40;
constexpr uint16_t INA219_CONFIG = 0x399F;           // 32V range, gain /8 (320mV), 12-bit, continuous
constexpr uint16_t INA219_CALIBRATION = 4096;        // Register calibration kept from 0.1R setup.
constexpr float INA219_CURRENT_LSB = 0.0002f;        // 200 uA; 0.05R shunt doubles physical current
constexpr float INA219_POWER_LSB = INA219_CURRENT_LSB * 20.0f;
constexpr float INA219_BUS_LSB = 0.004f;             // 4 mV
constexpr int INA219_I2C_FREQ_HZ = 100000;
constexpr TickType_t INA219_I2C_TIMEOUT = pdMS_TO_TICKS(250);
static bool shared_spi_bus_inited = false;
static GpsUnicoreClient gps_client;
static TaskHandle_t gps_log_task = nullptr;
static TaskHandle_t external_power_cycle_task = nullptr;
constexpr uint32_t kExternalPowerCycleStackBytes = 6144;
static uint64_t gps_log_file_start_us = 0;
static constexpr uint64_t kGnssLogRotateUs = 3'600'000'000ULL;
static std::string current_gnss_log_path;
static volatile bool gps_reconfigure_requested = false;

volatile uint32_t fan1_pulse_count = 0;
volatile uint32_t fan2_pulse_count = 0;

i2c_master_bus_handle_t i2c_bus = nullptr;
i2c_master_dev_handle_t ina219_dev = nullptr;

static uint64_t GpioMask(gpio_num_t pin) {
  if (pin == GPIO_NUM_NC) {
    return 0;
  }
  return 1ULL << static_cast<uint32_t>(pin);
}

static void IRAM_ATTR FanTachIsr(void* arg) {
  uint32_t gpio = (uint32_t)arg;
  if (gpio == static_cast<uint32_t>(FAN1_TACH)) {
    __atomic_fetch_add(&fan1_pulse_count, 1, __ATOMIC_RELAXED);
  } else if (gpio == static_cast<uint32_t>(FAN2_TACH)) {
    __atomic_fetch_add(&fan2_pulse_count, 1, __ATOMIC_RELAXED);
  }
}

static void EnsureGpioIsrServiceInstalled() {
  static bool installed = false;
  if (installed) {
    return;
  }
  esp_err_t err = gpio_install_isr_service(0);
  if (err == ESP_OK || err == ESP_ERR_INVALID_STATE) {
    installed = true;
    return;
  }
  ESP_LOGE(kTag, "gpio_install_isr_service failed: %s", esp_err_to_name(err));
  ESP_ERROR_CHECK(err);
}

bool IsHallTriggered() {
  return gpio_get_level(MT_HALL_SEN) == app_config.motor_hall_active_level;
}

void SetExternalPower(bool enabled) {
  gpio_set_level(EXT_PWR_ON, enabled ? 1 : 0);
  UpdateState([&](SharedState& s) { s.external_power_on = enabled; });
  ESP_LOGI(kTag, "External module power %s", enabled ? "ON" : "OFF");
}

static void ExternalPowerCycleTask(void* arg) {
  const uint32_t delay_ms = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(arg));
  SetExternalPower(false);
  vTaskDelay(pdMS_TO_TICKS(delay_ms));
  SetExternalPower(true);
  external_power_cycle_task = nullptr;
  vTaskDelete(nullptr);
}

bool CycleExternalPower(uint32_t off_ms) {
  if (external_power_cycle_task != nullptr) {
    return false;
  }
  off_ms = std::clamp<uint32_t>(off_ms, 100, 30000);
  BaseType_t ok = xTaskCreate(&ExternalPowerCycleTask, "ext_pwr_cycle", kExternalPowerCycleStackBytes,
                              reinterpret_cast<void*>(static_cast<uintptr_t>(off_ms)), 4,
                              &external_power_cycle_task);
  return ok == pdPASS;
}

int GetGpsAntennaShortRaw() {
  if (GPS_ANT_SHORT == GPIO_NUM_NC) {
    return -1;
  }
  return gpio_get_level(GPS_ANT_SHORT);
}

bool IsGpsAntennaShort() {
  return GetGpsAntennaShortRaw() == 0;
}

esp_err_t InitSpiBus();

std::string BuildLogFilename(const std::string& postfix_raw) {
  const std::string postfix = SanitizePostfix(postfix_raw);
  const uint32_t boot = GetBootId();

  EnsureTimeSynced(1000);
  const UtcTimeSnapshot now_snapshot = GetBestUtcTimeForData();
  time_t now = now_snapshot.unix_time;
  struct tm tm_info;
  gmtime_r(&now, &tm_info);
  char ts[32];
  strftime(ts, sizeof(ts), "%Y%m%d_%H%M%S", &tm_info);

  std::string name = "data_";
  name += ts;
  if (!postfix.empty()) {
    const size_t base_len = name.size() + 1 + 1 + 10 + 4;  // "_" + "_" + boot + ".txt"
    size_t max_postfix = 0;
    if (base_len < 255) {
      max_postfix = 255 - base_len;
    }
    if (max_postfix > 0) {
      name += "_";
      name += postfix.substr(0, max_postfix);
    }
  }
  name += "_";
  name += std::to_string(boot);
  name += ".txt";
  return name;
}


std::string GetGpsCurrentMode() {
  std::string mode;
  return gps_client.getCurrentMode(mode) ? mode : "";
}

bool GetGpsCurrentModeText(char* out, size_t out_len) {
  return gps_client.getCurrentMode(out, out_len);
}

static bool CopyGpsPositionSnapshot(const GpsPosition& pos, int64_t received_us, GpsPositionSnapshot* out) {
  if (!out || !pos.valid || received_us <= 0) {
    return false;
  }
  out->latitude_deg = pos.latitude_deg;
  out->longitude_deg = pos.longitude_deg;
  out->altitude_m = pos.altitude_m;
  out->fix_quality = pos.fix_quality;
  out->satellites = pos.satellites;
  out->age_ms = std::max<int64_t>((esp_timer_get_time() - received_us) / 1000, 0);
  out->valid = true;
  return true;
}

bool RequestGpsPositionOnce(int timeout_ms, GpsPositionSnapshot* out) {
  if (out) {
    *out = GpsPositionSnapshot{};
  }
  GpsPosition previous{};
  int64_t previous_received_us = 0;
  (void)gps_client.getLastPosition(previous, &previous_received_us);

  gps_client.sendCommand("GPGGA COM2");
  const int64_t deadline_us = esp_timer_get_time() + static_cast<int64_t>(std::max(timeout_ms, 0)) * 1000;
  while (esp_timer_get_time() < deadline_us) {
    GpsPosition pos{};
    int64_t received_us = 0;
    if (gps_client.getLastPosition(pos, &received_us) && received_us > previous_received_us) {
      return CopyGpsPositionSnapshot(pos, received_us, out);
    }
    vTaskDelay(pdMS_TO_TICKS(50));
  }

  GpsPosition cached{};
  int64_t cached_received_us = 0;
  if (gps_client.getLastPosition(cached, &cached_received_us)) {
    return CopyGpsPositionSnapshot(cached, cached_received_us, out);
  }
  return false;
}

void RequestGpsReconfigure() {
  gps_reconfigure_requested = true;
}

void ProbeGpsMode() {
  gps_client.sendCommand("MODE");
}

static esp_err_t StartGpsClient() {
  esp_err_t gps_err = gps_client.initUart();
  if (gps_err == ESP_OK) {
    gps_err = gps_client.startTasks();
  }
  if (gps_err != ESP_OK) {
    ESP_LOGE(kTag, "GPS init/start failed: %s", esp_err_to_name(gps_err));
  }
  return gps_err;
}

namespace {

constexpr time_t kValidUtcThreshold = 1'700'000'000;  // ~2023-11-14

int64_t DaysFromCivil(int year, unsigned month, unsigned day) {
  year -= month <= 2;
  const int era = (year >= 0 ? year : year - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(year - era * 400);
  const unsigned doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return static_cast<int64_t>(era) * 146097 + static_cast<int64_t>(doe) - 719468;
}

bool GpsDateTimeToUnix(const GpsDateTime& dt, time_t* out) {
  if (!out || !dt.valid) return false;
  if (dt.year < 2020 || dt.month < 1 || dt.month > 12 || dt.day < 1 || dt.day > 31 ||
      dt.hour > 23 || dt.minute > 59 || dt.second > 60) {
    return false;
  }
  const int64_t days = DaysFromCivil(dt.year, dt.month, dt.day);
  const int64_t seconds = days * 86400LL + static_cast<int64_t>(dt.hour) * 3600LL +
                          static_cast<int64_t>(dt.minute) * 60LL + dt.second;
  if (seconds <= kValidUtcThreshold) return false;
  *out = static_cast<time_t>(seconds);
  return true;
}

bool SystemUtcNow(time_t* out) {
  if (!out) return false;
  time_t now = time(nullptr);
  if (now <= kValidUtcThreshold) return false;
  *out = now;
  return true;
}


bool GpsUtcNow(UtcTimeSnapshot* out) {
  if (!out) return false;
  GpsDateTime dt{};
  int64_t received_us = 0;
  if (!gps_client.getLastDateTime(dt, &received_us)) {
    return false;
  }
  time_t gps_unix = 0;
  if (!GpsDateTimeToUnix(dt, &gps_unix)) {
    return false;
  }
  int64_t elapsed_us = received_us > 0 ? esp_timer_get_time() - received_us : 0;
  if (elapsed_us < 0) elapsed_us = 0;
  const time_t add_s = static_cast<time_t>(elapsed_us / 1'000'000LL);
  const uint16_t add_ms = static_cast<uint16_t>((elapsed_us % 1'000'000LL) / 1000LL);
  uint32_t ms = static_cast<uint32_t>(dt.millisecond) + add_ms;
  out->unix_time = gps_unix + add_s + static_cast<time_t>(ms / 1000U);
  out->millisecond = static_cast<uint16_t>(ms % 1000U);
  out->source = UtcTimeSource::kGps;
  out->valid = true;
  return true;
}

void FormatUtcIsoToBuffer(const UtcTimeSnapshot& snapshot, char* out, size_t out_len) {
  if (!out || out_len == 0) {
    return;
  }
  time_t now = snapshot.unix_time;
  if (now <= 0) {
    now = static_cast<time_t>(0);
  }
  struct tm tm_utc {};
  gmtime_r(&now, &tm_utc);
  strftime(out, out_len, "%Y-%m-%dT%H:%M:%SZ", &tm_utc);
}

void MaybeDisciplineSystemTimeFromGps(const UtcTimeSnapshot& gps_time);

bool RequestGpsUtcTimeOnce(int timeout_ms, UtcTimeSnapshot* out) {
  int64_t previous_received_us = 0;
  GpsDateTime previous_dt{};
  (void)gps_client.getLastDateTime(previous_dt, &previous_received_us);

  gps_client.sendCommand("GPZDA COM2");
  const int64_t deadline_us = esp_timer_get_time() + static_cast<int64_t>(timeout_ms) * 1000;
  while (esp_timer_get_time() < deadline_us) {
    GpsDateTime dt{};
    int64_t received_us = 0;
    if (gps_client.getLastDateTime(dt, &received_us) && received_us > previous_received_us) {
      UtcTimeSnapshot gps_time{};
      if (GpsUtcNow(&gps_time)) {
        MaybeDisciplineSystemTimeFromGps(gps_time);
        if (out) {
          *out = gps_time;
        }
        return true;
      }
    }
    vTaskDelay(pdMS_TO_TICKS(100));
  }
  return false;
}

void MaybeDisciplineSystemTimeFromGps(const UtcTimeSnapshot& gps_time) {
  if (!gps_time.valid || gps_time.source != UtcTimeSource::kGps || IsSntpUsable()) {
    return;
  }
  time_t system_now = 0;
  const bool system_ok = SystemUtcNow(&system_now);
  if (system_ok && std::llabs(static_cast<long long>(system_now - gps_time.unix_time)) < 2) {
    return;
  }
  timeval tv {};
  tv.tv_sec = gps_time.unix_time;
  tv.tv_usec = static_cast<suseconds_t>(gps_time.millisecond) * 1000;
  if (settimeofday(&tv, nullptr) == 0) {
    ESP_LOGI(kTag, "System time disciplined from GPZDA (%lld.%03u)",
             static_cast<long long>(gps_time.unix_time), static_cast<unsigned>(gps_time.millisecond));
  }
}

UtcTimeSnapshot MakeSystemTimeSnapshot(UtcTimeSource source, bool valid) {
  UtcTimeSnapshot out{};
  time_t now = 0;
  if (SystemUtcNow(&now)) {
    out.unix_time = now;
    out.source = source;
    out.valid = valid;
    return out;
  }
  out.unix_time = static_cast<time_t>(esp_timer_get_time() / 1'000'000ULL);
  out.source = UtcTimeSource::kMonotonic;
  out.valid = false;
  return out;
}

}  // namespace

const char* UtcTimeSourceName(UtcTimeSource source) {
  switch (source) {
    case UtcTimeSource::kSntp:
      return "sntp";
    case UtcTimeSource::kGps:
      return "gps";
    case UtcTimeSource::kSystemCached:
      return "system_cached";
    case UtcTimeSource::kMonotonic:
      return "monotonic";
    case UtcTimeSource::kNone:
    default:
      return "none";
  }
}

uint64_t UtcTimeToUnixMs(const UtcTimeSnapshot& snapshot) {
  if (snapshot.unix_time <= 0) {
    return esp_timer_get_time() / 1000ULL;
  }
  return static_cast<uint64_t>(snapshot.unix_time) * 1000ULL + snapshot.millisecond;
}

UtcTimeSnapshot GetBestUtcTimeForData() {
  if (IsSntpUsable()) {
    return MakeSystemTimeSnapshot(UtcTimeSource::kSntp, true);
  }
  UtcTimeSnapshot gps{};
  if (GpsUtcNow(&gps)) {
    MaybeDisciplineSystemTimeFromGps(gps);
    return gps;
  }
  time_t system_now = 0;
  if (SystemUtcNow(&system_now)) {
    return MakeSystemTimeSnapshot(UtcTimeSource::kSystemCached, true);
  }
  return MakeSystemTimeSnapshot(UtcTimeSource::kMonotonic, false);
}

UtcTimeSnapshot GetBestUtcTimeForGps() {
  UtcTimeSnapshot gps{};
  if (GpsUtcNow(&gps)) {
    MaybeDisciplineSystemTimeFromGps(gps);
    return gps;
  }
  if (IsSntpUsable()) {
    return MakeSystemTimeSnapshot(UtcTimeSource::kSntp, true);
  }
  time_t system_now = 0;
  if (SystemUtcNow(&system_now)) {
    return MakeSystemTimeSnapshot(UtcTimeSource::kSystemCached, true);
  }
  return MakeSystemTimeSnapshot(UtcTimeSource::kMonotonic, false);
}

GpsReceiverStatus GetGpsReceiverStatus() {
  GpsReceiverStatus status{};
  GpsPosition pos{};
  int64_t pos_received_us = 0;
  if (gps_client.getLastPosition(pos, &pos_received_us)) {
    status.position_valid = true;
    status.latitude_deg = pos.latitude_deg;
    status.longitude_deg = pos.longitude_deg;
    status.altitude_m = pos.altitude_m;
    status.fix_quality = pos.fix_quality;
    status.satellites = pos.satellites;
    status.position_age_ms = std::max<int64_t>((esp_timer_get_time() - pos_received_us) / 1000, 0);
  }

  GpsDateTime dt{};
  int64_t time_received_us = 0;
  if (gps_client.getLastDateTime(dt, &time_received_us)) {
    UtcTimeSnapshot gps_time{};
    if (GpsUtcNow(&gps_time)) {
      status.time_valid = true;
      FormatUtcIsoToBuffer(gps_time, status.time_iso, sizeof(status.time_iso));
      status.time_age_ms = std::max<int64_t>((esp_timer_get_time() - time_received_us) / 1000, 0);
    }
  }
  return status;
}

static bool WaitForTempSensors(int timeout_ms) {
  if (timeout_ms <= 0) {
    return CopyState().temp_sensor_count > 0;
  }
  const int64_t deadline = esp_timer_get_time() + static_cast<int64_t>(timeout_ms) * 1000;
  while (esp_timer_get_time() < deadline) {
    if (CopyState().temp_sensor_count > 0) {
      return true;
    }
    vTaskDelay(pdMS_TO_TICKS(200));
  }
  return CopyState().temp_sensor_count > 0;
}

// Devices
LTC2440 adc1(ADC_CS1, ADC_MISO, false);
LTC2440 adc2(ADC_CS2, ADC_MISO);
LTC2440 adc3(ADC_CS3, ADC_MISO);


std::string Basename(const std::string& path) {
  const size_t pos = path.find_last_of('/');
  if (pos == std::string::npos) return path;
  return path.substr(pos + 1);
}

bool MoveFileToDir(const std::string& src_path, const char* dest_dir, std::string* out_new_path) {
  if (src_path.empty() || !dest_dir) return false;
  if (!EnsureDirExists(dest_dir)) return false;
  std::string dest = std::string(dest_dir) + "/" + Basename(src_path);
  if (rename(src_path.c_str(), dest.c_str()) != 0) {
    ESP_LOGE(kTag, "Failed to move %s -> %s (errno %d)", src_path.c_str(), dest.c_str(), errno);
    return false;
  }
  if (out_new_path) {
    *out_new_path = dest;
  }
  return true;
}

bool IsGpsLogFilename(const char* name) {
  if (!name) return false;
  return std::strncmp(name, "gps_", 4) == 0;
}

bool IsMeteoLogFilename(const char* name) {
  if (!name) return false;
  return std::strncmp(name, "meteo_", 6) == 0;
}

static std::string BuildGnssLogFilename(const GpsDateTime* frame_time) {
  char ts[32] = {};
  if (frame_time && frame_time->valid) {
    const GpsDateTime& dt = *frame_time;
    std::snprintf(ts, sizeof(ts), "%04d%02d%02d_%02d%02d%02d",
                  dt.year, dt.month, dt.day, dt.hour, dt.minute, dt.second);
  } else {
    const UtcTimeSnapshot best = GetBestUtcTimeForGps();
    if (!best.valid) {
      char fallback[32] = {};
      std::snprintf(fallback, sizeof(fallback), "gps_log_%06u.rtcm3", static_cast<unsigned>(GetBootId()));
      return fallback;
    }
    struct tm tm_info {};
    gmtime_r(&best.unix_time, &tm_info);
    strftime(ts, sizeof(ts), "%Y%m%d_%H%M%S", &tm_info);
  }

  std::string name = "gps_rtcm3_";
  name += ts;
  name += "_";
  name += std::to_string(GetBootId());
  name += ".rtcm3";
  return name;
}

static int MoveRootGpsFilesToUploadLocked(const std::string& active_path) {
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
    if (!IsGpsLogFilename(ent->d_name)) continue;
    if (!active_name.empty() && active_name == ent->d_name) continue;
    const std::string src = std::string(mount_point) + "/" + ent->d_name;
    if (MoveFileToDir(src, to_upload.c_str(), nullptr)) {
      moved++;
    }
  }
  closedir(dir);
  return moved;
}

// Move all completed meteo_*.txt files at the root to to_upload (skips active_path).
static bool QueueCurrentGnssLogForUploadLocked() {
  if (current_gnss_log_path.empty()) {
    return true;
  }
  struct stat st {};
  if (stat(current_gnss_log_path.c_str(), &st) != 0) {
    current_gnss_log_path.clear();
    gps_log_file_start_us = 0;
    return errno == ENOENT;
  }
  if (!S_ISREG(st.st_mode)) {
    current_gnss_log_path.clear();
    gps_log_file_start_us = 0;
    return true;
  }
  if (st.st_size <= 0) {
    remove(current_gnss_log_path.c_str());
    current_gnss_log_path.clear();
    gps_log_file_start_us = 0;
    return true;
  }
  if (!EnsureUploadDirs()) {
    return false;
  }

  const std::string queued_name = Basename(current_gnss_log_path);
  const std::string to_upload = ActiveToUploadDir();
  if (!MoveFileToDir(current_gnss_log_path, to_upload.c_str(), nullptr)) {
    return false;
  }
  current_gnss_log_path.clear();
  gps_log_file_start_us = 0;
  ESP_LOGI(kTag, "Queued RTCM3 log for upload: %s", queued_name.c_str());
  return true;
}

static bool EnsureRtcmLogFileLocked(const GpsDateTime* frame_time) {
  if (current_gnss_log_path.empty() || gps_log_file_start_us == 0) {
    (void)MoveRootGpsFilesToUploadLocked("");
    const std::string filename = BuildGnssLogFilename(frame_time);
    current_gnss_log_path = std::string(ActiveStorageMountPoint()) + "/" + filename;
    gps_log_file_start_us = esp_timer_get_time();
    ESP_LOGI(kTag, "Starting RTCM3 log: %s", current_gnss_log_path.c_str());
  }

  FILE* f = fopen(current_gnss_log_path.c_str(), "ab");
  if (!f) {
    ESP_LOGE(kTag, "Failed to create RTCM3 log %s (errno %d)", current_gnss_log_path.c_str(), errno);
    return false;
  }
  const bool ok = fclose(f) == 0;
  return ok;
}

static bool IsGnssLogRotationDue(uint64_t now_us) {
  return !current_gnss_log_path.empty() && gps_log_file_start_us > 0 &&
         now_us - gps_log_file_start_us >= kGnssLogRotateUs;
}

static bool RotateStaleGnssLogLocked(uint64_t now_us) {
  if (!IsGnssLogRotationDue(now_us)) {
    return true;
  }
  const std::string old_path = current_gnss_log_path;
  if (!QueueCurrentGnssLogForUploadLocked()) {
    ESP_LOGW(kTag, "RTCM3 log rotation is due, but failed to queue %s", old_path.c_str());
    ErrorManagerSet(ErrorCode::kGpsRtcm, ErrorSeverity::kWarning, "RTCM3 log rotation failed");
    return false;
  }
  ESP_LOGI(kTag, "RTCM3 log rotated without new frame: %s", old_path.c_str());
  return true;
}

static bool WriteGnssFrameLocked(const CurrentFrame& frame) {
  const bool first_open = gps_log_file_start_us == 0;
  const uint64_t now_us = esp_timer_get_time();
  if (first_open || current_gnss_log_path.empty()) {
    if (!EnsureRtcmLogFileLocked(frame.timestamp.valid ? &frame.timestamp : nullptr)) {
      return false;
    }
  } else if (IsGnssLogRotationDue(now_us)) {
    if (!QueueCurrentGnssLogForUploadLocked()) {
      return false;
    }
    const std::string filename = BuildGnssLogFilename(frame.timestamp.valid ? &frame.timestamp : nullptr);
    current_gnss_log_path = std::string(ActiveStorageMountPoint()) + "/" + filename;
    gps_log_file_start_us = now_us;
    ESP_LOGI(kTag, "Starting RTCM3 log: %s", current_gnss_log_path.c_str());
  }

  FILE* f = fopen(current_gnss_log_path.c_str(), "ab");
  if (!f) {
    ESP_LOGE(kTag, "Failed to open RTCM3 log %s (errno %d)", current_gnss_log_path.c_str(), errno);
    return false;
  }
  bool ok = gps_client.writeRtcmFramesToFile(frame, f);
  fflush(f);
  const int fd = fileno(f);
  if (fd >= 0) {
    fsync(fd);
  }
  if (fclose(f) != 0) {
    ok = false;
  }
  if (ok) {
    if (app_config.storage_backend == StorageBackend::kSd) {
      UpdateSdStatsLocked();
    }
    ESP_LOGD(kTag, "GNSS frame %u appended to RTCM3 log %s", static_cast<unsigned>(frame.frame_index), current_gnss_log_path.c_str());
  } else {
    ESP_LOGE(kTag, "Failed to append GNSS frame %u to RTCM3 log", static_cast<unsigned>(frame.frame_index));
  }
  return ok;
}

static void WarnMissingGnssFrameData(const CurrentFrame& frame) {
  for (uint16_t type : app_config.gps_rtcm_types) {
    if (frame.rtcm_by_type.count(type) == 0) {
      ESP_LOGD(kTag, "GNSS frame %u missing RTCM%u", static_cast<unsigned>(frame.frame_index), static_cast<unsigned>(type));
    }
  }
}

static bool HasAnyGnssFrameData(const CurrentFrame& frame) {
  return !frame.rtcm_by_type.empty();
}

static void GpsLogTask(void*) {
  constexpr TickType_t kInterval = pdMS_TO_TICKS(30 * 1000);
  constexpr int64_t kCollectWindowUs = 35'000'000;
  constexpr uint32_t kEmptyRtcmWarnFrames = 4;
  uint32_t frame_index = 0;
  uint32_t empty_rtcm_frames = 0;
  vTaskDelay(pdMS_TO_TICKS(5000));
  gps_client.probeReceiver();
  vTaskDelay(pdMS_TO_TICKS(1000));
  gps_client.configurePeriodicOutput(app_config.gps_rtcm_types, app_config.gps_mode);
  {
    SdLockGuard guard(pdMS_TO_TICKS(1000));
    if (guard.locked() && MountActiveStorage()) {
      (void)MoveRootGpsFilesToUploadLocked("");
      if (app_config.storage_backend == StorageBackend::kSd && !log_file) {
        UnmountLogSd();
      }
    } else {
      ESP_LOGW(kTag, "Storage unavailable, RTCM3 log file will be created on first write");
    }
  }
  while (true) {
    const int64_t cycle_start_us = esp_timer_get_time();
    if (usb_mode == UsbMode::kMsc) {
      vTaskDelay(kInterval);
      continue;
    }
    if (gps_reconfigure_requested) {
      gps_reconfigure_requested = false;
      gps_client.configurePeriodicOutput(app_config.gps_rtcm_types, app_config.gps_mode);
    }
    if (IsGnssLogRotationDue(esp_timer_get_time())) {
      SdLockGuard guard(pdMS_TO_TICKS(1000));
      if (guard.locked() && MountActiveStorage()) {
        const bool already_mounted = IsLogSdMounted();
        (void)RotateStaleGnssLogLocked(esp_timer_get_time());
        if (app_config.storage_backend == StorageBackend::kSd && !already_mounted && !log_file) {
          UnmountLogSd();
        }
      } else {
        ESP_LOGW(kTag, "Storage unavailable, cannot rotate stale RTCM3 log");
      }
    }

    gps_client.startFrame(frame_index);
    gps_client.pollFrame();
    const int64_t deadline_us = esp_timer_get_time() + kCollectWindowUs;
    while (esp_timer_get_time() < deadline_us) {
      if (gps_client.isCurrentFrameComplete()) {
        break;
      }
      vTaskDelay(pdMS_TO_TICKS(100));
    }

    CurrentFrame frame{};
    if (!gps_client.finishFrame(frame)) {
      gps_client.stopFrameOutput();
      ESP_LOGW(kTag, "Failed to finish GNSS frame %u", static_cast<unsigned>(frame_index));
      frame_index++;
      continue;
    }
    gps_client.stopFrameOutput();
    WarnMissingGnssFrameData(frame);
    if (!HasAnyGnssFrameData(frame)) {
      empty_rtcm_frames++;
      if (empty_rtcm_frames >= kEmptyRtcmWarnFrames) {
        ErrorManagerSet(ErrorCode::kGpsRtcm, ErrorSeverity::kWarning, "GNSS RTCM frames are empty");
      }
      ESP_LOGW(kTag, "GNSS frame %u is empty, skip storage write", static_cast<unsigned>(frame.frame_index));
      frame_index++;
      const int64_t elapsed_us = esp_timer_get_time() - cycle_start_us;
      const int64_t interval_us = 30'000'000;
      if (elapsed_us < interval_us) {
        vTaskDelay(pdMS_TO_TICKS((interval_us - elapsed_us) / 1000));
      } else {
        vTaskDelay(pdMS_TO_TICKS(100));
      }
      continue;
    }
    empty_rtcm_frames = 0;
    ErrorManagerClear(ErrorCode::kGpsRtcm);

    {
      SdLockGuard guard(pdMS_TO_TICKS(1000));
      if (!guard.locked()) {
        ESP_LOGW(kTag, "Storage mutex unavailable, skip GNSS frame write");
        frame_index++;
        vTaskDelay(kInterval);
        continue;
      }

      const bool already_mounted = IsLogSdMounted();
      if (MountActiveStorage()) {
        (void)WriteGnssFrameLocked(frame);
        if (app_config.storage_backend == StorageBackend::kSd && !already_mounted && !log_file) {
          UnmountLogSd();
        }
      }
    }

    frame_index++;
    const int64_t elapsed_us = esp_timer_get_time() - cycle_start_us;
    const int64_t interval_us = 30'000'000;
    if (elapsed_us < interval_us) {
      vTaskDelay(pdMS_TO_TICKS((interval_us - elapsed_us) / 1000));
    } else {
      vTaskDelay(pdMS_TO_TICKS(100));
    }
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
  WaitForTempSensors(3000);
  SdLockGuard guard;
  if (!guard.locked()) {
    ESP_LOGW(kTag, "Storage mutex unavailable, cannot open log file");
    return false;
  }
  if (!MountActiveStorage()) {
    return false;
  }
  if (log_file) {
    fclose(log_file);
    log_file = nullptr;
  }

  const std::string filename = BuildLogFilename(postfix);
  std::string full_path;
  if (!BuildActiveStorageFilenamePath(filename, &full_path)) {
    ESP_LOGW(kTag, "Bad filename for logging: %s", filename.c_str());
    return false;
  }
  log_file = fopen(full_path.c_str(), "w");
  if (!log_file) {
    ESP_LOGE(kTag, "Failed to open log file %s", full_path.c_str());
    return false;
  }

  SharedState snapshot = CopyState();
  const int temp_count = std::min(snapshot.temp_sensor_count, MAX_TEMP_SENSORS);
  log_config.temp_sensor_count = temp_count;
  log_config.file_start_us = esp_timer_get_time();
  fprintf(log_file, "timestamp_iso,timestamp_ms,adc1,adc2,adc3");
  for (int i = 0; i < temp_count; ++i) {
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
  fprintf(log_file, ",gps_lat,gps_lon,gps_alt,gps_fix_quality,gps_satellites,gps_fix_age_ms");
  fprintf(log_file, "\n");
  FlushLogFile();

  UpdateState([&](SharedState& s) {
    s.logging = true;
    s.log_filename = filename;
  });
  current_log_path = full_path;
  if (app_config.storage_backend == StorageBackend::kSd) {
    UpdateSdStatsLocked();
  }
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

esp_err_t InitSpiBus() {
  if (shared_spi_bus_inited) {
    return ESP_OK;
  }
  spi_bus_config_t buscfg = {};
  buscfg.mosi_io_num = SPI_MOSI;
  buscfg.miso_io_num = SPI_MISO;
  buscfg.sclk_io_num = SPI_SCK;
  buscfg.quadwp_io_num = -1;
  buscfg.quadhd_io_num = -1;
  buscfg.max_transfer_sz = 1536;
  esp_err_t ret = spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO);
  if (ret == ESP_ERR_INVALID_STATE) {
    shared_spi_bus_inited = true;
    return ESP_OK;
  }
  if (ret == ESP_OK) {
    shared_spi_bus_inited = true;
  }
  return ret;
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
      GpioMask(RELAY_PIN) | GpioMask(STEPPER_EN) | GpioMask(STEPPER_DIR) | GpioMask(STEPPER_STEP) |
      GpioMask(HEATER_PWM) | GpioMask(FAN_PWM) | GpioMask(EXT_PWR_ON) |
      GpioMask(STATUS_LED_RED) | GpioMask(STATUS_LED_GREEN) |
      GpioMask(ADC_CS1) | GpioMask(ADC_CS2) | GpioMask(ADC_CS3) | GpioMask(ETH_CS) | GpioMask(ETH_RST);
  io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
  io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
  if (io_conf.pin_bit_mask != 0) {
    ESP_ERROR_CHECK(gpio_config(&io_conf));
  }

  gpio_set_level(RELAY_PIN, 0);
  gpio_set_level(STEPPER_EN, 1);   // disable motor by default
  gpio_set_level(STEPPER_DIR, 0);
  gpio_set_level(STEPPER_STEP, 0);
  gpio_set_level(HEATER_PWM, 0);
  SetExternalPower(true);
  gpio_set_level(STATUS_LED_RED, 0);
  gpio_set_level(STATUS_LED_GREEN, 0);
  gpio_set_level(ADC_CS1, 1);
  gpio_set_level(ADC_CS2, 1);
  gpio_set_level(ADC_CS3, 1);
  gpio_set_level(ETH_CS, 1);
  gpio_set_level(ETH_RST, 0);

  // Hall sensor input
  gpio_config_t hall_conf = {};
  hall_conf.intr_type = GPIO_INTR_DISABLE;
  hall_conf.mode = GPIO_MODE_INPUT;
  hall_conf.pin_bit_mask = (1ULL << MT_HALL_SEN);
  hall_conf.pull_up_en = GPIO_PULLUP_ENABLE;
  hall_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
  ESP_ERROR_CHECK(gpio_config(&hall_conf));

  if (GPS_ANT_SHORT != GPIO_NUM_NC) {
    gpio_config_t gps_ant_conf = {};
    gps_ant_conf.intr_type = GPIO_INTR_DISABLE;
    gps_ant_conf.mode = GPIO_MODE_INPUT;
    gps_ant_conf.pin_bit_mask = GpioMask(GPS_ANT_SHORT);
    gps_ant_conf.pull_up_en = GPIO_PULLUP_DISABLE;
    gps_ant_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    ESP_ERROR_CHECK(gpio_config(&gps_ant_conf));
  }

  if (ETH_INT != GPIO_NUM_NC || FAN1_TACH != GPIO_NUM_NC || FAN2_TACH != GPIO_NUM_NC) {
    EnsureGpioIsrServiceInstalled();
  }

  // Tachometer inputs with interrupts
  gpio_config_t tach_conf = {};
  tach_conf.intr_type = GPIO_INTR_POSEDGE;
  tach_conf.mode = GPIO_MODE_INPUT;
  tach_conf.pin_bit_mask = GpioMask(FAN1_TACH) | GpioMask(FAN2_TACH);
  tach_conf.pull_up_en = GPIO_PULLUP_ENABLE;
  tach_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
  if (tach_conf.pin_bit_mask == 0) {
    return;
  }
  ESP_ERROR_CHECK(gpio_config(&tach_conf));

  if (FAN1_TACH != GPIO_NUM_NC) {
    ESP_ERROR_CHECK(gpio_isr_handler_add(FAN1_TACH, FanTachIsr, (void*)FAN1_TACH));
  }
  if (FAN2_TACH != GPIO_NUM_NC) {
    ESP_ERROR_CHECK(gpio_isr_handler_add(FAN2_TACH, FanTachIsr, (void*)FAN2_TACH));
  }
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
  if (FAN_PWM == GPIO_NUM_NC) {
    UpdateState([&](SharedState& s) { s.fan_power = 100.0f; });
    return;
  }
  const uint32_t duty = static_cast<uint32_t>(p * 1023.0f / 100.0f);
  ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1, duty);
  ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1);
  UpdateState([&](SharedState& s) { s.fan_power = p; });
}

void InitFanPwm() {
  if (FAN_PWM == GPIO_NUM_NC) {
    UpdateState([](SharedState& s) { s.fan_power = 100.0f; });
    return;
  }
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
    bus_cfg.glitch_ignore_cnt = 7;
    bus_cfg.intr_priority = 0;
    bus_cfg.trans_queue_depth = 0;
    bus_cfg.flags.enable_internal_pullup = true;
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_cfg, &i2c_bus), kTag, "I2C bus init failed");
  }

  if (!ina219_dev) {
    i2c_device_config_t dev_cfg = {};
    dev_cfg.device_address = INA219_ADDR;
    dev_cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dev_cfg.scl_speed_hz = INA219_I2C_FREQ_HZ;
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(i2c_bus, &dev_cfg, &ina219_dev), kTag, "INA219 attach failed");
  }

  auto write_reg = [](uint8_t reg, uint16_t value) -> esp_err_t {
    uint8_t payload[3] = {
        reg,
        static_cast<uint8_t>((value >> 8) & 0xFF),
        static_cast<uint8_t>(value & 0xFF),
    };
    return i2c_master_transmit(ina219_dev, payload, sizeof(payload), INA219_I2C_TIMEOUT);
  };

  ESP_RETURN_ON_ERROR(write_reg(0x00, INA219_CONFIG), kTag, "INA219 config failed");
  ESP_RETURN_ON_ERROR(write_reg(0x05, INA219_CALIBRATION), kTag, "INA219 calibration failed");
  ESP_LOGI(kTag, "INA219 initialized");
  return ESP_OK;
}

esp_err_t ReadIna219() {
  if (!ina219_dev) {
    ErrorManagerSetLocal(ErrorCode::kInaRead, ErrorSeverity::kWarning, "INA219 not initialized");
    return ESP_ERR_INVALID_STATE;
  }
  auto read_reg = [](uint8_t reg, uint16_t* value) -> esp_err_t {
    if (!value) {
      return ESP_ERR_INVALID_ARG;
    }
    uint8_t reg_addr = reg;
    uint8_t rx[2] = {};
    esp_err_t res =
        i2c_master_transmit_receive(ina219_dev, &reg_addr, sizeof(reg_addr), rx, sizeof(rx), INA219_I2C_TIMEOUT);
    if (res == ESP_OK) {
      *value = static_cast<uint16_t>(static_cast<uint16_t>(rx[0]) << 8 | static_cast<uint16_t>(rx[1]));
    }
    return res;
  };

  uint16_t bus_raw = 0;
  uint16_t current_raw = 0;
  uint16_t power_raw = 0;

  esp_err_t err = read_reg(0x02, &bus_raw);
  if (err != ESP_OK) {
    ErrorManagerSetLocal(ErrorCode::kInaRead, ErrorSeverity::kWarning,
                         std::string("INA219 read bus failed (i2c): ") + esp_err_to_name(err));
    return err;
  }
  err = read_reg(0x04, &current_raw);
  if (err != ESP_OK) {
    ErrorManagerSetLocal(ErrorCode::kInaRead, ErrorSeverity::kWarning,
                         std::string("INA219 read current failed (i2c): ") + esp_err_to_name(err));
    return err;
  }
  err = read_reg(0x03, &power_raw);
  if (err != ESP_OK) {
    ErrorManagerSetLocal(ErrorCode::kInaRead, ErrorSeverity::kWarning,
                         std::string("INA219 read power failed (i2c): ") + esp_err_to_name(err));
    return err;
  }

  // Bus voltage: bits 3..15, LSB = 4 mV
  const float bus_v = static_cast<float>((bus_raw >> 3) & 0x1FFF) * INA219_BUS_LSB;
  const float current_a = static_cast<int16_t>(current_raw) * INA219_CURRENT_LSB;
  const float power_w = static_cast<uint16_t>(power_raw) * INA219_POWER_LSB;

  UpdateState([&](SharedState& s) {
    s.ina_bus_voltage = bus_v;
    s.ina_current = current_a;
    s.ina_power = power_w;
  });
  ErrorManagerClearLocal(ErrorCode::kInaRead);
  return ESP_OK;
}

esp_err_t InitSdCardForMsc(sdmmc_card_t** out_card) {
  bool host_init = false;
  sdmmc_card_t* card = static_cast<sdmmc_card_t*>(malloc(sizeof(sdmmc_card_t)));
  ESP_RETURN_ON_FALSE(card, ESP_ERR_NO_MEM, kTag, "No mem for sdmmc_card_t");

  sdmmc_host_t host = SDMMC_HOST_DEFAULT();
  host.flags |= SDMMC_HOST_FLAG_1BIT;

  sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
  slot_config.width = 1;
  slot_config.clk = SD_CLK;
  slot_config.cmd = SD_CMD;
  slot_config.d0 = SD_D0;
  slot_config.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

  ESP_RETURN_ON_ERROR((*host.init)(), kTag, "Host init failed");
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
    ESP_LOGW(kTag, "Insert SD card");
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
  ESP_RETURN_ON_ERROR(tinyusb_msc_storage_init_sdmmc(&config_sdmmc), kTag, "Init MSC storage failed");
  ESP_RETURN_ON_ERROR(tinyusb_msc_storage_mount(MSC_BASE_PATH), kTag, "Mount MSC storage failed");

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
  ESP_RETURN_ON_ERROR(tinyusb_driver_install(&tusb_cfg), kTag, "TinyUSB install failed");
  ESP_LOGI(kTag, "USB in MSC mode");
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

struct StepperTaskSnapshot {
  bool homing = false;
  bool stepper_abort = false;
  bool stepper_enabled = false;
  bool stepper_moving = false;
  bool stepper_direction_forward = true;
  int stepper_speed_us = 1;
  int64_t last_step_timestamp_us = 0;
};

static StepperTaskSnapshot ReadStepperTaskSnapshot() {
  StepperTaskSnapshot out{};
  if (state_mutex && xSemaphoreTake(state_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
    out.homing = state.homing;
    out.stepper_abort = state.stepper_abort;
    out.stepper_enabled = state.stepper_enabled;
    out.stepper_moving = state.stepper_moving;
    out.stepper_direction_forward = state.stepper_direction_forward;
    out.stepper_speed_us = state.stepper_speed_us;
    out.last_step_timestamp_us = state.last_step_timestamp_us;
    xSemaphoreGive(state_mutex);
  }
  out.stepper_speed_us = std::max(out.stepper_speed_us, 1);
  return out;
}

void StepperTask(void*) {
  const TickType_t idle_delay_active = pdMS_TO_TICKS(1);
  const TickType_t idle_delay_idle = pdMS_TO_TICKS(5);
  while (true) {
    StepperTaskSnapshot snapshot = ReadStepperTaskSnapshot();
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
    raw1 = 0;
  }
  err = adc2.Read(&raw2);
  if (err != ESP_OK) {
    ESP_LOGW(kTag, "ADC2 read failed: %s", esp_err_to_name(err));
    ErrorManagerSet(ErrorCode::kAdcRead, ErrorSeverity::kError, "ADC2 read failed");
    return err;
  }
  err = adc3.Read(&raw3);
  if (err != ESP_OK) {
    ESP_LOGW(kTag, "ADC3 read failed: %s", esp_err_to_name(err));
    ErrorManagerSet(ErrorCode::kAdcRead, ErrorSeverity::kError, "ADC3 read failed");
    return err;
  }

  *v1 = static_cast<float>(raw1) * ADC_SCALE;
  *v2 = static_cast<float>(raw2) * ADC_SCALE;
  *v3 = static_cast<float>(raw3) * ADC_SCALE;
  ErrorManagerClear(ErrorCode::kAdcRead);
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
    }
    vTaskDelay(pdMS_TO_TICKS(200));
  }
}

void Ina219Task(void*) {
  int consecutive_failures = 0;
  int64_t last_log_us = 0;
  int64_t last_reinit_us = 0;
  while (true) {
    esp_err_t err = ReadIna219();
    if (err != ESP_OK) {
      consecutive_failures++;
      const int64_t now_us = esp_timer_get_time();
      if (consecutive_failures == 1 || now_us - last_log_us > 10'000'000) {
        ESP_LOGW(kTag, "INA219 read failed: %s (consecutive=%d)", esp_err_to_name(err), consecutive_failures);
        last_log_us = now_us;
      }
      if (consecutive_failures >= 3 && now_us - last_reinit_us > 10'000'000) {
        last_reinit_us = now_us;
        ESP_LOGW(kTag, "Reinitializing INA219 after I2C failures");
        esp_err_t init_err = InitIna219();
        if (init_err != ESP_OK) {
          ESP_LOGW(kTag, "INA219 reinit failed: %s", esp_err_to_name(init_err));
        }
      }
    } else if (consecutive_failures > 0) {
      ESP_LOGI(kTag, "INA219 recovered after %d failed read(s)", consecutive_failures);
      consecutive_failures = 0;
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
      ErrorManagerClear(ErrorCode::kTempSensor);
      const auto meta = BuildTempMeta(count);
      UpdateState([&](SharedState& s) {
        s.temp_sensor_count = count;
        s.temps_c = temps;
        s.temp_labels = meta.labels;
        s.temp_addresses = meta.addresses;
        if (count > 0) {
          const uint16_t available_mask = static_cast<uint16_t>((1u << std::min(count, MAX_TEMP_SENSORS)) - 1u);
          uint16_t mask = static_cast<uint16_t>(s.pid_sensor_mask & available_mask);
          if (mask == 0) {
            int idx = s.pid_sensor_index;
            if (idx < 0 || idx >= count) idx = 0;
            mask = static_cast<uint16_t>(1u << idx);
          }
          s.pid_sensor_mask = mask;
          if (s.pid_sensor_index >= count || s.pid_sensor_index < 0) {
            s.pid_sensor_index = FirstSetBitIndex(mask);
          }
        }
      });
      if (count > 0) {
        ESP_LOGD(kTag, "Temps (%d):", count);
        for (int i = 0; i < count; ++i) {
          ESP_LOGD(kTag, "  Sensor %d: %.2f C", i + 1, temps[i]);
        }
      }
    } else {
      ESP_LOGW(kTag, "M1820ReadTemperatures failed");
      ErrorManagerSet(ErrorCode::kTempSensor, ErrorSeverity::kWarning, "M1820 read failed");
    }
    vTaskDelay(pdMS_TO_TICKS(2000));
  }
}

void PidTask(void*) {
  float integral = 0.0f;
  float prev_error = 0.0f;
  int64_t prev_update_us = 0;
  bool have_prev_error = false;
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
      prev_update_us = 0;
      have_prev_error = false;
      UpdateState([](SharedState& s) {
        s.pid_temperature = 0.0f;
        s.pid_error = 0.0f;
        s.pid_integral = 0.0f;
        s.pid_integral_candidate = 0.0f;
        s.pid_derivative = 0.0f;
        s.pid_p_term = 0.0f;
        s.pid_i_term = 0.0f;
        s.pid_d_term = 0.0f;
        s.pid_raw_output = 0.0f;
        s.pid_dt = 0.0f;
        s.pid_saturated_high = false;
        s.pid_saturated_low = false;
        s.pid_integral_held = false;
      });
      vTaskDelay(pdMS_TO_TICKS(1000));
      continue;
    }
    if (snapshot.temp_sensor_count <= 0) {
      vTaskDelay(pdMS_TO_TICKS(1000));
      continue;
    }
    uint16_t mask = snapshot.pid_sensor_mask;
    if (mask == 0) {
      int idx = std::clamp(snapshot.pid_sensor_index, 0, snapshot.temp_sensor_count - 1);
      mask = static_cast<uint16_t>(1u << idx);
    }
    float temp_sum = 0.0f;
    int temp_count = 0;
    for (int i = 0; i < snapshot.temp_sensor_count && i < MAX_TEMP_SENSORS; ++i) {
      if ((mask & (1u << i)) == 0) continue;
      float t = snapshot.temps_c[i];
      if (!std::isfinite(t)) continue;
      temp_sum += t;
      temp_count++;
    }
    if (temp_count == 0) {
      vTaskDelay(pdMS_TO_TICKS(1000));
      continue;
    }
    float temp = temp_sum / static_cast<float>(temp_count);
    const int64_t now_us = esp_timer_get_time();
    float dt = prev_update_us > 0 ? static_cast<float>(now_us - prev_update_us) / 1000000.0f : 1.0f;
    bool valid_dt = std::isfinite(dt) && dt > 0.0f && dt <= 10.0f;
    if (!valid_dt) {
      dt = 1.0f;
    }
    float error = snapshot.pid_setpoint - temp;
    float derivative = (have_prev_error && valid_dt) ? (error - prev_error) / dt : 0.0f;
    float candidate_integral = std::clamp(integral + error * dt, 0.0f, 1500.0f);
    float p_term = snapshot.pid_kp * error;
    float i_term = snapshot.pid_ki * candidate_integral;
    float d_term = snapshot.pid_kd * derivative;
    float raw_output = p_term + i_term + d_term;
    float output = std::clamp(raw_output, 0.0f, 100.0f);
    bool saturated_high = raw_output > 100.0f;
    bool saturated_low = raw_output < 0.0f;
    bool integral_held = true;
    if ((!saturated_high && !saturated_low) ||
        (saturated_high && error < 0.0f) ||
        (saturated_low && error > 0.0f)) {
      integral = candidate_integral;
      integral_held = false;
    }
    output = std::clamp(output, 0.0f, 100.0f);
    HeaterSetPowerPercent(output);
    UpdateState([&](SharedState& s) {
      s.pid_output = output;
      s.pid_temperature = temp;
      s.pid_error = error;
      s.pid_integral = integral;
      s.pid_integral_candidate = candidate_integral;
      s.pid_derivative = derivative;
      s.pid_p_term = p_term;
      s.pid_i_term = i_term;
      s.pid_d_term = d_term;
      s.pid_raw_output = raw_output;
      s.pid_dt = dt;
      s.pid_saturated_high = saturated_high;
      s.pid_saturated_low = saturated_low;
      s.pid_integral_held = integral_held;
    });
    prev_error = error;
    prev_update_us = now_us;
    have_prev_error = true;
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
    ESP_LOGW(kTag, "Calibration already in progress");
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
    ESP_LOGI(kTag, "Calibration done: offsets %.6f, %.6f, %.6f", new_offset1, new_offset2, new_offset3);
  } else {
    UpdateState([](SharedState& s) {
      s.calibrating = false;
    });
    ESP_LOGW(kTag, "Calibration collected no samples");
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
  const uint32_t boot_id = LoadAndIncrementBootId();
  ESP_LOGI(kTag, "Boot ID: %u", static_cast<unsigned>(boot_id));
  LogMemoryStatus("boot");
  ESP_LOGI(kTag, "TLS config: in=%d out=%d dynamic_buffer=%s mbedtls_alloc=%s",
           CONFIG_MBEDTLS_SSL_IN_CONTENT_LEN,
           CONFIG_MBEDTLS_SSL_OUT_CONTENT_LEN,
#if CONFIG_MBEDTLS_DYNAMIC_BUFFER
           "yes",
#else
           "no",
#endif
           MbedtlsAllocModeName());

  // Optionally disable task watchdog to avoid false positives from tight stepper loop
  esp_task_wdt_deinit();

  state_mutex = xSemaphoreCreateMutex();
  StorageManagerInit();

  LoadConfigFromSdCard(&app_config);

  bool usb_mode_found = false;
  usb_mode = LoadUsbModeFromNvs(&usb_mode_found);
  if (app_config.usb_mass_storage_from_file && !usb_mode_found) {
    usb_mode = app_config.usb_mass_storage ? UsbMode::kMsc : UsbMode::kCdc;
    ESP_LOGI(kTag, "USB mode set from config.txt (no NVS value): %s", usb_mode == UsbMode::kMsc ? "MSC" : "CDC");
    SaveUsbModeToNvs(usb_mode);
  }
  UpdateState([&](SharedState& s) { s.usb_msc_mode = (usb_mode == UsbMode::kMsc); });

  InitGpios();
  ErrorManagerInit();
  ErrorManagerSetPublisher(&PublishErrorPayload);
  if (app_config.storage_backend == StorageBackend::kInternalFlash) {
    SdLockGuard guard(pdMS_TO_TICKS(2000));
    if (!guard.locked() || !MountInternalFlashFs()) {
      ESP_LOGW(kTag, "Internal flash storage is selected but not mounted yet");
    }
  }

  if (usb_mode == UsbMode::kMsc) {
    esp_err_t msc_err = InitSdCardForMsc(&sd_card);
    if (msc_err == ESP_OK) {
      msc_err = StartUsbMsc(sd_card);
    }
    if (msc_err != ESP_OK) {
      msc_ok = false;
      ESP_LOGE(kTag, "USB MSC init failed, fallback to CDC mode: %s", esp_err_to_name(msc_err));
      usb_mode = UsbMode::kCdc;
      ErrorManagerSet(ErrorCode::kUsbMscInit, ErrorSeverity::kError,
                      std::string("MSC init failed: ") + esp_err_to_name(msc_err));
      UpdateState([&](SharedState& s) {
        s.usb_msc_mode = false;
        s.usb_error = "MSC init failed: " + std::string(esp_err_to_name(msc_err));
      });
      SaveUsbModeToNvs(usb_mode);
    } else {
      UpdateState([](SharedState& s) { s.usb_error.clear(); });
      ErrorManagerClear(ErrorCode::kUsbMscInit);
    }
  } else {
    UpdateState([](SharedState& s) { s.usb_error.clear(); });
    ErrorManagerClear(ErrorCode::kUsbMscInit);
  }

  InitHeaterPwm();
  InitFanPwm();
  bool temp_ok = M1820Init(TEMP_1WIRE);
  if (!temp_ok) {
    ESP_LOGW(kTag, "M1820 init failed or no sensors found");
    ErrorManagerSet(ErrorCode::kTempSensor, ErrorSeverity::kError, "M1820 init failed");
    init_ok = false;
  }
  ESP_ERROR_CHECK(InitSpiBus());
  ESP_ERROR_CHECK(adc1.Init(SPI2_HOST, ADC_SPI_FREQ_HZ));
  ESP_ERROR_CHECK(adc2.Init(SPI2_HOST, ADC_SPI_FREQ_HZ));
  ESP_ERROR_CHECK(adc3.Init(SPI2_HOST, ADC_SPI_FREQ_HZ));
  esp_err_t ina_err = InitIna219();
  if (ina_err != ESP_OK) {
    ESP_LOGE(kTag, "INA219 init failed: %s", esp_err_to_name(ina_err));
    ErrorManagerSet(ErrorCode::kInaInit, ErrorSeverity::kError, "INA219 init failed");
    init_ok = false;
  } else {
    ErrorManagerClear(ErrorCode::kInaInit);
  }

  if (!app_config.wifi_from_file) {
    ESP_LOGI(kTag, "Using default Wi-Fi config (SSID: %s)", app_config.wifi_ssid.c_str());
  }
  UpdateState([](SharedState& s) {
    s.pid_kp = pid_config.kp;
    s.pid_ki = pid_config.ki;
    s.pid_kd = pid_config.kd;
    s.pid_setpoint = pid_config.setpoint;
    s.pid_sensor_index = pid_config.sensor_index;
    s.pid_sensor_mask = pid_config.sensor_mask;
    s.stepper_speed_us = app_config.stepper_speed_us;
    s.stepper_home_offset_steps = app_config.stepper_home_offset_steps;
    s.motor_hall_active_level = app_config.motor_hall_active_level;
    if (s.stepper_home_status.empty()) {
      s.stepper_home_status = "idle";
    }
  });
  const esp_err_t gps_err = StartGpsClient();
  ApplyNetworkConfig();
  StartSntp();
  if (EnsureTimeSynced(8000)) {
    ESP_LOGI(kTag, "Time synced via NTP");
    ErrorManagerClear(ErrorCode::kTimeSync);
  } else {
    UtcTimeSnapshot fallback_time{};
    if (gps_err == ESP_OK) {
      (void)RequestGpsUtcTimeOnce(2500, &fallback_time);
    }
    if (!fallback_time.valid) {
      fallback_time = GetBestUtcTimeForData();
    }
    if (fallback_time.source == UtcTimeSource::kGps && fallback_time.valid) {
      ESP_LOGW(kTag, "NTP sync timed out, using GPZDA UTC time");
      ErrorManagerClear(ErrorCode::kTimeSync);
    } else if (fallback_time.source == UtcTimeSource::kSystemCached && fallback_time.valid) {
      ESP_LOGW(kTag, "NTP sync timed out, using cached system UTC time");
      ErrorManagerClear(ErrorCode::kTimeSync);
    } else {
      ESP_LOGW(kTag, "NTP sync timed out, using monotonic timestamp fallback");
      ErrorManagerSet(ErrorCode::kTimeSync, ErrorSeverity::kWarning, "NTP sync timed out");
    }
  }
  StartHttpServer();

  // Pin tasks to different cores: ADC на core0 (prio 4), шаги на core1 (prio 3) — idle0 свободен для WDT
  xTaskCreatePinnedToCore(&AdcTask, "adc_task", 4096, nullptr, 4, nullptr, 0);
  xTaskCreatePinnedToCore(&StepperTask, "stepper_task", 4096, nullptr, 3, nullptr, 1);
  if (ina_err == ESP_OK) {
    xTaskCreatePinnedToCore(&Ina219Task, "ina219_task", 5120, nullptr, 2, nullptr, 0);
  }
  if (FAN1_TACH != GPIO_NUM_NC || FAN2_TACH != GPIO_NUM_NC) {
    xTaskCreatePinnedToCore(&FanTachTask, "fan_tach_task", 2048, nullptr, 2, nullptr, 0);
  }
  if (temp_ok) {
    xTaskCreatePinnedToCore(&TempTask, "temp_task", 8192, nullptr, 2, nullptr, 0);
  }
  xTaskCreatePinnedToCore(&PidTask, "pid_task", 8192, nullptr, 2, nullptr, 0);
  StartNetworkTasks();
  if (upload_task == nullptr) {
    // Upload task uses esp_http_client and std::string, needs a bit more stack to avoid overflow.
    xTaskCreatePinnedToCore(&UploadTask, "upload_task", 12288, nullptr, 1, &upload_task, 0);
  }
  if (gps_log_task == nullptr && usb_mode == UsbMode::kCdc) {
    xTaskCreatePinnedToCore(&GpsLogTask, "gps_log", 6144, nullptr, 1, &gps_log_task, 0);
  }
  xTaskCreatePinnedToCore(&SdStatsTask, "sd_stats", 3072, nullptr, 1, nullptr, 0);

  if (app_config.logging_active && usb_mode == UsbMode::kCdc) {
    const std::string postfix = SanitizePostfix(app_config.logging_postfix);
    log_config.postfix = postfix;
    log_config.use_motor = app_config.logging_use_motor;
    log_config.duration_s = app_config.logging_duration_s;
    if (!StartLoggingToFile(postfix, usb_mode)) {
      ESP_LOGW(kTag, "Auto-start logging failed");
      app_config.logging_active = false;
      (void)SaveConfigToSdCard(app_config, pid_config, usb_mode);
    } else {
      app_config.logging_active = true;
      app_config.logging_postfix = postfix;
      app_config.logging_use_motor = log_config.use_motor;
      app_config.logging_duration_s = log_config.duration_s;
      UpdateState([&](SharedState& s) {
        s.logging = true;
        s.log_use_motor = log_config.use_motor;
        s.log_duration_s = log_config.duration_s;
      });
    }
  }

  init_ok = init_ok && msc_ok && (ina_err == ESP_OK);
  ESP_LOGI(kTag, "System ready");

  // Start MQTT after init (non-blocking)
  StartMqttBridge();
}
