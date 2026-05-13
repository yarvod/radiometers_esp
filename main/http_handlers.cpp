#include <algorithm>
#include <cerrno>
#include <cstdarg>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <string>
#include <vector>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include "cJSON.h"
#include "app_state.h"
#include "app_services.h"
#include "control_actions.h"
#include "app_utils.h"
#include "web_ui.h"
#include "mqtt_bridge.h"
#include "error_manager.h"
#include "sd_maintenance.h"
#include "hw_pins.h"
#include "esp_err.h"
#include "driver/gpio.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "esp_flash.h"
#include "esp_partition.h"
#include "esp_rom_sys.h"
#include "wear_levelling.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdmmc_cmd.h"
#include "tusb_msc_storage.h"
#include "sdkconfig.h"

static bool internal_flash_fs_mounted = false;
static wl_handle_t internal_flash_wl_handle = WL_INVALID_HANDLE;

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

static std::vector<uint16_t> ParseRtcmTypesText(const std::string& text) {
  std::vector<uint16_t> out;
  size_t start = 0;
  while (start < text.size()) {
    while (start < text.size() && (text[start] == ',' || text[start] == ';' || std::isspace(static_cast<unsigned char>(text[start])))) {
      start++;
    }
    if (start >= text.size()) break;
    size_t end = start;
    while (end < text.size() && text[end] != ',' && text[end] != ';' && !std::isspace(static_cast<unsigned char>(text[end]))) {
      end++;
    }
    const int type = std::atoi(text.substr(start, end - start).c_str());
    if (type > 0 && type <= 4095 &&
        std::find(out.begin(), out.end(), static_cast<uint16_t>(type)) == out.end()) {
      out.push_back(static_cast<uint16_t>(type));
    }
    start = end;
  }
  return out;
}

static void AppendGpsCsvFields(FILE* file, const GpsPositionSnapshot& gps) {
  if (!file) return;
  if (gps.valid) {
    fprintf(file, ",%.8f,%.8f,%.3f,%d,%d,%lld",
            gps.latitude_deg, gps.longitude_deg, gps.altitude_m,
            gps.fix_quality, gps.satellites, static_cast<long long>(gps.age_ms));
  } else {
    fprintf(file, ",,,,,,");
  }
}

static void AddGpsJsonFields(cJSON* root, const GpsPositionSnapshot& gps) {
  if (!root) return;
  cJSON_AddBoolToObject(root, "gpsPositionValid", gps.valid);
  if (!gps.valid) return;
  cJSON_AddNumberToObject(root, "gpsLat", gps.latitude_deg);
  cJSON_AddNumberToObject(root, "gpsLon", gps.longitude_deg);
  cJSON_AddNumberToObject(root, "gpsAlt", gps.altitude_m);
  cJSON_AddNumberToObject(root, "gpsFixQuality", gps.fix_quality);
  cJSON_AddNumberToObject(root, "gpsSatellites", gps.satellites);
  cJSON_AddNumberToObject(root, "gpsFixAgeMs", static_cast<double>(gps.age_ms));
}

static void PublishLogMeasurement(const std::string& iso, uint64_t ts_ms, const SharedState& base,
                                  const SharedState* cal, UtcTimeSource time_source,
                                  const GpsPositionSnapshot& gps) {
  cJSON* root = cJSON_CreateObject();
  cJSON_AddStringToObject(root, "timestampIso", iso.c_str());
  cJSON_AddNumberToObject(root, "timestampMs", static_cast<double>(ts_ms));
  cJSON_AddStringToObject(root, "timeSource", UtcTimeSourceName(time_source));
  cJSON_AddNumberToObject(root, "adc1", base.voltage1);
  cJSON_AddNumberToObject(root, "adc2", base.voltage2);
  cJSON_AddNumberToObject(root, "adc3", base.voltage3);
  cJSON* temps = cJSON_CreateArray();
  cJSON* temp_obj = cJSON_CreateObject();
  for (int i = 0; i < base.temp_sensor_count && i < MAX_TEMP_SENSORS; ++i) {
    cJSON_AddItemToArray(temps, cJSON_CreateNumber(base.temps_c[i]));
    const std::string key = "t" + std::to_string(i + 1);
    const std::string& addr = base.temp_addresses[i];
    cJSON* entry = cJSON_CreateObject();
    cJSON_AddNumberToObject(entry, "value", base.temps_c[i]);
    cJSON_AddStringToObject(entry, "address", addr.c_str());
    cJSON_AddStringToObject(entry, "label", key.c_str());
    cJSON_AddItemToObject(temp_obj, key.c_str(), entry);
  }
  cJSON_AddItemToObject(root, "temps", temps);
  cJSON_AddItemToObject(root, "tempSensors", temp_obj);
  cJSON_AddNumberToObject(root, "busV", base.ina_bus_voltage);
  cJSON_AddNumberToObject(root, "busI", base.ina_current);
  cJSON_AddNumberToObject(root, "busP", base.ina_power);
  cJSON_AddBoolToObject(root, "logUseMotor", log_config.use_motor);
  cJSON_AddNumberToObject(root, "logDuration", log_config.duration_s);
  SharedState current = CopyState();
  if (!current.log_filename.empty()) {
    cJSON_AddStringToObject(root, "logFilename", current.log_filename.c_str());
  }
  if (cal) {
    cJSON_AddNumberToObject(root, "adc1Cal", cal->voltage1);
    cJSON_AddNumberToObject(root, "adc2Cal", cal->voltage2);
    cJSON_AddNumberToObject(root, "adc3Cal", cal->voltage3);
  }
  AddGpsJsonFields(root, gps);
  const char* json = cJSON_PrintUnformatted(root);
  if (json) {
    PublishMeasurementPayload(json);
  }
  cJSON_free((void*)json);
  cJSON_Delete(root);
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
  cJSON_AddStringToObject(root, "ethIp", snapshot.eth_ip.c_str());
  cJSON_AddBoolToObject(root, "ethLink", snapshot.eth_link_up);
  cJSON_AddBoolToObject(root, "ethIpUp", snapshot.eth_ip_up);
  cJSON_AddNumberToObject(root, "sdTotalBytes", static_cast<double>(snapshot.sd_total_bytes));
  cJSON_AddNumberToObject(root, "sdUsedBytes", static_cast<double>(snapshot.sd_used_bytes));
  cJSON_AddNumberToObject(root, "sdRootDataFiles", snapshot.sd_data_root_files);
  cJSON_AddNumberToObject(root, "sdToUploadFiles", snapshot.sd_to_upload_files);
  cJSON_AddNumberToObject(root, "sdUploadedFiles", snapshot.sd_uploaded_files);
  cJSON_AddStringToObject(root, "storageBackend", StorageBackendToString(app_config.storage_backend).c_str());
  cJSON_AddBoolToObject(root, "sdMounted", log_sd_mounted);
  cJSON_AddBoolToObject(root, "internalFlashMounted", internal_flash_fs_mounted);
  cJSON_AddBoolToObject(root, "activeStorageMounted",
                        app_config.storage_backend == StorageBackend::kInternalFlash ? internal_flash_fs_mounted : log_sd_mounted);
  cJSON_AddNumberToObject(root, "heapFreeBytes", static_cast<double>(snapshot.heap_free_bytes));
  cJSON_AddNumberToObject(root, "heapLargestFreeBlockBytes", static_cast<double>(snapshot.heap_largest_free_block_bytes));
  cJSON_AddNumberToObject(root, "minioUploadAttempts", snapshot.minio_upload_attempts);
  cJSON_AddNumberToObject(root, "minioLastAttemptMs", static_cast<double>(snapshot.minio_last_attempt_ms));
  cJSON_AddNumberToObject(root, "heaterPower", snapshot.heater_power);
  cJSON_AddNumberToObject(root, "fanPower", snapshot.fan_power);
  cJSON_AddBoolToObject(root, "externalPowerOn", snapshot.external_power_on);
  cJSON_AddNumberToObject(root, "fan1Rpm", snapshot.fan1_rpm);
  cJSON_AddNumberToObject(root, "fan2Rpm", snapshot.fan2_rpm);
  cJSON_AddNumberToObject(root, "tempSensorCount", snapshot.temp_sensor_count);
  cJSON* temp_obj = cJSON_CreateObject();
  for (int i = 0; i < snapshot.temp_sensor_count && i < MAX_TEMP_SENSORS; ++i) {
    const std::string key = "t" + std::to_string(i + 1);
    const std::string& addr = snapshot.temp_addresses[i];
    cJSON* entry = cJSON_CreateObject();
    cJSON_AddNumberToObject(entry, "value", snapshot.temps_c[i]);
    cJSON_AddStringToObject(entry, "address", addr.c_str());
    cJSON_AddStringToObject(entry, "label", key.c_str());
    cJSON_AddItemToObject(temp_obj, key.c_str(), entry);
  }
  cJSON_AddItemToObject(root, "tempSensors", temp_obj);
  cJSON_AddBoolToObject(root, "logging", snapshot.logging);
  cJSON_AddStringToObject(root, "logFilename", snapshot.log_filename.c_str());
  cJSON_AddBoolToObject(root, "logUseMotor", snapshot.log_use_motor);
  cJSON_AddNumberToObject(root, "logDuration", snapshot.log_duration_s);
  cJSON_AddBoolToObject(root, "wifiApMode", app_config.wifi_ap_mode);
  cJSON_AddStringToObject(root, "wifiSsid", app_config.wifi_ssid.c_str());
  cJSON_AddStringToObject(root, "wifiPassword", app_config.wifi_password.c_str());
  cJSON_AddStringToObject(root, "netMode", NetModeToString(app_config.net_mode).c_str());
  cJSON_AddStringToObject(root, "netPriority", NetPriorityToString(app_config.net_priority).c_str());
  cJSON* gps_types = cJSON_CreateArray();
  for (uint16_t type : app_config.gps_rtcm_types) {
    cJSON_AddItemToArray(gps_types, cJSON_CreateNumber(type));
  }
  cJSON_AddItemToObject(root, "gpsRtcmTypes", gps_types);
  cJSON_AddStringToObject(root, "gpsMode", app_config.gps_mode.c_str());
  char gps_actual_mode[256] = {};
  GetGpsCurrentModeText(gps_actual_mode, sizeof(gps_actual_mode));
  cJSON_AddStringToObject(root, "gpsActualMode", gps_actual_mode);
  cJSON_AddNumberToObject(root, "gpsAntennaShortRaw", GetGpsAntennaShortRaw());
  cJSON_AddBoolToObject(root, "gpsAntennaShort", IsGpsAntennaShort());
  const GpsReceiverStatus gps_status = GetGpsReceiverStatus();
  cJSON_AddBoolToObject(root, "gpsPositionValid", gps_status.position_valid);
  if (gps_status.position_valid) {
    cJSON_AddNumberToObject(root, "gpsLat", gps_status.latitude_deg);
    cJSON_AddNumberToObject(root, "gpsLon", gps_status.longitude_deg);
    cJSON_AddNumberToObject(root, "gpsAlt", gps_status.altitude_m);
    cJSON_AddNumberToObject(root, "gpsFixQuality", gps_status.fix_quality);
    cJSON_AddNumberToObject(root, "gpsSatellites", gps_status.satellites);
    cJSON_AddNumberToObject(root, "gpsPositionAgeMs", static_cast<double>(gps_status.position_age_ms));
  }
  cJSON_AddBoolToObject(root, "gpsTimeValid", gps_status.time_valid);
  if (gps_status.time_valid) {
    cJSON_AddStringToObject(root, "gpsTimeIso", gps_status.time_iso);
    cJSON_AddNumberToObject(root, "gpsTimeAgeMs", static_cast<double>(gps_status.time_age_ms));
  }
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
  cJSON_AddNumberToObject(root, "pidSensorMask", snapshot.pid_sensor_mask);
  cJSON_AddNumberToObject(root, "pidKp", snapshot.pid_kp);
  cJSON_AddNumberToObject(root, "pidKi", snapshot.pid_ki);
  cJSON_AddNumberToObject(root, "pidKd", snapshot.pid_kd);
  cJSON_AddNumberToObject(root, "pidOutput", snapshot.pid_output);
  cJSON_AddStringToObject(root, "wifiMode", app_config.wifi_ap_mode ? "ap" : "sta");
  cJSON_AddNumberToObject(root, "timestamp", snapshot.last_update_ms);
  const UtcTimeSnapshot now = GetBestUtcTimeForData();
  const std::string iso = FormatUtcIso(now);
  cJSON_AddStringToObject(root, "timestampIso", iso.c_str());
  cJSON_AddStringToObject(root, "timeSource", UtcTimeSourceName(now.source));
  cJSON_AddBoolToObject(root, "stepperEnabled", snapshot.stepper_enabled);
  cJSON_AddBoolToObject(root, "stepperHoming", snapshot.homing);
  cJSON_AddBoolToObject(root, "stepperDirForward", snapshot.stepper_direction_forward);
  cJSON_AddNumberToObject(root, "stepperPosition", snapshot.stepper_position);
  cJSON_AddNumberToObject(root, "stepperTarget", snapshot.stepper_target);
  cJSON_AddNumberToObject(root, "stepperSpeedUs", snapshot.stepper_speed_us);
  cJSON_AddNumberToObject(root, "stepperHomeOffsetSteps", snapshot.stepper_home_offset_steps);
  cJSON_AddNumberToObject(root, "motorHallActiveLevel", snapshot.motor_hall_active_level);
  cJSON_AddNumberToObject(root, "motorHallRawLevel", gpio_get_level(MT_HALL_SEN));
  cJSON_AddBoolToObject(root, "motorHallTriggered", IsHallTriggered());
  cJSON_AddBoolToObject(root, "stepperMoving", snapshot.stepper_moving);
  cJSON_AddBoolToObject(root, "stepperHomed", snapshot.stepper_homed);
  cJSON_AddStringToObject(root, "stepperHomeStatus", snapshot.stepper_home_status.c_str());
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
  ActionResult res = ActionCalibrate();
  httpd_resp_set_type(req, "application/json");
  if (!res.ok) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, res.message.c_str());
    return ESP_FAIL;
  }
  return httpd_resp_sendstr(req, "{\"status\":\"calibration_started\"}");
}

