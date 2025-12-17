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

#include "cJSON.h"
#include "driver/gpio.h"
#include "driver/spi_common.h"
#include "driver/ledc.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_http_server.h"
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

namespace {

constexpr char TAG[] = "APP";

// ADC pins
constexpr gpio_num_t ADC_MISO = GPIO_NUM_4;
constexpr gpio_num_t ADC_MOSI = GPIO_NUM_5;
constexpr gpio_num_t ADC_SCK = GPIO_NUM_6;
constexpr gpio_num_t ADC_CS1 = GPIO_NUM_16;
constexpr gpio_num_t ADC_CS2 = GPIO_NUM_15;
constexpr gpio_num_t ADC_CS3 = GPIO_NUM_7;

// INA219 pins (I2C)
constexpr gpio_num_t INA_SDA = GPIO_NUM_42;
constexpr gpio_num_t INA_SCL = GPIO_NUM_41;

// Status LEDs (active high)
constexpr gpio_num_t STATUS_LED_RED = GPIO_NUM_45;
constexpr gpio_num_t STATUS_LED_GREEN = GPIO_NUM_48;

// Heater
constexpr gpio_num_t HEATER_PWM = GPIO_NUM_14;

// Fans
constexpr gpio_num_t FAN_PWM = GPIO_NUM_2;
constexpr gpio_num_t FAN1_TACH = GPIO_NUM_1;
constexpr gpio_num_t FAN2_TACH = GPIO_NUM_21;

// Temperature (1-Wire)
constexpr gpio_num_t TEMP_1WIRE = GPIO_NUM_18;
constexpr int MAX_TEMP_SENSORS = 8;
[[maybe_unused]] constexpr uint64_t TEMP_ADDR_RAD = 0x77062223A096AD28ULL;
[[maybe_unused]] constexpr uint64_t TEMP_ADDR_LOAD = 0xE80000105FF4E228ULL;

// Hall sensor
constexpr gpio_num_t MT_HALL_SEN = GPIO_NUM_3;

// SD card pins (1-bit SDMMC)
constexpr gpio_num_t SD_CLK = GPIO_NUM_39;
constexpr gpio_num_t SD_CMD = GPIO_NUM_38;
constexpr gpio_num_t SD_D0 = GPIO_NUM_40;

// Relay and stepper pins
constexpr gpio_num_t RELAY_PIN = GPIO_NUM_17;
constexpr gpio_num_t STEPPER_EN = GPIO_NUM_35;
constexpr gpio_num_t STEPPER_DIR = GPIO_NUM_36;
constexpr gpio_num_t STEPPER_STEP = GPIO_NUM_37;
constexpr int ADC_SPI_FREQ_HZ = 100'000;

constexpr float VREF = 4.096f;  // ±Vref/2 range, matches original sketch
constexpr float ADC_SCALE = (VREF / 2.0f) / static_cast<float>(1 << 23);

constexpr char DEFAULT_WIFI_SSID[] = "Altai INASAN";
constexpr char DEFAULT_WIFI_PASS[] = "89852936257";
constexpr char HOSTNAME[] = "miap-device";
constexpr bool USE_CUSTOM_MAC = true;
uint8_t CUSTOM_MAC[6] = {0x10, 0x00, 0x3B, 0x6E, 0x83, 0x70};
constexpr char MSC_BASE_PATH[] = "/usb_msc";
constexpr char CONFIG_MOUNT_POINT[] = "/sdcard";
constexpr char CONFIG_FILE_PATH[] = "/sdcard/config.txt";
constexpr size_t WIFI_SSID_MAX_LEN = 32;
constexpr size_t WIFI_PASSWORD_MAX_LEN = 64;
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

std::string SanitizePostfix(const std::string& raw) {
  std::string out;
  out.reserve(raw.size());
  for (char c : raw) {
    if (std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-') {
      out.push_back(c);
    } else if (std::isspace(static_cast<unsigned char>(c))) {
      out.push_back('_');
    }
    if (out.size() >= 24) break;  // keep filenames short
  }
  // Trim trailing underscores from whitespace-only input
  while (!out.empty() && out.back() == '_') {
    out.pop_back();
  }
  return out;
}

extern SemaphoreHandle_t sd_mutex;

class SdLockGuard {
 public:
  explicit SdLockGuard(TickType_t timeout_ticks = pdMS_TO_TICKS(2000)) {
    locked_ = (sd_mutex && xSemaphoreTake(sd_mutex, timeout_ticks) == pdTRUE);
  }
  ~SdLockGuard() {
    if (locked_ && sd_mutex) {
      xSemaphoreGive(sd_mutex);
    }
  }
  bool locked() const { return locked_; }

 private:
  bool locked_ = false;
};

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

struct AppConfig {
  std::string wifi_ssid = DEFAULT_WIFI_SSID;
  std::string wifi_password = DEFAULT_WIFI_PASS;
  bool wifi_from_file = false;
  bool wifi_ap_mode = false;
  bool usb_mass_storage = false;
  bool usb_mass_storage_from_file = false;
  bool logging_active = false;
  std::string logging_postfix;
  bool logging_use_motor = false;
  float logging_duration_s = 1.0f;
  int stepper_speed_us = 1000;
};

AppConfig app_config{};

struct PidConfig {
  float kp = 1.0f;
  float ki = 0.0f;
  float kd = 0.0f;
  float setpoint = 25.0f;
  int sensor_index = 0;
  bool from_file = false;
};

PidConfig pid_config{};

// State shared across tasks and HTTP handlers
struct SharedState {
  float voltage1 = 0.0f;
  float voltage2 = 0.0f;
  float voltage3 = 0.0f;
  float offset1 = 0.0f;
  float offset2 = 0.0f;
  float offset3 = 0.0f;
  float ina_bus_voltage = 0.0f;
  float ina_current = 0.0f;
  float ina_power = 0.0f;
  float heater_power = 0.0f;
  float fan_power = 0.0f;       // 0..100%
  uint32_t fan1_rpm = 0;
  uint32_t fan2_rpm = 0;
  int temp_sensor_count = 0;
  std::array<float, MAX_TEMP_SENSORS> temps_c{};
  std::array<std::string, MAX_TEMP_SENSORS> temp_labels{};
  std::array<std::string, MAX_TEMP_SENSORS> temp_addresses{};
  bool homing = false;
  bool logging = false;
  std::string log_filename;
  bool log_use_motor = false;
  float log_duration_s = 1.0f;
  bool pid_enabled = false;
  float pid_kp = 1.0f;
  float pid_ki = 0.0f;
  float pid_kd = 0.0f;
  float pid_setpoint = 25.0f;
  int pid_sensor_index = 0;
  float pid_output = 0.0f;
  bool stepper_enabled = false;
  bool stepper_moving = false;
  bool stepper_direction_forward = true;
  int stepper_speed_us = 1000;
  int stepper_target = 0;
  int stepper_position = 0;
  int64_t last_step_timestamp_us = 0;
  bool stepper_abort = false;
  uint64_t last_update_ms = 0;
  bool calibrating = false;
  bool usb_msc_mode = false;
  std::string usb_error;
};

SharedState state{};
SemaphoreHandle_t state_mutex = nullptr;

enum class UsbMode : uint8_t { kCdc = 0, kMsc = 1 };
UsbMode usb_mode = UsbMode::kCdc;

// Forward declarations for shared state helpers
SharedState CopyState();
void UpdateState(const std::function<void(SharedState&)>& updater);
bool MountLogSd();
void UnmountLogSd();
static bool SaveConfigToSdCard(const AppConfig& cfg, const PidConfig& pid, UsbMode current_usb_mode);

// Devices
LTC2440 adc1(ADC_CS1, ADC_MISO);
LTC2440 adc2(ADC_CS2, ADC_MISO);
// LTC2440 adc3(ADC_CS3, ADC_MISO);
sdmmc_card_t* sd_card = nullptr;
sdmmc_card_t* log_sd_card = nullptr;
FILE* log_file = nullptr;
bool log_sd_mounted = false;
SemaphoreHandle_t sd_mutex = nullptr;

struct LoggingConfig {
  bool active = false;
  bool use_motor = false;
  float duration_s = 1.0f;
  bool homed_once = false;
  std::string postfix;
  uint64_t file_start_us = 0;
};

LoggingConfig log_config{};

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
  fprintf(log_file, "timestamp_iso,timestamp_ms,adc1,adc2");
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
    fprintf(log_file, ",adc1_cal,adc2_cal");
  }
  fprintf(log_file, "\n");
  FlushLogFile();

  UpdateState([&](SharedState& s) {
    s.logging = true;
    s.log_filename = filename;
  });
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

// HTTP server
httpd_handle_t http_server = nullptr;
TaskHandle_t calibration_task = nullptr;
TaskHandle_t find_zero_task = nullptr;
TaskHandle_t log_task = nullptr;

