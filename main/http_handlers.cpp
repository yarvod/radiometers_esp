#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <string>
#include <vector>
#include <dirent.h>
#include <sys/stat.h>

#include "cJSON.h"
#include "app_state.h"
#include "app_services.h"
#include "control_actions.h"
#include "web_ui.h"
#include "hw_pins.h"
#include "esp_err.h"
#include "driver/gpio.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdmmc_cmd.h"
#include "tusb_msc_storage.h"
#include "sdkconfig.h"

static bool UrlDecode(const char* src, std::string* out) {
  if (!src || !out) return false;
  out->clear();
  for (size_t i = 0; src[i] != '\0'; ++i) {
    char c = src[i];
    if (c == '%') {
      if (!std::isxdigit(static_cast<unsigned char>(src[i + 1])) || !std::isxdigit(static_cast<unsigned char>(src[i + 2]))) {
        return false;
      }
      auto hex = [](char ch) -> int {
        if (ch >= '0' && ch <= '9') return ch - '0';
        if (ch >= 'a' && ch <= 'f') return 10 + (ch - 'a');
        if (ch >= 'A' && ch <= 'F') return 10 + (ch - 'A');
        return 0;
      };
      int v = (hex(src[i + 1]) << 4) | hex(src[i + 2]);
      out->push_back(static_cast<char>(v));
      i += 2;
    } else if (c == '+') {
      out->push_back(' ');
    } else {
      out->push_back(c);
    }
    if (out->size() > 512) return false;
  }
  return true;
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
  cJSON_AddNumberToObject(root, "wifiRssi", snapshot.wifi_rssi_dbm);
  cJSON_AddNumberToObject(root, "wifiQuality", snapshot.wifi_quality);
  cJSON_AddStringToObject(root, "wifiIp", snapshot.wifi_ip.c_str());
  cJSON_AddStringToObject(root, "wifiStaIp", snapshot.wifi_ip_sta.c_str());
  cJSON_AddStringToObject(root, "wifiApIp", snapshot.wifi_ip_ap.c_str());
  cJSON_AddNumberToObject(root, "heaterPower", snapshot.heater_power);
  cJSON_AddNumberToObject(root, "fanPower", snapshot.fan_power);
  cJSON_AddNumberToObject(root, "fan1Rpm", snapshot.fan1_rpm);
  cJSON_AddNumberToObject(root, "fan2Rpm", snapshot.fan2_rpm);
  cJSON_AddNumberToObject(root, "tempSensorCount", snapshot.temp_sensor_count);
  cJSON* temp_obj = cJSON_CreateObject();
  for (int i = 0; i < snapshot.temp_sensor_count && i < MAX_TEMP_SENSORS; ++i) {
    std::string label = snapshot.temp_labels[i];
    if (label.empty()) {
      label = "t" + std::to_string(i + 1);
    }
    const std::string& addr = snapshot.temp_addresses[i];
    cJSON* entry = cJSON_CreateObject();
    cJSON_AddNumberToObject(entry, "value", snapshot.temps_c[i]);
    cJSON_AddStringToObject(entry, "address", addr.c_str());
    cJSON_AddItemToObject(temp_obj, label.c_str(), entry);
  }
  cJSON_AddItemToObject(root, "tempSensors", temp_obj);
  cJSON_AddBoolToObject(root, "logging", snapshot.logging);
  cJSON_AddStringToObject(root, "logFilename", snapshot.log_filename.c_str());
  cJSON_AddBoolToObject(root, "logUseMotor", snapshot.log_use_motor);
  cJSON_AddNumberToObject(root, "logDuration", snapshot.log_duration_s);
  cJSON_AddBoolToObject(root, "wifiApMode", app_config.wifi_ap_mode);
  cJSON_AddStringToObject(root, "wifiSsid", app_config.wifi_ssid.c_str());
  cJSON_AddStringToObject(root, "wifiPassword", app_config.wifi_password.c_str());
  cJSON_AddStringToObject(root, "deviceId", app_config.device_id.c_str());
  cJSON_AddStringToObject(root, "minioEndpoint", app_config.minio_endpoint.c_str());
  cJSON_AddStringToObject(root, "minioAccessKey", app_config.minio_access_key.c_str());
  cJSON_AddStringToObject(root, "minioSecretKey", app_config.minio_secret_key.c_str());
  cJSON_AddStringToObject(root, "minioBucket", app_config.minio_bucket.c_str());
  cJSON_AddBoolToObject(root, "minioEnabled", app_config.minio_enabled);
  cJSON_AddStringToObject(root, "mqttUri", app_config.mqtt_uri.c_str());
  cJSON_AddStringToObject(root, "mqttUser", app_config.mqtt_user.c_str());
  cJSON_AddStringToObject(root, "mqttPassword", app_config.mqtt_password.c_str());
  cJSON_AddBoolToObject(root, "mqttEnabled", app_config.mqtt_enabled);
  cJSON_AddBoolToObject(root, "pidEnabled", snapshot.pid_enabled);
  cJSON_AddNumberToObject(root, "pidSetpoint", snapshot.pid_setpoint);
  cJSON_AddNumberToObject(root, "pidSensorIndex", snapshot.pid_sensor_index);
  cJSON_AddNumberToObject(root, "pidKp", snapshot.pid_kp);
  cJSON_AddNumberToObject(root, "pidKi", snapshot.pid_ki);
  cJSON_AddNumberToObject(root, "pidKd", snapshot.pid_kd);
  cJSON_AddNumberToObject(root, "pidOutput", snapshot.pid_output);
  cJSON_AddStringToObject(root, "wifiMode", app_config.wifi_ap_mode ? "ap" : "sta");
  cJSON_AddNumberToObject(root, "timestamp", snapshot.last_update_ms);
  const std::string iso = IsoUtcNow();
  cJSON_AddStringToObject(root, "timestampIso", iso.c_str());
  cJSON_AddBoolToObject(root, "stepperEnabled", snapshot.stepper_enabled);
  cJSON_AddBoolToObject(root, "stepperHoming", snapshot.homing);
  cJSON_AddBoolToObject(root, "stepperDirForward", snapshot.stepper_direction_forward);
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
  ActionResult res = ActionStepperEnable();
  httpd_resp_set_type(req, "application/json");
  if (!res.ok) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, res.message.c_str());
    return ESP_FAIL;
  }
  return httpd_resp_sendstr(req, "{\"status\":\"stepper_enabled\"}");
}

