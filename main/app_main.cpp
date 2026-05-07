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
#include "esp_mac.h"
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
#include "error_manager.h"
#include "sd_maintenance.h"
#include "gps_unicore.h"
#include "nvs.h"
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
// INA219 constants (32V range, hardware shunt is 0.05 ohm).
constexpr uint8_t INA219_ADDR = 0x40;
constexpr uint16_t INA219_CONFIG = 0x399F;           // 32V range, gain /8 (320mV), 12-bit, continuous
constexpr uint16_t INA219_CALIBRATION = 4096;        // Register calibration kept from 0.1R setup.
constexpr float INA219_CURRENT_LSB = 0.0002f;        // 200 uA; 0.05R shunt doubles physical current
constexpr float INA219_POWER_LSB = INA219_CURRENT_LSB * 20.0f;
constexpr float INA219_BUS_LSB = 0.004f;             // 4 mV
constexpr int INA219_I2C_FREQ_HZ = 100000;
constexpr TickType_t INA219_I2C_TIMEOUT = pdMS_TO_TICKS(250);
// Wi-Fi helpers
constexpr int WIFI_CONNECTED_BIT = BIT0;
constexpr int WIFI_FAIL_BIT = BIT1;
EventGroupHandle_t wifi_event_group = nullptr;
int retry_count = 0;
bool time_synced = false;
static bool netif_inited = false;
static bool sntp_started = false;
bool wifi_inited = false;
esp_netif_t* wifi_netif_sta = nullptr;
esp_netif_t* wifi_netif_ap = nullptr;
static TaskHandle_t wifi_recover_task = nullptr;
static TaskHandle_t eth_preferred_wifi_fallback_task = nullptr;
static TaskHandle_t config_ap_fallback_task = nullptr;
static bool fallback_ap_active = false;
static bool wifi_recover_active = false;
static wifi_config_t sta_cfg_cached = {};
static int64_t last_wifi_connect_attempt_us = 0;
static bool eth_inited = false;
static bool eth_started = false;
static bool shared_spi_bus_inited = false;
static bool eth_handlers_registered = false;
static esp_eth_handle_t eth_handle = nullptr;
static esp_netif_t* eth_netif = nullptr;
static esp_eth_mac_t* eth_mac = nullptr;
static esp_eth_phy_t* eth_phy = nullptr;
static spi_device_interface_config_t eth_devcfg = {};
static GpsUnicoreClient gps_client;
static TaskHandle_t gps_log_task = nullptr;
static TaskHandle_t external_power_cycle_task = nullptr;
constexpr uint32_t kExternalPowerCycleStackBytes = 6144;
static uint64_t gps_log_file_start_us = 0;
static std::string current_gnss_log_path;
static volatile bool gps_reconfigure_requested = false;
static bool sntp_time_available = false;
static int64_t last_sntp_sync_us = 0;

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
  ESP_LOGE(TAG, "gpio_install_isr_service failed: %s", esp_err_to_name(err));
  ESP_ERROR_CHECK(err);
}

bool IsHallTriggered() {
  return gpio_get_level(MT_HALL_SEN) == app_config.motor_hall_active_level;
}

void SetExternalPower(bool enabled) {
  gpio_set_level(EXT_PWR_ON, enabled ? 1 : 0);
  UpdateState([&](SharedState& s) { s.external_power_on = enabled; });
  ESP_LOGI(TAG, "External module power %s", enabled ? "ON" : "OFF");
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

void StartSntp();
bool EnsureTimeSynced(int timeout_ms);
esp_err_t InitSpiBus();
void WifiEventHandler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data);
static bool StartEthernet();
static esp_err_t EnsureWifiInit();
static void StopWifiInterface(bool clear_ap_ip);
static bool RequestWifiConnect(const char* reason);
static void StartWifiRecoverTask(int interval_ms);
static void StopWifiRecoverTask();
static void StartEthPreferredWifiFallbackTask(int delay_ms);
static void StopEthPreferredWifiFallbackTask();
static bool StartConfigAp();
static void StartConfigApFallbackTask(int delay_ms);
static void StopConfigApFallbackTask();
static void ReadNetworkUpFlags(bool* wifi_up, bool* eth_up);

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

static std::vector<uint16_t> ParseRtcmTypesString(const std::string& value) {
  std::vector<uint16_t> out;
  size_t start = 0;
  while (start < value.size()) {
    while (start < value.size() && (value[start] == ',' || value[start] == ' ' || value[start] == ';' || value[start] == '\t')) {
      start++;
    }
    if (start >= value.size()) break;
    size_t end = start;
    while (end < value.size() && value[end] != ',' && value[end] != ';' && !std::isspace(static_cast<unsigned char>(value[end]))) {
      end++;
    }
    const int type = std::atoi(value.substr(start, end - start).c_str());
    if (type > 0 && type <= 4095 &&
        std::find(out.begin(), out.end(), static_cast<uint16_t>(type)) == out.end()) {
      out.push_back(static_cast<uint16_t>(type));
    }
    start = end;
  }
  if (out.empty()) {
    out = {1004, 1006, 1033};
  }
  return out;
}