esp_err_t RestartHandler(httpd_req_t* req) {
  ActionResult res = ActionRestart();
  httpd_resp_set_type(req, "application/json");
  if (!res.ok) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, res.message.c_str());
    return ESP_FAIL;
  }
  return httpd_resp_sendstr(req, "{\"status\":\"restarting\"}");
}

esp_err_t ExternalPowerSetHandler(httpd_req_t* req) {
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
  cJSON* enabled_item = cJSON_GetObjectItem(root, "enabled");
  if (!enabled_item) {
    enabled_item = cJSON_GetObjectItem(root, "on");
  }
  if (!enabled_item || (!cJSON_IsBool(enabled_item) && !cJSON_IsNumber(enabled_item))) {
    cJSON_Delete(root);
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing enabled");
    return ESP_FAIL;
  }
  const bool enabled = cJSON_IsBool(enabled_item) ? cJSON_IsTrue(enabled_item) : enabled_item->valueint != 0;
  cJSON_Delete(root);

  ActionResult res = ActionExternalPowerSet(enabled);
  httpd_resp_set_type(req, "application/json");
  if (!res.ok) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, res.message.c_str());
    return ESP_FAIL;
  }
  return httpd_resp_sendstr(req, enabled ? "{\"status\":\"external_power_on\"}" : "{\"status\":\"external_power_off\"}");
}

esp_err_t ExternalPowerCycleHandler(httpd_req_t* req) {
  uint32_t off_ms = 1000;
  const size_t buf_len = std::min<size_t>(req->content_len, 64);
  if (buf_len > 0) {
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
    cJSON* off_item = cJSON_GetObjectItem(root, "offMs");
    if (off_item && cJSON_IsNumber(off_item) && off_item->valueint > 0) {
      off_ms = static_cast<uint32_t>(off_item->valueint);
    }
    cJSON_Delete(root);
  }

  ActionResult res = ActionExternalPowerCycle(off_ms);
  httpd_resp_set_type(req, "application/json");
  if (!res.ok) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, res.message.c_str());
    return ESP_FAIL;
  }
  return httpd_resp_sendstr(req, "{\"status\":\"external_power_cycle_started\"}");
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

esp_err_t StepperHomeOffsetHandler(httpd_req_t* req) {
  const size_t buf_len = std::min<size_t>(req->content_len, 128);
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
  cJSON* offset_item = cJSON_GetObjectItem(root, "offsetSteps");
  if (!offset_item) {
    offset_item = cJSON_GetObjectItem(root, "offset");
  }
  const int offset_steps = (offset_item && cJSON_IsNumber(offset_item))
                               ? offset_item->valueint
                               : app_config.stepper_home_offset_steps;
  cJSON* speed_item = cJSON_GetObjectItem(root, "speedUs");
  if (!speed_item) {
    speed_item = cJSON_GetObjectItem(root, "speed");
  }
  const int speed_us = (speed_item && cJSON_IsNumber(speed_item) && speed_item->valueint > 0)
                           ? speed_item->valueint
                           : app_config.stepper_speed_us;
  cJSON* hall_item = cJSON_GetObjectItem(root, "hallActiveLevel");
  if (!hall_item) {
    hall_item = cJSON_GetObjectItem(root, "motorHallActiveLevel");
  }
  bool hall_active_set = false;
  int hall_active_level = app_config.motor_hall_active_level;
  if (hall_item && cJSON_IsNumber(hall_item)) {
    hall_active_level = hall_item->valueint ? 1 : 0;
    hall_active_set = true;
  }
  cJSON_Delete(root);

  StepperHomeOffsetRequest action_req;
  action_req.offset_steps = offset_steps;
  action_req.speed_us = speed_us;
  action_req.hall_active_level = hall_active_level;
  action_req.hall_active_level_set = hall_active_set;
  ActionResult res = ActionStepperHomeOffset(action_req);
  if (!res.ok) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, res.message.c_str());
    return ESP_FAIL;
  }

  httpd_resp_set_type(req, "application/json");
  char resp[160];
  std::snprintf(resp, sizeof(resp),
                "{\"status\":\"stepper_settings_saved\",\"speedUs\":%d,\"offsetSteps\":%d,\"hallActiveLevel\":%d}",
                speed_us, offset_steps, app_config.motor_hall_active_level);
  return httpd_resp_sendstr(req, resp);
}

esp_err_t StepperStopHandler(httpd_req_t* req) {
  ActionResult res = ActionStepperStop();
  httpd_resp_set_type(req, "application/json");
  if (!res.ok) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, res.message.c_str());
    return ESP_FAIL;
  }
  return httpd_resp_sendstr(req, "{\"status\":\"movement_stopped\"}");
}

esp_err_t StepperZeroHandler(httpd_req_t* req) {
  ActionResult res = ActionStepperZero();
  httpd_resp_set_type(req, "application/json");
  if (!res.ok) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, res.message.c_str());
    return ESP_FAIL;
  }
  return httpd_resp_sendstr(req, "{\"status\":\"position_zeroed\"}");
}

namespace {

struct StepperHomeResult {
  bool aborted = false;
  bool hall_found = false;
  bool offset_done = false;
  int hall_steps = 0;
  int offset_steps = 0;
};

constexpr int kStepperHomeRetryAttempts = 3;

static bool StepperHomeSucceeded(const StepperHomeResult& result) {
  return !result.aborted && result.hall_found && result.offset_done;
}

static std::string StepperHomeFailureMessage(const char* phase, const StepperHomeResult& result, int attempts) {
  char buf[192];
  std::snprintf(buf, sizeof(buf),
                "%s failed after %d attempt(s): hall=%s offset=%s aborted=%s hall_steps=%d offset_steps=%d",
                phase ? phase : "Stepper homing",
                attempts,
                result.hall_found ? "yes" : "no",
                result.offset_done ? "yes" : "no",
                result.aborted ? "yes" : "no",
                result.hall_steps,
                result.offset_steps);
  return std::string(buf);
}

static void StepperPulseOnce() {
  gpio_set_level(STEPPER_STEP, 1);
  esp_rom_delay_us(4);
  gpio_set_level(STEPPER_STEP, 0);
}

static bool MoveStepperBlockingSigned(int signed_steps, int step_delay_us, const char* log_context) {
  if (signed_steps == 0) {
    return true;
  }
  const bool forward = signed_steps > 0;
  const int steps = std::abs(signed_steps);
  gpio_set_level(STEPPER_DIR, forward ? 1 : 0);
  UpdateState([&](SharedState& s) {
    s.stepper_direction_forward = forward;
    s.stepper_moving = true;
    s.stepper_target = s.stepper_position + signed_steps;
  });
  for (int i = 0; i < steps; ++i) {
    if (CopyState().stepper_abort) {
      ESP_LOGW(TAG, "%s offset aborted after %d/%d steps", log_context, i, steps);
      return false;
    }
    StepperPulseOnce();
    UpdateState([&](SharedState& s) {
      s.stepper_position += forward ? 1 : -1;
    });
    esp_rom_delay_us(step_delay_us);
  }
  UpdateState([](SharedState& s) { s.stepper_moving = false; });
  return true;
}

static StepperHomeResult HomeStepperToUserZeroBlocking(bool enable_motor, const char* log_context) {
  StepperHomeResult result{};
  if (enable_motor) {
    EnableStepper();
  }

  UpdateState([](SharedState& s) {
    s.stepper_abort = false;
    s.homing = true;
    s.stepper_moving = true;
    s.stepper_homed = false;
    s.stepper_home_status = "seeking_hall";
  });

  const int step_delay_us = std::max(CopyState().stepper_speed_us, 1);
  constexpr int max_steps = 20000;
  gpio_set_level(STEPPER_DIR, 0);
  UpdateState([](SharedState& s) { s.stepper_direction_forward = false; });

  while (!IsHallTriggered() && result.hall_steps < max_steps) {
    if (CopyState().stepper_abort) {
      ESP_LOGW(TAG, "%s aborted before Hall after %d steps", log_context, result.hall_steps);
      result.aborted = true;
      break;
    }
    StepperPulseOnce();
    UpdateState([](SharedState& s) {
      s.stepper_position -= 1;
      s.stepper_target = s.stepper_position;
    });
    esp_rom_delay_us(step_delay_us);
    result.hall_steps++;
  }

  result.hall_found = IsHallTriggered();
  if (result.hall_found && !result.aborted) {
    ESP_LOGI(TAG, "%s Hall detected after %d steps", log_context, result.hall_steps);
    const int offset = app_config.stepper_home_offset_steps;
    UpdateState([](SharedState& s) {
      s.stepper_position = 0;
      s.stepper_target = 0;
      s.stepper_home_status = "applying_offset";
    });
    result.offset_steps = offset;
    result.offset_done = MoveStepperBlockingSigned(offset, step_delay_us, log_context);
  } else if (!result.aborted) {
    ESP_LOGW(TAG, "%s Hall NOT detected, stopped after %d steps", log_context, result.hall_steps);
  }

  UpdateState([&](SharedState& s) {
    s.stepper_position = 0;
    s.stepper_target = 0;
    s.stepper_moving = false;
    s.homing = false;
    s.stepper_abort = false;
    if (result.aborted || (result.hall_found && !result.offset_done)) {
      s.stepper_home_status = "aborted";
      s.stepper_homed = false;
    } else if (result.hall_found) {
      s.stepper_home_status = (result.offset_steps == 0) ? "hall_zero" : "offset_zero";
      s.stepper_homed = true;
    } else {
      s.stepper_home_status = "not_found";
      s.stepper_homed = false;
    }
  });

  return result;
}

static StepperHomeResult HomeStepperToUserZeroWithRetries(bool enable_motor, const char* log_context, int attempts) {
  attempts = std::max(attempts, 1);
  StepperHomeResult last{};
  for (int attempt = 1; attempt <= attempts; ++attempt) {
    last = HomeStepperToUserZeroBlocking(enable_motor, log_context);
    if (StepperHomeSucceeded(last)) {
      if (attempt > 1) {
        ESP_LOGI(TAG, "%s succeeded on attempt %d/%d", log_context, attempt, attempts);
      }
      ErrorManagerClear(ErrorCode::kStepperHoming);
      return last;
    }

    ESP_LOGW(TAG, "%s failed attempt %d/%d (hall=%s offset=%s aborted=%s hall_steps=%d offset_steps=%d)",
             log_context,
             attempt,
             attempts,
             last.hall_found ? "yes" : "no",
             last.offset_done ? "yes" : "no",
             last.aborted ? "yes" : "no",
             last.hall_steps,
             last.offset_steps);
    if (last.aborted) {
      break;
    }
    if (attempt < attempts) {
      vTaskDelay(pdMS_TO_TICKS(300));
    }
  }
  return last;
}

}  // namespace