// HTML UI (kept close to original Arduino page)
constexpr char INDEX_HTML[] = R"rawliteral(
<!DOCTYPE HTML>
<html>
<head>
  <title>LTC2440 ADC Monitor & Stepper Control</title>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <style>
    body { font-family: Arial, sans-serif; margin: 20px; background: #f0f0f0; }
    .container { max-width: 1200px; margin: 0 auto; background: white; padding: 20px; border-radius: 10px; box-shadow: 0 2px 10px rgba(0,0,0,0.1); }
    .header { text-align: center; margin-bottom: 30px; }
    .adc-readings { display: flex; justify-content: space-around; flex-wrap: wrap; margin-bottom: 30px; }
    .adc-channel { background: #e8f4fd; padding: 20px; border-radius: 8px; margin: 10px; flex: 1; min-width: 200px; text-align: center; }
    .voltage { font-size: 24px; font-weight: bold; color: #2c3e50; margin: 10px 0; }
    .channel-name { font-size: 18px; color: #34495e; }
    .controls { display: flex; flex-wrap: wrap; justify-content: space-between; margin: 20px 0; }
    .control-panel { background: #f8f9fa; padding: 20px; border-radius: 8px; margin: 10px; flex: 1; min-width: 300px; }
    .btn { background: #3498db; color: white; border: none; padding: 12px 24px; margin: 5px; border-radius: 5px; cursor: pointer; font-size: 16px; }
    .btn:hover { background: #2980b9; }
    .btn-calibrate { background: #e74c3c; }
    .btn-calibrate:hover { background: #c0392b; }
    .btn-stepper { background: #27ae60; }
    .btn-stepper:hover { background: #219a52; }
    .btn-stop { background: #e67e22; }
    .btn-stop:hover { background: #d35400; }
    .btn-small { padding: 6px 12px; font-size: 14px; }
    .status { text-align: center; margin: 10px 0; color: #7f8c8d; }
    .stepper-status { background: #fff3cd; padding: 10px; border-radius: 5px; margin: 10px 0; }
    .form-group { margin: 10px 0; }
    label { display: block; margin-bottom: 5px; font-weight: bold; }
    input, select { width: 100%; padding: 8px; border: 1px solid #ddd; border-radius: 4px; }
    .speed-info { font-size: 12px; color: #666; margin-top: 5px; }
    .usb-panel { background: #eef7e8; padding: 20px; border-radius: 8px; margin-top: 10px; }
    .note { font-size: 12px; color: #666; margin-top: 5px; }
    .files-panel { background: #f4f4ff; padding: 20px; border-radius: 8px; margin-top: 20px; }
    .file-actions { display: flex; align-items: center; justify-content: space-between; gap: 10px; flex-wrap: wrap; }
    .file-actions .file-buttons { display: flex; gap: 10px; align-items: center; flex-wrap: wrap; }
    .file-row { display: flex; align-items: center; justify-content: space-between; padding: 6px 0; border-bottom: 1px solid #eee; }
    .file-info { display: flex; align-items: center; gap: 10px; }
    .file-checkbox { width: 18px; height: 18px; }
    .checkbox-label { display: flex; align-items: center; gap: 8px; font-weight: 600; }
    .file-name { word-break: break-all; }
    .temp-label { font-weight: 600; cursor: help; text-decoration: underline dotted; }
  </style>
</head>
<body>
  <div class="container">
    <div class="header">
      <h1>LTC2440 ADC Monitor & Stepper Control</h1>
      <p>Real-time voltage measurements and TMC2208 stepper motor control</p>
    </div>
    
    <div class="adc-readings">
      <div class="adc-channel">
        <div class="channel-name">ADC Channel 1</div>
        <div class="voltage" id="voltage1">0.000000 V</div>
      </div>
      <div class="adc-channel">
        <div class="channel-name">ADC Channel 2</div>
        <div class="voltage" id="voltage2">0.000000 V</div>
      </div>
      <div class="adc-channel">
        <div class="channel-name">ADC Channel 3</div>
        <div class="voltage" id="voltage3">0.000000 V</div>
      </div>
    </div>

    <div class="adc-readings">
      <div class="adc-channel">
        <div class="channel-name">INA219 Bus Voltage</div>
        <div class="voltage" id="inaVoltage">0.000 V</div>
        <div>Current: <span id="inaCurrent">0.000</span> A</div>
        <div>Power: <span id="inaPower">0.000</span> W</div>
      </div>
      <div class="adc-channel">
        <div class="channel-name">Fan Status</div>
        <div class="voltage" id="fanPowerDisplay">0 %</div>
        <div>FAN1 RPM: <span id="fan1RpmDisplay">0</span></div>
        <div>FAN2 RPM: <span id="fan2RpmDisplay">0</span></div>
      </div>
      <div class="adc-channel">
        <div class="channel-name">Temperatures</div>
        <div id="tempList">
          <div class="voltage">--.- &deg;C</div>
          <div>Sensor list will appear here</div>
        </div>
      </div>
    </div>
    
    <div class="controls">
      <div class="control-panel">
        <h3>ADC Controls</h3>
        <button class="btn" onclick="refreshData()">Refresh Data</button>
        <button class="btn btn-calibrate" onclick="calibrate()">Calibrate Zero</button>
      </div>

      <div class="control-panel">
        <h3>Heater Control</h3>
        <div class="form-group">
          <label for="heaterPower">Power (%)</label>
          <input type="number" id="heaterPower" value="0" min="0" max="100" step="0.1">
        </div>
        <button class="btn" onclick="setHeater()">Set Heater</button>
      </div>

      <div class="control-panel">
        <h3>Wi-Fi</h3>
        <div class="form-group">
          <label for="wifiMode">Mode</label>
          <select id="wifiMode">
            <option value="sta">Connect to Wi‑Fi</option>
            <option value="ap">Share own Wi‑Fi</option>
          </select>
        </div>
        <div class="form-group">
          <label for="wifiSsid">SSID</label>
          <input type="text" id="wifiSsid" value="">
        </div>
        <div class="form-group">
          <label for="wifiPassword">Password</label>
          <input type="text" id="wifiPassword" value="">
        </div>
        <button class="btn" onclick="applyWifi()">Apply Wi‑Fi</button>
      </div>

      <div class="control-panel">
        <h3>Measurements</h3>
        <div class="form-group">
          <label for="logFilename">Filename (on SD)</label>
          <input type="text" id="logFilename" value="log.txt">
        </div>
        <div class="form-group">
          <label><input type="checkbox" id="logUseMotor"> Use motor (home + 90° sweep)</label>
        </div>
        <div class="form-group">
          <label for="logDuration">Averaging duration, sec</label>
          <input type="number" id="logDuration" value="1" min="0.1" step="0.1">
        </div>
        <div>Status: <span id="logStatus">Idle</span></div>
        <button class="btn" onclick="startLog()">Start Logging</button>
        <button class="btn btn-stop" onclick="stopLog()">Stop Logging</button>
      </div>

      <div class="control-panel">
        <h3>Fan Control</h3>
        <div class="form-group">
          <label for="fanPower">Fan Power (%)</label>
          <input type="number" id="fanPower" value="100" min="0" max="100" step="1">
        </div>
        <button class="btn" onclick="setFan()">Set Fan</button>
      </div>

      <div class="control-panel">
        <h3>Heater PID Control</h3>
        <div class="form-group">
          <label for="pidSetpoint">Target Temp (°C)</label>
          <input type="number" id="pidSetpoint" value="25" step="0.1">
        </div>
        <div class="form-group">
          <label for="pidSensor">Sensor</label>
          <select id="pidSensor"></select>
        </div>
        <div class="form-group">
          <label for="pidKp">Kp</label>
          <input type="number" id="pidKp" value="1" step="0.01">
        </div>
        <div class="form-group">
          <label for="pidKi">Ki</label>
          <input type="number" id="pidKi" value="0" step="0.01">
        </div>
        <div class="form-group">
          <label for="pidKd">Kd</label>
          <input type="number" id="pidKd" value="0" step="0.01">
        </div>
        <div>Status: <span id="pidStatus">Off</span></div>
        <button class="btn" onclick="applyPid()">Apply PID</button>
        <button class="btn btn-stepper" onclick="enablePid()">Enable PID</button>
        <button class="btn btn-stop" onclick="disablePid()">Disable PID</button>
      </div>
      
      <div class="control-panel">
        <h3>Stepper Motor Control</h3>
        <div class="stepper-status">
          Status: <span id="stepperStatus">Disabled</span><br>
          Position: <span id="stepperPosition">0</span> steps<br>
          Target: <span id="stepperTarget">0</span> steps<br>
          Moving: <span id="stepperMoving">No</span>
        </div>
        
        <div class="form-group">
          <label for="steps">Number of Steps:</label>
          <input type="number" id="steps" value="400" min="1" max="10000">
        </div>
        
        <div class="form-group">
          <label for="direction">Direction:</label>
          <select id="direction">
            <option value="forward">Forward</option>
            <option value="backward">Backward</option>
          </select>
        </div>
        
        <div class="form-group">
          <label for="speed">Speed (microseconds delay):</label>
          <input type="number" id="speed" value="1000" step="1">
          <div class="speed-info">Lower value = faster speed. Значение берётся из веб-инпута без ограничений и сохраняется в config.txt.</div>
        </div>
        
        <button class="btn btn-stepper" onclick="enableStepper()">Enable Motor</button>
        <button class="btn btn-stop" onclick="disableStepper()">Disable Motor</button>
        <button class="btn" onclick="moveStepper()">Move</button>
        <button class="btn btn-stop" onclick="stopStepper()">Stop</button>
        <button class="btn" onclick="setZero()">Set Position to Zero</button>
        <button class="btn" onclick="findZero()">Find Zero (Hall)</button>
      </div>
    </div>
    
    <div class="status">
      Last update: <span id="lastUpdate">-</span>
      <br>USB mode: <span id="usbModeLabel">Serial (logs/flash)</span>
    </div>

    <div class="usb-panel">
      <h3>USB Mode (requires reboot)</h3>
      <label for="usbModeSelect">Select mode:</label>
      <select id="usbModeSelect">
        <option value="cdc">Serial (logs + flashing)</option>
        <option value="msc">Mass Storage (SD card over USB)</option>
      </select>
      <div class="note">MSC отключает доступ к логам/прошивке, даёт доступ к SD по USB. Переключение перезагружает устройство.</div>
      <button class="btn" onclick="applyUsbMode()">Apply & Reboot</button>
      <div class="note" id="usbError" style="color:#c0392b; display:none;"></div>
    </div>

    <div class="files-panel">
      <h3>Файлы на SD</h3>
      <div class="form-group file-actions">
        <label class="checkbox-label"><input type="checkbox" id="selectAllFiles"> Выбрать все</label>
        <div class="file-buttons">
          <button class="btn" onclick="loadFiles()">Обновить список</button>
          <button class="btn btn-stop" id="deleteSelectedBtn" disabled>Удалить выбранные</button>
        </div>
      </div>
      <div id="fileList"></div>
      <div class="note">Можно скачать или удалить файл. config.txt защищён от удаления. Одновременная запись логов и скачивание синхронизированы мьютексом.</div>
    </div>
  </div>

<script>
    let measurementsInitialized = false;
    const selectedFiles = new Set();
    let cachedFiles = [];

    function setValueIfIdle(id, value) {
      const el = document.getElementById(id);
      if (el && document.activeElement !== el) {
        el.value = value;
      }
    }

    function updateData(data) {
      document.getElementById('voltage1').textContent = data.voltage1.toFixed(6) + ' V';
      document.getElementById('voltage2').textContent = data.voltage2.toFixed(6) + ' V';
      document.getElementById('voltage3').textContent = data.voltage3.toFixed(6) + ' V';
      document.getElementById('inaVoltage').textContent = data.inaBusVoltage.toFixed(3) + ' V';
      document.getElementById('inaCurrent').textContent = data.inaCurrent.toFixed(3);
      document.getElementById('inaPower').textContent = data.inaPower.toFixed(3);
      document.getElementById('fanPowerDisplay').textContent = data.fanPower.toFixed(0) + ' %';
      document.getElementById('fan1RpmDisplay').textContent = data.fan1Rpm;
      document.getElementById('fan2RpmDisplay').textContent = data.fan2Rpm;
      setValueIfIdle('heaterPower', data.heaterPower?.toFixed(1) ?? data.heaterPower ?? 0);
    const list = document.getElementById('tempList');
    const labels = Array.isArray(data.tempLabels) ? data.tempLabels : [];
    const addresses = Array.isArray(data.tempAddresses) ? data.tempAddresses : [];
      if (Array.isArray(data.tempSensors) && data.tempSensors.length > 0) {
        let html = '';
        data.tempSensors.forEach((t, idx) => {
          const name = labels[idx] || `t${idx + 1}`;
          const addr = addresses[idx] || '';
          const title = addr ? ` title="1-Wire ${addr}"` : '';
          const labelHtml = `<span class="temp-label"${title}>${name}</span>`;
          const text = Number.isFinite(t) ? `${t.toFixed(2)} °C` : '--.- °C';
          const classAttr = idx === 0 ? ' class="voltage"' : '';
          html += `<div${classAttr}>${labelHtml}: ${text}</div>`;
        });
        list.innerHTML = html;
      } else {
        list.innerHTML = '<div class="voltage">--.- °C</div><div>No sensors</div>';
      }
      const logStatus = document.getElementById('logStatus');
      if (data.logging) {
        logStatus.textContent = 'Logging to ' + (data.logFilename || '');
      } else {
        logStatus.textContent = 'Idle';
      }
    if (!measurementsInitialized) {
      setValueIfIdle('logFilename', data.logFilename || '');
      const logUseMotorEl = document.getElementById('logUseMotor');
      if (logUseMotorEl && document.activeElement !== logUseMotorEl) {
        logUseMotorEl.checked = !!data.logUseMotor;
      }
      setValueIfIdle('logDuration', (data.logDuration ?? 1).toFixed(1));
      // Wi-Fi defaults
      const wifiModeEl = document.getElementById('wifiMode');
      if (wifiModeEl) wifiModeEl.value = data.wifiApMode ? 'ap' : 'sta';
      setValueIfIdle('wifiSsid', data.wifiSsid || '');
      setValueIfIdle('wifiPassword', data.wifiPassword || '');
      measurementsInitialized = true;
    }
      document.getElementById('stepperStatus').textContent = data.stepperEnabled ? 'Enabled' : 'Disabled';
      document.getElementById('stepperPosition').textContent = data.stepperPosition;
      document.getElementById('stepperTarget').textContent = data.stepperTarget;
      document.getElementById('stepperMoving').textContent = data.stepperMoving ? 'Yes' : 'No';
      setValueIfIdle('speed', data.stepperSpeedUs ?? '');
      document.getElementById('lastUpdate').textContent = new Date().toLocaleTimeString();
      const modeLabel = data.usbMode === 'msc' ? 'Mass Storage (SD over USB)' : 'Serial (logs/flash)';
      document.getElementById('usbModeLabel').textContent = modeLabel;
      const usbModeSelect = document.getElementById('usbModeSelect');
      usbModeSelect.value = data.usbMode === 'msc' ? 'msc' : 'cdc';
      usbModeSelect.disabled = !data.usbMscBuilt && data.usbMode !== 'msc';
      const usbErrorEl = document.getElementById('usbError');
      if (!data.usbMscBuilt) {
        usbErrorEl.textContent = 'Прошивка собрана без MSC. Сборка с CONFIG_TINYUSB_MSC_ENABLED=y обязательна.';
        usbErrorEl.style.display = 'block';
      }
      if (data.usbError) {
        usbErrorEl.textContent = data.usbError;
        usbErrorEl.style.display = 'block';
      } else {
        usbErrorEl.style.display = 'none';
      }

      const sensorSelect = document.getElementById('pidSensor');
      const currentSensor = data.pidSensorIndex ?? 0;
      const count = data.tempSensorCount || (data.tempSensors ? data.tempSensors.length : 0);
      sensorSelect.innerHTML = '';
      for (let i = 0; i < count; i++) {
        const opt = document.createElement('option');
        opt.value = i;
        const label = labels[i] || `t${i + 1}`;
        const addr = addresses[i] || '';
        opt.textContent = addr ? `${label} (${addr})` : label;
        if (addr) opt.title = `1-Wire ${addr}`;
        if (i === currentSensor) opt.selected = true;
        sensorSelect.appendChild(opt);
      }
      // PID inputs оставляем как ввёл пользователь, не трогаем автоданными
      document.getElementById('pidStatus').textContent = data.pidEnabled ? `On (out ${data.pidOutput?.toFixed(1) ?? 0}%)` : 'Off';
      setValueIfIdle('pidSetpoint', (data.pidSetpoint ?? 0).toFixed(2));
      setValueIfIdle('pidKp', (data.pidKp ?? 0).toFixed(4));
      setValueIfIdle('pidKi', (data.pidKi ?? 0).toFixed(4));
      setValueIfIdle('pidKd', (data.pidKd ?? 0).toFixed(4));
    }
    
    function refreshData() {
      fetch('/data')
        .then(response => response.json())
        .then(data => updateData(data))
        .catch(error => console.error('Error:', error));
    }
    
    function calibrate() {
      if(confirm('Start zero calibration? This will take about 20 seconds.')) {
        fetch('/calibrate', { method: 'POST' })
          .then(response => {
            alert('Calibration started in background...');
            refreshData();
          })
          .catch(error => {
            console.error('Error:', error);
            alert('Calibration started in background...');
            refreshData();
          });
      }
    }
    
    function enableStepper() {
      fetch('/stepper/enable', { method: 'POST' })
        .then(response => response.json())
        .then(data => {
          alert('Stepper motor enabled');
          refreshData();
        })
        .catch(error => {
          console.error('Error:', error);
          alert('Stepper motor enabled');
          refreshData();
        });
    }
    
    function disableStepper() {
      fetch('/stepper/disable', { method: 'POST' })
        .then(response => response.json())
        .then(data => {
          alert('Stepper motor disabled');
          refreshData();
        })
        .catch(error => {
          console.error('Error:', error);
          alert('Stepper motor disabled');
          refreshData();
        });
    }
    
    function moveStepper() {
      const steps = document.getElementById('steps').value;
      const direction = document.getElementById('direction').value;
      const speed = document.getElementById('speed').value;
      
      fetch('/stepper/move', {
        method: 'POST',
        headers: {
          'Content-Type': 'application/json',
        },
        body: JSON.stringify({
          steps: parseInt(steps),
          direction: direction,
          speed: parseInt(speed)
        })
      })
      .then(response => response.json())
      .then(data => {
        alert('Movement started');
        refreshData();
      })
      .catch(error => {
        console.error('Error:', error);
        alert('Movement started');
        refreshData();
      });
    }
    
    function stopStepper() {
      fetch('/stepper/stop', { method: 'POST' })
        .then(response => response.json())
        .then(data => {
          alert('Movement stopped');
          refreshData();
        })
        .catch(error => {
          console.error('Error:', error);
          alert('Movement stopped');
          refreshData();
        });
    }
    
    function setZero() {
      fetch('/stepper/zero', { method: 'POST' })
        .then(response => response.json())
        .then(data => {
          alert('Position set to zero');
          refreshData();
        })
        .catch(error => {
          console.error('Error:', error);
          alert('Position set to zero');
          refreshData();
        });
    }

    function findZero() {
      fetch('/stepper/find_zero', { method: 'POST' })
        .then(() => refreshData())
        .catch(() => refreshData());
    }

    function setHeater() {
      const p = parseFloat(document.getElementById('heaterPower').value);
      fetch('/heater/set', {
        method: 'POST',
        headers: {
          'Content-Type': 'application/json',
        },
        body: JSON.stringify({ power: p })
      })
      .then(() => refreshData())
      .catch(() => refreshData());
    }

    function setFan() {
      const p = parseFloat(document.getElementById('fanPower').value);
      fetch('/fan/set', {
        method: 'POST',
        headers: {
          'Content-Type': 'application/json',
        },
        body: JSON.stringify({ power: p })
      })
      .then(() => refreshData())
      .catch(() => refreshData());
    }

    function startLog() {
      const fname = document.getElementById('logFilename').value;
      const useMotor = document.getElementById('logUseMotor').checked;
      const duration = parseFloat(document.getElementById('logDuration').value);
      fetch('/log/start', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ filename: fname, useMotor: useMotor, durationSec: duration })
      }).then(() => refreshData()).catch(() => refreshData());
    }

    function stopLog() {
      fetch('/log/stop', { method: 'POST' })
      .then(() => refreshData())
      .catch(() => refreshData());
    }

    function applyWifi() {
      const mode = document.getElementById('wifiMode').value;
      const ssid = document.getElementById('wifiSsid').value;
      const password = document.getElementById('wifiPassword').value;
      fetch('/wifi/apply', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ mode, ssid, password })
      }).then(res => {
        if (!res.ok) throw new Error('Failed to apply Wi-Fi');
        return res.json();
      }).then(() => {
        alert('Wi‑Fi settings applied. Device may reconnect or switch mode.');
      }).catch(err => {
        alert('Wi‑Fi apply failed: ' + err.message);
      });
    }

    function applyPid() {
      const payload = {
        setpoint: parseFloat(document.getElementById('pidSetpoint').value),
        sensor: parseInt(document.getElementById('pidSensor').value),
        kp: parseFloat(document.getElementById('pidKp').value),
        ki: parseFloat(document.getElementById('pidKi').value),
        kd: parseFloat(document.getElementById('pidKd').value),
      };
      fetch('/pid/apply', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify(payload),
      }).then(() => refreshData()).catch(() => refreshData());
    }

    function enablePid() {
      fetch('/pid/enable', { method: 'POST' })
        .then(() => refreshData())
        .catch(() => refreshData());
    }

    function disablePid() {
      fetch('/pid/disable', { method: 'POST' })
        .then(() => refreshData())
        .catch(() => refreshData());
    }

    function getFileName(item) {
      if (item && typeof item === 'object') {
        return item.name || '';
      }
      return typeof item === 'string' ? item : '';
    }

    function refreshSelectionState() {
      const available = new Set(cachedFiles.map(getFileName).filter(name => name && name !== 'config.txt'));
      Array.from(selectedFiles).forEach(name => {
        if (!available.has(name)) {
          selectedFiles.delete(name);
        }
      });
    }

    function updateSelectionControls() {
      const selectAll = document.getElementById('selectAllFiles');
      const deleteBtn = document.getElementById('deleteSelectedBtn');
      const selectableCount = cachedFiles.filter(item => {
        const name = getFileName(item);
        return name && name !== 'config.txt';
      }).length;
      const selectedCount = selectedFiles.size;
      if (selectAll) {
        selectAll.checked = selectableCount > 0 && selectedCount === selectableCount;
        selectAll.indeterminate = selectedCount > 0 && selectedCount < selectableCount;
      }
      if (deleteBtn) {
        deleteBtn.disabled = selectedCount === 0;
      }
    }

    function toggleFileSelection(name, checked) {
      if (!name || name === 'config.txt') return;
      if (checked) {
        selectedFiles.add(name);
      } else {
        selectedFiles.delete(name);
      }
      updateSelectionControls();
    }

    function toggleSelectAll(checked) {
      cachedFiles.forEach(item => {
        const name = getFileName(item);
        if (!name || name === 'config.txt') return;
        if (checked) {
          selectedFiles.add(name);
        } else {
          selectedFiles.delete(name);
        }
      });
      renderFiles(cachedFiles);
    }

    function sendDeleteRequest(files) {
      const unique = Array.from(new Set(files.filter(name => name && name !== 'config.txt')));
      if (unique.length === 0) {
        alert('Нет выбранных файлов для удаления');
        return Promise.resolve();
      }
      if (!confirm(`Удалить ${unique.length} файл(ов)?`)) {
        return Promise.resolve();
      }
      return fetch('/fs/delete', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ files: unique }),
      }).then(async res => {
        const text = await res.text();
        let data = {};
        try { data = text ? JSON.parse(text) : {}; } catch (_) { data = { message: text }; }
        if (!res.ok) {
          throw new Error(data?.error || data?.message || text || 'Не удалось удалить файлы');
        }
        return data;
      }).then(result => {
        selectedFiles.clear();
        loadFiles();
        const deleted = Array.isArray(result.deleted) ? result.deleted : [];
        const skipped = Array.isArray(result.skipped) ? result.skipped : [];
        const failed = Array.isArray(result.failed) ? result.failed : [];
        if (deleted.length === 0 && skipped.length === 0 && failed.length === 0) {
          return;
        }
        let msg = '';
        if (deleted.length) msg += `Удалены: ${deleted.join(', ')}. `;
        if (skipped.length) msg += `Пропущены: ${skipped.join(', ')}. `;
        if (failed.length) msg += `Ошибки: ${failed.join(', ')}.`;
        if (msg.trim()) {
          alert(msg.trim());
        }
      }).catch(err => {
        alert(err.message || 'Не удалось удалить файлы');
      });
    }

    function deleteSelectedFiles() {
      return sendDeleteRequest(Array.from(selectedFiles));
    }

    function deleteSingleFile(name) {
      return sendDeleteRequest([name]);
    }

    function renderFiles(files) {
      const listEl = document.getElementById('fileList');
      cachedFiles = Array.isArray(files) ? files : [];
      refreshSelectionState();
      if (!listEl) return;
      if (!Array.isArray(files) || files.length === 0) {
        listEl.innerHTML = '<div>Нет файлов</div>';
        updateSelectionControls();
        return;
      }
      listEl.innerHTML = '';
      cachedFiles.forEach(item => {
        const name = getFileName(item);
        const sizeBytes = (item && typeof item === 'object' && Number.isFinite(item.size)) ? item.size : null;
        if (!name) return;
        const row = document.createElement('div');
        row.className = 'file-row';

        const info = document.createElement('div');
        info.className = 'file-info';

        const checkbox = document.createElement('input');
        checkbox.type = 'checkbox';
        checkbox.className = 'file-checkbox';
        checkbox.disabled = name === 'config.txt';
        checkbox.checked = selectedFiles.has(name);
        checkbox.onchange = () => {
          toggleFileSelection(name, checkbox.checked);
          updateSelectionControls();
        };

        const left = document.createElement('span');
        left.className = 'file-name';
        let sizeText = '';
        if (sizeBytes !== null) {
          sizeText = ` (${(sizeBytes / (1024 * 1024)).toFixed(2)} MB)`;
        }
        left.textContent = name + sizeText;

        info.appendChild(checkbox);
        info.appendChild(left);

        const actions = document.createElement('div');

        const dlBtn = document.createElement('button');
        dlBtn.className = 'btn btn-small';
        dlBtn.textContent = 'Скачать';
        dlBtn.onclick = () => { window.open('/fs/download?file=' + encodeURIComponent(name), '_blank'); };

        const delBtn = document.createElement('button');
        delBtn.className = 'btn btn-small btn-stop';
        delBtn.textContent = 'Удалить';
        delBtn.disabled = name === 'config.txt';
        delBtn.onclick = () => deleteSingleFile(name);

        actions.appendChild(dlBtn);
        actions.appendChild(delBtn);
        row.appendChild(info);
        row.appendChild(actions);
        listEl.appendChild(row);
      });
      updateSelectionControls();
    }

    function loadFiles() {
      const listEl = document.getElementById('fileList');
      if (listEl) listEl.innerHTML = 'Загружаю...';
      fetch('/fs/list')
        .then(res => res.json())
        .then(files => renderFiles(files))
        .catch(() => {
          cachedFiles = [];
          selectedFiles.clear();
          if (listEl) listEl.innerHTML = 'Ошибка загрузки списка';
          updateSelectionControls();
        });
    }

    const selectAllEl = document.getElementById('selectAllFiles');
    if (selectAllEl) {
      selectAllEl.addEventListener('change', (e) => toggleSelectAll(e.target.checked));
    }
    const deleteSelectedBtn = document.getElementById('deleteSelectedBtn');
    if (deleteSelectedBtn) {
      deleteSelectedBtn.addEventListener('click', deleteSelectedFiles);
    }

    // Auto-refresh every 2 seconds
    setInterval(refreshData, 2000);
    
    // Initial load
    refreshData();
    loadFiles();

    function applyUsbMode() {
      const mode = document.getElementById('usbModeSelect').value;
      const confirmText = mode === 'msc'
        ? 'Переключиться в Mass Storage. Логи/прошивка по USB станут недоступны. Перезагрузить устройство?'
        : 'Вернуть USB в режим Serial (логи/прошивка). Устройство перезагрузится. Продолжить?';
      if (!confirm(confirmText)) return;
      fetch('/usb/mode', {
        method: 'POST',
        headers: {'Content-Type': 'application/json'},
        body: JSON.stringify({mode})
      }).then(res => {
        if (!res.ok) {
          return res.text().then(t => { throw new Error(t || 'Request failed'); });
        }
        return res.text();
      }).then(() => {
        alert('Переключаюсь, устройство перезагрузится');
      }).catch(err => {
        alert('Не удалось переключить: ' + err.message);
      });
    }
  </script>
</body>
</html>
)rawliteral";

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
  return config->wifi_from_file || config->usb_mass_storage_from_file || log_active_set || log_postfix_set || pid_config.from_file;
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

// Helpers to guard shared state
SharedState CopyState() {
  SharedState snapshot;
  if (state_mutex && xSemaphoreTake(state_mutex, pdMS_TO_TICKS(1)) == pdTRUE) {
    snapshot = state;
    xSemaphoreGive(state_mutex);
  } else {
    // Fallback: relaxed read without lock to avoid long blocking in time-critical loops
    snapshot = state;
  }
  return snapshot;
}

void UpdateState(const std::function<void(SharedState&)>& updater) {
  if (state_mutex && xSemaphoreTake(state_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
    updater(state);
    xSemaphoreGive(state_mutex);
  }
}

void ScheduleRestart() {
  xTaskCreate(
      [](void*) {
        vTaskDelay(pdMS_TO_TICKS(300));
        esp_restart();
      },
      "restart_task", 2048, nullptr, 5, nullptr);
}

UsbMode LoadUsbModeFromNvs(bool* found) {
  nvs_handle_t handle;
  uint8_t mode = static_cast<uint8_t>(UsbMode::kCdc);
  if (found) {
    *found = false;
  }
  if (nvs_open("usb", NVS_READONLY, &handle) == ESP_OK) {
    if (nvs_get_u8(handle, "mode", &mode) == ESP_OK && found) {
      *found = true;
    }
    nvs_close(handle);
  }
  return mode == static_cast<uint8_t>(UsbMode::kMsc) ? UsbMode::kMsc : UsbMode::kCdc;
}

void SaveUsbModeToNvs(UsbMode mode) {
  nvs_handle_t handle;
  if (nvs_open("usb", NVS_READWRITE, &handle) == ESP_OK) {
    uint8_t v = static_cast<uint8_t>(mode);
    nvs_set_u8(handle, "mode", v);
    nvs_commit(handle);
    nvs_close(handle);
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
  // err = adc3.Read(&raw3);
  // if (err != ESP_OK) {
  //   ESP_LOGE(TAG, "ADC3 read failed");
  //   StartErrorBlink();
  //   return err;
  // }

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
      UpdateState([&](SharedState& s) {
        s.voltage1 = v1 - s.offset1;
        s.voltage2 = v2 - s.offset2;
        s.voltage3 = v3 - s.offset3;
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

// HTTP handlers
esp_err_t RootHandler(httpd_req_t* req) {
  httpd_resp_set_type(req, "text/html");
  return httpd_resp_send(req, INDEX_HTML, HTTPD_RESP_USE_STRLEN);
}

esp_err_t DataHandler(httpd_req_t* req) {
  SharedState snapshot = CopyState();
  cJSON* root = cJSON_CreateObject();
  cJSON_AddNumberToObject(root, "voltage1", snapshot.voltage1);
  cJSON_AddNumberToObject(root, "voltage2", snapshot.voltage2);
  cJSON_AddNumberToObject(root, "voltage3", snapshot.voltage3);
  cJSON_AddNumberToObject(root, "inaBusVoltage", snapshot.ina_bus_voltage);
  cJSON_AddNumberToObject(root, "inaCurrent", snapshot.ina_current);
  cJSON_AddNumberToObject(root, "inaPower", snapshot.ina_power);
  cJSON_AddNumberToObject(root, "heaterPower", snapshot.heater_power);
  cJSON_AddNumberToObject(root, "fanPower", snapshot.fan_power);
  cJSON_AddNumberToObject(root, "fan1Rpm", snapshot.fan1_rpm);
  cJSON_AddNumberToObject(root, "fan2Rpm", snapshot.fan2_rpm);
  cJSON_AddNumberToObject(root, "tempSensorCount", snapshot.temp_sensor_count);
  cJSON* temp_array = cJSON_CreateArray();
  for (int i = 0; i < snapshot.temp_sensor_count && i < MAX_TEMP_SENSORS; ++i) {
    cJSON_AddItemToArray(temp_array, cJSON_CreateNumber(snapshot.temps_c[i]));
  }
  cJSON_AddItemToObject(root, "tempSensors", temp_array);
  cJSON* label_array = cJSON_CreateArray();
  for (int i = 0; i < snapshot.temp_sensor_count && i < MAX_TEMP_SENSORS; ++i) {
    const std::string& name = snapshot.temp_labels[i];
    if (!name.empty()) {
      cJSON_AddItemToArray(label_array, cJSON_CreateString(name.c_str()));
    } else {
      char buf[16];
      std::snprintf(buf, sizeof(buf), "Sensor %d", i + 1);
      cJSON_AddItemToArray(label_array, cJSON_CreateString(buf));
    }
  }
  cJSON_AddItemToObject(root, "tempLabels", label_array);
  cJSON* addr_array = cJSON_CreateArray();
  for (int i = 0; i < snapshot.temp_sensor_count && i < MAX_TEMP_SENSORS; ++i) {
    const std::string& addr = snapshot.temp_addresses[i];
    cJSON_AddItemToArray(addr_array, cJSON_CreateString(addr.c_str()));
  }
  cJSON_AddItemToObject(root, "tempAddresses", addr_array);
  cJSON_AddBoolToObject(root, "logging", snapshot.logging);
  cJSON_AddStringToObject(root, "logFilename", snapshot.log_filename.c_str());
  cJSON_AddBoolToObject(root, "logUseMotor", snapshot.log_use_motor);
  cJSON_AddNumberToObject(root, "logDuration", snapshot.log_duration_s);
  cJSON_AddBoolToObject(root, "wifiApMode", app_config.wifi_ap_mode);
  cJSON_AddStringToObject(root, "wifiSsid", app_config.wifi_ssid.c_str());
  cJSON_AddStringToObject(root, "wifiPassword", app_config.wifi_password.c_str());
  cJSON_AddBoolToObject(root, "pidEnabled", snapshot.pid_enabled);
  cJSON_AddNumberToObject(root, "pidSetpoint", snapshot.pid_setpoint);
  cJSON_AddNumberToObject(root, "pidSensorIndex", snapshot.pid_sensor_index);
  cJSON_AddNumberToObject(root, "pidKp", snapshot.pid_kp);
  cJSON_AddNumberToObject(root, "pidKi", snapshot.pid_ki);
  cJSON_AddNumberToObject(root, "pidKd", snapshot.pid_kd);
  cJSON_AddNumberToObject(root, "pidOutput", snapshot.pid_output);
  cJSON_AddStringToObject(root, "wifiMode", app_config.wifi_ap_mode ? "ap" : "sta");
  cJSON_AddNumberToObject(root, "timestamp", snapshot.last_update_ms);
  cJSON_AddBoolToObject(root, "stepperEnabled", snapshot.stepper_enabled);
  cJSON_AddNumberToObject(root, "stepperPosition", snapshot.stepper_position);
  cJSON_AddNumberToObject(root, "stepperTarget", snapshot.stepper_target);
  cJSON_AddNumberToObject(root, "stepperSpeedUs", snapshot.stepper_speed_us);
  cJSON_AddBoolToObject(root, "stepperMoving", snapshot.stepper_moving);
  cJSON_AddStringToObject(root, "usbMode", snapshot.usb_msc_mode ? "msc" : "cdc");
  cJSON_AddBoolToObject(root, "usbMscBuilt", CONFIG_TINYUSB_MSC_ENABLED);
  if (!snapshot.usb_error.empty()) {
    cJSON_AddStringToObject(root, "usbError", snapshot.usb_error.c_str());
  }

  const char* resp = cJSON_PrintUnformatted(root);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_sendstr(req, resp);
  cJSON_free((void*)resp);
  cJSON_Delete(root);
  return ESP_OK;
}

esp_err_t CalibrateHandler(httpd_req_t* req) {
  if (calibration_task == nullptr) {
    xTaskCreatePinnedToCore(&CalibrationTask, "calibrate", 4096, nullptr, 4, &calibration_task, tskNO_AFFINITY);
  }
  httpd_resp_set_type(req, "application/json");
  return httpd_resp_sendstr(req, "{\"status\":\"calibration_started\"}");
}

esp_err_t StepperEnableHandler(httpd_req_t* req) {
  EnableStepper();
  httpd_resp_set_type(req, "application/json");
  return httpd_resp_sendstr(req, "{\"status\":\"stepper_enabled\"}");
}

esp_err_t StepperDisableHandler(httpd_req_t* req) {
  DisableStepper();
  httpd_resp_set_type(req, "application/json");
  return httpd_resp_sendstr(req, "{\"status\":\"stepper_disabled\"}");
}

esp_err_t StepperMoveHandler(httpd_req_t* req) {
  const size_t buf_len = std::min<size_t>(req->content_len, 256);
  if (buf_len == 0) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing body");
    return ESP_FAIL;
  }
  std::string body(buf_len, '\0');
  int received = httpd_req_recv(req, body.data(), buf_len);
  if (received <= 0) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing body");
    return ESP_FAIL;
  }
  body.resize(received);

  cJSON* root = cJSON_Parse(body.c_str());
  if (!root) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
    return ESP_FAIL;
  }
  int steps = cJSON_GetObjectItem(root, "steps") ? cJSON_GetObjectItem(root, "steps")->valueint : 400;
  const char* direction_str = cJSON_GetObjectItem(root, "direction") && cJSON_GetObjectItem(root, "direction")->valuestring
                                  ? cJSON_GetObjectItem(root, "direction")->valuestring
                                  : "forward";
  int speed = cJSON_GetObjectItem(root, "speed") ? cJSON_GetObjectItem(root, "speed")->valueint : app_config.stepper_speed_us;
  cJSON_Delete(root);

  steps = std::clamp(steps, 1, 10000);
  if (speed <= 0) {
    speed = 1;
  }

  SharedState snapshot = CopyState();
  if (!snapshot.stepper_enabled) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Stepper not enabled");
    return ESP_FAIL;
  }

  bool forward = std::strcmp(direction_str, "forward") == 0;
  StartStepperMove(steps, forward, speed);
  app_config.stepper_speed_us = speed;
  SaveConfigToSdCard(app_config, pid_config, usb_mode);

  httpd_resp_set_type(req, "application/json");
  return httpd_resp_sendstr(req, "{\"status\":\"movement_started\"}");
}

esp_err_t StepperStopHandler(httpd_req_t* req) {
  StopStepper();
  httpd_resp_set_type(req, "application/json");
  return httpd_resp_sendstr(req, "{\"status\":\"movement_stopped\"}");
}

esp_err_t StepperZeroHandler(httpd_req_t* req) {
  UpdateState([](SharedState& s) {
    s.stepper_position = 0;
    s.stepper_target = 0;
  });
  httpd_resp_set_type(req, "application/json");
  return httpd_resp_sendstr(req, "{\"status\":\"position_zeroed\"}");
}

void FindZeroTask(void*) {
  UpdateState([](SharedState& s) { s.stepper_abort = false; });
  const bool hall_initial = IsHallTriggered();
  ESP_LOGI(TAG, "FindZero: start, hall_triggered=%s", hall_initial ? "yes" : "no");
  UpdateState([](SharedState& s) { s.homing = true; });
  SharedState snapshot = CopyState();
  const int step_delay_us = std::max(snapshot.stepper_speed_us, 1);
  int steps = 0;
  const int max_steps = 20000;
  gpio_set_level(STEPPER_DIR, 0);
  while (!IsHallTriggered() && steps < max_steps) {
    if (CopyState().stepper_abort) {
      ESP_LOGW(TAG, "FindZero: aborted by user after %d steps", steps);
      break;
    }
    gpio_set_level(STEPPER_STEP, 1);
    esp_rom_delay_us(4);
    gpio_set_level(STEPPER_STEP, 0);
    UpdateState([](SharedState& s) {
      s.stepper_position -= 1;
      s.stepper_target = s.stepper_position;
    });
    esp_rom_delay_us(step_delay_us);
    steps++;
  }
  const bool hall_after = IsHallTriggered();
  if (hall_after) {
    ESP_LOGI(TAG, "FindZero: hall detected after %d steps", steps);
  } else {
    ESP_LOGW(TAG, "FindZero: hall NOT detected, stopped after %d steps", steps);
  }
  UpdateState([](SharedState& s) {
    s.stepper_position = 0;
    s.stepper_target = 0;
    s.stepper_moving = false;
    s.homing = false;
    s.stepper_abort = false;
  });
  find_zero_task = nullptr;
  vTaskDelete(nullptr);
}

bool MountLogSd() {
  if (log_sd_mounted) {
    return true;
  }
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

  esp_err_t ret = esp_vfs_fat_sdmmc_mount(CONFIG_MOUNT_POINT, &host, &slot_config, &mount_config, &log_sd_card);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "SD mount for logging failed: %s", esp_err_to_name(ret));
    return false;
  }
  log_sd_mounted = true;
  return true;
}

void UnmountLogSd() {
  if (log_sd_mounted) {
    esp_vfs_fat_sdcard_unmount(CONFIG_MOUNT_POINT, log_sd_card);
    log_sd_mounted = false;
    log_sd_card = nullptr;
  }
}

static bool SaveConfigToSdCard(const AppConfig& cfg, const PidConfig& pid, UsbMode current_usb_mode) {
  SdLockGuard guard;
  if (!guard.locked()) {
    ESP_LOGW(TAG, "SD mutex unavailable, skip config save");
    return false;
  }
  const bool already_mounted = log_sd_mounted;
  if (!already_mounted) {
    if (!MountLogSd()) {
      return false;
    }
  }
  FILE* f = fopen(CONFIG_FILE_PATH, "w");
  if (!f) {
    ESP_LOGE(TAG, "Failed to open %s for writing", CONFIG_FILE_PATH);
    if (!already_mounted && !log_file) {
      UnmountLogSd();
    }
    return false;
  }
  fprintf(f, "# Config generated by device\n");
  fprintf(f, "wifi_ssid = %s\n", cfg.wifi_ssid.c_str());
  fprintf(f, "wifi_password = %s\n", cfg.wifi_password.c_str());
  fprintf(f, "wifi_ap_mode = %s\n", (cfg.wifi_ap_mode ? "true" : "false"));
  fprintf(f, "usb_mass_storage = %s\n", (current_usb_mode == UsbMode::kMsc) ? "true" : "false");
  fprintf(f, "logging_active = %s\n", (cfg.logging_active ? "true" : "false"));
  if (!cfg.logging_postfix.empty()) {
    fprintf(f, "logging_postfix = %s\n", cfg.logging_postfix.c_str());
  }
  fprintf(f, "logging_use_motor = %s\n", (cfg.logging_use_motor ? "true" : "false"));
  fprintf(f, "logging_duration_s = %.3f\n", cfg.logging_duration_s);
  fprintf(f, "stepper_speed_us = %d\n", cfg.stepper_speed_us);
  fprintf(f, "pid_kp = %.6f\n", pid.kp);
  fprintf(f, "pid_ki = %.6f\n", pid.ki);
  fprintf(f, "pid_kd = %.6f\n", pid.kd);
  fprintf(f, "pid_setpoint = %.6f\n", pid.setpoint);
  fprintf(f, "pid_sensor = %d\n", pid.sensor_index);
  fprintf(f, "pid_enabled = %s\n", (CopyState().pid_enabled ? "true" : "false"));
  fclose(f);
  // Keep mounted if logging is active to avoid invalidating open log file.
  if (!already_mounted && !log_file) {
    UnmountLogSd();
  }
  ESP_LOGI(TAG, "Config saved to %s", CONFIG_FILE_PATH);
  return true;
}

void StopLogging() {
  SdLockGuard guard;
  if (guard.locked()) {
    if (log_file) {
      fclose(log_file);
      log_file = nullptr;
    }
  } else if (log_file) {
    fclose(log_file);
    log_file = nullptr;
  }
  UnmountLogSd();
  UpdateState([](SharedState& s) {
    s.logging = false;
    s.log_filename.clear();
  });
  log_config.active = false;
  log_config.homed_once = false;
  log_config.postfix.clear();
  log_config.file_start_us = 0;
  if (log_task) {
    vTaskDelete(log_task);
    log_task = nullptr;
  }
}

void LoggingTask(void*) {
  const int steps_180 = 200;  // adjust to your mechanics (microsteps for 180 degrees)

  auto home_blocking = [&]() {
    UpdateState([](SharedState& s) { s.homing = true; });
    EnableStepper();
    gpio_set_level(STEPPER_DIR, 0);
    int steps = 0;
    const int max_steps = 20000;
    const int step_delay_us = std::max(CopyState().stepper_speed_us, 1);
    while (!IsHallTriggered() && steps < max_steps) {
      if (CopyState().stepper_abort) {
        ESP_LOGW(TAG, "Logging home aborted after %d steps", steps);
        break;
      }
      gpio_set_level(STEPPER_STEP, 1);
      esp_rom_delay_us(4);
      gpio_set_level(STEPPER_STEP, 0);
      esp_rom_delay_us(step_delay_us);
      steps++;
    }
    DisableStepper();
    UpdateState([](SharedState& s) {
      s.stepper_position = 0;
      s.stepper_target = 0;
      s.stepper_moving = false;
      s.homing = false;
    });
  };

  auto move_blocking = [&](int steps, bool forward) {
    UpdateState([](SharedState& s) { s.homing = true; });
    EnableStepper();
    gpio_set_level(STEPPER_DIR, forward ? 1 : 0);
    const int step_delay_us = std::max(CopyState().stepper_speed_us, 1);
    for (int i = 0; i < steps; ++i) {
      if (CopyState().stepper_abort) {
        ESP_LOGW(TAG, "Logging move aborted after %d/%d steps", i, steps);
        break;
      }
      gpio_set_level(STEPPER_STEP, 1);
      esp_rom_delay_us(4);
      gpio_set_level(STEPPER_STEP, 0);
      esp_rom_delay_us(step_delay_us);
    }
    DisableStepper();
    UpdateState([&](SharedState& s) {
      s.homing = false;
      s.stepper_position += forward ? steps : -steps;
      s.stepper_target = s.stepper_position;
    });
  };

  auto collect_avg = [&](float duration_s, int temp_count, SharedState* out) -> bool {
    if (!out) return false;
    const TickType_t interval = pdMS_TO_TICKS(200);
    const uint64_t duration_ms = static_cast<uint64_t>(duration_s * 1000.0f);
    uint64_t start = esp_timer_get_time() / 1000ULL;
    int samples = 0;
    double sum_v1 = 0, sum_v2 = 0, sum_v3 = 0;
    std::array<double, MAX_TEMP_SENSORS> temp_sum{};
    double sum_bus_v = 0, sum_bus_i = 0, sum_bus_p = 0;
    while ((esp_timer_get_time() / 1000ULL - start) < duration_ms) {
      SharedState snap = CopyState();
      sum_v1 += snap.voltage1;
      sum_v2 += snap.voltage2;
      sum_v3 += snap.voltage3;
      for (int i = 0; i < temp_count && i < MAX_TEMP_SENSORS; ++i) {
        temp_sum[i] += snap.temps_c[i];
      }
      sum_bus_v += snap.ina_bus_voltage;
      sum_bus_i += snap.ina_current;
      sum_bus_p += snap.ina_power;
      samples++;
      vTaskDelay(interval);
    }
    if (samples == 0) return false;
    out->voltage1 = sum_v1 / samples;
    out->voltage2 = sum_v2 / samples;
    out->voltage3 = sum_v3 / samples;
    out->ina_bus_voltage = sum_bus_v / samples;
    out->ina_current = sum_bus_i / samples;
    out->ina_power = sum_bus_p / samples;
    out->temp_sensor_count = temp_count;
    for (int i = 0; i < temp_count && i < MAX_TEMP_SENSORS; ++i) {
      out->temps_c[i] = static_cast<float>(temp_sum[i] / samples);
    }
    return true;
  };

  while (true) {
    SharedState current = CopyState();
    if (!current.logging || !log_file) {
      StopLogging();
      vTaskDelete(nullptr);
    }

    // Rotate log every hour
    const uint64_t now_us = esp_timer_get_time();
    if (log_config.file_start_us > 0 && (now_us - log_config.file_start_us) >= 3'600'000'000ULL) {
      ESP_LOGI(TAG, "Rotating log file after 1 hour");
      if (!OpenLogFileWithPostfix(log_config.postfix)) {
        StopLogging();
        vTaskDelete(nullptr);
      }
      continue;
    }

    if (log_config.use_motor) {
      EnableStepper();
      home_blocking();
      if (CopyState().stepper_abort) {
        ESP_LOGW(TAG, "Logging aborted during homing");
        StopLogging();
        vTaskDelete(nullptr);
      }
    } else if (!log_config.homed_once) {
      EnableStepper();
      home_blocking();
      DisableStepper();
      log_config.homed_once = true;
      if (CopyState().stepper_abort) {
        ESP_LOGW(TAG, "Logging aborted during initial homing");
        StopLogging();
        vTaskDelete(nullptr);
      }
    }

    SharedState avg1{};
    if (!collect_avg(log_config.duration_s, current.temp_sensor_count, &avg1)) {
      ESP_LOGW(TAG, "Logging: no samples collected, retrying");
      vTaskDelay(pdMS_TO_TICKS(500));
      continue;
    }

    bool have_cal = false;
    SharedState avg2{};
    if (log_config.use_motor) {
      move_blocking(steps_180, false);
      have_cal = collect_avg(log_config.duration_s, current.temp_sensor_count, &avg2);
      if (!have_cal) {
        ESP_LOGW(TAG, "Logging: cal samples unavailable, skipping cal block this cycle");
      }
    }

    SdLockGuard guard(pdMS_TO_TICKS(2000));
    if (!guard.locked()) {
      ESP_LOGW(TAG, "SD mutex unavailable, retrying logging write");
      vTaskDelay(pdMS_TO_TICKS(200));
      continue;
    }
    uint64_t ts_ms = esp_timer_get_time() / 1000ULL;
    const std::string iso = IsoUtcNow();
    fprintf(log_file, "%s,%llu,%.6f,%.6f", iso.c_str(), (unsigned long long)ts_ms, avg1.voltage1, avg1.voltage2);
    for (int i = 0; i < avg1.temp_sensor_count && i < MAX_TEMP_SENSORS; ++i) {
      fprintf(log_file, ",%.2f", avg1.temps_c[i]);
    }
    fprintf(log_file, ",%.3f,%.3f,%.3f", avg1.ina_bus_voltage, avg1.ina_current, avg1.ina_power);
    if (log_config.use_motor && have_cal) {
      fprintf(log_file, ",%.6f,%.6f", avg2.voltage1, avg2.voltage2);
    }
    fprintf(log_file, "\n");
    FlushLogFile();
    ESP_LOGI(TAG, "Logging: wrote row ts=%llu iso=%s", (unsigned long long)ts_ms, iso.c_str());
  }
}

bool StartLoggingToFile(const std::string& postfix_raw, UsbMode current_usb_mode) {
  if (current_usb_mode == UsbMode::kMsc) {
    ESP_LOGW(TAG, "Cannot start logging in MSC mode");
    return false;
  }
  const std::string postfix = SanitizePostfix(postfix_raw);
  log_config.postfix = postfix;
  log_config.active = true;
  log_config.homed_once = false;
  if (log_config.duration_s <= 0.0f) {
    log_config.duration_s = 1.0f;
  }
  if (!OpenLogFileWithPostfix(postfix)) {
    log_config.active = false;
    return false;
  }

  if (log_task == nullptr) {
    xTaskCreatePinnedToCore(&LoggingTask, "log_task", 4096, nullptr, 2, &log_task, 0);
  }
  ESP_LOGI(TAG, "Logging started");
  UpdateState([&](SharedState& s) {
    s.logging = true;
    s.log_use_motor = log_config.use_motor;
    s.log_duration_s = log_config.duration_s;
  });
  return true;
}

esp_err_t StepperFindZeroHandler(httpd_req_t* req) {
  SharedState snapshot = CopyState();
  if (!snapshot.stepper_enabled) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Stepper not enabled");
    return ESP_FAIL;
  }
  if (find_zero_task == nullptr) {
    xTaskCreatePinnedToCore(&FindZeroTask, "find_zero", 4096, nullptr, 4, &find_zero_task, 1);
  }
  httpd_resp_set_type(req, "application/json");
  return httpd_resp_sendstr(req, "{\"status\":\"homing_started\"}");
}

esp_err_t HeaterSetHandler(httpd_req_t* req) {
  const size_t buf_len = std::min<size_t>(req->content_len, 64);
  if (buf_len == 0) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing body");
    return ESP_FAIL;
  }
  std::string body(buf_len, '\0');
  int received = httpd_req_recv(req, body.data(), buf_len);
  if (received <= 0) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Failed to read body");
    return ESP_FAIL;
  }
  body.resize(received);

  cJSON* root = cJSON_Parse(body.c_str());
  if (!root) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
    return ESP_FAIL;
  }
  float power = 0.0f;
  cJSON* power_item = cJSON_GetObjectItem(root, "power");
  if (power_item && cJSON_IsNumber(power_item)) {
    power = static_cast<float>(power_item->valuedouble);
  } else {
    cJSON_Delete(root);
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing power");
    return ESP_FAIL;
  }
  cJSON_Delete(root);

  HeaterSetPowerPercent(power);

  httpd_resp_set_type(req, "application/json");
  return httpd_resp_sendstr(req, "{\"status\":\"ok\"}");
}

esp_err_t FanSetHandler(httpd_req_t* req) {
  const size_t buf_len = std::min<size_t>(req->content_len, 64);
  if (buf_len == 0) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing body");
    return ESP_FAIL;
  }
  std::string body(buf_len, '\0');
  int received = httpd_req_recv(req, body.data(), buf_len);
  if (received <= 0) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Failed to read body");
    return ESP_FAIL;
  }
  body.resize(received);

  cJSON* root = cJSON_Parse(body.c_str());
  if (!root) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
    return ESP_FAIL;
  }
  cJSON* val = cJSON_GetObjectItem(root, "power");
  if (!val || !cJSON_IsNumber(val)) {
    cJSON_Delete(root);
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing power");
    return ESP_FAIL;
  }
  float p = static_cast<float>(val->valuedouble);
  cJSON_Delete(root);

  FanSetPowerPercent(p);

  httpd_resp_set_type(req, "application/json");
  return httpd_resp_sendstr(req, "{\"status\":\"ok\"}");
}

esp_err_t LogStartHandler(httpd_req_t* req) {
  const size_t buf_len = std::min<size_t>(req->content_len, 96);
  if (buf_len == 0) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing body");
    return ESP_FAIL;
  }
  std::string body(buf_len, '\0');
  int received = httpd_req_recv(req, body.data(), buf_len);
  if (received <= 0) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Failed to read body");
    return ESP_FAIL;
  }
  body.resize(received);

  cJSON* root = cJSON_Parse(body.c_str());
  if (!root) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
    return ESP_FAIL;
  }
  const cJSON* filename_item = cJSON_GetObjectItem(root, "filename");
  std::string filename = filename_item && cJSON_IsString(filename_item) && filename_item->valuestring
                             ? filename_item->valuestring
                             : "";
  cJSON* use_motor_item = cJSON_GetObjectItem(root, "useMotor");
  cJSON* duration_item = cJSON_GetObjectItem(root, "durationSec");
  bool use_motor = (use_motor_item && cJSON_IsBool(use_motor_item)) ? cJSON_IsTrue(use_motor_item) : false;
  float duration_s =
      (duration_item && cJSON_IsNumber(duration_item) && duration_item->valuedouble > 0.1) ? duration_item->valuedouble : 1.0f;
  cJSON_Delete(root);

  log_config.use_motor = use_motor;
  log_config.duration_s = duration_s;

  if (!StartLoggingToFile(filename, usb_mode)) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Failed to start logging");
    return ESP_FAIL;
  }

  app_config.logging_active = true;
  app_config.logging_postfix = SanitizePostfix(filename);
  app_config.logging_use_motor = use_motor;
  app_config.logging_duration_s = duration_s;
  UpdateState([&](SharedState& s) {
    s.logging = true;
    s.log_use_motor = use_motor;
    s.log_duration_s = duration_s;
  });
  SaveConfigToSdCard(app_config, pid_config, usb_mode);

  httpd_resp_set_type(req, "application/json");
  return httpd_resp_sendstr(req, "{\"status\":\"logging_started\"}");
}