std::string GetGpsCurrentMode() {
  std::string mode;
  return gps_client.getCurrentMode(mode) ? mode : "";
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
    ESP_LOGE(TAG, "GPS init/start failed: %s", esp_err_to_name(gps_err));
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

bool AnyNetworkHasIp() {
  bool wifi_up = false;
  bool eth_up = false;
  ReadNetworkUpFlags(&wifi_up, &eth_up);
  return wifi_up || eth_up;
}

void MarkSntpSynced() {
  time_synced = true;
  sntp_time_available = true;
  last_sntp_sync_us = esp_timer_get_time();
}

void MarkSntpUnavailableIfNoNetwork() {
  if (!AnyNetworkHasIp()) {
    sntp_time_available = false;
  }
}

bool IsSntpUsable() {
  time_t now = 0;
  if (!SystemUtcNow(&now)) return false;
  if (!sntp_time_available) return false;
  if (!AnyNetworkHasIp()) return false;
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
    ESP_LOGI(TAG, "System time disciplined from GPZDA (%lld.%03u)",
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

static std::string FormatIp4(const esp_ip4_addr_t& ip) {
  if (ip.addr == 0) {
    return "";
  }
  char buf[16];
  std::snprintf(buf, sizeof(buf), IPSTR, IP2STR(&ip));
  return std::string(buf);
}

static std::string GetNetifIp(esp_netif_t* netif) {
  if (!netif) return "";
  esp_netif_ip_info_t info{};
  if (esp_netif_get_ip_info(netif, &info) != ESP_OK) {
    return "";
  }
  return FormatIp4(info.ip);
}

static void EnsureNetifInit() {
  if (!netif_inited) {
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    netif_inited = true;
  }
}

static void ReadNetworkUpFlags(bool* wifi_up, bool* eth_up) {
  bool wifi = false;
  bool eth = false;
  if (state_mutex && xSemaphoreTake(state_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
    wifi = !state.wifi_ip_sta.empty();
    eth = state.eth_ip_up || !state.eth_ip.empty();
    xSemaphoreGive(state_mutex);
  } else {
    wifi = !state.wifi_ip_sta.empty();
    eth = state.eth_ip_up || !state.eth_ip.empty();
  }
  if (wifi_up) {
    *wifi_up = wifi;
  }
  if (eth_up) {
    *eth_up = eth;
  }
}

static void UpdateDefaultNetif() {
  esp_netif_t* target = nullptr;
  const NetMode mode = app_config.net_mode;
  const NetPriority prio = app_config.net_priority;
  bool wifi_up = false;
  bool eth_up = false;
  ReadNetworkUpFlags(&wifi_up, &eth_up);

  if (mode == NetMode::kWifiOnly) {
    if (wifi_netif_sta) {
      target = wifi_netif_sta;
    }
  } else if (mode == NetMode::kEthOnly) {
    if (eth_netif) {
      target = eth_netif;
    }
  } else {
    if (prio == NetPriority::kWifi) {
      if (wifi_up && wifi_netif_sta) {
        target = wifi_netif_sta;
      } else if (eth_up && eth_netif) {
        target = eth_netif;
      }
    } else {
      if (eth_up && eth_netif) {
        target = eth_netif;
      } else if (wifi_up && wifi_netif_sta) {
        target = wifi_netif_sta;
      }
    }
  }

  if (target) {
    esp_netif_set_default_netif(target);
  }
}

static void EnsureConfiguredNetworkProgress() {
  bool wifi_up = false;
  bool eth_up = false;
  ReadNetworkUpFlags(&wifi_up, &eth_up);

  if (app_config.net_mode == NetMode::kWifiOnly) {
    if (!wifi_inited) {
      InitWifi(app_config.wifi_ssid, app_config.wifi_password, app_config.wifi_ap_mode);
    } else if (!wifi_up && !fallback_ap_active && !wifi_recover_task) {
      StartWifiRecoverTask(15000);
    }
    return;
  }

  if (app_config.net_mode == NetMode::kEthOnly) {
    if (!eth_started) {
      StartEthernet();
    }
    if (!eth_up && !fallback_ap_active && !config_ap_fallback_task) {
      StartConfigApFallbackTask(15000);
    }
    return;
  }

  if (!eth_started) {
    StartEthernet();
  }

  if (app_config.net_priority == NetPriority::kEth) {
    if (!eth_up && !wifi_up && !wifi_recover_task && !fallback_ap_active && !eth_preferred_wifi_fallback_task) {
      StartEthPreferredWifiFallbackTask(15000);
    }
  } else if (wifi_inited && !wifi_up && !fallback_ap_active && !wifi_recover_task) {
    StartWifiRecoverTask(15000);
  }
}

static bool TrySyncTimeOnNetif(esp_netif_t* netif, int timeout_ms) {
  if (!netif || timeout_ms <= 0) {
    return false;
  }
  esp_netif_t* prev = esp_netif_get_default_netif();
  if (prev != netif) {
    esp_netif_set_default_netif(netif);
  }

  esp_sntp_set_sync_status(SNTP_SYNC_STATUS_RESET);
  if (!esp_sntp_restart()) {
    StartSntp();
  }

  const int step_ms = 200;
  const int max_steps = std::max(1, timeout_ms / step_ms);
  bool ok = false;
  for (int i = 0; i < max_steps; ++i) {
    if (sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED) {
      time_t now = 0;
      time(&now);
      if (now > kValidUtcThreshold) {
        ok = true;
        break;
      }
    }
    vTaskDelay(pdMS_TO_TICKS(step_ms));
  }
  if (ok) {
    MarkSntpSynced();
  } else {
    sntp_time_available = false;
  }
  if (prev && prev != netif) {
    esp_netif_set_default_netif(prev);
  }
  return ok;
}

static void NetworkMonitorTask(void*) {
  const int interval_ms = 15000;
  const int check_timeout_ms = 1500;
  while (true) {
    EnsureConfiguredNetworkProgress();
    if (app_config.net_mode == NetMode::kWifiEth) {
      bool wifi_up = false;
      bool eth_up = false;
      ReadNetworkUpFlags(&wifi_up, &eth_up);
      esp_netif_t* prefer = (app_config.net_priority == NetPriority::kWifi) ? wifi_netif_sta : eth_netif;
      esp_netif_t* other = (prefer == wifi_netif_sta) ? eth_netif : wifi_netif_sta;
      bool prefer_has_ip = (prefer == wifi_netif_sta) ? wifi_up : eth_up;
      bool other_has_ip = (other == wifi_netif_sta) ? wifi_up : eth_up;

      bool prefer_ok = false;
      bool other_ok = false;
      if (prefer && prefer_has_ip) {
        prefer_ok = TrySyncTimeOnNetif(prefer, check_timeout_ms);
      }
      if (!prefer_ok && other && other_has_ip) {
        other_ok = TrySyncTimeOnNetif(other, check_timeout_ms);
      }

      if (prefer_ok && prefer) {
        esp_netif_set_default_netif(prefer);
      } else if (other_ok && other) {
        esp_netif_set_default_netif(other);
      } else {
        sntp_time_available = false;
        UpdateDefaultNetif();
      }
    } else {
      UpdateDefaultNetif();
      bool wifi_up = false;
      bool eth_up = false;
      ReadNetworkUpFlags(&wifi_up, &eth_up);
      esp_netif_t* netif = nullptr;
      bool has_ip = false;
      if (app_config.net_mode == NetMode::kWifiOnly) {
        netif = wifi_netif_sta;
        has_ip = wifi_up;
      } else if (app_config.net_mode == NetMode::kEthOnly) {
        netif = eth_netif;
        has_ip = eth_up;
      }
      if (netif && has_ip) {
        if (!TrySyncTimeOnNetif(netif, check_timeout_ms)) {
          sntp_time_available = false;
        }
      } else {
        sntp_time_available = false;
      }
    }
    vTaskDelay(pdMS_TO_TICKS(interval_ms));
  }
}

void EthEventHandler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
  if (event_base == ETH_EVENT) {
    if (event_id == ETHERNET_EVENT_START) {
      ESP_LOGI(TAG, "Ethernet started");
    } else if (event_id == ETHERNET_EVENT_CONNECTED) {
      ESP_LOGI(TAG, "Ethernet link up");
      UpdateState([](SharedState& s) { s.eth_link_up = true; });
    } else if (event_id == ETHERNET_EVENT_DISCONNECTED) {
      ESP_LOGW(TAG, "Ethernet link down");
      UpdateState([](SharedState& s) {
        s.eth_link_up = false;
        s.eth_ip.clear();
        s.eth_ip_up = false;
      });
      UpdateDefaultNetif();
      if (app_config.net_mode == NetMode::kEthOnly) {
        StartConfigApFallbackTask(5000);
      } else if (app_config.net_mode == NetMode::kWifiEth && app_config.net_priority == NetPriority::kEth) {
        StartEthPreferredWifiFallbackTask(5000);
      }
    } else if (event_id == ETHERNET_EVENT_STOP) {
      ESP_LOGI(TAG, "Ethernet stopped");
      UpdateState([](SharedState& s) {
        s.eth_link_up = false;
        s.eth_ip.clear();
        s.eth_ip_up = false;
      });
      UpdateDefaultNetif();
    }
  } else if (event_base == IP_EVENT && event_id == IP_EVENT_ETH_GOT_IP) {
    auto* event = static_cast<ip_event_got_ip_t*>(event_data);
    const std::string ip = FormatIp4(event->ip_info.ip);
    ESP_LOGI(TAG, "Ethernet got IP: " IPSTR " mask " IPSTR " gw " IPSTR,
             IP2STR(&event->ip_info.ip),
             IP2STR(&event->ip_info.netmask),
             IP2STR(&event->ip_info.gw));
    UpdateState([&](SharedState& s) {
      s.eth_ip = ip;
      s.eth_ip_up = !ip.empty();
    });
    StopEthPreferredWifiFallbackTask();
    StopConfigApFallbackTask();
    if (wifi_inited &&
        (app_config.net_mode == NetMode::kEthOnly ||
         (app_config.net_mode == NetMode::kWifiEth && app_config.net_priority == NetPriority::kEth))) {
      ESP_LOGI(TAG, "Ethernet is preferred and has IP, stopping Wi-Fi fallback/AP");
      StopWifiInterface(true);
    }
    UpdateDefaultNetif();
  }
}

static esp_err_t InitEthernet() {
#if !CONFIG_ETH_SPI_ETHERNET_W5500
  ESP_LOGE(TAG, "W5500 support not enabled in sdkconfig");
  return ESP_ERR_NOT_SUPPORTED;
#else
  if (eth_inited) {
    return ESP_OK;
  }
  EnsureNetifInit();
  ESP_LOGI(TAG, "Initializing W5500 Ethernet (CS=%d INT=%d RST=%d)",
           static_cast<int>(ETH_CS), static_cast<int>(ETH_INT), static_cast<int>(ETH_RST));
  esp_err_t ret = InitSpiBus();
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Shared SPI bus init for Ethernet failed: %s", esp_err_to_name(ret));
    return ret;
  }
  gpio_set_level(ETH_CS, 1);
  gpio_set_level(ETH_RST, 1);
  vTaskDelay(pdMS_TO_TICKS(10));

  eth_devcfg = {};
  eth_devcfg.mode = 0;
  eth_devcfg.clock_speed_hz = ETH_SPI_FREQ_HZ;
  eth_devcfg.spics_io_num = ETH_CS;
  eth_devcfg.queue_size = 20;

  eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();
  eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();
  phy_config.reset_gpio_num = ETH_RST;

  eth_w5500_config_t w5500_config = ETH_W5500_DEFAULT_CONFIG(SPI2_HOST, &eth_devcfg);
  w5500_config.int_gpio_num = ETH_INT;
  w5500_config.poll_period_ms = 0;

  eth_mac = esp_eth_mac_new_w5500(&w5500_config, &mac_config);
  eth_phy = esp_eth_phy_new_w5500(&phy_config);
  if (!eth_mac || !eth_phy) {
    ESP_LOGE(TAG, "Failed to create W5500 MAC/PHY");
    if (eth_mac) {
      eth_mac->del(eth_mac);
      eth_mac = nullptr;
    }
    if (eth_phy) {
      eth_phy->del(eth_phy);
      eth_phy = nullptr;
    }
    gpio_set_level(ETH_CS, 1);
    gpio_set_level(ETH_RST, 0);
    return ESP_FAIL;
  }

  esp_eth_config_t config = ETH_DEFAULT_CONFIG(eth_mac, eth_phy);
  esp_err_t install_err = esp_eth_driver_install(&config, &eth_handle);
  if (install_err != ESP_OK) {
    ESP_LOGE(TAG, "Ethernet driver install failed: %s", esp_err_to_name(install_err));
    return install_err;
  }

  uint8_t base_mac[6] = {};
  uint8_t eth_addr[6] = {};
  esp_err_t mac_err = esp_efuse_mac_get_default(base_mac);
  if (mac_err == ESP_OK) {
    mac_err = esp_derive_local_mac(eth_addr, base_mac);
  }
  if (mac_err == ESP_OK) {
    mac_err = esp_eth_ioctl(eth_handle, ETH_CMD_S_MAC_ADDR, eth_addr);
  }
  if (mac_err != ESP_OK) {
    ESP_LOGE(TAG, "Ethernet MAC address setup failed: %s", esp_err_to_name(mac_err));
    return mac_err;
  }
  ESP_LOGI(TAG, "Ethernet MAC: %02x:%02x:%02x:%02x:%02x:%02x",
           eth_addr[0], eth_addr[1], eth_addr[2], eth_addr[3], eth_addr[4], eth_addr[5]);

  esp_netif_config_t cfg = ESP_NETIF_DEFAULT_ETH();
  eth_netif = esp_netif_new(&cfg);
  ESP_ERROR_CHECK(esp_netif_attach(eth_netif, esp_eth_new_netif_glue(eth_handle)));
  if (strlen(HOSTNAME) > 0) {
    esp_netif_set_hostname(eth_netif, HOSTNAME);
  }
  if (!eth_handlers_registered) {
    ESP_ERROR_CHECK(esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, &EthEventHandler, nullptr));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, &EthEventHandler, nullptr));
    eth_handlers_registered = true;
  }
  eth_inited = true;
  ESP_LOGI(TAG, "W5500 Ethernet initialized");
  return ESP_OK;