void FindZeroTask(void*) {
  const bool hall_initial = IsHallTriggered();
  ESP_LOGI(TAG, "FindZero: start, hall_triggered=%s, offset=%d",
           hall_initial ? "yes" : "no", app_config.stepper_home_offset_steps);
  (void)HomeStepperToUserZeroBlocking(false, "FindZero");
  find_zero_task = nullptr;
  vTaskDelete(nullptr);
}

bool StartFindZeroTask(std::string* out_message) {
  if (find_zero_task) {
    if (out_message) *out_message = "homing already running";
    return false;
  }
  UpdateState([](SharedState& s) {
    s.stepper_abort = false;
    s.stepper_home_status = "running";
    s.stepper_homed = false;
    s.homing = true;
  });
  xTaskCreatePinnedToCore(&FindZeroTask, "find_zero", 4096, nullptr, 4, &find_zero_task, 1);
  if (out_message) *out_message = "homing_started";
  return true;
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
  mount_config.max_files = 4;
  mount_config.allocation_unit_size = 0;

  esp_err_t ret = esp_vfs_fat_sdmmc_mount(CONFIG_MOUNT_POINT, &host, &slot_config, &mount_config, &log_sd_card);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "SD mount for logging failed: %s", esp_err_to_name(ret));
    ErrorManagerSet(ErrorCode::kSdMount, ErrorSeverity::kError,
                    std::string("SD mount failed: ") + esp_err_to_name(ret));
    return false;
  }
  log_sd_mounted = true;
  ErrorManagerClear(ErrorCode::kSdMount);
  return true;
}

void UnmountLogSd() {
  if (log_sd_mounted) {
    esp_vfs_fat_sdcard_unmount(CONFIG_MOUNT_POINT, log_sd_card);
    log_sd_mounted = false;
    log_sd_card = nullptr;
  }
}

bool MountInternalFlashFs() {
  if (internal_flash_fs_mounted) {
    return true;
  }
  const esp_partition_t* part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                                         ESP_PARTITION_SUBTYPE_DATA_FAT,
                                                         INTERNAL_FLASH_PARTITION_LABEL);
  if (!part) {
    ESP_LOGE(TAG, "Internal flash FAT partition '%s' not found", INTERNAL_FLASH_PARTITION_LABEL);
    return false;
  }

  esp_vfs_fat_mount_config_t mount_config = {};
  mount_config.max_files = 3;
  mount_config.format_if_mount_failed = true;
  mount_config.allocation_unit_size = 32768;

  esp_err_t ret = esp_vfs_fat_spiflash_mount_rw_wl(INTERNAL_FLASH_MOUNT_POINT,
                                                    INTERNAL_FLASH_PARTITION_LABEL,
                                                    &mount_config,
                                                    &internal_flash_wl_handle);
  if (ret != ESP_OK) {
    internal_flash_wl_handle = WL_INVALID_HANDLE;
    ESP_LOGE(TAG, "Internal flash FS mount failed: %s (heap=%u largest=%u)",
             esp_err_to_name(ret),
             static_cast<unsigned>(esp_get_free_heap_size()),
             static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)));
    return false;
  }
  internal_flash_fs_mounted = true;
  ESP_LOGI(TAG, "Internal flash FS mounted at %s", INTERNAL_FLASH_MOUNT_POINT);
  return true;
}

static void UnmountInternalFlashFs() {
  if (!internal_flash_fs_mounted || internal_flash_wl_handle == WL_INVALID_HANDLE) {
    return;
  }
  esp_err_t ret = esp_vfs_fat_spiflash_unmount_rw_wl(INTERNAL_FLASH_MOUNT_POINT, internal_flash_wl_handle);
  if (ret != ESP_OK) {
    ESP_LOGW(TAG, "Internal flash FS unmount failed: %s", esp_err_to_name(ret));
    return;
  }
  internal_flash_wl_handle = WL_INVALID_HANDLE;
  internal_flash_fs_mounted = false;
  ESP_LOGI(TAG, "Internal flash FS unmounted");
}

const char* ActiveStorageMountPoint() {
  return app_config.storage_backend == StorageBackend::kInternalFlash
             ? INTERNAL_FLASH_MOUNT_POINT
             : CONFIG_MOUNT_POINT;
}

std::string ActiveToUploadDir() {
  return std::string(ActiveStorageMountPoint()) + "/to_upload";
}

std::string ActiveUploadedDir() {
  return std::string(ActiveStorageMountPoint()) + "/uploaded";
}

bool MountActiveStorage() {
  if (app_config.storage_backend == StorageBackend::kInternalFlash) {
    return MountInternalFlashFs();
  }
  return MountLogSd();
}

static bool ValidateStorageFilename(const std::string& name) {
  if (name.empty() || name.size() > 255) return false;
  for (char c : name) {
    if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-' || c == '.')) {
      return false;
    }
  }
  return true;
}

bool BuildActiveStorageFilenamePath(const std::string& name, std::string* out_full) {
  if (!ValidateStorageFilename(name)) return false;
  if (out_full) {
    *out_full = std::string(ActiveStorageMountPoint()) + "/" + name;
  }
  return true;
}

bool BuildActiveStorageRelativePath(const std::string& rel_path_raw, std::string* out_full) {
  if (rel_path_raw.empty() || rel_path_raw.size() > 256) return false;
  std::string rel_path = rel_path_raw;
  if (!rel_path.empty() && rel_path[0] == '/') rel_path.erase(rel_path.begin());
  if (rel_path.empty() || rel_path.size() > 256) return false;
  if (rel_path.find("..") != std::string::npos) return false;
  if (rel_path.find("//") != std::string::npos) return false;
  for (char c : rel_path) {
    if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-' || c == '.' || c == '/' || c == ' ' ||
          c == '(' || c == ')')) {
      return false;
    }
  }
  if (out_full) {
    *out_full = std::string(ActiveStorageMountPoint()) + "/" + rel_path;
  }
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

static std::string BuildConfigText(const AppConfig& cfg, const PidConfig& pid, UsbMode current_usb_mode) {
  std::string text;
  text.reserve(1800);
  AppendConfigLine(&text, "# Config generated by device\n");
  AppendConfigLine(&text, "wifi_ssid = %s\n", cfg.wifi_ssid.c_str());
  AppendConfigLine(&text, "wifi_password = %s\n", cfg.wifi_password.c_str());
  AppendConfigLine(&text, "wifi_ap_mode = %s\n", cfg.wifi_ap_mode ? "true" : "false");
  AppendConfigLine(&text, "net_mode = %s\n", NetModeToString(cfg.net_mode).c_str());
  AppendConfigLine(&text, "net_priority = %s\n", NetPriorityToString(cfg.net_priority).c_str());
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
  AppendConfigLine(&text, "usb_mass_storage = %s\n", current_usb_mode == UsbMode::kMsc ? "true" : "false");
  AppendConfigLine(&text, "logging_active = %s\n", cfg.logging_active ? "true" : "false");
  AppendConfigLine(&text, "storage_backend = %s\n", StorageBackendToString(cfg.storage_backend).c_str());
  if (!cfg.logging_postfix.empty()) {
    AppendConfigLine(&text, "logging_postfix = %s\n", cfg.logging_postfix.c_str());
  }
  AppendConfigLine(&text, "logging_use_motor = %s\n", cfg.logging_use_motor ? "true" : "false");
  AppendConfigLine(&text, "logging_duration_s = %.3f\n", cfg.logging_duration_s);
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
  if (!cfg.device_id.empty()) {
    AppendConfigLine(&text, "device_id = %s\n", cfg.device_id.c_str());
  }
  if (!cfg.minio_endpoint.empty()) {
    AppendConfigLine(&text, "minio_endpoint = %s\n", cfg.minio_endpoint.c_str());
  }
  if (!cfg.minio_access_key.empty()) {
    AppendConfigLine(&text, "minio_access_key = %s\n", cfg.minio_access_key.c_str());
  }
  if (!cfg.minio_secret_key.empty()) {
    AppendConfigLine(&text, "minio_secret_key = %s\n", cfg.minio_secret_key.c_str());
  }
  if (!cfg.minio_bucket.empty()) {
    AppendConfigLine(&text, "minio_bucket = %s\n", cfg.minio_bucket.c_str());
  }
  AppendConfigLine(&text, "minio_enabled = %s\n", cfg.minio_enabled ? "true" : "false");
  if (!cfg.mqtt_uri.empty()) {
    AppendConfigLine(&text, "mqtt_uri = %s\n", cfg.mqtt_uri.c_str());
  }
  if (!cfg.mqtt_user.empty()) {
    AppendConfigLine(&text, "mqtt_user = %s\n", cfg.mqtt_user.c_str());
  }
  if (!cfg.mqtt_password.empty()) {
    AppendConfigLine(&text, "mqtt_password = %s\n", cfg.mqtt_password.c_str());
  }
  AppendConfigLine(&text, "mqtt_enabled = %s\n", cfg.mqtt_enabled ? "true" : "false");
  return text;
}