esp_err_t StepperDisableHandler(httpd_req_t* req) {
  ActionResult res = ActionStepperDisable();
  httpd_resp_set_type(req, "application/json");
  if (!res.ok) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, res.message.c_str());
    return ESP_FAIL;
  }
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
  bool forward = true;
  cJSON* dir_item = cJSON_GetObjectItem(root, "direction");
  if (dir_item) {
    if (cJSON_IsString(dir_item) && dir_item->valuestring) {
      std::string dir = dir_item->valuestring;
      std::transform(dir.begin(), dir.end(), dir.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
      if (dir.find("back") == 0) {
        forward = false;
      } else if (dir.find("rev") == 0) {
        forward = false;
      } else if (dir.find("fwd") == 0 || dir.find("forw") == 0) {
        forward = true;
      }
    } else if (cJSON_IsNumber(dir_item)) {
      forward = dir_item->valueint != 0;
    } else if (cJSON_IsBool(dir_item)) {
      forward = cJSON_IsTrue(dir_item);
    }
  }
  int speed = cJSON_GetObjectItem(root, "speed") ? cJSON_GetObjectItem(root, "speed")->valueint : app_config.stepper_speed_us;
  cJSON_Delete(root);

  StepperMoveRequest move_req;
  move_req.steps = steps;
  move_req.forward = forward;
  move_req.speed_us = speed;
  ActionResult res = ActionStepperMove(move_req);
  if (!res.ok) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, res.message.c_str());
    return ESP_FAIL;
  }

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