#endif
}

static bool StartEthernet() {
  if (eth_started) {
    ESP_LOGI(TAG, "Ethernet already started");
    return true;
  }
  if (InitEthernet() != ESP_OK) {
    return false;
  }
  ESP_LOGI(TAG, "Starting Ethernet");
  esp_err_t err = esp_eth_start(eth_handle);
  if (err == ESP_OK) {
    eth_started = true;
    return true;
  } else {
    ESP_LOGE(TAG, "Ethernet start failed: %s", esp_err_to_name(err));
    return false;
  }
}

static void StopEthernet() {
  if (!eth_started || !eth_handle) {
    return;
  }
  esp_eth_stop(eth_handle);
  eth_started = false;
  UpdateState([](SharedState& s) {
    s.eth_link_up = false;
    s.eth_ip.clear();
    s.eth_ip_up = false;
  });
  UpdateDefaultNetif();
}

void ApplyNetworkConfig() {
  const NetMode mode = app_config.net_mode;
  ESP_LOGI(TAG, "Applying network config: mode=%s priority=%s",
           NetModeToString(app_config.net_mode).c_str(),
           NetPriorityToString(app_config.net_priority).c_str());
  if (mode == NetMode::kWifiOnly) {
    StopConfigApFallbackTask();
    StopEthPreferredWifiFallbackTask();
    StopEthernet();
    InitWifi(app_config.wifi_ssid, app_config.wifi_password, app_config.wifi_ap_mode);
  } else if (mode == NetMode::kEthOnly) {
    StopConfigApFallbackTask();
    StopEthPreferredWifiFallbackTask();
    StopWifiInterface(true);
    const bool eth_start_ok = StartEthernet();
    StartConfigApFallbackTask(eth_start_ok ? 15000 : 1000);
  } else {
    StopConfigApFallbackTask();
    if (app_config.net_priority == NetPriority::kEth) {
      StopWifiInterface(false);
      const bool eth_start_ok = StartEthernet();
      StartEthPreferredWifiFallbackTask(eth_start_ok ? 15000 : 1000);
    } else {
      StopEthPreferredWifiFallbackTask();
      StartEthernet();
      InitWifi(app_config.wifi_ssid, app_config.wifi_password, app_config.wifi_ap_mode);
    }
  }
  UpdateDefaultNetif();
}

static void WifiRecoverTask(void* arg) {
  const int interval_ms = static_cast<int>(reinterpret_cast<intptr_t>(arg));
  while (wifi_recover_active) {
    RequestWifiConnect("recover");
    vTaskDelay(pdMS_TO_TICKS(interval_ms));
  }
  vTaskDelete(nullptr);
}

static void StartWifiRecoverTask(int interval_ms) {
  if (wifi_recover_task) return;
  wifi_recover_active = true;
  xTaskCreatePinnedToCore(&WifiRecoverTask, "wifi_recover", 2048,
                          reinterpret_cast<void*>(static_cast<intptr_t>(interval_ms)), 1, &wifi_recover_task, 0);
}

static void StopWifiRecoverTask() {
  if (!wifi_recover_task) return;
  wifi_recover_active = false;
  TaskHandle_t t = wifi_recover_task;
  wifi_recover_task = nullptr;
  vTaskDelete(t);
}

static void EthPreferredWifiFallbackTask(void* arg) {
  const int delay_ms = static_cast<int>(reinterpret_cast<intptr_t>(arg));
  vTaskDelay(pdMS_TO_TICKS(delay_ms));

  bool wifi_up = false;
  bool eth_up = false;
  ReadNetworkUpFlags(&wifi_up, &eth_up);
  eth_preferred_wifi_fallback_task = nullptr;

  if (app_config.net_mode == NetMode::kWifiEth && app_config.net_priority == NetPriority::kEth && !eth_up && !wifi_up) {
    ESP_LOGW(TAG, "Ethernet preferred but no Ethernet IP after %d ms, starting Wi-Fi fallback", delay_ms);
    InitWifi(app_config.wifi_ssid, app_config.wifi_password, app_config.wifi_ap_mode);
  }
  vTaskDelete(nullptr);
}

static void StartEthPreferredWifiFallbackTask(int delay_ms) {
  if (eth_preferred_wifi_fallback_task) {
    return;
  }
  xTaskCreatePinnedToCore(&EthPreferredWifiFallbackTask, "eth_wifi_fb", 4096,
                          reinterpret_cast<void*>(static_cast<intptr_t>(delay_ms)), 1,
                          &eth_preferred_wifi_fallback_task, 0);
}

static void StopEthPreferredWifiFallbackTask() {
  if (!eth_preferred_wifi_fallback_task) {
    return;
  }
  TaskHandle_t t = eth_preferred_wifi_fallback_task;
  eth_preferred_wifi_fallback_task = nullptr;
  vTaskDelete(t);
}

static void ConfigApFallbackTask(void* arg) {
  const int delay_ms = static_cast<int>(reinterpret_cast<intptr_t>(arg));
  vTaskDelay(pdMS_TO_TICKS(delay_ms));

  bool wifi_up = false;
  bool eth_up = false;
  ReadNetworkUpFlags(&wifi_up, &eth_up);
  config_ap_fallback_task = nullptr;

  if (!wifi_up && !eth_up) {
    ESP_LOGW(TAG, "No network IP after %d ms, starting configuration AP", delay_ms);
    StartConfigAp();
  }
  vTaskDelete(nullptr);
}