static bool SaveConfigTextToInternalFlash(const std::string& text) {
  if (text.empty() || text.size() > 8192) {
    ESP_LOGE(TAG, "Internal config save rejected, size=%u", static_cast<unsigned>(text.size()));
    return false;
  }
  nvs_handle_t handle;
  esp_err_t err = nvs_open(CONFIG_NVS_NAMESPACE, NVS_READWRITE, &handle);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "NVS open for config failed: %s", esp_err_to_name(err));
    return false;
  }
  err = nvs_set_blob(handle, CONFIG_NVS_KEY, text.c_str(), text.size() + 1);
  if (err == ESP_OK) {
    err = nvs_commit(handle);
  }
  nvs_close(handle);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "NVS config save failed: %s", esp_err_to_name(err));
    return false;
  }
  ESP_LOGI(TAG, "Config synced to ESP internal flash");
  return true;
}

bool SaveConfigToInternalFlash(const AppConfig& cfg, const PidConfig& pid, UsbMode current_usb_mode) {
  return SaveConfigTextToInternalFlash(BuildConfigText(cfg, pid, current_usb_mode));
}

bool SyncConfigToInternalFlash() {
  return SaveConfigToInternalFlash(app_config, pid_config, usb_mode);
}

static bool ReadConfigTextFromInternalFlash(std::string* out) {
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

bool SaveConfigToSdCard(const AppConfig& cfg, const PidConfig& pid, UsbMode current_usb_mode) {
  const std::string config_text = BuildConfigText(cfg, pid, current_usb_mode);
  const bool internal_saved = SaveConfigTextToInternalFlash(config_text);
  SdLockGuard guard;
  if (!guard.locked()) {
    ESP_LOGW(TAG, "SD mutex unavailable, config saved only to ESP internal flash");
    ErrorManagerSet(ErrorCode::kSdMutex, ErrorSeverity::kWarning, "SD mutex unavailable during config save");
    return internal_saved;
  }
  ErrorManagerClear(ErrorCode::kSdMutex);
  const bool already_mounted = log_sd_mounted;
  if (!already_mounted) {
    if (!MountLogSd()) {
      ESP_LOGW(TAG, "SD unavailable, config saved only to ESP internal flash");
      return internal_saved;
    }
  }
  const char* tmp_path = "/sdcard/config.tmp";
  const char* backup_path = "/sdcard/config.bak";
  FILE* f = fopen(tmp_path, "w");
  if (!f) {
    ESP_LOGE(TAG, "Failed to open %s for writing", tmp_path);
    if (!already_mounted && !log_file) {
      UnmountLogSd();
    }
    return internal_saved;
  }
  if (fwrite(config_text.data(), 1, config_text.size(), f) != config_text.size()) {
    ESP_LOGE(TAG, "Failed to write %s", tmp_path);
    fclose(f);
    remove(tmp_path);
    if (!already_mounted && !log_file) {
      UnmountLogSd();
    }
    return internal_saved;
  }

  bool write_ok = true;
  if (fflush(f) != 0) {
    write_ok = false;
  }
  if (write_ok && fsync(fileno(f)) != 0) {
    write_ok = false;
  }
  if (fclose(f) != 0) {
    write_ok = false;
  }
  if (!write_ok) {
    ESP_LOGE(TAG, "Failed to flush %s", tmp_path);
    remove(tmp_path);
    if (!already_mounted && !log_file) {
      UnmountLogSd();
    }
    return internal_saved;
  }
  remove(backup_path);
  if (rename(CONFIG_FILE_PATH, backup_path) != 0 && errno != ENOENT) {
    ESP_LOGW(TAG, "Failed to backup %s to %s: %d", CONFIG_FILE_PATH, backup_path, errno);
  }
  if (rename(tmp_path, CONFIG_FILE_PATH) != 0) {
    ESP_LOGE(TAG, "Failed to replace %s with %s: %d", CONFIG_FILE_PATH, tmp_path, errno);
    rename(backup_path, CONFIG_FILE_PATH);
    remove(tmp_path);
    if (!already_mounted && !log_file) {
      UnmountLogSd();
    }
    return internal_saved;
  }
  remove(backup_path);
  // Keep mounted if logging is active to avoid invalidating open log file.
  if (!already_mounted && !log_file) {
    UnmountLogSd();
  }
  ESP_LOGI(TAG, "Config saved to %s", CONFIG_FILE_PATH);
  return internal_saved;
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
  ErrorManagerClear(ErrorCode::kLogTaskStack);
  if (log_task) {
    vTaskDelete(log_task);
    log_task = nullptr;
  }
  DisableStepper();
}

void LoggingTask(void*) {
  constexpr int kLoggingMotorSteps = 100;  // 90 degrees on the current motor/driver setup.
  constexpr int kGpsPositionTimeoutMs = 1500;
  const TickType_t settle_delay = pdMS_TO_TICKS(1000);  // pause after motor moves
  constexpr UBaseType_t kLogStackLowWords = 512;

  auto home_blocking = [&]() {
    return HomeStepperToUserZeroWithRetries(true, "Logging home", kStepperHomeRetryAttempts);
  };

  auto move_blocking = [&](int steps, bool forward) -> bool {
    int completed_steps = 0;
    UpdateState([&](SharedState& s) {
      s.homing = true;
      s.stepper_abort = false;
      s.stepper_moving = true;
      s.stepper_direction_forward = forward;
      s.stepper_target = s.stepper_position + (forward ? steps : -steps);
    });
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
      completed_steps++;
    }
    UpdateState([&](SharedState& s) {
      s.homing = false;
      s.stepper_moving = false;
      s.stepper_position += forward ? completed_steps : -completed_steps;
      s.stepper_target = s.stepper_position;
    });
    return completed_steps == steps && !CopyState().stepper_abort;
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
      if (log_config.use_motor && (snap.stepper_moving || snap.homing)) {
        ESP_LOGW(TAG, "Logging: stepper moved during averaging, discarding samples");
        return false;
      }
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

  if (log_config.use_motor || !log_config.homed_once) {
    StepperHomeResult home_result = home_blocking();
    log_config.homed_once = true;
    if (!StepperHomeSucceeded(home_result)) {
      const std::string msg = StepperHomeFailureMessage("Logging stopped: homing",
                                                        home_result,
                                                        kStepperHomeRetryAttempts);
      ESP_LOGW(TAG, "%s", msg.c_str());
      ErrorManagerSet(ErrorCode::kStepperHoming, ErrorSeverity::kError, msg);
      StopLogging();
      vTaskDelete(nullptr);
    }
  }

  while (true) {
    const UBaseType_t watermark_words = uxTaskGetStackHighWaterMark(nullptr);
    if (watermark_words > 0 && watermark_words < kLogStackLowWords) {
      ErrorManagerSet(ErrorCode::kLogTaskStack, ErrorSeverity::kWarning, "log_task stack low");
      ESP_LOGW(TAG, "log_task stack low: %u words", static_cast<unsigned>(watermark_words));
    } else {
      ErrorManagerClear(ErrorCode::kLogTaskStack);
    }

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
      if (at_zero) {
        vTaskDelay(settle_delay);
        SharedState avg{};
        if (!collect_avg(log_config.duration_s, log_config.temp_sensor_count, &avg)) {
          ESP_LOGW(TAG, "Logging: no samples collected, retrying");
          vTaskDelay(pdMS_TO_TICKS(500));
          continue;
        }

        // Сохраняем базу на пользовательском нуле и уходим на +45 градусов.
        pending_base = avg;
        has_pending_base = true;
        if (!move_blocking(kLoggingMotorSteps, true)) {
          ESP_LOGW(TAG, "Logging aborted during stepper move");
          StopLogging();
          vTaskDelete(nullptr);
        }
        at_zero = false;
        continue;
      }

      // Мы в +45 градусах от пользовательского нуля, это калибровочный замер.
      vTaskDelay(settle_delay);
      SharedState avg{};
      if (!collect_avg(log_config.duration_s, log_config.temp_sensor_count, &avg)) {
        ESP_LOGW(TAG, "Logging: no samples collected, retrying");
        vTaskDelay(pdMS_TO_TICKS(500));
        continue;
      }
      if (!has_pending_base) {
        ESP_LOGW(TAG, "Logging: got cal without base, skipping");
      } else {
        GpsPositionSnapshot gps{};
        (void)RequestGpsPositionOnce(kGpsPositionTimeoutMs, &gps);
        SdLockGuard guard(pdMS_TO_TICKS(2000));
        if (!guard.locked()) {
          ESP_LOGW(TAG, "SD mutex unavailable, retrying logging write");
          vTaskDelay(pdMS_TO_TICKS(200));
          continue;
        }
        const UtcTimeSnapshot row_time = GetBestUtcTimeForData();
        const uint64_t ts_ms = UtcTimeToUnixMs(row_time);
        const std::string iso = FormatUtcIso(row_time);
        fprintf(log_file, "%s,%llu,%.6f,%.6f,%.6f", iso.c_str(), (unsigned long long)ts_ms,
                pending_base.voltage1, pending_base.voltage2, pending_base.voltage3);
        for (int i = 0; i < pending_base.temp_sensor_count && i < MAX_TEMP_SENSORS; ++i) {
          fprintf(log_file, ",%.2f", pending_base.temps_c[i]);
        }
        fprintf(log_file, ",%.3f,%.3f,%.3f", pending_base.ina_bus_voltage, pending_base.ina_current, pending_base.ina_power);
        fprintf(log_file, ",%.6f,%.6f,%.6f", avg.voltage1, avg.voltage2, avg.voltage3);
        AppendGpsCsvFields(log_file, gps);
        fprintf(log_file, "\n");
        FlushLogFile();
        ESP_LOGD(TAG, "Logging: wrote row ts=%llu iso=%s", (unsigned long long)ts_ms, iso.c_str());
        PublishLogMeasurement(iso, ts_ms, pending_base, &avg, row_time.source, gps);
        UpdateState([&](SharedState& s) {
          s.voltage1_cal = avg.voltage1;
          s.voltage2_cal = avg.voltage2;
          s.voltage3_cal = avg.voltage3;
        });
      }

      // Вернуться на пользовательский ноль через Hall + offset, чтобы ошибка шагов не накапливалась.
      StepperHomeResult home_result = home_blocking();
      if (!StepperHomeSucceeded(home_result)) {
        const std::string msg = StepperHomeFailureMessage("Logging stopped: return homing",
                                                          home_result,
                                                          kStepperHomeRetryAttempts);
        ESP_LOGW(TAG, "%s", msg.c_str());
        ErrorManagerSet(ErrorCode::kStepperHoming, ErrorSeverity::kError, msg);
        StopLogging();
        vTaskDelete(nullptr);
      }
      at_zero = true;
      has_pending_base = false;
      continue;
    }

    // Без мотора — обычный замер
    SharedState avg1{};
    if (!collect_avg(log_config.duration_s, log_config.temp_sensor_count, &avg1)) {
      ESP_LOGW(TAG, "Logging: no samples collected, retrying");
      vTaskDelay(pdMS_TO_TICKS(500));
      continue;
    }
    bool have_cal = false;
    SharedState avg2{};
    GpsPositionSnapshot gps{};
    (void)RequestGpsPositionOnce(kGpsPositionTimeoutMs, &gps);

    SdLockGuard guard(pdMS_TO_TICKS(2000));
    if (!guard.locked()) {
      ESP_LOGW(TAG, "SD mutex unavailable, retrying logging write");
      vTaskDelay(pdMS_TO_TICKS(200));
      continue;
    }
    const UtcTimeSnapshot row_time = GetBestUtcTimeForData();
    const uint64_t ts_ms = UtcTimeToUnixMs(row_time);
    const std::string iso = FormatUtcIso(row_time);
    fprintf(log_file, "%s,%llu,%.6f,%.6f,%.6f", iso.c_str(), (unsigned long long)ts_ms,
            avg1.voltage1, avg1.voltage2, avg1.voltage3);
    for (int i = 0; i < avg1.temp_sensor_count && i < MAX_TEMP_SENSORS; ++i) {
      fprintf(log_file, ",%.2f", avg1.temps_c[i]);
    }
    fprintf(log_file, ",%.3f,%.3f,%.3f", avg1.ina_bus_voltage, avg1.ina_current, avg1.ina_power);
    if (log_config.use_motor && have_cal) {
      fprintf(log_file, ",%.6f,%.6f,%.6f", avg2.voltage1, avg2.voltage2, avg2.voltage3);
    }
    AppendGpsCsvFields(log_file, gps);
    fprintf(log_file, "\n");
    FlushLogFile();
    ESP_LOGD(TAG, "Logging: wrote row ts=%llu iso=%s", (unsigned long long)ts_ms, iso.c_str());
    PublishLogMeasurement(iso, ts_ms, avg1, nullptr, row_time.source, gps);
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
    xTaskCreatePinnedToCore(&LoggingTask, "log_task", 12288, nullptr, 2, &log_task, 0);
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
  ActionResult res = ActionStepperFindZero();
  if (!res.ok) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, res.message.c_str());
    return ESP_FAIL;
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

esp_err_t UploadedClearHandler(httpd_req_t* req) {
  int max_files = 1000;
  const size_t buf_len = std::min<size_t>(req->content_len, 64);
  if (buf_len > 0) {
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
    cJSON* max_item = cJSON_GetObjectItem(root, "maxFiles");
    if (max_item && cJSON_IsNumber(max_item)) {
      max_files = max_item->valueint;
    }
    cJSON_Delete(root);
  }

  ActionResult res = ActionUploadedClear({max_files});
  httpd_resp_set_type(req, "application/json");
  if (!res.ok) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, res.message.c_str());
    return ESP_FAIL;
  }
  return httpd_resp_sendstr(req, res.json.empty() ? "{}" : res.json.c_str());
}