esp_err_t LogStopHandler(httpd_req_t* req) {
  StopLogging();
  app_config.logging_active = false;
  app_config.logging_use_motor = false;
  SaveConfigToSdCard(app_config, pid_config, usb_mode);
  UpdateState([](SharedState& s) {
    s.logging = false;
    s.log_use_motor = false;
  });
  httpd_resp_set_type(req, "application/json");
  return httpd_resp_sendstr(req, "{\"status\":\"logging_stopped\"}");
}

esp_err_t FsListHandler(httpd_req_t* req) {
  SharedState snapshot = CopyState();
  if (snapshot.usb_msc_mode) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "MSC mode active");
    return ESP_FAIL;
  }

  SdLockGuard guard;
  if (!guard.locked()) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "SD busy");
    return ESP_FAIL;
  }
  if (!MountLogSd()) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "SD mount failed");
    return ESP_FAIL;
  }

  DIR* dir = opendir(CONFIG_MOUNT_POINT);
  if (!dir) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Open dir failed");
    return ESP_FAIL;
  }

  cJSON* arr = cJSON_CreateArray();
  struct dirent* ent = nullptr;
  while ((ent = readdir(dir)) != nullptr) {
    if (ent->d_name[0] == '.') continue;
    if (ent->d_type != DT_REG && ent->d_type != DT_UNKNOWN) continue;
    std::string full_path = std::string(CONFIG_MOUNT_POINT) + "/" + ent->d_name;
    struct stat st {};
    uint64_t size = 0;
    if (stat(full_path.c_str(), &st) == 0) {
      size = static_cast<uint64_t>(st.st_size);
    }
    cJSON* obj = cJSON_CreateObject();
    cJSON_AddStringToObject(obj, "name", ent->d_name);
    cJSON_AddNumberToObject(obj, "size", static_cast<double>(size));
    cJSON_AddItemToArray(arr, obj);
  }
  closedir(dir);

  const char* json = cJSON_PrintUnformatted(arr);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_sendstr(req, json);
  cJSON_free((void*)json);
  cJSON_Delete(arr);
  return ESP_OK;
}