static void StartConfigApFallbackTask(int delay_ms) {
  if (config_ap_fallback_task || fallback_ap_active) {
    return;
  }
  xTaskCreatePinnedToCore(&ConfigApFallbackTask, "cfg_ap_fb", 4096,
                          reinterpret_cast<void*>(static_cast<intptr_t>(delay_ms)), 1,
                          &config_ap_fallback_task, 0);
}

static void StopConfigApFallbackTask() {
  if (!config_ap_fallback_task) {
    return;
  }
  TaskHandle_t t = config_ap_fallback_task;
  config_ap_fallback_task = nullptr;
  vTaskDelete(t);
}

static esp_err_t EnsureWifiInit() {
  if (!wifi_event_group) {
    wifi_event_group = xEventGroupCreate();
  }
  if (wifi_inited) {
    return ESP_OK;
  }

  EnsureNetifInit();
  wifi_netif_sta = esp_netif_create_default_wifi_sta();
  wifi_netif_ap = esp_netif_create_default_wifi_ap();
  if (wifi_netif_sta && strlen(HOSTNAME) > 0) {
    esp_netif_set_hostname(wifi_netif_sta, HOSTNAME);
  }

  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  ESP_RETURN_ON_ERROR(esp_wifi_init(&cfg), TAG, "Wi-Fi init failed");
  ESP_RETURN_ON_ERROR(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &WifiEventHandler, nullptr),
                      TAG, "Wi-Fi event handler register failed");
  ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &WifiEventHandler, nullptr),
                      TAG, "Wi-Fi IP event handler register failed");
  wifi_inited = true;
  return ESP_OK;
}

static void StopWifiInterface(bool clear_ap_ip) {
  StopWifiRecoverTask();
  StopConfigApFallbackTask();
  fallback_ap_active = false;
  last_wifi_connect_attempt_us = 0;
  ErrorManagerClearLocal(ErrorCode::kWifiFallback);
  if (wifi_inited) {
    esp_wifi_stop();
    esp_wifi_set_mode(WIFI_MODE_NULL);
  }
  UpdateState([&](SharedState& s) {
    s.wifi_ip.clear();
    s.wifi_ip_sta.clear();
    if (clear_ap_ip) {
      s.wifi_ip_ap.clear();
    }
    s.wifi_rssi_dbm = -127;
    s.wifi_quality = 0;
  });
}

static bool RequestWifiConnect(const char* reason) {
  if (!wifi_inited) {
    return false;
  }
  const int64_t now_us = esp_timer_get_time();
  if (last_wifi_connect_attempt_us != 0 && now_us - last_wifi_connect_attempt_us < 2000000) {
    return false;
  }
  last_wifi_connect_attempt_us = now_us;
  const esp_err_t err = esp_wifi_connect();
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "Wi-Fi connect request failed (%s): %s", reason ? reason : "connect", esp_err_to_name(err));
    return false;
  }
  return true;
}

static void FillApConfig(wifi_config_t* ap_config, const char* ssid, const char* password) {
  if (!ap_config) {
    return;
  }
  *ap_config = {};
  std::strncpy(reinterpret_cast<char*>(ap_config->ap.ssid), ssid, sizeof(ap_config->ap.ssid) - 1);
  std::strncpy(reinterpret_cast<char*>(ap_config->ap.password), password, sizeof(ap_config->ap.password) - 1);
  ap_config->ap.ssid_len = strlen(reinterpret_cast<char*>(ap_config->ap.ssid));
  ap_config->ap.channel = 1;
  ap_config->ap.authmode = (strlen(password) >= 8) ? WIFI_AUTH_WPA_WPA2_PSK : WIFI_AUTH_OPEN;
  ap_config->ap.max_connection = 4;
}

static bool StartConfigAp() {
  StopWifiRecoverTask();
  StopConfigApFallbackTask();
  if (fallback_ap_active && wifi_inited) {
    return true;
  }

  if (!wifi_event_group) {
    wifi_event_group = xEventGroupCreate();
  } else {
    xEventGroupClearBits(wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);
  }

  if (EnsureWifiInit() != ESP_OK) {
    return false;
  }

  esp_wifi_stop();
  wifi_config_t ap_config = {};
  FillApConfig(&ap_config, "esp", "12345678");

  esp_err_t err = esp_wifi_set_mode(WIFI_MODE_AP);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to switch Wi-Fi to configuration AP: %s", esp_err_to_name(err));
    return false;
  }
  err = esp_wifi_set_config(WIFI_IF_AP, &ap_config);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to configure configuration AP: %s", esp_err_to_name(err));
    return false;
  }
  err = esp_wifi_start();
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to start configuration AP: %s", esp_err_to_name(err));
    return false;
  }
  esp_wifi_set_ps(WIFI_PS_NONE);

  fallback_ap_active = true;
  ErrorManagerSetLocal(ErrorCode::kWifiFallback, ErrorSeverity::kWarning, "Configuration AP active");
  const std::string ap_ip = GetNetifIp(wifi_netif_ap);
  ESP_LOGW(TAG, "Configuration AP active: SSID=esp password=12345678 IP=%s", ap_ip.empty() ? "192.168.4.1" : ap_ip.c_str());
  UpdateState([&](SharedState& s) {
    s.wifi_ip_ap = ap_ip.empty() ? "192.168.4.1" : ap_ip;
    s.wifi_ip = s.wifi_ip_ap;
    s.wifi_ip_sta.clear();
    s.wifi_rssi_dbm = -127;
    s.wifi_quality = 0;
  });
  return true;
}

static bool EnableFallbackAp() {
  if (app_config.wifi_ap_mode || fallback_ap_active) {
    return false;
  }

  ESP_LOGW(TAG, "Starting fallback AP and continuing STA retries");
  fallback_ap_active = true;
  ErrorManagerSetLocal(ErrorCode::kWifiFallback, ErrorSeverity::kWarning, "Fallback AP active");

  wifi_config_t ap_config = {};
  FillApConfig(&ap_config, "esp", "12345678");

  esp_err_t err = esp_wifi_disconnect();
  if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_CONNECT) {
    ESP_LOGW(TAG, "Wi-Fi disconnect before fallback AP failed: %s", esp_err_to_name(err));
  }
  err = esp_wifi_set_mode(WIFI_MODE_APSTA);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to switch Wi-Fi to APSTA fallback: %s", esp_err_to_name(err));
    fallback_ap_active = false;
    return false;
  }
  err = esp_wifi_set_config(WIFI_IF_AP, &ap_config);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to configure fallback AP: %s", esp_err_to_name(err));
    fallback_ap_active = false;
    return false;
  }

  const std::string ap_ip = GetNetifIp(wifi_netif_ap);
  UpdateState([&](SharedState& s) {
    if (!ap_ip.empty()) {
      s.wifi_ip_ap = ap_ip;
    }
  });
  return true;
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

static bool IsDataLogFilename(const char* name) {
  if (!name) return false;
  return std::strncmp(name, "data_", 5) == 0;
}

static bool IsGpsLogFilename(const char* name) {
  if (!name) return false;
  return std::strncmp(name, "gps_", 4) == 0;
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

static int MoveRootDataFilesToUploadLocked(const std::string& active_path) {
  if (!EnsureUploadDirs()) return 0;
  DIR* dir = opendir(CONFIG_MOUNT_POINT);
  if (!dir) return 0;
  const std::string active_name = Basename(active_path);
  int moved = 0;
  struct dirent* ent = nullptr;
  while ((ent = readdir(dir)) != nullptr) {
    if (ent->d_name[0] == '.') continue;
    if (ent->d_type != DT_REG && ent->d_type != DT_UNKNOWN) continue;
    if (!IsDataLogFilename(ent->d_name)) continue;
    if (!active_name.empty() && active_name == ent->d_name) continue;
    const std::string src = std::string(CONFIG_MOUNT_POINT) + "/" + ent->d_name;
    if (MoveFileToDir(src, TO_UPLOAD_DIR, nullptr)) {
      moved++;
    }
  }
  closedir(dir);
  return moved;
}

static void UpdateSdStatsLocked() {
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
  (void)MoveRootDataFilesToUploadLocked("");
  UpdateSdStatsLocked();
  ESP_LOGI(TAG, "Queued log for upload: %s", new_path.c_str());
  return true;
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
  DIR* dir = opendir(CONFIG_MOUNT_POINT);
  if (!dir) return 0;
  const std::string active_name = Basename(active_path);
  int moved = 0;
  struct dirent* ent = nullptr;
  while ((ent = readdir(dir)) != nullptr) {
    if (ent->d_name[0] == '.') continue;
    if (ent->d_type != DT_REG && ent->d_type != DT_UNKNOWN) continue;
    if (!IsGpsLogFilename(ent->d_name)) continue;
    if (!active_name.empty() && active_name == ent->d_name) continue;
    const std::string src = std::string(CONFIG_MOUNT_POINT) + "/" + ent->d_name;
    if (MoveFileToDir(src, TO_UPLOAD_DIR, nullptr)) {
      moved++;
    }
  }
  closedir(dir);
  return moved;
}

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
  if (!MoveFileToDir(current_gnss_log_path, TO_UPLOAD_DIR, nullptr)) {
    return false;
  }
  current_gnss_log_path.clear();
  gps_log_file_start_us = 0;
  ESP_LOGI(TAG, "Queued RTCM3 log for upload: %s", queued_name.c_str());
  return true;
}