static const char* PartitionTypeName(esp_partition_type_t type) {
  switch (type) {
    case ESP_PARTITION_TYPE_APP:
      return "app";
    case ESP_PARTITION_TYPE_DATA:
      return "data";
    case ESP_PARTITION_TYPE_BOOTLOADER:
      return "bootloader";
    case ESP_PARTITION_TYPE_PARTITION_TABLE:
      return "partition_table";
    default:
      return "unknown";
  }
}

static const char* PartitionSubtypeName(esp_partition_type_t type, esp_partition_subtype_t subtype) {
  if (type == ESP_PARTITION_TYPE_APP) {
    if (subtype == ESP_PARTITION_SUBTYPE_APP_FACTORY) return "factory";
    if (subtype >= ESP_PARTITION_SUBTYPE_APP_OTA_MIN && subtype < ESP_PARTITION_SUBTYPE_APP_OTA_MAX) return "ota";
    if (subtype == ESP_PARTITION_SUBTYPE_APP_TEST) return "test";
  }
  if (type == ESP_PARTITION_TYPE_DATA) {
    switch (subtype) {
      case ESP_PARTITION_SUBTYPE_DATA_OTA:
        return "ota";
      case ESP_PARTITION_SUBTYPE_DATA_PHY:
        return "phy";
      case ESP_PARTITION_SUBTYPE_DATA_NVS:
        return "nvs";
      case ESP_PARTITION_SUBTYPE_DATA_COREDUMP:
        return "coredump";
      case ESP_PARTITION_SUBTYPE_DATA_NVS_KEYS:
        return "nvs_keys";
      case ESP_PARTITION_SUBTYPE_DATA_FAT:
        return "fat";
      case ESP_PARTITION_SUBTYPE_DATA_SPIFFS:
        return "spiffs";
      case ESP_PARTITION_SUBTYPE_DATA_LITTLEFS:
        return "littlefs";
      default:
        break;
    }
  }
  return "unknown";
}

static bool ValidateInternalFlashRelPath(const std::string& raw, std::string* rel_out, std::string* full_out) {
  std::string rel = raw;
  if (!rel.empty() && rel[0] == '/') rel.erase(rel.begin());
  if (rel.size() > 256) return false;
  if (rel.find("..") != std::string::npos || rel.find("//") != std::string::npos) return false;
  for (char c : rel) {
    if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-' || c == '.' || c == '/' || c == ' ' ||
          c == '(' || c == ')')) {
      return false;
    }
  }
  if (rel_out) *rel_out = rel;
  if (full_out) {
    *full_out = std::string(INTERNAL_FLASH_MOUNT_POINT);
    if (!rel.empty()) {
      *full_out += "/";
      *full_out += rel;
    }
  }
  return true;
}

esp_err_t FlashListHandler(httpd_req_t* req) {
  cJSON* root = cJSON_CreateObject();
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
        if (!UrlDecode(buf, &decoded) || !ValidateInternalFlashRelPath(decoded, &rel_path, nullptr)) {
          httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad path");
          cJSON_Delete(root);
          return ESP_FAIL;
        }
      }
      if (httpd_query_key_value(qs.c_str(), "page", buf, sizeof(buf)) == ESP_OK) {
        page = std::max(0, atoi(buf));
      }
      if (httpd_query_key_value(qs.c_str(), "pageSize", buf, sizeof(buf)) == ESP_OK) {
        int p = atoi(buf);
        if (p > 0 && p <= 100) page_size = p;
      }
    }
  }

  uint32_t flash_size = 0;
  if (esp_flash_get_size(nullptr, &flash_size) == ESP_OK) {
    cJSON_AddNumberToObject(root, "flashTotalBytes", static_cast<double>(flash_size));
  }

  nvs_stats_t nvs_stats {};
  if (nvs_get_stats(nullptr, &nvs_stats) == ESP_OK) {
    cJSON* nvs = cJSON_CreateObject();
    cJSON_AddNumberToObject(nvs, "usedEntries", nvs_stats.used_entries);
    cJSON_AddNumberToObject(nvs, "freeEntries", nvs_stats.free_entries);
    cJSON_AddNumberToObject(nvs, "availableEntries", nvs_stats.available_entries);
    cJSON_AddNumberToObject(nvs, "totalEntries", nvs_stats.total_entries);
    cJSON_AddNumberToObject(nvs, "namespaceCount", nvs_stats.namespace_count);
    cJSON_AddItemToObject(root, "nvs", nvs);
  }
  cJSON_AddStringToObject(root, "storageBackend", StorageBackendToString(app_config.storage_backend).c_str());

  struct FlashEntry {
    std::string name;
    std::string type;
    std::string path;
    std::string area;
    std::string part_type;
    std::string subtype;
    uint64_t size = 0;
    uint32_t offset = 0;
    bool downloadable = false;
  };
  std::vector<FlashEntry> all_entries;
  std::string internal_config;
  if (rel_path.empty() && ReadConfigTextFromInternalFlash(&internal_config)) {
    all_entries.push_back({"config.txt", "file", "config.txt", "NVS cfg/config_txt", "", "",
                           static_cast<uint64_t>(internal_config.size()), 0, true});
  }

  bool internal_fs_mounted = false;
  {
    SdLockGuard guard(pdMS_TO_TICKS(500));
    if (guard.locked() && MountInternalFlashFs()) {
      internal_fs_mounted = true;
      struct statvfs fs {};
      FsOps ops = DefaultFsOps();
      if (ops.statvfs_fn(INTERNAL_FLASH_MOUNT_POINT, &fs) == 0 && fs.f_blocks > 0) {
        const uint64_t total = static_cast<uint64_t>(fs.f_blocks) * fs.f_frsize;
        const uint64_t avail = static_cast<uint64_t>(fs.f_bavail) * fs.f_frsize;
        cJSON_AddNumberToObject(root, "internalFsTotalBytes", static_cast<double>(total));
        cJSON_AddNumberToObject(root, "internalFsUsedBytes", static_cast<double>(total > avail ? total - avail : 0));
      }

      std::string full_dir;
      if (!ValidateInternalFlashRelPath(rel_path, nullptr, &full_dir)) {
        full_dir = INTERNAL_FLASH_MOUNT_POINT;
      }
      DIR* dir = opendir(full_dir.c_str());
      if (dir) {
        struct dirent* ent = nullptr;
        while ((ent = readdir(dir)) != nullptr) {
          if (ent->d_name[0] == '.') continue;
          if (ent->d_type != DT_REG && ent->d_type != DT_DIR && ent->d_type != DT_UNKNOWN) continue;
          std::string full = full_dir + "/" + ent->d_name;
          struct stat st {};
          uint64_t size = 0;
          bool is_dir = ent->d_type == DT_DIR;
          if (stat(full.c_str(), &st) == 0) {
            size = static_cast<uint64_t>(st.st_size);
            is_dir = S_ISDIR(st.st_mode);
          }
          std::string child_path = rel_path.empty() ? ent->d_name : (rel_path + "/" + ent->d_name);
          all_entries.push_back({ent->d_name, is_dir ? "dir" : "file", child_path, "internal FAT /flashfs", "", "",
                                 size, 0, !is_dir});
        }
        closedir(dir);
      }
    }
  }
  cJSON_AddBoolToObject(root, "internalFsMounted", internal_fs_mounted);

  uint64_t partition_bytes = 0;
  {
    esp_partition_iterator_t it = esp_partition_find(ESP_PARTITION_TYPE_ANY, ESP_PARTITION_SUBTYPE_ANY, nullptr);
    while (it != nullptr) {
      const esp_partition_t* part = esp_partition_get(it);
      if (part) {
        partition_bytes += part->size;
        if (rel_path.empty()) {
          all_entries.push_back({part->label, "partition", part->label, "", PartitionTypeName(part->type),
                                 PartitionSubtypeName(part->type, part->subtype), part->size, part->address, false});
        }
      }
      it = esp_partition_next(it);
    }
  }
  std::sort(all_entries.begin(), all_entries.end(), [](const FlashEntry& a, const FlashEntry& b) {
    if (a.type != b.type) return a.type < b.type;
    return a.name < b.name;
  });
  const int total = static_cast<int>(all_entries.size());
  const int total_pages = (total + page_size - 1) / page_size;
  int start = page * page_size;
  if (start >= total) {
    page = 0;
    start = 0;
  }
  cJSON_AddStringToObject(root, "path", rel_path.c_str());
  cJSON_AddNumberToObject(root, "page", page);
  cJSON_AddNumberToObject(root, "pageSize", page_size);
  cJSON_AddNumberToObject(root, "total", total);
  cJSON_AddNumberToObject(root, "totalPages", total_pages);
  cJSON_AddNumberToObject(root, "partitionBytes", static_cast<double>(partition_bytes));
  cJSON* entries = cJSON_CreateArray();
  for (int i = start; i < total && i < start + page_size; ++i) {
    const FlashEntry& e = all_entries[i];
    cJSON* obj = cJSON_CreateObject();
    cJSON_AddStringToObject(obj, "name", e.name.c_str());
    cJSON_AddStringToObject(obj, "type", e.type.c_str());
    cJSON_AddStringToObject(obj, "path", e.path.c_str());
    if (!e.area.empty()) cJSON_AddStringToObject(obj, "area", e.area.c_str());
    if (!e.part_type.empty()) cJSON_AddStringToObject(obj, "partType", e.part_type.c_str());
    if (!e.subtype.empty()) cJSON_AddStringToObject(obj, "subtype", e.subtype.c_str());
    if (e.offset > 0) cJSON_AddNumberToObject(obj, "offset", e.offset);
    cJSON_AddNumberToObject(obj, "size", static_cast<double>(e.size));
    cJSON_AddBoolToObject(obj, "downloadable", e.downloadable);
    cJSON_AddItemToArray(entries, obj);
  }
  cJSON_AddItemToObject(root, "entries", entries);

  const char* json = cJSON_PrintUnformatted(root);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_sendstr(req, json ? json : "{}");
  cJSON_free((void*)json);
  cJSON_Delete(root);
  return ESP_OK;
}

