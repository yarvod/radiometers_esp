#include "config_loader.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdarg>
#include <unistd.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "app_utils.h"
#include "driver/sdmmc_host.h"
#include "storage_manager.h"
#include "error_manager.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "hw_pins.h"
#include "nvs.h"
#include "sdmmc_cmd.h"

static constexpr char kTag[] = "CFG";

// ---------------------------------------------------------------------------
// Private helpers
// ---------------------------------------------------------------------------

static std::vector<uint16_t> ParseRtcmTypesString(const std::string& value) {
  std::vector<uint16_t> out;
  size_t start = 0;
  while (start < value.size()) {
    while (start < value.size() &&
           (value[start] == ',' || value[start] == ' ' || value[start] == ';' || value[start] == '\t')) {
      start++;
    }
    if (start >= value.size()) break;
    size_t end = start;
    while (end < value.size() && value[end] != ',' && value[end] != ';' &&
           !std::isspace(static_cast<unsigned char>(value[end]))) {
      end++;
    }
    const int type = std::atoi(value.substr(start, end - start).c_str());
    if (type > 0 && type <= 4095 &&
        std::find(out.begin(), out.end(), static_cast<uint16_t>(type)) == out.end()) {
      out.push_back(static_cast<uint16_t>(type));
    }
    start = end;
  }
  if (out.empty()) out = {1004, 1006, 1033};
  return out;
}

static bool SaveConfigTextToInternalFlash(const std::string& text) {
  if (text.empty() || text.size() > 8192) {
    ESP_LOGE(kTag, "Internal config save rejected, size=%u", static_cast<unsigned>(text.size()));
    return false;
  }
  nvs_handle_t handle;
  esp_err_t err = nvs_open(CONFIG_NVS_NAMESPACE, NVS_READWRITE, &handle);
  if (err != ESP_OK) {
    ESP_LOGE(kTag, "NVS open for config failed: %s", esp_err_to_name(err));
    return false;
  }
  err = nvs_set_blob(handle, CONFIG_NVS_KEY, text.c_str(), text.size() + 1);
  if (err == ESP_OK) err = nvs_commit(handle);
  nvs_close(handle);
  if (err != ESP_OK) {
    ESP_LOGE(kTag, "NVS config save failed: %s", esp_err_to_name(err));
    return false;
  }
  ESP_LOGI(kTag, "Config synced to ESP internal flash");
  return true;
}

static void AppendConfigLine(std::string* out, const char* fmt, ...) {
  if (!out || !fmt) return;
  char buf[256];
  va_list args;
  va_start(args, fmt);
  const int n = std::vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  if (n <= 0) return;
  if (static_cast<size_t>(n) < sizeof(buf)) {
    out->append(buf, static_cast<size_t>(n));
    return;
  }
  std::vector<char> dyn(static_cast<size_t>(n) + 1);
  va_start(args, fmt);
  std::vsnprintf(dyn.data(), dyn.size(), fmt, args);
  va_end(args);
  out->append(dyn.data(), static_cast<size_t>(n));
}