bool SaveConfigToSdCard(const AppConfig& cfg, const PidConfig& pid, UsbMode current_usb_mode) {
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
  if (!cfg.device_id.empty()) {
    fprintf(f, "device_id = %s\n", cfg.device_id.c_str());
  }
  if (!cfg.minio_endpoint.empty()) {
    fprintf(f, "minio_endpoint = %s\n", cfg.minio_endpoint.c_str());
  }
  if (!cfg.minio_access_key.empty()) {
    fprintf(f, "minio_access_key = %s\n", cfg.minio_access_key.c_str());
  }
  if (!cfg.minio_secret_key.empty()) {
    fprintf(f, "minio_secret_key = %s\n", cfg.minio_secret_key.c_str());
  }
  if (!cfg.minio_bucket.empty()) {
    fprintf(f, "minio_bucket = %s\n", cfg.minio_bucket.c_str());
  }
  fprintf(f, "minio_enabled = %s\n", (cfg.minio_enabled ? "true" : "false"));
  if (!cfg.mqtt_uri.empty()) {
    fprintf(f, "mqtt_uri = %s\n", cfg.mqtt_uri.c_str());
  }
  if (!cfg.mqtt_user.empty()) {
    fprintf(f, "mqtt_user = %s\n", cfg.mqtt_user.c_str());
  }
  if (!cfg.mqtt_password.empty()) {
    fprintf(f, "mqtt_password = %s\n", cfg.mqtt_password.c_str());
  }
  fprintf(f, "mqtt_enabled = %s\n", (cfg.mqtt_enabled ? "true" : "false"));
  fclose(f);
  // Keep mounted if logging is active to avoid invalidating open log file.
  if (!already_mounted && !log_file) {
    UnmountLogSd();
  }
  ESP_LOGI(TAG, "Config saved to %s", CONFIG_FILE_PATH);
  return true;
}

void StopLogging() {
  // First try to flush/close and queue the current file for upload.
  if (!QueueCurrentLogForUpload()) {
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
  }
  UnmountLogSd();
  UpdateState([](SharedState& s) {
    s.logging = false;
    s.log_filename.clear();
    s.stepper_abort = true;
    s.stepper_moving = false;
  });
  log_config.active = false;
  log_config.homed_once = false;
  log_config.postfix.clear();
  log_config.file_start_us = 0;
  if (log_task) {
    vTaskDelete(log_task);
    log_task = nullptr;
  }
  DisableStepper();
}