esp_err_t FlashDownloadHandler(httpd_req_t* req) {
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
  char path_param[256] = {};
  if (httpd_query_key_value(qs.c_str(), "path", path_param, sizeof(path_param)) != ESP_OK) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing path");
    return ESP_FAIL;
  }
  std::string decoded;
  if (!UrlDecode(path_param, &decoded)) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid flash object");
    return ESP_FAIL;
  }
  if (decoded == "config.txt") {
    std::string text;
    if (!ReadConfigTextFromInternalFlash(&text)) {
      httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Config not found in internal flash");
      return ESP_FAIL;
    }
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=\"internal_config.txt\"");
    return httpd_resp_send(req, text.c_str(), text.size());
  }

  std::string rel = decoded;
  if (!rel.empty() && rel[0] == '/') rel.erase(rel.begin());
  if (rel.empty() || rel.find("..") != std::string::npos || rel.find("//") != std::string::npos) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid path");
    return ESP_FAIL;
  }
  for (char c : rel) {
    if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-' || c == '.' || c == '/' || c == ' ' ||
          c == '(' || c == ')')) {
      httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid path");
      return ESP_FAIL;
    }
  }

  SdLockGuard guard(pdMS_TO_TICKS(1000));
  if (!guard.locked() || !MountInternalFlashFs()) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Internal flash busy");
    return ESP_FAIL;
  }
  std::string full_path = std::string(INTERNAL_FLASH_MOUNT_POINT) + "/" + rel;
  FILE* f = fopen(full_path.c_str(), "rb");
  if (!f) {
    httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "File not found");
    return ESP_FAIL;
  }
  httpd_resp_set_type(req, "application/octet-stream");
  std::string name = rel;
  const size_t slash = name.find_last_of('/');
  if (slash != std::string::npos) name = name.substr(slash + 1);
  std::string disp = "attachment; filename=\"" + name + "\"";
  httpd_resp_set_hdr(req, "Content-Disposition", disp.c_str());
  char chunk[1024];
  size_t n = 0;
  while ((n = fread(chunk, 1, sizeof(chunk), f)) > 0) {
    if (httpd_resp_send_chunk(req, chunk, n) != ESP_OK) {
      fclose(f);
      return ESP_FAIL;
    }
  }
  fclose(f);
  httpd_resp_send_chunk(req, nullptr, 0);
  return ESP_OK;
}

static esp_err_t SendFlashDeleteResult(httpd_req_t* req,
                                       const std::vector<std::string>& deleted,
                                       const std::vector<std::string>& skipped,
                                       const std::vector<std::string>& failed) {
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
  httpd_resp_sendstr(req, json ? json : "{}");
  cJSON_free((void*)json);
  cJSON_Delete(resp);
  return ESP_OK;
}

esp_err_t FlashDeleteHandler(httpd_req_t* req) {
  std::vector<std::string> requested_files;
  const bool saw_body = req->content_len > 0;
  bool has_body_files = false;
  if (saw_body) {
    const size_t buf_len = std::min<size_t>(req->content_len, 2048);
    std::string body(buf_len, '\0');
    int received = httpd_req_recv(req, body.data(), buf_len);
    if (received <= 0) {
      httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Failed to read body");
      return ESP_FAIL;
    }
    body.resize(received);
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
  if (saw_body && !has_body_files) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON body");
    return ESP_FAIL;
  }
  if (requested_files.empty()) {
    return SendFlashDeleteResult(req, {}, {}, {});
  }

  std::vector<std::string> deleted;
  std::vector<std::string> skipped;
  std::vector<std::string> failed;
  std::set<std::string> seen;
  SdLockGuard guard(pdMS_TO_TICKS(2000));
  if (!guard.locked() || !MountInternalFlashFs()) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Internal flash busy");
    return ESP_FAIL;
  }

  for (const auto& raw : requested_files) {
    if (!seen.insert(raw).second) continue;
    std::string rel;
    std::string full;
    if (!ValidateInternalFlashRelPath(raw, &rel, &full) || rel.empty()) {
      failed.push_back(raw + " (invalid)");
      continue;
    }
    const std::string base = rel.substr(rel.find_last_of('/') == std::string::npos ? 0 : rel.find_last_of('/') + 1);
    if (base == "config.txt") {
      skipped.push_back(raw + " (protected)");
      continue;
    }
    if (full == current_log_path) {
      skipped.push_back(raw + " (active log)");
      continue;
    }
    struct stat st {};
    if (stat(full.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) {
      skipped.push_back(raw + " (not a file)");
      continue;
    }
    if (remove(full.c_str()) == 0) {
      deleted.push_back(raw);
    } else {
      failed.push_back(raw + " (delete failed)");
    }
  }
  return SendFlashDeleteResult(req, deleted, skipped, failed);
}

esp_err_t FlashClearUploadedHandler(httpd_req_t* req) {
  int max_files = 1000;
  const size_t buf_len = std::min<size_t>(req->content_len, 64);
  if (buf_len > 0) {
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
    cJSON* max_item = cJSON_GetObjectItem(root, "maxFiles");
    if (max_item && cJSON_IsNumber(max_item)) {
      max_files = max_item->valueint;
    }
    cJSON_Delete(root);
  }
  max_files = std::clamp(max_files > 0 ? max_files : 1000, 1, 1000);

  int scanned = 0;
  int deleted_count = 0;
  int failed_count = 0;
  SdLockGuard guard(pdMS_TO_TICKS(2000));
  if (!guard.locked() || !MountInternalFlashFs()) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Internal flash busy");
    return ESP_FAIL;
  }
  const std::string uploaded_dir = std::string(INTERNAL_FLASH_MOUNT_POINT) + "/uploaded";
  (void)EnsureDirExists(uploaded_dir.c_str());
  DIR* dir = opendir(uploaded_dir.c_str());
  if (dir) {
    struct dirent* ent = nullptr;
    while ((ent = readdir(dir)) != nullptr && scanned < max_files) {
      if (ent->d_name[0] == '.') continue;
      if (ent->d_type != DT_REG && ent->d_type != DT_UNKNOWN) continue;
      const std::string full = uploaded_dir + "/" + ent->d_name;
      struct stat st {};
      if (stat(full.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) continue;
      scanned++;
      if (remove(full.c_str()) == 0) {
        deleted_count++;
      } else {
        failed_count++;
      }
    }
    closedir(dir);
  }

  cJSON* resp = cJSON_CreateObject();
  cJSON_AddStringToObject(resp, "status", "flash_uploaded_cleared");
  cJSON_AddNumberToObject(resp, "scanned", scanned);
  cJSON_AddNumberToObject(resp, "deleted", deleted_count);
  cJSON_AddNumberToObject(resp, "failed", failed_count);
  cJSON_AddNumberToObject(resp, "maxFiles", max_files);
  const char* json = cJSON_PrintUnformatted(resp);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_sendstr(req, json ? json : "{}");
  cJSON_free((void*)json);
  cJSON_Delete(resp);
  return ESP_OK;
}

struct UploadQueueTransferResult {
  int scanned = 0;
  int moved = 0;
  int skipped = 0;
  int failed = 0;
};

static std::string ToUploadDirForBackend(StorageBackend backend) {
  return backend == StorageBackend::kInternalFlash
             ? std::string(INTERNAL_FLASH_MOUNT_POINT) + "/to_upload"
             : std::string(TO_UPLOAD_DIR);
}

static const char* StorageName(StorageBackend backend) {
  return backend == StorageBackend::kInternalFlash ? "internal_flash" : "sd";
}

static bool CopyFileThenRemove(const std::string& src, const std::string& dst) {
  FILE* in = fopen(src.c_str(), "rb");
  if (!in) {
    ESP_LOGW(TAG, "Transfer open source failed %s errno=%d", src.c_str(), errno);
    return false;
  }
  FILE* out = fopen(dst.c_str(), "wb");
  if (!out) {
    ESP_LOGW(TAG, "Transfer open destination failed %s errno=%d", dst.c_str(), errno);
    fclose(in);
    return false;
  }
  char buf[1024];
  bool ok = true;
  size_t n = 0;
  while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
    if (fwrite(buf, 1, n, out) != n) {
      ok = false;
      break;
    }
  }
  if (ferror(in)) ok = false;
  if (fflush(out) != 0) ok = false;
  fclose(out);
  fclose(in);
  if (!ok) {
    remove(dst.c_str());
    return false;
  }
  if (remove(src.c_str()) != 0) {
    ESP_LOGW(TAG, "Transfer remove source failed %s errno=%d", src.c_str(), errno);
    return false;
  }
  return true;
}