// ---------------------------------------------------------------------------
// ParseConfigText — parses config.txt key=value text into *config.
// Side-effects: also writes pid_config and log_config globals.
// ---------------------------------------------------------------------------
bool ParseConfigText(const std::string& text, AppConfig* config) {
  if (!config) return false;

  bool ssid_set = false, pass_set = false;
  bool wifi_ap_mode_set = false, wifi_ap_mode_val = false;
  bool log_active_set = false, log_active_val = false;
  bool log_postfix_set = false;
  std::string log_postfix;
  bool log_use_motor_set = false, log_use_motor_val = false;
  bool log_duration_set = false;
  float log_duration_val = log_config.duration_s;
  bool logging_motor_steps_set = false;
  int logging_motor_steps_val = config->logging_motor_steps;
  bool logging_home_each_cycle_set = false;
  bool logging_home_each_cycle_val = config->logging_home_each_cycle;
  bool storage_backend_set = false;
  StorageBackend storage_backend_val = config->storage_backend;
  bool stepper_speed_set = false;
  int stepper_speed_val = config->stepper_speed_us;
  bool stepper_home_offset_set = false;
  int stepper_home_offset_val = config->stepper_home_offset_steps;
  bool motor_hall_active_set = false;
  int motor_hall_active_val = config->motor_hall_active_level;
  bool pid_enabled_set = false, pid_enabled_val = false;
  bool pid_kp_set = false, pid_ki_set = false, pid_kd_set = false, pid_sp_set = false;
  bool pid_sensor_set = false, pid_mask_set = false;
  float pid_kp = pid_config.kp, pid_ki = pid_config.ki;
  float pid_kd = pid_config.kd, pid_sp = pid_config.setpoint;
  int pid_sensor = pid_config.sensor_index;
  uint16_t pid_mask = pid_config.sensor_mask;
  std::string ssid, password;
  std::string device_id = config->device_id;
  bool device_id_set = false;
  std::string minio_endpoint = config->minio_endpoint;
  std::string minio_access = config->minio_access_key;
  std::string minio_secret = config->minio_secret_key;
  std::string minio_bucket = config->minio_bucket;
  bool minio_endpoint_set = false, minio_access_set = false;
  bool minio_secret_set = false, minio_bucket_set = false;
  bool minio_enabled_val = config->minio_enabled, minio_enabled_set = false;
  std::string mqtt_uri = config->mqtt_uri;
  std::string mqtt_user = config->mqtt_user;
  std::string mqtt_password = config->mqtt_password;
  bool mqtt_uri_set = false, mqtt_user_set = false, mqtt_password_set = false;
  bool mqtt_enabled_val = config->mqtt_enabled, mqtt_enabled_set = false;
  bool net_mode_set = false;
  NetMode net_mode_val = config->net_mode;
  bool net_priority_set = false;
  NetPriority net_priority_val = config->net_priority;
  bool eth_dhcp_set = false, eth_dhcp_val = config->eth_dhcp;
  bool eth_static_ip_set = false, eth_static_netmask_set = false, eth_static_gateway_set = false, eth_static_dns_set = false;
  std::string eth_static_ip = config->eth_static_ip;
  std::string eth_static_netmask = config->eth_static_netmask;
  std::string eth_static_gateway = config->eth_static_gateway;
  std::string eth_static_dns = config->eth_static_dns;
  bool gps_rtcm_types_set = false;
  std::vector<uint16_t> gps_rtcm_types_val = config->gps_rtcm_types;
  bool gps_mode_set = false;
  std::string gps_mode_val = config->gps_mode;
  bool meteo_poll_interval_set = false;
  int meteo_poll_interval_val = config->meteo_poll_interval_s;
  bool meteo_file_interval_set = false;
  int meteo_file_interval_val = config->meteo_file_interval_s;
  bool meteo_enabled_set = false;
  bool meteo_enabled_val = config->meteo_enabled;

  size_t line_start = 0;
  while (line_start <= text.size()) {
    size_t line_end = text.find('\n', line_start);
    if (line_end == std::string::npos) line_end = text.size();
    std::string raw = text.substr(line_start, line_end - line_start);
    line_start = line_end + 1;
    std::string trimmed = Trim(raw);
    if (trimmed.empty() || trimmed[0] == '#') continue;
    const size_t eq = trimmed.find('=');
    if (eq == std::string::npos) continue;
    std::string key = Trim(trimmed.substr(0, eq));
    std::string value = Trim(trimmed.substr(eq + 1));

    if (key == "wifi_ssid") {
      if (!value.empty() && value.size() < WIFI_SSID_MAX_LEN) { ssid = value; ssid_set = true; }
      else ESP_LOGW(kTag, "Invalid wifi_ssid in config.txt");
    } else if (key == "wifi_password") {
      if (value.size() >= 8 && value.size() < WIFI_PASSWORD_MAX_LEN) { password = value; pass_set = true; }
      else ESP_LOGW(kTag, "Invalid wifi_password in config.txt");
    } else if (key == "wifi_ap_mode") {
      if (ParseBool(value, &wifi_ap_mode_val)) wifi_ap_mode_set = true;
    } else if (key == "pid_kp") {
      pid_kp = std::strtof(value.c_str(), nullptr); pid_kp_set = true;
    } else if (key == "pid_ki") {
      pid_ki = std::strtof(value.c_str(), nullptr); pid_ki_set = true;
    } else if (key == "pid_kd") {
      pid_kd = std::strtof(value.c_str(), nullptr); pid_kd_set = true;
    } else if (key == "pid_setpoint") {
      pid_sp = std::strtof(value.c_str(), nullptr); pid_sp_set = true;
    } else if (key == "pid_sensor") {
      pid_sensor = std::atoi(value.c_str()); pid_sensor_set = true;
    } else if (key == "pid_sensor_mask") {
      pid_mask = static_cast<uint16_t>(std::strtoul(value.c_str(), nullptr, 0)); pid_mask_set = true;
    } else if (key == "pid_enabled") {
      if (ParseBool(value, &pid_enabled_val)) pid_enabled_set = true;
    } else if (key == "logging_active") {
      if (ParseBool(value, &log_active_val)) log_active_set = true;
    } else if (key == "logging_postfix") {
      log_postfix = value; log_postfix_set = true;
    } else if (key == "logging_use_motor") {
      if (ParseBool(value, &log_use_motor_val)) log_use_motor_set = true;
    } else if (key == "logging_duration_s") {
      log_duration_val = std::strtof(value.c_str(), nullptr);
      log_duration_set = (log_duration_val > 0.0f);
    } else if (key == "logging_motor_steps") {
      logging_motor_steps_val = std::atoi(value.c_str());
      if (logging_motor_steps_val > 0) logging_motor_steps_set = true;
      else ESP_LOGW(kTag, "Invalid logging_motor_steps in config.txt");
    } else if (key == "logging_home_each_cycle") {
      if (ParseBool(value, &logging_home_each_cycle_val)) logging_home_each_cycle_set = true;
      else ESP_LOGW(kTag, "Invalid logging_home_each_cycle in config.txt");
    } else if (key == "storage_backend") {
      if (ParseStorageBackend(value, &storage_backend_val)) storage_backend_set = true;
      else ESP_LOGW(kTag, "Invalid storage_backend in config.txt");
    } else if (key == "stepper_speed_us") {
      stepper_speed_val = std::atoi(value.c_str());
      if (stepper_speed_val > 0) stepper_speed_set = true;
      else ESP_LOGW(kTag, "Invalid stepper_speed_us in config.txt");
    } else if (key == "stepper_home_offset_steps") {
      stepper_home_offset_val = std::atoi(value.c_str()); stepper_home_offset_set = true;
    } else if (key == "motor_hall_active_level") {
      motor_hall_active_val = std::atoi(value.c_str()) ? 1 : 0; motor_hall_active_set = true;
    } else if (key == "device_id") {
      if (!value.empty()) { device_id = value; device_id_set = true; }
    } else if (key == "minio_endpoint") {
      minio_endpoint = value; minio_endpoint_set = true;
    } else if (key == "minio_access_key") {
      minio_access = value; minio_access_set = true;
    } else if (key == "minio_secret_key") {
      minio_secret = value; minio_secret_set = true;
    } else if (key == "minio_bucket") {
      minio_bucket = value; minio_bucket_set = true;
    } else if (key == "minio_enabled") {
      if (ParseBool(value, &minio_enabled_val)) minio_enabled_set = true;
    } else if (key == "mqtt_uri") {
      mqtt_uri = NormalizeMqttUri(value); mqtt_uri_set = true;
    } else if (key == "mqtt_user") {
      mqtt_user = value; mqtt_user_set = true;
    } else if (key == "mqtt_password") {
      mqtt_password = value; mqtt_password_set = true;
    } else if (key == "mqtt_enabled") {
      if (ParseBool(value, &mqtt_enabled_val)) mqtt_enabled_set = true;
    } else if (key == "net_mode") {
      if (ParseNetMode(value, &net_mode_val)) net_mode_set = true;
      else ESP_LOGW(kTag, "Invalid net_mode in config.txt");
    } else if (key == "net_priority") {
      if (ParseNetPriority(value, &net_priority_val)) net_priority_set = true;
      else ESP_LOGW(kTag, "Invalid net_priority in config.txt");
    } else if (key == "eth_dhcp") {
      if (ParseBool(value, &eth_dhcp_val)) eth_dhcp_set = true;
    } else if (key == "eth_static_ip") {
      eth_static_ip = value; eth_static_ip_set = true;
    } else if (key == "eth_static_netmask") {
      eth_static_netmask = value; eth_static_netmask_set = true;
    } else if (key == "eth_static_gateway") {
      eth_static_gateway = value; eth_static_gateway_set = true;
    } else if (key == "eth_static_dns") {
      eth_static_dns = value; eth_static_dns_set = true;
    } else if (key == "gps_rtcm_types") {
      gps_rtcm_types_val = ParseRtcmTypesString(value); gps_rtcm_types_set = true;
    } else if (key == "gps_mode") {
      const std::string mode = Trim(value);
      if (mode == "keep" || mode == "base_time_60" || mode == "base" ||
          mode == "rover_uav" || mode == "rover") {
        gps_mode_val = mode; gps_mode_set = true;
      } else {
        ESP_LOGW(kTag, "Invalid gps_mode in config.txt");
      }
    } else if (key == "meteo_poll_interval_s") {
      const int v = std::atoi(value.c_str());
      if (v >= 1 && v <= 3600) {
        meteo_poll_interval_val = v;
        meteo_poll_interval_set = true;
      }
    } else if (key == "meteo_file_interval_s") {
      const int v = std::atoi(value.c_str());
      if (v >= 10 && v <= 86400) {
        meteo_file_interval_val = v;
        meteo_file_interval_set = true;
      }
    } else if (key == "meteo_enabled") {
      if (ParseBool(value, &meteo_enabled_val)) meteo_enabled_set = true;
    }
  }

  if (ssid_set && pass_set) { config->wifi_ssid = ssid; config->wifi_password = password; config->wifi_from_file = true; }
  if (wifi_ap_mode_set) { config->wifi_ap_mode = wifi_ap_mode_val; config->wifi_from_file = true; }
  if (log_active_set) config->logging_active = log_active_val;
  if (log_postfix_set) config->logging_postfix = log_postfix;
  if (log_use_motor_set) { config->logging_use_motor = log_use_motor_val; log_config.use_motor = log_use_motor_val; }
  if (log_duration_set) { config->logging_duration_s = log_duration_val; log_config.duration_s = log_duration_val; }
  if (logging_motor_steps_set) config->logging_motor_steps = std::clamp(logging_motor_steps_val, 1, 20000);
  if (logging_home_each_cycle_set) config->logging_home_each_cycle = logging_home_each_cycle_val;
  if (storage_backend_set) config->storage_backend = storage_backend_val;
  if (stepper_speed_set) { config->stepper_speed_us = stepper_speed_val; UpdateState([&](SharedState& s) { s.stepper_speed_us = stepper_speed_val; }); }
  if (stepper_home_offset_set) { config->stepper_home_offset_steps = stepper_home_offset_val; UpdateState([&](SharedState& s) { s.stepper_home_offset_steps = stepper_home_offset_val; }); }
  if (motor_hall_active_set) { config->motor_hall_active_level = motor_hall_active_val; UpdateState([&](SharedState& s) { s.motor_hall_active_level = motor_hall_active_val; }); }
  if (device_id_set) config->device_id = device_id;
  if (minio_endpoint_set) config->minio_endpoint = minio_endpoint;
  if (minio_access_set) config->minio_access_key = minio_access;
  if (minio_secret_set) config->minio_secret_key = minio_secret;
  if (minio_bucket_set) config->minio_bucket = minio_bucket;
  if (minio_enabled_set) config->minio_enabled = minio_enabled_val;
  if (mqtt_uri_set) config->mqtt_uri = mqtt_uri;
  if (mqtt_user_set) config->mqtt_user = mqtt_user;
  if (mqtt_password_set) config->mqtt_password = mqtt_password;
  if (mqtt_enabled_set) config->mqtt_enabled = mqtt_enabled_val;
  if (net_mode_set) config->net_mode = net_mode_val;
  if (net_priority_set) config->net_priority = net_priority_val;
  if (eth_dhcp_set) config->eth_dhcp = eth_dhcp_val;
  if (eth_static_ip_set) config->eth_static_ip = eth_static_ip;
  if (eth_static_netmask_set) config->eth_static_netmask = eth_static_netmask;
  if (eth_static_gateway_set) config->eth_static_gateway = eth_static_gateway;
  if (eth_static_dns_set) config->eth_static_dns = eth_static_dns;
  if (gps_rtcm_types_set) config->gps_rtcm_types = gps_rtcm_types_val;
  if (gps_mode_set) config->gps_mode = gps_mode_val;
  if (meteo_poll_interval_set) config->meteo_poll_interval_s = meteo_poll_interval_val;
  if (meteo_file_interval_set) config->meteo_file_interval_s = meteo_file_interval_val;
  if (meteo_enabled_set) config->meteo_enabled = meteo_enabled_val;
  if (pid_kp_set || pid_ki_set || pid_kd_set || pid_sp_set || pid_sensor_set || pid_mask_set) {
    pid_config.kp = pid_kp; pid_config.ki = pid_ki; pid_config.kd = pid_kd;
    pid_config.setpoint = pid_sp; pid_config.sensor_index = pid_sensor;
    if (pid_mask_set) {
      pid_mask = ClampSensorMask(pid_mask, MAX_TEMP_SENSORS);
      if (pid_mask == 0) pid_mask = static_cast<uint16_t>(1u << std::clamp(pid_sensor, 0, MAX_TEMP_SENSORS - 1));
      pid_config.sensor_mask = pid_mask;
      pid_config.sensor_index = FirstSetBitIndex(pid_mask);
    } else if (pid_sensor_set) {
      pid_config.sensor_mask = static_cast<uint16_t>(1u << std::clamp(pid_sensor, 0, MAX_TEMP_SENSORS - 1));
    }
    pid_config.from_file = true;
  }
  if (pid_enabled_set) { UpdateState([&](SharedState& s) { s.pid_enabled = pid_enabled_val; }); pid_config.from_file = true; }

  return config->wifi_from_file || log_active_set ||
         log_postfix_set || log_use_motor_set || log_duration_set || logging_motor_steps_set ||
         logging_home_each_cycle_set || storage_backend_set || stepper_speed_set ||
         stepper_home_offset_set || motor_hall_active_set || device_id_set ||
         minio_endpoint_set || minio_access_set || minio_secret_set || minio_bucket_set ||
         minio_enabled_set || mqtt_uri_set || mqtt_user_set || mqtt_password_set ||
         mqtt_enabled_set || net_mode_set || net_priority_set || eth_dhcp_set ||
         eth_static_ip_set || eth_static_netmask_set || eth_static_gateway_set || eth_static_dns_set || gps_rtcm_types_set ||
         gps_mode_set || meteo_poll_interval_set || meteo_file_interval_set ||
         meteo_enabled_set || pid_config.from_file;
}

