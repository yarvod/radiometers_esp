#include "data_logger.h"

#include <cstdio>
#include <ctime>
#include <string>
#include <unistd.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

#include "app_state.h"
#include "app_utils.h"
#include "gps_module.h"
#include "network_manager.h"
#include "sensor_hub.h"
#include "storage_manager.h"

static constexpr char kTag[] = "DLOG";

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
  return true;
}