static esp_err_t SendTransferResult(httpd_req_t* req,
                                    StorageBackend src_backend,
                                    StorageBackend dst_backend,
                                    const UploadQueueTransferResult& result) {
  cJSON* resp = cJSON_CreateObject();
  cJSON_AddStringToObject(resp, "status", "ok");
  cJSON_AddStringToObject(resp, "source", StorageName(src_backend));
  cJSON_AddStringToObject(resp, "destination", StorageName(dst_backend));
  cJSON_AddNumberToObject(resp, "scanned", result.scanned);
  cJSON_AddNumberToObject(resp, "moved", result.moved);
  cJSON_AddNumberToObject(resp, "skipped", result.skipped);
  cJSON_AddNumberToObject(resp, "failed", result.failed);
  const char* json = cJSON_PrintUnformatted(resp);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_sendstr(req, json ? json : "{}");
  cJSON_free((void*)json);
  cJSON_Delete(resp);
  return ESP_OK;
}

static esp_err_t TransferUploadQueueHandler(httpd_req_t* req, StorageBackend src_backend, StorageBackend dst_backend) {
  if (src_backend == dst_backend) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Source and destination are the same");
    return ESP_FAIL;
  }
  if (log_config.active || CopyState().logging || log_file != nullptr) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Stop logging before transfer");
    return ESP_FAIL;
  }
  if (usb_mode == UsbMode::kMsc && (src_backend == StorageBackend::kSd || dst_backend == StorageBackend::kSd)) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "USB MSC mode owns SD card");
    return ESP_FAIL;
  }

  int max_files = 10;
  const size_t buf_len = std::min<size_t>(req->content_len, 64);
  if (buf_len > 0) {
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
    cJSON* max_item = cJSON_GetObjectItem(root, "maxFiles");
    if (max_item && cJSON_IsNumber(max_item)) {
      max_files = max_item->valueint;
    }
    cJSON_Delete(root);
  }
  max_files = std::clamp(max_files > 0 ? max_files : 10, 1, 25);

  SdLockGuard guard(pdMS_TO_TICKS(10000));
  if (!guard.locked()) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Storage busy");
    return ESP_FAIL;
  }
  if (!MountLogSd()) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "SD mount failed");
    return ESP_FAIL;
  }
  if (!MountInternalFlashFs()) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Internal flash mount failed");
    return ESP_FAIL;
  }

  const std::string src_dir = ToUploadDirForBackend(src_backend);
  const std::string dst_dir = ToUploadDirForBackend(dst_backend);
  (void)EnsureDirExists(src_dir.c_str());
  if (!EnsureDirExists(dst_dir.c_str())) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Destination to_upload unavailable");
    return ESP_FAIL;
  }

  UploadQueueTransferResult result;
  DIR* dir = opendir(src_dir.c_str());
  if (!dir) {
    return SendTransferResult(req, src_backend, dst_backend, result);
  }

  struct dirent* ent = nullptr;
  while ((ent = readdir(dir)) != nullptr && result.scanned < max_files) {
    if (ent->d_name[0] == '.') continue;
    if (ent->d_type != DT_REG && ent->d_type != DT_UNKNOWN) continue;
    const std::string src = src_dir + "/" + ent->d_name;
    const std::string dst = dst_dir + "/" + ent->d_name;
    struct stat src_st {};
    if (stat(src.c_str(), &src_st) != 0 || !S_ISREG(src_st.st_mode)) {
      continue;
    }
    result.scanned++;
    struct stat dst_st {};
    if (stat(dst.c_str(), &dst_st) == 0) {
      result.skipped++;
      continue;
    }
    if (CopyFileThenRemove(src, dst)) {
      result.moved++;
    } else {
      result.failed++;
    }
    vTaskDelay(pdMS_TO_TICKS(5));
  }
  closedir(dir);
  return SendTransferResult(req, src_backend, dst_backend, result);
}

esp_err_t TransferFlashToSdHandler(httpd_req_t* req) {
  return TransferUploadQueueHandler(req, StorageBackend::kInternalFlash, StorageBackend::kSd);
}

esp_err_t TransferSdToFlashHandler(httpd_req_t* req) {
  return TransferUploadQueueHandler(req, StorageBackend::kSd, StorageBackend::kInternalFlash);
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
  uint16_t sensor_mask = pid_config.sensor_mask;
  bool sensor_mask_set = false;

  get_number("kp", &kp);
  get_number("ki", &ki);
  get_number("kd", &kd);
  get_number("setpoint", &sp);
  cJSON* sensor_item = cJSON_GetObjectItem(root, "sensor");
  if (sensor_item && cJSON_IsNumber(sensor_item)) {
    sensor = sensor_item->valueint;
  }
  cJSON* mask_item = cJSON_GetObjectItem(root, "sensorMask");
  if (mask_item && cJSON_IsNumber(mask_item)) {
    sensor_mask = static_cast<uint16_t>(mask_item->valuedouble);
    sensor_mask_set = true;
  }
  cJSON* sensors_item = cJSON_GetObjectItem(root, "sensors");
  if (sensors_item && cJSON_IsArray(sensors_item)) {
    sensor_mask = 0;
    const int len = cJSON_GetArraySize(sensors_item);
    for (int i = 0; i < len; ++i) {
      cJSON* entry = cJSON_GetArrayItem(sensors_item, i);
      if (!entry || !cJSON_IsNumber(entry)) continue;
      int idx = entry->valueint;
      if (idx < 0 || idx >= MAX_TEMP_SENSORS) continue;
      sensor_mask = static_cast<uint16_t>(sensor_mask | (1u << idx));
    }
    sensor_mask_set = true;
  }
  cJSON_Delete(root);

  PidApplyRequest action_req;
  action_req.kp = kp;
  action_req.ki = ki;
  action_req.kd = kd;
  action_req.setpoint = sp;
  action_req.sensor = sensor;
  action_req.sensor_mask = sensor_mask;
  action_req.sensor_mask_set = sensor_mask_set;
  ActionResult res = ActionPidApply(action_req);
  if (!res.ok) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, res.message.c_str());
    return ESP_FAIL;
  }

  httpd_resp_set_type(req, "application/json");
  return httpd_resp_sendstr(req, "{\"status\":\"pid_applied\"}");
}

esp_err_t PidEnableHandler(httpd_req_t* req) {
  ActionResult res = ActionPidEnable();
  if (!res.ok) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, res.message.c_str());
    return ESP_FAIL;
  }
  httpd_resp_set_type(req, "application/json");
  return httpd_resp_sendstr(req, "{\"status\":\"pid_enabled\"}");
}

esp_err_t PidDisableHandler(httpd_req_t* req) {
  ActionResult res = ActionPidDisable();
  httpd_resp_set_type(req, "application/json");
  if (!res.ok) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, res.message.c_str());
    return ESP_FAIL;
  }
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

  if (requested == usb_mode) {
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"status\":\"unchanged\"}");
  }

  ActionResult res = ActionUsbModeSet(requested);
  if (!res.ok) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, res.message.c_str());
    return ESP_FAIL;
  }
  httpd_resp_set_type(req, "application/json");
  httpd_resp_sendstr(req, "{\"status\":\"restarting\",\"mode\":\"pending\"}");
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

  ActionResult res = ActionWifiApply({mode, ssid, pass});
  if (!res.ok) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, res.message.c_str());
    return ESP_FAIL;
  }

  httpd_resp_set_type(req, "application/json");
  return httpd_resp_sendstr(req, "{\"status\":\"wifi_applied\"}");
}

esp_err_t NetApplyHandler(httpd_req_t* req) {
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
  cJSON* prio_item = cJSON_GetObjectItem(root, "priority");
  std::string mode = (mode_item && cJSON_IsString(mode_item) && mode_item->valuestring) ? mode_item->valuestring : "";
  std::string priority =
      (prio_item && cJSON_IsString(prio_item) && prio_item->valuestring) ? prio_item->valuestring : "";
  cJSON_Delete(root);

  ActionResult res = ActionNetApply({mode, priority});
  if (!res.ok) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, res.message.c_str());
    return ESP_FAIL;
  }

  httpd_resp_set_type(req, "application/json");
  return httpd_resp_sendstr(req, "{\"status\":\"net_applied\"}");
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
  CloudApplyRequest action_req;
  action_req.device_id = get_str("deviceId");
  action_req.minio_endpoint = get_str("minioEndpoint");
  action_req.minio_access_key = get_str("minioAccessKey");
  action_req.minio_secret_key = get_str("minioSecretKey");
  action_req.minio_bucket = get_str("minioBucket");
  action_req.minio_enabled = get_bool("minioEnabled", app_config.minio_enabled);
  action_req.mqtt_uri = get_str("mqttUri");
  action_req.mqtt_user = get_str("mqttUser");
  action_req.mqtt_password = get_str("mqttPassword");
  action_req.mqtt_enabled = get_bool("mqttEnabled", app_config.mqtt_enabled);
  cJSON_Delete(root);

  ActionResult res = ActionCloudApply(action_req);
  if (!res.ok) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, res.message.c_str());
    return ESP_FAIL;
  }

  httpd_resp_set_type(req, "application/json");
  return httpd_resp_sendstr(req, "{\"status\":\"cloud_applied\"}");
}

esp_err_t GpsApplyHandler(httpd_req_t* req) {
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

  std::vector<uint16_t> rtcm_types;
  cJSON* types_item = cJSON_GetObjectItem(root, "rtcmTypes");
  if (types_item && cJSON_IsArray(types_item)) {
    const int len = cJSON_GetArraySize(types_item);
    for (int i = 0; i < len; ++i) {
      cJSON* item = cJSON_GetArrayItem(types_item, i);
      if (!item) continue;
      int type = 0;
      if (cJSON_IsNumber(item)) {
        type = item->valueint;
      } else if (cJSON_IsString(item) && item->valuestring) {
        type = std::atoi(item->valuestring);
      }
      if (type > 0 && type <= 4095 &&
          std::find(rtcm_types.begin(), rtcm_types.end(), static_cast<uint16_t>(type)) == rtcm_types.end()) {
        rtcm_types.push_back(static_cast<uint16_t>(type));
      }
    }
  } else if (types_item && cJSON_IsString(types_item) && types_item->valuestring) {
    rtcm_types = ParseRtcmTypesText(types_item->valuestring);
  }
  if (rtcm_types.empty()) {
    cJSON_Delete(root);
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "RTCM type list is empty");
    return ESP_FAIL;
  }

  std::string mode = app_config.gps_mode.empty() ? "base_time_60" : app_config.gps_mode;
  cJSON* mode_item = cJSON_GetObjectItem(root, "mode");
  if (mode_item && cJSON_IsString(mode_item) && mode_item->valuestring) {
    mode = mode_item->valuestring;
  }
  cJSON_Delete(root);

  if (!(mode == "keep" || mode == "base_time_60" || mode == "base" || mode == "rover_uav" || mode == "rover")) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid GPS mode");
    return ESP_FAIL;
  }
  app_config.gps_rtcm_types = rtcm_types;
  app_config.gps_mode = mode;
  if (!SaveConfigToSdCard(app_config, pid_config, usb_mode)) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to save config.txt");
    return ESP_FAIL;
  }
  RequestGpsReconfigure();
  ProbeGpsMode();

  httpd_resp_set_type(req, "application/json");
  return httpd_resp_sendstr(req, "{\"status\":\"gps_saved\",\"reconfigure\":\"queued\"}");
}