static bool EnsureRtcmLogFileLocked(const GpsDateTime* frame_time) {
  if (current_gnss_log_path.empty() || gps_log_file_start_us == 0) {
    (void)MoveRootGpsFilesToUploadLocked("");
    const std::string filename = BuildGnssLogFilename(frame_time);
    current_gnss_log_path = std::string(CONFIG_MOUNT_POINT) + "/" + filename;
    gps_log_file_start_us = esp_timer_get_time();
    ESP_LOGI(TAG, "Starting RTCM3 log: %s", current_gnss_log_path.c_str());
  }

  FILE* f = fopen(current_gnss_log_path.c_str(), "ab");
  if (!f) {
    ESP_LOGE(TAG, "Failed to create RTCM3 log %s (errno %d)", current_gnss_log_path.c_str(), errno);
    return false;
  }
  const bool ok = fclose(f) == 0;
  return ok;
}

static bool WriteGnssFrameLocked(const CurrentFrame& frame) {
  constexpr uint64_t kRotateUs = 3'600'000'000ULL;

  const bool first_open = gps_log_file_start_us == 0;
  const uint64_t now_us = esp_timer_get_time();
  if (first_open || current_gnss_log_path.empty()) {
    if (!EnsureRtcmLogFileLocked(frame.timestamp.valid ? &frame.timestamp : nullptr)) {
      return false;
    }
  } else if (now_us - gps_log_file_start_us >= kRotateUs) {
    if (!QueueCurrentGnssLogForUploadLocked()) {
      return false;
    }
    const std::string filename = BuildGnssLogFilename(frame.timestamp.valid ? &frame.timestamp : nullptr);
    current_gnss_log_path = std::string(CONFIG_MOUNT_POINT) + "/" + filename;
    gps_log_file_start_us = now_us;
    ESP_LOGI(TAG, "Starting RTCM3 log: %s", current_gnss_log_path.c_str());
  }

  FILE* f = fopen(current_gnss_log_path.c_str(), "ab");
  if (!f) {
    ESP_LOGE(TAG, "Failed to open RTCM3 log %s (errno %d)", current_gnss_log_path.c_str(), errno);
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
    UpdateSdStatsLocked();
    ESP_LOGD(TAG, "GNSS frame %u appended to RTCM3 log %s", static_cast<unsigned>(frame.frame_index), current_gnss_log_path.c_str());
  } else {
    ESP_LOGE(TAG, "Failed to append GNSS frame %u to RTCM3 log", static_cast<unsigned>(frame.frame_index));
  }
  return ok;
}

static void WarnMissingGnssFrameData(const CurrentFrame& frame) {
  for (uint16_t type : app_config.gps_rtcm_types) {
    if (frame.rtcm_by_type.count(type) == 0) {
      ESP_LOGD(TAG, "GNSS frame %u missing RTCM%u", static_cast<unsigned>(frame.frame_index), static_cast<unsigned>(type));
    }
  }
}

static bool HasAnyGnssFrameData(const CurrentFrame& frame) {
  return !frame.rtcm_by_type.empty();
}