esp_err_t FsDownloadHandler(httpd_req_t* req) {
  SharedState snapshot = CopyState();
  if (snapshot.usb_msc_mode) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "MSC mode active");
    return ESP_FAIL;
  }

  int qs_len = httpd_req_get_url_query_len(req) + 1;
  if (qs_len <= 1) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing query");
    return ESP_FAIL;
  }
  std::string qs(qs_len, '\0');
  if (httpd_req_get_url_query_str(req, qs.data(), qs_len) != ESP_OK) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad query");
    return ESP_FAIL;
  }
  char file_param[96] = {};
  if (httpd_query_key_value(qs.c_str(), "file", file_param, sizeof(file_param)) != ESP_OK) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing file param");
    return ESP_FAIL;
  }
  std::string full_path;
  if (!SanitizeFilename(file_param, &full_path)) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid filename");
    return ESP_FAIL;
  }

  SdLockGuard guard;
  if (!guard.locked()) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "SD busy");
    return ESP_FAIL;
  }
  if (!MountLogSd()) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "SD mount failed");
    return ESP_FAIL;
  }

  FILE* f = fopen(full_path.c_str(), "rb");
  if (!f) {
    httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "File not found");
    return ESP_FAIL;
  }

  const char* ctype = "application/octet-stream";
  size_t len = full_path.size();
  if (len >= 4 && full_path.compare(len - 4, 4, ".csv") == 0) {
    ctype = "text/csv";
  }
  httpd_resp_set_type(req, ctype);
  std::string disp = "attachment; filename=\"";
  disp.append(file_param);
  disp.push_back('"');
  httpd_resp_set_hdr(req, "Content-Disposition", disp.c_str());

  char buf[1024];
  size_t n = 0;
  while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
    if (httpd_resp_send_chunk(req, buf, n) != ESP_OK) {
      fclose(f);
      httpd_resp_send_chunk(req, nullptr, 0);
      return ESP_FAIL;
    }
  }
  fclose(f);
  return httpd_resp_send_chunk(req, nullptr, 0);
}

