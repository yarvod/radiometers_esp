#pragma once

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstddef>
#include <functional>
#include <string>

#include "esp_http_server.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "sdmmc_cmd.h"

constexpr int MAX_TEMP_SENSORS = 8;
inline constexpr char CONFIG_MOUNT_POINT[] = "/sdcard";
inline constexpr char CONFIG_FILE_PATH[] = "/sdcard/config.txt";
inline constexpr char DEFAULT_WIFI_SSID[] = "Altai INASAN";
inline constexpr char DEFAULT_WIFI_PASS[] = "89852936257";
inline constexpr size_t WIFI_SSID_MAX_LEN = 32;
inline constexpr size_t WIFI_PASSWORD_MAX_LEN = 64;
inline constexpr char TAG[] = "APP";
inline constexpr char TO_UPLOAD_DIR[] = "/sdcard/to_upload";
inline constexpr char UPLOADED_DIR[] = "/sdcard/uploaded";

struct AppConfig {
  std::string wifi_ssid;
  std::string wifi_password;
  bool wifi_from_file;
  bool wifi_ap_mode;
  bool usb_mass_storage;
  bool usb_mass_storage_from_file;
  bool logging_active;
  std::string logging_postfix;
  bool logging_use_motor;
  float logging_duration_s;
  int stepper_speed_us;
  // Cloud / remote control
  std::string device_id;
  std::string minio_endpoint;
  std::string minio_access_key;
  std::string minio_secret_key;
  std::string minio_bucket;
  bool minio_enabled;
  std::string mqtt_uri;
  std::string mqtt_user;
  std::string mqtt_password;
  bool mqtt_enabled;
};

struct PidConfig {
  float kp;
  float ki;
  float kd;
  float setpoint;
  int sensor_index;
  uint16_t sensor_mask;
  bool from_file;
};

struct SharedState {
  float voltage1;
  float voltage2;
  float voltage3;
  float voltage1_cal;
  float voltage2_cal;
  float voltage3_cal;
  float offset1;
  float offset2;
  float offset3;
  float ina_bus_voltage;
  float ina_current;
  float ina_power;
  float heater_power;
  float fan_power;
  uint32_t fan1_rpm;
  uint32_t fan2_rpm;
  int temp_sensor_count;
  std::array<float, MAX_TEMP_SENSORS> temps_c;
  std::array<std::string, MAX_TEMP_SENSORS> temp_labels;
  std::array<std::string, MAX_TEMP_SENSORS> temp_addresses;
  bool homing;
  bool logging;
  std::string log_filename;
  bool log_use_motor;
  float log_duration_s;
  bool pid_enabled;
  float pid_kp;
  float pid_ki;
  float pid_kd;
  float pid_setpoint;
  int pid_sensor_index;
  uint16_t pid_sensor_mask;
  float pid_output;
  bool stepper_enabled;
  bool stepper_moving;
  bool stepper_direction_forward;
  bool stepper_homed;
  int stepper_speed_us;
  int stepper_target;
  int stepper_position;
  int64_t last_step_timestamp_us;
  bool stepper_abort;
  std::string stepper_home_status;
  uint64_t last_update_ms;
  bool calibrating;
  bool usb_msc_mode;
  std::string usb_error;
  int wifi_rssi_dbm;
  int wifi_quality;
  std::string wifi_ip;
  std::string wifi_ip_sta;
  std::string wifi_ip_ap;
  uint64_t sd_total_bytes;
  uint64_t sd_used_bytes;
  int sd_data_root_files;
  int sd_to_upload_files;
  int sd_uploaded_files;
};

enum class UsbMode : uint8_t { kCdc = 0, kMsc = 1 };

struct LoggingConfig {
  bool active;
  bool use_motor;
  float duration_s;
  bool homed_once;
  std::string postfix;
  uint64_t file_start_us;
};

extern AppConfig app_config;
extern PidConfig pid_config;
extern SharedState state;
extern LoggingConfig log_config;

extern SemaphoreHandle_t state_mutex;
extern SemaphoreHandle_t sd_mutex;

extern httpd_handle_t http_server;
extern UsbMode usb_mode;
extern TaskHandle_t calibration_task;
extern TaskHandle_t find_zero_task;
extern TaskHandle_t log_task;
extern TaskHandle_t upload_task;
extern TaskHandle_t mqtt_state_task;

extern sdmmc_card_t* sd_card;
extern sdmmc_card_t* log_sd_card;
extern FILE* log_file;
extern bool log_sd_mounted;
extern std::string current_log_path;

SharedState CopyState();
void UpdateState(const std::function<void(SharedState&)>& updater);
void ScheduleRestart();
UsbMode LoadUsbModeFromNvs(bool* found);
void SaveUsbModeToNvs(UsbMode mode);
uint32_t LoadAndIncrementBootId();
uint32_t GetBootId();

class SdLockGuard {
 public:
  explicit SdLockGuard(TickType_t timeout_ticks = pdMS_TO_TICKS(2000));
  ~SdLockGuard();
  bool locked() const { return locked_; }

 private:
  bool locked_ = false;
};
