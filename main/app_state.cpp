#include "app_state.h"

#include "esp_system.h"
#include "nvs_flash.h"
#include "nvs.h"

// FreeRTOS
#include "freertos/task.h"

AppConfig app_config{
    DEFAULT_WIFI_SSID,  // wifi_ssid
    DEFAULT_WIFI_PASS,  // wifi_password
    false,              // wifi_from_file
    false,              // wifi_ap_mode
    false,              // usb_mass_storage
    false,              // usb_mass_storage_from_file
    false,              // logging_active
    "",                 // logging_postfix
    false,              // logging_use_motor
    1.0f,               // logging_duration_s
    1500,               // stepper_speed_us
    "dev1",             // device_id
    "",                 // minio_endpoint
    "",                 // minio_access_key
    "",                 // minio_secret_key
    "",                 // minio_bucket
    false,              // minio_enabled
    "",                 // mqtt_uri
    "",                 // mqtt_user
    "",                 // mqtt_password
    false,              // mqtt_enabled
};

PidConfig pid_config{
    1.0f,  // kp
    0.0f,  // ki
    0.0f,  // kd
    25.0f, // setpoint
    0,     // sensor_index
    1,     // sensor_mask
    false, // from_file
};

SharedState state{};
LoggingConfig log_config{
    false,  // active
    false,  // use_motor
    1.0f,   // duration_s
    false,  // homed_once
    "",     // postfix
    0,      // file_start_us
};

SemaphoreHandle_t state_mutex = nullptr;
SemaphoreHandle_t sd_mutex = nullptr;

httpd_handle_t http_server = nullptr;
UsbMode usb_mode = UsbMode::kCdc;
TaskHandle_t calibration_task = nullptr;
TaskHandle_t find_zero_task = nullptr;
TaskHandle_t log_task = nullptr;
TaskHandle_t upload_task = nullptr;
TaskHandle_t mqtt_state_task = nullptr;

sdmmc_card_t* sd_card = nullptr;
sdmmc_card_t* log_sd_card = nullptr;
FILE* log_file = nullptr;
bool log_sd_mounted = false;
std::string current_log_path;
static uint32_t boot_id = 0;

SdLockGuard::SdLockGuard(TickType_t timeout_ticks) {
  locked_ = (sd_mutex && xSemaphoreTake(sd_mutex, timeout_ticks) == pdTRUE);
}

SdLockGuard::~SdLockGuard() {
  if (locked_ && sd_mutex) {
    xSemaphoreGive(sd_mutex);
  }
}

SharedState CopyState() {
  SharedState snapshot;
  if (state_mutex) {
    xSemaphoreTake(state_mutex, portMAX_DELAY);
    snapshot = state;
    xSemaphoreGive(state_mutex);
  } else {
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

uint32_t LoadAndIncrementBootId() {
  nvs_handle_t handle;
  uint32_t value = 0;
  if (nvs_open("boot", NVS_READWRITE, &handle) == ESP_OK) {
    esp_err_t err = nvs_get_u32(handle, "id", &value);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
      value = 0;
    }
    if (value == UINT32_MAX) {
      value = 0;
    }
    value += 1;
    nvs_set_u32(handle, "id", value);
    nvs_commit(handle);
    nvs_close(handle);
  }
  boot_id = value;
  return boot_id;
}

uint32_t GetBootId() {
  return boot_id;
}