static void GpsLogTask(void*) {
  constexpr TickType_t kInterval = pdMS_TO_TICKS(30 * 1000);
  constexpr int64_t kCollectWindowUs = 35'000'000;
  uint32_t frame_index = 0;
  vTaskDelay(pdMS_TO_TICKS(5000));
  gps_client.probeReceiver();
  vTaskDelay(pdMS_TO_TICKS(1000));
  gps_client.configurePeriodicOutput(app_config.gps_rtcm_types, app_config.gps_mode);
  {
    SdLockGuard guard(pdMS_TO_TICKS(1000));
    if (guard.locked() && MountLogSd()) {
      (void)MoveRootGpsFilesToUploadLocked("");
      if (!log_file) {
        UnmountLogSd();
      }
    } else {
      ESP_LOGW(TAG, "SD unavailable, RTCM3 log file will be created on first write");
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
      ESP_LOGW(TAG, "Failed to finish GNSS frame %u", static_cast<unsigned>(frame_index));
      frame_index++;
      continue;
    }
    gps_client.stopFrameOutput();
    WarnMissingGnssFrameData(frame);
    if (!HasAnyGnssFrameData(frame)) {
      ESP_LOGW(TAG, "GNSS frame %u is empty, skip SD write", static_cast<unsigned>(frame.frame_index));
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

    {
      SdLockGuard guard(pdMS_TO_TICKS(1000));
      if (!guard.locked()) {
        ESP_LOGW(TAG, "SD mutex unavailable, skip GNSS frame write");
        frame_index++;
        vTaskDelay(kInterval);
        continue;
      }

      const bool already_mounted = log_sd_mounted;
      if (MountLogSd()) {
        (void)WriteGnssFrameLocked(frame);
        if (!already_mounted && !log_file) {
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
    ErrorManagerClear(ErrorCode::kMinioUpload);
    ESP_LOGI(TAG, "MinIO disabled, skip upload");
    return false;
  }
  if (app_config.minio_endpoint.empty() || app_config.minio_access_key.empty() || app_config.minio_secret_key.empty() ||
      app_config.minio_bucket.empty()) {
    ErrorManagerSet(ErrorCode::kMinioUpload, ErrorSeverity::kWarning, "MinIO config incomplete");
    ESP_LOGW(TAG, "MinIO config incomplete, skip upload");
    return false;
  }
  const std::string filename = Basename(path);
  std::string object_prefix = "radiometers";
  if (IsGpsLogFilename(filename.c_str())) {
    object_prefix = "gps";
  } else if (IsDataLogFilename(filename.c_str())) {
    object_prefix = "radiometers";
  }
  const std::string object_key = object_prefix + "/" + filename;
  const std::string endpoint = TrimTrailingSlash(app_config.minio_endpoint);
  const std::string host = ExtractHost(endpoint);
  const bool use_https = endpoint.rfind("https://", 0) == 0;
  if (host.empty()) {
    ErrorManagerSet(ErrorCode::kMinioUpload, ErrorSeverity::kWarning, "Invalid MinIO endpoint");
    ESP_LOGE(TAG, "Invalid MinIO endpoint: %s", endpoint.c_str());
    return false;
  }
  const std::string url = endpoint + "/" + app_config.minio_bucket + "/" + object_key;

  std::string payload_hash;
  size_t file_size = 0;
  if (!Sha256File(path, &payload_hash, &file_size)) {
    ErrorManagerSet(ErrorCode::kMinioUpload, ErrorSeverity::kWarning, "Failed to hash upload file");
    ESP_LOGE(TAG, "Failed to hash %s", path.c_str());
    return false;
  }
  // Use device_id as bucket if not set
  if (app_config.minio_bucket.empty()) {
    app_config.minio_bucket = SanitizeId(app_config.device_id);
  }

  [[maybe_unused]] auto put_bucket_if_needed = [&]() {
    // Sign empty payload
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
  // Bucket creation is intentionally not attempted on every file upload. It costs
  // an extra TLS connection per file and can exhaust RAM when old files are queued.

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
    ESP_LOGE(TAG, "Failed to hash canonical request");
    return false;
  }

  const std::string credential_scope = std::string(date_buf) + "/" + region + "/s3/aws4_request";
  const std::string string_to_sign = std::string("AWS4-HMAC-SHA256\n") + amz_date + "\n" + credential_scope + "\n" + canonical_hash_hex;

  uint8_t signing_key[32];
  if (!DeriveS3SigningKey(app_config.minio_secret_key, date_buf, region, signing_key)) {
    ErrorManagerSet(ErrorCode::kMinioUpload, ErrorSeverity::kWarning, "Failed to derive signing key");
    ESP_LOGE(TAG, "Failed to derive signing key");
    return false;
  }
  uint8_t signature_bin[32];
  if (!HmacSha256(signing_key, sizeof(signing_key),
                  reinterpret_cast<const uint8_t*>(string_to_sign.data()), string_to_sign.size(), signature_bin)) {
    ErrorManagerSet(ErrorCode::kMinioUpload, ErrorSeverity::kWarning, "Failed to sign request");
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
    ErrorManagerSet(ErrorCode::kMinioUpload, ErrorSeverity::kWarning, "esp_http_client_init failed");
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
    ErrorManagerSet(ErrorCode::kMinioUpload, ErrorSeverity::kWarning, "HTTP open failed");
    ESP_LOGE(TAG, "Failed to open HTTP connection to %s", url.c_str());
    esp_http_client_cleanup(client);
    return false;
  }

  FILE* f = fopen(path.c_str(), "rb");
  if (!f) {
    ErrorManagerSet(ErrorCode::kMinioUpload, ErrorSeverity::kWarning, "Failed to open upload file");
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
      ErrorManagerSet(ErrorCode::kMinioUpload, ErrorSeverity::kWarning, "HTTP write failed");
      ESP_LOGE(TAG, "HTTP write failed");
      ok = false;
      break;
    }
  }
  fclose(f);
  if (!ok) {
    ErrorManagerSet(ErrorCode::kMinioUpload, ErrorSeverity::kWarning, "HTTP write failed");
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return false;
  }
  esp_http_client_fetch_headers(client);
  int status = esp_http_client_get_status_code(client);
  esp_http_client_close(client);
  esp_http_client_cleanup(client);
  if (status >= 200 && status < 300) {
    ErrorManagerClear(ErrorCode::kMinioUpload);
    ESP_LOGI(TAG, "Uploaded %s to MinIO (%d)", path.c_str(), status);
    return true;
  }
  ErrorManagerSet(ErrorCode::kMinioUpload, ErrorSeverity::kWarning, "MinIO upload failed");
  ESP_LOGE(TAG, "MinIO upload failed, status %d", status);
  return false;
}

static void CleanupUploadedDirIfNeeded(int max_percent);

static bool UploadPendingOnce() {
  constexpr int kMaxSdUsagePercent = 60;
  constexpr int kMaxUploadsPerCycle = 1;
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
    if (uploaded >= kMaxUploadsPerCycle) {
      break;
    }
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
    vTaskDelay(pdMS_TO_TICKS(1000));
  }
  if (uploaded > 0) {
    ESP_LOGI(TAG, "Uploaded %d file(s) to MinIO", uploaded);
  }
  CleanupUploadedDirIfNeeded(kMaxSdUsagePercent);
  {
    SdLockGuard guard(pdMS_TO_TICKS(50));
    if (guard.locked() && MountLogSd()) {
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
    ESP_LOGW(TAG, "SD mutex unavailable, skip cleanup");
    return;
  }
  if (!MountLogSd()) {
    return;
  }
  int deleted = PurgeUploadedFiles(CONFIG_MOUNT_POINT, UPLOADED_DIR, max_percent);
  if (deleted > 0) {
    ESP_LOGI(TAG, "Deleted %d uploaded file(s) to free space", deleted);
  }
}

static void UpdateSdStats() {
  if (usb_mode == UsbMode::kMsc) {
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
  const bool already_mounted = log_sd_mounted;
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

static void SdStatsTask(void*) {
  const TickType_t interval = pdMS_TO_TICKS(10000);
  while (true) {
    UpdateSdStats();
    vTaskDelay(interval);
  }
}

void UploadTask(void*) {
  const TickType_t interval = pdMS_TO_TICKS(10 * 60 * 1000);
  vTaskDelay(pdMS_TO_TICKS(2 * 60 * 1000));  // let Wi-Fi/MQTT/GNSS settle before TLS uploads
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
  WaitForTempSensors(3000);
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
  UpdateSdStatsLocked();
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
bool ParseConfigText(const std::string& text, AppConfig* config) {
  if (!config) {
    return false;
  }

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
  bool stepper_home_offset_set = false;
  int stepper_home_offset_val = config->stepper_home_offset_steps;
  bool motor_hall_active_set = false;
  int motor_hall_active_val = config->motor_hall_active_level;
  bool pid_enabled_set = false;
  bool pid_enabled_val = false;
  bool pid_kp_set = false, pid_ki_set = false, pid_kd_set = false, pid_sp_set = false, pid_sensor_set = false;
  bool pid_mask_set = false;
  float pid_kp = pid_config.kp;
  float pid_ki = pid_config.ki;
  float pid_kd = pid_config.kd;
  float pid_sp = pid_config.setpoint;
  int pid_sensor = pid_config.sensor_index;
  uint16_t pid_mask = pid_config.sensor_mask;
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
  bool net_mode_set = false;
  NetMode net_mode_val = config->net_mode;
  bool net_priority_set = false;
  NetPriority net_priority_val = config->net_priority;
  bool gps_rtcm_types_set = false;
  std::vector<uint16_t> gps_rtcm_types_val = config->gps_rtcm_types;
  bool gps_mode_set = false;
  std::string gps_mode_val = config->gps_mode;

  size_t line_start = 0;
  while (line_start <= text.size()) {
    size_t line_end = text.find('\n', line_start);
    if (line_end == std::string::npos) {
      line_end = text.size();
    }
    std::string raw = text.substr(line_start, line_end - line_start);
    line_start = line_end + 1;
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
    } else if (key == "pid_sensor_mask") {
      pid_mask = static_cast<uint16_t>(std::strtoul(value.c_str(), nullptr, 0));
      pid_mask_set = true;
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
    } else if (key == "stepper_home_offset_steps") {
      stepper_home_offset_val = std::atoi(value.c_str());
      stepper_home_offset_set = true;
    } else if (key == "motor_hall_active_level") {
      motor_hall_active_val = std::atoi(value.c_str()) ? 1 : 0;
      motor_hall_active_set = true;
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
      mqtt_uri = NormalizeMqttUri(value);
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
    } else if (key == "net_mode") {
      if (ParseNetMode(value, &net_mode_val)) {
        net_mode_set = true;
      } else {
        ESP_LOGW(TAG, "Invalid net_mode in config.txt");
      }
    } else if (key == "net_priority") {
      if (ParseNetPriority(value, &net_priority_val)) {
        net_priority_set = true;
      } else {
        ESP_LOGW(TAG, "Invalid net_priority in config.txt");
      }
    } else if (key == "gps_rtcm_types") {
      gps_rtcm_types_val = ParseRtcmTypesString(value);
      gps_rtcm_types_set = true;
    } else if (key == "gps_mode") {
      const std::string mode = Trim(value);
      if (mode == "keep" || mode == "base_time_60" || mode == "base" || mode == "rover_uav" || mode == "rover") {
        gps_mode_val = mode;
        gps_mode_set = true;
      } else {
        ESP_LOGW(TAG, "Invalid gps_mode in config.txt");
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
  if (stepper_home_offset_set) {
    config->stepper_home_offset_steps = stepper_home_offset_val;
    UpdateState([&](SharedState& s) { s.stepper_home_offset_steps = stepper_home_offset_val; });
  }
  if (motor_hall_active_set) {
    config->motor_hall_active_level = motor_hall_active_val;
    UpdateState([&](SharedState& s) { s.motor_hall_active_level = motor_hall_active_val; });
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
  if (net_mode_set) {
    config->net_mode = net_mode_val;
  }
  if (net_priority_set) {
    config->net_priority = net_priority_val;
  }
  if (gps_rtcm_types_set) {
    config->gps_rtcm_types = gps_rtcm_types_val;
  }
  if (gps_mode_set) {
    config->gps_mode = gps_mode_val;
  }
  if (pid_kp_set || pid_ki_set || pid_kd_set || pid_sp_set || pid_sensor_set || pid_mask_set) {
    pid_config.kp = pid_kp;
    pid_config.ki = pid_ki;
    pid_config.kd = pid_kd;
    pid_config.setpoint = pid_sp;
    pid_config.sensor_index = pid_sensor;
    if (pid_mask_set) {
      pid_mask = ClampSensorMask(pid_mask, MAX_TEMP_SENSORS);
      if (pid_mask == 0) {
        pid_mask = static_cast<uint16_t>(1u << std::clamp(pid_sensor, 0, MAX_TEMP_SENSORS - 1));
      }
      pid_config.sensor_mask = pid_mask;
      pid_config.sensor_index = FirstSetBitIndex(pid_mask);
    } else if (pid_sensor_set) {
      pid_config.sensor_mask = static_cast<uint16_t>(1u << std::clamp(pid_sensor, 0, MAX_TEMP_SENSORS - 1));
    }
    pid_config.from_file = true;
  }
  if (pid_enabled_set) {
    UpdateState([&](SharedState& s) { s.pid_enabled = pid_enabled_val; });
    pid_config.from_file = true;
  }
  return config->wifi_from_file || config->usb_mass_storage_from_file || log_active_set || log_postfix_set || log_use_motor_set ||
         log_duration_set || stepper_speed_set || stepper_home_offset_set || motor_hall_active_set || device_id_set || minio_endpoint_set || minio_access_set || minio_secret_set ||
         minio_bucket_set || minio_enabled_set || mqtt_uri_set || mqtt_user_set || mqtt_password_set || mqtt_enabled_set ||
         net_mode_set || net_priority_set || gps_rtcm_types_set || gps_mode_set || pid_config.from_file;
}

bool ParseConfigFile(FILE* file, AppConfig* config) {
  if (!file || !config) {
    return false;
  }
  std::string text;
  std::array<char, 256> buf{};
  while (fgets(buf.data(), buf.size(), file)) {
    text += buf.data();
    if (text.size() > 8192) {
      ESP_LOGW(TAG, "config.txt too large, ignoring tail");
      break;
    }
  }
  return ParseConfigText(text, config);
}

bool LoadConfigTextFromInternalFlash(std::string* out) {
  if (!out) return false;
  out->clear();
  nvs_handle_t handle;
  esp_err_t err = nvs_open(CONFIG_NVS_NAMESPACE, NVS_READONLY, &handle);
  if (err != ESP_OK) {
    return false;
  }
  size_t size = 0;
  err = nvs_get_blob(handle, CONFIG_NVS_KEY, nullptr, &size);
  if (err != ESP_OK || size == 0 || size > 8192) {
    nvs_close(handle);
    return false;
  }
  std::vector<char> buf(size + 1, '\0');
  err = nvs_get_blob(handle, CONFIG_NVS_KEY, buf.data(), &size);
  nvs_close(handle);
  if (err != ESP_OK || size == 0) {
    return false;
  }
  buf[size] = '\0';
  out->assign(buf.data(), strnlen(buf.data(), size));
  return !out->empty();
}

bool LoadConfigFromInternalFlash(AppConfig* config) {
  std::string text;
  if (!config || !LoadConfigTextFromInternalFlash(&text)) {
    return false;
  }
  const bool parsed = ParseConfigText(text, config);
  if (parsed) {
    ESP_LOGI(TAG, "Config loaded from ESP internal flash NVS");
  } else {
    ESP_LOGW(TAG, "ESP internal flash config is present but invalid");
  }
  return parsed;
}

void LoadConfigFromSdCard(AppConfig* config) {
  if (!config) {
    return;
  }

  SdLockGuard guard;
  if (!guard.locked()) {
    ESP_LOGW(TAG, "SD mutex unavailable, trying ESP internal flash config");
    (void)LoadConfigFromInternalFlash(config);
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
    ESP_LOGW(TAG, "SD mount failed for config.txt: %s, trying ESP internal flash config", esp_err_to_name(ret));
    (void)LoadConfigFromInternalFlash(config);
    return;
  }

  FILE* file = fopen(CONFIG_FILE_PATH, "r");
  if (!file) {
    file = fopen("/sdcard/config.bak", "r");
    if (!file) {
      ESP_LOGW(TAG, "Config file not found at %s, trying ESP internal flash config", CONFIG_FILE_PATH);
      esp_vfs_fat_sdcard_unmount(CONFIG_MOUNT_POINT, card);
      (void)LoadConfigFromInternalFlash(config);
      return;
    }
    ESP_LOGW(TAG, "Using backup config at /sdcard/config.bak");
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
    ESP_LOGW(TAG, "config.txt present but values are missing/invalid, trying ESP internal flash config");
    if (!LoadConfigFromInternalFlash(config)) {
      config->wifi_ssid = DEFAULT_WIFI_SSID;
      config->wifi_password = DEFAULT_WIFI_PASS;
    }
  }
}

void WifiEventHandler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
  if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
    RequestWifiConnect("sta_start");
  } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
    bool eth_up = false;
    ReadNetworkUpFlags(nullptr, &eth_up);
    if (app_config.net_mode == NetMode::kWifiEth && app_config.net_priority == NetPriority::kEth && eth_up) {
      ESP_LOGI(TAG, "Ignoring Wi-Fi disconnect because Ethernet is preferred and up");
      StopWifiRecoverTask();
      UpdateState([&](SharedState& s) {
        s.wifi_ip.clear();
        s.wifi_ip_sta.clear();
      });
      return;
    }
    std::string reason_msg = "Wi-Fi disconnected";
    if (event_data) {
      auto* info = static_cast<wifi_event_sta_disconnected_t*>(event_data);
      reason_msg += " reason=" + std::to_string(info->reason);
    }
    ErrorManagerSetLocal(ErrorCode::kWifiDisconnected, ErrorSeverity::kWarning, reason_msg);
    UpdateState([&](SharedState& s) {
      s.wifi_ip.clear();
      s.wifi_ip_sta.clear();
    });
    UpdateDefaultNetif();
    if (retry_count < 5) {
      if (RequestWifiConnect("event_retry")) {
        retry_count++;
        ESP_LOGW(TAG, "Retry Wi-Fi connection (%d)", retry_count);
      }
    } else {
      xEventGroupSetBits(wifi_event_group, WIFI_FAIL_BIT);
      EnableFallbackAp();
      StartWifiRecoverTask(15000);
    }
  } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
    retry_count = 0;
    last_wifi_connect_attempt_us = 0;
    ErrorManagerClearLocal(ErrorCode::kWifiDisconnected);
    auto* event = static_cast<ip_event_got_ip_t*>(event_data);
    ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
    const std::string sta_ip = FormatIp4(event->ip_info.ip);
    const std::string ap_ip = GetNetifIp(wifi_netif_ap);
    UpdateState([&](SharedState& s) {
      s.wifi_ip = sta_ip;
      s.wifi_ip_sta = sta_ip;
      if (!ap_ip.empty()) {
        s.wifi_ip_ap = ap_ip;
      }
    });
    UpdateDefaultNetif();
    xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
    StopWifiRecoverTask();
    if (fallback_ap_active && !app_config.wifi_ap_mode) {
      ESP_LOGI(TAG, "Disabling fallback AP after reconnect");
      fallback_ap_active = false;
      ErrorManagerClearLocal(ErrorCode::kWifiFallback);
      esp_wifi_set_mode(WIFI_MODE_STA);
    }
  }
}

void WifiMonitorTask(void*) {
  while (true) {
    wifi_ap_record_t ap{};
    esp_err_t err = esp_wifi_sta_get_ap_info(&ap);
    int rssi = (err == ESP_OK) ? ap.rssi : -127;
    int quality = (err == ESP_OK) ? RssiToQuality(rssi) : 0;
    UpdateState([&](SharedState& s) {
      s.wifi_rssi_dbm = rssi;
      s.wifi_quality = quality;
    });
    vTaskDelay(pdMS_TO_TICKS(5000));
  }
}

void InitWifi(const std::string& ssid, const std::string& password, bool ap_mode) {
  StopConfigApFallbackTask();
  StopWifiRecoverTask();
  fallback_ap_active = false;
  last_wifi_connect_attempt_us = 0;

  if (!wifi_event_group) {
    wifi_event_group = xEventGroupCreate();
  } else {
    xEventGroupClearBits(wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);
  }

  const bool was_wifi_inited = wifi_inited;
  ESP_ERROR_CHECK(EnsureWifiInit());
  if (was_wifi_inited) {
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
  FillApConfig(&ap_config, ap_ssid, ap_pass);

  wifi_mode_t mode = ap_mode ? WIFI_MODE_APSTA : WIFI_MODE_STA;
  sta_cfg_cached = wifi_config;
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
  const std::string ap_ip = GetNetifIp(wifi_netif_ap);
  UpdateState([&](SharedState& s) {
    if (!ap_ip.empty()) {
      s.wifi_ip_ap = ap_ip;
      if (ap_mode) {
        s.wifi_ip = ap_ip;
      }
    }
  });

  EventBits_t bits = xEventGroupWaitBits(
      wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT, pdFALSE, pdFALSE, pdMS_TO_TICKS(15'000));

  if (bits & WIFI_CONNECTED_BIT) {
    ESP_LOGI(TAG, "Connected to SSID:%s", ssid.c_str());
  } else {
    ESP_LOGW(TAG, "Failed to connect to SSID:%s, starting STA retries", ssid.c_str());
    xEventGroupSetBits(wifi_event_group, WIFI_FAIL_BIT);
    EnableFallbackAp();
    StartWifiRecoverTask(15000);
  }
}

void StartSntp() {
  if (sntp_started) {
    return;
  }
  sntp_started = true;
  esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
  esp_sntp_setservername(0, "pool.ntp.org");
  esp_sntp_init();
}

bool WaitForTimeSyncMs(int timeout_ms) {
  const int64_t deadline_us = esp_timer_get_time() + static_cast<int64_t>(timeout_ms) * 1000;
  while (esp_timer_get_time() < deadline_us) {
    if (sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED) {
      time_t now = 0;
      time(&now);
      if (now > kValidUtcThreshold) {
        MarkSntpSynced();
        return true;
      }
    }
    vTaskDelay(pdMS_TO_TICKS(200));
  }
  MarkSntpUnavailableIfNoNetwork();
  return false;
}

bool EnsureTimeSynced(int timeout_ms) {
  if (time_synced) return true;
  return WaitForTimeSyncMs(timeout_ms);
}

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
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_cfg, &i2c_bus), TAG, "I2C bus init failed");
  }

  if (!ina219_dev) {
    i2c_device_config_t dev_cfg = {};
    dev_cfg.device_address = INA219_ADDR;
    dev_cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dev_cfg.scl_speed_hz = INA219_I2C_FREQ_HZ;
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(i2c_bus, &dev_cfg, &ina219_dev), TAG, "INA219 attach failed");
  }

  auto write_reg = [](uint8_t reg, uint16_t value) -> esp_err_t {
    uint8_t payload[3] = {
        reg,
        static_cast<uint8_t>((value >> 8) & 0xFF),
        static_cast<uint8_t>(value & 0xFF),
    };
    return i2c_master_transmit(ina219_dev, payload, sizeof(payload), INA219_I2C_TIMEOUT);
  };

  ESP_RETURN_ON_ERROR(write_reg(0x00, INA219_CONFIG), TAG, "INA219 config failed");
  ESP_RETURN_ON_ERROR(write_reg(0x05, INA219_CALIBRATION), TAG, "INA219 calibration failed");
  ESP_LOGI(TAG, "INA219 initialized");
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
    ESP_LOGW(TAG, "ADC2 read failed: %s", esp_err_to_name(err));
    ErrorManagerSet(ErrorCode::kAdcRead, ErrorSeverity::kError, "ADC2 read failed");
    return err;
  }
  err = adc3.Read(&raw3);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "ADC3 read failed: %s", esp_err_to_name(err));
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
        ESP_LOGW(TAG, "INA219 read failed: %s (consecutive=%d)", esp_err_to_name(err), consecutive_failures);
        last_log_us = now_us;
      }
      if (consecutive_failures >= 3 && now_us - last_reinit_us > 10'000'000) {
        last_reinit_us = now_us;
        ESP_LOGW(TAG, "Reinitializing INA219 after I2C failures");
        esp_err_t init_err = InitIna219();
        if (init_err != ESP_OK) {
          ESP_LOGW(TAG, "INA219 reinit failed: %s", esp_err_to_name(init_err));
        }
      }
    } else if (consecutive_failures > 0) {
      ESP_LOGI(TAG, "INA219 recovered after %d failed read(s)", consecutive_failures);
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
        ESP_LOGD(TAG, "Temps (%d):", count);
        for (int i = 0; i < count; ++i) {
          ESP_LOGD(TAG, "  Sensor %d: %.2f C", i + 1, temps[i]);
        }
      }
    } else {
      ESP_LOGW(TAG, "M1820ReadTemperatures failed");
      ErrorManagerSet(ErrorCode::kTempSensor, ErrorSeverity::kWarning, "M1820 read failed");
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
  const uint32_t boot_id = LoadAndIncrementBootId();
  ESP_LOGI(TAG, "Boot ID: %u", static_cast<unsigned>(boot_id));

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

  InitGpios();
  ErrorManagerInit();
  ErrorManagerSetPublisher(&PublishErrorPayload);

  if (usb_mode == UsbMode::kMsc) {
    esp_err_t msc_err = InitSdCardForMsc(&sd_card);
    if (msc_err == ESP_OK) {
      msc_err = StartUsbMsc(sd_card);
    }
    if (msc_err != ESP_OK) {
      msc_ok = false;
      ESP_LOGE(TAG, "USB MSC init failed, fallback to CDC mode: %s", esp_err_to_name(msc_err));
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
    ESP_LOGW(TAG, "M1820 init failed or no sensors found");
    ErrorManagerSet(ErrorCode::kTempSensor, ErrorSeverity::kError, "M1820 init failed");
    init_ok = false;
  }
  ESP_ERROR_CHECK(InitSpiBus());
  ESP_ERROR_CHECK(adc1.Init(SPI2_HOST, ADC_SPI_FREQ_HZ));
  ESP_ERROR_CHECK(adc2.Init(SPI2_HOST, ADC_SPI_FREQ_HZ));
  ESP_ERROR_CHECK(adc3.Init(SPI2_HOST, ADC_SPI_FREQ_HZ));
  esp_err_t ina_err = InitIna219();
  if (ina_err != ESP_OK) {
    ESP_LOGE(TAG, "INA219 init failed: %s", esp_err_to_name(ina_err));
    ErrorManagerSet(ErrorCode::kInaInit, ErrorSeverity::kError, "INA219 init failed");
    init_ok = false;
  } else {
    ErrorManagerClear(ErrorCode::kInaInit);
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
  if (WaitForTimeSyncMs(8000)) {
    ESP_LOGI(TAG, "Time synced via NTP");
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
      ESP_LOGW(TAG, "NTP sync timed out, using GPZDA UTC time");
      ErrorManagerClear(ErrorCode::kTimeSync);
    } else if (fallback_time.source == UtcTimeSource::kSystemCached && fallback_time.valid) {
      ESP_LOGW(TAG, "NTP sync timed out, using cached system UTC time");
      ErrorManagerClear(ErrorCode::kTimeSync);
    } else {
      ESP_LOGW(TAG, "NTP sync timed out, using monotonic timestamp fallback");
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
  xTaskCreatePinnedToCore(&NetworkMonitorTask, "net_mon", 5120, nullptr, 1, nullptr, 0);
  if (upload_task == nullptr) {
    // Upload task uses esp_http_client and std::string, needs a bit more stack to avoid overflow.
    xTaskCreatePinnedToCore(&UploadTask, "upload_task", 12288, nullptr, 1, &upload_task, 0);
  }
  if (gps_log_task == nullptr && usb_mode == UsbMode::kCdc) {
    xTaskCreatePinnedToCore(&GpsLogTask, "gps_log", 6144, nullptr, 1, &gps_log_task, 0);
  }
  xTaskCreatePinnedToCore(&WifiMonitorTask, "wifi_mon", 2048, nullptr, 1, nullptr, 0);
  xTaskCreatePinnedToCore(&SdStatsTask, "sd_stats", 3072, nullptr, 1, nullptr, 0);

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
        s.log_use_motor = log_config.use_motor;
        s.log_duration_s = log_config.duration_s;
      });
    }
  }

  init_ok = init_ok && msc_ok && (ina_err == ESP_OK);
  ESP_LOGI(TAG, "System ready");

  // Start MQTT after init (non-blocking)
  StartMqttBridge();
}