bool ParseConfigFile(FILE* file, AppConfig* config) {
  if (!file || !config) return false;
  std::string text;
  std::array<char, 256> buf{};
  while (fgets(buf.data(), buf.size(), file)) {
    text += buf.data();
    if (text.size() > 8192) { ESP_LOGW(kTag, "config.txt too large, ignoring tail"); break; }
  }
  return ParseConfigText(text, config);
}

// ---------------------------------------------------------------------------
// NVS (internal flash)
// ---------------------------------------------------------------------------

bool LoadConfigTextFromInternalFlash(std::string* out) {
  if (!out) return false;
  out->clear();
  nvs_handle_t handle;
  esp_err_t err = nvs_open(CONFIG_NVS_NAMESPACE, NVS_READONLY, &handle);
  if (err != ESP_OK) return false;
  size_t size = 0;
  err = nvs_get_blob(handle, CONFIG_NVS_KEY, nullptr, &size);
  if (err != ESP_OK || size == 0 || size > 8192) { nvs_close(handle); return false; }
  std::vector<char> buf(size + 1, '\0');
  err = nvs_get_blob(handle, CONFIG_NVS_KEY, buf.data(), &size);
  nvs_close(handle);
  if (err != ESP_OK || size == 0) return false;
  buf[size] = '\0';
  out->assign(buf.data(), strnlen(buf.data(), size));
  return !out->empty();
}