esp_err_t GpsProbeHandler(httpd_req_t* req) {
  ProbeGpsMode();
  httpd_resp_set_type(req, "application/json");
  return httpd_resp_sendstr(req, "{\"status\":\"gps_mode_probe_sent\"}");
}

esp_err_t ConfigSyncInternalFlashHandler(httpd_req_t* req) {
  ActionResult res = ActionConfigSyncInternalFlash();
  if (!res.ok) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, res.message.c_str());
    return ESP_FAIL;
  }
  httpd_resp_set_type(req, "application/json");
  return httpd_resp_sendstr(req, "{\"status\":\"config_synced_to_internal_flash\"}");
}

esp_err_t StorageApplyHandler(httpd_req_t* req) {
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
  cJSON* item = cJSON_GetObjectItem(root, "backend");
  StorageBackend backend = app_config.storage_backend;
  const bool ok = item && cJSON_IsString(item) && item->valuestring && ParseStorageBackend(item->valuestring, &backend);
  cJSON_Delete(root);
  if (!ok) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid storage backend");
    return ESP_FAIL;
  }
  if (log_config.active || CopyState().logging || log_file != nullptr) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Stop logging before switching storage");
    return ESP_FAIL;
  }

  bool restart_required = false;
  if (backend == StorageBackend::kInternalFlash) {
    SdLockGuard guard(pdMS_TO_TICKS(2000));
    if (!guard.locked()) {
      restart_required = true;
    } else if (!MountInternalFlashFs()) {
      restart_required = true;
    }
  }

  app_config.storage_backend = backend;
  if (!SaveConfigToSdCard(app_config, pid_config, usb_mode)) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to save config");
    return ESP_FAIL;
  }

  httpd_resp_set_type(req, "application/json");
  std::string resp = std::string("{\"status\":\"storage_saved\",\"backend\":\"") +
                     StorageBackendToString(app_config.storage_backend) + "\",\"restartRequired\":" +
                     (restart_required ? "true" : "false") + "}";
  return httpd_resp_sendstr(req, resp.c_str());
}

esp_err_t StorageRemountHandler(httpd_req_t* req) {
  if (log_config.active || CopyState().logging || log_file != nullptr) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Stop logging before remount");
    return ESP_FAIL;
  }
  if (usb_mode == UsbMode::kMsc && app_config.storage_backend == StorageBackend::kSd) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "USB MSC mode owns SD card");
    return ESP_FAIL;
  }

  bool ok = false;
  {
    SdLockGuard guard(pdMS_TO_TICKS(3000));
    if (!guard.locked()) {
      httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Storage busy");
      return ESP_FAIL;
    }
    if (app_config.storage_backend == StorageBackend::kInternalFlash) {
      UnmountInternalFlashFs();
      ok = MountInternalFlashFs();
    } else {
      UnmountLogSd();
      ok = MountLogSd();
    }
  }

  if (!ok) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Remount failed");
    return ESP_FAIL;
  }
  httpd_resp_set_type(req, "application/json");
  std::string resp = std::string("{\"status\":\"storage_remounted\",\"backend\":\"") +
                     StorageBackendToString(app_config.storage_backend) +
                     "\",\"sdMounted\":" + (log_sd_mounted ? "true" : "false") +
                     ",\"internalFlashMounted\":" + (internal_flash_fs_mounted ? "true" : "false") + "}";
  return httpd_resp_sendstr(req, resp.c_str());
}

httpd_handle_t StartHttpServer() {
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = 80;
  config.lru_purge_enable = true;
  config.max_uri_handlers = 47;
  config.stack_size = 8192;

  if (httpd_start(&http_server, &config) != ESP_OK) {
    ESP_LOGE(TAG, "Failed to start HTTP server");
    return nullptr;
  }

  httpd_uri_t root_uri = {.uri = "/", .method = HTTP_GET, .handler = RootHandler, .user_ctx = nullptr};
  httpd_uri_t data_uri = {.uri = "/data", .method = HTTP_GET, .handler = DataHandler, .user_ctx = nullptr};
  httpd_uri_t calibrate_uri = {.uri = "/calibrate", .method = HTTP_POST, .handler = CalibrateHandler, .user_ctx = nullptr};
  httpd_uri_t restart_uri = {.uri = "/restart", .method = HTTP_POST, .handler = RestartHandler, .user_ctx = nullptr};
  httpd_uri_t external_power_set_uri = {.uri = "/external_power/set", .method = HTTP_POST, .handler = ExternalPowerSetHandler, .user_ctx = nullptr};
  httpd_uri_t external_power_cycle_uri = {.uri = "/external_power/cycle", .method = HTTP_POST, .handler = ExternalPowerCycleHandler, .user_ctx = nullptr};
  httpd_uri_t stepper_enable_uri = {.uri = "/stepper/enable", .method = HTTP_POST, .handler = StepperEnableHandler, .user_ctx = nullptr};
  httpd_uri_t stepper_disable_uri = {.uri = "/stepper/disable", .method = HTTP_POST, .handler = StepperDisableHandler, .user_ctx = nullptr};
  httpd_uri_t stepper_move_uri = {.uri = "/stepper/move", .method = HTTP_POST, .handler = StepperMoveHandler, .user_ctx = nullptr};
  httpd_uri_t stepper_stop_uri = {.uri = "/stepper/stop", .method = HTTP_POST, .handler = StepperStopHandler, .user_ctx = nullptr};
  httpd_uri_t stepper_zero_uri = {.uri = "/stepper/zero", .method = HTTP_POST, .handler = StepperZeroHandler, .user_ctx = nullptr};
  httpd_uri_t stepper_find_zero_uri = {.uri = "/stepper/find_zero", .method = HTTP_POST, .handler = StepperFindZeroHandler, .user_ctx = nullptr};
  httpd_uri_t stepper_home_offset_uri = {.uri = "/stepper/home_offset", .method = HTTP_POST, .handler = StepperHomeOffsetHandler, .user_ctx = nullptr};
  httpd_uri_t usb_mode_get_uri = {.uri = "/usb/mode", .method = HTTP_GET, .handler = UsbModeGetHandler, .user_ctx = nullptr};
  httpd_uri_t usb_mode_set_uri = {.uri = "/usb/mode", .method = HTTP_POST, .handler = UsbModeSetHandler, .user_ctx = nullptr};
  httpd_uri_t wifi_apply_uri = {.uri = "/wifi/apply", .method = HTTP_POST, .handler = WifiApplyHandler, .user_ctx = nullptr};
  httpd_uri_t net_apply_uri = {.uri = "/net/apply", .method = HTTP_POST, .handler = NetApplyHandler, .user_ctx = nullptr};
  httpd_uri_t cloud_apply_uri = {.uri = "/cloud/apply", .method = HTTP_POST, .handler = CloudApplyHandler, .user_ctx = nullptr};
  httpd_uri_t gps_apply_uri = {.uri = "/gps/apply", .method = HTTP_POST, .handler = GpsApplyHandler, .user_ctx = nullptr};
  httpd_uri_t gps_probe_uri = {.uri = "/gps/probe", .method = HTTP_POST, .handler = GpsProbeHandler, .user_ctx = nullptr};
  httpd_uri_t config_sync_internal_uri = {.uri = "/config/sync_internal_flash", .method = HTTP_POST, .handler = ConfigSyncInternalFlashHandler, .user_ctx = nullptr};
  httpd_uri_t storage_apply_uri = {.uri = "/storage/apply", .method = HTTP_POST, .handler = StorageApplyHandler, .user_ctx = nullptr};
  httpd_uri_t storage_remount_uri = {.uri = "/storage/remount", .method = HTTP_POST, .handler = StorageRemountHandler, .user_ctx = nullptr};
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
  httpd_uri_t uploaded_clear_uri = {.uri = "/fs/clear_uploaded", .method = HTTP_POST, .handler = UploadedClearHandler, .user_ctx = nullptr};
  httpd_uri_t flash_list_uri = {.uri = "/flash/list", .method = HTTP_GET, .handler = FlashListHandler, .user_ctx = nullptr};
  httpd_uri_t flash_download_uri = {.uri = "/flash/download", .method = HTTP_GET, .handler = FlashDownloadHandler, .user_ctx = nullptr};
  httpd_uri_t flash_delete_uri = {.uri = "/flash/delete", .method = HTTP_POST, .handler = FlashDeleteHandler, .user_ctx = nullptr};
  httpd_uri_t flash_clear_uploaded_uri = {.uri = "/flash/clear_uploaded", .method = HTTP_POST, .handler = FlashClearUploadedHandler, .user_ctx = nullptr};
  httpd_uri_t transfer_flash_to_sd_uri = {.uri = "/storage/transfer_flash_to_sd", .method = HTTP_POST, .handler = TransferFlashToSdHandler, .user_ctx = nullptr};
  httpd_uri_t transfer_sd_to_flash_uri = {.uri = "/storage/transfer_sd_to_flash", .method = HTTP_POST, .handler = TransferSdToFlashHandler, .user_ctx = nullptr};

  httpd_register_uri_handler(http_server, &root_uri);
  httpd_register_uri_handler(http_server, &data_uri);
  httpd_register_uri_handler(http_server, &calibrate_uri);
  httpd_register_uri_handler(http_server, &restart_uri);
  httpd_register_uri_handler(http_server, &external_power_set_uri);
  httpd_register_uri_handler(http_server, &external_power_cycle_uri);
  httpd_register_uri_handler(http_server, &stepper_enable_uri);
  httpd_register_uri_handler(http_server, &stepper_disable_uri);
  httpd_register_uri_handler(http_server, &stepper_move_uri);
  httpd_register_uri_handler(http_server, &stepper_stop_uri);
  httpd_register_uri_handler(http_server, &stepper_zero_uri);
  httpd_register_uri_handler(http_server, &stepper_find_zero_uri);
  httpd_register_uri_handler(http_server, &stepper_home_offset_uri);
  httpd_register_uri_handler(http_server, &usb_mode_get_uri);
  httpd_register_uri_handler(http_server, &usb_mode_set_uri);
  httpd_register_uri_handler(http_server, &wifi_apply_uri);
  httpd_register_uri_handler(http_server, &net_apply_uri);
  httpd_register_uri_handler(http_server, &cloud_apply_uri);
  httpd_register_uri_handler(http_server, &gps_apply_uri);
  httpd_register_uri_handler(http_server, &gps_probe_uri);
  httpd_register_uri_handler(http_server, &config_sync_internal_uri);
  httpd_register_uri_handler(http_server, &storage_apply_uri);
  httpd_register_uri_handler(http_server, &storage_remount_uri);
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
  httpd_register_uri_handler(http_server, &uploaded_clear_uri);
  httpd_register_uri_handler(http_server, &flash_list_uri);
  httpd_register_uri_handler(http_server, &flash_download_uri);
  httpd_register_uri_handler(http_server, &flash_delete_uri);
  httpd_register_uri_handler(http_server, &flash_clear_uploaded_uri);
  httpd_register_uri_handler(http_server, &transfer_flash_to_sd_uri);
  httpd_register_uri_handler(http_server, &transfer_sd_to_flash_uri);
  ESP_LOGI(TAG, "HTTP server started");
  return http_server;
}