esp_err_t FsDeleteHandler(httpd_req_t* req) {
  SharedState snapshot = CopyState();
  if (snapshot.usb_msc_mode) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "MSC mode active");
    return ESP_FAIL;
  }

  std::vector<std::string> requested_files;
  bool has_body_files = false;
  bool saw_body = false;
  const size_t kMaxDeleteBody = 8192;
  if (req->content_len > kMaxDeleteBody) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Body too large");
    return ESP_FAIL;
  }
  if (req->content_len > 0) {
    saw_body = true;
    std::string body(req->content_len, '\0');
    size_t offset = 0;
    while (offset < body.size()) {
      int received = httpd_req_recv(req, body.data() + offset, body.size() - offset);
      if (received <= 0) {
        if (received == HTTPD_SOCK_ERR_TIMEOUT) {
          continue;
        }
        break;
      }
      offset += static_cast<size_t>(received);
    }
    body.resize(offset);

    if (!body.empty()) {
      cJSON* root = cJSON_Parse(body.c_str());
      if (root) {
        auto collect = [&](cJSON* arr) {
          if (!arr || !cJSON_IsArray(arr)) return;
          has_body_files = true;
          cJSON* item = nullptr;
          cJSON_ArrayForEach(item, arr) {
            if (cJSON_IsString(item) && item->valuestring) {
              requested_files.emplace_back(item->valuestring);
            }
          }
        };
        if (cJSON_IsArray(root)) {
          collect(root);
        } else {
          collect(cJSON_GetObjectItem(root, "files"));
        }
        cJSON_Delete(root);
      }
    }
  }

  if (saw_body && !has_body_files) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON body");
    return ESP_FAIL;
  }

  if (!has_body_files) {
    int qs_len = httpd_req_get_url_query_len(req) + 1;
    if (qs_len <= 1) {
      httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing query");
      return ESP_FAIL;
    }
    std::string qs(qs_len, '\0');
    if (httpd_req_get_url_query_str(req, qs.data(), qs_len) != ESP_OK) {
      httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad query");
      return ESP_FAIL;
    }
    char file_param[96] = {};
    if (httpd_query_key_value(qs.c_str(), "file", file_param, sizeof(file_param)) != ESP_OK) {
      httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing file param");
      return ESP_FAIL;
    }
    requested_files.emplace_back(file_param);
  }

  auto is_protected_config = [](const std::string& name) {
    if (name.size() != 10) return false;
    std::string lower;
    lower.reserve(name.size());
    for (char c : name) {
      lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    return lower == "config.txt";
  };

  struct DeleteCandidate {
    std::string name;
    std::string full_path;
  };

  std::vector<DeleteCandidate> candidates;
  std::vector<std::string> skipped;
  std::vector<std::string> failed;
  std::set<std::string> seen;

  for (const auto& raw_name : requested_files) {
    if (!seen.insert(raw_name).second) continue;
    if (is_protected_config(raw_name)) {
      skipped.push_back(raw_name + " (protected)");
      continue;
    }

    std::string full_path;
    if (!SanitizeFilename(raw_name, &full_path)) {
      failed.push_back(raw_name + " (invalid)");
      continue;
    }

    if (snapshot.logging && snapshot.log_filename == raw_name) {
      skipped.push_back(raw_name + " (active log)");
      continue;
    }
    candidates.push_back({raw_name, std::move(full_path)});
  }

  auto send_result = [&](const std::vector<std::string>& deleted) -> esp_err_t {
    cJSON* resp = cJSON_CreateObject();
    auto add_array = [&](const char* key, const std::vector<std::string>& values) {
      cJSON* arr = cJSON_CreateArray();
      for (const auto& v : values) {
        cJSON_AddItemToArray(arr, cJSON_CreateString(v.c_str()));
      }
      cJSON_AddItemToObject(resp, key, arr);
    };
    add_array("deleted", deleted);
    add_array("skipped", skipped);
    add_array("failed", failed);
    const char* json = cJSON_PrintUnformatted(resp);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json);
    cJSON_free((void*)json);
    cJSON_Delete(resp);
    return ESP_OK;
  };

  if (candidates.empty()) {
    return send_result({});
  }

  SdLockGuard guard;
  if (!guard.locked()) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "SD busy");
    return ESP_FAIL;
  }
  if (!MountLogSd()) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "SD mount failed");
    return ESP_FAIL;
  }

  std::vector<std::string> deleted;
  for (const auto& entry : candidates) {
    if (remove(entry.full_path.c_str()) != 0) {
      failed.push_back(entry.name + " (delete failed)");
    } else {
      deleted.push_back(entry.name);
    }
  }

  return send_result(deleted);
}