bool LoadConfigFromInternalFlash(AppConfig* config) {
  std::string text;
  if (!config || !LoadConfigTextFromInternalFlash(&text)) return false;
  const bool parsed = ParseConfigText(text, config);
  if (parsed) ESP_LOGI(kTag, "Config loaded from ESP internal flash NVS");
  else        ESP_LOGW(kTag, "ESP internal flash config is present but invalid");
  return parsed;
}

bool SaveConfigToInternalFlash(const AppConfig& cfg, const PidConfig& pid) {
  return SaveConfigTextToInternalFlash(BuildConfigText(cfg, pid));
}

bool SyncConfigToInternalFlash() {
  return SaveConfigToInternalFlash(app_config, pid_config);
}

// ---------------------------------------------------------------------------
// SD card
// ---------------------------------------------------------------------------

void LoadConfigFromSdCard(AppConfig* config) {
  if (!config) return;

  SdLockGuard guard;
  if (!guard.locked()) {
    ESP_LOGW(kTag, "SD mutex unavailable, trying ESP internal flash config");
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

  esp_err_t ret = esp_vfs_fat_sdmmc_mount(CONFIG_MOUNT_POINT, &host, &slot_config, &mount_config, &card);
  if (ret != ESP_OK) {
    ESP_LOGW(kTag, "SD mount failed for config.txt: %s, trying ESP internal flash config", esp_err_to_name(ret));
    (void)LoadConfigFromInternalFlash(config);
    return;
  }

  FILE* file = fopen(CONFIG_FILE_PATH, "r");
  if (!file) {
    file = fopen("/sdcard/config.bak", "r");
    if (!file) {
      ESP_LOGW(kTag, "Config file not found at %s, trying ESP internal flash config", CONFIG_FILE_PATH);
      esp_vfs_fat_sdcard_unmount(CONFIG_MOUNT_POINT, card);
      (void)LoadConfigFromInternalFlash(config);
      return;
    }
    ESP_LOGW(kTag, "Using backup config at /sdcard/config.bak");
  }

  const bool parsed = ParseConfigFile(file, config);
  fclose(file);
  esp_vfs_fat_sdcard_unmount(CONFIG_MOUNT_POINT, card);

  if (parsed) {
    if (config->wifi_from_file) {
      ESP_LOGI(kTag, "Wi-Fi config loaded from config.txt (SSID: %s)", config->wifi_ssid.c_str());
    } else {
      config->wifi_ssid = DEFAULT_WIFI_SSID;
      config->wifi_password = DEFAULT_WIFI_PASS;
    }
  } else {
    ESP_LOGW(kTag, "config.txt present but values are missing/invalid, trying ESP internal flash config");
    if (!LoadConfigFromInternalFlash(config)) {
      config->wifi_ssid = DEFAULT_WIFI_SSID;
      config->wifi_password = DEFAULT_WIFI_PASS;
    }
  }
}

ConfigSaveResult SaveConfigEverywhere(const AppConfig& cfg, const PidConfig& pid) {
  const std::string config_text = BuildConfigText(cfg, pid);
  const bool internal_saved = SaveConfigTextToInternalFlash(config_text);
  SdLockGuard guard;
  if (!guard.locked()) {
    ESP_LOGW(kTag, "SD mutex unavailable, config saved only to ESP internal flash");
    ErrorManagerSet(ErrorCode::kSdMutex, ErrorSeverity::kWarning, "SD mutex unavailable during config save");
    return {internal_saved, false};
  }
  ErrorManagerClear(ErrorCode::kSdMutex);
  const bool already_mounted = IsLogSdMounted();
  if (!already_mounted) {
    if (!MountLogSd()) {
      ESP_LOGW(kTag, "SD unavailable, config saved only to ESP internal flash");
      return {internal_saved, false};
    }
  }
  const char* tmp_path    = "/sdcard/config.tmp";
  const char* backup_path = "/sdcard/config.bak";
  FILE* f = fopen(tmp_path, "w");
  if (!f) {
    ESP_LOGE(kTag, "Failed to open %s for writing", tmp_path);
    if (!already_mounted && !log_file) UnmountLogSd();
    return {internal_saved, false};
  }
  if (fwrite(config_text.data(), 1, config_text.size(), f) != config_text.size()) {
    ESP_LOGE(kTag, "Failed to write %s", tmp_path);
    fclose(f); remove(tmp_path);
    if (!already_mounted && !log_file) UnmountLogSd();
    return {internal_saved, false};
  }
  bool write_ok = (fflush(f) == 0);
  if (write_ok && fsync(fileno(f)) != 0) write_ok = false;
  if (fclose(f) != 0) write_ok = false;
  if (!write_ok) {
    ESP_LOGE(kTag, "Failed to flush %s", tmp_path);
    remove(tmp_path);
    if (!already_mounted && !log_file) UnmountLogSd();
    return {internal_saved, false};
  }
  remove(backup_path);
  if (rename(CONFIG_FILE_PATH, backup_path) != 0 && errno != ENOENT) {
    ESP_LOGW(kTag, "Failed to backup %s to %s: %d", CONFIG_FILE_PATH, backup_path, errno);
  }
  if (rename(tmp_path, CONFIG_FILE_PATH) != 0) {
    ESP_LOGE(kTag, "Failed to replace %s with %s: %d", CONFIG_FILE_PATH, tmp_path, errno);
    rename(backup_path, CONFIG_FILE_PATH);
    remove(tmp_path);
    if (!already_mounted && !log_file) UnmountLogSd();
    return {internal_saved, false};
  }
  remove(backup_path);
  if (!already_mounted && !log_file) UnmountLogSd();
  ESP_LOGI(kTag, "Config saved to %s", CONFIG_FILE_PATH);
  return {internal_saved, true};
}

bool SaveConfigToSdCard(const AppConfig& cfg, const PidConfig& pid) {
  // Keep the existing fallback contract for callers that can operate without SD.
  return SaveConfigEverywhere(cfg, pid).nvs_saved;
}

// ---------------------------------------------------------------------------
// BuildConfigText — serialise config to text for save/export
// ---------------------------------------------------------------------------
std::string BuildConfigText(const AppConfig& cfg, const PidConfig& pid) {
  std::string text;
  text.reserve(1800);
  AppendConfigLine(&text, "# Config generated by device\n");
  AppendConfigLine(&text, "wifi_ssid = %s\n", cfg.wifi_ssid.c_str());
  AppendConfigLine(&text, "wifi_password = %s\n", cfg.wifi_password.c_str());
  AppendConfigLine(&text, "wifi_ap_mode = %s\n", cfg.wifi_ap_mode ? "true" : "false");
  AppendConfigLine(&text, "net_mode = %s\n", NetModeToString(cfg.net_mode).c_str());
  AppendConfigLine(&text, "net_priority = %s\n", NetPriorityToString(cfg.net_priority).c_str());
  AppendConfigLine(&text, "eth_dhcp = %s\n", cfg.eth_dhcp ? "true" : "false");
  AppendConfigLine(&text, "eth_static_ip = %s\n", cfg.eth_static_ip.c_str());
  AppendConfigLine(&text, "eth_static_netmask = %s\n", cfg.eth_static_netmask.c_str());
  AppendConfigLine(&text, "eth_static_gateway = %s\n", cfg.eth_static_gateway.c_str());
  AppendConfigLine(&text, "eth_static_dns = %s\n", cfg.eth_static_dns.c_str());
  text += "gps_rtcm_types = ";
  if (cfg.gps_rtcm_types.empty()) {
    text += "1004,1006,1033";
  } else {
    for (size_t i = 0; i < cfg.gps_rtcm_types.size(); ++i) {
      if (i > 0) text += ",";
      text += std::to_string(static_cast<unsigned>(cfg.gps_rtcm_types[i]));
    }
  }
  text += "\n";
  AppendConfigLine(&text, "gps_mode = %s\n", cfg.gps_mode.empty() ? "base_time_60" : cfg.gps_mode.c_str());
  AppendConfigLine(&text, "logging_active = %s\n", cfg.logging_active ? "true" : "false");
  AppendConfigLine(&text, "storage_backend = %s\n", StorageBackendToString(cfg.storage_backend).c_str());
  if (!cfg.logging_postfix.empty()) AppendConfigLine(&text, "logging_postfix = %s\n", cfg.logging_postfix.c_str());
  AppendConfigLine(&text, "logging_use_motor = %s\n", cfg.logging_use_motor ? "true" : "false");
  AppendConfigLine(&text, "logging_duration_s = %.3f\n", cfg.logging_duration_s);
  AppendConfigLine(&text, "logging_motor_steps = %d\n", cfg.logging_motor_steps);
  AppendConfigLine(&text, "logging_home_each_cycle = %s\n", cfg.logging_home_each_cycle ? "true" : "false");
  AppendConfigLine(&text, "stepper_speed_us = %d\n", cfg.stepper_speed_us);
  AppendConfigLine(&text, "stepper_home_offset_steps = %d\n", cfg.stepper_home_offset_steps);
  AppendConfigLine(&text, "motor_hall_active_level = %d\n", cfg.motor_hall_active_level);
  AppendConfigLine(&text, "pid_kp = %.6f\n", pid.kp);
  AppendConfigLine(&text, "pid_ki = %.6f\n", pid.ki);
  AppendConfigLine(&text, "pid_kd = %.6f\n", pid.kd);
  AppendConfigLine(&text, "pid_setpoint = %.6f\n", pid.setpoint);
  AppendConfigLine(&text, "pid_sensor = %d\n", pid.sensor_index);
  AppendConfigLine(&text, "pid_sensor_mask = %u\n", static_cast<unsigned int>(pid.sensor_mask));
  AppendConfigLine(&text, "pid_enabled = %s\n", CopyState().pid_enabled ? "true" : "false");
  if (!cfg.device_id.empty()) AppendConfigLine(&text, "device_id = %s\n", cfg.device_id.c_str());
  if (!cfg.minio_endpoint.empty()) AppendConfigLine(&text, "minio_endpoint = %s\n", cfg.minio_endpoint.c_str());
  if (!cfg.minio_access_key.empty()) AppendConfigLine(&text, "minio_access_key = %s\n", cfg.minio_access_key.c_str());
  if (!cfg.minio_secret_key.empty()) AppendConfigLine(&text, "minio_secret_key = %s\n", cfg.minio_secret_key.c_str());
  if (!cfg.minio_bucket.empty()) AppendConfigLine(&text, "minio_bucket = %s\n", cfg.minio_bucket.c_str());
  AppendConfigLine(&text, "minio_enabled = %s\n", cfg.minio_enabled ? "true" : "false");
  if (!cfg.mqtt_uri.empty()) AppendConfigLine(&text, "mqtt_uri = %s\n", cfg.mqtt_uri.c_str());
  if (!cfg.mqtt_user.empty()) AppendConfigLine(&text, "mqtt_user = %s\n", cfg.mqtt_user.c_str());
  if (!cfg.mqtt_password.empty()) AppendConfigLine(&text, "mqtt_password = %s\n", cfg.mqtt_password.c_str());
  AppendConfigLine(&text, "mqtt_enabled = %s\n", cfg.mqtt_enabled ? "true" : "false");
  AppendConfigLine(&text, "meteo_poll_interval_s = %d\n", cfg.meteo_poll_interval_s);
  AppendConfigLine(&text, "meteo_file_interval_s = %d\n", cfg.meteo_file_interval_s);
  AppendConfigLine(&text, "meteo_enabled = %s\n", cfg.meteo_enabled ? "true" : "false");
  return text;
}