void LoggingTask(void*) {
  const int steps_180 = 200;  // adjust to your mechanics (microsteps for 180 degrees)
  const TickType_t settle_delay = pdMS_TO_TICKS(1000);  // pause after motor moves

  auto home_blocking = [&]() {
    UpdateState([](SharedState& s) { s.stepper_abort = false; });
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
    vTaskDelay(settle_delay);
    UpdateState([](SharedState& s) {
      s.stepper_position = 0;
      s.stepper_target = 0;
      s.stepper_moving = false;
      s.homing = false;
    });
  };

  auto move_blocking = [&](int steps, bool forward) {
    UpdateState([](SharedState& s) { s.homing = true; s.stepper_abort = false; });
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
    vTaskDelay(settle_delay);
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

  SharedState pending_base{};
  bool has_pending_base = false;
  bool at_zero = true;

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
      (void)QueueCurrentLogForUpload();
      if (!OpenLogFileWithPostfix(log_config.postfix)) {
        StopLogging();
        vTaskDelete(nullptr);
      }
      continue;
    }

    if (log_config.use_motor) {
      if (!log_config.homed_once) {
        EnableStepper();
        home_blocking();
        DisableStepper();
        log_config.homed_once = true;
        if (CopyState().stepper_abort) {
          ESP_LOGW(TAG, "Logging aborted during homing");
          StopLogging();
          vTaskDelete(nullptr);
        }
      }
      // Между движениями держим мотор отключенным, включая только в move_blocking
      DisableStepper();
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

    if (log_config.use_motor) {
      SharedState avg{};
      if (!collect_avg(log_config.duration_s, current.temp_sensor_count, &avg)) {
        ESP_LOGW(TAG, "Logging: no samples collected, retrying");
        vTaskDelay(pdMS_TO_TICKS(500));
        continue;
      }

      if (at_zero) {
        // Сохраняем базу, уходим назад на 200
        pending_base = avg;
        has_pending_base = true;
        move_blocking(steps_180, false);
        if (CopyState().stepper_abort) {
          ESP_LOGW(TAG, "Logging aborted during stepper move");
          StopLogging();
          vTaskDelete(nullptr);
        }
        at_zero = false;
        continue;
      }

      // Мы в -200 (или смещённом), это калибровочный замер
      if (!has_pending_base) {
        ESP_LOGW(TAG, "Logging: got cal without base, skipping");
      } else {
        SdLockGuard guard(pdMS_TO_TICKS(2000));
        if (!guard.locked()) {
          ESP_LOGW(TAG, "SD mutex unavailable, retrying logging write");
          vTaskDelay(pdMS_TO_TICKS(200));
          continue;
        }
        uint64_t ts_ms = esp_timer_get_time() / 1000ULL;
        const std::string iso = IsoUtcNow();
        fprintf(log_file, "%s,%llu,%.6f,%.6f,%.6f", iso.c_str(), (unsigned long long)ts_ms,
                pending_base.voltage1, pending_base.voltage2, pending_base.voltage3);
        for (int i = 0; i < pending_base.temp_sensor_count && i < MAX_TEMP_SENSORS; ++i) {
          fprintf(log_file, ",%.2f", pending_base.temps_c[i]);
        }
        fprintf(log_file, ",%.3f,%.3f,%.3f", pending_base.ina_bus_voltage, pending_base.ina_current, pending_base.ina_power);
        fprintf(log_file, ",%.6f,%.6f,%.6f", avg.voltage1, avg.voltage2, avg.voltage3);
        fprintf(log_file, "\n");
        FlushLogFile();
        ESP_LOGI(TAG, "Logging: wrote row ts=%llu iso=%s", (unsigned long long)ts_ms, iso.c_str());
        UpdateState([&](SharedState& s) {
          s.voltage1_cal = avg.voltage1;
          s.voltage2_cal = avg.voltage2;
          s.voltage3_cal = avg.voltage3;
        });
      }

      // Вернуться вперёд на 200 и продолжить цикл
      move_blocking(steps_180, true);
      if (CopyState().stepper_abort) {
        ESP_LOGW(TAG, "Logging aborted during stepper move");
        StopLogging();
        vTaskDelete(nullptr);
      }
      at_zero = true;
      has_pending_base = false;
      continue;
    }

    // Без мотора — обычный замер
    SharedState avg1{};
    if (!collect_avg(log_config.duration_s, current.temp_sensor_count, &avg1)) {
      ESP_LOGW(TAG, "Logging: no samples collected, retrying");
      vTaskDelay(pdMS_TO_TICKS(500));
      continue;
    }
    bool have_cal = false;
    SharedState avg2{};

    SdLockGuard guard(pdMS_TO_TICKS(2000));
    if (!guard.locked()) {
      ESP_LOGW(TAG, "SD mutex unavailable, retrying logging write");
      vTaskDelay(pdMS_TO_TICKS(200));
      continue;
    }
    uint64_t ts_ms = esp_timer_get_time() / 1000ULL;
    const std::string iso = IsoUtcNow();
    fprintf(log_file, "%s,%llu,%.6f,%.6f,%.6f", iso.c_str(), (unsigned long long)ts_ms,
            avg1.voltage1, avg1.voltage2, avg1.voltage3);
    for (int i = 0; i < avg1.temp_sensor_count && i < MAX_TEMP_SENSORS; ++i) {
      fprintf(log_file, ",%.2f", avg1.temps_c[i]);
    }
    fprintf(log_file, ",%.3f,%.3f,%.3f", avg1.ina_bus_voltage, avg1.ina_current, avg1.ina_power);
    if (log_config.use_motor && have_cal) {
      fprintf(log_file, ",%.6f,%.6f,%.6f", avg2.voltage1, avg2.voltage2, avg2.voltage3);
    }
    fprintf(log_file, "\n");
    FlushLogFile();
    ESP_LOGI(TAG, "Logging: wrote row ts=%llu iso=%s", (unsigned long long)ts_ms, iso.c_str());
    UpdateState([&](SharedState& s) {
      s.voltage1_cal = avg1.voltage1;
      s.voltage2_cal = avg1.voltage2;
      s.voltage3_cal = avg1.voltage3;
    });
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
    xTaskCreatePinnedToCore(&LoggingTask, "log_task", 6144, nullptr, 2, &log_task, 0);
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

  ActionResult res = ActionHeaterSet(power);

  httpd_resp_set_type(req, "application/json");
  if (!res.ok) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, res.message.c_str());
    return ESP_FAIL;
  }
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

  ActionResult res = ActionFanSet(p);

  httpd_resp_set_type(req, "application/json");
  if (!res.ok) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, res.message.c_str());
    return ESP_FAIL;
  }
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

  LogRequest log_req{filename, use_motor, duration_s};
  ActionResult res = ActionStartLog(log_req);
  if (!res.ok) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, res.message.c_str());
    return ESP_FAIL;
  }

  httpd_resp_set_type(req, "application/json");
  return httpd_resp_sendstr(req, "{\"status\":\"logging_started\"}");
}