esp_err_t PidApplyHandler(httpd_req_t* req) {
  const size_t buf_len = std::min<size_t>(req->content_len, 128);
  if (buf_len == 0) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing body");
    return ESP_FAIL;
  }
  std::string body(buf_len, '\0');
  int received = httpd_req_recv(req, body.data(), buf_len);
  if (received <= 0) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Failed to read body");
    return ESP_FAIL;
  }
  body.resize(received);

  cJSON* root = cJSON_Parse(body.c_str());
  if (!root) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
    return ESP_FAIL;
  }
  auto get_number = [&](const char* key, float* out) -> bool {
    cJSON* item = cJSON_GetObjectItem(root, key);
    if (item && cJSON_IsNumber(item)) {
      *out = static_cast<float>(item->valuedouble);
      return true;
    }
    return false;
  };
  float kp = pid_config.kp;
  float ki = pid_config.ki;
  float kd = pid_config.kd;
  float sp = pid_config.setpoint;
  int sensor = pid_config.sensor_index;

  get_number("kp", &kp);
  get_number("ki", &ki);
  get_number("kd", &kd);
  get_number("setpoint", &sp);
  cJSON* sensor_item = cJSON_GetObjectItem(root, "sensor");
  if (sensor_item && cJSON_IsNumber(sensor_item)) {
    sensor = sensor_item->valueint;
  }
  cJSON_Delete(root);

  SharedState snapshot = CopyState();
  if (snapshot.temp_sensor_count > 0) {
    sensor = std::clamp(sensor, 0, snapshot.temp_sensor_count - 1);
  } else if (sensor < 0) {
    sensor = 0;
  }

  pid_config.kp = kp;
  pid_config.ki = ki;
  pid_config.kd = kd;
  pid_config.setpoint = sp;
  pid_config.sensor_index = sensor;

  UpdateState([&](SharedState& s) {
    s.pid_kp = kp;
    s.pid_ki = ki;
    s.pid_kd = kd;
    s.pid_setpoint = sp;
    s.pid_sensor_index = sensor;
  });

  // Persist PID config and current enable state
  SaveConfigToSdCard(app_config, pid_config, usb_mode);

  httpd_resp_set_type(req, "application/json");
  return httpd_resp_sendstr(req, "{\"status\":\"pid_applied\"}");
}

esp_err_t PidEnableHandler(httpd_req_t* req) {
  SharedState snapshot = CopyState();
  if (snapshot.temp_sensor_count == 0) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No temp sensors");
    return ESP_FAIL;
  }
  UpdateState([](SharedState& s) { s.pid_enabled = true; });
  httpd_resp_set_type(req, "application/json");
  return httpd_resp_sendstr(req, "{\"status\":\"pid_enabled\"}");
}

esp_err_t PidDisableHandler(httpd_req_t* req) {
  UpdateState([](SharedState& s) { s.pid_enabled = false; });
  httpd_resp_set_type(req, "application/json");
  return httpd_resp_sendstr(req, "{\"status\":\"pid_disabled\"}");
}

esp_err_t UsbModeGetHandler(httpd_req_t* req) {
  httpd_resp_set_type(req, "application/json");
  if (usb_mode == UsbMode::kMsc) {
    return httpd_resp_sendstr(req, "{\"mode\":\"msc\"}");
  }
  return httpd_resp_sendstr(req, "{\"mode\":\"cdc\"}");
}

esp_err_t UsbModeSetHandler(httpd_req_t* req) {
  const size_t buf_len = std::min<size_t>(req->content_len, 64);
  if (buf_len == 0) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing body");
    return ESP_FAIL;
  }
  std::string body(buf_len, '\0');
  int received = httpd_req_recv(req, body.data(), buf_len);
  if (received <= 0) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Failed to read body");
    return ESP_FAIL;
  }
  body.resize(received);

  cJSON* root = cJSON_Parse(body.c_str());
  if (!root) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
    return ESP_FAIL;
  }
  const char* mode_str = cJSON_GetObjectItem(root, "mode") && cJSON_GetObjectItem(root, "mode")->valuestring
                             ? cJSON_GetObjectItem(root, "mode")->valuestring
                             : "";
  UsbMode requested = (std::strcmp(mode_str, "msc") == 0) ? UsbMode::kMsc : UsbMode::kCdc;
  cJSON_Delete(root);