esp_err_t LogStopHandler(httpd_req_t* req) {
  ActionResult res = ActionStopLog();
  if (!res.ok) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, res.message.c_str());
    return ESP_FAIL;
  }
  httpd_resp_set_type(req, "application/json");
  return httpd_resp_sendstr(req, "{\"status\":\"logging_stopped\"}");
}

esp_err_t FsListHandler(httpd_req_t* req) {
  SharedState snapshot = CopyState();
  if (snapshot.usb_msc_mode) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "MSC mode active");
    return ESP_FAIL;
  }

  std::string rel_path;
  int page = 0;
  int page_size = 10;
  int qs_len = httpd_req_get_url_query_len(req) + 1;
  if (qs_len > 1) {
    std::string qs(qs_len, '\0');
    if (httpd_req_get_url_query_str(req, qs.data(), qs_len) == ESP_OK) {
      char buf[256] = {};
      if (httpd_query_key_value(qs.c_str(), "path", buf, sizeof(buf)) == ESP_OK) {
        std::string decoded;
        if (!UrlDecode(buf, &decoded)) {
          httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad path");
          return ESP_FAIL;
        }
        rel_path = decoded;
      }
      if (httpd_query_key_value(qs.c_str(), "page", buf, sizeof(buf)) == ESP_OK) {
        page = std::max(0, atoi(buf));
      }
      if (httpd_query_key_value(qs.c_str(), "pageSize", buf, sizeof(buf)) == ESP_OK) {
        int p = atoi(buf);
        if (p > 0 && p <= 100) {
          page_size = p;
        }
      }
    }
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

  std::string full_dir = CONFIG_MOUNT_POINT;
  if (!rel_path.empty()) {
    if (!SanitizePath(rel_path, &full_dir)) {
      httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid path");
      return ESP_FAIL;
    }
  }
  DIR* dir = opendir(full_dir.c_str());
  if (!dir) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Open dir failed");
    return ESP_FAIL;
  }

  struct Entry {
    std::string name;
    uint64_t size;
    bool is_dir;
  };
  std::vector<Entry> entries;
  struct dirent* ent = nullptr;
  while ((ent = readdir(dir)) != nullptr) {
    if (ent->d_name[0] == '.') continue;
    const bool maybe_dir = ent->d_type == DT_DIR;
    if (ent->d_type != DT_REG && ent->d_type != DT_DIR && ent->d_type != DT_UNKNOWN) continue;
    std::string full_path = full_dir + "/" + ent->d_name;
    struct stat st {};
    uint64_t size = 0;
    if (stat(full_path.c_str(), &st) == 0) {
      size = static_cast<uint64_t>(st.st_size);
    }
    bool is_dir = maybe_dir || S_ISDIR(st.st_mode);
    entries.push_back({ent->d_name, size, is_dir});
  }
  closedir(dir);

  std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) { return a.name < b.name; });
  int total = static_cast<int>(entries.size());
  int total_pages = (total + page_size - 1) / page_size;
  int start = page * page_size;
  if (start >= total) {
    page = 0;
    start = 0;
  }

  cJSON* root = cJSON_CreateObject();
  cJSON_AddStringToObject(root, "path", rel_path.c_str());
  cJSON_AddNumberToObject(root, "page", page);
  cJSON_AddNumberToObject(root, "pageSize", page_size);
  cJSON_AddNumberToObject(root, "total", total);
  cJSON_AddNumberToObject(root, "totalPages", total_pages);
  cJSON* arr = cJSON_CreateArray();
  for (int i = start; i < total && i < start + page_size; ++i) {
    const Entry& e = entries[i];
    cJSON* obj = cJSON_CreateObject();
    cJSON_AddStringToObject(obj, "name", e.name.c_str());
    cJSON_AddStringToObject(obj, "type", e.is_dir ? "dir" : "file");
    cJSON_AddNumberToObject(obj, "size", static_cast<double>(e.size));
    std::string child_path = rel_path.empty() ? e.name : (rel_path + "/" + e.name);
    cJSON_AddStringToObject(obj, "path", child_path.c_str());
    cJSON_AddItemToArray(arr, obj);
  }
  cJSON_AddItemToObject(root, "entries", arr);

  const char* json = cJSON_PrintUnformatted(root);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_sendstr(req, json);
  cJSON_free((void*)json);
  cJSON_Delete(root);
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
  char file_param[256] = {};
  bool has_path = httpd_query_key_value(qs.c_str(), "path", file_param, sizeof(file_param)) == ESP_OK;
  if (!has_path) {
    if (httpd_query_key_value(qs.c_str(), "file", file_param, sizeof(file_param)) != ESP_OK) {
      httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing file param");
      return ESP_FAIL;
    }
  }
  std::string req_path = file_param;
  std::string decoded_path;
  if (!UrlDecode(req_path.c_str(), &decoded_path)) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid path");
    return ESP_FAIL;
  }
  std::string full_path;
  if (has_path) {
    if (!SanitizePath(decoded_path, &full_path)) {
      httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid path");
      return ESP_FAIL;
    }
  } else if (!SanitizeFilename(decoded_path, &full_path)) {
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
  std::string download_name = decoded_path;
  size_t slash = download_name.find_last_of('/');
  if (slash != std::string::npos) {
    download_name = download_name.substr(slash + 1);
  }
  std::string disp = "attachment; filename=\"";
  disp.append(download_name);
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
    char file_param[256] = {};
    if (httpd_query_key_value(qs.c_str(), "path", file_param, sizeof(file_param)) != ESP_OK &&
        httpd_query_key_value(qs.c_str(), "file", file_param, sizeof(file_param)) != ESP_OK) {
      httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing file param");
      return ESP_FAIL;
    }
    std::string decoded;
    if (!UrlDecode(file_param, &decoded)) {
      httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid path");
      return ESP_FAIL;
    }
    requested_files.emplace_back(decoded);
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

  auto basename = [](const std::string& path) {
    size_t pos = path.find_last_of('/');
    return (pos == std::string::npos) ? path : path.substr(pos + 1);
  };

  for (const auto& raw_name : requested_files) {
    if (!seen.insert(raw_name).second) continue;
    const std::string name_only = basename(raw_name);
    if (is_protected_config(name_only)) {
      skipped.push_back(raw_name + " (protected)");
      continue;
    }

    std::string full_path;
    if (!SanitizePath(raw_name, &full_path) && !SanitizeFilename(raw_name, &full_path)) {
      failed.push_back(raw_name + " (invalid)");
      continue;
    }

    if (snapshot.logging && snapshot.log_filename == name_only) {
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

esp_err_t CloudApplyHandler(httpd_req_t* req) {
  const size_t buf_len = std::min<size_t>(req->content_len, 512);
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
  auto get_str = [&](const char* key) -> std::string {
    cJSON* item = cJSON_GetObjectItem(root, key);
    return (item && cJSON_IsString(item) && item->valuestring) ? std::string(item->valuestring) : std::string();
  };
  auto get_bool = [&](const char* key, bool def) -> bool {
    cJSON* item = cJSON_GetObjectItem(root, key);
    if (item && cJSON_IsBool(item)) return cJSON_IsTrue(item);
    if (item && cJSON_IsNumber(item)) return item->valueint != 0;
    return def;
  };
  app_config.device_id = get_str("deviceId");
  app_config.minio_endpoint = get_str("minioEndpoint");
  app_config.minio_access_key = get_str("minioAccessKey");
  app_config.minio_secret_key = get_str("minioSecretKey");
  app_config.minio_bucket = get_str("minioBucket");
  app_config.minio_enabled = get_bool("minioEnabled", app_config.minio_enabled);
  app_config.mqtt_uri = get_str("mqttUri");
  app_config.mqtt_user = get_str("mqttUser");
  app_config.mqtt_password = get_str("mqttPassword");
  app_config.mqtt_enabled = get_bool("mqttEnabled", app_config.mqtt_enabled);
  cJSON_Delete(root);

  SaveConfigToSdCard(app_config, pid_config, usb_mode);

  httpd_resp_set_type(req, "application/json");
  return httpd_resp_sendstr(req, "{\"status\":\"cloud_applied\"}");
}

httpd_handle_t StartHttpServer() {
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = 80;
  config.lru_purge_enable = true;
  config.max_uri_handlers = 28;
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
  httpd_uri_t cloud_apply_uri = {.uri = "/cloud/apply", .method = HTTP_POST, .handler = CloudApplyHandler, .user_ctx = nullptr};
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
  httpd_register_uri_handler(http_server, &cloud_apply_uri);
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