#if !CONFIG_TINYUSB_MSC_ENABLED
  if (requested == UsbMode::kMsc) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "MSC not enabled in firmware");
    return ESP_FAIL;
  }
#endif

  if (requested == usb_mode) {
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"status\":\"unchanged\"}");
  }

  // If leaving MSC, unmount before reboot to keep host happy
  if (usb_mode == UsbMode::kMsc && requested == UsbMode::kCdc) {
    tinyusb_msc_storage_unmount();
  }

  SaveUsbModeToNvs(requested);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_sendstr(req, "{\"status\":\"restarting\",\"mode\":\"pending\"}");
  ScheduleRestart();
  return ESP_OK;
}

esp_err_t WifiApplyHandler(httpd_req_t* req) {
  const size_t buf_len = std::min<size_t>(req->content_len, 160);
  if (buf_len == 0) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing body");
    return ESP_FAIL;
  }
  std::string body(buf_len, '\0');
  int received = httpd_req_recv(req, body.data(), buf_len);
  if (received <= 0) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Failed to read body");
    return ESP_FAIL;
  }
  body.resize(received);

  cJSON* root = cJSON_Parse(body.c_str());
  if (!root) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
    return ESP_FAIL;
  }
  cJSON* mode_item = cJSON_GetObjectItem(root, "mode");
  cJSON* ssid_item = cJSON_GetObjectItem(root, "ssid");
  cJSON* pass_item = cJSON_GetObjectItem(root, "password");
  std::string mode = (mode_item && cJSON_IsString(mode_item) && mode_item->valuestring) ? mode_item->valuestring : "sta";
  std::string ssid = (ssid_item && cJSON_IsString(ssid_item) && ssid_item->valuestring) ? ssid_item->valuestring : "";
  std::string pass = (pass_item && cJSON_IsString(pass_item) && pass_item->valuestring) ? pass_item->valuestring : "";
  cJSON_Delete(root);

  if (ssid.empty() || ssid.size() >= WIFI_SSID_MAX_LEN) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid SSID");
    return ESP_FAIL;
  }
  if (pass.size() >= WIFI_PASSWORD_MAX_LEN) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid password");
    return ESP_FAIL;
  }

  app_config.wifi_ssid = ssid;
  app_config.wifi_password = pass;
  app_config.wifi_ap_mode = (mode == "ap");
  app_config.wifi_from_file = true;
  SaveConfigToSdCard(app_config, pid_config, usb_mode);

  InitWifi(app_config.wifi_ssid, app_config.wifi_password, app_config.wifi_ap_mode);

  httpd_resp_set_type(req, "application/json");
  return httpd_resp_sendstr(req, "{\"status\":\"wifi_applied\"}");
}

httpd_handle_t StartHttpServer() {
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = 80;
  config.lru_purge_enable = true;
  config.max_uri_handlers = 26;
  config.stack_size = 8192;

  if (httpd_start(&http_server, &config) != ESP_OK) {
    ESP_LOGE(TAG, "Failed to start HTTP server");
    return nullptr;
  }

  httpd_uri_t root_uri = {.uri = "/", .method = HTTP_GET, .handler = RootHandler, .user_ctx = nullptr};
  httpd_uri_t data_uri = {.uri = "/data", .method = HTTP_GET, .handler = DataHandler, .user_ctx = nullptr};
  httpd_uri_t calibrate_uri = {.uri = "/calibrate", .method = HTTP_POST, .handler = CalibrateHandler, .user_ctx = nullptr};
  httpd_uri_t stepper_enable_uri = {.uri = "/stepper/enable", .method = HTTP_POST, .handler = StepperEnableHandler, .user_ctx = nullptr};
  httpd_uri_t stepper_disable_uri = {.uri = "/stepper/disable", .method = HTTP_POST, .handler = StepperDisableHandler, .user_ctx = nullptr};
  httpd_uri_t stepper_move_uri = {.uri = "/stepper/move", .method = HTTP_POST, .handler = StepperMoveHandler, .user_ctx = nullptr};
  httpd_uri_t stepper_stop_uri = {.uri = "/stepper/stop", .method = HTTP_POST, .handler = StepperStopHandler, .user_ctx = nullptr};
  httpd_uri_t stepper_zero_uri = {.uri = "/stepper/zero", .method = HTTP_POST, .handler = StepperZeroHandler, .user_ctx = nullptr};
  httpd_uri_t stepper_find_zero_uri = {.uri = "/stepper/find_zero", .method = HTTP_POST, .handler = StepperFindZeroHandler, .user_ctx = nullptr};
  httpd_uri_t usb_mode_get_uri = {.uri = "/usb/mode", .method = HTTP_GET, .handler = UsbModeGetHandler, .user_ctx = nullptr};
  httpd_uri_t usb_mode_set_uri = {.uri = "/usb/mode", .method = HTTP_POST, .handler = UsbModeSetHandler, .user_ctx = nullptr};
  httpd_uri_t wifi_apply_uri = {.uri = "/wifi/apply", .method = HTTP_POST, .handler = WifiApplyHandler, .user_ctx = nullptr};
  httpd_uri_t heater_set_uri = {.uri = "/heater/set", .method = HTTP_POST, .handler = HeaterSetHandler, .user_ctx = nullptr};
  httpd_uri_t fan_set_uri = {.uri = "/fan/set", .method = HTTP_POST, .handler = FanSetHandler, .user_ctx = nullptr};
  httpd_uri_t log_start_uri = {.uri = "/log/start", .method = HTTP_POST, .handler = LogStartHandler, .user_ctx = nullptr};
  httpd_uri_t log_stop_uri = {.uri = "/log/stop", .method = HTTP_POST, .handler = LogStopHandler, .user_ctx = nullptr};
  httpd_uri_t pid_apply_uri = {.uri = "/pid/apply", .method = HTTP_POST, .handler = PidApplyHandler, .user_ctx = nullptr};
  httpd_uri_t pid_enable_uri = {.uri = "/pid/enable", .method = HTTP_POST, .handler = PidEnableHandler, .user_ctx = nullptr};
  httpd_uri_t pid_disable_uri = {.uri = "/pid/disable", .method = HTTP_POST, .handler = PidDisableHandler, .user_ctx = nullptr};
  httpd_uri_t fs_list_uri = {.uri = "/fs/list", .method = HTTP_GET, .handler = FsListHandler, .user_ctx = nullptr};
  httpd_uri_t fs_download_uri = {.uri = "/fs/download", .method = HTTP_GET, .handler = FsDownloadHandler, .user_ctx = nullptr};
  httpd_uri_t fs_delete_uri = {.uri = "/fs/delete", .method = HTTP_POST, .handler = FsDeleteHandler, .user_ctx = nullptr};

  httpd_register_uri_handler(http_server, &root_uri);
  httpd_register_uri_handler(http_server, &data_uri);
  httpd_register_uri_handler(http_server, &calibrate_uri);
  httpd_register_uri_handler(http_server, &stepper_enable_uri);
  httpd_register_uri_handler(http_server, &stepper_disable_uri);
  httpd_register_uri_handler(http_server, &stepper_move_uri);
  httpd_register_uri_handler(http_server, &stepper_stop_uri);
  httpd_register_uri_handler(http_server, &stepper_zero_uri);
  httpd_register_uri_handler(http_server, &stepper_find_zero_uri);
  httpd_register_uri_handler(http_server, &usb_mode_get_uri);
  httpd_register_uri_handler(http_server, &usb_mode_set_uri);
  httpd_register_uri_handler(http_server, &wifi_apply_uri);
  httpd_register_uri_handler(http_server, &heater_set_uri);
  httpd_register_uri_handler(http_server, &fan_set_uri);
  httpd_register_uri_handler(http_server, &log_start_uri);
  httpd_register_uri_handler(http_server, &log_stop_uri);
  httpd_register_uri_handler(http_server, &pid_apply_uri);
  httpd_register_uri_handler(http_server, &pid_enable_uri);
  httpd_register_uri_handler(http_server, &pid_disable_uri);
  httpd_register_uri_handler(http_server, &fs_list_uri);
  httpd_register_uri_handler(http_server, &fs_download_uri);
  httpd_register_uri_handler(http_server, &fs_delete_uri);
  ESP_LOGI(TAG, "HTTP server started");
  return http_server;
}

}  // namespace

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
  // ESP_ERROR_CHECK(adc3.Init(SPI2_HOST, ADC_SPI_FREQ_HZ));
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

  init_ok = init_ok && msc_ok && (ina_err == ESP_OK);
  SetStatusLeds(init_ok);
  ESP_LOGI(TAG, "System ready");
}
